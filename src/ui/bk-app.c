/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * big-keyboard: o aplicativo. Uma instância por sessão (GtkApplication
 * io.github.big_windowmaker.BigKeyboard); as ações saem pelo D-Bus para o
 * dockapp e para a linha de comando:
 *
 *   big-keyboard [--toggle|--show|--hide|--settings|--gamepad|--edit-gamepad
 *                 |--release|--daemon|--quit] [--remote-touch=MODO] [--output=CONECTOR]
 *
 * Integração (Big Remote Play e outros), pelas ações org.gtk.Actions:
 *   set-gamepad (b)           mostra/oculta o controle na tela (sem alternar)
 *   remote-touch (s, estado)  off | exclusive | shared — toque do Moonlight
 *   overlay-output (s, estado) conector do monitor transmitido ("" = principal)
 *   remote-touch-status (estado) off, idle, waiting, active, shared, busy,
 *                             denied, error (veja bk_remote_state)
 *
 * Fica residente (--daemon, no início da sessão) só com descritores
 * parados: o controle físico (udev + evdev) e o arquivo do tema. Nada de
 * polling; o teclado fechado não desenha.
 */
#include "bk-app.h"
#include "bk-focus.h"

#include <glib-unix.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <string.h>

static BkApp the_app;

BkApp *
bk_app_get(void)
{
    return &the_app;
}

/* ---- tema ----------------------------------------------------------------- */

static gboolean
read_dark(BkApp *app)
{
    if (!strcmp(app->cfg.theme, "dark"))
        return TRUE;
    if (!strcmp(app->cfg.theme, "light"))
        return FALSE;
    char *path = g_build_filename(g_get_user_config_dir(), "big-wm", "theme", NULL);
    char *data = NULL;
    gboolean dark = FALSE;
    if (g_file_get_contents(path, &data, NULL, NULL))
        dark = g_str_has_prefix(g_strstrip(data), "dark");
    g_free(data);
    g_free(path);
    return dark;
}

gboolean
bk_app_dark(BkApp *app)
{
    return app->dark;
}

static void
on_theme_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer data)
{
    (void)m;
    (void)f;
    (void)o;
    (void)ev;
    BkApp *app = data;
    gboolean dark = read_dark(app);
    if (dark != app->dark) {
        app->dark = dark;
        if (app->osk)
            bk_osk_reload(app->osk);
    }
}

/* ---- estado para o dockapp ------------------------------------------------ */

void
bk_app_update_state(BkApp *app)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(app->gtk), "mode");
    if (!a)
        return;
    gboolean kb = app->osk && bk_osk_visible(app->osk);
    gboolean gp = app->overlay && bk_overlay_visible(app->overlay);
    const char *mode = kb && gp ? "hybrid" : kb ? "keyboard" : gp ? "gamepad" : "hidden";
    g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_string(mode));
    GAction *rt = g_action_map_lookup_action(G_ACTION_MAP(app->gtk), "remote-touch-status");
    if (rt && app->remote)
        g_simple_action_set_state(G_SIMPLE_ACTION(rt), g_variant_new_string(bk_remote_state(app->remote)));
    bk_remote_sync(app->remote); /* o teclado ou o controle abriu ou fechou */
    GAction *p = g_action_map_lookup_action(G_ACTION_MAP(app->gtk), "pad-connected");
    if (p)
        g_simple_action_set_state(G_SIMPLE_ACTION(p), g_variant_new_boolean(app->pad_open && app->pad.npads > 0));
}

void
bk_app_switch(BkApp *app, gboolean to_gamepad)
{
    if (to_gamepad) {
        bk_overlay_set_visible(app->overlay, TRUE, FALSE); /* reusa o controle virtual */
        bk_osk_set_visible(app->osk, FALSE);
    } else {
        GdkMonitor *m = bk_overlay_monitor(app->overlay);
        bk_overlay_hide_keep_pad(app->overlay);
        if (!bk_osk_visible(app->osk))
            bk_osk_set_monitor(app->osk, m);
        bk_osk_set_visible(app->osk, TRUE);
    }
}

void
bk_app_release_all(BkApp *app)
{
    if (app->osk)
        bk_osk_release(app->osk);
    if (app->overlay)
        bk_overlay_release(app->overlay);
    bk_remote_release(app->remote);
    if (app->vpad)
        bk_vpad_neutral(app->vpad);
}

/* ---- controle físico ------------------------------------------------------ */

static void pad_watch(BkApp *app);

static void
pad_dispatch(BkApp *app, const enum bwg_action *a, int n)
{
    for (int i = 0; i < n; i++)
        if (app->osk) {
            if (a[i] == BWG_COMBO && !app->cfg.pad_combo)
                continue;
            bk_osk_pad_action(app->osk, a[i]);
        }
    if (app->osk)
        bk_osk_pad_sticks(app->osk, bwg_active(&app->pad), app->cfg.deadzone);
    bk_settings_pad_event(app);
}

static gboolean
on_pad_repeat(gpointer data)
{
    BkApp *app = data;
    app->pad_repeat_id = 0;
    enum bwg_action out[4];
    int n = bwg_tick(&app->pad, bwg_now_ms(), out, 4);
    pad_dispatch(app, out, n);
    pad_watch(app);
    return G_SOURCE_REMOVE;
}

static gboolean
on_pad_fd(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond;
    BkApp *app = data;
    unsigned before = app->pad.npads;
    enum bwg_action out[BWG_MAX_ACTIONS];
    int n = bwg_handle(&app->pad, fd, bwg_now_ms(), out, BWG_MAX_ACTIONS);
    pad_dispatch(app, out, n);
    if ((unsigned)app->pad.npads != before)
        bk_app_update_state(app);
    pad_watch(app);
    return G_SOURCE_CONTINUE;
}

static void
pad_watch(BkApp *app)
{
    if (app->pad.generation != app->pad_generation) {
        app->pad_generation = app->pad.generation;
        for (guint i = 0; i < app->pad_sources->len; i++)
            g_source_remove(g_array_index(app->pad_sources, guint, i));
        g_array_set_size(app->pad_sources, 0);
        int fds[BWG_MAX_PADS + 1];
        int n = bwg_fds(&app->pad, fds, BWG_MAX_PADS + 1);
        for (int i = 0; i < n; i++) {
            guint id = g_unix_fd_add(fds[i], G_IO_IN | G_IO_HUP | G_IO_ERR, on_pad_fd, app);
            g_array_append_val(app->pad_sources, id);
        }
    }
    if (app->pad_repeat_id) {
        g_source_remove(app->pad_repeat_id);
        app->pad_repeat_id = 0;
    }
    int t = bwg_timeout(&app->pad, bwg_now_ms());
    if (t >= 0)
        app->pad_repeat_id = g_timeout_add((guint)t, on_pad_repeat, app);
}

static void
pad_apply_config(BkApp *app)
{
    if (app->cfg.pad && !app->pad_open) {
        app->pad_open = bwg_open(&app->pad) >= 0;
        app->pad_generation = ~0u;
    } else if (!app->cfg.pad && app->pad_open) {
        for (guint i = 0; i < app->pad_sources->len; i++)
            g_source_remove(g_array_index(app->pad_sources, guint, i));
        g_array_set_size(app->pad_sources, 0);
        bwg_close(&app->pad);
        app->pad_open = FALSE;
    }
    app->pad.deadzone = app->cfg.deadzone;
    app->pad.no_stick_nav = strcmp(app->cfg.pad_mode, "cursor") != 0;
    if (app->pad_open)
        pad_watch(app);
}

/* ---- campo de texto em foco (como no celular) ------------------------------ */

static gboolean
on_focus_hide(gpointer data)
{
    BkApp *app = data;
    app->focus_hide_id = 0;
    if (bk_osk_auto_shown(app->osk))
        bk_osk_set_visible(app->osk, FALSE);
    return G_SOURCE_REMOVE;
}

/* Um campo de texto ganhou o foco: abre (se fechado). Saiu de um: fecha,
 * se foi aberto assim; com uma pequena espera, porque passar de um campo
 * a outro tira e põe o foco. */
static void
on_text_focus(gboolean editable, gpointer data)
{
    BkApp *app = data;
    if (app->focus_hide_id) {
        g_source_remove(app->focus_hide_id);
        app->focus_hide_id = 0;
    }
    if (editable) {
        if (!bk_osk_visible(app->osk)) {
            bk_osk_set_visible(app->osk, TRUE);
            bk_osk_set_auto_shown(app->osk, TRUE);
        }
    } else if (app->cfg.auto_hide && bk_osk_auto_shown(app->osk)) {
        app->focus_hide_id = g_timeout_add(400, on_focus_hide, app);
    }
}

static void
focus_apply_config(BkApp *app)
{
    if (app->cfg.auto_show && !app->focus) {
        app->focus = bk_focus_new(on_text_focus, app);
    } else if (!app->cfg.auto_show && app->focus) {
        bk_focus_free(app->focus);
        app->focus = NULL;
    }
}

void
bk_app_config_changed(BkApp *app)
{
    app->dark = read_dark(app);
    pad_apply_config(app);
    focus_apply_config(app);
    if (app->osk)
        bk_osk_reload(app->osk);
    /* o modo do toque do celular mudou nas configurações: a ação o aplica
     * (e o Big Remote Play vê o estado novo) */
    GVariant *rt = g_action_group_get_action_state(G_ACTION_GROUP(app->gtk), "remote-touch");
    if (rt) {
        if (strcmp(g_variant_get_string(rt, NULL), app->cfg.remote_touch))
            g_action_group_change_action_state(G_ACTION_GROUP(app->gtk), "remote-touch",
                                               g_variant_new_string(app->cfg.remote_touch));
        g_variant_unref(rt);
    }
}

/* ---- ações ---------------------------------------------------------------- */

/* Modo de exibição exclusivo: abrir o teclado oculta o controle na tela e
 * vice-versa. No independente (o padrão), os dois convivem. */
static gboolean
exclusive(BkApp *app)
{
    return !strcmp(app->cfg.display_mode, "exclusive");
}

static void
act_toggle(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    BkApp *app = data;
    gboolean show = !bk_osk_visible(app->osk);
    if (show && exclusive(app))
        bk_overlay_set_visible(app->overlay, FALSE, FALSE);
    bk_osk_set_visible(app->osk, show);
}

static void
act_show(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    bk_osk_set_visible(((BkApp *)data)->osk, TRUE);
}

static void
act_hide(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    bk_osk_set_visible(((BkApp *)data)->osk, FALSE);
}

static void
act_settings(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    const char *page = p ? g_variant_get_string(p, NULL) : NULL;
    bk_settings_show(data, page && *page ? page : NULL);
}

static void
act_gamepad(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    BkApp *app = data;
    gboolean show = !bk_overlay_visible(app->overlay);
    if (show && exclusive(app))
        bk_osk_set_visible(app->osk, FALSE);
    bk_overlay_set_visible(app->overlay, show, FALSE);
}

static void
act_edit_gamepad(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    bk_overlay_set_visible(((BkApp *)data)->overlay, TRUE, TRUE);
}

static void
act_set_gamepad(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    BkApp *app = data;
    bk_overlay_set_visible(app->overlay, g_variant_get_boolean(p), FALSE);
}

static void
save_config(BkApp *app)
{
    GError *err = NULL;
    if (!bk_config_save(&app->cfg, NULL, &err)) {
        g_warning("configurações: %s", err->message);
        g_clear_error(&err);
    }
}

static void
act_remote_touch(GSimpleAction *a, GVariant *v, gpointer data)
{
    BkApp *app = data;
    const char *mode = g_variant_get_string(v, NULL);
    if (strcmp(mode, "off") && strcmp(mode, "exclusive") && strcmp(mode, "shared") && strcmp(mode, "mouse"))
        return; /* valor desconhecido: o estado não muda */
    g_simple_action_set_state(a, v);
    if (strcmp(app->cfg.remote_touch, mode)) {
        g_strlcpy(app->cfg.remote_touch, mode, sizeof app->cfg.remote_touch);
        save_config(app);
    }
    bk_remote_restart(app->remote);
}

static void
act_overlay_output(GSimpleAction *a, GVariant *v, gpointer data)
{
    BkApp *app = data;
    const char *name = g_variant_get_string(v, NULL);
    if (!bk_config_valid_output(name))
        return;
    g_simple_action_set_state(a, v);
    if (strcmp(app->cfg.overlay_output, name)) {
        g_strlcpy(app->cfg.overlay_output, name, sizeof app->cfg.overlay_output);
        save_config(app);
        bk_overlay_output_changed(app->overlay);
    }
}

static void
act_hybrid(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    BkApp *app = data;
    gboolean on = !(bk_osk_visible(app->osk) && bk_overlay_visible(app->overlay));
    bk_overlay_set_visible(app->overlay, on, FALSE);
    bk_osk_set_visible(app->osk, on);
}

static void
act_release(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    bk_app_release_all(data);
}

static void
act_quit(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    BkApp *app = data;
    bk_app_release_all(app);
    g_application_quit(G_APPLICATION(app->gtk));
}

static void
act_mode(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    (void)data; /* só estado, para o dockapp */
}

static gboolean
on_signal(gpointer data)
{
    act_quit(NULL, NULL, data);
    return G_SOURCE_REMOVE;
}

static void
on_startup(GApplication *g, gpointer data)
{
    BkApp *app = data;
    bk_config_load(&app->cfg, NULL);
    app->dark = read_dark(app);
    app->pad_sources = g_array_new(FALSE, FALSE, sizeof(guint));
    app->inject = bk_inject_new(gdk_display_get_default());
    if (!bk_inject_backend(app->inject))
        g_warning("sem backend de teclado virtual (XTest ou zwp_virtual_keyboard_v1): as teclas não chegam aos aplicativos");
    static const GActionEntry entries[] = {
        { "toggle", act_toggle, NULL, NULL, NULL, { 0 } },
        { "show", act_show, NULL, NULL, NULL, { 0 } },
        { "hide", act_hide, NULL, NULL, NULL, { 0 } },
        { "settings", act_settings, "s", NULL, NULL, { 0 } },
        { "gamepad", act_gamepad, NULL, NULL, NULL, { 0 } },
        { "edit-gamepad", act_edit_gamepad, NULL, NULL, NULL, { 0 } },
        { "hybrid", act_hybrid, NULL, NULL, NULL, { 0 } },
        { "release", act_release, NULL, NULL, NULL, { 0 } },
        { "quit", act_quit, NULL, NULL, NULL, { 0 } },
        { "mode", act_mode, NULL, "'hidden'", NULL, { 0 } },
        { "pad-connected", act_mode, NULL, "false", NULL, { 0 } },
        { "set-gamepad", act_set_gamepad, "b", NULL, NULL, { 0 } },
        { "remote-touch", NULL, "s", "'exclusive'", act_remote_touch, { 0 } },
        { "overlay-output", NULL, "s", "''", act_overlay_output, { 0 } },
        { "remote-touch-status", act_mode, NULL, "'idle'", NULL, { 0 } },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(g), entries, G_N_ELEMENTS(entries), app);
    GAction *rta = g_action_map_lookup_action(G_ACTION_MAP(g), "remote-touch");
    g_simple_action_set_state(G_SIMPLE_ACTION(rta), g_variant_new_string(app->cfg.remote_touch));
    GAction *oa = g_action_map_lookup_action(G_ACTION_MAP(g), "overlay-output");
    g_simple_action_set_state(G_SIMPLE_ACTION(oa), g_variant_new_string(app->cfg.overlay_output));
    app->osk = bk_osk_new(app);
    app->overlay = bk_overlay_new(app);
    app->remote = bk_remote_new(app);
    pad_apply_config(app);
    focus_apply_config(app);
    char *theme = g_build_filename(g_get_user_config_dir(), "big-wm", "theme", NULL);
    GFile *f = g_file_new_for_path(theme);
    GFileMonitor *m = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (m)
        g_signal_connect(m, "changed", G_CALLBACK(on_theme_changed), app);
    g_object_unref(f);
    g_free(theme);
    g_unix_signal_add(SIGTERM, on_signal, app);
    g_unix_signal_add(SIGINT, on_signal, app);
    g_application_hold(g); /* residente: o dockapp e o controle chamam a qualquer hora */
    bk_app_update_state(app);
}

static int
on_command_line(GApplication *g, GApplicationCommandLine *cl, gpointer data)
{
    BkApp *app = data;
    GVariantDict *o = g_application_command_line_get_options_dict(cl);
    /* ajustes primeiro (podem vir junto de --gamepad) */
    const char *rmode = NULL, *output = NULL;
    if (g_variant_dict_lookup(o, "remote-touch", "&s", &rmode))
        g_action_group_change_action_state(G_ACTION_GROUP(g), "remote-touch", g_variant_new_string(rmode));
    if (g_variant_dict_lookup(o, "output", "&s", &output))
        g_action_group_change_action_state(G_ACTION_GROUP(g), "overlay-output", g_variant_new_string(output));
    const char *acts[] = { "show", "hide", "toggle", "gamepad", "edit-gamepad", "hybrid", "release", "quit" };
    for (size_t i = 0; i < G_N_ELEMENTS(acts); i++)
        if (g_variant_dict_contains(o, acts[i])) {
            g_action_group_activate_action(G_ACTION_GROUP(g), acts[i], NULL);
            return 0;
        }
    const char *page = NULL;
    g_variant_dict_lookup(o, "page", "&s", &page);
    if (g_variant_dict_contains(o, "settings") || page) {
        bk_settings_show(app, page && *page ? page : NULL);
        return 0;
    }
    if (g_variant_dict_contains(o, "daemon") || rmode || output)
        return 0;
    /* sem opções (menu de aplicativos): abre ou fecha o teclado */
    g_action_group_activate_action(G_ACTION_GROUP(g), "toggle", NULL);
    return 0;
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("big-keyboard", LOCALEDIR);
    bind_textdomain_codeset("big-keyboard", "UTF-8");
    textdomain("big-keyboard");
    BkApp *app = &the_app;
    app->gtk = gtk_application_new(BK_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    static const GOptionEntry opts[] = {
        { "show", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Show the keyboard"), NULL },
        { "hide", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Hide the keyboard"), NULL },
        { "toggle", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Show or hide the keyboard"), NULL },
        { "settings", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Open the settings"), NULL },
        { "page", 0, 0, G_OPTION_ARG_STRING, NULL,
          N_("Settings section (general, keyboard, gamepad, overlay, remote, games, xcloud, lock, advanced)"),
          N_("SECTION") },
        { "gamepad", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Show or hide the on-screen gamepad"), NULL },
        { "edit-gamepad", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Edit the on-screen gamepad"), NULL },
        { "hybrid", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Keyboard and gamepad together"), NULL },
        { "release", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Release all keys and buttons (emergency)"), NULL },
        { "daemon", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Start hidden (session start)"), NULL },
        { "remote-touch", 0, 0, G_OPTION_ARG_STRING, NULL,
          N_("Phone touch through Moonlight on the on-screen gamepad: off, exclusive (gamepad only), "
             "mouse (also the mouse with everything hidden), shared"), N_("MODE") },
        { "output", 0, 0, G_OPTION_ARG_STRING, NULL,
          N_("Monitor for the on-screen gamepad (connector, such as DP-1; empty = primary)"), N_("CONNECTOR") },
        { "quit", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Quit"), NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL },
    };
    g_application_add_main_option_entries(G_APPLICATION(app->gtk), opts);
    g_signal_connect(app->gtk, "startup", G_CALLBACK(on_startup), app);
    g_signal_connect(app->gtk, "command-line", G_CALLBACK(on_command_line), app);
    int status = g_application_run(G_APPLICATION(app->gtk), argc, argv);
    if (app->vpad)
        bk_vpad_free(app->vpad);
    bk_inject_free(app->inject);
    if (app->pad_open)
        bwg_close(&app->pad);
    g_object_unref(app->gtk);
    return status;
}

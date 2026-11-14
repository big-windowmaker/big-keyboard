/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * big-keyboard: o aplicativo. Uma instância por sessão (GtkApplication
 * io.github.big_windowmaker.BigKeyboard); as ações saem pelo D-Bus para o
 * dockapp e para a linha de comando:
 *
 *   big-keyboard [--toggle|--show|--hide|--settings|--gamepad|--edit-gamepad
 *                 |--release|--daemon|--quit]
 *
 * Fica residente (--daemon, no início da sessão) só com descritores
 * parados: o controle físico (udev + evdev) e o arquivo do tema. Nada de
 * polling; o teclado fechado não desenha.
 */
#include "bk-app.h"

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
    GAction *p = g_action_map_lookup_action(G_ACTION_MAP(app->gtk), "pad-connected");
    if (p)
        g_simple_action_set_state(G_SIMPLE_ACTION(p), g_variant_new_boolean(app->pad_open && app->pad.npads > 0));
}

void
bk_app_release_all(BkApp *app)
{
    if (app->osk)
        bk_osk_release(app->osk);
    if (app->overlay)
        bk_overlay_release(app->overlay);
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
        bk_osk_pad_sticks(app->osk, bwg_first(&app->pad), app->cfg.deadzone);
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

void
bk_app_config_changed(BkApp *app)
{
    app->dark = read_dark(app);
    pad_apply_config(app);
    if (app->osk)
        bk_osk_reload(app->osk);
}

/* ---- ações ---------------------------------------------------------------- */

static void
act_toggle(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    BkApp *app = data;
    bk_osk_set_visible(app->osk, !bk_osk_visible(app->osk));
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
    bk_overlay_set_visible(app->overlay, !bk_overlay_visible(app->overlay), FALSE);
}

static void
act_edit_gamepad(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    bk_overlay_set_visible(((BkApp *)data)->overlay, TRUE, TRUE);
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
    };
    g_action_map_add_action_entries(G_ACTION_MAP(g), entries, G_N_ELEMENTS(entries), app);
    app->osk = bk_osk_new(app);
    app->overlay = bk_overlay_new(app);
    pad_apply_config(app);
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
    if (g_variant_dict_contains(o, "daemon"))
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
        { "show", 0, 0, G_OPTION_ARG_NONE, NULL, "Mostrar o teclado", NULL },
        { "hide", 0, 0, G_OPTION_ARG_NONE, NULL, "Ocultar o teclado", NULL },
        { "toggle", 0, 0, G_OPTION_ARG_NONE, NULL, "Mostrar ou ocultar o teclado", NULL },
        { "settings", 0, 0, G_OPTION_ARG_NONE, NULL, "Abrir as configurações", NULL },
        { "page", 0, 0, G_OPTION_ARG_STRING, NULL, "Seção das configurações (general, keyboard, gamepad, overlay, games, xcloud, lock, advanced)", "SEÇÃO" },
        { "gamepad", 0, 0, G_OPTION_ARG_NONE, NULL, "Mostrar ou ocultar o controle na tela", NULL },
        { "edit-gamepad", 0, 0, G_OPTION_ARG_NONE, NULL, "Editar o controle na tela", NULL },
        { "hybrid", 0, 0, G_OPTION_ARG_NONE, NULL, "Teclado e controle juntos", NULL },
        { "release", 0, 0, G_OPTION_ARG_NONE, NULL, "Soltar todas as teclas e botões (emergência)", NULL },
        { "daemon", 0, 0, G_OPTION_ARG_NONE, NULL, "Iniciar oculto (início da sessão)", NULL },
        { "quit", 0, 0, G_OPTION_ARG_NONE, NULL, "Encerrar", NULL },
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

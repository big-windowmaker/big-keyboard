/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Configurações do Big WM Virtual Keyboard: uma janela GTK (o tema da
 * sessão, OneStepBack, dá o visual Window Maker) com seções à esquerda.
 * Cada mudança vale na hora e é gravada (bk_config_save, atômico).
 */
#include "bk-app.h"
#include "bk-controls.h"

#include <fcntl.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    BkApp *app;
    GtkWidget *win, *stack, *pads_label, *tester, *layouts, *remote_state;
    /* aparência do layout atual (controle na tela) */
    GtkWidget *look[6], *preview, *templates;
    gboolean updating;
    gulong remote_state_id;
} Settings;

static Settings *S;

static void
save(Settings *s)
{
    GError *err = NULL;
    if (!bk_config_save(&s->app->cfg, NULL, &err)) {
        g_warning("não foi possível gravar as preferências: %s", err->message);
        g_clear_error(&err);
    }
    bk_app_config_changed(s->app);
}

/* ---- construtores --------------------------------------------------------- */

static GtkWidget *
page(const char *title, const char *intro)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    GtkWidget *t = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<span size='large' weight='bold'>%s</span>", title);
    gtk_label_set_markup(GTK_LABEL(t), m);
    g_free(m);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    if (intro) {
        GtkWidget *i = gtk_label_new(intro);
        gtk_label_set_line_wrap(GTK_LABEL(i), TRUE);
        gtk_label_set_xalign(GTK_LABEL(i), 0);
        gtk_label_set_max_width_chars(GTK_LABEL(i), 70);
        gtk_box_pack_start(GTK_BOX(box), i, FALSE, FALSE, 0);
    }
    return box;
}

static GtkWidget *
row(GtkWidget *box, const char *label, const char *tip, GtkWidget *control)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_size_request(l, 230, -1);
    gtk_box_pack_start(GTK_BOX(r), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(r), control, TRUE, TRUE, 0);
    if (tip) {
        gtk_widget_set_tooltip_text(l, tip);
        gtk_widget_set_tooltip_text(control, tip);
    }
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), control);
    gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    return control;
}

static void
on_switch(GtkSwitch *w, GParamSpec *ps, gpointer field)
{
    (void)ps;
    *(gboolean *)field = gtk_switch_get_active(w);
    save(S);
}

static GtkWidget *
sw(gboolean *field)
{
    GtkWidget *w = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(w), *field);
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    g_signal_connect(w, "notify::active", G_CALLBACK(on_switch), field);
    return w;
}

static void
on_scale(GtkRange *r, gpointer field)
{
    *(double *)field = gtk_range_get_value(r);
    save(S);
}

static GtkWidget *
scale(double *field, double lo, double hi, double step)
{
    GtkWidget *w = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    gtk_range_set_value(GTK_RANGE(w), *field);
    gtk_scale_set_value_pos(GTK_SCALE(w), GTK_POS_RIGHT);
    g_signal_connect(w, "value-changed", G_CALLBACK(on_scale), field);
    return w;
}

static void
on_spin(GtkSpinButton *b, gpointer field)
{
    *(int *)field = gtk_spin_button_get_value_as_int(b);
    save(S);
}

static GtkWidget *
spin(int *field, int lo, int hi, int step)
{
    GtkWidget *w = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), *field);
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    g_signal_connect(w, "value-changed", G_CALLBACK(on_spin), field);
    return w;
}

typedef struct {
    char *field;
    size_t len;
} StrField;

static void
on_combo(GtkComboBox *c, gpointer data)
{
    StrField *f = data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id)
        g_strlcpy(f->field, id, f->len);
    save(S);
}

/* ids e rótulos alternados, terminados em NULL */
static GtkWidget *
combo(char *field, size_t len, const char *const *items)
{
    GtkWidget *w = gtk_combo_box_text_new();
    for (int i = 0; items[i]; i += 2)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w), items[i], _(items[i + 1]));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(w), field);
    StrField *f = g_new(StrField, 1);
    f->field = field;
    f->len = len;
    g_object_set_data_full(G_OBJECT(w), "bk-field", f, g_free);
    g_signal_connect(w, "changed", G_CALLBACK(on_combo), f);
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    return w;
}

static GtkWidget *
note(GtkWidget *box, const char *markup)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

static GtkWidget *
button(GtkWidget *box, const char *label, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_signal_connect(b, "clicked", cb, data);
    gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    return b;
}

static GtkWidget *
hbox(GtkWidget *box)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box), h, FALSE, FALSE, 0);
    return h;
}

/* ---- Geral ---------------------------------------------------------------- */

static char *
autostart_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart", "big-keyboard.desktop", NULL);
}

/* Início da sessão: o pacote instala /etc/xdg/autostart/big-keyboard.desktop;
 * desligar grava uma cópia do usuário com Hidden=true. */
static gboolean
autostart_on(void)
{
    char *p = autostart_path();
    GKeyFile *k = g_key_file_new();
    gboolean on = TRUE;
    if (g_key_file_load_from_file(k, p, G_KEY_FILE_NONE, NULL))
        on = !g_key_file_get_boolean(k, "Desktop Entry", "Hidden", NULL);
    g_key_file_free(k);
    g_free(p);
    return on;
}

static void
on_autostart(GtkSwitch *w, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    char *p = autostart_path();
    if (gtk_switch_get_active(w)) {
        g_unlink(p);
    } else {
        char *dir = g_path_get_dirname(p);
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        g_file_set_contents(p, "[Desktop Entry]\nType=Application\nName=Big WM Virtual Keyboard\n"
                               "Exec=big-keyboard --daemon\nHidden=true\n", -1, NULL);
    }
    g_free(p);
}

static GtkWidget *
page_general(Settings *s)
{
    GtkWidget *p = page(_("General"), _("Big WM Virtual Keyboard stays resident and quiet (using no CPU) so that the dockapp, the "
                                   "controller and the shortcut open the keyboard instantly."));
    GtkWidget *a = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(a), autostart_on());
    gtk_widget_set_halign(a, GTK_ALIGN_START);
    g_signal_connect(a, "notify::active", G_CALLBACK(on_autostart), NULL);
    row(p, _("Start with the session"), NULL, a);
    static const char *const themes[] = { "auto", N_("Follow Big WindowMaker"), "light", N_("Light (OPENSTEP)"),
                                          "dark", N_("Dark"), NULL };
    row(p, _("Theme"), NULL, combo(s->app->cfg.theme, sizeof s->app->cfg.theme, themes));
    static const char *const modes[] = { "independent", N_("Independent (keyboard and gamepad together)"), "exclusive",
                                         N_("Exclusive (opening one hides the other)"), NULL };
    row(p, _("Display mode"), _("Applies to the dockapp buttons, the menu and the command line"),
        combo(s->app->cfg.display_mode, sizeof s->app->cfg.display_mode, modes));
    row(p, _("Key size"), _("Larger keys help with touch and accessibility"),
        scale(&s->app->cfg.scale, 0.6, 2.0, 0.05));
    row(p, _("Opacity"), _("Needs a compositor (picom on X11; wlmaker on Wayland)"),
        scale(&s->app->cfg.opacity, 0.4, 1.0, 0.05));
    note(p, _("<small>Language: the system one (LANG). Screen scale: the monitor's, automatic.</small>"));
    return p;
}

/* ---- Teclado -------------------------------------------------------------- */

static void
on_restore_place(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    s->app->cfg.float_x = s->app->cfg.float_y = -1;
    s->app->cfg.float_width = 0;
    save(s);
}

static GtkWidget *
page_keyboard(Settings *s)
{
    GtkWidget *p = page(_("Keyboard"), _("Click, tap or use the controller. Modifiers (Shift, Ctrl, Alt, AltGr, Super): one tap "
                                     "applies to the next key, two taps lock it (green LED), the third releases it. Fn shows "
                                     "F1–F12 and navigation; ☺ shows symbols and emojis."));
    static const char *const layouts[] = { "auto", N_("System layout"), "br", N_("Portuguese (ABNT2)"), "us", N_("English (US)"),
                                           "us-intl", N_("English (US international)"), NULL };
    row(p, _("Layout"), NULL, combo(s->app->cfg.layout, sizeof s->app->cfg.layout, layouts));
    static const char *const wmodes[] = { "floating", N_("Floating window, with a frame (move and resize)"),
                                          "docked", N_("Docked at the bottom, full screen width (like on a phone)"), NULL };
    row(p, _("Window"), _("Floating: drag it by the title and resize it by the bottom edge; the position and size "
                       "are saved. Docked: takes the bottom of the whole screen."),
        combo(s->app->cfg.window_mode, sizeof s->app->cfg.window_mode, wmodes));
    static const char *const stackings[] = { "normal", N_("Like the other windows"), "above",
                                             N_("Above windows (recommended)"), "fullscreen",
                                             N_("Above everything, even games, videos and full-screen presentations"),
                                             NULL };
    row(p, _("Stay on top"), _("On Wayland the keyboard always stays above regular windows"),
        combo(s->app->cfg.stacking, sizeof s->app->cfg.stacking, stackings));
    row(p, _("Open when tapping a text field"), _("Like on a phone: the keyboard appears when a field you type in gets the focus (through "
                                                  "accessibility, AT-SPI)"),
        sw(&s->app->cfg.auto_show));
    row(p, _("Close when leaving the text field"), _("Only when it opened by itself"), sw(&s->app->cfg.auto_hide));
    row(p, _("Reserve the keyboard space"), _("Docked: windows move up so the keyboard does not cover the text field"),
        sw(&s->app->cfg.reserve));
    row(p, _("Close after Enter"), NULL, sw(&s->app->cfg.autohide));
    row(p, _("Repeat delay (ms)"), _("Holding a key repeats it after this time"),
        spin(&s->app->cfg.repeat_delay, 150, 2000, 50));
    row(p, _("Repeat interval (ms)"), NULL, spin(&s->app->cfg.repeat_rate, 20, 500, 10));
    button(hbox(p), _("_Restore the window position and size"), G_CALLBACK(on_restore_place), s);
    note(p, _("<small>Applications that expose accessibility (GTK, Qt, Firefox, Chromium, LibreOffice) "
              "report when a text field gets the focus. The key size (in General) applies again when it "
              "is changed.</small>"));
    return p;
}

/* ---- Controle físico ------------------------------------------------------ */

static void
list_pads(Settings *s)
{
    GString *t = g_string_new(NULL);
    for (int i = 0; s->app->pad_open && i < s->app->pad.npads; i++) {
        char name[128] = "?";
        ioctl(s->app->pad.pad[i].fd, EVIOCGNAME(sizeof name), name);
        g_string_append_printf(t, "🎮 %s <small>(%s)</small>\n", name, s->app->pad.pad[i].node);
    }
    if (!t->len)
        g_string_assign(t, _("No controller connected. Connect one via USB or Bluetooth; it shows up here right away."));
    gtk_label_set_markup(GTK_LABEL(s->pads_label), t->str);
    g_string_free(t, TRUE);
}

static gboolean
on_tester_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Settings *s = data;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_set_source_rgb(cr, 0.667, 0.667, 0.667);
    cairo_paint(cr);
    bk_draw_bevel(cr, 0, 0, W, H, FALSE, FALSE);
    const bwg_pad *p = s->app->pad_open ? bwg_active(&s->app->pad) : NULL;
    double sx[2] = { p ? p->sx : 0, p ? p->rx : 0 }, sy[2] = { p ? p->sy : 0, p ? p->ry : 0 };
    for (int i = 0; i < 2; i++) {
        double cx = W * (i ? 0.72 : 0.28), cy = H * 0.5, r = H * 0.32;
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, i ? 0.95 : 0.1, i ? 0.55 : 0.45, i ? 0.1 : 0.95);
        cairo_arc(cr, cx + sx[i] * r, cy + sy[i] * r, 6, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    double lt = p ? p->lt : 0, rt = p ? p->rt : 0;
    cairo_set_source_rgb(cr, 0.1, 0.45, 0.95);
    cairo_rectangle(cr, 8, H - 8 - (H - 16) * lt, 8, (H - 16) * lt);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.95, 0.55, 0.1);
    cairo_rectangle(cr, W - 16, H - 8 - (H - 16) * rt, 8, (H - 16) * rt);
    cairo_fill(cr);
    return TRUE;
}

/* Chamado pelo aplicativo a cada evento do controle: o testador redesenha
 * só quando algo muda (sem relógio). */
void
bk_settings_pad_event(BkApp *app)
{
    static int last_n = -1;
    if (!app->settings || !S || !S->tester)
        return;
    if (app->pad.npads != last_n) {
        last_n = app->pad.npads;
        list_pads(S);
    }
    gtk_widget_queue_draw(S->tester);
}

static GtkWidget *
page_pad(Settings *s)
{
    GtkWidget *p = page(_("Physical controller"),
                        _("D-pad: cursor from key to key. Sticks (point mode, like the SteamOS keyboard): the left "
                          "one points on the left half (blue cursor) and the right one on the right half (orange); "
                          "LT and RT press the key on each side."));
    row(p, _("Use the controller on the keyboard"), NULL, sw(&s->app->cfg.pad));
    row(p, _("Select + Start opens the keyboard"), _("From anywhere, even with the keyboard closed"),
        sw(&s->app->cfg.pad_combo));
    static const char *const modes[] = { "both", N_("D-pad and sticks (recommended)"), "cursor",
                                         N_("Cursor only (D-pad and left stick)"), "point",
                                         N_("Pointing with the sticks only"), NULL };
    row(p, _("Navigation"), NULL, combo(s->app->cfg.pad_mode, sizeof s->app->cfg.pad_mode, modes));
    row(p, _("Stick dead zone"), NULL, scale(&s->app->cfg.deadzone, 0.05, 0.6, 0.01));
    s->pads_label = note(p, "");
    list_pads(s);
    s->tester = gtk_drawing_area_new();
    gtk_widget_set_size_request(s->tester, -1, 120);
    g_signal_connect(s->tester, "draw", G_CALLBACK(on_tester_draw), s);
    gtk_widget_set_tooltip_text(s->tester, _("Tester: move the sticks and the triggers"));
    gtk_box_pack_start(GTK_BOX(p), s->tester, FALSE, FALSE, 0);
    note(p, _("<b>Buttons on the keyboard</b>\n"
              "A ✕: press the key · B ○: close · X □: delete · Y △: space\n"
              "LB L1: Shift · RB R1: next panel (letters, Fn, symbols) · LT/RT: Shift/AltGr (or press, "
              "in point mode)\n"
              "Start: Enter · Select: switch the layout · L3: Caps · R3: Tab · Select+Start: open/close"));
    return p;
}

/* ---- Controle na tela ----------------------------------------------------- */

static void
fill_layouts(Settings *s)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->layouts));
    char *dir = g_build_filename(g_get_user_data_dir(), "big-keyboard", "gamepad-layouts", NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    gboolean has_current = FALSE;
    const char *f;
    while (d && (f = g_dir_read_name(d)))
        if (g_str_has_suffix(f, ".ini")) {
            char *name = g_strndup(f, strlen(f) - 4);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->layouts), name, name);
            has_current |= !strcmp(name, s->app->cfg.overlay_layout);
            g_free(name);
        }
    if (d)
        g_dir_close(d);
    g_free(dir);
    if (!has_current)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->layouts), s->app->cfg.overlay_layout,
                                  s->app->cfg.overlay_layout);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->layouts), s->app->cfg.overlay_layout);
}

static void look_refresh(Settings *s);

static void
on_layout_changed(GtkComboBox *c, gpointer data)
{
    Settings *s = data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id && strcmp(id, s->app->cfg.overlay_layout)) {
        g_strlcpy(s->app->cfg.overlay_layout, id, sizeof s->app->cfg.overlay_layout);
        save(s);
        look_refresh(s);
        bk_overlay_reload(s->app->overlay);
    }
}

static void
on_show_overlay(GtkButton *b, gpointer data)
{
    (void)b;
    g_action_group_activate_action(G_ACTION_GROUP(((Settings *)data)->app->gtk), "gamepad", NULL);
}

static void
on_edit_overlay(GtkButton *b, gpointer data)
{
    (void)b;
    g_action_group_activate_action(G_ACTION_GROUP(((Settings *)data)->app->gtk), "edit-gamepad", NULL);
}

static char *
ask_name(Settings *s, const char *title, const char *initial)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(s->win), GTK_DIALOG_MODAL, _("_Cancel"),
                                               GTK_RESPONSE_CANCEL, _("_OK"), GTK_RESPONSE_OK, NULL);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), initial);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), 12);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), e);
    gtk_widget_show_all(d);
    char *name = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK && *gtk_entry_get_text(GTK_ENTRY(e)))
        name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
    gtk_widget_destroy(d);
    return name;
}

static void
current_layout(Settings *s, BkCtlLayout *l)
{
    char *path = bk_ctl_path(s->app->cfg.overlay_layout);
    if (!bk_ctl_load(l, path, NULL))
        bk_ctl_default(l);
    g_free(path);
}

static void
on_duplicate(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    char *init = g_strdup_printf(_("%s (copy)"), s->app->cfg.overlay_layout);
    char *name = ask_name(s, _("Duplicate layout"), init);
    g_free(init);
    if (!name)
        return;
    BkCtlLayout l;
    current_layout(s, &l);
    g_strlcpy(l.name, name, sizeof l.name);
    char *path = bk_ctl_path(name);
    if (bk_ctl_save(&l, path, NULL)) {
        g_strlcpy(s->app->cfg.overlay_layout, name, sizeof s->app->cfg.overlay_layout);
        save(s);
    }
    g_free(path);
    g_free(name);
    fill_layouts(s);
}

static void
on_delete(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, _("Delete the layout “%s”?"), s->app->cfg.overlay_layout);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        char *path = bk_ctl_path(s->app->cfg.overlay_layout);
        g_unlink(path);
        g_free(path);
        g_strlcpy(s->app->cfg.overlay_layout, "padrão", sizeof s->app->cfg.overlay_layout);
        save(s);
        fill_layouts(s);
    }
    gtk_widget_destroy(d);
}

static void
on_import(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    GtkFileChooserNative *fc = gtk_file_chooser_native_new(_("Import layout"), GTK_WINDOW(s->win),
                                                           GTK_FILE_CHOOSER_ACTION_OPEN, _("_Import"), _("_Cancel"));
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        BkCtlLayout l;
        GError *err = NULL;
        if (bk_ctl_load(&l, f, &err)) {
            char *path = bk_ctl_path(l.name);
            bk_ctl_save(&l, path, NULL); /* regravado validado, não copiado */
            g_strlcpy(s->app->cfg.overlay_layout, l.name, sizeof s->app->cfg.overlay_layout);
            save(s);
            g_free(path);
        } else {
            GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                  GTK_BUTTONS_CLOSE, _("Invalid layout: %s"), err->message);
            gtk_dialog_run(GTK_DIALOG(m));
            gtk_widget_destroy(m);
            g_clear_error(&err);
        }
        g_free(f);
        fill_layouts(s);
    }
    g_object_unref(fc);
}

static void
on_export(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    GtkFileChooserNative *fc = gtk_file_chooser_native_new(_("Export layout"), GTK_WINDOW(s->win),
                                                           GTK_FILE_CHOOSER_ACTION_SAVE, _("_Export"), _("_Cancel"));
    char *def = g_strdup_printf("%s.ini", s->app->cfg.overlay_layout);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), def);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    g_free(def);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        BkCtlLayout l;
        current_layout(s, &l);
        bk_ctl_save(&l, f, NULL);
        g_free(f);
    }
    g_object_unref(fc);
}

static void
on_restore_layout(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    BkCtlLayout l;
    bk_ctl_default(&l);
    g_strlcpy(l.name, s->app->cfg.overlay_layout, sizeof l.name);
    char *path = bk_ctl_path(l.name);
    bk_ctl_save(&l, path, NULL);
    g_free(path);
}

/* ---- Celular (toque remoto) ---------------------------------------------- */

static const char *
remote_state_text(const char *st)
{
    static const struct {
        const char *id, *text;
    } t[] = {
        { "off", N_("Off.") },
        { "idle", N_("Ready: the phone touch works when the keyboard or the on-screen gamepad is open.") },
        { "waiting", N_("Open, waiting for a phone (no Sunshine touchscreen connected).") },
        { "active", N_("Phone connected: fingers reach only this (exclusive).") },
        { "shared", N_("Phone connected: fingers reach this and also the desktop.") },
        { "busy", N_("Phone connected, but another program already grabbed the touch: the mouse by touch is off.") },
        { "denied", N_("No permission to read the phone touch (the package udev rule is missing).") },
        { "error", N_("The phone touch could not be read.") },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(t); i++)
        if (!strcmp(t[i].id, st))
            return _(t[i].text);
    return st;
}

static void
on_remote_state(GActionGroup *g, const char *name, GVariant *v, gpointer data)
{
    (void)g;
    (void)name;
    Settings *s = data;
    char *m = g_markup_printf_escaped(_("<b>Now:</b> %s"), remote_state_text(g_variant_get_string(v, NULL)));
    gtk_label_set_markup(GTK_LABEL(s->remote_state), m);
    g_free(m);
}

static GtkWidget *
page_remote(Settings *s)
{
    GtkWidget *p = page(_("Phone"),
                        _("When playing from a phone (Moonlight with native touch, like Big Remote Play Controller), "
                          "the fingers arrive through the Sunshine virtual touchscreen. With the keyboard or the "
                          "on-screen gamepad open, each finger goes to whatever is under it: a key, a control or the "
                          "mouse."));
    s->remote_state = note(p, "");
    GVariant *v = g_action_group_get_action_state(G_ACTION_GROUP(s->app->gtk), "remote-touch-status");
    if (v) {
        on_remote_state(NULL, NULL, v, s);
        g_variant_unref(v);
    }
    s->remote_state_id = g_signal_connect(s->app->gtk, "action-state-changed::remote-touch-status",
                                          G_CALLBACK(on_remote_state), s);
    static const char *const modes[] = { "exclusive", N_("Only the keyboard and the gamepad (recommended)"), "mouse",
                                         N_("Also the mouse with both hidden"), "shared", N_("Also the desktop"),
                                         "off", N_("Off"), NULL };
    row(p, _("Phone touch"), _("In exclusive mode, no finger turns into a click outside (and the mouse by touch works)"),
        combo(s->app->cfg.remote_touch, sizeof s->app->cfg.remote_touch, modes));
    static const char *const mice[] = { "auto", N_("Automatic (recommended)"), "touchpad", N_("Always touchpad"),
                                        "direct", N_("Always direct touch"), "off", N_("Off"), NULL };
    row(p, _("Mouse by touch"), _("Fingers outside the keyboard and the gamepad, with the keyboard open (or with both "
                                 "hidden, in the Also the mouse mode)"),
        combo(s->app->cfg.remote_mouse, sizeof s->app->cfg.remote_mouse, mice));
    row(p, _("Touchpad speed"), NULL, scale(&s->app->cfg.remote_mouse_speed, 0.5, 4.0, 0.1));
    note(p, _("<b>Automatic</b>\n"
              "• Only the keyboard open: <b>touchpad</b>. Drag a finger to move the pointer; a quick tap "
              "is a left click wherever the pointer is; hold the finger still for half a second and drag "
              "to drag with the button pressed.\n"
              "• Keyboard and on-screen gamepad together: <b>direct touch</b>. The pointer goes to the "
              "finger and left-clicks there; dragging drags.\n"
              "• In both: with one finger on the screen, a tap with another finger is a <b>right "
              "click</b>; two fingers together scroll the page.\n"
              "• Only the on-screen gamepad open: tapping outside the controls does nothing (in the "
              "middle of a game, a stray click gets in the way).\n"
              "• Keyboard and gamepad hidden, in the <b>Also the mouse with both hidden</b> mode (Big "
              "Remote Play uses it on wlmaker, which ignores the Sunshine touch): <b>touchpad</b> on the "
              "whole screen."));
    note(p, _("<small>The keyboard opens on the streamed monitor while a phone is connected. The monitor "
              "is the one of the on-screen gamepad (Big Remote Play chooses it; on the command line, "
              "<tt>--output</tt>).</small>"));
    return p;
}

/* ---- aparência do controle na tela ------------------------------------------ */

/* id, rótulo, id, rótulo... NULL; o campo em BkCtlLook (deslocamento e tamanho) */
static const char *const look_styles[] = { "classic", N_("Classic (gray bevel, OPENSTEP)"), "modern",
                                           N_("Modern (dark glass)"), "retro", N_("Retro (16-bit, strong colors)"),
                                           "neon", N_("Neon (glowing outline)"), "minimal", N_("Minimalist (outline only)"),
                                           NULL };
static const char *const look_buttons[] = { "xbox", "Xbox (A B X Y, LB RB, LT RT)", "playstation",
                                            "PlayStation (✕ ○ □ △, L1 R1, L2 R2)", "nintendo",
                                            "Nintendo (B A Y X, L R, ZL ZR)", NULL };
static const char *const look_accents[] = { "blue",   N_("Blue"),  "red",  N_("Red"), "green", N_("Green"), "purple",
                                            N_("Purple"),   "orange", N_("Orange"), "gray", N_("Gray"), NULL };
static const char *const look_sticks[] = { "fixed", N_("Fixed"), "floating", N_("Floating (the center goes where the finger touches)"),
                                           NULL };
static const char *const look_dpads[] = { "cross", N_("Cross"), "buttons", N_("Four arrows"), "disc", N_("Disc"), NULL };
static const char *const look_anims[] = { "none", N_("None"),  "sink",   N_("Sink"),  "glow", N_("Glow"),
                                          "ripple", N_("Ripple"), "bounce", N_("Shrink"), NULL };

static char *
look_field(BkCtlLook *k, int i, size_t *len)
{
    char *f[6] = { k->style, k->buttons, k->accent, k->stick, k->dpad, k->anim };
    size_t n[6] = { sizeof k->style, sizeof k->buttons, sizeof k->accent, sizeof k->stick, sizeof k->dpad,
                    sizeof k->anim };
    *len = n[i];
    return f[i];
}

static void
look_refresh(Settings *s)
{
    if (!s->look[0])
        return;
    BkCtlLayout l;
    current_layout(s, &l);
    s->updating = TRUE;
    for (int i = 0; i < 6; i++) {
        size_t len = 0;
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->look[i]), look_field(&l.look, i, &len));
    }
    s->updating = FALSE;
    gtk_widget_queue_draw(s->preview);
}

static void
on_look_changed(GtkComboBox *c, gpointer data)
{
    Settings *s = data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (s->updating || !id)
        return;
    BkCtlLayout l;
    current_layout(s, &l);
    for (int i = 0; i < 6; i++)
        if (GTK_WIDGET(c) == s->look[i]) {
            size_t len = 0;
            char *field = look_field(&l.look, i, &len);
            g_strlcpy(field, id, len);
        }
    g_strlcpy(l.name, s->app->cfg.overlay_layout, sizeof l.name);
    char *path = bk_ctl_path(l.name);
    GError *err = NULL;
    if (!bk_ctl_save(&l, path, &err)) {
        g_warning("layout: %s", err->message);
        g_clear_error(&err);
    }
    g_free(path);
    gtk_widget_queue_draw(s->preview);
    bk_overlay_reload(s->app->overlay);
}

/* Prévia 16:9 com o mesmo desenho do controle: o analógico esquerdo
 * inclinado, o direcional para cima e o botão de baixo apertados. */
static gboolean
on_preview_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Settings *s = data;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, W, H);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.18, 0.24, 0.32);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.33, 0.27, 0.22);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
    /* uma tela 1920×1080 reduzida: as proporções (e o mínimo de 44 px do
     * botão do teclado) ficam como no monitor */
    cairo_scale(cr, W / 1920.0, H / 1080.0);
    W = 1920;
    H = 1080;
    BkCtlLayout l;
    current_layout(s, &l);
    BkCtlState st;
    bk_ctl_state_init(&st);
    int t = 0;
    for (int i = 0; i < l.n && t < 3; i++) {
        const BkCtl *c = &l.c[i];
        double r = bk_ctl_px(&l, c, H) / 2;
        double dx = 0, dy = 0;
        if (c->kind == BKC_STICK && c->code == 0)
            dx = r * 0.55, dy = -r * 0.35;
        else if (c->kind == BKC_DPAD)
            dy = -r * 0.7;
        else if (!(c->kind == BKC_BUTTON && c->code == BTN_SOUTH))
            continue;
        st.used[t] = true;
        st.ctl[t] = i;
        st.ox[t] = c->x * W;
        st.oy[t] = c->y * H;
        st.px[t] = st.ox[t] + dx;
        st.py[t] = st.oy[t] + dy;
        t++;
    }
    bk_ctl_paint_layout(cr, &l, &st, W, H, bk_draw_icon_keyboard);
    return TRUE;
}

static void
on_from_template(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->templates));
    BkCtlLayout l;
    if (!id || !bk_ctl_template(&l, id))
        return;
    char *name = ask_name(s, _("New layout from the template"), _(l.name));
    if (!name)
        return;
    g_strlcpy(l.name, name, sizeof l.name);
    l.scale = s->app->cfg.overlay_scale;
    char *path = bk_ctl_path(name);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) { /* nunca sobrescreve um layout do usuário */
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                              _("A layout named “%s” already exists. Choose another name."), name);
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
    } else if (bk_ctl_save(&l, path, NULL)) {
        g_strlcpy(s->app->cfg.overlay_layout, name, sizeof s->app->cfg.overlay_layout);
        save(s);
        fill_layouts(s);
        look_refresh(s);
        bk_overlay_reload(s->app->overlay);
    }
    g_free(path);
    g_free(name);
}

static void
on_template_changed(GtkComboBox *c, gpointer data)
{
    (void)data;
    const char *id = gtk_combo_box_get_active_id(c);
    for (const BkCtlTemplate *t = bk_ctl_templates(); id && t->id; t++)
        if (!strcmp(t->id, id))
            gtk_widget_set_tooltip_text(GTK_WIDGET(c), _(t->description));
}

static GtkWidget *
page_overlay(Settings *s)
{
    GtkWidget *p = page(_("On-screen gamepad"),
                        _("Semi-transparent buttons and sticks over the game, for playing on the touch screen "
                          "(several fingers at once). Games see a regular controller (Big WM Virtual Gamepad)."));
    s->layouts = gtk_combo_box_text_new();
    fill_layouts(s);
    g_signal_connect(s->layouts, "changed", G_CALLBACK(on_layout_changed), s);
    row(p, _("Layout"), NULL, s->layouts);
    s->templates = gtk_combo_box_text_new();
    for (const BkCtlTemplate *t = bk_ctl_templates(); t->id; t++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->templates), t->id, _(t->name));
    g_signal_connect(s->templates, "changed", G_CALLBACK(on_template_changed), s);
    gtk_combo_box_set_active(GTK_COMBO_BOX(s->templates), 0);
    GtkWidget *th = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(th), s->templates, FALSE, FALSE, 0);
    GtkWidget *tb = gtk_button_new_with_mnemonic(_("_Create layout"));
    g_signal_connect(tb, "clicked", G_CALLBACK(on_from_template), s);
    gtk_box_pack_start(GTK_BOX(th), tb, FALSE, FALSE, 0);
    row(p, _("New from a template"), _("Xbox, PlayStation, Nintendo Switch, retro, arcade, racing or minimal: ready-made "
                                           "positions and look, in a new layout"), th);
    s->preview = gtk_drawing_area_new();
    gtk_widget_set_size_request(s->preview, 448, 252);
    gtk_widget_set_halign(s->preview, GTK_ALIGN_START);
    g_signal_connect(s->preview, "draw", G_CALLBACK(on_preview_draw), s);
    gtk_box_pack_start(GTK_BOX(p), s->preview, FALSE, FALSE, 0);
    const char *const *lists[6] = { look_styles, look_buttons, look_accents, look_sticks, look_dpads, look_anims };
    const char *titles[6] = { _("Style"), _("Buttons"), _("Accent color"), _("Stick"), _("D-pad"), _("Touch animation") };
    const char *tips[6] = { NULL, _("Only the names and colors change: the game sees the same buttons in the same positions"), NULL,
                            NULL, NULL, NULL };
    for (int i = 0; i < 6; i++) {
        s->look[i] = gtk_combo_box_text_new();
        for (int j = 0; lists[i][j]; j += 2)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->look[i]), lists[i][j], _(lists[i][j + 1]));
        g_signal_connect(s->look[i], "changed", G_CALLBACK(on_look_changed), s);
        gtk_widget_set_halign(s->look[i], GTK_ALIGN_START);
        row(p, titles[i], tips[i], s->look[i]);
    }
    look_refresh(s);
    row(p, _("Opacity (new layouts)"), NULL, scale(&s->app->cfg.overlay_opacity, 0.15, 1.0, 0.05));
    row(p, _("Control scale"), NULL, scale(&s->app->cfg.overlay_scale, 0.5, 2.0, 0.05));
    GtkWidget *h = hbox(p);
    button(h, _("_Show or hide"), G_CALLBACK(on_show_overlay), s);
    button(h, _("_Edit…"), G_CALLBACK(on_edit_overlay), s);
    button(h, _("_Duplicate…"), G_CALLBACK(on_duplicate), s);
    button(h, _("De_lete"), G_CALLBACK(on_delete), s);
    h = hbox(p);
    button(h, _("_Import…"), G_CALLBACK(on_import), s);
    button(h, _("E_xport…"), G_CALLBACK(on_export), s);
    button(h, _("_Restore the default"), G_CALLBACK(on_restore_layout), s);
    note(p, _("<small>In the editor: drag to move, the blue handle changes the size, yellow guides show "
              "the alignment. A control off the screen comes back inside when saving.</small>"));
    return p;
}

/* ---- Jogos ------------------------------------------------------------------ */

static void
on_release(GtkButton *b, gpointer data)
{
    (void)b;
    bk_app_release_all(((Settings *)data)->app);
}

static GtkWidget *
page_games(Settings *s)
{
    GtkWidget *p = page(_("Games"), _("The virtual controller (uinput) is created when the on-screen gamepad opens and goes away "
                                   "when it closes: games (native, Steam/Proton, Wine, browsers) see it as a regular gamepad."));
    static const char *const ids[] = { "generic", N_("Big WM Virtual Gamepad (recommended)"), "xbox360",
                                       N_("Xbox 360 compatible (only for games that require it)"), NULL };
    row(p, _("Virtual controller identity"), _("Some games only accept known controllers"),
        combo(s->app->cfg.identity, sizeof s->app->cfg.identity, ids));
    gboolean ok = access("/dev/uinput", W_OK) == 0;
    char *m = g_strdup_printf("%s <b>/dev/uinput</b>: %s", ok ? "✅" : "⚠️",
                              ok ? _("accessible (udev uaccess tag)")
                                 : _("no access — install steam-devices or game-devices-udev (uaccess rule); the program never "
                                     "runs as root"));
    note(p, m);
    g_free(m);
    GtkWidget *h = hbox(p);
    GtkWidget *rb = button(h, _("_Release everything now (emergency)"), G_CALLBACK(on_release), s);
    gtk_widget_set_tooltip_text(rb, _("Releases keys, modifiers and virtual controller buttons; also: big-keyboard --release"));
    note(p, _("<small>Do not use emulation in competitive matches that forbid macros or virtual input. "
              "One virtual controller at a time: close the on-screen gamepad before another emulation "
              "tool.</small>"));
    return p;
}

/* ---- Xbox Cloud Gaming ---------------------------------------------------- */

static void
on_open_uri(GtkButton *b, gpointer uri)
{
    (void)b;
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
}

static GtkWidget *
page_xcloud(Settings *s)
{
    GtkWidget *p = page("Xbox Cloud Gaming",
                        _("To play games that ask for a controller with keyboard and mouse, Big WM Virtual Keyboard "
                          "uses Better xCloud (MIT, redphx) — it already converts keyboard and mouse into a "
                          "controller inside the browser. There is no second competing emulation: choose one."));
    static const char *const backends[] = { "better-xcloud", N_("Better xCloud in the browser (recommended)"), "linux",
                                            N_("Linux virtual controller (touch screen)"), NULL };
    row(p, _("Mode"), NULL, combo(s->app->cfg.xcloud_backend, sizeof s->app->cfg.xcloud_backend, backends));
    GString *b = g_string_new(_("<b>Browsers found:</b> "));
    const char *browsers[] = { "firefox", "chromium", "google-chrome-stable", "brave", "vivaldi-stable",
                               "microsoft-edge-stable" };
    int found = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(browsers); i++) {
        char *path = g_find_program_in_path(browsers[i]);
        if (path) {
            g_string_append_printf(b, "%s%s", found++ ? ", " : "", browsers[i]);
            g_free(path);
        }
    }
    if (!found)
        g_string_append(b, _("none"));
    note(p, b->str);
    g_string_free(b, TRUE);
    note(p, _("<b>How to install Better xCloud</b> (from official sources only):\n"
              "1. Install a userscript manager in the browser (Violentmonkey or Tampermonkey).\n"
              "2. Open the official page and install the script with the button there.\n"
              "3. On xbox.com/play, open the Better xCloud settings → Controls → Keyboard and mouse, and "
              "choose a profile (FPS, racing...). The mouse becomes the right stick with the pointer "
              "locked.\n"
              "\n"
              "<b>Warning (November 2026):</b> Better xCloud works on <tt>xbox.com/play</tt>; if the "
              "site takes you to <tt>play.xbox.com</tt>, leave the preview from the profile menu (“Exit "
              "preview”). Fake sites: betterxcloud.com and betterxcloud.net. Not every game accepts the "
              "conversion; respect the rules of each game."));
    GtkWidget *h = hbox(p);
    button(h, _("Official Better _xCloud page"), G_CALLBACK(on_open_uri), (gpointer) "https://better-xcloud.github.io/");
    button(h, _("Open xbox.com/_play"), G_CALLBACK(on_open_uri), (gpointer) "https://www.xbox.com/play");
    note(p, _("<small>No Xbox password, token or cookie goes through Big WM Virtual Keyboard.</small>"));
    return p;
}

/* ---- Tela de bloqueio ----------------------------------------------------- */

static void
on_lock_settings(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    g_spawn_command_line_async("big-wm-lock-settings", NULL);
}

static GtkWidget *
page_lock(Settings *s)
{
    (void)s;
    GtkWidget *p = page(_("Lock screen"),
                        _("On the lock screen, the keyboard is part of the locker itself (gtklock module on Wayland, "
                          "xsecurelock auth_big_wm on X11) and uses the same controller reader as this application. "
                          "For security, this program does not type on the locked screen: the password never goes "
                          "through it, nor through any other process, log or clipboard."));
    note(p, _("<b>With a controller:</b> any button opens the keyboard; D-pad/stick move the cursor, A "
              "presses, X deletes, Y space, LB Shift, RB layout, Start unlocks. On X11, the first button "
              "also opens the password dialog."));
    GtkWidget *h = hbox(p);
    button(h, _("Open the _lock screen preferences"), G_CALLBACK(on_lock_settings), NULL);
    return p;
}

/* ---- Avançado ------------------------------------------------------------- */

static void
on_defaults(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, _("Restore all preferences to the defaults?"));
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        bk_config_defaults(&s->app->cfg);
        save(s);
        gtk_widget_destroy(s->win); /* reabre com os valores novos */
        bk_settings_show(s->app, "advanced");
    }
    gtk_widget_destroy(d);
}

static GtkWidget *
page_advanced(Settings *s)
{
    GtkWidget *p = page(_("Advanced"), NULL);
    char *cfg = bk_config_path();
    char *m = g_markup_printf_escaped(
        _("<b>Typing into applications:</b> %s\n"
          "<b>Preferences:</b> <tt>%s</tt>\n"
          "<b>On-screen gamepad layouts:</b> <tt>%s/big-keyboard/gamepad-layouts/</tt>\n"
          "<b>Command line:</b> <tt>big-keyboard --toggle | --settings | --gamepad | --release</tt>"),
        bk_inject_backend(s->app->inject) && !strcmp(bk_inject_backend(s->app->inject), "wayland")
            ? _("Wayland, zwp_virtual_keyboard_v1 protocol")
        : bk_inject_backend(s->app->inject) ? _("X11, XTest extension")
                                             : _("unavailable (neither XTest nor virtual-keyboard)"),
        cfg, g_get_user_data_dir());
    note(p, m);
    g_free(m);
    g_free(cfg);
    GtkWidget *h = hbox(p);
    button(h, _("_Restore all preferences"), G_CALLBACK(on_defaults), s);
    return p;
}

/* ---- janela ----------------------------------------------------------------- */

static void
on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    Settings *s = data;
    if (s->remote_state_id)
        g_signal_handler_disconnect(s->app->gtk, s->remote_state_id);
    s->app->settings = NULL;
    if (S == s)
        S = NULL;
    g_free(s);
}

void
bk_settings_show(BkApp *app, const char *pagename)
{
    if (app->settings) {
        if (pagename)
            gtk_stack_set_visible_child_name(GTK_STACK(S->stack), pagename);
        gtk_window_present(GTK_WINDOW(app->settings));
        return;
    }
    Settings *s = g_new0(Settings, 1);
    S = s;
    s->app = app;
    s->win = gtk_application_window_new(app->gtk);
    app->settings = s->win;
    gtk_window_set_title(GTK_WINDOW(s->win), _("Big WM Virtual Keyboard — Settings"));
    gtk_window_set_icon_name(GTK_WINDOW(s->win), "big-keyboard");
    gtk_window_set_default_size(GTK_WINDOW(s->win), 860, 600);
    g_signal_connect(s->win, "destroy", G_CALLBACK(on_destroy), s);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    s->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(s->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    struct {
        const char *id, *title;
        GtkWidget *(*build)(Settings *);
    } pages[] = { { "general", _("General"), page_general },     { "keyboard", _("Keyboard"), page_keyboard },
                  { "gamepad", _("Physical controller"), page_pad }, { "overlay", _("On-screen gamepad"), page_overlay },
                  { "remote", _("Phone"), page_remote },
                  { "games", _("Games"), page_games },         { "xcloud", "Xbox Cloud Gaming", page_xcloud },
                  { "lock", _("Lock screen"), page_lock }, { "advanced", _("Advanced"), page_advanced } };
    for (size_t i = 0; i < G_N_ELEMENTS(pages); i++) {
        GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(sc), pages[i].build(s));
        gtk_stack_add_titled(GTK_STACK(s->stack), sc, pages[i].id, pages[i].title);
    }
    GtkWidget *side = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(s->stack));
    gtk_widget_set_size_request(side, 190, -1);
    gtk_box_pack_start(GTK_BOX(box), side, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(s->win), box);
    gtk_widget_show_all(s->win);
    if (pagename)
        gtk_stack_set_visible_child_name(GTK_STACK(s->stack), pagename);
}

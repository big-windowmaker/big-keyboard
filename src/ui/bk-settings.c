/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Configurações do Big WM Virtual Keyboard: uma janela GTK (o tema da
 * sessão, OneStepBack, dá o visual Window Maker) com seções à esquerda.
 * Cada mudança vale na hora e é gravada (bk_config_save, atômico).
 */
#include "bk-app.h"
#include "bk-controls.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    BkApp *app;
    GtkWidget *win, *stack, *pads_label, *tester, *layouts;
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
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w), items[i], items[i + 1]);
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
    GtkWidget *p = page("Geral", "O Big WM Virtual Keyboard fica residente e quieto (sem consumir CPU) para o "
                                 "dockapp, o controle e o atalho abrirem o teclado na hora.");
    GtkWidget *a = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(a), autostart_on());
    gtk_widget_set_halign(a, GTK_ALIGN_START);
    g_signal_connect(a, "notify::active", G_CALLBACK(on_autostart), NULL);
    row(p, "Iniciar com a sessão", NULL, a);
    static const char *const themes[] = { "auto", "Acompanhar o Big WindowMaker", "light", "Claro (OPENSTEP)",
                                          "dark", "Escuro", NULL };
    row(p, "Tema", NULL, combo(s->app->cfg.theme, sizeof s->app->cfg.theme, themes));
    static const char *const modes[] = { "independent", "Independente (teclado e controle juntos)", "exclusive",
                                         "Exclusivo (abrir um oculta o outro)", NULL };
    row(p, "Modo de exibição", "Vale para os botões do dockapp, o menu e a linha de comando",
        combo(s->app->cfg.display_mode, sizeof s->app->cfg.display_mode, modes));
    row(p, "Tamanho das teclas", "Teclas maiores ajudam no toque e na acessibilidade",
        scale(&s->app->cfg.scale, 0.6, 2.0, 0.05));
    row(p, "Opacidade", "Precisa de um compositor (picom no X11; o wlmaker no Wayland)",
        scale(&s->app->cfg.opacity, 0.4, 1.0, 0.05));
    note(p, "<small>Idioma: o do sistema (LANG). Escala de tela: a do monitor, automática.</small>");
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
    GtkWidget *p = page("Teclado", "Clique, toque ou use o controle. Modificadores (Shift, Ctrl, Alt, AltGr, Super): "
                                   "um toque vale para a próxima tecla, dois toques travam (LED verde), o terceiro "
                                   "solta. Fn mostra F1–F12 e navegação; ☺ mostra símbolos e emojis.");
    static const char *const layouts[] = { "auto", "O do sistema", "br", "Português (ABNT2)", "us", "Inglês (US)",
                                           "us-intl", "Inglês (US internacional)", NULL };
    row(p, "Layout", NULL, combo(s->app->cfg.layout, sizeof s->app->cfg.layout, layouts));
    static const char *const wmodes[] = { "floating", "Janela flutuante, com moldura (mover e redimensionar)",
                                          "docked", "Encaixado embaixo, na largura da tela (como no celular)", NULL };
    row(p, "Janela", "Flutuante: arraste pelo título e redimensione pela borda de baixo; o lugar e o tamanho "
                     "ficam gravados. Encaixado: ocupa a parte de baixo da tela inteira.",
        combo(s->app->cfg.window_mode, sizeof s->app->cfg.window_mode, wmodes));
    static const char *const stackings[] = { "normal", "Como as outras janelas", "above",
                                             "Por cima das janelas (recomendado)", "fullscreen",
                                             "Por cima de tudo, até de jogos, vídeos e apresentações em tela cheia",
                                             NULL };
    row(p, "Ficar por cima", "No Wayland o teclado sempre fica por cima das janelas comuns",
        combo(s->app->cfg.stacking, sizeof s->app->cfg.stacking, stackings));
    row(p, "Abrir ao tocar num campo de texto", "Como no celular: o teclado aparece quando um campo onde se "
                                                "digita ganha o foco (pela acessibilidade, AT-SPI)",
        sw(&s->app->cfg.auto_show));
    row(p, "Fechar ao sair do campo de texto", "Só quando ele abriu sozinho", sw(&s->app->cfg.auto_hide));
    row(p, "Reservar o espaço do teclado", "Encaixado: as janelas sobem para o teclado não cobrir o campo de texto",
        sw(&s->app->cfg.reserve));
    row(p, "Fechar depois do Enter", NULL, sw(&s->app->cfg.autohide));
    row(p, "Atraso da repetição (ms)", "Segurar uma tecla repete depois deste tempo",
        spin(&s->app->cfg.repeat_delay, 150, 2000, 50));
    row(p, "Intervalo da repetição (ms)", NULL, spin(&s->app->cfg.repeat_rate, 20, 500, 10));
    button(hbox(p), "_Restaurar o lugar e o tamanho da janela", G_CALLBACK(on_restore_place), s);
    note(p, "<small>Aplicativos que expõem a acessibilidade (GTK, Qt, Firefox, Chromium, LibreOffice) avisam "
            "quando um campo de texto ganha o foco. O tamanho das teclas (em Geral) volta a valer ao ser "
            "mudado.</small>");
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
        g_string_assign(t, "Nenhum controle ligado. Conecte por USB ou Bluetooth; ele aparece aqui na hora.");
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
    GtkWidget *p = page("Controle físico",
                        "Direcional: cursor de tecla em tecla. Analógicos (modo apontar, como o teclado do SteamOS): "
                        "o esquerdo aponta na metade esquerda (cursor azul) e o direito na direita (laranja); LT e RT "
                        "apertam a tecla de cada lado.");
    row(p, "Usar o controle no teclado", NULL, sw(&s->app->cfg.pad));
    row(p, "Select + Start abre o teclado", "De qualquer lugar, mesmo com o teclado fechado",
        sw(&s->app->cfg.pad_combo));
    static const char *const modes[] = { "both", "Direcional e analógicos (recomendado)", "cursor",
                                         "Só cursor (direcional e analógico esquerdo)", "point",
                                         "Só apontar com os analógicos", NULL };
    row(p, "Navegação", NULL, combo(s->app->cfg.pad_mode, sizeof s->app->cfg.pad_mode, modes));
    row(p, "Zona morta dos analógicos", NULL, scale(&s->app->cfg.deadzone, 0.05, 0.6, 0.01));
    s->pads_label = note(p, "");
    list_pads(s);
    s->tester = gtk_drawing_area_new();
    gtk_widget_set_size_request(s->tester, -1, 120);
    g_signal_connect(s->tester, "draw", G_CALLBACK(on_tester_draw), s);
    gtk_widget_set_tooltip_text(s->tester, "Testador: mexa os analógicos e os gatilhos");
    gtk_box_pack_start(GTK_BOX(p), s->tester, FALSE, FALSE, 0);
    note(p, "<b>Botões no teclado</b>\n"
            "A ✕: apertar a tecla · B ○: fechar · X □: apagar · Y △: espaço\n"
            "LB L1: Shift · RB R1: próximo painel (letras, Fn, símbolos) · LT/RT: Shift/AltGr (ou apertar, no "
            "modo apontar)\n"
            "Start: Enter · Select: trocar o layout · L3: Caps · R3: Tab · Select+Start: abrir/fechar");
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

static void
on_layout_changed(GtkComboBox *c, gpointer data)
{
    Settings *s = data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id && strcmp(id, s->app->cfg.overlay_layout)) {
        g_strlcpy(s->app->cfg.overlay_layout, id, sizeof s->app->cfg.overlay_layout);
        save(s);
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
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(s->win), GTK_DIALOG_MODAL, "_Cancelar",
                                               GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
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
    char *init = g_strdup_printf("%s (cópia)", s->app->cfg.overlay_layout);
    char *name = ask_name(s, "Duplicar layout", init);
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
                                          GTK_BUTTONS_OK_CANCEL, "Excluir o layout “%s”?", s->app->cfg.overlay_layout);
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
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Importar layout", GTK_WINDOW(s->win),
                                                           GTK_FILE_CHOOSER_ACTION_OPEN, "_Importar", "_Cancelar");
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
                                                  GTK_BUTTONS_CLOSE, "Layout inválido: %s", err->message);
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
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Exportar layout", GTK_WINDOW(s->win),
                                                           GTK_FILE_CHOOSER_ACTION_SAVE, "_Exportar", "_Cancelar");
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

static GtkWidget *
page_overlay(Settings *s)
{
    GtkWidget *p = page("Controle na tela",
                        "Botões e analógicos semitransparentes sobre o jogo, para jogar na tela de toque (vários "
                        "dedos ao mesmo tempo). Os jogos veem um controle comum (Big WM Virtual Gamepad).");
    s->layouts = gtk_combo_box_text_new();
    fill_layouts(s);
    g_signal_connect(s->layouts, "changed", G_CALLBACK(on_layout_changed), s);
    row(p, "Layout", NULL, s->layouts);
    row(p, "Opacidade (novos layouts)", NULL, scale(&s->app->cfg.overlay_opacity, 0.15, 1.0, 0.05));
    row(p, "Escala dos controles", NULL, scale(&s->app->cfg.overlay_scale, 0.5, 2.0, 0.05));
    GtkWidget *h = hbox(p);
    button(h, "_Mostrar ou ocultar", G_CALLBACK(on_show_overlay), s);
    button(h, "_Editar…", G_CALLBACK(on_edit_overlay), s);
    button(h, "_Duplicar…", G_CALLBACK(on_duplicate), s);
    button(h, "E_xcluir", G_CALLBACK(on_delete), s);
    h = hbox(p);
    button(h, "_Importar…", G_CALLBACK(on_import), s);
    button(h, "Ex_portar…", G_CALLBACK(on_export), s);
    button(h, "_Restaurar o padrão", G_CALLBACK(on_restore_layout), s);
    note(p, "<small>No editor: arraste para mover, a alça azul muda o tamanho, guias amarelas mostram o "
            "alinhamento. Um controle fora da tela volta para dentro ao salvar.</small>");
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
    GtkWidget *p = page("Jogos", "O controle virtual (uinput) é criado ao abrir o controle na tela e some ao "
                                 "fechá-lo: os jogos (nativos, Steam/Proton, Wine, navegadores) o veem como um "
                                 "gamepad comum.");
    static const char *const ids[] = { "generic", "Big WM Virtual Gamepad (recomendado)", "xbox360",
                                       "Compatível com Xbox 360 (só para jogos que exigem)", NULL };
    row(p, "Identidade do controle virtual", "Alguns jogos só aceitam controles conhecidos",
        combo(s->app->cfg.identity, sizeof s->app->cfg.identity, ids));
    gboolean ok = access("/dev/uinput", W_OK) == 0;
    char *m = g_strdup_printf("%s <b>/dev/uinput</b>: %s", ok ? "✅" : "⚠️",
                              ok ? "acessível (tag uaccess do udev)"
                                 : "sem acesso — instale steam-devices ou game-devices-udev (regra uaccess); "
                                   "o programa nunca roda como root");
    note(p, m);
    g_free(m);
    GtkWidget *h = hbox(p);
    GtkWidget *rb = button(h, "_Soltar tudo agora (emergência)", G_CALLBACK(on_release), s);
    gtk_widget_set_tooltip_text(rb, "Solta teclas, modificadores e botões do controle virtual; também: "
                                    "big-keyboard --release");
    note(p, "<small>Não use emulação em partidas competitivas que proíbam macros ou entradas virtuais. Um "
            "controle virtual de cada vez: feche o controle na tela antes de outra ferramenta de "
            "emulação.</small>");
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
                        "Para jogar com teclado e mouse jogos que pedem controle, o Big WM Virtual Keyboard usa o "
                        "Better xCloud (MIT, redphx) — ele já converte teclado e mouse em controle dentro do "
                        "navegador. Não há uma segunda emulação concorrente: escolha uma.");
    static const char *const backends[] = { "better-xcloud", "Better xCloud no navegador (recomendado)", "linux",
                                            "Controle virtual do Linux (toque na tela)", NULL };
    row(p, "Modo", NULL, combo(s->app->cfg.xcloud_backend, sizeof s->app->cfg.xcloud_backend, backends));
    GString *b = g_string_new("<b>Navegadores encontrados:</b> ");
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
        g_string_append(b, "nenhum");
    note(p, b->str);
    g_string_free(b, TRUE);
    note(p, "<b>Como instalar o Better xCloud</b> (só das fontes oficiais):\n"
            "1. Instale um gerenciador de userscripts no navegador (Violentmonkey ou Tampermonkey).\n"
            "2. Abra a página oficial e instale o script pelo botão de lá.\n"
            "3. Em xbox.com/play, abra as configurações do Better xCloud → Controles → Teclado e mouse, e "
            "escolha um perfil (FPS, corrida...). O mouse vira o analógico direito com o ponteiro preso.\n\n"
            "<b>Atenção (novembro de 2026):</b> o Better xCloud funciona em <tt>xbox.com/play</tt>; se o site "
            "levar a <tt>play.xbox.com</tt>, saia da prévia pelo menu do perfil (“Exit preview”). Sites "
            "falsos: betterxcloud.com e betterxcloud.net. Nem todo jogo aceita a conversão; respeite as regras "
            "de cada jogo.");
    GtkWidget *h = hbox(p);
    button(h, "Página oficial do Better _xCloud", G_CALLBACK(on_open_uri), (gpointer) "https://better-xcloud.github.io/");
    button(h, "Abrir xbox.com/_play", G_CALLBACK(on_open_uri), (gpointer) "https://www.xbox.com/play");
    note(p, "<small>Nenhuma senha, token ou cookie do Xbox passa pelo Big WM Virtual Keyboard.</small>");
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
    GtkWidget *p = page("Tela de bloqueio",
                        "Na tela de bloqueio, o teclado é parte do próprio bloqueador (módulo do gtklock no "
                        "Wayland, auth_big_wm do xsecurelock no X11) e usa o mesmo leitor de controle deste "
                        "aplicativo. Por segurança, este programa não digita na tela bloqueada: a senha nunca "
                        "passa por ele, nem por outro processo, log ou área de transferência.");
    note(p, "<b>Com um controle:</b> qualquer botão abre o teclado; direcional/analógico movem o cursor, A aperta, "
            "X apaga, Y espaço, LB Shift, RB layout, Start desbloqueia. No X11, o primeiro botão também abre o "
            "diálogo de senha.");
    GtkWidget *h = hbox(p);
    button(h, "Abrir as preferências da _tela de bloqueio", G_CALLBACK(on_lock_settings), NULL);
    return p;
}

/* ---- Avançado ------------------------------------------------------------- */

static void
on_defaults(GtkButton *b, gpointer data)
{
    (void)b;
    Settings *s = data;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, "Restaurar todas as preferências do padrão?");
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
    GtkWidget *p = page("Avançado", NULL);
    char *cfg = bk_config_path();
    char *m = g_markup_printf_escaped(
        "<b>Digitação nos aplicativos:</b> %s\n<b>Preferências:</b> <tt>%s</tt>\n"
        "<b>Layouts do controle na tela:</b> <tt>%s/big-keyboard/gamepad-layouts/</tt>\n"
        "<b>Linha de comando:</b> <tt>big-keyboard --toggle | --settings | --gamepad | --release</tt>",
        bk_inject_backend(s->app->inject) && !strcmp(bk_inject_backend(s->app->inject), "wayland")
            ? "Wayland, protocolo zwp_virtual_keyboard_v1"
        : bk_inject_backend(s->app->inject) ? "X11, extensão XTest"
                                             : "indisponível (nem XTest nem virtual-keyboard)",
        cfg, g_get_user_data_dir());
    note(p, m);
    g_free(m);
    g_free(cfg);
    GtkWidget *h = hbox(p);
    button(h, "_Restaurar todas as preferências", G_CALLBACK(on_defaults), s);
    return p;
}

/* ---- janela ----------------------------------------------------------------- */

static void
on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    Settings *s = data;
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
    gtk_window_set_title(GTK_WINDOW(s->win), "Big WM Virtual Keyboard — Configurações");
    gtk_window_set_icon_name(GTK_WINDOW(s->win), "big-keyboard");
    gtk_window_set_default_size(GTK_WINDOW(s->win), 860, 600);
    g_signal_connect(s->win, "destroy", G_CALLBACK(on_destroy), s);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    s->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(s->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    struct {
        const char *id, *title;
        GtkWidget *(*build)(Settings *);
    } pages[] = { { "general", "Geral", page_general },     { "keyboard", "Teclado", page_keyboard },
                  { "gamepad", "Controle físico", page_pad }, { "overlay", "Controle na tela", page_overlay },
                  { "games", "Jogos", page_games },         { "xcloud", "Xbox Cloud Gaming", page_xcloud },
                  { "lock", "Tela de bloqueio", page_lock }, { "advanced", "Avançado", page_advanced } };
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

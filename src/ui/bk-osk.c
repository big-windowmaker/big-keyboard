/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * O teclado na tela da sessão: uma janela OPENSTEP (barra de título preta,
 * teclas em relevo) desenhada com cairo numa só área, para ficar idêntica ao
 * teclado da tela de bloqueio e barata de redesenhar.
 *
 * Nunca pega o foco: no Wayland é uma superfície layer-shell (camada TOP,
 * keyboard-interactivity NONE); no X11, uma janela tipo DOCK que não aceita
 * foco. Com "reservar espaço", as janelas sobem para não ficar por baixo
 * (zona exclusiva do layer-shell; _NET_WM_STRUT_PARTIAL no X11).
 *
 * Entrada: mouse e toques (cada dedo independente) apertam ao soltar, com
 * repetição ao segurar; controle físico com cursor de tecla em tecla
 * (direcional) ou dois cursores apontados pelos analógicos (estilo SteamOS,
 * gatilhos apertam). Fechado, não desenha nem acorda.
 */
#include "bk-app.h"
#include "bk-geom.h"

#include <math.h>
#include <string.h>
#ifdef BK_HAVE_X11
#include <gdk/gdkx.h>
#endif
#ifdef BK_HAVE_WAYLAND
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>
#endif

#define MAX_SLOTS 11 /* 10 dedos + o mouse */
#define FLASH_MS 90

typedef struct {
    gboolean used;
    GdkEventSequence *seq; /* NULL: o mouse */
    int key;               /* índice em geom, -1 nenhum */
    gboolean repeated;
} Slot;

struct BkOsk {
    BkApp *app;
    GtkWidget *win, *area;
    BkState st;
    BkGeom geom;
    double u, gap, pad, title_h, s;
    int width, height;
    gboolean layer;
    Slot slot[MAX_SLOTS];
    guint repeat_id;
    int repeat_slot;
    int cursor;             /* direcional: índice em geom; -1 nenhum */
    int point[2];           /* modo apontar: cursores esquerdo/direito; -1 nenhum */
    int flash;              /* tecla apertada pelo controle (feedback curto) */
    guint flash_id;
    /* arrastar pela barra de título (Wayland: margens do layer-shell) */
    gboolean dragging;
    double drag_x, drag_y;
    int margin_x, margin_y;
    /* botão do controle: troca ao soltar o dedo, não ao tocar (o controle
     * pega o touchscreen do celular; um dedo ainda na tela ficaria preso no
     * compositor) */
    gboolean switch_armed;
    GdkEventSequence *switch_seq;
    GdkMonitor *pinned; /* aberto pela troca: o monitor do controle */
};

static void osk_layout(BkOsk *osk);
static void on_monitor_removed(GdkDisplay *d, GdkMonitor *m, gpointer data);

/* ---- desenho ------------------------------------------------------------- */

void
bk_draw_bevel(cairo_t *cr, double x, double y, double w, double h, gboolean raised, gboolean dark)
{
    double hi = dark ? 0.47 : 1.0, lo = dark ? 0.0 : 0.33;
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, raised ? hi : lo, raised ? hi : lo, raised ? hi : lo);
    cairo_move_to(cr, x + 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + 0.5);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, raised ? 0 : hi, raised ? 0 : hi, raised ? 0 : hi);
    cairo_move_to(cr, x + w - 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + h - 0.5);
    cairo_stroke(cr);
    if (raised) { /* a linha interna escura do OPENSTEP */
        cairo_set_source_rgb(cr, dark ? 0.12 : 0.33, dark ? 0.12 : 0.33, dark ? 0.12 : 0.33);
        cairo_move_to(cr, x + w - 1.5, y + 1.5);
        cairo_line_to(cr, x + w - 1.5, y + h - 1.5);
        cairo_line_to(cr, x + 1.5, y + h - 1.5);
        cairo_stroke(cr);
    }
}

static void
text(cairo_t *cr, const char *t, double cx, double cy, double fs, gboolean bold, double r, double g, double b,
     int align /* -1 esquerda, 0 centro, 1 direita */)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd =
        pango_font_description_from_string("Helvetica, Nimbus Sans, Liberation Sans, DejaVu Sans, Sans");
    pango_font_description_set_absolute_size(fd, fs * PANGO_SCALE);
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, t, -1);
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    double x = align < 0 ? cx : align > 0 ? cx - w : cx - w / 2.0;
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, round(x), round(cy - h / 2.0));
    pango_cairo_show_layout(cr, l);
    pango_font_description_free(fd);
    g_object_unref(l);
}

void
bk_draw_titlebar(cairo_t *cr, double x, double y, double w, double h, const char *title, double fs)
{
    cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.16, 0.16, 0.18);
    cairo_pattern_add_color_stop_rgb(g, 1, 0, 0, 0);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    bk_draw_bevel(cr, x, y, w, h, TRUE, TRUE);
    text(cr, title, x + 10, y + h / 2, fs, TRUE, 1, 1, 1, -1);
}

/* Ícone de controle: corpo com as duas empunhaduras, direcional à esquerda,
 * dois botões à direita, no tom oposto ao do corpo. */
void
bk_draw_icon_gamepad(cairo_t *cr, double cx, double cy, double s, double r, double g, double b, double a)
{
    /* corpo: uma cápsula larga; empunhaduras: dois círculos que descem dos lados */
    double w = s, h = s * 0.5, x = cx - w / 2, y = cy - s * 0.3, rad = h / 2;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_arc(cr, x + rad, y + rad, rad, M_PI / 2, 1.5 * M_PI);
    cairo_arc(cr, x + w - rad, y + rad, rad, 1.5 * M_PI, M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
    double gr = s * 0.2;
    cairo_arc(cr, x + gr, y + h * 0.75, gr, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, x + w - gr, y + h * 0.75, gr, 0, 2 * M_PI);
    cairo_fill(cr);
    /* direcional e botões no tom oposto ao do corpo */
    double ink = (r + g + b) / 3 > 0.5 ? 0.0 : 1.0;
    cairo_set_source_rgba(cr, ink, ink, ink, a);
    double dx = x + w * 0.27, dy = y + h * 0.5, arm = s * 0.13, th = MAX(1.5, s * 0.08);
    cairo_rectangle(cr, dx - arm, dy - th / 2, 2 * arm, th);
    cairo_rectangle(cr, dx - th / 2, dy - arm, th, 2 * arm);
    cairo_fill(cr);
    double br = MAX(1.0, s * 0.065);
    cairo_arc(cr, x + w * 0.80, dy - s * 0.05, br, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, x + w * 0.67, dy + s * 0.06, br, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* Ícone de teclado: moldura arredondada, três fileiras de teclas e o espaço. */
void
bk_draw_icon_keyboard(cairo_t *cr, double cx, double cy, double s, double r, double g, double b, double a)
{
    double w = s * 0.94, h = s * 0.62, x = cx - w / 2, y = cy - h / 2, rad = s * 0.08;
    double lw = MAX(1.5, s * 0.07);
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, lw);
    double ix = x + lw / 2, iy = y + lw / 2, iw = w - lw, ih = h - lw;
    cairo_arc(cr, ix + iw - rad, iy + rad, rad, -M_PI / 2, 0);
    cairo_arc(cr, ix + iw - rad, iy + ih - rad, rad, 0, M_PI / 2);
    cairo_arc(cr, ix + rad, iy + ih - rad, rad, M_PI / 2, M_PI);
    cairo_arc(cr, ix + rad, iy + rad, rad, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
    cairo_stroke(cr);
    /* teclas: uma grade 3×5 dentro da moldura, a última fileira é o espaço */
    double px = x + lw * 1.8, py = y + lw * 1.8, pw = w - lw * 3.6, ph = h - lw * 3.6;
    double kw = pw / 5, kh = ph / 3, k = MIN(kw, kh) * 0.62;
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 5; col++)
            cairo_rectangle(cr, px + col * kw + (kw - k) / 2, py + row * kh + (kh - k) / 2, k, k);
    cairo_rectangle(cr, px + kw + (kw - k) / 2, py + 2 * kh + (kh - k) / 2, 3 * kw - (kw - k), k);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* Botões da barra de título: [✕] [⚙] [controle] da direita para a esquerda. */
#define TITLE_BUTTONS 3
static void
title_button_rect(BkOsk *osk, int i, double *x, double *y, double *w, double *h)
{
    double b = osk->title_h - 6;
    *w = *h = b;
    *x = osk->width - 4 - (i + 1) * (b + 4);
    *y = 3;
}

static void
draw_key(BkOsk *osk, cairo_t *cr, int i, gboolean down)
{
    const BkKey *k = osk->geom.key[i];
    const BkRect *r = &osk->geom.rect[i];
    gboolean dark = osk->app->dark;
    double x = osk->pad + r->x, y = osk->title_h + osk->pad + r->y, w = r->w, h = r->h;
    unsigned mods = bk_state_mods(&osk->st);
    gboolean latched = k->kind == BK_KEY_MOD && (osk->st.latched & k->mod);
    gboolean locked = (k->kind == BK_KEY_MOD && (osk->st.locked & k->mod)) || (k->kind == BK_KEY_CAPS && osk->st.caps);
    gboolean panel_on = k->kind == BK_KEY_PANEL && osk->st.panel == (BkPanel)k->panel;
    down = down || latched || locked || panel_on;
    gboolean special = k->kind != BK_KEY_CHAR || !strcmp(k->level[0], " ");
    double face;
    if (dark)
        face = down ? 0.17 : special ? 0.24 : 0.29;
    else
        face = down ? 0.56 : special ? 0.66 : 0.75;
    cairo_set_source_rgb(cr, face, face, face);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    bk_draw_bevel(cr, x, y, w, h, !down, dark);
    double ink = dark ? 0.95 : 0.0;
    double fs = osk->u * 0.36;
    const char *label = bk_state_label(&osk->st, k);
    if (k->kind == BK_KEY_CHAR && !strcmp(k->level[0], " "))
        label = bk_layout_get(osk->st.layout)->name; /* a barra de espaço mostra o layout, discreto */
    if (label && *label) {
        gboolean small = g_utf8_strlen(label, -1) > 2 || (k->kind == BK_KEY_CHAR && !strcmp(k->level[0], " "));
        double lfs = small ? fs * 0.72 : fs;
        double lr = ink, lg = ink, lb = ink;
        if (k->kind == BK_KEY_CHAR && !strcmp(k->level[0], " "))
            lr = lg = lb = dark ? 0.6 : 0.35;
        text(cr, label, x + w / 2, y + h / 2 + (down ? 1 : 0), lfs, FALSE, lr, lg, lb, 0);
    }
    /* símbolos de Shift (canto de cima) e AltGr (canto de baixo), como nas teclas reais */
    if (k->kind == BK_KEY_CHAR && k->level[1] && !(mods & BK_MOD_SHIFT)) {
        gboolean letter = g_utf8_strlen(k->level[0], -1) == 1 && g_unichar_isalpha(g_utf8_get_char(k->level[0]));
        if (!letter)
            text(cr, k->level[1], x + 4, y + fs * 0.45 + 1, fs * 0.5, FALSE, dark ? 0.7 : 0.25, dark ? 0.7 : 0.25,
                 dark ? 0.7 : 0.25, -1);
    }
    if (k->kind == BK_KEY_CHAR && k->level[2] && !(mods & BK_MOD_ALTGR))
        text(cr, k->level[2], x + w - 4, y + h - fs * 0.45 - 1, fs * 0.5, FALSE, dark ? 0.45 : 0.05,
             dark ? 0.7 : 0.25, dark ? 0.95 : 0.55, 1);
    if (locked) { /* LED verde: travado (dois toques, Caps) */
        cairo_set_source_rgb(cr, 0.19, 0.82, 0.19);
        cairo_rectangle(cr, x + 5, y + 5, MAX(4, osk->u * 0.12), MAX(4, osk->u * 0.12));
        cairo_fill(cr);
    }
}

static void
draw_cursor(BkOsk *osk, cairo_t *cr, int i, double r, double g, double b)
{
    if (i < 0 || i >= osk->geom.n)
        return;
    const BkRect *rc = &osk->geom.rect[i];
    double x = osk->pad + rc->x, y = osk->title_h + osk->pad + rc->y;
    double lw = MAX(2.5, osk->u * 0.08);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_set_line_width(cr, lw);
    cairo_rectangle(cr, x + lw / 2, y + lw / 2, rc->w - lw, rc->h - lw);
    cairo_stroke(cr);
}

static gboolean
on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)w;
    BkOsk *osk = data;
    gboolean dark = osk->app->dark;
    /* sombra discreta e corpo */
    double body = dark ? 0.2 : 0.667;
    cairo_set_source_rgb(cr, body, body, body);
    cairo_paint(cr);
    bk_draw_bevel(cr, 0, 0, osk->width, osk->height, TRUE, dark);
    const char *panel = osk->st.panel == BK_PANEL_FN ? "Fn" : osk->st.panel == BK_PANEL_SYMBOLS ? "☺" : "";
    char title[96];
    g_snprintf(title, sizeof title, "Big WM Virtual Keyboard%s%s", *panel ? " · " : "", panel);
    bk_draw_titlebar(cr, 0, 0, osk->width, osk->title_h, title, osk->title_h * 0.5);
    static const char *glyphs[] = { "✕", "⚙" };
    for (int i = 0; i < TITLE_BUTTONS; i++) {
        double x, y, bw, bh;
        title_button_rect(osk, i, &x, &y, &bw, &bh);
        gboolean down = i == 2 && osk->switch_armed;
        cairo_set_source_rgb(cr, down ? 0.56 : 0.75, down ? 0.56 : 0.75, down ? 0.56 : 0.75);
        cairo_rectangle(cr, x, y, bw, bh);
        cairo_fill(cr);
        bk_draw_bevel(cr, x, y, bw, bh, !down, FALSE);
        if (i < 2)
            text(cr, glyphs[i], x + bw / 2, y + bh / 2, bh * 0.7, TRUE, 0, 0, 0, 0);
        else /* controle na tela */
            bk_draw_icon_gamepad(cr, x + bw / 2, y + bh / 2 + (down ? 1 : 0), bh * 0.78, 0, 0, 0, 1);
    }
    for (int i = 0; i < osk->geom.n; i++) {
        gboolean down = i == osk->flash;
        for (int s = 0; s < MAX_SLOTS; s++)
            down |= osk->slot[s].used && osk->slot[s].key == i;
        draw_key(osk, cr, i, down);
    }
    draw_cursor(osk, cr, osk->cursor, dark ? 1 : 0, dark ? 1 : 0, dark ? 1 : 0);
    draw_cursor(osk, cr, osk->point[0], 0.1, 0.45, 0.95);  /* esquerdo: azul */
    draw_cursor(osk, cr, osk->point[1], 0.95, 0.55, 0.1);  /* direito: laranja */
    return TRUE;
}

/* ---- teclas -------------------------------------------------------------- */

static void
press_key(BkOsk *osk, const BkKey *k)
{
    if (k->kind == BK_KEY_HIDE) {
        bk_osk_set_visible(osk, FALSE);
        return;
    }
    int old_layout = osk->st.layout;
    BkPanel old_panel = osk->st.panel;
    BkOut out[BK_MAX_OUT];
    int n = bk_state_press(&osk->st, k, out, BK_MAX_OUT);
    for (int i = 0; i < n; i++)
        bk_inject_out(osk->app->inject, &out[i]);
    if (osk->st.layout != old_layout || osk->st.panel != old_panel) {
        /* outro conjunto de teclas: refaz a geometria (cursores voltam ao começo) */
        osk_layout(osk);
        osk->cursor = osk->cursor >= 0 ? MIN(osk->cursor, osk->geom.n - 1) : -1;
        osk->point[0] = osk->point[1] = -1;
    }
    if (osk->app->cfg.autohide && k->kind == BK_KEY_SPECIAL && !strcmp(k->keysym, "Return"))
        bk_osk_set_visible(osk, FALSE);
    gtk_widget_queue_draw(osk->area);
}

static gboolean
repeatable(const BkKey *k)
{
    if (k->kind == BK_KEY_CHAR)
        return TRUE;
    return k->kind == BK_KEY_SPECIAL && (!strcmp(k->keysym, "BackSpace") || !strcmp(k->keysym, "Delete") ||
                                         !strcmp(k->keysym, "Left") || !strcmp(k->keysym, "Right") ||
                                         !strcmp(k->keysym, "Up") || !strcmp(k->keysym, "Down"));
}

static gboolean
on_repeat(gpointer data)
{
    BkOsk *osk = data;
    Slot *s = &osk->slot[osk->repeat_slot];
    if (!s->used || s->key < 0) {
        osk->repeat_id = 0;
        return G_SOURCE_REMOVE;
    }
    s->repeated = TRUE;
    press_key(osk, osk->geom.key[s->key]);
    if (!s->used) { /* a tecla fechou o teclado */
        osk->repeat_id = 0;
        return G_SOURCE_REMOVE;
    }
    osk->repeat_id = g_timeout_add((guint)osk->app->cfg.repeat_rate, on_repeat, osk);
    return G_SOURCE_REMOVE;
}

static void
stop_repeat(BkOsk *osk)
{
    if (osk->repeat_id) {
        g_source_remove(osk->repeat_id);
        osk->repeat_id = 0;
    }
}

static Slot *
slot_for(BkOsk *osk, GdkEventSequence *seq, gboolean create)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (osk->slot[i].used && osk->slot[i].seq == seq)
            return &osk->slot[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (!osk->slot[i].used) {
            osk->slot[i] = (Slot){ TRUE, seq, -1, FALSE };
            return &osk->slot[i];
        }
    return NULL;
}

static int
key_at(BkOsk *osk, double x, double y)
{
    return bk_geom_hit(&osk->geom, x - osk->pad, y - osk->title_h - osk->pad);
}

/* Começo de um toque ou clique (x, y na área). */
static void
pointer_down(BkOsk *osk, GdkEventSequence *seq, double x, double y, GdkEvent *ev)
{
    if (y < osk->title_h) {
        for (int i = 0; i < TITLE_BUTTONS; i++) {
            double bx, by, bw, bh;
            title_button_rect(osk, i, &bx, &by, &bw, &bh);
            if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                if (i == 0) {
                    bk_osk_set_visible(osk, FALSE);
                } else if (i == 1) {
                    bk_settings_show(osk->app, "keyboard");
                } else if (!osk->switch_armed) {
                    osk->switch_armed = TRUE;
                    osk->switch_seq = seq;
                    gtk_widget_queue_draw(osk->area);
                }
                return;
            }
        }
        /* arrastar: o X11 move a janela; no Wayland mudam as margens */
#ifdef BK_HAVE_WAYLAND
        if (osk->layer) {
            osk->dragging = TRUE;
            gdk_event_get_root_coords(ev, &osk->drag_x, &osk->drag_y);
            return;
        }
#endif
        if (ev && ev->type == GDK_BUTTON_PRESS)
            gtk_window_begin_move_drag(GTK_WINDOW(osk->win), (int)ev->button.button, (int)ev->button.x_root,
                                       (int)ev->button.y_root, ev->button.time);
        return;
    }
    Slot *s = slot_for(osk, seq, TRUE);
    if (!s)
        return;
    s->key = key_at(osk, x, y);
    s->repeated = FALSE;
    if (s->key >= 0 && repeatable(osk->geom.key[s->key])) {
        stop_repeat(osk);
        osk->repeat_slot = (int)(s - osk->slot);
        osk->repeat_id = g_timeout_add((guint)osk->app->cfg.repeat_delay, on_repeat, osk);
    }
    gtk_widget_queue_draw(osk->area);
}

static void
pointer_move(BkOsk *osk, GdkEventSequence *seq, double x, double y, GdkEvent *ev)
{
    (void)x;
    (void)y;
#ifdef BK_HAVE_WAYLAND
    if (osk->dragging && seq == NULL && ev) {
        double rx, ry;
        gdk_event_get_root_coords(ev, &rx, &ry);
        osk->margin_x += (int)(rx - osk->drag_x);
        osk->margin_y -= (int)(ry - osk->drag_y);
        osk->drag_x = rx;
        osk->drag_y = ry;
        gtk_layer_set_anchor(GTK_WINDOW(osk->win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(osk->win), GTK_LAYER_SHELL_EDGE_LEFT, MAX(0, osk->margin_x));
        gtk_layer_set_margin(GTK_WINDOW(osk->win), GTK_LAYER_SHELL_EDGE_BOTTOM, MAX(0, osk->margin_y));
        return;
    }
#endif
    (void)ev;
    /* um dedo que escorrega para fora da tecla cancela (como num teclado de verdade) */
    Slot *s = slot_for(osk, seq, FALSE);
    if (s && s->key >= 0 && key_at(osk, x, y) != s->key) {
        s->key = -1;
        stop_repeat(osk);
        gtk_widget_queue_draw(osk->area);
    }
}

static void
pointer_up(BkOsk *osk, GdkEventSequence *seq, double x, double y, gboolean cancel)
{
    osk->dragging = FALSE;
    if (osk->switch_armed && seq == osk->switch_seq) {
        osk->switch_armed = FALSE;
        double bx, by, bw, bh;
        title_button_rect(osk, 2, &bx, &by, &bw, &bh);
        gtk_widget_queue_draw(osk->area);
        if (!cancel && x >= bx && x < bx + bw && y >= by && y < by + bh)
            bk_app_switch(osk->app, TRUE);
        return;
    }
    Slot *s = slot_for(osk, seq, FALSE);
    if (!s)
        return;
    int k = s->key;
    gboolean repeated = s->repeated;
    s->used = FALSE;
    if (osk->repeat_slot == (int)(s - osk->slot))
        stop_repeat(osk);
    if (!cancel && k >= 0 && key_at(osk, x, y) == k && !repeated)
        press_key(osk, osk->geom.key[k]);
    gtk_widget_queue_draw(osk->area);
}

static gboolean
on_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    BkOsk *osk = data;
    if (ev->button != 1)
        return FALSE;
    if (ev->type == GDK_BUTTON_PRESS)
        pointer_down(osk, NULL, ev->x, ev->y, (GdkEvent *)ev);
    else if (ev->type == GDK_BUTTON_RELEASE)
        pointer_up(osk, NULL, ev->x, ev->y, FALSE);
    return TRUE;
}

static gboolean
on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    pointer_move(data, NULL, ev->x, ev->y, (GdkEvent *)ev);
    return TRUE;
}

static gboolean
on_touch(GtkWidget *w, GdkEventTouch *ev, gpointer data)
{
    (void)w;
    BkOsk *osk = data;
    switch (ev->type) {
    case GDK_TOUCH_BEGIN:
        pointer_down(osk, ev->sequence, ev->x, ev->y, (GdkEvent *)ev);
        break;
    case GDK_TOUCH_UPDATE:
        pointer_move(osk, ev->sequence, ev->x, ev->y, NULL);
        break;
    case GDK_TOUCH_END:
        pointer_up(osk, ev->sequence, ev->x, ev->y, FALSE);
        break;
    case GDK_TOUCH_CANCEL:
        pointer_up(osk, ev->sequence, ev->x, ev->y, TRUE);
        break;
    default:
        break;
    }
    return TRUE;
}

/* ---- controle físico ----------------------------------------------------- */

static const BkKey *
key_by(BkOsk *osk, BkKeyKind kind, const char *keysym, unsigned mod)
{
    for (int i = 0; i < osk->geom.n; i++) {
        const BkKey *k = osk->geom.key[i];
        if (k->kind != kind)
            continue;
        if (keysym && (!k->keysym || strcmp(k->keysym, keysym)) &&
            !(kind == BK_KEY_CHAR && k->level[0] && !strcmp(k->level[0], keysym)))
            continue;
        if (mod && k->mod != mod)
            continue;
        return k;
    }
    /* fora do painel atual: teclas fixas equivalentes */
    static const BkKey bs = { BK_KEY_SPECIAL, { "⌫", NULL, NULL }, 0, "BackSpace", 0, 0, 1 };
    static const BkKey ret = { BK_KEY_SPECIAL, { "⏎", NULL, NULL }, 0, "Return", 0, 0, 1 };
    static const BkKey tab = { BK_KEY_SPECIAL, { "Tab", NULL, NULL }, 0, "Tab", 0, 0, 1 };
    static const BkKey space = { BK_KEY_CHAR, { " ", NULL, NULL }, 0, NULL, 0, 0, 1 };
    static const BkKey shift = { BK_KEY_MOD, { "⇧", NULL, NULL }, 0, NULL, BK_MOD_SHIFT, 0, 1 };
    static const BkKey altgr = { BK_KEY_MOD, { "AltGr", NULL, NULL }, 0, NULL, BK_MOD_ALTGR, 0, 1 };
    static const BkKey caps = { BK_KEY_CAPS, { "Caps", NULL, NULL }, 0, NULL, 0, 0, 1 };
    static const BkKey layout = { BK_KEY_LAYOUT, { "", NULL, NULL }, 0, NULL, 0, 0, 1 };
    if (kind == BK_KEY_SPECIAL && keysym && !strcmp(keysym, "BackSpace"))
        return &bs;
    if (kind == BK_KEY_SPECIAL && keysym && !strcmp(keysym, "Return"))
        return &ret;
    if (kind == BK_KEY_SPECIAL && keysym && !strcmp(keysym, "Tab"))
        return &tab;
    if (kind == BK_KEY_CHAR)
        return &space;
    if (kind == BK_KEY_MOD)
        return mod == BK_MOD_ALTGR ? &altgr : &shift;
    if (kind == BK_KEY_CAPS)
        return &caps;
    if (kind == BK_KEY_LAYOUT)
        return &layout;
    return NULL;
}

static gboolean
on_flash_end(gpointer data)
{
    BkOsk *osk = data;
    osk->flash = -1;
    osk->flash_id = 0;
    gtk_widget_queue_draw(osk->area);
    return G_SOURCE_REMOVE;
}

/* Aperta uma tecla pelo controle, com o mesmo feedback de um clique. */
static void
pad_press(BkOsk *osk, int idx)
{
    if (idx < 0 || idx >= osk->geom.n)
        return;
    osk->flash = idx;
    if (osk->flash_id)
        g_source_remove(osk->flash_id);
    osk->flash_id = g_timeout_add(FLASH_MS, on_flash_end, osk);
    press_key(osk, osk->geom.key[idx]);
}

void
bk_osk_pad_action(BkOsk *osk, enum bwg_action a)
{
    if (!bk_osk_visible(osk)) {
        if (a == BWG_COMBO)
            bk_osk_set_visible(osk, TRUE);
        return; /* fechado, o controle é dos jogos */
    }
    const BkKey *k = NULL;
    gboolean pointing = osk->point[0] >= 0 || osk->point[1] >= 0;
    switch (a) {
    case BWG_UP:
    case BWG_DOWN:
    case BWG_LEFT:
    case BWG_RIGHT: {
        bwg_rect r[BK_GEOM_MAX];
        for (int i = 0; i < osk->geom.n; i++)
            r[i] = (bwg_rect){ osk->geom.rect[i].x, osk->geom.rect[i].y, osk->geom.rect[i].w, osk->geom.rect[i].h };
        osk->cursor = bwg_spatial(r, osk->geom.n, osk->cursor, a);
        gtk_widget_queue_draw(osk->area);
        return;
    }
    case BWG_PRESS:
        if (osk->cursor < 0) {
            osk->cursor = 0;
            gtk_widget_queue_draw(osk->area);
        } else {
            pad_press(osk, osk->cursor);
        }
        return;
    case BWG_LT:
        if (pointing && osk->point[0] >= 0)
            pad_press(osk, osk->point[0]);
        else
            k = key_by(osk, BK_KEY_MOD, NULL, BK_MOD_SHIFT);
        break;
    case BWG_RT:
        if (pointing && osk->point[1] >= 0)
            pad_press(osk, osk->point[1]);
        else
            k = key_by(osk, BK_KEY_MOD, NULL, BK_MOD_ALTGR);
        break;
    case BWG_BACKSPACE:
        k = key_by(osk, BK_KEY_SPECIAL, "BackSpace", 0);
        break;
    case BWG_SPACE:
        k = key_by(osk, BK_KEY_CHAR, " ", 0);
        break;
    case BWG_SHIFT:
        k = key_by(osk, BK_KEY_MOD, NULL, BK_MOD_SHIFT);
        break;
    case BWG_LAYOUT: /* RB: próximo painel (letras → Fn → símbolos) */
        osk->st.panel = (BkPanel)((osk->st.panel + 1) % BK_PANEL_N);
        osk_layout(osk);
        osk->cursor = osk->cursor >= 0 ? MIN(osk->cursor, osk->geom.n - 1) : -1;
        osk->point[0] = osk->point[1] = -1;
        gtk_widget_queue_draw(osk->area);
        return;
    case BWG_CAPS:
        k = key_by(osk, BK_KEY_CAPS, NULL, 0);
        break;
    case BWG_R3:
        k = key_by(osk, BK_KEY_SPECIAL, "Tab", 0);
        break;
    case BWG_ENTER:
        k = key_by(osk, BK_KEY_SPECIAL, "Return", 0);
        break;
    case BWG_TOGGLE: /* Select: troca o layout */
        k = key_by(osk, BK_KEY_LAYOUT, NULL, 0);
        break;
    case BWG_BACK:
    case BWG_COMBO:
        bk_osk_set_visible(osk, FALSE);
        return;
    default:
        return;
    }
    if (k) {
        int idx = bk_geom_find(&osk->geom, k);
        if (idx >= 0)
            pad_press(osk, idx);
        else
            press_key(osk, k);
    }
}

void
bk_osk_pad_sticks(BkOsk *osk, const bwg_pad *p, double deadzone)
{
    if (!bk_osk_visible(osk) || !p)
        return;
    const char *mode = osk->app->cfg.pad_mode;
    if (!strcmp(mode, "cursor"))
        return;
    double x[2] = { p->sx, p->rx }, y[2] = { p->sy, p->ry };
    gboolean changed = FALSE;
    for (int side = 0; side < 2; side++) {
        if (hypot(x[side], y[side]) < deadzone)
            continue; /* solto: o cursor fica onde estava */
        int idx = bk_geom_point(&osk->geom, side, x[side], y[side]);
        if (idx != osk->point[side]) {
            osk->point[side] = idx;
            changed = TRUE;
        }
    }
    if (changed)
        gtk_widget_queue_draw(osk->area);
}

/* ---- janela -------------------------------------------------------------- */

static GdkMonitor *
target_monitor(BkOsk *osk)
{
    GdkDisplay *d = gtk_widget_get_display(osk->win);
    if (osk->pinned)
        return osk->pinned;
    GdkSeat *seat = gdk_display_get_default_seat(d);
    GdkDevice *ptr = seat ? gdk_seat_get_pointer(seat) : NULL;
    int px = 0, py = 0;
    if (ptr && !osk->layer) {
        gdk_device_get_position(ptr, NULL, &px, &py);
        return gdk_display_get_monitor_at_point(d, px, py);
    }
    GdkMonitor *m = gdk_display_get_primary_monitor(d);
    return m ? m : gdk_display_get_monitor(d, 0);
}

static void
osk_layout(BkOsk *osk)
{
    GdkMonitor *mon = target_monitor(osk);
    GdkRectangle geo = { 0, 0, 1280, 800 };
    if (mon)
        gdk_monitor_get_workarea(mon, &geo);
    double s = osk->app->cfg.scale;
    /* 15,5 unidades de largura: cabe com folga no monitor */
    double u = MIN(46 * s, (geo.width - 48) / 15.6);
    osk->u = round(MAX(24, u));
    osk->gap = round(MAX(3, osk->u * 0.09));
    osk->pad = round(osk->u * 0.22);
    osk->title_h = round(MAX(18, osk->u * 0.5));
    const BkLayout *l = bk_layout_get(osk->st.layout);
    bk_geom_build(&osk->geom, bk_panel_rows(l, osk->st.panel), osk->u, osk->gap);
    /* largura fixa pelo painel principal: trocar de painel não mexe na janela */
    BkGeom main_geom;
    bk_geom_build(&main_geom, bk_panel_rows(l, BK_PANEL_MAIN), osk->u, osk->gap);
    double gw = MAX(main_geom.width, osk->geom.width);
    if (osk->geom.width < gw) /* centra o painel mais estreito */
        for (int i = 0; i < osk->geom.n; i++)
            osk->geom.rect[i].x += round((gw - osk->geom.width) / 2);
    osk->width = (int)(gw + 2 * osk->pad);
    osk->height = (int)(osk->title_h + main_geom.height + 2 * osk->pad);
    if (g_getenv("BIG_KEYBOARD_DEBUG_GEOM")) {
        /* para os testes: a geometria (relativa à janela), sem nada digitado */
        g_printerr("geom %d %d\n", osk->width, osk->height);
        for (int i = 0; i < osk->geom.n; i++)
            g_printerr("key %d %d %s|%s %.0f %.0f %.0f %.0f\n", i, osk->geom.key[i]->kind,
                       osk->geom.key[i]->level[0] ? osk->geom.key[i]->level[0] : "",
                       osk->geom.key[i]->keysym ? osk->geom.key[i]->keysym : "",
                       osk->pad + osk->geom.rect[i].x, osk->title_h + osk->pad + osk->geom.rect[i].y,
                       osk->geom.rect[i].w, osk->geom.rect[i].h);
    }
    gtk_widget_set_size_request(osk->area, osk->width, osk->height);
    gtk_window_resize(GTK_WINDOW(osk->win), osk->width, osk->height);
#ifdef BK_HAVE_WAYLAND
    if (osk->layer) {
        if (mon)
            gtk_layer_set_monitor(GTK_WINDOW(osk->win), mon);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(osk->win), osk->app->cfg.reserve ? osk->height : 0);
        return;
    }
#endif
#ifdef BK_HAVE_X11
    GdkWindow *gw_ = gtk_widget_get_window(osk->win);
    int x = geo.x + (geo.width - osk->width) / 2, y = geo.y + geo.height - osk->height;
    gtk_window_move(GTK_WINDOW(osk->win), x, y);
    if (gw_ && GDK_IS_X11_WINDOW(gw_)) {
        /* reservar espaço: janelas maximizadas sobem (strut na borda de baixo) */
        GdkRectangle full;
        gdk_monitor_get_geometry(mon, &full);
        int scr_h = HeightOfScreen(DefaultScreenOfDisplay(GDK_WINDOW_XDISPLAY(gw_)));
        int sc = gdk_window_get_scale_factor(gw_);
        long strut[12] = { 0 };
        if (osk->app->cfg.reserve) {
            strut[3] = (scr_h - (full.y + full.height) * sc) + osk->height * sc;
            strut[10] = x * sc;
            strut[11] = (x + osk->width) * sc - 1;
        }
        gdk_property_change(gw_, gdk_atom_intern_static_string("_NET_WM_STRUT_PARTIAL"),
                            gdk_atom_intern_static_string("CARDINAL"), 32, GDK_PROP_MODE_REPLACE,
                            (guchar *)strut, 12);
    }
#endif
}

BkOsk *
bk_osk_new(BkApp *app)
{
    BkOsk *osk = g_new0(BkOsk, 1);
    osk->app = app;
    g_signal_connect(gdk_display_get_default(), "monitor-removed", G_CALLBACK(on_monitor_removed), osk);
    osk->cursor = -1;
    osk->point[0] = osk->point[1] = -1;
    osk->flash = -1;
    int layout = 0;
    const BkLayout *l = bk_layout_by_id(app->cfg.layout);
    if (!l && !strcmp(app->cfg.layout, "auto")) {
        /* o layout da sessão: o mesmo arquivo que o Big WindowMaker usa */
        char *conf = NULL;
        if (g_file_get_contents("/etc/X11/xorg.conf.d/00-keyboard.conf", &conf, NULL, NULL)) {
            l = bk_layout_by_id(strstr(conf, "\"br\"") ? "br" : strstr(conf, "intl") ? "us-intl" : "us");
            g_free(conf);
        }
        if (!l)
            l = bk_layout_by_id(g_str_has_prefix(g_getenv("LANG") ? g_getenv("LANG") : "", "pt") ? "br" : "us");
    }
    for (int i = 0; l && i < bk_layout_count(); i++)
        if (bk_layout_get(i) == l)
            layout = i;
    bk_state_init(&osk->st, layout);
    osk->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(osk->win), "Big WM Virtual Keyboard");
    gtk_window_set_role(GTK_WINDOW(osk->win), "big-keyboard");
    gtk_application_add_window(app->gtk, GTK_WINDOW(osk->win));
#ifdef BK_HAVE_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default()) && gtk_layer_is_supported()) {
        osk->layer = TRUE;
        gtk_layer_init_for_window(GTK_WINDOW(osk->win));
        gtk_layer_set_namespace(GTK_WINDOW(osk->win), "big-keyboard");
        gtk_layer_set_layer(GTK_WINDOW(osk->win), GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_anchor(GTK_WINDOW(osk->win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(osk->win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    }
#endif
    if (!osk->layer) {
        /* X11: sem foco, sem moldura, acima das janelas, fora da barra de tarefas */
        gtk_window_set_type_hint(GTK_WINDOW(osk->win), GDK_WINDOW_TYPE_HINT_DOCK);
        gtk_window_set_decorated(GTK_WINDOW(osk->win), FALSE);
        gtk_window_set_accept_focus(GTK_WINDOW(osk->win), FALSE);
        gtk_window_set_focus_on_map(GTK_WINDOW(osk->win), FALSE);
        gtk_window_set_keep_above(GTK_WINDOW(osk->win), TRUE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(osk->win), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(osk->win), TRUE);
        gtk_window_stick(GTK_WINDOW(osk->win));
    }
    osk->area = gtk_drawing_area_new();
    gtk_widget_add_events(osk->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                         GDK_TOUCH_MASK);
    g_signal_connect(osk->area, "draw", G_CALLBACK(on_draw), osk);
    g_signal_connect(osk->area, "button-press-event", G_CALLBACK(on_button), osk);
    g_signal_connect(osk->area, "button-release-event", G_CALLBACK(on_button), osk);
    g_signal_connect(osk->area, "motion-notify-event", G_CALLBACK(on_motion), osk);
    g_signal_connect(osk->area, "touch-event", G_CALLBACK(on_touch), osk);
    gtk_container_add(GTK_CONTAINER(osk->win), osk->area);
    gtk_widget_set_opacity(osk->win, app->cfg.opacity);
    /* atalhos para a acessibilidade: o leitor de tela anuncia a janela */
    atk_object_set_name(gtk_widget_get_accessible(osk->area), "Teclado na tela");
    gtk_widget_realize(osk->win);
    osk_layout(osk);
    return osk;
}

void
bk_osk_set_visible(BkOsk *osk, gboolean visible)
{
    if (visible == bk_osk_visible(osk))
        return;
    if (visible) {
        osk_layout(osk); /* monitor e escala do momento */
        gtk_widget_show_all(osk->win);
        osk_layout(osk); /* X11: o strut precisa da janela mapeada */
    } else {
        bk_osk_release(osk);
        gtk_widget_hide(osk->win);
        g_clear_object(&osk->pinned);
        /* o controle virtual que a troca manteve ligado sai junto, se o
         * controle na tela não voltou */
        if (osk->app->vpad && !bk_overlay_visible(osk->app->overlay)) {
            bk_vpad_free(osk->app->vpad);
            osk->app->vpad = NULL;
        }
    }
    bk_app_update_state(osk->app);
}

gboolean
bk_osk_visible(BkOsk *osk)
{
    return osk && gtk_widget_get_visible(osk->win);
}

void
bk_osk_release(BkOsk *osk)
{
    stop_repeat(osk);
    osk->switch_armed = FALSE;
    for (int i = 0; i < MAX_SLOTS; i++)
        osk->slot[i].used = FALSE;
    bk_state_release_all(&osk->st);
    osk->cursor = -1;
    osk->point[0] = osk->point[1] = -1;
    if (osk->area)
        gtk_widget_queue_draw(osk->area);
}

static void
on_monitor_removed(GdkDisplay *d, GdkMonitor *m, gpointer data)
{
    (void)d;
    BkOsk *osk = data;
    if (osk->pinned == m)
        g_clear_object(&osk->pinned);
}

void
bk_osk_set_monitor(BkOsk *osk, GdkMonitor *monitor)
{
    g_clear_object(&osk->pinned);
    if (monitor)
        osk->pinned = g_object_ref(monitor);
}

void
bk_osk_reload(BkOsk *osk)
{
    gtk_widget_set_opacity(osk->win, osk->app->cfg.opacity);
    osk_layout(osk);
    gtk_widget_queue_draw(osk->area);
}

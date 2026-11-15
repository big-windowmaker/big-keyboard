/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * O teclado na tela da sessão: teclas OPENSTEP em relevo desenhadas com cairo
 * numa só área, e uma coluna à direita com botões grandes (controle na tela,
 * configurações e, sem moldura, ocultar).
 *
 * Dois modos (Configurações > Teclado):
 *   flutuante  janela com a moldura do Window Maker (título, fechar, barra de
 *              redimensionar): vai para qualquer lugar e muda de tamanho, as
 *              teclas acompanham com a proporção mantida. Lembra o lugar e o
 *              tamanho.
 *   encaixado  embaixo, na largura inteira do monitor, como no celular;
 *              pode reservar o espaço (as janelas sobem).
 *
 * Nunca pega o foco. No X11 a moldura flutuante é a do próprio Window Maker
 * (a janela não aceita foco: WM_HINTS input=False). No Wayland, uma janela
 * comum ganharia o foco ao ser tocada e as teclas iriam para ela mesma; ali o
 * teclado é uma superfície layer-shell (keyboard-interactivity NONE) com a
 * moldura do Window Maker desenhada por ele. "Por cima até de tela cheia":
 * camada OVERLAY no Wayland; no X11, janela override-redirect (também com a
 * moldura desenhada) que volta ao topo quando o gerenciador reempilha.
 *
 * Entrada: mouse e toques (cada dedo independente) apertam ao soltar, com
 * repetição ao segurar; controle físico com cursor de tecla em tecla
 * (direcional) ou dois cursores apontados pelos analógicos (estilo SteamOS,
 * gatilhos apertam). Fechado, não desenha nem acorda.
 */
#include "bk-app.h"
#include "bk-geom.h"

#include <math.h>
#include <stdlib.h>
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

typedef enum { WIN_DOCKED, WIN_FLOAT_WM, WIN_FLOAT_OWN } WinKind;
typedef enum { COL_HIDE, COL_PAD, COL_SETTINGS, COL_N } ColButton;
typedef enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } DragKind;

struct BkOsk {
    BkApp *app;
    GtkWidget *win, *area;
    BkState st;
    BkGeom geom;
    double u, gap, pad, s;
    int width, height;
    /* janela */
    WinKind kind;
    gboolean layer;    /* superfície layer-shell (Wayland) */
    gboolean override; /* X11: override-redirect (acima até da tela cheia) */
    int level;         /* 0 normal, 1 acima das janelas, 2 acima da tela cheia */
    double last_scale; /* mudar o tamanho das teclas volta à largura padrão */
    char layout_cfg[16]; /* o layout das configurações já aplicado (o Select troca à parte) */
    /* áreas, em coordenadas da janela */
    double kx, ky;                /* origem das teclas */
    double border, title_h, resize_h; /* moldura desenhada (0 sem ela) */
    BkRect col[COL_N];
    gboolean col_on[COL_N];
    int col_down;                 /* botão da coluna apertado, -1 nenhum */
    GdkEventSequence *col_seq;
    Slot slot[MAX_SLOTS];
    guint repeat_id;
    int repeat_slot;
    int cursor;             /* direcional: índice em geom; -1 nenhum */
    int point[2];           /* modo apontar: cursores esquerdo/direito; -1 nenhum */
    int flash;              /* tecla apertada pelo controle (feedback curto) */
    guint flash_id;
    /* moldura desenhada: arrastar o título move, a barra de baixo
     * redimensiona (o canto de cima à esquerda fica parado) */
    DragKind drag;
    GdkEventSequence *drag_seq;
    double drag_x, drag_y; /* início: tela (X11) ou superfície (layer-shell) */
    int drag_px, drag_py, drag_w;
    int pos_x, pos_y;      /* janela flutuante, coordenadas da tela */
    /* mover no layer-shell: o dedo só é conhecido em relação à superfície,
     * que anda debaixo dele. Cada posição nova só é pedida depois que o
     * compositor confirmou a anterior (wl_display_sync após o quadro), e o
     * dedo é lido em relação à posição já aplicada: nada de ida e volta. */
    int applied_x, applied_y; /* posição que o compositor já usa */
    int asked_x, asked_y;     /* pedida, à espera da confirmação */
    gboolean move_pending, move_dirty;
    double last_lx, last_ly;  /* último ponto do dedo na superfície */
    gulong after_paint_id;
    gboolean placed;
    gboolean follow;        /* aberto no monitor do celular: a posição gravada não vale */
    GdkMonitor *placed_mon; /* onde place_window pôs a janela (só para comparar) */
    /* arrasto da janela pelo celular (coordenadas do monitor) */
    DragKind rdrag;
    const void *rdrag_id;
    double rdrag_mx, rdrag_my;
    int rdrag_px, rdrag_py, rdrag_w;
    guint save_id;
    GdkMonitor *pinned; /* aberto pela troca: o monitor do controle */
    gboolean auto_shown; /* aberto sozinho por um campo de texto */
};

static void osk_layout(BkOsk *osk);
static void osk_follow(BkOsk *osk);
static int configured_layout(BkApp *app);
static void place_window(BkOsk *osk);
static GdkMonitor *target_monitor(BkOsk *osk);
static void schedule_save(BkOsk *osk);
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

/* Engrenagem (configurações): dentes em volta de um anel. */
static void
draw_icon_gear(cairo_t *cr, double cx, double cy, double s, double ink, double face)
{
    double R = s / 2, body = R * 0.72, hole = R * 0.30, tw = R * 0.42;
    cairo_save(cr);
    cairo_set_source_rgb(cr, ink, ink, ink);
    for (int i = 0; i < 8; i++) {
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, i * M_PI / 4);
        cairo_rectangle(cr, -tw / 2, -R, tw, R);
        cairo_restore(cr);
    }
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, body, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, face, face, face);
    cairo_arc(cr, cx, cy, hole, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* Ocultar: o teclado com uma seta para baixo. */
static void
draw_icon_hide(cairo_t *cr, double cx, double cy, double s, double ink)
{
    bk_draw_icon_keyboard(cr, cx, cy - s * 0.12, s * 0.9, ink, ink, ink, 1);
    double t = s * 0.16, y = cy + s * 0.30;
    cairo_set_source_rgb(cr, ink, ink, ink);
    cairo_move_to(cr, cx - t, y - t / 2);
    cairo_line_to(cr, cx + t, y - t / 2);
    cairo_line_to(cr, cx, y + t / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static gboolean
in_rect(const BkRect *r, double x, double y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* Moldura desenhada (Wayland e "acima da tela cheia" no X11), como a do
 * Window Maker: barra de título preta com o nome ao centro e o botão de
 * fechar à direita; embaixo, a barra de redimensionar com os cantos. */
static void
close_rect(BkOsk *osk, BkRect *r)
{
    double b = osk->title_h - 6;
    *r = (BkRect){ osk->width - osk->border - 3 - b, osk->border + 3, b, b };
}

static void
draw_frame(BkOsk *osk, cairo_t *cr, const char *title)
{
    double b = osk->border, w = osk->width, h = osk->height, th = osk->title_h, rh = osk->resize_h;
    /* contorno preto de 1 px */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_rectangle(cr, b, b, w - 2 * b, h - 2 * b);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    /* título: degradê preto, o nome ao centro */
    cairo_pattern_t *g = cairo_pattern_create_linear(0, b, 0, b + th);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.16, 0.16, 0.18);
    cairo_pattern_add_color_stop_rgb(g, 1, 0, 0, 0);
    cairo_rectangle(cr, b, b, w - 2 * b, th);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    bk_draw_bevel(cr, b, b, w - 2 * b, th, TRUE, TRUE);
    BkRect c;
    close_rect(osk, &c);
    text(cr, title, (b + c.x) / 2, b + th / 2, th * 0.52, TRUE, 1, 1, 1, 0);
    /* fechar: botão cinza em relevo com o X */
    gboolean down = osk->col_down == COL_N; /* COL_N: o fechar da moldura */
    double f = down ? 0.56 : 0.75;
    cairo_set_source_rgb(cr, f, f, f);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h);
    cairo_fill(cr);
    bk_draw_bevel(cr, c.x, c.y, c.w, c.h, !down, FALSE);
    double m = c.w * 0.28, lw = MAX(1.5, c.w * 0.13);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, lw);
    cairo_move_to(cr, c.x + m, c.y + m);
    cairo_line_to(cr, c.x + c.w - m, c.y + c.h - m);
    cairo_move_to(cr, c.x + c.w - m, c.y + m);
    cairo_line_to(cr, c.x + m, c.y + c.h - m);
    cairo_stroke(cr);
    /* barra de redimensionar com os dois cantos marcados */
    double ry = h - b - rh, corner = MIN(30, w / 4);
    double face = osk->app->dark ? 0.24 : 0.667;
    cairo_set_source_rgb(cr, face, face, face);
    cairo_rectangle(cr, b, ry, w - 2 * b, rh);
    cairo_fill(cr);
    bk_draw_bevel(cr, b, ry, w - 2 * b, rh, TRUE, osk->app->dark);
    for (int i = 0; i < 2; i++) {
        double x = i == 0 ? b + corner : w - b - corner;
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_rectangle(cr, x - 1, ry + 1, 1, rh - 2);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, x, ry + 1, 1, rh - 2);
        cairo_fill(cr);
    }
}

static void
draw_column(BkOsk *osk, cairo_t *cr)
{
    static const char *labels[COL_N] = { "Ocultar", "Controle", "Configurar" };
    gboolean dark = osk->app->dark;
    for (int i = 0; i < COL_N; i++) {
        if (!osk->col_on[i])
            continue;
        const BkRect *r = &osk->col[i];
        gboolean down = osk->col_down == i;
        double face = dark ? (down ? 0.17 : 0.24) : (down ? 0.56 : 0.66);
        cairo_set_source_rgb(cr, face, face, face);
        cairo_rectangle(cr, r->x, r->y, r->w, r->h);
        cairo_fill(cr);
        bk_draw_bevel(cr, r->x, r->y, r->w, r->h, !down, dark);
        double ink = dark ? 0.95 : 0.0, o = down ? 1 : 0;
        /* rótulo embaixo, se couber; o ícone ocupa o resto */
        double fs = MAX(9, osk->u * 0.22);
        PangoLayout *pl = pango_cairo_create_layout(cr);
        PangoFontDescription *fd = pango_font_description_from_string("Sans");
        pango_font_description_set_absolute_size(fd, fs * PANGO_SCALE);
        pango_layout_set_font_description(pl, fd);
        pango_layout_set_text(pl, labels[i], -1);
        int lw, lh;
        pango_layout_get_pixel_size(pl, &lw, &lh);
        pango_font_description_free(fd);
        g_object_unref(pl);
        gboolean label = lw <= r->w - 6 && r->h >= osk->u * 1.4;
        double ih = label ? r->h - lh - 6 : r->h;
        double is = MIN(r->w, ih) * 0.62, cx = r->x + r->w / 2 + o, cy = r->y + ih / 2 + o + (label ? 3 : 0);
        if (i == COL_HIDE)
            draw_icon_hide(cr, cx, cy, is, ink);
        else if (i == COL_PAD)
            bk_draw_icon_gamepad(cr, cx, cy, is, ink, ink, ink, 1);
        else
            draw_icon_gear(cr, cx, cy, is * 0.9, ink, face);
        if (label)
            text(cr, labels[i], r->x + r->w / 2 + o, r->y + r->h - lh / 2.0 - 4 + o, fs, FALSE, ink, ink, ink, 0);
    }
}

static void
draw_key(BkOsk *osk, cairo_t *cr, int i, gboolean down)
{
    const BkKey *k = osk->geom.key[i];
    const BkRect *r = &osk->geom.rect[i];
    gboolean dark = osk->app->dark;
    double x = osk->kx + r->x, y = osk->ky + r->y, w = r->w, h = r->h;
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

/* Navegação pelo controle físico: as teclas (0..geom.n-1) e, depois delas,
 * os botões visíveis da coluna, na ordem de cima para baixo. */
static int
nav_count(BkOsk *osk)
{
    int n = osk->geom.n;
    for (int i = 0; i < COL_N; i++)
        n += osk->col_on[i];
    return n;
}

/* O botão da coluna do índice de navegação, ou -1 se for uma tecla. */
static int
nav_column(BkOsk *osk, int idx)
{
    if (idx < osk->geom.n)
        return -1;
    int k = idx - osk->geom.n;
    for (int i = 0; i < COL_N; i++)
        if (osk->col_on[i] && k-- == 0)
            return i;
    return -1;
}

/* Retângulo do item, em coordenadas da área. */
static gboolean
nav_rect(BkOsk *osk, int idx, BkRect *out)
{
    if (idx < 0)
        return FALSE;
    if (idx < osk->geom.n) {
        const BkRect *rc = &osk->geom.rect[idx];
        *out = (BkRect){ osk->kx + rc->x, osk->ky + rc->y, rc->w, rc->h };
        return TRUE;
    }
    int c = nav_column(osk, idx);
    if (c < 0)
        return FALSE;
    *out = osk->col[c];
    return TRUE;
}

static void
draw_cursor(BkOsk *osk, cairo_t *cr, int i, double r, double g, double b)
{
    BkRect rc;
    if (!nav_rect(osk, i, &rc))
        return;
    double lw = MAX(2.5, osk->u * 0.08);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_set_line_width(cr, lw);
    cairo_rectangle(cr, rc.x + lw / 2, rc.y + lw / 2, rc.w - lw, rc.h - lw);
    cairo_stroke(cr);
}

static gboolean
on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)w;
    BkOsk *osk = data;
    gboolean dark = osk->app->dark;
    /* corpo */
    double body = dark ? 0.2 : 0.667;
    cairo_set_source_rgb(cr, body, body, body);
    cairo_rectangle(cr, 0, 0, osk->width, osk->height);
    cairo_fill(cr);
    bk_draw_bevel(cr, 0, 0, osk->width, osk->height, TRUE, dark);
    if (osk->kind == WIN_FLOAT_OWN) {
        const char *panel = osk->st.panel == BK_PANEL_FN ? "Fn" : osk->st.panel == BK_PANEL_SYMBOLS ? "☺" : "";
        char title[96];
        g_snprintf(title, sizeof title, "Teclado virtual%s%s", *panel ? " · " : "", panel);
        draw_frame(osk, cr, title);
    }
    draw_column(osk, cr);
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
    return bk_geom_hit(&osk->geom, x - osk->kx, y - osk->ky);
}

/* Aciona um botão da coluna (COL_N: o fechar da moldura). */
static void
column_activate(BkOsk *osk, int b)
{
    if (b == COL_PAD)
        bk_app_switch(osk->app, TRUE);
    else if (b == COL_SETTINGS)
        bk_settings_show(osk->app, "keyboard");
    else /* ocultar, ou o fechar da moldura */
        bk_osk_set_visible(osk, FALSE);
}

/* Botão da coluna sob (x, y); COL_N é o fechar da moldura desenhada. */
static int
button_at(BkOsk *osk, double x, double y)
{
    for (int i = 0; i < COL_N; i++)
        if (osk->col_on[i] && in_rect(&osk->col[i], x, y))
            return i;
    if (osk->kind == WIN_FLOAT_OWN) {
        BkRect c;
        close_rect(osk, &c);
        if (in_rect(&c, x, y))
            return COL_N;
    }
    return -1;
}

/* Onde medir o arrasto: na tela (X11, a janela tem posição real) ou na
 * própria superfície (layer-shell: o GTK não sabe onde ela está). */
static void
drag_coords(BkOsk *osk, GdkEvent *ev, double x, double y, double *dx, double *dy)
{
    *dx = x;
    *dy = y;
    if (!osk->layer && ev)
        gdk_event_get_root_coords(ev, dx, dy);
}

#ifdef BK_HAVE_WAYLAND
static void layer_move_apply(BkOsk *osk);

static void
on_move_synced(void *data, struct wl_callback *cb, uint32_t t)
{
    (void)t;
    BkOsk *osk = data;
    wl_callback_destroy(cb);
    osk->applied_x = osk->asked_x;
    osk->applied_y = osk->asked_y;
    osk->move_pending = FALSE;
    if (osk->move_dirty && osk->drag == DRAG_MOVE)
        layer_move_apply(osk);
}

static const struct wl_callback_listener move_sync_listener = { on_move_synced };

/* O quadro com as margens novas foi enviado: o sync volta depois que o
 * compositor o processou. */
static void
on_after_paint(GdkFrameClock *fc, gpointer data)
{
    BkOsk *osk = data;
    g_signal_handler_disconnect(fc, osk->after_paint_id);
    osk->after_paint_id = 0;
    struct wl_display *dpy = gdk_wayland_display_get_wl_display(gtk_widget_get_display(osk->win));
    struct wl_callback *cb = wl_display_sync(dpy);
    wl_callback_add_listener(cb, &move_sync_listener, osk);
    wl_display_flush(dpy);
}

static void
layer_move_apply(BkOsk *osk)
{
    osk->move_dirty = FALSE;
    int x = osk->applied_x + (int)round(osk->last_lx - osk->drag_x);
    int y = osk->applied_y + (int)round(osk->last_ly - osk->drag_y);
    if (x == osk->applied_x && y == osk->applied_y)
        return;
    osk->pos_x = x;
    osk->pos_y = y;
    place_window(osk); /* pode limitar à tela */
    osk->asked_x = osk->pos_x;
    osk->asked_y = osk->pos_y;
    osk->move_pending = TRUE;
    GdkFrameClock *fc = gtk_widget_get_frame_clock(osk->win);
    if (fc && !osk->after_paint_id)
        osk->after_paint_id = g_signal_connect(fc, "after-paint", G_CALLBACK(on_after_paint), osk);
    gtk_widget_queue_draw(osk->area);
}
#endif

/* Começo de um toque ou clique (x, y na área). */
static void
pointer_down(BkOsk *osk, GdkEventSequence *seq, double x, double y, GdkEvent *ev)
{
    int b = button_at(osk, x, y);
    if (b >= 0) {
        /* age ao soltar, não ao tocar: a troca pelo controle pega o
         * touchscreen do celular, e um dedo ainda na tela ficaria preso */
        if (osk->col_down < 0) {
            osk->col_down = b;
            osk->col_seq = seq;
            gtk_widget_queue_draw(osk->area);
        }
        return;
    }
    if (osk->kind == WIN_FLOAT_OWN && osk->drag == DRAG_NONE) {
        gboolean title = y < osk->border + osk->title_h;
        gboolean bottom = y >= osk->height - osk->border - osk->resize_h;
        if (title || bottom) {
            osk->drag = title ? DRAG_MOVE : DRAG_RESIZE;
            osk->drag_seq = seq;
            drag_coords(osk, ev, x, y, &osk->drag_x, &osk->drag_y);
            osk->drag_px = osk->pos_x;
            osk->drag_py = osk->pos_y;
            osk->drag_w = osk->width;
            osk->applied_x = osk->pos_x;
            osk->applied_y = osk->pos_y;
            osk->move_pending = osk->move_dirty = FALSE;
            return;
        }
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
    if (osk->drag != DRAG_NONE && seq == osk->drag_seq) {
        double px, py;
        drag_coords(osk, ev, x, y, &px, &py);
        if (osk->drag == DRAG_MOVE) {
            if (osk->layer) {
#ifdef BK_HAVE_WAYLAND
                osk->last_lx = px;
                osk->last_ly = py;
                if (osk->move_pending)
                    osk->move_dirty = TRUE; /* aplica quando o compositor confirmar */
                else
                    layer_move_apply(osk);
#endif
            } else {
                osk->pos_x = osk->drag_px + (int)round(px - osk->drag_x);
                osk->pos_y = osk->drag_py + (int)round(py - osk->drag_y);
                place_window(osk);
            }
        } else {
            /* o canto de cima à esquerda fica; a largura segue o dedo e a
             * altura vem da proporção das teclas */
            double dx = x - osk->drag_x, dy = y - osk->drag_y;
            if (!osk->layer) {
                double rx, ry;
                drag_coords(osk, ev, x, y, &rx, &ry);
                dx = rx - osk->drag_x;
                dy = ry - osk->drag_y;
            }
            double ratio = osk->height > 0 ? (double)osk->width / osk->height : 3;
            int w = (int)round(osk->drag_w + MAX(dx, dy * ratio));
            if (w != osk->width) {
                osk->app->cfg.float_width = w;
                osk_layout(osk);
            }
        }
        return;
    }
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
    if (osk->drag != DRAG_NONE && seq == osk->drag_seq) {
        osk->drag = DRAG_NONE;
        osk->app->cfg.float_x = osk->pos_x;
        osk->app->cfg.float_y = osk->pos_y;
        schedule_save(osk);
        return;
    }
    if (osk->col_down >= 0 && seq == osk->col_seq) {
        int b = osk->col_down;
        osk->col_down = -1;
        gtk_widget_queue_draw(osk->area);
        if (cancel || button_at(osk, x, y) != b)
            return;
        column_activate(osk, b);
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
    if (bk_remote_shadows_local(osk->app->remote))
        return TRUE; /* os dedos do celular já chegam por bk_osk_remote_touch */
    switch (ev->type) {
    case GDK_TOUCH_BEGIN:
        pointer_down(osk, ev->sequence, ev->x, ev->y, (GdkEvent *)ev);
        break;
    case GDK_TOUCH_UPDATE:
        pointer_move(osk, ev->sequence, ev->x, ev->y, (GdkEvent *)ev);
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
    int c = nav_column(osk, idx);
    if (c >= 0) { /* Controle, Configurar ou Ocultar, como um toque */
        column_activate(osk, c);
        return;
    }
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
        bwg_rect r[BK_GEOM_MAX + COL_N];
        int n = nav_count(osk);
        for (int i = 0; i < n; i++) {
            BkRect rc;
            nav_rect(osk, i, &rc);
            r[i] = (bwg_rect){ rc.x, rc.y, rc.w, rc.h };
        }
        osk->cursor = bwg_spatial(r, n, osk->cursor, a);
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
        int ncol = nav_count(osk) - osk->geom.n;
        if (side == 1 && x[side] >= 0.92 && ncol > 0) /* na borda direita: a coluna, pela altura */
            idx = osk->geom.n + MIN(ncol - 1, (int)((y[side] + 1) / 2 * ncol));
        if (idx != osk->point[side]) {
            osk->point[side] = idx;
            changed = TRUE;
        }
    }
    if (changed)
        gtk_widget_queue_draw(osk->area);
}

/* ---- janela -------------------------------------------------------------- */

/* Qual janela: o modo, o "por cima" e o que a sessão permite. */
static void
choose_kind(BkOsk *osk, WinKind *kind, gboolean *layer, gboolean *ovr, int *level)
{
    const BkConfig *c = &osk->app->cfg;
    *level = !strcmp(c->stacking, "normal") ? 0 : !strcmp(c->stacking, "fullscreen") ? 2 : 1;
    gboolean docked = !strcmp(c->window_mode, "docked");
    *layer = *ovr = FALSE;
#ifdef BK_HAVE_WAYLAND
    *layer = GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default()) && gtk_layer_is_supported();
#endif
    gboolean x11 = FALSE;
#ifdef BK_HAVE_X11
    x11 = GDK_IS_X11_DISPLAY(gdk_display_get_default());
#endif
    if (*layer) {
        *kind = docked ? WIN_DOCKED : WIN_FLOAT_OWN;
    } else if (x11 && *level == 2) {
        /* acima da tela cheia: fora do gerenciador, com a moldura desenhada */
        *ovr = TRUE;
        *kind = docked ? WIN_DOCKED : WIN_FLOAT_OWN;
    } else {
        /* X11 com a moldura do gerenciador; ou um Wayland sem layer-shell
         * (GNOME): uma janela comum, o melhor que dá */
        *kind = docked && x11 ? WIN_DOCKED : WIN_FLOAT_WM;
    }
}

static GdkMonitor *
target_monitor(BkOsk *osk)
{
    GdkDisplay *d = gdk_display_get_default();
    if (osk->pinned)
        return osk->pinned;
    if (osk->kind != WIN_DOCKED && osk->placed) {
        GdkMonitor *m = gdk_display_get_monitor_at_point(d, osk->pos_x + osk->width / 2, osk->pos_y + 8);
        if (m)
            return m;
    }
    GdkSeat *seat = gdk_display_get_default_seat(d);
    GdkDevice *ptr = seat ? gdk_seat_get_pointer(seat) : NULL;
    if (ptr && !osk->layer) {
        int px = 0, py = 0;
        gdk_device_get_position(ptr, NULL, &px, &py);
        return gdk_display_get_monitor_at_point(d, px, py);
    }
    GdkMonitor *m = gdk_display_get_primary_monitor(d);
    return m ? m : gdk_display_get_monitor(d, 0);
}

static gboolean
on_save(gpointer data)
{
    BkOsk *osk = data;
    osk->save_id = 0;
    bk_config_save(&osk->app->cfg, NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* Posição e tamanho gravados um pouco depois (não a cada passo do arrasto). */
static void
schedule_save(BkOsk *osk)
{
    if (osk->save_id)
        g_source_remove(osk->save_id);
    osk->save_id = g_timeout_add(700, on_save, osk);
}

/* Põe a janela no lugar: margens do layer-shell, ou posição no X11. */
static void
place_window(BkOsk *osk)
{
    GdkDisplay *d = gdk_display_get_default();
    GdkMonitor *mon = target_monitor(osk);
    GdkRectangle wa = { 0, 0, 1280, 800 }, full = wa;
    if (mon) {
        gdk_monitor_get_workarea(mon, &wa);
        gdk_monitor_get_geometry(mon, &full);
    }
    int w = osk->width, h = osk->height;
    if (osk->kind != WIN_DOCKED) {
        if (!osk->placed) {
            const BkConfig *c = &osk->app->cfg;
            if (c->float_x >= 0 && c->float_y >= 0 && !osk->follow) {
                osk->pos_x = c->float_x;
                osk->pos_y = c->float_y;
            } else { /* embaixo, ao centro (com a moldura do gerenciador, se houver) */
                int deco = osk->kind == WIN_FLOAT_WM ? 34 : 0;
                osk->pos_x = wa.x + (wa.width - w) / 2;
                osk->pos_y = wa.y + wa.height - h - deco - 8;
            }
            osk->placed = TRUE;
        }
        /* nunca some da tela: o título (ou um pedaço da janela) fica ao alcance */
        GdkMonitor *m = gdk_display_get_monitor_at_point(d, osk->pos_x + w / 2, osk->pos_y + 8);
        if (!m)
            m = mon;
        if (m) {
            mon = m;
            gdk_monitor_get_geometry(mon, &full);
        }
        osk->pos_x = CLAMP(osk->pos_x, full.x - w + 80, full.x + full.width - 80);
        osk->pos_y = CLAMP(osk->pos_y, full.y, full.y + full.height - 40);
    }
    osk->placed_mon = mon;
#ifdef BK_HAVE_WAYLAND
    if (osk->layer) {
        GtkWindow *win = GTK_WINDOW(osk->win);
        gtk_widget_set_size_request(osk->area, w, h);
        if (mon)
            gtk_layer_set_monitor(win, mon);
        gboolean docked = osk->kind == WIN_DOCKED;
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, docked);
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, docked);
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, !docked);
        if (docked) {
            gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_LEFT, 0);
            gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_TOP, 0);
            gtk_layer_set_exclusive_zone(win, osk->app->cfg.reserve ? h : 0);
        } else {
            /* coordenadas do monitor inteiro: -1 ignora as zonas dos outros */
            gtk_layer_set_exclusive_zone(win, -1);
            gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_LEFT, osk->pos_x - full.x);
            gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_TOP, osk->pos_y - full.y);
        }
        gtk_window_resize(win, w, h);
        return;
    }
#endif
    GtkWindow *win = GTK_WINDOW(osk->win);
    if (osk->kind == WIN_FLOAT_WM) {
        /* o gerenciador redimensiona; a proporção das teclas fica */
        GdkGeometry g = { 0 };
        g.min_width = (int)ceil(osk->width * 22 / MAX(22, osk->u));
        g.min_height = (int)ceil(osk->height * 22 / MAX(22, osk->u));
        g.min_aspect = g.max_aspect = (double)w / h;
        gtk_window_set_geometry_hints(win, NULL, &g, GDK_HINT_MIN_SIZE | GDK_HINT_ASPECT);
        int cw, ch;
        gtk_window_get_size(win, &cw, &ch);
        if (cw != w || ch != h)
            gtk_window_resize(win, w, h);
        if (!gtk_widget_get_mapped(osk->win)) /* aberta, quem move é o usuário */
            gtk_window_move(win, osk->pos_x, osk->pos_y);
        return;
    }
    gtk_widget_set_size_request(osk->area, w, h);
    gtk_window_resize(win, w, h);
    if (osk->kind == WIN_FLOAT_OWN) {
        gtk_window_move(win, osk->pos_x, osk->pos_y);
        return;
    }
    /* encaixado embaixo, na borda do monitor (a área útil já desconta o
     * espaço que o próprio teclado reservou) */
    int x = full.x, y = full.y + full.height - h;
    gtk_window_move(win, x, y);
#ifdef BK_HAVE_X11
    GdkWindow *gw_ = gtk_widget_get_window(osk->win);
    if (!osk->override && gw_ && GDK_IS_X11_WINDOW(gw_) && mon) {
        /* reservar espaço: janelas maximizadas sobem (strut na borda de baixo) */
        int scr_h = HeightOfScreen(DefaultScreenOfDisplay(GDK_WINDOW_XDISPLAY(gw_)));
        int sc = gdk_window_get_scale_factor(gw_);
        long strut[12] = { 0 };
        if (osk->app->cfg.reserve) {
            strut[3] = (scr_h - (full.y + full.height) * sc) + h * sc;
            strut[10] = x * sc;
            strut[11] = (x + w) * sc - 1;
        }
        gdk_property_change(gw_, gdk_atom_intern_static_string("_NET_WM_STRUT_PARTIAL"),
                            gdk_atom_intern_static_string("CARDINAL"), 32, GDK_PROP_MODE_REPLACE,
                            (guchar *)strut, 12);
    }
#endif
}

/* As medidas saem de uma unidade u (a largura de uma tecla comum):
 *   largura = 2·moldura + 3·margem + teclas + coluna
 *   altura  = 2·moldura + título + barra de baixo + 2·margem + teclas
 * Flutuante: a largura é a escolhida (ou a do tamanho das teclas) e o resto
 * segue a proporção. Encaixado: a largura do monitor, sem passar de 42% da
 * altura dele; se sobrar largura, as teclas se esticam (como no celular). */
static void
osk_layout(BkOsk *osk)
{
    BkApp *app = osk->app;
    GdkMonitor *mon = target_monitor(osk);
    GdkRectangle wa = { 0, 0, 1280, 800 }, full = wa;
    if (mon) {
        gdk_monitor_get_workarea(mon, &wa);
        gdk_monitor_get_geometry(mon, &full);
    }
    if (app->cfg.scale != osk->last_scale) { /* mudou o tamanho das teclas: vale ele */
        if (osk->last_scale > 0)
            app->cfg.float_width = 0;
        osk->last_scale = app->cfg.scale;
    }
    const BkLayout *l = bk_layout_get(osk->st.layout);
    BkGeom m100;
    bk_geom_build(&m100, bk_panel_rows(l, BK_PANEL_MAIN), 100, 9);
    double per_w = m100.width / 100.0 + 3 * 0.22 + 1.5, per_h = m100.height / 100.0 + 2 * 0.22;
    gboolean own = osk->kind == WIN_FLOAT_OWN;
    double b = own ? 1 : 0, chrome_h = own ? 2 * b + 22 + 8 : 0;
    int target_w;
    double u;
    if (osk->kind == WIN_DOCKED) {
        target_w = full.width;
        u = MIN((target_w - 2 * b) / per_w, full.height * 0.42 / per_h);
        u = MIN(u, 46 * app->cfg.scale * 1.6);
    } else {
        int min_w = (int)ceil(22 * per_w + 2 * b);
        int def_w = (int)round(46 * app->cfg.scale * per_w + 2 * b);
        target_w = app->cfg.float_width > 0 ? app->cfg.float_width : def_w;
        target_w = CLAMP(target_w, min_w, MAX(min_w, full.width));
        u = (target_w - 2 * b) / per_w;
        u = MIN(u, (full.height * 0.7 - chrome_h) / per_h); /* nunca mais que 70% da altura */
    }
    osk->u = floor(MAX(20, u));
    osk->gap = round(MAX(3, osk->u * 0.09));
    osk->pad = round(osk->u * 0.22);
    osk->border = b;
    osk->title_h = own ? CLAMP(round(osk->u * 0.5), 18, 28) : 0;
    osk->resize_h = own ? CLAMP(round(osk->u * 0.16), 7, 10) : 0;
    bk_geom_build(&osk->geom, bk_panel_rows(l, osk->st.panel), osk->u, osk->gap);
    /* largura fixa pelo painel principal: trocar de painel não mexe na janela */
    BkGeom main_geom;
    bk_geom_build(&main_geom, bk_panel_rows(l, BK_PANEL_MAIN), osk->u, osk->gap);
    double gw = MAX(main_geom.width, osk->geom.width), gh = main_geom.height;
    if (osk->geom.width < gw) /* centra o painel mais estreito */
        for (int i = 0; i < osk->geom.n; i++)
            osk->geom.rect[i].x += round((gw - osk->geom.width) / 2);
    double colw = round(osk->u * 1.5);
    if (osk->kind == WIN_DOCKED) {
        double avail = target_w - 2 * b - 3 * osk->pad - colw;
        if (avail > gw + 1) { /* estica na horizontal */
            double sx = avail / gw;
            for (int i = 0; i < osk->geom.n; i++) {
                double x0 = round(osk->geom.rect[i].x * sx), x1 = round((osk->geom.rect[i].x + osk->geom.rect[i].w) * sx);
                osk->geom.rect[i].x = x0;
                osk->geom.rect[i].w = x1 - x0;
            }
            gw = avail;
        }
        colw = target_w - 2 * b - 3 * osk->pad - gw;
    } else {
        /* o que o arredondamento deixou vai para a coluna */
        colw = MAX(round(osk->u * 1.2), target_w - 2 * b - 3 * osk->pad - gw);
    }
    osk->kx = b + osk->pad;
    osk->ky = b + osk->title_h + osk->pad;
    osk->width = (int)round(2 * b + 3 * osk->pad + gw + colw);
    osk->height = (int)round(2 * b + osk->title_h + osk->resize_h + 2 * osk->pad + gh);
    /* a coluna: ocultar (só sem moldura), controle na tela, configurações */
    osk->col_on[COL_HIDE] = osk->kind == WIN_DOCKED;
    osk->col_on[COL_PAD] = osk->col_on[COL_SETTINGS] = TRUE;
    int n = 0;
    for (int i = 0; i < COL_N; i++)
        n += osk->col_on[i];
    double bh = floor((gh - (n - 1) * osk->gap) / n), y = osk->ky;
    for (int i = 0; i < COL_N; i++) {
        if (!osk->col_on[i])
            continue;
        osk->col[i] = (BkRect){ osk->kx + gw + osk->pad, y, colw, bh };
        y += bh + osk->gap;
    }
    if (g_getenv("BIG_KEYBOARD_DEBUG_GEOM")) {
        /* para os testes: a geometria (relativa à janela), sem nada digitado */
        g_printerr("geom %d %d kind %d\n", osk->width, osk->height, osk->kind);
        for (int i = 0; i < COL_N; i++)
            if (osk->col_on[i])
                g_printerr("col %d %.0f %.0f %.0f %.0f\n", i, osk->col[i].x, osk->col[i].y, osk->col[i].w,
                           osk->col[i].h);
        for (int i = 0; i < osk->geom.n; i++)
            g_printerr("key %d %d %s|%s %.0f %.0f %.0f %.0f\n", i, osk->geom.key[i]->kind,
                       osk->geom.key[i]->level[0] ? osk->geom.key[i]->level[0] : "",
                       osk->geom.key[i]->keysym ? osk->geom.key[i]->keysym : "", osk->kx + osk->geom.rect[i].x,
                       osk->ky + osk->geom.rect[i].y, osk->geom.rect[i].w, osk->geom.rect[i].h);
    }
    place_window(osk);
    gtk_widget_queue_draw(osk->area);
}

#ifdef BK_HAVE_X11
/* Acima da tela cheia (override-redirect): volta ao topo quando o
 * gerenciador reempilha (o mesmo do controle na tela). */
static GdkFilterReturn
on_root_event(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev;
    BkOsk *osk = data;
    XEvent *e = xev;
    if (e->type == PropertyNotify && osk->override && osk->win && gtk_widget_get_visible(osk->win)) {
        const char *name = gdk_x11_get_xatom_name(e->xproperty.atom);
        if (!strcmp(name, "_NET_CLIENT_LIST_STACKING") || !strcmp(name, "_NET_ACTIVE_WINDOW"))
            gdk_window_raise(gtk_widget_get_window(osk->win));
    }
    return GDK_FILTER_CONTINUE;
}
#endif

/* Moldura do gerenciador: o fechar oculta, e o lugar é lembrado. */
static gboolean
on_delete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w;
    (void)ev;
    bk_osk_set_visible(data, FALSE);
    return TRUE;
}

static gboolean
on_configure(GtkWidget *w, GdkEventConfigure *ev, gpointer data)
{
    (void)ev;
    BkOsk *osk = data;
    if (osk->kind != WIN_FLOAT_WM || !gtk_widget_get_visible(w))
        return FALSE;
    int x, y;
    gtk_window_get_position(GTK_WINDOW(w), &x, &y);
    if (x != osk->pos_x || y != osk->pos_y) {
        osk->pos_x = osk->app->cfg.float_x = x;
        osk->pos_y = osk->app->cfg.float_y = y;
        osk->placed = TRUE;
        schedule_save(osk);
    }
    return FALSE;
}

/* O gerenciador mudou o tamanho: as teclas acompanham. */
static void
on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer data)
{
    (void)w;
    BkOsk *osk = data;
    if (osk->kind != WIN_FLOAT_WM || abs(a->width - osk->width) <= 1)
        return;
    osk->app->cfg.float_width = a->width;
    osk_layout(osk);
    schedule_save(osk);
}

static void
build_window(BkOsk *osk)
{
    choose_kind(osk, &osk->kind, &osk->layer, &osk->override, &osk->level);
    GtkWindow *win;
    osk->win = gtk_window_new(osk->override ? GTK_WINDOW_POPUP : GTK_WINDOW_TOPLEVEL);
    win = GTK_WINDOW(osk->win);
    gtk_window_set_title(win, "Teclado virtual");
    gtk_window_set_role(win, "big-keyboard");
    gtk_window_set_icon_name(win, "big-keyboard");
    gtk_application_add_window(osk->app->gtk, win);
#ifdef BK_HAVE_WAYLAND
    if (osk->layer) {
        gtk_layer_init_for_window(win);
        gtk_layer_set_namespace(win, "big-keyboard");
        gtk_layer_set_layer(win, osk->level == 2 ? GTK_LAYER_SHELL_LAYER_OVERLAY : GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    }
#endif
    if (!osk->layer && !osk->override) {
        /* nunca pega o foco: as teclas vão para o aplicativo */
        gtk_window_set_accept_focus(win, FALSE);
        gtk_window_set_focus_on_map(win, FALSE);
        gtk_window_set_skip_taskbar_hint(win, TRUE);
        gtk_window_set_skip_pager_hint(win, TRUE);
        gtk_window_stick(win);
        if (osk->kind == WIN_DOCKED) {
            gtk_window_set_type_hint(win, GDK_WINDOW_TYPE_HINT_DOCK);
            gtk_window_set_decorated(win, FALSE);
            gtk_window_set_keep_above(win, TRUE);
        } else {
            /* a moldura do Window Maker: título, fechar, barra de redimensionar */
            gtk_window_set_type_hint(win, GDK_WINDOW_TYPE_HINT_UTILITY);
            gtk_window_set_decorated(win, TRUE);
            gtk_window_set_resizable(win, TRUE);
            gtk_window_set_keep_above(win, osk->level >= 1);
            g_signal_connect(osk->win, "delete-event", G_CALLBACK(on_delete), osk);
            g_signal_connect(osk->win, "configure-event", G_CALLBACK(on_configure), osk);
        }
    }
    osk->area = gtk_drawing_area_new();
    gtk_widget_add_events(osk->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                         GDK_TOUCH_MASK);
    g_signal_connect(osk->area, "draw", G_CALLBACK(on_draw), osk);
    g_signal_connect(osk->area, "button-press-event", G_CALLBACK(on_button), osk);
    g_signal_connect(osk->area, "button-release-event", G_CALLBACK(on_button), osk);
    g_signal_connect(osk->area, "motion-notify-event", G_CALLBACK(on_motion), osk);
    g_signal_connect(osk->area, "touch-event", G_CALLBACK(on_touch), osk);
    g_signal_connect(osk->area, "size-allocate", G_CALLBACK(on_size_allocate), osk);
    gtk_container_add(GTK_CONTAINER(osk->win), osk->area);
    gtk_widget_set_opacity(osk->win, osk->app->cfg.opacity);
    /* o leitor de tela anuncia a janela */
    atk_object_set_name(gtk_widget_get_accessible(osk->area), "Teclado na tela");
#ifdef BK_HAVE_X11
    static gboolean filter_added = FALSE;
    if (osk->override && !filter_added) {
        GdkWindow *root = gdk_get_default_root_window();
        gdk_window_set_events(root, gdk_window_get_events(root) | GDK_PROPERTY_CHANGE_MASK);
        gdk_window_add_filter(root, on_root_event, osk);
        filter_added = TRUE;
    }
#endif
    gtk_widget_realize(osk->win);
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
    osk->col_down = -1;
    osk->last_scale = app->cfg.scale;
    g_strlcpy(osk->layout_cfg, app->cfg.layout, sizeof osk->layout_cfg);
    bk_state_init(&osk->st, configured_layout(app));
    build_window(osk);
    osk_layout(osk);
    return osk;
}

/* O layout das configurações: o escolhido, ou o da sessão em "auto". */
static int
configured_layout(BkApp *app)
{
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
    return layout;
}

void
bk_osk_set_visible(BkOsk *osk, gboolean visible)
{
    osk->auto_shown = FALSE; /* quem chamou agora decide (o foco marca de novo) */
    if (visible == bk_osk_visible(osk))
        return;
    if (visible) {
        osk_follow(osk);
        osk_layout(osk); /* monitor e escala do momento */
        gtk_widget_show_all(osk->win);
        osk_layout(osk); /* X11: o strut precisa da janela mapeada */
    } else {
        bk_osk_release(osk);
        gtk_widget_hide(osk->win);
        g_clear_object(&osk->pinned);
        osk->follow = FALSE;
        osk->rdrag = DRAG_NONE;
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
    return osk && osk->win && gtk_widget_get_visible(osk->win);
}

void
bk_osk_set_auto_shown(BkOsk *osk, gboolean auto_shown)
{
    osk->auto_shown = auto_shown;
}

gboolean
bk_osk_auto_shown(BkOsk *osk)
{
    return osk->auto_shown;
}

void
bk_osk_release(BkOsk *osk)
{
    stop_repeat(osk);
    osk->col_down = -1;
    osk->drag = DRAG_NONE;
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
    if (strcmp(osk->layout_cfg, osk->app->cfg.layout)) {
        /* outro layout nas configurações: vale já, com nada apertado */
        g_strlcpy(osk->layout_cfg, osk->app->cfg.layout, sizeof osk->layout_cfg);
        bk_osk_release(osk);
        bk_state_init(&osk->st, configured_layout(osk->app));
        osk->cursor = -1;
        osk->point[0] = osk->point[1] = -1;
    }
    WinKind kind;
    gboolean layer, ovr;
    int level;
    choose_kind(osk, &kind, &layer, &ovr, &level);
    if (kind != osk->kind || layer != osk->layer || ovr != osk->override || level != osk->level) {
        /* outro tipo de janela (modo ou "por cima"): refaz, no mesmo estado */
        gboolean shown = bk_osk_visible(osk);
        bk_osk_release(osk);
        if (osk->after_paint_id) { /* um arrasto no meio: o relógio é da janela antiga */
            GdkFrameClock *fc = gtk_widget_get_frame_clock(osk->win);
            if (fc)
                g_signal_handler_disconnect(fc, osk->after_paint_id);
            osk->after_paint_id = 0;
        }
        osk->move_pending = osk->move_dirty = FALSE;
        gtk_widget_destroy(osk->win);
        osk->win = osk->area = NULL;
        build_window(osk);
        osk_layout(osk);
        if (shown) {
            gtk_widget_show_all(osk->win);
            osk_layout(osk);
        }
        bk_app_update_state(osk->app);
        return;
    }
    gtk_widget_set_opacity(osk->win, osk->app->cfg.opacity);
    if (osk->app->cfg.float_x < 0) /* "restaurar o lugar" nas configurações */
        osk->placed = FALSE;
    osk_layout(osk);
}

/* ---- toque remoto (celular; os dedos chegam por bk-remote.c) ---------------- */

/* Com um celular conectado, o teclado abre no monitor transmitido (o único
 * que o celular alcança), embaixo, ao centro, se estava em outro. */
static void
osk_follow(BkOsk *osk)
{
    BkRemote *r = osk->app->remote;
    if (!bk_remote_present(r) || osk->pinned)
        return;
    GdkMonitor *m = bk_remote_monitor(r);
    if (!m)
        return;
    osk->pinned = g_object_ref(m);
    if (osk->kind != WIN_DOCKED) {
        GdkMonitor *cur = osk->placed ? gdk_display_get_monitor_at_point(gdk_display_get_default(),
                                                                         osk->pos_x + osk->width / 2, osk->pos_y + 8)
                                      : NULL;
        if (cur != m) {
            osk->placed = FALSE;
            osk->follow = TRUE;
        }
    }
}

void
bk_osk_follow_remote(BkOsk *osk)
{
    if (!bk_osk_visible(osk) || osk->pinned)
        return;
    osk_follow(osk);
    if (!osk->pinned || osk->placed_mon == osk->pinned)
        return;
    /* outro monitor: o layer-shell só troca antes de mapear */
    bk_osk_release(osk);
    gtk_widget_hide(osk->win);
    osk_layout(osk);
    gtk_widget_show_all(osk->win);
    osk_layout(osk);
}

gboolean
bk_osk_remote_rect(BkOsk *osk, GdkMonitor *monitor, GdkRectangle *r)
{
    if (!bk_osk_visible(osk) || !monitor)
        return FALSE;
    GdkRectangle full;
    gdk_monitor_get_geometry(monitor, &full);
    int x, y;
    if (osk->layer) {
        if (osk->kind == WIN_DOCKED) {
            if (osk->placed_mon != monitor)
                return FALSE;
            x = full.x;
            y = full.y + full.height - osk->height;
        } else {
            x = osk->pos_x;
            y = osk->pos_y;
        }
    } else {
        GdkWindow *gw = gtk_widget_get_window(osk->area);
        if (!gw)
            return FALSE;
        gdk_window_get_origin(gw, &x, &y);
    }
    *r = (GdkRectangle){ x - full.x, y - full.y, osk->width, osk->height };
    return r->x < full.width && r->y < full.height && r->x + r->width > 0 && r->y + r->height > 0;
}

void
bk_osk_remote_touch(BkOsk *osk, GdkMonitor *monitor, const void *id, BkCtlPhase ph, double mx, double my)
{
    GdkEventSequence *seq = (GdkEventSequence *)id; /* só uma identidade, como a do GDK */
    gboolean ending = ph == BKC_END || ph == BKC_CANCEL;
    if (osk->rdrag != DRAG_NONE && id == osk->rdrag_id) {
        /* arrastar a janela pelo celular: pela barra de título (mover) ou
         * pela de baixo (largura), em coordenadas do monitor */
        if (ending) {
            osk->rdrag = DRAG_NONE;
            osk->app->cfg.float_x = osk->pos_x;
            osk->app->cfg.float_y = osk->pos_y;
            schedule_save(osk);
        } else if (osk->rdrag == DRAG_MOVE) {
            osk->pos_x = osk->rdrag_px + (int)round(mx - osk->rdrag_mx);
            osk->pos_y = osk->rdrag_py + (int)round(my - osk->rdrag_my);
            place_window(osk);
        } else {
            double ratio = osk->height > 0 ? (double)osk->width / osk->height : 3;
            int w = (int)round(osk->rdrag_w + MAX(mx - osk->rdrag_mx, (my - osk->rdrag_my) * ratio));
            if (w != osk->width) {
                osk->app->cfg.float_width = w;
                osk_layout(osk);
            }
        }
        return;
    }
    GdkRectangle r;
    if (!bk_osk_remote_rect(osk, monitor, &r)) {
        if (ending)
            pointer_up(osk, seq, -1, -1, TRUE);
        return;
    }
    double x = mx - r.x, y = my - r.y;
    switch (ph) {
    case BKC_BEGIN:
        if (osk->kind == WIN_FLOAT_OWN && osk->rdrag == DRAG_NONE && button_at(osk, x, y) < 0 &&
            (y < osk->border + osk->title_h || y >= osk->height - osk->border - osk->resize_h)) {
            osk->rdrag = y < osk->border + osk->title_h ? DRAG_MOVE : DRAG_RESIZE;
            osk->rdrag_id = id;
            osk->rdrag_mx = mx;
            osk->rdrag_my = my;
            osk->rdrag_px = osk->pos_x;
            osk->rdrag_py = osk->pos_y;
            osk->rdrag_w = osk->width;
            return;
        }
        pointer_down(osk, seq, x, y, NULL);
        break;
    case BKC_MOVE:
        pointer_move(osk, seq, x, y, NULL);
        break;
    case BKC_END:
        pointer_up(osk, seq, x, y, FALSE);
        break;
    case BKC_CANCEL:
        pointer_up(osk, seq, x, y, TRUE);
        break;
    }
}

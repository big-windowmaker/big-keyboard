/* SPDX-License-Identifier: GPL-2.0-or-later
 * O dockapp sem X11 (veja bk-dock.h). */
#include "bk-dock.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- geometria ------------------------------------------------------------ */

void
bk_dock_layout(BkDockLayout *l, int w, int h)
{
    memset(l, 0, sizeof *l);
    l->w = w > 8 ? w : 64;
    l->h = h > 8 ? h : 64;
    double s = MIN(l->w, l->h) / 64.0;
    double b = MAX(1, floor(s)); /* moldura de 1 px no tile de 64 */
    double gap = b, groove = 2 * b;
    double bw = floor((l->w - 2 * b - 2 * gap - groove) / 2);
    double bh = l->h - 2 * b - 2 * gap;
    l->border = b;
    l->btn[BK_HALF_KBD] = (BkRect){ b + gap, b + gap, bw, bh };
    l->btn[BK_HALF_PAD] = (BkRect){ l->w - b - gap - bw, b + gap, bw, bh };
}

static gboolean
inside(const BkRect *r, double x, double y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

BkHalf
bk_dock_hit(const BkDockLayout *l, double x, double y)
{
    if (inside(&l->btn[BK_HALF_KBD], x, y))
        return BK_HALF_KBD;
    if (inside(&l->btn[BK_HALF_PAD], x, y))
        return BK_HALF_PAD;
    return BK_HALF_NONE;
}

/* ---- clique e duplo clique ------------------------------------------------ */

void
bk_clicker_init(BkClicker *c, int dbl_ms, int dbl_dist)
{
    memset(c, 0, sizeof *c);
    c->dbl_ms = dbl_ms > 0 ? dbl_ms : 250;
    c->dbl_dist = dbl_dist > 0 ? dbl_dist : 5;
    c->pend_half = c->down_half = BK_HALF_NONE;
}

static BkClick
toggle_of(BkHalf h)
{
    return h == BK_HALF_KBD ? BK_CLICK_TOGGLE_KBD : h == BK_HALF_PAD ? BK_CLICK_TOGGLE_PAD : BK_CLICK_NONE;
}

BkClick
bk_clicker_flush(BkClicker *c)
{
    if (!c->pending)
        return BK_CLICK_NONE;
    c->pending = FALSE;
    c->second = FALSE;
    return toggle_of(c->pend_half);
}

BkClick
bk_clicker_press(BkClicker *c, BkHalf half, int x, int y, gint64 t)
{
    BkClick out = BK_CLICK_NONE;
    c->down = FALSE;
    c->second = FALSE;
    if (half == BK_HALF_NONE)
        return out; /* moldura ou sulco: nada */
    if (c->pending) {
        if (half == c->pend_half && t - c->pend_time <= c->dbl_ms && abs(x - c->pend_x) <= c->dbl_dist &&
            abs(y - c->pend_y) <= c->dbl_dist)
            c->second = TRUE;     /* o pendente continua até o segundo soltar */
        else
            out = bk_clicker_flush(c); /* outra metade, longe ou tarde: ações independentes */
    }
    c->down = TRUE;
    c->down_half = half;
    c->down_time = t;
    c->down_x = x;
    c->down_y = y;
    return out;
}

BkClick
bk_clicker_release(BkClicker *c, BkHalf half, gint64 t)
{
    if (!c->down)
        return BK_CLICK_NONE;
    c->down = FALSE;
    if (half != c->down_half) { /* soltou fora do botão: como um botão de verdade, nada */
        c->second = FALSE;
        return BK_CLICK_NONE;
    }
    if (c->second) {
        c->second = FALSE;
        c->pending = FALSE;
        return BK_CLICK_SETTINGS;
    }
    if (t - c->down_time >= c->dbl_ms) /* segurou além do intervalo: não vira duplo */
        return toggle_of(half);
    c->pending = TRUE;
    c->pend_half = half;
    c->pend_time = c->down_time;
    c->pend_x = c->down_x;
    c->pend_y = c->down_y;
    return BK_CLICK_NONE;
}

void
bk_clicker_cancel(BkClicker *c)
{
    c->down = FALSE;
    c->second = FALSE;
}

gint64
bk_clicker_deadline(const BkClicker *c)
{
    if (!c->pending || (c->down && c->second))
        return -1;
    return c->pend_time + c->dbl_ms;
}

BkClick
bk_clicker_timeout(BkClicker *c, gint64 now)
{
    gint64 d = bk_clicker_deadline(c);
    if (d < 0 || now < d)
        return BK_CLICK_NONE;
    return bk_clicker_flush(c);
}

/* ---- desenho -------------------------------------------------------------- */

static void
gray(cairo_t *cr, double v)
{
    cairo_set_source_rgb(cr, v, v, v);
}

static void
fill_rect(cairo_t *cr, double x, double y, double w, double h, double v)
{
    gray(cr, v);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

/* Relevo de 1 px: claro em cima e à esquerda (saliente) ou embaixo e à
 * direita (afundado). */
static void
bevel(cairo_t *cr, const BkRect *r, gboolean raised, gboolean dark)
{
    double hi = dark ? 0.50 : 1.0, lo = dark ? 0.06 : 0.33;
    fill_rect(cr, r->x, r->y, r->w, 1, raised ? hi : lo);
    fill_rect(cr, r->x, r->y, 1, r->h, raised ? hi : lo);
    fill_rect(cr, r->x, r->y + r->h - 1, r->w, 1, raised ? lo : hi);
    fill_rect(cr, r->x + r->w - 1, r->y, 1, r->h, raised ? lo : hi);
}

static void
rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* A tecla Enter: uma tecla alta (como a do teclado ISO) com a seta ↵
 * desenhada em vetor (sem depender do glifo da fonte). */
static void
draw_enter(cairo_t *cr, const BkRect *b, gboolean dark)
{
    double cw = round(b->w * 0.80), ch = round(MIN(b->h * 0.62, cw * 1.35));
    double x = b->x + floor((b->w - cw) / 2), y = b->y + floor((b->h - ch) / 2);
    double r = MAX(1.5, cw * 0.09);
    /* saia da tecla (a parte de baixo, mais escura) com contorno */
    rounded(cr, x + 0.5, y + 0.5, cw - 1, ch - 1, r);
    gray(cr, dark ? 0.30 : 0.47);
    cairo_fill_preserve(cr);
    gray(cr, 0);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    /* topo da tecla: mais claro, recuado (perspectiva) */
    double ix = 2.5 * cw / 23, it = 1.5 * cw / 23, ib = 4.5 * cw / 23;
    double tx = x + ix, ty = y + it, tw = cw - 2 * ix, th = ch - it - ib;
    rounded(cr, tx, ty, tw, th, MAX(1, r - 1));
    gray(cr, dark ? 0.72 : 0.95);
    cairo_fill(cr);
    /* brilho na borda de cima e à esquerda do topo */
    gray(cr, 1);
    cairo_rectangle(cr, tx + r * 0.6, ty, tw - r * 1.2, 1);
    cairo_fill(cr);
    /* seta ↵: desce pela direita e volta para a esquerda, com a ponta */
    double lw = MAX(2, cw * 0.10);
    double ax = tx + tw * 0.74, ay0 = ty + th * 0.20, ay = ty + th * 0.64, hx = tx + tw * 0.40;
    gray(cr, 0);
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_move_to(cr, ax, ay0);
    cairo_line_to(cr, ax, ay);
    cairo_line_to(cr, hx, ay);
    cairo_stroke(cr);
    double tip = tx + tw * 0.14, half = MAX(3, th * 0.14);
    cairo_move_to(cr, tip, ay);
    cairo_line_to(cr, hx + 0.5, ay - half);
    cairo_line_to(cr, hx + 0.5, ay + half);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* O direcional em cruz, com setas nos braços e o centro marcado. */
static void
draw_dpad(cairo_t *cr, const BkRect *b, gboolean dark, gboolean connected)
{
    double side = round(MIN(b->w, b->h) * 0.86);
    double arm = round(side / 3);
    if ((int)(side - arm) % 2) /* braços de mesma largura dos dois lados */
        side += 1;
    double x = b->x + floor((b->w - side) / 2), y = b->y + floor((b->h - side) / 2);
    double a0 = (side - arm) / 2, a1 = a0 + arm;
    /* sombra (1 px abaixo e à direita) e a cruz com contorno */
    for (int pass = 0; pass < 2; pass++) {
        double o = pass == 0 ? 1 : 0;
        cairo_new_path(cr);
        cairo_move_to(cr, x + a0 + o, y + o);
        cairo_line_to(cr, x + a1 + o, y + o);
        cairo_line_to(cr, x + a1 + o, y + a0 + o);
        cairo_line_to(cr, x + side + o, y + a0 + o);
        cairo_line_to(cr, x + side + o, y + a1 + o);
        cairo_line_to(cr, x + a1 + o, y + a1 + o);
        cairo_line_to(cr, x + a1 + o, y + side + o);
        cairo_line_to(cr, x + a0 + o, y + side + o);
        cairo_line_to(cr, x + a0 + o, y + a1 + o);
        cairo_line_to(cr, x + o, y + a1 + o);
        cairo_line_to(cr, x + o, y + a0 + o);
        cairo_line_to(cr, x + a0 + o, y + a0 + o);
        cairo_close_path(cr);
        if (pass == 0) {
            gray(cr, dark ? 0.02 : 0.33);
            cairo_fill(cr);
        } else {
            gray(cr, dark ? 0.78 : 0.20);
            cairo_fill(cr);
        }
    }
    /* brilho nas bordas de cima e da esquerda dos braços */
    gray(cr, dark ? 0.95 : 0.55);
    cairo_rectangle(cr, x + a0, y, arm, 1);
    cairo_rectangle(cr, x, y + a0, a0, 1);
    cairo_rectangle(cr, x, y + a0, 1, arm);
    cairo_rectangle(cr, x + a0, y, 1, a0);
    cairo_fill(cr);
    /* setas nos braços */
    double mid = side / 2, t = arm * 0.38, d = arm * 0.16;
    gray(cr, dark ? 0.30 : 0.80);
    const double tri[4][6] = {
        { mid, d, mid - t, d + t, mid + t, d + t },                             /* cima */
        { mid, side - d, mid - t, side - d - t, mid + t, side - d - t },        /* baixo */
        { d, mid, d + t, mid - t, d + t, mid + t },                             /* esquerda */
        { side - d, mid, side - d - t, mid - t, side - d - t, mid + t },        /* direita */
    };
    for (int i = 0; i < 4; i++) {
        cairo_move_to(cr, x + tri[i][0], y + tri[i][1]);
        cairo_line_to(cr, x + tri[i][2], y + tri[i][3]);
        cairo_line_to(cr, x + tri[i][4], y + tri[i][5]);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    /* centro: côncavo; azul com um controle físico ligado */
    double cr_r = arm * 0.28;
    cairo_arc(cr, x + mid, y + mid, cr_r, 0, 2 * M_PI);
    if (connected)
        cairo_set_source_rgb(cr, 0.20, 0.55, 1.0);
    else
        gray(cr, dark ? 0.55 : 0.36);
    cairo_fill(cr);
}

/* LED embaixo do botão, como os do Window Maker: verde com a janela aberta,
 * cinza fechada (apagado: mais fraco quando o botão está desabilitado). */
static void
draw_led(cairo_t *cr, const BkRect *led, gboolean on, gboolean dark, gboolean disabled)
{
    /* contorno afundado: escuro em cima e à esquerda, claro embaixo e à direita */
    fill_rect(cr, led->x - 1, led->y - 1, led->w + 2, led->h + 2, dark ? 0.04 : 0.20);
    fill_rect(cr, led->x, led->y + led->h, led->w + 1, 1, dark ? 0.45 : 1.0);
    fill_rect(cr, led->x + led->w, led->y, 1, led->h, dark ? 0.45 : 1.0);
    if (on)
        cairo_set_source_rgb(cr, 0.19, 0.86, 0.19);
    else
        gray(cr, disabled ? (dark ? 0.24 : 0.58) : (dark ? 0.36 : 0.47));
    cairo_rectangle(cr, led->x, led->y, led->w, led->h);
    cairo_fill(cr);
    if (on) { /* brilho do LED aceso */
        cairo_set_source_rgb(cr, 0.70, 1.0, 0.70);
        cairo_rectangle(cr, led->x, led->y, led->w, 1);
        cairo_fill(cr);
    }
}

void
bk_dock_led_rect(const BkDockLayout *l, int i, BkRect *led)
{
    const BkRect *b = &l->btn[i];
    double s = MIN(l->w, l->h) / 64.0;
    double lw = round(b->w * 0.46), lh = MAX(3, round(3 * s));
    led->x = b->x + floor((b->w - lw) / 2);
    led->y = b->y + b->h - lh - MAX(3, round(4 * s));
    led->w = lw;
    led->h = lh;
}

static double
face_of(const BkDockState *s, int i)
{
    gboolean act = s->active[i], hov = s->hover == i && !s->disabled[i], prs = s->pressed == i;
    if (s->dark)
        return prs ? 0.14 : act ? (hov ? 0.52 : 0.48) : hov ? 0.32 : 0.26;
    return prs ? 0.56 : act ? (hov ? 0.90 : 0.86) : hov ? 0.73 : 0.667;
}

void
bk_dock_draw(cairo_t *cr, const BkDockLayout *l, const BkDockState *s)
{
    double body = s->dark ? 0.20 : 0.667;
    cairo_save(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    fill_rect(cr, 0, 0, l->w, l->h, body);
    BkRect tile = { 0, 0, l->w, l->h };
    bevel(cr, &tile, TRUE, s->dark);
    /* sulco fino entre os botões */
    double gx = l->btn[BK_HALF_KBD].x + l->btn[BK_HALF_KBD].w;
    double gw = l->btn[BK_HALF_PAD].x - gx;
    fill_rect(cr, gx + floor(gw / 2) - 1, l->btn[0].y, 1, l->btn[0].h, s->dark ? 0.06 : 0.33);
    fill_rect(cr, gx + floor(gw / 2), l->btn[0].y, 1, l->btn[0].h, s->dark ? 0.42 : 1.0);
    for (int i = 0; i < 2; i++) {
        const BkRect *b = &l->btn[i];
        fill_rect(cr, b->x, b->y, b->w, b->h, face_of(s, i));
        gboolean sunk = s->pressed == i || s->active[i];
        bevel(cr, b, !sunk, s->dark);
        cairo_save(cr);
        cairo_rectangle(cr, b->x + 1, b->y + 1, b->w - 2, b->h - 2);
        cairo_clip(cr);
        if (s->pressed == i) /* a tecla afunda com o aperto */
            cairo_translate(cr, 1, 1);
        /* o símbolo sobe para dar lugar ao LED */
        BkRect led;
        bk_dock_led_rect(l, i, &led);
        BkRect sym = { b->x, b->y, b->w, led.y - 1 - b->y };
        if (s->disabled[i] && !s->active[i])
            cairo_push_group(cr);
        if (i == BK_HALF_KBD)
            draw_enter(cr, &sym, s->dark);
        else
            draw_dpad(cr, &sym, s->dark, s->pad_connected);
        if (s->disabled[i] && !s->active[i]) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.35);
        }
        draw_led(cr, &led, s->active[i], s->dark, s->disabled[i] && !s->active[i]);
        cairo_restore(cr);
    }
    cairo_restore(cr);
}

/* ---- intervalo de duplo clique ---------------------------------------------- */

int
bk_dock_double_click_ms(const char *path)
{
    int ms = 250;
    char *own = NULL, *data = NULL;
    if (!path) {
        const char *root = g_getenv("GNUSTEP_USER_ROOT");
        own = root && *root ? g_build_filename(root, "Defaults", "WindowMaker", NULL)
                            : g_build_filename(g_get_home_dir(), "GNUstep", "Defaults", "WindowMaker", NULL);
        path = own;
    }
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        GMatchInfo *m = NULL;
        GRegex *re = g_regex_new("\\bDoubleClickTime\\s*=\\s*\"?([0-9]+)\"?\\s*;", 0, 0, NULL);
        if (re && g_regex_match(re, data, 0, &m)) {
            char *v = g_match_info_fetch(m, 1);
            int n = atoi(v);
            if (n >= 100 && n <= 1000)
                ms = n;
            g_free(v);
        }
        g_match_info_free(m);
        if (re)
            g_regex_unref(re);
    }
    g_free(data);
    g_free(own);
    return ms;
}

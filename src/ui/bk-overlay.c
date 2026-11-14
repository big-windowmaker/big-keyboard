/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Controle na tela: uma sobreposição transparente em tela cheia com os
 * controles (bk-controls.c), ligados ao controle virtual (bk-vpad.c).
 *
 * Só recebe toques sobre os controles: a região de entrada é a união dos
 * círculos, o resto da tela continua sendo do jogo. Wayland: layer-shell
 * na camada OVERLAY (acima de janelas em tela cheia que o compositor deixe
 * cobrir), sem teclado; X11: janela DOCK acima, sem foco, com forma de
 * entrada (a transparência pede um compositor, como o picom da sessão).
 *
 * Editor (--edit-gamepad): arrastar move, a alça do canto redimensiona,
 * guias aparecem quando um centro alinha com outro, e uma barra OPENSTEP
 * no alto adiciona, remove, troca a função, ajusta zona morta,
 * sensibilidade e opacidade, salva e restaura o padrão.
 */
#include "bk-app.h"
#include "bk-controls.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>
#ifdef BK_HAVE_WAYLAND
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>
#endif

typedef enum { TB_ADD_BTN, TB_ADD_STICK, TB_ADD_DPAD, TB_DEL, TB_FUNC, TB_DZ, TB_SENS, TB_OPAC, TB_SAVE,
               TB_RESET, TB_DONE, TB_N } ToolButton;
static const char *tb_label[TB_N] = { "+ Botão", "+ Analógico", "+ Direcional", "Remover", "Função", "Zona morta",
                                      "Sensibilidade", "Opacidade", "Salvar", "Padrão", "Concluir" };

struct BkOverlay {
    BkApp *app;
    GtkWidget *win, *area;
    gboolean layer, edit;
    BkCtlLayout lay;
    BkCtlState st;
    BkCtlSink sink;
    int w, h;
    /* editor */
    int sel, drag;           /* controle selecionado / arrastado */
    gboolean resizing;
    double grab_dx, grab_dy;
    int guide_x, guide_y;    /* controle com quem alinha (-1 nenhum) */
    char status[160];
};

static void
s_button(void *d, int code, bool down)
{
    bk_vpad_button(((BkOverlay *)d)->app->vpad, code, down);
}
static void
s_stick(void *d, int side, double x, double y)
{
    bk_vpad_stick(((BkOverlay *)d)->app->vpad, side, x, y);
}
static void
s_trigger(void *d, int side, double v)
{
    bk_vpad_trigger(((BkOverlay *)d)->app->vpad, side, v);
}
static void
s_hat(void *d, int x, int y)
{
    bk_vpad_hat(((BkOverlay *)d)->app->vpad, x, y);
}
static void
s_sync(void *d)
{
    bk_vpad_sync(((BkOverlay *)d)->app->vpad);
}

/* ---- região de entrada --------------------------------------------------- */

static void
update_input_region(BkOverlay *ov)
{
    GdkWindow *gw = gtk_widget_get_window(ov->win);
    if (!gw)
        return;
    cairo_region_t *reg = cairo_region_create();
    if (ov->edit) {
        cairo_rectangle_int_t all = { 0, 0, ov->w, ov->h };
        cairo_region_union_rectangle(reg, &all); /* editando: a tela toda */
    } else {
        for (int i = 0; i < ov->lay.n; i++) {
            const BkCtl *c = &ov->lay.c[i];
            if (!c->enabled)
                continue;
            double d = bk_ctl_px(&ov->lay, c, ov->h) * 1.15;
            cairo_rectangle_int_t r = { (int)(c->x * ov->w - d / 2), (int)(c->y * ov->h - d / 2), (int)d, (int)d };
            cairo_region_union_rectangle(reg, &r);
        }
    }
    gdk_window_input_shape_combine_region(gw, reg, 0, 0);
    cairo_region_destroy(reg);
}

/* ---- desenho ------------------------------------------------------------- */

static void
tb_rect(BkOverlay *ov, int i, double *x, double *y, double *w, double *h)
{
    /* cabe na tela: botões mais estreitos em telas pequenas */
    double bw = MIN(104, (ov->w - 24.0) / TB_N - 4), bh = 26, total = TB_N * (bw + 4) - 4;
    *x = (ov->w - total) / 2 + i * (bw + 4);
    *y = 30;
    *w = bw;
    *h = bh;
}

static void
label(cairo_t *cr, const char *t, double cx, double cy, double fs, double r, double g, double b)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Helvetica, Nimbus Sans, DejaVu Sans, Sans Bold");
    pango_font_description_set_absolute_size(fd, fs * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, t, -1);
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, round(cx - w / 2.0), round(cy - h / 2.0));
    pango_cairo_show_layout(cr, l);
    pango_font_description_free(fd);
    g_object_unref(l);
}

static gboolean
held(BkOverlay *ov, int ci)
{
    for (int i = 0; i < BKC_TOUCHES; i++)
        if (ov->st.used[i] && ov->st.ctl[i] == ci)
            return TRUE;
    return FALSE;
}

static void
draw_control(BkOverlay *ov, cairo_t *cr, int i)
{
    const BkCtl *c = &ov->lay.c[i];
    double d = bk_ctl_px(&ov->lay, c, ov->h), r = d / 2, cx = c->x * ov->w, cy = c->y * ov->h;
    gboolean down = held(ov, i);
    double a = ov->lay.opacity * (c->enabled ? 1 : 0.35);
    /* relevo OPENSTEP num disco: luz em cima à esquerda, sombra embaixo
     * (caminho novo: senão o arco puxa uma linha do texto anterior) */
    cairo_new_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
    double top = down ? 0.45 : 0.86, bot = down ? 0.70 : 0.42;
    cairo_pattern_add_color_stop_rgba(g, 0, top, top, top, a);
    cairo_pattern_add_color_stop_rgba(g, 1, bot, bot, bot, a);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    cairo_set_line_width(cr, MAX(1.5, d * 0.03));
    cairo_set_source_rgba(cr, 0, 0, 0, MIN(1, a + 0.2));
    cairo_stroke(cr);
    if (c->kind == BKC_STICK || c->kind == BKC_DPAD) {
        /* área de movimento e o pino (centro, ou onde o dedo está) */
        cairo_set_source_rgba(cr, 0, 0, 0, a * 0.5);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, r * c->deadzone, 0, 2 * M_PI);
        cairo_stroke(cr);
    }
    label(cr, bk_ctl_label(c), cx, cy, MAX(12, d * (c->kind == BKC_BUTTON ? 0.38 : 0.3)), 0.05, 0.05, 0.05);
    if (ov->edit && i == ov->sel) {
        cairo_set_source_rgb(cr, 0.1, 0.45, 0.95);
        cairo_set_line_width(cr, 3);
        cairo_rectangle(cr, cx - r - 4, cy - r - 4, d + 8, d + 8);
        cairo_stroke(cr);
        cairo_rectangle(cr, cx + r - 6, cy + r - 6, 14, 14); /* alça de tamanho */
        cairo_fill(cr);
    }
}

static gboolean
on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)w;
    BkOverlay *ov = data;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, ov->edit ? 0.35 : 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (ov->edit) {
        /* guias de alinhamento */
        cairo_set_source_rgba(cr, 0.95, 0.85, 0.2, 0.9);
        cairo_set_line_width(cr, 1);
        if (ov->guide_x >= 0 && ov->sel >= 0) {
            double x = round(ov->lay.c[ov->sel].x * ov->w) + 0.5;
            cairo_move_to(cr, x, 0);
            cairo_line_to(cr, x, ov->h);
            cairo_stroke(cr);
        }
        if (ov->guide_y >= 0 && ov->sel >= 0) {
            double y = round(ov->lay.c[ov->sel].y * ov->h) + 0.5;
            cairo_move_to(cr, 0, y);
            cairo_line_to(cr, ov->w, y);
            cairo_stroke(cr);
        }
    }
    for (int i = 0; i < ov->lay.n; i++)
        draw_control(ov, cr, i);
    if (ov->edit) {
        double x, y, bw, bh;
        tb_rect(ov, 0, &x, &y, &bw, &bh);
        double total = TB_N * (bw + 4) - 4;
        bk_draw_titlebar(cr, x - 6, y - 26, total + 12, 22, "Editor do controle na tela", 12);
        cairo_set_source_rgb(cr, 0.667, 0.667, 0.667);
        cairo_rectangle(cr, x - 6, y - 4, total + 12, bh + 30);
        cairo_fill(cr);
        bk_draw_bevel(cr, x - 6, y - 4, total + 12, bh + 30, TRUE, FALSE);
        for (int i = 0; i < TB_N; i++) {
            tb_rect(ov, i, &x, &y, &bw, &bh);
            cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
            cairo_rectangle(cr, x, y, bw, bh);
            cairo_fill(cr);
            bk_draw_bevel(cr, x, y, bw, bh, TRUE, FALSE);
            label(cr, tb_label[i], x + bw / 2, y + bh / 2, bw < 90 ? 9.5 : 11, 0, 0, 0);
        }
        label(cr, ov->status, ov->w / 2.0, y + bh + 12, 11, 0, 0, 0);
    }
    return TRUE;
}

/* ---- editor ---------------------------------------------------------------- */

static void
set_status(BkOverlay *ov, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_vsnprintf(ov->status, sizeof ov->status, fmt, ap);
    va_end(ap);
}

static void
describe(BkOverlay *ov)
{
    if (ov->sel < 0) {
        set_status(ov, "Toque num controle para selecionar; arraste para mover, a alça azul muda o tamanho. "
                       "Opacidade %.0f%%.",
                   ov->lay.opacity * 100);
        return;
    }
    const BkCtl *c = &ov->lay.c[ov->sel];
    set_status(ov, "%s — tamanho %.0f%%, zona morta %.0f%%, sensibilidade %.1f×, opacidade %.0f%%", bk_ctl_label(c),
               c->size * 100, c->deadzone * 100, c->sens, ov->lay.opacity * 100);
}

static void
editor_button(BkOverlay *ov, int b)
{
    static const int funcs[] = { BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
                                 BTN_THUMBL, BTN_THUMBR, BTN_MODE };
    BkCtl *c = ov->sel >= 0 ? &ov->lay.c[ov->sel] : NULL;
    switch (b) {
    case TB_ADD_BTN:
    case TB_ADD_STICK:
    case TB_ADD_DPAD:
        if (ov->lay.n >= BKC_MAX) {
            set_status(ov, "Limite de %d controles.", BKC_MAX);
            return;
        }
        ov->lay.c[ov->lay.n] = (BkCtl){ b == TB_ADD_BTN ? BKC_BUTTON : b == TB_ADD_STICK ? BKC_STICK : BKC_DPAD,
                                        b == TB_ADD_BTN ? BTN_SOUTH : 0, 0.5, 0.5, b == TB_ADD_BTN ? 0.11 : 0.26,
                                        b == TB_ADD_BTN ? 0 : 0.15, 1.0, true };
        ov->sel = ov->lay.n++;
        break;
    case TB_DEL:
        if (!c)
            return;
        memmove(&ov->lay.c[ov->sel], &ov->lay.c[ov->sel + 1], (size_t)(ov->lay.n - ov->sel - 1) * sizeof(BkCtl));
        ov->lay.n--;
        ov->sel = -1;
        break;
    case TB_FUNC:
        if (!c)
            return;
        if (c->kind == BKC_BUTTON) {
            int k = 0;
            for (size_t i = 0; i < G_N_ELEMENTS(funcs); i++)
                if (funcs[i] == c->code)
                    k = (int)i;
            c->code = funcs[(k + 1) % G_N_ELEMENTS(funcs)];
        } else if (c->kind == BKC_STICK || c->kind == BKC_TRIGGER) {
            c->code = !c->code; /* esquerdo ↔ direito */
        }
        break;
    case TB_DZ:
        if (c && c->kind != BKC_BUTTON && c->kind != BKC_TRIGGER)
            c->deadzone = c->deadzone >= 0.5 ? 0.05 : c->deadzone + 0.05;
        break;
    case TB_SENS:
        if (c && c->kind == BKC_STICK)
            c->sens = c->sens >= 2.0 ? 0.6 : c->sens + 0.2;
        break;
    case TB_OPAC:
        ov->lay.opacity = ov->lay.opacity >= 0.95 ? 0.2 : ov->lay.opacity + 0.15;
        break;
    case TB_SAVE: {
        int fixed = 0;
        for (int i = 0; i < ov->lay.n; i++)
            fixed += bk_ctl_clamp(&ov->lay.c[i], ov->w, ov->h, ov->lay.scale);
        char *path = bk_ctl_path(ov->lay.name);
        GError *err = NULL;
        if (!bk_ctl_save(&ov->lay, path, &err))
            set_status(ov, "Não foi possível salvar: %s", err->message);
        else if (fixed)
            set_status(ov, "Salvo; %d controle(s) fora da tela foram trazidos para dentro.", fixed);
        else
            set_status(ov, "Salvo em %s.", path);
        g_clear_error(&err);
        g_free(path);
        gtk_widget_queue_draw(ov->area);
        return;
    }
    case TB_RESET: {
        char name[64];
        g_strlcpy(name, ov->lay.name, sizeof name);
        bk_ctl_default(&ov->lay);
        g_strlcpy(ov->lay.name, name, sizeof ov->lay.name);
        ov->sel = -1;
        break;
    }
    case TB_DONE:
        bk_overlay_set_visible(ov, TRUE, FALSE);
        return;
    }
    describe(ov);
    gtk_widget_queue_draw(ov->area);
}

static void
edit_press(BkOverlay *ov, double x, double y)
{
    for (int i = 0; i < TB_N; i++) {
        double bx, by, bw, bh;
        tb_rect(ov, i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            editor_button(ov, i);
            return;
        }
    }
    if (ov->sel >= 0) { /* a alça do selecionado */
        const BkCtl *c = &ov->lay.c[ov->sel];
        double r = bk_ctl_px(&ov->lay, c, ov->h) / 2, hx = c->x * ov->w + r, hy = c->y * ov->h + r;
        if (fabs(x - hx) < 14 && fabs(y - hy) < 14) {
            ov->resizing = TRUE;
            ov->drag = ov->sel;
            return;
        }
    }
    int i = bk_ctl_hit(&ov->lay, ov->w, ov->h, x, y);
    ov->sel = i;
    ov->drag = i;
    if (i >= 0) {
        ov->grab_dx = x - ov->lay.c[i].x * ov->w;
        ov->grab_dy = y - ov->lay.c[i].y * ov->h;
    }
    describe(ov);
    gtk_widget_queue_draw(ov->area);
}

static void
edit_move(BkOverlay *ov, double x, double y)
{
    if (ov->drag < 0)
        return;
    BkCtl *c = &ov->lay.c[ov->drag];
    if (ov->resizing) {
        double r = hypot(x - c->x * ov->w, y - c->y * ov->h) / sqrt(2);
        c->size = CLAMP(2 * r / (ov->h * ov->lay.scale), 0.04, 0.6);
    } else {
        c->x = (x - ov->grab_dx) / ov->w;
        c->y = (y - ov->grab_dy) / ov->h;
        /* encaixe: alinha com o centro de outro controle a menos de 8 px */
        ov->guide_x = ov->guide_y = -1;
        for (int i = 0; i < ov->lay.n; i++) {
            if (i == ov->drag)
                continue;
            if (fabs((ov->lay.c[i].x - c->x) * ov->w) < 8) {
                c->x = ov->lay.c[i].x;
                ov->guide_x = i;
            }
            if (fabs((ov->lay.c[i].y - c->y) * ov->h) < 8) {
                c->y = ov->lay.c[i].y;
                ov->guide_y = i;
            }
        }
    }
    describe(ov);
    gtk_widget_queue_draw(ov->area);
}

static void
edit_release(BkOverlay *ov)
{
    if (ov->drag >= 0 && bk_ctl_clamp(&ov->lay.c[ov->drag], ov->w, ov->h, ov->lay.scale))
        set_status(ov, "O controle saía da tela e voltou para dentro.");
    ov->drag = -1;
    ov->resizing = FALSE;
    ov->guide_x = ov->guide_y = -1;
    gtk_widget_queue_draw(ov->area);
}

/* ---- entrada -------------------------------------------------------------- */

static gboolean
on_touch(GtkWidget *w, GdkEventTouch *ev, gpointer data)
{
    (void)w;
    BkOverlay *ov = data;
    if (ov->edit) {
        if (ev->type == GDK_TOUCH_BEGIN)
            edit_press(ov, ev->x, ev->y);
        else if (ev->type == GDK_TOUCH_UPDATE)
            edit_move(ov, ev->x, ev->y);
        else
            edit_release(ov);
        return TRUE;
    }
    BkCtlPhase ph = ev->type == GDK_TOUCH_BEGIN    ? BKC_BEGIN
                    : ev->type == GDK_TOUCH_UPDATE ? BKC_MOVE
                    : ev->type == GDK_TOUCH_END    ? BKC_END
                                                   : BKC_CANCEL;
    bk_ctl_touch(&ov->st, &ov->lay, ov->w, ov->h, ev->sequence, ph, ev->x, ev->y, &ov->sink);
    gtk_widget_queue_draw(ov->area);
    return TRUE;
}

/* O mouse vale como um dedo (telas sem toque, testes): uma identidade só
 * para o toque do mouse, a mesma no clique, no movimento e ao soltar. */
static const int mouse_seq = 1;

static gboolean
on_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    BkOverlay *ov = data;
    if (g_getenv("BIG_KEYBOARD_DEBUG_INPUT"))
        g_printerr("overlay button %d type %d at %.0f,%.0f\n", ev->button, ev->type, ev->x, ev->y);
    if (ev->button != 1 || (ev->type != GDK_BUTTON_PRESS && ev->type != GDK_BUTTON_RELEASE))
        return FALSE;
    if (ov->edit) {
        if (ev->type == GDK_BUTTON_PRESS)
            edit_press(ov, ev->x, ev->y);
        else
            edit_release(ov);
        return TRUE;
    }
    bk_ctl_touch(&ov->st, &ov->lay, ov->w, ov->h, &mouse_seq, ev->type == GDK_BUTTON_PRESS ? BKC_BEGIN : BKC_END,
                 ev->x, ev->y, &ov->sink);
    gtk_widget_queue_draw(ov->area);
    return TRUE;
}

static gboolean
on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    BkOverlay *ov = data;
    if (g_getenv("BIG_KEYBOARD_DEBUG_INPUT"))
        g_printerr("overlay motion state %x at %.0f,%.0f\n", ev->state, ev->x, ev->y);
    if (!(ev->state & GDK_BUTTON1_MASK))
        return FALSE;
    if (ov->edit)
        edit_move(ov, ev->x, ev->y);
    else
        bk_ctl_touch(&ov->st, &ov->lay, ov->w, ov->h, &mouse_seq, BKC_MOVE, ev->x, ev->y, &ov->sink);
    gtk_widget_queue_draw(ov->area);
    return TRUE;
}

static void
on_size(GtkWidget *w, GdkRectangle *a, gpointer data)
{
    (void)w;
    BkOverlay *ov = data;
    ov->w = a->width;
    ov->h = a->height;
    update_input_region(ov);
}

/* ---- público -------------------------------------------------------------- */

static void
load_layout(BkOverlay *ov)
{
    char *path = bk_ctl_path(ov->app->cfg.overlay_layout);
    if (!bk_ctl_load(&ov->lay, path, NULL)) {
        bk_ctl_default(&ov->lay);
        g_strlcpy(ov->lay.name, ov->app->cfg.overlay_layout, sizeof ov->lay.name);
        ov->lay.opacity = ov->app->cfg.overlay_opacity;
    }
    ov->lay.scale = ov->app->cfg.overlay_scale;
    g_free(path);
}

BkOverlay *
bk_overlay_new(BkApp *app)
{
    BkOverlay *ov = g_new0(BkOverlay, 1);
    ov->app = app;
    ov->sel = ov->drag = ov->guide_x = ov->guide_y = -1;
    ov->sink = (BkCtlSink){ s_button, s_stick, s_trigger, s_hat, s_sync, ov };
    bk_ctl_state_init(&ov->st);
    ov->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ov->win), "Big WM Virtual Keyboard — controle na tela");
    gtk_application_add_window(app->gtk, GTK_WINDOW(ov->win));
    GdkScreen *scr = gtk_widget_get_screen(ov->win);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(scr);
    if (rgba)
        gtk_widget_set_visual(ov->win, rgba);
    gtk_widget_set_app_paintable(ov->win, TRUE);
#ifdef BK_HAVE_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default()) && gtk_layer_is_supported()) {
        ov->layer = TRUE;
        GtkWindow *w = GTK_WINDOW(ov->win);
        gtk_layer_init_for_window(w);
        gtk_layer_set_namespace(w, "big-keyboard-gamepad");
        gtk_layer_set_layer(w, GTK_LAYER_SHELL_LAYER_OVERLAY);
        for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
            gtk_layer_set_anchor(w, e, TRUE);
        gtk_layer_set_exclusive_zone(w, -1);
        gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    }
#endif
    if (!ov->layer) {
        gtk_window_set_type_hint(GTK_WINDOW(ov->win), GDK_WINDOW_TYPE_HINT_DOCK);
        gtk_window_set_decorated(GTK_WINDOW(ov->win), FALSE);
        gtk_window_set_accept_focus(GTK_WINDOW(ov->win), FALSE);
        gtk_window_set_keep_above(GTK_WINDOW(ov->win), TRUE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(ov->win), TRUE);
        gtk_window_stick(GTK_WINDOW(ov->win));
    }
    ov->area = gtk_drawing_area_new();
    gtk_widget_add_events(ov->area,
                          GDK_TOUCH_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(ov->area, "draw", G_CALLBACK(on_draw), ov);
    g_signal_connect(ov->area, "touch-event", G_CALLBACK(on_touch), ov);
    g_signal_connect(ov->area, "button-press-event", G_CALLBACK(on_button), ov);
    g_signal_connect(ov->area, "button-release-event", G_CALLBACK(on_button), ov);
    g_signal_connect(ov->area, "motion-notify-event", G_CALLBACK(on_motion), ov);
    g_signal_connect(ov->area, "size-allocate", G_CALLBACK(on_size), ov);
    gtk_container_add(GTK_CONTAINER(ov->win), ov->area);
    load_layout(ov);
    return ov;
}

void
bk_overlay_set_visible(BkOverlay *ov, gboolean visible, gboolean edit)
{
    if (visible) {
        if (!ov->app->vpad && !edit) {
            GError *err = NULL;
            ov->app->vpad = bk_vpad_new(ov->app->cfg.identity, &err);
            if (!ov->app->vpad) {
                g_warning("%s", err->message);
                g_clear_error(&err);
            }
        }
        if (!gtk_widget_get_visible(ov->win))
            load_layout(ov);
        ov->edit = edit;
        ov->sel = -1;
        describe(ov);
        if (!ov->layer) {
            GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
            if (!m)
                m = gdk_display_get_monitor(gdk_display_get_default(), 0);
            GdkRectangle g;
            gdk_monitor_get_geometry(m, &g);
            gtk_window_move(GTK_WINDOW(ov->win), g.x, g.y);
            gtk_window_resize(GTK_WINDOW(ov->win), g.width, g.height);
        }
        gtk_widget_show_all(ov->win);
        update_input_region(ov);
        gtk_widget_queue_draw(ov->area);
    } else {
        bk_overlay_release(ov);
        gtk_widget_hide(ov->win);
        if (ov->app->vpad) { /* o jogo vê o controle sair: nada fica preso nem órfão */
            bk_vpad_free(ov->app->vpad);
            ov->app->vpad = NULL;
        }
    }
    bk_app_update_state(ov->app);
}

gboolean
bk_overlay_visible(BkOverlay *ov)
{
    return ov && gtk_widget_get_visible(ov->win);
}

void
bk_overlay_release(BkOverlay *ov)
{
    if (!ov)
        return;
    bk_ctl_release(&ov->st, &ov->lay, &ov->sink);
    if (ov->area)
        gtk_widget_queue_draw(ov->area);
}

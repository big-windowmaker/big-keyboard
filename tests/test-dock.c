/* SPDX-License-Identifier: GPL-2.0-or-later
 * O dockapp sem X11: geometria, áreas de clique, clique × duplo clique e
 * desenho (pixels). */
#include "../src/dockapp/bk-dock.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...)                                                                                                   \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__);                                                      \
            fprintf(stderr, __VA_ARGS__);                                                                               \
            fputc('\n', stderr);                                                                                        \
            fails++;                                                                                                    \
        }                                                                                                               \
    } while (0)

/* ---- geometria e áreas de clique ------------------------------------------ */

static void
test_layout(void)
{
    BkDockLayout l;
    bk_dock_layout(&l, 64, 64);
    CHECK(l.border == 1, "moldura de 1 px: %g", l.border);
    const BkRect *k = &l.btn[BK_HALF_KBD], *p = &l.btn[BK_HALF_PAD];
    CHECK(k->w == p->w && k->h == p->h, "metades do mesmo tamanho");
    CHECK(k->x == 64 - (p->x + p->w), "metades simétricas");
    CHECK(k->w >= 28 && k->h >= 58, "área útil grande: %gx%g", k->w, k->h);
    CHECK(p->x - (k->x + k->w) <= 2, "sulco fino entre os botões");
    /* cada pixel dos botões é da sua metade; a moldura e o sulco de ninguém */
    int nk = 0, np = 0, nn = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            BkHalf h = bk_dock_hit(&l, x + 0.5, y + 0.5);
            nk += h == BK_HALF_KBD;
            np += h == BK_HALF_PAD;
            nn += h == BK_HALF_NONE;
            if (h == BK_HALF_KBD)
                CHECK(x < 32, "pixel %d,%d da esquerda está à direita", x, y);
            if (h == BK_HALF_PAD)
                CHECK(x >= 32, "pixel %d,%d da direita está à esquerda", x, y);
        }
    CHECK(nk == np && nk > 1600, "áreas iguais e grandes: %d %d", nk, np);
    /* área morta: só o anel de 2 px (moldura + folga) e o sulco de 2 px */
    CHECK(nn <= 2 * 64 * 4 - 16 + 2 * 60, "pouca área morta: %d", nn);
    CHECK(bk_dock_hit(&l, 0, 32) == BK_HALF_NONE && bk_dock_hit(&l, 31.5, 32) == BK_HALF_NONE &&
              bk_dock_hit(&l, 32.5, 32) == BK_HALF_NONE,
          "moldura e sulco não acionam botão");
    /* outra escala (tile de 96 ou 128): a mesma proporção */
    bk_dock_layout(&l, 128, 128);
    CHECK(l.border == 2 && l.btn[0].w == l.btn[1].w && bk_dock_hit(&l, 20, 64) == BK_HALF_KBD &&
              bk_dock_hit(&l, 108, 64) == BK_HALF_PAD,
          "128×128: moldura %g, botões %g %g", l.border, l.btn[0].w, l.btn[1].w);
}

/* ---- clique e duplo clique ------------------------------------------------- */

static void
test_clicks(void)
{
    BkClicker c;
    bk_clicker_init(&c, 250, 5);

    /* clique simples: vale ao fim do intervalo, contado do aperto */
    CHECK(bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 1000) == BK_CLICK_NONE, "aperto");
    CHECK(bk_clicker_release(&c, BK_HALF_KBD, 1080) == BK_CLICK_NONE, "solta: espera o intervalo");
    CHECK(bk_clicker_deadline(&c) == 1250, "prazo: %" G_GINT64_FORMAT, bk_clicker_deadline(&c));
    CHECK(bk_clicker_timeout(&c, 1249) == BK_CLICK_NONE, "cedo demais");
    CHECK(bk_clicker_timeout(&c, 1250) == BK_CLICK_TOGGLE_KBD, "clique no Enter alterna o teclado");
    CHECK(bk_clicker_deadline(&c) == -1, "nada pendente");

    /* duplo clique: só as configurações, nenhuma alternância */
    bk_clicker_press(&c, BK_HALF_PAD, 45, 30, 2000);
    bk_clicker_release(&c, BK_HALF_PAD, 2060);
    CHECK(bk_clicker_press(&c, BK_HALF_PAD, 46, 31, 2150) == BK_CLICK_NONE, "segundo aperto");
    CHECK(bk_clicker_deadline(&c) == -1, "segurando o segundo: nada vence");
    CHECK(bk_clicker_timeout(&c, 2400) == BK_CLICK_NONE, "segurando o segundo além do prazo: nada");
    CHECK(bk_clicker_release(&c, BK_HALF_PAD, 2410) == BK_CLICK_SETTINGS, "duplo clique: configurações");
    CHECK(bk_clicker_timeout(&c, 9999) == BK_CLICK_NONE && bk_clicker_deadline(&c) == -1,
          "duplo clique não deixa alternância pendente");

    /* metades diferentes, rápido: duas ações independentes, na hora */
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 3000);
    bk_clicker_release(&c, BK_HALF_KBD, 3050);
    CHECK(bk_clicker_press(&c, BK_HALF_PAD, 45, 30, 3100) == BK_CLICK_TOGGLE_KBD,
          "aperto na outra metade dispara o pendente");
    bk_clicker_release(&c, BK_HALF_PAD, 3150);
    CHECK(bk_clicker_timeout(&c, 3350) == BK_CLICK_TOGGLE_PAD, "e depois o seu");

    /* lento: dois cliques simples */
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 4000);
    bk_clicker_release(&c, BK_HALF_KBD, 4050);
    CHECK(bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 4300) == BK_CLICK_TOGGLE_KBD, "tarde: o primeiro vale");
    bk_clicker_release(&c, BK_HALF_KBD, 4350);
    CHECK(bk_clicker_timeout(&c, 4550) == BK_CLICK_TOGGLE_KBD, "e o segundo também");

    /* longe: não é duplo */
    bk_clicker_press(&c, BK_HALF_KBD, 5, 10, 5000);
    bk_clicker_release(&c, BK_HALF_KBD, 5040);
    CHECK(bk_clicker_press(&c, BK_HALF_KBD, 20, 50, 5100) == BK_CLICK_TOGGLE_KBD, "longe: o primeiro vale");
    bk_clicker_release(&c, BK_HALF_KBD, 5140);
    bk_clicker_flush(&c);

    /* segurou além do intervalo: alterna ao soltar, sem esperar */
    bk_clicker_press(&c, BK_HALF_PAD, 45, 30, 6000);
    CHECK(bk_clicker_release(&c, BK_HALF_PAD, 6400) == BK_CLICK_TOGGLE_PAD, "aperto longo");

    /* soltou fora do botão (ou no sulco): nada, como um botão de verdade */
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 7000);
    CHECK(bk_clicker_release(&c, BK_HALF_NONE, 7050) == BK_CLICK_NONE && bk_clicker_deadline(&c) == -1,
          "soltou fora");
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 7100);
    CHECK(bk_clicker_release(&c, BK_HALF_PAD, 7150) == BK_CLICK_NONE && bk_clicker_deadline(&c) == -1,
          "soltou na outra metade");

    /* moldura: nada */
    CHECK(bk_clicker_press(&c, BK_HALF_NONE, 0, 0, 7500) == BK_CLICK_NONE &&
              bk_clicker_release(&c, BK_HALF_NONE, 7550) == BK_CLICK_NONE && bk_clicker_deadline(&c) == -1,
          "clique na moldura");

    /* arrasto (o Window Maker move o tile): não alterna */
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 8000);
    bk_clicker_cancel(&c);
    CHECK(bk_clicker_release(&c, BK_HALF_KBD, 8300) == BK_CLICK_NONE && bk_clicker_deadline(&c) == -1,
          "arrastar não alterna");

    /* botão direito com um clique pendente: o clique vale antes do menu */
    bk_clicker_press(&c, BK_HALF_PAD, 45, 30, 9000);
    bk_clicker_release(&c, BK_HALF_PAD, 9040);
    CHECK(bk_clicker_flush(&c) == BK_CLICK_TOGGLE_PAD && bk_clicker_flush(&c) == BK_CLICK_NONE, "flush uma vez");

    /* cliques rápidos repetidos: 3 cliques = duplo + um pendente */
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 10000);
    bk_clicker_release(&c, BK_HALF_KBD, 10040);
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 10100);
    CHECK(bk_clicker_release(&c, BK_HALF_KBD, 10140) == BK_CLICK_SETTINGS, "triplo: duplo primeiro");
    bk_clicker_press(&c, BK_HALF_KBD, 10, 30, 10200);
    bk_clicker_release(&c, BK_HALF_KBD, 10240);
    CHECK(bk_clicker_timeout(&c, 10450) == BK_CLICK_TOGGLE_KBD, "triplo: o terceiro é um clique");
}

/* ---- desenho ------------------------------------------------------------------ */

static double
lum(cairo_surface_t *s, int x, int y)
{
    unsigned char *d = cairo_image_surface_get_data(s);
    int st = cairo_image_surface_get_stride(s);
    guint32 px = *(guint32 *)(d + y * st + x * 4);
    return (0.2126 * ((px >> 16) & 255) + 0.7152 * ((px >> 8) & 255) + 0.0722 * (px & 255)) / 255.0;
}

static cairo_surface_t *
render(const BkDockState *st)
{
    BkDockLayout l;
    bk_dock_layout(&l, 64, 64);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 64);
    cairo_t *cr = cairo_create(s);
    bk_dock_draw(cr, &l, st);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    return s;
}

/* Caixa da "tinta" (pixels longe da face do botão) dentro de um botão. */
static void
ink_box(cairo_surface_t *s, const BkRect *b, double face, int *w, int *h, double *darkest, double *lightest)
{
    int x0 = 99, y0 = 99, x1 = -1, y1 = -1;
    *darkest = 1;
    *lightest = 0;
    for (int y = (int)b->y + 2; y < (int)(b->y + b->h) - 2; y++)
        for (int x = (int)b->x + 2; x < (int)(b->x + b->w) - 2; x++) {
            double v = lum(s, x, y);
            if (v < *darkest)
                *darkest = v;
            if (v > *lightest)
                *lightest = v;
            if (fabs(v - face) > 0.08) {
                x0 = MIN(x0, x);
                y0 = MIN(y0, y);
                x1 = MAX(x1, x);
                y1 = MAX(y1, y);
            }
        }
    *w = x1 - x0 + 1;
    *h = y1 - y0 + 1;
}

/* Contraste WCAG: luminância relativa (sRGB linearizado) de cinzas. */
static double
linear(double v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double
contrast(double a, double b)
{
    a = linear(a);
    b = linear(b);
    return (MAX(a, b) + 0.05) / (MIN(a, b) + 0.05);
}

static void
test_draw(void)
{
    BkDockLayout l;
    bk_dock_layout(&l, 64, 64);
    for (int dark = 0; dark < 2; dark++) {
        BkDockState st = { .dark = dark, .hover = BK_HALF_NONE, .pressed = BK_HALF_NONE };
        cairo_surface_t *s = render(&st);
        const char *t = dark ? "escuro" : "claro";
        /* moldura fina: o 2º pixel da borda já é o fundo, não relevo */
        double body = lum(s, 1, 32);
        CHECK(fabs(lum(s, 0, 32) - body) > 0.1, "%s: moldura de 1 px", t);
        CHECK(fabs(lum(s, 1, 32) - lum(s, 1, 20)) < 0.01, "%s: sem moldura grossa", t);
        for (int i = 0; i < 2; i++) {
            const BkRect *b = &l.btn[i];
            double face = lum(s, (int)b->x + 3, (int)b->y + 3);
            int w, h;
            double dk, lt;
            ink_box(s, b, face, &w, &h, &dk, &lt);
            /* símbolo grande: 75–90 % da largura do botão */
            CHECK(w >= 0.75 * b->w && w <= 0.92 * b->w, "%s, metade %d: símbolo %d px de %g", t, i, w, b->w);
            CHECK(h >= 0.75 * b->w, "%s, metade %d: altura %d", t, i, h);
            /* contraste da silhueta do símbolo com a face (4,5:1, WCAG AA) */
            double c = MAX(contrast(dk, face), contrast(lt, face));
            CHECK(c >= 4.5, "%s, metade %d: contraste %.2f", t, i, c);
        }
        /* a seta ↵: a haste vertical à direita e a ponta à esquerda, escuras */
        const BkRect *k = &l.btn[BK_HALF_KBD];
        int arrow = 0;
        for (int x = (int)k->x; x < (int)(k->x + k->w); x++)
            for (int y = (int)(k->y + k->h * 0.45); y < (int)(k->y + k->h * 0.60); y++)
                arrow += lum(s, x, y) < 0.15;
        CHECK(arrow >= 20, "%s: seta do Enter (%d pixels escuros)", t, arrow);
        /* a seta contra o topo da tecla (o pixel mais claro da metade) */
        {
            int w, h;
            double dk, lt;
            ink_box(s, k, lum(s, (int)k->x + 3, (int)k->y + 3), &w, &h, &dk, &lt);
            CHECK(contrast(dk, lt) >= 7, "%s: seta sobre a tecla %.2f", t, contrast(dk, lt));
        }
        /* o centro do direcional marcado e a cruz: braços vazados nos cantos */
        const BkRect *p = &l.btn[BK_HALF_PAD];
        double pf = lum(s, (int)p->x + 3, (int)p->y + 3);
        int cx = (int)(p->x + p->w / 2), cy = (int)(p->y + p->h / 2);
        CHECK(fabs(lum(s, cx + 6, cy) - pf) > 0.2, "%s: braço do direcional", t);
        CHECK(fabs(lum(s, cx + 9, cy - 9) - pf) < 0.02, "%s: canto vazio (cruz, não quadrado)", t);
        cairo_surface_destroy(s);

        /* ativo: afundado (sombra em cima) e face diferente do normal */
        st.active[BK_HALF_KBD] = TRUE;
        cairo_surface_t *a = render(&st);
        st.active[BK_HALF_KBD] = FALSE;
        cairo_surface_t *n = render(&st);
        CHECK(lum(a, (int)k->x + 5, (int)k->y) < lum(n, (int)k->x + 5, (int)k->y), "%s: ativo afundado", t);
        CHECK(fabs(lum(a, (int)k->x + 4, (int)k->y + 4) - lum(n, (int)k->x + 4, (int)k->y + 4)) >= 0.15,
              "%s: ativo distinguível", t);
        CHECK(fabs(lum(a, (int)p->x + 4, (int)p->y + 4) - lum(n, (int)p->x + 4, (int)p->y + 4)) < 0.01,
              "%s: estados independentes", t);
        cairo_surface_destroy(a);
        /* desabilitado: o símbolo apagado */
        st.disabled[BK_HALF_PAD] = TRUE;
        cairo_surface_t *d = render(&st);
        CHECK(fabs(lum(d, cx + 6, cy) - pf) < fabs(lum(n, cx + 6, cy) - pf) * 0.5, "%s: desabilitado apagado", t);
        st.disabled[BK_HALF_PAD] = FALSE;
        cairo_surface_destroy(d);
        /* aperto e hover mudam só a metade deles */
        st.pressed = BK_HALF_PAD;
        cairo_surface_t *pr = render(&st);
        CHECK(fabs(lum(pr, (int)p->x + 4, (int)p->y + 4) - pf) > 0.05 &&
                  fabs(lum(pr, (int)k->x + 4, (int)k->y + 4) - lum(n, (int)k->x + 4, (int)k->y + 4)) < 0.01,
              "%s: aperto só na sua metade", t);
        st.pressed = BK_HALF_NONE;
        st.hover = BK_HALF_KBD;
        cairo_surface_t *hv = render(&st);
        CHECK(lum(hv, (int)k->x + 4, (int)k->y + 4) > lum(n, (int)k->x + 4, (int)k->y + 4), "%s: hover", t);
        cairo_surface_destroy(pr);
        cairo_surface_destroy(hv);
        cairo_surface_destroy(n);
    }
}

static void
test_double_click_time(void)
{
    char *dir = g_dir_make_tmp("bk-dock-XXXXXX", NULL);
    char *f = g_build_filename(dir, "WindowMaker", NULL);
    CHECK(bk_dock_double_click_ms(f) == 250, "sem arquivo: 250 ms");
    g_file_set_contents(f, "{\n  ColormapSize = 4;\n  DoubleClickTime = \"400\";\n}\n", -1, NULL);
    CHECK(bk_dock_double_click_ms(f) == 400, "DoubleClickTime do Window Maker");
    g_file_set_contents(f, "{ DoubleClickTime = 5; }", -1, NULL);
    CHECK(bk_dock_double_click_ms(f) == 250, "valor absurdo: padrão");
    g_unlink(f);
    g_rmdir(dir);
    g_free(f);
    g_free(dir);
}

int
main(void)
{
    test_layout();
    test_clicks();
    test_draw();
    test_double_click_time();
    if (!fails)
        printf("bk-dock: todos os casos passaram\n");
    return fails ? 1 : 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Geometria: fileiras centradas, toque, modo apontar e navegação de cursor. */
#include "bk-geom.h"
#include "big-wm-gamepad.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static const char *
lbl(const BkGeom *g, int i)
{
    return i < 0 ? "(nada)" : g->key[i]->level[0] ? g->key[i]->level[0] : "";
}

int
main(void)
{
    BkGeom g;
    const BkLayout *br = bk_layout_by_id("br");
    bk_geom_build(&g, bk_panel_rows(br, BK_PANEL_MAIN), 40, 4);
    CHECK(g.n > 50, "teclas: %d", g.n);
    CHECK(g.height == 5 * 40 + 4 * 4, "altura: %.0f", g.height);
    /* toque no meio da primeira tecla e fora de tudo */
    CHECK(!strcmp(lbl(&g, bk_geom_hit(&g, g.rect[0].x + 5, 5)), "'"), "toque na primeira");
    CHECK(bk_geom_hit(&g, -5, -5) == -1, "toque fora");
    /* modo apontar: analógico esquerdo no centro cai no meio da metade
     * esquerda (fileira do meio); empurrado para cima-esquerda, a primeira
     * fileira à esquerda; o direito todo à direita, o lado direito */
    int c = bk_geom_point(&g, 0, 0, 0);
    CHECK(c >= 0 && g.rect[c].x + g.rect[c].w <= g.width / 2 + 40, "centro esquerdo: %s", lbl(&g, c));
    int ul = bk_geom_point(&g, 0, -1, -1);
    CHECK(!strcmp(lbl(&g, ul), "'"), "cima-esquerda: %s", lbl(&g, ul));
    int r = bk_geom_point(&g, 1, 1, -1);
    CHECK(!strcmp(lbl(&g, r), "⌫"), "cima-direita do lado direito: %s", lbl(&g, r));
    int dr = bk_geom_point(&g, 1, 1, 1);
    CHECK(dr >= 0 && g.key[dr]->kind == BK_KEY_HIDE, "baixo-direita: %s", lbl(&g, dr));
    /* navegação de cursor sobre os retângulos (a mesma da tela de bloqueio) */
    bwg_rect rr[BK_GEOM_MAX];
    for (int i = 0; i < g.n; i++)
        rr[i] = (bwg_rect){ g.rect[i].x, g.rect[i].y, g.rect[i].w, g.rect[i].h };
    int q = bwg_spatial(rr, g.n, bk_geom_find(&g, &bk_panel_rows(br, 0)[0][1]), BWG_DOWN); /* de 1 para baixo */
    CHECK(!strcmp(lbl(&g, q), "q") || !strcmp(lbl(&g, q), "Tab"), "1 para baixo: %s", lbl(&g, q));
    int sp = bwg_spatial(rr, g.n, bk_geom_find(&g, &bk_panel_rows(br, 0)[3][6]), BWG_DOWN); /* de c para baixo */
    CHECK(!strcmp(lbl(&g, sp), " "), "c para baixo é o espaço: %s", lbl(&g, sp));
    if (!fails)
        printf("bk-geom: todos os casos passaram\n");
    return fails ? 1 : 0;
}

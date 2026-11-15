/* SPDX-License-Identifier: GPL-2.0-or-later
 * Controle na tela: multitoque independente, analógico, direcional,
 * liberação, limites da tela, gravar e ler. */
#include "bk-controls.h"

#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

typedef struct { bool btn[0x300]; double sx[2], sy[2], trig[2]; int hx, hy, syncs; } Rec;
static void r_button(void *d, int c, bool down) { ((Rec *)d)->btn[c] = down; }
static void r_stick(void *d, int s, double x, double y) { ((Rec *)d)->sx[s] = x; ((Rec *)d)->sy[s] = y; }
static void r_trigger(void *d, int s, double v) { ((Rec *)d)->trig[s] = v; }
static void r_hat(void *d, int x, int y) { ((Rec *)d)->hx = x; ((Rec *)d)->hy = y; }
static void r_sync(void *d) { ((Rec *)d)->syncs++; }

int
main(void)
{
    const double W = 1920, H = 1080;
    BkCtlLayout l;
    bk_ctl_default(&l);
    Rec r;
    memset(&r, 0, sizeof r);
    BkCtlSink sink = { r_button, r_stick, r_trigger, r_hat, r_sync, &r };
    BkCtlState s;
    bk_ctl_state_init(&s);
    /* localizar os controles do layout padrão */
    int st = -1, a = -1, dp = -1, rt = -1;
    for (int i = 0; i < l.n; i++) {
        if (l.c[i].kind == BKC_STICK && l.c[i].code == 0) st = i;
        if (l.c[i].kind == BKC_BUTTON && l.c[i].code == BTN_SOUTH) a = i;
        if (l.c[i].kind == BKC_DPAD) dp = i;
        if (l.c[i].kind == BKC_TRIGGER && l.c[i].code == 1) rt = i;
    }
    CHECK(st >= 0 && a >= 0 && dp >= 0 && rt >= 0, "layout padrão completo");
    double scx = l.c[st].x * W, scy = l.c[st].y * H, rad = bk_ctl_px(&l, &l.c[st], H) / 2;
    /* dedo 1 no analógico, dedo 2 no A, ao mesmo tempo */
    int f1, f2, f3;
    bk_ctl_touch(&s, &l, W, H, &f1, BKC_BEGIN, scx, scy, &sink);
    bk_ctl_touch(&s, &l, W, H, &f2, BKC_BEGIN, l.c[a].x * W, l.c[a].y * H, &sink);
    CHECK(r.btn[BTN_SOUTH] && r.sx[0] == 0, "A apertado, analógico no centro");
    bk_ctl_touch(&s, &l, W, H, &f1, BKC_MOVE, scx + rad, scy, &sink);
    CHECK(fabs(r.sx[0] - 1) < 1e-6 && fabs(r.sy[0]) < 1e-6, "analógico todo à direita: %.2f", r.sx[0]);
    bk_ctl_touch(&s, &l, W, H, &f1, BKC_MOVE, scx + rad * 0.10, scy, &sink);
    CHECK(r.sx[0] == 0, "dentro da zona morta: %.2f", r.sx[0]);
    bk_ctl_touch(&s, &l, W, H, &f1, BKC_MOVE, scx, scy - rad * 3, &sink);
    CHECK(fabs(r.sy[0] + 1) < 1e-6, "além do raio: limitado a -1: %.2f", r.sy[0]);
    /* o dedo do analógico escorrega sobre o A: o A não muda; tirar o dedo 2 solta o A */
    bk_ctl_touch(&s, &l, W, H, &f1, BKC_MOVE, l.c[a].x * W, l.c[a].y * H, &sink);
    CHECK(r.btn[BTN_SOUTH], "o A segue com o dedo 2");
    bk_ctl_touch(&s, &l, W, H, &f2, BKC_END, 0, 0, &sink);
    CHECK(!r.btn[BTN_SOUTH], "A solto ao tirar o dedo 2");
    bk_ctl_touch(&s, &l, W, H, &f1, BKC_CANCEL, 0, 0, &sink);
    CHECK(r.sx[0] == 0 && r.sy[0] == 0, "cancelar o toque centra o analógico");
    /* direcional: diagonal */
    double dcx = l.c[dp].x * W, dcy = l.c[dp].y * H, drad = bk_ctl_px(&l, &l.c[dp], H) / 2;
    bk_ctl_touch(&s, &l, W, H, &f3, BKC_BEGIN, dcx + drad * 0.7, dcy - drad * 0.7, &sink);
    CHECK(r.hx == 1 && r.hy == -1, "diagonal cima-direita: %d %d", r.hx, r.hy);
    bk_ctl_touch(&s, &l, W, H, &f3, BKC_MOVE, dcx, dcy + drad * 0.8, &sink);
    CHECK(r.hx == 0 && r.hy == 1, "para baixo: %d %d", r.hx, r.hy);
    /* RT + soltar tudo (troca de modo, emergência) */
    bk_ctl_touch(&s, &l, W, H, &f2, BKC_BEGIN, l.c[rt].x * W, l.c[rt].y * H, &sink);
    CHECK(r.trig[1] == 1.0, "RT apertado");
    bk_ctl_release(&s, &l, &sink);
    CHECK(r.trig[1] == 0 && r.hx == 0 && r.hy == 0, "soltar tudo");
    /* toque fora de qualquer controle não faz nada */
    int syncs = r.syncs;
    bk_ctl_touch(&s, &l, W, H, &f1, BKC_BEGIN, W / 2, H / 2, &sink);
    CHECK(r.syncs == syncs, "toque no vazio ignorado");
    /* um controle arrastado para fora da tela volta para dentro */
    BkCtl c = l.c[a];
    c.x = 1.3;
    c.y = -0.2;
    CHECK(bk_ctl_clamp(&c, W, H, 1.0) && c.x < 1 && c.y > 0, "limite da tela");
    /* gravar e ler de volta; layout inválido é recusado */
    char *dir = g_dir_make_tmp("bk-ctl-XXXXXX", NULL);
    char *f = g_build_filename(dir, "teste.ini", NULL);
    l.c[a].x = 0.5;
    g_strlcpy(l.name, "Corrida", sizeof l.name);
    CHECK(bk_ctl_save(&l, f, NULL), "gravar");
    BkCtlLayout m;
    CHECK(bk_ctl_load(&m, f, NULL) && m.n == l.n && fabs(m.c[a].x - 0.5) < 1e-9 && !strcmp(m.name, "Corrida"),
          "ler de volta");
    g_file_set_contents(f, "[Layout]\nName=x\n[Control 00]\nKind=laser\n", -1, NULL);
    CHECK(!bk_ctl_load(&m, f, NULL) && m.n == l.n, "layout sem controles válidos recusado (o anterior fica)");
    g_remove(f);
    g_rmdir(dir);
    char *p = bk_ctl_path("../../etc/x");
    CHECK(strstr(p, "/gamepad-layouts/") && !strstr(p, "/../"), "nome de arquivo sem barra: %s", p);
    g_free(p);
    /* botão do teclado: na tela, do tamanho de um dedo, sem cobrir nenhum
     * controle do layout padrão em telas 4:3, 16:10, 16:9 e 21:9 */
    BkCtlLayout d;
    bk_ctl_default(&d);
    const double ratios[] = { 4.0 / 3, 16.0 / 10, 16.0 / 9, 21.0 / 9 };
    const double heights[] = { 480, 1080, 2160 };
    for (size_t i = 0; i < G_N_ELEMENTS(ratios); i++)
        for (size_t j = 0; j < G_N_ELEMENTS(heights); j++) {
            double h = heights[j], w = h * ratios[i], cx, cy, rr;
            bk_ctl_switch_button(w, h, &cx, &cy, &rr);
            CHECK(rr * 2 >= 44 && cy - rr >= 0 && cx - rr >= 0 && cx + rr <= w, "botão do teclado na tela (%.0fx%.0f)",
                  w, h);
            for (int k = 0; k < d.n; k++) {
                double dist = hypot(d.c[k].x * w - cx, d.c[k].y * h - cy);
                CHECK(dist > rr * 1.15 + bk_ctl_px(&d, &d.c[k], h) / 2, "botão do teclado cobre %s (%.0fx%.0f)",
                      bk_ctl_label(&d.c[k]), w, h);
            }
            CHECK(bk_ctl_switch_hit(w, h, cx, cy) && bk_ctl_switch_hit(w, h, cx + rr, cy), "toque no botão (%.0fx%.0f)",
                  w, h);
            CHECK(!bk_ctl_switch_hit(w, h, cx, cy + rr * 1.5) && !bk_ctl_switch_hit(w, h, w / 2, h / 2),
                  "toque fora do botão (%.0fx%.0f)", w, h);
        }
    if (!fails)
        printf("bk-controls: todos os casos passaram\n");
    return fails ? 1 : 0;
}

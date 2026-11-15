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

    /* aparência: padrão de sempre, gravada e lida com o layout */
    CHECK(!strcmp(d.look.style, "classic") && !strcmp(d.look.buttons, "xbox") && !strcmp(d.look.anim, "sink"),
          "o padrão é o visual de sempre");
    {
        char *dir2 = g_dir_make_tmp("bkctl-XXXXXX", NULL), *f2 = g_build_filename(dir2, "l.ini", NULL);
        BkCtlLayout a, b;
        bk_ctl_template(&a, "playstation");
        CHECK(bk_ctl_save(&a, f2, NULL) && bk_ctl_load(&b, f2, NULL), "gravar e ler um modelo");
        CHECK(!memcmp(&a.look, &b.look, sizeof a.look) && b.n == a.n, "aparência de ida e volta: %s/%s", b.look.style,
              b.look.buttons);
        /* layout antigo (sem aparência) e valores estranhos: o padrão */
        g_file_set_contents(f2, "[Layout]\nName=v\nStyle=hacker\nButtons=sega\n[Control 00]\nKind=button\nCode=304\n"
                                "X=0.5\nY=0.5\nSize=0.1\n", -1, NULL);
        CHECK(bk_ctl_load(&b, f2, NULL) && !strcmp(b.look.style, "classic") && !strcmp(b.look.buttons, "xbox"),
              "aparência desconhecida vira o padrão: %s %s", b.look.style, b.look.buttons);
        g_remove(f2);
        g_rmdir(dir2);
        g_free(f2);
        g_free(dir2);
    }
    /* rótulos e cores por família: mesma posição, outro nome */
    {
        BkCtlLayout x;
        bk_ctl_default(&x);
        BkCtl south = { BKC_BUTTON, BTN_SOUTH, 0, 0, 0.1, 0, 1, true }, lt = { BKC_TRIGGER, 0, 0, 0, 0.1, 0, 1, true };
        double rgb[3];
        CHECK(!strcmp(bk_ctl_label_for(&x, &south), "A") && !strcmp(bk_ctl_label_for(&x, &lt), "LT"), "Xbox");
        CHECK(bk_ctl_color(&x, &south, rgb) && rgb[1] > rgb[0] && rgb[1] > rgb[2], "A verde");
        g_strlcpy(x.look.buttons, "playstation", sizeof x.look.buttons);
        CHECK(!strcmp(bk_ctl_label_for(&x, &south), "✕") && !strcmp(bk_ctl_label_for(&x, &lt), "L2"), "PlayStation");
        CHECK(bk_ctl_color(&x, &south, rgb) && rgb[2] > rgb[0], "✕ azul");
        g_strlcpy(x.look.buttons, "nintendo", sizeof x.look.buttons);
        CHECK(!strcmp(bk_ctl_label_for(&x, &south), "B") && !strcmp(bk_ctl_label_for(&x, &lt), "ZL"), "Nintendo");
        CHECK(!bk_ctl_color(&x, &lt, rgb), "gatilho sem cor de marca");
    }
    /* analógico flutuante: o centro é onde o dedo encostou */
    {
        BkCtlLayout x;
        bk_ctl_default(&x);
        g_strlcpy(x.look.stick, "floating", sizeof x.look.stick);
        Rec q;
        memset(&q, 0, sizeof q);
        BkCtlSink k2 = { r_button, r_stick, r_trigger, r_hat, r_sync, &q };
        BkCtlState s2;
        bk_ctl_state_init(&s2);
        int fl;
        double r0 = bk_ctl_px(&x, &x.c[st], H) / 2, bx = x.c[st].x * W + r0 * 0.6, by = x.c[st].y * H;
        bk_ctl_touch(&s2, &x, W, H, &fl, BKC_BEGIN, bx, by, &k2);
        CHECK(q.sx[0] == 0 && q.sy[0] == 0, "encostar fora do centro não inclina o flutuante");
        bk_ctl_touch(&s2, &x, W, H, &fl, BKC_MOVE, bx + r0, by, &k2);
        CHECK(fabs(q.sx[0] - 1) < 1e-6, "inclina a partir de onde encostou: %.2f", q.sx[0]);
        double vcx, vcy, kx, ky;
        bk_ctl_stick_visual(&x, &s2, W, H, st, &vcx, &vcy, &kx, &ky);
        CHECK(fabs(vcx - bx) < 1e-6 && fabs(kx - 1) < 1e-6 && fabs(ky) < 1e-6, "a base segue o dedo, o pino na borda");
        bk_ctl_touch(&s2, &x, W, H, &fl, BKC_END, bx + r0, by, &k2);
        bk_ctl_stick_visual(&x, &s2, W, H, st, &vcx, &vcy, &kx, &ky);
        CHECK(kx == 0 && fabs(vcx - x.c[st].x * W) < 1e-6, "solto: volta ao lugar, pino no centro");
        /* direcional: a direção para desenhar */
        int dd, hx, hy;
        double dcx2 = x.c[dp].x * W, dcy2 = x.c[dp].y * H, dr = bk_ctl_px(&x, &x.c[dp], H) / 2;
        bk_ctl_touch(&s2, &x, W, H, &dd, BKC_BEGIN, dcx2, dcy2 - dr * 0.8, &k2);
        bk_ctl_dpad_visual(&x, &s2, W, H, dp, &hx, &hy);
        CHECK(hx == 0 && hy == -1, "direcional para cima: %d %d", hx, hy);
    }
    /* modelos: dentro da tela, sem controles sobrepostos, longe do botão do teclado */
    {
        int nt = 0;
        for (const BkCtlTemplate *t = bk_ctl_templates(); t->id; t++, nt++) {
            BkCtlLayout x;
            CHECK(bk_ctl_template(&x, t->id) && x.n > 0 && x.name[0], "modelo %s", t->id);
            int guides = 0;
            for (int i = 0; i < x.n; i++)
                guides += x.c[i].kind == BKC_BUTTON && x.c[i].code == BTN_MODE && x.c[i].enabled;
            CHECK(guides == 1, "%s: um botão Guia (%d)", t->id, guides);
            const double rs[] = { 4.0 / 3, 16.0 / 9 };
            for (size_t ri = 0; ri < G_N_ELEMENTS(rs); ri++) {
                double h = 1080, w = h * rs[ri], cx, cy, rr;
                bk_ctl_switch_button(w, h, &cx, &cy, &rr);
                for (int i = 0; i < x.n; i++) {
                    BkCtl c = x.c[i];
                    CHECK(!bk_ctl_clamp(&c, w, h, x.scale), "%s: %s fora da tela (%.2f)", t->id,
                          bk_ctl_label_for(&x, &x.c[i]), rs[ri]);
                    double ri_ = bk_ctl_px(&x, &x.c[i], h) / 2;
                    CHECK(hypot(x.c[i].x * w - cx, x.c[i].y * h - cy) > rr * 1.15 + ri_,
                          "%s: %s cobre o botão do teclado (%.2f)", t->id, bk_ctl_label_for(&x, &x.c[i]), rs[ri]);
                    for (int j = i + 1; j < x.n; j++) {
                        double rj = bk_ctl_px(&x, &x.c[j], h) / 2;
                        CHECK(hypot((x.c[i].x - x.c[j].x) * w, (x.c[i].y - x.c[j].y) * h) >= ri_ + rj,
                              "%s: %s e %s se sobrepõem (%.2f)", t->id, bk_ctl_label_for(&x, &x.c[i]),
                              bk_ctl_label_for(&x, &x.c[j]), rs[ri]);
                    }
                }
            }
        }
        CHECK(nt >= 7, "sete modelos: %d", nt);
    }
    /* botão Guia: no padrão, e um layout antigo (Version 1 ou sem Version) o
     * ganha ao ser lido; desligado se algo do layout estiver no lugar dele.
     * Um layout novo (Version 2) sem ele fica sem (foi tirado de propósito). */
    {
        BkCtlLayout x;
        bk_ctl_default(&x);
        int g = -1;
        for (int i = 0; i < x.n; i++)
            if (x.c[i].kind == BKC_BUTTON && x.c[i].code == BTN_MODE)
                g = i;
        CHECK(g >= 0 && fabs(x.c[g].x - 0.5) < 1e-9, "Guia no padrão, ao centro");
        CHECK(!strcmp(bk_ctl_label_for(&x, &x.c[g]), "◉"), "rótulo do Guia (xbox)");
        char *dir3 = g_dir_make_tmp("bkctl-XXXXXX", NULL), *f3 = g_build_filename(dir3, "g.ini", NULL);
        BkCtlLayout b;
        g_file_set_contents(f3, "[Layout]\nName=v\nScale=1\n[Control 00]\nKind=button\nCode=304\n"
                                "X=0.88\nY=0.7\nSize=0.1\n", -1, NULL);
        CHECK(bk_ctl_load(&b, f3, NULL) && b.n == 2 && b.c[1].code == BTN_MODE && b.c[1].enabled,
              "layout antigo ganha o Guia (%d)", b.n);
        g_file_set_contents(f3, "[Layout]\nName=v\nVersion=1\nScale=1\n[Control 00]\nKind=button\nCode=304\n"
                                "X=0.5\nY=0.2\nSize=0.1\n", -1, NULL);
        CHECK(bk_ctl_load(&b, f3, NULL) && b.n == 2 && b.c[1].code == BTN_MODE && !b.c[1].enabled,
              "Guia desligado onde já há um controle");
        g_file_set_contents(f3, "[Layout]\nName=v\nVersion=2\nScale=1\n[Control 00]\nKind=button\nCode=304\n"
                                "X=0.88\nY=0.7\nSize=0.1\n", -1, NULL);
        CHECK(bk_ctl_load(&b, f3, NULL) && b.n == 1, "layout novo sem Guia fica sem");
        bk_ctl_default(&x);
        CHECK(bk_ctl_save(&x, f3, NULL) && bk_ctl_load(&b, f3, NULL) && b.n == x.n, "Guia não duplica ao regravar");
        g_remove(f3);
        g_rmdir(dir3);
        g_free(f3);
        g_free(dir3);
        CHECK(!bk_ctl_template(&x, "nao-existe"), "modelo desconhecido recusado");
    }
    if (!fails)
        printf("bk-controls: todos os casos passaram\n");
    return fails ? 1 : 0;
}

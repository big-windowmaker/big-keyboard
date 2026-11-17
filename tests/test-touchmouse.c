/* SPDX-License-Identifier: GPL-2.0-or-later
 * Mouse pelo toque: touchpad e toque direto, cliques, arrasto, clique
 * direito com dois dedos, rolagem, cancelamento. */
#include "bk-touchmouse.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

typedef struct {
    double px, py;           /* posição do ponteiro (rel acumulado ou abs) */
    int left_clicks, right_clicks;
    bool left, right;        /* apertados agora */
    int left_presses;
    double sx, sy;
    char log[256];
} Rec;

static void
r_rel(void *d, double dx, double dy)
{
    Rec *r = d;
    r->px += dx;
    r->py += dy;
}
static void
r_abs(void *d, double x, double y)
{
    Rec *r = d;
    r->px = x;
    r->py = y;
}
static void
r_button(void *d, int b, bool down)
{
    Rec *r = d;
    bool *st = b == BTN_LEFT ? &r->left : &r->right;
    if (b == BTN_LEFT && down)
        r->left_presses++;
    if (*st && !down)
        ++*(b == BTN_LEFT ? &r->left_clicks : &r->right_clicks);
    *st = down;
    strncat(r->log, b == BTN_LEFT ? (down ? "L" : "l") : (down ? "R" : "r"), sizeof r->log - strlen(r->log) - 1);
}
static void
r_scroll(void *d, double dx, double dy)
{
    Rec *r = d;
    r->sx += dx;
    r->sy += dy;
}

#define SINK(r) { r_rel, r_abs, r_button, r_scroll, &(r) }

int
main(void)
{
    int a, b; /* identidades de dois dedos */
    BkTouchMouse m;

    /* touchpad: toque rápido = clique esquerdo, sem mexer o ponteiro */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_TOUCHPAD, 1.5, 10);
        r.px = 500;
        r.py = 300;
        bk_tm_touch(&m, &a, BKC_BEGIN, 100, 100, 1000, &s);
        bk_tm_touch(&m, &a, BKC_MOVE, 103, 102, 1050, &s);
        bk_tm_touch(&m, &a, BKC_END, 103, 102, 1120, &s);
        CHECK(r.left_clicks == 1 && !strcmp(r.log, "Ll"), "toque = um clique esquerdo: %s", r.log);
        CHECK(r.px == 500 && r.py == 300, "o tremor do toque não move o ponteiro: %.0f,%.0f", r.px, r.py);
    }
    /* touchpad: arrastar move de forma relativa (velocidade), sem clicar */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_TOUCHPAD, 2.0, 10);
        bk_tm_touch(&m, &a, BKC_BEGIN, 100, 100, 0, &s);
        bk_tm_touch(&m, &a, BKC_MOVE, 120, 100, 30, &s);
        bk_tm_touch(&m, &a, BKC_MOVE, 150, 90, 60, &s);
        bk_tm_touch(&m, &a, BKC_END, 150, 90, 90, &s);
        CHECK(fabs(r.px - 100) < 1e-9 && fabs(r.py + 20) < 1e-9, "movimento ×2 desde o toque: %.1f,%.1f", r.px,
              r.py);
        CHECK(!r.left_clicks && !r.left, "arrastar não clica: %s", r.log);
    }
    /* touchpad: toque demorado sem mover não clica */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_TOUCHPAD, 1, 10);
        bk_tm_touch(&m, &a, BKC_BEGIN, 10, 10, 0, &s);
        bk_tm_touch(&m, &a, BKC_END, 10, 10, 900, &s);
        CHECK(!r.left_clicks, "segurar e soltar sem mover não clica: %s", r.log);
    }
    /* touchpad: segurar parado e então mover arrasta com o esquerdo */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_TOUCHPAD, 1, 10);
        bk_tm_touch(&m, &a, BKC_BEGIN, 10, 10, 0, &s);
        bk_tm_touch(&m, &a, BKC_MOVE, 40, 10, BKTM_HOLD_MS + 50, &s);
        CHECK(r.left && r.px == 30, "arrasto com o esquerdo apertado: %s %.0f", r.log, r.px);
        bk_tm_touch(&m, &a, BKC_END, 40, 10, BKTM_HOLD_MS + 90, &s);
        CHECK(!r.left && !strcmp(r.log, "Ll"), "soltar o dedo solta o esquerdo: %s", r.log);
    }
    /* touchpad: um dedo segurado + toque do segundo = clique direito, e o
     * primeiro não clica ao sair, mesmo segurado além do tempo */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_TOUCHPAD, 1, 10);
        bk_tm_touch(&m, &a, BKC_BEGIN, 10, 10, 0, &s);
        bk_tm_touch(&m, &b, BKC_BEGIN, 300, 200, 900, &s);
        bk_tm_touch(&m, &b, BKC_END, 300, 200, 1000, &s);
        bk_tm_touch(&m, &a, BKC_END, 10, 10, 1100, &s);
        CHECK(!strcmp(r.log, "Rr"), "só o clique direito: %s", r.log);
    }
    /* dois dedos que se movem: rolagem, sem cliques */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_TOUCHPAD, 1, 10);
        bk_tm_touch(&m, &a, BKC_BEGIN, 100, 300, 0, &s);
        bk_tm_touch(&m, &b, BKC_BEGIN, 160, 300, 10, &s);
        bk_tm_touch(&m, &a, BKC_MOVE, 100, 200, 40, &s);
        bk_tm_touch(&m, &b, BKC_MOVE, 160, 200, 40, &s);
        bk_tm_touch(&m, &a, BKC_END, 100, 200, 80, &s);
        bk_tm_touch(&m, &b, BKC_END, 160, 200, 80, &s);
        CHECK(fabs(r.sy - 100) < 1e-9 && fabs(r.sx) < 1e-9, "dedos para cima: rola 100 px para baixo: %.1f", r.sy);
        CHECK(!r.log[0] && r.px == 0, "rolar não clica nem move: %s", r.log);
    }
    /* direto: o ponteiro vai até o dedo; tirar sem arrastar = clique ali */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_DIRECT, 1, 16);
        bk_tm_touch(&m, &a, BKC_BEGIN, 800, 450, 0, &s);
        CHECK(r.px == 800 && r.py == 450 && !r.log[0], "toque leva o ponteiro sem clicar ainda");
        bk_tm_touch(&m, &a, BKC_END, 805, 452, 1500, &s);
        CHECK(!strcmp(r.log, "Ll") && r.px == 800, "clique esquerdo no ponto (mesmo demorado): %s", r.log);
    }
    /* direto: arrastar aperta no ponto do toque e arrasta */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_DIRECT, 1, 16);
        bk_tm_touch(&m, &a, BKC_BEGIN, 100, 100, 0, &s);
        bk_tm_touch(&m, &a, BKC_MOVE, 200, 150, 50, &s);
        CHECK(r.left && r.px == 200 && r.py == 150, "arrasto em curso: %s", r.log);
        bk_tm_touch(&m, &a, BKC_END, 200, 150, 90, &s);
        CHECK(!strcmp(r.log, "Ll") && !r.left, "um aperto, solto no fim: %s", r.log);
    }
    /* direto: dedo segurado + toque do segundo = clique direito no primeiro */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_DIRECT, 1, 16);
        bk_tm_touch(&m, &a, BKC_BEGIN, 640, 360, 0, &s);
        bk_tm_touch(&m, &b, BKC_BEGIN, 900, 500, 400, &s);
        bk_tm_touch(&m, &b, BKC_END, 900, 500, 500, &s);
        bk_tm_touch(&m, &a, BKC_END, 640, 360, 700, &s);
        CHECK(!strcmp(r.log, "Rr") && r.px == 640 && r.py == 360, "clique direito onde o ponteiro está: %s",
              r.log);
    }
    /* cancelar (dispositivo sumiu) no meio de um arrasto solta o botão */
    {
        Rec r = { 0 };
        BkTmSink s = SINK(r);
        bk_tm_init(&m, BKTM_DIRECT, 1, 16);
        bk_tm_touch(&m, &a, BKC_BEGIN, 100, 100, 0, &s);
        bk_tm_touch(&m, &a, BKC_MOVE, 300, 100, 30, &s);
        bk_tm_cancel(&m, &s);
        CHECK(!r.left && m.n == 0, "nada preso depois de cancelar");
        bk_tm_touch(&m, &a, BKC_BEGIN, 10, 10, 100, &s);
        bk_tm_touch(&m, &a, BKC_CANCEL, 10, 10, 120, &s);
        CHECK(r.left_presses == 1, "toque cancelado não clica");
    }
    { /* quando o mouse age: o pedido de 2026-11-17 — teclado e controle
         * ocultos no wlmaker paravam o ponteiro do celular */
        CHECK(bk_tm_reads("mouse", false, false, true), "modo mouse: lê o celular com tudo oculto");
        CHECK(!bk_tm_reads("mouse", false, false, false),
              "modo mouse sem ponteiro: não agarra o celular com tudo oculto (os dedos iriam fora)");
        CHECK(bk_tm_reads("mouse", true, false, false), "modo mouse sem ponteiro: o teclado aberto ainda lê");
        CHECK(!bk_tm_reads("exclusive", false, false, true) && bk_tm_reads("exclusive", true, false, true),
              "exclusive: só com algo aberto (como antes)");
        CHECK(!bk_tm_reads("off", true, true, true), "off: nunca");
        CHECK(bk_tm_exclusive("mouse") && bk_tm_exclusive("exclusive") && !bk_tm_exclusive("shared"),
              "mouse agarra o dispositivo como o exclusive");
        CHECK(bk_tm_mode_for("mouse", "auto", true, false, false, true) == BKTM_DIRECT,
              "tudo oculto: toque direto na tela inteira");
        CHECK(bk_tm_mode_for("mouse", "touchpad", true, false, false, true) == BKTM_TOUCHPAD,
              "tudo oculto respeita Sempre touchpad");
        CHECK(bk_tm_mode_for("exclusive", "auto", true, false, false, true) == -1,
              "exclusive com tudo oculto: nada (o desktop recebe o toque)");
        CHECK(bk_tm_mode_for("mouse", "auto", true, false, true, true) == -1,
              "só o controle aberto: fora dos controles não clica");
        CHECK(bk_tm_mode_for("mouse", "auto", true, true, false, true) == BKTM_TOUCHPAD &&
                  bk_tm_mode_for("mouse", "auto", true, true, true, true) == BKTM_DIRECT,
              "com o teclado aberto, igual ao exclusive");
        CHECK(bk_tm_mode_for("mouse", "off", true, false, false, true) == -1 &&
                  bk_tm_mode_for("mouse", "auto", false, false, false, true) == -1 &&
                  bk_tm_mode_for("mouse", "auto", true, false, false, false) == -1,
              "mouse desligado, sem exclusividade ou sem ponteiro: nada");
    }
    if (!fails)
        printf("bk-touchmouse: todos os casos passaram\n");
    return fails ? 1 : 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Mouse pelo toque: os dedos que não caem no teclado nem no controle na
 * tela viram ponteiro, cliques e rolagem. Puro (sem GTK nem dispositivo),
 * testado em tests/test-touchmouse.c; quem chama entrega os dedos em
 * pixels do monitor e o relógio, e recebe o que fazer pelo BkTmSink.
 *
 * Dois modos:
 *
 *   touchpad  (só o teclado aberto) como o touchpad de um notebook:
 *             arrastar um dedo move o ponteiro de onde ele está; um toque
 *             rápido é o clique esquerdo, onde o ponteiro estiver; segurar
 *             o dedo parado meio segundo e então movê-lo arrasta com o
 *             esquerdo apertado, até soltar.
 *   direct    (teclado e controle na tela juntos) como uma tela de toque:
 *             o ponteiro vai até o dedo; tirar o dedo sem arrastar é o
 *             clique esquerdo ali; arrastar aperta o esquerdo no ponto do
 *             toque e arrasta.
 *
 * Com o teclado e o controle ocultos, só no modo de toque remoto "mouse"
 * (o desktop ignora o touchscreen do Sunshine, como o wlmaker), todo dedo
 * vira o mouse em toque direto: sem isso o ponteiro pararia.
 *
 * Nos dois: com um dedo na tela, um toque rápido de um segundo dedo é o
 * clique direito (onde o ponteiro está), e o primeiro dedo, ao sair, não
 * clica mais; dois dedos que se movem juntos rolam a página.
 */
#ifndef BK_TOUCHMOUSE_H
#define BK_TOUCHMOUSE_H

#include "bk-controls.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum { BKTM_TOUCHPAD, BKTM_DIRECT } BkTmMode;

/* Botões como no evdev: BTN_LEFT (0x110) e BTN_RIGHT (0x111). */
typedef struct {
    void (*move_rel)(void *d, double dx, double dy);
    void (*move_abs)(void *d, double x, double y); /* pixels do monitor */
    void (*button)(void *d, int button, bool down);
    void (*scroll)(void *d, double dx, double dy); /* pixels; positivo: para baixo/direita */
    void *data;
} BkTmSink;

#define BKTM_FINGERS 4
#define BKTM_TAP_MS 280   /* toque rápido (touchpad) */
#define BKTM_HOLD_MS 500  /* parado este tempo, mover arrasta (touchpad) */

typedef struct {
    const void *id;
    double x, y, x0, y0;
    uint64_t t0;
    bool moved;
} BkTmFinger;

typedef struct {
    BkTmMode mode;
    double speed; /* touchpad: pixels do ponteiro por pixel do dedo */
    double slop;  /* pixels: abaixo disto o dedo "não se moveu" */
    BkTmFinger f[BKTM_FINGERS];
    int n;              /* dedos na tela */
    bool left_down;     /* o esquerdo apertado (arrasto) */
    bool right_done;    /* houve clique direito: o primeiro dedo não clica ao sair */
    bool scrolling;     /* dois dedos rolando */
    bool consumed;      /* o gesto já fez algo além de mover (não vira toque rápido) */
} BkTouchMouse;

void bk_tm_init(BkTouchMouse *m, BkTmMode mode, double speed, double slop);
/* Um dedo; now em milissegundos (monotônico). */
void bk_tm_touch(BkTouchMouse *m, const void *id, BkCtlPhase phase, double x, double y, uint64_t now,
                 const BkTmSink *sink);
/* Encerra tudo (dispositivo sumiu, modo mudou): solta o botão apertado. */
void bk_tm_cancel(BkTouchMouse *m, const BkTmSink *sink);

/* ---- quando o mouse age (toque remoto) ---------------------------------
 * remote_touch: off | exclusive | shared | mouse. "mouse" é o exclusive que
 * continua lendo o celular com o teclado e o controle ocultos. */

/* Ler o touchscreen do celular agora? Com tudo oculto, só no modo "mouse" e
 * com um ponteiro para mover: sem ele, agarrar o dispositivo só jogaria os
 * dedos fora. */
bool bk_tm_reads(const char *remote_touch, bool keyboard, bool controller, bool pointer);
/* Agarrar o dispositivo (nenhum dedo chega ao compositor)? */
bool bk_tm_exclusive(const char *remote_touch);
/* O modo do mouse para um dedo fora do teclado e do controle, ou -1
 * (nada): só com o dispositivo agarrado e um ponteiro para mover. */
int bk_tm_mode_for(const char *remote_touch, const char *remote_mouse, bool grabbed, bool keyboard,
                   bool controller, bool pointer);

#endif

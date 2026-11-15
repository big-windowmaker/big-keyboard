/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * O dockapp sem X11: geometria dos dois botões, quem está sob o ponteiro,
 * a distinção entre clique e duplo clique e o desenho (cairo). Testado em
 * tests/test-dock.c; a cola com o X11 fica em bk-dockapp.c.
 *
 *    ┌─────────┬┬─────────┐
 *    │ ┌─────┐ ││   ▲     │   esquerda: tecla Enter ↵ (teclado virtual)
 *    │ │   ↵ │ ││ ◀ ● ▶   │   direita: direcional (controle na tela)
 *    │ └─────┘ ││   ▼     │   o botão da janela aberta fica afundado
 *    │  ▬▬▬▬   ││  ▬▬▬▬   │   LED: verde aberto, cinza fechado
 *    └─────────┴┴─────────┘
 */
#ifndef BK_DOCK_H
#define BK_DOCK_H

#include <cairo.h>
#include <glib.h>

typedef enum { BK_HALF_NONE = -1, BK_HALF_KBD = 0, BK_HALF_PAD = 1 } BkHalf;

typedef struct {
    double x, y, w, h;
} BkRect;

typedef struct {
    int w, h;      /* tamanho real da janela do dockapp */
    double border; /* moldura externa (1 px em 64×64) */
    BkRect btn[2]; /* BK_HALF_KBD, BK_HALF_PAD */
} BkDockLayout;

void bk_dock_layout(BkDockLayout *l, int w, int h);
/* A metade sob (x, y); a moldura e o sulco entre os botões não são de
 * nenhuma (um clique ali não aciona o botão errado). */
BkHalf bk_dock_hit(const BkDockLayout *l, double x, double y);

/* ---- clique e duplo clique ------------------------------------------------ */

typedef enum {
    BK_CLICK_NONE,
    BK_CLICK_TOGGLE_KBD, /* clique simples na tecla Enter */
    BK_CLICK_TOGGLE_PAD, /* clique simples no direcional */
    BK_CLICK_SETTINGS,   /* duplo clique numa das metades */
} BkClick;

/* Um clique só vale depois do intervalo de duplo clique (contado de um
 * aperto ao outro); antes disso, um segundo aperto na mesma metade, perto,
 * faz dele um duplo clique: nenhuma alternância, só as configurações. Um
 * aperto na outra metade dispara na hora o clique pendente (são ações
 * independentes). Os tempos (ms) vêm de quem chama: nada de relógio aqui. */
typedef struct {
    int dbl_ms, dbl_dist;
    gboolean pending; /* primeiro clique esperando o intervalo */
    BkHalf pend_half;
    gint64 pend_time;
    int pend_x, pend_y;
    gboolean down; /* botão 1 apertado agora */
    BkHalf down_half;
    gint64 down_time;
    int down_x, down_y;
    gboolean second; /* este aperto é o segundo de um duplo clique */
} BkClicker;

void bk_clicker_init(BkClicker *c, int dbl_ms, int dbl_dist);
/* Aperto do botão 1. Pode devolver o clique pendente da outra metade. */
BkClick bk_clicker_press(BkClicker *c, BkHalf half, int x, int y, gint64 t);
/* Soltou o botão 1 sobre half (BK_HALF_NONE: fora dos botões). */
BkClick bk_clicker_release(BkClicker *c, BkHalf half, gint64 t);
/* O aperto virou arrasto (o Window Maker move o tile): não é clique. */
void bk_clicker_cancel(BkClicker *c);
/* Quando chamar bk_clicker_timeout; -1: nada pendente. */
gint64 bk_clicker_deadline(const BkClicker *c);
BkClick bk_clicker_timeout(BkClicker *c, gint64 now);
/* Dispara o clique pendente já (botão direito, saída). */
BkClick bk_clicker_flush(BkClicker *c);

/* ---- desenho -------------------------------------------------------------- */

typedef struct {
    gboolean dark;
    gboolean active[2];   /* a janela da metade está aberta */
    gboolean disabled[2]; /* indisponível (o controle sem /dev/uinput) */
    BkHalf hover, pressed;
    gboolean pad_connected; /* controle físico ligado: centro do direcional aceso */
} BkDockState;

void bk_dock_draw(cairo_t *cr, const BkDockLayout *l, const BkDockState *s);
/* O LED embaixo do botão i: verde com a janela aberta, cinza fechada. */
void bk_dock_led_rect(const BkDockLayout *l, int i, BkRect *led);

/* Intervalo de duplo clique do Window Maker (DoubleClickTime no domínio
 * WindowMaker do usuário, que o big-wm-wprefs-sync leva ao wlmaker); sem a
 * chave, 250 ms, o padrão do Window Maker. path NULL: o do usuário. */
int bk_dock_double_click_ms(const char *path);

#endif

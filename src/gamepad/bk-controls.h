/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Controle na tela (touchscreen): o layout dos controles e o que cada toque
 * faz, sem interface (testado em tests/test-controls.c).
 *
 * Posições e tamanhos são frações da tela (x, y do centro em 0..1; tamanho
 * em fração da altura), para o mesmo layout servir em qualquer resolução e
 * orientação. Cada dedo fica preso ao controle em que começou: escorregar
 * sobre outro controle não o aciona, e tirar o dedo (ou um cancelamento do
 * toque) solta o que ele segurava. As saídas vão para um BkCtlSink (o
 * controle virtual uinput, ou um registro nos testes).
 */
#ifndef BK_CONTROLS_H
#define BK_CONTROLS_H

#include <glib.h>
#include <stdbool.h>

typedef enum { BKC_STICK, BKC_DPAD, BKC_BUTTON, BKC_TRIGGER } BkCtlKind;

typedef struct {
    BkCtlKind kind;
    int code;          /* BKC_BUTTON: BTN_*; BKC_STICK/BKC_TRIGGER: lado 0/1 */
    double x, y, size; /* centro (fração da tela) e diâmetro (fração da altura) */
    double deadzone;   /* analógico/direcional: 0..0.8 */
    double sens;       /* analógico: 0.3..3 */
    bool enabled;
} BkCtl;

#define BKC_MAX 32
#define BKC_TOUCHES 16

typedef struct {
    char name[64];
    double opacity, scale;
    BkCtl c[BKC_MAX];
    int n;
} BkCtlLayout;

typedef struct {
    void (*button)(void *d, int code, bool down);
    void (*stick)(void *d, int side, double x, double y);
    void (*trigger)(void *d, int side, double v);
    void (*hat)(void *d, int x, int y);
    void (*sync)(void *d);
    void *data;
} BkCtlSink;

typedef struct {
    const void *seq[BKC_TOUCHES]; /* identidade do toque (GdkEventSequence) */
    int ctl[BKC_TOUCHES];         /* controle preso a cada toque; -1 livre */
    bool used[BKC_TOUCHES];
} BkCtlState;

typedef enum { BKC_BEGIN, BKC_MOVE, BKC_END, BKC_CANCEL } BkCtlPhase;

void bk_ctl_default(BkCtlLayout *l);
/* Diâmetro de um controle em pixels numa tela w×h. */
double bk_ctl_px(const BkCtlLayout *l, const BkCtl *c, double h);
/* O controle sob (px, py), ou -1. */
int bk_ctl_hit(const BkCtlLayout *l, double w, double h, double px, double py);
void bk_ctl_state_init(BkCtlState *s);
/* Um evento de toque; px/py em pixels da tela w×h. */
void bk_ctl_touch(BkCtlState *s, const BkCtlLayout *l, double w, double h, const void *seq, BkCtlPhase phase,
                  double px, double py, const BkCtlSink *sink);
/* Solta tudo o que os toques seguravam. */
void bk_ctl_release(BkCtlState *s, const BkCtlLayout *l, const BkCtlSink *sink);
/* Mantém o controle inteiro dentro da tela; TRUE se precisou mexer. */
bool bk_ctl_clamp(BkCtl *c, double w, double h, double scale);
const char *bk_ctl_label(const BkCtl *c);

/* O botão que troca o controle pelo teclado: fixo no alto, ao centro (entre
 * os ombros e acima de Select/Start no layout padrão), fora do layout para
 * nenhum layout salvo o perder. Centro e raio em pixels numa tela w×h. */
void bk_ctl_switch_button(double w, double h, double *cx, double *cy, double *r);
bool bk_ctl_switch_hit(double w, double h, double px, double py);

gboolean bk_ctl_save(const BkCtlLayout *l, const char *path, GError **err);
gboolean bk_ctl_load(BkCtlLayout *l, const char *path, GError **err);
/* ~/.local/share/big-keyboard/gamepad-layouts/<nome>.ini */
char *bk_ctl_path(const char *name);

#endif

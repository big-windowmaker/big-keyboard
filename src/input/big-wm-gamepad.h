/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * big-wm-gamepad: um controle físico (Xbox, DualShock/DualSense, Steam Deck,
 * genéricos USB/Bluetooth) para o teclado na tela da tela de bloqueio.
 *
 * Segurança: lido dentro do próprio processo da tela de bloqueio, sem
 * injeção de eventos nem outro processo. Só dispositivos que o udev marca
 * como joystick (ID_INPUT_JOYSTICK, os que o logind entrega ao usuário do
 * assento ativo pela tag uaccess) e que têm botões de gamepad; qualquer
 * dispositivo com teclas de letras é recusado, para nunca ler um teclado.
 * Nada é gravado nem registrado. Sem polling: descritores para o poll() ou
 * o laço do GLib, mais um prazo para a repetição de uma direção segurada.
 *
 * C puro (libudev e linux/input.h). Origem: big-wm/session/lockscreen (a tela
 * de bloqueio); esta cópia do Big WM Virtual Keyboard acrescenta o analógico
 * direito, os gatilhos, R3/Guide, o estado dos botões (combinações) e os
 * joysticks genéricos antigos (BTN_TRIGGER...), mapeados pela ordem.
 */
#ifndef BIG_WM_GAMEPAD_H
#define BIG_WM_GAMEPAD_H

enum bwg_action {
    BWG_NONE,
    BWG_UP,
    BWG_DOWN,
    BWG_LEFT,
    BWG_RIGHT,
    BWG_PRESS,     /* A / ✕: aperta a tecla do cursor */
    BWG_BACK,      /* B / ○: fecha o teclado */
    BWG_BACKSPACE, /* X / □: apaga */
    BWG_SPACE,     /* Y / △: espaço */
    BWG_SHIFT,     /* LB / L1 */
    BWG_LAYOUT,    /* RB / R1: ABNT2 / US */
    BWG_ENTER,     /* Start / Options / Menu: desbloquear */
    BWG_TOGGLE,    /* Select / View / Share: mostrar ou ocultar o teclado */
    BWG_CAPS,      /* clique do analógico esquerdo */
    BWG_LT,        /* gatilho esquerdo passou da metade */
    BWG_RT,        /* gatilho direito passou da metade */
    BWG_R3,        /* clique do analógico direito */
    BWG_GUIDE,     /* Xbox / PS / Steam */
    BWG_COMBO,     /* Select + Start juntos: abrir/fechar o teclado */
};

/* bits de bwg_pad.down */
#define BWG_DOWN_SELECT 1u
#define BWG_DOWN_START 2u
#define BWG_DOWN_LT 4u
#define BWG_DOWN_RT 8u

#define BWG_MAX_PADS 8
#define BWG_MAX_ACTIONS 32

typedef struct {
    int fd;
    int ax_min[2], ax_max[2]; /* ABS_X, ABS_Y */
    double sx, sy;            /* analógico esquerdo, -1..1 */
    double rx, ry;            /* analógico direito, -1..1 */
    double lt, rt;            /* gatilhos, 0..1 */
    int ax_rmin[2], ax_rmax[2], tr_min[2], tr_max[2];
    unsigned down;            /* BWG_DOWN_*: botões segurados */
    int combo;                /* Start apertado durante o Select */
    int stick;                /* direção do analógico (BWG_UP...), 0 centro */
    int hat_x, hat_y;         /* direcional (ABS_HAT0X/Y): -1, 0, 1 */
    int dpad;                 /* direção dos botões BTN_DPAD_*, 0 nenhuma */
    char node[64];
} bwg_pad;

typedef struct bwg {
    void *udev, *monitor; /* struct udev / udev_monitor (sem expor libudev) */
    bwg_pad pad[BWG_MAX_PADS];
    int npads;
    int held;               /* direção segurada (para a repetição), 0 nenhuma */
    long long next_repeat;  /* ms do relógio monotônico */
    unsigned generation;    /* muda quando um controle entra ou sai */
    double deadzone;        /* analógicos: abaixo disso é centro (0..1) */
    int no_stick_nav;       /* o analógico esquerdo não move o cursor (modo apontar) */
    int last;               /* índice do controle que mandou o último evento */
} bwg;

/* O controle usado por último (com vários ligados, o que a pessoa mexe), ou
 * NULL sem nenhum. */
const bwg_pad *bwg_active(const bwg *g);

/* Abre os controles presentes e o monitor do udev. Retorna o número de
 * controles (0 é normal: o monitor avisa quando um for ligado), -1 sem udev. */
int bwg_open(bwg *g);
void bwg_close(bwg *g);

/* Descritores a vigiar (monitor primeiro, depois os controles). */
int bwg_fds(const bwg *g, int *fds, int max);

/* Algo para ler em fd: preenche out com as ações e retorna quantas. */
int bwg_handle(bwg *g, int fd, long long now_ms, enum bwg_action *out, int max);

/* ms até a próxima repetição de uma direção segurada, -1 se nenhuma. */
int bwg_timeout(const bwg *g, long long now_ms);
/* A repetição vencida (no máximo uma ação). */
int bwg_tick(bwg *g, long long now_ms, enum bwg_action *out, int max);

/* Para testes: um descritor que fala o protocolo evdev (sem udev). */
int bwg_add_fd(bwg *g, int fd, int ax_min, int ax_max);

long long bwg_now_ms(void);

/* Navegação espacial: a partir do retângulo cur, o mais próximo na direção
 * dir (BWG_UP...). Sem candidato, dá a volta na mesma fileira ou coluna.
 * Retorna o índice, ou cur. */
typedef struct {
    double x, y, w, h;
} bwg_rect;
int bwg_spatial(const bwg_rect *r, int n, int cur, int dir);

#endif

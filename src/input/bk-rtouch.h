/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Toque remoto: os dedos de um celular ou tablet que joga pelo Moonlight.
 *
 * Quando o cliente Moonlight envia toques nativos (LiSendTouchEvent), o
 * Sunshine cria no Linux um touchscreen virtual por uinput, o
 * "libvirtualhid Touchscreen": multitoque protocolo B (ABS_MT_SLOT,
 * ABS_MT_TRACKING_ID, ABS_MT_POSITION_X/Y), com as posições normalizadas à
 * imagem transmitida (0..19200 × 0..10800 = o monitor compartilhado
 * inteiro, sem as barras pretas do celular).
 *
 * Este módulo lê esse dispositivo direto do kernel, sem depender do
 * compositor: o wlmaker, por exemplo, não entrega toques aos clientes, e
 * no X11/KDE o toque viraria clique de mouse fora dos controles. Cada
 * contato vira BEGIN/MOVE/END com uma identidade estável, e o controle na
 * tela (bk-controls) faz o resto.
 *
 * O decodificador (BkRtDecoder) é puro: recebe struct input_event e chama
 * a função de saída; testado em tests/test-rtouch.c. As funções de
 * dispositivo abrem, agarram (EVIOCGRAB, modo exclusivo) e procuram o
 * touchscreen pelo udev.
 *
 * Segurança: lê só dispositivos que o Sunshine cria (nome e propriedades
 * de touchscreen direto), que a regra udev do próprio Sunshine entrega ao
 * usuário do assento (uaccess). Nada roda como root; nenhuma posição de
 * toque vai para o log.
 */
#ifndef BK_RTOUCH_H
#define BK_RTOUCH_H

#include "bk-controls.h"

#include <glib.h>
#include <linux/input.h>
#include <stdbool.h>

#define BKRT_SLOTS 16

/* Uma mudança de contato: slot (0..BKRT_SLOTS-1, identidade estável enquanto
 * o dedo está na tela), fase e posição normalizada à imagem (0..1). */
typedef void (*BkRtFn)(void *data, int slot, BkCtlPhase phase, double nx, double ny);

typedef struct {
    int min_x, max_x, min_y, max_y;
    int slot;                   /* slot atual do protocolo B */
    int id[BKRT_SLOTS];         /* tracking id entregue (-1 livre) */
    int x[BKRT_SLOTS], y[BKRT_SLOTS];
    int nid[BKRT_SLOTS];        /* estado pendente até o SYN_REPORT */
    int nx[BKRT_SLOTS], ny[BKRT_SLOTS];
    bool dropped;               /* SYN_DROPPED: ignora até o próximo SYN_REPORT */
    bool need_resync;           /* o chamador relê o estado (bk_rt_resync) */
} BkRtDecoder;

void bk_rt_init(BkRtDecoder *d, int min_x, int max_x, int min_y, int max_y);
/* Um evento do kernel. As saídas saem no SYN_REPORT, nesta ordem: os dedos
 * que saíram (END), os que chegaram (BEGIN), os que se moveram (MOVE). */
void bk_rt_event(BkRtDecoder *d, const struct input_event *e, BkRtFn fn, void *data);
/* Depois de um SYN_DROPPED: o estado real de cada slot (tracking id, x, y)
 * vira o novo estado, com END/BEGIN/MOVE para o que mudou. */
void bk_rt_resync(BkRtDecoder *d, const int *ids, const int *xs, const int *ys, int n, BkRtFn fn, void *data);
/* Encerra todos os contatos (dispositivo sumiu, controle fechado). */
void bk_rt_cancel_all(BkRtDecoder *d, BkRtFn fn, void *data);
int bk_rt_active(const BkRtDecoder *d);

/* ---- dispositivo ------------------------------------------------------- */

/* O nome de um touchscreen de toque remoto do Sunshine. */
bool bk_rt_is_remote_name(const char *name);

typedef enum { BKRT_GRAB_NONE, BKRT_GRAB_OK, BKRT_GRAB_BUSY } BkRtGrab;

typedef struct {
    int fd;
    char node[64];
    BkRtGrab grab;
    BkRtDecoder dec;
} BkRtDevice;

/* Abre o nó, confere que é um touchscreen multitoque direto, lê os limites
 * dos eixos e, com exclusive, agarra o dispositivo (o compositor deixa de
 * recebê-lo; se outro leitor já o agarrou, grab = BUSY e a leitura segue). */
gboolean bk_rt_device_open(BkRtDevice *dev, const char *node, gboolean exclusive, GError **err);
/* Lê o que houver (descritor não bloqueante). FALSE: o dispositivo sumiu
 * (o chamador fecha; os contatos já foram encerrados). */
gboolean bk_rt_device_read(BkRtDevice *dev, BkRtFn fn, void *data);
void bk_rt_device_close(BkRtDevice *dev, BkRtFn fn, void *data);

/* Os touchscreens remotos presentes (nós /dev/input/eventN; NULL no fim). */
char **bk_rt_find_nodes(void);

/* Conexão a quente pelo udev (o touchscreen nasce quando o cliente conecta e
 * some quando a sessão acaba): um descritor para o laço do GLib, sem polling. */
typedef struct BkRtWatch BkRtWatch;
BkRtWatch *bk_rt_watch_new(void);
int bk_rt_watch_fd(BkRtWatch *w);
/* Esvazia a fila; TRUE se um nó eventN de entrada entrou ou saiu. */
gboolean bk_rt_watch_drain(BkRtWatch *w);
void bk_rt_watch_free(BkRtWatch *w);

#endif

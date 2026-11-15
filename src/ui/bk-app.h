/* SPDX-License-Identifier: GPL-2.0-or-later
 * Big WM Virtual Keyboard: o aplicativo e as peças que ele coordena. */
#ifndef BK_APP_H
#define BK_APP_H

#include "big-wm-gamepad.h"
#include "bk-config.h"
#include "bk-controls.h"
#include "bk-inject.h"
#include "bk-keys.h"
#include "bk-vpad.h"

#include <gtk/gtk.h>

#define BK_APP_ID "io.github.big_windowmaker.BigKeyboard"

typedef struct BkOsk BkOsk;
typedef struct BkOverlay BkOverlay;
typedef struct BkRemote BkRemote;

typedef struct {
    GtkApplication *gtk;
    BkConfig cfg;
    BkInject *inject;
    bwg pad;
    gboolean pad_open;
    GArray *pad_sources;
    guint pad_repeat_id;
    unsigned pad_generation;
    BkOsk *osk;
    BkOverlay *overlay;
    BkRemote *remote; /* toque do celular (Moonlight → Sunshine) */
    GtkWidget *settings;
    BkVpad *vpad; /* existe com o controle na tela, ou trocado pelo teclado */
    gboolean dark;
    struct BkFocus *focus; /* campo de texto em foco (AT-SPI), com a opção ligada */
    guint focus_hide_id;
} BkApp;

BkApp *bk_app_get(void);
/* O modo (para o dockapp): hidden, keyboard, gamepad, hybrid. */
void bk_app_update_state(BkApp *app);
/* Solta tudo: teclas, modificadores e o controle virtual. */
void bk_app_release_all(BkApp *app);
/* Releu as preferências (janela de configurações). */
void bk_app_config_changed(BkApp *app);
gboolean bk_app_dark(BkApp *app);
/* Troca o controle na tela pelo teclado (FALSE) ou o teclado pelo controle
 * (TRUE). O controle virtual continua ligado no meio: o jogo não o vê sair. */
void bk_app_switch(BkApp *app, gboolean to_gamepad);

/* ---- teclado ------------------------------------------------------------- */
BkOsk *bk_osk_new(BkApp *app);
void bk_osk_set_visible(BkOsk *osk, gboolean visible);
gboolean bk_osk_visible(BkOsk *osk);
void bk_osk_pad_action(BkOsk *osk, enum bwg_action a);
void bk_osk_pad_sticks(BkOsk *osk, const bwg_pad *p, double deadzone);
void bk_osk_release(BkOsk *osk);
void bk_osk_reload(BkOsk *osk);
/* O monitor da próxima abertura (a troca pelo controle: o teclado abre onde
 * o controle estava, o monitor que o celular alcança); NULL = o de sempre. */
void bk_osk_set_monitor(BkOsk *osk, GdkMonitor *monitor);
/* Toque remoto: onde está a janela, em pixels do monitor (FALSE: fechado
 * ou fora dele); um dedo, em pixels do monitor. */
gboolean bk_osk_remote_rect(BkOsk *osk, GdkMonitor *monitor, GdkRectangle *r);
void bk_osk_remote_touch(BkOsk *osk, GdkMonitor *monitor, const void *id, BkCtlPhase phase, double x, double y);
/* Um celular conectou: o teclado aberto vai para o monitor transmitido. */
void bk_osk_follow_remote(BkOsk *osk);
/* Aberto sozinho por um campo de texto (AT-SPI): fecha sozinho também. */
void bk_osk_set_auto_shown(BkOsk *osk, gboolean auto_shown);
gboolean bk_osk_auto_shown(BkOsk *osk);

/* ---- controle na tela (toque) --------------------------------------------- */
BkOverlay *bk_overlay_new(BkApp *app);
void bk_overlay_set_visible(BkOverlay *ov, gboolean visible, gboolean edit);
/* Oculta sem desligar o controle virtual (troca pelo teclado). */
void bk_overlay_hide_keep_pad(BkOverlay *ov);
/* Onde o controle está (NULL: o compositor escolheu). */
GdkMonitor *bk_overlay_monitor(BkOverlay *ov);
gboolean bk_overlay_visible(BkOverlay *ov);
void bk_overlay_release(BkOverlay *ov);
/* Toque remoto: off, idle (controle fechado), waiting (sem touchscreen do
 * Sunshine), active (exclusivo), shared, busy (outro leitor agarrou),
 * denied (sem permissão), error. */
/* Toque remoto, em coordenadas normalizadas ao monitor do controle: o
 * controle pega o dedo? (no botão do teclado, no editor, ou num controle
 * que não esteja sob a janela do teclado) */
gboolean bk_overlay_remote_claim(BkOverlay *ov, double nx, double ny, gboolean under_keyboard);
void bk_overlay_remote_touch(BkOverlay *ov, const void *id, BkCtlPhase phase, double nx, double ny);
void bk_overlay_output_changed(BkOverlay *ov);
GdkMonitor *bk_monitor_by_name(GdkDisplay *dpy, const char *name);

/* ---- desenho do controle na tela, nos estilos (bk-ctlpaint.c) -------------- */
/* Um controle; now e pressed_at em µs (g_get_monotonic_time; 0: sem animação). */
void bk_ctl_paint(cairo_t *cr, const BkCtlLayout *l, const BkCtlState *s, double w, double h, int i, gint64 now,
                  gint64 pressed_at);
/* A animação de toque ainda está em curso (pede outro quadro)? */
gboolean bk_ctl_paint_animating(const BkCtlLayout *l, gint64 now, gint64 pressed_at);
void bk_ctl_paint_switch(cairo_t *cr, const BkCtlLayout *l, double w, double h, gboolean down);
/* O layout inteiro, parado (prévia das configurações). */
void bk_ctl_paint_layout(cairo_t *cr, const BkCtlLayout *l, const BkCtlState *s, double w, double h);
/* O controle na tela releu o layout do disco (configurações mudaram a aparência). */
void bk_overlay_reload(BkOverlay *ov);

/* ---- toque remoto (bk-remote.c) ------------------------------------------- */
BkRemote *bk_remote_new(BkApp *app);
/* Abre ou fecha a leitura conforme o teclado e o controle (no laço principal). */
void bk_remote_sync(BkRemote *r);
/* O modo mudou (off, exclusive, shared): fecha e reabre. */
void bk_remote_restart(BkRemote *r);
/* off, idle, waiting, active, shared, busy, denied, error. */
const char *bk_remote_state(BkRemote *r);
gboolean bk_remote_present(BkRemote *r);
/* Lendo sem exclusividade: o compositor entrega os mesmos dedos às janelas. */
gboolean bk_remote_shadows_local(BkRemote *r);
/* O monitor transmitido (overlay-output; senão o principal). */
GdkMonitor *bk_remote_monitor(BkRemote *r);
/* Soltar tudo: os dedos ainda na tela só voltam a valer se se moverem. */
void bk_remote_release(BkRemote *r);

/* ---- configurações ------------------------------------------------------- */
void bk_settings_show(BkApp *app, const char *page);
void bk_settings_pad_event(BkApp *app);

/* Desenho OPENSTEP compartilhado. */
void bk_draw_bevel(cairo_t *cr, double x, double y, double w, double h, gboolean raised, gboolean dark);
void bk_draw_titlebar(cairo_t *cr, double x, double y, double w, double h, const char *title, double fs);
/* Ícones vetoriais (cabem num quadrado de lado s, centrado em cx, cy). */
void bk_draw_icon_gamepad(cairo_t *cr, double cx, double cy, double s, double r, double g, double b, double a);
void bk_draw_icon_keyboard(cairo_t *cr, double cx, double cy, double s, double r, double g, double b, double a);

#endif

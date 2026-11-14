/* SPDX-License-Identifier: GPL-2.0-or-later
 * Big WM Virtual Keyboard: o aplicativo e as peças que ele coordena. */
#ifndef BK_APP_H
#define BK_APP_H

#include "big-wm-gamepad.h"
#include "bk-config.h"
#include "bk-inject.h"
#include "bk-keys.h"
#include "bk-vpad.h"

#include <gtk/gtk.h>

#define BK_APP_ID "io.github.big_windowmaker.BigKeyboard"

typedef struct BkOsk BkOsk;
typedef struct BkOverlay BkOverlay;

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
    GtkWidget *settings;
    BkVpad *vpad; /* existe só com o controle virtual em uso */
    gboolean dark;
} BkApp;

BkApp *bk_app_get(void);
/* O modo (para o dockapp): hidden, keyboard, gamepad, hybrid. */
void bk_app_update_state(BkApp *app);
/* Solta tudo: teclas, modificadores e o controle virtual. */
void bk_app_release_all(BkApp *app);
/* Releu as preferências (janela de configurações). */
void bk_app_config_changed(BkApp *app);
gboolean bk_app_dark(BkApp *app);

/* ---- teclado ------------------------------------------------------------- */
BkOsk *bk_osk_new(BkApp *app);
void bk_osk_set_visible(BkOsk *osk, gboolean visible);
gboolean bk_osk_visible(BkOsk *osk);
void bk_osk_pad_action(BkOsk *osk, enum bwg_action a);
void bk_osk_pad_sticks(BkOsk *osk, const bwg_pad *p, double deadzone);
void bk_osk_release(BkOsk *osk);
void bk_osk_reload(BkOsk *osk);

/* ---- controle na tela (toque) --------------------------------------------- */
BkOverlay *bk_overlay_new(BkApp *app);
void bk_overlay_set_visible(BkOverlay *ov, gboolean visible, gboolean edit);
gboolean bk_overlay_visible(BkOverlay *ov);
void bk_overlay_release(BkOverlay *ov);
/* Toque remoto: off, idle (controle fechado), waiting (sem touchscreen do
 * Sunshine), active (exclusivo), shared, busy (outro leitor agarrou),
 * denied (sem permissão), error. */
const char *bk_overlay_remote_state(BkOverlay *ov);
void bk_overlay_remote_changed(BkOverlay *ov);
void bk_overlay_output_changed(BkOverlay *ov);
GdkMonitor *bk_monitor_by_name(GdkDisplay *dpy, const char *name);

/* ---- configurações ------------------------------------------------------- */
void bk_settings_show(BkApp *app, const char *page);
void bk_settings_pad_event(BkApp *app);

/* Desenho OPENSTEP compartilhado. */
void bk_draw_bevel(cairo_t *cr, double x, double y, double w, double h, gboolean raised, gboolean dark);
void bk_draw_titlebar(cairo_t *cr, double x, double y, double w, double h, const char *title, double fs);

#endif

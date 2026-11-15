/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Entrega das teclas do teclado virtual ao aplicativo em foco.
 *
 *   Wayland  zwp_virtual_keyboard_v1 (o wlmaker o oferece): um keymap
 *            próprio, gerado sob demanda com os keysyms usados, e os
 *            modificadores pelo pedido modifiers. Qualquer caractere
 *            Unicode, sem depender do layout da sessão.
 *   X11      XTest: a tecla do layout da sessão quando o caractere está
 *            nele (com Shift se preciso); senão um keycode livre, remapeado
 *            para o keysym (devolvido ao sair).
 *
 * Ponteiro (mouse pelo toque do celular):
 *
 *   Wayland  zwlr_virtual_pointer_v1, criado preso ao monitor transmitido
 *            (o movimento absoluto vale dentro dele); o wlmaker o oferece.
 *   X11      XTest (movimento, botões 1/3, rolagem pelos botões 4 a 7).
 *
 * Só para a sessão comum: a tela de bloqueio tem o teclado dela, dentro do
 * processo do bloqueador (big-wm-lockscreen), e nunca passa por aqui.
 */
#ifndef BK_INJECT_H
#define BK_INJECT_H

#include "bk-keys.h"

#include <gdk/gdk.h>

typedef struct BkInject BkInject;

BkInject *bk_inject_new(GdkDisplay *display);
void bk_inject_free(BkInject *in);
/* "wayland", "x11" ou NULL (sem backend: o teclado não digita). */
const char *bk_inject_backend(const BkInject *in);
void bk_inject_out(BkInject *in, const BkOut *out);

/* ---- ponteiro ---- */
gboolean bk_inject_has_pointer(const BkInject *in);
/* Movimento relativo, em pixels. */
void bk_inject_pointer_rel(BkInject *in, double dx, double dy);
/* Para (x, y), em pixels lógicos do monitor (NULL: o principal). */
void bk_inject_pointer_abs(BkInject *in, GdkMonitor *monitor, double x, double y);
/* Botão do evdev (BTN_LEFT, BTN_RIGHT). */
void bk_inject_pointer_button(BkInject *in, int button, gboolean down);
/* Rolagem em pixels; positivo: para baixo / para a direita. */
void bk_inject_pointer_scroll(BkInject *in, double dx, double dy);

#endif

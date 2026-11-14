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

#endif

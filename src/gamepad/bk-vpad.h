/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Controle virtual (uinput): o que os jogos veem quando o Big WM Virtual
 * Keyboard joga por você (toque na tela, teclado e mouse). Layout de botões
 * da API de gamepad do kernel (BTN_SOUTH..., ABS_X/Y/RX/RY, gatilhos
 * ABS_Z/RZ 0..1023, direcional ABS_HAT0X/Y), que o SDL, o Steam/Proton e os
 * navegadores (Gamepad API) entendem.
 *
 * Identidade: "generic" (vendor 0x1209, IDs de teste do pid.codes) por
 * padrão; "xbox360" usa os IDs 045e:028e só para jogos que não aceitam um
 * controle desconhecido (opcional, desligado de saída).
 *
 * Segurança: roda como o usuário. O /dev/uinput é do usuário do assento
 * ativo pela tag uaccess (regras do steam-devices / game-devices-udev); sem
 * acesso, bk_vpad_new falha com uma mensagem clara, sem pedir root. Nada
 * fica preso: bk_vpad_neutral solta tudo e o kernel apaga o dispositivo
 * quando o descritor fecha (inclusive numa queda).
 */
#ifndef BK_VPAD_H
#define BK_VPAD_H

#include <glib.h>
#include <stdbool.h>

typedef struct BkVpad BkVpad;

/* Botões: os códigos BTN_* de linux/input-event-codes.h. */
BkVpad *bk_vpad_new(const char *identity, GError **err);
void bk_vpad_free(BkVpad *v);
void bk_vpad_button(BkVpad *v, int code, bool down);
/* side 0 esquerdo, 1 direito; x, y em -1..1 */
void bk_vpad_stick(BkVpad *v, int side, double x, double y);
/* side 0 LT, 1 RT; 0..1 */
void bk_vpad_trigger(BkVpad *v, int side, double value);
/* direcional: -1, 0, 1 */
void bk_vpad_hat(BkVpad *v, int x, int y);
/* Fim de um quadro de eventos (SYN_REPORT). */
void bk_vpad_sync(BkVpad *v);
/* Solta todos os botões e centra eixos (troca de modo, emergência). */
void bk_vpad_neutral(BkVpad *v);
/* Nome do nó /dev/input/eventN (para diagnóstico), ou NULL. */
const char *bk_vpad_node(BkVpad *v);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Campo de texto em foco, pela acessibilidade (AT-SPI): o teclado abre
 * sozinho quando um campo editável ganha o foco, como no celular, e pode
 * fechar quando o foco sai dele. Vale no X11 e no Wayland (é D-Bus), para
 * GTK, Qt, navegadores e LibreOffice que expõem a acessibilidade.
 */
#ifndef BK_FOCUS_H
#define BK_FOCUS_H

#include <glib.h>

typedef struct BkFocus BkFocus;
/* editable: o foco foi para um campo de texto (TRUE) ou saiu dele (FALSE). */
typedef void (*BkFocusFunc)(gboolean editable, gpointer data);

/* NULL se o barramento de acessibilidade não está disponível. Pede aos
 * aplicativos que exponham a acessibilidade (org.a11y.Status.IsEnabled):
 * navegadores e Qt só a ligam com isso. */
BkFocus *bk_focus_new(BkFocusFunc func, gpointer data);
void bk_focus_free(BkFocus *f);

#endif

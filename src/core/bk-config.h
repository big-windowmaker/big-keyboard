/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Preferências do Big WM Virtual Keyboard: ~/.config/big-keyboard/
 * big-keyboard.conf (GKeyFile, [General] Version=1). Cada valor é validado e
 * limitado; um arquivo ilegível é guardado como .invalid e os padrões valem;
 * antes de migrar de versão, uma cópia .bak. Gravação atômica (arquivo
 * temporário + rename, g_file_set_contents). Nenhuma senha nem token.
 */
#ifndef BK_CONFIG_H
#define BK_CONFIG_H

#include <glib.h>

#define BK_CONFIG_VERSION 1

typedef struct {
    /* [Keyboard] */
    char layout[16];      /* auto, br, us, us-intl */
    char theme[8];        /* auto (o modo do Big WindowMaker), light, dark */
    double scale;         /* 0.6..2.0 */
    double opacity;       /* 0.4..1.0 */
    gboolean reserve;     /* reservar o espaço (janelas não ficam por baixo) */
    gboolean autohide;    /* fechar depois de Enter */
    int repeat_delay;     /* ms, 150..2000 */
    int repeat_rate;      /* ms entre repetições, 20..500 */
    gboolean sound;       /* clique sonoro (desligado) */
    /* [Gamepad] */
    gboolean pad;         /* navegar com o controle físico */
    gboolean pad_combo;   /* Select+Start abre/fecha o teclado de qualquer lugar */
    char pad_mode[8];     /* cursor, point, both */
    double deadzone;      /* 0.05..0.6 */
    /* [VirtualGamepad] */
    char identity[16];    /* generic, xbox360 (IDs de compatibilidade) */
    double overlay_opacity; /* 0.15..1.0 */
    double overlay_scale;   /* 0.5..2.0 */
    char overlay_layout[64];
    /* toque remoto (Moonlight → Sunshine): off, exclusive (só o controle
     * recebe os toques), shared (fora dos controles o toque segue para o
     * desktop, onde o compositor aceita toque) */
    char remote_touch[12];
    char overlay_output[32]; /* conector do monitor (DP-1); vazio: o principal */
    /* [Mouse] (teclado e mouse como controle) */
    double mouse_sens_x, mouse_sens_y; /* 0.1..10 */
    gboolean invert_y;
    double mouse_curve;   /* expoente da curva, 0.5..3 */
    /* [XCloud] */
    char xcloud_backend[16]; /* better-xcloud, linux */
    char browser[64];
} BkConfig;

void bk_config_defaults(BkConfig *c);
/* Carrega (padrões onde faltar). path NULL: o arquivo do usuário. */
void bk_config_load(BkConfig *c, const char *path);
gboolean bk_config_save(const BkConfig *c, const char *path, GError **err);
char *bk_config_path(void);
/* Nome de conector aceitável (letras, números, - _ .), ou vazio. */
gboolean bk_config_valid_output(const char *name);

#endif

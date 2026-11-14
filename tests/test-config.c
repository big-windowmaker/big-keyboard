/* SPDX-License-Identifier: GPL-2.0-or-later
 * Preferências: padrões, validação, gravação, arquivo inválido, migração. */
#include "bk-config.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

int
main(void)
{
    char *dir = g_dir_make_tmp("bk-config-XXXXXX", NULL);
    char *f = g_build_filename(dir, "sub", "big-keyboard.conf", NULL);
    BkConfig c;
    bk_config_load(&c, f); /* não existe: padrões */
    CHECK(!strcmp(c.layout, "auto") && c.pad && c.scale == 1.0, "padrões");
    /* valores fora da faixa e strings não aceitas */
    char *sub = g_path_get_dirname(f);
    g_mkdir_with_parents(sub, 0700);
    g_file_set_contents(f, "[General]\nVersion=1\n[Keyboard]\nLayout=klingon\nScale=99\nOpacity=0.1\n"
                           "RepeatDelay=5\n[Gamepad]\nNavigation=point\nDeadzone=-3\n", -1, NULL);
    bk_config_load(&c, f);
    CHECK(!strcmp(c.layout, "auto"), "layout inválido vira o padrão: %s", c.layout);
    CHECK(c.scale == 2.0 && c.opacity == 0.4 && c.repeat_delay == 150, "limites: %.1f %.1f %d", c.scale, c.opacity, c.repeat_delay);
    CHECK(!strcmp(c.pad_mode, "point") && c.deadzone == 0.05, "gamepad: %s %.2f", c.pad_mode, c.deadzone);
    /* gravação e leitura de volta */
    g_strlcpy(c.layout, "us-intl", sizeof c.layout);
    c.reserve = FALSE;
    CHECK(bk_config_save(&c, f, NULL), "gravar");
    BkConfig d;
    bk_config_load(&d, f);
    CHECK(!strcmp(d.layout, "us-intl") && !d.reserve && d.scale == 2.0, "ida e volta");
    /* migração: versão antiga deixa uma cópia .bak */
    g_file_set_contents(f, "[Keyboard]\nLayout=br\n", -1, NULL);
    bk_config_load(&d, f);
    char *bak = g_strdup_printf("%s.bak", f);
    CHECK(g_file_test(bak, G_FILE_TEST_EXISTS) && !strcmp(d.layout, "br"), "migração com .bak");
    /* arquivo ilegível: guardado como .invalid, padrões */
    g_file_set_contents(f, "isto não é um keyfile\x01[[[", -1, NULL);
    bk_config_load(&d, f);
    char *inv = g_strdup_printf("%s.invalid", f);
    CHECK(g_file_test(inv, G_FILE_TEST_EXISTS) && !strcmp(d.layout, "auto"), "inválido guardado");
    g_remove(inv);
    g_remove(bak);
    g_remove(f);
    g_rmdir(sub);
    g_rmdir(dir);
    if (!fails)
        printf("bk-config: todos os casos passaram\n");
    return fails ? 1 : 0;
}

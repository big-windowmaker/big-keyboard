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
    CHECK(!strcmp(c.display_mode, "independent"), "modo de exibição: independente por padrão");
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
    g_file_set_contents(f, "[General]\nVersion=1\nDisplayMode=exclusive\n", -1, NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.display_mode, "exclusive"), "modo exclusivo lido");
    g_file_set_contents(f, "[General]\nVersion=1\nDisplayMode=caos\n", -1, NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.display_mode, "independent"), "modo inválido vira o padrão");
    /* janela do teclado: padrões, leitura, limites e valores recusados */
    bk_config_defaults(&d);
    CHECK(!strcmp(d.window_mode, "floating") && !strcmp(d.stacking, "above") && !d.auto_show && d.auto_hide &&
              d.float_x == -1 && d.float_width == 0,
          "janela: padrões");
    g_file_set_contents(f, "[General]\nVersion=1\n[Keyboard]\nWindowMode=docked\nStacking=fullscreen\n"
                           "ShowOnTextFocus=true\nHideOnFocusLoss=false\nFloatX=120\nFloatY=-50\nFloatWidth=900\n",
                        -1, NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.window_mode, "docked") && !strcmp(d.stacking, "fullscreen") && d.auto_show && !d.auto_hide &&
              d.float_x == 120 && d.float_y == -1 && d.float_width == 900,
          "janela: lida e limitada (%s %s %d %d %d)", d.window_mode, d.stacking, d.float_x, d.float_y,
          d.float_width);
    CHECK(bk_config_save(&d, f, NULL), "janela: gravar");
    BkConfig e;
    bk_config_load(&e, f);
    CHECK(!strcmp(e.window_mode, "docked") && e.float_width == 900 && e.auto_show, "janela: ida e volta");
    g_file_set_contents(f, "[General]\nVersion=1\n[Keyboard]\nWindowMode=solto\nStacking=sempre\n", -1, NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.window_mode, "floating") && !strcmp(d.stacking, "above"), "janela: valores inválidos recusados");
    /* migração: versão antiga deixa uma cópia .bak */
    g_file_set_contents(f, "[Keyboard]\nLayout=br\n", -1, NULL);
    bk_config_load(&d, f);
    char *bak = g_strdup_printf("%s.bak", f);
    CHECK(g_file_test(bak, G_FILE_TEST_EXISTS) && !strcmp(d.layout, "br"), "migração com .bak");
    /* toque remoto e monitor: valores aceitos, o resto vira o padrão */
    g_file_set_contents(f, "[General]\nVersion=1\n[VirtualGamepad]\nRemoteTouch=shared\nOutput=DP-1\n", -1, NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.remote_touch, "shared") && !strcmp(d.overlay_output, "DP-1"), "toque remoto e monitor lidos");
    g_file_set_contents(f, "[General]\nVersion=1\n[VirtualGamepad]\nRemoteTouch=mouse\n", -1, NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.remote_touch, "mouse"), "toque remoto mouse lido");
    g_file_set_contents(f, "[General]\nVersion=1\n[VirtualGamepad]\nRemoteTouch=root\nOutput=../etc;rm\n", -1, NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.remote_touch, "exclusive") && !d.overlay_output[0], "valores estranhos recusados");
    /* mouse pelo toque: modo e velocidade validados */
    g_file_set_contents(f, "[General]\nVersion=1\n[VirtualGamepad]\nRemoteMouse=direct\nRemoteMouseSpeed=2.5\n", -1,
                        NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.remote_mouse, "direct") && d.remote_mouse_speed == 2.5, "mouse pelo toque lido");
    g_file_set_contents(f, "[General]\nVersion=1\n[VirtualGamepad]\nRemoteMouse=sempre\nRemoteMouseSpeed=99\n", -1,
                        NULL);
    bk_config_load(&d, f);
    CHECK(!strcmp(d.remote_mouse, "auto") && d.remote_mouse_speed == 4.0, "mouse: valor estranho vira o padrão, "
          "velocidade limitada: %s %.1f", d.remote_mouse, d.remote_mouse_speed);
    CHECK(bk_config_valid_output("HDMI-A-1") && bk_config_valid_output("") && !bk_config_valid_output("a b"),
          "nomes de conector");
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

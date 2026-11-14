/* SPDX-License-Identifier: GPL-2.0-or-later
 * Controle virtual: o sistema o vê como joystick, botões e eixos chegam,
 * a neutralização solta tudo. Pulado (77) sem acesso ao /dev/uinput. */
#include "bk-vpad.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

/* Lê eventos até o SYN; devolve o último valor do código pedido. */
static int
last_value(int fd, int type, int code, int *found)
{
    struct input_event e;
    int v = 0;
    *found = 0;
    for (int tries = 0; tries < 200; tries++) {
        ssize_t r = read(fd, &e, sizeof e);
        if (r != sizeof e) {
            usleep(5000);
            continue;
        }
        if (e.type == type && e.code == code) {
            v = e.value;
            *found = 1;
        }
        if (e.type == EV_SYN && *found)
            break;
    }
    return v;
}

int
main(void)
{
    if (access("/dev/uinput", W_OK) != 0) {
        printf("bk-vpad: sem /dev/uinput, pulado\n");
        return 77;
    }
    BkVpad *v = bk_vpad_new("generic", NULL);
    CHECK(v && bk_vpad_node(v), "criar o controle");
    if (!v)
        return 1;
    usleep(300000);
    int fd = open(bk_vpad_node(v), O_RDONLY | O_NONBLOCK);
    CHECK(fd >= 0, "abrir %s", bk_vpad_node(v));
    char name[128] = "";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    CHECK(!strcmp(name, "Big WM Virtual Gamepad"), "nome: %s", name);
    unsigned long keys[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(keys, 0, sizeof keys);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
#define HAS(b) ((keys[(b) / (8 * sizeof(unsigned long))] >> ((b) % (8 * sizeof(unsigned long)))) & 1)
    CHECK(HAS(BTN_SOUTH) && HAS(BTN_START) && HAS(BTN_THUMBR) && !HAS(KEY_A), "botões de gamepad, nenhuma tecla");
    int found;
    bk_vpad_button(v, BTN_SOUTH, true);
    bk_vpad_sync(v);
    CHECK(last_value(fd, EV_KEY, BTN_SOUTH, &found) == 1 && found, "A apertado");
    bk_vpad_stick(v, 0, 1.0, -1.0);
    bk_vpad_sync(v);
    CHECK(last_value(fd, EV_ABS, ABS_X, &found) == 32767 && found, "analógico à direita");
    bk_vpad_trigger(v, 1, 0.5);
    bk_vpad_sync(v);
    int rt = last_value(fd, EV_ABS, ABS_RZ, &found);
    CHECK(found && rt >= 510 && rt <= 513, "RT pela metade: %d", rt);
    /* neutro: o A solto e os eixos no centro */
    bk_vpad_neutral(v);
    struct input_absinfo ai;
    ioctl(fd, EVIOCGABS(ABS_X), &ai);
    CHECK(ai.value == 0, "eixo X no centro: %d", ai.value);
    memset(keys, 0, sizeof keys);
    ioctl(fd, EVIOCGKEY(sizeof keys), keys);
    CHECK(!HAS(BTN_SOUTH), "A solto depois do neutro");
    close(fd);
    char node[32];
    snprintf(node, sizeof node, "%s", bk_vpad_node(v));
    bk_vpad_free(v);
    usleep(200000);
    CHECK(access(node, F_OK) != 0, "dispositivo removido ao fechar");
    if (!fails)
        printf("bk-vpad: todos os casos passaram\n");
    return fails ? 1 : 0;
}

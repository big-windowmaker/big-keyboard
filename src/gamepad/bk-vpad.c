/* SPDX-License-Identifier: GPL-2.0-or-later
 * Controle virtual por uinput (veja bk-vpad.h). */
#define _GNU_SOURCE
#include "bk-vpad.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const int buttons[] = { BTN_SOUTH, BTN_EAST,   BTN_NORTH,  BTN_WEST,   BTN_TL,     BTN_TR,
                               BTN_SELECT, BTN_START, BTN_MODE,   BTN_THUMBL, BTN_THUMBR };
#define NBUTTONS ((int)(sizeof buttons / sizeof buttons[0]))

struct BkVpad {
    int fd;
    bool down[NBUTTONS];
    char node[32];
};

static void
emit(BkVpad *v, int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(v->fd, &e, sizeof e) != (ssize_t)sizeof e && errno != EAGAIN)
        g_debug("uinput: %s", g_strerror(errno));
}

static void
abs_setup(int fd, int code, int min, int max, int flat)
{
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof a);
    a.code = (unsigned short)code;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    a.absinfo.flat = flat;
    ioctl(fd, UI_SET_ABSBIT, code);
    ioctl(fd, UI_ABS_SETUP, &a);
}

BkVpad *
bk_vpad_new(const char *identity, GError **err)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Sem acesso ao /dev/uinput (%s). O controle virtual precisa da regra udev "
                    "do steam-devices ou game-devices-udev (tag uaccess).",
                    g_strerror(errno));
        return NULL;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int i = 0; i < NBUTTONS; i++)
        ioctl(fd, UI_SET_KEYBIT, buttons[i]);
    abs_setup(fd, ABS_X, -32768, 32767, 128);
    abs_setup(fd, ABS_Y, -32768, 32767, 128);
    abs_setup(fd, ABS_RX, -32768, 32767, 128);
    abs_setup(fd, ABS_RY, -32768, 32767, 128);
    abs_setup(fd, ABS_Z, 0, 1023, 0);
    abs_setup(fd, ABS_RZ, 0, 1023, 0);
    abs_setup(fd, ABS_HAT0X, -1, 1, 0);
    abs_setup(fd, ABS_HAT0Y, -1, 1, 0);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    if (identity && !strcmp(identity, "xbox360")) {
        us.id.vendor = 0x045e; /* só compatibilidade: jogos que exigem um controle conhecido */
        us.id.product = 0x028e;
        us.id.version = 0x0114;
        snprintf(us.name, sizeof us.name, "Big WM Virtual Gamepad (compatível com Xbox 360)");
    } else {
        us.id.vendor = 0x1209;
        us.id.product = 0xb1c0;
        us.id.version = 1;
        snprintf(us.name, sizeof us.name, "Big WM Virtual Gamepad");
    }
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno), "uinput: %s", g_strerror(errno));
        close(fd);
        return NULL;
    }
    BkVpad *v = g_new0(BkVpad, 1);
    v->fd = fd;
    char sys[64];
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys) >= 0) {
        char *dir = g_strdup_printf("/sys/devices/virtual/input/%s", sys);
        GDir *d = g_dir_open(dir, 0, NULL);
        const char *name;
        while (d && (name = g_dir_read_name(d)))
            if (g_str_has_prefix(name, "event"))
                snprintf(v->node, sizeof v->node, "/dev/input/%s", name);
        if (d)
            g_dir_close(d);
        g_free(dir);
    }
    bk_vpad_neutral(v);
    return v;
}

static int
btn_index(int code)
{
    for (int i = 0; i < NBUTTONS; i++)
        if (buttons[i] == code)
            return i;
    return -1;
}

void
bk_vpad_button(BkVpad *v, int code, bool down)
{
    int i = btn_index(code);
    if (!v || i < 0 || v->down[i] == down)
        return;
    v->down[i] = down;
    emit(v, EV_KEY, code, down);
}

static int
axis(double x)
{
    x = x < -1 ? -1 : x > 1 ? 1 : x;
    return (int)lround(x < 0 ? x * 32768 : x * 32767);
}

void
bk_vpad_stick(BkVpad *v, int side, double x, double y)
{
    if (!v)
        return;
    emit(v, EV_ABS, side ? ABS_RX : ABS_X, axis(x));
    emit(v, EV_ABS, side ? ABS_RY : ABS_Y, axis(y));
}

void
bk_vpad_trigger(BkVpad *v, int side, double value)
{
    if (!v)
        return;
    value = value < 0 ? 0 : value > 1 ? 1 : value;
    emit(v, EV_ABS, side ? ABS_RZ : ABS_Z, (int)lround(value * 1023));
}

void
bk_vpad_hat(BkVpad *v, int x, int y)
{
    if (!v)
        return;
    emit(v, EV_ABS, ABS_HAT0X, x < 0 ? -1 : x > 0);
    emit(v, EV_ABS, ABS_HAT0Y, y < 0 ? -1 : y > 0);
}

void
bk_vpad_sync(BkVpad *v)
{
    if (v)
        emit(v, EV_SYN, SYN_REPORT, 0);
}

void
bk_vpad_neutral(BkVpad *v)
{
    if (!v)
        return;
    for (int i = 0; i < NBUTTONS; i++)
        if (v->down[i]) {
            v->down[i] = false;
            emit(v, EV_KEY, buttons[i], 0);
        }
    bk_vpad_stick(v, 0, 0, 0);
    bk_vpad_stick(v, 1, 0, 0);
    bk_vpad_trigger(v, 0, 0);
    bk_vpad_trigger(v, 1, 0);
    bk_vpad_hat(v, 0, 0);
    bk_vpad_sync(v);
}

void
bk_vpad_free(BkVpad *v)
{
    if (!v)
        return;
    bk_vpad_neutral(v);
    ioctl(v->fd, UI_DEV_DESTROY);
    close(v->fd);
    g_free(v);
}

const char *
bk_vpad_node(BkVpad *v)
{
    return v && v->node[0] ? v->node : NULL;
}

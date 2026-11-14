/* SPDX-License-Identifier: GPL-2.0-or-later
 * Toque remoto: o touchscreen virtual do Sunshine (veja bk-rtouch.h). */
#define _GNU_SOURCE
#include "bk-rtouch.h"

#include <errno.h>
#include <libudev.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---- decodificador (protocolo B) --------------------------------------- */

void
bk_rt_init(BkRtDecoder *d, int min_x, int max_x, int min_y, int max_y)
{
    memset(d, 0, sizeof *d);
    d->min_x = min_x;
    d->max_x = max_x > min_x ? max_x : min_x + 1;
    d->min_y = min_y;
    d->max_y = max_y > min_y ? max_y : min_y + 1;
    for (int i = 0; i < BKRT_SLOTS; i++)
        d->id[i] = d->nid[i] = -1;
}

static double
norm(int v, int lo, int hi)
{
    double n = (double)(v - lo) / (double)(hi - lo);
    return n < 0 ? 0 : n > 1 ? 1 : n;
}

static void
out(BkRtDecoder *d, int s, BkCtlPhase ph, int x, int y, BkRtFn fn, void *data)
{
    fn(data, s, ph, norm(x, d->min_x, d->max_x), norm(y, d->min_y, d->max_y));
}

/* O estado pendente vira o entregue, com as saídas na ordem END, BEGIN, MOVE:
 * um botão solto e outro apertado no mesmo quadro nunca se sobrepõem. */
static void
commit(BkRtDecoder *d, BkRtFn fn, void *data)
{
    for (int s = 0; s < BKRT_SLOTS; s++)
        if (d->id[s] >= 0 && d->nid[s] != d->id[s])
            out(d, s, BKC_END, d->x[s], d->y[s], fn, data);
    for (int s = 0; s < BKRT_SLOTS; s++)
        if (d->nid[s] >= 0 && d->nid[s] != d->id[s])
            out(d, s, BKC_BEGIN, d->nx[s], d->ny[s], fn, data);
    for (int s = 0; s < BKRT_SLOTS; s++)
        if (d->nid[s] >= 0 && d->nid[s] == d->id[s] && (d->nx[s] != d->x[s] || d->ny[s] != d->y[s]))
            out(d, s, BKC_MOVE, d->nx[s], d->ny[s], fn, data);
    for (int s = 0; s < BKRT_SLOTS; s++) {
        d->id[s] = d->nid[s];
        d->x[s] = d->nx[s];
        d->y[s] = d->ny[s];
    }
}

void
bk_rt_event(BkRtDecoder *d, const struct input_event *e, BkRtFn fn, void *data)
{
    if (e->type == EV_SYN) {
        if (e->code == SYN_DROPPED) {
            /* o kernel perdeu eventos: o estado só volta a valer relendo-o */
            d->dropped = true;
        } else if (e->code == SYN_REPORT) {
            if (d->dropped) {
                d->dropped = false;
                d->need_resync = true;
            } else {
                commit(d, fn, data);
            }
        }
        return;
    }
    if (d->dropped || e->type != EV_ABS)
        return;
    int s = d->slot;
    switch (e->code) {
    case ABS_MT_SLOT:
        d->slot = e->value >= 0 && e->value < BKRT_SLOTS ? e->value : -1;
        break;
    case ABS_MT_TRACKING_ID:
        if (s >= 0)
            d->nid[s] = e->value < 0 ? -1 : e->value;
        break;
    case ABS_MT_POSITION_X:
        if (s >= 0)
            d->nx[s] = e->value;
        break;
    case ABS_MT_POSITION_Y:
        if (s >= 0)
            d->ny[s] = e->value;
        break;
    default:
        break; /* ABS_X/Y (contato único), pressão e orientação: não usados */
    }
}

void
bk_rt_resync(BkRtDecoder *d, const int *ids, const int *xs, const int *ys, int n, BkRtFn fn, void *data)
{
    for (int s = 0; s < BKRT_SLOTS; s++) {
        d->nid[s] = s < n && ids[s] >= 0 ? ids[s] : -1;
        d->nx[s] = s < n ? xs[s] : 0;
        d->ny[s] = s < n ? ys[s] : 0;
    }
    d->need_resync = false;
    d->dropped = false;
    commit(d, fn, data);
}

void
bk_rt_cancel_all(BkRtDecoder *d, BkRtFn fn, void *data)
{
    for (int s = 0; s < BKRT_SLOTS; s++) {
        if (d->id[s] >= 0 && fn)
            out(d, s, BKC_CANCEL, d->x[s], d->y[s], fn, data);
        d->id[s] = d->nid[s] = -1;
    }
    d->dropped = d->need_resync = false;
}

int
bk_rt_active(const BkRtDecoder *d)
{
    int n = 0;
    for (int s = 0; s < BKRT_SLOTS; s++)
        n += d->id[s] >= 0;
    return n;
}

/* ---- dispositivo -------------------------------------------------------- */

bool
bk_rt_is_remote_name(const char *name)
{
    /* Sunshine com libvirtualhid ("libvirtualhid Touchscreen"); o mesmo nome
     * com um prefixo do Sunshine também vale. O touchscreen antigo
     * (inputtino, "Touch passthrough") normalizava ao desktop inteiro, não
     * ao monitor: não é aceito, para nunca apertar o botão errado. */
    return name && strstr(name, "libvirtualhid") && g_str_has_suffix(name, "Touchscreen");
}

#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
static bool
test_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
}

static void
resync_from_kernel(BkRtDevice *dev, BkRtFn fn, void *data)
{
    struct {
        __u32 code;
        __s32 v[BKRT_SLOTS];
    } ids, xs, ys;
    ids.code = ABS_MT_TRACKING_ID;
    xs.code = ABS_MT_POSITION_X;
    ys.code = ABS_MT_POSITION_Y;
    if (ioctl(dev->fd, EVIOCGMTSLOTS(sizeof ids), &ids) < 0 || ioctl(dev->fd, EVIOCGMTSLOTS(sizeof xs), &xs) < 0 ||
        ioctl(dev->fd, EVIOCGMTSLOTS(sizeof ys), &ys) < 0) {
        bk_rt_cancel_all(&dev->dec, fn, data); /* sem o estado: soltar é o seguro */
        return;
    }
    struct input_absinfo slot;
    if (ioctl(dev->fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0)
        dev->dec.slot = slot.value >= 0 && slot.value < BKRT_SLOTS ? slot.value : -1;
    bk_rt_resync(&dev->dec, ids.v, xs.v, ys.v, BKRT_SLOTS, fn, data);
}

gboolean
bk_rt_device_open(BkRtDevice *dev, const char *node, gboolean exclusive, GError **err)
{
    memset(dev, 0, sizeof *dev);
    dev->fd = -1;
    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno), "%s: %s", node, g_strerror(errno));
        return FALSE;
    }
    char name[128] = "";
    unsigned long absbits[NBITS(ABS_CNT)] = { 0 }, props[NBITS(INPUT_PROP_CNT)] = { 0 };
    struct input_absinfo ax, ay;
    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0 || !bk_rt_is_remote_name(name) ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0 ||
        ioctl(fd, EVIOCGPROP(sizeof props), props) < 0 || !test_bit(props, INPUT_PROP_DIRECT) ||
        !test_bit(absbits, ABS_MT_SLOT) || !test_bit(absbits, ABS_MT_TRACKING_ID) ||
        !test_bit(absbits, ABS_MT_POSITION_X) || !test_bit(absbits, ABS_MT_POSITION_Y) ||
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 || ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0 ||
        ax.maximum <= ax.minimum || ay.maximum <= ay.minimum) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s não é o touchscreen remoto do Sunshine", node);
        close(fd);
        return FALSE;
    }
    dev->fd = fd;
    g_strlcpy(dev->node, node, sizeof dev->node);
    bk_rt_init(&dev->dec, ax.minimum, ax.maximum, ay.minimum, ay.maximum);
    /* o kernel só envia ABS_MT_SLOT quando o slot muda: começar do atual.
     * Dedos que já estavam na tela ficam de fora até saírem (seguro). */
    struct input_absinfo slot;
    if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0)
        dev->dec.slot = slot.value >= 0 && slot.value < BKRT_SLOTS ? slot.value : -1;
    dev->grab = BKRT_GRAB_NONE;
    if (exclusive)
        dev->grab = ioctl(fd, EVIOCGRAB, 1) == 0 ? BKRT_GRAB_OK : BKRT_GRAB_BUSY;
    return TRUE;
}

gboolean
bk_rt_device_read(BkRtDevice *dev, BkRtFn fn, void *data)
{
    struct input_event ev[64];
    for (;;) {
        ssize_t n = read(dev->fd, ev, sizeof ev);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return TRUE;
        if (n <= 0) { /* ENODEV: o Sunshine encerrou a sessão e apagou o dispositivo */
            bk_rt_cancel_all(&dev->dec, fn, data);
            return FALSE;
        }
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
            bk_rt_event(&dev->dec, &ev[i], fn, data);
            if (dev->dec.need_resync)
                resync_from_kernel(dev, fn, data);
        }
    }
}

void
bk_rt_device_close(BkRtDevice *dev, BkRtFn fn, void *data)
{
    if (dev->fd < 0)
        return;
    bk_rt_cancel_all(&dev->dec, fn, data);
    if (dev->grab == BKRT_GRAB_OK)
        ioctl(dev->fd, EVIOCGRAB, 0);
    close(dev->fd);
    dev->fd = -1;
    dev->grab = BKRT_GRAB_NONE;
}

char **
bk_rt_find_nodes(void)
{
    GPtrArray *found = g_ptr_array_new();
    GDir *d = g_dir_open("/sys/class/input", 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d))) {
        if (!g_str_has_prefix(e, "event"))
            continue;
        char *path = g_build_filename("/sys/class/input", e, "device", "name", NULL);
        char *name = NULL;
        if (g_file_get_contents(path, &name, NULL, NULL) && bk_rt_is_remote_name(g_strstrip(name)))
            g_ptr_array_add(found, g_build_filename("/dev/input", e, NULL));
        g_free(name);
        g_free(path);
    }
    if (d)
        g_dir_close(d);
    g_ptr_array_add(found, NULL);
    return (char **)g_ptr_array_free(found, FALSE);
}

/* ---- conexão a quente ---------------------------------------------------- */

struct BkRtWatch {
    struct udev *udev;
    struct udev_monitor *mon;
};

BkRtWatch *
bk_rt_watch_new(void)
{
    struct udev *u = udev_new();
    if (!u)
        return NULL;
    /* "udev", não "kernel": o evento chega depois das regras (ACL uaccess) */
    struct udev_monitor *m = udev_monitor_new_from_netlink(u, "udev");
    if (!m || udev_monitor_filter_add_match_subsystem_devtype(m, "input", NULL) < 0 ||
        udev_monitor_enable_receiving(m) < 0) {
        if (m)
            udev_monitor_unref(m);
        udev_unref(u);
        return NULL;
    }
    BkRtWatch *w = g_new0(BkRtWatch, 1);
    w->udev = u;
    w->mon = m;
    return w;
}

int
bk_rt_watch_fd(BkRtWatch *w)
{
    return w ? udev_monitor_get_fd(w->mon) : -1;
}

gboolean
bk_rt_watch_drain(BkRtWatch *w)
{
    gboolean changed = FALSE;
    struct udev_device *d;
    while (w && (d = udev_monitor_receive_device(w->mon))) {
        const char *sys = udev_device_get_sysname(d), *act = udev_device_get_action(d);
        if (sys && g_str_has_prefix(sys, "event") && act && (!strcmp(act, "add") || !strcmp(act, "remove")))
            changed = TRUE;
        udev_device_unref(d);
    }
    return changed;
}

void
bk_rt_watch_free(BkRtWatch *w)
{
    if (!w)
        return;
    udev_monitor_unref(w->mon);
    udev_unref(w->udev);
    g_free(w);
}

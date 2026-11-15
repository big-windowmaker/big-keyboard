/* SPDX-License-Identifier: GPL-2.0-or-later
 * Entrega das teclas: virtual-keyboard (Wayland) e XTest (X11). */
#define _GNU_SOURCE
#include "bk-inject.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#ifdef BK_HAVE_X11
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>
#include <gdk/gdkx.h>
#endif
#ifdef BK_HAVE_WAYLAND
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include <gdk/gdkwayland.h>
#endif

#define MAX_SYMS 240 /* keycodes do keymap gerado (Wayland) */
#define SPARE 4      /* keycodes livres rodando (X11) */

struct BkInject {
    const char *backend;
#ifdef BK_HAVE_WAYLAND
    struct wl_display *wl;
    struct zwp_virtual_keyboard_manager_v1 *mgr;
    struct zwp_virtual_keyboard_v1 *vk;
    xkb_keysym_t syms[MAX_SYMS];
    int nsyms;
    struct zwlr_virtual_pointer_manager_v1 *pmgr;
    uint32_t pmgr_version;
    struct zwlr_virtual_pointer_v1 *vp; /* preso a vp_output (NULL: sem monitor) */
    struct wl_output *vp_output;
#endif
    double scroll_x, scroll_y; /* X11: o resto que ainda não deu um passo da roda */
#ifdef BK_HAVE_X11
    Display *dpy;
    int spare[SPARE], nspare, next_spare;
    KeySym spare_orig[SPARE];
#endif
};

/* Próximo caractere de uma string UTF-8; avança *p. */
static uint32_t
utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c;
    int n;
    if (s[0] < 0x80) {
        c = s[0];
        n = 1;
    } else if ((s[0] & 0xe0) == 0xc0) {
        c = s[0] & 0x1f;
        n = 2;
    } else if ((s[0] & 0xf0) == 0xe0) {
        c = s[0] & 0x0f;
        n = 3;
    } else {
        c = s[0] & 0x07;
        n = 4;
    }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            *p += i;
            return 0xfffd;
        }
        c = (c << 6) | (s[i] & 0x3f);
    }
    *p += n;
    return c;
}

/* keysym de um evento de tecla (nome, ou um caractere). */
static xkb_keysym_t
out_keysym(const BkOut *o)
{
    const char *k = o->keysym;
    xkb_keysym_t sym = xkb_keysym_from_name(k, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) {
        const char *p = k;
        uint32_t c = utf8_next(&p);
        if (c && !*p)
            sym = xkb_utf32_to_keysym(c);
    }
    return sym;
}

static uint32_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- Wayland ------------------------------------------------------------- */
#ifdef BK_HAVE_WAYLAND

static void
reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    BkInject *in = data;
    if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
        in->mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) {
        in->pmgr_version = MIN(version, 2);
        in->pmgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, in->pmgr_version);
    }
}

static void
reg_remove(void *data, struct wl_registry *r, uint32_t name)
{
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* O keymap com um keycode por keysym usado (como o wtype). Modificadores
 * reais do "complete": Shift 1, Control 4, Mod1 (Alt) 8, Mod4 (Super) 64,
 * Mod5 (AltGr) 128. */
static int
wl_upload_keymap(BkInject *in)
{
    GString *km = g_string_new("xkb_keymap {\nxkb_keycodes \"bk\" { minimum = 8; maximum = 255;\n");
    for (int i = 0; i < in->nsyms; i++)
        g_string_append_printf(km, "<K%d> = %d;\n", i, i + 9);
    g_string_append(km, "};\nxkb_types \"bk\" { include \"complete\" };\n"
                        "xkb_compatibility \"bk\" { include \"complete\" };\nxkb_symbols \"bk\" {\n");
    for (int i = 0; i < in->nsyms; i++) {
        char name[64];
        xkb_keysym_get_name(in->syms[i], name, sizeof name);
        g_string_append_printf(km, "key <K%d> {[ %s ]};\n", i, name);
    }
    g_string_append(km, "};\n};\n");
    int fd = memfd_create("big-keyboard-keymap", MFD_CLOEXEC);
    int ok = fd >= 0 && write(fd, km->str, km->len + 1) == (ssize_t)(km->len + 1);
    if (ok)
        zwp_virtual_keyboard_v1_keymap(in->vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)km->len + 1);
    if (fd >= 0)
        close(fd);
    g_string_free(km, TRUE);
    return ok;
}

static int
wl_keycode(BkInject *in, xkb_keysym_t sym)
{
    for (int i = 0; i < in->nsyms; i++)
        if (in->syms[i] == sym)
            return i + 1; /* evdev = xkb - 8 */
    if (in->nsyms >= MAX_SYMS)
        in->nsyms = 0; /* recomeça o keymap */
    in->syms[in->nsyms++] = sym;
    wl_upload_keymap(in);
    return in->nsyms;
}

static uint32_t
wl_mask(unsigned mods)
{
    return (mods & BK_MOD_SHIFT ? 1 : 0) | (mods & BK_MOD_CTRL ? 4 : 0) | (mods & BK_MOD_ALT ? 8 : 0) |
           (mods & BK_MOD_SUPER ? 64 : 0) | (mods & BK_MOD_ALTGR ? 128 : 0);
}

static void
wl_send(BkInject *in, xkb_keysym_t sym, unsigned mods)
{
    int kc = wl_keycode(in, sym);
    uint32_t m = wl_mask(mods);
    if (m)
        zwp_virtual_keyboard_v1_modifiers(in->vk, m, 0, 0, 0);
    zwp_virtual_keyboard_v1_key(in->vk, now_ms(), (uint32_t)kc, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(in->vk, now_ms(), (uint32_t)kc, WL_KEYBOARD_KEY_STATE_RELEASED);
    if (m)
        zwp_virtual_keyboard_v1_modifiers(in->vk, 0, 0, 0, 0);
}

static int
wl_open(BkInject *in, GdkDisplay *display)
{
    if (!GDK_IS_WAYLAND_DISPLAY(display))
        return 0;
    in->wl = gdk_wayland_display_get_wl_display(display);
    struct wl_registry *reg = wl_display_get_registry(in->wl);
    wl_registry_add_listener(reg, &reg_listener, in);
    wl_display_roundtrip(in->wl);
    wl_registry_destroy(reg);
    GdkSeat *seat = gdk_display_get_default_seat(display);
    struct wl_seat *wseat = seat ? gdk_wayland_seat_get_wl_seat(seat) : NULL;
    if (!in->mgr || !wseat)
        return 0;
    in->vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(in->mgr, wseat);
    in->syms[in->nsyms++] = XKB_KEY_space; /* o protocolo pede um keymap antes da primeira tecla */
    wl_upload_keymap(in);
    wl_display_flush(in->wl);
    in->backend = "wayland";
    return 1;
}
#endif

/* ---- X11 ----------------------------------------------------------------- */
#ifdef BK_HAVE_X11

static void
x11_mod_keys(BkInject *in, unsigned mods, Bool press)
{
    static const struct {
        unsigned mod;
        KeySym sym;
    } t[] = { { BK_MOD_SHIFT, XK_Shift_L },
              { BK_MOD_CTRL, XK_Control_L },
              { BK_MOD_ALT, XK_Alt_L },
              { BK_MOD_SUPER, XK_Super_L },
              { BK_MOD_ALTGR, XK_ISO_Level3_Shift } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        size_t j = press ? i : sizeof t / sizeof t[0] - 1 - i;
        if (!(mods & t[j].mod))
            continue;
        KeyCode kc = XKeysymToKeycode(in->dpy, t[j].sym);
        if (kc)
            XTestFakeKeyEvent(in->dpy, kc, press, CurrentTime);
    }
}

/* Keycodes sem nenhum keysym: os de reserva para caracteres fora do layout. */
static void
x11_find_spare(BkInject *in)
{
    int min, max, per;
    XDisplayKeycodes(in->dpy, &min, &max);
    KeySym *map = XGetKeyboardMapping(in->dpy, (KeyCode)min, max - min + 1, &per);
    if (!map)
        return;
    for (int kc = max; kc >= min && in->nspare < SPARE; kc--) {
        int empty = 1;
        for (int j = 0; j < per; j++)
            if (map[(kc - min) * per + j] != NoSymbol)
                empty = 0;
        if (empty)
            in->spare[in->nspare++] = kc;
    }
    XFree(map);
}

static void
x11_send(BkInject *in, KeySym sym, unsigned mods)
{
    KeyCode kc = XKeysymToKeycode(in->dpy, sym);
    unsigned extra = 0;
    if (kc) {
        KeySym l0 = XkbKeycodeToKeysym(in->dpy, kc, 0, 0), l1 = XkbKeycodeToKeysym(in->dpy, kc, 0, 1);
        if (l0 != sym && l1 == sym)
            extra = BK_MOD_SHIFT;
        else if (l0 != sym)
            kc = 0; /* em outro nível (AltGr...): usa um keycode de reserva */
    }
    if (!kc && in->nspare) {
        kc = (KeyCode)in->spare[in->next_spare];
        in->next_spare = (in->next_spare + 1) % in->nspare;
        KeySym syms[2] = { sym, sym };
        XChangeKeyboardMapping(in->dpy, kc, 2, syms, 1);
        XSync(in->dpy, False);
    }
    if (!kc)
        return;
    /* Ctrl+Shift+letra: a letra minúscula com Shift (a tecla de base) */
    x11_mod_keys(in, mods | extra, True);
    XTestFakeKeyEvent(in->dpy, kc, True, CurrentTime);
    XTestFakeKeyEvent(in->dpy, kc, False, CurrentTime);
    x11_mod_keys(in, mods | extra, False);
    XFlush(in->dpy);
}

static int
x11_open(BkInject *in, GdkDisplay *display)
{
    if (!GDK_IS_X11_DISPLAY(display))
        return 0;
    in->dpy = GDK_DISPLAY_XDISPLAY(display);
    int ev, er, maj, min;
    if (!XTestQueryExtension(in->dpy, &ev, &er, &maj, &min))
        return 0;
    x11_find_spare(in);
    in->backend = "x11";
    return 1;
}

static void
x11_close(BkInject *in)
{
    /* devolve os keycodes de reserva vazios */
    KeySym none[2] = { NoSymbol, NoSymbol };
    for (int i = 0; i < in->nspare; i++)
        XChangeKeyboardMapping(in->dpy, (KeyCode)in->spare[i], 2, none, 1);
    XSync(in->dpy, False);
}
#endif

/* ---- público ------------------------------------------------------------- */

BkInject *
bk_inject_new(GdkDisplay *display)
{
    BkInject *in = g_new0(BkInject, 1);
#ifdef BK_HAVE_WAYLAND
    if (wl_open(in, display))
        return in;
#endif
#ifdef BK_HAVE_X11
    if (x11_open(in, display))
        return in;
#endif
    (void)display;
    return in;
}

void
bk_inject_free(BkInject *in)
{
    if (!in)
        return;
#ifdef BK_HAVE_WAYLAND
    if (in->vp)
        zwlr_virtual_pointer_v1_destroy(in->vp);
    if (in->pmgr)
        zwlr_virtual_pointer_manager_v1_destroy(in->pmgr);
    if (in->vk)
        zwp_virtual_keyboard_v1_destroy(in->vk);
    if (in->mgr)
        zwp_virtual_keyboard_manager_v1_destroy(in->mgr);
    if (in->wl)
        wl_display_flush(in->wl);
#endif
#ifdef BK_HAVE_X11
    if (in->dpy)
        x11_close(in);
#endif
    g_free(in);
}

const char *
bk_inject_backend(const BkInject *in)
{
    return in ? in->backend : NULL;
}

static void
send_sym(BkInject *in, xkb_keysym_t sym, unsigned mods)
{
    if (sym == XKB_KEY_NoSymbol)
        return;
#ifdef BK_HAVE_WAYLAND
    if (in->vk) {
        wl_send(in, sym, mods);
        wl_display_flush(in->wl);
        return;
    }
#endif
#ifdef BK_HAVE_X11
    if (in->dpy)
        x11_send(in, (KeySym)sym, mods);
#endif
}

void
bk_inject_out(BkInject *in, const BkOut *o)
{
    if (!in || !in->backend)
        return;
    if (o->kind == BK_OUT_KEY) {
        send_sym(in, out_keysym(o), o->mods);
        return;
    }
    /* texto: cada caractere (um emoji pode ter dois: ❤ + U+FE0F) */
    for (const char *p = o->text; *p;) {
        uint32_t c = utf8_next(&p);
        send_sym(in, xkb_utf32_to_keysym(c), 0);
    }
}

/* ---- ponteiro -------------------------------------------------------------- */

#ifdef BK_HAVE_WAYLAND
/* O ponteiro virtual preso ao monitor (o movimento absoluto vale dentro
 * dele); outro monitor, outro ponteiro. */
static struct zwlr_virtual_pointer_v1 *
wl_pointer_for(BkInject *in, GdkMonitor *monitor)
{
    struct wl_output *out = monitor && GDK_IS_WAYLAND_MONITOR(monitor) ? gdk_wayland_monitor_get_wl_output(monitor)
                                                                        : NULL;
    if (in->vp && (out == in->vp_output || !out))
        return in->vp;
    if (in->vp)
        zwlr_virtual_pointer_v1_destroy(in->vp);
    if (out && in->pmgr_version >= 2)
        in->vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(in->pmgr, NULL, out);
    else
        in->vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(in->pmgr, NULL);
    in->vp_output = out;
    return in->vp;
}
#endif

gboolean
bk_inject_has_pointer(const BkInject *in)
{
#ifdef BK_HAVE_WAYLAND
    if (in->pmgr)
        return TRUE;
#endif
#ifdef BK_HAVE_X11
    if (in->dpy)
        return TRUE;
#endif
    (void)in;
    return FALSE;
}

void
bk_inject_pointer_rel(BkInject *in, double dx, double dy)
{
#ifdef BK_HAVE_WAYLAND
    if (in->pmgr) {
        struct zwlr_virtual_pointer_v1 *vp = wl_pointer_for(in, NULL);
        zwlr_virtual_pointer_v1_motion(vp, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_flush(in->wl);
        return;
    }
#endif
#ifdef BK_HAVE_X11
    if (in->dpy) {
        XTestFakeRelativeMotionEvent(in->dpy, (int)lround(dx), (int)lround(dy), CurrentTime);
        XFlush(in->dpy);
    }
#endif
    (void)in;
    (void)dx;
    (void)dy;
}

void
bk_inject_pointer_abs(BkInject *in, GdkMonitor *monitor, double x, double y)
{
    GdkRectangle g = { 0, 0, 1, 1 };
    if (monitor)
        gdk_monitor_get_geometry(monitor, &g);
#ifdef BK_HAVE_WAYLAND
    if (in->pmgr) {
        struct zwlr_virtual_pointer_v1 *vp = wl_pointer_for(in, monitor);
        /* dentro do monitor do ponteiro: extensão = o tamanho dele */
        uint32_t w = (uint32_t)MAX(1, g.width), h = (uint32_t)MAX(1, g.height);
        uint32_t px = (uint32_t)CLAMP(lround(x), 0, (long)w - 1), py = (uint32_t)CLAMP(lround(y), 0, (long)h - 1);
        zwlr_virtual_pointer_v1_motion_absolute(vp, now_ms(), px, py, w, h);
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_flush(in->wl);
        return;
    }
#endif
#ifdef BK_HAVE_X11
    if (in->dpy) {
        int sc = monitor ? gdk_monitor_get_scale_factor(monitor) : 1;
        XTestFakeMotionEvent(in->dpy, -1, (int)lround((g.x + x) * sc), (int)lround((g.y + y) * sc), CurrentTime);
        XFlush(in->dpy);
    }
#endif
    (void)in;
    (void)x;
    (void)y;
}

void
bk_inject_pointer_button(BkInject *in, int button, gboolean down)
{
#ifdef BK_HAVE_WAYLAND
    if (in->pmgr) {
        struct zwlr_virtual_pointer_v1 *vp = wl_pointer_for(in, NULL);
        zwlr_virtual_pointer_v1_button(vp, now_ms(), (uint32_t)button,
                                       down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_flush(in->wl);
        return;
    }
#endif
#ifdef BK_HAVE_X11
    if (in->dpy) {
        XTestFakeButtonEvent(in->dpy, button == BTN_RIGHT ? 3 : button == BTN_MIDDLE ? 2 : 1, down, CurrentTime);
        XFlush(in->dpy);
    }
#endif
    (void)in;
    (void)button;
    (void)down;
}

#define WHEEL_PX 40.0 /* X11: pixels por passo da roda */

void
bk_inject_pointer_scroll(BkInject *in, double dx, double dy)
{
#ifdef BK_HAVE_WAYLAND
    if (in->pmgr) {
        struct zwlr_virtual_pointer_v1 *vp = wl_pointer_for(in, NULL);
        uint32_t t = now_ms();
        zwlr_virtual_pointer_v1_axis_source(vp, WL_POINTER_AXIS_SOURCE_FINGER);
        if (dy != 0)
            zwlr_virtual_pointer_v1_axis(vp, t, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
        if (dx != 0)
            zwlr_virtual_pointer_v1_axis(vp, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_flush(in->wl);
        return;
    }
#endif
#ifdef BK_HAVE_X11
    if (in->dpy) {
        in->scroll_x += dx;
        in->scroll_y += dy;
        for (; fabs(in->scroll_y) >= WHEEL_PX; in->scroll_y -= copysign(WHEEL_PX, in->scroll_y)) {
            unsigned b = in->scroll_y > 0 ? 5 : 4;
            XTestFakeButtonEvent(in->dpy, b, True, CurrentTime);
            XTestFakeButtonEvent(in->dpy, b, False, CurrentTime);
        }
        for (; fabs(in->scroll_x) >= WHEEL_PX; in->scroll_x -= copysign(WHEEL_PX, in->scroll_x)) {
            unsigned b = in->scroll_x > 0 ? 7 : 6;
            XTestFakeButtonEvent(in->dpy, b, True, CurrentTime);
            XTestFakeButtonEvent(in->dpy, b, False, CurrentTime);
        }
        XFlush(in->dpy);
    }
#endif
    (void)in;
    (void)dx;
    (void)dy;
}

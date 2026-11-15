/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * O monitor pelo nome do conector (DP-1, HDMI-A-1): o controle na tela
 * precisa ficar no monitor que o Sunshine transmite, porque o toque remoto
 * chega normalizado à imagem desse monitor.
 *
 * O GTK 3 não dá o nome do conector no Wayland (gdk_monitor_get_model diz
 * "Unknown"); ele vem do xdg-output, pedido numa fila própria para não
 * misturar com os eventos do GDK. No X11, o modelo do GdkMonitor já é o
 * nome da saída RandR, o mesmo que o GTK 4 (Big Remote Play) mostra.
 */
#include "bk-app.h"

#include <string.h>
#ifdef BK_HAVE_WAYLAND
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <gdk/gdkwayland.h>

typedef struct {
    struct zxdg_output_manager_v1 *mgr;
} Ctx;

typedef struct {
    char s[64];
} Name;

static void
reg_global(void *data, struct wl_registry *r, uint32_t id, const char *iface, uint32_t version)
{
    Ctx *c = data;
    if (!strcmp(iface, zxdg_output_manager_v1_interface.name) && version >= 2 && !c->mgr)
        c->mgr = wl_registry_bind(r, id, &zxdg_output_manager_v1_interface, 2);
}
static void
reg_remove(void *data, struct wl_registry *r, uint32_t id)
{
    (void)data;
    (void)r;
    (void)id;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void xo_pos(void *d, struct zxdg_output_v1 *o, int32_t x, int32_t y) { (void)d; (void)o; (void)x; (void)y; }
static void xo_size(void *d, struct zxdg_output_v1 *o, int32_t w, int32_t h) { (void)d; (void)o; (void)w; (void)h; }
static void xo_done(void *d, struct zxdg_output_v1 *o) { (void)d; (void)o; }
static void
xo_name(void *d, struct zxdg_output_v1 *o, const char *name)
{
    (void)o;
    g_strlcpy(d, name, 64);
}
static void xo_desc(void *d, struct zxdg_output_v1 *o, const char *s) { (void)d; (void)o; (void)s; }
static const struct zxdg_output_v1_listener xo_listener = { xo_pos, xo_size, xo_done, xo_name, xo_desc };

static GdkMonitor *
wayland_by_name(GdkDisplay *dpy, const char *name)
{
    struct wl_display *wd = gdk_wayland_display_get_wl_display(dpy);
    struct wl_event_queue *q = wl_display_create_queue(wd);
    struct wl_display *wrap = wl_proxy_create_wrapper(wd);
    wl_proxy_set_queue((struct wl_proxy *)wrap, q);
    struct wl_registry *reg = wl_display_get_registry(wrap);
    Ctx ctx = { 0 };
    wl_registry_add_listener(reg, &reg_listener, &ctx);
    wl_display_roundtrip_queue(wd, q);
    GdkMonitor *found = NULL;
    int n = gdk_display_get_n_monitors(dpy);
    if (ctx.mgr) {
        Name *names = g_new0(Name, n);
        struct zxdg_output_v1 **xo = g_new0(struct zxdg_output_v1 *, n);
        for (int i = 0; i < n; i++) {
            struct wl_output *out = gdk_wayland_monitor_get_wl_output(gdk_display_get_monitor(dpy, i));
            if (!out)
                continue;
            xo[i] = zxdg_output_manager_v1_get_xdg_output(ctx.mgr, out);
            zxdg_output_v1_add_listener(xo[i], &xo_listener, names[i].s);
        }
        wl_display_roundtrip_queue(wd, q);
        for (int i = 0; i < n; i++) {
            if (!found && xo[i] && !strcmp(names[i].s, name))
                found = gdk_display_get_monitor(dpy, i);
            if (xo[i])
                zxdg_output_v1_destroy(xo[i]);
        }
        g_free(xo);
        g_free(names);
        zxdg_output_manager_v1_destroy(ctx.mgr);
    }
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrap);
    wl_event_queue_destroy(q);
    return found;
}

/* ---- área útil: o GDK 3 dá o monitor inteiro como workarea no Wayland ---- *
 * Uma superfície layer-shell presa às quatro bordas, sem buffer, recebe do
 * compositor o tamanho que sobra depois das zonas exclusivas dos painéis.
 * Sem buffer ela nunca aparece; é destruída logo depois do configure. */
typedef struct {
    struct wl_compositor *comp;
    struct zwlr_layer_shell_v1 *shell;
    int height; /* 0: sem configure */
} Probe;

static void
probe_global(void *data, struct wl_registry *r, uint32_t id, const char *iface, uint32_t version)
{
    (void)version;
    Probe *p = data;
    if (!strcmp(iface, wl_compositor_interface.name) && !p->comp)
        p->comp = wl_registry_bind(r, id, &wl_compositor_interface, 1);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name) && !p->shell)
        p->shell = wl_registry_bind(r, id, &zwlr_layer_shell_v1_interface, 1);
}
static const struct wl_registry_listener probe_reg_listener = { probe_global, reg_remove };

static void
probe_configure(void *data, struct zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h)
{
    (void)s;
    (void)serial;
    (void)w;
    ((Probe *)data)->height = (int)h;
}
static void probe_closed(void *data, struct zwlr_layer_surface_v1 *s) { (void)data; (void)s; }
static const struct zwlr_layer_surface_v1_listener probe_listener = { probe_configure, probe_closed };

static int
wayland_usable_height(GdkDisplay *dpy, GdkMonitor *mon, int full)
{
    struct wl_output *out = gdk_wayland_monitor_get_wl_output(mon);
    if (!out)
        return full;
    struct wl_display *wd = gdk_wayland_display_get_wl_display(dpy);
    struct wl_event_queue *q = wl_display_create_queue(wd);
    struct wl_display *wrap = wl_proxy_create_wrapper(wd);
    wl_proxy_set_queue((struct wl_proxy *)wrap, q);
    struct wl_registry *reg = wl_display_get_registry(wrap);
    Probe p = { 0 };
    wl_registry_add_listener(reg, &probe_reg_listener, &p);
    wl_display_roundtrip_queue(wd, q);
    if (p.comp && p.shell) {
        struct wl_surface *surf = wl_compositor_create_surface(p.comp);
        struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(
            p.shell, surf, out, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "big-keyboard-probe");
        zwlr_layer_surface_v1_add_listener(ls, &probe_listener, &p);
        zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(ls, 0, 0);
        zwlr_layer_surface_v1_set_exclusive_zone(ls, 0);
        wl_surface_commit(surf);
        wl_display_roundtrip_queue(wd, q);
        zwlr_layer_surface_v1_destroy(ls);
        wl_surface_destroy(surf);
        wl_display_flush(wd);
    }
    /* o destroy do layer-shell só existe a partir da versão 3: só o proxy */
    if (p.shell)
        wl_proxy_destroy((struct wl_proxy *)p.shell);
    if (p.comp)
        wl_compositor_destroy(p.comp);
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrap);
    wl_event_queue_destroy(q);
    return p.height > 0 && p.height < full ? p.height : full;
}
#endif

int
bk_monitor_usable_height(GdkDisplay *dpy, GdkMonitor *mon, int full)
{
#ifdef BK_HAVE_WAYLAND
    if (dpy && mon && GDK_IS_WAYLAND_DISPLAY(dpy))
        return wayland_usable_height(dpy, mon, full);
#else
    (void)dpy;
    (void)mon;
#endif
    return full;
}

GdkMonitor *
bk_monitor_by_name(GdkDisplay *dpy, const char *name)
{
    if (!dpy || !name || !*name)
        return NULL;
#ifdef BK_HAVE_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(dpy))
        return wayland_by_name(dpy, name);
#endif
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++) {
        GdkMonitor *m = gdk_display_get_monitor(dpy, i);
        const char *model = gdk_monitor_get_model(m);
        if (model && !strcmp(model, name))
            return m;
    }
    return NULL;
}

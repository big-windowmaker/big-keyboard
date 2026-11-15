/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Toque remoto: os dedos de um celular que joga pelo Moonlight, lidos do
 * touchscreen virtual do Sunshine (bk-rtouch.c) enquanto o teclado ou o
 * controle na tela está aberto, e entregues a quem estiver embaixo deles.
 *
 * Cada dedo escolhe o destino ao tocar e fica com ele até sair da tela:
 *
 *   1. o controle na tela, se caiu no botão do teclado, se o editor está
 *      aberto, ou num controle fora da janela do teclado;
 *   2. o teclado, se caiu na janela dele (as teclas ganham dos controles
 *      que ficam embaixo dela);
 *   3. o mouse (bk-touchmouse.c), com o teclado aberto e o dispositivo só
 *      nosso (modo exclusive): touchpad com o teclado sozinho, toque
 *      direto com teclado e controle juntos (RemoteMouse=auto);
 *   4. nada: com só o controle aberto, tocar fora dos controles não faz
 *      nada (no meio do jogo, um clique perdido atrapalha).
 *
 * O toque chega normalizado à imagem do monitor transmitido
 * (overlay-output); o teclado e o controle ficam nele enquanto houver um
 * celular conectado.
 *
 * Abrir e fechar o dispositivo nunca acontece dentro da leitura dele: um
 * dedo pode fechar o teclado (Ocultar) ou trocar pelo controle, então quem
 * muda a visibilidade pede bk_remote_sync, que age no laço principal.
 */
#include "bk-app.h"
#include "bk-rtouch.h"
#include "bk-touchmouse.h"

#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>

#define RT_DEVS 4

typedef enum { TGT_NONE, TGT_OVERLAY, TGT_KEYBOARD, TGT_MOUSE } Target;

typedef struct {
    BkRemote *r;
    BkRtDevice dev;
    guint src;
    char token[BKRT_SLOTS];  /* identidade de cada dedo (endereços únicos) */
    bool stale[BKRT_SLOTS];  /* soltos à força (--release) com o dedo ainda na tela */
    Target tgt[BKRT_SLOTS];
} RtDev;

struct BkRemote {
    BkApp *app;
    RtDev rt[RT_DEVS]; /* pode haver dois Sunshine: o normal e o do Game Window */
    BkRtWatch *watch;
    guint watch_src;
    gboolean running;
    guint sync_id;
    char state[16];
    BkTouchMouse tm;
    BkTmSink sink;
    GdkMonitor *tm_monitor; /* o monitor do gesto em curso (movimento absoluto) */
};

/* ---- estado -------------------------------------------------------------- */

static void
set_state(BkRemote *r, const char *state)
{
    if (!strcmp(r->state, state))
        return;
    g_strlcpy(r->state, state, sizeof r->state);
    g_message("toque remoto: %s", state);
    bk_app_update_state(r->app);
}

static void
update_state(BkRemote *r)
{
    int open = 0, grabbed = 0, busy = 0;
    for (int i = 0; i < RT_DEVS; i++)
        if (r->rt[i].dev.fd >= 0) {
            open++;
            grabbed += r->rt[i].dev.grab == BKRT_GRAB_OK;
            busy += r->rt[i].dev.grab == BKRT_GRAB_BUSY;
        }
    if (!open)
        set_state(r, "waiting");
    else
        set_state(r, busy ? "busy" : grabbed ? "active" : "shared");
}

const char *
bk_remote_state(BkRemote *r)
{
    return r && r->state[0] ? r->state : "idle";
}

gboolean
bk_remote_present(BkRemote *r)
{
    for (int i = 0; r && i < RT_DEVS; i++)
        if (r->rt[i].dev.fd >= 0)
            return TRUE;
    return FALSE;
}

gboolean
bk_remote_shadows_local(BkRemote *r)
{
    /* lendo sem exclusividade, o compositor também entrega os mesmos dedos
     * às janelas: lá eles não podem valer de novo */
    for (int i = 0; r && i < RT_DEVS; i++)
        if (r->rt[i].dev.fd >= 0 && r->rt[i].dev.grab != BKRT_GRAB_OK)
            return TRUE;
    return FALSE;
}

GdkMonitor *
bk_remote_monitor(BkRemote *r)
{
    GdkDisplay *d = gdk_display_get_default();
    GdkMonitor *m = bk_monitor_by_name(d, r->app->cfg.overlay_output);
    if (!m)
        m = gdk_display_get_primary_monitor(d);
    return m ? m : gdk_display_get_monitor(d, 0);
}

/* ---- mouse --------------------------------------------------------------- */

static void
m_rel(void *d, double dx, double dy)
{
    bk_inject_pointer_rel(((BkRemote *)d)->app->inject, dx, dy);
}
static void
m_abs(void *d, double x, double y)
{
    BkRemote *r = d;
    bk_inject_pointer_abs(r->app->inject, r->tm_monitor, x, y);
}
static void
m_button(void *d, int b, bool down)
{
    bk_inject_pointer_button(((BkRemote *)d)->app->inject, b, down);
}
static void
m_scroll(void *d, double dx, double dy)
{
    bk_inject_pointer_scroll(((BkRemote *)d)->app->inject, dx, dy);
}

/* O modo do mouse agora, ou -1 (desligado). Só com o dispositivo agarrado:
 * sem a exclusividade, o compositor também faria o clique. */
static int
mouse_mode(BkRemote *r, const RtDev *rd)
{
    BkApp *app = r->app;
    const char *m = app->cfg.remote_mouse;
    if (!strcmp(m, "off") || rd->dev.grab != BKRT_GRAB_OK || !bk_osk_visible(app->osk) ||
        !bk_inject_has_pointer(app->inject))
        return -1;
    if (!strcmp(m, "touchpad"))
        return BKTM_TOUCHPAD;
    if (!strcmp(m, "direct"))
        return BKTM_DIRECT;
    return bk_overlay_visible(app->overlay) ? BKTM_DIRECT : BKTM_TOUCHPAD;
}

/* ---- roteamento ----------------------------------------------------------- */

static void
route(RtDev *rd, int slot, BkCtlPhase ph, double nx, double ny)
{
    BkRemote *r = rd->r;
    BkApp *app = r->app;
    const void *id = &rd->token[slot];
    GdkMonitor *mon = bk_remote_monitor(r);
    GdkRectangle g = { 0, 0, 1, 1 };
    if (mon)
        gdk_monitor_get_geometry(mon, &g);
    double x = nx * g.width, y = ny * g.height;
    if (ph == BKC_BEGIN) {
        GdkRectangle kr;
        int mode;
        gboolean on_kb = bk_osk_remote_rect(app->osk, mon, &kr) && x >= kr.x && x < kr.x + kr.width && y >= kr.y &&
                         y < kr.y + kr.height;
        /* sobre o teclado, as teclas ganham dos controles que ficam embaixo
         * dele; o botão de troca do controle e o editor ganham de tudo */
        if (bk_overlay_visible(app->overlay) && bk_overlay_remote_claim(app->overlay, nx, ny, on_kb))
            rd->tgt[slot] = TGT_OVERLAY;
        else if (on_kb)
            rd->tgt[slot] = TGT_KEYBOARD;
        else if ((mode = mouse_mode(r, rd)) >= 0) {
            if (r->tm.n == 0) { /* gesto novo: modo e folga do momento */
                double slop = MAX(10.0, g.height * (mode == BKTM_DIRECT ? 0.02 : 0.012));
                bk_tm_init(&r->tm, (BkTmMode)mode, app->cfg.remote_mouse_speed, slop);
                r->tm_monitor = mon;
            }
            rd->tgt[slot] = TGT_MOUSE;
        } else
            rd->tgt[slot] = TGT_NONE;
    }
    Target t = rd->tgt[slot];
    if (ph == BKC_END || ph == BKC_CANCEL)
        rd->tgt[slot] = TGT_NONE;
    switch (t) {
    case TGT_OVERLAY:
        bk_overlay_remote_touch(app->overlay, id, ph, nx, ny);
        break;
    case TGT_KEYBOARD:
        bk_osk_remote_touch(app->osk, mon, id, ph, x, y);
        break;
    case TGT_MOUSE:
        bk_tm_touch(&r->tm, id, ph, x, y, (uint64_t)(g_get_monotonic_time() / 1000), &r->sink);
        break;
    case TGT_NONE:
        break;
    }
}

static void
on_contact(void *data, int slot, BkCtlPhase ph, double nx, double ny)
{
    RtDev *rd = data;
    if (rd->stale[slot]) {
        /* Soltamos este dedo à força (o Big Remote Play viu a sessão cair, ou
         * --release), mas o Sunshine ainda o mantém na tela. Se ele voltar a
         * se mover, é o celular de novo: vale como um toque novo ali. */
        rd->stale[slot] = false;
        if (ph != BKC_MOVE)
            return;
        ph = BKC_BEGIN;
    }
    route(rd, slot, ph, nx, ny);
}

/* ---- dispositivos ------------------------------------------------------------ */

static void
detach(RtDev *rd)
{
    if (rd->src) {
        g_source_remove(rd->src);
        rd->src = 0;
    }
    bk_rt_device_close(&rd->dev, on_contact, rd); /* encerra os dedos (CANCEL) */
    memset(rd->stale, 0, sizeof rd->stale);
    memset(rd->tgt, 0, sizeof rd->tgt);
}

static gboolean
on_fd(gint fd, GIOCondition cond, gpointer data)
{
    (void)fd;
    RtDev *rd = data;
    if (bk_rt_device_read(&rd->dev, on_contact, rd) && !(cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)))
        return G_SOURCE_CONTINUE;
    /* o dispositivo sumiu: os dedos dele já foram soltos pelo leitor */
    rd->src = 0;
    detach(rd);
    update_state(rd->r);
    return G_SOURCE_REMOVE;
}

static void
attach(BkRemote *r)
{
    gboolean exclusive = !strcmp(r->app->cfg.remote_touch, "exclusive");
    char **nodes = bk_rt_find_nodes();
    const char *problem = NULL;
    gboolean added = FALSE;
    for (int n = 0; nodes && nodes[n]; n++) {
        RtDev *free_rd = NULL;
        gboolean known = FALSE;
        for (int i = 0; i < RT_DEVS; i++) {
            if (r->rt[i].dev.fd >= 0 && !strcmp(r->rt[i].dev.node, nodes[n]))
                known = TRUE;
            else if (r->rt[i].dev.fd < 0 && !free_rd)
                free_rd = &r->rt[i];
        }
        if (known || !free_rd)
            continue;
        GError *err = NULL;
        if (!bk_rt_device_open(&free_rd->dev, nodes[n], exclusive, &err)) {
            gboolean denied = g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_ACCES) ||
                              g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_PERM);
            g_warning("toque remoto: %s", err->message);
            g_clear_error(&err);
            free_rd->dev.fd = -1;
            problem = denied ? "denied" : "error";
            continue;
        }
        free_rd->src = g_unix_fd_add(free_rd->dev.fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_fd, free_rd);
        added = TRUE;
        g_message("toque remoto: lendo %s%s", nodes[n],
                  !exclusive ? "" : free_rd->dev.grab == BKRT_GRAB_OK ? " (exclusivo)" : " (outro leitor o agarrou)");
    }
    g_strfreev(nodes);
    update_state(r);
    if (problem && !strcmp(r->state, "waiting"))
        set_state(r, problem);
    if (added) /* um celular chegou: o teclado aberto vai para o monitor dele */
        bk_osk_follow_remote(r->app->osk);
}

static gboolean
on_watch(gint fd, GIOCondition cond, gpointer data)
{
    (void)fd;
    (void)cond;
    BkRemote *r = data;
    if (bk_rt_watch_drain(r->watch))
        attach(r); /* um dispositivo novo; o que sumiu já avisou pelo HUP */
    return G_SOURCE_CONTINUE;
}

static void
start(BkRemote *r)
{
    r->running = TRUE;
    if (!r->watch && (r->watch = bk_rt_watch_new()))
        r->watch_src = g_unix_fd_add(bk_rt_watch_fd(r->watch), G_IO_IN, on_watch, r);
    attach(r);
}

static void
stop(BkRemote *r)
{
    r->running = FALSE;
    for (int i = 0; i < RT_DEVS; i++)
        detach(&r->rt[i]);
    bk_tm_cancel(&r->tm, &r->sink);
    if (r->watch_src) {
        g_source_remove(r->watch_src);
        r->watch_src = 0;
    }
    bk_rt_watch_free(r->watch);
    r->watch = NULL;
}

/* Lê o touchscreen enquanto o teclado ou o controle estiver aberto. */
static gboolean
on_sync(gpointer data)
{
    BkRemote *r = data;
    r->sync_id = 0;
    BkApp *app = r->app;
    gboolean off = !strcmp(app->cfg.remote_touch, "off");
    gboolean want = !off && (bk_osk_visible(app->osk) || bk_overlay_visible(app->overlay));
    if (want && !r->running)
        start(r);
    else if (!want && r->running)
        stop(r);
    if (!r->running)
        set_state(r, off ? "off" : "idle");
    return G_SOURCE_REMOVE;
}

void
bk_remote_sync(BkRemote *r)
{
    if (r && !r->sync_id)
        r->sync_id = g_idle_add(on_sync, r);
}

void
bk_remote_restart(BkRemote *r)
{
    if (r->running)
        stop(r);
    bk_remote_sync(r);
}

void
bk_remote_release(BkRemote *r)
{
    if (!r)
        return;
    for (int i = 0; i < RT_DEVS; i++)
        for (int s = 0; s < BKRT_SLOTS; s++)
            if (r->rt[i].dev.fd >= 0 && r->rt[i].dev.dec.id[s] >= 0) {
                r->rt[i].stale[s] = true;
                r->rt[i].tgt[s] = TGT_NONE;
            }
    bk_tm_cancel(&r->tm, &r->sink);
}

BkRemote *
bk_remote_new(BkApp *app)
{
    BkRemote *r = g_new0(BkRemote, 1);
    r->app = app;
    for (int i = 0; i < RT_DEVS; i++) {
        r->rt[i].r = r;
        r->rt[i].dev.fd = -1;
    }
    r->sink = (BkTmSink){ m_rel, m_abs, m_button, m_scroll, r };
    bk_tm_init(&r->tm, BKTM_TOUCHPAD, 1.5, 10);
    bk_remote_sync(r);
    return r;
}

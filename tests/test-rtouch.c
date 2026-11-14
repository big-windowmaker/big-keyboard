/* SPDX-License-Identifier: GPL-2.0-or-later
 * Toque remoto: o protocolo B do touchscreen do Sunshine (libvirtualhid)
 * até os botões do controle virtual, com vários dedos ao mesmo tempo.
 *
 * Os eventos são gerados como o libvirtualhid gera (uhid_backend.cpp,
 * place_touch_contact / release_touch_contact): SLOT, TRACKING_ID na
 * chegada, ABS_X + MT_X, ABS_Y + MT_Y, pressão, orientação e SYN_REPORT
 * por contato; na saída TRACKING_ID -1, BTN_TOUCH 0 no último e SYN. */
#include "bk-controls.h"
#include "bk-rtouch.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

/* ---- registro do decodificador --------------------------------------- */

typedef struct { int slot; BkCtlPhase ph; double x, y; } Ev;
typedef struct { Ev ev[256]; int n; } Log;
static void
log_fn(void *d, int slot, BkCtlPhase ph, double x, double y)
{
    Log *l = d;
    if (l->n < 256)
        l->ev[l->n++] = (Ev){ slot, ph, x, y };
}

/* ---- gerador no formato do libvirtualhid ----------------------------- */

#define MAXX 19200
#define MAXY 10800
typedef struct { BkRtDecoder dec; int cur_slot, next_id, ids[BKRT_SLOTS], active; BkRtFn fn; void *data; } Gen;

static void
put(Gen *g, int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    bk_rt_event(&g->dec, &e, g->fn, g->data);
}

static void
gen_init(Gen *g, BkRtFn fn, void *data)
{
    memset(g, 0, sizeof *g);
    bk_rt_init(&g->dec, 0, MAXX, 0, MAXY);
    g->cur_slot = 0;
    for (int i = 0; i < BKRT_SLOTS; i++)
        g->ids[i] = -1;
    g->fn = fn;
    g->data = data;
}

/* um dedo (slot) em nx, ny normalizados à imagem transmitida */
static void
place(Gen *g, int slot, double nx, double ny)
{
    if (g->cur_slot != slot) {
        put(g, EV_ABS, ABS_MT_SLOT, slot);
        g->cur_slot = slot;
    }
    if (g->ids[slot] < 0) {
        g->ids[slot] = g->next_id++;
        g->active++;
        put(g, EV_ABS, ABS_MT_TRACKING_ID, g->ids[slot]);
        put(g, EV_KEY, BTN_TOUCH, 1);
    }
    int x = (int)lround(nx * MAXX), y = (int)lround(ny * MAXY);
    put(g, EV_ABS, ABS_X, x);
    put(g, EV_ABS, ABS_MT_POSITION_X, x);
    put(g, EV_ABS, ABS_Y, y);
    put(g, EV_ABS, ABS_MT_POSITION_Y, y);
    put(g, EV_ABS, ABS_MT_PRESSURE, 120);
    put(g, EV_ABS, ABS_MT_ORIENTATION, 0);
    put(g, EV_SYN, SYN_REPORT, 0);
}

static void
lift(Gen *g, int slot)
{
    if (g->cur_slot != slot) {
        put(g, EV_ABS, ABS_MT_SLOT, slot);
        g->cur_slot = slot;
    }
    put(g, EV_ABS, ABS_MT_TRACKING_ID, -1);
    g->ids[slot] = -1;
    if (--g->active == 0)
        put(g, EV_KEY, BTN_TOUCH, 0);
    put(g, EV_SYN, SYN_REPORT, 0);
}

/* ---- gamepad registrado ---------------------------------------------- */

typedef struct { bool btn[0x300]; double sx[2], sy[2], trig[2]; int hx, hy, syncs; } Pad;
static void p_button(void *d, int c, bool down) { ((Pad *)d)->btn[c] = down; }
static void p_stick(void *d, int s, double x, double y) { ((Pad *)d)->sx[s] = x; ((Pad *)d)->sy[s] = y; }
static void p_trigger(void *d, int s, double v) { ((Pad *)d)->trig[s] = v; }
static void p_hat(void *d, int x, int y) { ((Pad *)d)->hx = x; ((Pad *)d)->hy = y; }
static void p_sync(void *d) { ((Pad *)d)->syncs++; }

/* a ligação que o overlay faz: slot → identidade do toque, normalizado → pixels */
typedef struct { BkCtlState st; BkCtlLayout lay; BkCtlSink sink; double w, h; char token[BKRT_SLOTS]; Pad pad; } Rig;
static void
rig_fn(void *d, int slot, BkCtlPhase ph, double nx, double ny)
{
    Rig *r = d;
    bk_ctl_touch(&r->st, &r->lay, r->w, r->h, &r->token[slot], ph, nx * r->w, ny * r->h, &r->sink);
}

static void
rig_init(Rig *r, double w, double h)
{
    memset(r, 0, sizeof *r);
    bk_ctl_default(&r->lay);
    bk_ctl_state_init(&r->st);
    r->sink = (BkCtlSink){ p_button, p_stick, p_trigger, p_hat, p_sync, &r->pad };
    r->w = w;
    r->h = h;
}

static int
find(const BkCtlLayout *l, BkCtlKind k, int code)
{
    for (int i = 0; i < l->n; i++)
        if (l->c[i].kind == k && l->c[i].code == code)
            return i;
    return -1;
}

/* o centro de um controle, normalizado (o que o celular envia ao tocá-lo) */
static void
at(const BkCtlLayout *l, int ci, double *nx, double *ny)
{
    *nx = l->c[ci].x;
    *ny = l->c[ci].y;
}

/* deslocado dx, dy raios do centro de um controle */
static void
off(const Rig *r, int ci, double dx, double dy, double *nx, double *ny)
{
    double rad = bk_ctl_px(&r->lay, &r->lay.c[ci], r->h) / 2;
    *nx = (r->lay.c[ci].x * r->w + dx * rad) / r->w;
    *ny = (r->lay.c[ci].y * r->h + dy * rad) / r->h;
}

static void
decoder_tests(void)
{
    Log lg;
    Gen g;
    memset(&lg, 0, sizeof lg);
    gen_init(&g, log_fn, &lg);
    place(&g, 0, 0.25, 0.5);
    CHECK(lg.n == 1 && lg.ev[0].ph == BKC_BEGIN && lg.ev[0].slot == 0, "um dedo: BEGIN");
    CHECK(fabs(lg.ev[0].x - 0.25) < 1e-4 && fabs(lg.ev[0].y - 0.5) < 1e-4, "posição normalizada %.4f %.4f",
          lg.ev[0].x, lg.ev[0].y);
    place(&g, 0, 0.26, 0.5);
    CHECK(lg.n == 2 && lg.ev[1].ph == BKC_MOVE, "MOVE");
    place(&g, 0, 0.26, 0.5);
    CHECK(lg.n == 2, "posição igual: nenhum MOVE");
    lift(&g, 0);
    CHECK(lg.n == 3 && lg.ev[2].ph == BKC_END && bk_rt_active(&g.dec) == 0, "END");

    /* cinco dedos, cada um com sua identidade (MT-08) */
    memset(&lg, 0, sizeof lg);
    for (int s = 0; s < 5; s++)
        place(&g, s, 0.1 + s * 0.2, 0.5);
    CHECK(lg.n == 5 && bk_rt_active(&g.dec) == 5, "cinco contatos: %d", bk_rt_active(&g.dec));
    for (int s = 0; s < 5; s++)
        CHECK(lg.ev[s].slot == s && lg.ev[s].ph == BKC_BEGIN, "contato %d", s);
    lift(&g, 2);
    CHECK(lg.ev[lg.n - 1].slot == 2 && lg.ev[lg.n - 1].ph == BKC_END && bk_rt_active(&g.dec) == 4,
          "só o dedo 3 sai");
    for (int s = 0; s < 5; s++)
        if (s != 2)
            lift(&g, s);
    CHECK(bk_rt_active(&g.dec) == 0, "todos saíram");

    /* mesmo slot, dedo novo no mesmo quadro: END do antigo antes do BEGIN */
    memset(&lg, 0, sizeof lg);
    place(&g, 0, 0.3, 0.3);
    put(&g, EV_ABS, ABS_MT_TRACKING_ID, 999);
    put(&g, EV_ABS, ABS_MT_POSITION_X, 100);
    put(&g, EV_SYN, SYN_REPORT, 0);
    CHECK(lg.n == 3 && lg.ev[1].ph == BKC_END && lg.ev[2].ph == BKC_BEGIN, "troca no slot: END, BEGIN");
    g.ids[0] = 999;
    lift(&g, 0);

    /* SYN_DROPPED: nada é aplicado até reler o estado do kernel */
    memset(&lg, 0, sizeof lg);
    place(&g, 1, 0.5, 0.5);
    put(&g, EV_SYN, SYN_DROPPED, 0);
    put(&g, EV_ABS, ABS_MT_POSITION_X, 0);
    put(&g, EV_SYN, SYN_REPORT, 0);
    CHECK(lg.n == 1 && g.dec.need_resync, "eventos perdidos: pede releitura");
    int ids[BKRT_SLOTS], xs[BKRT_SLOTS], ys[BKRT_SLOTS];
    for (int i = 0; i < BKRT_SLOTS; i++)
        ids[i] = -1, xs[i] = ys[i] = 0;
    ids[3] = 77, xs[3] = MAXX, ys[3] = 0; /* o dedo 1 saiu, outro chegou no slot 3 */
    bk_rt_resync(&g.dec, ids, xs, ys, BKRT_SLOTS, log_fn, &lg);
    CHECK(lg.n == 3 && lg.ev[1].ph == BKC_END && lg.ev[1].slot == 1 && lg.ev[2].ph == BKC_BEGIN &&
              lg.ev[2].slot == 3 && fabs(lg.ev[2].x - 1) < 1e-9,
          "releitura: END do que saiu, BEGIN do que chegou");
    bk_rt_cancel_all(&g.dec, log_fn, &lg);
    CHECK(lg.ev[lg.n - 1].ph == BKC_CANCEL && bk_rt_active(&g.dec) == 0, "cancelar tudo");

    /* slot fora do intervalo é ignorado, sem escrever fora do vetor */
    memset(&lg, 0, sizeof lg);
    put(&g, EV_ABS, ABS_MT_SLOT, 400);
    put(&g, EV_ABS, ABS_MT_TRACKING_ID, 5);
    put(&g, EV_SYN, SYN_REPORT, 0);
    CHECK(lg.n == 0, "slot inválido ignorado");
    put(&g, EV_ABS, ABS_MT_SLOT, 0);

    CHECK(bk_rt_is_remote_name("libvirtualhid Touchscreen"), "nome do Sunshine");
    CHECK(bk_rt_is_remote_name("Sunshine (libvirtualhid) Touchscreen"), "nome com prefixo");
    CHECK(!bk_rt_is_remote_name("libvirtualhid Mouse (Absolute)"), "mouse absoluto não");
    CHECK(!bk_rt_is_remote_name("Touch passthrough"), "inputtino (desktop inteiro) não");
    CHECK(!bk_rt_is_remote_name("ELAN Touchscreen"), "touchscreen físico não");
    CHECK(!bk_rt_is_remote_name(NULL), "sem nome");
}

/* Os cenários MT da missão, do evento do kernel ao botão do controle. */
static void
scenario_tests(double W, double H)
{
    Rig r;
    Gen g;
    rig_init(&r, W, H);
    gen_init(&g, rig_fn, &r);
    const BkCtlLayout *l = &r.lay;
    int A = find(l, BKC_BUTTON, BTN_SOUTH), B = find(l, BKC_BUTTON, BTN_EAST), X = find(l, BKC_BUTTON, BTN_WEST),
        Y = find(l, BKC_BUTTON, BTN_NORTH), DP = find(l, BKC_DPAD, 0), LS = find(l, BKC_STICK, 0),
        RS = find(l, BKC_STICK, 1), RT = find(l, BKC_TRIGGER, 1), LT = find(l, BKC_TRIGGER, 0),
        LB = find(l, BKC_BUTTON, BTN_TL);
    double nx, ny;

    /* MT-01 */
    at(l, A, &nx, &ny);
    place(&g, 0, nx, ny);
    CHECK(r.pad.btn[BTN_SOUTH], "MT-01 %gx%g: A", W, H);
    /* MT-02 */
    at(l, B, &nx, &ny);
    place(&g, 1, nx, ny);
    CHECK(r.pad.btn[BTN_SOUTH] && r.pad.btn[BTN_EAST], "MT-02: A+B");
    /* MT-07: três botões */
    at(l, Y, &nx, &ny);
    place(&g, 2, nx, ny);
    CHECK(r.pad.btn[BTN_SOUTH] && r.pad.btn[BTN_EAST] && r.pad.btn[BTN_NORTH], "MT-07: A+B+Y");
    /* MT-09: só o A sai */
    lift(&g, 0);
    CHECK(!r.pad.btn[BTN_SOUTH] && r.pad.btn[BTN_EAST] && r.pad.btn[BTN_NORTH], "MT-09: A solto, B e Y seguem");
    lift(&g, 1);
    lift(&g, 2);

    /* MT-03: direita no direcional + A; o cenário principal da missão */
    off(&r, DP, 0.8, 0, &nx, &ny);
    place(&g, 0, nx, ny);
    at(l, A, &nx, &ny);
    place(&g, 1, nx, ny);
    off(&r, DP, 0, 0, &nx, &ny);
    at(l, B, &nx, &ny);
    place(&g, 2, nx, ny);
    CHECK(r.pad.hx == 1 && r.pad.hy == 0 && r.pad.btn[BTN_SOUTH] && r.pad.btn[BTN_EAST], "MT-03: DIREITA + A + B");
    lift(&g, 1);
    CHECK(r.pad.hx == 1 && !r.pad.btn[BTN_SOUTH] && r.pad.btn[BTN_EAST], "dedo 2 sai: DIREITA, B; A solto");
    lift(&g, 0);
    CHECK(r.pad.hx == 0 && !r.pad.btn[BTN_SOUTH] && r.pad.btn[BTN_EAST], "dedo 1 sai: B segue sozinho");
    lift(&g, 2);
    CHECK(!r.pad.btn[BTN_EAST], "todos soltos");

    /* MT-04: diagonal (baixo-direita) + B */
    off(&r, DP, 0.6, 0.6, &nx, &ny);
    place(&g, 0, nx, ny);
    at(l, B, &nx, &ny);
    place(&g, 1, nx, ny);
    CHECK(r.pad.hx == 1 && r.pad.hy == 1 && r.pad.btn[BTN_EAST], "MT-04: diagonal %d,%d + B", r.pad.hx, r.pad.hy);
    /* o dedo do direcional gira para cima-esquerda sem soltar */
    off(&r, DP, -0.6, -0.6, &nx, &ny);
    place(&g, 0, nx, ny);
    CHECK(r.pad.hx == -1 && r.pad.hy == -1 && r.pad.btn[BTN_EAST], "diagonal oposta, B segue");
    lift(&g, 0);
    lift(&g, 1);

    /* MT-05 e MT-10: dois analógicos, independentes e contínuos */
    off(&r, LS, 0, 0, &nx, &ny);
    place(&g, 0, nx, ny);
    off(&r, RS, 0, 0, &nx, &ny);
    place(&g, 1, nx, ny);
    double prev = 0;
    bool mono = true;
    for (int i = 1; i <= 10; i++) {
        off(&r, LS, i / 10.0, 0, &nx, &ny);
        place(&g, 0, nx, ny);
        mono &= r.pad.sx[0] >= prev - 1e-9;
        prev = r.pad.sx[0];
        CHECK(r.pad.sx[1] == 0 && r.pad.sy[1] == 0, "MT-05: o direito parado enquanto o esquerdo anda");
    }
    CHECK(mono && fabs(r.pad.sx[0] - 1) < 2e-3, "MT-10: esquerdo contínuo até 1 (%.4f)", r.pad.sx[0]);
    off(&r, RS, 0, -1, &nx, &ny);
    place(&g, 1, nx, ny);
    CHECK(fabs(r.pad.sy[1] + 1) < 2e-3 && fabs(r.pad.sx[0] - 1) < 2e-3, "MT-05: direito em cima, esquerdo à direita");
    lift(&g, 0);
    CHECK(r.pad.sx[0] == 0 && fabs(r.pad.sy[1] + 1) < 2e-3, "esquerdo volta ao centro, direito segue");
    lift(&g, 1);
    CHECK(r.pad.sy[1] == 0, "direito ao centro");

    /* MT-06: analógico + RT; e LT + RT + LB + X (dois gatilhos e botões) */
    off(&r, LS, -1, 0, &nx, &ny);
    place(&g, 0, nx, ny);
    at(l, RT, &nx, &ny);
    place(&g, 1, nx, ny);
    CHECK(fabs(r.pad.sx[0] + 1) < 2e-3 && r.pad.trig[1] == 1, "MT-06: analógico + RT");
    at(l, LT, &nx, &ny);
    place(&g, 2, nx, ny);
    at(l, LB, &nx, &ny);
    place(&g, 3, nx, ny);
    at(l, X, &nx, &ny);
    place(&g, 4, nx, ny);
    CHECK(r.pad.trig[0] == 1 && r.pad.trig[1] == 1 && r.pad.btn[BTN_TL] && r.pad.btn[BTN_WEST],
          "MT-08: cinco dedos, cinco controles");
    /* um dedo que escorrega para outro controle não o aciona */
    at(l, Y, &nx, &ny);
    place(&g, 4, nx, ny);
    CHECK(r.pad.btn[BTN_WEST] && !r.pad.btn[BTN_NORTH], "escorregar não troca de controle");
    /* MT-11/MT-19: a sessão cai com tudo apertado → o dispositivo some */
    bk_rt_cancel_all(&g.dec, rig_fn, &r);
    CHECK(r.pad.trig[0] == 0 && r.pad.trig[1] == 0 && !r.pad.btn[BTN_TL] && !r.pad.btn[BTN_WEST] &&
              r.pad.sx[0] == 0,
          "MT-11: queda solta botões, zera gatilhos e centra analógicos");
    for (int i = 0; i < BKC_TOUCHES; i++)
        CHECK(!r.st.used[i], "nenhum toque preso (%d)", i);

    /* fora dos controles: nada acontece (nada de mouse) */
    int syncs = r.pad.syncs;
    for (int s = 0; s < 5; s++)
        g.ids[s] = -1;
    g.active = 0;
    place(&g, 0, 0.5, 0.5);
    lift(&g, 0);
    CHECK(r.pad.syncs == syncs, "toque no meio da tela não aciona nada");
}

/* Precisão: o centro de cada controle, tocado na imagem, cai nele (≤ 1 px). */
static void
mapping_tests(void)
{
    const double sizes[][2] = { { 3440, 1440 }, { 2560, 1080 }, { 1920, 1080 }, { 1080, 1920 }, { 2560, 1600 } };
    for (size_t k = 0; k < G_N_ELEMENTS(sizes); k++) {
        double W = sizes[k][0], H = sizes[k][1];
        BkCtlLayout l;
        bk_ctl_default(&l);
        for (int i = 0; i < l.n; i++) {
            int dx = (int)lround(l.c[i].x * MAXX), dy = (int)lround(l.c[i].y * MAXY);
            double px = (double)dx / MAXX * W, py = (double)dy / MAXY * H;
            CHECK(fabs(px - l.c[i].x * W) <= 1 && fabs(py - l.c[i].y * H) <= 1, "%gx%g controle %d: erro %.2f px",
                  W, H, i, hypot(px - l.c[i].x * W, py - l.c[i].y * H));
            CHECK(bk_ctl_hit(&l, W, H, px, py) == i, "%gx%g: o toque no centro acerta o controle %d", W, H, i);
        }
    }
}

int
main(void)
{
    decoder_tests();
    scenario_tests(1920, 1080);
    scenario_tests(3440, 1440);
    scenario_tests(2560, 1080);
    mapping_tests();
    if (fails)
        fprintf(stderr, "%d falha(s)\n", fails);
    return fails ? 1 : 0;
}

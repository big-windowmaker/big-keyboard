/* SPDX-License-Identifier: GPL-2.0-or-later
 * Controle na tela: layout e toques (veja bk-controls.h). */
#include "bk-controls.h"

#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>

#define B(code_, x_, y_, s_) { BKC_BUTTON, code_, x_, y_, s_, 0, 1, true }

void
bk_ctl_default(BkCtlLayout *l)
{
    memset(l, 0, sizeof *l);
    g_strlcpy(l->name, "padrão", sizeof l->name);
    l->opacity = 0.55;
    l->scale = 1.0;
    /* como num controle: analógico e direcional à esquerda, botões e o
     * analógico direito à direita, ombros em cima, Start/Select no meio */
    const BkCtl def[] = {
        { BKC_STICK, 0, 0.13, 0.62, 0.30, 0.15, 1.0, true },
        { BKC_DPAD, 0, 0.26, 0.84, 0.22, 0.30, 1.0, true },
        { BKC_STICK, 1, 0.74, 0.84, 0.24, 0.15, 1.0, true },
        B(BTN_SOUTH, 0.88, 0.70, 0.11), B(BTN_EAST, 0.95, 0.58, 0.11), B(BTN_WEST, 0.81, 0.58, 0.11),
        B(BTN_NORTH, 0.88, 0.46, 0.11),
        B(BTN_TL, 0.08, 0.22, 0.10), B(BTN_TR, 0.92, 0.22, 0.10),
        { BKC_TRIGGER, 0, 0.08, 0.08, 0.10, 0, 1, true },
        { BKC_TRIGGER, 1, 0.92, 0.08, 0.10, 0, 1, true },
        B(BTN_SELECT, 0.43, 0.10, 0.08), B(BTN_START, 0.57, 0.10, 0.08),
        B(BTN_THUMBL, 0.20, 0.42, 0.07), B(BTN_THUMBR, 0.66, 0.66, 0.07),
    };
    l->n = (int)G_N_ELEMENTS(def);
    memcpy(l->c, def, sizeof def);
}

void
bk_ctl_switch_button(double w, double h, double *cx, double *cy, double *r)
{
    double d = MAX(44.0, h * 0.085); /* 44 px: o mínimo confortável para um dedo */
    *r = d / 2;
    *cx = w / 2;
    *cy = MAX(*r + 4, h * 0.06);
}

bool
bk_ctl_switch_hit(double w, double h, double px, double py)
{
    double cx, cy, r;
    bk_ctl_switch_button(w, h, &cx, &cy, &r);
    return hypot(px - cx, py - cy) <= r * 1.15;
}

double
bk_ctl_px(const BkCtlLayout *l, const BkCtl *c, double h)
{
    return c->size * h * l->scale;
}

int
bk_ctl_hit(const BkCtlLayout *l, double w, double h, double px, double py)
{
    int best = -1;
    double bd = 0;
    for (int i = 0; i < l->n; i++) {
        const BkCtl *c = &l->c[i];
        if (!c->enabled)
            continue;
        double r = bk_ctl_px(l, c, h) / 2, dx = px - c->x * w, dy = py - c->y * h;
        double d = hypot(dx, dy);
        /* um pouco de folga além do círculo (dedos), o mais próximo vence */
        if (d <= r * 1.15 && (best < 0 || d < bd)) {
            best = i;
            bd = d;
        }
    }
    return best;
}

void
bk_ctl_state_init(BkCtlState *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < BKC_TOUCHES; i++)
        s->ctl[i] = -1;
}

/* Analógico: vetor do centro ao dedo, normalizado pelo raio; zona morta
 * reescalada (sem salto ao sair dela) e sensibilidade. */
static void
stick_vec(const BkCtlLayout *l, const BkCtl *c, double w, double h, double px, double py, double *ox, double *oy)
{
    double r = bk_ctl_px(l, c, h) / 2;
    double dx = (px - c->x * w) / r, dy = (py - c->y * h) / r;
    double m = hypot(dx, dy);
    if (m < c->deadzone || m == 0) {
        *ox = *oy = 0;
        return;
    }
    double k = (m - c->deadzone) / (1 - c->deadzone) * c->sens;
    if (k > 1)
        k = 1;
    *ox = dx / m * k;
    *oy = dy / m * k;
}

static void
dpad_dir(const BkCtlLayout *l, const BkCtl *c, double w, double h, double px, double py, int *hx, int *hy)
{
    double r = bk_ctl_px(l, c, h) / 2;
    double dx = (px - c->x * w) / r, dy = (py - c->y * h) / r;
    *hx = *hy = 0;
    if (hypot(dx, dy) < c->deadzone)
        return;
    /* 8 direções: setores de 45° (diagonais apertam as duas) */
    double a = atan2(dy, dx) * 180 / M_PI; /* 0 = direita, 90 = baixo */
    int sector = (int)lround(a / 45.0);    /* -4..4 */
    static const int sx[9] = { -1, -1, 0, 1, 1, 1, 0, -1, -1 }, sy[9] = { 0, -1, -1, -1, 0, 1, 1, 1, 0 };
    *hx = sx[sector + 4];
    *hy = sy[sector + 4];
}

static void
apply(const BkCtlLayout *l, int ci, double w, double h, double px, double py, bool down, const BkCtlSink *k)
{
    const BkCtl *c = &l->c[ci];
    switch (c->kind) {
    case BKC_BUTTON:
        k->button(k->data, c->code, down);
        break;
    case BKC_TRIGGER:
        k->trigger(k->data, c->code, down ? 1.0 : 0.0);
        break;
    case BKC_STICK: {
        double x = 0, y = 0;
        if (down)
            stick_vec(l, c, w, h, px, py, &x, &y);
        k->stick(k->data, c->code, x, y);
        break;
    }
    case BKC_DPAD: {
        int hx = 0, hy = 0;
        if (down)
            dpad_dir(l, c, w, h, px, py, &hx, &hy);
        k->hat(k->data, hx, hy);
        break;
    }
    }
}

void
bk_ctl_touch(BkCtlState *s, const BkCtlLayout *l, double w, double h, const void *seq, BkCtlPhase phase,
             double px, double py, const BkCtlSink *sink)
{
    int t = -1;
    for (int i = 0; i < BKC_TOUCHES; i++)
        if (s->used[i] && s->seq[i] == seq)
            t = i;
    if (phase == BKC_BEGIN) {
        if (t >= 0)
            return;
        int ci = bk_ctl_hit(l, w, h, px, py);
        if (ci < 0)
            return;
        for (int i = 0; i < BKC_TOUCHES; i++)
            if (s->used[i] && s->ctl[i] == ci)
                return; /* um segundo dedo no mesmo controle não o rouba */
        for (int i = 0; i < BKC_TOUCHES && t < 0; i++)
            if (!s->used[i])
                t = i;
        if (t < 0)
            return;
        s->used[t] = true;
        s->seq[t] = seq;
        s->ctl[t] = ci;
        apply(l, ci, w, h, px, py, true, sink);
    } else if (t >= 0 && phase == BKC_MOVE) {
        const BkCtl *c = &l->c[s->ctl[t]];
        if (c->kind == BKC_STICK || c->kind == BKC_DPAD)
            apply(l, s->ctl[t], w, h, px, py, true, sink);
    } else if (t >= 0) { /* END, CANCEL: solta e o analógico volta ao centro */
        apply(l, s->ctl[t], w, h, px, py, false, sink);
        s->used[t] = false;
        s->ctl[t] = -1;
    } else {
        return;
    }
    sink->sync(sink->data);
}

void
bk_ctl_release(BkCtlState *s, const BkCtlLayout *l, const BkCtlSink *sink)
{
    for (int i = 0; i < BKC_TOUCHES; i++)
        if (s->used[i]) {
            apply(l, s->ctl[i], 1, 1, 0, 0, false, sink);
            s->used[i] = false;
            s->ctl[i] = -1;
        }
    sink->sync(sink->data);
}

bool
bk_ctl_clamp(BkCtl *c, double w, double h, double scale)
{
    double r = c->size * h * scale / 2;
    double minx = r / w, maxx = 1 - r / w, miny = r / h, maxy = 1 - r / h;
    double x = c->x < minx ? minx : c->x > maxx ? maxx : c->x;
    double y = c->y < miny ? miny : c->y > maxy ? maxy : c->y;
    bool moved = fabs(x - c->x) > 1e-9 || fabs(y - c->y) > 1e-9;
    c->x = x;
    c->y = y;
    return moved;
}

const char *
bk_ctl_label(const BkCtl *c)
{
    switch (c->kind) {
    case BKC_STICK:
        return c->code ? "R" : "L";
    case BKC_DPAD:
        return "✚";
    case BKC_TRIGGER:
        return c->code ? "RT" : "LT";
    case BKC_BUTTON:
        break;
    }
    switch (c->code) {
    case BTN_SOUTH:
        return "A";
    case BTN_EAST:
        return "B";
    case BTN_WEST:
        return "X";
    case BTN_NORTH:
        return "Y";
    case BTN_TL:
        return "LB";
    case BTN_TR:
        return "RB";
    case BTN_SELECT:
        return "⧉";
    case BTN_START:
        return "☰";
    case BTN_THUMBL:
        return "L3";
    case BTN_THUMBR:
        return "R3";
    case BTN_MODE:
        return "◉";
    default:
        return "?";
    }
}

/* ---- arquivo ------------------------------------------------------------- */

char *
bk_ctl_path(const char *name)
{
    char *safe = g_strdup(name && *name ? name : "padrão");
    g_strdelimit(safe, "/\\:", '-');
    char *file = g_strconcat(safe, ".ini", NULL);
    char *path = g_build_filename(g_get_user_data_dir(), "big-keyboard", "gamepad-layouts", file, NULL);
    g_free(file);
    g_free(safe);
    return path;
}

static const char *kinds[] = { "stick", "dpad", "button", "trigger" };

gboolean
bk_ctl_save(const BkCtlLayout *l, const char *path, GError **err)
{
    GKeyFile *k = g_key_file_new();
    g_key_file_set_integer(k, "Layout", "Version", 1);
    g_key_file_set_string(k, "Layout", "Name", l->name);
    g_key_file_set_double(k, "Layout", "Opacity", l->opacity);
    g_key_file_set_double(k, "Layout", "Scale", l->scale);
    for (int i = 0; i < l->n; i++) {
        char g[32];
        g_snprintf(g, sizeof g, "Control %02d", i);
        const BkCtl *c = &l->c[i];
        g_key_file_set_string(k, g, "Kind", kinds[c->kind]);
        g_key_file_set_integer(k, g, "Code", c->code);
        g_key_file_set_double(k, g, "X", c->x);
        g_key_file_set_double(k, g, "Y", c->y);
        g_key_file_set_double(k, g, "Size", c->size);
        g_key_file_set_double(k, g, "Deadzone", c->deadzone);
        g_key_file_set_double(k, g, "Sensitivity", c->sens);
        g_key_file_set_boolean(k, g, "Enabled", c->enabled);
    }
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    gboolean ok = g_key_file_save_to_file(k, path, err); /* atômico */
    g_key_file_free(k);
    return ok;
}

static double
clampd(double v, double lo, double hi)
{
    return isnan(v) ? lo : v < lo ? lo : v > hi ? hi : v;
}

gboolean
bk_ctl_load(BkCtlLayout *l, const char *path, GError **err)
{
    GKeyFile *k = g_key_file_new();
    if (!g_key_file_load_from_file(k, path, G_KEY_FILE_NONE, err)) {
        g_key_file_free(k);
        return FALSE;
    }
    BkCtlLayout tmp;
    memset(&tmp, 0, sizeof tmp);
    char *name = g_key_file_get_string(k, "Layout", "Name", NULL);
    g_strlcpy(tmp.name, name ? name : "importado", sizeof tmp.name);
    g_free(name);
    tmp.opacity = clampd(g_key_file_get_double(k, "Layout", "Opacity", NULL), 0.15, 1.0);
    tmp.scale = clampd(g_key_file_get_double(k, "Layout", "Scale", NULL), 0.5, 2.0);
    gsize ngroups = 0;
    char **groups = g_key_file_get_groups(k, &ngroups);
    for (gsize i = 0; i < ngroups && tmp.n < BKC_MAX; i++) {
        if (!g_str_has_prefix(groups[i], "Control "))
            continue;
        char *kind = g_key_file_get_string(k, groups[i], "Kind", NULL);
        int ki = -1;
        for (int j = 0; kind && j < 4; j++)
            if (!strcmp(kind, kinds[j]))
                ki = j;
        g_free(kind);
        if (ki < 0)
            continue; /* controle desconhecido: ignorado, não derruba o layout */
        BkCtl *c = &tmp.c[tmp.n++];
        c->kind = (BkCtlKind)ki;
        c->code = g_key_file_get_integer(k, groups[i], "Code", NULL);
        if ((c->kind == BKC_STICK || c->kind == BKC_TRIGGER) && c->code != 1)
            c->code = 0;
        c->x = clampd(g_key_file_get_double(k, groups[i], "X", NULL), 0, 1);
        c->y = clampd(g_key_file_get_double(k, groups[i], "Y", NULL), 0, 1);
        c->size = clampd(g_key_file_get_double(k, groups[i], "Size", NULL), 0.04, 0.6);
        c->deadzone = clampd(g_key_file_get_double(k, groups[i], "Deadzone", NULL), 0, 0.8);
        c->sens = clampd(g_key_file_get_double(k, groups[i], "Sensitivity", NULL), 0.3, 3);
        GError *e = NULL;
        c->enabled = g_key_file_get_boolean(k, groups[i], "Enabled", &e);
        if (e) {
            c->enabled = true;
            g_error_free(e);
        }
    }
    g_strfreev(groups);
    g_key_file_free(k);
    if (tmp.n == 0) {
        g_set_error(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE, "nenhum controle no layout");
        return FALSE;
    }
    *l = tmp;
    return TRUE;
}

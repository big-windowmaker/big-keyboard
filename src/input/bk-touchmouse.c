/* SPDX-License-Identifier: GPL-2.0-or-later
 * Mouse pelo toque (veja bk-touchmouse.h). */
#include "bk-touchmouse.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>

void
bk_tm_init(BkTouchMouse *m, BkTmMode mode, double speed, double slop)
{
    memset(m, 0, sizeof *m);
    m->mode = mode;
    m->speed = speed;
    m->slop = slop;
}

static int
find(const BkTouchMouse *m, const void *id)
{
    for (int i = 0; i < m->n; i++)
        if (m->f[i].id == id)
            return i;
    return -1;
}

static void
click(const BkTmSink *s, int button)
{
    s->button(s->data, button, true);
    s->button(s->data, button, false);
}

static void
release_left(BkTouchMouse *m, const BkTmSink *s)
{
    if (m->left_down) {
        s->button(s->data, BTN_LEFT, false);
        m->left_down = false;
    }
}

static void
begin(BkTouchMouse *m, const void *id, double x, double y, uint64_t now, const BkTmSink *s)
{
    if (m->n >= BKTM_FINGERS || find(m, id) >= 0)
        return;
    if (m->n == 0) {
        m->right_done = m->scrolling = m->consumed = false;
        if (m->mode == BKTM_DIRECT)
            s->move_abs(s->data, x, y);
    }
    m->f[m->n++] = (BkTmFinger){ id, x, y, x, y, now, false };
}

static void
move(BkTouchMouse *m, int i, double x, double y, uint64_t now, const BkTmSink *s)
{
    BkTmFinger *f = &m->f[i];
    double dx = x - f->x, dy = y - f->y;
    bool started = false;
    if (!f->moved && hypot(x - f->x0, y - f->y0) > m->slop) {
        f->moved = true;
        started = true;
        dx = x - f->x0; /* o caminho guardado até sair da folga */
        dy = y - f->y0;
    }
    f->x = x;
    f->y = y;
    if (!f->moved)
        return;
    if (m->n >= 2) {
        /* dois dedos: rolagem (a média dos dois; o conteúdo segue os dedos) */
        if (!m->left_down) {
            m->scrolling = m->consumed = true;
            s->scroll(s->data, -dx / m->n, -dy / m->n);
        }
        return;
    }
    if (m->scrolling || m->right_done)
        return; /* sobra de um gesto de dois dedos */
    if (m->mode == BKTM_TOUCHPAD) {
        if (started && !m->left_down && now - f->t0 >= BKTM_HOLD_MS) {
            s->button(s->data, BTN_LEFT, true); /* segurou parado e moveu: arrasta */
            m->left_down = m->consumed = true;
        }
        s->move_rel(s->data, dx * m->speed, dy * m->speed);
    } else {
        if (!m->left_down) {
            s->button(s->data, BTN_LEFT, true); /* aperta onde o dedo tocou */
            m->left_down = m->consumed = true;
        }
        s->move_abs(s->data, x, y);
    }
}

static void
end(BkTouchMouse *m, int i, bool cancel, uint64_t now, const BkTmSink *s)
{
    BkTmFinger f = m->f[i];
    memmove(&m->f[i], &m->f[i + 1], (size_t)(m->n - i - 1) * sizeof m->f[0]);
    m->n--;
    if (m->n > 0) {
        /* sai um dedo e outro fica: o segundo dedo num toque rápido é o
         * clique direito; o primeiro saindo antes encerra o gesto */
        if (i > 0 && !cancel && !f.moved && !m->scrolling && !m->left_down) {
            click(s, BTN_RIGHT);
            m->right_done = true;
        }
        m->consumed = true;
        return;
    }
    if (m->left_down) {
        release_left(m, s);
        return;
    }
    if (cancel || f.moved || m->consumed || m->right_done)
        return;
    if (m->mode == BKTM_DIRECT || now - f.t0 <= BKTM_TAP_MS)
        click(s, BTN_LEFT);
}

void
bk_tm_touch(BkTouchMouse *m, const void *id, BkCtlPhase phase, double x, double y, uint64_t now,
            const BkTmSink *sink)
{
    if (phase == BKC_BEGIN) {
        begin(m, id, x, y, now, sink);
        return;
    }
    int i = find(m, id);
    if (i < 0)
        return;
    if (phase == BKC_MOVE)
        move(m, i, x, y, now, sink);
    else
        end(m, i, phase == BKC_CANCEL, now, sink);
}

void
bk_tm_cancel(BkTouchMouse *m, const BkTmSink *sink)
{
    release_left(m, sink);
    m->n = 0;
    m->scrolling = m->consumed = m->right_done = false;
}

/* ---- quando o mouse age ------------------------------------------------ */

bool
bk_tm_reads(const char *remote_touch, bool keyboard, bool controller, bool pointer)
{
    if (!strcmp(remote_touch, "off"))
        return false;
    return keyboard || controller || (!strcmp(remote_touch, "mouse") && pointer);
}

bool
bk_tm_exclusive(const char *remote_touch)
{
    return !strcmp(remote_touch, "exclusive") || !strcmp(remote_touch, "mouse");
}

int
bk_tm_mode_for(const char *remote_touch, const char *remote_mouse, bool grabbed, bool keyboard, bool controller,
               bool pointer)
{
    if (!strcmp(remote_mouse, "off") || !grabbed || !pointer)
        return -1;
    if (keyboard) {
        if (!strcmp(remote_mouse, "touchpad"))
            return BKTM_TOUCHPAD;
        if (!strcmp(remote_mouse, "direct"))
            return BKTM_DIRECT;
        return controller ? BKTM_DIRECT : BKTM_TOUCHPAD;
    }
    /* só o controle aberto: tocar fora dos controles não faz nada (no meio
     * do jogo, um clique perdido atrapalha) */
    if (controller || strcmp(remote_touch, "mouse"))
        return -1;
    /* nada aberto: a tela inteira é o mouse, como uma tela de toque */
    return !strcmp(remote_mouse, "touchpad") ? BKTM_TOUCHPAD : BKTM_DIRECT;
}

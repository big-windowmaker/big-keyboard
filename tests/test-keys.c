/* SPDX-License-Identifier: GPL-2.0-or-later
 * O núcleo de teclas sem X11 nem Wayland: níveis, modificadores, mortas. */
#include "bk-keys.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...)                                                                                                   \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FALHOU %s:%d: ", __FILE__, __LINE__);                                                      \
            fprintf(stderr, __VA_ARGS__);                                                                               \
            fputc('\n', stderr);                                                                                        \
            fails++;                                                                                                    \
        }                                                                                                               \
    } while (0)

/* A tecla do painel principal cujo nível 0 é t (ou o keysym/rótulo). */
static const BkKey *
find(const BkState *s, const char *t)
{
    const BkKey(*rows)[BK_MAX_KEYS] = bk_panel_rows(bk_layout_get(s->layout), s->panel);
    for (int r = 0; r < BK_MAX_ROWS; r++)
        for (int c = 0; rows[r][c].width > 0; c++) {
            const BkKey *k = &rows[r][c];
            if ((k->level[0] && !strcmp(k->level[0], t)) || (k->keysym && !strcmp(k->keysym, t)))
                return k;
        }
    return NULL;
}

/* Digita uma sequência de rótulos e junta o texto produzido. */
static void
type(BkState *s, const char *const *keys, char *buf, size_t len)
{
    buf[0] = 0;
    for (int i = 0; keys[i]; i++) {
        const BkKey *k = find(s, keys[i]);
        if (!k) {
            fprintf(stderr, "tecla não encontrada: %s\n", keys[i]);
            fails++;
            continue;
        }
        BkOut out[BK_MAX_OUT];
        int n = bk_state_press(s, k, out, BK_MAX_OUT);
        for (int j = 0; j < n; j++)
            if (out[j].kind == BK_OUT_TEXT)
                strncat(buf, out[j].text, len - strlen(buf) - 1);
            else {
                char tmp[64];
                snprintf(tmp, sizeof tmp, "<%s%s%s%s>", out[j].mods & BK_MOD_CTRL ? "C-" : "",
                         out[j].mods & BK_MOD_ALT ? "A-" : "", out[j].mods & BK_MOD_SHIFT ? "S-" : "", out[j].keysym);
                strncat(buf, tmp, len - strlen(buf) - 1);
            }
    }
}

int
main(void)
{
    BkState s;
    char buf[256];
    bk_state_init(&s, 0);
    CHECK(!strcmp(bk_layout_get(0)->id, "br") && bk_layout_by_id("us-intl"), "layouts");

    /* ABNT2: Shift vale para uma tecla; Caps para letras */
    type(&s, (const char *[]){ "⇧", "o", "l", "a", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "Ola"), "Shift de uma tecla: %s", buf);
    type(&s, (const char *[]){ "Caps", "a", "1", "ç", "Caps", "a", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "A1Ça"), "Caps: %s", buf);
    /* dois toques travam o Shift, o terceiro solta */
    type(&s, (const char *[]){ "⇧", "⇧", "a", "b", "⇧", "c", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "ABc"), "Shift travado: %s", buf);
    CHECK(bk_state_mods(&s) == 0, "nada preso depois: %u", bk_state_mods(&s));

    /* teclas mortas do ABNT2 */
    type(&s, (const char *[]){ "´", "a", "~", "o", "´", "c", "´", " ", "~", "x", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "áõç´~x"), "mortas: %s", buf);
    type(&s, (const char *[]){ "⇧", "´", "a", "⇧", "~", "e", "⇧", "6", "u", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "àêü"), "mortas com Shift: %s", buf);

    /* AltGr */
    type(&s, (const char *[]){ "AltGr", "q", "AltGr", "e", "AltGr", "2", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "/€²"), "AltGr: %s", buf);

    /* atalhos: a tecla de base com os modificadores, sem texto */
    type(&s, (const char *[]){ "Ctrl", "c", "Ctrl", "⇧", "t", "Alt", "Tab", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "<C-c><C-S-t><A-Tab>"), "atalhos: %s", buf);

    /* especiais e a morta pendente */
    type(&s, (const char *[]){ "´", "Return", "BackSpace", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "´<Return><BackSpace>"), "especiais: %s", buf);
    type(&s, (const char *[]){ "´", "BackSpace", "e", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "<BackSpace>e"), "Backspace descarta a morta: %s", buf);

    /* layout US internacional */
    s.layout = 2;
    type(&s, (const char *[]){ "'", "a", "⇧", "'", "u", "AltGr", "n", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "áüñ"), "US-intl: %s", buf);
    /* a tecla de layout gira */
    const BkKey *lk = find(&s, "");
    CHECK(lk && lk->kind == BK_KEY_LAYOUT, "tecla de layout");
    BkOut o[BK_MAX_OUT];
    bk_state_press(&s, lk, o, BK_MAX_OUT);
    CHECK(s.layout == 0 && !strcmp(bk_state_label(&s, lk), "BR"), "layout volta ao BR");

    /* painéis: Fn e símbolos */
    s.panel = BK_PANEL_FN;
    type(&s, (const char *[]){ "F5", "Ctrl", "Home", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "<F5><C-Home>"), "Fn: %s", buf);
    s.panel = BK_PANEL_SYMBOLS;
    type(&s, (const char *[]){ "😀", "€", NULL }, buf, sizeof buf);
    CHECK(!strcmp(buf, "😀€"), "símbolos: %s", buf);

    /* emergência: nada preso */
    s.locked = BK_MOD_CTRL | BK_MOD_SHIFT;
    snprintf(s.dead, sizeof s.dead, "´");
    bk_state_release_all(&s);
    CHECK(bk_state_mods(&s) == 0 && !s.dead[0], "soltar tudo");

    /* todas as fileiras cabem e todo caractere tem texto */
    for (int l = 0; l < bk_layout_count(); l++)
        for (int p = 0; p < BK_PANEL_N; p++) {
            const BkKey(*rows)[BK_MAX_KEYS] = bk_panel_rows(bk_layout_get(l), (BkPanel)p);
            for (int r = 0; r < BK_MAX_ROWS; r++) {
                float w = 0;
                for (int c = 0; rows[r][c].width > 0; c++) {
                    w += rows[r][c].width;
                    CHECK(rows[r][c].level[0] || rows[r][c].kind == BK_KEY_LAYOUT, "tecla sem rótulo");
                }
                CHECK(w <= 16.5f, "fileira larga demais: layout %d painel %d fileira %d (%.1f)", l, p, r, w);
            }
        }

    if (!fails)
        printf("bk-keys: todos os casos passaram\n");
    return fails ? 1 : 0;
}

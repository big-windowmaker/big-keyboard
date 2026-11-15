/* SPDX-License-Identifier: GPL-2.0-or-later
 * Preferências (veja bk-config.h). */
#include "bk-config.h"

#include <glib/gstdio.h>
#include <string.h>

void
bk_config_defaults(BkConfig *c)
{
    memset(c, 0, sizeof *c);
    g_strlcpy(c->display_mode, "independent", sizeof c->display_mode);
    g_strlcpy(c->layout, "auto", sizeof c->layout);
    g_strlcpy(c->theme, "auto", sizeof c->theme);
    c->scale = 1.0;
    c->opacity = 1.0;
    c->reserve = TRUE;
    c->autohide = FALSE;
    c->repeat_delay = 500;
    c->repeat_rate = 60;
    g_strlcpy(c->window_mode, "floating", sizeof c->window_mode);
    g_strlcpy(c->stacking, "above", sizeof c->stacking);
    c->auto_show = FALSE;
    c->auto_hide = TRUE;
    c->float_x = c->float_y = -1;
    c->float_width = 0;
    c->pad = TRUE;
    c->pad_combo = TRUE;
    g_strlcpy(c->pad_mode, "both", sizeof c->pad_mode);
    c->deadzone = 0.25;
    g_strlcpy(c->identity, "generic", sizeof c->identity);
    c->overlay_opacity = 0.55;
    c->overlay_scale = 1.0;
    g_strlcpy(c->overlay_layout, "padrão", sizeof c->overlay_layout);
    g_strlcpy(c->remote_touch, "exclusive", sizeof c->remote_touch);
    c->overlay_output[0] = 0;
    g_strlcpy(c->remote_mouse, "auto", sizeof c->remote_mouse);
    c->remote_mouse_speed = 1.5;
    c->mouse_sens_x = c->mouse_sens_y = 1.0;
    c->mouse_curve = 1.0;
    g_strlcpy(c->xcloud_backend, "better-xcloud", sizeof c->xcloud_backend);
}

char *
bk_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "big-keyboard", "big-keyboard.conf", NULL);
}

static double
get_d(GKeyFile *k, const char *g, const char *key, double def, double lo, double hi)
{
    GError *e = NULL;
    double v = g_key_file_get_double(k, g, key, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v < lo ? lo : v > hi ? hi : v;
}

static int
get_i(GKeyFile *k, const char *g, const char *key, int def, int lo, int hi)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(k, g, key, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v < lo ? lo : v > hi ? hi : v;
}

static gboolean
get_b(GKeyFile *k, const char *g, const char *key, gboolean def)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(k, g, key, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v;
}

/* Só um dos valores aceitos (senão o padrão). */
static void
get_s(GKeyFile *k, const char *g, const char *key, char *dst, size_t len, const char *const *allowed)
{
    char *v = g_key_file_get_string(k, g, key, NULL);
    if (!v)
        return;
    g_strstrip(v);
    gboolean ok = allowed == NULL;
    for (int i = 0; allowed && allowed[i]; i++)
        ok |= !strcmp(allowed[i], v);
    /* texto livre (nome de layout, navegador): sem caracteres de controle */
    for (const char *p = v; ok && !allowed && *p; p++)
        ok = (unsigned char)*p >= 0x20;
    if (ok && *v)
        g_strlcpy(dst, v, len);
    g_free(v);
}

void
bk_config_load(BkConfig *c, const char *path)
{
    bk_config_defaults(c);
    char *own = path ? NULL : bk_config_path();
    const char *file = path ? path : own;
    GKeyFile *k = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(k, file, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            /* ilegível: guarda para o usuário ver e segue com os padrões */
            char *bad = g_strdup_printf("%s.invalid", file);
            g_rename(file, bad);
            g_free(bad);
        }
        g_clear_error(&err);
        g_key_file_free(k);
        g_free(own);
        return;
    }
    static const char *const layouts[] = { "auto", "br", "us", "us-intl", NULL };
    static const char *const themes[] = { "auto", "light", "dark", NULL };
    static const char *const modes[] = { "cursor", "point", "both", NULL };
    static const char *const ids[] = { "generic", "xbox360", NULL };
    static const char *const backends[] = { "better-xcloud", "linux", NULL };
    static const char *const rtouch[] = { "off", "exclusive", "shared", NULL };
    static const char *const display_modes[] = { "independent", "exclusive", NULL };
    get_s(k, "General", "DisplayMode", c->display_mode, sizeof c->display_mode, display_modes);
    get_s(k, "Keyboard", "Layout", c->layout, sizeof c->layout, layouts);
    get_s(k, "Keyboard", "Theme", c->theme, sizeof c->theme, themes);
    c->scale = get_d(k, "Keyboard", "Scale", c->scale, 0.6, 2.0);
    c->opacity = get_d(k, "Keyboard", "Opacity", c->opacity, 0.4, 1.0);
    c->reserve = get_b(k, "Keyboard", "ReserveSpace", c->reserve);
    c->autohide = get_b(k, "Keyboard", "HideAfterEnter", c->autohide);
    c->repeat_delay = get_i(k, "Keyboard", "RepeatDelay", c->repeat_delay, 150, 2000);
    c->repeat_rate = get_i(k, "Keyboard", "RepeatInterval", c->repeat_rate, 20, 500);
    c->sound = get_b(k, "Keyboard", "Sound", c->sound);
    static const char *const wmodes[] = { "floating", "docked", NULL };
    static const char *const stackings[] = { "normal", "above", "fullscreen", NULL };
    get_s(k, "Keyboard", "WindowMode", c->window_mode, sizeof c->window_mode, wmodes);
    get_s(k, "Keyboard", "Stacking", c->stacking, sizeof c->stacking, stackings);
    c->auto_show = get_b(k, "Keyboard", "ShowOnTextFocus", c->auto_show);
    c->auto_hide = get_b(k, "Keyboard", "HideOnFocusLoss", c->auto_hide);
    c->float_x = get_i(k, "Keyboard", "FloatX", c->float_x, -1, 65535);
    c->float_y = get_i(k, "Keyboard", "FloatY", c->float_y, -1, 65535);
    c->float_width = get_i(k, "Keyboard", "FloatWidth", c->float_width, 0, 16384);
    c->pad = get_b(k, "Gamepad", "Enabled", c->pad);
    c->pad_combo = get_b(k, "Gamepad", "SelectStartToggles", c->pad_combo);
    get_s(k, "Gamepad", "Navigation", c->pad_mode, sizeof c->pad_mode, modes);
    c->deadzone = get_d(k, "Gamepad", "Deadzone", c->deadzone, 0.05, 0.6);
    get_s(k, "VirtualGamepad", "Identity", c->identity, sizeof c->identity, ids);
    c->overlay_opacity = get_d(k, "VirtualGamepad", "Opacity", c->overlay_opacity, 0.15, 1.0);
    c->overlay_scale = get_d(k, "VirtualGamepad", "Scale", c->overlay_scale, 0.5, 2.0);
    get_s(k, "VirtualGamepad", "Layout", c->overlay_layout, sizeof c->overlay_layout, NULL);
    get_s(k, "VirtualGamepad", "RemoteTouch", c->remote_touch, sizeof c->remote_touch, rtouch);
    get_s(k, "VirtualGamepad", "Output", c->overlay_output, sizeof c->overlay_output, NULL);
    if (!bk_config_valid_output(c->overlay_output))
        c->overlay_output[0] = 0;
    static const char *const rmouse[] = { "auto", "touchpad", "direct", "off", NULL };
    get_s(k, "VirtualGamepad", "RemoteMouse", c->remote_mouse, sizeof c->remote_mouse, rmouse);
    c->remote_mouse_speed = get_d(k, "VirtualGamepad", "RemoteMouseSpeed", c->remote_mouse_speed, 0.5, 4.0);
    c->mouse_sens_x = get_d(k, "Mouse", "SensitivityX", c->mouse_sens_x, 0.1, 10);
    c->mouse_sens_y = get_d(k, "Mouse", "SensitivityY", c->mouse_sens_y, 0.1, 10);
    c->invert_y = get_b(k, "Mouse", "InvertY", c->invert_y);
    c->mouse_curve = get_d(k, "Mouse", "Curve", c->mouse_curve, 0.5, 3);
    get_s(k, "XCloud", "Backend", c->xcloud_backend, sizeof c->xcloud_backend, backends);
    get_s(k, "XCloud", "Browser", c->browser, sizeof c->browser, NULL);
    /* versão antiga (ou sem versão): cópia antes da próxima gravação */
    if (get_i(k, "General", "Version", 0, 0, 1000) < BK_CONFIG_VERSION) {
        char *bak = g_strdup_printf("%s.bak", file);
        char *data = NULL;
        gsize len = 0;
        if (g_file_get_contents(file, &data, &len, NULL))
            g_file_set_contents(bak, data, (gssize)len, NULL);
        g_free(data);
        g_free(bak);
    }
    g_key_file_free(k);
    g_free(own);
}

gboolean
bk_config_save(const BkConfig *c, const char *path, GError **err)
{
    char *own = path ? NULL : bk_config_path();
    const char *file = path ? path : own;
    char *dir = g_path_get_dirname(file);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GKeyFile *k = g_key_file_new();
    g_key_file_set_integer(k, "General", "Version", BK_CONFIG_VERSION);
    g_key_file_set_string(k, "General", "DisplayMode", c->display_mode);
    g_key_file_set_string(k, "Keyboard", "Layout", c->layout);
    g_key_file_set_string(k, "Keyboard", "Theme", c->theme);
    g_key_file_set_double(k, "Keyboard", "Scale", c->scale);
    g_key_file_set_double(k, "Keyboard", "Opacity", c->opacity);
    g_key_file_set_boolean(k, "Keyboard", "ReserveSpace", c->reserve);
    g_key_file_set_boolean(k, "Keyboard", "HideAfterEnter", c->autohide);
    g_key_file_set_integer(k, "Keyboard", "RepeatDelay", c->repeat_delay);
    g_key_file_set_integer(k, "Keyboard", "RepeatInterval", c->repeat_rate);
    g_key_file_set_boolean(k, "Keyboard", "Sound", c->sound);
    g_key_file_set_string(k, "Keyboard", "WindowMode", c->window_mode);
    g_key_file_set_string(k, "Keyboard", "Stacking", c->stacking);
    g_key_file_set_boolean(k, "Keyboard", "ShowOnTextFocus", c->auto_show);
    g_key_file_set_boolean(k, "Keyboard", "HideOnFocusLoss", c->auto_hide);
    g_key_file_set_integer(k, "Keyboard", "FloatX", c->float_x);
    g_key_file_set_integer(k, "Keyboard", "FloatY", c->float_y);
    g_key_file_set_integer(k, "Keyboard", "FloatWidth", c->float_width);
    g_key_file_set_boolean(k, "Gamepad", "Enabled", c->pad);
    g_key_file_set_boolean(k, "Gamepad", "SelectStartToggles", c->pad_combo);
    g_key_file_set_string(k, "Gamepad", "Navigation", c->pad_mode);
    g_key_file_set_double(k, "Gamepad", "Deadzone", c->deadzone);
    g_key_file_set_string(k, "VirtualGamepad", "Identity", c->identity);
    g_key_file_set_double(k, "VirtualGamepad", "Opacity", c->overlay_opacity);
    g_key_file_set_double(k, "VirtualGamepad", "Scale", c->overlay_scale);
    g_key_file_set_string(k, "VirtualGamepad", "Layout", c->overlay_layout);
    g_key_file_set_string(k, "VirtualGamepad", "RemoteTouch", c->remote_touch);
    g_key_file_set_string(k, "VirtualGamepad", "Output", c->overlay_output);
    g_key_file_set_string(k, "VirtualGamepad", "RemoteMouse", c->remote_mouse);
    g_key_file_set_double(k, "VirtualGamepad", "RemoteMouseSpeed", c->remote_mouse_speed);
    g_key_file_set_double(k, "Mouse", "SensitivityX", c->mouse_sens_x);
    g_key_file_set_double(k, "Mouse", "SensitivityY", c->mouse_sens_y);
    g_key_file_set_boolean(k, "Mouse", "InvertY", c->invert_y);
    g_key_file_set_double(k, "Mouse", "Curve", c->mouse_curve);
    g_key_file_set_string(k, "XCloud", "Backend", c->xcloud_backend);
    g_key_file_set_string(k, "XCloud", "Browser", c->browser);
    gboolean ok = g_key_file_save_to_file(k, file, err);
    g_key_file_free(k);
    g_free(own);
    return ok;
}

gboolean
bk_config_valid_output(const char *name)
{
    if (!name || strlen(name) >= 32)
        return FALSE;
    for (const char *p = name; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
            return FALSE;
    return TRUE;
}

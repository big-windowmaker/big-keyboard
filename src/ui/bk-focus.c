/* SPDX-License-Identifier: GPL-2.0-or-later
 * Campo de texto em foco pela acessibilidade (veja bk-focus.h). */
#include "bk-focus.h"

#include <atspi/atspi.h>
#include <gio/gio.h>

struct BkFocus {
    AtspiEventListener *listener;
    BkFocusFunc func;
    gpointer data;
};

/* Um campo onde se digita: editável e não somente leitura, ou um terminal
 * (que nem sempre se diz editável). */
static gboolean
is_text_field(AtspiAccessible *acc)
{
    AtspiRole role = atspi_accessible_get_role(acc, NULL);
    if (role == ATSPI_ROLE_TERMINAL)
        return TRUE;
    AtspiStateSet *ss = atspi_accessible_get_state_set(acc);
    if (!ss)
        return FALSE;
    gboolean ok = atspi_state_set_contains(ss, ATSPI_STATE_EDITABLE) &&
                  !atspi_state_set_contains(ss, ATSPI_STATE_READ_ONLY);
    g_object_unref(ss);
    return ok;
}

static void
on_focus(AtspiEvent *ev, void *data)
{
    BkFocus *f = data;
    if (ev->source && ev->detail1) /* ganhou o foco */
        f->func(is_text_field(ev->source), f->data);
    else if (ev->source && is_text_field(ev->source)) /* um campo perdeu o foco */
        f->func(FALSE, f->data);
    g_boxed_free(ATSPI_TYPE_EVENT, ev);
}

/* Navegadores (Firefox, Chromium) e Qt só expõem a acessibilidade quando
 * alguém a pede: org.a11y.Status.IsEnabled no barramento da sessão. */
static void
request_accessibility(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return;
    GVariant *r = g_dbus_connection_call_sync(
        bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", "org.a11y.Status", "IsEnabled", g_variant_new_boolean(TRUE)), NULL,
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (r)
        g_variant_unref(r);
    g_object_unref(bus);
}

/* O barramento de acessibilidade responde? Sem ele, a libatspi encerra o
 * processo (g_error) em vez de devolver um erro: testado antes. */
static gboolean
a11y_bus_reachable(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return FALSE;
    GVariant *r = g_dbus_connection_call_sync(bus, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress",
                                              NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    g_object_unref(bus);
    if (!r)
        return FALSE;
    const char *addr = NULL;
    g_variant_get(r, "(&s)", &addr);
    GError *err = NULL;
    GDBusConnection *c = addr && *addr ? g_dbus_connection_new_for_address_sync(
                                             addr,
                                             G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                             NULL, NULL, &err)
                                       : NULL;
    if (!c)
        g_warning("acessibilidade (AT-SPI) indisponível: %s", err ? err->message : "sem endereço");
    g_clear_error(&err);
    g_variant_unref(r);
    if (!c)
        return FALSE;
    g_dbus_connection_close_sync(c, NULL, NULL);
    g_object_unref(c);
    return TRUE;
}

BkFocus *
bk_focus_new(BkFocusFunc func, gpointer data)
{
    if (!a11y_bus_reachable() || atspi_init() < 0)
        return NULL;
    request_accessibility();
    BkFocus *f = g_new0(BkFocus, 1);
    f->func = func;
    f->data = data;
    f->listener = atspi_event_listener_new(on_focus, f, NULL);
    GError *err = NULL;
    if (!atspi_event_listener_register(f->listener, "object:state-changed:focused", &err)) {
        g_warning("acessibilidade (AT-SPI) indisponível: %s", err ? err->message : "?");
        g_clear_error(&err);
        bk_focus_free(f);
        return NULL;
    }
    return f;
}

void
bk_focus_free(BkFocus *f)
{
    if (!f)
        return;
    if (f->listener) {
        atspi_event_listener_deregister(f->listener, "object:state-changed:focused", NULL);
        g_object_unref(f->listener);
    }
    g_free(f);
}

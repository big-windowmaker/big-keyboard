/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * big-keyboard-dockapp: o dockapp do Big WM Virtual Keyboard (58×58 num
 * ladrilho de 64×64),
 * dois botões de verdade (desenho, áreas e cliques em bk-dock.c):
 *
 *    ┌─────────┬┬─────────┐
 *    │ ┌─────┐ ││   ▲     │   esquerda: tecla Enter ↵, abre/oculta o teclado
 *    │ │   ↵ │ ││ ◀ ● ▶   │   direita: direcional, abre/oculta o controle na
 *    │ └─────┘ ││   ▼     │   tela; o botão da janela aberta fica afundado
 *    └─────────┴┴─────────┘
 *
 *   clique: alterna a metade · duplo clique (na mesma metade):
 *   configurações, sem alternar nada · botão direito: menu · arrastar: o
 *   Window Maker move o tile · parar o ponteiro: dica da metade.
 *
 *   Em volta, a borda do ladrilho (3 px): segurar ali e arrastar move o
 *   ladrilho, no Window Maker e no wlmaker. O wlmaker só move um dockapp X11
 *   pela borda (o que fica fora da janela dele) ou com o modificador de mover
 *   janelas, e mantém o tamanho que o dockapp pede: com 64×64 não sobrava
 *   borda nenhuma.
 *
 * Um dockapp X11 (líder retirado com janela de ícone): o Window Maker o
 * acopla, e o wlmaker o acopla pelo XWayland, como os outros dockapps da
 * sessão. Fala com o aplicativo pelas ações D-Bus (org.gtk.Actions) e segue
 * o estado dele (mode, pad-connected): nada de relógio; o único temporizador
 * é o do clique pendente (o intervalo de duplo clique) e o da dica.
 */
#include "bk-dock.h"

#include <cairo-xlib.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define SIZE 58 /* como wmcpuload e wmmemload: a borda do ladrilho fica livre */
#define NAME "big-keyboard-dockapp"
#define APP_ID "io.github.big_windowmaker.BigKeyboard"
#define APP_PATH "/io/github/big_windowmaker/BigKeyboard"
#define DRAG 4
#define TIP_DELAY_MS 500

typedef struct {
    Display *dpy;
    Window root, leader, icon;
    GdkWindow *gicon;
    GDBusConnection *bus;
    GDBusActionGroup *actions;
    char mode[16];
    BkDockLayout lay;
    BkDockState st;
    BkClicker clk;
    guint click_timer;
    gboolean down, menu_pending, dragged;
    int px, py, pxr, pyr;
    guint32 menu_time;
    int menu_x, menu_y;
    GtkWidget *tip;
    GtkWidget *tip_label;
    guint tip_timer;
    int tile_x, tile_y; /* canto do tile na raiz, pelo último evento do ponteiro */
} Dock;

static gboolean
is_dark(void)
{
    char *p = g_build_filename(g_get_user_config_dir(), "big-wm", "theme", NULL), *d = NULL;
    gboolean dark = g_file_get_contents(p, &d, NULL, NULL) && g_str_has_prefix(d, "dark");
    g_free(d);
    g_free(p);
    return dark;
}

static gint64
now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

/* O controle na tela só serve com o controle virtual (uinput): sem acesso,
 * o direcional fica apagado e a dica explica (conferido a cada passagem do
 * ponteiro, sem relógio). */
static gboolean
uinput_ok(void)
{
    return access("/dev/uinput", W_OK) == 0;
}

/* ---- desenho ------------------------------------------------------------- */

static void
redraw(Dock *d)
{
    d->st.active[BK_HALF_KBD] = !strcmp(d->mode, "keyboard") || !strcmp(d->mode, "hybrid");
    d->st.active[BK_HALF_PAD] = !strcmp(d->mode, "gamepad") || !strcmp(d->mode, "hybrid");
    cairo_surface_t *s = cairo_xlib_surface_create(d->dpy, d->icon, DefaultVisual(d->dpy, DefaultScreen(d->dpy)),
                                                   d->lay.w, d->lay.h);
    cairo_t *cr = cairo_create(s);
    bk_dock_draw(cr, &d->lay, &d->st);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    XFlush(d->dpy);
}

/* ---- aplicativo ---------------------------------------------------------- */

static void
on_state(GActionGroup *g, const char *name, GVariant *v, gpointer data)
{
    (void)g;
    Dock *d = data;
    if (!strcmp(name, "mode"))
        g_strlcpy(d->mode, g_variant_get_string(v, NULL), sizeof d->mode);
    else if (!strcmp(name, "pad-connected"))
        d->st.pad_connected = g_variant_get_boolean(v);
    redraw(d);
}

static void
read_states(Dock *d)
{
    if (!d->actions)
        return;
    GVariant *m = g_action_group_get_action_state(G_ACTION_GROUP(d->actions), "mode");
    if (m) {
        g_strlcpy(d->mode, g_variant_get_string(m, NULL), sizeof d->mode);
        g_variant_unref(m);
    }
    GVariant *p = g_action_group_get_action_state(G_ACTION_GROUP(d->actions), "pad-connected");
    if (p) {
        d->st.pad_connected = g_variant_get_boolean(p);
        g_variant_unref(p);
    }
    redraw(d);
}

static void
on_added(GActionGroup *g, const char *name, gpointer data)
{
    (void)g;
    (void)name;
    read_states(data);
}

static void
on_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer data)
{
    (void)name;
    (void)owner;
    Dock *d = data;
    g_clear_object(&d->actions);
    d->actions = g_dbus_action_group_get(c, APP_ID, APP_PATH);
    g_signal_connect(d->actions, "action-state-changed", G_CALLBACK(on_state), d);
    g_signal_connect(d->actions, "action-added", G_CALLBACK(on_added), d);
    /* a lista de ações chega assíncrona: pedir uma a dispara */
    g_action_group_list_actions(G_ACTION_GROUP(d->actions));
    read_states(d);
}

static void
on_vanished(GDBusConnection *c, const char *name, gpointer data)
{
    (void)c;
    (void)name;
    Dock *d = data;
    g_clear_object(&d->actions);
    g_strlcpy(d->mode, "hidden", sizeof d->mode);
    d->st.pad_connected = FALSE;
    redraw(d);
}

/* Uma ação do aplicativo; sem ele no ar, inicia-o com a mesma ação. */
static void
act(Dock *d, const char *name, GVariant *param)
{
    if (d->actions && g_action_group_has_action(G_ACTION_GROUP(d->actions), name)) {
        g_action_group_activate_action(G_ACTION_GROUP(d->actions), name, param);
        return;
    }
    char *opt = !strcmp(name, "settings") ? g_strdup("--settings") : g_strdup_printf("--%s", name);
    char *argv[] = { "big-keyboard", opt, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(opt);
}

/* ---- menu ---------------------------------------------------------------- */

static void
menu_act(GtkMenuItem *i, gpointer data)
{
    Dock *d = data;
    const char *a = g_object_get_data(G_OBJECT(i), "action");
    const char *p = g_object_get_data(G_OBJECT(i), "param");
    act(d, a, p ? g_variant_new_string(p) : NULL);
}

static void
item(GtkWidget *m, Dock *d, const char *label, const char *action, const char *param)
{
    GtkWidget *i = gtk_menu_item_new_with_mnemonic(label);
    g_object_set_data(G_OBJECT(i), "action", (gpointer)action);
    g_object_set_data(G_OBJECT(i), "param", (gpointer)param);
    g_signal_connect(i, "activate", G_CALLBACK(menu_act), d);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), i);
}

static void
on_help(GtkMenuItem *i, gpointer d)
{
    (void)i;
    (void)d;
    g_app_info_launch_default_for_uri("https://github.com/big-windowmaker/big-keyboard#readme", NULL, NULL);
}

static void
on_about(GtkMenuItem *i, gpointer d)
{
    (void)i;
    (void)d;
    const char *authors[] = { "Big WindowMaker", NULL };
    gtk_show_about_dialog(NULL, "program-name", "Big WM Virtual Keyboard", "version", BK_VERSION, "comments",
                          "Teclado na tela, controle físico, controle na tela de toque e integração com jogos, "
                          "no estilo Window Maker.",
                          "website", "https://github.com/big-windowmaker/big-keyboard", "license-type",
                          GTK_LICENSE_GPL_2_0, "logo-icon-name", "big-keyboard", "authors", authors, NULL);
}

static void
show_menu(Dock *d)
{
    GtkWidget *m = gtk_menu_new();
    item(m, d, strcmp(d->mode, "keyboard") && strcmp(d->mode, "hybrid") ? "Abrir o _teclado" : "Fechar o _teclado",
         "toggle", NULL);
    item(m, d,
         strcmp(d->mode, "gamepad") && strcmp(d->mode, "hybrid") ? "Abrir o _controle na tela"
                                                                 : "Fechar o _controle na tela",
         "gamepad", NULL);
    item(m, d, "Modo _híbrido (teclado e controle)", "hybrid", NULL);
    item(m, d, "_Escolher o layout do controle…", "settings", "overlay");
    item(m, d, "E_ditar os controles…", "edit-gamepad", NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    item(m, d, "_Configurações…", "settings", "general");
    item(m, d, "Soltar _tudo (emergência)", "release", NULL);
    GtkWidget *h = gtk_menu_item_new_with_mnemonic("Aj_uda");
    g_signal_connect(h, "activate", G_CALLBACK(on_help), d);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), h);
    GtkWidget *a = gtk_menu_item_new_with_mnemonic("_Sobre");
    g_signal_connect(a, "activate", G_CALLBACK(on_about), d);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), a);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    item(m, d, "Sai_r do Big WM Virtual Keyboard", "quit", NULL);
    gtk_widget_show_all(m);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    /* O gatilho: um clique na janela raiz (uma janela do GDK, ao contrário
     * da do dockapp), onde está o ponteiro, com o ponteiro do assento; sem
     * ele o GTK não sabe ancorar o menu e não o abre. */
    GdkEvent *trigger = gdk_event_new(GDK_BUTTON_PRESS);
    trigger->button.window = g_object_ref(gdk_get_default_root_window());
    trigger->button.button = 3;
    trigger->button.time = d->menu_time;
    trigger->button.x = trigger->button.x_root = d->menu_x;
    trigger->button.y = trigger->button.y_root = d->menu_y;
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    if (seat)
        gdk_event_set_device(trigger, gdk_seat_get_pointer(seat));
    gtk_menu_popup_at_pointer(GTK_MENU(m), trigger);
    gdk_event_free(trigger);
}

static gboolean
show_menu_idle(gpointer data)
{
    show_menu(data);
    return G_SOURCE_REMOVE;
}

/* ---- eventos X ------------------------------------------------------------- */

static Window
wm_parent(Dock *d)
{
    Window root, parent, *kids = NULL;
    unsigned n;
    if (!XQueryTree(d->dpy, d->icon, &root, &parent, &kids, &n))
        return None;
    if (kids)
        XFree(kids);
    return parent == d->root ? None : parent;
}

/* Arrastar: o Window Maker move o tile (um clique sintético no pai). */
static void
wm_forward(Dock *d)
{
    Window parent = wm_parent(d), child;
    if (parent == None)
        return;
    int x, y;
    XTranslateCoordinates(d->dpy, d->icon, parent, d->px, d->py, &x, &y, &child);
    XUngrabPointer(d->dpy, CurrentTime);
    XEvent ev = { 0 };
    ev.xbutton.type = ButtonPress;
    ev.xbutton.display = d->dpy;
    ev.xbutton.window = parent;
    ev.xbutton.root = d->root;
    ev.xbutton.x = x;
    ev.xbutton.y = y;
    ev.xbutton.x_root = d->pxr;
    ev.xbutton.y_root = d->pyr;
    ev.xbutton.button = Button1;
    ev.xbutton.same_screen = True;
    XSendEvent(d->dpy, parent, False, ButtonPressMask, &ev);
    XFlush(d->dpy);
}

/* ---- dica ------------------------------------------------------------------ */

static void
tip_hide(Dock *d)
{
    if (d->tip_timer) {
        g_source_remove(d->tip_timer);
        d->tip_timer = 0;
    }
    if (d->tip)
        gtk_widget_hide(d->tip);
}

static gboolean
tip_show(gpointer data)
{
    Dock *d = data;
    d->tip_timer = 0;
    BkHalf h = d->st.hover;
    if (h == BK_HALF_NONE || d->down)
        return G_SOURCE_REMOVE;
    if (!d->tip) {
        d->tip = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_type_hint(GTK_WINDOW(d->tip), GDK_WINDOW_TYPE_HINT_TOOLTIP);
        gtk_style_context_add_class(gtk_widget_get_style_context(d->tip), "tooltip");
        d->tip_label = gtk_label_new(NULL);
        gtk_container_set_border_width(GTK_CONTAINER(d->tip), 6);
        gtk_container_add(GTK_CONTAINER(d->tip), d->tip_label);
    }
    const char *text;
    if (h == BK_HALF_KBD)
        text = "<b>Teclado virtual</b> — Clique para abrir/ocultar\n"
               "<small>Duplo clique: configurações · botão direito: menu</small>";
    else if (d->st.disabled[BK_HALF_PAD] && !d->st.active[BK_HALF_PAD])
        text = "<b>Controle na tela</b> — indisponível\n"
               "<small>Sem acesso ao /dev/uinput: instale o steam-devices (ou o game-devices-udev).\n"
               "Duplo clique: configurações · botão direito: menu</small>";
    else
        text = "<b>Controle na tela</b> — Clique para abrir/ocultar\n"
               "<small>Duplo clique: configurações · botão direito: menu</small>";
    gtk_label_set_markup(GTK_LABEL(d->tip_label), text);
    gtk_widget_show_all(d->tip);
    /* abaixo do tile (acima, se não couber no monitor). A posição vem do
     * ponteiro (raiz − local): no XWayland, a janela do dockapp fica em 0,0
     * na raiz X, e só os eventos trazem a posição real na tela. */
    int tx = d->tile_x, ty = d->tile_y;
    GtkRequisition req;
    gtk_widget_get_preferred_size(d->tip, NULL, &req);
    GdkMonitor *m = gdk_display_get_monitor_at_point(gdk_display_get_default(), tx + d->lay.w / 2, ty);
    GdkRectangle g = { 0, 0, 1 << 15, 1 << 15 };
    if (m)
        gdk_monitor_get_geometry(m, &g);
    int x = tx + (int)d->lay.btn[h].x, y = ty + d->lay.h + 4;
    if (y + req.height > g.y + g.height)
        y = ty - req.height - 4;
    x = CLAMP(x, g.x, MAX(g.x, g.x + g.width - req.width));
    gtk_window_move(GTK_WINDOW(d->tip), x, y);
    return G_SOURCE_REMOVE;
}

static void
tip_restart(Dock *d)
{
    tip_hide(d);
    if (d->st.hover != BK_HALF_NONE)
        d->tip_timer = g_timeout_add(TIP_DELAY_MS, tip_show, d);
}

/* ---- cliques ------------------------------------------------------------------ */

static void
run_click(Dock *d, BkClick c)
{
    switch (c) {
    case BK_CLICK_TOGGLE_KBD:
        act(d, "toggle", NULL);
        break;
    case BK_CLICK_TOGGLE_PAD:
        /* indisponível: só fecha (aberto pelo menu); abrir não faria nada */
        if (d->st.disabled[BK_HALF_PAD] && !d->st.active[BK_HALF_PAD]) {
            tip_show(d);
            break;
        }
        act(d, "gamepad", NULL);
        break;
    case BK_CLICK_SETTINGS:
        act(d, "settings", g_variant_new_string("general"));
        break;
    case BK_CLICK_NONE:
        break;
    }
}

static void schedule_click(Dock *d);

static gboolean
on_click_timer(gpointer data)
{
    Dock *d = data;
    d->click_timer = 0;
    run_click(d, bk_clicker_timeout(&d->clk, now_ms()));
    schedule_click(d);
    return G_SOURCE_REMOVE;
}

/* Um temporizador só enquanto há um clique pendente. */
static void
schedule_click(Dock *d)
{
    if (d->click_timer) {
        g_source_remove(d->click_timer);
        d->click_timer = 0;
    }
    gint64 dl = bk_clicker_deadline(&d->clk);
    if (dl >= 0)
        d->click_timer = g_timeout_add((guint)MAX(0, dl - now_ms()), on_click_timer, d);
}

static void
set_hover(Dock *d, BkHalf h)
{
    if (h == d->st.hover)
        return;
    d->st.hover = h;
    d->st.disabled[BK_HALF_PAD] = !uinput_ok();
    redraw(d);
    tip_restart(d);
}

static GdkFilterReturn
x_filter(GdkXEvent *xev, GdkEvent *event, gpointer data)
{
    (void)event;
    Dock *d = data;
    XEvent *ev = xev;
    if (ev->type == MotionNotify) {
        d->tile_x = ev->xmotion.x_root - ev->xmotion.x;
        d->tile_y = ev->xmotion.y_root - ev->xmotion.y;
    } else if (ev->type == EnterNotify) {
        d->tile_x = ev->xcrossing.x_root - ev->xcrossing.x;
        d->tile_y = ev->xcrossing.y_root - ev->xcrossing.y;
    }
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0)
            redraw(d);
        return GDK_FILTER_REMOVE;
    case ConfigureNotify:
        if (ev->xconfigure.width != d->lay.w || ev->xconfigure.height != d->lay.h) {
            bk_dock_layout(&d->lay, ev->xconfigure.width, ev->xconfigure.height);
            redraw(d);
        }
        return GDK_FILTER_CONTINUE;
    case ButtonPress:
        tip_hide(d);
        if (ev->xbutton.button == Button3) {
            /* um clique pendente vale antes do menu; o direito não alterna nada */
            run_click(d, bk_clicker_flush(&d->clk));
            schedule_click(d);
            d->menu_pending = TRUE; /* abre ao soltar: a pegada do clique impediria a do menu */
        } else if (ev->xbutton.button == Button1) {
            BkHalf h = bk_dock_hit(&d->lay, ev->xbutton.x, ev->xbutton.y);
            run_click(d, bk_clicker_press(&d->clk, h, ev->xbutton.x, ev->xbutton.y, now_ms()));
            schedule_click(d);
            d->down = TRUE;
            d->dragged = FALSE;
            d->px = ev->xbutton.x;
            d->py = ev->xbutton.y;
            d->pxr = ev->xbutton.x_root;
            d->pyr = ev->xbutton.y_root;
            d->st.pressed = h;
            redraw(d);
        }
        return GDK_FILTER_REMOVE;
    case MotionNotify:
        if (d->down && !d->dragged &&
            (abs(ev->xmotion.x - d->px) > DRAG || abs(ev->xmotion.y - d->py) > DRAG)) {
            d->dragged = TRUE;
            d->down = FALSE;
            bk_clicker_cancel(&d->clk);
            schedule_click(d);
            d->st.pressed = BK_HALF_NONE;
            redraw(d);
            wm_forward(d);
        } else if (d->down) {
            /* apertado: a metade só aparece afundada com o ponteiro sobre ela */
            BkHalf h = bk_dock_hit(&d->lay, ev->xmotion.x, ev->xmotion.y);
            BkHalf p = h == d->clk.down_half ? h : BK_HALF_NONE;
            if (p != d->st.pressed) {
                d->st.pressed = p;
                redraw(d);
            }
        } else {
            set_hover(d, bk_dock_hit(&d->lay, ev->xmotion.x, ev->xmotion.y));
        }
        return GDK_FILTER_REMOVE;
    case ButtonRelease:
        if (ev->xbutton.button == Button3 && d->menu_pending) {
            d->menu_pending = FALSE;
            d->menu_time = (guint32)ev->xbutton.time;
            d->menu_x = ev->xbutton.x_root;
            d->menu_y = ev->xbutton.y_root;
            g_idle_add(show_menu_idle, d); /* depois de soltar: a pegada do clique já acabou */
        } else if (ev->xbutton.button == Button1 && d->down) {
            d->down = FALSE;
            BkHalf h = bk_dock_hit(&d->lay, ev->xbutton.x, ev->xbutton.y);
            run_click(d, bk_clicker_release(&d->clk, h, now_ms()));
            schedule_click(d);
            d->st.pressed = BK_HALF_NONE;
            d->st.hover = h;
            redraw(d);
        }
        return GDK_FILTER_REMOVE;
    case EnterNotify:
        set_hover(d, bk_dock_hit(&d->lay, ev->xcrossing.x, ev->xcrossing.y));
        return GDK_FILTER_REMOVE;
    case LeaveNotify:
        if (!d->down)
            set_hover(d, BK_HALF_NONE);
        tip_hide(d);
        return GDK_FILTER_REMOVE;
    default:
        return GDK_FILTER_CONTINUE;
    }
}

static void
create_windows(Dock *d, int argc, char **argv)
{
    GdkDisplay *gd = gdk_display_get_default();
    d->dpy = GDK_DISPLAY_XDISPLAY(gd);
    int scr = DefaultScreen(d->dpy);
    d->root = RootWindow(d->dpy, scr);
    XSetWindowAttributes a = { 0 };
    a.background_pixel = BlackPixel(d->dpy, scr);
    d->leader = XCreateWindow(d->dpy, d->root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent,
                              CWBackPixel, &a);
    d->icon = XCreateWindow(d->dpy, d->root, 0, 0, SIZE, SIZE, 0, CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixel, &a);
    XClassHint ch = { NAME, "DockApp" };
    XSetClassHint(d->dpy, d->leader, &ch);
    XSetClassHint(d->dpy, d->icon, &ch);
    XStoreName(d->dpy, d->leader, "Big WM Virtual Keyboard");
    XSizeHints sh = { 0 };
    sh.flags = PMinSize | PMaxSize | USPosition;
    sh.min_width = sh.max_width = sh.min_height = sh.max_height = SIZE;
    XSetWMNormalHints(d->dpy, d->leader, &sh);
    XWMHints wh = { 0 };
    wh.flags = StateHint | IconWindowHint | WindowGroupHint;
    wh.initial_state = WithdrawnState; /* um dockapp: só a janela de ícone aparece */
    wh.icon_window = d->icon;
    wh.window_group = d->leader;
    XSetWMHints(d->dpy, d->leader, &wh);
    XSetCommand(d->dpy, d->leader, argv, argc); /* o Command que o Window Maker guarda */
    XSelectInput(d->dpy, d->icon,
                 ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     EnterWindowMask | LeaveWindowMask);
    XMapWindow(d->dpy, d->leader);
    XFlush(d->dpy);
    d->gicon = gdk_x11_window_foreign_new_for_display(gd, d->icon);
    gdk_window_add_filter(d->gicon, x_filter, d);
}

static void
on_theme(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent e, gpointer data)
{
    (void)m;
    (void)f;
    (void)o;
    (void)e;
    Dock *d = data;
    d->st.dark = is_dark();
    redraw(d);
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_setenv("GDK_BACKEND", "x11", TRUE); /* um dockapp é X11 (no wlmaker, pelo XWayland) */
    gtk_init(&argc, &argv);
    Dock d = { 0 };
    g_strlcpy(d.mode, "hidden", sizeof d.mode);
    d.st.dark = is_dark();
    d.st.hover = d.st.pressed = BK_HALF_NONE;
    d.st.disabled[BK_HALF_PAD] = !uinput_ok();
    bk_dock_layout(&d.lay, SIZE, SIZE);
    int dist = 5;
    g_object_get(gtk_settings_get_default(), "gtk-double-click-distance", &dist, NULL);
    bk_clicker_init(&d.clk, bk_dock_double_click_ms(NULL), dist);
    create_windows(&d, argc, argv);
    redraw(&d);
    char *theme = g_build_filename(g_get_user_config_dir(), "big-wm", "theme", NULL);
    GFile *tf = g_file_new_for_path(theme);
    GFileMonitor *tm = g_file_monitor_file(tf, G_FILE_MONITOR_NONE, NULL, NULL);
    if (tm)
        g_signal_connect(tm, "changed", G_CALLBACK(on_theme), &d);
    g_object_unref(tf);
    g_free(theme);
    g_bus_watch_name(G_BUS_TYPE_SESSION, APP_ID, G_BUS_NAME_WATCHER_FLAGS_NONE, on_appeared, on_vanished, &d, NULL);
    gtk_main();
    return 0;
}

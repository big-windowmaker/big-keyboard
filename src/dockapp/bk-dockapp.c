/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * big-keyboard-dockapp: o dockapp do Big WM Virtual Keyboard (tile 64×64).
 *
 *    ┌──────────┬──────────┐
 *    │ ▫▫▫▫▫▫  │   ┌─┐ ●  │   metade esquerda: um teclado; direita: um
 *    │ ▫▫▫▫▫▫  │  ─┼─  ●● │   controle. A metade do modo ativo acende (LED
 *    │ ▫▬▬▬▫   │   └─┘ ●  │   verde); o LED azul no canto: controle ligado.
 *    └──────────┴──────────┘
 *
 *   clique: abre/fecha o teclado · duplo clique: configurações ·
 *   botão direito: menu (teclado, controle na tela, híbrido, layout do
 *   controle, editar, configurações, ajuda, sobre, sair) · arrastar: o
 *   Window Maker move o tile.
 *
 * Um dockapp X11 (líder retirado com janela de ícone): o Window Maker o
 * acopla, e o wlmaker o acopla pelo XWayland, como os outros dockapps da
 * sessão. Fala com o aplicativo pelas ações D-Bus (org.gtk.Actions) e segue
 * o estado dele (mode, pad-connected): nada de relógio, só eventos.
 */
#include <cairo-xlib.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define SIZE 64
#define NAME "big-keyboard-dockapp"
#define APP_ID "io.github.big_windowmaker.BigKeyboard"
#define APP_PATH "/io/github/big_windowmaker/BigKeyboard"
#define DRAG 4

typedef struct {
    Display *dpy;
    Window root, leader, icon;
    GdkWindow *gicon;
    GDBusConnection *bus;
    GDBusActionGroup *actions;
    char mode[16];
    gboolean pad;
    gboolean pressed, hover, menu_pending, dragged;
    int px, py, pxr, pyr;
    guint32 last_click, menu_time;
    int menu_x, menu_y;
    gboolean dark;
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

/* ---- desenho ------------------------------------------------------------- */

static void
bevel(cairo_t *cr, double x, double y, double w, double h, gboolean raised)
{
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, raised ? 1 : 0.33, raised ? 1 : 0.33, raised ? 1 : 0.33);
    cairo_move_to(cr, x + 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + 0.5);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, raised ? 0 : 1, raised ? 0 : 1, raised ? 0 : 1);
    cairo_move_to(cr, x + w - 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + h - 0.5);
    cairo_stroke(cr);
}

static void
rect(cairo_t *cr, double x, double y, double w, double h, double v)
{
    cairo_set_source_rgb(cr, v, v, v);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

/* Metade esquerda: um tecladinho em relevo sobre um visor fundo. */
static void
draw_keyboard(cairo_t *cr, double x, double y, double w, double h, gboolean on, gboolean dark)
{
    rect(cr, x, y, w, h, on ? (dark ? 0.22 : 0.80) : (dark ? 0.12 : 0.55));
    bevel(cr, x, y, w, h, FALSE);
    double k = 4, g = 1, ox = x + 2.5, oy = y + 10;
    static const int cols[3] = { 6, 6, 5 };
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < cols[r]; c++) {
            double kx = ox + c * (k + g) + (r == 2 ? 2.5 : r == 1 ? 1 : 0), ky = oy + r * (k + g);
            rect(cr, kx, ky, k, k, on ? 0.95 : 0.80);
            cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
            cairo_rectangle(cr, kx + k - 1, ky, 1, k);
            cairo_rectangle(cr, kx, ky + k - 1, k, 1);
            cairo_fill(cr);
        }
    /* barra de espaço */
    rect(cr, ox + 6, oy + 3 * (k + g), 16, k, on ? 0.95 : 0.80);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_rectangle(cr, ox + 6, oy + 3 * (k + g) + k - 1, 16, 1);
    cairo_fill(cr);
}

/* Metade direita: um controle (direcional e quatro botões). */
static void
draw_pad(cairo_t *cr, double x, double y, double w, double h, gboolean on, gboolean dark)
{
    rect(cr, x, y, w, h, on ? (dark ? 0.22 : 0.80) : (dark ? 0.12 : 0.55));
    bevel(cr, x, y, w, h, FALSE);
    double cx = x + w / 2, cy = y + h / 2 + 3;
    /* corpo do controle */
    cairo_set_source_rgb(cr, on ? 0.30 : 0.22, on ? 0.30 : 0.22, on ? 0.33 : 0.25);
    cairo_new_path(cr);
    cairo_arc(cr, cx - 7, cy, 7, M_PI / 2, 3 * M_PI / 2);
    cairo_arc(cr, cx + 7, cy, 7, -M_PI / 2, M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
    /* direcional */
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_rectangle(cr, cx - 10, cy - 1, 6, 2);
    cairo_rectangle(cr, cx - 8, cy - 3, 2, 6);
    cairo_fill(cr);
    /* botões coloridos, como manda a tradição */
    const double b[4][5] = { { 8, -3, 0.95, 0.75, 0.10 }, { 11, 0, 0.85, 0.20, 0.15 }, { 8, 3, 0.20, 0.70, 0.25 },
                             { 5, 0, 0.20, 0.45, 0.90 } };
    for (int i = 0; i < 4; i++) {
        cairo_set_source_rgb(cr, b[i][2], b[i][3], b[i][4]);
        cairo_arc(cr, cx + b[i][0], cy + b[i][1], 1.4, 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

static void
led(cairo_t *cr, double x, double y, double r, double g, double b, gboolean on)
{
    cairo_set_source_rgb(cr, on ? r : 0.25, on ? g : 0.25, on ? b : 0.25);
    cairo_rectangle(cr, x, y, 4, 4);
    cairo_fill(cr);
}

static void
redraw(Dock *d)
{
    cairo_surface_t *s = cairo_xlib_surface_create(d->dpy, d->icon, DefaultVisual(d->dpy, DefaultScreen(d->dpy)),
                                                   SIZE, SIZE);
    cairo_t *cr = cairo_create(s);
    gboolean kb = !strcmp(d->mode, "keyboard") || !strcmp(d->mode, "hybrid");
    gboolean gp = !strcmp(d->mode, "gamepad") || !strcmp(d->mode, "hybrid");
    double body = d->dark ? 0.24 : 0.667;
    rect(cr, 0, 0, SIZE, SIZE, d->hover && !d->pressed ? body + 0.05 : body);
    bevel(cr, 0, 0, SIZE, SIZE, !d->pressed);
    draw_keyboard(cr, 4, 4, 28, 48, kb, d->dark);
    draw_pad(cr, 33, 4, 27, 48, gp, d->dark);
    /* etiqueta e LEDs no rodapé */
    led(cr, 6, 55, 0.19, 0.82, 0.19, kb);
    led(cr, 35, 55, 0.19, 0.82, 0.19, gp);
    led(cr, 54, 55, 0.20, 0.55, 1.0, d->pad); /* controle físico ligado */
    cairo_set_source_rgb(cr, d->dark ? 0.9 : 0, d->dark ? 0.9 : 0, d->dark ? 0.9 : 0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 7);
    cairo_move_to(cr, 13, 60);
    cairo_show_text(cr, "KBD");
    cairo_move_to(cr, 42, 60);
    cairo_show_text(cr, "PAD");
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
        d->pad = g_variant_get_boolean(v);
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
        d->pad = g_variant_get_boolean(p);
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
    d->pad = FALSE;
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

static GdkFilterReturn
x_filter(GdkXEvent *xev, GdkEvent *event, gpointer data)
{
    (void)event;
    Dock *d = data;
    XEvent *ev = xev;
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0)
            redraw(d);
        return GDK_FILTER_REMOVE;
    case ButtonPress:
        if (ev->xbutton.button == Button3) {
            d->menu_pending = TRUE; /* abre ao soltar: a pegada do clique impediria a do menu */
        } else if (ev->xbutton.button == Button1) {
            d->pressed = TRUE;
            d->dragged = FALSE;
            d->px = ev->xbutton.x;
            d->py = ev->xbutton.y;
            d->pxr = ev->xbutton.x_root;
            d->pyr = ev->xbutton.y_root;
            redraw(d);
        }
        return GDK_FILTER_REMOVE;
    case MotionNotify:
        if (d->pressed && !d->dragged &&
            (abs(ev->xmotion.x - d->px) > DRAG || abs(ev->xmotion.y - d->py) > DRAG)) {
            d->dragged = TRUE;
            d->pressed = FALSE;
            redraw(d);
            wm_forward(d);
        }
        return GDK_FILTER_REMOVE;
    case ButtonRelease:
        if (ev->xbutton.button == Button3 && d->menu_pending) {
            d->menu_pending = FALSE;
            d->menu_time = (guint32)ev->xbutton.time;
            d->menu_x = ev->xbutton.x_root;
            d->menu_y = ev->xbutton.y_root;
            g_idle_add(show_menu_idle, d); /* depois de soltar: a pegada do clique já acabou */
        } else if (ev->xbutton.button == Button1 && d->pressed) {
            d->pressed = FALSE;
            redraw(d);
            int dbl = 400;
            g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &dbl, NULL);
            /* cada clique alterna o teclado na hora; o segundo de um duplo
             * clique (que o devolve ao estado anterior) abre as configurações */
            act(d, "toggle", NULL);
            if (d->last_click && ev->xbutton.time - d->last_click <= (guint32)dbl) {
                d->last_click = 0;
                act(d, "settings", g_variant_new_string("general"));
            } else {
                d->last_click = ev->xbutton.time;
            }
        }
        return GDK_FILTER_REMOVE;
    case EnterNotify:
    case LeaveNotify:
        d->hover = ev->type == EnterNotify;
        redraw(d);
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
                 ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask |
                     LeaveWindowMask);
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
    d->dark = is_dark();
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
    d.dark = is_dark();
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

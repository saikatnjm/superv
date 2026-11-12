/*
 * SuperV extras: three small Windows shortcuts. Nothing here runs, or takes memory, until you press one,
 * and each can be turned off in SuperV's settings.
 *
 *   Ctrl+Super+Alt+V   paste as plain text       (Ctrl+Win+Alt+V, PowerToys Advanced Paste)
 *   Super+Shift+C      colour picker             (Win+Shift+C, PowerToys Color Picker)
 *   Super+Shift+T      copy text from the screen (Win+Shift+T, PowerToys Text Extractor)
 *
 * Colours: on X11 the screen is frozen with a magnifier and you click; on Wayland apps can't read the screen,
 * so the desktop's own colour picker is used (xdg-desktop-portal: GNOME, KDE Plasma and others).
 * Text: the screenshot tool's frozen screen (shot.c) on every desktop; you drag over the text.
 * Text recognition uses Tesseract when it is installed (tesseract-ocr); nothing is sent anywhere.
 *
 * MIT License.
 */
#define _GNU_SOURCE
#include <math.h>
#include <malloc.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <sys/wait.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include "superv.h"

static void logmsg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap); va_end(ap);
    GDateTime *t = g_date_time_new_now_local();
    char *ts = g_date_time_format(t, "%H:%M:%S");
    g_printerr("%s [superv:extras] %s\n", ts, m);
    g_free(ts); g_free(m); g_date_time_unref(t);
}

static GtkClipboard *clipboard(void) { return gtk_clipboard_get(GDK_SELECTION_CLIPBOARD); }

/* The monitor you're working on (on Wayland the pointer can't be seen: the primary one). */
static void work_area(GdkRectangle *a) {
    GdkDisplay *disp = gdk_display_get_default();
    GdkMonitor *m = NULL;
    if (!sv_wayland) {
        int px, py;
        gdk_device_get_position(gdk_seat_get_pointer(gdk_display_get_default_seat(disp)), NULL, &px, &py);
        m = gdk_display_get_monitor_at_point(disp, px, py);
    }
    if (!m) m = gdk_display_get_primary_monitor(disp);
    if (!m) m = gdk_display_get_monitor(disp, 0);
    gdk_monitor_get_workarea(m, a);
}

/* ---- a toast: a small card for a few seconds, near the bottom of the screen. Unlike a notification it
 * shows even with Do Not Disturb on, and it never takes the keyboard. Click it to close it. */
static GtkWidget *toast_win, *toast_title, *toast_body;
static guint toast_src;

static gboolean toast_hide(gpointer d) {
    toast_src = 0;
    if (toast_win) gtk_widget_hide(toast_win);
    return G_SOURCE_REMOVE;
}

static gboolean toast_click(GtkWidget *w, GdkEvent *e, gpointer d) {
    if (toast_src) g_source_remove(toast_src);
    toast_hide(NULL);
    return TRUE;
}

static void toast(const char *title, const char *body) {
    if (!toast_win) {
        toast_win = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_type_hint(GTK_WINDOW(toast_win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
        sv_style_window(toast_win);
        GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_style_context_add_class(gtk_widget_get_style_context(panel), "cbh-panel");
        toast_title = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(toast_title), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(toast_title), "cbh-title");
        toast_body = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(toast_body), 0);
        gtk_label_set_line_wrap(GTK_LABEL(toast_body), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(toast_body), 48);
        gtk_label_set_lines(GTK_LABEL(toast_body), 3);
        gtk_label_set_ellipsize(GTK_LABEL(toast_body), PANGO_ELLIPSIZE_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(toast_body), "cbh-sub");
        gtk_widget_set_margin_start(toast_title, 4); gtk_widget_set_margin_start(toast_body, 4);
        gtk_box_pack_start(GTK_BOX(panel), toast_title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(panel), toast_body, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(toast_win), panel);
        gtk_widget_add_events(toast_win, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(toast_win, "button-press-event", G_CALLBACK(toast_click), NULL);
        gtk_widget_show_all(panel);
    }
    sv_style_window(toast_win);   /* the theme may have changed */
    gtk_label_set_text(GTK_LABEL(toast_title), title);
    gtk_label_set_text(GTK_LABEL(toast_body), body ? body : "");
    gtk_widget_set_visible(toast_body, body && *body);
    gtk_label_set_width_chars(GTK_LABEL(toast_body), body ? MIN((int)g_utf8_strlen(body, -1), 46) : 0);   /* not a narrow column */
    gtk_window_resize(GTK_WINDOW(toast_win), 1, 1);   /* fit the new text */
    GtkRequisition nat;
    gtk_widget_get_preferred_size(toast_win, NULL, &nat);
    GdkRectangle a;
    work_area(&a);
    gtk_window_move(GTK_WINDOW(toast_win), a.x + (a.width - nat.width) / 2, a.y + a.height - nat.height - 40);
    gtk_widget_show(toast_win);
    if (toast_src) g_source_remove(toast_src);
    toast_src = g_timeout_add(body && *body ? 4000 : 2500, toast_hide, NULL);
}
#define notify toast

/* ================================================================== paste as plain text */

static guint plain_src;
static int plain_tries;
static gboolean plain_x11, plain_pending;   /* pending: waiting for the clipboard's owner to send its text */

/* Still holding Ctrl / Alt / Super / Shift from the shortcut? Pressing paste now would send Ctrl+Alt+…+Insert. */
static gboolean modifiers_held(void) {
    if (sv_wayland && !plain_x11) return FALSE;   /* can't see the keyboard of Wayland apps: we waited instead */
    Display *d = gdk_x11_display_get_xdisplay(gdk_display_get_default());
    static const KeySym mods[] = { XK_Control_L, XK_Control_R, XK_Alt_L, XK_Alt_R, XK_Super_L, XK_Super_R,
                                   XK_Shift_L, XK_Shift_R, XK_Meta_L, XK_Meta_R, XK_ISO_Level3_Shift };
    char map[32];
    XQueryKeymap(d, map);
    for (guint i = 0; i < G_N_ELEMENTS(mods); i++) {
        KeyCode kc = XKeysymToKeycode(d, mods[i]);
        if (kc && (map[kc / 8] & (1 << (kc % 8)))) return TRUE;
    }
    return FALSE;
}

static gboolean plain_press(gpointer d) {
    if (modifiers_held() && ++plain_tries < 40) return G_SOURCE_CONTINUE;   /* up to 2 s */
    plain_src = 0;
    if (!sv_send_keys(sv_paste_keys(), plain_x11))
        notify("Copied as plain text — press Ctrl+V",
               "Pasting for you needs a one-time permission: open SuperV (Super+V) and click Enable.");
    return G_SOURCE_REMOVE;
}

static void plain_got_text(GtkClipboard *c, const char *text, gpointer d) {
    plain_pending = FALSE;
    if (!text || !*text) {
        notify("Nothing to paste as text", "What's on the clipboard has no text. Copy some text first.");
        return;
    }
    char *t = g_strdup(text);   /* the clipboard now holds the text alone: no fonts, colours, links or images */
    gtk_clipboard_set_text(clipboard(), t, -1);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), t, -1);   /* Shift+Insert in terminals */
    g_free(t);
    if (!g_strcmp0(sv_paste_keys(), "none")) return;
    plain_tries = 0;
    /* In a Wayland app we can't see when you let go of the shortcut's keys: give you a moment. */
    plain_src = g_timeout_add(sv_wayland && !plain_x11 ? 450 : 50, plain_press, NULL);
}

static void plain_paste(void) {
    if (plain_src || plain_pending) return;   /* pressed twice: one paste */
    plain_pending = TRUE;
    plain_x11 = sv_focused_is_x11();
    gtk_clipboard_request_text(clipboard(), plain_got_text, NULL);
}

/* ================================================================== the desktop's own picker and screenshot (Wayland) */

enum { PORTAL_OK, PORTAL_CANCELLED, PORTAL_FAILED };
typedef void (*PortalDone)(int status, GVariant *results, gpointer data);
typedef struct { guint sub, timer; PortalDone done; gpointer data; gboolean answered, called; } PortalReq;
static int portal_pending;

static void portal_finish(GDBusConnection *bus, PortalReq *r, int status, GVariant *results) {
    if (r->answered) return;
    r->answered = TRUE;
    if (r->timer) { g_source_remove(r->timer); r->timer = 0; }
    g_dbus_connection_signal_unsubscribe(bus, r->sub);
    portal_pending--;
    r->done(status, results, r->data);
}

static gboolean portal_timeout(gpointer p) {   /* a dialog left open, or a portal that died */
    PortalReq *r = p;
    r->timer = 0;
    logmsg("desktop portal: no answer, giving up");
    portal_finish(g_application_get_dbus_connection(G_APPLICATION(sv_app)), r, PORTAL_CANCELLED, NULL);
    if (r->called) g_free(r);   /* else on_portal_called frees it */
    return G_SOURCE_REMOVE;
}

static void on_portal_response(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                               const char *signal, GVariant *params, gpointer p) {
    guint32 response;
    GVariant *results;
    g_variant_get(params, "(u@a{sv})", &response, &results);
    portal_finish(bus, p, response == 0 ? PORTAL_OK : PORTAL_CANCELLED, results);
    g_variant_unref(results);
    g_free(p);
}

static void on_portal_called(GObject *src, GAsyncResult *res, gpointer p) {
    PortalReq *r = p;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    r->called = TRUE;
    if (v) {   /* the answer comes as a Response signal */
        g_variant_unref(v);
        if (r->answered) g_free(r);   /* ...unless we stopped waiting for it */
        return;
    }
    logmsg("desktop portal: %s", e->message);
    g_error_free(e);
    portal_finish(G_DBUS_CONNECTION(src), r, PORTAL_FAILED, NULL);
    g_free(r);
}

/* org.freedesktop.portal.Screenshot.<method>(parent, options), answered by a Request.Response signal */
static void portal_call(const char *method, GVariantBuilder *opts, PortalDone done, gpointer data) {
    GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(sv_app));
    PortalReq *r = g_new0(PortalReq, 1);
    r->done = done; r->data = data;
    if (!bus) { g_variant_builder_unref(opts); done(PORTAL_FAILED, NULL, data); g_free(r); return; }
    char *sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
    g_strdelimit(sender, ".", '_');
    char *token = g_strdup_printf("superv%u", g_random_int());
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    g_variant_builder_add(opts, "{sv}", "handle_token", g_variant_new_string(token));
    r->sub = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
                                                "Response", path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                on_portal_response, r, NULL);
    portal_pending++;
    r->timer = g_timeout_add_seconds(120, portal_timeout, r);
    g_dbus_connection_call(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.Screenshot", method, g_variant_new("(sa{sv})", "", opts),
                           G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, on_portal_called, r);
    g_variant_builder_unref(opts);
    g_free(sender); g_free(token); g_free(path);
}

/* ================================================================== the frozen screen with a magnifier (colour picker, X11) */

typedef void (*OverlayDone)(gboolean ok, double r, double g, double b);
static GtkWidget *ov;
static cairo_surface_t *shot;   /* the frozen screen, in device pixels */
static int shot_w, shot_h;
static OverlayDone ov_done;
static double ov_scale, px = -1, py = -1;

static void shot_pixel(double x, double y, double *r, double *g, double *b) {
    int ix = CLAMP((int)(x * ov_scale), 0, shot_w - 1), iy = CLAMP((int)(y * ov_scale), 0, shot_h - 1);
    guint32 p;
    memcpy(&p, cairo_image_surface_get_data(shot) + iy * cairo_image_surface_get_stride(shot) + ix * 4, 4);
    *r = ((p >> 16) & 255) / 255.0; *g = ((p >> 8) & 255) / 255.0; *b = (p & 255) / 255.0;
}

static guint ov_grab_src;

static void ov_close(void) {
    if (ov_grab_src) { g_source_remove(ov_grab_src); ov_grab_src = 0; }
    gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
    gtk_widget_destroy(ov);
    ov = NULL;
}

static gboolean trim_memory(gpointer d) { malloc_trim(0); return G_SOURCE_REMOVE; }

static void ov_finish(gboolean ok) {
    OverlayDone done = ov_done;
    double r = 0, g = 0, b = 0;
    if (ok) shot_pixel(px, py, &r, &g, &b);
    ov_close();
    g_clear_pointer(&shot, cairo_surface_destroy);
    done(ok, r, g, b);
    g_timeout_add(500, trim_memory, NULL);   /* the frozen screen took a few MB: give them back */
}

static void hint(cairo_t *cr, int W, const char *text) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("sans 11");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, text, -1);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    double x = (W - tw) / 2.0 - 14, y = 28, w = tw + 28, h = th + 14, r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, G_PI / 2);
    cairo_arc(cr, x + r, y + r, r, G_PI / 2, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.09, 0.08, 0.12, 0.88);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.92, 0.91, 0.96);
    cairo_move_to(cr, x + 14, y + 7);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static gboolean ov_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_save(cr);
    cairo_scale(cr, 1 / ov_scale, 1 / ov_scale);
    cairo_set_source_surface(cr, shot, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_restore(cr);
    hint(cr, W, "Click to copy the colour · Esc cancels");
    if (px < 0) return TRUE;
    /* magnifier: 11 × 11 pixels around the pointer, 11 times bigger, with the colour's HEX underneath */
    const int N = 11, Z = 11, S = N * Z;
    double mx = px + 24, my = py + 24;
    if (mx + S > W) mx = px - 24 - S;
    if (my + S + 28 > H) my = py - 24 - S - 28;
    cairo_save(cr);
    cairo_rectangle(cr, mx, my, S, S);
    cairo_clip(cr);
    cairo_translate(cr, mx, my);
    cairo_scale(cr, Z, Z);   /* the picture is in screen pixels: each one becomes Z × Z */
    int cx = (int)(px * ov_scale), cy = (int)(py * ov_scale);
    cairo_set_source_surface(cr, shot, -(cx - N / 2), -(cy - N / 2));
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_rectangle(cr, mx + (N / 2) * Z + 0.5, my + (N / 2) * Z + 0.5, Z - 1, Z - 1);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_rectangle(cr, mx + 0.5, my + 0.5, S - 1, S - 1);
    { double ac[3]; sv_accent(ac); cairo_set_source_rgb(cr, ac[0], ac[1], ac[2]); }
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    double r, g, b;
    shot_pixel(px, py, &r, &g, &b);
    char hex[16];
    g_snprintf(hex, sizeof hex, "#%02X%02X%02X", (int)lround(r * 255), (int)lround(g * 255), (int)lround(b * 255));
    cairo_rectangle(cr, mx, my + S + 2, S, 24);
    cairo_set_source_rgba(cr, 0.09, 0.08, 0.12, 0.92);
    cairo_fill(cr);
    cairo_rectangle(cr, mx + 6, my + S + 8, 12, 12);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_fill(cr);
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("monospace bold 10");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, hex, -1);
    cairo_set_source_rgb(cr, 0.92, 0.91, 0.96);
    cairo_move_to(cr, mx + 26, my + S + 6);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return TRUE;
}

/* Redraw only what the pointer changed: the magnifier's old and new place. Repainting the whole frozen
 * screen on every mouse move is what makes such overlays feel heavy. */
static void ov_damage(GtkWidget *w, double x, double y) {
    const int R = 24 + 11 * 11 + 34;   /* the magnifier can be on any side of the pointer */
    gtk_widget_queue_draw_area(w, (int)x - R, (int)y - R, 2 * R, 2 * R);
}

static gboolean ov_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    if (px >= 0) ov_damage(w, px, py);
    px = e->x; py = e->y;
    ov_damage(w, px, py);
    return TRUE;
}

static gboolean ov_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (e->type != GDK_BUTTON_PRESS) return TRUE;
    px = e->x; py = e->y;
    ov_finish(e->button == 1);   /* right-click cancels */
    return TRUE;
}

static gboolean ov_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (e->keyval == GDK_KEY_Escape) ov_finish(FALSE);
    return TRUE;
}

static int ov_grab_tries;

/* The shortcut's own key grab is released a moment after you press it: keep trying for 1.5 s. */
static gboolean ov_grab(gpointer d) {
    if (!ov) { ov_grab_src = 0; return G_SOURCE_REMOVE; }
    GdkDisplay *disp = gdk_display_get_default();
    GdkCursor *c = gdk_cursor_new_from_name(disp, "crosshair");
    GdkGrabStatus st = gdk_seat_grab(gdk_display_get_default_seat(disp), gtk_widget_get_window(ov),
                                     GDK_SEAT_CAPABILITY_ALL, FALSE, c, NULL, NULL, NULL);
    if (c) g_object_unref(c);
    if (st != GDK_GRAB_SUCCESS && ++ov_grab_tries < 50) return G_SOURCE_CONTINUE;
    ov_grab_src = 0;
    if (st != GDK_GRAB_SUCCESS) {
        logmsg("couldn't take the mouse and keyboard (%d)", st);
        ov_finish(FALSE);
        toast("Couldn't start", "Another app is holding the mouse or keyboard. Close its menu and try again.");
    }
    return G_SOURCE_REMOVE;
}

/* Freeze the screen and let you click a colour. X11 only. */
static gboolean overlay_start(OverlayDone done) {
    GdkWindow *root = gdk_get_default_root_window();
    int W = gdk_window_get_width(root), H = gdk_window_get_height(root);
    GdkPixbuf *pb = gdk_pixbuf_get_from_window(root, 0, 0, W, H);
    if (!pb) return FALSE;
    shot_w = gdk_pixbuf_get_width(pb); shot_h = gdk_pixbuf_get_height(pb);
    shot = cairo_image_surface_create(CAIRO_FORMAT_RGB24, shot_w, shot_h);   /* once: drawing a pixbuf converts it every time */
    cairo_t *cr = cairo_create(shot);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_object_unref(pb);
    cairo_surface_flush(shot);
    ov_scale = (double)shot_w / W;
    ov_done = done; px = py = -1;
    ov = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_move(GTK_WINDOW(ov), 0, 0);
    gtk_widget_set_size_request(ov, W, H);
    gtk_widget_set_app_paintable(ov, TRUE);
    gtk_widget_add_events(ov, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(ov, "draw", G_CALLBACK(ov_draw), NULL);
    g_signal_connect(ov, "motion-notify-event", G_CALLBACK(ov_motion), NULL);
    g_signal_connect(ov, "button-press-event", G_CALLBACK(ov_press), NULL);
    g_signal_connect(ov, "key-press-event", G_CALLBACK(ov_key), NULL);
    gtk_widget_show(ov);
    ov_grab_tries = 0;
    ov_grab_src = g_timeout_add(30, ov_grab, NULL);   /* once the window is on screen */
    return TRUE;
}

/* ================================================================== colour picker */

static GtkWidget *cwin, *crows[3], *clabels[3];
static double cr_, cg_, cb_;
static gboolean c_had_focus;

static void color_formats(double r, double g, double b, char out[3][40]) {
    int R = (int)lround(r * 255), G = (int)lround(g * 255), B = (int)lround(b * 255);
    double mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), l = (mx + mn) / 2, h = 0, s = 0, dd = mx - mn;
    if (dd > 1e-9) {
        s = l > 0.5 ? dd / (2 - mx - mn) : dd / (mx + mn);
        h = mx == r ? (g - b) / dd + (g < b ? 6 : 0) : mx == g ? (b - r) / dd + 2 : (r - g) / dd + 4;
        h *= 60;
    }
    g_snprintf(out[0], 40, "#%02X%02X%02X", R, G, B);
    g_snprintf(out[1], 40, "rgb(%d, %d, %d)", R, G, B);
    g_snprintf(out[2], 40, "hsl(%d, %d%%, %d%%)", (int)lround(h) % 360, (int)lround(s * 100), (int)lround(l * 100));
}

static void copy_text(const char *t) { gtk_clipboard_set_text(clipboard(), t, -1); }

static void on_color_row(GtkButton *b, gpointer i) {
    char f[3][40];
    color_formats(cr_, cg_, cb_, f);
    copy_text(f[GPOINTER_TO_INT(i)]);
    for (int k = 0; k < 3; k++) {
        char *t = g_strdup_printf("%s%s", f[k], k == GPOINTER_TO_INT(i) ? "   ✓ copied" : "");
        gtk_label_set_text(GTK_LABEL(clabels[k]), t);
        g_free(t);
    }
}

static gboolean swatch_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double r = 10;
    cairo_new_sub_path(cr);
    cairo_arc(cr, W - r, r, r, -G_PI / 2, 0); cairo_arc(cr, W - r, H - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, H - r, r, G_PI / 2, G_PI); cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, cr_, cg_, cb_);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.35);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return TRUE;
}

static gboolean cwin_hide_if_inactive(gpointer d) {
    if (cwin && gtk_widget_get_visible(cwin) && !gtk_window_is_active(GTK_WINDOW(cwin))) gtk_widget_hide(cwin);
    return G_SOURCE_REMOVE;
}
static gboolean cwin_focus_in(GtkWidget *w, GdkEvent *e, gpointer d) { c_had_focus = TRUE; return FALSE; }
static gboolean cwin_focus_out(GtkWidget *w, GdkEvent *e, gpointer d) {
    if (c_had_focus) g_timeout_add(150, cwin_hide_if_inactive, NULL);
    return FALSE;
}
static gboolean cwin_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (e->keyval == GDK_KEY_Escape) { gtk_widget_hide(cwin); return TRUE; }
    return FALSE;
}
static gboolean snapshot_of(GtkWidget *w, const char *path) {   /* SUPERV_EXTRAS_SNAPSHOT=file.png: for screenshots */
    if (!w || !gtk_widget_get_visible(w)) return G_SOURCE_REMOVE;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gtk_widget_get_allocated_width(w),
                                                    gtk_widget_get_allocated_height(w));
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(w, cr);
    cairo_destroy(cr);
    if (cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS) logmsg("saved %s", path);
    cairo_surface_destroy(s);
    return G_SOURCE_REMOVE;
}
static gboolean cwin_snapshot(gpointer path) { return snapshot_of(cwin, path); }
static GtkWidget *iwin;
static gboolean card_snapshot(gpointer path) { return snapshot_of(iwin, path); }

/* The colour is copied as HEX; this small card shows it, with RGB and HSL one click away. */
/* Drag the card by any part that isn't a button. */
static gboolean card_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS) return FALSE;
    gtk_window_begin_move_drag(GTK_WINDOW(w), e->button, (int)e->x_root, (int)e->y_root, e->time);
    return TRUE;
}

/* Put a small window next to (x, y), on that monitor, flipped so it stays on screen. */
static void place_near(GtkWidget *w, int x, int y) {
    GtkRequisition nat;
    gtk_widget_get_preferred_size(w, NULL, &nat);
    GdkRectangle a;
    GdkMonitor *m = gdk_display_get_monitor_at_point(gdk_display_get_default(), x, y);
    if (m) gdk_monitor_get_workarea(m, &a); else work_area(&a);
    int nx = x + 18 + nat.width <= a.x + a.width ? x + 18 : x - 18 - nat.width;
    int ny = y + 18 + nat.height <= a.y + a.height ? y + 18 : y - 18 - nat.height;
    gtk_window_move(GTK_WINDOW(w), CLAMP(nx, a.x, MAX(a.x, a.x + a.width - nat.width)),
                    CLAMP(ny, a.y, MAX(a.y, a.y + a.height - nat.height)));
}

/* ---- Where is the pointer? On Wayland, X11 apps only learn it when the pointer is over one of their windows,
 * so cover the screen with an invisible window for a moment: the desktop then tells us where the pointer is. */
typedef void (*PointerDone)(gboolean known, int x, int y, gpointer data);
static GtkWidget *probe;
static PointerDone probe_done;
static gpointer probe_data;
static gboolean probe_known;
static int probe_x, probe_y;

static gboolean probe_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d) {
    probe_known = TRUE; probe_x = (int)e->x_root; probe_y = (int)e->y_root;
    return FALSE;
}
static gboolean probe_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    probe_known = TRUE; probe_x = (int)e->x_root; probe_y = (int)e->y_root;
    return FALSE;
}
static gboolean probe_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    return TRUE;
}
static gboolean probe_end(gpointer d) {
    gtk_widget_destroy(probe);
    probe = NULL;
    probe_done(probe_known, probe_x, probe_y, probe_data);
    return G_SOURCE_REMOVE;
}

static void find_pointer(PointerDone done, gpointer data) {
    probe_done = done; probe_data = data; probe_known = FALSE;
    if (!sv_wayland) {   /* X11: just ask */
        gdk_device_get_position(gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())),
                                NULL, &probe_x, &probe_y);
        done(TRUE, probe_x, probe_y, data);
        return;
    }
    GdkScreen *scr = gdk_screen_get_default();
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (!vis) { done(FALSE, 0, 0, data); return; }
    GdkWindow *root = gdk_get_default_root_window();
    probe = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_widget_set_visual(probe, vis);
    gtk_widget_set_app_paintable(probe, TRUE);
    gtk_window_move(GTK_WINDOW(probe), 0, 0);
    gtk_widget_set_size_request(probe, gdk_window_get_width(root), gdk_window_get_height(root));
    gtk_widget_add_events(probe, GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(probe, "draw", G_CALLBACK(probe_draw), NULL);
    g_signal_connect(probe, "enter-notify-event", G_CALLBACK(probe_enter), NULL);
    g_signal_connect(probe, "motion-notify-event", G_CALLBACK(probe_motion), NULL);
    gtk_widget_show(probe);
    g_timeout_add(120, probe_end, NULL);
}

static void color_show(double r, double g, double b, gboolean known, int x, int y) {
    cr_ = r; cg_ = g; cb_ = b;
    char f[3][40];
    color_formats(r, g, b, f);
    copy_text(f[0]);
    if (!cwin) {
        cwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        GtkWindow *w = GTK_WINDOW(cwin);
        gtk_window_set_title(w, "Colour");
        gtk_window_set_decorated(w, FALSE);
        gtk_window_set_resizable(w, FALSE);
        gtk_window_set_keep_above(w, TRUE);
        gtk_window_set_skip_taskbar_hint(w, TRUE);
        gtk_window_set_skip_pager_hint(w, TRUE);
        gtk_window_set_type_hint(w, GDK_WINDOW_TYPE_HINT_DIALOG);
        gtk_window_set_icon_name(w, SV_APP_ID);
        sv_style_window(cwin);
        GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_style_context_add_class(gtk_widget_get_style_context(panel), "cbh-panel");
        GtkWidget *sw = gtk_drawing_area_new();
        gtk_widget_set_size_request(sw, 64, 64);
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        g_signal_connect(sw, "draw", G_CALLBACK(swatch_draw), NULL);
        gtk_box_pack_start(GTK_BOX(panel), sw, FALSE, FALSE, 0);
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *t = gtk_label_new("Colour copied");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_widget_set_halign(t, GTK_ALIGN_START);
        gtk_widget_set_margin_start(t, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(t), "cbh-title");
        gtk_box_pack_start(GTK_BOX(col), t, FALSE, FALSE, 2);
        for (int k = 0; k < 3; k++) {
            crows[k] = gtk_button_new();
            clabels[k] = gtk_label_new("");
            gtk_label_set_xalign(GTK_LABEL(clabels[k]), 0);
            gtk_container_add(GTK_CONTAINER(crows[k]), clabels[k]);
            gtk_style_context_add_class(gtk_widget_get_style_context(crows[k]), "cbh-menuitem");
            gtk_widget_set_tooltip_text(crows[k], "Click to copy this one instead");
            g_signal_connect(crows[k], "clicked", G_CALLBACK(on_color_row), GINT_TO_POINTER(k));
            gtk_box_pack_start(GTK_BOX(col), crows[k], FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(panel), col, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(cwin), panel);
        g_signal_connect(cwin, "focus-in-event", G_CALLBACK(cwin_focus_in), NULL);
        g_signal_connect(cwin, "focus-out-event", G_CALLBACK(cwin_focus_out), NULL);
        g_signal_connect(cwin, "key-press-event", G_CALLBACK(cwin_key), NULL);
        gtk_widget_add_events(cwin, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(cwin, "button-press-event", G_CALLBACK(card_press), NULL);
        gtk_widget_set_tooltip_text(cwin, NULL);
        g_signal_connect(cwin, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
        gtk_widget_show_all(panel);
    }
    sv_style_window(cwin);   /* the theme may have changed */
    for (int k = 0; k < 3; k++) {
        char *t = g_strdup_printf("%s%s", f[k], k == 0 ? "   ✓ copied" : "");
        gtk_label_set_text(GTK_LABEL(clabels[k]), t);
        g_free(t);
    }
    gtk_widget_queue_draw(cwin);
    c_had_focus = FALSE;
    if (known) {   /* next to where you clicked */
        gtk_window_set_position(GTK_WINDOW(cwin), GTK_WIN_POS_NONE);
        place_near(cwin, x, y);
    } else {
        gtk_window_set_position(GTK_WINDOW(cwin), GTK_WIN_POS_CENTER_ALWAYS);
    }
    gtk_widget_show(cwin);
    gtk_window_present(GTK_WINDOW(cwin));
    const char *snap = g_getenv("SUPERV_EXTRAS_SNAPSHOT");
    if (snap && *snap) g_timeout_add(600, cwin_snapshot, (gpointer)snap);
    logmsg("colour picked: %s", f[0]);
}

static void color_from_overlay(gboolean ok, double r, double g, double b) {
    if (ok) color_show(r, g, b, TRUE, (int)px, (int)py);   /* the overlay covered the screen from 0, 0 */
}

static void color_at_pointer(gboolean known, int x, int y, gpointer c) {
    double *rgb = c;
    color_show(rgb[0], rgb[1], rgb[2], known, x, y);
    g_free(rgb);
}

static void color_from_portal(int status, GVariant *results, gpointer d) {
    double r, g, b;
    if (status == PORTAL_OK && g_variant_lookup(results, "color", "(ddd)", &r, &g, &b)) {
        double *c = g_new(double, 3);
        c[0] = r; c[1] = g; c[2] = b;
        find_pointer(color_at_pointer, c);   /* the card opens next to where you clicked */
    }
    else if (status == PORTAL_FAILED)
        notify("Colour picking isn't available here",
               "Your desktop doesn't offer a colour picker to apps (xdg-desktop-portal). GNOME and KDE Plasma do.");
}

/* Our own toast or colour card must not be in the picture: hide them, and on X11 give the screen a moment
 * to repaint before it is read. */
static guint clear_src;

static void clear_screen_then(GSourceFunc go) {
    gboolean was = (toast_win && gtk_widget_get_visible(toast_win)) || (cwin && gtk_widget_get_visible(cwin));
    if (toast_win) { if (toast_src) { g_source_remove(toast_src); toast_src = 0; } gtk_widget_hide(toast_win); }
    if (cwin) gtk_widget_hide(cwin);
    if (!go) return;
    if (was && !sv_wayland) clear_src = g_timeout_add(60, go, NULL);
    else go(NULL);
}

static gboolean pick_color_go(gpointer d) {
    clear_src = 0;
    if (!sv_wayland && overlay_start(color_from_overlay)) return G_SOURCE_REMOVE;
    portal_call("PickColor", g_variant_builder_new(G_VARIANT_TYPE("a{sv}")), color_from_portal, NULL);
    return G_SOURCE_REMOVE;
}

static void pick_color(void) {
    if (ov || portal_pending || clear_src || shot_busy()) return;
    clear_screen_then(pick_color_go);
}

/* ================================================================== copy text from the screen */

static GSubprocess *ocr_proc;
static char *ocr_file;

gboolean extras_ocr_available(void) {
    char *p = g_find_program_in_path("tesseract");
    gboolean ok = p != NULL;
    g_free(p);
    return ok;
}

/* English plus your own languages, when their Tesseract data is installed (e.g. bn_BD → "ben") */
static char *ocr_languages(void) {
    static char *langs;
    if (langs) return langs;
    static const char *map[][2] = {
        { "ar", "ara" }, { "bn", "ben" }, { "cs", "ces" }, { "da", "dan" }, { "de", "deu" }, { "el", "ell" },
        { "es", "spa" }, { "fa", "fas" }, { "fi", "fin" }, { "fr", "fra" }, { "he", "heb" }, { "hi", "hin" },
        { "hu", "hun" }, { "id", "ind" }, { "it", "ita" }, { "ja", "jpn" }, { "ko", "kor" }, { "nl", "nld" },
        { "no", "nor" }, { "nb", "nor" }, { "pl", "pol" }, { "pt", "por" }, { "ro", "ron" }, { "ru", "rus" },
        { "sv", "swe" }, { "ta", "tam" }, { "th", "tha" }, { "tr", "tur" }, { "uk", "ukr" }, { "ur", "urd" },
        { "vi", "vie" }, { "zh_TW", "chi_tra" }, { "zh", "chi_sim" },
    };
    char *out = NULL;
    char *argv[] = { "tesseract", "--list-langs", NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL);
    char **have = g_strsplit(out ? out : "", "\n", -1);
    GString *l = g_string_new("eng");
    const char *const *names = g_get_language_names();
    for (int i = 0; names[i]; i++)
        for (guint k = 0; k < G_N_ELEMENTS(map); k++)
            if (g_str_has_prefix(names[i], map[k][0]) && g_strv_contains((const char *const *)have, map[k][1])
                && !strstr(l->str, map[k][1])) {
                g_string_append_printf(l, "+%s", map[k][1]);
                break;
            }
    g_strfreev(have); g_free(out);
    langs = g_string_free(l, FALSE);
    return langs;
}

static void ocr_done(GObject *src, GAsyncResult *res, gpointer d) {
    char *out = NULL;
    GError *e = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &e);
    if (ok && !g_subprocess_get_successful(G_SUBPROCESS(src))) {
        ok = FALSE;
        g_set_error(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "Tesseract stopped with an error. Its language data may be "
                    "missing: install it again (tesseract-ocr-eng, or tesseract-data-eng).");
        g_clear_pointer(&out, g_free);
    }
    g_clear_object(&ocr_proc);
    if (ocr_file) { g_unlink(ocr_file); g_clear_pointer(&ocr_file, g_free); }
    g_timeout_add(500, trim_memory, NULL);
    if (!ok) {
        logmsg("text recognition failed: %s", e->message);
        notify("Couldn't read the text", e->message);
        g_error_free(e);
        return;
    }
    /* Tesseract ends pages with a form feed and adds blank lines: keep only the text */
    if (out) { g_strdelimit(out, "\f", ' '); g_strstrip(out); }
    if (!out || !*out) {
        notify("No text found", "Try selecting a bigger area, or text with more contrast.");
    } else {
        copy_text(out);
        char *preview = g_utf8_substring(out, 0, MIN(g_utf8_strlen(out, -1), 120));
        notify("Text copied", preview);
        logmsg("copied %ld characters of text from the screen", g_utf8_strlen(out, -1));
        g_free(preview);
    }
    g_free(out);
}

/* ---- installing Tesseract (text recognition), once, with the desktop's own password dialog */
static void extract_text(void);
static GtkWidget *ibody, *ibtn, *icancel;
static gboolean installing;

/* the package manager's install command, and the same thing to type in a terminal */
static gboolean install_command(char ***argv, const char **typed) {
    static char *apt[] = { "pkexec", "env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "tesseract-ocr", NULL };
    static char *dnf[] = { "pkexec", "dnf", "install", "-y", "tesseract", NULL };
    static char *pac[] = { "pkexec", "pacman", "-S", "--noconfirm", "--needed", "tesseract", "tesseract-data-eng", NULL };
    static char *zyp[] = { "pkexec", "zypper", "--non-interactive", "install", "tesseract-ocr", "tesseract-ocr-traineddata-english", NULL };
    static const struct { const char *tool; char **argv; const char *typed; } PM[] = {
        { "apt-get", apt, "sudo apt install tesseract-ocr" },
        { "dnf", dnf, "sudo dnf install tesseract" },
        { "pacman", pac, "sudo pacman -S tesseract tesseract-data-eng" },
        { "zypper", zyp, "sudo zypper install tesseract-ocr tesseract-ocr-traineddata-english" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(PM); i++) {
        char *p = g_find_program_in_path(PM[i].tool);
        if (!p) continue;
        g_free(p);
        *argv = PM[i].argv; *typed = PM[i].typed;
        p = g_find_program_in_path("pkexec");
        gboolean ok = p != NULL;
        g_free(p);
        return ok;
    }
    *argv = NULL; *typed = "install tesseract with your package manager";
    return FALSE;
}

static gboolean start_capture(gpointer d) { extract_text(); return G_SOURCE_REMOVE; }

static void on_installed(GPid pid, gint status, gpointer d) {
    g_spawn_close_pid(pid);
    installing = FALSE;
    gtk_widget_set_sensitive(icancel, TRUE);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && extras_ocr_available()) {
        logmsg("Tesseract installed");
        gtk_widget_hide(iwin);
        toast("Tesseract is installed", "Now select the area with the text.");
        g_timeout_add(900, start_capture, NULL);   /* what you pressed Super+Shift+T for */
        return;
    }
    char **argv; const char *typed;
    install_command(&argv, &typed);
    char *t = g_strdup_printf("It wasn't installed: the password dialog was closed, or another installer is busy "
                              "(close Software Updater and try again). In a terminal: %s", typed);
    gtk_label_set_text(GTK_LABEL(ibody), t);
    g_free(t);
    gtk_button_set_label(GTK_BUTTON(ibtn), "Try again");
    gtk_widget_set_sensitive(ibtn, TRUE);
}

static void on_install(GtkButton *b, gpointer d) {
    char **argv; const char *typed;
    if (!install_command(&argv, &typed)) {   /* no pkexec or unknown package manager: copy the command instead */
        gtk_clipboard_set_text(clipboard(), typed, -1);
        char *t = g_strdup_printf("Copied the command to your clipboard: paste it into a terminal.\n%s", typed);
        gtk_label_set_text(GTK_LABEL(ibody), t);
        g_free(t);
        return;
    }
    GPid pid;
    GError *e = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
                       NULL, NULL, &pid, &e)) {
        gtk_label_set_text(GTK_LABEL(ibody), e->message);
        g_error_free(e);
        return;
    }
    installing = TRUE;
    gtk_widget_set_sensitive(ibtn, FALSE);
    gtk_widget_set_sensitive(icancel, FALSE);
    gtk_button_set_label(GTK_BUTTON(ibtn), "Installing…");
    gtk_label_set_text(GTK_LABEL(ibody), "Enter your password in the dialog. Downloading and installing takes a moment "
                                         "(about 10 MB).");
    g_child_watch_add(pid, on_installed, NULL);
}

static void on_install_cancel(GtkButton *b, gpointer d) { gtk_widget_hide(iwin); }
static gboolean iwin_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (e->keyval == GDK_KEY_Escape && !installing) { gtk_widget_hide(iwin); return TRUE; }
    return FALSE;
}

/* "Copying text needs Tesseract" with an Install button. Shown instead of a notification you might not see. */
static void ask_install(void) {
    if (!iwin) {
        iwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        GtkWindow *w = GTK_WINDOW(iwin);
        gtk_window_set_title(w, "Copy text from the screen");
        gtk_window_set_decorated(w, FALSE);
        gtk_window_set_resizable(w, FALSE);
        gtk_window_set_keep_above(w, TRUE);
        gtk_window_set_skip_taskbar_hint(w, TRUE);
        gtk_window_set_type_hint(w, GDK_WINDOW_TYPE_HINT_DIALOG);
        gtk_window_set_position(w, GTK_WIN_POS_CENTER_ALWAYS);
        gtk_window_set_icon_name(w, SV_APP_ID);
        sv_style_window(iwin);
        GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(panel), "cbh-panel");
        GtkWidget *t = gtk_label_new("Copying text needs Tesseract");
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(t), "cbh-title");
        ibody = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ibody), 0);
        gtk_label_set_line_wrap(GTK_LABEL(ibody), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(ibody), 44);
        gtk_style_context_add_class(gtk_widget_get_style_context(ibody), "cbh-sub");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        icancel = gtk_button_new_with_label("Not now");
        gtk_style_context_add_class(gtk_widget_get_style_context(icancel), "cbh-flat");
        ibtn = gtk_button_new_with_label("Install Tesseract");
        gtk_style_context_add_class(gtk_widget_get_style_context(ibtn), "cbh-primary");
        g_signal_connect(icancel, "clicked", G_CALLBACK(on_install_cancel), NULL);
        g_signal_connect(ibtn, "clicked", G_CALLBACK(on_install), NULL);
        gtk_box_pack_end(GTK_BOX(row), ibtn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(row), icancel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(panel), t, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(panel), ibody, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(panel), row, FALSE, FALSE, 4);
        gtk_container_add(GTK_CONTAINER(iwin), panel);
        g_signal_connect(iwin, "key-press-event", G_CALLBACK(iwin_key), NULL);
        gtk_widget_add_events(iwin, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(iwin, "button-press-event", G_CALLBACK(card_press), NULL);
        g_signal_connect(iwin, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
        gtk_widget_show_all(panel);
    }
    if (installing) { gtk_window_present(GTK_WINDOW(iwin)); return; }
    sv_style_window(iwin);
    gtk_label_set_text(GTK_LABEL(ibody), "SuperV reads the text with Tesseract, a free text-recognition program that "
                                         "works on your computer only. It isn't installed yet: install it once and "
                                         "Super+Shift+T works from then on.");
    gtk_button_set_label(GTK_BUTTON(ibtn), "Install Tesseract");
    gtk_widget_set_sensitive(ibtn, TRUE);
    gtk_widget_show(iwin);
    gtk_window_present(GTK_WINDOW(iwin));
    const char *snap = g_getenv("SUPERV_EXTRAS_SNAPSHOT");
    if (snap && *snap) {
        char *p = g_strdup_printf("%s.install.png", snap);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 700, card_snapshot, p, g_free);
    }
}

static void ocr(GdkPixbuf *pb) {
    if (!extras_ocr_available()) { ask_install(); return; }
    /* small text reads better a bit bigger */
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    double k = h < 150 ? MIN(3.0, 150.0 / h) : 1.0;
    GdkPixbuf *big = k > 1 ? gdk_pixbuf_scale_simple(pb, (int)(w * k), (int)(h * k), GDK_INTERP_BILINEAR) : g_object_ref(pb);
    g_free(ocr_file);
    ocr_file = g_strdup_printf("%s/superv-text-%d.png", g_get_user_runtime_dir(), getpid());
    gboolean saved = gdk_pixbuf_save(big, ocr_file, "png", NULL, "compression", "1", NULL);   /* read once: write it fast */
    g_object_unref(big);
    if (!saved) { notify("Couldn't read the text", "The picture couldn't be saved for reading."); return; }
    g_chmod(ocr_file, 0600);
    GError *e = NULL;
    ocr_proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &e,
                                "tesseract", ocr_file, "stdout", "-l", ocr_languages(), NULL);
    if (!ocr_proc) {
        notify("Couldn't read the text", e->message);
        g_error_free(e);
        g_unlink(ocr_file); g_clear_pointer(&ocr_file, g_free);
        return;
    }
    g_subprocess_communicate_utf8_async(ocr_proc, NULL, NULL, ocr_done, NULL);
}

static gboolean is_gnome_shot(GdkPixbuf *pb) {   /* GNOME tags the screenshots it saves */
    return !g_strcmp0(gdk_pixbuf_get_option(pb, "tEXt::Software"), "gnome-screenshot");
}

/* The desktop saved a screenshot file only for us: load it, and don't leave it in your Pictures folder.
 * It is ours if GNOME tagged it, if it has a screenshot's name, or if it is in a temporary folder. */
static GdkPixbuf *load_portal_shot(GVariant *results) {
    const char *uri = NULL;
    if (!g_variant_lookup(results, "uri", "&s", &uri)) return NULL;
    char *path = g_filename_from_uri(uri, NULL, NULL);
    GdkPixbuf *pb = path ? gdk_pixbuf_new_from_file(path, NULL) : NULL;
    GStatBuf st;
    if (pb && g_stat(path, &st) == 0 && time(NULL) - st.st_mtime < 120) {
        char *base = g_path_get_basename(path);
        if (is_gnome_shot(pb) || g_str_has_prefix(base, "Screenshot") || g_str_has_prefix(base, "screenshot")
            || g_str_has_prefix(path, g_get_tmp_dir()) || g_str_has_prefix(path, g_get_user_runtime_dir()))
            g_unlink(path);
        g_free(base);
    }
    g_free(path);
    return pb;
}

/* Super+Shift+T: the screen freezes, you drag over the text, it is read and copied. The frozen screen and the
 * selection are the screenshot tool's (shot.c), so it looks and works the same on every desktop, and on GNOME
 * no dialog is shown. */
static void extract_text(void) {
    if (ov || portal_pending || ocr_proc || clear_src || shot_busy()) return;
    if (!extras_ocr_available()) { ocr(NULL); return; }   /* says how to install it */
    shot_text();
}

/* ================================================================== for the screenshot tool (shot.c) */

typedef struct { SvShotDone done; gpointer data; } CaptureReq;

static void capture_from_portal(int status, GVariant *results, gpointer p) {
    CaptureReq *c = p;
    GdkPixbuf *pb = status == PORTAL_OK ? load_portal_shot(results) : NULL;
    if (status == PORTAL_FAILED)
        toast("Screenshots aren't available here",
              "Your desktop doesn't offer screenshots to apps (xdg-desktop-portal). GNOME and KDE Plasma do.");
    c->done(pb, c->data);
    if (pb) g_object_unref(pb);
    g_free(c);
}

/* ---- GNOME on Wayland: apps must ask for every screenshot ("Share screenshot?"), but GNOME's own Shift+Print
 * takes one at once. So press it for you, take the file GNOME saves, and tidy up: the file is deleted and
 * what you had copied comes back (GNOME also puts the picture on the clipboard). If GNOME doesn't answer in a
 * few seconds, ask the portal instead. */
#define GNOME_DIRS 4
static GFileMonitor *gmon[GNOME_DIRS];
static char *gdir[GNOME_DIRS];
static guint g_send_src, g_resend_src, g_fail_src, g_linger_src;
static gint64 g_pressed;    /* when Shift+Print was pressed for you (real time, µs); 0: not yet */
static gboolean g_seen;     /* a new picture showed up: GNOME got the key press */
static CaptureReq *g_req;
static char *g_combo;

static char *gnome_shot_combo(void) {   /* GNOME's "screenshot" shortcut as "shift+print", if it has one */
    if (!sv_wayland) return NULL;
    const char *de = g_getenv("XDG_CURRENT_DESKTOP");
    if (!de || !strstr(de, "GNOME")) return NULL;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *sc = src ? g_settings_schema_source_lookup(src, "org.gnome.shell.keybindings", TRUE) : NULL;
    if (!sc || !g_settings_schema_has_key(sc, "screenshot")) { if (sc) g_settings_schema_unref(sc); return NULL; }
    g_settings_schema_unref(sc);
    GSettings *s = g_settings_new("org.gnome.shell.keybindings");
    char **v = g_settings_get_strv(s, "screenshot"), *out = NULL;
    for (int i = 0; v[i] && !out; i++) {
        guint key; GdkModifierType mods;
        gtk_accelerator_parse(v[i], &key, &mods);
        if (key != GDK_KEY_Print) continue;
        GString *c = g_string_new(NULL);
        if (mods & GDK_CONTROL_MASK) g_string_append(c, "ctrl+");
        if (mods & GDK_MOD1_MASK) g_string_append(c, "alt+");
        if (mods & GDK_SHIFT_MASK) g_string_append(c, "shift+");
        if (mods & (GDK_SUPER_MASK | GDK_MOD4_MASK)) g_string_append(c, "super+");
        g_string_append(c, "print");
        out = g_string_free(c, FALSE);
    }
    g_strfreev(v); g_object_unref(s);
    return out;
}

static void gnome_stop_timers(void) {
    if (g_send_src) { g_source_remove(g_send_src); g_send_src = 0; }
    if (g_resend_src) { g_source_remove(g_resend_src); g_resend_src = 0; }
    if (g_fail_src) { g_source_remove(g_fail_src); g_fail_src = 0; }
    g_clear_pointer(&g_combo, g_free);
}

static void gnome_unwatch(void) {
    if (g_linger_src) { g_source_remove(g_linger_src); g_linger_src = 0; }
    for (int i = 0; i < GNOME_DIRS; i++) { g_clear_object(&gmon[i]); g_clear_pointer(&gdir[i], g_free); }
}

static gboolean gnome_linger_end(gpointer d) { g_linger_src = 0; gnome_unwatch(); return G_SOURCE_REMOVE; }
static gboolean guard_off(gpointer d) { sv_clipboard_guard(FALSE); return G_SOURCE_REMOVE; }

static void gnome_done(GdkPixbuf *pb) {
    CaptureReq *c = g_req;
    g_req = NULL;
    gnome_stop_timers();
    portal_pending--;
    g_timeout_add(400, guard_off, NULL);   /* after GNOME's own clipboard copy has arrived */
    /* keep watching a little: if both key presses reached GNOME, the second picture is removed too */
    g_linger_src = g_timeout_add(2500, gnome_linger_end, NULL);
    c->done(pb, c->data);
    g_free(c);
}

static void on_shot_file(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer d);

static void gnome_watch(const char *dir) {   /* a folder GNOME may save into; nothing is created */
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) return;
    for (int i = 0; i < GNOME_DIRS; i++) {
        if (gdir[i]) { if (!strcmp(gdir[i], dir)) return; else continue; }
        GFile *f = g_file_new_for_path(dir);
        gmon[i] = g_file_monitor_directory(f, G_FILE_MONITOR_NONE, NULL, NULL);
        g_object_unref(f);
        if (!gmon[i]) return;
        gdir[i] = g_strdup(dir);
        g_signal_connect(gmon[i], "changed", G_CALLBACK(on_shot_file), NULL);
        return;
    }
}

/* A file appeared in a watched folder. Only a picture GNOME itself made after our key press is taken:
 * never something you saved there yourself. */
static void gnome_file(const char *path, gboolean deep) {
    if (!g_pressed) return;
    if (deep && g_file_test(path, G_FILE_TEST_IS_DIR)) {   /* GNOME made its Screenshots folder just now */
        gnome_watch(path);
        GDir *dir = g_dir_open(path, 0, NULL);
        const char *name;
        while (dir && (g_req || g_linger_src) && (name = g_dir_read_name(dir))) {
            char *child = g_build_filename(path, name, NULL);
            gnome_file(child, FALSE);
            g_free(child);
        }
        if (dir) g_dir_close(dir);
        return;
    }
    GStatBuf st;
    if (!g_str_has_suffix(path, ".png") || g_stat(path, &st) != 0) return;
    gint64 made = (gint64)st.st_mtim.tv_sec * G_USEC_PER_SEC + st.st_mtim.tv_nsec / 1000;
    if (made < g_pressed - 100000) return;   /* it was there before */
    g_seen = TRUE;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);   /* NULL while GNOME is still writing it */
    if (!pb) return;
    if (is_gnome_shot(pb)) {
        g_unlink(path);   /* GNOME saved it only because we asked: don't leave it in your Pictures */
        if (g_req) {
            logmsg("took GNOME's screenshot (%d × %d)", gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
            gnome_done(pb);
        } else {
            logmsg("removed a second screenshot GNOME saved");
        }
    }
    g_object_unref(pb);
}

static void on_shot_file(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer d) {
    if ((!g_req && !g_linger_src) || (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && ev != G_FILE_MONITOR_EVENT_CREATED)) return;
    char *path = g_file_get_path(f);
    if (path) gnome_file(path, TRUE);
    g_free(path);
}

static gboolean gnome_press(gpointer d) {
    g_send_src = 0;
    g_pressed = g_get_real_time();
    if (!sv_send_keys(g_combo, FALSE)) logmsg("couldn't press %s", g_combo);
    return G_SOURCE_REMOVE;
}

static gboolean gnome_press_again(gpointer d) {   /* you were still holding Super+Shift the first time */
    g_resend_src = 0;
    if (g_req && g_combo && !g_seen) sv_send_keys(g_combo, FALSE);
    return G_SOURCE_REMOVE;
}

static gboolean gnome_give_up(gpointer d) {
    g_fail_src = 0;
    if (!g_req) return G_SOURCE_REMOVE;
    logmsg("GNOME didn't save a screenshot: asking the desktop portal instead");
    CaptureReq *c = g_req;
    g_req = NULL;
    gnome_stop_timers();
    gnome_unwatch();   /* the portal may save its picture into the same folder: that one is not to be removed */
    portal_pending--;
    sv_clipboard_guard(FALSE);
    GVariantBuilder *o = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(o, "{sv}", "interactive", g_variant_new_boolean(FALSE));
    portal_call("Screenshot", o, capture_from_portal, c);
    return G_SOURCE_REMOVE;
}

static gboolean gnome_capture(CaptureReq *c) {
    char *combo = gnome_shot_combo();
    if (!combo || !sv_paste_everywhere()) { g_free(combo); return FALSE; }   /* no shortcut, or can't press keys */
    gnome_unwatch();
    /* GNOME saves into Pictures/Screenshots, named in your language; older versions straight into Pictures */
    const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (!pics) pics = g_get_home_dir();
    char *en = g_build_filename(pics, "Screenshots", NULL);
    char *own = g_build_filename(pics, g_dgettext("gnome-shell", "Screenshots"), NULL);
    gnome_watch(pics); gnome_watch(en); gnome_watch(own);
    g_free(en); g_free(own);
    g_req = c; g_combo = combo;
    g_pressed = 0; g_seen = FALSE;
    portal_pending++;   /* busy */
    sv_clipboard_guard(TRUE);
    /* the shortcut you pressed (Super+Shift+S) must be let go first, or GNOME sees Super+Shift+Print */
    g_send_src = g_timeout_add(450, gnome_press, NULL);
    g_resend_src = g_timeout_add(1700, gnome_press_again, NULL);
    g_fail_src = g_timeout_add(4500, gnome_give_up, NULL);
    return TRUE;
}

/* The whole screen. X11: read it. GNOME on Wayland: GNOME's own instant screenshot. Other Wayland desktops:
 * ask through the portal (it may show a dialog). */
static CaptureReq *x11_req;

static gboolean capture_x11(gpointer d) {
    clear_src = 0;
    CaptureReq *c = x11_req;
    x11_req = NULL;
    GdkWindow *root = gdk_get_default_root_window();
    GdkPixbuf *pb = gdk_pixbuf_get_from_window(root, 0, 0, gdk_window_get_width(root), gdk_window_get_height(root));
    if (!pb) toast("Couldn't read the screen", "Your desktop didn't let SuperV take a picture of it.");
    c->done(pb, c->data);
    if (pb) g_object_unref(pb);
    g_free(c);
    return G_SOURCE_REMOVE;
}

void extras_capture_screen(SvShotDone done, gpointer data) {
    CaptureReq *c = g_new0(CaptureReq, 1);
    c->done = done; c->data = data;
    if (!sv_wayland) {
        if (x11_req) { g_free(c); done(NULL, data); return; }   /* one at a time */
        x11_req = c;
        clear_screen_then(capture_x11);
        return;
    }
    clear_screen_then(NULL);
    if (gnome_capture(c)) return;
    GVariantBuilder *o = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(o, "{sv}", "interactive", g_variant_new_boolean(FALSE));
    portal_call("Screenshot", o, capture_from_portal, c);
}

void extras_ocr_pixbuf(GdkPixbuf *pb) { if (!ocr_proc) ocr(pb); }
void extras_toast(const char *title, const char *body) { toast(title, body); }
gboolean extras_capturing(void) { return ov || probe || portal_pending || clear_src; }

/* ================================================================== SuperV's entry points */

void extras_command(const char *what) {
    if (!strcmp(what, "--plain-paste")) plain_paste();
    else if (!strcmp(what, "--pick-color")) pick_color();
    else if (!strcmp(what, "--extract-text")) extract_text();
}

gboolean extras_busy(void) {
    return ov || probe || portal_pending || ocr_proc || plain_src || plain_pending || clear_src || installing
        || (cwin && gtk_widget_get_visible(cwin));
}

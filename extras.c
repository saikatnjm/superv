/*
 * SuperV extras: three small Windows shortcuts. Nothing here runs, or takes memory, until you press one,
 * and each can be turned off in SuperV's settings.
 *
 *   Ctrl+Super+Alt+V   paste as plain text       (Ctrl+Win+Alt+V, PowerToys Advanced Paste)
 *   Super+Shift+C      colour picker             (Win+Shift+C, PowerToys Color Picker)
 *   Super+Shift+T      copy text from the screen (Win+Shift+T, PowerToys Text Extractor)
 *
 * Reading the screen: on X11, the screen is frozen in a full-screen overlay and you click or drag on it.
 * On Wayland apps can't read the screen, so the desktop's own colour picker and screenshot tool are used
 * (xdg-desktop-portal: GNOME, KDE Plasma and others).
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

static void notify(const char *title, const char *body) { sv_notify("extras", title, body); }
static GtkClipboard *clipboard(void) { return gtk_clipboard_get(GDK_SELECTION_CLIPBOARD); }

/* ================================================================== paste as plain text */

static guint plain_src;
static int plain_tries;
static gboolean plain_x11;

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
    if (plain_src) return;
    plain_x11 = sv_focused_is_x11();
    gtk_clipboard_request_text(clipboard(), plain_got_text, NULL);
}

/* ================================================================== the desktop's own picker and screenshot (Wayland) */

enum { PORTAL_OK, PORTAL_CANCELLED, PORTAL_FAILED };
typedef void (*PortalDone)(int status, GVariant *results, gpointer data);
typedef struct { guint sub; PortalDone done; gpointer data; gboolean answered; } PortalReq;
static int portal_pending;

static void portal_finish(GDBusConnection *bus, PortalReq *r, int status, GVariant *results) {
    if (r->answered) return;
    r->answered = TRUE;
    g_dbus_connection_signal_unsubscribe(bus, r->sub);
    portal_pending--;
    r->done(status, results, r->data);
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
    if (v) { g_variant_unref(v); return; }   /* the answer comes as a Response signal */
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
    g_dbus_connection_call(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.Screenshot", method, g_variant_new("(sa{sv})", "", opts),
                           G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL, on_portal_called, r);
    g_variant_builder_unref(opts);
    g_free(sender); g_free(token); g_free(path);
}

/* ================================================================== the frozen-screen overlay (X11) */

enum { OV_COLOR, OV_AREA };
typedef void (*OverlayDone)(gboolean ok, GdkPixbuf *area, double r, double g, double b);
static GtkWidget *ov;
static GdkPixbuf *shot;
static OverlayDone ov_done;
static int ov_mode;
static double ov_scale, px = -1, py = -1, ax, ay;
static gboolean dragging;

static void shot_pixel(double x, double y, double *r, double *g, double *b) {
    int w = gdk_pixbuf_get_width(shot), h = gdk_pixbuf_get_height(shot), n = gdk_pixbuf_get_n_channels(shot);
    int ix = CLAMP((int)(x * ov_scale), 0, w - 1), iy = CLAMP((int)(y * ov_scale), 0, h - 1);
    const guchar *p = gdk_pixbuf_read_pixels(shot) + iy * gdk_pixbuf_get_rowstride(shot) + ix * n;
    *r = p[0] / 255.0; *g = p[1] / 255.0; *b = p[2] / 255.0;
}

static void ov_close(void) {
    gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
    gtk_widget_destroy(ov);
    ov = NULL;
}

static gboolean trim_memory(gpointer d) { malloc_trim(0); return G_SOURCE_REMOVE; }

static void ov_finish(gboolean ok) {
    OverlayDone done = ov_done;
    double r = 0, g = 0, b = 0;
    GdkPixbuf *area = NULL;
    if (ok && ov_mode == OV_COLOR) shot_pixel(px, py, &r, &g, &b);
    if (ok && ov_mode == OV_AREA) {
        int x = (int)(MIN(ax, px) * ov_scale), y = (int)(MIN(ay, py) * ov_scale);
        int w = (int)(fabs(px - ax) * ov_scale), h = (int)(fabs(py - ay) * ov_scale);
        w = MIN(w, gdk_pixbuf_get_width(shot) - x); h = MIN(h, gdk_pixbuf_get_height(shot) - y);
        if (w >= 4 && h >= 4) {
            GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(shot, x, y, w, h);
            area = gdk_pixbuf_copy(sub);
            g_object_unref(sub);
        } else {
            ok = FALSE;   /* a click, not a drag */
        }
    }
    ov_close();
    g_clear_object(&shot);
    done(ok, area, r, g, b);
    if (area) g_object_unref(area);
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
    gdk_cairo_set_source_pixbuf(cr, shot, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    if (ov_mode == OV_AREA) {
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cr, 0, 0, W, H);
        if (dragging) cairo_rectangle(cr, MIN(ax, px), MIN(ay, py), fabs(px - ax), fabs(py - ay));
        cairo_set_source_rgba(cr, 0, 0, 0, 0.38);
        cairo_fill(cr);
        if (dragging) {
            cairo_rectangle(cr, MIN(ax, px) + 0.5, MIN(ay, py) + 0.5, fabs(px - ax), fabs(py - ay));
            cairo_set_source_rgb(cr, 0.655, 0.545, 0.98);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }
        hint(cr, W, "Drag over the text you want to copy · Esc cancels");
        return TRUE;
    }
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
    gdk_cairo_set_source_pixbuf(cr, shot, -(cx - N / 2), -(cy - N / 2));
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_rectangle(cr, mx + (N / 2) * Z + 0.5, my + (N / 2) * Z + 0.5, Z - 1, Z - 1);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_rectangle(cr, mx + 0.5, my + 0.5, S - 1, S - 1);
    cairo_set_source_rgb(cr, 0.655, 0.545, 0.98);
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

static gboolean ov_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    px = e->x; py = e->y;
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean ov_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (e->type != GDK_BUTTON_PRESS) return TRUE;
    px = e->x; py = e->y;
    if (e->button != 1) { ov_finish(FALSE); return TRUE; }   /* right-click cancels */
    if (ov_mode == OV_COLOR) { ov_finish(TRUE); return TRUE; }
    ax = px; ay = py; dragging = TRUE;
    return TRUE;
}

static gboolean ov_release(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (ov_mode != OV_AREA || !dragging || e->button != 1) return TRUE;
    px = e->x; py = e->y;
    ov_finish(TRUE);
    return TRUE;
}

static gboolean ov_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (e->keyval == GDK_KEY_Escape) ov_finish(FALSE);
    return TRUE;
}

static gboolean ov_grab(gpointer d) {
    if (!ov) return G_SOURCE_REMOVE;
    GdkDisplay *disp = gdk_display_get_default();
    GdkCursor *c = gdk_cursor_new_from_name(disp, "crosshair");
    GdkGrabStatus st = gdk_seat_grab(gdk_display_get_default_seat(disp), gtk_widget_get_window(ov),
                                     GDK_SEAT_CAPABILITY_ALL, FALSE, c, NULL, NULL, NULL);
    if (c) g_object_unref(c);
    if (st != GDK_GRAB_SUCCESS) {
        logmsg("couldn't take the mouse and keyboard (%d)", st);
        ov_finish(FALSE);
    }
    return G_SOURCE_REMOVE;
}

/* Freeze the screen and let you click (colour) or drag (area). X11 only. */
static gboolean overlay_start(int mode, OverlayDone done) {
    GdkWindow *root = gdk_get_default_root_window();
    int W = gdk_window_get_width(root), H = gdk_window_get_height(root);
    shot = gdk_pixbuf_get_from_window(root, 0, 0, W, H);
    if (!shot) return FALSE;
    ov_scale = (double)gdk_pixbuf_get_width(shot) / W;
    ov_mode = mode; ov_done = done; dragging = FALSE; px = py = -1;
    ov = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_move(GTK_WINDOW(ov), 0, 0);
    gtk_widget_set_size_request(ov, W, H);
    gtk_widget_set_app_paintable(ov, TRUE);
    gtk_widget_add_events(ov, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(ov, "draw", G_CALLBACK(ov_draw), NULL);
    g_signal_connect(ov, "motion-notify-event", G_CALLBACK(ov_motion), NULL);
    g_signal_connect(ov, "button-press-event", G_CALLBACK(ov_press), NULL);
    g_signal_connect(ov, "button-release-event", G_CALLBACK(ov_release), NULL);
    g_signal_connect(ov, "key-press-event", G_CALLBACK(ov_key), NULL);
    gtk_widget_show(ov);
    g_timeout_add(30, ov_grab, NULL);   /* once the window is on screen */
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
static gboolean cwin_snapshot(gpointer path) {   /* SUPERV_EXTRAS_SNAPSHOT=file.png: for screenshots */
    if (!cwin || !gtk_widget_get_visible(cwin)) return G_SOURCE_REMOVE;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gtk_widget_get_allocated_width(cwin),
                                                    gtk_widget_get_allocated_height(cwin));
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(cwin, cr);
    cairo_destroy(cr);
    if (cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS) logmsg("saved %s", (char *)path);
    cairo_surface_destroy(s);
    return G_SOURCE_REMOVE;
}

/* The colour is copied as HEX; this small card shows it, with RGB and HSL one click away. */
static void color_show(double r, double g, double b) {
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
    gtk_window_set_position(GTK_WINDOW(cwin), sv_wayland ? GTK_WIN_POS_CENTER_ALWAYS : GTK_WIN_POS_MOUSE);
    gtk_widget_show(cwin);
    gtk_window_present(GTK_WINDOW(cwin));
    const char *snap = g_getenv("SUPERV_EXTRAS_SNAPSHOT");
    if (snap && *snap) g_timeout_add(600, cwin_snapshot, (gpointer)snap);
    logmsg("colour picked: %s", f[0]);
}

static void color_from_overlay(gboolean ok, GdkPixbuf *area, double r, double g, double b) {
    if (ok) color_show(r, g, b);
}

static void color_from_portal(int status, GVariant *results, gpointer d) {
    double r, g, b;
    if (status == PORTAL_OK && g_variant_lookup(results, "color", "(ddd)", &r, &g, &b)) color_show(r, g, b);
    else if (status == PORTAL_FAILED)
        notify("Colour picking isn't available here",
               "Your desktop doesn't offer a colour picker to apps (xdg-desktop-portal). GNOME and KDE Plasma do.");
}

static void pick_color(void) {
    if (ov || portal_pending) return;
    if (!sv_wayland && overlay_start(OV_COLOR, color_from_overlay)) return;
    portal_call("PickColor", g_variant_builder_new(G_VARIANT_TYPE("a{sv}")), color_from_portal, NULL);
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

static void ocr(GdkPixbuf *pb) {
    if (!extras_ocr_available()) {
        notify("Copying text needs Tesseract",
               "Install it once: sudo apt install tesseract-ocr (Ubuntu, Mint, Debian), sudo dnf install tesseract "
               "(Fedora), sudo pacman -S tesseract tesseract-data-eng (Arch).");
        return;
    }
    /* small text reads better a bit bigger */
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    double k = h < 150 ? MIN(3.0, 150.0 / h) : 1.0;
    GdkPixbuf *big = k > 1 ? gdk_pixbuf_scale_simple(pb, (int)(w * k), (int)(h * k), GDK_INTERP_BILINEAR) : g_object_ref(pb);
    g_free(ocr_file);
    ocr_file = g_strdup_printf("%s/superv-text-%d.png", g_get_user_runtime_dir(), getpid());
    gboolean saved = gdk_pixbuf_save(big, ocr_file, "png", NULL, NULL);
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

static void text_from_overlay(gboolean ok, GdkPixbuf *area, double r, double g, double b) {
    if (ok && area) ocr(area);
}

static void text_from_portal(int status, GVariant *results, gpointer d) {
    const char *uri = NULL;
    if (status == PORTAL_FAILED) {
        notify("Copying text from the screen isn't available here",
               "Your desktop doesn't offer screenshots to apps (xdg-desktop-portal). GNOME and KDE Plasma do.");
        return;
    }
    if (status != PORTAL_OK || !g_variant_lookup(results, "uri", "&s", &uri)) return;
    char *path = g_filename_from_uri(uri, NULL, NULL);
    GdkPixbuf *pb = path ? gdk_pixbuf_new_from_file(path, NULL) : NULL;
    /* the desktop saved this screenshot only for us: don't leave it in your Pictures folder */
    GStatBuf st;
    if (path && g_stat(path, &st) == 0 && time(NULL) - st.st_mtime < 120) {
        char *base = g_path_get_basename(path);
        if (g_str_has_prefix(base, "Screenshot") || g_str_has_prefix(base, "screenshot")) g_unlink(path);
        g_free(base);
    }
    if (pb) { ocr(pb); g_object_unref(pb); }
    g_free(path);
}

static void extract_text(void) {
    if (ov || portal_pending || ocr_proc) return;
    if (!extras_ocr_available()) { ocr(NULL); return; }   /* says how to install it */
    if (!sv_wayland && overlay_start(OV_AREA, text_from_overlay)) return;
    GVariantBuilder *o = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(o, "{sv}", "interactive", g_variant_new_boolean(TRUE));   /* you choose the area */
    portal_call("Screenshot", o, text_from_portal, NULL);
}

/* ================================================================== SuperV's entry points */

void extras_command(const char *what) {
    if (!strcmp(what, "--plain-paste")) plain_paste();
    else if (!strcmp(what, "--pick-color")) pick_color();
    else if (!strcmp(what, "--extract-text")) extract_text();
}

gboolean extras_busy(void) {
    return ov || portal_pending || ocr_proc || plain_src || (cwin && gtk_widget_get_visible(cwin));
}

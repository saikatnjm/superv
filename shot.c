/*
 * SuperV screenshots: Super+Shift+S, Flameshot-style. The screen freezes; drag over an area, draw on it,
 * then copy, save, pin it to the screen or copy its text. Nothing of this exists until you press the shortcut,
 * and all of it (the frozen screen too) is freed as soon as you're done.
 *
 *   drag                  select an area; drag its edges or corners to resize, its inside to move it
 *   P L A R C M T B N     pen, line, arrow, rectangle, circle, marker, text, pixelate, numbers
 *   V                     move the selection again
 *   mouse wheel  [  ]     thinner / thicker
 *   Ctrl+Z  Ctrl+Y        undo / redo
 *   Enter  Ctrl+C         copy        Ctrl+S save        Ctrl+P pin        Ctrl+T copy the text
 *   arrows                move the selection by a pixel (Shift: resize)       Ctrl+A the whole screen
 *   Esc                   close
 *
 * X11: the screen is read directly. GNOME on Wayland: GNOME's own instant screenshot, pressed for you (no dialog).
 * Other Wayland desktops: the desktop portal is asked (it may show a dialog).
 * MIT License.
 */
#define _GNU_SOURCE
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include "superv.h"

static void logmsg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap); va_end(ap);
    GDateTime *t = g_date_time_new_now_local();
    char *ts = g_date_time_format(t, "%H:%M:%S");
    g_printerr("%s [superv:shot] %s\n", ts, m);
    g_free(ts); g_free(m); g_date_time_unref(t);
}

enum { T_MOVE, T_PEN, T_LINE, T_ARROW, T_RECT, T_CIRCLE, T_MARKER, T_TEXT, T_PIXEL, T_COUNTER, N_TOOLS };
static const char *TOOL_TIPS[N_TOOLS] = {
    "Move the selection (V)", "Pen (P)", "Line (L)", "Arrow (A)", "Rectangle (R)", "Circle (C)",
    "Marker (M)", "Text (T)", "Pixelate (B)", "Numbers (N)",
};
static const guint TOOL_KEYS[N_TOOLS] = { GDK_KEY_v, GDK_KEY_p, GDK_KEY_l, GDK_KEY_a, GDK_KEY_r, GDK_KEY_c,
                                          GDK_KEY_m, GDK_KEY_t, GDK_KEY_b, GDK_KEY_n };
static const double COLORS[][3] = {
    { 0.93, 0.20, 0.20 }, { 1.00, 0.55, 0.10 }, { 1.00, 0.85, 0.15 }, { 0.20, 0.75, 0.35 },
    { 0.20, 0.50, 1.00 }, { -1, -1, -1 } /* SuperV's colour */, { 0.08, 0.08, 0.10 }, { 1, 1, 1 },
};
#define N_COLORS 8

typedef struct {
    int tool, num;
    double rgb[3], size, x1, y1, x2, y2;   /* screen coordinates (as the desktop counts them) */
    GArray *pts;                           /* pen and marker: x, y, x, y… */
    GString *text;
    cairo_surface_t *px;                   /* pixelate: the blocks, kept until the box changes */
    int pk[5];
} Shape;

static GPtrArray *wins, *geos;     /* one full-screen window per monitor, and where each monitor is */
static cairo_surface_t *shot;      /* the frozen screen, in real pixels: one surface, made once */
static int shot_w, shot_h;
static double scale = 1;           /* real pixels per screen coordinate */
static double root_w, root_h;      /* the desktop's size, in screen coordinates */
static GtkIMContext *im;           /* for the text tool: your keyboard layout, dead keys and input method */
static int n_pins;
static GPtrArray *shapes, *redo;
static Shape *cur, *typing;
static gboolean has_sel, starting;
static gboolean text_mode;         /* Super+Shift+T: only select an area; its text is copied when you let go */
static double sx1, sy1, sx2, sy2, mx = -1, my = -1, ax, ay, o1, o2, o3, o4;
static int tool = T_MOVE, color = 0, hover = -1, handle = -1;
static double size = 4;
static enum { D_NONE, D_SELECT, D_MOVE, D_RESIZE, D_DRAW } drag;

gboolean shot_busy(void) { return wins != NULL || starting; }
int shot_pins(void) { return n_pins; }

static void cur_color(double rgb[3]) {
    if (COLORS[color][0] < 0) { sv_accent(rgb); return; }
    memcpy(rgb, COLORS[color], sizeof(double) * 3);
}

static void redraw(void) {
    for (guint i = 0; wins && i < wins->len; i++) gtk_widget_queue_draw(wins->pdata[i]);
}

/* Redraw only that box (screen coordinates). Repainting every monitor on every mouse move is what makes
 * a screenshot tool feel heavy; a drag only changes a small part of the picture. */
static void damage(double x1, double y1, double x2, double y2, double pad) {
    int x = (int)floor(MIN(x1, x2) - pad), y = (int)floor(MIN(y1, y2) - pad);
    int w = (int)ceil(fabs(x2 - x1) + 2 * pad) + 2, h = (int)ceil(fabs(y2 - y1) + 2 * pad) + 2;
    for (guint i = 0; wins && i < wins->len; i++) {
        GdkRectangle *g = geos->pdata[i];
        gtk_widget_queue_draw_area(wins->pdata[i], x - g->x, y - g->y, w, h);
    }
}

static void sel_norm(double *x1, double *y1, double *x2, double *y2) {
    *x1 = MIN(sx1, sx2); *y1 = MIN(sy1, sy2); *x2 = MAX(sx1, sx2); *y2 = MAX(sy1, sy2);
}

static void shape_free(gpointer p) {
    Shape *s = p;
    if (s->pts) g_array_free(s->pts, TRUE);
    if (s->text) g_string_free(s->text, TRUE);
    if (s->px) cairo_surface_destroy(s->px);
    g_free(s);
}

/* ------------------------------------------------------------------ drawing the marks */

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    r = MIN(r, MIN(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static PangoLayout *text_layout(cairo_t *cr, Shape *s) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(fd, MAX(14, s->size * 4) * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, s->text->str, -1);
    return l;
}

/* pixelate: the frozen screen's pixels in that box, averaged into blocks. The blocks are worked out once and
 * kept with the mark, not on every redraw. */
static void draw_pixelate(cairo_t *cr, Shape *s) {
    double x = MIN(s->x1, s->x2), y = MIN(s->y1, s->y2), w = fabs(s->x2 - s->x1), h = fabs(s->y2 - s->y1);
    int X = (int)(x * scale), Y = (int)(y * scale), W = (int)(w * scale), H = (int)(h * scale);
    X = CLAMP(X, 0, shot_w - 1); Y = CLAMP(Y, 0, shot_h - 1); W = MIN(W, shot_w - X); H = MIN(H, shot_h - Y);
    if (W < 2 || H < 2) return;
    int block = (int)MAX(6, s->size * 3), bw = MAX(1, W / block), bh = MAX(1, H / block);
    int key[5] = { X, Y, W, H, block };
    if (!s->px || memcmp(key, s->pk, sizeof key)) {
        if (s->px) cairo_surface_destroy(s->px);
        s->px = cairo_image_surface_create(CAIRO_FORMAT_RGB24, bw, bh);
        memcpy(s->pk, key, sizeof key);
        const guchar *src = cairo_image_surface_get_data(shot);
        guchar *dst = cairo_image_surface_get_data(s->px);
        int ss = cairo_image_surface_get_stride(shot), ds = cairo_image_surface_get_stride(s->px);
        for (int j = 0; j < bh; j++)
            for (int i = 0; i < bw; i++) {
                int xa = X + i * W / bw, xb = X + (i + 1) * W / bw, ya = Y + j * H / bh, yb = Y + (j + 1) * H / bh;
                guint r = 0, g = 0, b = 0, n = 0;
                for (int yy = ya; yy < yb; yy++) {
                    const guint32 *row = (const guint32 *)(void *)(src + (gsize)yy * ss);
                    for (int xx = xa; xx < xb; xx++) { guint32 c = row[xx]; r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; n++; }
                }
                if (!n) n = 1;
                ((guint32 *)(void *)(dst + (gsize)j * ds))[i] = 0xff000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        cairo_surface_mark_dirty(s->px);
    }
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_translate(cr, X / scale, Y / scale);
    cairo_scale(cr, (double)W / scale / bw, (double)H / scale / bh);
    cairo_set_source_surface(cr, s->px, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_shape(cairo_t *cr, Shape *s) {
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgb(cr, s->rgb[0], s->rgb[1], s->rgb[2]);
    cairo_set_line_width(cr, s->size);
    switch (s->tool) {
    case T_PEN: case T_MARKER:
        if (!s->pts || s->pts->len < 2) break;
        if (s->tool == T_MARKER) {
            cairo_set_source_rgba(cr, s->rgb[0], s->rgb[1], s->rgb[2], 0.38);
            cairo_set_line_width(cr, s->size * 4);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
        }
        cairo_move_to(cr, g_array_index(s->pts, double, 0), g_array_index(s->pts, double, 1));
        for (guint i = 2; i + 1 < s->pts->len; i += 2)
            cairo_line_to(cr, g_array_index(s->pts, double, i), g_array_index(s->pts, double, i + 1));
        if (s->pts->len == 2) cairo_line_to(cr, g_array_index(s->pts, double, 0) + 0.1, g_array_index(s->pts, double, 1));
        cairo_stroke(cr);
        break;
    case T_LINE:
        cairo_move_to(cr, s->x1, s->y1); cairo_line_to(cr, s->x2, s->y2); cairo_stroke(cr);
        break;
    case T_ARROW: {
        double a = atan2(s->y2 - s->y1, s->x2 - s->x1), head = MAX(12, s->size * 4), len = hypot(s->x2 - s->x1, s->y2 - s->y1);
        double bx = s->x2 - cos(a) * head * 0.8, by = s->y2 - sin(a) * head * 0.8;
        if (len > head) { cairo_move_to(cr, s->x1, s->y1); cairo_line_to(cr, bx, by); cairo_stroke(cr); }
        cairo_move_to(cr, s->x2, s->y2);
        cairo_line_to(cr, s->x2 - head * cos(a - 0.45), s->y2 - head * sin(a - 0.45));
        cairo_line_to(cr, s->x2 - head * cos(a + 0.45), s->y2 - head * sin(a + 0.45));
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    }
    case T_RECT:
        cairo_rectangle(cr, MIN(s->x1, s->x2), MIN(s->y1, s->y2), fabs(s->x2 - s->x1), fabs(s->y2 - s->y1));
        cairo_stroke(cr);
        break;
    case T_CIRCLE: {
        double w = fabs(s->x2 - s->x1), h = fabs(s->y2 - s->y1);
        if (w < 1 || h < 1) break;
        cairo_save(cr);
        cairo_translate(cr, (s->x1 + s->x2) / 2, (s->y1 + s->y2) / 2);
        cairo_scale(cr, w / 2, h / 2);
        cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
        cairo_restore(cr);
        cairo_stroke(cr);
        break;
    }
    case T_TEXT: {
        PangoLayout *l = text_layout(cr, s);
        cairo_move_to(cr, s->x1, s->y1);
        pango_cairo_show_layout(cr, l);
        if (s == typing) {   /* the text cursor */
            PangoRectangle pos;
            pango_layout_get_cursor_pos(l, s->text->len, &pos, NULL);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, s->x1 + pos.x / PANGO_SCALE + 1, s->y1 + pos.y / PANGO_SCALE);
            cairo_rel_line_to(cr, 0, pos.height / PANGO_SCALE);
            cairo_stroke(cr);
        }
        g_object_unref(l);
        break;
    }
    case T_PIXEL:
        draw_pixelate(cr, s);
        break;
    case T_COUNTER: {
        double r = MAX(13, s->size * 3);
        cairo_arc(cr, s->x1, s->y1, r, 0, 2 * G_PI);
        cairo_fill(cr);
        double lum = 0.3 * s->rgb[0] + 0.59 * s->rgb[1] + 0.11 * s->rgb[2];
        PangoLayout *l = pango_cairo_create_layout(cr);
        PangoFontDescription *fd = pango_font_description_from_string("Sans Bold");
        pango_font_description_set_absolute_size(fd, r * 1.1 * PANGO_SCALE);
        pango_layout_set_font_description(l, fd);
        pango_font_description_free(fd);
        char n[8];
        g_snprintf(n, sizeof n, "%d", s->num);
        pango_layout_set_text(l, n, -1);
        int tw, th;
        pango_layout_get_pixel_size(l, &tw, &th);
        if (lum > 0.6) cairo_set_source_rgb(cr, 0.08, 0.08, 0.1); else cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, s->x1 - tw / 2.0, s->y1 - th / 2.0);
        pango_cairo_show_layout(cr, l);
        g_object_unref(l);
        break;
    }
    }
    cairo_restore(cr);
}

static void draw_marks(cairo_t *cr) {
    for (guint i = 0; i < shapes->len; i++) draw_shape(cr, shapes->pdata[i]);
    if (cur) draw_shape(cr, cur);
    if (typing) draw_shape(cr, typing);
}

/* ------------------------------------------------------------------ the toolbar (drawn by us, under the selection) */

enum { B_TOOL, B_UNDO = N_TOOLS, B_REDO, B_OCR, B_PIN, B_SAVE, B_COPY, B_CLOSE, N_BTNS };
static const char *BTN_TIPS[] = { [B_UNDO] = "Undo (Ctrl+Z)", [B_REDO] = "Redo (Ctrl+Y)",
    [B_OCR] = "Copy the text in it (Ctrl+T)", [B_PIN] = "Pin to the screen (Ctrl+P)",
    [B_SAVE] = "Save to Pictures/Screenshots (Ctrl+S)", [B_COPY] = "Copy (Enter or Ctrl+C)", [B_CLOSE] = "Close (Esc)" };
#define BS 34      /* button size */
#define SWS 22     /* colour swatch size */
static double bar_x, bar_y, bar_w, bar_h;

static int bar_buttons(void) { return N_BTNS; }

static void bar_layout(void) {
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    bar_w = bar_buttons() * BS + 4 * 8 + 12;
    bar_h = BS + 12 + SWS + 8;
    GdkRectangle a;
    GdkMonitor *m = gdk_display_get_monitor_at_point(gdk_display_get_default(), (int)((x1 + x2) / 2), (int)((y1 + y2) / 2));
    gdk_monitor_get_geometry(m, &a);
    bar_x = CLAMP((x1 + x2) / 2 - bar_w / 2, a.x + 6, a.x + a.width - bar_w - 6);
    if (y2 + 10 + bar_h <= a.y + a.height) bar_y = y2 + 10;            /* below the selection */
    else if (y1 - 10 - bar_h >= a.y) bar_y = y1 - 10 - bar_h;          /* above it */
    else bar_y = y2 - bar_h - 10;                                       /* inside, at the bottom */
}

/* where button i is (the gaps separate tools / undo, redo / actions) */
static void bar_button(int i, double *x, double *y) {
    int gap = i >= B_UNDO ? 1 : 0;
    gap += i >= B_OCR ? 1 : 0;
    gap += i >= B_CLOSE ? 1 : 0;
    *x = bar_x + 6 + i * BS + gap * 8;
    *y = bar_y + 6;
}

static void swatch_pos(int i, double *x, double *y) {
    *x = bar_x + 10 + i * (SWS + 6);
    *y = bar_y + BS + 10;
}

/* -1 nothing, 0…N_BTNS-1 a button, 100+i a colour */
static int bar_hit(double x, double y) {
    if (text_mode) return -1;   /* no toolbar */
    if (!has_sel || drag == D_SELECT || x < bar_x || x > bar_x + bar_w || y < bar_y || y > bar_y + bar_h) return -1;
    for (int i = 0; i < N_BTNS; i++) {
        double bx, by;
        bar_button(i, &bx, &by);
        if (x >= bx && x < bx + BS && y >= by && y < by + BS) return i;
    }
    for (int i = 0; i < N_COLORS; i++) {
        double cx, cy;
        swatch_pos(i, &cx, &cy);
        if (x >= cx - 2 && x < cx + SWS + 2 && y >= cy - 2 && y < cy + SWS + 2) return 100 + i;
    }
    return -2;   /* the bar, not a button */
}

static void icon(cairo_t *cr, int i, double cx, double cy) {
    cairo_save(cr);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (i) {
    case T_MOVE:
        cairo_move_to(cr, cx - 8, cy); cairo_line_to(cr, cx + 8, cy); cairo_move_to(cr, cx, cy - 8); cairo_line_to(cr, cx, cy + 8);
        for (int k = 0; k < 4; k++) {
            double a = k * G_PI / 2, ex = cx + 8 * cos(a), ey = cy + 8 * sin(a);
            cairo_move_to(cr, ex - 3 * cos(a) - 3 * sin(a), ey - 3 * sin(a) + 3 * cos(a));
            cairo_line_to(cr, ex, ey);
            cairo_line_to(cr, ex - 3 * cos(a) + 3 * sin(a), ey - 3 * sin(a) - 3 * cos(a));
        }
        cairo_stroke(cr); break;
    case T_PEN:
        cairo_move_to(cr, cx - 7, cy + 5);
        cairo_curve_to(cr, cx - 3, cy - 8, cx + 1, cy + 9, cx + 7, cy - 5);
        cairo_stroke(cr); break;
    case T_LINE: cairo_move_to(cr, cx - 7, cy + 7); cairo_line_to(cr, cx + 7, cy - 7); cairo_stroke(cr); break;
    case T_ARROW:
        cairo_move_to(cr, cx - 7, cy + 7); cairo_line_to(cr, cx + 6, cy - 6);
        cairo_move_to(cr, cx - 1, cy - 7); cairo_line_to(cr, cx + 7, cy - 7); cairo_line_to(cr, cx + 7, cy + 1);
        cairo_stroke(cr); break;
    case T_RECT: cairo_rectangle(cr, cx - 8, cy - 6, 16, 12); cairo_stroke(cr); break;
    case T_CIRCLE: cairo_arc(cr, cx, cy, 7.5, 0, 2 * G_PI); cairo_stroke(cr); break;
    case T_MARKER:
        cairo_set_line_width(cr, 6); cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
        cairo_push_group(cr); cairo_move_to(cr, cx - 7, cy + 3); cairo_line_to(cr, cx + 7, cy - 3); cairo_stroke(cr);
        cairo_pop_group_to_source(cr); cairo_paint_with_alpha(cr, 0.55); break;
    case T_TEXT:
        cairo_move_to(cr, cx - 7, cy - 7); cairo_line_to(cr, cx + 7, cy - 7);
        cairo_move_to(cr, cx, cy - 7); cairo_line_to(cr, cx, cy + 8); cairo_stroke(cr); break;
    case T_PIXEL:
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
            if ((r + c) % 2 == 0) cairo_rectangle(cr, cx - 7.5 + c * 5, cy - 7.5 + r * 5, 5, 5);
        cairo_fill(cr); cairo_rectangle(cr, cx - 7.5, cy - 7.5, 15, 15); cairo_set_line_width(cr, 1); cairo_stroke(cr); break;
    case T_COUNTER: {
        cairo_arc(cr, cx, cy, 8, 0, 2 * G_PI); cairo_stroke(cr);
        cairo_move_to(cr, cx - 2, cy - 3); cairo_line_to(cr, cx + 1, cy - 5); cairo_line_to(cr, cx + 1, cy + 5);
        cairo_stroke(cr); break;
    }
    case B_UNDO: case B_REDO: {   /* a curved arrow, pointing back (undo) or forward (redo) */
        double d = i == B_UNDO ? 1 : -1;
        cairo_move_to(cr, cx + d * 6, cy + 6); cairo_curve_to(cr, cx + d * 9, cy - 4, cx - d * 2, cy - 8, cx - d * 6, cy - 2);
        cairo_move_to(cr, cx - d * 7, cy - 7); cairo_line_to(cr, cx - d * 6, cy - 2); cairo_line_to(cr, cx - d * 1, cy - 3);
        cairo_stroke(cr); break;
    }
    case B_OCR:
        cairo_move_to(cr, cx - 8, cy - 4); cairo_line_to(cr, cx - 8, cy - 8); cairo_line_to(cr, cx - 4, cy - 8);
        cairo_move_to(cr, cx + 4, cy - 8); cairo_line_to(cr, cx + 8, cy - 8); cairo_line_to(cr, cx + 8, cy - 4);
        cairo_move_to(cr, cx + 8, cy + 4); cairo_line_to(cr, cx + 8, cy + 8); cairo_line_to(cr, cx + 4, cy + 8);
        cairo_move_to(cr, cx - 4, cy + 8); cairo_line_to(cr, cx - 8, cy + 8); cairo_line_to(cr, cx - 8, cy + 4);
        cairo_move_to(cr, cx - 4, cy - 3); cairo_line_to(cr, cx + 4, cy - 3);
        cairo_move_to(cr, cx - 4, cy + 1); cairo_line_to(cr, cx + 4, cy + 1);
        cairo_move_to(cr, cx - 4, cy + 5); cairo_line_to(cr, cx + 1, cy + 5);
        cairo_stroke(cr); break;
    case B_PIN:
        cairo_move_to(cr, cx - 4, cy - 8); cairo_line_to(cr, cx + 4, cy - 8); cairo_line_to(cr, cx + 3, cy - 2);
        cairo_line_to(cr, cx + 6, cy + 1); cairo_line_to(cr, cx - 6, cy + 1); cairo_line_to(cr, cx - 3, cy - 2);
        cairo_close_path(cr); cairo_move_to(cr, cx, cy + 1); cairo_line_to(cr, cx, cy + 8); cairo_stroke(cr); break;
    case B_SAVE:
        cairo_move_to(cr, cx, cy - 8); cairo_line_to(cr, cx, cy + 3);
        cairo_move_to(cr, cx - 4, cy - 1); cairo_line_to(cr, cx, cy + 3); cairo_line_to(cr, cx + 4, cy - 1);
        cairo_move_to(cr, cx - 8, cy + 2); cairo_line_to(cr, cx - 8, cy + 7); cairo_line_to(cr, cx + 8, cy + 7);
        cairo_line_to(cr, cx + 8, cy + 2); cairo_stroke(cr); break;
    case B_COPY:
        rounded(cr, cx - 7, cy - 4, 10, 12, 2); cairo_stroke(cr);
        cairo_move_to(cr, cx - 3, cy - 6); cairo_line_to(cr, cx - 3, cy - 8); cairo_line_to(cr, cx + 7, cy - 8);
        cairo_line_to(cr, cx + 7, cy + 4); cairo_line_to(cr, cx + 5, cy + 4); cairo_stroke(cr); break;
    case B_CLOSE:
        cairo_move_to(cr, cx - 6, cy - 6); cairo_line_to(cr, cx + 6, cy + 6);
        cairo_move_to(cr, cx + 6, cy - 6); cairo_line_to(cr, cx - 6, cy + 6); cairo_stroke(cr); break;
    }
    cairo_restore(cr);
}

static void draw_tip(cairo_t *cr, const char *tip, double cx, double y) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 9.5");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, tip, -1);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    double x = cx - tw / 2.0 - 8, h = th + 8;
    rounded(cr, x, y - h - 6, tw + 16, h, 6);
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.09, 0.95);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.93, 0.92, 0.96);
    cairo_move_to(cr, x + 8, y - h - 2);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static void draw_bar(cairo_t *cr) {
    double ac[3];
    sv_accent(ac);
    rounded(cr, bar_x, bar_y, bar_w, bar_h, 10);
    cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, ac[0], ac[1], ac[2], 0.45);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    for (int i = 0; i < N_BTNS; i++) {
        double bx, by;
        bar_button(i, &bx, &by);
        gboolean active = i < N_TOOLS && i == tool, off = (i == B_UNDO && !shapes->len) || (i == B_REDO && !redo->len);
        if (active || i == hover) {
            rounded(cr, bx + 2, by + 2, BS - 4, BS - 4, 7);
            if (active) cairo_set_source_rgba(cr, ac[0], ac[1], ac[2], 0.30);
            else cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
            cairo_fill(cr);
        }
        if (active) cairo_set_source_rgb(cr, ac[0], ac[1], ac[2]);
        else if (off) cairo_set_source_rgba(cr, 0.93, 0.92, 0.96, 0.35);
        else cairo_set_source_rgb(cr, 0.93, 0.92, 0.96);
        icon(cr, i, bx + BS / 2.0, by + BS / 2.0);
    }
    for (int i = 0; i < N_COLORS; i++) {
        double x, y, c[3];
        swatch_pos(i, &x, &y);
        if (COLORS[i][0] < 0) memcpy(c, ac, sizeof c); else memcpy(c, COLORS[i], sizeof c);
        cairo_arc(cr, x + SWS / 2.0, y + SWS / 2.0, SWS / 2.0 - 3, 0, 2 * G_PI);
        cairo_set_source_rgb(cr, c[0], c[1], c[2]);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        if (i == color) {
            cairo_arc(cr, x + SWS / 2.0, y + SWS / 2.0, SWS / 2.0, 0, 2 * G_PI);
            cairo_set_source_rgb(cr, 0.93, 0.92, 0.96);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        }
    }
    /* the thickness, after the colours */
    double x, y, c[3];
    swatch_pos(N_COLORS, &x, &y);
    cur_color(c);
    cairo_arc(cr, x + 8, y + SWS / 2.0, MIN(SWS / 2.0, MAX(1.5, size / 2)), 0, 2 * G_PI);
    cairo_set_source_rgb(cr, c[0], c[1], c[2]);
    cairo_fill(cr);
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    char t[48];
    g_snprintf(t, sizeof t, "size %d · wheel or [ ]", (int)size);
    pango_layout_set_text(l, t, -1);
    cairo_set_source_rgba(cr, 0.93, 0.92, 0.96, 0.7);
    cairo_move_to(cr, x + 24, y + 4);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    if (hover >= 0 && hover < N_BTNS) {
        double bx, by;
        bar_button(hover, &bx, &by);
        draw_tip(cr, hover < N_TOOLS ? TOOL_TIPS[hover] : BTN_TIPS[hover], bx + BS / 2.0, by);
    }
}

/* ------------------------------------------------------------------ the frozen screen */

static void hint(cairo_t *cr, GdkRectangle *a, const char *text) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, text, -1);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    double x = a->x + (a->width - tw) / 2.0 - 14, y = a->y + 28;
    rounded(cr, x, y, tw + 28, th + 14, (th + 14) / 2.0);
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.09, 0.88);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.93, 0.92, 0.96);
    cairo_move_to(cr, x + 14, y + 7);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static const double HANDLES[8][2] = { {0,0}, {.5,0}, {1,0}, {1,.5}, {1,1}, {.5,1}, {0,1}, {0,.5} };

static int handle_at(double x, double y) {
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    for (int i = 0; i < 8; i++) {
        double hx = x1 + (x2 - x1) * HANDLES[i][0], hy = y1 + (y2 - y1) * HANDLES[i][1];
        if (fabs(x - hx) <= 8 && fabs(y - hy) <= 8) return i;
    }
    return -1;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer mon) {
    GdkRectangle *g = mon;
    double ac[3];
    sv_accent(ac);
    cairo_translate(cr, -g->x, -g->y);   /* everything below is in screen coordinates */
    cairo_save(cr);
    cairo_scale(cr, 1 / scale, 1 / scale);
    cairo_set_source_surface(cr, shot, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_restore(cr);
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    if (has_sel) {
        cairo_save(cr);
        cairo_rectangle(cr, x1, y1, x2 - x1, y2 - y1);
        cairo_clip(cr);
        draw_marks(cr);
        cairo_restore(cr);
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, g->x, g->y, g->width, g->height);
    if (has_sel) cairo_rectangle(cr, x1, y1, x2 - x1, y2 - y1);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    if (!has_sel || text_mode) {
        hint(cr, g, text_mode ? "Drag over the text you want to copy · Esc cancels"
                              : "Drag over the area to capture · Ctrl+A the whole screen · Esc cancels");
        if (!has_sel) return TRUE;
    }
    cairo_rectangle(cr, x1 + 0.5, y1 + 0.5, x2 - x1 - 1, y2 - y1 - 1);
    cairo_set_source_rgb(cr, ac[0], ac[1], ac[2]);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    if (text_mode) return TRUE;   /* just the box: no handles, size or toolbar */
    for (int i = 0; i < 8 && drag != D_SELECT; i++) {
        double hx = x1 + (x2 - x1) * HANDLES[i][0], hy = y1 + (y2 - y1) * HANDLES[i][1];
        cairo_arc(cr, hx, hy, 4.5, 0, 2 * G_PI);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, ac[0], ac[1], ac[2]);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    }
    /* the size, in real pixels */
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Monospace Bold 9");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    char t[32];
    g_snprintf(t, sizeof t, "%d × %d", (int)lround((x2 - x1) * scale), (int)lround((y2 - y1) * scale));
    pango_layout_set_text(l, t, -1);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    double ly = y1 - th - 12 >= g->y ? y1 - th - 12 : y1 + 6;
    rounded(cr, x1, ly, tw + 12, th + 6, 5);
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.09, 0.85);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.93, 0.92, 0.96);
    cairo_move_to(cr, x1 + 6, ly + 3);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    if (drag != D_SELECT) { bar_layout(); draw_bar(cr); }
    return TRUE;
}

/* ------------------------------------------------------------------ results */

static GdkPixbuf *render(void) {   /* the selected area with the marks, in real pixels */
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    int X = CLAMP((int)lround(x1 * scale), 0, shot_w), Y = CLAMP((int)lround(y1 * scale), 0, shot_h);
    int W = MIN((int)lround(x2 * scale) - X, shot_w - X);
    int H = MIN((int)lround(y2 * scale) - Y, shot_h - Y);
    if (W < 1 || H < 1) return NULL;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cairo_t *cr = cairo_create(s);
    cairo_translate(cr, -X, -Y);
    cairo_set_source_surface(cr, shot, 0, 0);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    draw_marks(cr);
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(s, 0, 0, W, H);
    cairo_surface_destroy(s);
    return pb;
}

static gboolean trim(gpointer d) { malloc_trim(0); return G_SOURCE_REMOVE; }

static void finish(void) {   /* close the frozen screen, free everything */
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    gdk_seat_ungrab(seat);
    if (im) { gtk_im_context_focus_out(im); g_clear_object(&im); }
    for (guint i = 0; wins && i < wins->len; i++) gtk_widget_destroy(wins->pdata[i]);   /* frees its place in geos */
    g_clear_pointer(&wins, g_ptr_array_unref);
    g_clear_pointer(&geos, g_ptr_array_unref);
    g_clear_pointer(&shapes, g_ptr_array_unref);
    g_clear_pointer(&redo, g_ptr_array_unref);
    if (cur) { shape_free(cur); cur = NULL; }
    if (typing) { shape_free(typing); typing = NULL; }
    g_clear_pointer(&shot, cairo_surface_destroy);
    has_sel = FALSE; drag = D_NONE; hover = -1; text_mode = FALSE;
    g_timeout_add(500, trim, NULL);
}

static void commit_text(void) {
    if (!typing) return;
    if (im) { gtk_im_context_reset(im); gtk_im_context_focus_out(im); }
    if (typing->text->len) { g_ptr_array_add(shapes, typing); g_ptr_array_set_size(redo, 0); }
    else shape_free(typing);
    typing = NULL;
}

static char *save_path(void) {
    const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    char *dir = pics ? g_build_filename(pics, "Screenshots", NULL)
                     : g_build_filename(g_get_home_dir(), "Pictures", "Screenshots", NULL);
    g_mkdir_with_parents(dir, 0755);
    GDateTime *t = g_date_time_new_now_local();
    char *name = g_date_time_format(t, "Screenshot from %Y-%m-%d %H-%M-%S.png");
    char *p = g_build_filename(dir, name, NULL);
    for (int n = 2; g_file_test(p, G_FILE_TEST_EXISTS); n++) {   /* two in the same second */
        g_free(p);
        char *alt = g_date_time_format(t, "Screenshot from %Y-%m-%d %H-%M-%S");
        char *nm = g_strdup_printf("%s (%d).png", alt, n);
        p = g_build_filename(dir, nm, NULL);
        g_free(alt); g_free(nm);
    }
    g_date_time_unref(t); g_free(name); g_free(dir);
    return p;
}

static void pin(GdkPixbuf *pb, double x, double y);

enum { A_COPY, A_SAVE, A_PIN, A_OCR };
static void act(int what) {
    commit_text();
    GdkPixbuf *pb = render();
    if (!pb) { extras_toast("Nothing to capture there", "Drag over an area of the screen first."); return; }
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    finish();
    if (what == A_COPY) {
        sv_copy_image(pb);
        extras_toast("Screenshot copied", "Paste it anywhere with Ctrl+V. It's in your clipboard history too.");
    } else if (what == A_SAVE) {
        char *p = save_path();
        GError *e = NULL;
        if (gdk_pixbuf_save(pb, p, "png", &e, NULL)) {
            char *base = g_path_get_basename(p), *body = g_strdup_printf("Pictures/Screenshots/%s", base);
            extras_toast("Screenshot saved", body);
            logmsg("saved %s", p);
            g_free(base); g_free(body);
        } else {
            extras_toast("Couldn't save the screenshot", e->message);
            g_error_free(e);
        }
        g_free(p);
    } else if (what == A_PIN) {
        pin(pb, x1, y1);
    } else {
        extras_ocr_pixbuf(pb);
    }
    g_object_unref(pb);
}

static const char *last;   /* the cursor now shown */
static void set_cursor(const char *name) {
    if (last == name) return;
    last = name;
    for (guint i = 0; wins && i < wins->len; i++) {
        GdkWindow *gw = gtk_widget_get_window(wins->pdata[i]);
        GdkCursor *c = gdk_cursor_new_from_name(gdk_display_get_default(), name);
        if (gw) gdk_window_set_cursor(gw, c);
        if (c) g_object_unref(c);
    }
}

static void update_cursor(void) {
    static const char *rs[8] = { "nw-resize", "n-resize", "ne-resize", "e-resize", "se-resize", "s-resize",
                                 "sw-resize", "w-resize" };
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    int h = has_sel && drag == D_NONE ? handle_at(mx, my) : -1;
    if (bar_hit(mx, my) != -1) set_cursor("default");
    else if (h >= 0) set_cursor(rs[h]);
    else if (has_sel && mx > x1 && mx < x2 && my > y1 && my < y2)
        set_cursor(tool == T_MOVE ? "move" : tool == T_TEXT ? "text" : "crosshair");
    else set_cursor("crosshair");
}

static void undo(void) {
    if (typing) { commit_text(); }
    if (!shapes->len) return;
    g_ptr_array_add(redo, g_ptr_array_steal_index(shapes, shapes->len - 1));
    redraw();
}

static void redo_one(void) {
    if (!redo->len) return;
    g_ptr_array_add(shapes, g_ptr_array_steal_index(redo, redo->len - 1));
    redraw();
}

static void set_tool(int t) { commit_text(); tool = t; update_cursor(); redraw(); }

static void press_button(int b) {
    if (b < N_TOOLS) set_tool(b);
    else if (b == B_UNDO) undo();
    else if (b == B_REDO) redo_one();
    else if (b == B_OCR) act(A_OCR);
    else if (b == B_PIN) act(A_PIN);
    else if (b == B_SAVE) act(A_SAVE);
    else if (b == B_COPY) act(A_COPY);
    else if (b == B_CLOSE) finish();
}

/* ------------------------------------------------------------------ mouse and keys */

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    mx = e->x_root; my = e->y_root;
    if (e->type != GDK_BUTTON_PRESS) return TRUE;
    if (e->button == 3) { if (!has_sel || text_mode) finish(); else undo(); return TRUE; }   /* right-click: undo, or cancel */
    if (e->button != 1) return TRUE;
    if (text_mode) {   /* every press starts the selection */
        has_sel = TRUE; drag = D_SELECT;
        sx1 = sx2 = mx; sy1 = sy2 = my;
        redraw();
        return TRUE;
    }
    int b = bar_hit(mx, my);
    if (b >= 0 && b < N_BTNS) { press_button(b); return TRUE; }
    if (b >= 100) { color = b - 100; redraw(); return TRUE; }
    if (b == -2) return TRUE;
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    gboolean inside = has_sel && mx >= x1 && mx <= x2 && my >= y1 && my <= y2;
    if (typing) {
        commit_text();
        if (tool == T_TEXT && inside) { redraw(); return TRUE; }   /* click away from text: done typing */
    }
    if (!has_sel || (!inside && handle_at(mx, my) < 0 && !shapes->len)) {   /* a new selection while nothing is drawn */
        has_sel = TRUE; drag = D_SELECT;
        sx1 = sx2 = mx; sy1 = sy2 = my;
        g_ptr_array_set_size(redo, 0);   /* undone marks belonged to the old selection */
    } else if ((handle = handle_at(mx, my)) >= 0) {
        drag = D_RESIZE;
        o1 = x1; o2 = y1; o3 = x2; o4 = y2;
    } else if (inside && tool == T_MOVE) {
        drag = D_MOVE; ax = mx; ay = my; o1 = x1; o2 = y1; o3 = x2; o4 = y2;
    } else if (inside) {
        Shape *s = g_new0(Shape, 1);
        s->tool = tool; s->size = size;
        cur_color(s->rgb);
        s->x1 = s->x2 = mx; s->y1 = s->y2 = my;
        if (tool == T_TEXT) {
            s->text = g_string_new(NULL);
            typing = s;
            if (im) gtk_im_context_focus_in(im);
        } else if (tool == T_COUNTER) {
            int n = 1;
            for (guint i = 0; i < shapes->len; i++) n += ((Shape *)shapes->pdata[i])->tool == T_COUNTER;
            s->num = n;
            g_ptr_array_add(shapes, s);
            g_ptr_array_set_size(redo, 0);
        } else {
            if (tool == T_PEN || tool == T_MARKER) {
                s->pts = g_array_new(FALSE, FALSE, sizeof(double));
                g_array_append_val(s->pts, mx); g_array_append_val(s->pts, my);
            }
            cur = s; drag = D_DRAW;
        }
    }
    redraw();
    return TRUE;
}

/* what the selection occupies on screen: its box, the size label above it, and the toolbar with its tooltip */
static void damage_sel(gboolean bar) {
    double x1, y1, x2, y2;
    sel_norm(&x1, &y1, &x2, &y2);
    damage(x1, y1 - 30, MAX(x2, x1 + 120), y2, 12);
    if (bar) damage(bar_x - 170, bar_y - 44, bar_x + bar_w + 170, bar_y + bar_h, 4);
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    mx = e->x_root; my = e->y_root;
    switch (drag) {
    case D_SELECT:
        damage_sel(FALSE);
        sx2 = CLAMP(mx, 0, root_w); sy2 = CLAMP(my, 0, root_h);
        damage_sel(FALSE);
        break;
    case D_MOVE: {   /* stays on the screen */
        double dx = CLAMP(mx - ax, -o1, MAX(-o1, root_w - o3)), dy = CLAMP(my - ay, -o2, MAX(-o2, root_h - o4));
        damage_sel(TRUE);
        sx1 = o1 + dx; sy1 = o2 + dy; sx2 = o3 + dx; sy2 = o4 + dy;
        bar_layout();
        damage_sel(TRUE);
        break;
    }
    case D_RESIZE: {
        double hx = HANDLES[handle][0], hy = HANDLES[handle][1], cx = CLAMP(mx, 0, root_w), cy = CLAMP(my, 0, root_h);
        damage_sel(TRUE);
        sx1 = o1; sy1 = o2; sx2 = o3; sy2 = o4;
        if (hx == 0) sx1 = cx; else if (hx == 1) sx2 = cx;
        if (hy == 0) sy1 = cy; else if (hy == 1) sy2 = cy;
        bar_layout();
        damage_sel(TRUE);
        break;
    }
    case D_DRAW: {
        double pad = MAX(14, cur->size * 4) + cur->size + 4;
        if (cur->pts) {   /* pen, marker: only the new stroke */
            damage(cur->x2, cur->y2, mx, my, pad);
            g_array_append_val(cur->pts, mx); g_array_append_val(cur->pts, my);
        } else {
            damage(cur->x1, cur->y1, cur->x2, cur->y2, pad);
            damage(cur->x1, cur->y1, mx, my, pad);
        }
        cur->x2 = mx; cur->y2 = my;
        break;
    }
    default: {
        int h = bar_hit(mx, my);
        h = h >= 0 && h < N_BTNS ? h : -1;
        if (h != hover) {   /* nothing is redrawn while the pointer just moves about */
            hover = h;
            damage(bar_x - 170, bar_y - 44, bar_x + bar_w + 170, bar_y + bar_h, 4);
        }
        update_cursor();
    }
    }
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (e->button != 1) return TRUE;
    mx = e->x_root; my = e->y_root;
    if (drag == D_SELECT && (fabs(sx2 - sx1) < 4 || fabs(sy2 - sy1) < 4)) has_sel = FALSE;   /* a click, not a drag */
    if (text_mode) {
        gboolean go = drag == D_SELECT && has_sel;
        drag = D_NONE;
        if (go) act(A_OCR); else redraw();   /* read the text in it, and close */
        return TRUE;
    }
    if (drag == D_DRAW && cur) {
        gboolean dot = cur->tool != T_PEN && cur->tool != T_MARKER && hypot(cur->x2 - cur->x1, cur->y2 - cur->y1) < 3;
        if (dot) shape_free(cur);
        else { g_ptr_array_add(shapes, cur); g_ptr_array_set_size(redo, 0); }
        cur = NULL;
    }
    if (drag == D_SELECT && has_sel && tool == T_MOVE) tool = T_ARROW;   /* most people mark up next: like Flameshot */
    drag = D_NONE;
    update_cursor();
    redraw();
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer d) {
    double dy = 0;
    if (e->direction == GDK_SCROLL_UP) dy = -1;
    else if (e->direction == GDK_SCROLL_DOWN) dy = 1;
    else if (e->direction == GDK_SCROLL_SMOOTH) dy = e->delta_y > 0 ? 1 : e->delta_y < 0 ? -1 : 0;
    size = CLAMP(size - dy, 1, 40);
    redraw();
    return TRUE;
}

static void select_monitor_at_pointer(void) {
    GdkRectangle a;
    GdkMonitor *m = gdk_display_get_monitor_at_point(gdk_display_get_default(), (int)MAX(mx, 0), (int)MAX(my, 0));
    gdk_monitor_get_geometry(m, &a);
    has_sel = TRUE; sx1 = a.x; sy1 = a.y; sx2 = a.x + a.width; sy2 = a.y + a.height;
    if (tool == T_MOVE) tool = T_ARROW;
}

/* The letter on that key in a Latin layout: Ctrl+C and the tool keys work while you type Bengali or Russian. */
static guint latin_key(GdkEventKey *e) {
    guint k = gdk_keyval_to_lower(e->keyval);
    if (k < 0x80 || k >= 0xfd00) return k;   /* Latin already, or a key like Esc or an arrow */
    GdkKeymapKey *keys = NULL;
    guint *vals = NULL;
    int n = 0;
    if (gdk_keymap_get_entries_for_keycode(gdk_keymap_get_for_display(gdk_display_get_default()), e->hardware_keycode,
                                           &keys, &vals, &n)) {
        for (int i = 0; i < n; i++)
            if (keys[i].level == 0 && vals[i] < 0x80) { k = gdk_keyval_to_lower(vals[i]); break; }
        g_free(keys); g_free(vals);
    }
    return k;
}

static void on_im_commit(GtkIMContext *c, const char *str, gpointer d) {
    if (typing && str) { g_string_append(typing->text, str); redraw(); }
}

static gboolean on_key_release(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (typing && im) gtk_im_context_filter_keypress(im, e);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    gboolean ctrl = (e->state & GDK_CONTROL_MASK) != 0, shift = (e->state & GDK_SHIFT_MASK) != 0;
    guint k = latin_key(e);
    if (typing && !ctrl) {   /* typing a text mark */
        if (im && gtk_im_context_filter_keypress(im, e)) return TRUE;   /* a letter, a dead key, your input method */
        if (e->keyval == GDK_KEY_Escape) commit_text();
        else if (e->keyval == GDK_KEY_BackSpace) {
            if (typing->text->len) {
                const char *end = typing->text->str + typing->text->len;
                const char *prev = g_utf8_find_prev_char(typing->text->str, end);
                g_string_truncate(typing->text, prev ? (gsize)(prev - typing->text->str) : 0);
            }
        } else if (e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter) {
            g_string_append_c(typing->text, '\n');
        } else {
            gunichar c = gdk_keyval_to_unicode(e->keyval);
            if (c && !g_unichar_iscntrl(c)) g_string_append_unichar(typing->text, c);
        }
        redraw();
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Escape) { finish(); return TRUE; }
    if (text_mode) return TRUE;   /* only the mouse and Esc */
    if (ctrl && k == GDK_KEY_a) { select_monitor_at_pointer(); redraw(); return TRUE; }
    if (!has_sel) return TRUE;
    if (e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter || (ctrl && k == GDK_KEY_c)) { act(A_COPY); return TRUE; }
    if (ctrl && k == GDK_KEY_s) { act(A_SAVE); return TRUE; }
    if (ctrl && k == GDK_KEY_p) { act(A_PIN); return TRUE; }
    if (ctrl && k == GDK_KEY_t) { act(A_OCR); return TRUE; }
    if (ctrl && k == GDK_KEY_z && !shift) { undo(); return TRUE; }
    if (ctrl && (k == GDK_KEY_y || (k == GDK_KEY_z && shift))) { redo_one(); return TRUE; }
    if (e->keyval == GDK_KEY_bracketleft) { size = MAX(1, size - 1); redraw(); return TRUE; }
    if (e->keyval == GDK_KEY_bracketright) { size = MIN(40, size + 1); redraw(); return TRUE; }
    double dx = 0, dy = 0;
    switch (e->keyval) {
    case GDK_KEY_Left: dx = -1; break;
    case GDK_KEY_Right: dx = 1; break;
    case GDK_KEY_Up: dy = -1; break;
    case GDK_KEY_Down: dy = 1; break;
    }
    if (dx || dy) {
        double x1, y1, x2, y2;
        sel_norm(&x1, &y1, &x2, &y2);
        if (shift) { x2 = CLAMP(x2 + dx, x1 + 4, MAX(x1 + 4, root_w)); y2 = CLAMP(y2 + dy, y1 + 4, MAX(y1 + 4, root_h)); }   /* resize */
        else {                                                                /* move, staying on the screen */
            dx = CLAMP(dx, -x1, MAX(-x1, root_w - x2)); dy = CLAMP(dy, -y1, MAX(-y1, root_h - y2));
            x1 += dx; x2 += dx; y1 += dy; y2 += dy;
        }
        sx1 = x1; sy1 = y1; sx2 = x2; sy2 = y2;
        redraw();
        return TRUE;
    }
    if (!ctrl)
        for (int t = 0; t < N_TOOLS; t++) if (k == TOOL_KEYS[t]) { set_tool(t); return TRUE; }
    return TRUE;
}

/* ------------------------------------------------------------------ pinned screenshots */

typedef struct { GtkWidget *w; cairo_surface_t *s; int pw, ph; double zoom, scale; } Pin;   /* scale: as it was taken */

static gboolean pin_draw(GtkWidget *w, cairo_t *cr, gpointer p) {
    Pin *pn = p;
    double ac[3];
    sv_accent(ac);
    cairo_save(cr);
    cairo_scale(cr, pn->zoom / pn->scale, pn->zoom / pn->scale);
    cairo_set_source_surface(cr, pn->s, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_rectangle(cr, 0.5, 0.5, gtk_widget_get_allocated_width(w) - 1, gtk_widget_get_allocated_height(w) - 1);
    cairo_set_source_rgb(cr, ac[0], ac[1], ac[2]);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return TRUE;
}

static void pin_resize(Pin *p) {
    gtk_window_resize(GTK_WINDOW(p->w), MAX(16, (int)(p->pw / p->scale * p->zoom)), MAX(16, (int)(p->ph / p->scale * p->zoom)));
    gtk_widget_queue_draw(p->w);
}

static void pin_free(gpointer p) {
    Pin *pn = p;
    cairo_surface_destroy(pn->s);
    g_free(pn);
    n_pins--;
    g_timeout_add(500, trim, NULL);
}
static void pin_close(GtkMenuItem *i, gpointer w) { gtk_widget_destroy(w); }
static void pin_copy(GtkMenuItem *i, gpointer p) {
    Pin *pn = p;
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(pn->s, 0, 0, pn->pw, pn->ph);
    if (!pb) return;
    sv_copy_image(pb);
    g_object_unref(pb);
    extras_toast("Screenshot copied", NULL);
}
static void pin_save(GtkMenuItem *i, gpointer p) {
    Pin *pn = p;
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(pn->s, 0, 0, pn->pw, pn->ph);
    char *path = save_path();
    GError *e = NULL;
    if (pb && gdk_pixbuf_save(pb, path, "png", &e, NULL)) {
        char *base = g_path_get_basename(path), *b = g_strdup_printf("Pictures/Screenshots/%s", base);
        extras_toast("Screenshot saved", b);
        g_free(base); g_free(b);
    } else {
        extras_toast("Couldn't save the screenshot", e ? e->message : NULL);
        g_clear_error(&e);
    }
    if (pb) g_object_unref(pb);
    g_free(path);
}

/* The menu is destroyed after its item has run: destroying it as it closes would cancel the click. */
static gboolean menu_destroy(gpointer m) { gtk_widget_destroy(m); g_object_unref(m); return G_SOURCE_REMOVE; }
static void menu_closed(GtkMenuShell *m, gpointer d) { g_idle_add(menu_destroy, g_object_ref(m)); }

static gboolean pin_press(GtkWidget *w, GdkEventButton *e, gpointer p) {
    if (e->type == GDK_2BUTTON_PRESS && e->button == 1) { gtk_widget_destroy(w); return TRUE; }
    if (e->type != GDK_BUTTON_PRESS) return TRUE;
    if (e->button == 1) { gtk_window_begin_move_drag(GTK_WINDOW(w), 1, (int)e->x_root, (int)e->y_root, e->time); return TRUE; }
    if (e->button == 3) {
        GtkWidget *m = gtk_menu_new();
        static const char *labels[] = { "Copy", "Save to Pictures/Screenshots", "Close" };
        GCallback cbs[] = { G_CALLBACK(pin_copy), G_CALLBACK(pin_save), G_CALLBACK(pin_close) };
        for (int i = 0; i < 3; i++) {
            GtkWidget *it = gtk_menu_item_new_with_label(labels[i]);
            g_signal_connect(it, "activate", cbs[i], i == 2 ? (gpointer)w : p);
            gtk_menu_shell_append(GTK_MENU_SHELL(m), it);
        }
        gtk_widget_show_all(m);
        g_signal_connect(m, "deactivate", G_CALLBACK(menu_closed), NULL);
        gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)e);
    }
    return TRUE;
}

static gboolean pin_scroll(GtkWidget *w, GdkEventScroll *e, gpointer p) {
    Pin *pn = p;
    double dy = e->direction == GDK_SCROLL_UP ? -1 : e->direction == GDK_SCROLL_DOWN ? 1 :
                e->direction == GDK_SCROLL_SMOOTH ? (e->delta_y > 0 ? 1 : e->delta_y < 0 ? -1 : 0) : 0;
    pn->zoom = CLAMP(pn->zoom * (dy < 0 ? 1.1 : dy > 0 ? 1 / 1.1 : 1), 0.2, 5);
    pin_resize(pn);
    return TRUE;
}

static gboolean pin_key(GtkWidget *w, GdkEventKey *e, gpointer p) {
    if (e->keyval == GDK_KEY_Escape) gtk_widget_destroy(w);
    else if ((e->state & GDK_CONTROL_MASK) && gdk_keyval_to_lower(e->keyval) == GDK_KEY_c) pin_copy(NULL, p);
    return TRUE;
}

/* The screenshot stays on top of everything, where you took it. Drag it; wheel zooms; double-click closes;
 * right-click: copy, save, close. */
static void pin(GdkPixbuf *pb, double x, double y) {
    Pin *p = g_new0(Pin, 1);
    p->pw = gdk_pixbuf_get_width(pb); p->ph = gdk_pixbuf_get_height(pb);
    p->s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, p->pw, p->ph);   /* drawn from as it is, at any zoom */
    cairo_t *cr = cairo_create(p->s);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    p->zoom = 1;
    p->scale = scale;
    n_pins++;
    p->w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(p->w);
    gtk_window_set_title(w, "Pinned screenshot");
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_keep_above(w, TRUE);
    gtk_window_set_skip_taskbar_hint(w, TRUE);
    gtk_window_set_skip_pager_hint(w, TRUE);
    gtk_window_set_type_hint(w, GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_icon_name(w, SV_APP_ID);
    gtk_widget_set_app_paintable(p->w, TRUE);
    gtk_widget_set_tooltip_text(p->w, "Drag to move · wheel to zoom · double-click to close · right-click for more");
    gtk_widget_add_events(p->w, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(p->w, "draw", G_CALLBACK(pin_draw), p);
    g_signal_connect(p->w, "button-press-event", G_CALLBACK(pin_press), p);
    g_signal_connect(p->w, "scroll-event", G_CALLBACK(pin_scroll), p);
    g_signal_connect(p->w, "key-press-event", G_CALLBACK(pin_key), p);
    g_object_set_data_full(G_OBJECT(p->w), "pin", p, pin_free);
    gtk_window_move(w, (int)x, (int)y);
    pin_resize(p);
    gtk_widget_show(p->w);
    logmsg("pinned a %d × %d screenshot", gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
}

/* ------------------------------------------------------------------ starting */

static guint grab_src;
static int grab_tries;

static gboolean grab_keyboard(gpointer d) {
    if (!wins) { grab_src = 0; return G_SOURCE_REMOVE; }
    GtkWidget *w = wins->pdata[0];
    if (!grab_tries) gtk_window_present(GTK_WINDOW(w));
    /* X11: hold the keyboard and mouse so Esc and the shortcuts always reach us. The shortcut's own key grab
     * is released a moment after you press it: keep trying for 1.5 s. */
    if (!sv_wayland && gdk_seat_grab(gdk_display_get_default_seat(gdk_display_get_default()), gtk_widget_get_window(w),
                                     GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL) != GDK_GRAB_SUCCESS
        && ++grab_tries < 50) return G_SOURCE_CONTINUE;
    grab_src = 0;
    return G_SOURCE_REMOVE;
}

static void got_screen(GdkPixbuf *pb, gpointer d) {
    starting = FALSE;
    if (!pb) { text_mode = FALSE; return; }   /* cancelled, or screenshots aren't possible here (already said) */
    GdkDisplay *disp = gdk_display_get_default();
    GdkWindow *root = gdk_get_default_root_window();
    shot_w = gdk_pixbuf_get_width(pb); shot_h = gdk_pixbuf_get_height(pb);
    shot = cairo_image_surface_create(CAIRO_FORMAT_RGB24, shot_w, shot_h);   /* once: a pixbuf is converted on every draw */
    cairo_t *cr = cairo_create(shot);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(shot);
    root_w = gdk_window_get_width(root); root_h = gdk_window_get_height(root);
    scale = (double)shot_w / root_w;
    shapes = g_ptr_array_new_with_free_func(shape_free);
    redo = g_ptr_array_new_with_free_func(shape_free);
    wins = g_ptr_array_new();
    geos = g_ptr_array_new();
    has_sel = FALSE; drag = D_NONE; tool = T_MOVE; hover = -1;
    int n = gdk_display_get_n_monitors(disp);
    for (int i = 0; i < n; i++) {   /* one window per monitor: Wayland desktops only let a window fill one */
        GdkRectangle *g = g_new(GdkRectangle, 1);
        gdk_monitor_get_geometry(gdk_display_get_monitor(disp, i), g);
        GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        GtkWindow *gw = GTK_WINDOW(w);
        gtk_window_set_title(gw, "SuperV screenshot");
        gtk_window_set_decorated(gw, FALSE);
        gtk_window_set_keep_above(gw, TRUE);
        gtk_window_set_skip_taskbar_hint(gw, TRUE);
        gtk_window_set_skip_pager_hint(gw, TRUE);
        gtk_window_set_icon_name(gw, SV_APP_ID);
        gtk_widget_set_app_paintable(w, TRUE);
        gtk_window_move(gw, g->x, g->y);
        gtk_window_set_default_size(gw, g->width, g->height);
        gtk_window_fullscreen_on_monitor(gw, gdk_display_get_default_screen(disp), i);
        gtk_widget_add_events(w, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                 GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
        g_signal_connect_data(w, "draw", G_CALLBACK(on_draw), g, (GClosureNotify)(void (*)(void))g_free, 0);
        g_signal_connect(w, "button-press-event", G_CALLBACK(on_press), NULL);
        g_signal_connect(w, "button-release-event", G_CALLBACK(on_release), NULL);
        g_signal_connect(w, "motion-notify-event", G_CALLBACK(on_motion), NULL);
        g_signal_connect(w, "scroll-event", G_CALLBACK(on_scroll), NULL);
        g_signal_connect(w, "key-press-event", G_CALLBACK(on_key), NULL);
        g_signal_connect(w, "key-release-event", G_CALLBACK(on_key_release), NULL);
        g_signal_connect(w, "delete-event", G_CALLBACK(gtk_true), NULL);
        g_ptr_array_add(wins, w);
        g_ptr_array_add(geos, g);
        gtk_widget_show(w);
    }
    im = gtk_im_multicontext_new();
    gtk_im_context_set_client_window(im, gtk_widget_get_window(wins->pdata[0]));
    gtk_im_context_set_use_preedit(im, FALSE);
    g_signal_connect(im, "commit", G_CALLBACK(on_im_commit), NULL);
    last = NULL;
    set_cursor("crosshair");
    grab_tries = 0;
    if (grab_src) g_source_remove(grab_src);
    grab_src = g_timeout_add(30, grab_keyboard, NULL);
    logmsg("screen frozen: %d × %d pixels on %d monitor%s", shot_w, shot_h, n, n == 1 ? "" : "s");
}

static gboolean capture_now(gpointer d) { extras_capture_screen(got_screen, NULL); return G_SOURCE_REMOVE; }

void shot_start(int delay_s) {
    if (wins || starting || extras_capturing()) return;   /* one thing reads the screen at a time */
    text_mode = FALSE;
    starting = TRUE;
    if (delay_s > 0) g_timeout_add_seconds(delay_s, capture_now, NULL);
    else capture_now(NULL);
}

/* Copy text from the screen: the same frozen screen, but you only drag over the text. */
void shot_text(void) {
    if (wins || starting || extras_capturing()) return;
    text_mode = TRUE;
    starting = TRUE;
    capture_now(NULL);
}

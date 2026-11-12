/*
 * SuperV — Windows-style (Win+V) clipboard history for Linux, plus the Win+. emoji panel (emoji.c).
 * GNOME, KDE Plasma, Cinnamon, MATE, Xfce… on Wayland and X11.
 *
 *   Super+V (or your own shortcut)  open / close the panel
 *   Super+. (when turned on)         the emoji panel, see emoji.c
 *   Up / Down                        move between items
 *   Enter / click                    paste into the app you were using
 *   Delete                           remove the selected item
 *   Esc / click elsewhere            close
 *   ⋯ menu                           Pin / Unpin, Delete, Clear all
 *
 * MIT License.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/keysym.h>
#include "superv.h"

#define APP_ID   SV_APP_ID
#define VERSION  SV_VERSION
#define REPO_URL SV_REPO_URL
#ifndef SETUP_DIR
#define SETUP_DIR "/usr/lib/superv"   /* where packages put superv-setup (set by the Makefile) */
#endif
#define WIN_W   380
#define WIN_H   500

typedef struct {
    char *id, *hash, *text;          /* text is NULL for images */
    gboolean image, pinned;
    gint64 time, pasted;             /* pasted: when you last pasted it from the panel (0: not since it was copied) */
} Item;

static struct {
    gboolean history_enabled, clear_on_reboot, restore_on_exit;
    int max_items, max_text_bytes, paste_delay_ms, pos_x, pos_y, width, height, font_scale;
    gboolean setup_done, emoji_panel;
    gboolean feat[4];                  /* plain paste, colour picker, text from screen, screenshot: off until turned on */
    gboolean tip_seen, emoji_asked;    /* the "more features" tip was answered; "turn on emoji?" was declined */
    char *paste_keys, *hotkey, *theme, *palette;
} cfg;

static GPtrArray *items;             /* newest first */
static char *data_dir, *img_dir, *hist_path, *cfg_dir, *cfg_path;
char *sv_self_exe, *sv_self_dir;
gboolean sv_debug, sv_wayland;
static char *current_id, *paste_err, *image_link, *link_candidate;
static gboolean reading, had_focus, capturing, prev_is_x11, geom_dirty;
static int thumb_w, enable_tries;
static guint relayout_src;
static gint64 ignore_until, hotkey_applied_at;
static guint pending_src;
static int ui_fd = -1;
static int (*xtest_fake)(Display *, unsigned int, int, unsigned long);
GtkApplication *sv_app;
static GtkClipboard *clip, *prim;
static GtkWidget *win, *listbox, *stack, *clear_btn, *switch_w, *settings_pop, *hotkey_btn, *hint_lbl;
static GtkWidget *hk_status, *banner, *banner_lbl, *banner_btn, *theme_btns[3], *size_lbl;
static GtkWidget *emoji_sw, *emoji_lbl, *emoji_btn;

/* Small Windows shortcuts (extras.c). Each can be turned off; nothing of it runs until you press it. */
enum { F_PLAIN, F_COLOR, F_TEXT, F_SHOT, N_FEAT };
static const struct { const char *id, *arg, *label, *what, *def, *slug, *name; } FEAT[N_FEAT] = {
    { "plain_paste", "--plain-paste", "Paste as plain text", "pastes without formatting",
      "<Primary><Super><Alt>v", "plain-paste", "SuperV Plain Paste" },
    { "color_picker", "--pick-color", "Colour picker", "copies the colour you click",
      "<Shift><Super>c", "color-picker", "SuperV Colour Picker" },
    { "text_extractor", "--extract-text", "Copy text from screen", "copies the text in an area",
      "<Shift><Super>t", "text-extractor", "SuperV Text Extractor" },
    { "screenshot", "--screenshot", "Screenshot", "select, draw on it, copy, save or pin",
      "<Shift><Super>s", "screenshot", "SuperV Screenshot" },
};
static GtkWidget *feat_sw[N_FEAT], *feat_lbl[N_FEAT];
static GtkCssProvider *font_css, *pal_css;

/* Colour palettes for every SuperV window: an accent for dark and for light themes. Violet is SuperV's. */
static const struct { const char *id, *name, *dark, *light; } PALETTES[] = {
    { "violet",   "Violet · SuperV's colour",                                  "#a78bfa", "#7c3aed" },
    { "azure",    "Azure · calm and technical (developers, engineers)",       "#7cb4ff", "#2563eb" },
    { "graphite", "Graphite · neutral and formal (office, finance, law)",      "#b9c2d0", "#475569" },
    { "emerald",  "Emerald · fresh and steady (health, science, education)",   "#62d6a8", "#0f8a5f" },
    { "amber",    "Amber · warm and friendly (writing, teaching, support)",    "#f3c26d", "#b45309" },
    { "rose",     "Rose · soft and expressive (design, marketing, social)",    "#f4a3b8", "#c2416b" },
};
#define N_PAL G_N_ELEMENTS(PALETTES)
static GtkWidget *pal_btns[6];

static int palette_index(void) {
    for (guint i = 0; i < N_PAL; i++) if (!g_strcmp0(cfg.palette, PALETTES[i].id)) return i;
    return 0;
}

const char *sv_palette_id(void) { return PALETTES[palette_index()].id; }
void sv_palette_colors(const char **dark, const char **light) {
    *dark = PALETTES[palette_index()].dark; *light = PALETTES[palette_index()].light;
}
void sv_accent(double rgb[3]) {   /* the accent as drawn over a screenshot (the dark-theme one: it stands out) */
    GdkRGBA c;
    gdk_rgba_parse(&c, PALETTES[palette_index()].dark);
    rgb[0] = c.red; rgb[1] = c.green; rgb[2] = c.blue;
}

static void apply_palette(void) {
    if (!pal_css) return;
    int pi = palette_index();
    GString *css = g_string_new(NULL);
    g_string_append_printf(css, "@define-color sv_d %s; @define-color sv_l %s;", PALETTES[pi].dark, PALETTES[pi].light);
    for (guint k = 0; k < N_PAL; k++)
        g_string_append_printf(css, ".cbh-dark .cbh-settings button.cbh-sw%u { background-color: %s; }"
                                    ".cbh-light .cbh-settings button.cbh-sw%u { background-color: %s; }",
                               k, PALETTES[k].dark, k, PALETTES[k].light);
    gtk_css_provider_load_from_data(pal_css, css->str, -1, NULL);
    g_string_free(css, TRUE);
}
static gboolean syncing;
static GHashTable *thumbs;

static void refresh(int keep);
static void on_menu_closed(GtkPopover *p, gpointer d);

static void logmsg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap); va_end(ap);
    GDateTime *t = g_date_time_new_now_local();
    char *ts = g_date_time_format(t, "%H:%M:%S");
    g_printerr("%s [superv] %s\n", ts, m);
    g_free(ts); g_free(m); g_date_time_unref(t);
}

/* ------------------------------------------------------------------ config */

static void config_save(void) {
    GKeyFile *k = g_key_file_new();
    g_key_file_set_boolean(k, "superv", "history_enabled", cfg.history_enabled);
    g_key_file_set_integer(k, "superv", "max_items", cfg.max_items);
    g_key_file_set_integer(k, "superv", "max_text_bytes", cfg.max_text_bytes);
    g_key_file_set_boolean(k, "superv", "clear_unpinned_on_reboot", cfg.clear_on_reboot);
    g_key_file_set_boolean(k, "superv", "restore_when_owner_exits", cfg.restore_on_exit);
    g_key_file_set_string(k, "superv", "paste_keys", cfg.paste_keys);
    g_key_file_set_integer(k, "superv", "paste_delay_ms", cfg.paste_delay_ms);
    g_key_file_set_string(k, "superv", "hotkey", cfg.hotkey);
    g_key_file_set_string(k, "superv", "theme", cfg.theme);
    g_key_file_set_string(k, "superv", "palette", cfg.palette);
    g_key_file_set_integer(k, "superv", "font_scale", cfg.font_scale);
    g_key_file_set_integer(k, "superv", "width", cfg.width);
    g_key_file_set_integer(k, "superv", "height", cfg.height);
    g_key_file_set_integer(k, "superv", "pos_x", cfg.pos_x);
    g_key_file_set_integer(k, "superv", "pos_y", cfg.pos_y);
    g_key_file_set_boolean(k, "superv", "shortcut_configured", cfg.setup_done);
    g_key_file_set_boolean(k, "superv", "emoji_panel", cfg.emoji_panel);
    for (int f = 0; f < N_FEAT; f++) g_key_file_set_boolean(k, "superv", FEAT[f].id, cfg.feat[f]);
    g_key_file_set_boolean(k, "superv", "features_tip_seen", cfg.tip_seen);
    g_key_file_set_boolean(k, "superv", "emoji_asked", cfg.emoji_asked);
    g_key_file_set_comment(k, "superv", NULL,
        " SuperV settings. After editing run: superv --quit && superv --daemon &\n"
        " Features are off until you turn them on (the switches in the gear menu do the same):\n"
        "   emoji_panel (Super+.), plain_paste (Ctrl+Super+Alt+V), color_picker (Super+Shift+C),\n"
        "   text_extractor (Super+Shift+T), screenshot (Super+Shift+S)\n"
        "   their shortcuts are in emoji.ini / shortcuts.ini; change one: superv-setup --feature color_picker --hotkey '<Super>k'\n"
        " paste_keys: shift+insert (default; works in terminals too), ctrl+v, ctrl+shift+v, none\n"
        " theme: auto (follow the desktop), light, dark: for every SuperV window      font_scale: text size in % (80-160)\n"
        " palette: violet (SuperV's), azure, graphite, emerald, amber, rose: the colour of every SuperV window\n"
        " width/height/pos_x/pos_y: panel size and position (drag it; double-click its header to reset)\n"
        " hotkey: change it from the gear menu in the panel, or: superv-setup --hotkey '<Ctrl><Alt>v'",
        NULL);
    g_mkdir_with_parents(cfg_dir, 0700);
    g_key_file_save_to_file(k, cfg_path, NULL);
    g_chmod(cfg_path, 0600);
    g_key_file_unref(k);
}

#define DEFAULT_HOTKEY "<Super>v"

/* Everything except the shortcut, which is applied separately (it changes desktop settings). */
static void config_defaults(void) {
    cfg.history_enabled = TRUE; cfg.clear_on_reboot = TRUE; cfg.restore_on_exit = TRUE;
    cfg.max_items = 25; cfg.max_text_bytes = 4 * 1024 * 1024; cfg.paste_delay_ms = 180;
    g_free(cfg.paste_keys); cfg.paste_keys = g_strdup("shift+insert");
    g_free(cfg.theme); cfg.theme = g_strdup("auto");
    g_free(cfg.palette); cfg.palette = g_strdup("violet");
    cfg.font_scale = 100;
    cfg.width = WIN_W + 20; cfg.height = WIN_H + 20; cfg.pos_x = cfg.pos_y = -1;
}

/* A colour or setting kept by SuperDot 1.0 or SuperV 2.0's emoji panel, before SuperV had one palette. */
static char *old_emoji_setting(const char *key) {
    static const char *files[][3] = { { "superv", "emoji.ini", "emoji" }, { "superdot", "config.ini", "superdot" } };
    char *v = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(files) && !v; i++) {
        GKeyFile *k = g_key_file_new();
        char *p = g_build_filename(g_get_user_config_dir(), files[i][0], files[i][1], NULL);
        if (g_key_file_load_from_file(k, p, G_KEY_FILE_NONE, NULL)) v = g_key_file_get_string(k, files[i][2], key, NULL);
        g_free(p); g_key_file_unref(k);
    }
    return v ? g_strstrip(v) : NULL;
}

static void config_load(void) {
    config_defaults();
    cfg.hotkey = g_strdup(DEFAULT_HOTKEY);
    /* Only the clipboard panel is on at first: every other feature takes a desktop shortcut, so you choose. */
    cfg.emoji_panel = FALSE;
    for (int f = 0; f < N_FEAT; f++) cfg.feat[f] = FALSE;
    GKeyFile *k = g_key_file_new();
    gboolean loaded = g_key_file_load_from_file(k, cfg_path, G_KEY_FILE_NONE, NULL);
    if (!loaded || !g_key_file_has_key(k, "superv", "emoji_panel", NULL)) {
        /* never decided here: people who had the emoji panel (SuperDot, its own app, or SuperV 2.0) keep it */
        char *old = g_build_filename(g_get_user_config_dir(), "superdot", "config.ini", NULL);
        char *v20 = g_build_filename(g_get_user_config_dir(), "superv", "emoji.ini", NULL);
        cfg.emoji_panel = g_file_test(old, G_FILE_TEST_EXISTS) || g_file_test(v20, G_FILE_TEST_EXISTS);
        g_free(old); g_free(v20);
    }
    if (!loaded || !g_key_file_has_key(k, "superv", "palette", NULL)) {
        /* a colour picked in the emoji panel before (azure was its default, not a choice) becomes SuperV's colour */
        char *c = old_emoji_setting("color");
        if (c && g_strcmp0(c, "azure")) { g_free(cfg.palette); cfg.palette = c; } else g_free(c);
    }
    if (loaded) {
        GError *e = NULL;
#define KB(n, f) do { gboolean v = g_key_file_get_boolean(k, "superv", n, &e); if (!e) f = v; g_clear_error(&e); } while (0)
#define KI(n, f) do { int v = g_key_file_get_integer(k, "superv", n, &e); if (!e && v > 0) f = v; g_clear_error(&e); } while (0)
#define KS(n, f) do { char *v = g_key_file_get_string(k, "superv", n, NULL); if (v) { g_strstrip(v); g_free(f); f = v; } } while (0)
#define KN(n, f) do { int v = g_key_file_get_integer(k, "superv", n, &e); if (!e) f = v; g_clear_error(&e); } while (0)
        KB("history_enabled", cfg.history_enabled);
        KB("clear_unpinned_on_reboot", cfg.clear_on_reboot);
        KB("restore_when_owner_exits", cfg.restore_on_exit);
        KI("max_items", cfg.max_items);
        KI("max_text_bytes", cfg.max_text_bytes);
        KI("paste_delay_ms", cfg.paste_delay_ms);
        KS("paste_keys", cfg.paste_keys);
        KS("hotkey", cfg.hotkey);
        KS("theme", cfg.theme);
        KS("palette", cfg.palette);
        KI("font_scale", cfg.font_scale);
        KI("width", cfg.width);
        KI("height", cfg.height);
        KN("pos_x", cfg.pos_x);
        KN("pos_y", cfg.pos_y);
        KB("shortcut_configured", cfg.setup_done);
        KB("emoji_panel", cfg.emoji_panel);
        for (int f = 0; f < N_FEAT; f++) KB(FEAT[f].id, cfg.feat[f]);
        KB("features_tip_seen", cfg.tip_seen);
        KB("emoji_asked", cfg.emoji_asked);
    }
    g_key_file_unref(k);
    config_save();
}

/* ------------------------------------------------------------------ history store */

static char *boot_id(void) {
    char *s = NULL;
    if (!g_file_get_contents("/proc/sys/kernel/random/boot_id", &s, NULL, NULL)) return g_strdup("");
    return g_strstrip(s);
}

static char *img_path(Item *it) { return g_strdup_printf("%s/%s.png", img_dir, it->hash); }
static char *thumb_path(const char *hash) { return g_strdup_printf("%s/%s.t.png", img_dir, hash); }   /* small copy for the list */

static void item_free(Item *it) { g_free(it->id); g_free(it->hash); g_free(it->text); g_free(it); }

static Item *find_item(const char *id, const char *hash) {
    for (guint i = 0; i < items->len; i++) {
        Item *it = items->pdata[i];
        if ((id && !strcmp(it->id, id)) || (hash && !strcmp(it->hash, hash))) return it;
    }
    return NULL;
}

static void history_write(void) {
    GKeyFile *k = g_key_file_new();
    char *bid = boot_id();
    g_key_file_set_string(k, "meta", "boot_id", bid);
    const char **order = g_new0(const char *, items->len + 1);
    for (guint i = 0; i < items->len; i++) {
        Item *it = items->pdata[i];
        order[i] = it->id;
        g_key_file_set_string(k, it->id, "kind", it->image ? "image" : "text");
        g_key_file_set_string(k, it->id, "hash", it->hash);
        g_key_file_set_boolean(k, it->id, "pinned", it->pinned);
        g_key_file_set_int64(k, it->id, "time", it->time);
        if (it->pasted) g_key_file_set_int64(k, it->id, "pasted", it->pasted);
        if (it->text) g_key_file_set_string(k, it->id, "text", it->text);
    }
    g_key_file_set_string_list(k, "meta", "order", order, items->len);
    gsize len;
    char *data = g_key_file_to_data(k, &len, NULL);
    g_file_set_contents(hist_path, data, len, NULL);
    g_chmod(hist_path, 0600);
    g_free(data); g_free(order); g_free(bid); g_key_file_unref(k);
}

/* Every change marks the list for redrawing and is written to disk a moment later, off the path of whatever
 * you are doing (a paste, opening the panel); several changes in a row become one write. */
static guint save_src;
static gboolean rows_dirty = TRUE;

static gboolean history_write_cb(gpointer d) { save_src = 0; history_write(); return G_SOURCE_REMOVE; }

static void history_save(void) {
    rows_dirty = TRUE;
    if (!save_src) save_src = g_timeout_add(400, history_write_cb, NULL);
}

static void history_flush(void) {   /* before quitting or restarting */
    if (save_src) { g_source_remove(save_src); save_src = 0; history_write(); }
}

static void history_load(void) {
    items = g_ptr_array_new();
    GKeyFile *k = g_key_file_new();
    if (g_key_file_load_from_file(k, hist_path, G_KEY_FILE_NONE, NULL)) {
        char *saved = g_key_file_get_string(k, "meta", "boot_id", NULL), *now = boot_id();
        gboolean rebooted = g_strcmp0(saved, now) != 0;
        gsize n = 0;
        char **order = g_key_file_get_string_list(k, "meta", "order", &n, NULL);
        for (gsize i = 0; i < n; i++) {
            const char *id = order[i];
            char *kind = g_key_file_get_string(k, id, "kind", NULL);
            char *hash = g_key_file_get_string(k, id, "hash", NULL);
            gboolean pinned = g_key_file_get_boolean(k, id, "pinned", NULL);
            if (!kind || !hash || (rebooted && cfg.clear_on_reboot && !pinned)) {
                g_free(kind); g_free(hash); continue;
            }
            Item *it = g_new0(Item, 1);
            it->id = g_strdup(id); it->hash = hash; it->pinned = pinned;
            it->image = !strcmp(kind, "image");
            it->time = g_key_file_get_int64(k, id, "time", NULL);
            it->pasted = rebooted ? 0 : g_key_file_get_int64(k, id, "pasted", NULL);
            g_free(kind);
            gboolean ok;
            if (it->image) {
                char *p = img_path(it); ok = g_file_test(p, G_FILE_TEST_EXISTS); g_free(p);
            } else {
                it->text = g_key_file_get_string(k, id, "text", NULL); ok = it->text != NULL;
            }
            if (ok) g_ptr_array_add(items, it); else item_free(it);
        }
        g_strfreev(order); g_free(saved); g_free(now);
    }
    g_key_file_unref(k);
    /* delete images (and their small copies, and half-written ones) no longer referenced */
    GDir *d = g_dir_open(img_dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        gboolean used = FALSE;
        for (guint i = 0; i < items->len && !used; i++) {
            Item *it = items->pdata[i];
            used = it->image && g_str_has_prefix(name, it->hash) && !g_str_has_suffix(name, ".tmp");
        }
        if (!used) { char *p = g_build_filename(img_dir, name, NULL); g_unlink(p); g_free(p); }
    }
    if (d) g_dir_close(d);
    history_save();
}

static void drop_item(Item *it) {
    g_ptr_array_remove(items, it);
    if (it->image && !find_item(NULL, it->hash)) {
        char *p = img_path(it), *t = thumb_path(it->hash);
        g_unlink(p); g_unlink(t);
        g_free(p); g_free(t);
    }
    item_free(it);
}

/* ---- "what did I paste last?": pasting keeps the list's order and ticks the item; the panel then opens on the
 * next one in the direction you are going. A new copy ends the run, and starts a new batch once the last paste
 * is two minutes old (the ticks of the old batch go). */
static gboolean run_active;
static int run_dir, run_idx = -1;
static gint64 last_paste_at;

static void paste_run_end(void) {
    run_active = FALSE; run_dir = 0; run_idx = -1;
    if (g_get_monotonic_time() - last_paste_at > 120 * G_USEC_PER_SEC)
        for (guint i = 0; i < items->len; i++) ((Item *)items->pdata[i])->pasted = 0;
}

static Item *last_pasted(void) {
    Item *best = NULL;
    for (guint i = 0; i < items->len; i++) {
        Item *it = items->pdata[i];
        if (it->pasted && (!best || it->pasted >= best->pasted)) best = it;
    }
    return best;
}

static void move_to_top(Item *it) {   /* copied again */
    g_ptr_array_remove(items, it);
    g_ptr_array_insert(items, 0, it);
    it->time = g_get_real_time() / 1000000;
    paste_run_end();
    it->pasted = 0;
    history_save();
}

static Item *insert_item(Item *it) {
    g_ptr_array_insert(items, 0, it);
    paste_run_end();
    int unpinned = 0;
    for (guint i = 0; i < items->len;) {
        Item *x = items->pdata[i];
        if (!x->pinned && ++unpinned > cfg.max_items) { drop_item(x); continue; }
        i++;
    }
    history_save();
    return it;
}

static Item *new_item(char *hash, gboolean image) {
    Item *it = g_new0(Item, 1);
    it->id = g_uuid_string_random();
    it->hash = hash; it->image = image;
    it->time = g_get_real_time() / 1000000;
    return it;
}

static Item *add_text(const char *text) {
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(c, (const guchar *)"t", 1);
    g_checksum_update(c, (const guchar *)text, strlen(text));
    char *hash = g_strdup(g_checksum_get_string(c));
    g_checksum_free(c);
    Item *it = find_item(NULL, hash);
    if (it) { g_free(hash); move_to_top(it); return it; }
    it = new_item(hash, FALSE);
    it->text = g_strdup(text);
    return insert_item(it);
}

/* ---- images are hashed, saved as PNG and given a small copy for the list in a worker thread: a full-screen
 * screenshot takes a few hundred ms to encode, and the panels must open instantly meanwhile. */
#define THUMB_W 480   /* the small copy the list shows: quick to load, sharp at the panel's usual width */
#define THUMB_H 240

static void fit(int w, int h, int max_w, int max_h, int *ow, int *oh) {   /* keep the proportions, never enlarge */
    double k = MIN(1.0, MIN((double)max_w / w, (double)max_h / h));
    *ow = MAX(1, (int)(w * k)); *oh = MAX(1, (int)(h * k));
}

static gboolean save_png(GdkPixbuf *pb, const char *path) {   /* whole or not at all */
    char *tmp = g_strconcat(path, ".tmp", NULL);
    gboolean ok = gdk_pixbuf_save(pb, tmp, "png", NULL, "compression", "2", NULL);
    if (ok) { g_chmod(tmp, 0600); ok = g_rename(tmp, path) == 0; }
    if (!ok) g_unlink(tmp);
    g_free(tmp);
    return ok;
}

static void save_thumb(GdkPixbuf *pb, const char *hash) {
    int w, h;
    fit(gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb), THUMB_W, THUMB_H, &w, &h);
    GdkPixbuf *small = gdk_pixbuf_scale_simple(pb, w, h, GDK_INTERP_BILINEAR);
    char *t = thumb_path(hash);
    if (small) { save_png(small, t); g_object_unref(small); }
    g_free(t);
}

typedef struct { GdkPixbuf *pb; char *hash; gboolean ok, from_clipboard; } ImgJob;
static GMutex img_lock;   /* one image at a time: two copies of the same picture must not write the same file */

static void img_job_free(gpointer p) { ImgJob *j = p; g_object_unref(j->pb); g_free(j->hash); g_free(j); }

static void img_job_run(GTask *task, gpointer src, gpointer data, GCancellable *cancel) {
    ImgJob *j = data;
    GdkPixbuf *pb = j->pb;
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA1);
    int meta[4] = { gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
                    gdk_pixbuf_get_rowstride(pb), gdk_pixbuf_get_has_alpha(pb) };
    g_checksum_update(c, (const guchar *)"i", 1);
    g_checksum_update(c, (const guchar *)meta, sizeof meta);
    g_checksum_update(c, gdk_pixbuf_read_pixels(pb), gdk_pixbuf_get_byte_length(pb));
    j->hash = g_strdup(g_checksum_get_string(c));
    g_checksum_free(c);
    char *p = g_strdup_printf("%s/%s.png", img_dir, j->hash), *t = thumb_path(j->hash);
    g_mutex_lock(&img_lock);
    j->ok = g_file_test(p, G_FILE_TEST_EXISTS) || save_png(pb, p);
    if (j->ok && !g_file_test(t, G_FILE_TEST_EXISTS)) save_thumb(pb, j->hash);
    g_mutex_unlock(&img_lock);
    g_free(p); g_free(t);
    g_task_return_boolean(task, TRUE);
}

static void got_item(Item *it);

static void img_job_done(GObject *src, GAsyncResult *res, gpointer d) {
    ImgJob *j = g_task_get_task_data(G_TASK(res));
    if (j->from_clipboard) reading = FALSE;
    if (j->ok) {
        Item *it = find_item(NULL, j->hash);
        if (it) move_to_top(it);
        else it = insert_item(new_item(g_strdup(j->hash), TRUE));
        got_item(it);
    } else if (sv_debug) {
        logmsg("image could not be saved");
    }
}

static void add_image(GdkPixbuf *pb, gboolean from_clipboard) {
    ImgJob *j = g_new0(ImgJob, 1);
    j->pb = g_object_ref(pb);
    j->from_clipboard = from_clipboard;
    GTask *task = g_task_new(NULL, NULL, img_job_done, NULL);
    g_task_set_task_data(task, j, img_job_free);
    g_task_run_in_thread(task, img_job_run);
    g_object_unref(task);
}

static void clear_unpinned(void) {
    for (guint i = 0; i < items->len;) {
        Item *x = items->pdata[i];
        if (!x->pinned) drop_item(x); else i++;
    }
    history_save();
}

/* ------------------------------------------------------------------ key injection (auto paste) */

static const struct { const char *name; int code; KeySym sym; } KEYS[] = {
    { "ctrl", KEY_LEFTCTRL, XK_Control_L }, { "control", KEY_LEFTCTRL, XK_Control_L },
    { "shift", KEY_LEFTSHIFT, XK_Shift_L }, { "alt", KEY_LEFTALT, XK_Alt_L },
    { "super", KEY_LEFTMETA, XK_Super_L },  { "insert", KEY_INSERT, XK_Insert },
    { "v", KEY_V, XK_v },       { "print", KEY_SYSRQ, XK_Print },   /* Print Screen: GNOME's instant screenshot */
};

gboolean sv_uinput_open(void) {
    if (ui_fd >= 0) return TRUE;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        struct uinput_user_dev dev = { 0 };
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_EVBIT, EV_SYN);
        for (int c = 1; c < 249; c++) ioctl(fd, UI_SET_KEYBIT, c);   /* looks like a real keyboard */
        snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "SuperV virtual keyboard");
        dev.id.bustype = BUS_VIRTUAL; dev.id.vendor = 0x1209; dev.id.product = 0xC11B; dev.id.version = 1;
        if (write(fd, &dev, sizeof dev) == sizeof dev && ioctl(fd, UI_DEV_CREATE) == 0) {
            ui_fd = fd;
            logmsg("paste backend: uinput");
            return TRUE;
        }
        close(fd);
    }
    g_free(paste_err);
    paste_err = g_strdup_printf("/dev/uinput: %s", g_strerror(errno));
    return FALSE;
}

/* Can we paste into every app? (uinput everywhere, or XTest on a pure X11 session) */
gboolean sv_paste_everywhere(void) { return ui_fd >= 0 || (xtest_fake && !sv_wayland); }
#define paste_everywhere sv_paste_everywhere

static void paste_init(void) {
    void *h = dlopen("libXtst.so.6", RTLD_LAZY);   /* X11 apps, even on Wayland (XWayland) */
    if (h) xtest_fake = dlsym(h, "XTestFakeKeyEvent");
    if (!sv_uinput_open())
        logmsg("paste backend: %s (%s)", xtest_fake ? (sv_wayland ? "XTest for X11 apps only" : "XTest") : "none",
               paste_err);
}

static void ui_emit(int type, int code, int value) {
    struct input_event ev = { 0 };
    gettimeofday(&ev.time, NULL);
    ev.type = type; ev.code = code; ev.value = value;
    if (write(ui_fd, &ev, sizeof ev) < 0) logmsg("uinput write failed: %s", g_strerror(errno));
}

/* to_x11: the app we type into is an X11 one (XTest can reach it even on Wayland) */
gboolean sv_send_keys(const char *combo, gboolean to_x11) {
    int idx[8], n = 0;
    char **parts = g_strsplit(combo, "+", 8);
    for (int i = 0; parts[i] && n < 8; i++) {
        char *p = g_ascii_strdown(g_strstrip(parts[i]), -1);
        int found = -1;
        for (guint j = 0; j < G_N_ELEMENTS(KEYS); j++) if (!strcmp(KEYS[j].name, p)) found = j;
        g_free(p);
        if (found < 0) { g_strfreev(parts); logmsg("unknown key in paste_keys: %s", combo); return FALSE; }
        idx[n++] = found;
    }
    g_strfreev(parts);
    if (ui_fd >= 0) {
        for (int i = 0; i < n; i++) { ui_emit(EV_KEY, KEYS[idx[i]].code, 1); ui_emit(EV_SYN, SYN_REPORT, 0); g_usleep(10000); }
        for (int i = n - 1; i >= 0; i--) { ui_emit(EV_KEY, KEYS[idx[i]].code, 0); ui_emit(EV_SYN, SYN_REPORT, 0); g_usleep(10000); }
        return TRUE;
    }
    if (xtest_fake && (!sv_wayland || to_x11)) {
        Display *d = gdk_x11_display_get_xdisplay(gdk_display_get_default());
        for (int i = 0; i < n; i++) xtest_fake(d, XKeysymToKeycode(d, KEYS[idx[i]].sym), True, 0);
        for (int i = n - 1; i >= 0; i--) xtest_fake(d, XKeysymToKeycode(d, KEYS[idx[i]].sym), False, 0);
        XFlush(d);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ clipboard */

static void history_changed(void);

static void set_clipboard(Item *it, gboolean primary) {
    ignore_until = g_get_monotonic_time() + 600000;
    if (!it->image) {
        gtk_clipboard_set_text(clip, it->text, -1);
        if (primary) gtk_clipboard_set_text(prim, it->text, -1);
    } else {
        char *p = img_path(it);
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(p, NULL);
        g_free(p);
        if (pb) {
            gtk_clipboard_set_image(clip, pb);
            if (primary) gtk_clipboard_set_image(prim, pb);
            g_object_unref(pb);
        }
    }
    g_free(current_id);
    current_id = g_strdup(it->id);
}

static void got_item(Item *it) {
    if (!it) return;
    g_free(current_id);
    current_id = g_strdup(it->id);
    history_changed();
}

static void on_image(GtkClipboard *c, GdkPixbuf *pb, gpointer d);

/* Browsers put the image's address next to a copied image; that copy is an image, not a link. */
static gboolean is_bare_link(const char *text) {
    char *t = g_strstrip(g_strdup(text));
    gboolean link = !*t || (!strchr(t, '\n') &&
        (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://") ||
         g_str_has_prefix(t, "file://") || g_str_has_prefix(t, "data:image/")));
    g_free(t);
    return link;
}

static void on_text(GtkClipboard *c, const char *text, gpointer has_image) {
    if (has_image && (!text || is_bare_link(text))) {
        if (sv_debug) logmsg("image + link offered: saving the image");
        g_free(image_link);
        image_link = g_strdup(text ? text : "");
        gtk_clipboard_request_image(c, on_image, NULL);
        return;
    }
    reading = FALSE;
    /* When the app that copied an image quits, the desktop may keep only its link: not a new copy. */
    if (link_candidate && text && !strcmp(text, link_candidate)) {
        if (sv_debug) logmsg("link of the last image re-offered: ignored");
        g_clear_pointer(&link_candidate, g_free);
        return;
    }
    g_clear_pointer(&link_candidate, g_free);
    if (!text || !*text || g_str_has_prefix(text, "x-special/nautilus-clipboard")) return;
    if (strlen(text) > (size_t)cfg.max_text_bytes) { logmsg("skipped text over max_text_bytes"); return; }
    got_item(add_text(text));
}

static void on_image(GtkClipboard *c, GdkPixbuf *pb, gpointer d) {
    if (pb) { add_image(pb, TRUE); return; }   /* saved in the background; the next clipboard change waits for it */
    reading = FALSE;
    if (sv_debug) logmsg("image could not be read");
}

static gboolean plain_text_only(GdkAtom *atoms, gint n) {
    static const char *plain[] = { "TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS", "UTF8_STRING", "STRING",
                                   "TEXT", "COMPOUND_TEXT", "text/plain", "text/plain;charset=utf-8" };
    for (int i = 0; i < n; i++) {
        char *name = gdk_atom_name(atoms[i]);
        gboolean ok = FALSE;
        for (guint j = 0; j < G_N_ELEMENTS(plain) && !ok; j++) ok = !strcmp(name, plain[j]);
        g_free(name);
        if (!ok) return FALSE;
    }
    return TRUE;
}

static void on_targets(GtkClipboard *c, GdkAtom *atoms, gint n, gpointer d) {
    /* The last image's link is only a re-offer if it arrives right after, as bare text
     * (a deliberate "Copy Image Link" comes with richer formats and is kept). */
    g_free(link_candidate);
    link_candidate = (atoms && n > 0 && plain_text_only(atoms, n)) ? image_link : NULL;
    if (!link_candidate) g_free(image_link);
    image_link = NULL;
    if (!atoms || n <= 0) {
        /* The app that owned the clipboard quit. Windows keeps the content; so do we. */
        reading = FALSE;
        Item *it = current_id && cfg.restore_on_exit ? find_item(current_id, NULL) : NULL;
        if (it) set_clipboard(it, FALSE);
        return;
    }
    gboolean skip = !cfg.history_enabled, rich = FALSE;
    GString *names = sv_debug ? g_string_new(NULL) : NULL;   /* format names only, never content */
    for (int i = 0; i < n; i++) {
        char *name = gdk_atom_name(atoms[i]);
        skip = skip || !g_strcmp0(name, "x-kde-passwordManagerHint")  /* password managers */
            || !g_strcmp0(name, "x-special/gnome-copied-files")       /* copied files, like Windows */
            || !g_strcmp0(name, "application/x-kde-cutselection");
        /* office apps offer a picture of the copied cells or text next to the text itself: that copy is text */
        rich = rich || g_str_has_prefix(name, "application/x-openoffice") || strstr(name, "rtf") || strstr(name, "richtext");
        if (names) g_string_append_printf(names, "%s%s", i ? " " : "", name);
        g_free(name);
    }
    gboolean has_text = gtk_targets_include_text(atoms, n);
    gboolean has_image = gtk_targets_include_image(atoms, n, FALSE);   /* any format we can read */
    if (has_text && rich) has_image = FALSE;
    if (names) {
        logmsg("clipboard offers: %s -> %s", names->str,
               skip ? "skipped" : has_text ? (has_image ? "text or image" : "text") : has_image ? "image" : "ignored");
        g_string_free(names, TRUE);
    }
    if (!skip && has_text) { gtk_clipboard_request_text(c, on_text, GINT_TO_POINTER(has_image)); return; }
    if (!skip && has_image) { gtk_clipboard_request_image(c, on_image, NULL); return; }
    /* not recorded (a password, copied files, history off): what's on the clipboard is no item of ours any more,
     * so nothing is "put back" from history when that app quits or clears it */
    reading = FALSE;
    g_clear_pointer(&current_id, g_free);
}

static gboolean read_clipboard(gpointer d);

static void schedule_read(guint ms) {
    if (pending_src) g_source_remove(pending_src);
    pending_src = g_timeout_add(ms, read_clipboard, NULL);
}

static gint64 guard_until;   /* the screenshot tool borrowed the clipboard: don't record, put yours back after */
static char *guard_id;
static gboolean guard_changed;

static gboolean read_clipboard(gpointer d) {
    pending_src = 0;
    if (reading) { schedule_read(60); return G_SOURCE_REMOVE; }
    if (g_get_monotonic_time() < ignore_until || g_get_monotonic_time() < guard_until) return G_SOURCE_REMOVE;
    reading = TRUE;
    gtk_clipboard_request_targets(clip, on_targets, NULL);
    return G_SOURCE_REMOVE;
}

static void on_owner_change(GtkClipboard *c, GdkEvent *e, gpointer d) {
    if (g_get_monotonic_time() < guard_until) guard_changed = TRUE;
    schedule_read(60);
}

/* Our own picture (a screenshot): on the clipboard and into history, without reading it back from ourselves
 * (that would encode and decode the whole image on the spot). */
void sv_copy_image(GdkPixbuf *pb) {
    ignore_until = g_get_monotonic_time() + 1500000;
    gtk_clipboard_set_image(clip, pb);
    g_clear_pointer(&current_id, g_free);   /* until it is saved */
    if (cfg.history_enabled) add_image(pb, FALSE);
}

/* Is that selection held by SuperV with a history item on it? Then the emoji panel needn't copy it aside
 * to put it back afterwards: SuperV can simply offer the item again. */
char *sv_selection_item(gboolean primary) {   /* its id (free it), or NULL */
    if (!current_id || !find_item(current_id, NULL) || gtk_clipboard_get_owner(primary ? prim : clip)) return NULL;
    return gdk_selection_owner_get_for_display(gdk_display_get_default(),
               primary ? GDK_SELECTION_PRIMARY : GDK_SELECTION_CLIPBOARD) ? g_strdup(current_id) : NULL;
}

void sv_reoffer_item(const char *id, gboolean primary) {
    Item *it = find_item(id, NULL);
    if (!it) return;
    if (!primary) { set_clipboard(it, FALSE); return; }
    if (it->text) gtk_clipboard_set_text(prim, it->text, -1); else gtk_clipboard_clear(prim);
}

/* ------------------------------------------------------------------ UI */

static const char *CSS =
    ".cbh-window { background-color: transparent; }"
    ".cbh-panel { border-radius: 12px; padding: 12px 10px 10px 10px; margin: 10px; }"
    ".cbh-nocomp .cbh-panel { margin: 0; border-radius: 0; box-shadow: none; }"
    ".cbh-dark .cbh-panel { background-color: mix(#141418, @sv_d, 0.05); border: 1px solid mix(#2a2a30, @sv_d, 0.14); color: #ebe8f5;"
    "  box-shadow: 0 6px 18px rgba(0,0,0,0.55); }"
    ".cbh-light .cbh-panel { background-color: mix(#f6f6f8, @sv_l, 0.03); border: 1px solid mix(#dcdce4, @sv_l, 0.10); color: #1d1a26;"
    "  box-shadow: 0 6px 18px rgba(40,20,80,0.18); }"
    ".cbh-title { font-weight: bold; font-size: 1.08em; margin-left: 4px; }"
    ".cbh-sub { font-size: 0.92em; }"
    ".cbh-dark .cbh-sub { color: #a29db4; } .cbh-light .cbh-sub { color: #5f5a70; }"
    ".cbh-list, .cbh-list row, .cbh-scroll, .cbh-scroll viewport { background-color: transparent; background-image: none; }"
    "list.cbh-list row.cbh-row { padding: 0; margin: 0 2px 6px 2px; outline-style: none; border: none; }"
    "list.cbh-list row.cbh-row:selected { background-color: transparent; }"
    ".cbh-card { border-radius: 8px; padding: 10px 4px 10px 12px; border: 1px solid transparent; }"
    ".cbh-dark .cbh-card { background-color: mix(#1e1e24, @sv_d, 0.08); border-color: mix(#2a2a30, @sv_d, 0.14); }"
    ".cbh-light .cbh-card { background-color: #ffffff; border-color: mix(#e4e4ec, @sv_l, 0.08); }"
    ".cbh-dark list.cbh-list row.cbh-row:hover .cbh-card { background-color: mix(#24242b, @sv_d, 0.15); }"
    ".cbh-light list.cbh-list row.cbh-row:hover .cbh-card { background-color: mix(#ffffff, @sv_l, 0.07); }"
    ".cbh-dark list.cbh-list row.cbh-row:focus .cbh-card, .cbh-dark list.cbh-list row.cbh-row:selected .cbh-card { border-color: @sv_d; }"
    ".cbh-light list.cbh-list row.cbh-row:focus .cbh-card, .cbh-light list.cbh-list row.cbh-row:selected .cbh-card { border-color: @sv_l; }"
    ".cbh-dark .cbh-text { color: #ebe8f5; } .cbh-light .cbh-text { color: #1d1a26; }"
    "button.cbh-flat, button.cbh-icon, button.cbh-menuitem { background-color: transparent; background-image: none;"
    "  border: none; box-shadow: none; text-shadow: none; border-radius: 6px; }"
    "button.cbh-flat { padding: 3px 10px; font-weight: bold; }"
    "button.cbh-icon { padding: 2px; min-width: 26px; min-height: 26px; }"
    "button.cbh-menuitem { padding: 6px 12px; }"
    ".cbh-dark button.cbh-flat { color: @sv_d; } .cbh-light button.cbh-flat { color: @sv_l; }"
    ".cbh-dark button.cbh-icon { color: #c9c3dc; } .cbh-light button.cbh-icon { color: #4a4560; }"
    ".cbh-dark button.cbh-flat:hover, .cbh-dark button.cbh-icon:hover { background-color: mix(#26262d, @sv_d, 0.18); }"
    ".cbh-light button.cbh-flat:hover, .cbh-light button.cbh-icon:hover { background-color: mix(#ffffff, @sv_l, 0.10); }"
    ".cbh-dark .cbh-pin { color: @sv_d; } .cbh-light .cbh-pin { color: @sv_l; }"
    "button.cbh-primary { background-image: none; border: none; box-shadow: none; text-shadow: none;"
    "  border-radius: 6px; padding: 6px 18px; font-weight: bold; }"
    ".cbh-dark popover.background { background-color: mix(#1e1e24, @sv_d, 0.08); border-color: mix(#33333b, @sv_d, 0.16); color: #ebe8f5; }"
    ".cbh-light popover.background { background-color: #ffffff; border-color: mix(#dcdce4, @sv_l, 0.10); color: #1d1a26; }"
    ".cbh-dark button.cbh-menuitem:hover { background-color: mix(#26262d, @sv_d, 0.18); }"
    ".cbh-light button.cbh-menuitem:hover { background-color: mix(#ffffff, @sv_l, 0.10); }"
    ".cbh-seg button { padding: 3px 10px; min-height: 0; }"
    ".cbh-size { padding: 0 6px; }"
    ".cbh-dark .cbh-seg button:checked { background-image: none; background-color: mix(@sv_d, @sv_l, 0.5); color: #ffffff; border-color: @sv_l; }"
    ".cbh-light .cbh-seg button:checked { background-image: none; background-color: @sv_l; color: #ffffff; border-color: shade(@sv_l, 0.85); }"
    ".cbh-settings label, .cbh-settings button { font-size: 13px; }"
    ".cbh-settings button { padding: 2px 9px; min-height: 24px; }"
    ".cbh-settings switch { min-width: 34px; min-height: 18px; padding: 0; margin: 0; border-radius: 9px; }"
    ".cbh-settings switch slider { min-width: 14px; min-height: 14px; margin: 2px; padding: 0;"
    "  border-radius: 7px; border-width: 0; box-shadow: none; }"
    ".cbh-settings .cbh-seg button { padding: 2px 8px; }"
    ".cbh-settings .cbh-status { font-size: 11.5px; }"
    ".cbh-settings .cbh-section { font-size: 11px; font-weight: bold; letter-spacing: 1px; margin-top: 4px; }"
    ".cbh-dark .cbh-section { color: @sv_d; } .cbh-light .cbh-section { color: @sv_l; }"
    ".cbh-dark .cbh-status { color: #a29db4; } .cbh-light .cbh-status { color: #5f5a70; }"
    ".cbh-dark .cbh-status.cbh-ok { color: #4ade80; } .cbh-light .cbh-status.cbh-ok { color: #15803d; }"
    ".cbh-dark .cbh-status.cbh-err { color: #f87171; } .cbh-light .cbh-status.cbh-err { color: #b91c1c; }"
    ".cbh-keys .cbh-keydesc { font-size: 11px; }"
    ".cbh-dark .cbh-keydesc { color: #a29db4; } .cbh-light .cbh-keydesc { color: #5f5a70; }"
    ".cbh-settings .cbh-kbd { font-family: monospace; font-size: 10px; padding: 0 5px; border-radius: 4px; }"
    ".cbh-dark .cbh-kbd { background-color: mix(#24242a, @sv_d, 0.08); border: 1px solid mix(#3a3a42, @sv_d, 0.14); color: #ebe8f5; }"
    ".cbh-light .cbh-kbd { background-color: mix(#f3f3f7, @sv_l, 0.05); border: 1px solid mix(#d8d8e2, @sv_l, 0.10); color: #1d1a26; }"
    ".cbh-settings .cbh-footbtn, .cbh-settings .cbh-footbtn label, .cbh-settings .cbh-ver { font-size: 11px; }"
    ".cbh-settings button.cbh-footbtn { padding: 1px 4px; min-height: 0; background-color: transparent;"
    "  background-image: none; border: none; box-shadow: none; text-shadow: none; border-radius: 5px; }"
    ".cbh-dark .cbh-footbtn, .cbh-dark .cbh-ver { color: #8a84a0; }"
    ".cbh-light .cbh-footbtn, .cbh-light .cbh-ver { color: #6f6a82; }"
    ".cbh-dark button.cbh-footbtn:hover, .cbh-dark button.cbh-footbtn:checked { color: @sv_d; background-color: mix(#26262d, @sv_d, 0.18); }"
    ".cbh-light button.cbh-footbtn:hover, .cbh-light button.cbh-footbtn:checked { color: @sv_l; background-color: mix(#ffffff, @sv_l, 0.10); }"
    ".cbh-banner { border-radius: 8px; padding: 8px 8px 8px 12px; }"
    ".cbh-dark .cbh-banner { background-color: mix(#1e1e24, @sv_d, 0.16); border: 1px solid mix(#2a2a30, @sv_d, 0.40); }"
    ".cbh-light .cbh-banner { background-color: mix(#ffffff, @sv_l, 0.08); border: 1px solid mix(#ffffff, @sv_l, 0.25); }"
    ".cbh-dark switch:checked { background-color: mix(@sv_d, @sv_l, 0.5); border-color: @sv_l; }"
    ".cbh-light switch:checked { background-color: @sv_l; border-color: shade(@sv_l, 0.85); }"
    ".cbh-dark button.cbh-primary { background-color: @sv_d; color: mix(#141418, @sv_d, 0.05); }"
    ".cbh-light button.cbh-primary { background-color: @sv_l; color: #ffffff; }"
    /* colour swatches in the settings */
    ".cbh-settings button.cbh-sw { min-width: 18px; min-height: 18px; padding: 0; margin: 0 2px; border-radius: 11px;"
    "  border: 2px solid transparent; background-image: none; box-shadow: none; }"
    ".cbh-dark .cbh-settings button.cbh-sw:checked { border-color: #ebe8f5; }"
    ".cbh-light .cbh-settings button.cbh-sw:checked { border-color: #1d1a26; }"
    /* pasted items: faded, a tick; the last one pasted: an accent bar and a label */
    "row.cbh-pasted .cbh-text { opacity: 0.5; }"
    ".cbh-dark .cbh-tick, .cbh-dark .cbh-lastlbl { color: @sv_d; } .cbh-light .cbh-tick, .cbh-light .cbh-lastlbl { color: @sv_l; }"
    ".cbh-tick { font-weight: bold; } .cbh-lastlbl { font-size: 0.82em; font-weight: bold; }"
    ".cbh-dark list.cbh-list row.cbh-last .cbh-card { border-left: 3px solid @sv_d; }"
    ".cbh-light list.cbh-list row.cbh-last .cbh-card { border-left: 3px solid @sv_l; }";

gboolean sv_theme_is_dark(void) {   /* SuperV's theme setting, or the desktop's when it is "auto" */
    if (!g_strcmp0(cfg.theme, "dark")) return TRUE;
    if (!g_strcmp0(cfg.theme, "light")) return FALSE;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    gboolean dark = FALSE;
    GSettingsSchema *s = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE) : NULL;
    if (s) {
        if (g_settings_schema_has_key(s, "color-scheme")) {
            GSettings *g = g_settings_new("org.gnome.desktop.interface");
            char *v = g_settings_get_string(g, "color-scheme");
            dark = !g_strcmp0(v, "prefer-dark");
            g_free(v); g_object_unref(g);
        }
        g_settings_schema_unref(s);
    }
    gboolean prefer_dark = FALSE;   /* set by KDE Plasma and others in GTK's settings.ini */
    g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
    dark = dark || prefer_dark;
    char *name = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-theme-name", &name, NULL);
    if (name) { char *l = g_ascii_strdown(name, -1); dark = dark || strstr(l, "dark"); g_free(l); g_free(name); }
    const char *schemas[] = { "org.cinnamon.desktop.interface", "org.mate.interface" };
    for (guint i = 0; i < 2 && !dark && src; i++) {
        s = g_settings_schema_source_lookup(src, schemas[i], TRUE);
        if (!s) continue;
        if (g_settings_schema_has_key(s, "gtk-theme")) {
            GSettings *g = g_settings_new(schemas[i]);
            char *v = g_settings_get_string(g, "gtk-theme"), *l = g_ascii_strdown(v, -1);
            dark = strstr(l, "dark") != NULL;
            g_free(v); g_free(l); g_object_unref(g);
        }
        g_settings_schema_unref(s);
    }
    return dark;
}

GtkWidget *sv_icon(const char *a, const char *b, const char *fallback) {
    GtkIconTheme *t = gtk_icon_theme_get_default();
    if (gtk_icon_theme_has_icon(t, a)) return gtk_image_new_from_icon_name(a, GTK_ICON_SIZE_MENU);
    if (b && gtk_icon_theme_has_icon(t, b)) return gtk_image_new_from_icon_name(b, GTK_ICON_SIZE_MENU);
    return gtk_label_new(fallback);
}
#define icon sv_icon

static void add_class(GtkWidget *w, const char *c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }

static int focused_index(void) {
    GtkWidget *w = gtk_window_get_focus(GTK_WINDOW(win));
    while (w && !GTK_IS_LIST_BOX_ROW(w)) w = gtk_widget_get_parent(w);
    if (!w) w = GTK_WIDGET(gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox)));
    return w ? gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(w)) : -1;
}

static GtkWidget *row_pop;   /* the open ⋯ menu of a row, made when you click ⋯ */
static guint row_pop_gen;

static gboolean any_menu_open(void) {
    return gtk_widget_get_visible(settings_pop) || (row_pop && gtk_widget_get_visible(row_pop));
}

/* actions */
static gboolean do_paste(gpointer d) {
    if (!sv_send_keys(cfg.paste_keys, prev_is_x11))
        extras_toast("Copied — press Ctrl+V to paste",
                     "Click-to-paste needs a one-time permission: open SuperV and click Enable.");
    return G_SOURCE_REMOVE;
}

static int index_of(const char *id) {
    for (guint i = 0; id && i < items->len; i++)
        if (!strcmp(((Item *)items->pdata[i])->id, id)) return (int)i;
    return 0;
}

static void paste_item(Item *it) {
    gtk_widget_hide(win);   /* first: nothing below may keep the panel on screen */
    int idx = index_of(it->id);
    if (run_active && run_idx >= 0 && idx != run_idx) run_dir = idx < run_idx ? -1 : 1;
    run_active = TRUE; run_idx = idx;
    it->pasted = g_get_real_time();   /* the list keeps its order: the item is ticked, not moved to the top */
    last_paste_at = g_get_monotonic_time();
    set_clipboard(it, TRUE);   /* PRIMARY too, so Shift+Insert also works in terminals */
    history_save();
    if (g_strcmp0(cfg.paste_keys, "none")) g_timeout_add(cfg.paste_delay_ms, do_paste, NULL);
}

/* Where the panel opens: during a paste run, the next unpasted item the way you are going (after the first
 * paste: towards newer copies, the order you copied them in); otherwise the newest item. */
static int next_index(void) {
    Item *last = run_active ? last_pasted() : NULL;
    if (!last) return 0;
    int n = items->len, i = index_of(last->id), dir = run_dir ? run_dir : (i > 0 ? -1 : 1);
    for (int pass = 0; pass < 2; pass++, dir = -dir)
        for (int j = i + dir; j >= 0 && j < n; j += dir)
            if (!((Item *)items->pdata[j])->pasted) return j;
    return i;
}

static void clear_all(void) { clear_unpinned(); if (win) refresh(0); }

static void reload_hotkey(void);
static void sync_settings(void);

static void set_history_enabled(gboolean on) {
    if (cfg.history_enabled != on) {
        reload_hotkey();
        cfg.history_enabled = on;
        config_save();
        if (!on) clear_unpinned();   /* Windows also wipes history when it is turned off */
    }
    sync_settings();
    refresh(0);
}

static char *selected_id(void) {   /* the item the keyboard is on, read from the row itself */
    GtkWidget *w = gtk_window_get_focus(GTK_WINDOW(win));
    while (w && !GTK_IS_LIST_BOX_ROW(w)) w = gtk_widget_get_parent(w);
    if (!w) w = GTK_WIDGET(gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox)));
    return w ? g_strdup(g_object_get_data(G_OBJECT(w), "id")) : NULL;
}

static void history_changed(void) {
    if (!win || !gtk_widget_get_visible(win)) return;
    char *id = selected_id();
    refresh(index_of(id));
    g_free(id);
}

enum { ACT_DELETE, ACT_PIN, ACT_CLEAR };
typedef struct { int act; char *id; } Pending;

static gboolean run_action(gpointer data) {
    Pending *p = data;
    Item *it = find_item(p->id, NULL);
    if (p->act == ACT_CLEAR) clear_all();
    else if (it) {
        int idx = index_of(p->id);   /* the row whose ⋯ was clicked */
        if (p->act == ACT_DELETE) drop_item(it); else it->pinned = !it->pinned;
        history_save();
        refresh(idx);
    }
    g_free(p->id); g_free(p);
    return G_SOURCE_REMOVE;
}

static void on_menu_item(GtkButton *b, gpointer act) {
    if (row_pop) gtk_popover_popdown(GTK_POPOVER(row_pop));
    Pending *p = g_new0(Pending, 1);
    p->act = GPOINTER_TO_INT(act);
    p->id = g_strdup(g_object_get_data(G_OBJECT(b), "id"));
    g_idle_add(run_action, p);   /* the rows get rebuilt */
}

static void menu_item(GtkWidget *box, const char *ic1, const char *ic2, const char *fb,
                      const char *label, int act, const char *id) {
    GtkWidget *b = gtk_button_new(), *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *l = gtk_label_new(label);
    add_class(b, "cbh-menuitem");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(inner), icon(ic1, ic2, fb), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), l, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(b), inner);
    g_object_set_data_full(G_OBJECT(b), "id", g_strdup(id), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_menu_item), GINT_TO_POINTER(act));
    gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
}

static void drop_row_pop(void) {
    if (!row_pop) return;
    gtk_widget_destroy(row_pop);
    row_pop = NULL;
    row_pop_gen++;
}

static gboolean drop_row_pop_later(gpointer gen) {
    if (GPOINTER_TO_UINT(gen) == row_pop_gen) drop_row_pop();
    return G_SOURCE_REMOVE;
}

static void on_row_pop_closed(GtkPopover *p, gpointer d) {
    on_menu_closed(p, d);
    g_idle_add(drop_row_pop_later, GUINT_TO_POINTER(row_pop_gen));   /* after its closing animation started */
}

/* The ⋯ menu of a row is built when you click ⋯ (most rows never need one). */
static void on_more(GtkButton *b, gpointer d) {
    GtkWidget *row = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
    Item *it = row ? find_item(g_object_get_data(G_OBJECT(row), "id"), NULL) : NULL;
    if (!it) return;
    drop_row_pop();
    row_pop = gtk_popover_new(GTK_WIDGET(b));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    menu_item(box, "user-trash-symbolic", "edit-delete-symbolic", "🗑", "Delete", ACT_DELETE, it->id);
    menu_item(box, "view-pin-symbolic", NULL, "📌", it->pinned ? "Unpin" : "Pin", ACT_PIN, it->id);
    menu_item(box, "edit-clear-all-symbolic", "edit-clear-symbolic", "✕", "Clear all", ACT_CLEAR, it->id);
    gtk_widget_show_all(box);
    gtk_container_add(GTK_CONTAINER(row_pop), box);
    g_signal_connect(row_pop, "closed", G_CALLBACK(on_row_pop_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(row_pop));
}

/* The picture for the list comes from the item's small copy on disk (a few ms), never from the full image. */
static GdkPixbuf *thumbnail(Item *it) {
    GdkPixbuf *pb = g_hash_table_lookup(thumbs, it->id);
    if (pb) return pb;
    int max_h = MAX(150, thumb_w / 2);
    char *t = thumb_path(it->hash);
    pb = gdk_pixbuf_new_from_file_at_scale(t, thumb_w, max_h, TRUE, NULL);
    if (!pb) {   /* an item saved by an older version has no small copy yet: make it, once */
        char *p = img_path(it);
        GdkPixbuf *full = gdk_pixbuf_new_from_file(p, NULL);
        g_free(p);
        if (full) {
            save_thumb(full, it->hash);
            g_object_unref(full);
            pb = gdk_pixbuf_new_from_file_at_scale(t, thumb_w, max_h, TRUE, NULL);
        }
    }
    g_free(t);
    if (pb) g_hash_table_insert(thumbs, g_strdup(it->id), pb);
    return pb;
}

static GtkWidget *make_row(Item *it, gboolean last) {
    GtkWidget *row = gtk_list_box_row_new(), *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *content;
    add_class(row, "cbh-row");
    add_class(card, "cbh-card");
    if (it->pasted && !it->pinned) add_class(row, "cbh-pasted");   /* faded: done with (pins are yours to keep) */
    if (last) add_class(row, "cbh-last");
    g_object_set_data_full(G_OBJECT(row), "id", g_strdup(it->id), g_free);
    if (!it->image) {
        const char *end = it->text;   /* first 800 characters (not bytes) */
        for (int n = 0; *end && n < 800; n++) end = g_utf8_next_char(end);
        char *preview = g_strndup(it->text, end - it->text);
        g_strdelimit(preview, "\t", ' ');
        content = gtk_label_new(preview);
        g_free(preview);
        GtkLabel *l = GTK_LABEL(content);
        gtk_label_set_xalign(l, 0); gtk_label_set_yalign(l, 0);
        gtk_label_set_line_wrap(l, TRUE);
        gtk_label_set_line_wrap_mode(l, PANGO_WRAP_WORD_CHAR);
        gtk_label_set_ellipsize(l, PANGO_ELLIPSIZE_END);
        gtk_label_set_lines(l, 4);
        gtk_label_set_max_width_chars(l, 32);
        add_class(content, "cbh-text");
    } else {
        GdkPixbuf *pb = thumbnail(it);
        content = pb ? gtk_image_new_from_pixbuf(pb) : gtk_label_new("[image]");
        gtk_widget_set_halign(content, GTK_ALIGN_START);
        add_class(content, "cbh-text");
    }
    if (last) {   /* "Last pasted" under the item */
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *l = gtk_label_new("✓ Last pasted");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        add_class(l, "cbh-lastlbl");
        gtk_box_pack_start(GTK_BOX(col), content, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
        content = col;
    }
    gtk_box_pack_start(GTK_BOX(card), content, TRUE, TRUE, 0);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *more = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(more), icon("view-more-horizontal-symbolic", "view-more-symbolic", "⋯"));
    add_class(more, "cbh-icon");
    gtk_widget_set_tooltip_text(more, "See more");
    gtk_widget_set_valign(more, GTK_ALIGN_START);
    g_signal_connect(more, "clicked", G_CALLBACK(on_more), NULL);
    gtk_box_pack_start(GTK_BOX(side), more, FALSE, FALSE, 0);
    if (it->pinned) {
        GtkWidget *pin = icon("view-pin-symbolic", NULL, "📌");
        add_class(pin, "cbh-pin");
        gtk_widget_set_tooltip_text(pin, "Pinned");
        gtk_box_pack_start(GTK_BOX(side), pin, FALSE, FALSE, 2);
    }
    if (it->pasted && !last) {
        GtkWidget *tick = gtk_label_new("✓");
        add_class(tick, "cbh-tick");
        gtk_widget_set_tooltip_text(tick, "Pasted");
        gtk_box_pack_start(GTK_BOX(side), tick, FALSE, FALSE, 2);
    }
    gtk_box_pack_start(GTK_BOX(card), side, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), card);
    gtk_widget_show_all(row);
    return row;
}

static void update_hotkey_labels(void) {
    char *label = sv_accel_label(cfg.hotkey);
    if (!capturing) gtk_button_set_label(GTK_BUTTON(hotkey_btn), label);
    gtk_label_set_text(GTK_LABEL(hint_lbl), label);
    g_free(label);
}

/* ---- click-to-paste permission (shared with the emoji panel, which shows the same state in its own words) */
static int paste_state = SV_PASTE_ASK;
static gboolean paste_rule_installed(void);

int sv_paste_state(void) {
    if (sv_paste_everywhere()) return SV_PASTE_OK;
    if (paste_state == SV_PASTE_ASK && paste_rule_installed()) return SV_PASTE_RELOGIN;   /* a package added it */
    return paste_state;
}

static void update_banner(void) {
    int st = g_strcmp0(cfg.paste_keys, "none") ? sv_paste_state() : SV_PASTE_OK;   /* "none": you paste yourself */
    gtk_widget_set_visible(banner, st != SV_PASTE_OK);
    gtk_widget_set_visible(banner_btn, st == SV_PASTE_ASK || st == SV_PASTE_FAILED || st == SV_PASTE_ENABLING);
    gtk_widget_set_sensitive(banner_btn, st != SV_PASTE_ENABLING);
    switch (st) {
    case SV_PASTE_RELOGIN:
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Permission added. Log out and back in once to finish.");
        break;
    case SV_PASTE_FAILED:
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Not enabled: the password prompt was cancelled.");
        gtk_button_set_label(GTK_BUTTON(banner_btn), "Try again");
        break;
    case SV_PASTE_ENABLING:
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Enter your password in the dialog to turn on click-to-paste…");
        break;
    case SV_PASTE_ASK:
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Click-to-paste needs a one-time permission. "
                           "Until then, picking an item copies it — press Ctrl+V.");
        gtk_button_set_label(GTK_BUTTON(banner_btn), "Enable");
    }
}

/* ---- the tip under the header: "more features" once, or "turn on the emoji panel?" after clicking ☺ */
enum { TIP_NONE, TIP_FEATURES, TIP_EMOJI };
static int tip_mode;
static GtkWidget *tip, *tip_lbl, *tip_yes, *tip_no;

static void update_tip(void) {
    if (tip_mode != TIP_EMOJI) {
        gboolean all = cfg.emoji_panel;
        for (int f = 0; f < N_FEAT; f++) all = all && cfg.feat[f];
        tip_mode = cfg.tip_seen || all ? TIP_NONE : TIP_FEATURES;
    }
    gtk_widget_set_visible(tip, tip_mode != TIP_NONE);
    if (tip_mode == TIP_FEATURES) {
        gtk_label_set_text(GTK_LABEL(tip_lbl), "SuperV can do more: emoji panel, screenshots, colour picker, "
                           "plain-text paste, copy text from the screen. Each is off until you turn it on.");
        gtk_button_set_label(GTK_BUTTON(tip_yes), "Choose");
        gtk_button_set_label(GTK_BUTTON(tip_no), "Not now");
    } else if (tip_mode == TIP_EMOJI) {
        gtk_label_set_text(GTK_LABEL(tip_lbl), "Turn on the emoji panel? Super+. then opens emoji, kaomoji and "
                           "symbols, typed where you are writing.");
        gtk_button_set_label(GTK_BUTTON(tip_yes), "Turn on");
        gtk_button_set_label(GTK_BUTTON(tip_no), "Not now");
    }
    gtk_widget_set_visible(emoji_btn, cfg.emoji_panel || !cfg.emoji_asked);
}

/* ---- the rows are rebuilt only when the history (or their width) changed since they were last built, and
 * they and their pictures are freed a while after the panel closes */
static guint drop_src;
#define ROWS_KEEP_S 300   /* reopening within five minutes is instant; after that the memory is given back */

static void drop_rows(void) {
    drop_row_pop();
    GList *rows = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = rows; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(rows);
    rows_dirty = TRUE;
}

static gboolean drop_rows_idle(gpointer d) {
    drop_src = 0;
    if (gtk_widget_get_visible(win)) return G_SOURCE_REMOVE;
    drop_rows();
    g_hash_table_remove_all(thumbs);
    malloc_trim(0);
    return G_SOURCE_REMOVE;
}

static void refresh(int keep) {
    int tw = CLAMP(cfg.width - 110, 120, 1600);
    if (tw != thumb_w) { thumb_w = tw; g_hash_table_remove_all(thumbs); rows_dirty = TRUE; }
    update_banner();
    update_tip();
    if (rows_dirty) {
        drop_rows();
        GHashTableIter hi; gpointer key;
        g_hash_table_iter_init(&hi, thumbs);
        while (g_hash_table_iter_next(&hi, &key, NULL)) if (!find_item(key, NULL)) g_hash_table_iter_remove(&hi);
        Item *last = last_pasted();
        for (guint i = 0; i < items->len; i++)
            gtk_container_add(GTK_CONTAINER(listbox), make_row(items->pdata[i], items->pdata[i] == (gpointer)last));
        rows_dirty = FALSE;
    }
    gboolean any_unpinned = FALSE;
    for (guint i = 0; i < items->len; i++) any_unpinned |= !((Item *)items->pdata[i])->pinned;
    gtk_widget_set_visible(clear_btn, cfg.history_enabled && any_unpinned);
    if (!cfg.history_enabled) gtk_stack_set_visible_child_name(GTK_STACK(stack), "off");
    else if (!items->len) gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");
    else {
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "list");
        int idx = CLAMP(keep, 0, (int)items->len - 1);
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(listbox), idx);
        gtk_list_box_select_row(GTK_LIST_BOX(listbox), row);
        /* don't pull focus out of an open menu (that would close it) */
        if (gtk_widget_get_visible(win) && !any_menu_open()) gtk_widget_grab_focus(GTK_WIDGET(row));
    }
}

/* The controls in the gear menu show what's saved; brought up to date when the menu opens, not on every
 * opening of the panel. */
static void update_emoji_controls(void);
static void sync_hotkey_from_desktop(void);

static void sync_settings(void) {
    syncing = TRUE;
    gtk_switch_set_active(GTK_SWITCH(switch_w), cfg.history_enabled);
    const char *themes[] = { "auto", "light", "dark" };
    for (int i = 0; i < 3; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(theme_btns[i]), !g_strcmp0(cfg.theme, themes[i]));
    syncing = FALSE;
    update_hotkey_labels();
    update_emoji_controls();
}

static int at_x = -1, at_y = -1;   /* open here once (switching from the emoji panel) */

/* ---- placing, resizing and moving a panel that has no title bar (the clipboard and the emoji panel) */
void sv_place_panel(GtkWidget *w, int W, int H, int wx, int wy, int *ox, int *oy) {
    GdkDisplay *disp = gdk_display_get_default();
    int px = 0, py = 0, x, y;
    GdkRectangle a;
    gtk_window_resize(GTK_WINDOW(w), W, H);
    if (wx >= 0 && wy >= 0) {   /* where you dragged it last time, or where the other panel was */
        gdk_monitor_get_workarea(gdk_display_get_monitor_at_point(disp, wx + W / 2, wy + 20), &a);
        x = CLAMP(wx, a.x, MAX(a.x, a.x + a.width - W));
        y = CLAMP(wy, a.y, MAX(a.y, a.y + a.height - H));
    } else {
        gdk_device_get_position(gdk_seat_get_pointer(gdk_display_get_default_seat(disp)), NULL, &px, &py);
        GdkMonitor *m = gdk_display_get_monitor_at_point(disp, px, py);
        if (!m) m = gdk_display_get_primary_monitor(disp);
        if (!m) m = gdk_display_get_monitor(disp, 0);
        gdk_monitor_get_workarea(m, &a);
        if (sv_wayland) {   /* XWayland can't see the pointer over Wayland windows: centre on screen */
            x = a.x + (a.width - W) / 2;
            y = a.y + (a.height - H) / 2;
        } else {            /* like Windows: open next to where you are working */
            x = CLAMP(px - W / 2, a.x, a.x + a.width - W);
            y = (py + 16 + H <= a.y + a.height) ? py + 16 : py - H - 16;
            y = CLAMP(y, a.y, a.y + a.height - H);
        }
    }
    gtk_window_move(GTK_WINDOW(w), x, y);
    if (ox) { *ox = x; *oy = y; }
}

static void place_window(void) {
    /* switching from the emoji panel: open where it was, like Windows 11's one panel; else where you put it */
    int wx = at_x >= 0 ? at_x : cfg.pos_x, wy = at_x >= 0 ? at_y : cfg.pos_y;
    at_x = at_y = -1;
    sv_place_panel(win, cfg.width, cfg.height, wx, wy, NULL, NULL);
}

static GdkWindowEdge edge_at(GtkWidget *w, double xr, double yr, gboolean *hit) {
    GtkStyleContext *c = gtk_widget_get_style_context(w);
    int ox, oy, z = gtk_style_context_has_class(c, "cbh-nocomp") || gtk_style_context_has_class(c, "sd-nocomp") ? 8 : 16;
    gdk_window_get_origin(gtk_widget_get_window(w), &ox, &oy);
    int x = (int)xr - ox, y = (int)yr - oy;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    gboolean l = x < z, r = x >= W - z, t = y < z, b = y >= H - z;
    *hit = l || r || t || b;
    if (t && l) return GDK_WINDOW_EDGE_NORTH_WEST;
    if (t && r) return GDK_WINDOW_EDGE_NORTH_EAST;
    if (b && l) return GDK_WINDOW_EDGE_SOUTH_WEST;
    if (b && r) return GDK_WINDOW_EDGE_SOUTH_EAST;
    if (t) return GDK_WINDOW_EDGE_NORTH;
    if (b) return GDK_WINDOW_EDGE_SOUTH;
    if (l) return GDK_WINDOW_EDGE_WEST;
    return GDK_WINDOW_EDGE_EAST;
}

gboolean sv_panel_edge(GtkWidget *w, GdkEventButton *e) {   /* is the pointer on the panel's resize border? */
    gboolean hit;
    edge_at(w, e->x_root, e->y_root, &hit);
    return hit;
}

/* The panel window's "button-press-event": drag an edge or a corner to resize. *moved is set when a drag starts. */
gboolean sv_panel_press(GtkWidget *w, GdkEventButton *e, gpointer moved) {
    gboolean hit;
    GdkWindowEdge edge = edge_at(w, e->x_root, e->y_root, &hit);
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS || !hit) return FALSE;
    *(gboolean *)moved = TRUE;
    gtk_window_begin_resize_drag(GTK_WINDOW(w), edge, e->button, (int)e->x_root, (int)e->y_root, e->time);
    return TRUE;
}

/* Its "motion-notify-event": the resize cursors on the border. */
gboolean sv_panel_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    static const char *names[] = { "nw-resize", "n-resize", "ne-resize", "w-resize",
                                   "e-resize", "sw-resize", "s-resize", "se-resize" };
    gboolean hit;
    GdkWindowEdge edge = edge_at(w, e->x_root, e->y_root, &hit);
    int cur = hit ? (int)edge + 1 : 0, last = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "sv-edge"));
    if (cur != last) {
        g_object_set_data(G_OBJECT(w), "sv-edge", GINT_TO_POINTER(cur));
        GdkCursor *c = hit ? gdk_cursor_new_from_name(gtk_widget_get_display(w), names[edge]) : NULL;
        gdk_window_set_cursor(gtk_widget_get_window(w), c);
        if (c) g_object_unref(c);
    }
    return FALSE;
}

static gboolean on_header_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (e->button != 1 || sv_panel_edge(win, e)) return FALSE;
    if (e->type == GDK_2BUTTON_PRESS) {   /* double-click the header: back to default size + place */
        reload_hotkey();
        cfg.pos_x = cfg.pos_y = -1; cfg.width = WIN_W + 20; cfg.height = WIN_H + 20;
        config_save();
        place_window();
        geom_dirty = FALSE;
        return TRUE;
    }
    if (e->type != GDK_BUTTON_PRESS) return FALSE;
    geom_dirty = TRUE;
    gtk_window_begin_move_drag(GTK_WINDOW(win), e->button, (int)e->x_root, (int)e->y_root, e->time);
    return TRUE;
}

static gboolean relayout(gpointer d) { relayout_src = 0; refresh(focused_index()); return G_SOURCE_REMOVE; }

static gboolean on_configure(GtkWidget *w, GdkEventConfigure *e, gpointer d) {
    if (!geom_dirty || !gtk_widget_get_visible(win)) return FALSE;
    int x, y, W, H;
    gtk_window_get_position(GTK_WINDOW(win), &x, &y);
    gtk_window_get_size(GTK_WINDOW(win), &W, &H);
    gboolean resized = W != cfg.width;
    cfg.pos_x = x; cfg.pos_y = y; cfg.width = W; cfg.height = H;
    if (resized) {   /* re-make the pictures for the new width, once the drag settles */
        if (relayout_src) g_source_remove(relayout_src);
        relayout_src = g_timeout_add(250, relayout, NULL);
    }
    return FALSE;
}

static void apply_theme(void) {
    GtkStyleContext *c = gtk_widget_get_style_context(win);
    gboolean dark = sv_theme_is_dark();
    gtk_style_context_remove_class(c, dark ? "cbh-light" : "cbh-dark");
    gtk_style_context_add_class(c, dark ? "cbh-dark" : "cbh-light");
}

static gboolean ensure_focus(gpointer ts) {
    if (gtk_widget_get_visible(win) && !gtk_window_is_active(GTK_WINDOW(win)))
        gdk_window_focus(gtk_widget_get_window(win), GPOINTER_TO_UINT(ts));
    return G_SOURCE_REMOVE;
}

static void reload_hotkey(void) {   /* superv-setup --hotkey may have changed it */
    GKeyFile *k = g_key_file_new();
    char *v = g_key_file_load_from_file(k, cfg_path, G_KEY_FILE_NONE, NULL)
              ? g_key_file_get_string(k, "superv", "hotkey", NULL) : NULL;
    if (v) { g_free(cfg.hotkey); cfg.hotkey = g_strstrip(v); }
    GError *e = NULL;
    gboolean done = g_key_file_get_boolean(k, "superv", "shortcut_configured", &e);
    if (!e) cfg.setup_done = done;
    g_clear_error(&e);
    g_key_file_unref(k);
}

static void hotkey_status(const char *text, gboolean ok);

/* Show the shortcut the desktop really uses: people also change it in their desktop's settings. */
static void sync_hotkey_from_desktop(void) {
    if (g_get_monotonic_time() - hotkey_applied_at < 5 * G_USEC_PER_SEC) return;   /* our own change is still being applied */
    char *d = sv_desktop_hotkey("", "SuperV");
    if (d && g_strcmp0(d, cfg.hotkey)) {
        g_free(cfg.hotkey);
        cfg.hotkey = d;
        config_save();
    } else {
        g_free(d);
    }
}

/* Draw the panel into a picture (not read back from the screen: that trips cairo up), with transparency. */
static gboolean snapshot_window(gpointer path) {
    if (!gtk_widget_get_visible(win)) return G_SOURCE_REMOVE;
    int w = gtk_widget_get_allocated_width(win), h = gtk_widget_get_allocated_height(win);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    gtk_widget_draw(win, cr);
    cairo_destroy(cr);
    if (cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS) logmsg("saved %s", (char *)path);
    cairo_surface_destroy(s);
    return G_SOURCE_REMOVE;
}

/* Is the app you're in an X11 one? (on Wayland, the active window is only known for X11 apps) */
gboolean sv_focused_is_x11(void) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkWindow *aw = gdk_screen_get_active_window(gdk_screen_get_default());
    G_GNUC_END_IGNORE_DEPRECATIONS
    gboolean x11 = aw && (!gtk_widget_get_realized(win) || aw != gtk_widget_get_window(win));
    if (aw) g_object_unref(aw);
    return x11;
}

/* Everything on this path is kept short: the panel must be on screen the moment you press the shortcut. */
static void show_popup(void) {
    int ex, ey;
    if (emoji_started() && emoji_position(&ex, &ey)) {   /* Super+V while the emoji panel is open: same spot */
        if (at_x < 0) { at_x = ex; at_y = ey; }
        emoji_command(2, (char *[]){ "superv", "--hide", NULL });
    }
    if (drop_src) { g_source_remove(drop_src); drop_src = 0; }
    prev_is_x11 = sv_focused_is_x11();
    apply_theme();
    capturing = FALSE;
    hotkey_status(NULL, TRUE);
    int idx = next_index();
    refresh(idx);
    had_focus = FALSE;
    place_window();
    gtk_widget_show(win);
    GdkWindow *gw = gtk_widget_get_window(win);
    guint32 ts = GDK_IS_X11_WINDOW(gw) ? gdk_x11_get_server_time(gw) : GDK_CURRENT_TIME;
    gtk_window_present_with_time(GTK_WINDOW(win), ts);
    g_timeout_add(60, ensure_focus, GUINT_TO_POINTER(ts));
    g_timeout_add(300, ensure_focus, GUINT_TO_POINTER(ts));
    g_timeout_add(700, ensure_focus, GUINT_TO_POINTER(ts));
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(listbox), idx);
    if (row) gtk_widget_grab_focus(GTK_WIDGET(row));
    const char *snap = g_getenv("SUPERV_SNAPSHOT");   /* SUPERV_SNAPSHOT=file.png: for screenshots */
    if (snap && *snap) g_timeout_add(900, snapshot_window, (gpointer)snap);
}

static void toggle(void) { if (gtk_widget_get_visible(win)) gtk_widget_hide(win); else show_popup(); }

/* A window's first opening is its slowest: GTK creates it, loads fonts and styles, measures every label.
 * Do that once, unseen, shortly after login, so the first Super+V is as quick as every later one. What it
 * built is freed again after a while, like after any closing of the panel. */
static gboolean warm_up(gpointer d) {
    if (gtk_widget_get_visible(win) || !rows_dirty) return G_SOURCE_REMOVE;   /* already opened */
    apply_theme();
    refresh(0);
    gtk_widget_realize(win);
    GtkRequisition nat;
    gtk_widget_get_preferred_size(win, NULL, &nat);
    GtkAllocation al = { 0, 0, cfg.width, cfg.height };
    gtk_widget_size_allocate(win, &al);
    if (!drop_src) drop_src = g_timeout_add_seconds(ROWS_KEEP_S, drop_rows_idle, NULL);
    return G_SOURCE_REMOVE;
}

const char *sv_paste_keys(void) { return cfg.paste_keys; }

/* GNOME's instant screenshot also lands on the clipboard. While on: nothing is recorded.
 * Off: what you had copied comes back, if GNOME really replaced it. */
void sv_clipboard_guard(gboolean on) {
    if (on) {
        guard_until = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
        guard_changed = FALSE;
        g_free(guard_id);
        guard_id = g_strdup(current_id);   /* NULL when what's on the clipboard isn't an item of ours */
        return;
    }
    guard_until = 0;
    Item *it = guard_id && guard_changed ? find_item(guard_id, NULL) : NULL;
    if (it) set_clipboard(it, FALSE);
    g_clear_pointer(&guard_id, g_free);
}

/* SuperV's look (rounded panel, theme, colours) for another small window. */
void sv_style_window(GtkWidget *w) {
    GdkScreen *scr = gtk_widget_get_screen(w);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis && gdk_screen_is_composited(scr)) { gtk_widget_set_visual(w, vis); gtk_widget_set_app_paintable(w, TRUE); }
    else add_class(w, "cbh-nocomp");
    add_class(w, "cbh-window");
    GtkStyleContext *c = gtk_widget_get_style_context(w);
    gboolean dark = sv_theme_is_dark();
    gtk_style_context_remove_class(c, dark ? "cbh-light" : "cbh-dark");
    gtk_style_context_add_class(c, dark ? "cbh-dark" : "cbh-light");
}

/* The emoji panel's 📋 button, or Super+V while it is open: one panel at a time, like Windows. */
void sv_show_clipboard_at(int x, int y) {
    if (gtk_widget_get_visible(win)) return;
    at_x = x; at_y = y;
    show_popup();
}

/* Where the clipboard panel is, if it is open. */
static gboolean clipboard_position(int *x, int *y) {
    if (!gtk_widget_get_visible(win)) return FALSE;
    gtk_window_get_position(GTK_WINDOW(win), x, y);
    return TRUE;
}

static void hotkey_status(const char *text, gboolean ok) {
    if (!hk_status) return;
    GtkStyleContext *c = gtk_widget_get_style_context(hk_status);
    gtk_style_context_remove_class(c, ok ? "cbh-err" : "cbh-ok");
    gtk_style_context_add_class(c, ok ? "cbh-ok" : "cbh-err");
    gtk_label_set_text(GTK_LABEL(hk_status), text);
    gtk_widget_set_visible(hk_status, text != NULL);
}

char *sv_accel_label(const char *accel) {   /* "<Super>v" → "Super+V" */
    guint key; GdkModifierType mods;
    gtk_accelerator_parse(accel, &key, &mods);
    return key ? gtk_accelerator_get_label(key, mods) : g_strdup(accel);
}
#define accel_label sv_accel_label

/* ---- the setup helper (superv-setup, Python) registers shortcuts with the desktop. Its runs are queued: they
 * all edit the same desktop shortcut lists. One that hangs is killed after half a minute. */
typedef struct { char **argv; SvHelperDone done; gpointer data; GPid pid; guint kill_src; } HelperJob;
static GQueue helper_jobs = G_QUEUE_INIT;
static HelperJob *helper_job;   /* the one running */
static void helper_next(void);

static char *setup_path(void) {
    const char *dirs[] = { sv_self_dir, SETUP_DIR, "/usr/lib/superv", "/usr/local/lib/superv", "/usr/libexec/superv" };
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        char *p = g_build_filename(dirs[i], "superv-setup", NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
        g_free(p);
    }
    return g_build_filename(SETUP_DIR, "superv-setup", NULL);
}

/* the system's Python (it has the GLib bindings), not whichever one a conda or pyenv shell put first */
static const char *python(void) {
    return g_file_test("/usr/bin/python3", G_FILE_TEST_IS_EXECUTABLE) ? "/usr/bin/python3" : "python3";
}

static gboolean helper_kill(gpointer p) {
    HelperJob *j = p;
    j->kill_src = 0;
    logmsg("the setup helper didn't finish: stopping it");
    kill(j->pid, SIGKILL);
    return G_SOURCE_REMOVE;
}

static void on_helper_exit(GPid pid, gint status, gpointer p) {
    HelperJob *j = p;
    g_spawn_close_pid(pid);
    if (j->kill_src) g_source_remove(j->kill_src);
    helper_job = NULL;
    if (j->done) j->done(WIFEXITED(status) ? WEXITSTATUS(status) : -1, j->data);
    g_strfreev(j->argv); g_free(j);
    helper_next();
}

static void helper_next(void) {
    HelperJob *j;
    while (!helper_job && (j = g_queue_pop_head(&helper_jobs))) {
        GError *e = NULL;
        if (g_spawn_async(NULL, j->argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &j->pid, &e)) {
            helper_job = j;
            j->kill_src = g_timeout_add_seconds(30, helper_kill, j);
            g_child_watch_add(j->pid, on_helper_exit, j);
            return;
        }
        logmsg("could not run %s: %s", j->argv[1], e->message);
        g_error_free(e);
        if (j->done) j->done(-1, j->data);
        g_strfreev(j->argv); g_free(j);
    }
}

/* args: superv-setup's arguments, NULL-terminated. done gets its exit status: 0 done, 3 this desktop needs the
 * shortcut added by hand, anything else: it failed. */
void sv_run_helper(char **args, SvHelperDone done, gpointer data) {
    HelperJob *j = g_new0(HelperJob, 1);
    guint n = g_strv_length(args);
    j->argv = g_new0(char *, n + 3);
    j->argv[0] = g_strdup(python());
    j->argv[1] = setup_path();
    for (guint i = 0; i < n; i++) j->argv[i + 2] = g_strdup(args[i]);
    j->done = done; j->data = data;
    g_queue_push_tail(&helper_jobs, j);
    helper_next();
}

#define MANUAL_SHORTCUT "This desktop can't be set up automatically: add a keyboard shortcut in its settings " \
                        "that runs “superv”."

static void on_hotkey_applied(int status, gpointer accel) {
    reload_hotkey();
    cfg.setup_done = status == 0 || status == 3;   /* a failure is tried again at the next start */
    config_save();
    if (status == 0) {
        char *label = accel_label(accel), *msg = g_strdup_printf("✓ Shortcut set to %s", label);
        hotkey_status(msg, TRUE);
        g_free(msg); g_free(label);
    } else {
        hotkey_status(status == 3 ? MANUAL_SHORTCUT : "Couldn't save the shortcut. Run “superv --doctor” to see why.", FALSE);
    }
    g_free(accel);
}

/* custom shortcut: press the new keys while the button says "Press keys…" */
static void apply_hotkey(const char *new_accel) {
    char *accel = g_strdup(new_accel);   /* new_accel may be cfg.hotkey itself */
    g_free(cfg.hotkey);
    cfg.hotkey = accel;
    hotkey_applied_at = g_get_monotonic_time();
    config_save();
    char *quoted = g_shell_quote(sv_self_exe), *cmd = g_strdup_printf("%s --toggle", quoted);
    char *args[] = { "--setup-hotkeys", cmd, "--hotkey", accel, NULL };
    sv_run_helper(args, on_hotkey_applied, g_strdup(accel));
    g_free(quoted); g_free(cmd);
}

/* ---- reading a shortcut back from the desktop: GNOME and Cinnamon keep them where we can look; elsewhere
 * (KDE, MATE, Xfce, tiling WMs) we trust what was saved. slug: "" for the clipboard, else the feature's;
 * name: its name in the desktop's shortcut list. */
#define GNOME_KB_SCHEMA "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding"
#define CIN_KB_SCHEMA   "org.cinnamon.desktop.keybindings.custom-keybinding"

static gboolean schema_has_key(const char *schema_id, const char *key) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, schema_id, TRUE) : NULL;
    gboolean ok = schema && g_settings_schema_has_key(schema, key);
    if (schema) g_settings_schema_unref(schema);
    return ok;
}

enum { DE_OTHER, DE_GNOME, DE_CINNAMON };
static int desktop_kind(void) {
    char *de = g_ascii_strup(g_getenv("XDG_CURRENT_DESKTOP") ? g_getenv("XDG_CURRENT_DESKTOP") : "", -1);
    int kind = strstr(de, "CINNAMON") ? DE_CINNAMON
             : (strstr(de, "GNOME") || strstr(de, "UNITY") || strstr(de, "BUDGIE") || strstr(de, "PANTHEON"))
               ? DE_GNOME : DE_OTHER;
    g_free(de);
    return kind;
}

/* The desktop's entry for that shortcut: *bound says whether there is one (TRUE too when we can't look),
 * and the key it is on is returned when we can read it. */
static char *desktop_entry(const char *slug, const char *name, gboolean *bound) {
    char *key = NULL;
    *bound = TRUE;
    switch (desktop_kind()) {
    case DE_GNOME: {
        if (!schema_has_key("org.gnome.settings-daemon.plugins.media-keys", "custom-keybindings")) break;
        char *path = g_strdup_printf("/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/superv%s%s/",
                                     *slug ? "-" : "", slug);
        GSettings *s = g_settings_new("org.gnome.settings-daemon.plugins.media-keys");
        char **v = g_settings_get_strv(s, "custom-keybindings");
        *bound = g_strv_contains((const char *const *)v, path);
        if (*bound) {
            GSettings *kb = g_settings_new_with_path(GNOME_KB_SCHEMA, path);
            key = g_settings_get_string(kb, "binding");
            g_object_unref(kb);
        }
        g_strfreev(v); g_object_unref(s); g_free(path);
        break;
    }
    case DE_CINNAMON: {
        if (!schema_has_key("org.cinnamon.desktop.keybindings", "custom-list")) break;
        GSettings *s = g_settings_new("org.cinnamon.desktop.keybindings");
        char **ids = g_settings_get_strv(s, "custom-list");
        *bound = FALSE;
        for (int i = 0; ids[i] && !*bound; i++) {
            char *path = g_strdup_printf("/org/cinnamon/desktop/keybindings/custom-keybindings/%s/", ids[i]);
            GSettings *kb = g_settings_new_with_path(CIN_KB_SCHEMA, path);
            char *n = g_settings_get_string(kb, "name");
            if (!g_strcmp0(n, name)) {
                *bound = TRUE;
                GVariant *v = g_settings_get_value(kb, "binding");
                if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
                    const char **b = g_variant_get_strv(v, NULL);
                    if (b[0]) key = g_strdup(b[0]);
                    g_free(b);
                } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
                    key = g_variant_dup_string(v, NULL);
                }
                g_variant_unref(v);
            }
            g_free(n); g_object_unref(kb); g_free(path);
        }
        g_strfreev(ids); g_object_unref(s);
        break;
    }
    }
    if (key && !*key) g_clear_pointer(&key, g_free);
    return key;
}

char *sv_desktop_hotkey(const char *slug, const char *name) {   /* NULL when we can't tell */
    gboolean bound;
    return desktop_entry(slug, name, &bound);
}

gboolean sv_shortcut_bound(const char *slug, const char *name) {   /* TRUE when we can't tell */
    gboolean bound;
    g_free(desktop_entry(slug, name, &bound));
    return bound;
}

static void on_hotkey_btn(GtkButton *b, gpointer d) {
    capturing = TRUE;
    gtk_button_set_label(b, "Press keys…");
    hotkey_status("Press the new shortcut, e.g. Super+V or Ctrl+Alt+V. Esc cancels.", TRUE);
    gtk_style_context_remove_class(gtk_widget_get_style_context(hk_status), "cbh-ok");
}

/* A key press while a shortcut is being chosen. Returns SV_KEY_WAIT (a modifier so far), SV_KEY_CANCEL (Esc),
 * SV_KEY_REFUSED (*msg says why; keep listening) or SV_KEY_CHOSEN (*accel, e.g. "<Super>v"). Free both. */
int sv_capture_accel(GdkEventKey *e, char **accel, char **msg) {
    if (e->is_modifier) return SV_KEY_WAIT;
    if (e->keyval == GDK_KEY_Escape) return SV_KEY_CANCEL;
    GdkModifierType state = e->state;
    gdk_keymap_add_virtual_modifiers(gdk_keymap_get_for_display(gdk_display_get_default()), &state);
    state &= gtk_accelerator_get_default_mod_mask() & ~(GDK_HYPER_MASK | GDK_META_MASK);
    guint key = gdk_keyval_to_lower(e->keyval);
    /* Must use Super, or Ctrl+Alt: Ctrl+V, Ctrl+Shift+V, Alt+V… belong to apps (paste, menus). */
    gboolean ctrl = state & GDK_CONTROL_MASK, alt = state & GDK_MOD1_MASK, super = state & GDK_SUPER_MASK;
    char *name = gtk_accelerator_name(key, state);
    if (super || (ctrl && alt)) { *accel = name; return SV_KEY_CHOSEN; }
    char *label = accel_label(name);
    *msg = g_strdup_printf(ctrl ? "%s isn't allowed: apps use Ctrl shortcuts (copy, paste, undo…). Use Super or Ctrl+Alt with a key."
                         : alt  ? "%s isn't allowed: apps use Alt shortcuts for their menus. Use Super or Ctrl+Alt with a key."
                                : "%s isn't allowed: a shortcut needs Super, or Ctrl+Alt, plus a key.", label);
    g_free(label); g_free(name);
    return SV_KEY_REFUSED;
}

static gboolean capture_key(GdkEventKey *e) {
    char *accel = NULL, *msg = NULL;
    switch (sv_capture_accel(e, &accel, &msg)) {
    case SV_KEY_CANCEL:
        capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels();
        break;
    case SV_KEY_REFUSED:
        hotkey_status(msg, FALSE);
        break;
    case SV_KEY_CHOSEN:
        capturing = FALSE;
        hotkey_status("Saving…", TRUE);
        gtk_style_context_remove_class(gtk_widget_get_style_context(hk_status), "cbh-ok");
        apply_hotkey(accel);
        update_hotkey_labels();
    }
    g_free(accel); g_free(msg);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (capturing) return capture_key(e);
    if (any_menu_open()) return FALSE;
    switch (e->keyval) {
    case GDK_KEY_Escape:
        gtk_widget_hide(win);
        return TRUE;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: {
        int i = focused_index();
        if (i >= 0 && i < (int)items->len) { drop_item(items->pdata[i]); history_save(); refresh(i); }
        return TRUE;
    }
    case GDK_KEY_Up: case GDK_KEY_Down:
        if (focused_index() < 0) {
            GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(listbox), 0);
            if (r) gtk_widget_grab_focus(GTK_WIDGET(r));
            return TRUE;
        }
    }
    return FALSE;
}

/* Close every open menu at once (no animation), so none is still open next time. */
static void close_menus(void) {
    if (settings_pop) gtk_widget_hide(settings_pop);
    if (row_pop) gtk_widget_hide(row_pop);
}

/* Like Windows: a click anywhere outside closes the panel, menus included. */
static gboolean hide_if_inactive(gpointer d) {
    if (gtk_widget_get_visible(win) && !gtk_window_is_active(GTK_WINDOW(win))) {
        if (sv_debug) logmsg("panel lost focus: closing");
        if (capturing) { capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels(); }
        close_menus();
        gtk_widget_hide(win);
    }
    return G_SOURCE_REMOVE;
}

static gboolean trim_memory(gpointer d) { malloc_trim(0); return G_SOURCE_REMOVE; }
/* ---- updates: when a package replaces our program file, restart into the new version.
 * Waits until nothing is open or in progress; keeps what's on the clipboard. */
static guint restart_src;
static gboolean restart_pending;
static GFileMonitor *exe_monitor;

static gboolean restart_now(gpointer d) {
    restart_src = 0;
    if (gtk_widget_get_visible(win)) { restart_pending = TRUE; return G_SOURCE_REMOVE; }
    if ((emoji_started() && emoji_busy()) || extras_busy() || shot_busy() || shot_pins() || helper_job || reading) {
        restart_src = g_timeout_add_seconds(10, restart_now, NULL);   /* a panel, picker, pin or helper: later */
        return G_SOURCE_REMOVE;
    }
    restart_pending = FALSE;
    if (!g_file_test(sv_self_exe, G_FILE_TEST_IS_EXECUTABLE)) return G_SOURCE_REMOVE;   /* removed, not updated */
    logmsg("SuperV was updated: restarting into the new version");
    if (geom_dirty) { geom_dirty = FALSE; reload_hotkey(); config_save(); }
    history_save();
    history_flush();
    if (emoji_started()) emoji_shutdown();
    char *restore = current_id ? g_strdup_printf("--restore=%s", current_id) : NULL;
    char *argv[] = { sv_self_exe, "--daemon", restore, NULL };
    GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(sv_app));
    if (bus) g_dbus_connection_close_sync(bus, NULL, NULL);   /* free our D-Bus name for the new process */
    g_usleep(200000);
    execv(sv_self_exe, argv);
    /* we gave up our D-Bus name, so we can't carry on: a second SuperV would start next to this one. Quit;
     * the next press of the shortcut starts the new version. */
    logmsg("restart failed (%s): quitting", g_strerror(errno));
    exit(1);
}

static void on_exe_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer d) {
    if (ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_MOVED_OUT) return;
    if (restart_src) g_source_remove(restart_src);
    restart_src = g_timeout_add_seconds(3, restart_now, NULL);   /* let the package manager finish */
}

static void watch_for_updates(void) {
    GFile *f = g_file_new_for_path(sv_self_exe);
    exe_monitor = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (exe_monitor) g_signal_connect(exe_monitor, "changed", G_CALLBACK(on_exe_changed), NULL);
    g_object_unref(f);
}

static void on_hide(GtkWidget *w, gpointer d) {
    if (geom_dirty) { geom_dirty = FALSE; reload_hotkey(); config_save(); }
    if (tip_mode == TIP_EMOJI) tip_mode = TIP_NONE;   /* the question goes away with the panel */
    g_timeout_add(500, trim_memory, NULL);
    /* reopening soon is instant (the rows are kept); after a while they and their pictures are freed */
    if (drop_src) g_source_remove(drop_src);
    drop_src = g_timeout_add_seconds(ROWS_KEEP_S, drop_rows_idle, NULL);
    if (restart_pending && !restart_src) restart_src = g_timeout_add(1500, restart_now, NULL);   /* after any paste */
}

static gboolean on_focus_in(GtkWidget *w, GdkEvent *e, gpointer d) { had_focus = TRUE; return FALSE; }

static gboolean on_focus_out(GtkWidget *w, GdkEvent *e, gpointer d) {
    if (had_focus) g_timeout_add(120, hide_if_inactive, NULL);
    return FALSE;
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer d) {
    Item *it = find_item(g_object_get_data(G_OBJECT(row), "id"), NULL);
    if (it) paste_item(it);
}

static gboolean on_switch(GtkSwitch *s, gboolean state, gpointer d) { set_history_enabled(state); return FALSE; }
static void on_clear(GtkWidget *b, gpointer d) { if (settings_pop) gtk_popover_popdown(GTK_POPOVER(settings_pop)); clear_all(); }
static void on_turn_on(GtkWidget *b, gpointer d) { set_history_enabled(TRUE); }

static GtkWidget *message_page(const char *title, const char *body, const char *btn, GCallback cb) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10), *t = gtk_label_new(title), *b = gtk_label_new(body);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(box, 24); gtk_widget_set_margin_end(box, 24);
    add_class(t, "cbh-title"); add_class(b, "cbh-sub");
    gtk_label_set_line_wrap(GTK_LABEL(b), TRUE);
    gtk_label_set_justify(GTK_LABEL(b), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(b), 36);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    if (btn) {
        GtkWidget *bt = gtk_button_new_with_label(btn);
        gtk_widget_set_halign(bt, GTK_ALIGN_CENTER);
        add_class(bt, "cbh-primary");
        g_signal_connect(bt, "clicked", cb, NULL);
        gtk_box_pack_start(GTK_BOX(box), bt, FALSE, FALSE, 6);
    }
    return box;
}

/* ---- appearance: theme (System / Light / Dark) and text size */
static void apply_font(void) {
    cfg.font_scale = CLAMP(cfg.font_scale, 80, 160);
    char *css = g_strdup_printf(".cbh-window { font-size: %d%%; }", cfg.font_scale);
    gtk_css_provider_load_from_data(font_css, css, -1, NULL);
    g_free(css);
    char *l = g_strdup_printf("%d%%", cfg.font_scale);
    if (size_lbl) gtk_label_set_text(GTK_LABEL(size_lbl), l);
    g_free(l);
}

/* One theme for every SuperV window; the emoji panel's own Theme row sets it too. */
const char *sv_theme(void) { return cfg.theme; }

void sv_set_theme(const char *id) {
    if (!g_strcmp0(cfg.theme, id)) return;
    reload_hotkey();
    g_free(cfg.theme);
    cfg.theme = g_strdup(id);
    config_save();
    apply_theme();
    sync_settings();
    if (emoji_started()) emoji_palette_changed();
}

static void on_theme_toggled(GtkToggleButton *b, gpointer which) {
    if (!syncing && gtk_toggle_button_get_active(b)) sv_set_theme(which);
}

static void on_font_step(GtkButton *b, gpointer delta) {
    cfg.font_scale += GPOINTER_TO_INT(delta);
    apply_font();
    reload_hotkey();
    config_save();
    refresh(focused_index());
}

static GtkWidget *settings_row(GtkWidget *box, const char *label, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16), *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}

static void on_palette(GtkToggleButton *b, gpointer id) {
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    sv_set_palette(id);
}

/* Also used by the emoji panel's own colour row: one palette for every SuperV window. */
void sv_set_palette(const char *id) {
    if (!g_strcmp0(cfg.palette, id)) return;
    reload_hotkey();
    g_free(cfg.palette);
    cfg.palette = g_strdup(id);
    config_save();
    apply_palette();
    syncing = TRUE;
    for (guint k = 0; k < N_PAL; k++)
        if (pal_btns[k]) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pal_btns[k]), (int)k == palette_index());
    syncing = FALSE;
    if (emoji_started()) emoji_palette_changed();
    logmsg("palette: %s", cfg.palette);
}

static GtkWidget *palette_control(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GSList *group = NULL;
    for (guint k = 0; k < N_PAL; k++) {
        char cls[16];
        g_snprintf(cls, sizeof cls, "cbh-sw%u", k);
        GtkWidget *b = pal_btns[k] = gtk_radio_button_new(group);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(b));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
        add_class(b, "cbh-sw");
        add_class(b, cls);
        gtk_widget_set_tooltip_text(b, PALETTES[k].name);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), (int)k == palette_index());
        g_signal_connect(b, "toggled", G_CALLBACK(on_palette), (gpointer)PALETTES[k].id);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    }
    return box;
}

static GtkWidget *theme_control(void) {
    static const char *names[] = { "System", "Light", "Dark" }, *values[] = { "auto", "light", "dark" };
    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(seg, "linked");
    add_class(seg, "cbh-seg");
    GSList *group = NULL;
    for (int i = 0; i < 3; i++) {
        theme_btns[i] = gtk_radio_button_new_with_label(group, names[i]);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(theme_btns[i]));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(theme_btns[i]), FALSE);   /* look like buttons */
        g_signal_connect(theme_btns[i], "toggled", G_CALLBACK(on_theme_toggled), (gpointer)values[i]);
        gtk_box_pack_start(GTK_BOX(seg), theme_btns[i], FALSE, FALSE, 0);
    }
    return seg;
}

static GtkWidget *size_control(void) {
    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *minus = gtk_button_new_with_label("A−"), *plus = gtk_button_new_with_label("A+");
    add_class(seg, "linked");
    add_class(seg, "cbh-seg");
    size_lbl = gtk_label_new("");
    gtk_label_set_width_chars(GTK_LABEL(size_lbl), 5);
    add_class(size_lbl, "cbh-size");
    gtk_widget_set_tooltip_text(minus, "Smaller text");
    gtk_widget_set_tooltip_text(plus, "Larger text");
    g_signal_connect(minus, "clicked", G_CALLBACK(on_font_step), GINT_TO_POINTER(-10));
    g_signal_connect(plus, "clicked", G_CALLBACK(on_font_step), GINT_TO_POINTER(10));
    gtk_box_pack_start(GTK_BOX(seg), minus, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(seg), size_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(seg), plus, FALSE, FALSE, 0);
    return seg;
}

static void on_reset(GtkButton *b, gpointer d) {
    sv_set_theme("auto");
    sv_set_palette("violet");
    reload_hotkey();
    cfg.font_scale = 100;
    cfg.width = WIN_W + 20; cfg.height = WIN_H + 20; cfg.pos_x = cfg.pos_y = -1;
    apply_font();
    if (g_strcmp0(cfg.hotkey, DEFAULT_HOTKEY)) apply_hotkey(DEFAULT_HOTKEY);   /* also saves */
    else config_save();
    place_window();
    geom_dirty = FALSE;   /* the resulting move/resize must not be saved as "user placed it here" */
    refresh(focused_index());
}

static GtkWidget *keys_revealer;

static void on_keys_toggled(GtkToggleButton *b, gpointer d) {
    gboolean open = gtk_toggle_button_get_active(b);
    gtk_revealer_set_reveal_child(GTK_REVEALER(keys_revealer), open);
    gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(open ? "pan-down-symbolic" : "pan-end-symbolic",
                                                                     GTK_ICON_SIZE_MENU));
}

static void on_open_repo(GtkButton *b, gpointer d) {
    GError *e = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(win), REPO_URL, gtk_get_current_event_time(), &e)) {
        logmsg("could not open the browser: %s", e->message);
        g_error_free(e);
    }
}

static GtkWidget *keys_help(void) {
    static const char *rows[][2] = {
        { NULL,      "Open or close this panel" },
        { "↑  ↓",    "Choose an item" },
        { "Enter",   "Paste the chosen item" },
        { "Delete",  "Remove the chosen item" },
        { "Esc",     "Close the panel" },
    };
    GtkWidget *grid = gtk_grid_new();
    add_class(grid, "cbh-keys");
    gtk_widget_set_margin_bottom(grid, 4);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    for (int i = 0; i < (int)G_N_ELEMENTS(rows); i++) {
        GtkWidget *key = gtk_label_new(rows[i][0] ? rows[i][0] : ""), *what = gtk_label_new(rows[i][1]);
        if (!rows[i][0]) hint_lbl = key;   /* shows the current shortcut */
        add_class(key, "cbh-kbd");
        gtk_widget_set_halign(key, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(what), 0);
        add_class(what, "cbh-keydesc");
        gtk_grid_attach(GTK_GRID(grid), key, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), what, 1, i, 1, 1);
    }
    return grid;
}

static gboolean focus_selected_row(gpointer d) {
    if (!gtk_widget_get_visible(win) || any_menu_open()) return G_SOURCE_REMOVE;
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox));
    if (!r) r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(listbox), 0);
    if (r) gtk_widget_grab_focus(GTK_WIDGET(r));
    return G_SOURCE_REMOVE;
}

static void on_menu_closed(GtkPopover *p, gpointer d) {
    if (capturing) { capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels(); }
    g_timeout_add(200, focus_selected_row, NULL);   /* after the close animation */
}

/* ---- the features (emoji.c, extras.c, shot.c): each with a switch; their shortcuts are registered by
 * superv-setup, which keeps them in emoji.ini / shortcuts.ini */
static GKeyFile *ini(const char *file) {
    GKeyFile *k = g_key_file_new();
    char *p = g_build_filename(cfg_dir, file, NULL);
    g_key_file_load_from_file(k, p, G_KEY_FILE_NONE, NULL);
    g_free(p);
    return k;
}

static gboolean ini_configured(const char *file, const char *group) {
    GKeyFile *k = ini(file);
    gboolean done = g_key_file_get_boolean(k, group, "shortcut_configured", NULL);
    g_key_file_unref(k);
    return done;
}

static char *feat_hotkey(int f) {
    GKeyFile *k = ini("shortcuts.ini");
    char *v = g_key_file_get_string(k, FEAT[f].id, "hotkey", NULL);
    g_key_file_unref(k);
    if (v) g_strstrip(v);
    return v && *v ? v : (g_free(v), g_strdup(FEAT[f].def));
}

static void update_feature_controls(void);

static void on_feat_applied(int status, gpointer fp) {
    int f = GPOINTER_TO_INT(fp) - 1;
    if (status == 3 && f >= 0) {   /* turned on, on a desktop whose shortcuts we can't set */
        char *body = g_strdup_printf("Add a keyboard shortcut in your desktop's settings that runs: superv %s", FEAT[f].arg);
        extras_toast(FEAT[f].label, body);
        g_free(body);
    }
    update_feature_controls();
}

/* Register the feature's shortcut with the desktop, or give it back (and what we took for it). */
static void feat_shortcut(int f, gboolean on) {
    char *quoted = g_shell_quote(sv_self_exe), *cmd = g_strdup_printf("%s %s", quoted, FEAT[f].arg);
    char *accel = feat_hotkey(f);
    char *set[] = { "--feature", (char *)FEAT[f].id, "--setup-hotkeys", cmd, "--hotkey", accel, NULL };
    char *unset[] = { "--feature", (char *)FEAT[f].id, "--remove-hotkeys", NULL };
    sv_run_helper(on ? set : unset, on_feat_applied, GINT_TO_POINTER(on ? f + 1 : 0));
    g_free(quoted); g_free(cmd); g_free(accel);
}

static void update_feature_controls(void) {
    for (int f = 0; f < N_FEAT && feat_sw[f]; f++) {
        syncing = TRUE;
        gtk_switch_set_active(GTK_SWITCH(feat_sw[f]), cfg.feat[f]);
        syncing = FALSE;
        char *accel = feat_hotkey(f), *label = accel_label(accel), *text;
        if (!cfg.feat[f]) text = g_strdup_printf("Off · turn on for %s", label);
        else if (f == F_TEXT && !extras_ocr_available()) text = g_strdup_printf("%s · press it once to install Tesseract", label);
        else text = g_strdup_printf("%s %s", label, FEAT[f].what);
        gtk_label_set_text(GTK_LABEL(feat_lbl[f]), text);
        g_free(text); g_free(label); g_free(accel);
    }
}

static gboolean on_feat_switch(GtkSwitch *s, gboolean state, gpointer fp) {
    int f = GPOINTER_TO_INT(fp);
    if (syncing || cfg.feat[f] == state) return FALSE;
    reload_hotkey();
    cfg.feat[f] = state;
    config_save();
    feat_shortcut(f, state);   /* (turning Screenshot off gives Super+Shift+S back to the desktop's own tool) */
    if (sv_debug) logmsg("%s turned %s", FEAT[f].id, state ? "on" : "off");
    update_feature_controls();
    update_tip();
    return FALSE;
}

static void feature_row(GtkWidget *box, int f) {
    static const char *tips[N_FEAT] = {
        "Like Ctrl+Win+Alt+V (PowerToys): pastes what you copied as plain text, without fonts, colours or links.",
        "Like Win+Shift+C (PowerToys): click anywhere on screen; its colour is copied as HEX, and RGB and HSL are one click away.",
        "Like Win+Shift+T (PowerToys) or the Snipping Tool's text actions: select an area of the screen, its text is copied.",
        "Like Win+Shift+S (Snipping Tool), Flameshot-style: the screen freezes, select an area, draw arrows, boxes, "
        "text, numbers or blur on it, then copy, save or pin it. Off: Super+Shift+S opens your desktop's own tool.",
    };
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16), *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *l = gtk_label_new(FEAT[f].label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    feat_lbl[f] = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(feat_lbl[f]), 0);
    add_class(feat_lbl[f], "cbh-status");
    gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), feat_lbl[f], FALSE, FALSE, 0);
    feat_sw[f] = gtk_switch_new();
    gtk_widget_set_valign(feat_sw[f], GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(row, tips[f]);
    g_signal_connect(feat_sw[f], "state-set", G_CALLBACK(on_feat_switch), GINT_TO_POINTER(f));
    gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), feat_sw[f], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
}

/* ---- the emoji panel (emoji.c): started only while it is turned on */
static void update_emoji_controls(void) {
    if (!emoji_sw) return;
    update_feature_controls();
    syncing = TRUE;
    gtk_switch_set_active(GTK_SWITCH(emoji_sw), cfg.emoji_panel);
    syncing = FALSE;
    char *label = cfg.emoji_panel && emoji_started() ? emoji_hotkey_label() : NULL;
    char *text = label ? g_strdup_printf("%s opens emoji, kaomoji, symbols", label) : g_strdup("Off · turn on for Super+.");
    gtk_label_set_text(GTK_LABEL(emoji_lbl), text);
    g_free(text); g_free(label);
}

static void set_emoji_panel(gboolean on) {
    if (cfg.emoji_panel == on) return;
    reload_hotkey();
    cfg.emoji_panel = on;
    config_save();
    if (on && !emoji_started()) emoji_start();   /* sets up Super+. itself */
    else if (emoji_started()) emoji_set_shortcut(on);
    if (sv_debug) logmsg("emoji panel turned %s", on ? "on" : "off");
    update_emoji_controls();
    update_tip();
}

static gboolean on_emoji_switch(GtkSwitch *s, gboolean state, gpointer d) {
    if (!syncing) set_emoji_panel(state);
    return FALSE;
}

static void on_emoji_btn(GtkButton *b, gpointer d) {   /* ☺ in the header */
    if (!cfg.emoji_panel) {   /* off: ask before taking Super+. from the desktop */
        tip_mode = TIP_EMOJI;
        update_tip();
        return;
    }
    int x = -1, y = -1;   /* on: switch to the emoji panel, in the same spot */
    clipboard_position(&x, &y);
    gtk_widget_hide(win);
    if (!emoji_started()) emoji_start();
    emoji_show_at(x, y);
}

static void on_tip_yes(GtkButton *b, gpointer d) {
    if (tip_mode == TIP_EMOJI) {
        tip_mode = TIP_NONE;
        set_emoji_panel(TRUE);
        on_emoji_btn(NULL, NULL);
        return;
    }
    reload_hotkey();
    cfg.tip_seen = TRUE;
    config_save();
    update_tip();
    gtk_popover_popup(GTK_POPOVER(settings_pop));   /* where the switches are */
}

static void on_tip_no(GtkButton *b, gpointer d) {
    reload_hotkey();
    if (tip_mode == TIP_EMOJI) cfg.emoji_asked = TRUE; else cfg.tip_seen = TRUE;
    tip_mode = TIP_NONE;
    config_save();
    update_tip();
}

/* The gear menu is opening: bring its controls up to date (and never taller than the panel: it scrolls). */
static void on_settings_show(GtkWidget *pop, gpointer scroll) {
    reload_hotkey();
    sync_hotkey_from_desktop();
    sync_settings();
    int h = gtk_widget_get_allocated_height(win) - 90;   /* room below the header */
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), MAX(h, 160));
}

static GtkWidget *settings_button(void) {
    GtkWidget *btn = gtk_menu_button_new(), *old = gtk_bin_get_child(GTK_BIN(btn));
    if (old) gtk_container_remove(GTK_CONTAINER(btn), old);
    gtk_container_add(GTK_CONTAINER(btn), icon("emblem-system-symbolic", "preferences-system-symbolic", "⚙"));
    add_class(btn, "cbh-icon");
    gtk_widget_set_tooltip_text(btn, "Clipboard settings");
    settings_pop = gtk_popover_new(btn);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    add_class(box, "cbh-settings");   /* compact, fixed-size text (not changed by A-/A+) */

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16), *l = gtk_label_new("Clipboard history"), *col;
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    switch_w = gtk_switch_new();
    gtk_widget_set_valign(switch_w, GTK_ALIGN_CENTER);   /* don't stretch to the row height */
    g_signal_connect(switch_w, "state-set", G_CALLBACK(on_switch), NULL);
    gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), switch_w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    l = gtk_label_new("Shortcut");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    hotkey_btn = gtk_button_new_with_label(cfg.hotkey);
    gtk_widget_set_tooltip_text(hotkey_btn, "Click, then press the key combination you want");
    g_signal_connect(hotkey_btn, "clicked", G_CALLBACK(on_hotkey_btn), NULL);
    gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), hotkey_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    hk_status = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(hk_status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hk_status), 34);
    gtk_label_set_xalign(GTK_LABEL(hk_status), 0);
    add_class(hk_status, "cbh-status");
    gtk_widget_set_no_show_all(hk_status, TRUE);
    gtk_box_pack_start(GTK_BOX(box), hk_status, FALSE, FALSE, 0);

    /* Features: each with its own switch; one that is off loads nothing and leaves its keys to the desktop */
    GtkWidget *sec = gtk_label_new("Features");
    gtk_label_set_xalign(GTK_LABEL(sec), 0);
    add_class(sec, "cbh-section");
    gtk_box_pack_start(GTK_BOX(box), sec, FALSE, FALSE, 0);
    /* Emoji panel: on/off, with its shortcut underneath */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    l = gtk_label_new("Emoji panel");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    emoji_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(emoji_lbl), 0);
    add_class(emoji_lbl, "cbh-status");
    gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), emoji_lbl, FALSE, FALSE, 0);
    emoji_sw = gtk_switch_new();
    gtk_widget_set_valign(emoji_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(row, "Emoji, kaomoji and symbols, typed where you are writing (Win+. on Windows). "
                                     "Its own settings are behind the gear in the emoji panel.");
    g_signal_connect(emoji_sw, "state-set", G_CALLBACK(on_emoji_switch), NULL);
    gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), emoji_sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    for (int f = 0; f < N_FEAT; f++) feature_row(box, f);


    settings_row(box, "Theme", theme_control());
    gtk_widget_set_tooltip_text(settings_row(box, "Colour", palette_control()),
                                "The colour of every SuperV window: clipboard, emoji, colour picker, screenshots.");
    settings_row(box, "Text size", size_control());

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(actions), TRUE);
    GtkWidget *clear = gtk_button_new_with_label("Clear history");
    gtk_widget_set_tooltip_text(clear, "Removes everything except pinned items");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear), NULL);
    GtkWidget *reset = gtk_button_new_with_label("Reset settings");
    gtk_widget_set_tooltip_text(reset, "Back to defaults: theme, text size, panel size and position, and the "
                                       "Super+V shortcut. Your clipboard history is kept.");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), NULL);
    gtk_box_pack_start(GTK_BOX(actions), clear, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), reset, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);

    /* Footer: one divider, the shortcut list (slides open), then "Shortcuts ›  ·  v1.1.2 · GitHub ↗" */
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    keys_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(keys_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(keys_revealer), 150);
    gtk_container_add(GTK_CONTAINER(keys_revealer), keys_help());
    gtk_box_pack_start(GTK_BOX(box), keys_revealer, FALSE, FALSE, 0);

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    add_class(foot, "cbh-foot");
    GtkWidget *keys = gtk_toggle_button_new_with_label("Shortcuts");
    gtk_button_set_image(GTK_BUTTON(keys), gtk_image_new_from_icon_name("pan-end-symbolic", GTK_ICON_SIZE_MENU));
    gtk_button_set_image_position(GTK_BUTTON(keys), GTK_POS_RIGHT);
    gtk_button_set_always_show_image(GTK_BUTTON(keys), TRUE);
    gtk_widget_set_tooltip_text(keys, "Show keyboard shortcuts");
    g_signal_connect(keys, "toggled", G_CALLBACK(on_keys_toggled), NULL);
    GtkWidget *ver = gtk_label_new("v" VERSION " ·");
    GtkWidget *repo = gtk_button_new_with_label("GitHub ↗");   /* opens the browser only when clicked */
    gtk_widget_set_tooltip_text(repo, REPO_URL);
    g_signal_connect(repo, "clicked", G_CALLBACK(on_open_repo), NULL);
    add_class(keys, "cbh-footbtn");
    add_class(repo, "cbh-footbtn");
    add_class(ver, "cbh-ver");
    gtk_box_pack_start(GTK_BOX(foot), keys, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), repo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), ver, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), foot, FALSE, FALSE, 0);
    /* Never taller than the panel: scroll inside instead (e.g. at large text sizes). */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    gtk_widget_show_all(scroll);
    gtk_container_add(GTK_CONTAINER(settings_pop), scroll);
    g_signal_connect(settings_pop, "show", G_CALLBACK(on_settings_show), scroll);
    g_signal_connect(settings_pop, "closed", G_CALLBACK(on_menu_closed), NULL);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), settings_pop);
    return btn;
}

/* ---- one-time permission for click-to-paste on Wayland (a virtual keyboard through /dev/uinput), asked with
 * the desktop's own password dialog. Both panels show its state. */
static gboolean paste_rule_installed(void) {   /* by a package, install.sh, or the Enable button (SuperDot 1.0 too) */
    static const char *rules[] = {
        "/usr/lib/udev/rules.d/70-superv-uinput.rules", "/lib/udev/rules.d/70-superv-uinput.rules",
        "/usr/local/lib/udev/rules.d/70-superv-uinput.rules", "/etc/udev/rules.d/70-superv-uinput.rules",
        "/usr/lib/udev/rules.d/70-superdot-uinput.rules", "/etc/udev/rules.d/70-superdot-uinput.rules",
    };
    for (guint i = 0; i < G_N_ELEMENTS(rules); i++) if (g_file_test(rules[i], G_FILE_TEST_EXISTS)) return TRUE;
    return FALSE;
}

static void paste_state_changed(int st) {
    paste_state = st;
    update_banner();
    if (emoji_started()) emoji_paste_changed();
}

static gboolean retry_uinput(gpointer d) {
    if (sv_uinput_open()) {
        paste_state_changed(SV_PASTE_ASK);
        extras_toast("Click-to-paste is on", "Pick an item or an emoji and it goes where you were typing.");
        return G_SOURCE_REMOVE;
    }
    if (++enable_tries < 12) return G_SOURCE_CONTINUE;
    paste_state_changed(SV_PASTE_RELOGIN);
    return G_SOURCE_REMOVE;
}

static void on_enable_done(GPid pid, gint status, gpointer d) {
    g_spawn_close_pid(pid);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        enable_tries = 0;
        g_timeout_add(500, retry_uinput, NULL);
    } else {
        paste_state_changed(SV_PASTE_FAILED);
    }
}

void sv_enable_paste(void) {
    static const char *script =
        "set -e; printf '%s\\n' 'KERNEL==\"uinput\", SUBSYSTEM==\"misc\", OPTIONS+=\"static_node=uinput\", TAG+=\"uaccess\"'"
        " > /etc/udev/rules.d/70-superv-uinput.rules;"
        " echo uinput > /etc/modules-load.d/superv-uinput.conf;"
        " modprobe uinput; udevadm control --reload-rules;"
        " udevadm trigger --action=change --sysname-match=uinput; udevadm settle";
    char *argv[] = { "pkexec", "/bin/sh", "-c", (char *)script, NULL };
    GPid pid;
    if (paste_state == SV_PASTE_ENABLING) return;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL)) {
        paste_state_changed(SV_PASTE_FAILED);   /* no pkexec here */
        return;
    }
    paste_state_changed(SV_PASTE_ENABLING);
    g_child_watch_add(pid, on_enable_done, NULL);
}

static void on_enable_paste(GtkButton *b, gpointer d) { sv_enable_paste(); }

static void build_window(void) {
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(win);
    gtk_window_set_title(w, "Clipboard");
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_resizable(w, TRUE);
    gtk_widget_set_size_request(win, 300, 320);
    gtk_window_set_default_size(w, cfg.width, cfg.height);
    gtk_widget_add_events(win, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
    gtk_window_set_keep_above(w, TRUE);
    gtk_window_set_skip_taskbar_hint(w, TRUE);
    gtk_window_set_skip_pager_hint(w, TRUE);
    gtk_window_set_type_hint(w, GDK_WINDOW_TYPE_HINT_DIALOG);
    GdkScreen *scr = gtk_widget_get_screen(win);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis && gdk_screen_is_composited(scr)) { gtk_widget_set_visual(win, vis); gtk_widget_set_app_paintable(win, TRUE); }
    else add_class(win, "cbh-nocomp");
    add_class(win, "cbh-window");
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    pal_css = gtk_css_provider_new();
    apply_palette();
    gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(pal_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    font_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(font_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_class(panel, "cbh-panel");
    gtk_container_add(GTK_CONTAINER(win), panel);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4), *title = gtk_label_new("Clipboard");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    add_class(title, "cbh-title");
    clear_btn = gtk_button_new_with_label("Clear all");
    add_class(clear_btn, "cbh-flat");
    gtk_widget_set_tooltip_text(clear_btn, "Clear everything except pinned items");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear), NULL);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(header), clear_btn, FALSE, FALSE, 0);
    emoji_btn = gtk_button_new();   /* like the tabs on top of Windows 11's Win+V panel */
    gtk_container_add(GTK_CONTAINER(emoji_btn), icon("face-smile-symbolic", "emoji-people-symbolic", "☺"));
    add_class(emoji_btn, "cbh-icon");
    gtk_widget_set_tooltip_text(emoji_btn, "Emoji, kaomoji and symbols");
    gtk_widget_set_no_show_all(emoji_btn, TRUE);
    gtk_widget_show(gtk_bin_get_child(GTK_BIN(emoji_btn)));
    g_signal_connect(emoji_btn, "clicked", G_CALLBACK(on_emoji_btn), NULL);
    gtk_box_pack_start(GTK_BOX(header), emoji_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), settings_button(), FALSE, FALSE, 0);
    GtkWidget *grab = gtk_event_box_new();   /* drag the header to move the panel */
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(grab), FALSE);
    gtk_container_add(GTK_CONTAINER(grab), header);
    gtk_widget_set_tooltip_text(title, "Drag to move · double-click to reset");
    g_signal_connect(grab, "button-press-event", G_CALLBACK(on_header_press), NULL);
    gtk_box_pack_start(GTK_BOX(panel), grab, FALSE, FALSE, 0);

    banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    add_class(banner, "cbh-banner");
    banner_lbl = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(banner_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(banner_lbl), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(banner_lbl), 30);
    add_class(banner_lbl, "cbh-sub");
    banner_btn = gtk_button_new_with_label("Enable");
    add_class(banner_btn, "cbh-primary");
    gtk_widget_set_valign(banner_btn, GTK_ALIGN_CENTER);
    g_signal_connect(banner_btn, "clicked", G_CALLBACK(on_enable_paste), NULL);
    gtk_box_pack_start(GTK_BOX(banner), banner_lbl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(banner), banner_btn, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(banner, TRUE);
    gtk_widget_show(banner_lbl); gtk_widget_show(banner_btn);
    gtk_box_pack_start(GTK_BOX(panel), banner, FALSE, FALSE, 0);

    tip = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);   /* "more features" once; "turn on the emoji panel?" */
    add_class(tip, "cbh-banner");
    tip_lbl = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(tip_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(tip_lbl), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(tip_lbl), 36);
    add_class(tip_lbl, "cbh-sub");
    GtkWidget *tip_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tip_yes = gtk_button_new_with_label("Choose");
    tip_no = gtk_button_new_with_label("Not now");
    add_class(tip_yes, "cbh-primary");
    add_class(tip_no, "cbh-flat");
    g_signal_connect(tip_yes, "clicked", G_CALLBACK(on_tip_yes), NULL);
    g_signal_connect(tip_no, "clicked", G_CALLBACK(on_tip_no), NULL);
    gtk_box_pack_end(GTK_BOX(tip_row), tip_yes, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tip_row), tip_no, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tip), tip_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tip), tip_row, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(tip, TRUE);
    gtk_widget_show_all(tip_lbl); gtk_widget_show_all(tip_row);
    gtk_box_pack_start(GTK_BOX(panel), tip, FALSE, FALSE, 0);

    stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(stack), FALSE);
    gtk_box_pack_start(GTK_BOX(panel), stack, TRUE, TRUE, 0);
    listbox = gtk_list_box_new();
    add_class(listbox, "cbh-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(listbox), TRUE);
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_row_activated), NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    add_class(scroll, "cbh-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), listbox);
    gtk_stack_add_named(GTK_STACK(stack), scroll, "list");
    gtk_stack_add_named(GTK_STACK(stack), message_page("Nothing here yet",
        "Copy text or an image (Ctrl+C) and it will show up here.\nPick an item to paste it into the app you are using.",
        NULL, NULL), "empty");
    gtk_stack_add_named(GTK_STACK(stack), message_page("Clipboard history is off",
        "Turn it on to keep a list of what you copy and paste from it any time.",
        "Turn on", G_CALLBACK(on_turn_on)), "off");

    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(win, "focus-in-event", G_CALLBACK(on_focus_in), NULL);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(on_focus_out), NULL);
    g_signal_connect(win, "hide", G_CALLBACK(on_hide), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(sv_panel_press), &geom_dirty);
    g_signal_connect(win, "motion-notify-event", G_CALLBACK(sv_panel_motion), NULL);
    g_signal_connect(win, "configure-event", G_CALLBACK(on_configure), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    gtk_widget_show_all(panel);
    apply_font();
}

/* ------------------------------------------------------------------ application */

static void act_toggle(GSimpleAction *a, GVariant *p, gpointer d) { toggle(); }
static void act_show(GSimpleAction *a, GVariant *p, gpointer d) { show_popup(); }
static void act_clear(GSimpleAction *a, GVariant *p, gpointer d) { clear_all(); }
static void act_quit(GSimpleAction *a, GVariant *p, gpointer d) { g_application_quit(G_APPLICATION(sv_app)); }
static void act_emoji(GSimpleAction *a, GVariant *p, gpointer d) {
    char *argv[] = { "superv", "--toggle", NULL };
    if (emoji_started()) emoji_command(2, argv);
}
static gboolean on_signal(gpointer d) { g_application_quit(G_APPLICATION(sv_app)); return G_SOURCE_REMOVE; }
static void on_shutdown(GApplication *a, gpointer d) {
    history_flush();
    if (emoji_started()) emoji_shutdown();
}

/* After the clipboard part is up (login stays quick): start the emoji panel if it is on, and make the desktop's
 * shortcuts match the switches: on but not registered (first start, or removed since) → register; off but
 * still registered (turned off in config.ini) → give it back. */
static gboolean start_features(gpointer d) {
    if (cfg.emoji_panel) {
        if (!emoji_started()) emoji_start();
    } else if (ini_configured("emoji.ini", "emoji")) {
        char *args[] = { "--feature", "emoji", "--remove-hotkeys", NULL };
        sv_run_helper(args, NULL, NULL);
    }
    for (int f = 0; f < N_FEAT; f++) {
        gboolean reg = ini_configured("shortcuts.ini", FEAT[f].id);
        if (cfg.feat[f] && (!reg || !sv_shortcut_bound(FEAT[f].slug, FEAT[f].name))) feat_shortcut(f, TRUE);
        else if (!cfg.feat[f] && reg) feat_shortcut(f, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static void on_startup(GApplication *a, gpointer d) {
    static const GActionEntry actions[] = {
        { "toggle", act_toggle }, { "show", act_show }, { "clear", act_clear }, { "quit", act_quit },
        { "emoji", act_emoji },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(a), actions, G_N_ELEMENTS(actions), NULL);
    /* history and settings are private: owner-only folders, enforced on every start */
    g_mkdir_with_parents(img_dir, 0700);
    g_mkdir_with_parents(cfg_dir, 0700);
    g_chmod(data_dir, 0700);
    g_chmod(img_dir, 0700);
    g_chmod(cfg_dir, 0700);
    config_load();
    history_load();
    thumbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    paste_init();
    build_window();
    g_unsetenv("GDK_GL");   /* (see main) not for the programs we start */
    clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    prim = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    g_signal_connect(clip, "owner-change", G_CALLBACK(on_owner_change), NULL);
    schedule_read(300);   /* pick up what is already on the clipboard */
    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_application_hold(a);
    watch_for_updates();
    /* First run (e.g. installed from the .deb), or the shortcut was removed since: (re)create it. */
    if (!cfg.setup_done || !sv_shortcut_bound("", "SuperV")) apply_hotkey(cfg.hotkey);
    g_idle_add(start_features, NULL);
    g_timeout_add(1500, warm_up, NULL);
    logmsg("running (%s), history in %s", sv_wayland ? "wayland via XWayland" : "x11", data_dir);
}

static int on_cmdline(GApplication *a, GApplicationCommandLine *cl, gpointer d) {
    int argc;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    const char *m = argc > 1 ? argv[1] : "--toggle";
    if (!strcmp(m, "--quit")) g_application_quit(a);
    else if (!strcmp(m, "--clear")) clear_all();
    else if (!strcmp(m, "--show")) show_popup();
    else if (!strcmp(m, "--emoji")) {   /* Super+. : superv --emoji [--search heart | --tab symbols | --insert TEXT] */
        if (!cfg.emoji_panel) {
            extras_toast("The emoji panel is off", "Turn it on in SuperV's settings: press Super+V, then the gear.");
        } else {
            if (!emoji_started()) emoji_start();
            int x, y;
            if (clipboard_position(&x, &y)) {   /* one panel at a time, in the same spot, like Windows */
                gtk_widget_hide(win);
                if (argc <= 2) emoji_show_at(x, y);
                else emoji_command(argc - 1, argv + 1);
            } else {
                emoji_command(argc - 1, argv + 1);
            }
        }
    }
    else if (!strcmp(m, "--plain-paste") || !strcmp(m, "--pick-color") || !strcmp(m, "--extract-text")
             || !strcmp(m, "--screenshot")) {
        int f = !strcmp(m, "--plain-paste") ? F_PLAIN : !strcmp(m, "--pick-color") ? F_COLOR
              : !strcmp(m, "--extract-text") ? F_TEXT : F_SHOT;
        if (!cfg.feat[f]) {
            char *t = g_strdup_printf("%s is off", FEAT[f].label);
            extras_toast(t, "Turn it on in SuperV's settings: press Super+V, then the gear.");
            g_free(t);
        } else {
            if (gtk_widget_get_visible(win)) gtk_widget_hide(win);
            if (emoji_started() && emoji_visible()) emoji_command(2, (char *[]){ "superv", "--hide", NULL });
            if (f == F_SHOT) shot_start(argc > 3 && !strcmp(argv[2], "--delay") ? atoi(argv[3]) : 0);
            else extras_command(m);
        }
    }
    else if (strcmp(m, "--daemon")) toggle();
    /* after restarting into a new version: keep serving what was on the clipboard */
    if (argc > 2 && g_str_has_prefix(argv[2], "--restore=") && find_item(argv[2] + 10, NULL)) {
        g_free(current_id);
        current_id = g_strdup(argv[2] + 10);
    }
    g_strfreev(argv);
    return 0;
}

/* Run superv-setup with these arguments (NULL-terminated) and wait for it: for --doctor and uninstalling. */
static int run_setup(char **argv) {
    guint n = g_strv_length(argv);
    char **args = g_new0(char *, n + 3);
    args[0] = (char *)python(); args[1] = setup_path();
    for (guint i = 0; i < n; i++) args[i + 2] = argv[i];
    int status = 1 << 8;
    GError *e = NULL;
    if (!g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN, NULL, NULL,
                      NULL, NULL, &status, &e)) {
        fprintf(stderr, "superv: %s\n", e->message);
        g_error_free(e);
    }
    g_free(args[1]); g_free(args);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "";
    sv_self_exe = g_file_read_link("/proc/self/exe", NULL);
    if (!sv_self_exe) sv_self_exe = g_strdup(argv[0]);
    sv_self_dir = g_path_get_dirname(sv_self_exe);
    if (!strcmp(m, "--version")) { puts("superv " VERSION); return 0; }
    if (!strcmp(m, "--quit")) {   /* ask a running SuperV to stop; never start one just to quit */
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        char *path = g_strdup_printf("/%s", APP_ID);
        g_strdelimit(path, ".", '/');
        g_strdelimit(path, "-", '_');
        if (bus) {
            GVariant *r = g_dbus_connection_call_sync(bus, APP_ID, path, "org.gtk.Actions",
                "Activate", g_variant_new_parsed("('quit', @av [], @a{sv} {})"), NULL,
                G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, NULL);
            if (r) g_variant_unref(r);
            g_object_unref(bus);
        }
        g_free(path);
        return 0;
    }
    if (!strcmp(m, "--help") || !strcmp(m, "-h")) {
        puts("SuperV " VERSION " — clipboard history (Win+V), emoji panel (Win+.) and more Windows shortcuts for Linux\n\n"
             "  superv                         open / close the clipboard panel (what Super+V runs)\n"
             "  superv --emoji                 open / close the emoji panel (what Super+. runs)\n"
             "  superv --emoji --tab kaomoji   open it on a tab: emoji, kaomoji or symbols\n"
             "  superv --emoji --search heart  open it with a search\n"
             "  superv --emoji --insert TEXT   type TEXT into the focused app (bind it to a key!)\n"
             "  superv --plain-paste           paste what you copied as plain text (Ctrl+Super+Alt+V)\n"
             "  superv --pick-color            pick a colour on screen, copy its HEX (Super+Shift+C)\n"
             "  superv --extract-text          select an area, copy its text (Super+Shift+T)\n"
             "  superv --screenshot [--delay 3] take a screenshot, draw on it, copy/save/pin (Super+Shift+S)\n"
             "  superv --daemon                start in the background (runs at login)\n"
             "  superv --show                  open the clipboard panel\n"
             "  superv --clear                 clear history (pinned items stay)\n"
             "  superv --quit                  stop SuperV\n"
             "  superv --doctor                check the setup\n\n"
             "Every feature but the clipboard is off until you turn it on: Super+V, then the gear.\n"
             "Settings: ~/.config/superv/config.ini (clipboard, which features are on), emoji.ini (emoji panel),\n"
             "          shortcuts.ini (the features' shortcuts)");
        return 0;
    }
    if (!strcmp(m, "--doctor")) return run_setup((char *[]){ "--doctor", NULL });
    if (!strcmp(m, "--remove-hotkeys")) return run_setup((char *[]){ "--all", "--remove-hotkeys", NULL });   /* uninstalling */
    if (!strcmp(m, "--setup-hotkeys")) return run_setup(argv + 1);
    sv_debug = g_getenv("SUPERV_DEBUG") != NULL;
    sv_wayland = !g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") || g_getenv("WAYLAND_DISPLAY");
    if (!g_getenv("DISPLAY")) { fputs("superv: no X11/XWayland display (DISPLAY not set)\n", stderr); return 1; }
    /* Use X11/XWayland: on GNOME Wayland only X clients may watch the clipboard in the background. */
    gdk_set_allowed_backends("x11");
    /* SuperV draws with cairo only. Without this, GTK loads the OpenGL driver at start just to choose a visual:
     * several MB of memory on X11 desktops for nothing. (Unset again once the display is open.) */
    g_setenv("GDK_GL", "disable", FALSE);
    mallopt(M_ARENA_MAX, 1);   /* one heap for our few threads: freed memory goes back to the system sooner */
    data_dir = g_build_filename(g_get_user_data_dir(), "superv", NULL);
    img_dir = g_build_filename(data_dir, "images", NULL);
    hist_path = g_build_filename(data_dir, "history.ini", NULL);
    cfg_dir = g_build_filename(g_get_user_config_dir(), "superv", NULL);
    cfg_path = g_build_filename(cfg_dir, "config.ini", NULL);
    sv_app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(sv_app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(sv_app, "command-line", G_CALLBACK(on_cmdline), NULL);
    g_signal_connect(sv_app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int status = g_application_run(G_APPLICATION(sv_app), argc, argv);
    g_object_unref(sv_app);
    return status;
}

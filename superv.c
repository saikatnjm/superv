/*
 * SuperV — Windows-style (Win+V) clipboard history for Linux.
 * Ubuntu (GNOME, X11 + Wayland) and Linux Mint (Cinnamon, MATE, Xfce).
 *
 *   Super+V (or your own shortcut)  open / close the panel
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

#define APP_ID  "io.github.superv.SuperV"
#define VERSION "1.2.2"
#define REPO_URL "https://github.com/saikatnjm/superv"
#ifndef SETUP_DIR
#define SETUP_DIR "/usr/lib/superv"   /* where packages put superv-setup (set by the Makefile) */
#endif
#define WIN_W   380
#define WIN_H   500

typedef struct {
    char *id, *hash, *text;          /* text is NULL for images */
    gboolean image, pinned;
    gint64 time;
} Item;

static struct {
    gboolean history_enabled, clear_on_reboot, restore_on_exit;
    int max_items, max_text_bytes, paste_delay_ms, pos_x, pos_y, width, height, font_scale;
    gboolean setup_done;
    char *paste_keys, *hotkey, *theme;
} cfg;

static GPtrArray *items;             /* newest first */
static char *data_dir, *img_dir, *hist_path, *cfg_dir, *cfg_path, *self_exe, *self_dir;
static char *current_id, *paste_err, *image_link, *link_candidate;
static gboolean debug_log, is_wayland, reading, had_focus, capturing, prev_is_x11, geom_dirty, enabling;
static int thumb_w, enable_tries;
static guint relayout_src;
static gint64 ignore_until, hotkey_applied_at;
static guint pending_src;
static int ui_fd = -1;
static int (*xtest_fake)(Display *, unsigned int, int, unsigned long);
static GtkApplication *app;
static GtkClipboard *clip, *prim;
static GtkWidget *win, *listbox, *stack, *clear_btn, *switch_w, *settings_pop, *hotkey_btn, *hint_lbl;
static GtkWidget *hk_status, *banner, *banner_lbl, *banner_btn, *theme_btns[3], *size_lbl;
static GtkCssProvider *font_css;
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
    g_key_file_set_integer(k, "superv", "font_scale", cfg.font_scale);
    g_key_file_set_integer(k, "superv", "width", cfg.width);
    g_key_file_set_integer(k, "superv", "height", cfg.height);
    g_key_file_set_integer(k, "superv", "pos_x", cfg.pos_x);
    g_key_file_set_integer(k, "superv", "pos_y", cfg.pos_y);
    g_key_file_set_boolean(k, "superv", "shortcut_configured", cfg.setup_done);
    g_key_file_set_comment(k, "superv", NULL,
        " SuperV settings. After editing run: superv --quit && superv --daemon &\n"
        " paste_keys: shift+insert (default; works in terminals too), ctrl+v, ctrl+shift+v, none\n"
        " theme: auto (follow the desktop), light, dark      font_scale: text size in % (80-160)\n"
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
    cfg.font_scale = 100;
    cfg.width = WIN_W + 20; cfg.height = WIN_H + 20; cfg.pos_x = cfg.pos_y = -1;
}

static void config_load(void) {
    config_defaults();
    cfg.hotkey = g_strdup(DEFAULT_HOTKEY);
    GKeyFile *k = g_key_file_new();
    if (g_key_file_load_from_file(k, cfg_path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
#define KB(n, f) do { gboolean v = g_key_file_get_boolean(k, "superv", n, &e); if (!e) f = v; g_clear_error(&e); } while (0)
#define KI(n, f) do { int v = g_key_file_get_integer(k, "superv", n, &e); if (!e && v > 0) f = v; g_clear_error(&e); } while (0)
#define KS(n, f) do { char *v = g_key_file_get_string(k, "superv", n, NULL); if (v) { g_strstrip(v); g_free(f); f = v; } } while (0)
        KB("history_enabled", cfg.history_enabled);
        KB("clear_unpinned_on_reboot", cfg.clear_on_reboot);
        KB("restore_when_owner_exits", cfg.restore_on_exit);
        KI("max_items", cfg.max_items);
        KI("max_text_bytes", cfg.max_text_bytes);
        KI("paste_delay_ms", cfg.paste_delay_ms);
        KS("paste_keys", cfg.paste_keys);
        KS("hotkey", cfg.hotkey);
        KS("theme", cfg.theme);
        KI("font_scale", cfg.font_scale);
        KI("width", cfg.width);
        KI("height", cfg.height);
#define KN(n, f) do { int v = g_key_file_get_integer(k, "superv", n, &e); if (!e) f = v; g_clear_error(&e); } while (0)
        KN("pos_x", cfg.pos_x);
        KN("pos_y", cfg.pos_y);
        KB("shortcut_configured", cfg.setup_done);
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

static void item_free(Item *it) { g_free(it->id); g_free(it->hash); g_free(it->text); g_free(it); }

static Item *find_item(const char *id, const char *hash) {
    for (guint i = 0; i < items->len; i++) {
        Item *it = items->pdata[i];
        if ((id && !strcmp(it->id, id)) || (hash && !strcmp(it->hash, hash))) return it;
    }
    return NULL;
}

static void history_save(void) {
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
        if (it->text) g_key_file_set_string(k, it->id, "text", it->text);
    }
    g_key_file_set_string_list(k, "meta", "order", order, items->len);
    gsize len;
    char *data = g_key_file_to_data(k, &len, NULL);
    g_file_set_contents(hist_path, data, len, NULL);
    g_chmod(hist_path, 0600);
    g_free(data); g_free(order); g_free(bid); g_key_file_unref(k);
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
    /* delete images no longer referenced */
    GDir *d = g_dir_open(img_dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        gboolean used = FALSE;
        for (guint i = 0; i < items->len && !used; i++) {
            Item *it = items->pdata[i];
            used = it->image && g_str_has_prefix(name, it->hash);
        }
        if (!used) { char *p = g_build_filename(img_dir, name, NULL); g_unlink(p); g_free(p); }
    }
    if (d) g_dir_close(d);
    history_save();
}

static void drop_item(Item *it) {
    g_ptr_array_remove(items, it);
    if (it->image && !find_item(NULL, it->hash)) { char *p = img_path(it); g_unlink(p); g_free(p); }
    item_free(it);
}

static void move_to_top(Item *it) {
    g_ptr_array_remove(items, it);
    g_ptr_array_insert(items, 0, it);
    it->time = g_get_real_time() / 1000000;
    history_save();
}

static Item *insert_item(Item *it) {
    g_ptr_array_insert(items, 0, it);
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

static Item *add_image(GdkPixbuf *pb) {
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA1);
    int meta[4] = { gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
                    gdk_pixbuf_get_rowstride(pb), gdk_pixbuf_get_has_alpha(pb) };
    g_checksum_update(c, (const guchar *)"i", 1);
    g_checksum_update(c, (const guchar *)meta, sizeof meta);
    g_checksum_update(c, gdk_pixbuf_read_pixels(pb), gdk_pixbuf_get_byte_length(pb));
    char *hash = g_strdup(g_checksum_get_string(c));
    g_checksum_free(c);
    Item *it = find_item(NULL, hash);
    if (it) { g_free(hash); move_to_top(it); return it; }
    it = new_item(hash, TRUE);
    char *p = img_path(it);
    gboolean ok = gdk_pixbuf_save(pb, p, "png", NULL, NULL);
    g_chmod(p, 0600); g_free(p);
    if (!ok) { item_free(it); return NULL; }
    return insert_item(it);
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
    { "v", KEY_V, XK_v },
};

static gboolean uinput_open(void) {
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
static gboolean paste_everywhere(void) { return ui_fd >= 0 || (xtest_fake && !is_wayland); }

static void paste_init(void) {
    void *h = dlopen("libXtst.so.6", RTLD_LAZY);   /* X11 apps, even on Wayland (XWayland) */
    if (h) xtest_fake = dlsym(h, "XTestFakeKeyEvent");
    if (!uinput_open())
        logmsg("paste backend: %s (%s)", xtest_fake ? (is_wayland ? "XTest for X11 apps only" : "XTest") : "none",
               paste_err);
}

static void ui_emit(int type, int code, int value) {
    struct input_event ev = { 0 };
    gettimeofday(&ev.time, NULL);
    ev.type = type; ev.code = code; ev.value = value;
    if (write(ui_fd, &ev, sizeof ev) < 0) logmsg("uinput write failed: %s", g_strerror(errno));
}

static gboolean send_keys(const char *combo) {
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
    if (xtest_fake && (!is_wayland || prev_is_x11)) {
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
        if (debug_log) logmsg("image + link offered: saving the image");
        g_free(image_link);
        image_link = g_strdup(text ? text : "");
        gtk_clipboard_request_image(c, on_image, NULL);
        return;
    }
    reading = FALSE;
    /* When the app that copied an image quits, the desktop may keep only its link: not a new copy. */
    if (link_candidate && text && !strcmp(text, link_candidate)) {
        if (debug_log) logmsg("link of the last image re-offered: ignored");
        g_clear_pointer(&link_candidate, g_free);
        return;
    }
    g_clear_pointer(&link_candidate, g_free);
    if (!text || !*text || g_str_has_prefix(text, "x-special/nautilus-clipboard")) return;
    if (strlen(text) > (size_t)cfg.max_text_bytes) { logmsg("skipped text over max_text_bytes"); return; }
    got_item(add_text(text));
}

static void on_image(GtkClipboard *c, GdkPixbuf *pb, gpointer d) {
    reading = FALSE;
    if (pb) got_item(add_image(pb));
    else if (debug_log) logmsg("image could not be read");
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
    gboolean skip = !cfg.history_enabled;
    GString *names = debug_log ? g_string_new(NULL) : NULL;   /* format names only, never content */
    for (int i = 0; i < n; i++) {
        char *name = gdk_atom_name(atoms[i]);
        skip = skip || !g_strcmp0(name, "x-kde-passwordManagerHint")  /* password managers */
            || !g_strcmp0(name, "x-special/gnome-copied-files")       /* copied files, like Windows */
            || !g_strcmp0(name, "application/x-kde-cutselection");
        if (names) g_string_append_printf(names, "%s%s", i ? " " : "", name);
        g_free(name);
    }
    gboolean has_text = gtk_targets_include_text(atoms, n);
    gboolean has_image = gtk_targets_include_image(atoms, n, FALSE);   /* any format we can read */
    if (names) {
        logmsg("clipboard offers: %s -> %s", names->str,
               skip ? "skipped" : has_text ? (has_image ? "text or image" : "text") : has_image ? "image" : "ignored");
        g_string_free(names, TRUE);
    }
    if (skip) reading = FALSE;
    else if (has_text) gtk_clipboard_request_text(c, on_text, GINT_TO_POINTER(has_image));
    else if (has_image) gtk_clipboard_request_image(c, on_image, NULL);
    else reading = FALSE;
}

static gboolean read_clipboard(gpointer d);

static void schedule_read(guint ms) {
    if (pending_src) g_source_remove(pending_src);
    pending_src = g_timeout_add(ms, read_clipboard, NULL);
}

static gboolean read_clipboard(gpointer d) {
    pending_src = 0;
    if (reading) { schedule_read(60); return G_SOURCE_REMOVE; }
    if (g_get_monotonic_time() < ignore_until) return G_SOURCE_REMOVE;
    reading = TRUE;
    gtk_clipboard_request_targets(clip, on_targets, NULL);
    return G_SOURCE_REMOVE;
}

static void on_owner_change(GtkClipboard *c, GdkEvent *e, gpointer d) { schedule_read(60); }

/* ------------------------------------------------------------------ UI */

static const char *CSS =
    ".cbh-window { background-color: transparent; }"
    ".cbh-panel { border-radius: 12px; padding: 12px 10px 10px 10px; margin: 10px; }"
    ".cbh-nocomp .cbh-panel { margin: 0; border-radius: 0; box-shadow: none; }"
    ".cbh-dark .cbh-panel { background-color: #17151f; border: 1px solid #2f2b3d; color: #ebe8f5;"
    "  box-shadow: 0 6px 18px rgba(0,0,0,0.55); }"
    ".cbh-light .cbh-panel { background-color: #f6f5fa; border: 1px solid #dcd8e8; color: #1d1a26;"
    "  box-shadow: 0 6px 18px rgba(40,20,80,0.18); }"
    ".cbh-title { font-weight: bold; font-size: 1.08em; margin-left: 4px; }"
    ".cbh-sub { font-size: 0.92em; }"
    ".cbh-dark .cbh-sub { color: #a29db4; } .cbh-light .cbh-sub { color: #5f5a70; }"
    ".cbh-list, .cbh-list row, .cbh-scroll, .cbh-scroll viewport { background-color: transparent; background-image: none; }"
    "list.cbh-list row.cbh-row { padding: 0; margin: 0 2px 6px 2px; outline-style: none; border: none; }"
    "list.cbh-list row.cbh-row:selected { background-color: transparent; }"
    ".cbh-card { border-radius: 8px; padding: 10px 4px 10px 12px; border: 1px solid transparent; }"
    ".cbh-dark .cbh-card { background-color: #221f2e; border-color: #2f2b3d; }"
    ".cbh-light .cbh-card { background-color: #ffffff; border-color: #e3e0ee; }"
    ".cbh-dark list.cbh-list row.cbh-row:hover .cbh-card { background-color: #2b2740; }"
    ".cbh-light list.cbh-list row.cbh-row:hover .cbh-card { background-color: #f0ecfb; }"
    ".cbh-dark list.cbh-list row.cbh-row:focus .cbh-card, .cbh-dark list.cbh-list row.cbh-row:selected .cbh-card { border-color: #a78bfa; }"
    ".cbh-light list.cbh-list row.cbh-row:focus .cbh-card, .cbh-light list.cbh-list row.cbh-row:selected .cbh-card { border-color: #7c3aed; }"
    ".cbh-dark .cbh-text { color: #ebe8f5; } .cbh-light .cbh-text { color: #1d1a26; }"
    "button.cbh-flat, button.cbh-icon, button.cbh-menuitem { background-color: transparent; background-image: none;"
    "  border: none; box-shadow: none; text-shadow: none; border-radius: 6px; }"
    "button.cbh-flat { padding: 3px 10px; font-weight: bold; }"
    "button.cbh-icon { padding: 2px; min-width: 26px; min-height: 26px; }"
    "button.cbh-menuitem { padding: 6px 12px; }"
    ".cbh-dark button.cbh-flat { color: #a78bfa; } .cbh-light button.cbh-flat { color: #7c3aed; }"
    ".cbh-dark button.cbh-icon { color: #c9c3dc; } .cbh-light button.cbh-icon { color: #4a4560; }"
    ".cbh-dark button.cbh-flat:hover, .cbh-dark button.cbh-icon:hover { background-color: #2f2a45; }"
    ".cbh-light button.cbh-flat:hover, .cbh-light button.cbh-icon:hover { background-color: #ebe5fb; }"
    ".cbh-dark .cbh-pin { color: #a78bfa; } .cbh-light .cbh-pin { color: #7c3aed; }"
    "button.cbh-primary { background-image: none; border: none; box-shadow: none; text-shadow: none;"
    "  border-radius: 6px; padding: 6px 18px; font-weight: bold; }"
    ".cbh-dark popover.background { background-color: #221f2e; border-color: #3a3550; color: #ebe8f5; }"
    ".cbh-light popover.background { background-color: #ffffff; border-color: #dcd8e8; color: #1d1a26; }"
    ".cbh-dark button.cbh-menuitem:hover { background-color: #2f2a45; }"
    ".cbh-light button.cbh-menuitem:hover { background-color: #ebe5fb; }"
    ".cbh-seg button { padding: 3px 10px; min-height: 0; }"
    ".cbh-size { padding: 0 6px; }"
    ".cbh-dark .cbh-seg button:checked { background-image: none; background-color: #8b5cf6; color: #ffffff; border-color: #7c3aed; }"
    ".cbh-light .cbh-seg button:checked { background-image: none; background-color: #7c3aed; color: #ffffff; border-color: #6d28d9; }"
    ".cbh-settings label, .cbh-settings button { font-size: 13px; }"
    ".cbh-settings button { padding: 2px 9px; min-height: 24px; }"
    ".cbh-settings switch { min-width: 34px; min-height: 18px; padding: 0; margin: 0; border-radius: 9px; }"
    ".cbh-settings switch slider { min-width: 14px; min-height: 14px; margin: 2px; padding: 0;"
    "  border-radius: 7px; border-width: 0; box-shadow: none; }"
    ".cbh-settings .cbh-seg button { padding: 2px 8px; }"
    ".cbh-settings .cbh-status { font-size: 11.5px; }"
    ".cbh-dark .cbh-status { color: #a29db4; } .cbh-light .cbh-status { color: #5f5a70; }"
    ".cbh-dark .cbh-status.cbh-ok { color: #4ade80; } .cbh-light .cbh-status.cbh-ok { color: #15803d; }"
    ".cbh-dark .cbh-status.cbh-err { color: #f87171; } .cbh-light .cbh-status.cbh-err { color: #b91c1c; }"
    ".cbh-keys .cbh-keydesc { font-size: 11px; }"
    ".cbh-dark .cbh-keydesc { color: #a29db4; } .cbh-light .cbh-keydesc { color: #5f5a70; }"
    ".cbh-settings .cbh-kbd { font-family: monospace; font-size: 10px; padding: 0 5px; border-radius: 4px; }"
    ".cbh-dark .cbh-kbd { background-color: #2a2638; border: 1px solid #3f3a55; color: #ebe8f5; }"
    ".cbh-light .cbh-kbd { background-color: #f1eef9; border: 1px solid #d6d0e8; color: #1d1a26; }"
    ".cbh-settings .cbh-footbtn, .cbh-settings .cbh-footbtn label, .cbh-settings .cbh-ver { font-size: 11px; }"
    ".cbh-settings button.cbh-footbtn { padding: 1px 4px; min-height: 0; background-color: transparent;"
    "  background-image: none; border: none; box-shadow: none; text-shadow: none; border-radius: 5px; }"
    ".cbh-dark .cbh-footbtn, .cbh-dark .cbh-ver { color: #8a84a0; }"
    ".cbh-light .cbh-footbtn, .cbh-light .cbh-ver { color: #6f6a82; }"
    ".cbh-dark button.cbh-footbtn:hover, .cbh-dark button.cbh-footbtn:checked { color: #a78bfa; background-color: #2f2a45; }"
    ".cbh-light button.cbh-footbtn:hover, .cbh-light button.cbh-footbtn:checked { color: #7c3aed; background-color: #ebe5fb; }"
    ".cbh-banner { border-radius: 8px; padding: 8px 8px 8px 12px; }"
    ".cbh-dark .cbh-banner { background-color: #2a2342; border: 1px solid #4c3d7a; }"
    ".cbh-light .cbh-banner { background-color: #f1ebff; border: 1px solid #d9ccfb; }"
    ".cbh-dark switch:checked { background-color: #8b5cf6; border-color: #7c3aed; }"
    ".cbh-light switch:checked { background-color: #7c3aed; border-color: #6d28d9; }"
    ".cbh-dark button.cbh-primary { background-color: #a78bfa; color: #17151f; }"
    ".cbh-light button.cbh-primary { background-color: #7c3aed; color: #ffffff; }";

static gboolean theme_is_dark(void) {
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

static GtkWidget *icon(const char *a, const char *b, const char *fallback) {
    GtkIconTheme *t = gtk_icon_theme_get_default();
    if (gtk_icon_theme_has_icon(t, a)) return gtk_image_new_from_icon_name(a, GTK_ICON_SIZE_MENU);
    if (b && gtk_icon_theme_has_icon(t, b)) return gtk_image_new_from_icon_name(b, GTK_ICON_SIZE_MENU);
    return gtk_label_new(fallback);
}

static void add_class(GtkWidget *w, const char *c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }

static int focused_index(void) {
    GtkWidget *w = gtk_window_get_focus(GTK_WINDOW(win));
    while (w && !GTK_IS_LIST_BOX_ROW(w)) w = gtk_widget_get_parent(w);
    if (!w) w = GTK_WIDGET(gtk_list_box_get_selected_row(GTK_LIST_BOX(listbox)));
    return w ? gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(w)) : -1;
}

static gboolean any_menu_open(void) {
    if (gtk_widget_get_visible(settings_pop)) return TRUE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(listbox));
    gboolean open = FALSE;
    for (GList *l = rows; l && !open; l = l->next) {
        GtkWidget *p = g_object_get_data(l->data, "pop");
        open = p && gtk_widget_get_visible(p);
    }
    g_list_free(rows);
    return open;
}

/* actions */
static void paste_item(Item *it);

static gboolean do_paste(gpointer d) {
    if (!send_keys(cfg.paste_keys)) {
        GNotification *n = g_notification_new("Copied — press Ctrl+V to paste");
        char *b = g_strdup("Click-to-paste needs a one-time permission: open SuperV and click Enable.");
        g_notification_set_body(n, b);
        g_application_send_notification(G_APPLICATION(app), "paste", n);
        g_object_unref(n); g_free(b);
    }
    return G_SOURCE_REMOVE;
}

static void paste_item(Item *it) {
    set_clipboard(it, TRUE);   /* PRIMARY too, so Shift+Insert also works in terminals */
    move_to_top(it);
    gtk_widget_hide(win);
    if (g_strcmp0(cfg.paste_keys, "none")) g_timeout_add(cfg.paste_delay_ms, do_paste, NULL);
}

static void clear_all(void) { clear_unpinned(); if (win) refresh(0); }

static void reload_hotkey(void);

static void set_history_enabled(gboolean on) {
    if (cfg.history_enabled != on) {
        reload_hotkey();
        cfg.history_enabled = on;
        config_save();
        if (!on) clear_unpinned();   /* Windows also wipes history when it is turned off */
    }
    refresh(0);
}

static int index_of(const char *id) {
    for (guint i = 0; id && i < items->len; i++)
        if (!strcmp(((Item *)items->pdata[i])->id, id)) return (int)i;
    return 0;
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
typedef struct { int act; char *id; int idx; } Pending;

static gboolean run_action(gpointer data) {
    Pending *p = data;
    Item *it = find_item(p->id, NULL);
    if (p->act == ACT_CLEAR) clear_all();
    else if (it) {
        int idx = index_of(p->id);   /* the row whose ⋯ was clicked */
        if (p->act == ACT_DELETE) { drop_item(it); history_save(); }
        else { it->pinned = !it->pinned; history_save(); }
        refresh(idx);
    }
    g_free(p->id); g_free(p);
    return G_SOURCE_REMOVE;
}

static void on_menu_item(GtkButton *b, gpointer act) {
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    Pending *p = g_new0(Pending, 1);
    p->act = GPOINTER_TO_INT(act);
    p->id = g_strdup(g_object_get_data(G_OBJECT(b), "id"));
    p->idx = focused_index();
    g_idle_add(run_action, p);   /* the row (and this popover) gets rebuilt */
}

static GtkWidget *menu_item(GtkWidget *box, const char *ic1, const char *ic2, const char *fb,
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
    return b;
}

static GdkPixbuf *thumbnail(Item *it) {
    GdkPixbuf *pb = g_hash_table_lookup(thumbs, it->id);
    if (!pb) {
        char *p = img_path(it);
        pb = gdk_pixbuf_new_from_file_at_scale(p, thumb_w, MAX(150, thumb_w / 2), TRUE, NULL);
        g_free(p);
        if (pb) g_hash_table_insert(thumbs, g_strdup(it->id), pb);
    }
    return pb;
}

static GtkWidget *make_row(Item *it) {
    GtkWidget *row = gtk_list_box_row_new(), *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *content;
    add_class(row, "cbh-row");
    add_class(card, "cbh-card");
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
    }
    gtk_box_pack_start(GTK_BOX(card), content, TRUE, TRUE, 0);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *more = gtk_menu_button_new();
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(more));
    if (old) gtk_container_remove(GTK_CONTAINER(more), old);
    gtk_container_add(GTK_CONTAINER(more), icon("view-more-horizontal-symbolic", "view-more-symbolic", "⋯"));
    add_class(more, "cbh-icon");
    gtk_widget_set_tooltip_text(more, "See more");
    gtk_widget_set_valign(more, GTK_ALIGN_START);
    GtkWidget *pop = gtk_popover_new(more), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    menu_item(box, "user-trash-symbolic", "edit-delete-symbolic", "🗑", "Delete", ACT_DELETE, it->id);
    menu_item(box, "view-pin-symbolic", NULL, "📌", it->pinned ? "Unpin" : "Pin", ACT_PIN, it->id);
    menu_item(box, "edit-clear-all-symbolic", "edit-clear-symbolic", "✕", "Clear all", ACT_CLEAR, it->id);
    gtk_widget_show_all(box);
    gtk_container_add(GTK_CONTAINER(pop), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(more), pop);
    g_object_set_data(G_OBJECT(row), "pop", pop);
    g_signal_connect(pop, "closed", G_CALLBACK(on_menu_closed), NULL);
    gtk_box_pack_start(GTK_BOX(side), more, FALSE, FALSE, 0);
    if (it->pinned) {
        GtkWidget *pin = icon("view-pin-symbolic", NULL, "📌");
        add_class(pin, "cbh-pin");
        gtk_widget_set_tooltip_text(pin, "Pinned");
        gtk_box_pack_start(GTK_BOX(side), pin, FALSE, FALSE, 2);
    }
    gtk_box_pack_start(GTK_BOX(card), side, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), card);
    gtk_widget_show_all(row);
    return row;
}

static void update_hotkey_labels(void) {
    guint key; GdkModifierType mods;
    gtk_accelerator_parse(cfg.hotkey, &key, &mods);
    char *label = key ? gtk_accelerator_get_label(key, mods) : g_strdup(cfg.hotkey);
    if (!capturing) gtk_button_set_label(GTK_BUTTON(hotkey_btn), label);
    gtk_label_set_text(GTK_LABEL(hint_lbl), label);
    g_free(label);
}

enum { BANNER_ASK, BANNER_FAILED, BANNER_RELOGIN };
static int banner_state = BANNER_ASK;

static void update_banner(void) {
    gboolean show = !paste_everywhere() && g_strcmp0(cfg.paste_keys, "none");
    gtk_widget_set_visible(banner, show);
    if (!show || enabling) return;
    switch (banner_state) {
    case BANNER_RELOGIN:
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Permission added. Log out and back in once to finish.");
        gtk_widget_set_visible(banner_btn, FALSE);
        return;
    case BANNER_FAILED:
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Not enabled: the password prompt was cancelled.");
        gtk_button_set_label(GTK_BUTTON(banner_btn), "Try again");
        break;
    default:
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Click-to-paste needs a one-time permission. "
                           "Until then, picking an item copies it — press Ctrl+V.");
        gtk_button_set_label(GTK_BUTTON(banner_btn), "Enable");
    }
    gtk_widget_set_visible(banner_btn, TRUE);
    gtk_widget_set_sensitive(banner_btn, TRUE);
}

static void refresh(int keep) {
    int w = gtk_widget_get_visible(win) ? gtk_widget_get_allocated_width(win) : cfg.width;
    int tw = CLAMP(w - 110, 120, 1600);
    if (tw != thumb_w) { thumb_w = tw; g_hash_table_remove_all(thumbs); }
    update_banner();
    GList *rows = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = rows; l; l = l->next) {
        GtkWidget *pop = g_object_get_data(l->data, "pop");
        if (pop) gtk_widget_destroy(pop);
        gtk_widget_destroy(l->data);
    }
    g_list_free(rows);
    GHashTableIter hi; gpointer key;
    g_hash_table_iter_init(&hi, thumbs);
    while (g_hash_table_iter_next(&hi, &key, NULL)) if (!find_item(key, NULL)) g_hash_table_iter_remove(&hi);
    gboolean any_unpinned = FALSE;
    for (guint i = 0; i < items->len; i++) {
        Item *it = items->pdata[i];
        any_unpinned |= !it->pinned;
        gtk_container_add(GTK_CONTAINER(listbox), make_row(it));
    }
    syncing = TRUE;
    gtk_switch_set_active(GTK_SWITCH(switch_w), cfg.history_enabled);
    const char *themes[] = { "auto", "light", "dark" };
    for (int i = 0; i < 3; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(theme_btns[i]), !g_strcmp0(cfg.theme, themes[i]));
    syncing = FALSE;
    gtk_widget_set_visible(clear_btn, cfg.history_enabled && any_unpinned);
    update_hotkey_labels();
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

static void place_window(void) {
    GdkDisplay *disp = gdk_display_get_default();
    int px = 0, py = 0, W = cfg.width, H = cfg.height, x, y;
    GdkRectangle a;
    gtk_window_resize(GTK_WINDOW(win), W, H);
    if (cfg.pos_x >= 0 && cfg.pos_y >= 0) {   /* where you dragged it last time */
        gdk_monitor_get_workarea(gdk_display_get_monitor_at_point(disp, cfg.pos_x + W / 2, cfg.pos_y + 20), &a);
        x = CLAMP(cfg.pos_x, a.x, MAX(a.x, a.x + a.width - W));
        y = CLAMP(cfg.pos_y, a.y, MAX(a.y, a.y + a.height - H));
        gtk_window_move(GTK_WINDOW(win), x, y);
        return;
    }
    gdk_device_get_position(gdk_seat_get_pointer(gdk_display_get_default_seat(disp)), NULL, &px, &py);
    GdkMonitor *m = gdk_display_get_monitor_at_point(disp, px, py);
    if (!m) m = gdk_display_get_primary_monitor(disp);
    if (!m) m = gdk_display_get_monitor(disp, 0);
    gdk_monitor_get_workarea(m, &a);
    if (is_wayland) {   /* XWayland can't see the pointer over Wayland windows: centre on screen */
        x = a.x + (a.width - W) / 2;
        y = a.y + (a.height - H) / 2;
    } else {            /* like Windows: open next to where you are working */
        x = CLAMP(px - W / 2, a.x, a.x + a.width - W);
        y = (py + 16 + H <= a.y + a.height) ? py + 16 : py - H - 16;
        y = CLAMP(y, a.y, a.y + a.height - H);
    }
    gtk_window_move(GTK_WINDOW(win), x, y);
}

/* ---- move / resize with the mouse (the panel has no title bar) */
static GdkWindowEdge edge_at(double xr, double yr, gboolean *hit) {
    int ox, oy, z = gtk_style_context_has_class(gtk_widget_get_style_context(win), "cbh-nocomp") ? 8 : 16;
    gdk_window_get_origin(gtk_widget_get_window(win), &ox, &oy);
    int x = (int)xr - ox, y = (int)yr - oy;
    int w = gtk_widget_get_allocated_width(win), h = gtk_widget_get_allocated_height(win);
    gboolean l = x < z, r = x >= w - z, t = y < z, b = y >= h - z;
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

static gboolean on_win_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    gboolean hit;
    GdkWindowEdge edge = edge_at(e->x_root, e->y_root, &hit);
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS || !hit) return FALSE;
    geom_dirty = TRUE;
    gtk_window_begin_resize_drag(GTK_WINDOW(win), edge, e->button, (int)e->x_root, (int)e->y_root, e->time);
    return TRUE;
}

static gboolean on_win_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    static const char *names[] = { "nw-resize", "n-resize", "ne-resize", "w-resize",
                                   "e-resize", "sw-resize", "s-resize", "se-resize" };
    static int last = -2;
    gboolean hit;
    GdkWindowEdge edge = edge_at(e->x_root, e->y_root, &hit);
    int cur = hit ? (int)edge : -1;
    if (cur != last) {
        last = cur;
        GdkCursor *c = hit ? gdk_cursor_new_from_name(gtk_widget_get_display(win), names[edge]) : NULL;
        gdk_window_set_cursor(gtk_widget_get_window(win), c);
        if (c) g_object_unref(c);
    }
    return FALSE;
}

static gboolean on_header_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    gboolean hit;
    edge_at(e->x_root, e->y_root, &hit);
    if (e->button != 1 || hit) return FALSE;
    if (e->type == GDK_2BUTTON_PRESS) {   /* double-click the header: back to default size + place */
        reload_hotkey();
        cfg.pos_x = cfg.pos_y = -1; cfg.width = WIN_W + 20; cfg.height = WIN_H + 20;
        config_save();
        place_window();
        geom_dirty = FALSE;
        return TRUE;
    }
    if (e->type != GDK_BUTTON_PRESS) return FALSE;
    if (debug_log) logmsg("header pressed: moving the panel");
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
    if (resized) {   /* re-make thumbnails for the new width, once the drag settles */
        if (relayout_src) g_source_remove(relayout_src);
        relayout_src = g_timeout_add(250, relayout, NULL);
    }
    return FALSE;
}

static void apply_theme(void) {
    GtkStyleContext *c = gtk_widget_get_style_context(win);
    gboolean dark = theme_is_dark();
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

static char *desktop_hotkey(void);
static void hotkey_status(const char *text, gboolean ok);

/* Show the shortcut the desktop really uses: people also change it in their desktop's settings. */
static void sync_hotkey_from_desktop(void) {
    if (g_get_monotonic_time() - hotkey_applied_at < 5 * G_USEC_PER_SEC) return;   /* our own change is still being applied */
    char *d = desktop_hotkey();
    if (d && g_strcmp0(d, cfg.hotkey)) {
        g_free(cfg.hotkey);
        cfg.hotkey = d;
        config_save();
    } else {
        g_free(d);
    }
}

static void show_popup(void) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkWindow *aw = gdk_screen_get_active_window(gdk_screen_get_default());
    G_GNUC_END_IGNORE_DEPRECATIONS
    /* on Wayland, _NET_ACTIVE_WINDOW is only set when an X11 (XWayland) app has focus */
    prev_is_x11 = aw && (!gtk_widget_get_realized(win) || aw != gtk_widget_get_window(win));
    if (aw) g_object_unref(aw);
    reload_hotkey();
    sync_hotkey_from_desktop();
    apply_theme();
    capturing = FALSE;
    hotkey_status(NULL, TRUE);
    refresh(0);
    had_focus = FALSE;
    place_window();
    gtk_widget_show(win);
    GdkWindow *gw = gtk_widget_get_window(win);
    guint32 ts = GDK_IS_X11_WINDOW(gw) ? gdk_x11_get_server_time(gw) : GDK_CURRENT_TIME;
    gtk_window_present_with_time(GTK_WINDOW(win), ts);
    g_timeout_add(60, ensure_focus, GUINT_TO_POINTER(ts));
    g_timeout_add(300, ensure_focus, GUINT_TO_POINTER(ts));
    g_timeout_add(700, ensure_focus, GUINT_TO_POINTER(ts));
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(listbox), 0);
    if (row) gtk_widget_grab_focus(GTK_WIDGET(row));
}

static void toggle(void) { if (gtk_widget_get_visible(win)) gtk_widget_hide(win); else show_popup(); }

static char *setup_path(void) {
    const char *dirs[] = { self_dir, SETUP_DIR, "/usr/lib/superv", "/usr/local/lib/superv", "/usr/libexec/superv" };
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        char *p = g_build_filename(dirs[i], "superv-setup", NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
        g_free(p);
    }
    return g_build_filename(SETUP_DIR, "superv-setup", NULL);
}

static void hotkey_status(const char *text, gboolean ok) {
    if (!hk_status) return;
    GtkStyleContext *c = gtk_widget_get_style_context(hk_status);
    gtk_style_context_remove_class(c, ok ? "cbh-err" : "cbh-ok");
    gtk_style_context_add_class(c, ok ? "cbh-ok" : "cbh-err");
    gtk_label_set_text(GTK_LABEL(hk_status), text);
    gtk_widget_set_visible(hk_status, text != NULL);
}

static char *accel_label(const char *accel) {
    guint key; GdkModifierType mods;
    gtk_accelerator_parse(accel, &key, &mods);
    return key ? gtk_accelerator_get_label(key, mods) : g_strdup(accel);
}

static void spawn_setup_helper(const char *accel);
static gboolean helper_running;
static char *queued_accel;

static void on_hotkey_applied(GPid pid, gint status, gpointer accel) {
    g_spawn_close_pid(pid);
    helper_running = FALSE;
    if (queued_accel) {   /* a newer shortcut was chosen meanwhile: apply that one */
        char *next = queued_accel;
        queued_accel = NULL;
        spawn_setup_helper(next);
        g_free(next); g_free(accel);
        return;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char *label = accel_label(accel), *msg = g_strdup_printf("✓ Shortcut set to %s", label);
        hotkey_status(msg, TRUE);
        g_free(msg); g_free(label);
    } else {
        hotkey_status("Couldn't save the shortcut. Run “superv --doctor” to see why.", FALSE);
    }
    g_free(accel);
}

/* custom shortcut: press the new keys while the button says "Press keys…" */
static void apply_hotkey(const char *new_accel) {
    char *accel = g_strdup(new_accel);   /* new_accel may be cfg.hotkey itself */
    g_free(cfg.hotkey);
    cfg.hotkey = accel;
    cfg.setup_done = TRUE;
    hotkey_applied_at = g_get_monotonic_time();
    config_save();
    if (helper_running) {   /* finish the running one first, then apply the latest choice */
        g_free(queued_accel);
        queued_accel = g_strdup(accel);
        return;
    }
    spawn_setup_helper(accel);
}

static void spawn_setup_helper(const char *accel) {
    char *setup = setup_path();
    char *quoted = g_shell_quote(self_exe), *cmd = g_strdup_printf("%s --toggle", quoted);
    char *argv[] = { "python3", setup, "--setup-hotkeys", cmd, "--hotkey", (char *)accel, NULL };
    GError *e = NULL;
    GPid pid;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &e)) {
        helper_running = TRUE;
        g_child_watch_add(pid, on_hotkey_applied, g_strdup(accel));
    } else {
        logmsg("could not run %s: %s", setup, e->message);
        hotkey_status("Couldn't save the shortcut: the SuperV setup helper is missing.", FALSE);
        g_error_free(e);
    }
    g_free(setup); g_free(quoted); g_free(cmd);
}

static gboolean strv_setting_has(const char *schema_id, const char *key, const char *value, gboolean *known) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, schema_id, TRUE) : NULL;
    gboolean found = FALSE;
    if (schema && g_settings_schema_has_key(schema, key)) {
        GSettings *s = g_settings_new(schema_id);
        char **v = g_settings_get_strv(s, key);
        *known = TRUE;
        for (int i = 0; v[i] && !found; i++) {
            if (!value) {   /* Cinnamon: look for a custom binding named SuperV */
                char *path = g_strdup_printf("/org/cinnamon/desktop/keybindings/custom-keybindings/%s/", v[i]);
                GSettings *kb = g_settings_new_with_path("org.cinnamon.desktop.keybindings.custom-keybinding", path);
                char *name = g_settings_get_string(kb, "name");
                found = !g_strcmp0(name, "SuperV");
                g_free(name); g_object_unref(kb); g_free(path);
            } else {
                found = !g_strcmp0(v[i], value);
            }
        }
        g_strfreev(v); g_object_unref(s);
    }
    if (schema) g_settings_schema_unref(schema);
    return found;
}

#define GNOME_KB_SCHEMA "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding"
#define GNOME_KB_PATH   "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/superv/"
#define CIN_KB_SCHEMA   "org.cinnamon.desktop.keybindings.custom-keybinding"

static gboolean schema_has_key(const char *schema_id, const char *key) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, schema_id, TRUE) : NULL;
    gboolean ok = schema && g_settings_schema_has_key(schema, key);
    if (schema) g_settings_schema_unref(schema);
    return ok;
}

static char *gnome_hotkey(void) {
    gboolean known = FALSE;
    if (!strv_setting_has("org.gnome.settings-daemon.plugins.media-keys", "custom-keybindings", GNOME_KB_PATH, &known))
        return NULL;
    GSettings *kb = g_settings_new_with_path(GNOME_KB_SCHEMA, GNOME_KB_PATH);
    char *b = g_settings_get_string(kb, "binding");
    g_object_unref(kb);
    if (!*b) { g_free(b); return NULL; }
    return b;
}

static char *cinnamon_hotkey(void) {
    if (!schema_has_key("org.cinnamon.desktop.keybindings", "custom-list")) return NULL;
    GSettings *s = g_settings_new("org.cinnamon.desktop.keybindings");
    char **ids = g_settings_get_strv(s, "custom-list"), *found = NULL;
    for (int i = 0; ids[i] && !found; i++) {
        char *path = g_strdup_printf("/org/cinnamon/desktop/keybindings/custom-keybindings/%s/", ids[i]);
        GSettings *kb = g_settings_new_with_path(CIN_KB_SCHEMA, path);
        char *name = g_settings_get_string(kb, "name");
        if (!g_strcmp0(name, "SuperV")) {
            GVariant *v = g_settings_get_value(kb, "binding");
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
                const char **b = g_variant_get_strv(v, NULL);
                if (b[0] && *b[0]) found = g_strdup(b[0]);
                g_free(b);
            } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) && *g_variant_get_string(v, NULL)) {
                found = g_variant_dup_string(v, NULL);
            }
            g_variant_unref(v);
        }
        g_free(name); g_object_unref(kb); g_free(path);
    }
    g_strfreev(ids); g_object_unref(s);
    return found;
}

/* Which shortcut system the running desktop uses (we can only read GNOME's and Cinnamon's). */
enum { DE_OTHER, DE_GNOME, DE_CINNAMON };
static int desktop_kind(void) {
    char *de = g_ascii_strup(g_getenv("XDG_CURRENT_DESKTOP") ? g_getenv("XDG_CURRENT_DESKTOP") : "", -1);
    int kind = strstr(de, "CINNAMON") ? DE_CINNAMON
             : (strstr(de, "GNOME") || strstr(de, "UNITY") || strstr(de, "BUDGIE") || strstr(de, "PANTHEON"))
               ? DE_GNOME : DE_OTHER;
    g_free(de);
    return kind;
}

/* The shortcut as the running desktop sees it (NULL if we can't tell, e.g. KDE/MATE/Xfce). */
static char *desktop_hotkey(void) {
    switch (desktop_kind()) {
    case DE_GNOME:    return gnome_hotkey();
    case DE_CINNAMON: return cinnamon_hotkey();
    default:          return NULL;
    }
}

/* Is SuperV's shortcut actually registered with the desktop? (TRUE when we can't tell, e.g. Xfce) */
static gboolean shortcut_bound(void) {
    gboolean known = FALSE, found;
    switch (desktop_kind()) {
    case DE_GNOME:
        found = strv_setting_has("org.gnome.settings-daemon.plugins.media-keys", "custom-keybindings",
                                 GNOME_KB_PATH, &known);
        break;
    case DE_CINNAMON:
        found = strv_setting_has("org.cinnamon.desktop.keybindings", "custom-list", NULL, &known);
        break;
    default:
        return TRUE;   /* KDE, MATE, Xfce, tiling WMs: trust the saved setting */
    }
    return found || !known;
}

static void on_hotkey_btn(GtkButton *b, gpointer d) {
    capturing = TRUE;
    gtk_button_set_label(b, "Press keys…");
    hotkey_status("Press the new shortcut, e.g. Super+V or Ctrl+Alt+V. Esc cancels.", TRUE);
    gtk_style_context_remove_class(gtk_widget_get_style_context(hk_status), "cbh-ok");
}

static gboolean capture_key(GdkEventKey *e) {
    if (e->is_modifier) return TRUE;
    if (e->keyval == GDK_KEY_Escape) { capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels(); return TRUE; }
    GdkModifierType state = e->state;
    gdk_keymap_add_virtual_modifiers(gdk_keymap_get_for_display(gdk_display_get_default()), &state);
    state &= gtk_accelerator_get_default_mod_mask() & ~(GDK_HYPER_MASK | GDK_META_MASK);
    guint key = gdk_keyval_to_lower(e->keyval);
    /* Must use Super, or Ctrl+Alt: Ctrl+V, Ctrl+Shift+V, Alt+V… belong to apps (paste, menus). */
    gboolean ctrl = state & GDK_CONTROL_MASK, alt = state & GDK_MOD1_MASK, super = state & GDK_SUPER_MASK;
    if (!super && !(ctrl && alt)) {
        char *name = gtk_accelerator_name(key, state), *label = accel_label(name), *msg;
        g_free(name);
        if (ctrl)
            msg = g_strdup_printf("%s isn't allowed: apps use Ctrl shortcuts (copy, paste, undo…). "
                                  "Use Super or Ctrl+Alt with a key.", label);
        else if (alt)
            msg = g_strdup_printf("%s isn't allowed: apps use Alt shortcuts for their menus. "
                                  "Use Super or Ctrl+Alt with a key.", label);
        else
            msg = g_strdup_printf("%s isn't allowed: a shortcut needs Super, or Ctrl+Alt, plus a key.", label);
        hotkey_status(msg, FALSE);
        g_free(msg); g_free(label);
        return TRUE;   /* keep listening for a valid combination */
    }
    capturing = FALSE;
    hotkey_status("Saving…", TRUE);
    gtk_style_context_remove_class(gtk_widget_get_style_context(hk_status), "cbh-ok");
    char *accel = gtk_accelerator_name(key, state);
    apply_hotkey(accel);
    g_free(accel);
    update_hotkey_labels();
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
    GList *rows = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = rows; l; l = l->next) {
        GtkWidget *p = g_object_get_data(l->data, "pop");
        if (p) gtk_widget_hide(p);
    }
    g_list_free(rows);
}

/* Like Windows: a click anywhere outside closes the panel, menus included. */
static gboolean hide_if_inactive(gpointer d) {
    if (gtk_widget_get_visible(win) && !gtk_window_is_active(GTK_WINDOW(win))) {
        if (debug_log) logmsg("panel lost focus: closing");
        if (capturing) { capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels(); }
        close_menus();
        gtk_widget_hide(win);
    }
    return G_SOURCE_REMOVE;
}

static gboolean trim_memory(gpointer d) { malloc_trim(0); return G_SOURCE_REMOVE; }
/* ---- updates: when a package replaces our program file, restart into the new version.
 * Waits until the panel is closed; keeps what's on the clipboard. */
static guint restart_src;
static gboolean restart_pending;
static GFileMonitor *exe_monitor;

static gboolean restart_now(gpointer d) {
    restart_src = 0;
    if (gtk_widget_get_visible(win)) { restart_pending = TRUE; return G_SOURCE_REMOVE; }
    restart_pending = FALSE;
    if (!g_file_test(self_exe, G_FILE_TEST_IS_EXECUTABLE)) return G_SOURCE_REMOVE;   /* removed, not updated */
    logmsg("SuperV was updated: restarting into the new version");
    if (geom_dirty) { geom_dirty = FALSE; reload_hotkey(); config_save(); }
    history_save();
    char *restore = current_id ? g_strdup_printf("--restore=%s", current_id) : NULL;
    char *argv[] = { self_exe, "--daemon", restore, NULL };
    GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(app));
    if (bus) g_dbus_connection_close_sync(bus, NULL, NULL);   /* free our D-Bus name for the new process */
    g_usleep(200000);
    execv(self_exe, argv);
    logmsg("restart failed (%s); still running the old version", g_strerror(errno));
    g_free(restore);
    return G_SOURCE_REMOVE;
}

static void on_exe_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer d) {
    if (ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_MOVED_OUT) return;
    if (restart_src) g_source_remove(restart_src);
    restart_src = g_timeout_add_seconds(3, restart_now, NULL);   /* let the package manager finish */
}

static void watch_for_updates(void) {
    GFile *f = g_file_new_for_path(self_exe);
    exe_monitor = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (exe_monitor) g_signal_connect(exe_monitor, "changed", G_CALLBACK(on_exe_changed), NULL);
    g_object_unref(f);
}

static void on_hide(GtkWidget *w, gpointer d) {
    if (geom_dirty) { geom_dirty = FALSE; reload_hotkey(); config_save(); }
    g_timeout_add(500, trim_memory, NULL);
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

static void on_theme_toggled(GtkToggleButton *b, gpointer which) {
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    g_free(cfg.theme);
    cfg.theme = g_strdup(which);
    reload_hotkey();
    config_save();
    apply_theme();
}

static void on_font_step(GtkButton *b, gpointer delta) {
    cfg.font_scale += GPOINTER_TO_INT(delta);
    apply_font();
    reload_hotkey();
    config_save();
    g_hash_table_remove_all(thumbs);
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
    reload_hotkey();
    g_free(cfg.theme); cfg.theme = g_strdup("auto");
    cfg.font_scale = 100;
    cfg.width = WIN_W + 20; cfg.height = WIN_H + 20; cfg.pos_x = cfg.pos_y = -1;
    apply_font();
    apply_theme();
    if (g_strcmp0(cfg.hotkey, DEFAULT_HOTKEY)) apply_hotkey(DEFAULT_HOTKEY);   /* also saves */
    else config_save();
    place_window();
    geom_dirty = FALSE;   /* the resulting move/resize must not be saved as "user placed it here" */
    g_hash_table_remove_all(thumbs);
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

static void fit_settings(GtkWidget *pop, gpointer scroll) {
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

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16), *l = gtk_label_new("Clipboard history");
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

    settings_row(box, "Theme", theme_control());
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
    g_signal_connect(settings_pop, "show", G_CALLBACK(fit_settings), scroll);
    g_signal_connect(settings_pop, "closed", G_CALLBACK(on_menu_closed), NULL);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), settings_pop);
    return btn;
}

/* One-time permission for click-to-paste on Wayland, with the desktop's own password dialog. */
static void notify(const char *title, const char *body) {
    GNotification *n = g_notification_new(title);
    g_notification_set_body(n, body);
    g_application_send_notification(G_APPLICATION(app), "superv", n);
    g_object_unref(n);
}

static gboolean retry_uinput(gpointer d) {
    if (uinput_open()) {
        enabling = FALSE;
        update_banner();
        notify("Click-to-paste is on", "Pick an item in SuperV and it's pasted where you were typing.");
        return G_SOURCE_REMOVE;
    }
    if (++enable_tries < 12) return G_SOURCE_CONTINUE;
    enabling = FALSE;
    banner_state = BANNER_RELOGIN;
    update_banner();
    return G_SOURCE_REMOVE;
}

static void on_enable_done(GPid pid, gint status, gpointer d) {
    g_spawn_close_pid(pid);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        enable_tries = 0;
        g_timeout_add(500, retry_uinput, NULL);
    } else {
        enabling = FALSE;
        banner_state = BANNER_FAILED;
        update_banner();
    }
}

static void on_enable_paste(GtkButton *b, gpointer d) {
    static const char *script =
        "set -e; printf '%s\\n' 'KERNEL==\"uinput\", SUBSYSTEM==\"misc\", OPTIONS+=\"static_node=uinput\", TAG+=\"uaccess\"'"
        " > /etc/udev/rules.d/70-superv-uinput.rules;"
        " echo uinput > /etc/modules-load.d/superv-uinput.conf;"
        " modprobe uinput; udevadm control --reload-rules;"
        " udevadm trigger --action=change --sysname-match=uinput; udevadm settle";
    char *argv[] = { "pkexec", "/bin/sh", "-c", (char *)script, NULL };
    GPid pid;
    GError *e = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &e)) {
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Couldn't open the password prompt. Run install.sh in a terminal instead.");
        g_error_free(e);
        return;
    }
    enabling = TRUE;
    gtk_widget_set_sensitive(banner_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(banner_lbl), "Enter your password in the dialog to turn on click-to-paste…");
    g_child_watch_add(pid, on_enable_done, NULL);
}

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
    g_signal_connect(win, "button-press-event", G_CALLBACK(on_win_press), NULL);
    g_signal_connect(win, "motion-notify-event", G_CALLBACK(on_win_motion), NULL);
    g_signal_connect(win, "configure-event", G_CALLBACK(on_configure), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    gtk_widget_show_all(panel);
    apply_font();
}

/* ------------------------------------------------------------------ application */

static void act_toggle(GSimpleAction *a, GVariant *p, gpointer d) { toggle(); }
static void act_show(GSimpleAction *a, GVariant *p, gpointer d) { show_popup(); }
static void act_clear(GSimpleAction *a, GVariant *p, gpointer d) { clear_all(); }
static void act_quit(GSimpleAction *a, GVariant *p, gpointer d) { g_application_quit(G_APPLICATION(app)); }
static gboolean on_signal(gpointer d) { g_application_quit(G_APPLICATION(app)); return G_SOURCE_REMOVE; }

static void on_startup(GApplication *a, gpointer d) {
    static const GActionEntry actions[] = {
        { "toggle", act_toggle }, { "show", act_show }, { "clear", act_clear }, { "quit", act_quit },
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
    clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    prim = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    g_signal_connect(clip, "owner-change", G_CALLBACK(on_owner_change), NULL);
    schedule_read(300);   /* pick up what is already on the clipboard */
    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_application_hold(a);
    watch_for_updates();
    /* First run (e.g. installed from the .deb), or the shortcut was removed since: (re)create it. */
    if (!cfg.setup_done || !shortcut_bound()) apply_hotkey(cfg.hotkey);
    logmsg("running (%s), history in %s", is_wayland ? "wayland via XWayland" : "x11", data_dir);
}

static int on_cmdline(GApplication *a, GApplicationCommandLine *cl, gpointer d) {
    int argc;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    const char *m = argc > 1 ? argv[1] : "--toggle";
    if (!strcmp(m, "--quit")) g_application_quit(a);
    else if (!strcmp(m, "--clear")) clear_all();
    else if (!strcmp(m, "--show")) show_popup();
    else if (strcmp(m, "--daemon")) toggle();
    /* after restarting into a new version: keep serving what was on the clipboard */
    if (argc > 2 && g_str_has_prefix(argv[2], "--restore=") && find_item(argv[2] + 10, NULL)) {
        g_free(current_id);
        current_id = g_strdup(argv[2] + 10);
    }
    g_strfreev(argv);
    return 0;
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "";
    self_exe = g_file_read_link("/proc/self/exe", NULL);
    if (!self_exe) self_exe = g_strdup(argv[0]);
    self_dir = g_path_get_dirname(self_exe);
    if (!strcmp(m, "--version")) { puts("superv " VERSION); return 0; }
    if (!strcmp(m, "--quit")) {   /* ask a running SuperV to stop; never start one just to quit */
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (bus) {
            GVariant *r = g_dbus_connection_call_sync(bus, APP_ID, "/io/github/superv/SuperV", "org.gtk.Actions",
                "Activate", g_variant_new_parsed("('quit', @av [], @a{sv} {})"), NULL,
                G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, NULL);
            if (r) g_variant_unref(r);
            g_object_unref(bus);
        }
        return 0;
    }
    if (!strcmp(m, "--help") || !strcmp(m, "-h")) {
        puts("SuperV " VERSION " — Win+V clipboard history for Linux\n\n"
             "  superv            open / close the panel (what the shortcut runs)\n"
             "  superv --daemon   start in the background (runs at login)\n"
             "  superv --show     open the panel\n"
             "  superv --clear    clear history (pinned items stay)\n"
             "  superv --quit     stop SuperV\n"
             "  superv --doctor   check the setup\n\n"
             "Settings: ~/.config/superv/config.ini");
        return 0;
    }
    if (!strcmp(m, "--doctor") || !strcmp(m, "--setup-hotkeys") || !strcmp(m, "--remove-hotkeys")) {
        char *setup = setup_path();
        char **args = g_new0(char *, argc + 3);
        args[0] = "python3"; args[1] = setup;
        for (int i = 1; i < argc; i++) args[i + 1] = argv[i];
        execvp("python3", args);
        perror("python3");
        return 1;
    }
    debug_log = g_getenv("SUPERV_DEBUG") != NULL;
    is_wayland = !g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") || g_getenv("WAYLAND_DISPLAY");
    if (!g_getenv("DISPLAY")) { fputs("superv: no X11/XWayland display (DISPLAY not set)\n", stderr); return 1; }
    /* Use X11/XWayland: on GNOME Wayland only X clients may watch the clipboard in the background. */
    gdk_set_allowed_backends("x11");
    data_dir = g_build_filename(g_get_user_data_dir(), "superv", NULL);
    img_dir = g_build_filename(data_dir, "images", NULL);
    hist_path = g_build_filename(data_dir, "history.ini", NULL);
    cfg_dir = g_build_filename(g_get_user_config_dir(), "superv", NULL);
    cfg_path = g_build_filename(cfg_dir, "config.ini", NULL);
    app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_cmdline), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

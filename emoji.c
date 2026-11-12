/*
 * SuperV's emoji panel — the Windows emoji panel (Win+.) for Linux. Was SuperDot 1.0; now part of SuperV
 * (same process as the clipboard history in superv.c, loaded only while it is turned on).
 *
 *   Super+.  (or your own shortcut)   open / close the panel
 *   type                              search ("heart", "cat", "shrug", "euro"…)
 *   arrows / Page Up / Page Down      move around
 *   Enter or click                    insert into the app you were typing in
 *   Shift+Enter or Shift+click        insert and close the panel
 *   drag the edges / the top row      resize / move the panel
 *   Tab                               Emoji → Kaomoji → Symbols
 *   Esc                               clear the search, then close
 *
 * Picking an emoji puts it on the clipboard, presses Shift+Insert in your app, then puts
 * back whatever you had copied before. The emoji never shows up in clipboard history
 * (it is marked like a password, which SuperV, Klipper and CopyQ skip).
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
#include <sys/time.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include "emoji-data.h"
#include "superv.h"

#define APP_ID   SV_APP_ID
#define VERSION  SV_VERSION
#define REPO_URL SV_REPO_URL
#define DEFAULT_HOTKEY "<Super>period"
#define WIN_W      400     /* includes the 10 px shadow margin on each side */
#define WIN_H      480
#define MIN_W      300
#define MIN_H      330
#define MAX_RECENT 36
#define SNAP_MAX   (32 * 1024 * 1024)   /* biggest clipboard content we keep aside while pasting */

enum { TAB_EMOJI, TAB_KAOMOJI, TAB_SYMBOLS, N_TABS };
static const char *TAB_NAMES[N_TABS] = { "Emoji", "Kaomoji", "Symbols" };

static void logmsg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *m = g_strdup_vprintf(fmt, ap); va_end(ap);
    GDateTime *t = g_date_time_new_now_local();
    char *ts = g_date_time_format(t, "%H:%M:%S");
    g_printerr("%s [superv:emoji] %s\n", ts, m);
    g_free(ts); g_free(m); g_date_time_unref(t);
}

/* ------------------------------------------------------------------ data */

/* The data is used in place (no copy): items keep 4-byte offsets into EMOJI_DATA. */
typedef struct {
    guint32 text, tone, name, kw;   /* tone: text with \x01 where the skin tone goes; 0 = no skin tones */
    guint8 tab, group, ok;          /* ok: 0 = not checked yet, 1 = the font draws it, 2 = it can't */
} Item;
typedef struct { const char *title, *icon; } Group;
#define TXT(it)  (EMOJI_DATA + (it)->text)
#define NAME(it) (EMOJI_DATA + (it)->name)
#define KW(it)   (EMOJI_DATA + (it)->kw)

static Item *items;
static int n_items;
static Group groups[N_TABS][24];
static int n_groups[N_TABS];

static int tab_of(char c) { return c == 'e' ? TAB_EMOJI : c == 'k' ? TAB_KAOMOJI : c == 's' ? TAB_SYMBOLS : -1; }

static void load_data(void) {
    int cap = 0;
    for (int i = 0; i < EMOJI_DATA_LEN; i++) cap += EMOJI_DATA[i] == 0;
    items = g_new0(Item, cap / 5 + 1);
    for (const char *p = EMOJI_DATA, *end = EMOJI_DATA + EMOJI_DATA_LEN; p < end;) {
        const char *f[5];
        int n = p[0] == '#' ? 3 : 5;
        for (int k = 0; k < n; k++) { f[k] = p < end ? p : end - 1; p += strlen(p) + 1; }
        int t = tab_of(f[0][f[0][0] == '#' ? 1 : 0]);
        if (t < 0) continue;
        if (n == 3) {
            if (n_groups[t] < (int)G_N_ELEMENTS(groups[t])) {
                groups[t][n_groups[t]].title = f[1];
                groups[t][n_groups[t]].icon = f[2];
                n_groups[t]++;
            }
        } else if (f[0][1] >= '0' && f[0][1] - '0' < n_groups[t]) {
            Item *it = &items[n_items++];
            it->tab = t; it->group = f[0][1] - '0';
            it->text = f[1] - EMOJI_DATA; it->tone = *f[2] ? f[2] - EMOJI_DATA : 0;
            it->name = f[3] - EMOJI_DATA; it->kw = f[4] - EMOJI_DATA;
        }
    }
}

/* Same emoji, ignoring skin tones and the emoji-style selector (FE0F)? */
static gboolean same_emoji(const char *a, const char *b) {
    for (;;) {
        gunichar x, y;
        while ((x = g_utf8_get_char(a)) == 0xFE0F || (x >= 0x1F3FB && x <= 0x1F3FF)) a = g_utf8_next_char(a);
        while ((y = g_utf8_get_char(b)) == 0xFE0F || (y >= 0x1F3FB && y <= 0x1F3FF)) b = g_utf8_next_char(b);
        if (x != y) return FALSE;
        if (!x) return TRUE;
        a = g_utf8_next_char(a); b = g_utf8_next_char(b);
    }
}

/* ------------------------------------------------------------------ config */

static struct {
    char *hotkey, *theme, *paste_keys, *color;
    int skin_tone, paste_delay_ms, tab, pos_x, pos_y, width, height;
    gboolean keep_open, restore_clipboard, setup_done, glass;
} cfg;

static char *cfg_dir, *cfg_path, *data_dir, *recent_path;

static void config_save(void) {
    GKeyFile *k = g_key_file_new();
    g_key_file_set_string(k, "emoji", "hotkey", cfg.hotkey);
    g_key_file_set_string(k, "emoji", "theme", cfg.theme);
    g_key_file_set_string(k, "emoji", "color", cfg.color);
    g_key_file_set_integer(k, "emoji", "skin_tone", cfg.skin_tone);
    g_key_file_set_string(k, "emoji", "paste_keys", cfg.paste_keys);
    g_key_file_set_integer(k, "emoji", "paste_delay_ms", cfg.paste_delay_ms);
    g_key_file_set_boolean(k, "emoji", "stay_open", cfg.keep_open);
    g_key_file_set_integer(k, "emoji", "width", cfg.width);
    g_key_file_set_integer(k, "emoji", "height", cfg.height);
    g_key_file_set_boolean(k, "emoji", "restore_clipboard", cfg.restore_clipboard);
    g_key_file_set_boolean(k, "emoji", "glass", cfg.glass);
    g_key_file_set_integer(k, "emoji", "tab", cfg.tab);
    g_key_file_set_integer(k, "emoji", "pos_x", cfg.pos_x);
    g_key_file_set_integer(k, "emoji", "pos_y", cfg.pos_y);
    g_key_file_set_boolean(k, "emoji", "shortcut_configured", cfg.setup_done);
    g_key_file_set_comment(k, "emoji", NULL,
        " SuperV emoji panel settings. After editing run: superv --quit && superv --daemon &\n"
        " paste_keys: shift+insert (default; works in terminals too), ctrl+v, ctrl+shift+v, none (copy only)\n"
        " theme: auto (follow the desktop), light, dark     skin_tone: 0 (default yellow) to 5 (dark)\n"
        " stay_open: stay open after picking, to pick several     restore_clipboard: put back what you had copied\n"
        " glass: see-through panel (KDE Plasma also blurs what is behind it)\n"
        " color: azure, graphite, emerald, amber, rose, violet\n"
        " width/height/pos_x/pos_y: panel size and place (drag its edges or top row; double-click the top row to reset)\n"
        " hotkey: change it from the gear menu in the emoji panel\n"
        " turn the whole panel off: SuperV's settings (Super+V, then the gear), or emoji_panel=false in config.ini",
        NULL);
    g_mkdir_with_parents(cfg_dir, 0700);
    g_key_file_save_to_file(k, cfg_path, NULL);
    g_chmod(cfg_path, 0600);
    g_key_file_unref(k);
}

static void config_defaults(void) {   /* everything except the shortcut */
    g_free(cfg.theme); cfg.theme = g_strdup("auto");
    g_free(cfg.color); cfg.color = g_strdup("violet");   /* SuperV's colour */
    g_free(cfg.paste_keys); cfg.paste_keys = g_strdup("shift+insert");
    cfg.skin_tone = 0; cfg.paste_delay_ms = 160; cfg.tab = TAB_EMOJI;
    cfg.keep_open = TRUE; cfg.restore_clipboard = TRUE; cfg.glass = TRUE;
    cfg.pos_x = cfg.pos_y = -1;
    cfg.width = WIN_W; cfg.height = WIN_H;
}

static void config_load(void) {
    config_defaults();
    cfg.hotkey = g_strdup(DEFAULT_HOTKEY);
    GKeyFile *k = g_key_file_new();
    if (g_key_file_load_from_file(k, cfg_path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
#define KB(n, f) do { gboolean v = g_key_file_get_boolean(k, "emoji", n, &e); if (!e) f = v; g_clear_error(&e); } while (0)
#define KI(n, f) do { int v = g_key_file_get_integer(k, "emoji", n, &e); if (!e) f = v; g_clear_error(&e); } while (0)
#define KS(n, f) do { char *v = g_key_file_get_string(k, "emoji", n, NULL); if (v) { g_strstrip(v); g_free(f); f = v; } } while (0)
        KS("hotkey", cfg.hotkey);
        KS("theme", cfg.theme);
        KS("color", cfg.color);
        KI("skin_tone", cfg.skin_tone);
        KS("paste_keys", cfg.paste_keys);
        KI("paste_delay_ms", cfg.paste_delay_ms);
        KB("stay_open", cfg.keep_open);
        KI("width", cfg.width);
        KI("height", cfg.height);
        KB("restore_clipboard", cfg.restore_clipboard);
        KB("glass", cfg.glass);
        KI("tab", cfg.tab);
        KI("pos_x", cfg.pos_x);
        KI("pos_y", cfg.pos_y);
        KB("shortcut_configured", cfg.setup_done);
    }
    g_key_file_unref(k);
    cfg.skin_tone = CLAMP(cfg.skin_tone, 0, 5);
    cfg.tab = CLAMP(cfg.tab, 0, N_TABS - 1);
    cfg.paste_delay_ms = CLAMP(cfg.paste_delay_ms, 20, 2000);
    cfg.width = CLAMP(cfg.width, MIN_W, 1600);
    cfg.height = CLAMP(cfg.height, MIN_H, 1400);
    config_save();
}

static void reload_hotkey(void) {   /* superv-feature-setup --hotkey may have changed it */
    GKeyFile *k = g_key_file_new();
    char *v = g_key_file_load_from_file(k, cfg_path, G_KEY_FILE_NONE, NULL)
              ? g_key_file_get_string(k, "emoji", "hotkey", NULL) : NULL;
    if (v) { g_free(cfg.hotkey); cfg.hotkey = g_strstrip(v); }
    GError *e = NULL;
    gboolean done = g_key_file_get_boolean(k, "emoji", "shortcut_configured", &e);
    if (!e) cfg.setup_done = done;
    g_clear_error(&e);
    g_key_file_unref(k);
}

static void save_settings(void) { reload_hotkey(); config_save(); }

/* ------------------------------------------------------------------ recently used */

static GPtrArray *recent;   /* newest first */

static void recent_load(void) {
    recent = g_ptr_array_new_with_free_func(g_free);
    char *s = NULL;
    if (!g_file_get_contents(recent_path, &s, NULL, NULL)) return;
    char **lines = g_strsplit(s, "\n", -1);
    for (int i = 0; lines[i] && recent->len < MAX_RECENT; i++)
        if (*lines[i] && g_utf8_validate(lines[i], -1, NULL)) g_ptr_array_add(recent, g_strdup(lines[i]));
    g_strfreev(lines); g_free(s);
}

static void recent_save(void) {
    GString *o = g_string_new(NULL);
    for (guint i = 0; i < recent->len; i++) g_string_append_printf(o, "%s\n", (char *)recent->pdata[i]);
    g_mkdir_with_parents(data_dir, 0700);
    g_file_set_contents(recent_path, o->str, o->len, NULL);
    g_chmod(recent_path, 0600);
    g_string_free(o, TRUE);
}

static void recent_add(const char *s) {
    for (guint i = 0; i < recent->len; i++)
        if (!strcmp(recent->pdata[i], s)) { g_ptr_array_remove_index(recent, i); break; }
    g_ptr_array_insert(recent, 0, g_strdup(s));
    while (recent->len > MAX_RECENT) g_ptr_array_remove_index(recent, recent->len - 1);
    recent_save();
}

static int item_for(const char *s) {   /* -1 when unknown */
    for (int i = 0; i < n_items; i++)   /* exact first: ♥ (symbol) is not ♥️ (emoji) */
        if (!strcmp(TXT(&items[i]), s)) return i;
    for (int i = 0; i < n_items; i++)   /* then the same emoji with another skin tone */
        if ((guchar)*TXT(&items[i]) == (guchar)*s && same_emoji(TXT(&items[i]), s)) return i;
    return -1;
}

/* ------------------------------------------------------------------ typing: SuperV's shared virtual keyboard */

static gboolean prev_is_x11;   /* the app we type into is an X11 one */

/* ------------------------------------------------------------------ clipboard: insert, then put back */

static GtkClipboard *clip, *prim;

typedef struct { GdkAtom target, type; int format; GBytes *data; } SnapEntry;
typedef struct { GtkClipboard *cb; GPtrArray *entries; int pending; guint gen; gsize total; gboolean done, empty, failed; } Snap;
typedef struct { Snap *s; guint gen; } SnapReq;
static Snap snaps[2];            /* what was on CLIPBOARD and PRIMARY before we put the emoji there */
static char *emoji_text;         /* what we are serving right now */
static GdkAtom hint_atom;
static gint64 keys_sent_at;
static gboolean served;
static guint restore_src;

static void snap_entry_free(gpointer p) { SnapEntry *e = p; g_bytes_unref(e->data); g_free(e); }

static void snap_reset(Snap *s) {
    if (!s->entries) s->entries = g_ptr_array_new_with_free_func(snap_entry_free);
    g_ptr_array_set_size(s->entries, 0);
    s->pending = 0; s->total = 0; s->gen++;
    s->done = s->empty = s->failed = FALSE;
}

static void on_snap_contents(GtkClipboard *c, GtkSelectionData *sd, gpointer d) {
    SnapReq *r = d;
    Snap *s = r->s;
    if (r->gen == s->gen) {
        int len = gtk_selection_data_get_length(sd);
        if (len >= 0 && s->total + len <= SNAP_MAX) {
            SnapEntry *e = g_new0(SnapEntry, 1);
            e->target = gtk_selection_data_get_target(sd);
            e->type = gtk_selection_data_get_data_type(sd);
            e->format = gtk_selection_data_get_format(sd);
            e->data = g_bytes_new(gtk_selection_data_get_data(sd), len);
            s->total += len;
            g_ptr_array_add(s->entries, e);
        } else if (len > 0) {
            s->failed = TRUE;   /* too big to keep aside: we won't touch the clipboard back */
        }
        if (--s->pending == 0) s->done = TRUE;
    }
    g_free(r);
}

static gboolean skip_target(const char *n) {
    return !strcmp(n, "TARGETS") || !strcmp(n, "MULTIPLE") || !strcmp(n, "TIMESTAMP") || !strcmp(n, "SAVE_TARGETS")
        || !strcmp(n, "DELETE") || !strcmp(n, "INSERT_SELECTION") || !strcmp(n, "INSERT_PROPERTY");
}

static void on_snap_targets(GtkClipboard *c, GdkAtom *atoms, gint n, gpointer d) {
    SnapReq *r = d;
    Snap *s = r->s;
    if (r->gen != s->gen) { g_free(r); return; }
    if (!atoms || n <= 0) { s->empty = s->done = TRUE; g_free(r); return; }
    gboolean have_png = FALSE, image_taken = FALSE, secret = FALSE;
    for (int i = 0; i < n; i++) {
        char *m = gdk_atom_name(atoms[i]);
        have_png |= !strcmp(m, "image/png");
        secret |= !strcmp(m, "x-kde-passwordManagerHint");
        g_free(m);
    }
    if (secret) {   /* a password: don't read it, don't bring it back after the manager clears it */
        s->failed = s->done = TRUE;
        g_free(r);
        return;
    }
    for (int i = 0; i < n; i++) {
        char *m = gdk_atom_name(atoms[i]);
        /* images: keep one format (PNG if offered), each conversion can be slow and big */
        gboolean skip = skip_target(m) || (g_str_has_prefix(m, "image/") &&
                                           (have_png ? strcmp(m, "image/png") != 0 : image_taken));
        image_taken |= !skip && g_str_has_prefix(m, "image/");
        g_free(m);
        if (skip) continue;
        SnapReq *q = g_new(SnapReq, 1);
        *q = *r;
        s->pending++;
        gtk_clipboard_request_contents(c, atoms[i], on_snap_contents, q);
    }
    if (!s->pending) s->empty = s->done = TRUE;
    g_free(r);
}

static void take_snapshot(Snap *s) {
    if (gtk_clipboard_get_owner(s->cb) == G_OBJECT(sv_app)) return;   /* still ours: the last snapshot is current */
    snap_reset(s);
    SnapReq *r = g_new(SnapReq, 1);
    r->s = s; r->gen = s->gen;
    gtk_clipboard_request_targets(s->cb, on_snap_targets, r);
}

static guint insert_src, paste_src;
static GtkWidget *win;   /* the panel (defined again with the rest of the UI below) */

static void on_owner_change(GtkClipboard *c, GdkEvent *e, gpointer d) {
    GObject *o = gtk_clipboard_get_owner(c);
    if (o == G_OBJECT(sv_app) || GTK_IS_WIDGET(o)) return;   /* our own emoji, or text selected in our search box */
    if (gtk_widget_get_visible(win) || insert_src) take_snapshot(c == clip ? &snaps[0] : &snaps[1]);
}

static void snap_get(GtkClipboard *c, GtkSelectionData *sd, guint info, gpointer owner) {
    Snap *s = c == clip ? &snaps[0] : &snaps[1];
    if (info >= s->entries->len) return;
    SnapEntry *e = s->entries->pdata[info];
    gsize len;
    const guchar *p = g_bytes_get_data(e->data, &len);
    gtk_selection_data_set(sd, e->type, e->format, p, len);
}

static gboolean replacing;   /* we are replacing our own clipboard content: keep the snapshot */

static void snap_clear(GtkClipboard *c, gpointer owner) {   /* someone else copied: drop what we kept aside */
    if (!replacing) snap_reset(c == clip ? &snaps[0] : &snaps[1]);
}

static void restore_one(Snap *s) {
    if (gtk_clipboard_get_owner(s->cb) != G_OBJECT(sv_app)) return;   /* you copied something new meanwhile */
    if (!s->done || s->failed) return;
    if (s->empty || !s->entries->len) { gtk_clipboard_clear(s->cb); return; }
    int n = s->entries->len;
    GtkTargetEntry *t = g_new0(GtkTargetEntry, n);
    for (int i = 0; i < n; i++) {
        t[i].target = gdk_atom_name(((SnapEntry *)s->entries->pdata[i])->target);
        t[i].info = i;
    }
    gtk_clipboard_set_with_owner(s->cb, t, n, snap_get, snap_clear, G_OBJECT(sv_app));
    for (int i = 0; i < n; i++) g_free(t[i].target);
    g_free(t);
}

static gboolean restore_clipboard(gpointer d) {
    restore_src = 0;
    if (sv_debug) logmsg("putting the clipboard back (%s)", served ? "emoji was pasted" : "timeout");
    restore_one(&snaps[0]);
    restore_one(&snaps[1]);
    return G_SOURCE_REMOVE;
}

static void emoji_get(GtkClipboard *c, GtkSelectionData *sd, guint info, gpointer owner) {
    if (info == 1) {   /* "don't keep me in clipboard history" */
        gtk_selection_data_set(sd, gtk_selection_data_get_target(sd), 8, (const guchar *)"secret", 6);
        return;
    }
    gtk_selection_data_set_text(sd, emoji_text ? emoji_text : "", -1);
    /* the app asked for it after we pressed paste: put the old clipboard back shortly */
    if (keys_sent_at && g_get_monotonic_time() >= keys_sent_at && !served && cfg.restore_clipboard) {
        served = TRUE;
        if (restore_src) g_source_remove(restore_src);
        restore_src = g_timeout_add(600, restore_clipboard, NULL);
    }
}

static void set_emoji(const char *s) {
    g_free(emoji_text);
    emoji_text = g_strdup(s);
    GtkTargetList *tl = gtk_target_list_new(NULL, 0);
    gtk_target_list_add_text_targets(tl, 0);
    gtk_target_list_add(tl, hint_atom, 0, 1);
    int n;
    GtkTargetEntry *t = gtk_target_table_new_from_list(tl, &n);
    replacing = TRUE;
    gtk_clipboard_set_with_owner(clip, t, n, emoji_get, NULL, G_OBJECT(sv_app));
    gtk_clipboard_set_with_owner(prim, t, n, emoji_get, NULL, G_OBJECT(sv_app));   /* Shift+Insert in terminals */
    replacing = FALSE;
    gtk_target_table_free(t, n);
    gtk_target_list_unref(tl);
}

/* ------------------------------------------------------------------ UI state */

typedef struct { int item; char *text; int row, col; } Cell;    /* text: our copy of a recent entry, else NULL */
typedef struct { const char *title; int first, count, y, group, tab; } Section;  /* group -1: recent, -2: results */
typedef struct { int first, count, y, w, h, tab; } Row;   /* w, h: cell size (kaomoji cells are wider) */

static GtkWidget *win, *area, *scroll, *search, *status_lbl, *cat_box, *cat_scroll, *tone_btn, *tone_lbl;
static GtkWidget *tab_btns[N_TABS], *settings_pop, *tone_pop, *hotkey_btn, *hk_status, *hint_lbl;
static GtkWidget *banner, *banner_lbl, *banner_btn, *theme_btns[3], *paste_btns[2], *keep_sw, *restore_sw;
static GtkWidget *keys_revealer, *keys_win, *glass_sw;
static int panel_x, panel_y;
static gboolean dropping;   /* we gave the keyboard back to your app to type the emoji; the panel stays */
static GArray *cells, *sections, *rows;
static GPtrArray *cat_btns;
static PangoLayout *lay, *chk_lay;
static int cur_tab, sel, hover = -1, content_h, laid_w, active_cat = -99;
static gboolean dark, had_focus, capturing, syncing, geom_dirty, enabling, reopen_after;
static guint check_src, hide_src;

#define PAD      4
#define HEADER_H 26

static const struct { double text[3], sub[3]; } PAL[2] = {
    { { 0.094, 0.125, 0.200 }, { 0.353, 0.400, 0.502 } },     /* light */
    { { 0.902, 0.922, 0.961 }, { 0.576, 0.627, 0.722 } },     /* dark */
};

/* Colour palettes: one accent each (for dark and light), which also tints the panel and the glass. */
static const struct { const char *id, *name, *dark, *light; } PALS[] = {
    { "azure",    "Azure · calm and technical (developers, engineers)",       "#7cb4ff", "#2563eb" },
    { "graphite", "Graphite · neutral and formal (office, finance, law)",      "#b9c2d0", "#475569" },
    { "emerald",  "Emerald · fresh and steady (health, science, education)",   "#62d6a8", "#0f8a5f" },
    { "amber",    "Amber · warm and friendly (writing, teaching, support)",    "#f3c26d", "#b45309" },
    { "rose",     "Rose · soft and expressive (design, marketing, social)",    "#f4a3b8", "#c2416b" },
    { "violet",   "Violet · bold and playful (art, music, gaming)",            "#b9a4ff", "#6d3fd4" },
};
static double acc[3];   /* the accent, for the grid we draw ourselves */
static GtkCssProvider *pal_css;
static GtkWidget *pal_btns[G_N_ELEMENTS(PALS)];

static Cell *cell_at(int i) { return &g_array_index(cells, Cell, i); }

/* The emoji as it is shown and inserted: with your skin tone where it has one. */
static const char *display_text(Cell *c, char *buf, gsize size) {
    if (c->text) return c->text;
    Item *it = &items[c->item];
    if (!it->tone || !cfg.skin_tone) return TXT(it);
    char mod[8];
    mod[g_unichar_to_utf8(0x1F3FA + cfg.skin_tone, mod)] = 0;
    GString *o = g_string_new(NULL);
    for (const char *p = EMOJI_DATA + it->tone; *p; p++) {
        if (*p == '\x01') g_string_append(o, mod);
        else g_string_append_c(o, *p);
    }
    g_strlcpy(buf, o->str, size);
    g_string_free(o, TRUE);
    return buf;
}

/* Does this system's font draw it as one picture? (new emoji can be missing from older fonts) */
static gboolean font_draws(Item *it) {
    if (it->tab == TAB_KAOMOJI) return TRUE;
    if (it->ok) return it->ok == 1;
    pango_layout_set_text(chk_lay, TXT(it), -1);
    gboolean ok = pango_layout_get_unknown_glyphs_count(chk_lay) == 0;
    if (ok && it->tab == TAB_EMOJI) {
        int visible = 0;
        PangoLayoutIter *iter = pango_layout_get_iter(chk_lay);
        do {
            PangoLayoutRun *run = pango_layout_iter_get_run_readonly(iter);
            for (int i = 0; run && i < run->glyphs->num_glyphs; i++) {
                PangoGlyphInfo *g = &run->glyphs->glyphs[i];
                visible += g->glyph != PANGO_GLYPH_EMPTY && g->geometry.width > 0;
            }
        } while (pango_layout_iter_next_run(iter));
        pango_layout_iter_free(iter);
        ok = visible == 1;
    }
    it->ok = ok ? 1 : 2;
    return ok;
}

static gboolean check_some(gpointer d);
static gboolean start_checks(gpointer d) {
    check_src = g_idle_add_full(G_PRIORITY_LOW, check_some, NULL, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean check_some(gpointer d) {   /* checks everything in the background after login */
    static int next;
    for (int end = MIN(next + 150, n_items); next < end; next++) font_draws(&items[next]);
    if (next < n_items) return G_SOURCE_CONTINUE;
    int missing = 0;
    for (int i = 0; i < n_items; i++) missing += items[i].ok == 2;
    logmsg("font check done: %d of %d items can't be drawn by your fonts and are hidden", missing, n_items);
    check_src = 0;
    return G_SOURCE_REMOVE;
}

/* ---- search: every word you type must start a word of the name or keywords */
static gboolean word_prefix(const char *hay, const char *w, gsize wl) {
    for (const char *p = hay; *p; p++)
        if ((p == hay || !g_ascii_isalnum(p[-1])) && !g_ascii_strncasecmp(p, w, wl)) return TRUE;
    return FALSE;
}

/* -1: no match; then best first: the character itself, the exact name, every word in the name, keywords */
static int match_score(const Item *it, const char *q, char **words) {
    if (!strcmp(TXT(it), q)) return 0;
    gboolean in_name = TRUE;
    for (int i = 0; words[i]; i++) {
        if (!*words[i]) continue;
        gsize wl = strlen(words[i]);
        gboolean n = word_prefix(NAME(it), words[i], wl);
        if (!n && !word_prefix(KW(it), words[i], wl)) return -1;
        in_name &= n;
    }
    if (!g_ascii_strcasecmp(NAME(it), q)) return 1;
    return in_name ? 2 : 3;
}

typedef struct { int score, len, item; } Hit;
static int hit_cmp(gconstpointer a, gconstpointer b) {   /* then shorter names: "red heart" before "heart with arrow" */
    const Hit *x = a, *y = b;
    return x->score != y->score ? x->score - y->score : x->len != y->len ? x->len - y->len : x->item - y->item;
}

static void add_section(const char *title, int group, int tab) {
    Section s = { title, cells->len, 0, 0, group, tab };
    g_array_append_val(sections, s);
}

static void cell_clear(gpointer p) { g_free(((Cell *)p)->text); }

static void add_cell(int item, const char *text) {
    Cell c = { item, g_strdup(text), 0, 0 };   /* a copy: the recent list changes while the panel is open */
    g_array_append_val(cells, c);
    g_array_index(sections, Section, sections->len - 1).count++;
}

static void update_status(void);
static void update_cats(void);
static void build_cats(void);

static void layout_grid(void) {
    int w = gtk_widget_get_allocated_width(area);
    if (w <= 1) w = WIN_W - 40;
    laid_w = w;
    int avail = w - 2 * PAD;
    g_array_set_size(rows, 0);
    int y = 2;
    for (guint s = 0; s < sections->len; s++) {
        Section *sec = &g_array_index(sections, Section, s);
        int kao = sec->tab == TAB_KAOMOJI, cols = MAX(1, avail / (kao ? 112 : 39)), ch = kao ? 36 : 39;
        sec->y = y;
        y += HEADER_H;
        for (int k = 0; k < sec->count; k++) {
            Cell *c = cell_at(sec->first + k);
            c->col = k % cols;
            if (c->col == 0) {
                Row r = { sec->first + k, 0, y, avail / cols, ch, sec->tab };
                g_array_append_val(rows, r);
                y += ch;
            }
            c->row = rows->len - 1;
            g_array_index(rows, Row, rows->len - 1).count++;
        }
        y += 6;
    }
    int ch = MAX(y + 4, 120);
    if (ch != content_h) { content_h = ch; gtk_widget_set_size_request(area, -1, content_h); }
    gtk_widget_queue_draw(area);
}

static char *built_query;   /* the search the grid shows (the entry reports changes 150 ms late) */

static void build_cells(void) {
    g_array_set_size(cells, 0);
    g_array_set_size(sections, 0);
    const char *q = gtk_entry_get_text(GTK_ENTRY(search));
    g_free(built_query);
    built_query = g_strdup(q);
    char *query = g_strstrip(g_utf8_strdown(q, -1));
    if (!*query) {
        add_section("Recently used", -1, cur_tab);
        for (guint i = 0; i < recent->len; i++) {
            int it = item_for(recent->pdata[i]);
            if (it >= 0 && items[it].tab == cur_tab && font_draws(&items[it])) add_cell(it, recent->pdata[i]);
        }
        for (int g = 0; g < n_groups[cur_tab]; g++) {
            add_section(groups[cur_tab][g].title, g, cur_tab);
            for (int i = 0; i < n_items; i++)
                if (items[i].tab == cur_tab && items[i].group == g && font_draws(&items[i])) add_cell(i, NULL);
        }
        for (guint k = 0; k < sections->len;)   /* e.g. Flags without a colour emoji font */
            if (!g_array_index(sections, Section, k).count) g_array_remove_index(sections, k); else k++;
    } else {
        char **words = g_strsplit_set(query, " ,", -1);
        GArray *hits = g_array_new(FALSE, FALSE, sizeof(Hit));
        for (int k = 0; k < N_TABS; k++) {   /* every tab, the one you are on first */
            int t = (cur_tab + k) % N_TABS;
            g_array_set_size(hits, 0);
            for (int i = 0; i < n_items; i++) {
                if (items[i].tab != t) continue;
                int sc = match_score(&items[i], query, words);
                if (sc >= 0 && font_draws(&items[i])) { Hit h = { sc, strlen(NAME(&items[i])), i }; g_array_append_val(hits, h); }
            }
            if (!hits->len) continue;
            g_array_sort(hits, hit_cmp);
            add_section(TAB_NAMES[t], -2, t);
            for (guint i = 0; i < hits->len; i++) add_cell(g_array_index(hits, Hit, i).item, NULL);
        }
        g_array_free(hits, TRUE);
        g_strfreev(words);
    }
    g_free(query);
    sel = 0;
    hover = -1;
    layout_grid();
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), 0);
    gtk_widget_set_visible(cat_scroll, !*q);
    if (!*q) build_cats();
    update_status();
    update_cats();
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void set_font(const char *family, double px, gboolean bold) {
    PangoFontDescription *fd = pango_font_description_from_string(family);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
}

static void draw_grid(GtkWidget *w, cairo_t *cr);

/* Draw into memory, then hand the finished picture to the screen in one go: drawing colour emoji
 * straight to an X11 window makes the X server do a slow round trip per emoji. */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    double x1, y1, x2, y2;
    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    int x = (int)x1, y = (int)y1, wd = (int)x2 - x + 1, ht = (int)y2 - y + 1, sc = gtk_widget_get_scale_factor(w);
    if (wd <= 0 || ht <= 0) return FALSE;
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, wd * sc, ht * sc);
    cairo_surface_set_device_scale(img, sc, sc);
    cairo_t *c = cairo_create(img);
    cairo_translate(c, -x, -y);
    draw_grid(w, c);
    cairo_destroy(c);
    cairo_set_source_surface(cr, img, x, y);
    cairo_paint(cr);
    cairo_surface_destroy(img);
    return FALSE;
}

static void draw_grid(GtkWidget *w, cairo_t *cr) {
    double x1, y1, x2, y2;
    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    const typeof(PAL[0]) *p = &PAL[dark];
    pango_layout_set_width(lay, -1);
    pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_NONE);
    if (!cells->len) {
        const char *q = gtk_entry_get_text(GTK_ENTRY(search));
        char *msg = g_strdup_printf("Nothing found for “%s”", q);
        set_font("sans", 13, FALSE);
        pango_layout_set_text(lay, msg, -1);
        int lw, lh;
        pango_layout_get_pixel_size(lay, &lw, &lh);
        cairo_set_source_rgb(cr, p->sub[0], p->sub[1], p->sub[2]);
        cairo_move_to(cr, MAX(0, (gtk_widget_get_allocated_width(w) - lw) / 2), 40);
        pango_cairo_show_layout(cr, lay);
        g_free(msg);
        return;
    }
    set_font("sans", 11.5, TRUE);
    for (guint s = 0; s < sections->len; s++) {
        Section *sec = &g_array_index(sections, Section, s);
        if (!sec->title || sec->y + HEADER_H < y1 || sec->y > y2) continue;
        pango_layout_set_text(lay, sec->title, -1);
        cairo_set_source_rgb(cr, p->sub[0], p->sub[1], p->sub[2]);
        cairo_move_to(cr, PAD + 6, sec->y + 6);
        pango_cairo_show_layout(cr, lay);
    }
    char buf[96];
    int font_tab = -1;
    for (guint r = 0; r < rows->len; r++) {
        Row *row = &g_array_index(rows, Row, r);
        int cell_w = row->w, cell_h = row->h;
        if (row->y + cell_h < y1 || row->y > y2) continue;
        if (row->tab != font_tab) {
            font_tab = row->tab;
            pango_layout_set_width(lay, -1);
            pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_NONE);
            if (font_tab == TAB_EMOJI) set_font("emoji", 23, FALSE);
            else if (font_tab == TAB_SYMBOLS) set_font("sans", 19, FALSE);
            else {
                set_font("sans", 13, FALSE);
                pango_layout_set_width(lay, (cell_w - 10) * PANGO_SCALE);
                pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
            }
        }
        for (int k = 0; k < row->count; k++) {
            int i = row->first + k;
            double x = PAD + k * cell_w, y = row->y;
            if (i == sel || i == hover) {
                rounded(cr, x + 1.5, y + 1.5, cell_w - 3, cell_h - 3, 7);
                cairo_set_source_rgba(cr, acc[0], acc[1], acc[2], i == sel ? 0.20 : 0.12);
                cairo_fill_preserve(cr);
                if (i == sel) {
                    cairo_set_source_rgb(cr, acc[0], acc[1], acc[2]);
                    cairo_set_line_width(cr, 1.5);
                    cairo_stroke(cr);
                }
                cairo_new_path(cr);
            }
            pango_layout_set_text(lay, display_text(cell_at(i), buf, sizeof buf), -1);
            int lw, lh;
            pango_layout_get_pixel_size(lay, &lw, &lh);
            cairo_set_source_rgb(cr, p->text[0], p->text[1], p->text[2]);
            cairo_move_to(cr, x + (cell_w - lw) / 2.0, y + (cell_h - lh) / 2.0);
            pango_cairo_show_layout(cr, lay);
        }
    }
}

static int cell_at_point(double x, double y) {
    for (guint r = 0; r < rows->len; r++) {
        Row *row = &g_array_index(rows, Row, r);
        if (y < row->y || y >= row->y + row->h) continue;
        int col = (int)(x - PAD) / MAX(row->w, 1);
        return x >= PAD && col < row->count ? row->first + col : -1;
    }
    return -1;
}

static void ensure_visible(int i) {
    if (i < 0 || i >= (int)cells->len) return;
    GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    Row *row = &g_array_index(rows, Row, cell_at(i)->row);
    double top = row->y, bottom = row->y + row->h, v = gtk_adjustment_get_value(a), page = gtk_adjustment_get_page_size(a);
    for (guint s = 0; s < sections->len; s++) {   /* first row of a section: show its title too */
        Section *sec = &g_array_index(sections, Section, s);
        if (sec->first == row->first) top = sec->y;
    }
    if (top < v) gtk_adjustment_set_value(a, top);
    else if (bottom > v + page) gtk_adjustment_set_value(a, bottom - page + 4);
}

static void select_cell(int i) {
    if (!cells->len) return;
    sel = CLAMP(i, 0, (int)cells->len - 1);
    ensure_visible(sel);
    update_status();
    gtk_widget_queue_draw(area);
}

static void move_rows(int dy) {
    if (!cells->len || sel < 0 || sel >= (int)cells->len) return;
    Cell *c = cell_at(sel);
    int r = CLAMP(c->row + dy, 0, (int)rows->len - 1);
    Row *from = &g_array_index(rows, Row, c->row), *row = &g_array_index(rows, Row, r);
    int col = row->w == from->w ? c->col : (c->col * from->w + from->w / 2) / MAX(row->w, 1);
    select_cell(row->first + MIN(col, row->count - 1));
}

static char *capitalized(const char *s) {
    char *o = g_strdup(s);
    if (*o) o[0] = g_ascii_toupper(o[0]);
    return o;
}

static void update_status(void) {
    int i = hover >= 0 ? hover : sel;
    if (i < 0 || i >= (int)cells->len) { gtk_label_set_text(GTK_LABEL(status_lbl), " "); return; }
    Cell *c = cell_at(i);
    char buf[96], *name = capitalized(c->item >= 0 ? NAME(&items[c->item]) : "");
    char *t = c->item >= 0 && items[c->item].tab == TAB_KAOMOJI ? g_strdup_printf("%s  ·  %s", display_text(c, buf, sizeof buf), name)
                                     : g_strdup_printf("%s   %s", display_text(c, buf, sizeof buf), name);
    gtk_label_set_text(GTK_LABEL(status_lbl), t);
    g_free(t); g_free(name);
}

/* ---- category bar */
static void add_class(GtkWidget *w, const char *c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }
static void rm_class(GtkWidget *w, const char *c) { gtk_style_context_remove_class(gtk_widget_get_style_context(w), c); }

static void update_cats(void) {   /* underline the category you are looking at */
    GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    double v = gtk_adjustment_get_value(a) + 8;
    int group = -99;
    for (guint s = 0; s < sections->len; s++) {
        Section *sec = &g_array_index(sections, Section, s);
        if (sec->y <= v || s == 0) group = sec->group;
    }
    double page = gtk_adjustment_get_page_size(a), upper = gtk_adjustment_get_upper(a);
    if (page > 1 && upper > page + 1 && v - 8 + page >= upper - 2 && sections->len)
        group = g_array_index(sections, Section, sections->len - 1).group;   /* scrolled to the end */
    if (group == active_cat) return;
    active_cat = group;
    for (guint i = 0; i < cat_btns->len; i++) {
        GtkWidget *b = cat_btns->pdata[i];
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "group")) == group) {
            add_class(b, "sd-cat-active");
            GtkAdjustment *h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(cat_scroll));
            GtkAllocation al;
            gtk_widget_get_allocation(b, &al);
            double hv = gtk_adjustment_get_value(h), hp = gtk_adjustment_get_page_size(h);
            if (al.x < hv) gtk_adjustment_set_value(h, al.x);
            else if (al.x + al.width > hv + hp) gtk_adjustment_set_value(h, al.x + al.width - hp);
        } else {
            rm_class(b, "sd-cat-active");
        }
    }
}

static void on_cat(GtkButton *b, gpointer d) {
    int group = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "group"));
    for (guint s = 0; s < sections->len; s++) {
        Section *sec = &g_array_index(sections, Section, s);
        if (sec->group != group || !sec->count) continue;
        gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), sec->y);
        sel = sec->first;
        update_status();
        gtk_widget_queue_draw(area);
    }
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(search));
}

static void build_cats(void) {   /* one button per section on screen */
    GList *old = gtk_container_get_children(GTK_CONTAINER(cat_box));
    for (GList *l = old; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(old);
    g_ptr_array_set_size(cat_btns, 0);
    active_cat = -99;
    for (guint s = 0; s < sections->len; s++) {
        Section *sec = &g_array_index(sections, Section, s);
        int g = sec->group;
        if (g < -1) continue;
        GtkWidget *b = gtk_button_new_with_label(g < 0 ? "🕘" : groups[sec->tab][g].icon);
        gtk_widget_set_tooltip_text(b, g < 0 ? "Recently used" : groups[sec->tab][g].title);
        gtk_widget_set_can_focus(b, FALSE);
        add_class(b, "sd-cat");
        if (sec->tab == TAB_KAOMOJI) add_class(b, "sd-cat-text");
        g_object_set_data(G_OBJECT(b), "group", GINT_TO_POINTER(g));
        g_signal_connect(b, "clicked", G_CALLBACK(on_cat), NULL);
        gtk_box_pack_start(GTK_BOX(cat_box), b, FALSE, FALSE, 0);
        g_ptr_array_add(cat_btns, b);
    }
    gtk_widget_show_all(cat_box);
}

static void set_tab(int t) {
    cur_tab = t;
    if (cfg.tab != t) { cfg.tab = t; save_settings(); }
    syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tab_btns[t]), TRUE);
    syncing = FALSE;
    gtk_widget_set_visible(tone_btn, t == TAB_EMOJI);
    char *ph = g_strdup_printf("Search %s", t == TAB_EMOJI ? "emoji" : t == TAB_KAOMOJI ? "kaomoji" : "symbols");
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), ph);
    g_free(ph);
    build_cells();
}

static void on_tab_toggled(GtkToggleButton *b, gpointer t) {
    if (!syncing && gtk_toggle_button_get_active(b)) {
        set_tab(GPOINTER_TO_INT(t));
        gtk_entry_grab_focus_without_selecting(GTK_ENTRY(search));
    }
}

/* ------------------------------------------------------------------ inserting */

static void notify(const char *title, const char *body) {
    GNotification *n = g_notification_new(title);
    g_notification_set_body(n, body);
    g_application_send_notification(G_APPLICATION(sv_app), "emoji", n);
    g_object_unref(n);
}

static void show_popup(void);

static void grab_keys(void);

static gboolean reopen(gpointer d) {   /* the emoji is typed: take the keyboard again for the next one */
    if (gtk_widget_get_visible(win)) grab_keys();
    return G_SOURCE_REMOVE;
}

static gboolean do_paste(gpointer d) {
    paste_src = 0;
    served = FALSE;
    keys_sent_at = g_get_monotonic_time();
    if (!sv_send_keys(cfg.paste_keys, prev_is_x11)) {
        keys_sent_at = 0;
        notify("Copied — press Ctrl+V to paste",
               "Typing it for you needs a one-time permission: open the emoji panel and click Enable.");
        gtk_widget_hide(win);
        return G_SOURCE_REMOVE;
    }
    if (restore_src) g_source_remove(restore_src);
    restore_src = cfg.restore_clipboard ? g_timeout_add(2500, restore_clipboard, NULL) : 0;   /* if never asked */
    if (reopen_after) g_timeout_add(120, reopen, NULL);
    return G_SOURCE_REMOVE;
}

static char *pending_text;
static gboolean pending_keep;
static int insert_waits;

/* Take the clipboard only once we've read what was on it (or after 0.5 s if its owner is slow). */
static gboolean continue_insert(gpointer d) {
    if (!(snaps[0].done && snaps[1].done) && insert_waits++ < 25) return G_SOURCE_CONTINUE;
    insert_src = 0;
    set_emoji(pending_text);
    reopen_after = pending_keep;
    if (g_strcmp0(cfg.paste_keys, "none")) paste_src = g_timeout_add(cfg.paste_delay_ms, do_paste, NULL);
    return G_SOURCE_REMOVE;
}

static void insert_text(const char *text, gboolean keep_open) {
    if (!text || !*text) return;
    char *s = g_strdup(text);   /* text may be a "Recently used" entry, which recent_add() frees */
    if (item_for(s) >= 0) recent_add(s);   /* only things from the panel (not any --insert text) */
    if (paste_src) { g_source_remove(paste_src); paste_src = 0; }   /* a second pick before the first pasted */
    if (restore_src) { g_source_remove(restore_src); restore_src = 0; }
    keys_sent_at = 0;
    if (!gtk_widget_get_visible(win)) { take_snapshot(&snaps[0]); take_snapshot(&snaps[1]); }   /* --insert */
    gboolean keep = keep_open && gtk_widget_get_visible(win) && g_strcmp0(cfg.paste_keys, "none");
    if (keep) { dropping = TRUE; gtk_widget_hide(keys_win); }   /* focus returns to your app; the panel stays put */
    else gtk_widget_hide(win);
    g_free(pending_text);
    pending_text = s;
    pending_keep = keep;
    insert_waits = 0;
    if (!insert_src) insert_src = g_timeout_add(20, continue_insert, NULL);
}

static void pick(int i, gboolean shift) {
    if (i < 0 || i >= (int)cells->len) return;
    char buf[96];
    insert_text(display_text(cell_at(i), buf, sizeof buf), shift != cfg.keep_open);
}

static gboolean on_area_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS) return FALSE;
    int i = cell_at_point(e->x, e->y);
    if (i >= 0) { sel = i; pick(i, (e->state & GDK_SHIFT_MASK) != 0); }
    return TRUE;
}

static gboolean on_area_motion(GtkWidget *w, GdkEventMotion *e, gpointer d) {
    int i = cell_at_point(e->x, e->y);
    if (i != hover) { hover = i; update_status(); gtk_widget_queue_draw(area); }
    return FALSE;
}

static gboolean on_area_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d) {
    if (hover >= 0) { hover = -1; update_status(); gtk_widget_queue_draw(area); }
    return FALSE;
}

static void on_area_size(GtkWidget *w, GdkRectangle *a, gpointer d) {
    if (a->width != laid_w) layout_grid();
}

static void on_scrolled(GtkAdjustment *a, gpointer d) { update_cats(); }
static void on_search_changed(GtkSearchEntry *e, gpointer d) { build_cells(); }

/* ------------------------------------------------------------------ theme + window */

static const char *CSS =
    ".sd-window { background-color: transparent; }"
    ".sd-panel { border-radius: 12px; padding: 10px 10px 6px 10px; margin: 10px; }"
    ".sd-nocomp .sd-panel { margin: 0; border-radius: 0; box-shadow: none; }"
    ".sd-dark .sd-panel { background-color: @sd_bg; border: 1px solid #263045; color: #e6ebf5;"
    "  box-shadow: 0 6px 18px rgba(0,0,0,0.55); }"
    ".sd-light .sd-panel { background-color: @sd_bg; border: 1px solid #d5dcea; color: #182033;"
    "  box-shadow: 0 6px 18px rgba(20,40,90,0.18); }"
    /* glass: see-through, a soft sheen from the top, a bright hairline edge */
    ".sd-glass .sd-panel { border-radius: 18px; }"
    ".sd-glass.sd-dark .sd-panel { background-color: transparent;"
    "  background-image: linear-gradient(to bottom, alpha(mix(#3d4350, @sd_accent, 0.32), 0.70), alpha(@sd_bg, 0.80) 45%, alpha(shade(@sd_bg, 0.8), 0.84));"
    "  border: 1px solid rgba(255,255,255,0.16);"
    "  box-shadow: inset 0 1px rgba(255,255,255,0.22), 0 6px 18px rgba(0,0,0,0.45); }"
    ".sd-glass.sd-light .sd-panel { background-color: transparent;"
    "  background-image: linear-gradient(to bottom, rgba(255,255,255,0.84), alpha(mix(#f4f5f7, @sd_accent, 0.10), 0.74) 45%, alpha(mix(#eceef2, @sd_accent, 0.18), 0.72));"
    "  border: 1px solid rgba(255,255,255,0.80);"
    "  box-shadow: inset 0 1px rgba(255,255,255,0.95), 0 6px 18px rgba(20,40,90,0.20); }"
    "button.sd-tab, button.sd-icon, button.sd-cat, button.sd-menuitem, button.sd-tone {"
    "  background-color: transparent; background-image: none; border: none; box-shadow: none; text-shadow: none;"
    "  border-radius: 6px; min-height: 0; min-width: 0; }"
    "button.sd-tab { padding: 4px 10px; font-weight: bold; }"
    ".sd-dark button.sd-tab { color: #93a0b8; } .sd-light button.sd-tab { color: #5a6680; }"
    ".sd-dark button.sd-tab:checked { color: @sd_accent; background-color: alpha(@sd_accent, 0.18); }"
    ".sd-light button.sd-tab:checked { color: @sd_strong; background-color: alpha(@sd_accent, 0.16); }"
    "button.sd-icon { padding: 3px 5px; min-width: 26px; min-height: 26px; }"
    ".sd-dark button.sd-icon { color: #c3cde0; } .sd-light button.sd-icon { color: #45506a; }"
    "button.sd-cat { padding: 2px 5px; font-size: 16px; }"
    "button.sd-cat.sd-cat-text { font-size: 11px; padding: 4px 6px; }"
    ".sd-dark button.sd-cat { color: #c3cde0; } .sd-light button.sd-cat { color: #45506a; }"
    ".sd-dark button.sd-cat.sd-cat-active { box-shadow: inset 0 -2px @sd_accent; }"
    ".sd-light button.sd-cat.sd-cat-active { box-shadow: inset 0 -2px @sd_strong; }"
    "button.sd-menuitem { padding: 6px 12px; }"
    "button.sd-tone { padding: 2px 4px; font-size: 20px; }"
    ".sd-dark button.sd-tab:hover, .sd-dark button.sd-icon:hover, .sd-dark button.sd-cat:hover,"
    " .sd-dark button.sd-menuitem:hover, .sd-dark button.sd-tone:hover { background-color: #1f2a3d; }"
    ".sd-light button.sd-tab:hover, .sd-light button.sd-icon:hover, .sd-light button.sd-cat:hover,"
    " .sd-light button.sd-menuitem:hover, .sd-light button.sd-tone:hover { background-color: #e8eef8; }"
    ".sd-dark button.sd-tone:checked { box-shadow: inset 0 0 0 2px @sd_accent; }"
    ".sd-light button.sd-tone:checked { box-shadow: inset 0 0 0 2px @sd_strong; }"
    "entry.sd-search { border-radius: 8px; min-height: 32px; background-image: none; box-shadow: none; }"
    ".sd-dark entry.sd-search { background-color: #1b2230; border: 1px solid #2a3449; color: #e6ebf5; }"
    ".sd-light entry.sd-search { background-color: #ffffff; border: 1px solid #d5dcea; color: #182033; }"
    ".sd-glass entry.sd-search { border-radius: 12px; }"
    ".sd-glass.sd-dark entry.sd-search { background-color: rgba(255,255,255,0.08); border-color: rgba(255,255,255,0.14); }"
    ".sd-glass.sd-light entry.sd-search { background-color: rgba(255,255,255,0.60); border-color: rgba(255,255,255,0.85); }"
    ".sd-dark entry.sd-search:focus { border-color: @sd_accent; } .sd-light entry.sd-search:focus { border-color: @sd_strong; }"
    ".sd-scroll, .sd-scroll viewport, .sd-catscroll, .sd-catscroll viewport { background-color: transparent; background-image: none; border: none; }"
    ".sd-status { font-size: 0.9em; padding: 0 4px; }"
    ".sd-dark .sd-status { color: #93a0b8; } .sd-light .sd-status { color: #5a6680; }"
    ".sd-dark popover.background { background-color: #1b2230; border-color: #2f3b55; color: #e6ebf5; }"
    ".sd-light popover.background { background-color: #ffffff; border-color: #d5dcea; color: #182033; }"
    ".sd-seg button { padding: 2px 8px; min-height: 0; }"
    ".sd-dark .sd-seg button:checked { background-image: none; background-color: @sd_strong; color: #ffffff; border-color: @sd_strong; }"
    ".sd-light .sd-seg button:checked { background-image: none; background-color: @sd_strong; color: #ffffff; border-color: shade(@sd_strong, 0.85); }"
    ".sd-settings label, .sd-settings button { font-size: 13px; }"
    ".sd-settings button { padding: 2px 9px; min-height: 24px; }"
    ".sd-settings switch { min-width: 34px; min-height: 18px; padding: 0; margin: 0; border-radius: 9px; }"
    ".sd-settings switch slider { min-width: 14px; min-height: 14px; margin: 2px; padding: 0;"
    "  border-radius: 7px; border-width: 0; box-shadow: none; }"
    ".sd-dark switch:checked { background-color: @sd_strong; border-color: @sd_strong; }"
    ".sd-light switch:checked { background-color: @sd_strong; border-color: shade(@sd_strong, 0.85); }"
    ".sd-settings .sd-hk { font-size: 11.5px; }"
    ".sd-dark .sd-hk { color: #93a0b8; } .sd-light .sd-hk { color: #5a6680; }"
    ".sd-dark .sd-hk.sd-ok { color: #4ade80; } .sd-light .sd-hk.sd-ok { color: #15803d; }"
    ".sd-dark .sd-hk.sd-err { color: #f87171; } .sd-light .sd-hk.sd-err { color: #b91c1c; }"
    ".sd-keys .sd-keydesc { font-size: 11px; }"
    ".sd-dark .sd-keydesc { color: #93a0b8; } .sd-light .sd-keydesc { color: #5a6680; }"
    ".sd-settings .sd-kbd { font-family: monospace; font-size: 10px; padding: 0 5px; border-radius: 4px; }"
    ".sd-dark .sd-kbd { background-color: #1f2738; border: 1px solid #33405a; color: #e6ebf5; }"
    ".sd-light .sd-kbd { background-color: #eef2f9; border: 1px solid #d0d9ea; color: #182033; }"
    ".sd-settings .sd-footbtn, .sd-settings .sd-footbtn label, .sd-settings .sd-ver { font-size: 11px; }"
    ".sd-settings button.sd-footbtn { padding: 1px 4px; min-height: 0; background-color: transparent;"
    "  background-image: none; border: none; box-shadow: none; text-shadow: none; border-radius: 5px; }"
    ".sd-dark .sd-footbtn, .sd-dark .sd-ver { color: #7d8aa3; } .sd-light .sd-footbtn, .sd-light .sd-ver { color: #66728c; }"
    ".sd-dark button.sd-footbtn:hover, .sd-dark button.sd-footbtn:checked { color: @sd_accent; background-color: #1f2a3d; }"
    ".sd-light button.sd-footbtn:hover, .sd-light button.sd-footbtn:checked { color: @sd_strong; background-color: #e8eef8; }"
    ".sd-banner { border-radius: 8px; padding: 8px 8px 8px 12px; }"
    ".sd-dark .sd-banner { background-color: alpha(@sd_accent, 0.14); border: 1px solid alpha(@sd_accent, 0.35); }"
    ".sd-light .sd-banner { background-color: alpha(@sd_accent, 0.10); border: 1px solid alpha(@sd_accent, 0.30); }"
    ".sd-banner label { font-size: 0.92em; }"
    ".sd-settings button.sd-sw { min-width: 18px; min-height: 18px; padding: 0; margin: 0 2px; border-radius: 11px;"
    "  border: 2px solid transparent; background-image: none; box-shadow: none; }"
    ".sd-dark .sd-settings button.sd-sw:checked { border-color: #e6ebf5; }"
    ".sd-light .sd-settings button.sd-sw:checked { border-color: #182033; }"
    "button.sd-primary { background-image: none; border: none; box-shadow: none; text-shadow: none;"
    "  border-radius: 6px; padding: 5px 14px; font-weight: bold; }"
    ".sd-dark button.sd-primary { background-color: @sd_accent; color: @sd_bg; }"
    ".sd-light button.sd-primary { background-color: @sd_strong; color: #ffffff; }";

static gboolean theme_is_dark(void) {
    if (!g_strcmp0(cfg.theme, "dark")) return TRUE;
    if (!g_strcmp0(cfg.theme, "light")) return FALSE;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    gboolean is_dark = FALSE;
    GSettingsSchema *s = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE) : NULL;
    if (s) {
        if (g_settings_schema_has_key(s, "color-scheme")) {
            GSettings *g = g_settings_new("org.gnome.desktop.interface");
            char *v = g_settings_get_string(g, "color-scheme");
            is_dark = !g_strcmp0(v, "prefer-dark");
            g_free(v); g_object_unref(g);
        }
        g_settings_schema_unref(s);
    }
    gboolean prefer_dark = FALSE;   /* set by KDE Plasma and others in GTK's settings.ini */
    g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
    is_dark = is_dark || prefer_dark;
    char *name = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-theme-name", &name, NULL);
    if (name) { char *l = g_ascii_strdown(name, -1); is_dark = is_dark || strstr(l, "dark"); g_free(l); g_free(name); }
    const char *schemas[] = { "org.cinnamon.desktop.interface", "org.mate.interface" };
    for (guint i = 0; i < 2 && !is_dark && src; i++) {
        s = g_settings_schema_source_lookup(src, schemas[i], TRUE);
        if (!s) continue;
        if (g_settings_schema_has_key(s, "gtk-theme")) {
            GSettings *g = g_settings_new(schemas[i]);
            char *v = g_settings_get_string(g, "gtk-theme"), *l = g_ascii_strdown(v, -1);
            is_dark = strstr(l, "dark") != NULL;
            g_free(v); g_free(l); g_object_unref(g);
        }
        g_settings_schema_unref(s);
    }
    return is_dark;
}

static void update_blur(void);

static void apply_theme(void) {
    dark = theme_is_dark();
    rm_class(win, dark ? "sd-light" : "sd-dark");
    add_class(win, dark ? "sd-dark" : "sd-light");
    int pi = 0;
    for (guint k = 0; k < G_N_ELEMENTS(PALS); k++) if (!g_strcmp0(cfg.color, PALS[k].id)) pi = k;
    const char *hex = dark ? PALS[pi].dark : PALS[pi].light;
    GdkRGBA c;
    if (gdk_rgba_parse(&c, hex)) { acc[0] = c.red; acc[1] = c.green; acc[2] = c.blue; }
    GString *css = g_string_new(NULL);
    g_string_append_printf(css, "@define-color sd_accent %s; @define-color sd_strong %s; @define-color sd_bg mix(%s, %s, %s);",
                           hex, PALS[pi].light, dark ? "#14161b" : "#f6f7f9", hex, dark ? "0.07" : "0.05");
    for (guint k = 0; k < G_N_ELEMENTS(PALS); k++)
        g_string_append_printf(css, ".sd-settings button.sd-sw%u { background-color: %s; }", k, dark ? PALS[k].dark : PALS[k].light);
    gtk_css_provider_load_from_data(pal_css, css->str, -1, NULL);
    g_string_free(css, TRUE);
    if (cfg.glass && !gtk_style_context_has_class(gtk_widget_get_style_context(win), "sd-nocomp")) add_class(win, "sd-glass");
    else rm_class(win, "sd-glass");
    update_blur();
    gtk_widget_queue_draw(area);
}

static GtkWidget *icon(const char *a, const char *b, const char *fallback) {
    GtkIconTheme *t = gtk_icon_theme_get_default();
    if (gtk_icon_theme_has_icon(t, a)) return gtk_image_new_from_icon_name(a, GTK_ICON_SIZE_MENU);
    if (b && gtk_icon_theme_has_icon(t, b)) return gtk_image_new_from_icon_name(b, GTK_ICON_SIZE_MENU);
    return gtk_label_new(fallback);
}

static gboolean any_popover_open(void) {
    return gtk_widget_get_visible(settings_pop) || gtk_widget_get_visible(tone_pop);
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
        gtk_window_move(GTK_WINDOW(win), panel_x = x, panel_y = y);
        return;
    }
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
    gtk_window_move(GTK_WINDOW(win), panel_x = x, panel_y = y);
}

/* ---- resize with the mouse (the panel has no title bar): drag any edge or corner */
static GdkWindowEdge edge_at(double xr, double yr, gboolean *hit) {
    int ox, oy, z = gtk_style_context_has_class(gtk_widget_get_style_context(win), "sd-nocomp") ? 8 : 16;
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
    if (e->type == GDK_2BUTTON_PRESS) {   /* double-click: back to the default size and place */
        cfg.pos_x = cfg.pos_y = -1; cfg.width = WIN_W; cfg.height = WIN_H;
        save_settings();
        place_window();
        geom_dirty = FALSE;
        return TRUE;
    }
    if (e->type != GDK_BUTTON_PRESS) return FALSE;
    geom_dirty = TRUE;
    gtk_window_begin_move_drag(GTK_WINDOW(win), e->button, (int)e->x_root, (int)e->y_root, e->time);
    return TRUE;
}

static gboolean on_configure(GtkWidget *w, GdkEventConfigure *e, gpointer d) {
    if (geom_dirty && gtk_widget_get_visible(win)) {
        gtk_window_get_position(GTK_WINDOW(win), &cfg.pos_x, &cfg.pos_y);
        panel_x = cfg.pos_x; panel_y = cfg.pos_y;
        gtk_window_get_size(GTK_WINDOW(win), &cfg.width, &cfg.height);
    }
    return FALSE;
}

static gboolean ensure_focus(gpointer ts) {
    if (gtk_widget_get_visible(win) && gtk_widget_get_visible(keys_win) && !gtk_window_is_active(GTK_WINDOW(keys_win)))
        gdk_window_focus(gtk_widget_get_window(keys_win), GPOINTER_TO_UINT(ts));
    return G_SOURCE_REMOVE;
}

/* The panel never takes focus itself (so it can stay on screen while your app receives the emoji).
 * This invisible 1-pixel window holds the keyboard for it and passes the keys on. */
static void grab_keys(void) {
    dropping = FALSE;
    gtk_window_move(GTK_WINDOW(keys_win), panel_x + cfg.width / 2, panel_y + cfg.height / 2);
    gtk_widget_show(keys_win);
    GdkWindow *gw = gtk_widget_get_window(keys_win);
    guint32 ts = GDK_IS_X11_WINDOW(gw) ? gdk_x11_get_server_time(gw) : GDK_CURRENT_TIME;
    gtk_window_present_with_time(GTK_WINDOW(keys_win), ts);
    g_timeout_add(60, ensure_focus, GUINT_TO_POINTER(ts));
    g_timeout_add(300, ensure_focus, GUINT_TO_POINTER(ts));
    g_timeout_add(700, ensure_focus, GUINT_TO_POINTER(ts));
}

static gboolean on_keys_event(GtkWidget *w, GdkEvent *e, gpointer d) { return gtk_widget_event(win, e); }

static void panel_active(gboolean in) {   /* draw the panel as focused (text cursor…) while we hold the keyboard */
    if (!gtk_widget_get_realized(win)) return;
    GdkEvent *ev = gdk_event_new(GDK_FOCUS_CHANGE);
    ev->focus_change.window = g_object_ref(gtk_widget_get_window(win));
    ev->focus_change.in = in;
    gtk_widget_send_focus_change(win, ev);
    gdk_event_free(ev);
}

/* KDE Plasma blurs what is behind the glass when asked to; other desktops ignore this. */
static void update_blur(void) {
    if (!gtk_widget_get_realized(win)) return;
    GdkWindow *gw = gtk_widget_get_window(win);
    Display *d = GDK_WINDOW_XDISPLAY(gw);
    Atom a = XInternAtom(d, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    if (gtk_style_context_has_class(gtk_widget_get_style_context(win), "sd-glass")) {
        int s = gdk_window_get_scale_factor(gw);
        unsigned long r[4] = { 10 * s, 10 * s, (gdk_window_get_width(gw) - 20) * s, (gdk_window_get_height(gw) - 20) * s };
        XChangeProperty(d, GDK_WINDOW_XID(gw), a, XA_CARDINAL, 32, PropModeReplace, (guchar *)r, 4);
    } else {
        XDeleteProperty(d, GDK_WINDOW_XID(gw), a);
    }
}

static void on_win_size(GtkWidget *w, GdkRectangle *a, gpointer d) { update_blur(); }

/* SUPERV_EMOJI_SNAPSHOT=file.png: for screenshots. Drawn into a picture, not read back from the screen. */
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

static void update_banner(void);
static void sync_hotkey_from_desktop(void);
static void hotkey_status(const char *text, gboolean ok);
static void update_hotkey_labels(void);
static void sync_settings(void);

static void show_popup(void) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkWindow *aw = gdk_screen_get_active_window(gdk_screen_get_default());
    G_GNUC_END_IGNORE_DEPRECATIONS
    /* on Wayland, _NET_ACTIVE_WINDOW is only set when an X11 (XWayland) app has focus */
    prev_is_x11 = aw && (!gtk_widget_get_realized(win) || aw != gtk_widget_get_window(win))
               && (!gtk_widget_get_realized(keys_win) || aw != gtk_widget_get_window(keys_win));
    if (aw) g_object_unref(aw);
    if (hide_src) { g_source_remove(hide_src); hide_src = 0; }
    take_snapshot(&snaps[0]);
    take_snapshot(&snaps[1]);
    reload_hotkey();
    sync_hotkey_from_desktop();
    apply_theme();
    capturing = FALSE;
    hotkey_status(NULL, TRUE);
    update_hotkey_labels();
    sync_settings();
    update_banner();
    if (*gtk_entry_get_text(GTK_ENTRY(search))) gtk_entry_set_text(GTK_ENTRY(search), "");
    build_cells();   /* recently used may have changed */
    place_window();
    gtk_widget_show(win);
    gdk_window_raise(gtk_widget_get_window(win));
    update_blur();
    grab_keys();
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(search));
    had_focus = FALSE;
    const char *snap = g_getenv("SUPERV_EMOJI_SNAPSHOT");
    if (snap && *snap) g_timeout_add(900, snapshot_window, (gpointer)snap);
}

static void toggle(void) { if (gtk_widget_get_visible(win)) gtk_widget_hide(win); else show_popup(); }

/* Like Windows: a click anywhere outside closes the panel. */
static gboolean hide_if_inactive(gpointer d) {
    hide_src = 0;
    if (gtk_widget_get_visible(win) && !dropping && !gtk_window_is_active(GTK_WINDOW(keys_win))) {
        if (sv_debug) logmsg("panel lost focus: closing");
        if (capturing) { capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels(); }
        gtk_widget_hide(settings_pop);
        gtk_widget_hide(tone_pop);
        gtk_widget_hide(win);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_focus_in(GtkWidget *w, GdkEvent *e, gpointer d) { had_focus = TRUE; panel_active(TRUE); return FALSE; }
static gboolean on_focus_out(GtkWidget *w, GdkEvent *e, gpointer d) {
    panel_active(FALSE);
    if (dropping) return FALSE;
    if (hide_src) g_source_remove(hide_src);
    hide_src = had_focus ? g_timeout_add(120, hide_if_inactive, NULL) : 0;
    return FALSE;
}

static gboolean trim_memory(gpointer d) { malloc_trim(0); return G_SOURCE_REMOVE; }

/* We hold your restored clipboard: hand it to the desktop's clipboard manager before exiting. */
static void store_clipboard(void) {
    if (gtk_clipboard_get_owner(clip) != G_OBJECT(sv_app)) return;
    gtk_clipboard_set_can_store(clip, NULL, 0);
    gtk_clipboard_store(clip);
}

static void on_hide(GtkWidget *w, gpointer d) {
    dropping = FALSE;
    gtk_widget_hide(keys_win);
    if (geom_dirty) { geom_dirty = FALSE; save_settings(); }
    g_timeout_add(500, trim_memory, NULL);
}

/* ------------------------------------------------------------------ shortcut */

static char *accel_label(const char *accel) {
    guint key; GdkModifierType mods;
    gtk_accelerator_parse(accel, &key, &mods);
    return key ? gtk_accelerator_get_label(key, mods) : g_strdup(accel);
}

static void update_hotkey_labels(void) {
    char *label = accel_label(cfg.hotkey);
    if (!capturing) gtk_button_set_label(GTK_BUTTON(hotkey_btn), label);
    gtk_label_set_text(GTK_LABEL(hint_lbl), label);
    g_free(label);
}

static void hotkey_status(const char *text, gboolean ok) {
    if (!hk_status) return;
    rm_class(hk_status, ok ? "sd-err" : "sd-ok");
    add_class(hk_status, ok ? "sd-ok" : "sd-err");
    gtk_label_set_text(GTK_LABEL(hk_status), text);
    gtk_widget_set_visible(hk_status, text != NULL);
}

static char *setup_path(void) { return sv_setup_path("superv-feature-setup"); }

static gint64 hotkey_applied_at;

static void on_hotkey_applied(gboolean ok, gpointer accel) {
    if (!*(char *)accel) {   /* the shortcut was given back to the desktop */
        cfg.setup_done = FALSE;
    } else if (ok) {
        cfg.setup_done = TRUE;   /* the helper saved it too */
        char *label = accel_label(accel), *msg = g_strdup_printf("✓ Shortcut set to %s", label);
        hotkey_status(msg, TRUE);
        g_free(msg); g_free(label);
    } else {
        hotkey_status("Couldn't save the shortcut. Run “superv --doctor” to see why.", FALSE);
    }
    g_free(accel);
}

/* accel "" removes the shortcut (and gives the desktop back what we freed for it).
 * SuperV runs setup helpers one at a time, in order. */
static void run_helper(const char *accel) {
    char *setup = setup_path();
    char *quoted = g_shell_quote(sv_self_exe), *cmd = g_strdup_printf("%s --emoji", quoted);
    char *set_argv[] = { "python3", setup, "--feature", "emoji", "--setup-hotkeys", cmd, "--hotkey", (char *)accel, NULL };
    char *rm_argv[] = { "python3", setup, "--feature", "emoji", "--remove-hotkeys", NULL };
    sv_run_helper(*accel ? set_argv : rm_argv, on_hotkey_applied, g_strdup(accel));
    g_free(setup); g_free(quoted); g_free(cmd);
}

static void apply_hotkey(const char *new_accel) {
    char *accel = g_strdup(new_accel);   /* new_accel may be cfg.hotkey itself */
    g_free(cfg.hotkey);
    cfg.hotkey = accel;
    hotkey_applied_at = g_get_monotonic_time();
    config_save();
    run_helper(accel);
}

#define GNOME_KB_SCHEMA "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding"
#define GNOME_KB_PATH   "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/superv-emoji/"
#define CIN_KB_SCHEMA   "org.cinnamon.desktop.keybindings.custom-keybinding"

static gboolean schema_has_key(const char *schema_id, const char *key) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, schema_id, TRUE) : NULL;
    gboolean ok = schema && g_settings_schema_has_key(schema, key);
    if (schema) g_settings_schema_unref(schema);
    return ok;
}

static gboolean gnome_bound(void) {
    if (!schema_has_key("org.gnome.settings-daemon.plugins.media-keys", "custom-keybindings")) return TRUE;
    GSettings *s = g_settings_new("org.gnome.settings-daemon.plugins.media-keys");
    char **v = g_settings_get_strv(s, "custom-keybindings");
    gboolean found = g_strv_contains((const char *const *)v, GNOME_KB_PATH);
    g_strfreev(v); g_object_unref(s);
    return found;
}

static char *gnome_hotkey(void) {
    if (!schema_has_key("org.gnome.settings-daemon.plugins.media-keys", "custom-keybindings") || !gnome_bound())
        return NULL;
    GSettings *kb = g_settings_new_with_path(GNOME_KB_SCHEMA, GNOME_KB_PATH);
    char *b = g_settings_get_string(kb, "binding");
    g_object_unref(kb);
    if (!*b) { g_free(b); return NULL; }
    return b;
}

/* Cinnamon: the emoji panel's custom binding (found = TRUE when there is one), and its key */
static char *cinnamon_hotkey(gboolean *found) {
    *found = FALSE;
    if (!schema_has_key("org.cinnamon.desktop.keybindings", "custom-list")) { *found = TRUE; return NULL; }
    GSettings *s = g_settings_new("org.cinnamon.desktop.keybindings");
    char **ids = g_settings_get_strv(s, "custom-list"), *key = NULL;
    for (int i = 0; ids[i] && !*found; i++) {
        char *path = g_strdup_printf("/org/cinnamon/desktop/keybindings/custom-keybindings/%s/", ids[i]);
        GSettings *kb = g_settings_new_with_path(CIN_KB_SCHEMA, path);
        char *name = g_settings_get_string(kb, "name");
        if (!g_strcmp0(name, "SuperV Emoji")) {
            *found = TRUE;
            GVariant *v = g_settings_get_value(kb, "binding");
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
                const char **b = g_variant_get_strv(v, NULL);
                if (b[0] && *b[0]) key = g_strdup(b[0]);
                g_free(b);
            } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) && *g_variant_get_string(v, NULL)) {
                key = g_variant_dup_string(v, NULL);
            }
            g_variant_unref(v);
        }
        g_free(name); g_object_unref(kb); g_free(path);
    }
    g_strfreev(ids); g_object_unref(s);
    return key;
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

/* Show the shortcut the desktop really uses: people also change it in their desktop's settings. */
static void sync_hotkey_from_desktop(void) {
    if (g_get_monotonic_time() - hotkey_applied_at < 5 * G_USEC_PER_SEC) return;   /* ours is still being applied */
    gboolean found;
    int kind = desktop_kind();
    char *d = kind == DE_GNOME ? gnome_hotkey() : kind == DE_CINNAMON ? cinnamon_hotkey(&found) : NULL;
    if (d && g_strcmp0(d, cfg.hotkey)) { g_free(cfg.hotkey); cfg.hotkey = d; config_save(); }
    else g_free(d);
}

/* Is the emoji panel's shortcut registered with the desktop? (TRUE when we can't tell: KDE, MATE, Xfce…) */
static gboolean shortcut_bound(void) {
    gboolean found = TRUE;
    switch (desktop_kind()) {
    case DE_GNOME: return gnome_bound();
    case DE_CINNAMON: g_free(cinnamon_hotkey(&found)); return found;
    default: return TRUE;
    }
}

static void on_hotkey_btn(GtkButton *b, gpointer d) {
    capturing = TRUE;
    gtk_button_set_label(b, "Press keys…");
    hotkey_status("Press the new shortcut, e.g. Super+. or Ctrl+Alt+E. Esc cancels.", TRUE);
    rm_class(hk_status, "sd-ok");
}

static gboolean capture_key(GdkEventKey *e) {
    if (e->is_modifier) return TRUE;
    if (e->keyval == GDK_KEY_Escape) { capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels(); return TRUE; }
    GdkModifierType state = e->state;
    gdk_keymap_add_virtual_modifiers(gdk_keymap_get_for_display(gdk_display_get_default()), &state);
    state &= gtk_accelerator_get_default_mod_mask() & ~(GDK_HYPER_MASK | GDK_META_MASK);
    guint key = gdk_keyval_to_lower(e->keyval);
    /* Must use Super, or Ctrl+Alt: Ctrl+…, Alt+… belong to apps (copy, paste, menus). */
    gboolean ctrl = state & GDK_CONTROL_MASK, alt = state & GDK_MOD1_MASK, super = state & GDK_SUPER_MASK;
    if (!super && !(ctrl && alt)) {
        char *name = gtk_accelerator_name(key, state), *label = accel_label(name);
        char *msg = g_strdup_printf("%s isn't allowed: apps use %s shortcuts. Use Super, or Ctrl+Alt, plus a key.",
                                    label, ctrl ? "Ctrl" : alt ? "Alt" : "single-key");
        hotkey_status(msg, FALSE);
        g_free(msg); g_free(label); g_free(name);
        return TRUE;   /* keep listening for a valid combination */
    }
    capturing = FALSE;
    hotkey_status("Saving…", TRUE);
    rm_class(hk_status, "sd-ok");
    char *accel = gtk_accelerator_name(key, state);
    apply_hotkey(accel);
    g_free(accel);
    update_hotkey_labels();
    return TRUE;
}

/* ------------------------------------------------------------------ keyboard */

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    if (capturing) return capture_key(e);
    if (any_popover_open()) return FALSE;
    gboolean shift = (e->state & GDK_SHIFT_MASK) != 0, ctrl = (e->state & GDK_CONTROL_MASK) != 0;
    gboolean empty = !*gtk_entry_get_text(GTK_ENTRY(search));
    int page = MAX(1, (int)(gtk_adjustment_get_page_size(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll))) / 39) - 1);
    switch (e->keyval) {
    case GDK_KEY_Escape:
        if (!empty) gtk_entry_set_text(GTK_ENTRY(search), "");
        else gtk_widget_hide(win);
        return TRUE;
    case GDK_KEY_Left: case GDK_KEY_KP_Left:   select_cell(sel - 1); return TRUE;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: select_cell(sel + 1); return TRUE;
    case GDK_KEY_Up: case GDK_KEY_KP_Up:       move_rows(-1); return TRUE;
    case GDK_KEY_Down: case GDK_KEY_KP_Down:   move_rows(1); return TRUE;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up:     move_rows(-page); return TRUE;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: move_rows(page); return TRUE;
    case GDK_KEY_Home: if (empty || ctrl) { select_cell(0); return TRUE; } break;
    case GDK_KEY_End:  if (empty || ctrl) { select_cell(cells->len - 1); return TRUE; } break;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter:
        if (g_strcmp0(built_query, gtk_entry_get_text(GTK_ENTRY(search)))) build_cells();   /* typed < 150 ms ago */
        pick(sel, shift);
        return TRUE;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: case GDK_KEY_KP_Tab:
        set_tab((cur_tab + (e->keyval == GDK_KEY_ISO_Left_Tab || shift ? N_TABS - 1 : 1)) % N_TABS);
        return TRUE;
    }
    if (gtk_window_get_focus(GTK_WINDOW(win)) != search && !ctrl && gdk_keyval_to_unicode(e->keyval)) {
        gtk_entry_grab_focus_without_selecting(GTK_ENTRY(search));   /* typing always searches */
        return gtk_widget_event(search, (GdkEvent *)e);
    }
    return FALSE;
}

/* ------------------------------------------------------------------ settings + skin tone popovers */

static void on_tone(GtkButton *b, gpointer t) {
    if (syncing) return;   /* set_active() emits "clicked" too */
    cfg.skin_tone = GPOINTER_TO_INT(t);
    save_settings();
    gtk_popover_popdown(GTK_POPOVER(tone_pop));
    sync_settings();
    update_status();
    gtk_widget_queue_draw(area);
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(search));
}

static char *tone_hand(int t) {   /* ✋ with skin tone t */
    char s[16];
    int n = g_unichar_to_utf8(0x270B, s);
    if (t) n += g_unichar_to_utf8(0x1F3FA + t, s + n);
    s[n] = 0;
    return g_strdup(s);
}

static GtkWidget *tone_buttons[6];

static GtkWidget *tone_menu(void) {
    tone_btn = gtk_menu_button_new();
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(tone_btn));
    if (old) gtk_container_remove(GTK_CONTAINER(tone_btn), old);
    tone_lbl = gtk_label_new("");
    gtk_container_add(GTK_CONTAINER(tone_btn), tone_lbl);
    add_class(tone_btn, "sd-icon");
    gtk_widget_set_tooltip_text(tone_btn, "Skin tone");
    gtk_widget_set_can_focus(tone_btn, FALSE);
    tone_pop = gtk_popover_new(tone_btn);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    static const char *names[] = { "Default", "Light", "Medium-light", "Medium", "Medium-dark", "Dark" };
    for (int t = 0; t < 6; t++) {
        char *hand = tone_hand(t);
        GtkWidget *b = gtk_toggle_button_new_with_label(hand);
        g_free(hand);
        add_class(b, "sd-tone");
        gtk_widget_set_tooltip_text(b, names[t]);
        g_signal_connect(b, "clicked", G_CALLBACK(on_tone), GINT_TO_POINTER(t));
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
        tone_buttons[t] = b;
    }
    gtk_widget_show_all(box);
    gtk_container_add(GTK_CONTAINER(tone_pop), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(tone_btn), tone_pop);
    return tone_btn;
}

static void on_theme_toggled(GtkToggleButton *b, gpointer which) {
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    g_free(cfg.theme);
    cfg.theme = g_strdup(which);
    save_settings();
    apply_theme();
}

static void on_paste_mode(GtkToggleButton *b, gpointer which) {
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    g_free(cfg.paste_keys);
    cfg.paste_keys = g_strdup(which);
    save_settings();
    update_banner();
}

static gboolean on_keep_open(GtkSwitch *s, gboolean on, gpointer d) {
    if (!syncing) { cfg.keep_open = on; save_settings(); }
    return FALSE;
}

static void on_palette(GtkToggleButton *b, gpointer id) {
    if (syncing || !gtk_toggle_button_get_active(b)) return;
    g_free(cfg.color);
    cfg.color = g_strdup(id);
    save_settings();
    apply_theme();
}

static GtkWidget *palette_control(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GSList *group = NULL;
    for (guint k = 0; k < G_N_ELEMENTS(PALS); k++) {
        char cls[16];
        g_snprintf(cls, sizeof cls, "sd-sw%u", k);
        GtkWidget *b = pal_btns[k] = gtk_radio_button_new(group);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(b));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
        add_class(b, "sd-sw");
        add_class(b, cls);
        gtk_widget_set_tooltip_text(b, PALS[k].name);
        g_signal_connect(b, "toggled", G_CALLBACK(on_palette), (gpointer)PALS[k].id);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    }
    return box;
}

static gboolean on_glass(GtkSwitch *s, gboolean on, gpointer d) {
    if (!syncing) { cfg.glass = on; save_settings(); apply_theme(); }
    return FALSE;
}

static gboolean on_restore(GtkSwitch *s, gboolean on, gpointer d) {
    if (!syncing) { cfg.restore_clipboard = on; save_settings(); }
    return FALSE;
}

static void on_clear_recent(GtkButton *b, gpointer d) {
    g_ptr_array_set_size(recent, 0);
    recent_save();
    gtk_popover_popdown(GTK_POPOVER(settings_pop));
    build_cells();
}

static void on_reset(GtkButton *b, gpointer d) {
    reload_hotkey();
    config_defaults();
    if (g_strcmp0(cfg.hotkey, DEFAULT_HOTKEY)) apply_hotkey(DEFAULT_HOTKEY);   /* also saves */
    else config_save();
    apply_theme();
    sync_settings();
    place_window();
    geom_dirty = FALSE;
    set_tab(cfg.tab);
}

static void sync_settings(void) {   /* the controls show what's saved */
    syncing = TRUE;
    static const char *themes[] = { "auto", "light", "dark" };
    for (int i = 0; i < 3; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(theme_btns[i]), !g_strcmp0(cfg.theme, themes[i]));
    gboolean copy_only = !g_strcmp0(cfg.paste_keys, "none");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(paste_btns[copy_only]), TRUE);
    gtk_switch_set_active(GTK_SWITCH(keep_sw), cfg.keep_open);
    gtk_switch_set_active(GTK_SWITCH(restore_sw), cfg.restore_clipboard);
    gtk_switch_set_active(GTK_SWITCH(glass_sw), cfg.glass);
    int pi = 0;
    for (guint k = 0; k < G_N_ELEMENTS(PALS); k++) if (!g_strcmp0(cfg.color, PALS[k].id)) pi = k;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pal_btns[pi]), TRUE);
    for (int t = 0; t < 6; t++) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tone_buttons[t]), t == cfg.skin_tone);
    char *hand = tone_hand(cfg.skin_tone);
    gtk_label_set_text(GTK_LABEL(tone_lbl), hand);
    g_free(hand);
    syncing = FALSE;
}

static GtkWidget *segmented(const char **names, const char **values, int n, GtkWidget **out, GCallback cb) {
    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(seg, "linked");
    add_class(seg, "sd-seg");
    GSList *group = NULL;
    for (int i = 0; i < n; i++) {
        out[i] = gtk_radio_button_new_with_label(group, names[i]);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(out[i]));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(out[i]), FALSE);   /* look like buttons */
        g_signal_connect(out[i], "toggled", cb, (gpointer)values[i]);
        gtk_box_pack_start(GTK_BOX(seg), out[i], FALSE, FALSE, 0);
    }
    return seg;
}

static GtkWidget *settings_row(GtkWidget *box, const char *label, const char *tip, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16), *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (tip) gtk_widget_set_tooltip_text(row, tip);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}

static GtkWidget *settings_scroll;

static gboolean scroll_settings_down(gpointer d) {   /* bring the shortcut list into view */
    GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(settings_scroll));
    gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a));
    return G_SOURCE_REMOVE;
}

/* The menu is never taller than the panel: it scrolls inside instead of spilling out. */
static void fit_settings(GtkWidget *pop, gpointer d) {
    int h = gtk_widget_get_allocated_height(win) - 110;
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(settings_scroll), MAX(h, 160));
}

static void on_keys_toggled(GtkToggleButton *b, gpointer d) {
    gboolean open = gtk_toggle_button_get_active(b);
    gtk_revealer_set_reveal_child(GTK_REVEALER(keys_revealer), open);
    if (open) g_idle_add(scroll_settings_down, NULL);
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
    static const char *rows_[][2] = {
        { NULL,          "Open or close the emoji panel" },
        { "Type",        "Search" },
        { "← ↑ ↓ →",     "Choose" },
        { "Enter",       "Insert it" },
        { "Shift+Enter", "Insert and close" },
        { "Tab",         "Emoji · Kaomoji · Symbols" },
        { "Esc",         "Clear the search, then close" },
    };
    GtkWidget *grid = gtk_grid_new();
    add_class(grid, "sd-keys");
    gtk_widget_set_margin_bottom(grid, 4);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    for (int i = 0; i < (int)G_N_ELEMENTS(rows_); i++) {
        GtkWidget *key = gtk_label_new(rows_[i][0] ? rows_[i][0] : ""), *what = gtk_label_new(rows_[i][1]);
        if (!rows_[i][0]) hint_lbl = key;   /* shows the current shortcut */
        add_class(key, "sd-kbd");
        gtk_widget_set_halign(key, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(what), 0);
        add_class(what, "sd-keydesc");
        gtk_grid_attach(GTK_GRID(grid), key, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), what, 1, i, 1, 1);
    }
    return grid;
}

static void on_popover_closed(GtkPopover *p, gpointer d) {
    if (capturing) { capturing = FALSE; hotkey_status(NULL, TRUE); update_hotkey_labels(); }
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(search));
}

static GtkWidget *settings_button(void) {
    GtkWidget *btn = gtk_menu_button_new(), *old = gtk_bin_get_child(GTK_BIN(btn));
    if (old) gtk_container_remove(GTK_CONTAINER(btn), old);
    gtk_container_add(GTK_CONTAINER(btn), icon("emblem-system-symbolic", "preferences-system-symbolic", "⚙"));
    add_class(btn, "sd-icon");
    gtk_widget_set_tooltip_text(btn, "Settings");
    gtk_widget_set_can_focus(btn, FALSE);
    settings_pop = gtk_popover_new(btn);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    add_class(box, "sd-settings");

    hotkey_btn = gtk_button_new_with_label(cfg.hotkey);
    gtk_widget_set_tooltip_text(hotkey_btn, "Click, then press the key combination you want");
    g_signal_connect(hotkey_btn, "clicked", G_CALLBACK(on_hotkey_btn), NULL);
    settings_row(box, "Shortcut", NULL, hotkey_btn);
    hk_status = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(hk_status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hk_status), 34);
    gtk_label_set_xalign(GTK_LABEL(hk_status), 0);
    add_class(hk_status, "sd-hk");
    gtk_widget_set_no_show_all(hk_status, TRUE);
    gtk_box_pack_start(GTK_BOX(box), hk_status, FALSE, FALSE, 0);

    static const char *pn[] = { "Type it", "Copy only" }, *pv[] = { "shift+insert", "none" };
    settings_row(box, "When you pick one", "Type it: inserted where you were typing.\n"
                 "Copy only: put on the clipboard, you press Ctrl+V.",
                 segmented(pn, pv, 2, paste_btns, G_CALLBACK(on_paste_mode)));
    keep_sw = gtk_switch_new();
    g_signal_connect(keep_sw, "state-set", G_CALLBACK(on_keep_open), NULL);
    settings_row(box, "Stay open after picking", "Pick several in a row; Esc closes. Shift+click does the opposite.", keep_sw);
    restore_sw = gtk_switch_new();
    g_signal_connect(restore_sw, "state-set", G_CALLBACK(on_restore), NULL);
    settings_row(box, "Keep my clipboard", "After typing the emoji, put back what you had copied.", restore_sw);
    settings_row(box, "Colour", "Pick the palette that suits you; the glass takes its tint.", palette_control());
    glass_sw = gtk_switch_new();
    g_signal_connect(glass_sw, "state-set", G_CALLBACK(on_glass), NULL);
    settings_row(box, "Glass effect", "See-through panel. KDE Plasma also blurs what is behind it.", glass_sw);
    static const char *tn[] = { "System", "Light", "Dark" }, *tv[] = { "auto", "light", "dark" };
    settings_row(box, "Theme", NULL, segmented(tn, tv, 3, theme_btns, G_CALLBACK(on_theme_toggled)));

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(actions), TRUE);
    GtkWidget *clear = gtk_button_new_with_label("Clear recently used");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_recent), NULL);
    GtkWidget *reset = gtk_button_new_with_label("Reset settings");
    gtk_widget_set_tooltip_text(reset, "Back to defaults, including the Super+. shortcut");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), NULL);
    gtk_box_pack_start(GTK_BOX(actions), clear, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), reset, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    keys_revealer = gtk_revealer_new();
    /* no slide animation: a popover that changes size every frame jumps around */
    gtk_revealer_set_transition_type(GTK_REVEALER(keys_revealer), GTK_REVEALER_TRANSITION_TYPE_NONE);
    gtk_container_add(GTK_CONTAINER(keys_revealer), keys_help());
    gtk_box_pack_start(GTK_BOX(box), keys_revealer, FALSE, FALSE, 0);
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *keys = gtk_toggle_button_new_with_label("Shortcuts");
    gtk_button_set_image(GTK_BUTTON(keys), gtk_image_new_from_icon_name("pan-end-symbolic", GTK_ICON_SIZE_MENU));
    gtk_button_set_image_position(GTK_BUTTON(keys), GTK_POS_RIGHT);
    gtk_button_set_always_show_image(GTK_BUTTON(keys), TRUE);
    g_signal_connect(keys, "toggled", G_CALLBACK(on_keys_toggled), NULL);
    GtkWidget *ver = gtk_label_new("v" VERSION " ·");
    GtkWidget *repo = gtk_button_new_with_label("GitHub ↗");
    gtk_widget_set_tooltip_text(repo, REPO_URL);
    g_signal_connect(repo, "clicked", G_CALLBACK(on_open_repo), NULL);
    add_class(keys, "sd-footbtn");
    add_class(repo, "sd-footbtn");
    add_class(ver, "sd-ver");
    gtk_box_pack_start(GTK_BOX(foot), keys, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), repo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), ver, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), foot, FALSE, FALSE, 0);
    settings_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(settings_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(settings_scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(settings_scroll), TRUE);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(settings_scroll), FALSE);
    add_class(settings_scroll, "sd-scroll");
    gtk_container_add(GTK_CONTAINER(settings_scroll), box);
    gtk_widget_show_all(settings_scroll);
    gtk_container_add(GTK_CONTAINER(settings_pop), settings_scroll);
    g_signal_connect(settings_pop, "show", G_CALLBACK(fit_settings), NULL);
    g_signal_connect(settings_pop, "closed", G_CALLBACK(on_popover_closed), NULL);
    g_signal_connect(tone_pop, "closed", G_CALLBACK(on_popover_closed), NULL);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), settings_pop);
    return btn;
}

/* ---- one-time permission to type into Wayland apps, with the desktop's own password dialog */
enum { BANNER_ASK, BANNER_FAILED, BANNER_RELOGIN };
static int banner_state = BANNER_ASK, enable_tries;

static gboolean rule_installed(void) {   /* by a package, install.sh, or the Enable button (SuperDot 1.0 too) */
    static const char *rules[] = {
        "/usr/lib/udev/rules.d/70-superdot-uinput.rules", "/lib/udev/rules.d/70-superdot-uinput.rules",
        "/usr/local/lib/udev/rules.d/70-superdot-uinput.rules", "/etc/udev/rules.d/70-superdot-uinput.rules",
        "/usr/lib/udev/rules.d/70-superv-uinput.rules", "/etc/udev/rules.d/70-superv-uinput.rules",
    };
    for (guint i = 0; i < G_N_ELEMENTS(rules); i++) if (g_file_test(rules[i], G_FILE_TEST_EXISTS)) return TRUE;
    return FALSE;
}

static void update_banner(void) {
    gboolean show = !sv_paste_everywhere() && g_strcmp0(cfg.paste_keys, "none");
    if (show && banner_state == BANNER_ASK && rule_installed()) banner_state = BANNER_RELOGIN;
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
        gtk_label_set_text(GTK_LABEL(banner_lbl), "Typing emoji into apps needs a one-time permission. "
                           "Until then, picking one copies it: press Ctrl+V.");
        gtk_button_set_label(GTK_BUTTON(banner_btn), "Enable");
    }
    gtk_widget_set_visible(banner_btn, TRUE);
    gtk_widget_set_sensitive(banner_btn, TRUE);
}

static gboolean retry_uinput(gpointer d) {
    if (sv_uinput_open()) {
        enabling = FALSE;
        update_banner();
        notify("SuperV types for you now", "Pick an emoji and it appears where you were typing.");
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
    gtk_label_set_text(GTK_LABEL(banner_lbl), "Enter your password in the dialog…");
    g_child_watch_add(pid, on_enable_done, NULL);
}

/* ------------------------------------------------------------------ window */

static void on_clip_btn(GtkButton *b, gpointer d) { gtk_widget_hide(win); sv_show_clipboard(); }

static void build_window(void) {
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(win);
    gtk_window_set_title(w, "Emoji");
    gtk_window_set_decorated(w, FALSE);
    gtk_window_set_resizable(w, TRUE);
    gtk_window_set_default_size(w, cfg.width, cfg.height);
    gtk_widget_set_size_request(win, MIN_W, MIN_H);
    gtk_widget_add_events(win, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
    gtk_window_set_keep_above(w, TRUE);
    gtk_window_set_accept_focus(w, FALSE);   /* see grab_keys() */
    gtk_window_set_skip_taskbar_hint(w, TRUE);
    gtk_window_set_skip_pager_hint(w, TRUE);
    gtk_window_set_type_hint(w, GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_icon_name(w, APP_ID);
    GdkScreen *scr = gtk_widget_get_screen(win);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis && gdk_screen_is_composited(scr)) { gtk_widget_set_visual(win, vis); gtk_widget_set_app_paintable(win, TRUE); }
    else add_class(win, "sd-nocomp");
    add_class(win, "sd-window");
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    pal_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(pal_css, "@define-color sd_accent #7cb4ff; @define-color sd_strong #2563eb;"
                                    "@define-color sd_bg #14161b;", -1, NULL);
    gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(pal_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_class(panel, "sd-panel");
    gtk_container_add(GTK_CONTAINER(win), panel);

    /* top row: tabs (drag the empty space to move the panel), skin tone, settings */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GSList *group = NULL;
    for (int t = 0; t < N_TABS; t++) {
        tab_btns[t] = gtk_radio_button_new_with_label(group, TAB_NAMES[t]);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(tab_btns[t]));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(tab_btns[t]), FALSE);
        gtk_widget_set_can_focus(tab_btns[t], FALSE);
        add_class(tab_btns[t], "sd-tab");
        gtk_widget_set_tooltip_text(tab_btns[t], "Tab switches between Emoji, Kaomoji and Symbols");
        g_signal_connect(tab_btns[t], "toggled", G_CALLBACK(on_tab_toggled), GINT_TO_POINTER(t));
        gtk_box_pack_start(GTK_BOX(top), tab_btns[t], FALSE, FALSE, 0);
    }
    GtkWidget *tones = tone_menu();   /* settings_button() needs tone_pop */
    gtk_box_pack_end(GTK_BOX(top), settings_button(), FALSE, FALSE, 0);
    GtkWidget *clip_btn = gtk_button_new();   /* back to the clipboard history, like Windows 11's Win+V tabs */
    gtk_container_add(GTK_CONTAINER(clip_btn), icon("edit-paste-symbolic", "edit-copy-symbolic", "📋"));
    add_class(clip_btn, "sd-icon");
    gtk_widget_set_can_focus(clip_btn, FALSE);
    gtk_widget_set_tooltip_text(clip_btn, "Clipboard history");
    g_signal_connect(clip_btn, "clicked", G_CALLBACK(on_clip_btn), NULL);
    gtk_box_pack_end(GTK_BOX(top), clip_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), tones, FALSE, FALSE, 0);
    GtkWidget *grab = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(grab), FALSE);
    gtk_container_add(GTK_CONTAINER(grab), top);
    g_signal_connect(grab, "button-press-event", G_CALLBACK(on_header_press), NULL);
    gtk_box_pack_start(GTK_BOX(panel), grab, FALSE, FALSE, 0);

    search = gtk_search_entry_new();
    add_class(search, "sd-search");
    g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(panel), search, FALSE, FALSE, 0);

    banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    add_class(banner, "sd-banner");
    banner_lbl = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(banner_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(banner_lbl), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(banner_lbl), 30);
    banner_btn = gtk_button_new_with_label("Enable");
    add_class(banner_btn, "sd-primary");
    gtk_widget_set_valign(banner_btn, GTK_ALIGN_CENTER);
    g_signal_connect(banner_btn, "clicked", G_CALLBACK(on_enable_paste), NULL);
    gtk_box_pack_start(GTK_BOX(banner), banner_lbl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(banner), banner_btn, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(banner, TRUE);
    gtk_widget_show(banner_lbl); gtk_widget_show(banner_btn);
    gtk_box_pack_start(GTK_BOX(panel), banner, FALSE, FALSE, 0);

    /* the grid: one drawing area (fast with thousands of emoji), only visible rows are drawn */
    area = gtk_drawing_area_new();
    gtk_widget_add_events(area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(area, "button-press-event", G_CALLBACK(on_area_press), NULL);
    g_signal_connect(area, "motion-notify-event", G_CALLBACK(on_area_motion), NULL);
    g_signal_connect(area, "leave-notify-event", G_CALLBACK(on_area_leave), NULL);
    g_signal_connect(area, "size-allocate", G_CALLBACK(on_area_size), NULL);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    add_class(scroll, "sd-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), area);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), "value-changed",
                     G_CALLBACK(on_scrolled), NULL);
    gtk_box_pack_start(GTK_BOX(panel), scroll, TRUE, TRUE, 0);

    status_lbl = gtk_label_new(" ");
    gtk_label_set_xalign(GTK_LABEL(status_lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(status_lbl), PANGO_ELLIPSIZE_END);
    add_class(status_lbl, "sd-status");
    gtk_box_pack_start(GTK_BOX(panel), status_lbl, FALSE, FALSE, 0);

    cat_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
    cat_scroll = gtk_scrolled_window_new(NULL, NULL);
    add_class(cat_scroll, "sd-catscroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cat_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(cat_scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(cat_scroll), cat_box);
    gtk_box_pack_start(GTK_BOX(panel), cat_scroll, FALSE, FALSE, 0);

    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(win, "size-allocate", G_CALLBACK(on_win_size), NULL);
    keys_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *kw = GTK_WINDOW(keys_win);
    gtk_window_set_title(kw, "SuperV Emoji");
    gtk_window_set_decorated(kw, FALSE);
    gtk_window_set_default_size(kw, 1, 1);
    gtk_window_set_skip_taskbar_hint(kw, TRUE);
    gtk_window_set_skip_pager_hint(kw, TRUE);
    gtk_window_set_type_hint(kw, GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_icon_name(kw, APP_ID);
    if (vis && gdk_screen_is_composited(scr)) { gtk_widget_set_visual(keys_win, vis); gtk_widget_set_app_paintable(keys_win, TRUE); }
    add_class(keys_win, "sd-window");
    g_signal_connect(keys_win, "key-press-event", G_CALLBACK(on_keys_event), NULL);
    g_signal_connect(keys_win, "key-release-event", G_CALLBACK(on_keys_event), NULL);
    g_signal_connect(keys_win, "focus-in-event", G_CALLBACK(on_focus_in), NULL);
    g_signal_connect(keys_win, "focus-out-event", G_CALLBACK(on_focus_out), NULL);
    g_signal_connect(keys_win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    g_signal_connect(win, "hide", G_CALLBACK(on_hide), NULL);
    g_signal_connect(win, "configure-event", G_CALLBACK(on_configure), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(on_win_press), NULL);
    g_signal_connect(win, "motion-notify-event", G_CALLBACK(on_win_motion), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    gtk_widget_show_all(panel);

    lay = gtk_widget_create_pango_layout(area, NULL);
    chk_lay = gtk_widget_create_pango_layout(area, NULL);
    PangoFontDescription *fd = pango_font_description_from_string("emoji");
    pango_font_description_set_absolute_size(fd, 23 * PANGO_SCALE);
    pango_layout_set_font_description(chk_lay, fd);
    pango_font_description_free(fd);
}

/* ------------------------------------------------------------------ SuperV's emoji module */

static gboolean started;

/* SuperDot 1.0 users: bring their settings and recently used emoji along, and stop the old app. */
static void migrate_from_superdot(void) {
    char *old_cfg = g_build_filename(g_get_user_config_dir(), "superdot", "config.ini", NULL);
    char *old_recent = g_build_filename(g_get_user_data_dir(), "superdot", "recent.txt", NULL);
    gboolean had = g_file_test(old_cfg, G_FILE_TEST_EXISTS);
    if (had && !g_file_test(cfg_path, G_FILE_TEST_EXISTS)) {
        GKeyFile *o = g_key_file_new(), *n = g_key_file_new();
        if (g_key_file_load_from_file(o, old_cfg, G_KEY_FILE_NONE, NULL)) {
            char **keys = g_key_file_get_keys(o, "superdot", NULL, NULL);
            for (int i = 0; keys && keys[i]; i++) {
                char *v = g_key_file_get_value(o, "superdot", keys[i], NULL);
                if (v) g_key_file_set_value(n, "emoji", keys[i], v);
                g_free(v);
            }
            g_strfreev(keys);
            g_key_file_set_boolean(n, "emoji", "shortcut_configured", FALSE);   /* ours is set up anew */
            g_key_file_save_to_file(n, cfg_path, NULL);
            g_chmod(cfg_path, 0600);
            logmsg("brought SuperDot's settings over from %s", old_cfg);
        }
        g_key_file_unref(o); g_key_file_unref(n);
    }
    char *s = NULL;
    gsize len;
    if (!g_file_test(recent_path, G_FILE_TEST_EXISTS) && g_file_get_contents(old_recent, &s, &len, NULL)) {
        g_file_set_contents(recent_path, s, len, NULL);
        g_chmod(recent_path, 0600);
    }
    g_free(s);
    if (had) {   /* a SuperDot 1.0 still running (e.g. installed with install.sh): stop it, free Super+. */
        GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(sv_app));
        GVariant *r = bus ? g_dbus_connection_call_sync(bus, "io.github.superdot.SuperDot", "/io/github/superdot/SuperDot",
            "org.gtk.Actions", "Activate", g_variant_new_parsed("('quit', @av [], @a{sv} {})"), NULL,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, NULL, NULL) : NULL;
        if (r) {
            g_variant_unref(r);
            char *old = g_find_program_in_path("superdot");
            if (old) {
                char *argv[] = { old, "--remove-hotkeys", NULL };
                g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL);
                g_free(old);
            }
            notify("SuperDot is now part of SuperV",
                   "Super+. opens SuperV's emoji panel, with your settings. You can uninstall the separate SuperDot app.");
        }
    }
    g_free(old_cfg); g_free(old_recent);
}

void emoji_start(void) {
    if (started) return;
    started = TRUE;
    data_dir = g_build_filename(g_get_user_data_dir(), "superv", NULL);
    recent_path = g_build_filename(data_dir, "emoji-recent.txt", NULL);
    cfg_dir = g_build_filename(g_get_user_config_dir(), "superv", NULL);
    cfg_path = g_build_filename(cfg_dir, "emoji.ini", NULL);
    g_mkdir_with_parents(data_dir, 0700);
    g_mkdir_with_parents(cfg_dir, 0700);
    migrate_from_superdot();
    config_load();
    load_data();
    recent_load();
    cells = g_array_new(FALSE, FALSE, sizeof(Cell));
    g_array_set_clear_func(cells, cell_clear);
    sections = g_array_new(FALSE, FALSE, sizeof(Section));
    rows = g_array_new(FALSE, FALSE, sizeof(Row));
    cat_btns = g_ptr_array_new();
    clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    prim = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    snaps[0].cb = clip;
    snaps[1].cb = prim;
    hint_atom = gdk_atom_intern_static_string("x-kde-passwordManagerHint");
    g_signal_connect(clip, "owner-change", G_CALLBACK(on_owner_change), NULL);
    g_signal_connect(prim, "owner-change", G_CALLBACK(on_owner_change), NULL);
    build_window();
    apply_theme();
    sync_settings();
    set_tab(cfg.tab);
    g_timeout_add_seconds(2, start_checks, NULL);   /* after login settles */
    /* First start, or the shortcut was removed since: (re)create it. */
    if (!cfg.setup_done || !shortcut_bound()) apply_hotkey(cfg.hotkey);
    int per_tab[N_TABS] = { 0 };
    for (int i = 0; i < n_items; i++) per_tab[items[i].tab]++;
    logmsg("emoji panel ready: %d emoji, %d kaomoji, %d symbols",
           per_tab[TAB_EMOJI], per_tab[TAB_KAOMOJI], per_tab[TAB_SYMBOLS]);
}

gboolean emoji_started(void) { return started; }
gboolean emoji_visible(void) { return started && gtk_widget_get_visible(win); }
gboolean emoji_busy(void) { return started && (gtk_widget_get_visible(win) || insert_src || paste_src || restore_src); }
void emoji_shutdown(void) { if (started) store_clipboard(); }

char *emoji_hotkey_label(void) {
    reload_hotkey();
    return accel_label(cfg.hotkey);
}

void emoji_set_shortcut(gboolean on) {
    if (on) { apply_hotkey(cfg.hotkey); return; }
    if (gtk_widget_get_visible(win)) gtk_widget_hide(win);
    run_helper("");
}

void emoji_command(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "--toggle";
    if (!strcmp(m, "--show")) { if (!gtk_widget_get_visible(win)) show_popup(); }
    else if (!strcmp(m, "--hide")) gtk_widget_hide(win);
    else if (!strcmp(m, "--insert") && argc > 2) insert_text(argv[2], FALSE);
    else if (!strcmp(m, "--search") && argc > 2) {
        if (!gtk_widget_get_visible(win)) show_popup();
        gtk_entry_set_text(GTK_ENTRY(search), argv[2]);
        gtk_editable_set_position(GTK_EDITABLE(search), -1);
    }
    else if (!strcmp(m, "--tab") && argc > 2) {
        for (int t = 0; t < N_TABS; t++) if (!g_ascii_strcasecmp(argv[2], TAB_NAMES[t])) set_tab(t);
        if (!gtk_widget_get_visible(win)) show_popup();
    }
    else toggle();
}

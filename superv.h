/*
 * SuperV: what its parts share. One program, one background process, one virtual keyboard.
 *   superv.c  clipboard history (Super+V), settings, startup, updates, and the code every part uses
 *   emoji.c   the emoji panel (Super+.)
 *   extras.c  plain-text paste, colour picker, copy text from the screen, toasts, screen capture
 *   shot.c    the screenshot tool (Super+Shift+S)
 * Only the clipboard is on at first; each other part starts when you turn it on (emoji) or use it.
 * MIT License.
 */
#ifndef SUPERV_H
#define SUPERV_H
#include <gtk/gtk.h>

#ifndef SV_APP_ID
#define SV_APP_ID   "io.github.superv.SuperV"   /* D-Bus name, icon and desktop file */
#endif
#define SV_VERSION  "2.1.0"
#define SV_REPO_URL "https://github.com/saikatnjm/superv"

/* ---- superv.c */
extern GtkApplication *sv_app;
extern gboolean sv_debug, sv_wayland;
extern char *sv_self_exe, *sv_self_dir;

/* pasting and typing: the shared virtual keyboard */
gboolean sv_uinput_open(void);
gboolean sv_paste_everywhere(void);                         /* can we type into every app? */
gboolean sv_send_keys(const char *combo, gboolean to_x11);  /* e.g. "shift+insert" */
const char *sv_paste_keys(void);                            /* "shift+insert" unless changed */
gboolean sv_focused_is_x11(void);                           /* is the app you're in an X11 one? */
enum { SV_PASTE_OK, SV_PASTE_ASK, SV_PASTE_FAILED, SV_PASTE_RELOGIN, SV_PASTE_ENABLING };
int sv_paste_state(void);                                   /* the one-time permission for typing into Wayland apps */
void sv_enable_paste(void);                                 /* ask for it (the desktop's password dialog) */

/* the clipboard */
void sv_clipboard_guard(gboolean on);                       /* don't record the clipboard; off: put yours back */
void sv_copy_image(GdkPixbuf *pb);                          /* a picture of ours: on the clipboard and into history */
char *sv_selection_item(gboolean primary);                  /* the history item SuperV holds on that selection, or NULL */
void sv_reoffer_item(const char *id, gboolean primary);     /* put it there again */
void sv_show_clipboard_at(int x, int y);                    /* the emoji panel's clipboard button: same spot */

/* shortcuts: superv-setup registers them with the desktop */
typedef void (*SvHelperDone)(int status, gpointer data);    /* 0 done, 3 add it by hand, else failed */
void sv_run_helper(char **args, SvHelperDone done, gpointer data);   /* superv-setup's arguments; runs are queued */
char *sv_desktop_hotkey(const char *slug, const char *name);         /* as the desktop has it; NULL: can't tell */
gboolean sv_shortcut_bound(const char *slug, const char *name);      /* TRUE too when we can't tell */
char *sv_accel_label(const char *accel);                             /* "<Super>v" to "Super+V" */
enum { SV_KEY_WAIT, SV_KEY_CANCEL, SV_KEY_REFUSED, SV_KEY_CHOSEN };
int sv_capture_accel(GdkEventKey *e, char **accel, char **msg);      /* a key press while choosing a shortcut */

/* looks: one theme and one colour for every SuperV window */
gboolean sv_theme_is_dark(void);
const char *sv_theme(void);                                 /* "auto", "light" or "dark" */
void sv_set_theme(const char *id);
const char *sv_palette_id(void);                            /* "violet" unless you picked another colour */
void sv_palette_colors(const char **dark, const char **light);   /* the accent for dark and light themes */
void sv_set_palette(const char *id);
void sv_accent(double rgb[3]);                              /* the accent, for drawing over the screen */
void sv_style_window(GtkWidget *win);                       /* SuperV's look for a small window */
GtkWidget *sv_icon(const char *a, const char *b, const char *fallback);   /* a themed icon, or the fallback text */

/* panels without a title bar: placing, resizing from the border */
void sv_place_panel(GtkWidget *w, int W, int H, int x, int y, int *ox, int *oy);   /* x, y < 0: by the pointer */
gboolean sv_panel_edge(GtkWidget *w, GdkEventButton *e);
gboolean sv_panel_press(GtkWidget *w, GdkEventButton *e, gpointer moved);    /* "button-press-event" */
gboolean sv_panel_motion(GtkWidget *w, GdkEventMotion *e, gpointer d);       /* "motion-notify-event" */

/* ---- emoji.c (started only while the emoji panel is turned on) */
void emoji_start(void);
gboolean emoji_started(void);
void emoji_command(int argc, char **argv);   /* argv[1]: --toggle (default), --show, --hide, --search, --tab, --insert */
void emoji_set_shortcut(gboolean on);        /* register Super+. with the desktop, or give it back */
char *emoji_hotkey_label(void);              /* "Super+." */
gboolean emoji_visible(void);
gboolean emoji_position(int *x, int *y);     /* where the emoji panel is, if it is open */
void emoji_show_at(int x, int y);            /* open it there (switching from the clipboard panel) */
gboolean emoji_busy(void);                   /* open, or about to put your clipboard back */
void emoji_shutdown(void);
void emoji_palette_changed(void);            /* SuperV's theme or colour changed: repaint */
void emoji_paste_changed(void);              /* the typing permission changed: update its banner */

/* ---- extras.c (nothing runs until you use one) */
void extras_command(const char *what);       /* "--plain-paste", "--pick-color", "--extract-text" */
gboolean extras_busy(void);
gboolean extras_ocr_available(void);
typedef void (*SvShotDone)(GdkPixbuf *pb, gpointer data);   /* pb NULL: cancelled or not possible */
void extras_capture_screen(SvShotDone done, gpointer data);  /* the whole screen */
void extras_ocr_pixbuf(GdkPixbuf *pb);       /* copy the text in a picture */
void extras_toast(const char *title, const char *body);     /* a small card for a few seconds */
gboolean extras_capturing(void);         /* reading the screen right now */

/* ---- shot.c: the screenshot tool, Flameshot-style. Only alive while you take a screenshot (and for pins). */
void shot_start(int delay_s);
void shot_text(void);                    /* the same frozen screen, to drag over text and copy it */
gboolean shot_busy(void);
int shot_pins(void);                     /* screenshots pinned to the screen */

#endif

/*
 * SuperV: what the clipboard part (superv.c) and the emoji part (emoji.c) share.
 * One program, one background process, one virtual keyboard.
 * MIT License.
 */
#ifndef SUPERV_H
#define SUPERV_H
#include <gtk/gtk.h>

#ifndef SV_APP_ID
#define SV_APP_ID   "io.github.superv.SuperV"   /* D-Bus name, icon and desktop file */
#endif
#define SV_VERSION  "2.0.0"
#define SV_REPO_URL "https://github.com/saikatnjm/superv"

/* superv.c */
extern GtkApplication *sv_app;
extern gboolean sv_debug, sv_wayland;
extern char *sv_self_exe, *sv_self_dir;
gboolean sv_uinput_open(void);                              /* the shared virtual keyboard */
gboolean sv_paste_everywhere(void);                         /* can we type into every app? */
gboolean sv_send_keys(const char *combo, gboolean to_x11);  /* e.g. "shift+insert" */
char *sv_setup_path(const char *name);                      /* superv-setup, superv-emoji-setup */
void sv_show_clipboard(void);                               /* the emoji panel's 📋 button */

/* emoji.c (loaded only while the emoji panel is turned on) */
void emoji_start(void);
gboolean emoji_started(void);
void emoji_command(int argc, char **argv);   /* argv[1]: --toggle (default), --show, --hide, --search, --tab, --insert */
void emoji_set_shortcut(gboolean on);        /* register Super+. with the desktop, or give it back */
char *emoji_hotkey_label(void);              /* "Super+." */
gboolean emoji_visible(void);
gboolean emoji_busy(void);                   /* open, or about to put your clipboard back */
void emoji_shutdown(void);

#endif

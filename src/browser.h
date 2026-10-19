/* browser.h - the file browser to open programs (.prg) and cartridges
 * (.crt). Nuklear has no file dialog: this one is made of its pieces. */
#ifndef BROWSER_H
#define BROWSER_H
#include "ui_common.h"

/* Remember where we started from (and pick ./carts if it is there). */
void browser_init(void);
/* The machine files are for (rescans if the browser is open). */
void browser_set_machine(const machine_t *m);
void browser_open(void);
void browser_close(void);
bool browser_is_open(void);
/* Draw the browser over 'area'. When the user picks a file, its path is
 * returned (and *run says if a program should be RUN); else NULL. */
const char *browser_window(struct nk_context *ctx, struct nk_rect area, bool *run);
/* A file couldn't be loaded: say why, in the browser. */
void browser_error(const char *msg);
/* Keys while the browser is open (arrows, Enter, Esc, Backspace...). */
void browser_key(const sapp_event *ev);
/* The window on the screen, so the board below ignores the mouse. */
bool browser_bounds(struct nk_rect *r);

/* Recently loaded files, newest first. */
void browser_add_recent(const char *path);
int browser_recent_count(void);
const char *browser_recent(int i);

#endif

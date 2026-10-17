/* ui.h - the graphical interface: board view, monitor, scope, controls. */
#ifndef UI_H
#define UI_H
#include "sokol_headers.h"
#include "board.h"

void ui_init(board_t *b, const char *prg, const char *scope_pin);
void ui_frame(void);
void ui_event(const sapp_event *ev);
void ui_save_screenshot(const char *path);
int ui_emulated_frame(void);
void ui_inspect(const char *ref);
void ui_view(float cx, float cy, float zoom);
void ui_apply_fault(const char *spec);
void ui_shutdown(void);

#endif

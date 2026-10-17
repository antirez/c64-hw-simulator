/* inspect.h - the chip inspector: a chip drawn large with its pins and
 * their live states, the gates inside the logic chips, and for the PLA
 * its equations with the product terms lit while they are true. The same
 * gate diagrams are drawn on the board when zooming in a lot. */
#ifndef INSPECT_H
#define INSPECT_H
#include "ui_common.h"

/* Where a pin enters a chip body, for the diagrams: a point on the body
 * edge and the side (-1 left, +1 right). Indexed by pin number. */
typedef struct { float x, y; int side; bool valid; } pinpos_t;

bool chip_has_diagram(board_t *b, int part);
/* Draw the gates of a logic chip inside its body, wired to its pins.
 * 'pitch' is the distance between two pins. Returns false (and draws
 * nothing) if the body is too small for it. */
bool chip_diagram(struct nk_command_buffer *cv, struct nk_context *ctx, board_t *b,
                  const uint8_t *net_state, int part, struct nk_rect body,
                  const pinpos_t *pos, float pitch);

/* Open the inspector on a chip (-1 closes it). */
void inspect_open(board_t *b, int part);
int inspect_current(void);
/* Draw the inspector window, if open, over 'area'. Returns the pin the
 * user clicked (the caller selects it and opens its menu) or -1. */
int inspect_window(struct nk_context *ctx, board_t *b, const uint8_t *net_state, struct nk_rect area);
/* The inspector window on the screen, to keep the board below from
 * reacting to the mouse. False if closed. */
bool inspect_bounds(struct nk_rect *r);

#endif

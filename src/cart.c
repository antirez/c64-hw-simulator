/* cart.c - cartridges (.CRT files) in the expansion port.
 *
 * A cartridge is modeled for what it is electrically: something that
 * drives pins of the expansion port CN6. It ties /GAME and/or /EXROM to
 * ground (that is how it tells the PLA the memory map it wants), and puts
 * the byte of its ROM on D0-D7 while the PLA pulls /ROML or /ROMH low. The
 * cartridge pins are CN6's pins, so a bad contact on CN6 (a fault on one
 * of its pins) is a bad contact for the cartridge too.
 *
 * Everything else comes from the board. In Ultimax mode (/GAME low, /EXROM
 * high, like the Dead Test cartridge) the cartridge replaces the KERNAL at
 * $E000 and the PLA leaves holes where nothing answers, not even RAM. The
 * VIC reads the cartridge too: during its cycles the CPU lets go of A12-A15
 * and the RP4 pull-ups raise them, so the cartridge sees $F000-$FFFF.
 *
 * Supported: the plain cartridges (CRT hardware type 0: 8K, 16K, Ultimax),
 * which is what the diagnostic cartridges are. Cartridges with bank
 * switching (type 1 and up) are refused with a message. */
#include "c64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cart {
    char name[33];
    bool exrom, game;           /* lines the cartridge holds low */
    bool has_roml, has_romh;
    uint8_t roml[8192], romh[8192];
    /* CN6 pins, by function. */
    int pin_roml, pin_romh, pin_game, pin_exrom, pin_a[13], pin_d[8];
};

/* The pad of CN6 for every line the cartridge uses (the expansion port
 * pinout: data on 14-21, address on F-Y). */
static const char *const pad_a[13] = { "Y", "X", "W", "V", "U", "T", "S", "R", "P", "N", "M", "L", "K" };
static const char *const pad_d[8] = { "21", "20", "19", "18", "17", "16", "15", "14" };

static int cn6_pin(board_t *b, const char *pad) { return board_pad_pin(b, C64(b)->p_cn6, pad); }

/* The ROM selected by the PLA drives the data bus; the address is A0-A12
 * (8K). If a fault pulls both lines low, the ROML chip answers: on a real
 * cartridge it depends on how it is wired. */
static void cart_eval(sim_t *s, part_t *p) {
    struct cart *c = p->state;
    const uint8_t *rom = NULL;
    if (c->has_roml && !sim_read(s, c->pin_roml)) rom = c->roml;
    else if (c->has_romh && !sim_read(s, c->pin_romh)) rom = c->romh;
    unsigned v = 0;
    if (rom) {
        unsigned a = 0;
        for (int i = 0; i < 13; i++) a |= (unsigned)sim_read(s, c->pin_a[i]) << i;
        v = rom[a];
    }
    for (int i = 0; i < 8; i++) sim_drive(s, c->pin_d[i], rom ? DRV_BIT((v >> i) & 1) : DRV_Z);
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32(const uint8_t *p) {
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

/* Read a .CRT file (the VICE format). */
static struct cart *crt_load(const char *path, char *msg, int msglen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msglen, "can't open %s", path); return NULL; }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 0x40 || memcmp(buf, "C64 CARTRIDGE   ", 16)) {
        snprintf(msg, msglen, "%s is not a .CRT cartridge", path);
        return NULL;
    }
    unsigned type = be16(buf + 0x16);
    char name[33];
    memcpy(name, buf + 0x20, 32);
    name[32] = 0;
    if (type != 0) {
        snprintf(msg, msglen, "%s: cartridge type %u has bank switching, not supported (only plain "
                 "8K/16K/Ultimax ones)", name, type);
        return NULL;
    }
    struct cart *c = calloc(1, sizeof(*c));
    memcpy(c->name, name, sizeof(c->name));
    if (!c->name[0]) {      /* no name in the file: use the file name */
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        snprintf(c->name, sizeof(c->name), "%.*s", (int)strcspn(base, "."), base);
    }
    /* The header gives the levels of the two lines: 0 means held low. */
    c->exrom = buf[0x18] == 0;
    c->game = buf[0x19] == 0;
    size_t off = be32(buf + 0x10);
    while (off + 16 <= n && !memcmp(buf + off, "CHIP", 4)) {
        unsigned long len = be32(buf + off + 4);
        unsigned load = be16(buf + off + 12), size = be16(buf + off + 14);
        if (len < 16 || off + 16 + size > n) break;
        const uint8_t *data = buf + off + 16;
        /* $8000: ROML (a 16K image continues into ROMH); $A000 or $E000 and
         * up: ROMH. A smaller chip repeats over the 8K window (its upper
         * address lines are not connected): a 4K ROM at $F000 is also seen
         * at $E000. */
        unsigned m = size < 8192 ? size : 8192;
        if (m && load == 0x8000) {
            for (unsigned i = 0; i < 8192; i++) c->roml[i] = data[i % m];
            c->has_roml = true;
            if (size > 8192) {
                for (unsigned i = 0; i < 8192; i++) c->romh[i] = data[8192 + i % (size - 8192)];
                c->has_romh = true;
            }
        } else if (m && (load == 0xA000 || load >= 0xE000)) {
            unsigned base = load >= 0xE000 ? load - 0xE000 : 0;
            for (unsigned i = 0; i < 8192; i++) c->romh[i] = data[(i + 8192 - base) % m];
            c->has_romh = true;
        }
        off += len;
    }
    if (!c->has_roml && !c->has_romh) {
        snprintf(msg, msglen, "%s: no ROM in the file", c->name);
        free(c);
        return NULL;
    }
    return c;
}

/* Pin flags and drives for the cartridge, or back to a bare connector. */
static void plug(board_t *b, struct cart *c) {
    sim_t *s = &b->sim;
    part_t *p = &s->parts[C64(b)->p_cn6];
    struct cart *old = p->state;
    struct cart *k = c ? c : old;
    int ins[15], nin = 0, outs[10], nout = 0;
    ins[nin++] = k->pin_roml;
    ins[nin++] = k->pin_romh;
    for (int i = 0; i < 13; i++) ins[nin++] = k->pin_a[i];
    for (int i = 0; i < 8; i++) outs[nout++] = k->pin_d[i];
    outs[nout++] = k->pin_game;
    outs[nout++] = k->pin_exrom;
    for (int i = 0; i < nin; i++) if (ins[i] >= 0) s->pins[ins[i]].flags = c ? PIN_IN | PIN_SENSE : 0;
    for (int i = 0; i < nout; i++) if (outs[i] >= 0) {
        s->pins[outs[i]].flags = c ? PIN_OUT : 0;
        s->pins[outs[i]].drive = DRV_Z;
    }
    /* A wire from the line to ground on the cartridge. */
    if (c && c->game && c->pin_game >= 0) s->pins[c->pin_game].drive = DRIVE(S_SUPPLY, 0);
    if (c && c->exrom && c->pin_exrom >= 0) s->pins[c->pin_exrom].drive = DRIVE(S_SUPPLY, 0);
    p->state = c;
    p->eval = c ? cart_eval : NULL;
    C64(b)->cart = c;
}

bool c64_insert_cart(board_t *b, const char *path, char *msg, int msglen) {
    if (!board_is_c64(b)) { snprintf(msg, msglen, "cartridges go in the C64"); return false; }
    struct cart *c = crt_load(path, msg, msglen);
    if (!c) return false;
    c->pin_roml = cn6_pin(b, "11");
    c->pin_romh = cn6_pin(b, "B");
    c->pin_game = cn6_pin(b, "8");
    c->pin_exrom = cn6_pin(b, "9");
    for (int i = 0; i < 13; i++) c->pin_a[i] = cn6_pin(b, pad_a[i]);
    for (int i = 0; i < 8; i++) c->pin_d[i] = cn6_pin(b, pad_d[i]);
    c64_remove_cart(b);
    plug(b, c);
    /* Cartridges go in with the power off: switch the C64 on again. */
    board_rebuild(b);
    board_power_on(b);
    snprintf(msg, msglen, "cartridge \"%s\": %s%s%s", c->name,
             c->game && !c->exrom ? "Ultimax" : c->game ? "16K" : "8K",
             c->has_roml ? ", ROML" : "", c->has_romh ? ", ROMH" : "");
    return true;
}

void c64_remove_cart(board_t *b) {
    if (!board_is_c64(b)) return;
    struct cart *c = C64(b)->cart;
    if (!c) return;
    plug(b, NULL);
    free(c);
    board_rebuild(b);
    board_power_on(b);
}

const char *c64_cart_name(board_t *b) {
    if (!board_is_c64(b) || !C64(b)->cart) return NULL;
    return ((struct cart *)C64(b)->cart)->name;
}

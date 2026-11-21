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
 * and EasyFlash (type 32, boot jumper set). Flash contents are read-only. */
#include "c64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cart {
    char name[33];
    bool exrom, game;           /* lines the cartridge holds low */
    bool has_roml, has_romh;
    bool easyflash;
    uint8_t bank, ram[256], write_data, write_addr;
    enum { EF_IDLE, EF_CONTROL, EF_BANK, EF_RAM } pending;
    int pin_io1, pin_io2, pin_rw, pin_phi2, pin_reset;
    /* CN6 pins, by function. */
    int pin_roml, pin_romh, pin_game, pin_exrom, pin_a[13], pin_d[8];
    uint8_t rom[][2][8192];    /* [bank][ROML/ROMH][offset] */
};

/* The pad of CN6 for every line the cartridge uses (the expansion port
 * pinout: data on 14-21, address on F-Y). */
static const char *const pad_a[13] = { "Y", "X", "W", "V", "U", "T", "S", "R", "P", "N", "M", "L", "K" };
static const char *const pad_d[8] = { "21", "20", "19", "18", "17", "16", "15", "14" };

static int cn6_pin(board_t *b, const char *pad) { return board_pad_pin(b, C64(b)->p_cn6, pad); }

static unsigned connector_bus(sim_t *s, const int *pins, int bits) {
    unsigned value = 0;
    for (int i = 0; i < bits; i++) value |= (unsigned)sim_read(s, pins[i]) << i;
    return value;
}

static void cart_eval(sim_t *s, part_t *p) {
    struct cart *c = p->state;
    const uint8_t *rom = NULL;
    if (c->has_roml && !sim_read(s, c->pin_roml)) rom = c->rom[c->bank][0];
    else if (c->has_romh && !sim_read(s, c->pin_romh)) rom = c->rom[c->bank][1];
    int value = -1;
    if (rom && (!c->easyflash || sim_read(s, c->pin_rw)))
        value = rom[connector_bus(s, c->pin_a, 13)];
    else if (c->easyflash && !sim_read(s, c->pin_io2) && sim_read(s, c->pin_rw))
        value = c->ram[connector_bus(s, c->pin_a, 8)];
    for (int i = 0; i < 8; i++)
        sim_drive(s, c->pin_d[i], value >= 0 ? DRV_BIT((value >> i) & 1) : DRV_Z);
}

static void easyflash_mode(sim_t *s, struct cart *c, uint8_t mode) {
    /* With M clear, /GAME follows the boot jumper (ground). */
    sim_drive(s, c->pin_game, (!(mode & 4) || (mode & 1)) ? DRV_LO : DRV_Z);
    sim_drive(s, c->pin_exrom, (mode & 2) ? DRV_LO : DRV_Z);
}

static void easyflash_reset(sim_t *s, part_t *p) {
    struct cart *c = p->state;
    if (c->bank) sim_mark_dirty(s, (int)(p - s->parts));
    c->bank = 0;
    c->pending = EF_IDLE;
    easyflash_mode(s, c, 0);
}

/* Latch address/data while PHI2 is high and the I/O select is active.
 * At the falling edge the CPU releases the bus. Use the sampled pins. */
static void easyflash_commit(sim_t *s, part_t *p) {
    struct cart *c = p->state;
    if (!sim_read(s, c->pin_reset)) {
        easyflash_reset(s, p);
    } else if (sim_read(s, c->pin_phi2)) {
        c->pending = EF_IDLE;
        if (sim_read(s, c->pin_rw)) return;
        if (!sim_read(s, c->pin_io1))
            c->pending = sim_read(s, c->pin_a[1]) ? EF_CONTROL : EF_BANK;
        else if (!sim_read(s, c->pin_io2)) {
            c->pending = EF_RAM;
            c->write_addr = connector_bus(s, c->pin_a, 8);
        }
        if (c->pending != EF_IDLE) c->write_data = connector_bus(s, c->pin_d, 8);
    } else if (c->pending != EF_IDLE) {
        if (c->pending == EF_CONTROL) easyflash_mode(s, c, c->write_data);
        else if (c->pending == EF_BANK) c->bank = c->write_data & 63;
        else c->ram[c->write_addr] = c->write_data;
        c->pending = EF_IDLE;
        sim_mark_dirty(s, (int)(p - s->parts));
    }
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32(const uint8_t *p) {
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

/* Read a .CRT file (the VICE format). */
static struct cart *crt_load(const char *path, char *msg, int msglen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msglen, "can't open %s", path); return NULL; }
    static uint8_t buf[64 + 128 * (16 + 8192)];
    size_t n = fread(buf, 1, sizeof(buf), f);
    bool oversized = n == sizeof(buf) && fgetc(f) != EOF;
    fclose(f);
    if (oversized) { snprintf(msg, msglen, "%s: CRT image is too large", path); return NULL; }
    if (n < 0x40 || memcmp(buf, "C64 CARTRIDGE   ", 16)) {
        snprintf(msg, msglen, "%s is not a .CRT cartridge", path);
        return NULL;
    }
    unsigned type = be16(buf + 0x16);
    char name[33];
    memcpy(name, buf + 0x20, 32);
    name[32] = 0;
    if (type != 0 && type != 32) {
        snprintf(msg, msglen, "%s: unsupported cartridge type %u (supported: plain and EasyFlash)", name, type);
        return NULL;
    }
    size_t banks = type == 32 ? 64 : 1;
    struct cart *c = calloc(1, sizeof(*c) + banks * sizeof(c->rom[0]));
    if (!c) { snprintf(msg, msglen, "out of memory loading cartridge"); return NULL; }
    c->easyflash = type == 32;
    memset(c->rom, 0xff, banks * sizeof(c->rom[0]));
    memcpy(c->name, name, sizeof(c->name));
    if (!c->name[0]) {      /* no name in the file: use the file name */
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        snprintf(c->name, sizeof(c->name), "%.*s", (int)strcspn(base, "."), base);
    }
    /* The header gives the levels of the two lines: 0 means held low. */
    c->exrom = buf[0x18] == 0;
    c->game = buf[0x19] == 0;
    size_t off = be32(buf + 0x10);
    if (off < 64 || off > n) goto malformed;
    while (off < n) {
        if (n - off < 16 || memcmp(buf + off, "CHIP", 4)) goto malformed;
        unsigned long len = be32(buf + off + 4);
        unsigned load = be16(buf + off + 12), size = be16(buf + off + 14);
        if (len < 16u + size || len > n - off) goto malformed;
        if (c->easyflash) {
            unsigned bank = be16(buf + off + 10);
            if (bank >= banks) goto malformed;
            if (size == 16384 && load == 0x8000)
                memcpy(c->rom[bank], buf + off + 16, size);
            else if (size == 8192 && (load == 0x8000 || load == 0xa000 || load == 0xe000))
                memcpy(c->rom[bank][load != 0x8000], buf + off + 16, size);
            else goto malformed;
            c->has_roml = c->has_romh = true;   /* unpopulated flash banks read $ff */
            off += len;
            continue;
        }
        const uint8_t *data = buf + off + 16;
        /* $8000: ROML (a 16K image continues into ROMH); $A000 or $E000 and
         * up: ROMH. A smaller chip repeats over the 8K window (its upper
         * address lines are not connected): a 4K ROM at $F000 is also seen
         * at $E000. */
        unsigned m = size < 8192 ? size : 8192;
        if (m && load == 0x8000) {
            for (unsigned i = 0; i < 8192; i++) c->rom[0][0][i] = data[i % m];
            c->has_roml = true;
            if (size > 8192) {
                for (unsigned i = 0; i < 8192; i++) c->rom[0][1][i] = data[8192 + i % (size - 8192)];
                c->has_romh = true;
            }
        } else if (m && (load == 0xA000 || load >= 0xE000)) {
            unsigned base = load >= 0xE000 ? load - 0xE000 : 0;
            for (unsigned i = 0; i < 8192; i++) c->rom[0][1][i] = data[(i + 8192 - base) % m];
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
malformed:
    snprintf(msg, msglen, "%s: malformed CRT chip packet", c->name);
    free(c);
    return NULL;
}

/* Pin flags and drives for the cartridge, or back to a bare connector. */
static void plug(board_t *b, struct cart *c) {
    sim_t *s = &b->sim;
    part_t *p = &s->parts[C64(b)->p_cn6];
    struct cart *old = p->state;
    struct cart *k = c ? c : old;
    int ins[20], nin = 0, outs[10], nout = 0;
    if (k->easyflash) {
        ins[nin++] = k->pin_io1;
        ins[nin++] = k->pin_io2;
        ins[nin++] = k->pin_rw;
        ins[nin++] = k->pin_phi2;
        ins[nin++] = k->pin_reset;
    }
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
    p->commit = c && c->easyflash ? easyflash_commit : NULL;
    p->reset = c && c->easyflash ? easyflash_reset : NULL;
    C64(b)->cart = c;
}

bool c64_insert_cart(board_t *b, const char *path, char *msg, int msglen) {
    if (!board_is_c64(b)) { snprintf(msg, msglen, "cartridges go in the C64"); return false; }
    struct cart *c = crt_load(path, msg, msglen);
    if (!c) return false;
    if (c->easyflash) {
        c->pin_io1 = cn6_pin(b, "7");
        c->pin_io2 = cn6_pin(b, "10");
        c->pin_rw = cn6_pin(b, "5");
        c->pin_phi2 = cn6_pin(b, "E");
        c->pin_reset = cn6_pin(b, "C");
    }
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
             c->easyflash ? "EasyFlash" : c->game && !c->exrom ? "Ultimax" : c->game ? "16K" : "8K",
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

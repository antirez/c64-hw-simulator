/* png.c - minimal PNG writer: zlib stream made of "stored" (uncompressed)
 * deflate blocks, so no compression library is needed. */
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(const unsigned char *buf, size_t len, uint32_t c) {
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void write_chunk(FILE *f, const char *type, const unsigned char *data, size_t len) {
    unsigned char hdr[8];
    put32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc((const unsigned char *)type, 4, 0xFFFFFFFFu);
    if (len) c = crc(data, len, c);
    unsigned char tail[4];
    put32(tail, c ^ 0xFFFFFFFFu);
    fwrite(tail, 1, 4, f);
}

bool png_write(const char *path, const uint32_t *pixels, int w, int h) {
    crc_init();
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8 bit RGBA */
    write_chunk(f, "IHDR", ihdr, 13);

    /* Raw scanlines: filter byte 0 + RGBA pixels. */
    size_t row = (size_t)w * 4 + 1, raw_len = row * (size_t)h;
    unsigned char *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, pixels + (size_t)y * (size_t)w, (size_t)w * 4);
    }
    size_t nblocks = (raw_len + 65534) / 65535;
    unsigned char *z = malloc(2 + raw_len + nblocks * 5 + 4);
    size_t zi = 0, pos = 0;
    uint32_t a = 1, b = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    do {
        size_t n = raw_len - pos;
        if (n > 65535) n = 65535;
        z[zi++] = (pos + n >= raw_len) ? 1 : 0;
        z[zi++] = (unsigned char)(n & 0xFF); z[zi++] = (unsigned char)(n >> 8);
        z[zi++] = (unsigned char)(~n & 0xFF); z[zi++] = (unsigned char)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + pos, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[pos + i]) % 65521; b = (b + a) % 65521; }
        zi += n;
        pos += n;
    } while (pos < raw_len);
    put32(z + zi, (b << 16) | a);
    zi += 4;
    write_chunk(f, "IDAT", z, zi);
    write_chunk(f, "IEND", NULL, 0);
    free(z);
    free(raw);
    fclose(f);
    return true;
}

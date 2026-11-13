/* Loading the original game files and decoding pictures. See gamedata.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamedata.h"

#define GAM1_BASE 0x900u
#define GAM2_BASE 0x49CE0u

size_t lzss_unpack(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_max,
                   size_t *consumed)
{
    uint8_t ring[4096];
    unsigned r = 0xFEE, flags = 0;
    size_t i = 0, o = 0;
    memset(ring, 0, sizeof ring);
    for (;;) {
        flags >>= 1;
        if (!(flags & 0x100)) {
            if (i >= src_len) break;
            flags = src[i++] | 0xFF00u;
        }
        if (flags & 1) {
            if (i >= src_len) break;
            uint8_t c = src[i++];
            if (o < out_max) out[o] = c;
            o++;
            ring[r] = c;
            r = (r + 1) & 0xFFF;
        } else {
            if (i + 1 >= src_len) break;
            unsigned pos = src[i] | ((src[i + 1] & 0xF0u) << 4);
            unsigned len = (src[i + 1] & 0x0Fu) + 3;
            i += 2;
            for (unsigned k = 0; k < len; k++) {
                uint8_t c = ring[(pos + k) & 0xFFF];
                if (o < out_max) out[o] = c;
                o++;
                ring[r] = c;
                r = (r + 1) & 0xFFF;
            }
        }
    }
    if (consumed) *consumed = i;
    return o < out_max ? o : out_max;
}

static uint8_t *read_file(const char *dir, const char *name, size_t *len, char *err)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, 256, "cannot open %.200s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
        snprintf(err, 256, "cannot read %.200s", path);
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int gamedata_load(GameData *gd, const char *data_dir)
{
    memset(gd, 0, sizeof *gd);
    gd->mem = calloc(1, AMIGA_MEM_SIZE);
    if (!gd->mem) {
        snprintf(gd->error, sizeof gd->error, "out of memory");
        return -1;
    }
    size_t len;
    uint8_t *gam1 = read_file(data_dir, "GAM1", &len, gd->error);
    if (!gam1) return -1;
    /* GAM1 is unpacked to $900; GAM2 is then loaded over its padded tail. */
    lzss_unpack(gam1, len, gd->mem + GAM1_BASE, AMIGA_MEM_SIZE - GAM1_BASE, NULL);
    free(gam1);
    uint8_t *gam2 = read_file(data_dir, "GAM2", &len, gd->error);
    if (!gam2) return -1;
    if (len > AMIGA_MEM_SIZE - GAM2_BASE) len = AMIGA_MEM_SIZE - GAM2_BASE;
    memcpy(gd->mem + GAM2_BASE, gam2, len);
    free(gam2);
    gamedata_boot_icons(gd->mem);
    return 0;
}

/* $6A68 (first boot): picture $5F426 is shown and $EE88 copies the 8 craft
 * icons, 5 bytes x 29 lines x 5 planes each, from the screen at $72000 + 5k
 * to $59BEE + $2D5 x k (planes interleaved by byte). The screens draw the
 * craft pictures from there. */
void gamedata_boot_icons(uint8_t *mem)
{
    static uint8_t raw[5 * 8000];
    if (picture_unpack_raw(mem, 0x5F426u, raw, NULL) != 0) return;
    uint8_t *o = mem + 0x59BEEu;
    for (unsigned k = 0; k < 8; k++)
        for (unsigned y = 0; y < 29; y++)
            for (unsigned x = 0; x < 5; x++)
                for (unsigned pl = 0; pl < 5; pl++)
                    *o++ = raw[y * 200u + pl * 40u + 5u * k + x];
}

void gamedata_free(GameData *gd)
{
    free(gd->mem);
    gd->mem = NULL;
}

static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8) | p[1]; }

int picture_unpack_raw(const uint8_t *m, uint32_t addr, uint8_t *raw, uint16_t *palette)
{
    if (addr + 8 > AMIGA_MEM_SIZE) return -1;
    unsigned planes = be16(m + addr);
    if (planes < 1 || planes > 5) return -1;
    unsigned ncol = 1u << planes;
    for (unsigned c = 0; c < 32; c++)
        if (palette && c < ncol) palette[c] = (uint16_t)be16(m + addr + 2 + 2 * c);
    uint32_t p = addr + 2 + 2 * ncol;
    unsigned zero = be16(m + p), plen = be16(m + p + 2);
    size_t total = (size_t)planes * 8000u;
    memset(raw, 0, total);
    if (zero < total)
        lzss_unpack(m + p + 4, plen, raw + zero, total - zero, NULL);
    return 0;
}

/* Picture format (unpacker $F9E0): u16 planes; u16 palette[1 << planes];
 * u16 leading zero bytes; u16 packed length; LZSS data. The unpacked bytes
 * are the interleaved screen: each line = planes rows of 40 bytes. */
int picture_decode(const GameData *gd, uint32_t addr, Picture *pic)
{
    const uint8_t *m = gd->mem;
    if (addr + 8 > AMIGA_MEM_SIZE) return -1;
    unsigned planes = be16(m + addr);
    if (planes < 1 || planes > 5) return -1;
    unsigned ncol = 1u << planes;
    memset(pic, 0, sizeof *pic);
    pic->planes = (int)planes;
    for (unsigned c = 0; c < 32; c++) {
        unsigned v = c < ncol ? be16(m + addr + 2 + 2 * c) : 0;
        unsigned r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
        pic->palette[c] = (r * 17u) << 16 | (g * 17u) << 8 | (b * 17u);
    }
    uint32_t p = addr + 2 + 2 * ncol;
    unsigned zero = be16(m + p), plen = be16(m + p + 2);
    p += 4;
    size_t total = (size_t)planes * 8000u;
    uint8_t *raw = calloc(1, total + 256);
    if (!raw) return -1;
    if (zero < total)
        lzss_unpack(m + p, plen, raw + zero, total + 256 - zero, NULL);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 320; x++) {
            unsigned idx = 0, bit = 7 - (x & 7);
            for (unsigned pl = 0; pl < planes; pl++) {
                uint8_t byte = raw[(size_t)y * planes * 40 + pl * 40 + (x >> 3)];
                idx |= ((byte >> bit) & 1u) << pl;
            }
            pic->pixels[y * 320 + x] = (uint8_t)idx;
        }
    }
    free(raw);
    return 0;
}

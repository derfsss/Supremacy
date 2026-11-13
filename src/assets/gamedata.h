/*
 * Loading the original Amiga game files at run time.
 *
 * The port ships without the original data: the player points it at the
 * SupremacyData drawer of their own copy (the WHDLoad install). This module
 * rebuilds the original's memory image the same way the game does
 * at start-up: GAM1 is LZSS-packed and unpacked to $900,
 * GAM2 is loaded raw at $49CE0. Everything else (pictures, animations, font,
 * sound data) is then found at its original address.
 */
#ifndef SUP_GAMEDATA_H
#define SUP_GAMEDATA_H

#include <stddef.h>
#include <stdint.h>

#define AMIGA_MEM_SIZE 0x80000u     /* 512 KB chip RAM image */

typedef struct GameData {
    uint8_t *mem;                   /* AMIGA_MEM_SIZE bytes, original addresses */
    char     error[256];
} GameData;

/* Load GAM1 + GAM2 from the SupremacyData directory. Returns 0 on success;
 * on failure gd->error says why. */
int  gamedata_load(GameData *gd, const char *data_dir);
void gamedata_free(GameData *gd);

/* The original's first-boot step $6A68 that matters to the image: the 8
 * craft icons copied from picture $5F426 to $59BEE (gamedata_load does it). */
void gamedata_boot_icons(uint8_t *mem);

/* Okumura LZSS as used by the game (4 KB ring, start $FEE, flag byte LSB
 * first, 1 = literal). Writes at most out_max bytes; returns bytes written
 * (stops when the input runs out). *consumed gets the input bytes used. */
size_t lzss_unpack(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_max,
                   size_t *consumed);

/* A decoded full-screen picture (the original's picture format 1). */
typedef struct Picture {
    int      planes;                /* 5 for every game picture */
    uint32_t palette[32];           /* 0xRRGGBB, from the 12-bit $0RGB entries */
    uint8_t  pixels[320 * 200];     /* colour index per pixel */
} Picture;

/* Decode the picture whose header is at original address `addr`. 0 = ok. */
int picture_decode(const GameData *gd, uint32_t addr, Picture *pic);

/* Unpack the picture at `addr` of the memory image `m` into `raw` (planes x
 * 8,000 bytes, the original's interleaved screen layout) and its 12-bit
 * $0RGB colours into palette[0 .. 2^planes - 1] (may be NULL). 0 = ok. */
int picture_unpack_raw(const uint8_t *m, uint32_t addr, uint8_t *raw, uint16_t *palette);

/* Original addresses of the full-screen pictures (amiga-graphics.md). */
enum {
    PIC_MAIN       = 0x114A8,
    PIC_STATISTICS = 0x1E924,
    PIC_SHIPYARD   = 0x44EFA,
    PIC_FLEET      = 0x211C8,
    PIC_TROOPS     = 0x1495A,
    PIC_CARGO      = 0x169C0,
    PIC_SURFACE    = 0x19210,
    PIC_COMBAT     = 0x1BB16,
    PIC_TITLE      = 0x5D854
};

#endif

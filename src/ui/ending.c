/* The end sequences WIN0-WIN3 / WIN_. See ending.h. Addresses in the
 * comments are WIN0's; WIN_'s routines are the same code 2 bytes later
 * (data moved: see the table below). */
#include <stdlib.h>
#include <string.h>

#include "ending.h"
#include "gamedata.h"

#define MEM 0x80000u
#define PROG 0x900u

enum { S_BLANK, S_HOLD_CARD, S_FADE1, S_HOLD_SCENE, S_ANIM, S_WHITE, S_FADE2, S_FADE3, S_END };

static uint16_t r16(const Ending *e, uint32_t a)
{
    a &= MEM - 1;
    return (uint16_t)(e->mem[a] << 8 | e->mem[(a + 1) & (MEM - 1)]);
}
static uint32_t r32(const Ending *e, uint32_t a) { return (uint32_t)r16(e, a) << 16 | r16(e, a + 2); }
static void w16(Ending *e, uint32_t a, uint16_t v)
{
    a &= MEM - 1;
    e->mem[a] = (uint8_t)(v >> 8);
    e->mem[(a + 1) & (MEM - 1)] = (uint8_t)v;
}
static void w32(Ending *e, uint32_t a, uint32_t v) { w16(e, a, (uint16_t)(v >> 16)); w16(e, a + 2, (uint16_t)v); }
static uint8_t *m8(Ending *e, uint32_t a) { return &e->mem[a & (MEM - 1)]; }

/* $E3A: $B20 = $B1C, 32 colours from a0 to COLOR00-31 and the shadow,
 * DMA on (bitplanes, copper, sprites) */
static void set_palette(Ending *e, uint32_t a0)
{
    w32(e, 0xB20, r32(e, 0xB1C));
    for (int k = 0; k < 32; k++) {
        uint16_t c = r16(e, a0 + 2u * (uint32_t)k);
        e->colour[k] = c;
        w16(e, e->shadow + 2u * (uint32_t)k, c);
    }
    e->display = 1;
}

/* $16FC: LZSS from a1 (d0 bytes) to a2, ring at a3 (cleared $FEE bytes) */
static uint32_t unpack(Ending *e, uint32_t a1, uint32_t n, uint32_t a2)
{
    uint32_t a4 = a1 + n, ring = e->ring;
    for (uint32_t k = 0; k < 0xFEE; k++) *m8(e, ring + k) = 0;
    unsigned d3 = 0xFEE, d5 = 0;
    for (;;) {
        d5 = (d5 & 0xFFFF0000u) | ((d5 & 0xFFFFu) >> 1);
        if (!(d5 & 0x100)) {
            if (a1 >= a4) break;
            d5 = 0xFFFFFF00u | *m8(e, a1++);
        }
        if (d5 & 1) {
            if (a1 >= a4) break;
            uint8_t c = *m8(e, a1++);
            *m8(e, a2++) = c;
            *m8(e, ring + d3) = c;
            d3 = (d3 + 1) & 0xFFF;
        } else {
            if (a1 >= a4) break;
            unsigned b0 = *m8(e, a1++);
            if (a1 >= a4) break;
            unsigned b1 = *m8(e, a1++);
            unsigned d0 = b0 | ((b1 << 4) & 0xF00u);
            unsigned cnt = (b1 & 15) + 2;
            for (unsigned k = 0; k <= cnt; k++) {
                uint8_t c = *m8(e, ring + d0);
                d0 = (d0 + 1) & 0xFFF;
                *m8(e, a2++) = c;
                *m8(e, ring + d3) = c;
                d3 = (d3 + 1) & 0xFFF;
            }
        }
    }
    return a2;
}

/* $16A4: COLOR00 = 0, DMA off, unpack the picture at a1 to the screen, then
 * its palette ($E3A) */
static void show_picture(Ending *e, uint32_t a1)
{
    e->colour[0] = 0;
    e->display = 0;
    uint32_t a2 = e->screen;
    unsigned planes = r16(e, a1);                      /* $16C8 */
    a1 += 2;
    uint32_t pal = a1;
    uint32_t end = a2 + (uint32_t)(uint16_t)planes * 0x1F40u;
    a1 += (uint16_t)(2u << planes);
    unsigned zero = r16(e, a1);
    a1 += 2;
    for (unsigned k = 0; k < zero; k++) *m8(e, a2++) = 0;
    unsigned n = r16(e, a1);
    a1 += 2;
    a2 = unpack(e, a1, n, a2);
    if (end > a2) {                                    /* $16EA: the rest cleared */
        uint16_t d0 = (uint16_t)(end - a2);
        for (unsigned k = 0; k < d0; k++) *m8(e, a2++) = 0;
    }
    set_palette(e, pal);
}

/* $1294 (after its wait): every component of the shadow one step down;
 * 1 while any colour was not black */
static int fade_out(Ending *e)
{
    uint16_t d5 = 0;
    for (int k = 0; k < 32; k++) {
        uint16_t d0 = r16(e, e->shadow + 2u * (uint32_t)k);
        d5 |= d0;
        if (d0 & 0xF00) d0 -= 0x100;
        if (d0 & 0x0F0) d0 -= 0x10;
        if (d0 & 0x00F) d0 -= 1;
        e->colour[k] = d0;
        w16(e, e->shadow + 2u * (uint32_t)k, d0);
    }
    return d5 != 0;
}

/* $12C8 (defeat, after its wait): every component one step up to $F; 1 while
 * the colours were not all white */
static int fade_white(Ending *e)
{
    uint32_t d5 = 0;
    for (int k = 0; k < 32; k++) {
        uint16_t d0 = r16(e, e->shadow + 2u * (uint32_t)k);
        d5 += d0;
        if ((d0 & 0xF00) != 0xF00) d0 += 0x100;
        if ((d0 & 0x0F0) != 0x0F0) d0 += 0x10;
        if ((d0 & 0x00F) != 0x00F) d0 += 1;
        e->colour[k] = d0;
        w16(e, e->shadow + 2u * (uint32_t)k, d0);
    }
    return d5 != 0x1FFE0u;
}

/* $1680: XOR one delta frame onto the screen: u16 offset, u16 count - 1,
 * bytes (0 = skip the next byte's count of bytes, which counts 2) */
static void xor_delta(Ending *e, uint32_t a0)
{
    uint32_t a1 = e->screen + r16(e, a0);
    uint16_t d1 = r16(e, a0 + 2);
    a0 += 4;
    for (;;) {
        uint8_t d0 = *m8(e, a0++);
        if (d0) {
            *m8(e, a1++) ^= d0;
            if (d1-- == 0) return;
        } else {
            a1 += *m8(e, a0++);
            d1 = (uint16_t)(d1 - 2);
        }
    }
}

/* $1338: one animation tick. Script longs: $FF end, 4 + n hold n ticks,
 * 0 + address jump, else a delta frame address. One step every `speed`
 * ticks ($13C3; $FF stops it). */
static void anim_tick(Ending *e)
{
    uint32_t pv = e->anim_ptr, dv = e->anim_delay;
    uint16_t delay = r16(e, dv);
    if (delay) { w16(e, dv, (uint16_t)(delay - 1)); return; }
    uint8_t speed = *m8(e, dv + 3);
    if (speed == 0xFF) return;
    uint8_t ph = (uint8_t)(*m8(e, dv + 2) + 1);
    if (ph == speed) ph = 0;
    *m8(e, dv + 2) = ph;
    if (ph) return;
    uint32_t a0 = r32(e, pv);
    if (!a0) return;
    for (;;) {
        uint32_t d0 = r32(e, a0);
        a0 += 4;
        if (d0 == 0xFF) { w32(e, pv, 0); return; }
        if (d0 == 4) {
            w16(e, dv, (uint16_t)r32(e, a0));
            w32(e, pv, a0 + 4);
            return;
        }
        if (d0 == 0) { a0 = r32(e, a0); continue; }
        w32(e, pv, a0);
        xor_delta(e, d0);
        return;
    }
}

void ending_frame(Ending *e)
{
    int defeat = e->which == ENDING_DEFEAT;
    switch (e->state) {
    case S_BLANK:                                      /* $E96 after its wait */
        e->colour[0] = 0;
        e->display = 0;
        show_picture(e, e->card);
        e->count = 501;                                /* $1F4 + dbra */
        e->state = S_HOLD_CARD;
        return;
    case S_HOLD_CARD:
        if (--e->count) return;
        e->state = S_FADE1;                            /* $1294 starts with a wait */
        return;
    case S_FADE1:
        if (fade_out(e)) return;
        show_picture(e, e->scene);
        if (defeat) {
            w32(e, e->anim_ptr, e->anim);              /* $120C */
            e->state = S_ANIM;
        } else {
            e->count = 151;                            /* $96 + dbra */
            e->state = S_HOLD_SCENE;
        }
        return;
    case S_HOLD_SCENE:
        if (--e->count) return;
        set_palette(e, 0x30FA);                        /* $1216 */
        w32(e, e->anim_ptr, e->anim);                  /* $1220 */
        *m8(e, e->anim_delay + 3) = 6;                 /* $122A */
        e->state = S_ANIM;
        return;
    case S_ANIM:
        anim_tick(e);
        if (r32(e, e->anim_ptr)) return;
        e->state = defeat ? S_WHITE : S_FADE2;
        return;
    case S_WHITE:
        if (fade_white(e)) return;
        e->state = S_FADE2;
        return;
    case S_FADE2:
        if (fade_out(e)) return;
        if (defeat) { e->state = S_FADE3; return; }
        show_picture(e, e->final);
        e->state = S_END;
        e->done = 1;
        return;
    case S_FADE3:
        if (fade_out(e)) return;
        show_picture(e, e->final);
        e->state = S_END;
        e->done = 1;
        return;
    default:                                           /* jmp * : nothing more */
        return;
    }
}

/* The data of each program (the code is the same). */
static const struct {
    uint32_t screen, shadow, ring_at, ptr, delay, card, scene, final, anim_at;
    uint32_t music_off;                                /* the music-enable byte cleared at the start */
    uint32_t check_at, check;                          /* a long of the main sequence */
} PROGS[5] = {
    /* victory WIN0-WIN3: the script long at $1222 and the unpacker's ring
     * (the lea at $16B8) differ */
    {0x6C000, 0x1254, 0x16BA, 0x13BC, 0x13C0, 0x313A, 0x1786, 0x2D68, 0x1222, 0x11D0, 0x11E6, 0x43F90000},
    {0x6C000, 0x1254, 0x16BA, 0x13BC, 0x13C0, 0x313A, 0x1786, 0x2D68, 0x1222, 0x11D0, 0x11E6, 0x43F90000},
    {0x6C000, 0x1254, 0x16BA, 0x13BC, 0x13C0, 0x313A, 0x1786, 0x2D68, 0x1222, 0x11D0, 0x11E6, 0x43F90000},
    {0x6C000, 0x1254, 0x16BA, 0x13BC, 0x13C0, 0x313A, 0x1786, 0x2D68, 0x1222, 0x11D0, 0x11E6, 0x43F90000},
    /* defeat WIN_ */
    {0x72000, 0x1240, 0x13EA, 0x13A8, 0x13AC, 0x2A33A, 0x16BE, 0x29FA8, 0x120E, 0xD2A, 0x11E8, 0x43F90002},
};

int ending_init_unpacked(Ending *e, int which, const uint8_t *prog, size_t len)
{
    memset(e, 0, sizeof *e);
    if (which < ENDING_DEFEAT || which > 3) return -1;
    e->mem = calloc(1, MEM);
    if (!e->mem) return -1;
    if (len > MEM - PROG) len = MEM - PROG;
    memcpy(e->mem + PROG, prog, len);
    int i = which < 0 ? 4 : which;
    e->which = which;
    e->screen = PROGS[i].screen;
    e->shadow = PROGS[i].shadow;
    e->anim_ptr = PROGS[i].ptr;
    e->anim_delay = PROGS[i].delay;
    e->card = PROGS[i].card;
    e->scene = PROGS[i].scene;
    e->final = PROGS[i].final;
    e->anim = r32(e, PROGS[i].anim_at);
    e->ring = r32(e, PROGS[i].ring_at);
    if (r32(e, PROGS[i].check_at) != PROGS[i].check) { ending_free(e); return -1; }
    /* $D2A: $AA2 = the screen, cleared ($4E20 longs; defeat $2710); the
     * program then waits ($E96) */
    w32(e, 0xAA2, e->screen);
    *m8(e, PROGS[i].music_off) = 0;                   /* $11DE: the music on */
    memset(e->mem + e->screen, 0, which < 0 ? 0x2710u * 4u : 0x4E20u * 4u);
    e->state = S_BLANK;
    return 0;
}

int ending_init(Ending *e, int which, const uint8_t *file, size_t len)
{
    uint8_t *prog = malloc(MEM - PROG);
    if (!prog) return -1;
    size_t n = lzss_unpack(file, len, prog, MEM - PROG, NULL);
    int r = ending_init_unpacked(e, which, prog, n);
    free(prog);
    return r;
}

void ending_free(Ending *e)
{
    free(e->mem);
    e->mem = NULL;
}

void ending_indices(const Ending *e, uint8_t *idx)
{
    if (!e->display) { memset(idx, 0, 320 * 200); return; }
    const uint8_t *s = e->mem + e->screen;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            unsigned v = 0, bit = 7 - (x & 7);
            for (int p = 0; p < 5; p++)
                v |= ((s[y * 200 + p * 40 + (x >> 3)] >> bit) & 1u) << p;
            idx[y * 320 + x] = (uint8_t)v;
        }
}

const char *ending_file(int which)
{
    static const char *n[] = {"WIN0", "WIN1", "WIN2", "WIN3"};
    return which >= 0 && which <= 3 ? n[which] : "WIN_";
}
const char *ending_music_file(int which) { return which >= 0 ? "WINM" : "DIEM"; }

Ending *ending_new(int which, const uint8_t *prog, size_t len)
{
    Ending *e = malloc(sizeof *e);
    if (e && ending_init_unpacked(e, which, prog, len)) { free(e); return NULL; }
    return e;
}
uint8_t *ending_mem(Ending *e) { return e->mem; }
uint16_t *ending_colours(Ending *e) { return e->colour; }
int ending_display(Ending *e) { return e->display; }
void ending_delete(Ending *e) { if (e) { ending_free(e); free(e); } }

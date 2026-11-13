/*
 * Supremacy port - end-sequence / intro music player. See endmusic.h.
 *
 * Routine names are the END player's addresses (WINM/DIEM); the intro's
 * copy is the same code moved (its address in brackets where it exists).
 * Routines marked [END only] are not in the intro's player. Channel fields
 * are offsets into the $80-byte channel block; most have the same meaning
 * as in the in-game player.
 *
 * Every store wraps to the width of its field, and temporaries are
 * uint32_t like the 68000's registers; word and byte operations on a
 * register keep its other bits where that matters.
 */
#include "endmusic.h"

#include <stdlib.h>
#include <string.h>

/* data tables: same addresses in both players */
#define SILENCE       0x6A000u
#define WAVE_PULSE8   0x6A0F0u     /* pulse wave built per tick (32 bytes) */
#define WAVE_PULSE32  0x6A180u     /* pulse wave built per tick (8 bytes, high notes) */
#define WAVE_6A200    0x6A200u
#define PULSE_SRC     0x6A300u
#define INSTR_PTR     0x6A800u
#define SAMPLE_PTR    0x6A804u
#define SONGS_PTR     0x6A808u
#define ARP_TABLES    0x6A80Cu
#define TIMBRE_TABLES 0x6A88Cu
#define SEQ_NOTE_TABLES 0x6A8C8u
#define WAVEMOD_TABLES  0x6A904u
#define SEQ_WAVE_TABLES 0x6A940u
#define CUT_LEVEL     0x6AA3Du     /* $6865C */
#define DROP_LEVEL    0x6AA3Eu     /* fall-off */
#define GLOBAL_BEND   0x6AA3Fu
#define INIT_LC       0x6B000u     /* init's AUDxLC (outside WINM/DIEM) */

/* state area, relative to the channel blocks ($69480 END, $68F12 intro) */
#define O_PERIODS     0x200u       /* $69680: period table, words */
#define O_DMABITS     0x276u       /* $696F6: 4 longs 1, 2, 4, 8 */
#define O_WAVES       0x29Eu       /* $6971E: long pointers, 32-byte waves */
#define O_WAVES_HI    0x2DEu       /* $6975E: long pointers, 8-byte waves */
#define O_CHANNO      0x31Eu       /* $6979E: long, channel number * 4 */
#define O_TEMPO       0x322u       /* $697A2: word */
#define O_MASK        0x324u       /* $697A4: word, channel mask */
#define O_RATES       0x326u       /* $697A6: envelope rates, words */
#define O_TRANSPOSE   0x366u       /* $697E6: word */
#define O_LOOPED      0x368u       /* $697E8: word, set to 1 on a list loop (never read) */
#define O_PULSE8      0x36Eu       /* $697EE */
#define O_PULSE32     0x376u       /* $697F6 */
#define O_BUFFERS     0x396u       /* $69816: 4 long pointers, build buffers */

#define FADE_REQ      0x69434u     /* END only: byte, fade-out time (never set by the programs) */
#define FADE_DONE     0x69435u     /* END only: byte, 1 once the fade request is taken */

#define LOOP_GUARD    4096

#define FULL(e)       ((e)->variant == ENDMUS_END)
#define A(o)          (e->blocks + (o))

/* ------------------------------------------------------------ memory */
static uint8_t r8(EndMusic *e, uint32_t a)
{
    uint32_t o = (a & 0xFFFFFFu) - e->base;
    if (o < e->len) return e->mem[o];
    e->outside++;
    return 0;
}
static uint32_t r16(EndMusic *e, uint32_t a) { return ((uint32_t)r8(e, a) << 8) | r8(e, a + 1); }
static uint32_t r32(EndMusic *e, uint32_t a) { return (r16(e, a) << 16) | r16(e, a + 2); }
static void w8(EndMusic *e, uint32_t a, uint32_t v)
{
    uint32_t o = (a & 0xFFFFFFu) - e->base;
    if (o < e->len)
        e->mem[o] = (uint8_t)v;
}
static void w16(EndMusic *e, uint32_t a, uint32_t v) { w8(e, a, v >> 8); w8(e, a + 1, v); }
static void w32(EndMusic *e, uint32_t a, uint32_t v) { w16(e, a, v >> 16); w16(e, a + 2, v); }

/* channel fields */
static uint32_t b(EndMusic *e, uint32_t o)  { return r8(e, e->a5 + o); }
static uint32_t w(EndMusic *e, uint32_t o)  { return r16(e, e->a5 + o); }
static uint32_t lg(EndMusic *e, uint32_t o) { return r32(e, e->a5 + o); }
static void sb(EndMusic *e, uint32_t o, uint32_t v) { w8(e, e->a5 + o, v); }
static void sw(EndMusic *e, uint32_t o, uint32_t v) { w16(e, e->a5 + o, v); }
static void sl(EndMusic *e, uint32_t o, uint32_t v) { w32(e, e->a5 + o, v); }

/* Paula */
static void hw(EndMusic *e, uint32_t reg, uint32_t v)
{
    uint16_t r = (uint16_t)reg, x = (uint16_t)v;
    if (e->nwrites < ENDMUS_MAX_WRITES) {
        e->writes[e->nwrites].reg = r;
        e->writes[e->nwrites].val = x;
        e->nwrites++;
    } else {
        e->overflow++;
    }
    if (e->paula)
        e->paula(e->paula_user, r, x);
}
static void hw32(EndMusic *e, uint32_t reg, uint32_t v)
{
    hw(e, reg, v >> 16);
    hw(e, reg + 2, v & 0xFFFFu);
}

/* max(v, 1000) / 1000 on the low word, as cmpi.w #$3E8 / divu.w #$3E8 */
static uint32_t div1000(uint32_t v)
{
    v &= 0xFFFFu;
    return (v <= 1000u ? 1000u : v) / 1000u;
}

/* ------------------------------------------------------------ note set-up */
static void s68696(EndMusic *e)                   /* envelope set-up [$68650] */
{
    uint32_t a3 = e->a3, n, d0;
    if (b(e, 0x4D) & 2) {
        sb(e, 0x27, 3);
        sw(e, 0x66, w(e, 0x68));
        return;
    }
    sb(e, 0x27, 0);
    n = r8(e, a3 + 2) & 0xF0u;
    d0 = n == 0 ? 0x7D01u : r16(e, A(O_RATES) + (n >> 3));
    sw(e, 0x62, d0);
    sw(e, 0x60, 0);
    n = r8(e, a3 + 2) & 0x0Fu;
    d0 = n == 0 ? 0x7D01u : r16(e, A(O_RATES) + n * 2);
    sw(e, 0x68, (r8(e, a3 + 3) & 0xF0u) * 0x10Au);
    sw(e, 0x6A, d0);
    sw(e, 0x6C, 0xFA00);
    n = r8(e, a3 + 3) & 0x0Fu;
    d0 = n == 0 ? 0x7D01u : r16(e, A(O_RATES) + n * 2);
    sw(e, 0x64, d0);
    sw(e, 0x66, 0xFA00);
}

static void s688fe(EndMusic *e)                   /* release point [$688B8] */
{
    int32_t d1 = (int32_t)((r8(e, e->a3 + 4) & 0xF0u) >> 4) + 1;
    int32_t lo = (int16_t)(uint16_t)((w(e, 0x70) << 2) & 0xFFFFu);   /* muls.w */
    sw(e, 0x72, (uint32_t)(lo * d1) >> 6);        /* lsr.l */
}

static void s68b40(EndMusic *e)                   /* vibrato depth set-up [$68AFA] */
{
    uint32_t a3 = e->a3, x = r8(e, a3 + 9), d1, d2;
    if (x == 0) {
        sb(e, 0x25, 0);
        return;
    }
    d1 = ((x & 0xF0u) >> 4) + 1;
    d2 = w(e, 0x70) * d1;                         /* mulu.w, then ror.l #4 */
    sw(e, 0x74, d2 >> 4);
    d1 = (((x & 0x0Eu) ^ 0x0Eu) + 1) & 0xFFu;
    sb(e, 0x25, d1);
    sb(e, 0x29, d1);
    sb(e, 0x26, r8(e, a3 + 1) & 4u);
}

static void s68962(EndMusic *e)                   /* vibrato set-up [$6891C] */
{
    uint32_t a3 = e->a3, x = r8(e, a3 + 5);
    if (x != 0) {
        sb(e, 0x56, 0);
        sb(e, 0x2A, ((x & 0xF0u) >> 4) ^ 0x0Fu);
        sb(e, 0x2B, x & 0x0Fu);
        sb(e, 0x2C, 0x80);
        sb(e, 0x2E, 0);
        sb(e, 0x2F, (r8(e, a3 + 6) & 0xF0u) >> 2);   /* $68B14 */
        s68b40(e);
    } else {
        sb(e, 0x2C, 0x80);
        sb(e, 0x2A, 0x0F);
        sb(e, 0x56, 0);
    }
}

static void s68b94(EndMusic *e)                   /* arpeggio select [END only] */
{
    uint32_t p = lg(e, 0x0C), op = r8(e, p);
    if (op >= 0x5F && op <= 0x7F) {
        uint32_t d0, t;
        sl(e, 0x0C, p + 1);
        d0 = (((op - 0x60u) & 0xFFu) << 2) & 0xFFFFu;   /* $5F wraps to $3FC */
        t = r32(e, ARP_TABLES + d0);
        sl(e, 0x18, t);
        sb(e, 0x30, 1);
        sb(e, 0x31, (r8(e, t) & 0xF0u) >> 4);
        sb(e, 0x3C, r8(e, t) & 0x0Fu);
        sb(e, 0x33, 1);
        sb(e, 0x32, r8(e, t + 1));
    } else if (b(e, 0x33) == 0) {
        sb(e, 0x32, 0);
        sb(e, 0x30, 0);
    } else {
        sb(e, 0x30, 1);
        sb(e, 0x32, 0);
    }
}

static void s68c94(EndMusic *e)                   /* fall-off set-up [END only] */
{
    if (r8(e, e->a3 + 7) & 2) {
        sb(e, 0x34, 1);
        sb(e, 0x35, 0);
        sb(e, 0x36, 0);
    } else {
        sb(e, 0x34, 0);
    }
}

static void s68d8c(EndMusic *e)                   /* $FD slide set-up [$68BE2] */
{
    uint32_t p = lg(e, 0x0C);
    if (r8(e, p) == 0xFD) {
        uint32_t x = r8(e, p + 1), d0;
        sb(e, 0x3B, (x & 0xF0u) >> 3);
        sb(e, 0x38, ((x & 0x0Fu) - 1u) ^ 0x0Fu);  /* nibble 0 gives $F0 */
        d0 = ((r8(e, p + 2) + b(e, 0x1D)) & 0xFFu) + r16(e, A(O_TRANSPOSE));
        sb(e, 0x7A, d0);
        sb(e, 0x39, d0);
        sb(e, 0x1F, d0);
        d0 = ((r8(e, p + 3) + b(e, 0x1D)) & 0xFFu) + r16(e, A(O_TRANSPOSE));
        sb(e, 0x3A, d0);
        sb(e, 0x7B, 0x80);
    } else {
        sb(e, 0x38, 0);
        sb(e, 0x7B, 0x80);
        sb(e, 0x56, 0);
    }
}

static void s68ed0(EndMusic *e)                   /* timbre sweep set-up [END only] */
{
    uint32_t a3 = e->a3, d1 = r8(e, a3 + 6) & 0x0Fu, t;
    if (d1 == 0) {
        sb(e, 0x3D, 0);
        sb(e, 0x4E, 0);
        sl(e, 0x08, 0);
        return;
    }
    if (b(e, 0x4E) & 2)
        return;
    t = r32(e, TIMBRE_TABLES + (((d1 - 1u) & 0xFFu) << 2));
    sl(e, 0x08, t);
    sb(e, 0x3F, 2);
    sb(e, 0x21, r8(e, a3));
    sb(e, 0x3E, 0);
    sb(e, 0x3D, r8(e, t + 3));
    if (b(e, 0x4E) != 0)
        sb(e, 0x4E, b(e, 0x4E) | 2);
}

static void s69082(EndMusic *e)                   /* note/wave sequence set-up [END only] */
{
    uint32_t d1 = r8(e, e->a3 + 7) & 0xF0u;
    if (d1 == 0) {
        sl(e, 0x40, 0);
        return;
    }
    d1 = ((d1 - 0x10u) & 0xFFu) >> 2;
    sl(e, 0x44, r32(e, SEQ_NOTE_TABLES + d1));
    sl(e, 0x40, r32(e, SEQ_WAVE_TABLES + d1));
    sb(e, 0x49, 1);
    sb(e, 0x4A, 1);
}

static void s69228(EndMusic *e)                   /* wave-pointer sweep set-up [END only] */
{
    uint32_t d0 = r8(e, e->a3 + 4) & 0x0Fu, t;
    if (d0 == 0) {
        sl(e, 0x7C, 0);
        return;
    }
    d0 = ((d0 - 1u) & 0xFFu) * 4u;
    e->a0 = WAVEMOD_TABLES;
    t = r32(e, WAVEMOD_TABLES + d0);
    sl(e, 0x7C, t);
    sb(e, 0x22, 4);
    sb(e, 0x77, r8(e, t + 1));
    sb(e, 0x23, r8(e, t + 2));
    sb(e, 0x1C, r8(e, t + 3));
}

/* ------------------------------------------------------------ per-tick */
static void t68772(EndMusic *e)                   /* volume envelope [$6872C] */
{
    uint32_t st = b(e, 0x4D), d0, d2;
    if (st != 0) {
        if (st & 4) {
            sb(e, 0x4D, 0);
            goto release;                         /* jmp $68894 */
        }
        sb(e, 0x4D, st | 2);
    }
    if (b(e, 0x27) == 0) {                        /* attack */
        d0 = w(e, 0x60) + w(e, 0x62);
        if (d0 <= 0xFA00u) {
            sw(e, 0x60, d0);
            sb(e, 0x28, div1000(d0));
            return;
        }
        sb(e, 0x28, 0x40);
        sb(e, 0x27, 1);
        sw(e, 0x60, 0xFA00);
    }
    if (b(e, 0x27) == 1) {                        /* decay */
        d0 = w(e, 0x6C) + 100000u - w(e, 0x6A);
        d2 = w(e, 0x68) + 100000u;
        if (d0 >= d2) {
            d0 -= 100000u;
            sw(e, 0x6C, d0);
            sb(e, 0x28, div1000(d0));
            return;
        }
        sb(e, 0x27, 3);
        d0 = w(e, 0x68);
        sw(e, 0x66, d0);
        sb(e, 0x28, d0 / 1000u);                  /* no 1000 floor here */
    }
    if (b(e, 0x27) != 3)
        return;
    if (b(e, 0x4D) != 0) {                        /* held */
        sb(e, 0x28, div1000(w(e, 0x66)));
        return;
    }
release:                                          /* $68894 */
    if (b(e, 0x4C) != 1 && w(e, 0x72) < w(e, 0x70))
        return;
    d0 = w(e, 0x66) + 10000u - w(e, 0x64);
    if (d0 < 10000u) {
        sb(e, 0x28, 0);
        return;
    }
    d0 -= 10000u;
    sw(e, 0x66, d0);
    sb(e, 0x28, div1000(d0));
}

static void t68e04(EndMusic *e)                   /* slide step [$68C5A] */
{
    uint32_t v;
    if (b(e, 0x38) == 0)
        return;
    if (b(e, 0x3B) != 0) {
        sb(e, 0x3B, b(e, 0x3B) - 1);
        return;
    }
    if (w(e, 0x70) == 0) {
        sb(e, 0x38, 0);
        sl(e, 0x0C, lg(e, 0x0C) + 4);
        return;
    }
    if (b(e, 0x56) == 1) {
        sb(e, 0x56, 1);
        return;
    }
    if (b(e, 0x39) < b(e, 0x3A)) {                /* up ($68E88) */
        if (b(e, 0x1F) == b(e, 0x3A)) {
            sb(e, 0x56, 1);
            return;
        }
        v = (b(e, 0x7B) + b(e, 0x38)) & 0xFFu;
        sb(e, 0x7B, v);
        if (v > 0x90) {
            sb(e, 0x7B, v - 0x90u + 0x80u);
            sb(e, 0x1F, b(e, 0x1F) + 1);
        }
    } else {                                      /* down */
        if (b(e, 0x1F) == b(e, 0x3A)) {
            sb(e, 0x56, 1);
            return;
        }
        v = (b(e, 0x7B) - b(e, 0x38)) & 0xFFu;
        sb(e, 0x7B, v);
        if (v < 0x70) {
            sb(e, 0x1F, b(e, 0x1F) - 1);
            sb(e, 0x7B, 0x80u - ((0x70u - v) & 0xFFu));
        }
    }
}

static void t68cb4(EndMusic *e)                   /* fall-off step [END only] */
{
    if (b(e, 0x34) == 0)
        return;
    if (b(e, 0x35) == 0) {
        sb(e, 0x36, 0);
        sb(e, 0x35, b(e, 0x35) ^ 1);
        return;
    }
    if (r8(e, DROP_LEVEL) < w(e, 0x70))
        return;
    sb(e, 0x34, b(e, 0x34) + 1);
    sb(e, 0x35, b(e, 0x35) ^ 1);
    sb(e, 0x36, b(e, 0x34));
}

static void t68c16(EndMusic *e)                   /* arpeggio step [END only] */
{
    int guard;
    if (b(e, 0x30) == 0)
        return;
    if (b(e, 0x31) != 0) {
        sb(e, 0x31, b(e, 0x31) - 1);
        return;
    }
    for (guard = 0; guard < LOOP_GUARD; guard++) {   /* $68C32 */
        uint32_t t = lg(e, 0x18), v;
        sb(e, 0x31, (r8(e, t) & 0xF0u) >> 4);
        sb(e, 0x30, b(e, 0x30) + 1);
        v = r8(e, t + b(e, 0x30));
        if (v == 0xFF) {
            sb(e, 0x30, b(e, 0x3C));
            continue;
        }
        if (v == 0xFE) {
            sb(e, 0x30, b(e, 0x30) - 2);
            continue;
        }
        sb(e, 0x32, v);
        if (r8(e, e->a3 + 8) != 0) {
            sb(e, 0x54, 0);
            sb(e, 0x57, 1);
        }
        return;
    }
}

static void t68f32(EndMusic *e)                   /* timbre sweep step [END only] */
{
    int guard;
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        if (lg(e, 0x08) == 0)
            return;
        if (!(b(e, 0x4E) & 4))
            sb(e, 0x4E, 0);
        if (b(e, 0x3D) == 0) {                    /* $68FAA: next segment */
            uint32_t t = lg(e, 0x08), d0 = b(e, 0x3F);
            if (r8(e, t + d0 + 2) == 0xFF) {
                sb(e, 0x3D, r8(e, t + d0));
            } else {
                sb(e, 0x3F, d0 + 2);
                sb(e, 0x3D, r8(e, t + d0 + 2));
            }
            continue;                             /* jmp $68F32 */
        }
        for (; guard < LOOP_GUARD; guard++) {      /* $68F56 */
            uint32_t t = lg(e, 0x08), step;
            int d0;
            sb(e, 0x3D, b(e, 0x3D) - 1);          /* may wrap below 0 after a reflection */
            step = r8(e, t + b(e, 0x3F) + 1);
            if (b(e, 0x3E) == 0) {                /* rising */
                int hi = r8(e, t + 1);
                d0 = (int)step + (int)b(e, 0x21);
                if (d0 <= hi) {
                    sb(e, 0x21, (uint32_t)d0);
                    return;
                }
                sb(e, 0x21, (uint32_t)(hi - (d0 - hi)));      /* reflect off the top */
                sb(e, 0x3E, b(e, 0x3E) ^ 1);
            } else {                              /* falling ($68FE4) */
                int lo = r8(e, t);
                d0 = (int)b(e, 0x21) + 0x1000 - (int)step;
                if (d0 >= lo + 0x1000) {
                    sb(e, 0x21, (uint32_t)(d0 - 0x1000));
                    return;
                }
                sb(e, 0x3E, b(e, 0x3E) ^ 1);
                sb(e, 0x21, (uint32_t)(lo + (lo + 0x2000 - d0)));   /* reflect off the bottom */
            }
        }
    }
}

static uint32_t seq_next(EndMusic *e, uint32_t a2, uint32_t idx)
{
    uint32_t v = 0;
    int guard;
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        v = r8(e, a2 + b(e, idx));
        if (v == 0xFF)
            sb(e, idx, r8(e, a2) + 1);
        else if (v == 0xFE)
            sb(e, idx, b(e, idx) - 1);
        else
            break;
    }
    return v;
}

static void t690ca(EndMusic *e)                   /* note/wave sequence step [END only] */
{
    if (lg(e, 0x40) == 0)
        return;
    sb(e, 0x4B, seq_next(e, lg(e, 0x40), 0x49));
    sb(e, 0x49, b(e, 0x49) + 1);
    sb(e, 0x55, seq_next(e, lg(e, 0x44), 0x4A));
    sb(e, 0x4A, b(e, 0x4A) + 1);
    sb(e, 0x4C, (b(e, 0x4B) & 1) ^ 1);
    sb(e, 0x54, 0);
}

static void t6865c(EndMusic *e)                   /* cut flag $48 [$68616] (never read) */
{
    uint32_t v;
    if (!(r8(e, e->a3 + 7) & 8) || (v = r8(e, CUT_LEVEL)) == 0) {
        sb(e, 0x48, 0);
        return;
    }
    w8(e, e->cmp_imm, v);                         /* self-modifies cmpi.w #imm,$70(a5) */
    if (w(e, 0x70) > r16(e, e->cmp_imm - 1))
        sb(e, 0x48, 0);
    else
        sb(e, 0x48, 1);
}

static void t6926e(EndMusic *e)                   /* wave-pointer sweep step [END only] */
{
    uint32_t d0;
    if (lg(e, 0x7C) == 0)
        return;
    if (b(e, 0x23) != 0) {
        sb(e, 0x23, b(e, 0x23) - 1);
    } else {
        uint32_t t = lg(e, 0x7C);
        e->a0 = t;
        d0 = b(e, 0x22);
        if (r8(e, t + d0) == 0xFF)
            return;
        sb(e, 0x22, d0 + 2);
        sb(e, 0x23, r8(e, t + d0));
        sb(e, 0x1C, r8(e, t + d0 + 1));
    }
    d0 = b(e, 0x1C);
    if (d0 <= 0x7F)
        sb(e, 0x77, b(e, 0x77) + d0);
    else
        sb(e, 0x77, b(e, 0x77) - (d0 & 0x7Fu));
}

static void t68ac0(EndMusic *e)                   /* vibrato depth step [$68A7A] */
{
    if (w(e, 0x74) < w(e, 0x70))
        return;
    if (b(e, 0x29) != 1) {
        sb(e, 0x29, b(e, 0x29) - 1);
        return;
    }
    sb(e, 0x29, b(e, 0x25));
    if (b(e, 0x26) != 0) {
        if (b(e, 0x2A) != 0)
            sb(e, 0x2A, b(e, 0x2A) - 1);
    } else if (b(e, 0x2A) != 0x0E) {
        sb(e, 0x2A, b(e, 0x2A) + 1);
    }
}

static void t689ba(EndMusic *e)                   /* vibrato step [$68974] */
{
    int guard;
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        if (b(e, 0x38) != 0 && b(e, 0x56) != 1)
            return;
        if (b(e, 0x2A) == 0x0F)
            return;
        if (b(e, 0x2E) == 0) {
            if (!(((b(e, 0x2A) + 0x80u) & 0xFFu) < b(e, 0x2C))) {
                sb(e, 0x2C, b(e, 0x2C) + b(e, 0x2B));
                return;
            }
        } else {
            if (!(((0x80u - b(e, 0x2A)) & 0xFFu) > b(e, 0x2C))) {
                sb(e, 0x2C, b(e, 0x2C) - b(e, 0x2B));
                return;
            }
        }
        sb(e, 0x2E, b(e, 0x2E) ^ 1);             /* $68A00 */
        t68ac0(e);
    }
}

static void t68b26(EndMusic *e)                   /* vibrato delay [$68AE0] */
{
    if (b(e, 0x2F) != 0)
        sb(e, 0x2F, b(e, 0x2F) - 1);
    else
        t689ba(e);
}

static void t68cf8(EndMusic *e)                   /* note index [$68B4E] */
{
    uint32_t d1 = r8(e, e->a3 + 7) & 4u, d0 = b(e, 0x1F);
    if (lg(e, 0x40) != 0) {
        uint32_t d2 = b(e, 0x55);
        d0 = (d2 & 0x80u) ? ((d0 + (d2 & 0x7Fu)) & 0xFFu) : d2;
    }
    d0 = (d0 + b(e, 0x32)) & 0xFFu;
    d0 = d1 ? ((d0 + b(e, 0x36)) & 0xFFu) : ((d0 - b(e, 0x36)) & 0xFFu);
    if (b(e, 0x36) != 0)
        d0 = (d0 + 1u) & 0xFFu;
    sb(e, 0x37, d0 <= 0x54 ? d0 : 2);
}

static void t68a2e(EndMusic *e)                   /* fine pitch offset [$689E8] */
{
    uint32_t d0 = (b(e, 0x37) << 1), d1, d2, prod, q;
    if (d0 >= 0x70)
        d0 -= 0x30;
    d1 = r16(e, A(O_PERIODS) + d0) - r16(e, A(O_PERIODS) + d0 + 6);
    if (b(e, 0x38) != 0 && b(e, 0x56) != 1)
        d2 = b(e, 0x7B);
    else
        d2 = b(e, 0x2C);
    d2 -= 0x70u;
    if (r8(e, e->a3 + 7) & 1) {
        if (b(e, 0x1E) != 0)
            d2 += b(e, 0x24) >> 4;
        else if (r8(e, GLOBAL_BEND) != 0)
            d2 += (uint32_t)r8(e, GLOBAL_BEND) >> 4;
    }
    prod = (d1 & 0xFFFFu) * (d2 & 0xFFFFu);       /* mulu.w */
    q = prod / 0x30u;
    sw(e, 0x78, q > 0xFFFFu ? prod : q);          /* divu overflow leaves d1 */
}

/* ------------------------------------------------------------ output */
static void o68478(EndMusic *e)                   /* AUDxPER/LEN/LC [$68432] */
{
    uint32_t d0 = e->a5 - e->blocks, d1, d2, d3, d4, d5, a0, a2;
    int i;
    if (d0)
        d0 = (d0 & 0xFFFFu) >> 5;
    w32(e, A(O_CHANNO), d0);
    if (b(e, 0x54) == 0) {
        hw(e, 0x096, r32(e, A(O_DMABITS) + d0));   /* DMA off for the new note */
        e->a0 = lg(e, 0x10);
        d1 = b(e, 0x37) << 1;                     /* no high-note adjustment here */
        goto out;
    }
    d1 = b(e, 0x37) << 1;
    if (lg(e, 0x10) == WAVE_6A200) {
        e->a0 = WAVE_6A200;
        sw(e, 0x50, 0x80);
        goto rest;
    }
    if (lg(e, 0x7C) != 0) {
        sw(e, 0x50, 0x40);
        e->a0 = ((b(e, 0x77) & 0xF0u) << 3) + INSTR_PTR;
        goto rest;
    }
    if (d1 >= 0x70) {                             /* high notes: 8-byte waves */
        d1 -= 0x30;
        a2 = r32(e, A(O_WAVES_HI) + b(e, 0x76));
        if (a2 == WAVE_PULSE32) {
            d4 = b(e, 0x21);
            d5 = d4;
            d4 >>= 5;
            d3 = d4;
            d2 = d3 << 5;
            /* adda.l d4,a1 (dead: a1 is reloaded), so the 8 source bytes are
             * always $6A180-$6A187 (the in-game player offsets them) */
            a0 = r32(e, A(O_BUFFERS) + r32(e, A(O_CHANNO)));
            for (i = 0; i < 8; i++)
                w8(e, a0 + (uint32_t)i, r8(e, a2 + (uint32_t)i));
            w8(e, a0 + d3, r8(e, A(O_PULSE32) + (d5 - d2)));
            e->a0 = a0;
            sw(e, 0x50, 4);
            goto rest;
        }
        sw(e, 0x50, 4);
        goto out;                                 /* a0 left over from the channel's routines */
    }
    if (lg(e, 0x10) == WAVE_PULSE8) {             /* pulse wave built per tick */
        d4 = b(e, 0x21);
        d5 = d4;
        d4 >>= 3;
        d3 = d4;
        d2 = d3 << 3;
        a2 = PULSE_SRC + (d4 << 5);
        a0 = r32(e, A(O_BUFFERS) + r32(e, A(O_CHANNO)));
        for (i = 0; i < 32; i++)
            w8(e, a0 + (uint32_t)i, r8(e, a2 + (uint32_t)i));
        w8(e, a0 + d3, r8(e, A(O_PULSE8) + (d5 - d2)));
        e->a0 = a0;
        sw(e, 0x50, 0x10);
        goto rest;
    }
    e->a0 = lg(e, 0x10);                          /* $68606 */
    if (r8(e, e->a3 + 8) == 0)
        sw(e, 0x50, 0x10);
rest:                                             /* $6861A */
    if (b(e, 0x7A) == 0) {
        e->a0 = SILENCE;
        sw(e, 0x50, 0x10);
    }
out:                                              /* $68630 */
    if (b(e, 0x57) == 2)
        return;
    hw(e, e->hwb + 6u, r16(e, A(O_PERIODS) + d1) - w(e, 0x78));
    hw(e, e->hwb + 4u, w(e, 0x50));
    hw32(e, e->hwb + 0u, e->a0);
}

static void o6918a(EndMusic *e)                   /* AUDxVOL with fades [$68D26] */
{
    uint32_t d0, d1, q;
    uint32_t mode = b(e, 0x5E);
    if (mode == 0) {
        hw(e, e->hwb + 8u, b(e, 0x28));
        return;
    }
    d0 = lg(e, 0x5A);
    if (mode == 1) {                              /* fade out ($691B8) */
        d0 += 1000u;
        d0 = (d0 & 0xFFFF0000u) | ((d0 - w(e, 0x52)) & 0xFFFFu);   /* sub.w */
        if (d0 <= 1000u) {
            hw(e, e->hwb + 8u, 0);                /* clr.w AUDxVOL */
            return;
        }
        d0 -= 1000u;
    } else {                                      /* fade in */
        d0 = (d0 & 0xFFFF0000u) | ((d0 + w(e, 0x52)) & 0xFFFFu);   /* add.w */
        if (d0 >= 0xFA00u) {
            hw(e, e->hwb + 8u, 0x40);
            return;
        }
    }
    sl(e, 0x5A, d0);                              /* $691DC */
    if (d0 <= 1000u)
        d0 = 1000u;
    q = d0 / 1000u;
    if (q <= 0xFFFFu)                             /* divu.w overflow leaves d0 */
        d0 = q;
    d0 &= 0xFFFFu;
    d1 = (b(e, 0x28) * d0) & 0xFFFFu;             /* mulu.w; lsr.w #6; andi.l #$FF */
    hw(e, e->hwb + 8u, (d1 >> 6) & 0xFFu);
}

/* ------------------------------------------------------------ channel */
static void sustain(EndMusic *e)                  /* $6838A [$68362] */
{
    sw(e, 0x70, w(e, 0x70) - 1);
    t68772(e);
    t68e04(e);
    if (FULL(e)) {
        t68cb4(e);
        t68c16(e);
        t68f32(e);
        t690ca(e);
    }
    t6865c(e);
    if (FULL(e))
        t6926e(e);
    t68b26(e);
    t68cf8(e);
    t68a2e(e);
}

/* the $FD/$FC list entries: fade time in 1/50 s */
static void fade_rate(EndMusic *e, uint32_t a1)
{
    uint32_t d0 = (r32(e, a1 + 4) & 0xFFFFu) * 50u, div = d0 & 0xFFFFu;
    if (div)                                      /* 0: the 68000 traps (never in the data) */
        sw(e, 0x52, 0xFA00u / div);
}

static void list_and_note(EndMusic *e)            /* $680B2 [$680AE] */
{
    uint32_t a1, op, p;
    int guard = 0;
    if (b(e, 0x20) == 1)
        goto pattern;
    for (; guard < LOOP_GUARD; guard++) {
        /* $680BC: the pattern list (longs) */
        a1 = lg(e, 0x04);
        if (r32(e, a1) == 0xFD) {                 /* fade out */
            fade_rate(e, a1);
            sb(e, 0x5E, 1);
            sl(e, 0x5A, 0xFA00);
            a1 += 8;
        }
        if (r32(e, a1) == 0xFC) {                 /* fade in */
            fade_rate(e, a1);
            sb(e, 0x5E, 2);
            sl(e, 0x5A, 0);
            a1 += 8;
        }
        if (r32(e, a1) == 0xFF) {                 /* loop */
            sl(e, 0x04, lg(e, 0x00));
            w16(e, A(O_LOOPED), 1);
            sb(e, 0x1E, 0);
            continue;
        }
        if (r32(e, a1) == 0xFE)                   /* stop here (every tick) */
            return;
        if (r32(e, a1) == 0xFB) {                 /* pitch bend */
            sb(e, 0x24, r32(e, a1 + 4));
            sb(e, 0x1E, 1);
            a1 += 8;
        }
        if (r32(e, a1) <= 0x100u) {
            if (r32(e, a1) >= 0x7Fu) {            /* transpose */
                sb(e, 0x1D, r32(e, a1) - 0x80u);
                a1 += 4;
                if (r32(e, a1) > 0x7Fu)
                    goto store;
            }
            sb(e, 0x4F, r32(e, a1) - 0x60u);      /* instrument offset */
            a1 += 4;
        }
store:                                            /* $681B0 */
        sl(e, 0x04, a1);
        sl(e, 0x0C, r32(e, lg(e, 0x04)));
        sb(e, 0x20, 1);
        sb(e, 0x33, 0);
        sb(e, 0x30, 0);
pattern:                                          /* $681CA: the pattern (bytes) */
        p = lg(e, 0x0C);
        if (r8(e, p) == 0xFF) {
            sb(e, 0x20, 0);
            sl(e, 0x04, lg(e, 0x04) + 4);
            continue;
        }
        op = r8(e, p);
        if (op >= 0xC1 && op <= 0xF9) {           /* instrument select */
            uint32_t d0 = (((op - 0xC1u) & 0xFFu) + b(e, 0x4F)) & 0xFFu;
            uint32_t a2 = r32(e, INSTR_PTR), a3;
            d0 *= 10u;
            sl(e, 0x14, d0);
            a3 = a2 + d0;
            e->a3 = a3;
            sb(e, 0x21, r8(e, a3));
            if (r8(e, a3 + 8) != 0) {             /* sample */
                uint32_t d1 = (((r8(e, a3 + 8) - 1u) & 0xFFu) << 3) & 0xFFFFu;
                uint32_t s = r32(e, SAMPLE_PTR), d2;
                sl(e, 0x10, r32(e, s + d1));
                sw(e, 0x50, (r16(e, s + d1 + 6) & 0xFFFEu) >> 1);
                d2 = r8(e, s + d1 + 7) & 1u;
                sb(e, 0x57, d2);
                sb(e, 0x2D, d2);
                sb(e, 0x54, 0);
            } else {                              /* synth wave ($6826E) */
                uint32_t d2 = (uint32_t)r8(e, a2 + d0 + 1) >> 2;
                sb(e, 0x57, 0);
                sb(e, 0x76, d2);
                sl(e, 0x10, r32(e, A(O_WAVES) + d2));
                sw(e, 0x50, 0x10);
                sb(e, 0x54, 1);
            }
            sl(e, 0x0C, lg(e, 0x0C) + 1);
        }
        p = lg(e, 0x0C);                          /* $6829C: durations */
        if (r8(e, p) >= 0x7F && r8(e, p) <= 0xC0) {
            sw(e, 0x6E, 0);
            do {
                uint32_t d0 = (r8(e, p) - 0x80u) & 0xFFFFu;   /* $7F gives $FFFF */
                sw(e, 0x6E, w(e, 0x6E) + d0 * r16(e, A(O_TEMPO)));
                sl(e, 0x0C, lg(e, 0x0C) + 1);
                p++;
            } while (r8(e, p) >= 0x7F && r8(e, p) <= 0xC0);
        }
        sw(e, 0x70, w(e, 0x6E));                  /* no duration: the last one again */
        s68696(e);
        if (FULL(e))
            s68b94(e);
        s68d8c(e);
        p = lg(e, 0x0C);                          /* $68920 */
        op = r8(e, p);
        if (op == 0xFC) {
            sl(e, 0x0C, p + 1);
            sb(e, 0x4D, 1);
            goto pattern;
        }
        if (op == 0xFE) {
            sb(e, 0x4D, b(e, 0x4D) | 4);
            sl(e, 0x0C, p + 1);
            goto pattern;
        }
        if (FULL(e) && op == 0xFA) {              /* $69040 */
            sl(e, 0x0C, p + 1);
            sb(e, 0x4E, 1);
            goto pattern;
        }
        if (FULL(e) && op == 0xFB) {
            sl(e, 0x0C, p + 1);
            sb(e, 0x4E, b(e, 0x4E) | 4);
            goto pattern;
        }
        if (b(e, 0x38) == 0) {                    /* $68310: the note */
            uint32_t d0 = r8(e, p);
            sb(e, 0x7A, d0);
            d0 = (d0 + b(e, 0x1D)) & 0xFFu;
            d0 = d0 + r16(e, A(O_TRANSPOSE));
            sb(e, 0x1F, d0);
            if (r8(e, e->a3 + 8) != 0) {
                sb(e, 0x54, 0);
                sb(e, 0x57, b(e, 0x2D));
            }
            sl(e, 0x0C, p + 1);
        }
        s688fe(e);
        if (b(e, 0x4D) & 2) {
            sustain(e);
            return;
        }
        if (FULL(e)) {
            s68c94(e);
            s68ed0(e);
        }
        s68962(e);
        if (FULL(e)) {
            s69082(e);
            s69228(e);
        }
        sw(e, 0x70, w(e, 0x70) - 1);              /* no pitch step on a new note */
        return;
    }
}

static void channel(EndMusic *e)                  /* $6806A [$68066] */
{
    uint32_t d1;
    if (r8(e, e->credits) == 0x4D) {              /* 'M' of "Music coding ..." */
        d1 = e->a5 - e->blocks;
        if (d1)
            d1 = (d1 & 0xFFFFu) >> 5;
        e->a0 = A(O_DMABITS);
        if ((r16(e, A(O_MASK)) & r32(e, A(O_DMABITS) + d1) & 0xFFFFu) != 0) {
            e->a3 = lg(e, 0x14) + r32(e, INSTR_PTR);
            if (w(e, 0x70) != 0)
                sustain(e);
            else
                list_and_note(e);
        }
    }
    o68478(e);                                    /* $683D2 */
    o6918a(e);
}

static void fade_request(EndMusic *e)             /* $692D6 [END only] */
{
    uint32_t d0, q, n;
    if (r8(e, FADE_DONE) == 1)
        return;
    d0 = r8(e, FADE_REQ);
    if (d0 == 0)
        return;
    q = 0xFA00u / (d0 * 50u);
    for (n = 0; n < 4; n++) {
        uint32_t blk = e->blocks + 0x80u * n;
        w16(e, blk + 0x52, q);
        w8(e, blk + 0x5E, 1);
        w32(e, blk + 0x5A, 0xFA00);
    }
    w8(e, FADE_DONE, 1);
}

static void end_of_tick(EndMusic *e)              /* $683E0 [$6839A] */
{
    uint32_t n;
    /* The original first waits about 4 raster lines (it writes VHPOS + 4
     * into its own cmpi.b at wait_imm and polls $DFF006); dropped here. */
    for (n = 0; n < 4; n++) {
        uint32_t hwb = 0xA0u + 16u * n;
        e->a5 = e->blocks + 0x80u * n;
        if (b(e, 0x54) == 0) {
            hw32(e, hwb, lg(e, 0x10));
            hw(e, hwb + 4, w(e, 0x50));
            hw(e, 0x096, r32(e, A(O_DMABITS) + 4u * n) + 0x8000u);
            sb(e, 0x54, 1);
        } else if (b(e, 0x57) == 1) {
            hw32(e, hwb, SILENCE);
            hw(e, hwb + 4, 8);
            sb(e, 0x57, 2);
        }
    }
}

/* ------------------------------------------------------------ entry points */
SND_API void endmus_start(EndMusic *e, int song)  /* $69330 [$68DC4] */
{
    uint32_t a, n, d0, a2;
    e->nwrites = 0;
    for (a = e->blocks; a < e->blocks + 0x200u; a++)
        w8(e, a, 0);
    for (n = 0; n < 4; n++)
        w8(e, e->blocks + 0x80u * n + 0x54, 1);
    d0 = ((uint32_t)song & 0xFFFFu) * 0x18u;
    a2 = r32(e, SONGS_PTR);
    for (n = 0; n < 4; n++) {
        uint32_t p = r32(e, a2 + d0 + 4u * n);
        w32(e, e->blocks + 0x80u * n, p);         /* list start */
        w32(e, e->blocks + 0x80u * n + 4, p);     /* list position */
    }
    w16(e, A(O_MASK), r16(e, a2 + d0 + 0x12));
    w16(e, A(O_TEMPO), r16(e, a2 + d0 + 0x10));
    w16(e, A(O_TRANSPOSE), r16(e, a2 + d0 + 0x14));
    e->filter_off = 1;                            /* ori.b #2,$BFE001 */
    hw(e, 0x096, 0x000F);
    for (n = 0; n < 4; n++)
        hw32(e, 0xA0u + 16u * n, INIT_LC);
    for (n = 0; n < 4; n++)
        hw(e, 0xA4u + 16u * n, 0x10);
    hw(e, 0x096, 0x8000u + r16(e, A(O_MASK)));
}

SND_API void endmus_tick(EndMusic *e)             /* $6800C [$6800E] */
{
    uint32_t n;
    e->nwrites = 0;
    if (!e->keep_regs)
        e->a0 = e->a3 = 0;
    for (n = 0; n < 4; n++) {
        e->a5 = e->blocks + 0x80u * n;
        e->hwb = (uint16_t)(0xA0u + 16u * n);
        channel(e);
    }
    if (FULL(e))
        fade_request(e);
    end_of_tick(e);
}

/* ------------------------------------------------------------ set-up */
SND_API int endmus_init(EndMusic *e, const uint8_t *file, size_t len, int variant)
{
    static const uint8_t jmp_end[12] = {0x4E, 0xF9, 0x00, 0x06, 0x93, 0x30,
                                        0x4E, 0xF9, 0x00, 0x06, 0x80, 0x0C};
    static const uint8_t jmp_intro[12] = {0x4E, 0xF9, 0x00, 0x06, 0x8D, 0xC4,
                                          0x4E, 0xF9, 0x00, 0x06, 0x80, 0x0E};
    uint32_t base = variant == ENDMUS_INTRO ? ENDMUS_INTRO_BASE : ENDMUS_END_BASE;
    memset(e, 0, sizeof *e);
    if (!file || (variant != ENDMUS_END && variant != ENDMUS_INTRO) ||
        len < 0x6AB00u - base || len > 0x80000u)
        return -1;
    if (memcmp(file + (0x68000u - base), variant == ENDMUS_INTRO ? jmp_intro : jmp_end, 12) != 0)
        return -1;
    e->mem = (uint8_t *)malloc(len);
    if (!e->mem)
        return -1;
    memcpy(e->mem, file, len);
    e->base = base;
    e->len = (uint32_t)len;
    e->variant = variant;
    if (variant == ENDMUS_INTRO) {
        e->blocks = 0x68F12u;
        e->credits = 0x68EC8u;
        e->cmp_imm = 0x6863Bu;
        e->wait_imm = 0x683AEu;
    } else {
        e->blocks = 0x69480u;
        e->credits = 0x69436u;
        e->cmp_imm = 0x68681u;
        e->wait_imm = 0x683F4u;
    }
    e->a5 = e->blocks;
    e->hwb = 0xA0;
    return 0;
}

SND_API void endmus_free(EndMusic *e)
{
    free(e->mem);
    e->mem = NULL;
    e->len = 0;
}

SND_API void endmus_set_paula(EndMusic *e, seq_paula_fn fn, void *user)
{
    e->paula = fn;
    e->paula_user = user;
}

SND_API uint8_t *endmus_ptr(EndMusic *e, uint32_t addr)
{
    uint32_t o = (addr & 0xFFFFFFu) - e->base;
    return o < e->len ? &e->mem[o] : NULL;
}

/* ------------------------------------------------------------ sound output */
SND_API int endsnd_init(EndSound *o, const uint8_t *file, size_t len, int variant, int rate)
{
    memset(o, 0, sizeof *o);
    if (endmus_init(&o->mus, file, len, variant) != 0)
        return -1;
    paula_init(&o->paula, rate, o->mus.mem, o->mus.base, o->mus.len);
    endmus_set_paula(&o->mus, paula_write, &o->paula);
    endmus_start(&o->mus, 0);
    o->acc = 0;                                   /* first tick one period after init */
    return 0;
}

SND_API void endsnd_free(EndSound *o)
{
    endmus_free(&o->mus);
    o->paula.mem = NULL;
    o->paula.len = 0;
}

SND_API void endsnd_render(EndSound *o, int16_t *out, int n)
{
    uint64_t thr = (uint64_t)SND_TICK_DEN * (uint64_t)o->paula.rate;
    while (n > 0) {
        uint64_t need;
        int m;
        if (o->acc >= thr) {
            o->acc -= thr;
            if (!o->hold)
                endmus_tick(&o->mus);
        }
        need = (thr - o->acc + SND_TICK_NUM - 1) / SND_TICK_NUM;   /* frames to the next tick */
        m = need < (uint64_t)n ? (int)need : n;
        if (m < 1)
            m = 1;
        paula_render(&o->paula, out, m);
        o->acc += (uint64_t)m * SND_TICK_NUM;
        out += 2 * m;
        n -= m;
    }
}

SND_API EndMusic *endmus_new(void)        { return (EndMusic *)calloc(1, sizeof(EndMusic)); }
SND_API void endmus_delete(EndMusic *e)   { if (e) { endmus_free(e); free(e); } }
SND_API int  endmus_nwrites(EndMusic *e)  { return e->nwrites; }
SND_API int  endmus_overflow(EndMusic *e) { return e->overflow; }
SND_API SeqWrite *endmus_writes(EndMusic *e) { return e->writes; }
SND_API uint32_t endmus_outside(EndMusic *e) { return e->outside; }
SND_API void endmus_set_regs(EndMusic *e, uint32_t a0, uint32_t a3, int keep)
{
    e->a0 = a0;
    e->a3 = a3;
    e->keep_regs = keep;
}
SND_API EndSound *endsnd_new(void)       { return (EndSound *)calloc(1, sizeof(EndSound)); }
SND_API void endsnd_delete(EndSound *o)  { if (o) { endsnd_free(o); free(o); } }

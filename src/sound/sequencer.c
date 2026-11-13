/*
 * Supremacy port - sound sequencer, translated from the original's sound
 * player. See sequencer.h. Routine names are the original addresses;
 * channel fields are offsets into the $80-byte channel block. It was
 * checked tick for tick against a model of the original player.
 *
 * Every store wraps to the width of its field (sb/sw/sl mask), and every
 * temporary is a plain int or uint32_t so sums and products behave like the
 * 68000's 32-bit registers.
 */
#include "sequencer.h"

#include <stdlib.h>
#include <string.h>

#define BLOCKS_END    0x694E2u
#define PERIODS       0x694E2u     /* period table, words */
#define DMABITS       0x69558u     /* 4 longs: 1, 2, 4, 8 */
#define WAVES         0x69580u     /* long pointers, 32-byte waves (byte offset) */
#define WAVES_HI      0x695C0u     /* long pointers, 8-byte waves for high notes */
#define CHANNO        0x69600u     /* long: channel number * 4 */
#define TEMPO         0x69604u     /* word */
#define MASK          0x69606u     /* byte: header byte 0 */
#define RATES         0x69608u     /* envelope rates, 16 words */
#define TRANSPOSE     0x69648u     /* word */
#define PULSE8        0x69650u
#define PULSE32       0x69658u
#define BUFFERS       0x69678u     /* 4 long pointers: build buffers */
#define SILENCE       0x6A000u
#define INSTR_PTR     0x6A800u
#define SAMPLE_PTR    0x6A804u
#define ARP_TABLES    0x6A80Cu
#define TIMBRE_TABLES 0x6A88Cu
#define SEQ_NOTE_TABLES 0x6A8C8u
#define WAVEMOD_TABLES  0x6A904u
#define SEQ_WAVE_TABLES 0x6A940u
#define DROP_LEVEL    0x6AA3Du
#define GLOBAL_BEND   0x6AA3Eu
#define SOUNDS        0x6AA40u
#define ENABLE        0x68012u

/* The original loops forever on malformed data in a few places; the port
 * gives up after this many steps instead (never reached with the game data). */
#define LOOP_GUARD    4096

/* ------------------------------------------------------------ memory */
static uint8_t r8(Sequencer *s, uint32_t a)
{
    uint32_t o = (a & 0x7FFFFu) - s->base;
    if (o < s->len) return s->mem[o];
    a &= 0x7FFFFu;
    return (s->ext && a < s->ext_len) ? s->ext[a] : 0;
}
static uint16_t r16(Sequencer *s, uint32_t a) { return (uint16_t)((r8(s, a) << 8) | r8(s, a + 1)); }
static uint32_t r32(Sequencer *s, uint32_t a) { return ((uint32_t)r16(s, a) << 16) | r16(s, a + 2); }
static void w8(Sequencer *s, uint32_t a, uint32_t v)
{
    uint32_t o = (a & 0x7FFFFu) - s->base;
    if (o < s->len)
        s->mem[o] = (uint8_t)v;
}
static void w16(Sequencer *s, uint32_t a, uint32_t v) { w8(s, a, v >> 8); w8(s, a + 1, v); }
static void w32(Sequencer *s, uint32_t a, uint32_t v) { w16(s, a, v >> 16); w16(s, a + 2, v); }

/* channel fields */
static uint32_t b(Sequencer *s, uint32_t o)  { return r8(s, s->a5 + o); }
static uint32_t w(Sequencer *s, uint32_t o)  { return r16(s, s->a5 + o); }
static uint32_t lg(Sequencer *s, uint32_t o) { return r32(s, s->a5 + o); }
static void sb(Sequencer *s, uint32_t o, uint32_t v) { w8(s, s->a5 + o, v); }
static void sw(Sequencer *s, uint32_t o, uint32_t v) { w16(s, s->a5 + o, v); }
static void sl(Sequencer *s, uint32_t o, uint32_t v) { w32(s, s->a5 + o, v); }

/* Paula */
static void hw(Sequencer *s, uint32_t reg, uint32_t v)
{
    uint16_t r = (uint16_t)reg, x = (uint16_t)v;
    if (s->nwrites < SEQ_MAX_WRITES) {
        s->writes[s->nwrites].reg = r;
        s->writes[s->nwrites].val = x;
        s->nwrites++;
    } else {
        s->overflow++;
    }
    if (s->paula)
        s->paula(s->paula_user, r, x);
}
static void hw32(Sequencer *s, uint32_t reg, uint32_t v)
{
    hw(s, reg, v >> 16);
    hw(s, reg + 2, v & 0xFFFFu);
}

static uint32_t div1000(uint32_t v) { return (v < 1000 ? 1000 : v) / 1000; }   /* max(v,1000)//1000 */

/* ------------------------------------------------------------ forward */
static void finish(Sequencer *s);
static void sustain(Sequencer *s);

/* ------------------------------------------------------------ note start */
static void s68550(Sequencer *s)                  /* envelope set-up */
{
    uint32_t a3 = s->a3, hi, lo, d0;
    if (b(s, 0x4D) & 2) {
        sb(s, 0x27, 3);
        sw(s, 0x66, w(s, 0x68));
        return;
    }
    sb(s, 0x27, 0);
    hi = r8(s, a3 + 2) & 0xF0u;
    d0 = hi == 0 ? 0x7D01u : r16(s, RATES + (hi >> 3));
    sw(s, 0x62, d0);
    sw(s, 0x60, 0);
    lo = r8(s, a3 + 2) & 0x0Fu;
    d0 = lo == 0 ? 0x7D01u : r16(s, RATES + lo * 2);
    sw(s, 0x68, (r8(s, a3 + 3) & 0xF0u) * 0x10Au);
    sw(s, 0x6A, d0);
    sw(s, 0x6C, 0xFA00);
    lo = r8(s, a3 + 3) & 0x0Fu;
    d0 = lo == 0 ? 0x7D01u : r16(s, RATES + lo * 2);
    sw(s, 0x64, d0);
    sw(s, 0x66, 0xFA00);
}

static void s68a4e(Sequencer *s)                  /* arpeggio select */
{
    uint32_t p = lg(s, 0x0C), op = r8(s, p);
    if (op >= 0x5F && op <= 0x7F) {
        uint32_t d0, t, x;
        sl(s, 0x0C, p + 1);
        d0 = (((op - 0x60) & 0xFFu) << 2) & 0xFFFFu;
        t = r32(s, ARP_TABLES + d0);
        sl(s, 0x18, t);
        sb(s, 0x30, 1);
        x = r8(s, t);
        sb(s, 0x31, x >> 4);
        sb(s, 0x3C, x & 0x0F);
        sb(s, 0x33, 1);
        sb(s, 0x32, r8(s, t + 1));
    } else if (b(s, 0x33) == 0) {
        sb(s, 0x32, 0);
        sb(s, 0x30, 0);
    } else {
        sb(s, 0x30, 1);
        sb(s, 0x32, 0);
    }
}

static void s68c46(Sequencer *s)                  /* $FD slide set-up */
{
    uint32_t p = lg(s, 0x0C);
    if (r8(s, p) == 0xFD) {
        uint32_t x = r8(s, p + 1), d0;
        sb(s, 0x3B, (x & 0xF0) >> 3);
        sb(s, 0x38, ((x & 0x0F) - 1) ^ 0x0F);     /* wraps: nibble 0 gives $F0 */
        d0 = (((r8(s, p + 2) + b(s, 0x1D)) & 0xFF) + r16(s, TRANSPOSE)) & 0xFF;
        sb(s, 0x7A, d0);
        sb(s, 0x39, d0);
        sb(s, 0x1F, d0);
        d0 = (((r8(s, p + 3) + b(s, 0x1D)) & 0xFF) + r16(s, TRANSPOSE)) & 0xFF;
        sb(s, 0x3A, d0);
        sb(s, 0x7B, 0x80);
    } else {
        sb(s, 0x38, 0);
        sb(s, 0x7B, 0x80);
        sb(s, 0x56, 0);
    }
}

static void s687b8(Sequencer *s)                  /* release point */
{
    int32_t d1 = (int32_t)((r8(s, s->a3 + 4) & 0xF0) >> 4) + 1;
    int32_t lo = (int16_t)(uint16_t)((w(s, 0x70) << 2) & 0xFFFF);   /* muls: signed word */
    uint32_t prod = (uint32_t)(lo * d1);
    sw(s, 0x72, prod >> 6);                       /* lsr.l */
}

static void s68b4e(Sequencer *s)                  /* fall-off set-up */
{
    if (r8(s, s->a3 + 7) & 2) {
        sb(s, 0x34, 1);
        sb(s, 0x35, 0);
        sb(s, 0x36, 0);
    } else {
        sb(s, 0x34, 0);
    }
}

static void s68d8a(Sequencer *s)                  /* timbre sweep set-up */
{
    uint32_t a3 = s->a3, d1 = r8(s, a3 + 6) & 0x0F, t;
    if (d1 == 0) {
        sb(s, 0x3D, 0);
        sb(s, 0x4E, 0);
        sl(s, 0x08, 0);
        return;
    }
    if (b(s, 0x4E) & 2)
        return;
    t = r32(s, TIMBRE_TABLES + (((d1 - 1) & 0xFF) << 2));
    sl(s, 0x08, t);
    sb(s, 0x3F, 2);
    sb(s, 0x21, r8(s, a3));
    sb(s, 0x3E, 0);
    sb(s, 0x3D, r8(s, t + 3));
    if (b(s, 0x4E) != 0)
        sb(s, 0x4E, b(s, 0x4E) | 2);
}

static void s689fa(Sequencer *s)                  /* vibrato depth change */
{
    uint32_t a3 = s->a3, x = r8(s, a3 + 9), d1, d2;
    if (x == 0) {
        sb(s, 0x25, 0);
        return;
    }
    d1 = ((x & 0xF0) >> 4) + 1;
    d2 = w(s, 0x70) * d1;
    sw(s, 0x74, d2 >> 4);
    d1 = (((x & 0x0E) ^ 0x0E) + 1) & 0xFF;
    sb(s, 0x25, d1);
    sb(s, 0x29, d1);
    sb(s, 0x26, r8(s, a3 + 1) & 4);
}

static void s6881c(Sequencer *s)                  /* vibrato set-up */
{
    uint32_t a3 = s->a3, x = r8(s, a3 + 5);
    if (x != 0) {
        sb(s, 0x56, 0);
        sb(s, 0x2A, ((x & 0xF0) >> 4) ^ 0x0F);
        sb(s, 0x2B, x & 0x0F);
        sb(s, 0x2C, 0x80);
        sb(s, 0x2E, 0);
        sb(s, 0x2F, (r8(s, a3 + 6) & 0xF0) >> 2);   /* $689CE */
        s689fa(s);
    } else {
        sb(s, 0x2C, 0x80);
        sb(s, 0x2A, 0x0F);
        sb(s, 0x56, 0);
    }
}

static void s68f3c(Sequencer *s)                  /* note/wave sequence set-up */
{
    uint32_t d1 = r8(s, s->a3 + 7) & 0xF0;
    if (d1 == 0) {
        sl(s, 0x40, 0);
        return;
    }
    d1 = ((d1 - 0x10) & 0xFF) >> 2;
    sl(s, 0x44, r32(s, SEQ_NOTE_TABLES + d1));
    sl(s, 0x40, r32(s, SEQ_WAVE_TABLES + d1));
    sb(s, 0x49, 1);
    sb(s, 0x4A, 1);
}

static void s69044(Sequencer *s)                  /* wave-pointer sweep set-up */
{
    uint32_t d0 = r8(s, s->a3 + 4) & 0x0F, t;
    if (d0 == 0) {
        sl(s, 0x7C, 0);
        return;
    }
    d0 = ((d0 - 1) & 0xFF) * 4;
    s->a0 = WAVEMOD_TABLES;
    t = r32(s, WAVEMOD_TABLES + d0);
    sl(s, 0x7C, t);
    sb(s, 0x22, 4);
    sb(s, 0x77, r8(s, t + 1));
    sb(s, 0x23, r8(s, t + 2));
    sb(s, 0x1C, r8(s, t + 3));
}

/* ------------------------------------------------------------ per-tick */
static void release(Sequencer *s)                 /* $6874E */
{
    uint32_t d0;
    if (b(s, 0x4C) != 1 && w(s, 0x72) < w(s, 0x70))
        return;
    d0 = w(s, 0x66) + 10000u - w(s, 0x64);       /* unsigned 32-bit compare */
    if (d0 < 10000u) {
        sb(s, 0x28, 0);
        return;
    }
    d0 = (d0 - 10000u) & 0xFFFFu;
    sw(s, 0x66, d0);
    sb(s, 0x28, div1000(d0));
}

static void t6862c(Sequencer *s)                  /* volume envelope */
{
    uint32_t st = b(s, 0x4D), d0, d2;
    if (st != 0) {
        if (st & 4) {
            sb(s, 0x4D, 0);
            release(s);
            return;
        }
        sb(s, 0x4D, st | 2);
    }
    if (b(s, 0x27) == 0) {                        /* attack */
        d0 = w(s, 0x60) + w(s, 0x62);
        if (d0 > 0xFA00u) {
            sb(s, 0x28, 0x40);
            sb(s, 0x27, 1);
            sw(s, 0x60, 0xFA00);
        } else {
            sw(s, 0x60, d0);
            sb(s, 0x28, div1000(d0));
            return;
        }
    }
    if (b(s, 0x27) == 1) {                        /* decay */
        d0 = w(s, 0x6C) + 100000u - w(s, 0x6A);
        d2 = w(s, 0x68) + 100000u;
        if (d0 >= d2) {
            d0 = (d0 - 100000u) & 0xFFFFu;
            sw(s, 0x6C, d0);
            sb(s, 0x28, div1000(d0));
            return;
        }
        sb(s, 0x27, 3);
        d0 = w(s, 0x68);
        sw(s, 0x66, d0);
        sb(s, 0x28, d0 / 1000);
    }
    if (b(s, 0x27) == 3) {                        /* sustain / release */
        if (b(s, 0x4D) != 0) {
            sb(s, 0x28, div1000(w(s, 0x66)));
            return;
        }
        release(s);
    }
}

static void t68cbe(Sequencer *s)                  /* slide ($FD) */
{
    uint32_t v;
    if (b(s, 0x38) == 0)
        return;
    if (b(s, 0x3B) != 0) {
        sb(s, 0x3B, b(s, 0x3B) - 1);
        return;
    }
    if (w(s, 0x70) == 0) {
        sb(s, 0x38, 0);
        sl(s, 0x0C, lg(s, 0x0C) + 4);
        return;
    }
    if (b(s, 0x56) == 1)
        return;
    if (b(s, 0x1F) == b(s, 0x3A)) {
        sb(s, 0x56, 1);
        return;
    }
    if (b(s, 0x39) < b(s, 0x3A)) {                /* up */
        v = (b(s, 0x7B) + b(s, 0x38)) & 0xFF;
        sb(s, 0x7B, v);
        if (v > 0x90) {
            sb(s, 0x7B, v - 0x90 + 0x80);
            sb(s, 0x1F, b(s, 0x1F) + 1);
        }
    } else {                                      /* down */
        v = (b(s, 0x7B) - b(s, 0x38)) & 0xFF;
        sb(s, 0x7B, v);
        if (v < 0x70) {
            sb(s, 0x1F, b(s, 0x1F) - 1);
            sb(s, 0x7B, 0x80 - ((0x70 - v) & 0xFF));
        }
    }
}

static void t68b6e(Sequencer *s)                  /* fall-off */
{
    if (b(s, 0x34) == 0)
        return;
    if (b(s, 0x35) == 0) {
        sb(s, 0x36, 0);
        sb(s, 0x35, b(s, 0x35) ^ 1);
        return;
    }
    if (r8(s, DROP_LEVEL) < w(s, 0x70))
        return;
    sb(s, 0x34, b(s, 0x34) + 1);
    sb(s, 0x35, b(s, 0x35) ^ 1);
    sb(s, 0x36, b(s, 0x34));
}

static void t68ad0(Sequencer *s)                  /* arpeggio step */
{
    int guard;
    if (b(s, 0x30) == 0)
        return;
    if (b(s, 0x31) != 0) {
        sb(s, 0x31, b(s, 0x31) - 1);
        return;
    }
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        uint32_t t = lg(s, 0x18), v;
        sb(s, 0x31, r8(s, t) >> 4);
        sb(s, 0x30, b(s, 0x30) + 1);
        v = r8(s, t + b(s, 0x30));
        if (v == 0xFF) {
            sb(s, 0x30, b(s, 0x3C));
            continue;
        }
        if (v == 0xFE) {
            sb(s, 0x30, b(s, 0x30) - 2);
            continue;
        }
        sb(s, 0x32, v);
        if (r8(s, s->a3 + 8) != 0) {
            sb(s, 0x54, 0);
            sb(s, 0x57, 1);
        }
        return;
    }
}

static void t68dec(Sequencer *s)                  /* timbre sweep step */
{
    int guard;
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        if (lg(s, 0x08) == 0)
            return;
        if (!(b(s, 0x4E) & 4))
            sb(s, 0x4E, 0);
        if (b(s, 0x3D) == 0) {                    /* $68E64: next segment */
            uint32_t t = lg(s, 0x08), d0 = b(s, 0x3F);
            if (r8(s, t + d0 + 2) == 0xFF) {
                sb(s, 0x3D, r8(s, t + d0));
            } else {
                sb(s, 0x3F, d0 + 2);
                sb(s, 0x3D, r8(s, t + d0 + 2));
            }
            continue;
        }
        for (; guard < LOOP_GUARD; guard++) {      /* $68E10 */
            uint32_t t = lg(s, 0x08), step;
            int d0;
            sb(s, 0x3D, b(s, 0x3D) - 1);
            step = r8(s, t + b(s, 0x3F) + 1);
            if (b(s, 0x3E) == 0) {                /* rising */
                int hi = r8(s, t + 1);
                d0 = (int)step + (int)b(s, 0x21);
                if (d0 <= hi) {
                    sb(s, 0x21, (uint32_t)d0);
                    return;
                }
                sb(s, 0x21, (uint32_t)(hi - (d0 - hi)));   /* reflect off the top */
                sb(s, 0x3E, b(s, 0x3E) ^ 1);
            } else {                              /* falling ($68E9E) */
                int lo = r8(s, t);
                d0 = (int)b(s, 0x21) + 0x1000 - (int)step;
                if (d0 >= lo + 0x1000) {
                    sb(s, 0x21, (uint32_t)(d0 - 0x1000));
                    return;
                }
                sb(s, 0x3E, b(s, 0x3E) ^ 1);
                sb(s, 0x21, (uint32_t)(lo + (lo + 0x2000 - d0)));   /* reflect off the bottom */
            }
            /* back to $68E10 (decrements $3D again) */
        }
    }
}

static uint32_t seq_step(Sequencer *s, uint32_t a2, uint32_t idx_field)
{
    uint32_t v = 0;
    int guard;
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        v = r8(s, a2 + b(s, idx_field));
        if (v == 0xFF)
            sb(s, idx_field, r8(s, a2) + 1);
        else if (v == 0xFE)
            sb(s, idx_field, b(s, idx_field) - 1);
        else
            break;
    }
    return v;
}

static void t68f84(Sequencer *s)                  /* note/wave sequence step */
{
    uint32_t v;
    if (lg(s, 0x40) == 0)
        return;
    v = seq_step(s, lg(s, 0x40), 0x49);
    sb(s, 0x4B, v);
    sb(s, 0x49, b(s, 0x49) + 1);
    v = seq_step(s, lg(s, 0x44), 0x4A);
    sb(s, 0x55, v);
    sb(s, 0x4A, b(s, 0x4A) + 1);
    sb(s, 0x4C, (b(s, 0x4B) & 1) ^ 1);
    sb(s, 0x54, 0);
}

static void t6908a(Sequencer *s)                  /* wave-pointer sweep step */
{
    uint32_t d0;
    if (lg(s, 0x7C) == 0)
        return;
    if (b(s, 0x23) != 0) {
        sb(s, 0x23, b(s, 0x23) - 1);
    } else {
        uint32_t t = lg(s, 0x7C);
        s->a0 = t;
        d0 = b(s, 0x22);
        if (r8(s, t + d0) == 0xFF)
            return;
        sb(s, 0x22, d0 + 2);
        sb(s, 0x23, r8(s, t + d0));
        sb(s, 0x1C, r8(s, t + d0 + 1));
    }
    d0 = b(s, 0x1C);
    if (d0 <= 0x7F)
        sb(s, 0x77, b(s, 0x77) + d0);
    else
        sb(s, 0x77, b(s, 0x77) - (d0 & 0x7F));
}

static void t6897a(Sequencer *s)                  /* vibrato depth step */
{
    if (w(s, 0x74) < w(s, 0x70))
        return;
    if (b(s, 0x29) != 1) {
        sb(s, 0x29, b(s, 0x29) - 1);
        return;
    }
    sb(s, 0x29, b(s, 0x25));
    if (b(s, 0x26) != 0) {
        if (b(s, 0x2A) != 0)
            sb(s, 0x2A, b(s, 0x2A) - 1);
    } else if (b(s, 0x2A) != 0x0E) {
        sb(s, 0x2A, b(s, 0x2A) + 1);
    }
}

static void t68874(Sequencer *s)                  /* vibrato step */
{
    int guard;
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        if (b(s, 0x38) != 0 && b(s, 0x56) != 1)
            return;
        if (b(s, 0x2A) == 0x0F)
            return;
        if (b(s, 0x2E) == 0) {
            if (!(((b(s, 0x2A) + 0x80) & 0xFF) < b(s, 0x2C))) {
                sb(s, 0x2C, b(s, 0x2C) + b(s, 0x2B));
                return;
            }
        } else {
            if (!(((0x80 - b(s, 0x2A)) & 0xFF) > b(s, 0x2C))) {
                sb(s, 0x2C, b(s, 0x2C) - b(s, 0x2B));
                return;
            }
        }
        sb(s, 0x2E, b(s, 0x2E) ^ 1);
        t6897a(s);
    }
}

static void t689e0(Sequencer *s)                  /* vibrato delay */
{
    if (b(s, 0x2F) != 0)
        sb(s, 0x2F, b(s, 0x2F) - 1);
    else
        t68874(s);
}

static void t68bb2(Sequencer *s)                  /* note index */
{
    uint32_t d1 = r8(s, s->a3 + 7) & 4, d0 = b(s, 0x1F);
    if (lg(s, 0x40) != 0) {
        uint32_t d2 = b(s, 0x55);
        d0 = (d2 & 0x80) ? ((d0 + (d2 & 0x7F)) & 0xFF) : d2;
    }
    d0 = (d0 + b(s, 0x32)) & 0xFF;
    d0 = d1 ? ((d0 + b(s, 0x36)) & 0xFF) : ((d0 - b(s, 0x36)) & 0xFF);
    if (b(s, 0x36) != 0)
        d0 = (d0 + 1) & 0xFF;
    sb(s, 0x37, d0 <= 0x54 ? d0 : 2);
}

static void t688e8(Sequencer *s)                  /* fine pitch offset */
{
    uint32_t d0 = b(s, 0x37) << 1, d1, d2, prod, q;
    if (d0 >= 0x70)
        d0 -= 0x30;
    d1 = (uint32_t)r16(s, PERIODS + d0) - r16(s, PERIODS + d0 + 6);
    if (b(s, 0x38) != 0 && b(s, 0x56) != 1)
        d2 = b(s, 0x7B);
    else
        d2 = b(s, 0x2C);
    d2 = d2 - 0x70;
    if (r8(s, s->a3 + 7) & 1) {
        if (b(s, 0x1E) != 0)
            d2 += b(s, 0x24) >> 4;
        else if (r8(s, GLOBAL_BEND) != 0)
            d2 += (uint32_t)r8(s, GLOBAL_BEND) >> 4;
    }
    prod = (d1 & 0xFFFF) * (d2 & 0xFFFF);         /* mulu.w */
    q = prod / 0x30;
    sw(s, 0x78, q > 0xFFFF ? prod : q);           /* divu overflow leaves d1 */
}

/* ------------------------------------------------------------ output */
static void wr(Sequencer *s, uint32_t d1)         /* $68522 */
{
    if (b(s, 0x57) == 2)
        return;
    hw(s, s->hwb + 6u, (uint32_t)r16(s, PERIODS + d1) - w(s, 0x78));
    hw(s, s->hwb + 4u, w(s, 0x50));
    hw32(s, s->hwb + 0u, s->a0);
}

static void rest(Sequencer *s, uint32_t d1)       /* $6850C */
{
    if (b(s, 0x7A) == 0) {
        s->a0 = SILENCE;
        sw(s, 0x50, 0x10);
    }
    wr(s, d1);
}

static void s6836a(Sequencer *s)                  /* set AUDxPER/LEN/LC */
{
    uint32_t d0 = s->a5 - SEQ_BLOCKS, d1;
    if (d0)
        d0 = (d0 & 0xFFFF) >> 5;
    w32(s, CHANNO, d0);
    d1 = b(s, 0x37) << 1;
    if (b(s, 0x54) == 0) {
        hw(s, 0x096, r32(s, DMABITS + d0) & 0xFFFF);
        s->a0 = lg(s, 0x10);
        wr(s, d1);
        return;
    }
    if (lg(s, 0x10) == 0x6A200u) {
        s->a0 = 0x6A200u;
        sw(s, 0x50, 0x80);
        rest(s, d1);
        return;
    }
    if (lg(s, 0x7C) != 0) {
        sw(s, 0x50, 0x40);
        s->a0 = INSTR_PTR + ((b(s, 0x77) & 0xF0) << 3);
        rest(s, d1);
        return;
    }
    if (d1 >= 0x70) {                             /* high notes: 8-byte waves */
        uint32_t a2;
        d1 -= 0x30;
        a2 = r32(s, WAVES_HI + b(s, 0x76));
        if (a2 == 0x6A180u) {
            uint32_t t = b(s, 0x21), k = t >> 5, src = a2 + (k << 3);
            uint32_t a0 = r32(s, BUFFERS + r32(s, CHANNO));
            int i;
            for (i = 0; i < 8; i++)
                w8(s, a0 + (uint32_t)i, r8(s, src + (uint32_t)i));
            w8(s, a0 + k, r8(s, PULSE32 + (t & 0x1F)));
            s->a0 = a0;
            sw(s, 0x50, 4);
            rest(s, d1);
            return;
        }
        sw(s, 0x50, 4);
        wr(s, d1);                                /* a0 is stale here */
        return;
    }
    if (lg(s, 0x10) == 0x6A0F0u) {                /* pulse wave built per tick */
        uint32_t t = b(s, 0x21), k = t >> 3, src = 0x6A300u + (k << 5);
        uint32_t a0 = r32(s, BUFFERS + r32(s, CHANNO));
        int i;
        for (i = 0; i < 32; i++)
            w8(s, a0 + (uint32_t)i, r8(s, src + (uint32_t)i));
        w8(s, a0 + k, r8(s, PULSE8 + (t & 7)));
        s->a0 = a0;
        sw(s, 0x50, 0x10);
        rest(s, d1);
        return;
    }
    s->a0 = lg(s, 0x10);
    if (r8(s, s->a3 + 8) == 0)
        sw(s, 0x50, 0x10);
    rest(s, d1);
}

/* ------------------------------------------------------------ channel step */
static void finish(Sequencer *s)                  /* $682B6 */
{
    s6836a(s);
    hw(s, s->hwb + 8u, b(s, 0x28));
}

static void pitch_step(Sequencer *s)              /* $682A4 */
{
    t689e0(s);
    t68bb2(s);
    t688e8(s);
    finish(s);
}

static void sustain(Sequencer *s)                 /* $68268 */
{
    sw(s, 0x70, w(s, 0x70) - 1);
    t6862c(s);
    t68cbe(s);
    t68b6e(s);
    t68ad0(s);
    t68dec(s);
    t68f84(s);
    t6908a(s);
    pitch_step(s);
}

static void note(Sequencer *s)                    /* $681F4 */
{
    uint32_t a3 = s->a3;
    if (b(s, 0x38) == 0) {
        uint32_t p = lg(s, 0x0C), d0 = r8(s, p);
        sb(s, 0x7A, d0);
        d0 = (d0 + b(s, 0x1D)) & 0xFF;
        d0 = (d0 + r16(s, TRANSPOSE)) & 0xFFFF;
        sb(s, 0x1F, d0);
        if (r8(s, a3 + 8) != 0) {
            sb(s, 0x54, 0);
            sb(s, 0x57, b(s, 0x2D));
        }
        sl(s, 0x0C, p + 1);
    }
    s687b8(s);
    if (b(s, 0x4D) & 2) {
        sustain(s);
        return;
    }
    s68b4e(s);
    s68d8a(s);
    s6881c(s);
    s68f3c(s);
    s69044(s);
    sw(s, 0x70, w(s, 0x70) - 1);                  /* $6829E */
    pitch_step(s);
}

static void parse(Sequencer *s)                   /* $68096 */
{
    int guard;
    for (guard = 0; guard < LOOP_GUARD; guard++) {
        uint32_t p = lg(s, 0x0C), op = r8(s, p);
        if (op == 0xFF) {
            sb(s, 0x20, 0);
            sb(s, 0x04, 0);
            hw(s, s->hwb + 8u, 0);
            sb(s, 0x28, 0);
            finish(s);
            return;
        }
        if (op == 0xF9) {
            sl(s, 0x0C, lg(s, 0x5A));
            continue;
        }
        if (op >= 0xC1 && op <= 0xF8) {           /* instrument select */
            uint32_t d0 = ((op - 0xC1) & 0xFF) * 10;
            uint32_t a2 = r32(s, INSTR_PTR), a3 = a2 + d0;
            sl(s, 0x14, d0);
            s->a3 = a3;
            sb(s, 0x21, r8(s, a3));
            if (r8(s, a3 + 8) != 0) {             /* sample */
                uint32_t d1 = (((r8(s, a3 + 8) - 1u) & 0xFF) << 3) & 0xFFFF;
                uint32_t a1 = r32(s, SAMPLE_PTR), d2;
                sl(s, 0x10, r32(s, a1 + d1));
                sw(s, 0x50, (r16(s, a1 + d1 + 6) & 0xFFFEu) >> 1);
                d2 = r8(s, a1 + d1 + 7) & 1;
                sb(s, 0x57, d2);
                sb(s, 0x2D, d2);
                sb(s, 0x54, 0);
            } else {                              /* synth waveform ($68152) */
                uint32_t d2 = (uint32_t)r8(s, a2 + d0 + 1) >> 2;
                sb(s, 0x57, 0);
                sb(s, 0x76, d2);
                sl(s, 0x10, r32(s, WAVES + d2));
                sw(s, 0x50, 0x10);
                sb(s, 0x54, 1);
            }
            sl(s, 0x0C, lg(s, 0x0C) + 1);
        }
        p = lg(s, 0x0C);                          /* $68180: durations */
        if (r8(s, p) >= 0x7F && r8(s, p) <= 0xC0) {
            sw(s, 0x6E, 0);
            for (;;) {
                uint32_t d0 = (r8(s, p) - 0x80u) & 0xFFFF;
                sw(s, 0x6E, w(s, 0x6E) + d0 * r16(s, TEMPO));
                sl(s, 0x0C, lg(s, 0x0C) + 1);
                p++;
                if (!(r8(s, p) >= 0x7F && r8(s, p) <= 0xC0))
                    break;
            }
        }
        sw(s, 0x70, w(s, 0x6E));
        s68550(s);
        s68a4e(s);
        s68c46(s);
        p = lg(s, 0x0C);                          /* $687DA / $68EFA */
        op = r8(s, p);
        if (op == 0xFC) {
            sl(s, 0x0C, p + 1);
            sb(s, 0x4D, 1);
            continue;
        }
        if (op == 0xFE) {
            sb(s, 0x4D, b(s, 0x4D) | 4);
            sl(s, 0x0C, p + 1);
            continue;
        }
        if (op == 0xFA) {
            sl(s, 0x0C, p + 1);
            sb(s, 0x4E, 1);
            continue;
        }
        if (op == 0xFB) {
            sl(s, 0x0C, p + 1);
            sb(s, 0x4E, b(s, 0x4E) | 4);
            continue;
        }
        note(s);
        return;
    }
}

static void channel(Sequencer *s)                 /* $68078 */
{
    if (b(s, 0x04) == 0) {
        finish(s);
        return;
    }
    s->a3 = lg(s, 0x14) + r32(s, INSTR_PTR);
    if (w(s, 0x70) != 0)
        sustain(s);
    else
        parse(s);
}

static void end_of_tick(Sequencer *s)             /* $682C8 (the line wait is dropped) */
{
    uint32_t n;
    for (n = 0; n < 4; n++) {
        uint32_t hwb = 0xA0 + 16 * n;
        s->a5 = SEQ_BLOCKS + 0x80 * n;
        if (b(s, 0x04) == 0)
            continue;
        if (b(s, 0x54) == 0) {
            hw32(s, hwb, lg(s, 0x10));
            hw(s, hwb + 4, w(s, 0x50));
            hw(s, 0x096, r32(s, DMABITS + 4 * n) + 0x8000u);
            sb(s, 0x54, 1);
        } else if (b(s, 0x57) == 1) {
            hw32(s, hwb, SILENCE);
            hw(s, hwb + 4, 8);
            sb(s, 0x57, 2);
        }
    }
}

/* ------------------------------------------------------------ entry points */
static void stop(Sequencer *s)                    /* $690F2 */
{
    uint32_t a, n;
    for (a = SEQ_BLOCKS; a < BLOCKS_END; a++)
        w8(s, a, 0);
    for (n = 0; n < 4; n++) {
        w8(s, SEQ_BLOCKS + 0x80 * n + 0x54, 1);
        w8(s, SEQ_BLOCKS + 0x80 * n + 0x04, 0);
    }
    hw(s, 0x096, 0x000F);
    for (n = 0; n < 4; n++)
        hw32(s, 0xA0 + 16 * n, SILENCE);
    for (n = 0; n < 4; n++)
        hw(s, 0xA4 + 16 * n, 0x10);
}

static void start_channel(Sequencer *s, int32_t off, uint32_t p)
{
    uint32_t blk = SEQ_BLOCKS + (uint32_t)off;
    w32(s, blk + 0x0C, p);
    w32(s, blk + 0x5A, p);
    w8(s, blk + 0x04, 1);
    w8(s, blk + 0x33, 0);
    w8(s, blk + 0x30, 0);
}

static void play(Sequencer *s, uint32_t d7)       /* $69190 */
{
    int32_t d0 = 0;
    uint32_t a1, a2, mask, d1;
    int n, guard;
    for (n = 0; n < 4; n++) {
        if (r8(s, SEQ_BLOCKS + 0x80u * (uint32_t)n + 4) == 0) {
            d0 = 0x80 * n;
            break;
        }
    }
    a2 = r32(s, SOUNDS + ((d7 << 2) & 0xFFFF));
    mask = r8(s, a2);
    w8(s, MASK, mask);
    w8(s, TEMPO + 1, r8(s, a2 + 1));
    w8(s, TRANSPOSE + 1, r8(s, a2 + 2));
    a2 += 3;
    start_channel(s, d0, a2);
    hw(s, 0x096, (mask << 1) + 1 + 0x8000u);
    a1 = a2;
    d1 = 0;
    for (guard = 0; guard < 256 && ((r8(s, MASK) >> (d1 & 7)) & 1); guard++) {
        int k;
        a1++;
        for (k = 0; k < 0x80000 && r8(s, a1) != 0xFF; k++)
            a1++;
        a1++;
        if (d0 == 0x180)
            d0 = -0x80;
        d0 += 0x80;
        start_channel(s, d0, a1);
        d1++;
    }
}

SND_API void seq_stop(Sequencer *s)
{
    s->nwrites = 0;
    stop(s);
}

SND_API void seq_play(Sequencer *s, int sound)
{
    s->nwrites = 0;
    stop(s);
    /* The game has 50 sounds. A larger number only comes from the tension
     * table overrun under the WHDLoad clamp (sound 71, whose table entry
     * points into GAM1): it plays from there as in the original, through
     * the read-only `ext` image (writes outside the copy are dropped). */
    play(s, (uint32_t)sound);
}

SND_API void seq_tick(Sequencer *s)
{
    uint32_t n;
    s->nwrites = 0;
    if (!s->keep_regs)
        s->a0 = s->a3 = 0;
    if (r8(s, ENABLE) == 0)
        return;
    for (n = 0; n < 4; n++) {
        s->a5 = SEQ_BLOCKS + 0x80 * n;
        s->hwb = (uint16_t)(0xA0 + 16 * n);
        channel(s);
    }
    end_of_tick(s);
}

/* ------------------------------------------------------------ set-up */
SND_API int seq_init(Sequencer *s, const uint8_t *image, size_t len, uint32_t base)
{
    memset(s, 0, sizeof *s);
    if (!image || base > 0x68000u || (uint64_t)base + len < 0x6AB00u || len > 0x80000u)
        return -1;
    s->mem = (uint8_t *)malloc(len);
    if (!s->mem)
        return -1;
    memcpy(s->mem, image, len);
    s->base = base;
    s->len = (uint32_t)len;
    s->a5 = SEQ_BLOCKS;
    s->hwb = 0xA0;
    return 0;
}

SND_API void seq_free(Sequencer *s)
{
    free(s->mem);
    s->mem = NULL;
    s->len = 0;
}

SND_API void seq_set_paula(Sequencer *s, seq_paula_fn fn, void *user)
{
    s->paula = fn;
    s->paula_user = user;
}

SND_API uint8_t *seq_ptr(Sequencer *s, uint32_t addr)
{
    uint32_t o = (addr & 0x7FFFFu) - s->base;
    return o < s->len ? &s->mem[o] : NULL;
}

SND_API Sequencer *seq_new(void)       { return (Sequencer *)calloc(1, sizeof(Sequencer)); }
SND_API void seq_delete(Sequencer *s)  { if (s) { seq_free(s); free(s); } }
SND_API int  seq_nwrites(Sequencer *s) { return s->nwrites; }
SND_API int  seq_overflow(Sequencer *s) { return s->overflow; }
SND_API SeqWrite *seq_writes(Sequencer *s) { return s->writes; }
SND_API void seq_set_regs(Sequencer *s, uint32_t a0, uint32_t a3, int keep)
{
    s->a0 = a0;
    s->a3 = a3;
    s->keep_regs = keep;
}

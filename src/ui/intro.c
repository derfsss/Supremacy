/* The intro program INT1 / INT2. See intro.h. Addresses are the program's
 * (INT1 at $900, INT2 at $49CE0). */
#include <stdlib.h>
#include <string.h>

#include "intro.h"
#include "gamedata.h"

#define MEM    0x80000u
#define PROG   0x900u
#define INT2AT 0x49CE0u
#define SCREEN 0x72000u                /* 5 interleaved planes, 200 bytes a row */
#define RING   0x6B000u                /* the unpacker's ring buffer */
#define SHADOW 0x1C20u                 /* palette copy the fades work on */

/* timing (PAL): 313 lines of 453 cycles, E-clock = CPU / 10 */
#define LINES       313
#define TICKS_LINE  (453 / 10.0)
#define TIMER_LATCH 0x0DEB             /* $BCC: CIA-A timer B, continuous */
#define BOOT_FRAME  2                  /* timer started and interrupts on ... */
#define BOOT_LINE   167                /* ... during this line */
#define VBL_LINES   5                  /* the vertical-blank handler: lines 0-4 */
#define MAIN_LINE   5                  /* the main program reads $B44 after line 5's interrupts */

/* program points (where the main program waits, or continues) */
enum {
    P_BOOT, P_DF0, P_HOLD, P_ANIM, P_FADE,
    P_T1HOLD, P_AFTER_TITLE, P_STORY1, P_AFTER_S1, P_S2, P_S2A, P_AFTER_S2, P_S3A, P_AFTER_S3,
    P_T2, P_T2HOLD, P_AFTER_T2, P_T2LOOP, P_PRE_BANNER, P_BANNER, P_STARS1,
    P_CLOOP, P_CIN, P_AFTER_CIN, P_C2, P_AFTER_C2, P_CHOLD_T, P_CHOLD, P_COUT, P_AFTER_COUT,
    P_SEL, P_SEL1, P_SEL2, P_SEL3, P_SELLOOP, P_VIEW_XOR,
    P_LOAD1, P_LOADW, P_FINAL, P_DONE
};

static uint16_t r16(const Intro *in, uint32_t a)
{
    a &= MEM - 1;
    return (uint16_t)(in->mem[a] << 8 | in->mem[(a + 1) & (MEM - 1)]);
}
static uint32_t r32(const Intro *in, uint32_t a) { return (uint32_t)r16(in, a) << 16 | r16(in, a + 2); }
static void w16(Intro *in, uint32_t a, uint16_t v)
{
    a &= MEM - 1;
    in->mem[a] = (uint8_t)(v >> 8);
    in->mem[(a + 1) & (MEM - 1)] = (uint8_t)v;
}
static void w32(Intro *in, uint32_t a, uint32_t v) { w16(in, a, (uint16_t)(v >> 16)); w16(in, a + 2, (uint16_t)v); }
static uint8_t *m8(Intro *in, uint32_t a) { return &in->mem[a & (MEM - 1)]; }
static int skip(const Intro *in) { return in->mem[0x2368] != 0; }   /* tst.b $2368 */

/* ------------------------------------------------------------ pictures */

/* $460C4's LZSS ($460F8): from a1 (d0 bytes) to a2, ring at $6B000 */
static uint32_t unpack(Intro *in, uint32_t a1, uint32_t n, uint32_t a2)
{
    uint32_t a4 = a1 + n;
    for (uint32_t k = 0; k < 0xFEE; k++) *m8(in, RING + k) = 0;
    unsigned d3 = 0xFEE, d5 = 0;
    for (;;) {
        d5 = (d5 & 0xFFFF0000u) | ((d5 & 0xFFFFu) >> 1);
        if (!(d5 & 0x100)) {
            if (a1 >= a4) break;
            d5 = 0xFFFFFF00u | *m8(in, a1++);
        }
        if (d5 & 1) {
            if (a1 >= a4) break;
            uint8_t c = *m8(in, a1++);
            *m8(in, a2++) = c;
            *m8(in, RING + d3) = c;
            d3 = (d3 + 1) & 0xFFF;
        } else {
            if (a1 >= a4) break;
            unsigned b0 = *m8(in, a1++);
            if (a1 >= a4) break;
            unsigned b1 = *m8(in, a1++);
            unsigned d0 = b0 | ((b1 << 4) & 0xF00u);
            unsigned cnt = (b1 & 15) + 2;
            for (unsigned k = 0; k <= cnt; k++) {
                uint8_t c = *m8(in, RING + d0);
                d0 = (d0 + 1) & 0xFFF;
                *m8(in, a2++) = c;
                *m8(in, RING + d3) = c;
                d3 = (d3 + 1) & 0xFFF;
            }
        }
    }
    return a2;
}

/* Frames the 68000 needs from the start of the pass that unpacks a picture
 * to its next frame wait (measured in the emulator, no input; the boot's
 * includes the screen clear). The port idles for as long. */
static int unpack_frames(uint32_t pic, int boot)
{
    switch (pic) {
    case 0x46182: return boot ? 36 : 34;   /* title */
    case 0x054A6: return 34;               /* the figure under the star */
    case 0x0A150: return 31;               /* the hand with the planet */
    case 0x0D940: return 31;
    case 0x14776: return 13;               /* the banner with the credits font */
    case 0x11054: return 30;               /* opponent selection */
    case 0x1439C: return 9;                /* LOADING */
    default:      return 0;
    }
}

/* $53CE / $53F2 up to the palette: COLOR00 = 0, display DMA off, unpack the
 * picture at a1 to the screen ($460C4). Returns its palette (a0). */
static uint32_t show_picture(Intro *in, uint32_t a1)
{
    in->colour[0] = 0;
    in->dmacon &= (uint16_t)~0x1A0;
    in->busy = unpack_frames(a1, in->pc == P_BOOT);
    uint32_t a2 = SCREEN;
    unsigned planes = r16(in, a1);                     /* $460C4 */
    a1 += 2;
    uint32_t pal = a1;
    uint32_t end = a2 + (uint32_t)(uint16_t)planes * 0x1F40u;
    a1 += (uint16_t)(2u << planes);
    unsigned zero = r16(in, a1);
    a1 += 2;
    for (unsigned k = 0; k < zero; k++) *m8(in, a2++) = 0;
    unsigned n = r16(in, a1);
    a1 += 2;
    a2 = unpack(in, a1, n, a2);
    if (end > a2) {                                    /* $460E6: the rest cleared */
        uint16_t d0 = (uint16_t)(end - a2);
        for (unsigned k = 0; k < d0; k++) *m8(in, a2++) = 0;
    }
    return pal;
}

/* $DF0 before its wait: $B1C = $B18; the palette is set after the wait */
static void df0(Intro *in, uint32_t pal, int next)
{
    w32(in, 0xB1C, r32(in, 0xB18));
    in->pal = pal;
    in->next = next;
    in->pc = P_DF0;
}

/* $DF0 after its wait: 32 colours from a0 to COLOR00-31 and the shadow,
 * display DMA on */
static void set_palette(Intro *in, uint32_t a0)
{
    for (int k = 0; k < 32; k++) {
        uint16_t c = r16(in, a0 + 2u * (uint32_t)k);
        in->colour[k] = c;
        w16(in, SHADOW + 2u * (uint32_t)k, c);
    }
    in->dmacon |= 0x1A0;
}

/* $E56 (after $53F2): $B1C = $B18, 32 colours to the shadow only, display
 * DMA on (the colour registers keep what they have) */
static void set_shadow(Intro *in, uint32_t a0)
{
    w32(in, 0xB1C, r32(in, 0xB18));
    for (int k = 0; k < 32; k++) w16(in, SHADOW + 2u * (uint32_t)k, r16(in, a0 + 2u * (uint32_t)k));
    in->dmacon |= 0x1A0;
}

/* $E92 after its wait */
static void blank(Intro *in)
{
    in->colour[0] = 0;
    in->dmacon &= (uint16_t)~0x1A0;
}

/* $13D6 after its wait: every component of the shadow one step down, to
 * COLOR00-31 too; 1 while any colour was not black */
static int fade_out(Intro *in)
{
    uint16_t d5 = 0;
    for (int k = 0; k < 32; k++) {
        uint16_t d0 = r16(in, SHADOW + 2u * (uint32_t)k);
        d5 |= d0;
        if (d0 & 0xF00) d0 -= 0x100;
        if (d0 & 0x0F0) d0 -= 0x10;
        if (d0 & 0x00F) d0 -= 1;
        in->colour[k] = d0;
        w16(in, SHADOW + 2u * (uint32_t)k, d0);
    }
    return d5 != 0;
}

/* $53AA: XOR one delta frame onto the screen: u16 offset, u16 count - 1,
 * bytes (0 = skip the next byte's count of bytes, which counts 2) */
static void xor_delta(Intro *in, uint32_t a0)
{
    uint32_t a1 = SCREEN + r16(in, a0);
    uint16_t d1 = r16(in, a0 + 2);
    a0 += 4;
    for (;;) {
        uint8_t d0 = *m8(in, a0++);
        if (d0) {
            *m8(in, a1++) ^= d0;
            if (d1-- == 0) return;
        } else {
            a1 += *m8(in, a0++);
            d1 = (uint16_t)(d1 - 2);
        }
    }
}

/* $134A: one animation tick. Script ($13CE) longs: $FF end, 4 + n hold n
 * ticks ($13D2), 0 + address jump, else a delta frame address. One step
 * every $13D5 ticks ($13D4 counts; $FF stops it). */
static void anim_tick(Intro *in)
{
    uint16_t delay = r16(in, 0x13D2);
    if (delay) { w16(in, 0x13D2, (uint16_t)(delay - 1)); return; }
    uint8_t speed = *m8(in, 0x13D5);
    if (speed == 0xFF) return;
    uint8_t ph = (uint8_t)(*m8(in, 0x13D4) + 1);
    if (ph == speed) ph = 0;
    *m8(in, 0x13D4) = ph;
    if (ph) return;
    uint32_t a0 = r32(in, 0x13CE);
    if (!a0) return;
    for (;;) {
        uint32_t d0 = r32(in, a0);
        a0 += 4;
        if (d0 == 0xFF) { w32(in, 0x13CE, 0); return; }
        if (d0 == 4) {
            w16(in, 0x13D2, (uint16_t)r32(in, a0));
            w32(in, 0x13CE, a0 + 4);
            return;
        }
        if (d0 == 0) { a0 = r32(in, a0); continue; }
        w32(in, 0x13CE, a0);
        xor_delta(in, d0);
        return;
    }
}

/* ------------------------------------------------------------ credits */

/* $14B4: take a 16 x 24 glyph (planes 2-4) off the screen at a0 into a5,
 * clearing it there */
static uint32_t grab_glyph(Intro *in, uint32_t a0, uint32_t a5)
{
    for (int r = 0; r < 24; r++) {
        for (uint32_t o = 0x50; o <= 0xA0; o += 0x28) {
            w16(in, a5, r16(in, a0 + o));
            a5 += 2;
            w16(in, a0 + o, 0);
        }
        a0 += 0xC8;
    }
    return a5;
}

/* $14DA: draw the glyph at a5 at a0 (byte writes: a0 may be odd) */
static void draw_glyph(Intro *in, uint32_t a0, uint32_t a5)
{
    for (int r = 0; r < 24; r++) {
        for (uint32_t o = 0x50; o <= 0xA0; o += 0x28) {
            *m8(in, a0 + o) = *m8(in, a5++);
            *m8(in, a0 + o + 1) = *m8(in, a5++);
        }
        a0 += 0xC8;
    }
}

/* $1500: clear a glyph cell */
static void clear_glyph(Intro *in, uint32_t a0)
{
    for (int r = 0; r < 24; r++) {
        for (uint32_t o = 0x50; o <= 0xA0; o += 0x28) {
            *m8(in, a0 + o) = 0;
            *m8(in, a0 + o + 1) = 0;
        }
        a0 += 0xC8;
    }
}

/* $1BA2: type one page of the credits. Text at ($1528): 2 bytes to $1B9F /
 * $1BA0 ($1BA0: the fade style and hold time), then lines: x offset, chars
 * ($20 space, else 'A'... a glyph), $FE + n (next line: back n, down $1428),
 * $FF (end of the page), 0 (end of the credits: the skip flag). */
static void credits_page(Intro *in)
{
    uint32_t a1 = r32(in, 0x1528);
    uint32_t a0 = 0x74328;
    *m8(in, 0x1B9F) = *m8(in, a1++);
    *m8(in, 0x1BA0) = *m8(in, a1++);
    for (;;) {
        a0 += *m8(in, a1++);                           /* $1BB4 */
        for (;;) {
            uint8_t d0 = *m8(in, a1++);                /* $1BBA */
            if (d0 == 0) { *m8(in, 0x2368) = 0xFF; return; }
            if (d0 == 0xFF) { w32(in, 0x1528, a1); return; }
            if (d0 == 0xFE) {
                a0 -= *m8(in, a1++);
                a0 += 0x1428;
                break;
            }
            if (d0 == 0x20) clear_glyph(in, a0);
            else draw_glyph(in, a0, 0x24E0u + (uint32_t)(uint8_t)(d0 - 0x41) * 0x90u);
            a0 += 2;
        }
    }
}

/* $1C60 / $1CE2 / $1D8C after their waits: colour groups one step up to
 * their targets (every 4th shadow colour). `groups` groups of 4 registers
 * from COLOR(first), current values from `cur`, targets from `target` step
 * 8 bytes. swap: $1D8C's quirk, red written as blue and blue dropped.
 * 1 while something changed. */
static int fade_in(Intro *in, int first, uint32_t target, uint32_t cur, int groups, int swap)
{
    int d5 = 0;
    for (int g = 0; g < groups; g++) {
        uint16_t now = r16(in, cur), to = r16(in, target);
        uint16_t d3 = 0, d6 = 0, d0;
        d0 = now & 0xF00;
        if (d0 != (to & 0xF00)) { d0 = (uint16_t)(d0 + 0x100); d5 = 1; }
        d3 |= d0;
        d6 |= (uint16_t)(d0 >> 8);
        d0 = now & 0x0F0;
        if (d0 != (to & 0x0F0)) { d0 = (uint16_t)(d0 + 0x10); d5 = 1; }
        d3 |= d0;
        d6 |= d0;
        d0 = now & 0x00F;
        if (d0 != (to & 0x00F)) { d0 = (uint16_t)(d0 + 1); d5 = 1; }
        d3 |= d0;                                      /* $1E26: lsl.w #8,d0 not or'ed into d6 */
        w16(in, cur, d3);
        cur += 2;
        for (int k = 0; k < 4; k++) in->colour[first + 4 * g + k] = swap ? d6 : d3;
        target += 8;
    }
    return d5;
}

/* $1E40 after its wait: COLOR04, 08 ... 28 one step down from $1C12...;
 * 1 while any was not black */
static int fade_text_out(Intro *in)
{
    uint16_t d5 = 0;
    for (int k = 0; k < 7; k++) {
        uint32_t a = 0x1C12u + 2u * (uint32_t)k;
        uint16_t d0 = r16(in, a);
        d5 |= d0;
        if (d0 & 0xF00) d0 -= 0x100;
        if (d0 & 0x0F0) d0 -= 0x10;
        if (d0 & 0x00F) d0 -= 1;
        in->colour[4 + 4 * k] = d0;
        w16(in, a, d0);
    }
    return d5 != 0;
}

/* ------------------------------------------------------------ starfield */

/* $5480: s = 13 s + 7; returns the low word XOR the high word */
static uint16_t rnd(Intro *in)
{
    uint32_t s = r32(in, 0x5456) * 13u + 7u;
    w32(in, 0x5456, s);
    return (uint16_t)(s ^ (s >> 16));
}

/* $236A: x += dx, y += dy */
static void star_move(Intro *in, uint32_t a0)
{
    w16(in, a0, (uint16_t)(r16(in, a0) + r16(in, a0 + 2)));
    w16(in, a0 + 4, (uint16_t)(r16(in, a0 + 4) + r16(in, a0 + 6)));
}

/* $242C: clear the byte of planes 0-1 where the star was, plot it in colour
 * d4 & 3 (0 -> 1) */
static void star_plot(Intro *in, uint32_t a0, uint16_t d4)
{
    unsigned d2 = d4 & 3;
    if (!d2) d2 = 1;
    uint16_t x = r16(in, a0), y = r16(in, a0 + 4);
    uint32_t a1 = r32(in, a0 + 8);
    *m8(in, a1) = 0;
    *m8(in, a1 + 0x28) = 0;
    a1 = r32(in, 0x45DA4u + (uint32_t)(int16_t)(uint16_t)(y * 4));
    a1 += (uint16_t)(x >> 3);
    w32(in, a0 + 8, a1);
    unsigned bit = (x & 7) ^ 7;
    *m8(in, a1) &= (uint8_t)~(1u << bit);
    if (d2 & 1) *m8(in, a1) |= (uint8_t)(1u << bit);
    *m8(in, a1 + 0x28) &= (uint8_t)~(1u << bit);
    if (d2 & 2) *m8(in, a1 + 0x28) |= (uint8_t)(1u << bit);
}

/* $237A: move and plot $2366 + 1 stars (12-byte entries at $1EAA: x, dx, y,
 * dy, screen address); a star off the screen starts again near the centre
 * with a random non-zero speed, 6 steps out. One more star each frame up
 * to 100. */
static void stars(Intro *in)
{
    uint32_t a0 = 0x1EAA;
    uint16_t d4 = r16(in, 0x2366);
    for (;;) {
        if (r16(in, a0) >= 0x140 || r16(in, a0 + 4) >= 0xC8) goto respawn;      /* $2382 */
        star_move(in, a0);
        if (r16(in, a0) >= 0x140 || r16(in, a0 + 4) >= 0xC8) goto respawn;
        star_plot(in, a0, d4);
        a0 += 12;
        if (d4-- == 0) break;
        continue;
    respawn:                                           /* $23BC */
        w16(in, a0, (uint16_t)((rnd(in) & 7) + 0x9C));
        w16(in, a0 + 4, (uint16_t)((rnd(in) & 7) + 0x60));
        for (;;) {
            uint16_t d5 = rnd(in) & 3;
            d5 = (uint16_t)(d5 - (rnd(in) & 3));
            w16(in, a0 + 2, d5);
            uint16_t d6 = rnd(in) & 3;
            d6 = (uint16_t)(d6 - (rnd(in) & 3));
            w16(in, a0 + 6, d6);
            if (d5 | d6) break;
        }
        for (int k = 0; k < 6; k++) star_move(in, a0);
    }
    if (r16(in, 0x2366) != 0x63) w16(in, 0x2366, (uint16_t)(r16(in, 0x2366) + 1));
}

/* ------------------------------------------------------------ selection */

/* $3582: the pointer sprite ($362E) at ($A90, $A96) unless $3580 is set ($F80) */
static void pointer(Intro *in)
{
    if (*m8(in, 0x3580)) return;
    uint16_t d0 = (uint16_t)(r16(in, 0xA90) + 0x80);
    uint16_t d1 = (uint16_t)(r16(in, 0xA96) + 0x2C);
    uint16_t d2 = (uint16_t)(16 + d1);
    uint32_t a0 = 0x362E;
    *m8(in, a0++) = (uint8_t)d1;
    d1 &= 0xFF00;
    unsigned x = d0 & 1;
    d0 >>= 1;
    d1 = (uint16_t)((d1 & 0xFF00) | x);
    *m8(in, a0++) = (uint8_t)d0;
    if (d1 & 0x100) d1 |= 4;
    *m8(in, a0++) = (uint8_t)d2;
    if (d2 & 0x100) d1 |= 2;
    *m8(in, a0) = (uint8_t)d1;
}

/* $1420 (VIEW OPPONENT): the next entry of the list at $24A0 (delta frame,
 * Rorn flag -> $24D8, number -> $249C); wraps at the zero entry. Rorn: the
 * plain copper list (the vertical blank picks it) and the palette at $1474,
 * which waits a frame. Returns 1 if it waits. */
static int view_next(Intro *in)
{
    uint32_t a0 = r32(in, 0x24DC);
    uint32_t d0;
    for (;;) {
        d0 = r32(in, a0);
        w32(in, 0x24D8, r32(in, a0 + 4));
        w32(in, 0x249C, r32(in, a0 + 8));
        a0 += 12;
        if (d0) break;
        a0 = 0x24A0;
    }
    w32(in, 0x24DC, a0);
    if (r32(in, 0x24D8)) {
        /* move.l #$B14,COP2LC: the second copper list, never jumped to */
        df0(in, 0x1474, P_VIEW_XOR);
        in->view_delta = d0;
        return 1;
    }
    xor_delta(in, d0);
    return 0;
}

/* $3730 (CHALLENGE): opponent and seed for the game, LOADING */
static void challenge(Intro *in)
{
    *m8(in, 0x800) = (uint8_t)r32(in, 0x249C);
    w32(in, 0x802, r32(in, 0xB44));
    in->challenge = 1;
    in->opponent = *m8(in, 0x800);
    in->seed = r32(in, 0x802);
    w32(in, 0xB18, 0xB24);
    df0(in, show_picture(in, 0x1439C), P_LOAD1);
}

/* $35A8: the hotspots ($3570: table, $3574: count; 16-byte entries x0, y0,
 * x1, y1, delta frame, handler). A press inside one draws its delta frame
 * and calls the handler, once per press ($35A6); the release draws the
 * delta frame again. */
static void hotspots(Intro *in)
{
    if (*m8(in, 0x3580)) return;
    if (!*m8(in, 0xA76)) {                             /* $3626 */
        if (*m8(in, 0x35A6)) {
            *m8(in, 0x35A6) = 0;
            uint32_t a0 = r32(in, 0x35A2);
            if (a0) xor_delta(in, a0);
        }
        return;
    }
    if (*m8(in, 0x35A6)) return;
    uint32_t a2 = r32(in, 0x3570);
    uint16_t d4 = (uint16_t)(r16(in, 0x3574) - 1);
    int16_t x = (int16_t)r16(in, 0xA90), y = (int16_t)r16(in, 0xA96);
    for (;;) {
        if (x >= (int16_t)r16(in, a2) && y >= (int16_t)r16(in, a2 + 2) &&
            x <= (int16_t)r16(in, a2 + 4) && y <= (int16_t)r16(in, a2 + 6)) {
            uint32_t a0 = r32(in, a2 + 8);
            w32(in, 0x35A2, a0);
            if (a0) xor_delta(in, a0);
            *m8(in, 0x35A6) = 0xFF;
            uint32_t h = r32(in, a2 + 12);
            if (h == 0x1420) {
                if (view_next(in)) return;
            } else if (h == 0x3730) {
                challenge(in);
            }
            return;
        }
        a2 += 16;
        if (d4-- == 0) return;
    }
}

/* ------------------------------------------------------------ the main program */

/* $900-$9BC: the boot's memory effects ($FB0 mouse, $CE0 screen, $BCC) */
static void boot(Intro *in)
{
    *m8(in, 0xA0C) = 0;                                /* $FB0 */
    uint16_t d2 = (uint16_t)(0x13E * r16(in, 0xA94));
    uint16_t d5 = (uint16_t)(0xC6 * r16(in, 0xA9A));
    w16(in, 0xA92, (uint16_t)(d2 - 1));
    w16(in, 0xA90, (uint16_t)(d2 >> 1));
    w16(in, 0xA98, (uint16_t)(d5 - 1));
    w16(in, 0xA96, (uint16_t)(d5 >> 1));
    /* $CE0: its colour loop is `movea.l d0,a0` 16 times, so the colours
     * are not cleared */
    w32(in, 0xA9E, SCREEN);
    memset(in->mem + SCREEN, 0, 0x2710u * 4u);
    in->cop1lc = 0xAAC;
    for (int i = 0; i < 8; i++) in->sprpt[i] = 0xB20;
    *m8(in, 0xB48) = 0;                                /* $BCC */
    *m8(in, 0xB4A) = 0;
}

/* one pass: from where the program waits up to its next frame wait */
void intro_main(Intro *in)
{
#define WAIT(p) do { in->pc = (p); goto out; } while (0)
    in->busy = 0;
    for (;;) switch (in->pc) {
    case P_BOOT:
        boot(in);                                      /* $10D6: jsr $68000 (d7 = 0) in frame 2, see cia_lines */
        *m8(in, 0x10D5) = 0;
        df0(in, show_picture(in, 0x46182), P_T1HOLD);
        goto out;
    case P_DF0:                                        /* $DF0 after its wait */
        set_palette(in, in->pal);
        in->pc = in->next;
        continue;
    case P_HOLD:                                       /* $378E: $FA + 1 frames or the skip flag */
        if (skip(in) || in->count-- == 0) { in->pc = in->ret; continue; }
        WAIT(P_HOLD);
    case P_ANIM:                                       /* $1330 */
        anim_tick(in);
        if (skip(in) || !r32(in, 0x13CE)) { in->pc = in->ret; continue; }
        WAIT(P_ANIM);
    case P_FADE:                                       /* $13D6 (no skip test) */
        if (fade_out(in)) WAIT(P_FADE);
        in->pc = in->ret;
        continue;

    case P_T1HOLD:
        in->count = 0xFA;
        in->ret = P_AFTER_TITLE;
        WAIT(P_HOLD);
    case P_AFTER_TITLE:
        if (skip(in)) { in->pc = P_SEL; continue; }
        df0(in, show_picture(in, 0x54A6), P_STORY1);
        goto out;
    case P_STORY1:
        *m8(in, 0x13D5) = 0x0B;
        w32(in, 0x13CE, 0x50A6);
        in->ret = P_AFTER_S1;
        WAIT(P_ANIM);
    case P_AFTER_S1:
        if (skip(in)) { in->pc = P_SEL; continue; }
        in->ret = P_S2;
        WAIT(P_FADE);
    case P_S2:
        df0(in, show_picture(in, 0xA150), P_S2A);
        goto out;
    case P_S2A:
        *m8(in, 0x13D5) = 8;
        w32(in, 0x13CE, 0x51F2);
        in->ret = P_AFTER_S2;
        WAIT(P_ANIM);
    case P_AFTER_S2:
        if (skip(in)) { in->pc = P_SEL; continue; }
        df0(in, show_picture(in, 0xD940), P_S3A);
        goto out;
    case P_S3A:
        *m8(in, 0x13D5) = 8;
        w32(in, 0x13CE, 0x524E);
        in->ret = P_AFTER_S3;
        WAIT(P_ANIM);
    case P_AFTER_S3:
        if (skip(in)) { in->pc = P_SEL; continue; }
        in->ret = P_T2;
        WAIT(P_FADE);
    case P_T2:
        df0(in, show_picture(in, 0x46182), P_T2HOLD);
        goto out;
    case P_T2HOLD:
        in->count = 0xFA;
        in->ret = P_AFTER_T2;
        WAIT(P_HOLD);
    case P_AFTER_T2:
        if (skip(in)) { in->pc = P_SEL; continue; }
        in->count = 0x7D;                              /* $119C */
        WAIT(P_T2LOOP);
    case P_T2LOOP:
        if (skip(in)) { in->pc = P_SEL; continue; }
        if (in->count-- != 0) WAIT(P_T2LOOP);
        in->ret = P_PRE_BANNER;
        WAIT(P_FADE);
    case P_PRE_BANNER:
        WAIT(P_BANNER);                                /* $11B6 */
    case P_BANNER: {
        set_shadow(in, show_picture(in, 0x14776));     /* $53F2 */
        uint32_t a5 = 0x24E0, a0 = SCREEN;             /* $11C4: the font */
        for (int k = 0; k < 18; k++, a0 += 2) a5 = grab_glyph(in, a0, a5);
        a0 += 0x129C;
        for (int k = 0; k < 11; k++, a0 += 2) a5 = grab_glyph(in, a0, a5);
        for (int k = 0; k < 32; k++) in->colour[k] = r16(in, SHADOW + 2u * (uint32_t)k);
        in->colour[1] = 0xFFF;
        in->colour[2] = 0xAAA;
        in->colour[3] = 0x555;
        in->count = 0x63;
        WAIT(P_STARS1);
    }
    case P_STARS1:                                     /* $122C */
        stars(in);
        if (skip(in)) { in->pc = P_SEL; continue; }
        if (in->count-- != 0) WAIT(P_STARS1);
        for (int k = 0; k < 7; k++) {                  /* $1246 */
            w16(in, 0x1C12u + 2u * (uint32_t)k, 0);
            for (int j = 0; j < 4; j++) in->colour[4 + 4 * k + j] = 0;
        }
        in->pc = P_CLOOP;
        continue;
    case P_CLOOP:                                      /* $1264 */
        credits_page(in);
        if (skip(in)) { in->pc = P_SEL; continue; }
        WAIT(P_CIN);
    case P_CIN:                                        /* $1C60 */
        stars(in);
        if (!skip(in) && fade_in(in, 4, 0x1C28, 0x1C12, 1, 0)) WAIT(P_CIN);
        in->pc = P_AFTER_CIN;
        continue;
    case P_AFTER_CIN:
        if (skip(in)) { in->pc = P_SEL; continue; }
        in->sub = 0;                                   /* $1CE2 / $1D8C: 3 frames a step */
        WAIT(P_C2);
    case P_C2:
        stars(in);
        if (skip(in)) { in->pc = P_AFTER_C2; continue; }
        if (++in->sub < 3) WAIT(P_C2);
        in->sub = 0;
        if (fade_in(in, 8, 0x1C30, 0x1C14, 6, *m8(in, 0x1BA0) != 0)) WAIT(P_C2);
        in->pc = P_AFTER_C2;
        continue;
    case P_AFTER_C2:
        if (skip(in)) { in->pc = P_SEL; continue; }
        in->count = *m8(in, 0x1BA0) ? 0x96 : 0x32;    /* $1296 */
        in->pc = P_CHOLD_T;
        continue;
    case P_CHOLD_T:                                    /* $12A6 */
        if (skip(in)) { in->pc = P_SEL; continue; }
        WAIT(P_CHOLD);
    case P_CHOLD:
        stars(in);
        if (in->count-- != 0) { in->pc = P_CHOLD_T; continue; }
        for (int k = 0; k < 7; k++)                    /* $1E40 */
            w16(in, 0x1C12u + 2u * (uint32_t)k, r16(in, 0x1C28u + 8u * (uint32_t)k));
        WAIT(P_COUT);
    case P_COUT:
        stars(in);
        if (!skip(in) && fade_text_out(in)) WAIT(P_COUT);
        in->pc = P_AFTER_COUT;
        continue;
    case P_AFTER_COUT:
        if (skip(in)) { in->pc = P_SEL; continue; }
        in->pc = P_CLOOP;
        continue;

    case P_SEL:                                        /* $12CC: $E92 */
        WAIT(P_SEL1);
    case P_SEL1:
        blank(in);
        w32(in, 0x3570, 0x3550);
        w16(in, 0x3574, 2);
        w32(in, 0xB18, 0x3530);                        /* the sprite list with the pointer */
        w32(in, 0x24D8, 0);
        df0(in, show_picture(in, 0x11054), P_SEL2);
        goto out;
    case P_SEL2:
        df0(in, 0x1474, P_SEL3);
        goto out;
    case P_SEL3:
        WAIT(P_SELLOOP);                               /* $1304 */
    case P_SELLOOP:                                    /* $3576 */
        pointer(in);
        hotspots(in);
        if (in->pc != P_SELLOOP) goto out;             /* a handler waits */
        WAIT(P_SELLOOP);
    case P_VIEW_XOR:
        xor_delta(in, in->view_delta);
        WAIT(P_SELLOOP);
    case P_LOAD1:                                      /* $3750 */
        in->count = 0x64;
        WAIT(P_LOADW);
    case P_LOADW:
        if (in->count-- != 0) WAIT(P_LOADW);
        memmove(in->mem + 0x7C000, in->mem + 0x37A2, 0xD6C);   /* the loader */
        WAIT(P_FINAL);                                 /* $E92 */
    case P_FINAL:
        blank(in);
        memset(in->mem + SCREEN, 0, 0x2710u * 4u);
        in->handover = 1;                              /* trap #0, jmp $7C000 */
        in->pc = P_DONE;
        goto out;
    default:                                           /* the game loader runs */
        goto out;
    }
out:
    if (in->pc != P_DONE) *m8(in, 0xAAA) = 0xFF;      /* st.b $AAA: waiting */
#undef WAIT
}

/* ------------------------------------------------------------ interrupts */

/* $EC4: the vertical-blank interrupt (as far as the C has it) */
static void vblank(Intro *in)
{
    /* $101A: JOY0DAT never moves, so the pointer stays where it was put
     * (clamped); no joystick in port 2 ($A9C = 0) */
    *m8(in, 0xA8E) = 0;
    *m8(in, 0xA8F) = 0;
    int16_t x = (int16_t)r16(in, 0xA90), y = (int16_t)r16(in, 0xA96);
    if (x < 0) x = 0;
    if ((uint16_t)x > r16(in, 0xA92)) x = (int16_t)r16(in, 0xA92);
    if (y < 0) y = 0;
    if ((uint16_t)y > r16(in, 0xA98)) y = (int16_t)r16(in, 0xA98);
    w16(in, 0xA90, (uint16_t)x);
    w16(in, 0xA96, (uint16_t)y);
    w16(in, 0xA9C, 0);
    *m8(in, 0xA76) = (in->in_button & 1) ? 0xFF : 0;
    *m8(in, 0xA77) = (in->in_button & 2) ? 0xFF : 0;
    uint32_t b1c = r32(in, 0xB1C);
    for (int i = 0; i < 8; i++) in->sprpt[i] = r32(in, b1c + 4u * (uint32_t)i);
    *m8(in, 0xAAA) = 0;
    if (r32(in, 0x24D8)) in->cop1lc = 0xAAC;          /* title, story, credits, Rorn */
    else in->cop1lc = (*m8(in, 0x10D4) & 1) ? 0x4AF2 : 0x453E;   /* the bands, two lists */
    for (uint32_t a = 0xA0E; a < 0xA8E; a += 4)        /* any key or button down */
        if (r32(in, a)) { *m8(in, 0x2368) = 0xFF; break; }
    *m8(in, 0x10D4) = (uint8_t)(*m8(in, 0x10D4) + 1);
}

/* $C78: a CIA-A interrupt (timer B; the keyboard is read when its bit is
 * set too); every 4th one ticks the music while $10D5 is 0 */
static void cia_irq(Intro *in)
{
    uint8_t d0 = in->icr;
    in->icr = 0;
    if (d0 & 8) {                                      /* $FF2 */
        uint8_t k = (uint8_t)~in->sdr;
        *m8(in, 0xA0C) = k;
        *m8(in, 0xA0E + (k >> 1)) = (k & 1) ? 0 : 0xFF;
    }
    if (d0 & 2) {
        *m8(in, 0xB48) = (uint8_t)(*m8(in, 0xB48) + 1);
        w32(in, 0xB44, r32(in, 0xB44) + 1);
        *m8(in, 0xB49) = 0;
    }
    uint16_t c = (uint16_t)((r16(in, 0xCDE) + 1) & 3);
    w16(in, 0xCDE, c);
    if (!c && !*m8(in, 0x10D5)) in->music_ticks++;    /* jsr $68006 */
}

static void cia_lines(Intro *in, int from, int to)
{
    for (int line = from; line <= to; line++) {
        if (in->handover) return;                      /* the loader turns interrupts off */
        in->eclock_frac += TICKS_LINE;
        int ticks = (int)in->eclock_frac;
        in->eclock_frac -= ticks;
        if (in->timer_on) {
            int32_t t = in->tb - ticks;
            while (t < 0) {
                if (line == in->lose_line) in->lose_line = -1;   /* tests: see intro_lose_irq */
                else in->icr |= 2;
                t += TIMER_LATCH + 1;
            }
            in->tb = t & 0xFFFF;
        }
        if (in->frame == BOOT_FRAME && line == BOOT_LINE) {
            in->tb = TIMER_LATCH;                      /* $BCC: load, start */
            in->timer_on = 1;
            in->irq_on = 1;                            /* $9B8: the pending vertical blank */
            vblank(in);
            in->music_init = 1;                        /* $10D6: jsr $68000 with d7 = 0 */
        }
        if (in->irq_on && (in->icr & 2) && line >= VBL_LINES) cia_irq(in);
    }
}

/* ------------------------------------------------------------ frame */

void intro_input(Intro *in, int mx, int my, int button, int key)
{
    in->in_mx = mx;
    in->in_my = my;
    in->in_button = button;
    in->in_key = key;
}

void intro_irq_head(Intro *in)
{
    in->music_ticks = 0;
    in->music_init = 0;
    if (in->handover) { in->music_on = 0; return; }
    if (in->in_mx >= 0 && in->in_my >= 0) {           /* the front end's pointer */
        w16(in, 0xA90, (uint16_t)in->in_mx);
        w16(in, 0xA96, (uint16_t)in->in_my);
    }
    int down = in->in_key != 0;                        /* a key goes to the serial port */
    if (down != in->key_down) {
        unsigned code = INTRO_KEY_RAW | (down ? 0 : 0x80);
        in->sdr = (uint8_t)~((code << 1) | (code >> 7));
        in->icr |= 8;                                  /* not enabled: seen with the timer */
        in->key_down = down;
    }
    if (in->irq_on) vblank(in);
    cia_lines(in, 0, MAIN_LINE);
}

void intro_irq_tail(Intro *in)
{
    cia_lines(in, MAIN_LINE + 1, LINES - 1);
    in->music_on = in->irq_on && !in->handover && !*m8(in, 0x10D5);
    in->frame++;
}

void intro_frame(Intro *in, int mx, int my, int button, int key)
{
    intro_input(in, mx, my, button, key);
    intro_irq_head(in);
    if (in->busy) in->busy--;
    else intro_main(in);
    intro_irq_tail(in);
}

/* ------------------------------------------------------------ setup */

int intro_init_image(Intro *in, const uint8_t *img, size_t len)
{
    memset(in, 0, sizeof *in);
    in->mem = calloc(1, MEM);
    if (!in->mem) return -1;
    if (len > MEM) len = MEM;
    memcpy(in->mem, img, len);
    /* the main sequence at $10D6 and the music player's entry at $68000 */
    if (r32(in, 0x10D6) != 0x2E3C0000u || r32(in, 0x68000) != 0x4EF90006u) {
        intro_free(in);
        return -1;
    }
    in->pc = P_BOOT;
    in->cop1lc = 0xAAC;
    for (int i = 0; i < 8; i++) in->sprpt[i] = 0xB20;
    in->tb = 0xFFFF;
    in->lose_line = -1;
    return 0;
}

int intro_init(Intro *in, const uint8_t *int1, size_t len1, const uint8_t *int2, size_t len2)
{
    uint8_t *img = calloc(1, MEM);
    if (!img) return -1;
    lzss_unpack(int1, len1, img + PROG, MEM - PROG, NULL);
    if (len2 > MEM - INT2AT) len2 = MEM - INT2AT;
    memcpy(img + INT2AT, int2, len2);
    int r = intro_init_image(in, img, MEM);
    free(img);
    return r;
}

void intro_free(Intro *in)
{
    free(in->mem);
    in->mem = NULL;
}

/* ------------------------------------------------------------ display */

static uint32_t argb12(uint16_t c)
{
    return 0xFF000000u | (uint32_t)((c >> 8) & 15) * 0x110000u
         | (uint32_t)((c >> 4) & 15) * 0x1100u | (uint32_t)(c & 15) * 0x11u;
}

void intro_render(const Intro *in, uint32_t *argb)
{
    if (!(in->dmacon & 0x100)) {                       /* bitplanes off: the background */
        for (int i = 0; i < 320 * 200; i++) argb[i] = argb12(in->colour[0]);
        return;
    }
    /* the copper list: waits for a line (hpos 0) and moves; colours and
     * bitplane pointers in force on each display line ($2C + y) */
    uint16_t reg[0x100];
    for (int i = 0; i < 0x100; i++) reg[i] = 0;
    for (int k = 0; k < 32; k++) reg[(0x180 >> 1) + k] = in->colour[k];
    uint32_t pc = in->cop1lc;
    unsigned wait_line = 0;
    int ended = 0;
    uint32_t pt[5];
    for (int y = 0; y < 200; y++) {
        unsigned beam = 0x2C + (unsigned)y;
        while (!ended && wait_line <= beam) {
            uint16_t w1 = r16(in, pc), w2 = r16(in, pc + 2);
            pc += 4;
            if (w1 == 0xFFFF && w2 == 0xFFFE) { ended = 1; break; }
            if (!(w1 & 1)) { reg[(w1 & 0x1FE) >> 1] = w2; continue; }
            if (w2 & 1) continue;                      /* skip: not used here */
            wait_line = w1 >> 8;
        }
        uint16_t pal[32];
        for (int k = 0; k < 32; k++) pal[k] = reg[(0x180 >> 1) + k];
        for (int p = 0; p < 5; p++)
            pt[p] = ((uint32_t)reg[(0xE0 >> 1) + 2 * p] << 16 | reg[(0xE2 >> 1) + 2 * p])
                  + (uint32_t)y * (40u + 0xA0u);       /* BPLxMOD $A0 */
        uint32_t *row = argb + y * 320;
        for (int x = 0; x < 320; x++) {
            unsigned v = 0, bit = 7 - (x & 7);
            for (int p = 0; p < 5; p++)
                v |= ((in->mem[(pt[p] + (uint32_t)(x >> 3)) & (MEM - 1)] >> bit) & 1u) << p;
            row[x] = argb12(pal[v]);
        }
        if (!(in->dmacon & 0x20)) continue;
        /* sprites: one block each (pos, ctl, data); sprite 0 in front;
         * horizontal start $80 is pixel 0 (as the emulator draws them) */
        for (int i = 7; i >= 0; i--) {
            uint32_t s = in->sprpt[i];
            uint16_t pos = r16(in, s), ctl = r16(in, s + 2);
            if (!pos && !ctl) continue;
            unsigned vs = (unsigned)(pos >> 8) | ((ctl & 4u) << 6);
            unsigned ve = (unsigned)(ctl >> 8) | ((ctl & 2u) << 7);
            int hs = ((pos & 0xFF) << 1) | (ctl & 1);
            if (beam < vs || beam >= ve) continue;
            uint32_t d = s + 4 + 4 * (beam - vs);
            uint16_t a = r16(in, d), b = r16(in, d + 2);
            for (int bit = 0; bit < 16; bit++) {
                unsigned v = ((a >> (15 - bit)) & 1u) | (((b >> (15 - bit)) & 1u) << 1);
                int xx = hs - 0x80 + bit;
                if (v && xx >= 0 && xx < 320) row[xx] = argb12(pal[16 + (i / 2) * 4 + (int)v]);
            }
        }
    }
}

/* ------------------------------------------------------------ for tests */

Intro *intro_new_image(const uint8_t *img, size_t len)
{
    Intro *in = malloc(sizeof *in);
    if (in && intro_init_image(in, img, len)) { free(in); return NULL; }
    return in;
}
void intro_delete(Intro *in) { if (in) { intro_free(in); free(in); } }
uint8_t *intro_mem(Intro *in) { return in->mem; }
uint16_t *intro_colours(Intro *in) { return in->colour; }
int intro_dma(Intro *in) { return in->dmacon; }
/* The music player's raster wait ($6839A: until VHPOSR's low byte is 4 more
 * than now) started on lines 309-312 lasts until line ~58 of the next frame
 * (the byte wraps at 256 and the frame ends at 312), and a CIA interrupt
 * that falls in it is lost: its INTREQ bit is cleared at the handler's end.
 * Without the 68000's cycle timing the C cannot know when that happens
 * (about every 139 frames); a test tells it: the timer underflow at `line`
 * of the coming intro_irq_tail is lost. */
void intro_lose_irq(Intro *in, int line) { in->lose_line = line; }
/* 0 pc, 1 busy, 2 music_ticks, 3 music_on, 4 music_init, 5 challenge,
 * 6 handover, 7 opponent, 8 seed, 9 frame */
int intro_state(Intro *in, int what)
{
    switch (what) {
    case 0: return in->pc;
    case 1: return in->busy;
    case 2: return in->music_ticks;
    case 3: return in->music_on;
    case 4: return in->music_init;
    case 5: return in->challenge;
    case 6: return in->handover;
    case 7: return in->opponent;
    case 8: return (int)in->seed;
    case 9: return (int)in->frame;
    default: return 0;
    }
}

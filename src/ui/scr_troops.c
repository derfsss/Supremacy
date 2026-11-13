/*
 * Screen: troops / platoons ($5CC2, table $F64C), translated routine by
 * routine with the original's addresses. Checked frame by frame against the
 * original game running in an emulator.
 *
 * enter(): $5CC2 ($47DC panel save, only for that entry) / $5CC6 (from the
 * combat screen's $461A) up to the loop at $5D3A.
 * frame(): one iteration of $5D3A: $4ED8 viewer tick, the second viewer
 * (the suit picture's animation, $E3F4/$E3FA/$E42F/$E430) swapped in for a
 * second $4ED8, $5C90 TRAIN lamp, $E42C + 1, $71E0 game pass (and the
 * troops-screen draws the original does inside it), $9A6C hotspots and the
 * handler, the right button ($A9A) -> $60A6.
 *
 * State machine: "NOT ENOUGH CREDITS!" ($5DEA) holds the screen with $31A6,
 * 44 frames of ($E42C + 1, $4ED8) inside the handler. Here the frame that
 * clicks EQUIP starts the hold and returns UI_PAUSED; each of the next 44
 * frames runs one hold step and returns UI_PAUSED, except the 44th, which
 * also finishes the handler ($E421 = 0, $6106) and the rest of the
 * iteration (the right-button test).
 *
 * Auto-repeat (recruit / dismiss) needs no extra state: $E44A-$E44D and the
 * $E422 clear in $5E92 drive it, one call per frame while held.
 *
 * Not modelled (display timing only): the frame waits ($2FFA) inside $62A2
 * and $63D8; the pointer image ($B08); $26C8's slow print ($E43F, never set
 * on this screen).
 */
#include <stddef.h>

#include "ui.h"

#define TROOPS_PICTURE 0x1495Au
#define TROOPS_TABLE   0xF64Cu
#define SUITS          0x59A16u      /* GAM2: 4 x 30 bytes */
#define WEAPONS        0x59A8Eu      /* GAM2: 3 x 20 bytes */

/* ---- shared helpers (also in scr_combat.c, static there) ---- */
static int inblk(uint32_t a, uint32_t n)
{
    return a >= STATE_BASE && a + n <= STATE_BASE + STATE_SIZE && a + n >= a;
}
static unsigned cinc(unsigned c, unsigned n) { return (c & 0xFF00u) | ((c + n) & 0xFFu); }

/* $26C8 (without the slow print): the text at `addr`; col / row are left
 * where the original leaves d0 / d1. */
static void str(MainUI *m, unsigned *col, unsigned *row, uint32_t addr)
{
    const uint8_t *a = ui_text(m, addr);
    unsigned c0 = *col, r0 = *row;
    if (!a) return;
    for (;;) {
        unsigned c = *a++;
        if (c == 0xFF) break;
        if (c >= 0x20) {
            ui_draw_char(m->s, c0, r0, c);
            c0 = cinc(c0, 1);
            continue;
        }
        if (c == 1) { r0 = (r0 & 0xFF00u) | ((r0 + *a++) & 0xFFu); continue; }
        if (c == 2) { c0 = cinc(c0, *a++); continue; }
        if (c == 3 || c == 4) {
            unsigned n = ((*a++ - 1u) & 0xFFu) + 1u, ch = *a++;
            for (unsigned k = 0; k < n; k++) {
                ui_draw_char(m->s, c0, r0, ch);
                if (c == 3) c0 = cinc(c0, 1);
                else r0 = (r0 + 6) & 0xFFFFu;
            }
            continue;
        }
        break;                                  /* code 0: never used */
    }
    *col = c0;
    *row = r0;
}
static unsigned text(MainUI *m, unsigned col, unsigned row, uint32_t addr)
{
    str(m, &col, &row, addr);
    return col;
}

static unsigned put(MainUI *m, unsigned col, unsigned row, unsigned ch)   /* $335E */
{
    ui_draw_char(m->s, col, row, ch);
    return cinc(col, 1);
}

/* $32D4: one digit, unsigned subtraction until a borrow */
static unsigned dig32(MainUI *m, unsigned col, unsigned row, uint32_t *v, uint32_t p)
{
    unsigned ch = 0x2F;
    uint32_t d = *v;
    for (;;) {
        ch = (ch + 1) & 0xFFFFu;
        uint32_t b = d;
        d -= p;
        if (b < p) break;
    }
    *v = d + p;
    return put(m, col, row, ch);
}
static const uint32_t pw32[8] = {10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
/* $31E0 (8 columns), $3204 (6), $3216 (5): padded with spaces, signed compares */
static unsigned pad32(MainUI *m, unsigned col, unsigned row, uint32_t v, int cols)
{
    int k = 8 - cols;
    for (; k < 7; k++) {
        if ((int32_t)v >= (int32_t)pw32[k]) break;
        col = put(m, col, row, 0x20);
    }
    for (; k < 8; k++) col = dig32(m, col, row, &v, pw32[k]);
    return col;
}
/* $327C: unpadded, from 1,000 */
static unsigned unpad1000(MainUI *m, unsigned col, unsigned row, uint32_t v)
{
    int k = 4;
    while (k < 7 && (int32_t)v < (int32_t)pw32[k]) k++;
    for (; k < 8; k++) col = dig32(m, col, row, &v, pw32[k]);
    return col;
}
/* $3346: one digit of a 16-bit number (32-bit signed loop) */
static unsigned dig16(MainUI *m, unsigned col, unsigned row, uint32_t *v, uint32_t p)
{
    uint32_t d = *v & 0xFFFFu;
    unsigned ch = 0x2F;
    do { ch++; d -= p; } while ((int32_t)d >= 0);
    *v = d + p;
    return put(m, col, row, ch);
}
/* $32F0 (5 columns), $32FE (4), $330C (3), $331A (2): signed word compares */
static unsigned pad16(MainUI *m, unsigned col, unsigned row, uint32_t v, int cols)
{
    static const uint16_t pw[5] = {10000, 1000, 100, 10, 1};
    int k = 5 - cols;
    v &= 0xFFFFu;
    for (; k < 4; k++) {
        if ((int16_t)v >= (int16_t)pw[k]) break;
        col = put(m, col, row, 0x20);
    }
    for (; k < 5; k++) col = dig16(m, col, row, &v, pw[k]);
    return col;
}

/* $603C: "ST", "ND", "RD", "TH" after the number n */
static unsigned suffix(MainUI *m, unsigned col, unsigned row, unsigned n)
{
    uint32_t a = 0xE0C;
    uint8_t b = (uint8_t)n;
    if (b != 11 && b != 12 && b != 13) {
        unsigned r = (uint16_t)n % 10u;          /* divs.w #10, swap, andi #$FF */
        a = r == 1 ? 0xE03 : r == 2 ? 0xE06 : r == 3 ? 0xE09 : 0xE0C;
    }
    return text(m, col, row, a);
}

static uint32_t starbase(MainUI *m)             /* $3506 */
{
    return (uint32_t)g16(m->g, 0xE410) * 0x3Au + g32(m->g, G_PLANET_TABLE);
}

/* $95CC / $9644: the "INCOMING MESSAGE" banner on screens other than the
 * main one, at the screen's position in the table $59BCE. */
static void banner(Game *g, int k)
{
    MainUI *m = (MainUI *)g->user;
    if (!m) return;
    uint32_t t = 0x59BCEu + ((uint8_t)(g8(g, 0xE425) << 2));
    unsigned col = ui_m16(m, t), row = ui_m16(m, t + 2);
    if (k >= 0) {
        ui_draw_char(m->s, cinc(col, (unsigned)k), row, g->banner[k]);
    } else if (g8(g, 0xE426) & 1) {
        text(m, col, row, 0xE20);
    } else {
        ui_draw_string(m->s, col, row, g->banner, 0);
    }
}

/* ---- the screen ---- */
static struct {
    int left;                                   /* hold frames still to run */
} hold;

static uint32_t sel(MainUI *m) { return g32(m->g, 0xE3A4); }

static void draw_troops(MainUI *m, uint32_t p)  /* $5F60 */
{
    pad16(m, 0x2C, 0x0B, ui_m16(m, p), 3);
}

static void select_ptr(MainUI *m)               /* $5F46 */
{
    uint32_t p = PLATOON_TABLE + (uint32_t)g8(m->g, 0xE420) * PLATOON_SIZE;
    s32(m->g, 0xE3A4, p);
    draw_troops(m, p);
}

static void draw_credits(MainUI *m)             /* $5F82 */
{
    if (g8(m->g, 0xE425) != 1) return;
    pad32(m, 0x45, 0x32, ui_m32(m, starbase(m) + P_CREDITS), 8);
}

static void draw_pop(MainUI *m)                 /* $5FA4 */
{
    if (g8(m->g, 0xE425) != 1) return;
    pad16(m, 0x49, 0x0B, ui_m16(m, starbase(m) + P_POP), 5);
}

static void panel_clear(MainUI *m)              /* $60EC */
{
    s8(m->g, 0xE421, 0);
    ui_clear_rect(m->s, 0x1A, 0x4E, 5, 0x21);
}

static void redraw(MainUI *m)                   /* $6106 */
{
    Game *g = m->g;
    uint32_t p = sel(m);
    uint8_t f = ui_m8(m, p + 6);
    if (f & 1) {                                /* on a mission */
        if (g8(g, 0xE421) == 2) return;
        panel_clear(m);
        text(m, 0x37, 0x52, 0xE92);
        s16(g, 0xE400, 4);
        s8(g, 0xE421, 2);
        return;
    }
    if (f & 2) {                                /* equipped: where it is */
        uint32_t a = ui_m32(m, p + 2);
        if (!(ui_m8(m, p + 6) & 4)) a += 0x0E;
        text(m, 0x44, 0x25, a);
        if (g8(g, 0xE421) == 3) return;
        panel_clear(m);
        text(m, 0x36, 0x57, 0xEC5);
        text(m, 0x06, 0x2C, 0xED7);
        text(m, 0x1E, 0x2C, 0xEE8);
        s16(g, 0xE400, 4);
        s8(g, 0xE421, 3);
        return;
    }
    text(m, 0x44, 0x25, 0xE12);
    if (g8(g, 0xE421) != 1) {
        panel_clear(m);
        text(m, 0x35, 0x4F, 0xE41);
        s8(g, 0xE421, 1);
    }
    /* $61FC: the equip cost */
    uint16_t c = (uint16_t)(ui_m16(m, SUITS + (uint16_t)(ui_m16(m, sel(m) + 8) * 0x1Eu) + 4)
                          + ui_m16(m, WEAPONS + (uint16_t)(ui_m16(m, sel(m) + 10) * 0x14u) + 4));
    s16(g, 0xE400, 11);
    uint32_t cost = (uint32_t)c * ui_m16(m, sel(m));
    s32(g, 0xE3A0, cost);
    pad32(m, 0x37, 0x6A, cost, 6);              /* $3204 */
}

static void rank(MainUI *m)                     /* $6DB8 */
{
    uint32_t p = g32(m->g, 0xE3A8);
    uint32_t a = (uint32_t)(ui_m8(m, p + 7) / 10u) * 0x13u + 0x194Fu;
    text(m, 0x3B, 0x3F, a);
}

static void show_platoon(MainUI *m, uint32_t p) /* $778E */
{
    Game *g = m->g;
    s32(g, 0xE3A8, p);
    if (inblk(p, PLATOON_SIZE)) {
        if (g16(g, p) == 0) s8(g, p + 7, 0);
    }
    s8(g, 0xE44E, 0xFF);
    uint8_t tr = ui_m8(m, p + 7);
    if (tr) s8(g, 0xE44E, (uint8_t)(9 - (tr >> 4)));
    s8(g, 0xE435, 0);
    if (tr && !(ui_m8(m, p + 6) & 2)) s8(g, 0xE435, 0xFF);
    pad16(m, 0x46, 0xC0, tr, 3);
    rank(m);
}

/* $EF2E: picture static, 118 rows of random words in planes 0 and 1
 * (5 words a row, 4 with $E441 set) */
static void picture_static(MainUI *m, uint32_t addr, int words)
{
    uint32_t o = addr - SCREEN_BASE;
    for (int r = 0; r < 118; r++, o += SCREEN_LINE)
        for (int w = 0; w < words; w++)
            for (int pl = 0; pl < 2; pl++) {
                uint16_t v = rng_next(m->g);
                uint32_t q = o + (uint32_t)pl * 40u + (uint32_t)w * 2u;
                if (q + 1 < SCREEN_SIZE) {
                    m->s->bpl[q] = (uint8_t)(v >> 8);
                    m->s->bpl[q + 1] = (uint8_t)v;
                }
            }
}

static void suit_picture(MainUI *m)             /* $62A2 */
{
    Game *g = m->g;
    ui_clear_rect(m->s, 2, 0x38, 4, 0x75);      /* $60C4 */
    for (int i = 0; i < 3; i++) picture_static(m, 0x74BC2u, 5);
    ui_clear_rect(m->s, 2, 0x38, 4, 0x75);
    uint32_t e = SUITS + (uint16_t)(ui_m16(m, sel(m) + 8) * 0x1Eu);
    for (int i = 0; i < 8; i++) m->colour[16 + i] = ui_m16(m, e + 6 + 2u * i);
    s32(g, 0xE3F4, ui_m32(m, e + 0x16));
    s16(g, 0xE3FA, ui_m16(m, e + 0x1A));
    s8(g, 0xE42F, ui_m8(m, e + 0x1C));
    s8(g, 0xE430, ui_m8(m, e + 0x1D));
    ui_xor_delta(m, ui_m32(m, e));
    uint32_t cost = ui_m16(m, e + 4);
    if (ui_m8(m, sel(m) + 6) & 2) return;
    unsigned col = 6, row = 0x2C;
    str(m, &col, &row, 0xEB4);
    col = unpad1000(m, col, row, cost);
    str(m, &col, &row, 0xEBF);
}

static void weapon_picture(MainUI *m)           /* $63D8 */
{
    Game *g = m->g;
    ui_clear_rect(m->s, 0x0E, 0x38, 3, 0x75);   /* $60D8 */
    s8(g, 0xE441, 0xFF);
    for (int i = 0; i < 3; i++) picture_static(m, 0x74BCEu, 4);
    s8(g, 0xE441, 0);
    ui_clear_rect(m->s, 0x0E, 0x38, 3, 0x75);
    uint32_t e = WEAPONS + (uint16_t)(ui_m16(m, sel(m) + 10) * 0x14u);
    for (int i = 0; i < 7; i++) m->colour[24 + i] = ui_m16(m, e + 6 + 2u * i);
    ui_xor_delta(m, ui_m32(m, e));
    uint32_t cost = ui_m16(m, e + 4);
    if (ui_m8(m, sel(m) + 6) & 2) return;
    unsigned col = 0x1E, row = 0x2C;
    str(m, &col, &row, 0xEB9);
    col = unpad1000(m, col, row, cost);
    str(m, &col, &row, 0xEBF);
}

static void after_redraw(MainUI *m)             /* $6020 */
{
    redraw(m);
    s8(m->g, 0xE42E, 0);
    s16(m->g, 0xE3F8, 0);
    show_platoon(m, sel(m));
}

static void show_number(MainUI *m)              /* $5FF6 */
{
    unsigned n = (unsigned)g8(m->g, 0xE420) + 1u;
    unsigned col = pad16(m, 0x13, 0x0B, n, 3);
    suffix(m, col, 0x0B, n);
    select_ptr(m);
    suit_picture(m);
    weapon_picture(m);
    after_redraw(m);
}

static void pick(MainUI *m, uint8_t d3)         /* $5FE8 */
{
    if (d3 == g8(m->g, 0xE420)) return;
    s8(m->g, 0xE420, d3);
    show_number(m);
}

static void disband(MainUI *m)                  /* $5C50 */
{
    Game *g = m->g;
    ui_sound(m, 13);
    uint32_t p = sel(m), sb = starbase(m);
    if (ui_m32(m, p + 2) != sb) return;
    if (inblk(sb, PLANET_SIZE))
        s32(g, sb + P_CREDITS, g32(g, sb + P_CREDITS) + (uint32_t)ui_m16(m, p) * 100u);
    if (inblk(p, PLATOON_SIZE)) {
        s32(g, p + 2, 0);
        s8(g, p + 6, 0);
    }
    s8(g, 0xE435, 0);
    select_ptr(m);
    after_redraw(m);
}

static void equip(MainUI *m)                    /* $5DEA */
{
    Game *g = m->g;
    uint32_t p = sel(m);
    if (ui_m8(m, p + 6) & 2) return;
    if (ui_m16(m, p) == 0) return;
    ui_sound(m, 13);
    uint32_t sb = starbase(m);
    uint32_t credits = ui_m32(m, sb + P_CREDITS), cost = g32(g, 0xE3A0);
    if (cost > credits) {
        panel_clear(m);
        text(m, 0x36, 0x57, 0xEF5);             /* "NOT ENOUGH CREDITS!" */
        hold.left = 44;                         /* $31A6, then $5E3A */
        return;
    }
    credits -= cost;
    if (inblk(sb, PLANET_SIZE)) s32(g, sb + P_CREDITS, credits);
    pad32(m, 0x45, 0x32, credits, 8);
    p = sel(m);
    if (inblk(p, PLATOON_SIZE)) {
        s8(g, p + 6, (uint8_t)(g8(g, p + 6) | 2));
        s8(g, 0xD458, 0xFF);
        s32(g, p + 2, starbase(m));
    } else {
        s8(g, 0xD458, 0xFF);
    }
    s8(g, 0xE421, 0);
    s8(g, 0xE435, 0);
    redraw(m);
}

static void recruit_dismiss(MainUI *m, int dismiss)   /* $5E84 / $5E8C */
{
    Game *g = m->g;
    s8(g, 0xE44D, dismiss ? 0xFF : 0);
    s8(g, 0xE422, 0);
    uint8_t last = g8(g, 0xE44A);
    s8(g, 0xE44A, g8(g, 0xE42C));
    if ((uint8_t)(last + 1) != g8(g, 0xE42C)) {
        s8(g, 0xE44B, 12);
        s8(g, 0xE44C, 0);
    }
    if (g8(g, 0xE44C)) {
        s8(g, 0xE44C, (uint8_t)(g8(g, 0xE44C) - 1));
        return;
    }
    uint8_t d = g8(g, 0xE44B);
    if (d) d--;
    s8(g, 0xE44B, d);
    s8(g, 0xE44C, d);
    uint32_t p = sel(m), sb = starbase(m);
    if (!inblk(p, PLATOON_SIZE) || !inblk(sb, PLANET_SIZE)) return;
    if (!g8(g, 0xE44D)) {
        if (g16(g, sb + P_POP) == 0 || g16(g, p) == 200) return;
        s16(g, p, (uint16_t)(g16(g, p) + 1));
        s16(g, sb + P_POP, (uint16_t)(g16(g, sb + P_POP) - 1));
        s8(g, 0xD456, 0xFF);
        if (g8(g, p + 7)) s8(g, p + 7, (uint8_t)(g8(g, p + 7) - 1));
    } else {
        if (g16(g, p) == 0 || g16(g, sb + P_POP) == 30000) return;
        s16(g, sb + P_POP, (uint16_t)(g16(g, sb + P_POP) + 1));
        s16(g, p, (uint16_t)(g16(g, p) - 1));
    }
    s8(g, 0xE422, 0);
    select_ptr(m);
    draw_pop(m);
    redraw(m);
}

static void suit(MainUI *m, int up)             /* $6254 / $6276 */
{
    ui_sound(m, 13);
    uint32_t p = sel(m);
    uint16_t s = (uint16_t)(ui_m16(m, p + 8) + (up ? 1 : -1));
    if (s >= 4) s = up ? 0 : 3;
    if (inblk(p, PLATOON_SIZE)) s16(m->g, p + 8, s);
    suit_picture(m);
    redraw(m);
}

static void weapon(MainUI *m, int up)           /* $6382 / $63AC */
{
    ui_sound(m, 13);
    uint32_t p = sel(m);
    uint16_t w = (uint16_t)(ui_m16(m, p + 10) + (up ? 1 : -1));
    if (w >= 3) w = up ? 0 : 2;
    if (inblk(p, PLATOON_SIZE)) s16(m->g, p + 10, w);
    weapon_picture(m);
    redraw(m);
}

/* $5C90: the TRAIN lamp blinks every 4th pass while $E435 is set */
static void lamp(MainUI *m)
{
    Game *g = m->g;
    if (g8(g, 0xE435)) {
        if (g8(g, 0xE42C) & 3) return;
    } else if (!g8(g, 0xE436)) {
        return;
    }
    s8(g, 0xE436, (uint8_t)~g8(g, 0xE436));
    ui_xor_delta(m, 0x239B2);
}

/* The draws the original makes inside $71E0 on this screen: $7736's tail
 * $778E for the selected platoon (passes $20-$37), $5F6E troops (pass $A7),
 * $5FA4 population ($FC), $5F82 credits ($FD). */
static void pass_draws(MainUI *m)
{
    Game *g = m->g;
    uint8_t c = g8(g, 0xE426);
    if (c >= 0x20 && c < 0x38) {
        uint32_t p = PLATOON_TABLE + (uint32_t)(c - 0x20) * PLATOON_SIZE;
        if (!(g8(g, p + 6) & 2) && g8(g, 0xE425) == 1 && sel(m) == p) {
            /* the state part ran in platoon_pass; this is its draw */
            uint8_t tr = g8(g, p + 7);
            pad16(m, 0x46, 0xC0, tr, 3);
            rank(m);
        }
        return;
    }
    if (c < 0x38) return;
    uint16_t n = g16(g, 0xE40E);
    if (c < (uint8_t)(0x56 + 2 * n)) return;
    if (c == 0xA7) { if (g8(g, 0xE425) == 1) draw_troops(m, sel(m)); }
    else if (c == 0xFC) draw_pop(m);
    else if (c == 0xFD) draw_credits(m);
}

static void hooks(MainUI *m)
{
    m->g->user = m;
    m->g->banner_hook = banner;
}

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    Game *g = m->g;
    (void)next;
    hooks(m);
    hold.left = 0;
    if (handler == 0x5CC2) ui_panel_save(m);   /* $47DC ($461A enters at $5CC6) */
    ui_sound(m, 13);
    s32(g, 0xE3EC, TROOPS_TABLE);
    s16(g, 0xE400, 11);
    ui_show_picture(m, TROOPS_PICTURE);
    s8(g, 0xE435, 0);
    s8(g, 0xE436, 0);
    s8(g, 0xE421, 0);
    s8(g, 0xE425, 1);
    select_ptr(m);
    draw_credits(m);
    draw_pop(m);
    s16(g, 0xE3F8, 0);
    s8(g, 0xE42E, 0);
    s32(g, 0xE3F0, 0xFC1E);
    ui_viewer_step(m);
    show_number(m);
    return UI_OK;
}

static int handler(MainUI *m, uint32_t h, uint32_t *next)
{
    switch (h) {
    case 0x44D2: *next = 0x44D2; return UI_GOTO;           /* to the combat screen */
    case 0x6082: {                                          /* next platoon */
        ui_sound(m, 13);
        uint8_t d = (uint8_t)(g8(m->g, 0xE420) + 1);
        if (d >= 0x18) d = 0;
        pick(m, d);
        break;
    }
    case 0x5FC8: {                                          /* previous platoon */
        ui_sound(m, 13);
        uint8_t d = (uint8_t)(g8(m->g, 0xE420) - 1);
        if (d >= 0x18) d = 0x17;
        pick(m, d);
        break;
    }
    case 0x5C50: disband(m); break;
    case 0x5DEA: equip(m); break;
    case 0x5E8C: recruit_dismiss(m, 0); break;
    case 0x5E84: recruit_dismiss(m, 1); break;
    case 0x6276: suit(m, 0); break;
    case 0x6254: suit(m, 1); break;
    case 0x63AC: weapon(m, 0); break;
    case 0x6382: weapon(m, 1); break;
    default: *next = h; return UI_GOTO;                     /* not a box of this table */
    }
    return UI_OK;
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    hooks(m);
    if (hold.left) {                                        /* $31B8 */
        s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        ui_viewer_tick(m);
        if (--hold.left) return UI_PAUSED;
        s8(g, 0xE421, 0);                                   /* $5E3A */
        redraw(m);
        goto right_button;
    }
    ui_viewer_tick(m);
    {                                                       /* $5D42: the second viewer */
        uint32_t a = g32(g, 0xE3F0);
        uint16_t w = g16(g, 0xE3F8);
        uint8_t p = g8(g, 0xE44E), f = g8(g, 0xE42E);
        s32(g, 0xE3F0, g32(g, 0xE3F4));
        s16(g, 0xE3F8, g16(g, 0xE3FA));
        s8(g, 0xE44E, g8(g, 0xE42F));
        s8(g, 0xE42E, g8(g, 0xE430));
        ui_viewer_tick(m);
        s32(g, 0xE3F4, g32(g, 0xE3F0));
        s16(g, 0xE3FA, g16(g, 0xE3F8));
        s8(g, 0xE42F, g8(g, 0xE44E));
        s8(g, 0xE430, g8(g, 0xE42E));
        s8(g, 0xE42E, f);
        s8(g, 0xE44E, p);
        s16(g, 0xE3F8, w);
        s32(g, 0xE3F0, a);
    }
    lamp(m);
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    {
        uint32_t st = 0;
        int r = ui_game_pass(m, &st);
        if (r != UI_OK) { *next = st; return r; }
        pass_draws(m);
    }
    {
        uint32_t h = ui_hotspots(m, ms);
        if (h) {
            int r = handler(m, h, next);
            if (r != UI_OK) return r;
            if (hold.left) return UI_PAUSED;
        }
    }
right_button:
    if (ms->rbutton) return UI_EXIT_TO_MAIN;                /* $60A6 */
    return UI_OK;
}

const ScreenDef scr_troops = {SCR_TROOPS, 0x5CC2, enter, frame};

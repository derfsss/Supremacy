/*
 * Screen: combat ($3A2A, table $F52C), translated routine by routine with
 * the original's addresses. Checked frame by frame against the original
 * game running in an emulator.
 *
 * Entries (enter()):
 *   $3A2A  main-screen box 12: $47DC panel save, then the gate on $E3DC:
 *          owner 10 enters; owner 4 / 1 posts "THIS PLANET NEEDS
 *          FORMATTING!" ($1038) and returns into the main screen's loop
 *          (the rest of that iteration is run here); no slot in use and no
 *          troops: owner 7 gives ACCESS DENIED ($8F0E), others the
 *          formatting message.
 *   $44D2  troops box 0: $E3D4 = the platoon's planet if +$02 is a planet
 *          record, then $4500 (or straight to its gate).
 *   $4500  left-column combat icon of the other screens: $3534, then the
 *          gate on $E3D4 (else $E3DC): owner 10, a slot in use or troops
 *          enter; anything else (owners 4 and 1 included) is ACCESS DENIED.
 *   then $3A76 up to the loop: $464A, $393E, $86C4, $372E, $9714, the
 *   $91E2 ground pass (core), $33D4 auto-select of the first battle cruiser.
 *
 * frame(): one iteration of $3B14: $4ED8 viewer tick, $38CC redraws
 * ($E39C bits 4 and 19), $8198 platoon grid (load), $3568 bars, $9A6C
 * hotspots and the handler, $71E0 game pass, $E42C + 1, the right button
 * ($A9A): sound 13, then $60A6 (sound 13 again).
 *
 * State machine: the refusals ("THAT'S EMPTY!", "CAN'T CARRY TROOPS!",
 * "SELECT A SHIP!", "SHIP IS FULL!") print, lock input ($3006), hold the
 * screen 44 frames ($31A6: $E42C + 1, $4ED8 each), unlock ($301C) and blank
 * the line. The frame that starts one returns UI_PAUSED; each of the next 44
 * frames runs one hold step (UI_PAUSED), and the 44th also finishes the
 * routine and the rest of the iteration: from $3568 when the hold was in
 * $8198, from the game pass when it was in a hotspot handler.
 *
 * WHDLoad's $3854 patch: "CAN'T CARRY TROOPS!" shows the clicked ship's
 * class ($388C with the clicked bay), not $E3D8's.
 *
 * Kept outside the block: the platoons aboard $E3D8 ($59B3E, 4 longs in the
 * original's GAM2 memory, rebuilt only by $33EC; scr_combat_set_aboard
 * resyncs it for the check).
 *
 * Not modelled (display timing only): the frame waits ($2FFA) in $3974 /
 * $39D0 and $245E, the busy loop in $245E, the pointer image ($B08),
 * $26C8's slow print ($E43F, never set on this screen).
 */
#include <stddef.h>
#include <string.h>

#include "ui.h"
#include "combat.h"

#define COMBAT_PICTURE 0x1BB16u
#define COMBAT_TABLE   0xF52Cu

extern const ScreenDef scr_troops;

/* ---- shared helpers (also in scr_troops.c, static there) ---- */
static int inblk(uint32_t a, uint32_t n)
{
    return a >= STATE_BASE && a + n <= STATE_BASE + STATE_SIZE && a + n >= a;
}
static unsigned cinc(unsigned c, unsigned n) { return (c & 0xFF00u) | ((c + n) & 0xFFu); }
static unsigned cdec(unsigned c, unsigned n) { return (c & 0xFF00u) | ((c - n) & 0xFFu); }

/* $26C8 (without the slow print); col / row are left as d0 / d1 */
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
        break;
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
static unsigned dig32(MainUI *m, unsigned col, unsigned row, uint32_t *v, uint32_t p)  /* $32D4 */
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
/* $3216: 5 columns, padded, signed compares */
static unsigned pad32(MainUI *m, unsigned col, unsigned row, uint32_t v, int cols)
{
    static const uint32_t pw[8] = {10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    int k = 8 - cols;
    for (; k < 7; k++) {
        if ((int32_t)v >= (int32_t)pw[k]) break;
        col = put(m, col, row, 0x20);
    }
    for (; k < 8; k++) col = dig32(m, col, row, &v, pw[k]);
    return col;
}
static unsigned dig16(MainUI *m, unsigned col, unsigned row, uint32_t *v, uint32_t p)  /* $3346 */
{
    uint32_t d = *v & 0xFFFFu;
    unsigned ch = 0x2F;
    do { ch++; d -= p; } while ((int32_t)d >= 0);
    *v = d + p;
    return put(m, col, row, ch);
}
/* $32FE (4 columns), $330C (3), $331A (2) */
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
static unsigned suffix(MainUI *m, unsigned col, unsigned row, unsigned n)  /* $603C */
{
    uint32_t a = 0xE0C;
    uint8_t b = (uint8_t)n;
    if (b != 11 && b != 12 && b != 13) {
        unsigned r = (uint16_t)n % 10u;
        a = r == 1 ? 0xE03 : r == 2 ? 0xE06 : r == 3 ? 0xE09 : 0xE0C;
    }
    return text(m, col, row, a);
}

/* $95CC / $9644: the "INCOMING MESSAGE" banner (as in scr_troops.c) */
static void banner(Game *g, int k)
{
    MainUI *m = (MainUI *)g->user;
    if (!m) return;
    uint32_t t = 0x59BCEu + ((uint8_t)(g8(g, 0xE425) << 2));
    unsigned col = ui_m16(m, t), row = ui_m16(m, t + 2);
    if (k >= 0) ui_draw_char(m->s, cinc(col, (unsigned)k), row, g->banner[k]);
    else if (g8(g, 0xE426) & 1) text(m, col, row, 0xE20);
    else ui_draw_string(m->s, col, row, g->banner, 0);
}

static const uint8_t *text_at(void *user, uint32_t a) { return ui_text((MainUI *)user, a); }

/* ---- the screen ---- */
enum { AFTER_LOAD = 1, AFTER_HOTSPOT = 2 };
static struct {
    int left;                                   /* hold frames still to run */
    int resume;                                 /* AFTER_LOAD / AFTER_HOTSPOT */
} hold;
static uint32_t aboard[NUM_PLATOONS];           /* $59B3E (4 used) */

UI_API void scr_combat_set_aboard(const uint8_t *be16)
{
    for (int i = 0; i < 4; i++)
        aboard[i] = (uint32_t)be16[4 * i] << 24 | (uint32_t)be16[4 * i + 1] << 16
                  | (uint32_t)be16[4 * i + 2] << 8 | be16[4 * i + 3];
}
UI_API void scr_combat_get_aboard(uint8_t *be16)
{
    for (int i = 0; i < 4; i++) {
        be16[4 * i] = (uint8_t)(aboard[i] >> 24);
        be16[4 * i + 1] = (uint8_t)(aboard[i] >> 16);
        be16[4 * i + 2] = (uint8_t)(aboard[i] >> 8);
        be16[4 * i + 3] = (uint8_t)aboard[i];
    }
}

static uint32_t shown(MainUI *m) { return g32(m->g, 0xE3D4); }

static void bay_name(MainUI *m, unsigned col, unsigned row, uint8_t n)   /* $8704 */
{
    text(m, col, row, n ? BAY_TABLE + (uint32_t)(uint8_t)(n - 1) * BAY_SIZE : 0x1090u);
}

static void orbit_slots(MainUI *m)              /* $86C4 */
{
    uint32_t a = shown(m);
    bay_name(m, 5, 0x29, ui_m8(m, a + 0x18));
    bay_name(m, 5, 0x34, ui_m8(m, a + 0x1C));
    bay_name(m, 5, 0x3F, ui_m8(m, a + 0x20));
}
static void orbit_hook(Game *g)
{
    if (g->user) orbit_slots((MainUI *)g->user);
}

/* one platoon line of the two lists: number, suffix, troops */
static unsigned platoon_line(MainUI *m, unsigned col, unsigned row, uint32_t p)
{
    unsigned n = (unsigned)(uint8_t)((p - PLATOON_TABLE) / PLATOON_SIZE + 1);
    col = pad16(m, col, row, n, 2);
    col = suffix(m, col, row, n);
    col = cinc(col, 1);
    return pad16(m, col, row, ui_m16(m, p), 3);
}
static unsigned blank_line(MainUI *m, unsigned col, unsigned row)
{
    col = text(m, col, row, 0xE17);
    col = cinc(col, 1);
    return text(m, col, row, 0xE18);
}

static void aboard_list(MainUI *m)              /* $33EC */
{
    Game *g = m->g;
    for (int i = 0; i < 4; i++) aboard[i] = 0;
    uint32_t craft = g32(g, 0xE3D8);
    unsigned col = 0x14, row = 0x1F, n = 0;
    uint8_t d5 = 0;
    for (int k = 0; k < NUM_PLATOONS; k++) {
        uint32_t p = PLATOON_TABLE + (uint32_t)k * PLATOON_SIZE;
        if (g32(g, p + 2) != craft || !(g8(g, p + 6) & 2)) continue;
        if (n < NUM_PLATOONS) aboard[n] = p;
        n++;
        col = platoon_line(m, col, row, p);
        col = cdec(col, 8);
        row = (row & 0xFF00u) | ((row + 10) & 0xFFu);
        d5++;
    }
    for (uint8_t d4 = (uint8_t)(4 - d5); d4; d4--) {
        col = blank_line(m, col, row);
        col = cdec(col, 8);
        row = (row & 0xFF00u) | ((row + 10) & 0xFFu);
    }
}

static void planet_list(MainUI *m)              /* $372E */
{
    Game *g = m->g;
    for (int i = 0; i < NUM_PLATOONS; i++) s32(g, 0xEC74 + 4u * i, 0);
    uint32_t a = shown(m), w = 0xEC74;
    unsigned col = 3, row = 0x74;
    uint8_t d5 = 0;
    for (int k = 0; k < NUM_PLATOONS; k++) {
        uint32_t p = PLATOON_TABLE + (uint32_t)k * PLATOON_SIZE;
        if (g32(g, p + 2) != a || (g8(g, p + 6) & 8)) continue;
        if (inblk(w, 4)) s32(g, w, p);
        w += 4;
        col = platoon_line(m, col, row, p);
        col = cdec(col, 8);
        row = (row & 0xFF00u) | ((row + 10) & 0xFFu);
        d5++;
        if ((uint8_t)row == 0xC4) { row = 0x74; col = cinc(col, 0x0A); }
    }
    for (uint8_t d4 = (uint8_t)(0x18 - d5); d4; d4--) {
        col = blank_line(m, col, row);
        col = cdec(col, 8);
        row = (row & 0xFF00u) | ((row + 10) & 0xFFu);
        if ((uint8_t)row == 0xC4) { row = 0x74; col = cinc(col, 0x0A); }
    }
}

static void class_name(MainUI *m, uint32_t bay)   /* $388C */
{
    uint32_t a = (uint32_t)(uint8_t)(ui_m8(m, bay + 0x1E) - 1) * 0x16u + 0x1E76u;
    text(m, 6, 0x50, a);
}

/* $3814 with the text at (col, $59): print, $3006, then the hold */
static void hold_msg(MainUI *m, uint32_t msg, unsigned col, int resume)
{
    text(m, col, 0x59, msg);
    s8(m->g, 0xE42D, 0xFF);
    hold.left = 44;
    hold.resume = resume;
}

static void select_bay(MainUI *m, uint32_t bay)   /* $3860 */
{
    s32(m->g, 0xE3D8, bay);
    text(m, 0x14, 0x0E, bay);
    s8(m->g, 0xE434, 0xFF);
    class_name(m, g32(m->g, 0xE3D8));           /* $3886 */
    aboard_list(m);
    orbit_slots(m);
}

static void slot(MainUI *m, uint32_t a)         /* $3808 */
{
    uint8_t n = ui_m8(m, a);
    if (!n) { hold_msg(m, 0xF13, 0x0B, AFTER_HOTSPOT); return; }     /* "THAT'S EMPTY!" */
    uint32_t bay = BAY_TABLE + (uint32_t)(uint8_t)(n - 1) * BAY_SIZE;
    if (ui_m8(m, bay + 0x1E) != 1) {
        class_name(m, bay);                     /* the $3854 patch: the clicked ship */
        hold_msg(m, 0x1A65, 8, AFTER_HOTSPOT); /* "CAN'T CARRY TROOPS!" */
        return;
    }
    select_bay(m, bay);
}

static void auto_select(MainUI *m)              /* $33D4 */
{
    uint32_t a = shown(m);
    for (int i = 0; i < 3; i++) {
        uint8_t n = ui_m8(m, a + 0x18 + 4u * i);
        if (!n) continue;
        uint32_t bay = 0xE62Cu + (uint32_t)n * BAY_SIZE;
        if (ui_m8(m, bay + 0x1E) == 1) { select_bay(m, bay); return; }
    }
}

static void unload(MainUI *m, int i)            /* $6D74.. */
{
    uint32_t p = aboard[i];
    if (!p) { hold_msg(m, 0xF13, 0x0B, AFTER_HOTSPOT); return; }
    if (inblk(p, PLATOON_SIZE)) {
        s32(m->g, p + 2, g32(m->g, 0xE3DC));
        s8(m->g, p + 6, (uint8_t)(g8(m->g, p + 6) & 0xFB));
    }
    aboard_list(m);
    planet_list(m);
}

static void bars(MainUI *m)                     /* $3568 / $6D42 */
{
    Game *g = m->g;
    uint32_t a = g32(g, 0xE3DC);
    uint16_t sc = g16(g, 0xE406);
    if (!sc) return;                            /* divu by 0 (never: 1..350) */
    uint16_t v[3];
    v[0] = (uint16_t)(ui_m16(m, a + 0x22) / sc);
    v[1] = v[2] = (uint16_t)(ui_m16(m, a + 0x26) / sc);
    static const unsigned off[3] = {0xB2, 0xB6, 0x16};
    for (int b = 0; b < 3; b++) {
        uint16_t d3 = v[b], d4 = 0xC2;
        if (d4 < d3) d3 = d4;
        d4 = (uint16_t)(d4 - d3);
        uint32_t o = 4u * SCREEN_LINE + off[b];
        for (; d4; d4--, o += SCREEN_LINE) {
            if (o + 1 >= SCREEN_SIZE) continue;
            m->s->bpl[o] &= 0x60;
            m->s->bpl[o + 1] = 0;
        }
        for (; d3; d3--, o += SCREEN_LINE) {
            if (o + 1 >= SCREEN_SIZE) continue;
            m->s->bpl[o] |= 0x9F;
            m->s->bpl[o + 1] |= 0xE9;
        }
    }
}

static void scale_text(MainUI *m)               /* $96D8 */
{
    uint32_t d3 = (uint32_t)g16(m->g, 0xE406) * 10u / 0x23u;
    unsigned col = pad16(m, 0x44, 0x69, d3 & 0xFF, 3);
    text(m, col, 0x69, 0x1844);
}
static void scale_step(MainUI *m, int up)       /* $96BE / $9702 */
{
    Game *g = m->g;
    uint16_t v = g16(g, 0xE406);
    if (up ? v == 0x15E : v == 1) return;
    s16(g, 0xE406, (uint16_t)(up ? v + 1 : v - 1));
    bars(m);
    s8(g, 0xE422, 0);
    scale_text(m);
}
static void scale_fit(MainUI *m)                /* $9714 */
{
    uint32_t a = g32(m->g, 0xE3DC);
    uint16_t d0 = ui_m16(m, a + 0x22), d1 = ui_m16(m, a + 0x26);
    if (d0 < d1) d0 = d1;
    uint16_t q = (uint16_t)(d0 / 0xAC);
    s16(m->g, 0xE406, q ? q : 1);
    scale_text(m);
}

static void aggression_pictures(MainUI *m)      /* $393E */
{
    uint8_t v = ui_m8(m, shown(m) + 0x29);
    if (v == 0) return;
    if (v != 1) {
        if (v != 2) ui_xor_delta(m, 0x2413C);
        ui_xor_delta(m, 0x23D22);
    }
    ui_xor_delta(m, 0x23A00);
}

static void aggression(MainUI *m, int up)       /* $3974 / $39D0 */
{
    uint32_t a = shown(m);
    uint8_t v = ui_m8(m, a + 0x29);
    if (up ? v == 3 : v == 0) return;
    v = (uint8_t)(up ? v + 1 : v - 1);
    if (inblk(a, PLANET_SIZE)) s8(m->g, a + 0x29, v);
    int lv = up ? v : v + 1;
    ui_xor_delta(m, lv == 1 ? 0x23A00 : lv == 2 ? 0x23D22 : 0x2413C);
    ui_sound(m, lv == 1 ? 0x1A : lv == 2 ? 0x1B : 0x1C);
}

static void cvr(MainUI *m)                      /* $464A */
{
    if (g32(m->g, 0xE3F0)) return;
    s8(m->g, 0xE449, 0xFF);
    ui_clear_rect(m->s, 0x1C, 6, 3, 0x41);
    text(m, 0x39, 0x39, 0x26A2);
}

/* $245E from pass $F9 (the core has tested $E449, $E425 = 7, $E3F0 = 0) */
static void war_scene(Game *g)
{
    MainUI *m = (MainUI *)g->user;
    if (!m) return;
    ui_clear_rect(m->s, 0x1C, 6, 3, 0x41);
    text(m, 0x38, 0x1E, 0x265A);
    uint32_t a = shown(m);
    uint16_t d0;
    if (ui_m16(m, a + 0x2E)) {
        if (!ui_m16(m, a + 0x26)) d0 = 1;
        else {
            uint16_t e = g16(g, 0xE404);
            d0 = (uint16_t)((e & 0xFF00u) | ((e + 1) & 0xFFu));
            if ((uint8_t)d0 == 5) d0 = 2;
            s16(g, 0xE404, d0);
        }
    } else if (ui_m16(m, a + 0x26)) {
        d0 = 0;
    } else if (ui_m16(m, a + 0x0C) == 7) {
        d0 = 0;
    } else if (ui_m16(m, a + 0x0C) == 10) {
        d0 = 1;
    } else {
        text(m, 0x38, 0x1E, 0x2678);
        return;
    }
    text(m, 0x38, 0x1E, 0x2697);
    text(m, 0x39, 0x39, 0x26B0);
    uint32_t e = 0x25C4u + (uint32_t)d0 * 0x1Eu;
    s16(g, 0xE3F8, 0);
    s8(g, 0xE42E, 0);
    s32(g, 0xE3F0, ui_m32(m, e));
    for (int i = 0; i < 12; i++) m->colour[4 + i] = ui_m16(m, e + 4 + 2u * i);
    s8(g, 0xE44E, ui_m8(m, e + 0x1C));
    ui_viewer_step(m);
    s8(g, 0xE449, 0);
}

static void redraws(MainUI *m)                  /* $38CC */
{
    Game *g = m->g;
    uint32_t d7 = g32(g, G_REDRAW);
    if (d7 & 0x10) {
        s32(g, G_REDRAW, g32(g, G_REDRAW) & ~0x10u);
        uint32_t a = shown(m);
        pad32(m, 0x49, 0x4E, ui_m16(m, a + 0x22), 5);
        pad32(m, 0x49, 0x55, ui_m16(m, a + 0x26), 5);
        pad16(m, 0x47, 0x5F, ui_m16(m, a + 0x2E), 4);
    } else if (d7 & 0x80000) {
        s32(g, G_REDRAW, g32(g, G_REDRAW) & ~0x80000u);
        planet_list(m);
    }
}

/* $8198: a click on the platoon grid loads that platoon onto $E3D8.
 * Returns 1 if it started a hold. */
static int grid(MainUI *m, const Mouse *ms)
{
    Game *g = m->g;
    if (!ms->button) { s8(g, 0xE422, 0); return 0; }
    if (g8(g, 0xE422)) return 0;
    s8(g, 0xE422, 0xFF);
    uint16_t x = (uint16_t)ms->x, y = (uint16_t)ms->y;
    if ((uint8_t)y < 0x71 || x < 9) { s8(g, 0xE422, 0); return 0; }
    uint16_t c = (uint16_t)((uint16_t)(x - 9) / 0x28);
    if (c >= 3) { s8(g, 0xE422, 0); return 0; }
    uint32_t rq = (uint32_t)(uint16_t)(y - 0x71) / 10u;
    if (rq > 0xFFFF) rq &= 0xFFFF;              /* never: y is a word */
    uint16_t r = (uint16_t)rq;
    if (r >= 8) { s8(g, 0xE422, 0); return 0; }
    uint32_t e = 0xEC74u + (uint8_t)((((uint32_t)c * 8u + r) & 0x1Fu) << 2);
    uint32_t p = g32(g, e);
    if (!p) { hold_msg(m, 0xF13, 0x0B, AFTER_LOAD); return 1; }        /* "THAT'S EMPTY!" */
    if (!g32(g, 0xE3D8)) { hold_msg(m, 0xF40, 0x0A, AFTER_LOAD); return 1; }   /* "SELECT A SHIP!" */
    if (aboard[3]) { hold_msg(m, 0xF79, 0x0A, AFTER_LOAD); return 1; }  /* "SHIP IS FULL!" */
    if (inblk(p, PLATOON_SIZE)) {
        s32(g, p + 2, g32(g, 0xE3D8));
        s8(g, p + 6, (uint8_t)(g8(g, p + 6) | 4));
    }
    s8(g, 0xD45A, 0xFF);
    aboard_list(m);
    planet_list(m);
    return 0;
}

static void sel_3534(MainUI *m)                 /* $3534: $E3DC = $E3D4, the main-screen selection */
{
    Game *g = m->g;
    uint32_t a = g32(g, 0xE3D4);
    if (!a) return;
    uint16_t idx = (uint16_t)(g32(g, 0xE3BC) * 0x3Au);           /* $6E5C */
    uint32_t r = g32(g, G_PLANET_TABLE) + (uint32_t)(int16_t)idx + 5;
    if (inblk(r, 1)) s8(g, r, 0);
    s32(g, 0xE3DC, a);
    uint32_t d0 = a - g32(g, G_PLANET_TABLE);
    d0 = (d0 / 0x3Au > 0xFFFF) ? d0 : (uint32_t)(((d0 % 0x3Au) << 16) | (d0 / 0x3Au));   /* divu.w */
    s32(g, 0xE3BC, d0 & 0xFF);
    ui_mark_selected(m);
}

static void hooks(MainUI *m)
{
    static const CombatHooks h = {NULL, orbit_hook};
    m->g->user = m;
    m->g->banner_hook = banner;
    m->g->war_scene_hook = war_scene;
    combat_set_hooks(&h);
}

/* The rest of the main screen's iteration after its hotspot handler
 * returned ($479E..$47C6), for the refusal that returns into that loop. */
static int main_rest(MainUI *m, uint32_t *next)
{
    Game *g = m->g;
    uint32_t st = 0, arrow = 0;
    int r = ui_game_pass(m, &st);
    if (r != UI_OK) { *next = st; return r; }
    ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);
    ui_mark_selected(m);
    /* $4B88: the click was on the combat icon, outside the planet list */
    ui_message_pass(m->s, g, text_at, m);
    if (!g8(g, 0xE429)) ui_message_type(m->s, g, text_at, m);
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    m->colour[31] = g16(g, 0xE3FE);
    return UI_OK;
}

static int enters(MainUI *m, uint32_t a)        /* the common gate: in use or troops */
{
    return (ui_m8(m, a + 0x18) | ui_m8(m, a + 0x1C) | ui_m8(m, a + 0x20)) || ui_m16(m, a + 0x2E);
}

static void body(MainUI *m)                     /* $3A76 */
{
    Game *g = m->g;
    ui_sound(m, 0x1D);
    s32(g, 0xE3D4, g32(g, 0xE3DC));
    ui_show_picture(m, COMBAT_PICTURE);
    s32(g, 0xE3D8, 0);
    s32(g, G_REDRAW, 0);
    s16(g, 0xE3F8, 0);
    s32(g, 0xE3F0, 0);
    cvr(m);
    s8(g, 0xE425, 7);
    s32(g, 0xE3EC, COMBAT_TABLE);
    s16(g, 0xE400, 0x12);
    aggression_pictures(m);
    orbit_slots(m);
    planet_list(m);
    text(m, 3, 0x0E, shown(m) + P_NAME);
    scale_fit(m);
    text(m, 0x34, 0x4E, 0x1883);
    ground_forces_entry(g);                     /* $91E2 */
    auto_select(m);
}

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    Game *g = m->g;
    hooks(m);
    hold.left = 0;
    uint32_t a;
    if (handler == 0x3A2A) {
        ui_panel_save(m);                       /* $47DC */
        a = g32(g, 0xE3DC);
        uint16_t o = ui_m16(m, a + P_OWNER);
        if (o == OWNER_PLAYER) goto in;
        if (o != 4 && o != 1) {
            if (enters(m, a)) goto in;
            if (o == 7) goto denied;
        }
        post_message(g, 0x1038);                /* "THIS PLANET NEEDS FORMATTING!" */
        m->screen = SCR_MAIN;                   /* back into the main loop */
        return main_rest(m, next);
    }
    if (handler == 0x44D2) {
        s32(g, 0xE3F0, 0);
        uint32_t loc = ui_m32(m, g32(g, 0xE3A4) + 2);
        uint32_t end = (uint32_t)g16(g, 0xE410) * 0x3Au + g32(g, G_PLANET_TABLE) + 0x3A;
        if (loc < g32(g, G_PLANET_TABLE) || loc >= end) goto gate;
        s32(g, 0xE3D4, loc);
    }
    s32(g, 0xE3F0, 0);                          /* $4500 */
    sel_3534(m);
gate:                                           /* $450A */
    a = g32(g, 0xE3D4);
    if (!a) a = g32(g, 0xE3DC);
    {
        uint16_t o = ui_m16(m, a + P_OWNER);
        if (o == OWNER_PLAYER) goto in;
        if (o != 4 && o != 1 && enters(m, a)) goto in;
    }
denied:                                         /* $8F0E, then $60A6 */
    post_message(g, 0x1919);
    return UI_EXIT_TO_MAIN;
in:
    body(m);
    return UI_OK;
}

/* the switches of the left column (boxes 9-13) */
static int to_screen(MainUI *m, uint32_t h, uint32_t *next)
{
    if (h == 0x461A) {                          /* troops: $5CC6, no panel save */
        m->screen = SCR_TROOPS;
        return scr_troops.enter(m, 0x461A, next);
    }
    *next = h;                                  /* $4620 fleet, $4614 statistics, $8EFC cargo, $59B08 surface */
    return UI_GOTO;
}

static int handler(MainUI *m, uint32_t h, uint32_t *next)
{
    switch (h) {
    case 0x37FC: slot(m, g32(m->g, 0xE3DC) + 0x18); break;
    case 0x38AC: slot(m, shown(m) + 0x1C); break;
    case 0x38BC: slot(m, shown(m) + 0x20); break;
    case 0x6D8C: unload(m, 0); break;
    case 0x6D84: unload(m, 1); break;
    case 0x6D7C: unload(m, 2); break;
    case 0x6D74: unload(m, 3); break;
    case 0x96BE: scale_step(m, 0); break;
    case 0x9702: scale_step(m, 1); break;
    case 0x9714: scale_fit(m); break;
    case 0x3974: aggression(m, 1); break;
    case 0x39D0: aggression(m, 0); break;
    case 0x464A: cvr(m); break;
    case 0x461A: case 0x4620: case 0x4614: case 0x8EFC: case 0x59B08:
        return to_screen(m, h, next);
    default: *next = h; return UI_GOTO;
    }
    return UI_OK;
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    int resume = 0;
    hooks(m);
    if (hold.left) {                            /* $31B8 */
        s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        ui_viewer_tick(m);
        if (--hold.left) return UI_PAUSED;
        s8(g, 0xE42D, 0);                       /* $301C */
        text(m, 7, 0x59, 0xE1C);
        resume = hold.resume;
        if (resume == AFTER_HOTSPOT) goto game_pass;
        goto after_load;
    }
    ui_viewer_tick(m);
    redraws(m);
    if (grid(m, ms)) return UI_PAUSED;
after_load:
    bars(m);
    {
        uint32_t h = ui_hotspots(m, ms);
        if (h) {
            int r = handler(m, h, next);
            if (r != UI_OK) return r;
            if (m->screen != SCR_COMBAT) return UI_OK;
            if (hold.left) return UI_PAUSED;
        }
    }
game_pass:
    {
        uint32_t st = 0;
        int r = ui_game_pass(m, &st);
        if (r != UI_OK) { *next = st; return r; }
    }
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    if (ms->rbutton) {
        ui_sound(m, 13);
        return UI_EXIT_TO_MAIN;                 /* $60A6 */
    }
    return UI_OK;
}

const ScreenDef scr_combat = {SCR_COMBAT, 0x3A2A, enter, frame};

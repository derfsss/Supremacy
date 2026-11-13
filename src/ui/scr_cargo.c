/*
 * Screen: cargo bays ($7874, table $F0FC), translated routine by routine
 * onto the screen buffer and the state block. Addresses are the original's.
 * Checked frame by frame against the original game running in an emulator.
 *
 * Entries (enter's `handler`):
 *   $7874  main-screen box 10: owner test, panel save ($47DC), then $788E;
 *   $8EF8  left-column icon on other screens: $3534 (select $E3D4), then $8EFC;
 *   $8EFC  the same without $3534: owner test, then $788E (no panel save);
 *   $8F1A  fleet screen "cargo with this ship" (not in ui.c's table yet).
 *
 * One frame() = one iteration of the loop at $7904 (after its $2FFA):
 *   blink "SELECT A BAY!" while no ship is picked ($E434 = 0) or one
 *   redraw by the $E39C bits, $306A stock bars, $71E0 game pass, $9B66
 *   pointer (platform), $9A6C hotspots + handler, $E42C + 1, right button.
 *
 * Handlers that run frames of their own in the original are state machines
 * across frame() calls; frame() returns UI_PAUSED while one is running:
 *   - message hold $31A6 (44 frames of $31B8: $561C, [$FFC6 on the surface
 *     screen], $E42C + 1, $9B66, $4ED8). The click frame returns UI_PAUSED
 *     after its handler; hold frames 1..43 return UI_PAUSED; hold frame 44
 *     runs the handler's tail and the rest of the click frame's iteration
 *     ($E42C + 1, right button). Used by "THAT'S EMPTY!" ($7CD8) and the
 *     crew messages ($7BAA).
 *   - scrap ($0BDC): "ARE YOU SURE? (TYPE Y / N)", then the original spins on
 *     the key table with no frame wait. The port waits in frame() (UI_PAUSED)
 *     until scr_cargo_key() delivers a key: Y scraps and re-enters the screen
 *     at $788E (that iteration ends there: the return address was dropped),
 *     any other key blanks the line, unlocks and finishes the iteration.
 * The load/unload/fuel boxes repeat while the button is held because their
 * handlers clear $E422 (no state machine needed). Unload all ($7BF4) loops
 * without a frame wait, so it runs inside one frame().
 *
 * The helpers cs_* are shared with scr_surface.c (prototypes there).
 */
#include <ctype.h>

#include "ui.h"
#include "day.h"
#include "craft.h"
#include "gamedata.h"

#define CARGO_PICTURE  0x169C0u
#define CARGO_TABLE    0xF0FCu
#define BLANK_PIC      0x5AFC1u          /* $9B2C source of an empty bay */
#define CRAFT_PICS     0x59BEEu          /* (type - 1) * $2D5 */
#define CARGO_PIC_AT   0x77DC1u

/* ------------------------------------------------------------ shared helpers */

uint8_t *cs_scr(MainUI *m, uint32_t a)
{
    static uint8_t sink;
    return (a >= SCREEN_BASE && a < SCREEN_BASE + SCREEN_SIZE) ? &m->s->bpl[a - SCREEN_BASE] : &sink;
}

static unsigned colb(unsigned col, unsigned n) { return (col & 0xFF00u) | ((col + n) & 0xFFu); }

/* $26C8 with its registers: d0 (*col) and d1 (*row) are left where the
 * string ends, as the callers that go on drawing need. */
void cs_str(MainUI *m, unsigned *col, unsigned *row, uint32_t addr)
{
    const uint8_t *a = ui_text(m, addr);
    if (!a) return;
    unsigned c0 = *col, r0 = *row;
    int typing = g8(m->g, 0xE43F);
    for (;;) {
        unsigned c = *a++;
        if (c == 0xFF) break;
        if (c >= 0x20) {
            ui_draw_char(m->s, c0, r0, c);
            if (typing && m->s->char_hook) m->s->char_hook(m->s->hook_user);
            c0 = colb(c0, 1);
            continue;
        }
        if (c == 1) { r0 = (r0 & 0xFF00u) | ((r0 + *a++) & 0xFFu); continue; }
        if (c == 2) { c0 = colb(c0, *a++); continue; }
        if (c == 3 || c == 4) {
            unsigned n = (unsigned)((*a++ - 1) & 0xFF) + 1, ch = *a++;
            for (unsigned k = 0; k < n; k++) {
                ui_draw_char(m->s, c0, r0, ch);
                if (c == 3) c0 = colb(c0, 1);
                else r0 = (r0 + 6) & 0xFFFFu;
            }
            continue;
        }
        break;                                       /* not used by the game */
    }
    *col = c0;
    *row = r0;
}

void cs_str_at(MainUI *m, unsigned col, unsigned row, uint32_t addr)
{
    cs_str(m, &col, &row, addr);
}

/* $3346: one digit by repeated subtraction (d2 from '/'), d0.b + 1 */
static unsigned digit(MainUI *m, unsigned col, unsigned row, uint32_t *v, uint32_t p)
{
    uint32_t d3 = *v & 0xFFFFu;
    unsigned ch = 0x2F;
    do { ch = (ch + 1) & 0xFFFFu; d3 -= p; } while ((int32_t)d3 >= 0);
    d3 += p;
    *v = d3;
    ui_draw_char(m->s, col, row, ch);
    return colb(col, 1);
}

/* $32F0 (width 5), $32FE (4), $330C (3), $331A (2): a 16-bit number padded
 * with spaces on the left (signed compares, as the original). */
void cs_num(MainUI *m, unsigned *col, unsigned row, uint16_t v, int width)
{
    static const uint32_t pw[5] = {10000, 1000, 100, 10, 1};
    uint32_t d3 = v;
    int k = 5 - width;
    for (; k < 4; k++) {
        if ((int16_t)v >= (int16_t)pw[k]) break;
        ui_draw_char(m->s, *col, row, 0x20);
        *col = colb(*col, 1);
    }
    for (; k < 5; k++) *col = digit(m, *col, row, &d3, pw[k]);
}

static void num_at(MainUI *m, unsigned col, unsigned row, uint16_t v, int width)
{
    cs_num(m, &col, row, v, width);
}

/* The 8 craft icons at $59BEE ($2D5 bytes each; the 8th, $5AFC1, is the
 * empty bay), copied there at boot ($6A68; gamedata_boot_icons). */

/* $9B2C: a 40x29 craft picture (5 bytes per plane, planes interleaved by
 * byte in the source) to the screen at `dst`. */
void cs_blit(MainUI *m, uint32_t dst, uint32_t src)
{
    for (int y = 0; y < 29; y++) {
        for (int x = 0; x < 5; x++)
            for (int pl = 0; pl < 5; pl++, src++)
                *cs_scr(m, dst + (uint32_t)pl * 40u + (uint32_t)x) = ui_m8(m, src);
        dst += SCREEN_LINE;
    }
}

/* $8704: a dock slot's craft name ("BAY EMPTY" $1090 when 0) */
void cs_slot_name(MainUI *m, unsigned col, unsigned row, uint8_t slot)
{
    uint32_t a = slot ? BAY_TABLE + (uint32_t)(uint8_t)(slot - 1) * BAY_SIZE : 0x1090u;
    cs_str(m, &col, &row, a);
}

/* $3534: make the screen's planet $E3D4 the main-screen planet */
void cs_select_e3d4(MainUI *m)
{
    Game *g = m->g;
    uint32_t p = g32(g, 0xE3D4);
    if (!p) return;
    uint16_t idx = (uint16_t)(g32(g, 0xE3BC) * 0x3Au);           /* $6E5C */
    s8(g, g32(g, G_PLANET_TABLE) + (uint32_t)(int16_t)idx + 5, 0);
    s32(g, 0xE3DC, p);
    uint32_t d0 = p - g32(g, G_PLANET_TABLE);
    d0 = (uint32_t)(((d0 & 0xFFFFu) / 0x3Au) & 0xFFu);          /* divu.w, low byte */
    s32(g, 0xE3BC, d0);
    ui_mark_selected(m);                                         /* $4B4C */
}

void cs_surface_anim(MainUI *m);                                 /* $FFC6, scr_surface.c */

/* $31B8 after its $2FFA: one frame of the $31A6 hold */
void cs_hold_frame(MainUI *m)
{
    Game *g = m->g;
    /* $561C: the shipyard starfield, only with $E432 set and $E425 = 5 */
    if (g8(g, 0xE425) == 6) cs_surface_anim(m);
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    /* $9B66: pointer (platform) */
    ui_viewer_tick(m);                                           /* $4ED8 */
}

/* ------------------------------------------------------------ the screen */

enum { TAIL_NONE, TAIL_EMPTY, TAIL_CREW };

static int hold_n;           /* frames of the $31A6 hold still to run */
static int hold_tail;        /* what the handler does after it */
static int wait_key;         /* scrap: waiting for the answer */
static int key_in;           /* the key delivered by the platform (ASCII) */

UI_API void scr_cargo_key(int ch) { key_in = ch ? ch : -1; }
UI_API int  scr_cargo_wants_key(void) { return wait_key; }

static uint32_t bay_of(MainUI *m) { return g32(m->g, 0xE3D8); }
static uint32_t planet(MainUI *m) { return g32(m->g, 0xE3DC); }

static void message(MainUI *m, uint32_t a) { cs_str_at(m, 0x0A, 0x52, a); }     /* $7868 */

static void lock(MainUI *m) { s8(m->g, 0xE42D, 0xFF); }                          /* $3006 */
static void unlock(MainUI *m) { s8(m->g, 0xE42D, 0); }                           /* $301C */

static void planet_name(MainUI *m)                                               /* $7A00 */
{
    cs_str_at(m, 3, 0x0E, planet(m) + P_NAME);
}

static void dock_slots(MainUI *m)                                                /* $86C4 */
{
    uint32_t p = g32(m->g, 0xE3D4);
    cs_slot_name(m, 5, 0x29, g8(m->g, p + 0x18));
    cs_slot_name(m, 5, 0x34, g8(m->g, p + 0x1C));
    cs_slot_name(m, 5, 0x3F, g8(m->g, p + 0x20));
}

static void draw_value(MainUI *m)                                                /* $7E60 */
{
    if (!bay_of(m)) return;
    num_at(m, 0x16, 0x40, g16(m->g, bay_of(m) + B_SCRAP), 5);
}

static void draw_people(MainUI *m)                                               /* $7E80 */
{
    num_at(m, 0x16, 0x10, g16(m->g, planet(m) + P_POP), 5);
    num_at(m, 0x11, 0x7A, g16(m->g, bay_of(m) + B_CIVIL), 4);
}

static void draw_tank(MainUI *m)                                                 /* $7EAC */
{
    unsigned col = 0x19, row = 0x7A;
    if (g16(m->g, craft_template(m->g, bay_of(m)) + T_FUEL) == 0xFFFF) {
        cs_str(m, &col, &row, 0x1B1D);                                           /* NUCLEAR */
        return;
    }
    cs_str(m, &col, &row, 0x0E19);
    cs_num(m, &col, row, g16(m->g, bay_of(m) + B_TANK), 4);
    cs_str(m, &col, &row, 0x0E1A);
}

static void draw_held(MainUI *m)                                                 /* $7EE6 */
{
    uint32_t b = bay_of(m);
    uint16_t v = (uint16_t)(g16(m->g, b + B_FOOD) + g16(m->g, b + B_MINERALS) +
                            g16(m->g, b + B_FUELS) + g16(m->g, b + B_ENERGY));
    num_at(m, 0x16, 0x38, v, 5);
}

static void draw_pair(MainUI *m, unsigned col, uint32_t pa, uint32_t ba)         /* $7F0A */
{
    unsigned row = 0xB4;
    cs_num(m, &col, row, g16(m->g, pa), 5);
    col = (col & 0xFF00u) | ((col - 5) & 0xFFu);
    row = (row & 0xFF00u) | ((row + 10) & 0xFFu);
    cs_num(m, &col, row, g16(m->g, ba), 5);
}

static void draw_food(MainUI *m)                                                 /* $7F92 */
{
    draw_pair(m, 0x30, planet(m) + P_FOOD, bay_of(m) + B_FOOD);
    draw_held(m);
}
static void draw_minerals(MainUI *m)                                             /* $7FE8 */
{
    draw_pair(m, 0x38, planet(m) + P_MINERALS, bay_of(m) + B_MINERALS);
    draw_held(m);
}
static void draw_fuels(MainUI *m)                                                /* $803E */
{
    draw_pair(m, 0x40, planet(m) + P_FUELS, bay_of(m) + B_FUELS);
    draw_tank(m);
    draw_held(m);
}
static void draw_energy(MainUI *m)                                               /* $809A */
{
    draw_pair(m, 0x48, planet(m) + P_ENERGY, bay_of(m) + B_ENERGY);
    draw_held(m);
}

static void (*const draw_cargo[4])(MainUI *) = {draw_food, draw_minerals, draw_fuels, draw_energy};

/* $6CFE: the top of one stock bar. d0 = byte column, d1 = bottom line,
 * d2 = plane offset, d3 = height (capped by the animation $E41C). */
static void bar(MainUI *m, unsigned d0, unsigned d1, unsigned d2, uint32_t d3)
{
    if ((uint16_t)d3 == 0) {
        uint32_t a = SCREEN_BASE + (uint32_t)(d1 - 2) * SCREEN_LINE + d0 + d2;
        *cs_scr(m, a) = 0;
        return;
    }
    uint16_t d4 = g16(m->g, 0xE41C);
    if ((uint8_t)d4 < (uint8_t)d3) d3 = (d3 & 0xFFFF0000u) | d4;
    int16_t idx = (int16_t)((uint16_t)(d1 << 2) - (uint16_t)(d3 << 3));
    int32_t line = idx / 4;
    uint32_t a = (uint32_t)((int32_t)SCREEN_BASE + line * (int32_t)SCREEN_LINE) + d0 + d2;
    *cs_scr(m, a) = 0xFE;
    *cs_scr(m, a - 0x190) = 0;
}

static void bars(MainUI *m)                                                      /* $306A */
{
    Game *g = m->g;
    if (g16(g, 0xE41C) != 0x49) s16(g, 0xE41C, (uint16_t)(g16(g, 0xE41C) + 1));
    static const uint8_t po[4] = {P_FOOD, P_MINERALS, P_FUELS, P_ENERGY};
    static const uint8_t bo[4] = {B_FOOD, B_MINERALS, B_FUELS, B_ENERGY};
    for (int k = 0; k < 4; k++)
        bar(m, 0x18u + 4u * (unsigned)k, 0x9F, 0x50, (g16(g, planet(m) + po[k]) / 250u) & 0xFFu);
    if (!g8(g, 0xE434)) return;
    for (int k = 0; k < 4; k++)
        bar(m, 0x19u + 4u * (unsigned)k, k == 3 ? 0x9E : 0x9F, 0xA0,
            (g16(g, bay_of(m) + bo[k]) / 25u) & 0xFFu);
}

/* $7D12: show the ship in bay `b` (already in $E3D8 except from $8F1A,
 * where it is $E3D8 too) */
static void show_ship(MainUI *m, uint32_t b)
{
    Game *g = m->g;
    s16(g, 0xE400, 0x17);
    s8(g, 0xE434, 0xFF);
    cs_str_at(m, 0x12, 0x08, b + B_NAME);
    uint32_t t = craft_template(g, bay_of(m));
    unsigned col = 0x14, row = 0x20;
    if (g16(g, t + T_CREW) == 0xFFFE) {
        cs_str(m, &col, &row, 0x183C);                                           /* REMOTE */
    } else {
        cs_str(m, &col, &row, 0x0E17);
        cs_num(m, &col, row, g16(g, t + T_CREW), 3);
    }
    num_at(m, 0x16, 0x28, g16(g, t + T_PAYLOAD), 5);
    draw_value(m);
    draw_people(m);
    num_at(m, 0x17, 0x18, g16(g, t + T_SEATS), 4);
    uint16_t fuel = g16(g, t + T_FUEL);
    num_at(m, 0x17, 0x30, fuel == 0xFFFF ? 0 : fuel, 4);
    uint8_t type = (uint8_t)(g8(g, bay_of(m) + B_TYPE) - 1);
    cs_str_at(m, 0x0D, 0x9D, 0x1E76u + (uint32_t)type * 0x16u);
    cs_blit(m, CARGO_PIC_AT, CRAFT_PICS + (uint32_t)type * 0x2D5u);
    draw_food(m);
    draw_minerals(m);
    draw_fuels(m);
    draw_energy(m);
    draw_held(m);
    message(m, 0x0E35);
    s16(g, 0xE41C, 0);
    for (unsigned c = 0x18; c <= 0x24; c += 4)
        ui_clear_rect(m->s, c, 0x0C, 0, 0x92);                                   /* $336E */
}

/* $7CD8: select the bay in a dock slot (d0 = the slot byte) */
static void select_slot(MainUI *m, uint8_t slot)
{
    if (!slot) {
        message(m, 0x0F13);                                                      /* THAT'S EMPTY! */
        hold_n = 44;
        hold_tail = TAIL_EMPTY;
        return;
    }
    uint32_t b = BAY_TABLE + (uint32_t)(uint8_t)(slot - 1) * BAY_SIZE;
    s32(m->g, 0xE3D8, b);
    ui_sound(m, 13);
    show_ship(m, b);
}

static void autoselect(MainUI *m)                                                /* $56BA */
{
    uint32_t p = planet(m);
    uint8_t a = g8(m->g, p + 0x18), b = g8(m->g, p + 0x1C), c = g8(m->g, p + 0x20);
    if (a) { if (!b && !c) select_slot(m, a); }
    else if (b) { if (!c) select_slot(m, b); }
    else if (c) select_slot(m, c);
}

static void enter_788e(MainUI *m)                                                /* $788E */
{
    Game *g = m->g;
    m->screen = SCR_CARGO;
    s32(g, 0xE3D8, 0);
    s8(g, 0xE434, 0);
    ui_show_picture(m, CARGO_PICTURE);
    ui_sound(m, 0x25);
    s16(g, 0xE400, 8);
    s32(g, 0xE3F0, 0);
    s16(g, 0xE41C, 0);
    s32(g, 0xE3EC, CARGO_TABLE);
    s32(g, 0xE3D4, g32(g, 0xE3DC));
    s8(g, 0xE425, 2);
    planet_name(m);
    dock_slots(m);
    cs_blit(m, CARGO_PIC_AT, BLANK_PIC);
    autoselect(m);
}

static int denied(MainUI *m)                                                     /* $8F0E */
{
    post_message(m->g, 0x1919);
    return UI_EXIT_TO_MAIN;
}

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    Game *g = m->g;
    (void)next;
    hold_n = 0;
    wait_key = 0;
    key_in = 0;
    switch (handler) {
    case 0x7874:                                                 /* main-screen box */
        if (g16(g, planet(m) + P_OWNER) != 10) {
            post_message(g, 0x1919);                             /* bra $4A9E: back to the main loop */
            m->screen = SCR_MAIN;
            return UI_OK;
        }
        ui_panel_save(m);                                        /* $47DC */
        enter_788e(m);
        return UI_OK;
    case 0x8F1A: {                                               /* fleet: this ship */
        uint32_t b = bay_of(m);
        if (b) {
            if (g8(g, b + B_FLAGS) & BF_SURFACE) {
                post_message(g, 0x4714);
                return UI_EXIT_TO_MAIN;
            }
            if (!(g8(g, b + B_FLAGS) & BF_ORBIT)) {
                if (g16(g, craft_location(g, b) + P_OWNER) != 10) return denied(m);
                ui_show_picture(m, CARGO_PICTURE);
                show_ship(m, b);
                cs_select_e3d4(m);
                s32(g, 0xE3F0, 0);                               /* $2C9A */
                s16(g, 0xE41C, 0);
                s32(g, 0xE3EC, CARGO_TABLE);
                s8(g, 0xE425, 2);
                planet_name(m);
                dock_slots(m);
                m->screen = SCR_CARGO;
                return UI_OK;
            }
        }
    }
        /* fall through - to $8EF8 */
    case 0x8EF8:
        cs_select_e3d4(m);
        /* fall through */
    case 0x8EFC:
        if (g16(g, planet(m) + P_OWNER) != 10) return denied(m);
        enter_788e(m);
        return UI_OK;
    }
    return UI_HANDLER;
}

/* $7BAA: a crew message held for 44 frames with the input locked */
static void crew_message(MainUI *m, uint32_t a)
{
    lock(m);
    message(m, a);
    hold_n = 44;
    hold_tail = TAIL_CREW;
}

static void unload_all(MainUI *m)                                                /* $7BF4 */
{
    Game *g = m->g;
    static const uint8_t po[5] = {P_FOOD, P_MINERALS, P_FUELS, P_ENERGY, P_POP};
    static const uint8_t bo[5] = {B_FOOD, B_MINERALS, B_FUELS, B_ENERGY, B_CIVIL};
    lock(m);
    int k = 0;
    for (;;) {
        for (; k < 5; k++) {
            if (g16(g, g32(g, 0xE3D4) + po[k]) >= 30000u) continue;
            if (k < 4) {
                craft_unload(g, bay_of(m), planet(m), k);        /* $80C0.. */
                draw_cargo[k](m);
            } else {
                craft_unload_civilians(g, bay_of(m), planet(m)); /* $7A60 */
                draw_people(m);
            }
        }
        bars(m);
        s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        for (k = 0; k < 5; k++)
            if (g16(g, g32(g, 0xE3D4) + po[k]) < 30000u && g16(g, bay_of(m) + bo[k])) break;
        if (k == 5) break;
    }
    unlock(m);
}

/* fuel ($7AAC fill, $7AA4 empty): craft_fuel does the state; the screen
 * redraws ($803E) only when a step was made, decided as the original does */
static void fuel(MainUI *m, int empty)
{
    Game *g = m->g;
    uint32_t b = bay_of(m);
    int step = 0;
    if (g16(g, craft_template(g, b) + T_FUEL) != 0xFFFF) {
        int reset = (uint8_t)(g8(g, 0xE44A) + 1) != g8(g, 0xE42C);
        step = reset || g8(g, 0xE44C) == 0;
    }
    craft_fuel(g, b, planet(m), empty);
    if (step) draw_fuels(m);
}

/* $0BDC after the answer */
static void scrap_answer(MainUI *m, int key)
{
    Game *g = m->g;
    if (toupper(key) != 'Y') {
        message(m, 0x0E35);
        unlock(m);
        return;
    }
    /* $7BF4 + the scrap: craft_scrap runs the unload loop with the bars'
     * counter and $E42C; the drawing is lost under $788E's picture */
    craft_scrap(g, bay_of(m), planet(m), g32(g, 0xE3D4));
    unlock(m);                                                   /* $7BF4 ends with $301C */
    s32(g, 0xE3D8, 0);
    enter_788e(m);                                               /* drop the return, bra $788E */
}

static int handler(MainUI *m, uint32_t h, uint32_t *next)
{
    Game *g = m->g;
    uint32_t p = planet(m);
    switch (h) {
    case 0x7CCC: select_slot(m, g8(g, p + 0x18)); return UI_OK;
    case 0x7E40: select_slot(m, g8(g, p + 0x1C)); return UI_OK;
    case 0x7E50: select_slot(m, g8(g, p + 0x20)); return UI_OK;
    case 0x59B08:                                                /* surface screen */
        m->screen = SCR_SURFACE;
        return scr_surface.enter(m, h, next);
    case 0x4620: case 0x4614: case 0x460E: case 0x461A:          /* other screens */
        *next = h;
        return UI_GOTO;
    }
    uint32_t b = bay_of(m);
    if (!b) return UI_OK;                                        /* boxes 8-22 need a ship */
    switch (h) {
    case 0x80C0: case 0x80F6: case 0x812C: case 0x8162: {        /* unload */
        int k = (int)(h == 0x80C0 ? 0 : h == 0x80F6 ? 1 : h == 0x812C ? 2 : 3);
        craft_unload(g, b, p, k);
        draw_cargo[k](m);
        return UI_OK;
    }
    case 0x7F64: case 0x7FB8: case 0x800E: case 0x8068: {        /* load */
        int k = (int)(h == 0x7F64 ? 0 : h == 0x7FB8 ? 1 : h == 0x800E ? 2 : 3);
        craft_load(g, b, p, k);
        if (!g8(g, 0xE438)) draw_cargo[k](m);
        return UI_OK;
    }
    case 0x7B88: {                                               /* crew */
        uint32_t t = craft_assign_crew(g, b, p);
        if (t == 0x0FA0) draw_people(m);
        crew_message(m, t);
        return UI_OK;
    }
    case 0x7BF4: unload_all(m); return UI_OK;
    case 0x0BDC:                                                 /* scrap */
        lock(m);
        message(m, 0x185D);
        wait_key = 1;
        key_in = 0;
        return UI_OK;
    case 0x7A18: craft_load_civilians(g, b, p); draw_people(m); return UI_OK;
    case 0x7A60: craft_unload_civilians(g, b, p); draw_people(m); return UI_OK;
    case 0x7AAC: fuel(m, 0); return UI_OK;
    case 0x7AA4: fuel(m, 1); return UI_OK;
    }
    return UI_OK;
}

static void redraw(MainUI *m)                                    /* $7924..$79DC */
{
    Game *g = m->g;
    if (!g8(g, 0xE434)) {
        message(m, (int8_t)(g8(g, 0xE42C) & 7) < 4 ? 0x0F21 : 0x0E35);   /* SELECT A BAY! */
        return;
    }
    uint32_t d7 = g32(g, 0xE39C);
    static const struct { int bit; void (*fn)(MainUI *); } r[] = {
        {11, draw_value}, {5, draw_people}, {14, draw_food},
        {15, draw_minerals}, {16, draw_fuels}, {17, draw_energy},
    };
    for (unsigned k = 0; k < sizeof r / sizeof r[0]; k++)
        if (d7 & (1u << r[k].bit)) {
            r[k].fn(m);
            s32(g, 0xE39C, g32(g, 0xE39C) & ~(1u << r[k].bit));
            return;
        }
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    if (wait_key) {
        if (!key_in) return UI_PAUSED;
        int k = key_in;
        key_in = 0;
        wait_key = 0;
        scrap_answer(m, k);
        if (toupper(k) == 'Y') return UI_OK;
        goto after_handler;
    }
    if (hold_n) {
        cs_hold_frame(m);
        if (--hold_n) return UI_PAUSED;
        message(m, 0x0E35);
        if (hold_tail == TAIL_CREW) unlock(m);
        goto after_handler;
    }
    redraw(m);
    bars(m);
    uint32_t st = 0;
    int r = ui_game_pass(m, &st);
    if (r != UI_OK) { if (next) *next = st; return r; }
    /* $9B66: pointer (platform) */
    uint32_t h = ui_hotspots(m, ms);
    if (h) {
        r = handler(m, h, next);
        if (r != UI_OK || m->screen != SCR_CARGO) return r;
        if (hold_n || wait_key) return UI_PAUSED;
    }
after_handler:
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    if (ms->rbutton) return UI_EXIT_TO_MAIN;                     /* $60A6 */
    return UI_OK;
}

const ScreenDef scr_cargo = {SCR_CARGO, 0x7874, enter, frame};

/* ---- for tests (ctypes): enter one of these two screens by any of their
 * entry handlers, including those ui.c's table does not list yet ($8F1A,
 * $59B08); results as ui_goto gives them */
UI_API int scr_cs_enter(MainUI *m, uint32_t h)
{
    const ScreenDef *d = (h == 0x59B08 || h == 0x70C2) ? &scr_surface : &scr_cargo;
    uint32_t nx = 0;
    int prev = m->screen;
    m->screen = d->id;
    int r = d->enter(m, h, &nx);
    if (r == UI_EXIT_TO_MAIN) { ui_exit_to_main(m); return UI_OK; }
    if (r == UI_GOTO) return ui_goto(m, nx, NULL);
    if (r == UI_HANDLER) m->screen = prev;
    return r;
}

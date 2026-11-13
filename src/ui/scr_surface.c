/*
 * Screen: planet surface ($70C2, table $EFCC, 19 entries), translated
 * routine by routine. Addresses are the original's. Checked frame by frame
 * against the original game running in an emulator.
 *
 * Entries (enter's `handler`):
 *   $70C2   main-screen box 11: planet type and owner tests (refusals post a
 *           message and return to the main loop), panel save, then $70EC;
 *   $59B08  left-column icon (cargo, fleet, combat screens; GAM2 code):
 *           $3534, owner test of $E3D4, then $70EC (else ACCESS DENIED and
 *           $60A6). Not in ui.c's table yet; the cargo screen calls it
 *           directly.
 *
 * One frame() = one iteration of the loop at $717E (after its $2FFA):
 *   $220C twinkling star (one RNG call), $FFC6 craft animations, $71E0 game
 *   pass, $9B66 pointer (platform), $9A6C hotspots + handler, $E42C + 1,
 *   right button.
 *
 * State machine: the refusal texts ($6EEC: "THAT'S EMPTY!", "ALL DOCKING
 * BAYS ARE FULL, ABORTED!", "ASSIGN A CREW FIRST!") lock the input and hold
 * the screen for 44 frames ($31A6, see scr_cargo.c): the click frame and
 * hold frames 1..43 return UI_PAUSED, hold frame 44 unlocks, puts the title
 * back and finishes the click frame's iteration.
 *
 * Memory outside the state block that the screen changes is kept here, in
 * the original's layout (big-endian), for the check to load and compare:
 *   $21AC  16 stars (x, y, state words);
 *   $59B26 the craft on the surface (6 bay pointers, $6FC4);
 *   $10046 6 animations (start, current script pointers; $FFB0 / $FFC6).
 */
#include <string.h>

#include "ui.h"
#include "day.h"
#include "craft.h"

#define SURFACE_PICTURE 0x19210u
#define SURFACE_TABLE   0xEFCCu
#define TITLE           0x208Au      /* DOCKING BAY TO PLANET SURFACE TRANSFERENCE SYSTEM. */

/* shared with scr_cargo.c */
uint8_t *cs_scr(MainUI *m, uint32_t a);
void cs_str(MainUI *m, unsigned *col, unsigned *row, uint32_t addr);
void cs_str_at(MainUI *m, unsigned col, unsigned row, uint32_t addr);
void cs_blit(MainUI *m, uint32_t dst, uint32_t src);
void cs_slot_name(MainUI *m, unsigned col, unsigned row, uint8_t slot);
void cs_select_e3d4(MainUI *m);
void cs_hold_frame(MainUI *m);
void cs_surface_anim(MainUI *m);

static uint8_t stars[16 * 6];        /* $21AC */
static uint8_t list[6 * 4];          /* $59B26 */
static uint8_t anim[6 * 8];          /* $10046 */
static int loaded;

static int hold_n;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void load(MainUI *m)
{
    if (loaded || !m->image) return;
    memcpy(stars, m->image + 0x21AC, sizeof stars);
    memcpy(list, m->image + 0x59B26, sizeof list);
    memcpy(anim, m->image + 0x10046, sizeof anim);
    loaded = 1;
}

/* tests: 0 = $21AC, 1 = $59B26, 2 = $10046 */
UI_API uint8_t *scr_surface_mem(int which)
{
    return which == 0 ? stars : which == 1 ? list : anim;
}

static uint32_t surf(int k) { return rd32(list + 4 * k); }

/* $220C: one RNG call picks a star, its state flips and it is replotted ($2234) */
static void twinkle(MainUI *m)
{
    uint16_t r = rng_next(m->g);
    uint8_t *e = stars + (r & 0xF) * 6;
    unsigned x = (unsigned)(e[0] << 8 | e[1]), y = (unsigned)(e[2] << 8 | e[3]);
    e[5] ^= 1;
    uint8_t d2 = (uint8_t)(e[5] + 1);
    uint32_t a = SCREEN_BASE + (y & 0xFFFFu) * SCREEN_LINE + ((x & 0xFFFFu) >> 3);
    uint8_t bit = (uint8_t)(1u << (7 - (x & 7)));
    for (int pl = 0; pl < 2; pl++) {
        uint8_t *p = cs_scr(m, a + (uint32_t)pl * 40u);
        *p &= (uint8_t)~bit;
        if (d2 & 1) *p |= bit;
        d2 >>= 1;
    }
}

static void anim_step(MainUI *m, uint8_t *e, uint32_t a)        /* $FFFA */
{
    s32(m->g, 0xE3F0, a);
    ui_viewer_step(m);                                           /* $4F0E */
    wr32(e + 4, g32(m->g, 0xE3F0));
}

/* $FFC6: every 4th frame, one step of each surface craft's animation */
void cs_surface_anim(MainUI *m)
{
    Game *g = m->g;
    load(m);
    if (g8(g, 0xE42C) & 3) return;
    s8(g, 0xE44E, 1);
    for (int k = 0; k < 6; k++) {
        uint8_t *e = anim + 8 * k;
        uint32_t cur = rd32(e + 4), start = rd32(e);
        if (cur == start && !surf(k)) continue;
        if (ui_m32(m, cur) != 0 || surf(k)) {
            anim_step(m, e, cur);
        } else {                                                 /* $1001A: back to the start */
            wr32(e + 4, start);
            s32(g, 0xE3F0, start);
            ui_viewer_step(m);                                   /* (not stored back) */
        }
    }
    s32(g, 0xE3F0, 0);
}

static void anim_reset(void)                                     /* $FFB0 */
{
    for (int k = 0; k < 6; k++) memcpy(anim + 8 * k + 4, anim + 8 * k, 4);
}

static void dock_slots(MainUI *m)                                /* $6F5C */
{
    uint32_t p = g32(m->g, 0xE3D4);
    cs_slot_name(m, 5, 0x8F, g8(m->g, p + 0x18));
    cs_slot_name(m, 5, 0x9A, g8(m->g, p + 0x1C));
    cs_slot_name(m, 5, 0xA5, g8(m->g, p + 0x20));
}

static uint32_t pic_at(unsigned col)                             /* $77DC0 + (d0.b >> 1) - 5 */
{
    uint8_t d7 = (uint8_t)(((uint8_t)col >> 1) - 5);
    return 0x77DC0u + (uint32_t)(int32_t)(int16_t)(uint16_t)((col & 0xFF00u) | d7);
}

/* $6FC4: list and draw the craft on the planet's surface (6 places) */
static void surface_list(MainUI *m)
{
    Game *g = m->g;
    load(m);
    memset(list, 0, sizeof list);
    uint8_t idx = g8(g, g32(g, 0xE3DC) + 0x28);
    unsigned col = 0x13, row = 0x9B;
    int n = 0;
    for (int i = 0; i < BAY_COUNT; i++) {
        uint32_t b = BAY_TABLE + (uint32_t)i * BAY_SIZE;
        if (!g8(g, b + B_TYPE) || g8(g, b + B_LOC) != idx || !(g8(g, b + B_FLAGS) & BF_SURFACE))
            continue;
        if (n < 6) wr32(list + 4 * n, b);
        cs_str(m, &col, &row, b + B_NAME);
        cs_blit(m, pic_at(col), 0x59BEEu + (uint32_t)(uint8_t)(g8(g, b + B_TYPE) - 1) * 0x2D5u);
        uint32_t t = (g8(g, b + B_FLAGS) & BF_RUNNING) ? 0x0F09u : 0x0E12u;  /* RUNNING / blank */
        row = (row & 0xFF00u) | ((row + 8) & 0xFFu);
        col = (col & 0xFF00u) | ((col - 9) & 0xFFu);
        cs_str(m, &col, &row, t);
        row = (row & 0xFF00u) | ((row - 8) & 0xFFu);
        col = (col & 0xFF00u) | ((col + 1) & 0xFFu);
        n++;
    }
    if ((uint8_t)n >= 6) return;
    for (int k = 0; k < 6 - n; k++) {                            /* empty places */
        cs_str(m, &col, &row, 0x0E12);
        cs_blit(m, pic_at(col), 0x5AFC1u);
        row = (row & 0xFF00u) | ((row + 8) & 0xFFu);
        col = (col & 0xFF00u) | ((col - 9) & 0xFFu);
        cs_str(m, &col, &row, 0x0E12);
        row = (row & 0xFF00u) | ((row - 8) & 0xFFu);
        col = (col & 0xFF00u) | ((col + 1) & 0xFFu);
    }
}

/* $6EEC: a refusal text in the title line, held 44 frames */
static void refuse(MainUI *m, unsigned col, uint32_t text)
{
    cs_str_at(m, 4, 0xB4, 0x4710);                               /* 50 spaces */
    cs_str_at(m, col, 0xB4, text);
    s8(m->g, 0xE42D, 0xFF);                                      /* $3006 */
    hold_n = 44;
}

static void enter_70ec(MainUI *m)
{
    Game *g = m->g;
    m->screen = SCR_SURFACE;
    ui_sound(m, 13);
    s32(g, 0xE3F0, 0);
    s32(g, 0xE3D8, 0);
    s8(g, 0xE434, 0);
    s32(g, 0xE3EC, SURFACE_TABLE);
    s16(g, 0xE400, 0x13);
    ui_show_picture(m, SURFACE_PICTURE);
    s32(g, 0xE3D4, g32(g, 0xE3DC));
    s8(g, 0xE425, 6);
    cs_str_at(m, 3, 0x76, g32(g, 0xE3DC) + P_NAME);
    dock_slots(m);
    cs_str_at(m, 4, 0xB4, TITLE);
    surface_list(m);
    load(m);
    anim_reset();
}

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    Game *g = m->g;
    (void)next;
    hold_n = 0;
    load(m);
    if (handler == 0x59B08) {
        cs_select_e3d4(m);
        if (g16(g, g32(g, 0xE3D4) + P_OWNER) != 10) {          /* $8F0E */
            post_message(g, 0x1919);
            return UI_EXIT_TO_MAIN;
        }
        enter_70ec(m);
        return UI_OK;
    }
    if (handler != 0x70C2) return UI_HANDLER;
    uint32_t p = g32(g, 0xE3DC);
    if (!g8(g, p + P_TYPE) || g16(g, p + P_OWNER) != 10) {
        /* "THIS PLANET NEEDS FORMATTING!" / ACCESS DENIED, then rts: back
         * to the main loop (the rest of that main iteration is not run here) */
        post_message(g, g8(g, p + P_TYPE) ? 0x1919 : 0x1038);
        m->screen = SCR_MAIN;
        return UI_OK;
    }
    ui_panel_save(m);                                            /* $47DC */
    enter_70ec(m);
    return UI_OK;
}

static void land(MainUI *m, uint32_t slot)                       /* $6EE0 */
{
    Game *g = m->g;
    uint8_t n = g8(g, slot);
    if (!n) { refuse(m, 0x17, 0x0F13); return; }                /* THAT'S EMPTY! */
    if (surf(5)) return;                                         /* $59B3A: surface full */
    s8(g, slot, 0);
    uint32_t b = BAY_TABLE + (uint32_t)(uint8_t)(n - 1) * BAY_SIZE;
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) | BF_SURFACE));
    s8(g, 0xE434, 0xFF);
    surface_list(m);
    dock_slots(m);
}

static int handler(MainUI *m, uint32_t h, uint32_t *next)
{
    Game *g = m->g;
    static const uint32_t take_off[6] = {0x6E08, 0x6DE0, 0x6DE8, 0x6DF0, 0x6DF8, 0x6E00};
    static const uint32_t toggle[6] = {0x6E9A, 0x6E72, 0x6E7A, 0x6E82, 0x6E8A, 0x6E92};
    uint32_t p = g32(g, 0xE3DC);
    switch (h) {
    case 0x6ED4: land(m, p + 0x18); return UI_OK;
    case 0x6FA4: land(m, p + 0x1C); return UI_OK;
    case 0x6FB4: land(m, p + 0x20); return UI_OK;
    case 0x460E: case 0x4614: case 0x4620: case 0x8EFC:
        *next = h;
        return UI_GOTO;
    }
    for (int k = 0; k < 6; k++) {
        if (h == take_off[k]) {                                  /* $6E0E */
            uint32_t r = craft_take_off(g, p, surf(k));
            if (r == 0) { surface_list(m); dock_slots(m); }
            else if (r != CRAFT_NOTHING) refuse(m, 0x0B, r);
            return UI_OK;
        }
        if (h == toggle[k]) {                                    /* $6EA0 */
            uint32_t r = craft_toggle_running(g, surf(k));
            if (r == 0) surface_list(m);
            else if (r != CRAFT_NOTHING) refuse(m, 0x11, r);
            return UI_OK;
        }
    }
    return UI_OK;
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    if (hold_n) {
        cs_hold_frame(m);
        if (--hold_n) return UI_PAUSED;
        s8(g, 0xE42D, 0);                                        /* $301C */
        cs_str_at(m, 4, 0xB4, TITLE);
        goto after_handler;
    }
    twinkle(m);
    cs_surface_anim(m);
    uint32_t st = 0;
    int r = ui_game_pass(m, &st);
    if (r != UI_OK) { if (next) *next = st; return r; }
    /* $9B66: pointer (platform) */
    uint32_t h = ui_hotspots(m, ms);
    if (h) {
        r = handler(m, h, next);
        if (r != UI_OK) return r;
        if (hold_n) return UI_PAUSED;
    }
after_handler:
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    if (ms->rbutton) return UI_EXIT_TO_MAIN;                     /* $60A6 */
    return UI_OK;
}

const ScreenDef scr_surface = {SCR_SURFACE, 0x70C2, enter, frame};

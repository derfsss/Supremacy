/*
 * Screen: fleet / docking bays ($8C30 from the main screen, $4620 -> $8C34
 * from the other screens' left column; table $F37C, 14 boxes): the three
 * docking bays of the planet shown ($E3D4), the selected craft ($E3D8) and
 * its status line, LAUNCH, DOCK, FLY TO with its planet grid, STOP, RENAME.
 * Addresses are the original's. The state changes are the core's craft_*
 * (src/core/craft.h); the UI bytes they leave (the selection $E3D8 / $E3D4,
 * $E3E8, the grid mode $E439 / $E43A, $E3A8, the input lock $E42D, the
 * rename backup $D5E6) are written here.
 * Checked frame by frame against the original game running in an emulator.
 *
 * Frame model and the blocking pieces (message hold, typed text, name entry,
 * busy waits): see the top of scr_shipyard.c, which holds the shared "fy_"
 * routines. Here:
 *  - a refusal ($889C) prints its text and holds 44 frames (no game pass,
 *    input locked), then clears the message line;
 *  - FLY TO ($8A02) runs its own loop ($8AD4: $4ED8, $71E0, the planet grid
 *    $902A) until a planet is picked or the right button leaves to the main
 *    screen; before and after it waits for the button to be released;
 *  - RENAME ($285C) is the name entry (time frozen).
 */
#include "ui.h"
#include "craft.h"

#if defined(__GNUC__) && __GNUC__ >= 7
#define FALL __attribute__((fallthrough))
#else
#define FALL
#endif
#define PT_BEGIN(l)    switch (l) { case 0:
#define PT_YIELD(l)    do { (l) = __LINE__; return 1; case __LINE__:; } while (0)
#define PT_AWAIT(l, c) do { (l) = __LINE__; FALL; case __LINE__: if (c) return 1; } while (0)
#define PT_EXIT(l)     do { (l) = 0; return 0; } while (0)
#define PT_END(l)      FALL; default: break; } (l) = 0; return 0

/* ---- shared, in scr_shipyard.c ---- */
void     fy_reset(void);
void     fy_install_banner(MainUI *m);
int      fy_res(uint32_t *next);
int      fy_peek(void);
void     fy_set_res(int r, uint32_t next);
void     fy_pass(MainUI *m);
void     fy_num(MainUI *m, unsigned *col, unsigned row, uint32_t v, uint32_t entry);
void     fy_str(MainUI *m, unsigned *col, unsigned *row, uint32_t a);
void     fy_str_at(MainUI *m, unsigned col, unsigned row, uint32_t a);
void     fy_starfield(MainUI *m);
void     fy_lock(MainUI *m);
void     fy_unlock(MainUI *m);
uint32_t fy_starbase(MainUI *m);
void     fy_select_shown(MainUI *m);
void     fy_hold_start(void);
int      fy_hold_step(MainUI *m);
void     fy_type_start(MainUI *m, unsigned col, unsigned row, uint32_t a);
int      fy_type_step(MainUI *m);
void     fy_name_start(uint32_t a0, unsigned col, unsigned row, uint16_t d5, uint8_t count);
int      fy_name_step(MainUI *m);
int      fy_name_escaped(void);
void     fy_key_push(int ascii);
int      fy_busy_wait(int waiting);
const Mouse *fy_mouse(void);
void     fy_set_mouse(const Mouse *ms);

/* Platform: call with each key press while the RENAME prompt may be open
 * (ASCII; 27 = ESC, 8 = DEL / backspace, 13 = RETURN). Same queue as
 * ui_shipyard_key. */
UI_API void ui_fleet_key(int ascii) { fy_key_push(ascii); }

#define ROW_MSG 0x5F

static uint8_t *scr(MainUI *m, uint32_t a)
{
    static uint8_t sink;
    a &= 0xFFFFFFu;
    return (a >= SCREEN_BASE && a < SCREEN_BASE + SCREEN_SIZE) ? &m->s->bpl[a - SCREEN_BASE] : &sink;
}

static unsigned col_inc(unsigned col, unsigned n) { return (col & 0xFF00u) | ((col + n) & 0xFFu); }

static uint32_t line_addr(MainUI *m, unsigned row) { return ui_m32(m, 0x58F58u + ((row * 4u) & 0xFFFFu)); }

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void clear_msg(MainUI *m) { fy_str_at(m, 0x0B, ROW_MSG, 0x0E3D); }        /* $88C2 */

static void blit(MainUI *m, uint32_t dst, uint32_t src)                          /* $9B2C */
{
    for (int r = 0; r < 29; r++, dst += 200)
        for (int k = 0; k < 5; k++)
            for (int pl = 0; pl < 5; pl++)
                *scr(m, dst + (uint32_t)k + 40u * (uint32_t)pl) = ui_m8(m, src++);
}

static void slot_name(MainUI *m, unsigned row, uint8_t slot)                     /* $8704 */
{
    uint32_t a = slot ? BAY_TABLE + (uint32_t)(uint16_t)((uint8_t)(slot - 1) * BAY_SIZE) : 0x1090;
    fy_str_at(m, 5, row, a);
}

static void slots(MainUI *m)                                                     /* $86C4 */
{
    uint32_t p = g32(m->g, 0xE3D4);
    slot_name(m, 0x29, ui_m8(m, p + 0x18));
    slot_name(m, 0x34, ui_m8(m, p + 0x1C));
    slot_name(m, 0x3F, ui_m8(m, p + 0x20));
}

/* the grid boxes $2A88 / $2A24 / $2B10: a vertical edge at byte +4 and the
 * bottom line, `set` = the planes drawn, `clr` = the planes cleared */
static void box(MainUI *m, unsigned col, unsigned row, unsigned set, unsigned clr)
{
    uint32_t a = line_addr(m, row) + ((col & 0xFFu) >> 1) + 4;
    for (int k = 0; k <= 6; k++) {
        uint32_t b = a + (uint32_t)k * 200u;
        for (int pl = 0; pl < 5; pl++) {
            if (set >> pl & 1) *scr(m, b + 40u * pl) = 0x08;
            else if (clr >> pl & 1) *scr(m, b + 40u * pl) = 0;
        }
    }
    a += 6u * 200u - 4u;
    for (int k = 0; k < 5; k++)
        for (int pl = 0; pl < 5; pl++) {
            if (set >> pl & 1) *scr(m, a + (uint32_t)k + 40u * pl) = k < 4 ? 0xFF : 0xF8;
            else if (clr >> pl & 1) *scr(m, a + (uint32_t)k + 40u * pl) = 0;
        }
}
#define BOX_EMPTY(m, c, r)  box(m, c, r, 0x12, 0x04)     /* $2A88: planes 1, 4 */
#define BOX_ENEMY(m, c, r)  box(m, c, r, 0x10, 0x02)     /* $2A24: plane 4 */
#define BOX_PLAYER(m, c, r) box(m, c, r, 0x04, 0x12)     /* $2B10: plane 2 */

static void grid_blank(MainUI *m)                                                /* $8690 */
{
    unsigned row = 0x83;
    for (int r = 0; r < 8; r++) {
        unsigned col = 8;
        for (int c = 0; c < 4; c++) {
            BOX_EMPTY(m, col, row);
            fy_str(m, &col, &row, 0x0E12);
            col = col_inc(col, 1);
        }
        row = (row & 0xFF00u) | ((row + 8) & 0xFFu);
    }
}

static void planet_list(MainUI *m)                                               /* $8F70 */
{
    Game *g = m->g;
    unsigned col = 8, row = 0x83;
    uint32_t a = g32(g, 0xE3B0);
    for (unsigned k = 0; k <= g16(g, 0xE410); k++, a += PLANET_SIZE) {
        uint16_t o = g16(g, a + P_OWNER);
        if (o == 1 || o == 4) BOX_EMPTY(m, col, row);
        else {
            if (o == 10) BOX_PLAYER(m, col, row);
            else BOX_ENEMY(m, col, row);
            fy_str(m, &col, &row, a + P_NAME);
            col = (col - 9) & 0xFFFFu;
        }
        row = (row & 0xFF00u) | ((row + 8) & 0xFFu);
        if ((row & 0xFF) == 0xC3) { row = (row & 0xFF00u) | 0x83; col = (col & 0xFF00u) | ((col + 10) & 0xFFu); }
    }
}

static void craft_list(MainUI *m)                                                /* $8FDE */
{
    Game *g = m->g;
    unsigned col = 8, row = 0x83;
    for (int i = 0; i < BAY_COUNT; i++) {
        uint32_t b = BAY_TABLE + (uint32_t)i * BAY_SIZE;
        if (!g8(g, b + B_TYPE)) continue;
        s32(g, 0xE3A8, b);
        fy_str(m, &col, &row, b);
        col = (col - 9) & 0xFFFFu;
        row = (row & 0xFF00u) | ((row + 8) & 0xFFu);
        if ((row & 0xFF) == 0xC3) { row = (row & 0xFF00u) | 0x83; col = (col & 0xFF00u) | ((col + 10) & 0xFFu); }
    }
}

static void grid(MainUI *m, int craft)                                           /* $8678 / $8670 */
{
    s8(m->g, 0xE439, craft ? 0xFF : 0);
    grid_blank(m);
    if (g8(m->g, 0xE439)) craft_list(m);
    else planet_list(m);
}

static void clear_fly(MainUI *m)                                                 /* $5760 */
{
    unsigned row = 0x8F;
    for (int k = 0; k < 5; k++) {
        fy_str_at(m, 0x34, row, 0x0E12);
        row = (row & 0xFF00u) | ((row + 10) & 0xFFu);
    }
}

static uint32_t tpl_of(MainUI *m, uint32_t b)                                    /* $4682 */
{
    return 0xECD4u + ((uint32_t)(uint8_t)(ui_m8(m, b + B_TYPE) - 1) * 0x30u & 0xFFFFu);
}

static void status(MainUI *m)                                                    /* $8D16 */
{
    Game *g = m->g;
    uint32_t b = g32(g, 0xE3D8);
    if (!b) {                                             /* "SELECT A SHIP!" blinks */
        if ((int8_t)(g8(g, 0xE42C) & 7) >= 5) fy_str_at(m, 0x2D, ROW_MSG, 0x0E30);
        else fy_str_at(m, 0x12, ROW_MSG, 0x0F4F);
        return;
    }
    uint32_t d7 = g32(g, 0xE39C);
    if (d7 & 0x1000) {
        clear_msg(m);
        fy_str_at(m, 3, 0x0E, g32(g, 0xE3D4) + P_NAME);
        fy_str_at(m, 0x1D, 0x2B, b);
        uint32_t ty = (uint8_t)(ui_m8(m, b + B_TYPE) - 1);
        fy_str_at(m, 0x1C, 0x48, 0x1E76u + ty * 0x16u);
        blit(m, 0x747F9, 0x59BEEu + ty * 0x2D5u);
        slots(m);
    }
    d7 = g32(g, 0xE39C);
    if (d7 & 0x2000) {
        unsigned col = 0x2A, row = 0x2B;
        uint32_t t = tpl_of(m, b);
        if (ui_m16(m, t + T_CREW) == 0xFFFE) fy_str(m, &col, &row, 0x183C);    /* "REMOTE " */
        else {
            fy_str(m, &col, &row, 0x0E18);
            fy_num(m, &col, row, ui_m16(m, b + B_CREW), 0x32FE);
        }
        t = tpl_of(m, b);
        col = 0x29;
        row = 0x3A;
        if (ui_m16(m, t + T_FUEL) == 0xFFFF) fy_str(m, &col, &row, 0x1833);    /* "NUCLEAR " */
        else {
            fy_num(m, &col, row, ui_m16(m, b + B_TANK), 0x32F0);
            fy_str(m, &col, &row, 0x20DB);                                     /* " T." */
        }
        col = 0x0E;
        row = ROW_MSG;
        fy_str(m, &col, &row, b);
        uint8_t fl = ui_m8(m, b + B_FLAGS);
        if (fl & BF_TRANSIT) {
            fy_str(m, &col, &row, 0x0FE7);                                     /* " IS NOW IN TRANSIT TO " */
            fy_str(m, &col, &row, ui_m32(m, b + B_DEST) + P_NAME);
            fy_str(m, &col, &row, 0x1015);                                     /* " : E.D.A. " */
            fy_num(m, &col, row, ui_m16(m, b + B_TRAVEL), 0x3284);
            fy_str(m, &col, &row, 0x0E1A);
        } else {
            uint32_t a = (fl & BF_ORBIT) ? 0x1020u : (fl & BF_SURFACE) ? 0x0FFEu : 0x0FCEu;
            fy_str(m, &col, &row, a);
            uint32_t loc = g32(g, 0xE3B0) + (uint32_t)(uint16_t)(ui_m8(m, b + B_LOC) * PLANET_SIZE);
            fy_str(m, &col, &row, ui_m8(m, loc + 0x24) ? loc + P_NAME : 0x1A45);
        }
    }
    s32(g, 0xE39C, 0);
}

/* $902A: a click in the planet grid shows that planet (the fleet screen's
 * part; $E425 = 3 is the statistics screen's) */
static void planet_click(MainUI *m)
{
    Game *g = m->g;
    const Mouse *ms = fy_mouse();
    if (!ms->button) return;
    uint32_t d3 = (uint16_t)ms->x, d4 = (uint16_t)ms->y;
    if ((uint8_t)d4 < 0x83 || (uint16_t)d3 < 0x1C) return;
    d3 = (uint16_t)((uint16_t)(d3 - 0x1C) / 0x28u);
    if (d3 >= 4) return;
    d3 *= 8;
    d4 = (uint16_t)((uint16_t)(d4 - 0x83) / 8u);
    if (d4 >= 8) return;
    d3 = (uint16_t)(d3 + d4);
    uint8_t lv = g8(g, 0xE43B);
    uint32_t lim = lv == 0 ? 8 : lv == 1 ? 16 : 32;
    if (d3 >= lim) return;
    uint32_t a = d3 * PLANET_SIZE + g32(g, 0xE3B0);
    if (!ui_m8(m, a + 0x24)) return;
    if (g8(g, 0xE425) == 4) {
        s32(g, 0xE39C, g32(g, 0xE39C) | 0x7D3F6u);
        s32(g, 0xE3D4, a);
    } else if (g8(g, 0xE425) == 3) {
        /* the statistics screen's branch ($90CC); not reached from here */
    } else {
        s32(g, 0xE3D4, a);
    }
}

/* $913A: a click in the craft grid selects that craft */
static void craft_click(MainUI *m)
{
    Game *g = m->g;
    const Mouse *ms = fy_mouse();
    if (!ms->button) return;
    uint32_t d3 = (uint16_t)ms->x, d4 = (uint16_t)ms->y;
    if ((uint8_t)d4 < 0x83 || (uint16_t)d3 < 0x1C) return;
    d3 = (uint16_t)((uint16_t)(d3 - 0x1C) / 0x28u);
    if (d3 >= 7) return;
    d3 *= 8;
    d4 = (uint16_t)((uint16_t)(d4 - 0x83) / 8u);
    if (d4 >= 8) return;
    uint8_t n = (uint8_t)(d3 + d4);
    for (int i = 0; i < BAY_COUNT; i++) {
        uint32_t b = BAY_TABLE + (uint32_t)i * BAY_SIZE;
        if (!g8(g, b + B_TYPE)) continue;
        if (n) { n--; continue; }
        s32(g, 0xE3D8, b);
        s32(g, 0xE3D4, g32(g, 0xE3B0) + (uint32_t)(uint16_t)(g8(g, b + B_LOC) * PLANET_SIZE));
        s32(g, 0xE39C, g32(g, 0xE39C) | 0x3000u);
        if (g8(g, b + B_FLAGS) & BF_TRANSIT) s32(g, 0xE3D4, g32(g, b + B_DEST));
        return;
    }
}

/* ------------------------------------------------------------------ */
/* handlers                                                            */
/* ------------------------------------------------------------------ */

static int it_line, h_line, msg_line;
static uint32_t it_h;

/* $889C: the text in the message line, held 44 frames */
static unsigned msg_col;
static uint32_t msg_text;

static void msg_start(unsigned col, uint32_t text) { msg_col = col; msg_text = text; msg_line = 0; }

static int msg_step(MainUI *m)
{
    PT_BEGIN(msg_line);
    clear_msg(m);
    fy_str_at(m, msg_col, ROW_MSG, msg_text);
    fy_lock(m);
    ui_sound(m, 3);                                       /* $59ACA */
    fy_hold_start();
    PT_AWAIT(msg_line, fy_hold_step(m));
    fy_unlock(m);
    clear_msg(m);
    PT_END(msg_line);
}

static unsigned col_of(uint32_t text, const uint32_t *map)
{
    for (; map[0]; map += 2)
        if (map[0] == text) return map[1];
    return 0x12;
}

static const uint32_t LAUNCH_COL[] = {0x16FB, 0x1F, 0x170E, 0x1A, 0x179D, 0x0E, 0x1AD6, 0x1A,
                                      0x18E1, 0x12, 0x1817, 0x1B, 0x1802, 0x1E, 0};
static const uint32_t DOCK_COL[] = {0x16FB, 0x1F, 0x170E, 0x1A, 0x174F, 0x1C, 0x1AF0, 0x11,
                                    0x172B, 0x16, 0};
static const uint32_t FLY_COL[] = {0x16FB, 0x1F, 0x170E, 0x1A, 0x16E5, 0x1D, 0x17D0, 0x1C,
                                   0x1817, 0x1A, 0};

/* $8722 / $8730 / $873E: docking bay 1-3 of the planet shown */
static int pick_slot(MainUI *m, unsigned off)
{
    Game *g = m->g;
    PT_BEGIN(h_line);
    {
        uint8_t n = ui_m8(m, g32(g, 0xE3D4) + off);
        if (!n) {
            s32(g, 0xE39C, g32(g, 0xE39C) | 0x2000u);
            msg_start(0x19, 0x0F13);                      /* "THAT'S EMPTY!" */
        } else {
            uint32_t b = BAY_TABLE + (uint32_t)(uint16_t)((uint8_t)(n - 1) * BAY_SIZE);
            s32(g, 0xE3D8, b);
            if (g8(g, b + B_FLAGS) & BF_TRANSIT) s32(g, 0xE3D4, g32(g, b + B_DEST));
            s32(g, 0xE39C, g32(g, 0xE39C) | 0x3000u);
            PT_EXIT(h_line);
        }
    }
    PT_AWAIT(h_line, msg_step(m));
    PT_END(h_line);
}

/* $8882 launch / $8796 dock */
static int launch_dock(MainUI *m, int dock)
{
    Game *g = m->g;
    PT_BEGIN(h_line);
    {
        uint32_t b = g32(g, 0xE3D8), p = g32(g, 0xE3D4);
        uint32_t r = dock ? craft_dock(g, b, p) : craft_launch(g, b, p);
        if (!r) { clear_msg(m); PT_EXIT(h_line); }
        msg_start(col_of(r, dock ? DOCK_COL : LAUNCH_COL), r);
    }
    PT_AWAIT(h_line, msg_step(m));
    PT_END(h_line);
}

/* $8A02 FLY TO */
static int fly_to(MainUI *m)
{
    static uint32_t b, r;
    Game *g = m->g;
    PT_BEGIN(h_line);
    clear_fly(m);                                         /* $5760 */
    b = g32(g, 0xE3D8);
    r = craft_fly_check(g, b);
    if (r) {
        msg_start(col_of(r, FLY_COL), r);
        PT_AWAIT(h_line, msg_step(m));
        PT_EXIT(h_line);
    }
    fy_str_at(m, 0x34, 0x8F, b);
    ui_sound(m, 13);
    fy_str_at(m, 0x34, 0x99, g32(g, 0xE3B0) + (uint32_t)(uint16_t)(g8(g, b + B_LOC) * PLANET_SIZE) + P_NAME);
    clear_msg(m);
    fy_str_at(m, 0x1A, ROW_MSG, 0x1780);                  /* "SELECT THE PLANET TO FLY TO." */
    s32(g, 0xE3E8, g32(g, 0xE3D4));
    s32(g, 0xE3D4, 0);
    grid(m, 0);                                           /* $8670 */
    s8(g, 0xE422, 0);
    s8(g, 0xE43A, 0xFF);
    PT_AWAIT(h_line, fy_busy_wait(fy_mouse()->button));  /* $8ACE */
    for (;;) {
        PT_YIELD(h_line);                                 /* $2FFA */
        ui_viewer_tick(m);                                /* $4ED8 */
        fy_pass(m);                                       /* $71E0 */
        planet_click(m);                                  /* $902A */
        if (g32(g, 0xE3D4)) break;
        if (fy_mouse()->rbutton) {                        /* back to the main screen */
            s8(g, 0xE43A, 0);
            s32(g, 0xE3D4, g32(g, 0xE3E8));
            fy_set_res(UI_EXIT_TO_MAIN, 0);
            PT_EXIT(h_line);
        }
    }
    PT_AWAIT(h_line, fy_busy_wait(fy_mouse()->button));  /* $8B0C */
    fy_str_at(m, 0x34, 0xA3, g32(g, 0xE3D4) + P_NAME);
    clear_msg(m);
    r = craft_fly_to(g, b, g32(g, 0xE3D4));               /* $8B2E */
    if (r == 0x17D0) {                                    /* "SHIP IS ALREADY THERE!" */
        s8(g, 0xE43A, 0);
        msg_start(0x1C, r);
        PT_AWAIT(h_line, msg_step(m));
        grid(m, 1);
        clear_fly(m);
        PT_EXIT(h_line);
    }
    {
        unsigned col = 0x35, row = 0xAD;
        uint16_t days = g16(g, 0xE40C);
        fy_num(m, &col, row, days, 0x330C);
        fy_str(m, &col, &row, 0x20DF);                    /* " DAYS" */
        uint16_t need = (uint16_t)(days * 0x19u);
        if (!g8(g, 0xD483)) need = (uint16_t)(need + need);
        col = 0x36;
        row = 0xB7;
        fy_num(m, &col, row, need, 0x32FE);
        fy_str(m, &col, &row, 0x20DB);                    /* " T." */
    }
    if (r) {                                              /* "NEED MORE FUEL!" */
        s32(g, 0xE3D4, g32(g, 0xE3E8));
        msg_start(0x1A, r);
        PT_AWAIT(h_line, msg_step(m));
        grid(m, 1);
        PT_EXIT(h_line);
    }
    s8(g, 0xE43A, 0);
    grid(m, 1);
    PT_END(h_line);
}

/* $285C RENAME */
static int rename_craft(MainUI *m)
{
    static uint32_t b;
    Game *g = m->g;
    PT_BEGIN(h_line);
    b = g32(g, 0xE3D8);
    if (!b) { ui_sound(m, 3); PT_EXIT(h_line); }
    ui_sound(m, 13);
    fy_lock(m);
    clear_msg(m);
    fy_str_at(m, 0x1B, ROW_MSG, 0x1A2A);                  /* "TYPE NEW NAME: >         <" */
    for (int i = 0; i < 9; i++) s8(g, 0xD5E6 + (uint32_t)i, g8(g, b + (uint32_t)i));
    {                                                     /* $2272 */
        static const uint8_t un[9] = {'U', 'N', '-', 'N', 'A', 'M', 'E', 'D', '!'};
        for (int i = 0; i < 9; i++) s8(g, b + (uint32_t)i, un[i]);
    }
    fy_name_start(b, 0x2B, ROW_MSG, 8, 0);
    PT_AWAIT(h_line, fy_name_step(m));
    if (fy_name_escaped()) {
        for (int i = 0; i < 9; i++) s8(g, b + (uint32_t)i, g8(g, 0xD5E6 + (uint32_t)i));
        s8(g, 0xE43F, 0xFF);
        fy_type_start(m, 0x2B, ROW_MSG, 0x1086);          /* " ABORTED " */
        PT_AWAIT(h_line, fy_type_step(m));
        s8(g, 0xE43F, 0);
        fy_hold_start();
        PT_AWAIT(h_line, fy_hold_step(m));
    } else {
        uint8_t name[9];
        for (int i = 0; i < 9; i++) name[i] = g8(g, b + (uint32_t)i);
        craft_rename(g, b, name);                         /* the entry wrote it in place */
        craft_list(m);                                    /* $8FDE */
    }
    clear_msg(m);
    fy_unlock(m);
    PT_END(h_line);
}

static int handler(MainUI *m)
{
    switch (it_h) {
    case 0x8722: return pick_slot(m, 0x18);
    case 0x8730: return pick_slot(m, 0x1C);
    case 0x873E: return pick_slot(m, 0x20);
    case 0x8882: return launch_dock(m, 0);
    case 0x8796: return launch_dock(m, 1);
    case 0x8A02: return fly_to(m);
    case 0x285C: return rename_craft(m);
    case 0x5708: craft_stop(m->g, g32(m->g, 0xE3D8)); return 0;
    }
    return 0;
}

/* one iteration of the loop $8CD6 */
static int iteration(MainUI *m)
{
    Game *g = m->g;
    PT_BEGIN(it_line);
    status(m);                                            /* $8D16 */
    if (g8(g, 0xE439)) craft_click(m);                    /* $913A */
    else planet_click(m);                                 /* $902A */
    ui_viewer_tick(m);                                    /* $4ED8 */
    {
        uint32_t st = 0;
        int r = ui_game_pass(m, &st);                     /* $71E0 */
        if (r != UI_OK) { fy_set_res(r, st); PT_EXIT(it_line); }
    }
    it_h = ui_hotspots(m, fy_mouse());                    /* $9A6C */
    if (it_h == 0x455A || it_h == 0x460E || it_h == 0x8F1A || it_h == 0x59B08 ||
        it_h == 0x4614 || it_h == 0x461A) {
        fy_set_res(UI_GOTO, it_h);                        /* the left / right column: other screens */
        PT_EXIT(it_line);
    }
    if (it_h) {
        h_line = 0;
        PT_AWAIT(it_line, handler(m));
        if (fy_peek() == UI_EXIT_TO_MAIN || fy_peek() == UI_GAME_OVER) PT_EXIT(it_line);
    }
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    if (fy_mouse()->rbutton) {                            /* $A9A */
        fy_select_shown(m);                               /* $3534 */
        fy_set_res(UI_EXIT_TO_MAIN, 0);
    }
    PT_END(it_line);
}

static int enter(MainUI *m, uint32_t handler_addr, uint32_t *next)
{
    Game *g = m->g;
    (void)next;
    if (handler_addr == 0x8C30) ui_panel_save(m);        /* $47DC ($4620 -> $8C34 skips it) */
    fy_reset();
    fy_install_banner(m);
    it_line = h_line = msg_line = 0;
    s8(g, 0xE43F, 0);
    ui_sound(m, 13);
    s8(g, 0xE425, 4);
    s32(g, 0xE3EC, 0xF37C);
    s16(g, 0xE400, 0x0E);
    ui_show_picture(m, 0x211C8);
    slots(m);                                             /* $86C4 (the $E3D4 of before) */
    blit(m, 0x747F9, 0x5AFC1);
    s32(g, 0xE3F0, 0);
    s32(g, 0xE3D8, 0);
    s32(g, 0xE3D4, g32(g, 0xE3DC));
    s32(g, 0xE39C, 0x200);
    s8(g, 0xE43A, 0);
    grid(m, 1);                                           /* $8678 */
    fy_str_at(m, 3, 0x0E, g32(g, 0xE3DC) + P_NAME);       /* $7A00 */
    ui_draw_date(m);                                      /* $7806 */
    s8(g, 0xE44E, 6);
    s16(g, 0xE3F8, 0);
    s8(g, 0xE42E, 0);
    s32(g, 0xE3F0, 0xFE6A);
    return UI_OK;
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    fy_set_mouse(ms);
    iteration(m);
    return fy_res(next);
}

const ScreenDef scr_fleet = {SCR_FLEET, 0x8C30, enter, frame};

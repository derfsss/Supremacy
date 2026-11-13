/*
 * Craft: the daily bay pass ($71E0, bays) and the state-changing actions of
 * the shipyard, fleet, cargo and surface screens and the format icon.
 * Checked against the original game running in an emulator, byte for
 * byte over the whole state block and the random-number seed.
 *
 * Only game state is changed here. The UI parts of the original handlers
 * (text drawing, the input lock $E42D, the selection $E3D8 / $E3D4 and its
 * saved copy $E3E8, the fly-to grid mode $E43A, the surface redraw flag
 * $E434, the scratch $E3A8, prompts and waits) belong to the UI layer.
 * Redraw bits ($E39C) are kept, as in economy.c.
 *
 * Bug fixes (each guarded by g->fix_bugs; 0 = the original's behaviour):
 *  1. Dock-slot search default (launch $89BC, scrap $0CC2, format $50D6): the
 *     original clears slot 3 when neither slot 1 nor 2 holds the craft,
 *     throwing out whatever craft is docked there (a processor that sits on
 *     a surface is in no slot, so every later format clears slot 3). Fixed:
 *     a slot is cleared only if it holds the craft.
 *  2. Launch takes 100 fuel from nuclear craft too ($89E2): the tank of a
 *     solar satellite wraps to 65,436 (and scrapping it later adds that to
 *     the planet's fuels, wrapping them). Fixed: no fuel taken when the
 *     template's fuel is $FFFF.
 *  3. Launch clears the slot on the fleet screen's planet ($E3D4) and dock
 *     takes a slot there, not on the craft's location planet. Fixed: the
 *     location planet is used (the same planet in normal play).
 *  4. STOP on an atmosphere processor that is formatting ($5708) leaves
 *     FORMAT set: the target is formatted although the processor never
 *     arrives. Fixed: STOP does nothing for a craft with FORMAT set (like
 *     launch, the processor is controlled from the star map panel).
 *  5. A craft in transit with 0 travel days left is never processed again
 *     ($738A returns early), so such a format never finishes. Fixed: it
 *     arrives on that pass.
 *  6. Unload everything ($7BF4) tests the 30,000 limits on the screen's
 *     planet ($E3D4) but unloads onto the main-screen planet ($E3DC); if they
 *     differ and the latter is full the loop never ends. Fixed: the limits
 *     are tested on the planet unloaded onto.
 */
#include <stddef.h>

#include "craft.h"

/* globals */
#define G_LEVEL       0xE43Bu      /* u8 opponent level 0..2 */
#define G_NPLANETS_1  0xE410u      /* u16 starbase index (planets - 1) */
#define G_SB_INDEX    0xE41Eu      /* u8 starbase index, copied to new craft */
#define G_SELECTED    0xE3D8u      /* u32 selected bay (fleet / cargo screens) */
#define G_FMT_TARGET  0xE3C8u      /* u32 planet being formatted by the player */
#define G_FMT_DAYS    0xD56Cu      /* u16 format days (report) */
#define G_FMT_TRAVEL  0xD570u      /* u16 days to arrive (report) */
#define G_NO_BAY      0xE437u      /* u8 $FF when no bay is free */
#define G_HOLD_FULL   0xE438u      /* u8 $FF when the hold is full (load) */
#define G_IDLE        0xE422u      /* u8 cleared by the cargo buttons */
#define G_FRAME       0xE42Cu      /* u8 frame counter of the screen loops */
#define G_FUEL_LAST   0xE44Au      /* u8 frame of the last fuel step */
#define G_FUEL_RATE   0xE44Bu      /* u8 12 - amount per step */
#define G_FUEL_WAIT   0xE44Cu      /* u8 frames to wait before the next step */
#define G_FUEL_EMPTY  0xE44Du      /* u8 $FF emptying, 0 filling */
#define G_BARS        0xE41Cu      /* u16 cargo screen bar animation counter */
#define G_DAYS_TMP    0xE40Cu      /* u16 trip length (fly to) */
#define G_FLOWN       0xD45Fu      /* u8 set by FLY TO (event script trigger) */
#define G_FORMATTED   0xD45Cu      /* u8 set by a format start (event script trigger) */
#define G_DRILL       0xD482u      /* u8 drill bit upgrade */
#define G_HYBRID      0xD47Fu      /* u8 hybrid plant upgrade */
#define G_VALVE       0xD483u      /* u8 valve system: half fuel use */
#define G_PLANET_NAME_DEFAULT 0xE38Eu  /* "PLANET..1", counter digit at $E396 */
#define PLATOONS      0xEB54u      /* 24 platoons, 12 bytes */

/* posted messages and printed texts (original addresses) */
#define MSG_MACHINE_FAILED 0xD48Cu
#define MSG_MACHINE_NAME   0xD4B7u
#define MSG_FORMAT_DONE    0xD572u
#define MSG_FORMAT_NAME    0xD58Au

#define P_SLOT1   0x18
#define P_FMTTIME 0x1A
#define P_INDEX   0x28

#define CAP 30000u

static uint32_t starbase(Game *g)                          /* $3506 */
{
    return g32(g, G_PLANET_TABLE) + (uint32_t)g16(g, G_NPLANETS_1) * PLANET_SIZE;
}

static uint32_t planet_at(Game *g, uint8_t index)
{
    return g32(g, G_PLANET_TABLE) + (uint32_t)index * PLANET_SIZE;
}

uint32_t craft_template(Game *g, uint32_t bay)             /* $4682 */
{
    return TPL_TABLE + (uint32_t)(uint8_t)(g8(g, bay + B_TYPE) - 1) * TPL_SIZE;
}

uint32_t craft_location(Game *g, uint32_t bay)             /* $71CC */
{
    return planet_at(g, g8(g, bay + B_LOC));
}

static uint8_t bay_number(uint32_t bay)                    /* slot value: bay + 1 */
{
    return (uint8_t)((bay - BAY_TABLE) / BAY_SIZE + 1);
}

static void redraw(Game *g, uint32_t bits) { or32(g, G_REDRAW, bits); }

static void sound(Game *g, int n)
{
    if (g->play_sound)
        g->play_sound(g, n);
}

/* Clear the dock slot of `planet` that holds `bay`. The original falls back
 * to slot 3 when neither slot 1 nor 2 holds it (fix 1). */
static void slot_clear(Game *g, uint32_t planet, uint32_t bay)
{
    uint8_t n = bay_number(bay);
    uint32_t a = planet + P_SLOT1;
    if (g8(g, a) != n) {
        a += 4;
        if (g8(g, a) != n)
            a += 4;
    }
    if (g->fix_bugs && g8(g, a) != n)
        return;
    s8(g, a, 0);
}

/* first free dock slot of a planet, 0 if all three are used */
static uint32_t slot_free(Game *g, uint32_t planet)
{
    for (int k = 0; k < 3; k++)
        if (g8(g, planet + P_SLOT1 + 4 * k) == 0)
            return planet + P_SLOT1 + 4 * k;
    return 0;
}

static void copy9(Game *g, uint32_t dst, uint32_t src)
{
    for (int i = 0; i < 9; i++)
        s8(g, dst + i, g8(g, src + i));
}

/* ------------------------------------------------------------ daily pass */

/* $71AA: "A MACHINE HAS FAILED ON <planet>" */
static void machine_failed(Game *g, uint32_t planet)
{
    copy9(g, MSG_MACHINE_NAME, planet + P_NAME);
    post_message(g, MSG_MACHINE_FAILED);
}

/* $7432: the formatted planet becomes the player's */
static void format_complete(Game *g, uint32_t bay)
{
    s8(g, bay + B_FLAGS, (uint8_t)(g8(g, bay + B_FLAGS) & 0xFE));
    post_message(g, MSG_FORMAT_DONE);
    uint32_t p = g32(g, G_FMT_TARGET);
    s16(g, p + P_OWNER, OWNER_PLAYER);
    uint8_t t;
    do {
        t = (uint8_t)(rng_next(g) & 7);
    } while (t == 0 || t == 2 || t >= 6);
    s8(g, p + P_TYPE, t);
    s16(g, p + P_POP, rng_below(g, 1000));
    s32(g, p + P_CREDITS, 0);
    s16(g, p + P_FOOD, (uint16_t)(rng_below(g, 1200) + 300));
    s16(g, p + P_MINERALS, 20);
    s16(g, p + P_FUELS, 150);
    s16(g, p + P_ENERGY, 35);
    redraw(g, 0x08);
    /* the original then redraws the main screen's planet viewer ($4E28): UI */
}

void craft_bay_pass(Game *g, int i)
{
    uint32_t b = BAY_TABLE + (uint32_t)i * BAY_SIZE;
    uint8_t type = g8(g, b + B_TYPE);
    if (type == 0)
        return;
    if (g16(g, b + B_SCRAP)) {
        s16(g, b + B_SCRAP, (uint16_t)(g16(g, b + B_SCRAP) - 1));
        redraw(g, 0x800);
    }

    uint8_t fl = g8(g, b + B_FLAGS);
    if (!(fl & BF_ORBIT)) {
        if ((type == 6 || type == 7) && (fl & BF_RUNNING)) {
            uint32_t L = craft_location(g, b);
            if (g16(g, L + P_ENERGY) == 0) {
                machine_failed(g, L);
                s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) & ~BF_RUNNING));
            } else {
                s16(g, L + P_ENERGY, (uint16_t)(g16(g, L + P_ENERGY) - 1));
                uint8_t pt = g8(g, L + P_TYPE);
                if (type == 6) {                         /* mining */
                    if (g16(g, L + P_MINERALS) < CAP) {
                        uint16_t v = 2;
                        if (g8(g, G_DRILL)) v += 7;
                        if (pt == 5) v += 5;
                        s16(g, L + P_MINERALS, (uint16_t)(g16(g, L + P_MINERALS) + v));
                    }
                    if (g16(g, L + P_FUELS) < CAP) {
                        uint16_t v = 7;
                        if (g8(g, G_DRILL)) v += 25;
                        if (pt == 5) v += 15;
                        s16(g, L + P_FUELS, (uint16_t)(g16(g, L + P_FUELS) + v));
                    }
                    redraw(g, 0x38000);
                } else if (g16(g, L + P_FOOD) < CAP) {   /* farming */
                    uint16_t v = 12;
                    if (g8(g, G_HYBRID)) v += 25;
                    if (pt == 4) v += 28;
                    s16(g, L + P_FOOD, (uint16_t)(g16(g, L + P_FOOD) + v));
                    redraw(g, 0x4000);
                }
            }
        }
    } else if (type == 3) {                              /* solar satellite in orbit */
        uint32_t L = craft_location(g, b);
        if (g16(g, L + P_ENERGY) < CAP) {
            s16(g, L + P_ENERGY, (uint16_t)(g16(g, L + P_ENERGY) + 6));
            redraw(g, 0x20000);
            if (g8(g, L + P_TYPE) == 3)
                s16(g, L + P_ENERGY, (uint16_t)(g16(g, L + P_ENERGY) + 7));
        }
    }

    fl = g8(g, b + B_FLAGS);
    if (fl & BF_TRANSIT) {
        if (g16(g, b + B_TRAVEL) == 0) {
            if (!g->fix_bugs)
                return;                                  /* fix 5: arrive now */
        } else {
            if (g16(g, craft_template(g, b) + T_FUEL) != 0xFFFF) {
                uint16_t burn = g8(g, G_VALVE) ? 25 : 50;
                uint16_t tank = g16(g, b + B_TANK);
                if (burn >= tank)
                    burn = tank;
                s16(g, b + B_TANK, (uint16_t)(tank - burn));
            }
            uint16_t left = (uint16_t)(g16(g, b + B_TRAVEL) - 1);
            s16(g, b + B_TRAVEL, left);
            if (left) {
                if (b == g32(g, G_SELECTED))
                    redraw(g, 0x2000);
                if (g8(g, b + B_FLAGS) & BF_FORMAT)
                    s16(g, G_FMT_TRAVEL, left);
                return;
            }
        }
        if (b == g32(g, G_SELECTED))
            redraw(g, 0x3000);
        s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) & ~BF_TRANSIT));   /* arrived */
        s8(g, b + B_LOC, g8(g, g32(g, b + B_DEST) + P_INDEX));
        if (g8(g, b + B_FLAGS) & BF_FORMAT)
            s8(g, b + B_FLAGS, BF_FORMAT | BF_SURFACE);  /* the processor lands */
        return;
    }
    if (!(fl & BF_FORMAT))
        return;
    uint16_t left = (uint16_t)(g16(g, b + B_FORMAT) - 1);
    s16(g, b + B_FORMAT, left);
    if (left) {
        s16(g, G_FMT_DAYS, left);
        return;
    }
    format_complete(g, b);
}

/* ------------------------------------------------------------ shipyard */

static uint32_t first_free_bay(Game *g)                    /* $6530 */
{
    s8(g, G_NO_BAY, 0);
    for (int i = 0; i < BAY_COUNT; i++) {
        uint32_t b = BAY_TABLE + (uint32_t)i * BAY_SIZE;
        if (g8(g, b + B_TYPE) == 0)
            return b;
    }
    s8(g, G_NO_BAY, 0xFF);
    return 0;
}

uint32_t craft_buy_check(Game *g, uint32_t tpl)
{
    uint8_t level = g8(g, G_LEVEL);
    if (g8(g, tpl + T_TYPE) == 4) {
        if (level == 0 && g16(g, G_YEAR) < 2011)
            return 0x135C;               /* "TRY LATER, BASIC TERRA-FORMATTER IS BEING RE-DESIGNED" */
        for (int i = 0; i < BAY_COUNT; i++)
            if (g8(g, BAY_TABLE + (uint32_t)i * BAY_SIZE + B_TYPE) == 4)
                return 0x11AC;           /* "YOU CAN ONLY HAVE ONE ATMOSPHERE PROCESSOR" */
    }
    uint32_t sb = starbase(g);
    if (!slot_free(g, sb))
        return 0x11E2;                   /* "NO ROOM LEFT IN THE DOCKING BAYS" */
    if (g32(g, sb + P_CREDITS) < g16(g, tpl + T_PRICE))
        return 0x1218;                   /* not enough credits */
    if (level >= 2 && g16(g, sb + P_MINERALS) < g16(g, tpl + T_MINERALS))
        return 0x124E;                   /* not enough minerals */
    if (level != 0 && g16(g, sb + P_ENERGY) < g16(g, tpl + T_ENERGY))
        return 0x1284;                   /* not enough energy */
    if (!first_free_bay(g))
        return 0x12BA;                   /* "YOUR FLEET IS AT A MAXIMUM" */
    return 0;
}

void craft_name_step(Game *g, uint32_t a)                  /* $242E */
{
    uint8_t d0 = (uint8_t)(g8(g, a) + 1), d1 = g8(g, a - 1);
    if (d0 == ':') {
        d0 = '0';
        if (d1 == '.')
            d1 = '0';
        d1++;
        if (d1 == ':')
            d1 = '.';
    }
    s8(g, a, d0);
    s8(g, a - 1, d1);
}

uint32_t craft_buy(Game *g, uint32_t tpl, const uint8_t *name)
{
    uint32_t b = first_free_bay(g);
    if (!b)
        return 0;
    /* $23D6: the default name, edited by the player, then the counter steps */
    for (int i = 0; i < 9; i++)
        s8(g, b + B_NAME + i, name ? name[i] : g8(g, tpl + T_NAME + i));
    craft_name_step(g, tpl + T_NAME + 8);

    b = first_free_bay(g);                                 /* $9CD8 */
    uint8_t n = bay_number(b);
    uint32_t sb = starbase(g);
    if (g8(g, sb + 0x18) == 0)
        s8(g, sb + 0x18, n);
    else if (g8(g, sb + 0x1C) == 0)
        s8(g, sb + 0x1C, n);
    else
        s8(g, sb + 0x20, n);
    s16(g, b + B_FOOD, 0);
    s16(g, b + B_SCRAP, g16(g, tpl + T_SCRAP));
    s16(g, b + B_MINERALS, 0);
    s16(g, b + B_FUELS, 0);
    s16(g, b + B_ENERGY, 0);
    s16(g, b + B_CREW, 0);
    s16(g, b + B_TRAVEL, 0);
    s8(g, b + B_TYPE, g8(g, tpl + T_TYPE));
    s8(g, b + B_FLAGS, 0);
    s8(g, b + B_RANGE, g8(g, tpl + T_RANGE));
    s8(g, b + B_LOC, g8(g, G_SB_INDEX));
    s16(g, b + B_FORMAT, 0);
    s16(g, b + B_CIVIL, 0);
    s16(g, b + B_TANK, 0);
    s32(g, sb + P_CREDITS, g32(g, sb + P_CREDITS) - g16(g, tpl + T_PRICE));
    uint8_t level = g8(g, G_LEVEL);
    if (level >= 2)
        s16(g, sb + P_MINERALS, (uint16_t)(g16(g, sb + P_MINERALS) - g16(g, tpl + T_MINERALS)));
    if (level != 0)
        s16(g, sb + P_ENERGY, (uint16_t)(g16(g, sb + P_ENERGY) - g16(g, tpl + T_ENERGY)));
    return b;
}

/* ------------------------------------------------------------ fleet screen */

uint32_t craft_launch(Game *g, uint32_t b, uint32_t screen_planet)
{
    if (!b) { redraw(g, 0x2000); return 0x16FB; }          /* "PICK A SHIP FIRST!" */
    uint8_t fl = g8(g, b + B_FLAGS);
    if (fl & BF_TRANSIT) { redraw(g, 0x2000); return 0x170E; }
    if (g8(g, b + B_TYPE) == 4) { redraw(g, 0x2000); return 0x179D; }  /* "CONTROL ATMOSPHERE PROCESSORS..." */
    if (fl & BF_ORBIT) { redraw(g, 0x2000); return 0x1AD6; }
    if (fl & BF_SURFACE) { redraw(g, 0x2000); return 0x18E1; }         /* "PUT SHIP IN A DOCKING BAY FIRST!" */
    uint32_t t = craft_template(g, b);
    int nuclear = g16(g, t + T_FUEL) == 0xFFFF;
    if (!nuclear && g16(g, b + B_TANK) < 100) { redraw(g, 0x2000); return 0x1817; }  /* "NEED MORE FUEL!" */
    if (g16(g, t + T_CREW) != 0xFFFE && g16(g, b + B_CREW) == 0) { redraw(g, 0x2000); return 0x1802; }
    slot_clear(g, g->fix_bugs ? craft_location(g, b) : screen_planet, b);   /* fixes 1, 3 */
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) | BF_ORBIT));
    if (!(g->fix_bugs && nuclear))                                         /* fix 2 */
        s16(g, b + B_TANK, (uint16_t)(g16(g, b + B_TANK) - 100));
    redraw(g, 0x3000);
    sound(g, 14);
    return 0;
}

uint32_t craft_dock(Game *g, uint32_t b, uint32_t screen_planet)
{
    if (!b) { redraw(g, 0x2000); return 0x16FB; }
    uint8_t fl = g8(g, b + B_FLAGS);
    if (fl & BF_TRANSIT) { redraw(g, 0x2000); return 0x170E; }
    if (!(fl & BF_ORBIT)) { redraw(g, 0x2000); return 0x174F; }    /* "IT'S ALREADY ON PLANET!" */
    if (g8(g, craft_location(g, b) + P_TYPE) == 0) { redraw(g, 0x2000); return 0x1AF0; }  /* dead planet */
    uint32_t slot = slot_free(g, g->fix_bugs ? craft_location(g, b) : screen_planet);    /* fix 3 */
    if (!slot) { redraw(g, 0x2000); return 0x172B; }             /* "ALL DOCKING BAYS ARE FULL" */
    s8(g, slot, bay_number(b));
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) & ~BF_ORBIT));
    redraw(g, 0x3000);
    sound(g, 15);
    return 0;
}

uint32_t craft_fly_check(Game *g, uint32_t b)
{
    if (!b) { redraw(g, 0x2000); return 0x16FB; }
    if (g8(g, b + B_FLAGS) & BF_TRANSIT) { redraw(g, 0x2000); return 0x170E; }  /* "SHIP IS IN TRANSIT" */
    if (!(g8(g, b + B_FLAGS) & BF_ORBIT)) { redraw(g, 0x2000); return 0x16E5; } /* "SHIP IS NOT IN ORBIT!" */
    return 0;
}

uint32_t craft_fly_to(Game *g, uint32_t b, uint32_t dest)
{
    uint8_t diff = (uint8_t)(g8(g, dest + P_INDEX) - g8(g, b + B_LOC));
    if (diff == 0) { redraw(g, 0x2000); return 0x17D0; }    /* "SHIP IS ALREADY THERE!" */
    if (diff & 0x80)
        diff = (uint8_t)-diff;
    uint16_t d = (uint16_t)(int16_t)(int8_t)diff;
    s16(g, G_DAYS_TMP, d);
    uint16_t need = (uint16_t)((uint32_t)d * 25u);
    if (!g8(g, G_VALVE))
        need = (uint16_t)(need + need);
    if (g16(g, craft_template(g, b) + T_FUEL) != 0xFFFF && need > g16(g, b + B_TANK)) {
        redraw(g, 0x3000);
        return 0x1817;                                       /* "NEED MORE FUEL!" */
    }
    s8(g, G_FLOWN, 0xFF);
    s16(g, b + B_TRAVEL, g16(g, G_DAYS_TMP));
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) | BF_TRANSIT));
    s32(g, b + B_DEST, dest);
    redraw(g, 0x3000);
    return 0;
}

uint32_t craft_stop(Game *g, uint32_t b)
{
    if (!b || !(g8(g, b + B_FLAGS) & BF_TRANSIT))
        return CRAFT_NOTHING;
    if (g->fix_bugs && (g8(g, b + B_FLAGS) & BF_FORMAT))   /* fix 4 */
        return CRAFT_NOTHING;
    uint8_t loc = g8(g, b + B_LOC);
    uint16_t idx = (uint16_t)((g32(g, b + B_DEST) - g32(g, G_PLANET_TABLE)) / PLANET_SIZE);
    if (loc < (uint8_t)idx)
        idx = (uint16_t)(idx - g16(g, b + B_TRAVEL));
    else
        idx = (uint16_t)(idx + g16(g, b + B_TRAVEL));
    redraw(g, 0x3000);
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) & ~BF_TRANSIT));
    s8(g, b + B_LOC, (uint8_t)idx);
    sound(g, 13);
    return 0;
}

void craft_rename(Game *g, uint32_t b, const uint8_t *name)
{
    for (int i = 0; i < 9; i++)
        s8(g, b + B_NAME + i, name[i]);
}

/* ------------------------------------------------------------ cargo screen */

static const uint8_t CARGO_BAY[4] = { B_FOOD, B_MINERALS, B_FUELS, B_ENERGY };
static const uint8_t CARGO_PLANET[4] = { P_FOOD, P_MINERALS, P_FUELS, P_ENERGY };

void craft_load(Game *g, uint32_t b, uint32_t planet, int what)
{
    /* $7F24: room in the hold, at most 5 */
    int16_t room = (int16_t)g16(g, craft_template(g, b) + T_PAYLOAD);
    uint16_t held = (uint16_t)(g16(g, b + B_FOOD) + g16(g, b + B_MINERALS) +
                               g16(g, b + B_FUELS) + g16(g, b + B_ENERGY));
    if (room <= (int16_t)held) {
        s8(g, G_HOLD_FULL, 0xFF);
        return;
    }
    uint16_t n = (uint16_t)((uint16_t)room - held);
    if (n >= 6)
        n = 5;
    s8(g, G_HOLD_FULL, 0);
    s8(g, G_IDLE, 0);
    uint32_t pa = planet + CARGO_PLANET[what], ba = b + CARGO_BAY[what];
    if (n >= g16(g, pa))
        n = g16(g, pa);
    s16(g, pa, (uint16_t)(g16(g, pa) - n));
    s16(g, ba, (uint16_t)(g16(g, ba) + n));
}

void craft_unload(Game *g, uint32_t b, uint32_t planet, int what)
{
    s8(g, G_IDLE, 0);
    uint32_t pa = planet + CARGO_PLANET[what], ba = b + CARGO_BAY[what];
    uint16_t n = 5;
    if ((int16_t)g16(g, ba) < 5)
        n = g16(g, ba);
    if ((int16_t)g16(g, pa) >= (int16_t)CAP)
        return;
    s16(g, ba, (uint16_t)(g16(g, ba) - n));
    s16(g, pa, (uint16_t)(g16(g, pa) + n));
}

void craft_load_civilians(Game *g, uint32_t b, uint32_t planet)
{
    s8(g, G_IDLE, 0);
    uint16_t seats = g16(g, craft_template(g, b) + T_SEATS);
    uint16_t on = g16(g, b + B_CIVIL);
    uint16_t n = 5;
    if ((int16_t)(uint16_t)(on + 5) > (int16_t)seats)
        n = (uint16_t)(seats - on);
    if ((int16_t)g16(g, planet + P_POP) < (int16_t)n)
        n = g16(g, planet + P_POP);
    s16(g, planet + P_POP, (uint16_t)(g16(g, planet + P_POP) - n));
    s16(g, b + B_CIVIL, (uint16_t)(on + n));
}

void craft_unload_civilians(Game *g, uint32_t b, uint32_t planet)
{
    s8(g, G_IDLE, 0);
    uint16_t n = 5;
    if ((int16_t)g16(g, b + B_CIVIL) < 5)
        n = g16(g, b + B_CIVIL);
    s16(g, b + B_CIVIL, (uint16_t)(g16(g, b + B_CIVIL) - n));
    if (g16(g, planet + P_POP) < CAP)
        s16(g, planet + P_POP, (uint16_t)(g16(g, planet + P_POP) + n));
    else
        s16(g, b + B_CIVIL, (uint16_t)(g16(g, b + B_CIVIL) + n));   /* they stay aboard */
}

void craft_fuel(Game *g, uint32_t b, uint32_t planet, int empty)
{
    s8(g, G_FUEL_EMPTY, empty ? 0xFF : 0);
    uint16_t cap = g16(g, craft_template(g, b) + T_FUEL);
    if (cap == 0xFFFF)
        return;                                              /* nuclear */
    s8(g, G_IDLE, 0);
    /* the rate: 1 a step, faster while the button is held on consecutive frames */
    uint8_t last = g8(g, G_FUEL_LAST);
    s8(g, G_FUEL_LAST, g8(g, G_FRAME));
    if ((uint8_t)(last + 1) != g8(g, G_FRAME)) {
        s8(g, G_FUEL_RATE, 12);
        s8(g, G_FUEL_WAIT, 0);
    }
    if (g8(g, G_FUEL_WAIT)) {
        s8(g, G_FUEL_WAIT, (uint8_t)(g8(g, G_FUEL_WAIT) - 1));
        return;
    }
    uint8_t r = g8(g, G_FUEL_RATE);
    if (r)
        r--;
    s8(g, G_FUEL_RATE, r);
    s8(g, G_FUEL_WAIT, r);
    uint16_t n = (uint16_t)(12 - r);
    uint16_t tank = g16(g, b + B_TANK), fuels = g16(g, planet + P_FUELS);
    if (!empty) {
        if ((int16_t)n > (int16_t)fuels)
            n = fuels;
        if ((uint16_t)(cap - n) < tank)
            n = (uint16_t)(cap - tank);
        s16(g, planet + P_FUELS, (uint16_t)(fuels - n));
        s16(g, b + B_TANK, (uint16_t)(tank + n));
    } else {
        if ((int16_t)n > (int16_t)tank)
            n = tank;
        if ((uint16_t)(CAP - n) < fuels)
            n = (uint16_t)(CAP - fuels);
        s16(g, b + B_TANK, (uint16_t)(tank - n));
        s16(g, planet + P_FUELS, (uint16_t)(fuels + n));
    }
}

uint32_t craft_assign_crew(Game *g, uint32_t b, uint32_t planet)
{
    uint16_t crew = g16(g, craft_template(g, b) + T_CREW);
    if (crew == 0xFFFE)
        return 0x1A52;                                       /* "NO CREW IS NEEDED!" */
    if (g16(g, b + B_CREW))
        return 0x0F87;                                       /* "SHIP ALREADY HAS A CREW!" */
    if ((int16_t)g16(g, planet + P_POP) < (int16_t)crew)
        return 0x0FB3;                                       /* "NEED MORE PEOPLE TO CREW!" */
    s16(g, planet + P_POP, (uint16_t)(g16(g, planet + P_POP) - crew));
    s16(g, b + B_CREW, crew);
    return 0x0FA0;                                           /* "CREW NOW ON BOARD!" */
}

void craft_unload_all(Game *g, uint32_t b, uint32_t planet, uint32_t limit)
{
    static const uint8_t PL[5] = { P_FOOD, P_MINERALS, P_FUELS, P_ENERGY, P_POP };
    static const uint8_t BY[5] = { B_FOOD, B_MINERALS, B_FUELS, B_ENERGY, B_CIVIL };
    if (g->fix_bugs)
        limit = planet;                                      /* fix 6 */
    int k = 0;
    for (;;) {
        for (; k < 5; k++) {
            if (g16(g, limit + PL[k]) >= CAP)
                continue;
            if (k < 4)
                craft_unload(g, b, planet, k);
            else
                craft_unload_civilians(g, b, planet);
        }
        if (g16(g, G_BARS) != 0x49)                          /* $306A */
            s16(g, G_BARS, (uint16_t)(g16(g, G_BARS) + 1));
        s8(g, G_FRAME, (uint8_t)(g8(g, G_FRAME) + 1));
        for (k = 0; k < 5; k++)
            if (g16(g, limit + PL[k]) < CAP && g16(g, b + BY[k]))
                break;
        if (k == 5)
            return;
    }
}

void craft_scrap(Game *g, uint32_t b, uint32_t planet, uint32_t limit)
{
    craft_unload_all(g, b, planet, limit);
    uint32_t loc = craft_location(g, b);
    for (int k = 0; k < 24; k++) {                           /* platoons aboard go ashore */
        uint32_t p = PLATOONS + (uint32_t)k * 12;
        if (g32(g, p + 2) == b) {
            s32(g, p + 2, loc);
            s8(g, p + 6, (uint8_t)(g8(g, p + 6) & ~0x04));
        }
    }
    s32(g, planet + P_CREDITS, g32(g, planet + P_CREDITS) + g16(g, b + B_SCRAP));
    s16(g, planet + P_FUELS, (uint16_t)(g16(g, planet + P_FUELS) + g16(g, b + B_TANK)));
    s16(g, b + B_TANK, 0);
    if (g16(g, planet + P_FUELS) >= CAP)
        s16(g, planet + P_FUELS, CAP);
    uint16_t crew = g16(g, b + B_CREW);
    if (crew != 0xFFFE) {
        s16(g, planet + P_POP, (uint16_t)(g16(g, planet + P_POP) + crew));
        if (g16(g, planet + P_POP) >= CAP)
            s16(g, planet + P_POP, CAP);
    }
    s16(g, b + B_CREW, 0);
    uint32_t t = craft_template(g, b);
    s16(g, planet + P_ENERGY, (uint16_t)(g16(g, planet + P_ENERGY) + g16(g, t + T_ENERGY)));
    s16(g, planet + P_MINERALS, (uint16_t)(g16(g, planet + P_MINERALS) + g16(g, t + T_MINERALS)));
    slot_clear(g, planet, b);                                /* fix 1 */
    s8(g, b + B_TYPE, 0);
}

/* ------------------------------------------------------------ surface screen */

int craft_surface_list(Game *g, uint32_t planet, uint32_t out[6])
{
    uint8_t idx = g8(g, planet + P_INDEX);
    int n = 0;
    for (int k = 0; k < 6; k++)
        out[k] = 0;
    for (int i = 0; i < BAY_COUNT; i++) {
        uint32_t b = BAY_TABLE + (uint32_t)i * BAY_SIZE;
        if (g8(g, b + B_TYPE) && g8(g, b + B_LOC) == idx && (g8(g, b + B_FLAGS) & BF_SURFACE)) {
            if (n < 6)
                out[n] = b;
            n++;
        }
    }
    return n;
}

uint32_t craft_land(Game *g, uint32_t planet, int slot)
{
    uint32_t a = planet + P_SLOT1 + 4u * (uint32_t)slot;
    uint8_t n = g8(g, a);
    if (n == 0)
        return 0x0F13;                                       /* "THAT'S EMPTY!" */
    uint32_t list[6];
    if (craft_surface_list(g, planet, list) >= 6)
        return CRAFT_NOTHING;                                /* the surface is full */
    s8(g, a, 0);
    uint32_t b = BAY_TABLE + (uint32_t)(n - 1) * BAY_SIZE;
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) | BF_SURFACE));
    return 0;
}

uint32_t craft_take_off(Game *g, uint32_t planet, uint32_t b)
{
    if (!b)
        return CRAFT_NOTHING;
    uint32_t slot = slot_free(g, planet);
    if (!slot)
        return 0x172B;                                       /* "ALL DOCKING BAYS ARE FULL" */
    s8(g, slot, bay_number(b));
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) & 0xCF));
    return 0;
}

uint32_t craft_toggle_running(Game *g, uint32_t b)
{
    if (!b)
        return CRAFT_NOTHING;
    uint8_t t = g8(g, b + B_TYPE);
    if (t != 6 && t != 7)
        return CRAFT_NOTHING;
    if (g16(g, b + B_CREW) == 0)
        return 0x1802;                                       /* "ASSIGN A CREW FIRST!" */
    s8(g, b + B_FLAGS, (uint8_t)(g8(g, b + B_FLAGS) ^ BF_RUNNING));
    return 0;
}

/* ------------------------------------------------------------ format icon */

static uint32_t processor(Game *g)
{
    for (int i = 0; i < BAY_COUNT; i++) {
        uint32_t b = BAY_TABLE + (uint32_t)i * BAY_SIZE;
        if (g8(g, b + B_TYPE) == 4)
            return b;
    }
    return 0;
}

static void post_days(Game *g, uint32_t number_msg, uint32_t days_addr)
{
    post_message(g, number_msg);
    post_message(g, g16(g, days_addr) == 1 ? 0x0D28 : 0x0D2D);   /* " DAY" / " DAYS" */
}

uint32_t craft_format_check(Game *g, uint32_t planet)
{
    uint32_t b = processor(g);
    if (!b) {
        post_message(g, 0x1646);                             /* "YOU NEED AN ATMOSPHERE PROCESSOR!" */
        post_message(g, 0x1639);                             /* "ABORTED" */
        return CRAFT_NOTHING;
    }
    if (g8(g, b + B_FLAGS) & BF_FORMAT) {                    /* progress report */
        post_message(g, 0x0D9B);
        if (g16(g, b + B_TRAVEL) == 0) {
            post_days(g, 0xD56B, G_FMT_DAYS);
            post_message(g, 0x0DB1);
        } else {
            post_days(g, 0xD56F, G_FMT_TRAVEL);
            post_message(g, 0x0DF5);
        }
        post_message(g, 0x0DDD);
        return CRAFT_NOTHING;
    }
    uint16_t owner = g16(g, planet + P_OWNER);
    if (owner != 4) {
        post_message(g, owner == 1 ? 0x1C7E : 0x1608);       /* enemy formatting / already alive */
        return CRAFT_NOTHING;
    }
    s16(g, G_FMT_DAYS, (uint16_t)(g16(g, planet + P_FMTTIME) / 250u));
    uint8_t diff = (uint8_t)(g8(g, planet + P_INDEX) - g8(g, b + B_LOC));
    if (diff & 0x80)
        diff = (uint8_t)-diff;
    s16(g, G_FMT_TRAVEL, (uint16_t)(int16_t)(int8_t)diff);
    post_message(g, 0x0D04);                                 /* "TERRA FORMATTING / STAR DISTANCE IS" */
    post_days(g, 0xD56F, G_FMT_TRAVEL);
    post_message(g, 0x0D73);                                 /* "'DOUBLE DENSITY' FORMAT TIME APPROX:" */
    post_days(g, 0xD56B, G_FMT_DAYS);
    post_message(g, 0x0D33);                                 /* "PRESS ESC TO ABORT!" */
    return 0;
}

void craft_format_start(Game *g, uint32_t planet, const uint8_t *name)
{
    /* $2390: the default name, edited by the player, then its counter steps */
    for (int i = 0; i < 9; i++)
        s8(g, planet + P_NAME + i, name ? name[i] : g8(g, G_PLANET_NAME_DEFAULT + i));
    craft_name_step(g, G_PLANET_NAME_DEFAULT + 8);

    uint32_t b = processor(g);
    if (!b)
        return;
    s32(g, b + B_DEST, planet);
    s16(g, b + B_FORMAT, g16(g, G_FMT_DAYS));
    s8(g, b + B_FLAGS, BF_FORMAT | BF_ORBIT | BF_TRANSIT);
    s16(g, b + B_TRAVEL, g16(g, G_FMT_TRAVEL));
    s8(g, G_FORMATTED, 0xFF);
    slot_clear(g, craft_location(g, b), b);                  /* fix 1 */
    post_message(g, 0x0DC6);                                 /* "FORMATTER IN TRANSIT!" */
    s32(g, G_FMT_TARGET, planet);
    copy9(g, MSG_FORMAT_NAME, planet + P_NAME);
    s16(g, planet + P_OWNER, 1);                             /* being formatted */
}

void craft_format_abort(Game *g, uint32_t planet)
{
    static const uint8_t lifeless[9] = { 'L', 'I', 'F', 'E', 'L', 'E', 'S', 'S', '!' };
    for (int i = 0; i < 9; i++)
        s8(g, planet + P_NAME + i, lifeless[i]);
    post_message(g, 0x1639);                                 /* "ABORTED" */
}

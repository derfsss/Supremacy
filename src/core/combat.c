/*
 * Platoons and ground combat. See combat.h.
 * Translated from the 68000 code (addresses in the comments are the
 * original's). Checked against the original game running in an emulator.
 *
 * Original bugs fixed when g->fix_bugs is set (the checks run with 0):
 *   F1  $9508 conquest bonus: the +7 training went to platoons on the planet
 *       on show ($E3D4), not to the winners on P. Fixed: platoons on P.
 *   F2  $91E2 (combat-screen entry pass) did not set $E3E4, so a conquest on
 *       entry wrote owner 10, the message name and the captured craft to the
 *       planet of the last daily pass. Fixed: $E3E4 = P first.
 *
 * Kept as the WHDLoad version (V1.1 slave) plays: the port matches the
 * WHDLoad version, which fixes only two bugs. These three are deliberate-looking guards or a
 * judgement call rather than clear bugs, so the port plays them as WHDLoad:
 *   (was F3) $9342 capture is put off while the player is on screens 1-4 or
 *       6 (screen 4 always, others when P is the planet on show).
 *   (was F4) Troops-screen guard: with $E425 = 1 the wipe loops in
 *       $91EA/$9342 stop at the selected platoon ($E3A4).
 *   (was F5) $5786 captured craft: fuel is rand(template +$0E, the energy
 *       cost); the long at +$0C goes to the RNG, which uses its low word.
 */
#include <stddef.h>

#include "combat.h"
#include "enemy.h"                 /* random_name_at ($5AAC) */

/* ---- addresses ---- */
#define G_SCREEN      0xE425u     /* u8 current screen */
#define G_SHOWN       0xE3D4u     /* u32 planet on show */
#define G_MAINPLANET  0xE3DCu     /* u32 main-screen / combat planet */
#define G_CRAFT       0xE3D8u     /* u32 craft selected on the combat screen */
#define G_CURPLANET   0xE3E4u     /* u32 planet of the ground pass */
#define G_LOSTPLANET  0xE398u     /* u32 planet being captured */
#define G_RANKPLAT    0xE3A8u     /* u32 platoon for the rank text / conquered planet */
#define G_SELPLAT     0xE3A4u     /* u32 selected platoon */
#define G_EQUIPCOST   0xE3A0u     /* u32 equip cost */
#define G_SELINDEX    0xE420u     /* u8 selected platoon index */
#define G_HOTSPOTS    0xE400u     /* u16 active hotspot count */
#define G_HOTTABLE    0xE3ECu     /* u32 hotspot table */
#define G_PANEL       0xE421u     /* u8 troops panel state */
#define G_MOUSELATCH  0xE422u     /* u8 */
#define G_LOOPCOUNT   0xE42Cu     /* u8 screen-loop passes */
#define G_LASTPRESS   0xE44Au     /* u8 auto-repeat */
#define G_REPDELAY    0xE44Bu
#define G_REPWAIT     0xE44Cu
#define G_DISMISS     0xE44Du
#define G_TRAINLAMP   0xE435u
#define G_FREEBAY     0xE437u     /* u8 $FF: no free bay */
#define G_WARFLAG     0xE449u
#define G_ANIMPARAM   0xE44Eu     /* soldier animation parameter */
#define G_VIEWSCRIPT  0xE3F0u     /* viewer animation script */
#define G_VIEWWAIT    0xE3F8u
#define G_VIEWFRAME   0xE42Eu
#define G_VIEW2SCRIPT 0xE3F4u     /* troops screen's second viewer */
#define G_VIEW2WAIT   0xE3FAu
#define G_VIEW2PARAM  0xE42Fu
#define G_VIEW2FRAME  0xE430u
#define G_BARSCALE    0xE406u
#define G_PLATLIST    0xEC74u     /* 24 longs: platoons at $E3D4 */
#define G_LEVEL       0xE43Bu
#define G_NUMPLANETS  0xE40Eu
#define G_STARBASE    0xE410u     /* u16 index of the player's starbase */
#define F_RECRUITED   0xD456u
#define F_EQUIPPED    0xD458u
#define F_LOADED      0xD45Au

#define MSG_WAR       0xD675u     /* "$$$$ WAR UPDATE $$$$" */
#define MSG_TAKEN     0xD691u     /* "ENEMY HAS TAKEN PLANET : " */
#define NAME_TAKEN    0xD6AAu
#define MSG_WON       0xD6B7u     /* "YOU'VE CONQUERED PLANET :" */
#define NAME_WON      0xD6D0u
#define MSG_FORMAT    0x1038u     /* outside the block (text only) */
#define MSG_DENIED    0x1919u

#define P_SLOT1   0x18
#define P_SLOT2   0x1C
#define P_SLOT3   0x20
#define P_STRENGTH 0x22
#define P_ENEMY   0x26
#define P_INDEX   0x28
#define P_AGGR    0x29
#define P_TROOPS  0x2E

#define T_FARMING 0xEDF4u          /* craft templates (in the block) */
#define T_MINING  0xEDC4u
#define T_CARGO   0xED94u
#define T_BATTLE  0xECD4u

static CombatHooks hooks;
static unsigned fix_hits;      /* bits 1,2,4,8,16: F1..F5 condition met (for the checks) */

unsigned combat_fix_hits(int clear)
{
    unsigned h = fix_hits;
    if (clear)
        fix_hits = 0;
    return h;
}

void combat_set_hooks(const CombatHooks *h)
{
    if (h)
        hooks = *h;
    else
        hooks.viewer_scene = NULL, hooks.orbit_slots = NULL;
}

/* A block-stored pointer may be stale or 0 in the original (it then reads or
 * writes low memory); the core only touches addresses inside the block. */
static int inblk(uint32_t a, uint32_t n)
{
    return a >= STATE_BASE && a + n <= STATE_BASE + STATE_SIZE && a + n >= a;
}

static void sound(Game *g, int n)
{
    if (g->play_sound)
        g->play_sound(g, n);
}

/* 68000 divu.w: the 16-bit quotient, or on overflow the dividend unchanged
 * (callers then take its low word, as the original's andi.l / move.w do). */
static uint32_t divu(uint32_t dividend, uint16_t divisor)
{
    uint32_t q = dividend / divisor;
    if (q > 0xFFFFu)
        return dividend;
    return ((dividend % divisor) << 16) | q;
}

static uint32_t platoon(int k) { return PLATOON_TABLE + PLATOON_SIZE * (uint32_t)k; }
static uint32_t bay(int n) { return BAY_TABLE + BAY_SIZE * (uint32_t)n; }   /* n = 0..31 */

static uint32_t starbase(Game *g)          /* $3506 */
{
    return g32(g, G_PLANET_TABLE) + (uint32_t)g16(g, G_STARBASE) * PLANET_SIZE;
}

static void wipe(Game *g, uint32_t p)      /* troops, location, flags, training */
{
    s16(g, p, 0);
    s32(g, p + 2, 0);
    s8(g, p + 6, 0);
    s8(g, p + 7, 0);
}

/* $E425 = 1 and this is the selected platoon: the wipe loops stop (F4). */
static int guard_stop(Game *g, uint32_t p)
{
    if (g8(g, G_SCREEN) != 1 || g32(g, G_SELPLAT) != p)
        return 0;
    fix_hits |= 8;
    return 1;                                   /* kept: WHDLoad behaviour (see the header) */
}

/* ---------------------------------------------------------------- $4E28 */
/* The main-screen viewer scene for $E3DC. State part only: the viewer's
 * animation registers. The scene table ($FED2, 14 bytes per planet type;
 * $FF26 for a planet being formatted) and the first step of each script
 * ($4F0E: every script starts with a picture, so the script pointer ends
 * 4 bytes on) are static image data, copied here. */
static const uint32_t scene_script[7] = { 0xFD8A, 0xFD02, 0xFE12, 0xFEB2, 0xFEBA, 0xFA9E, 0xFDBE };
static const uint8_t  scene_param[7]  = { 5, 4, 5, 6, 7, 7, 5 };

static void viewer_scene(Game *g)
{
    if (g8(g, G_SCREEN) != 0)
        return;
    s16(g, G_VIEWWAIT, 0);
    s8(g, G_VIEWFRAME, 0);
    s32(g, G_VIEWSCRIPT, 0);
    if (hooks.viewer_scene)
        hooks.viewer_scene(g);
    uint32_t a = g32(g, G_MAINPLANET);
    if (!inblk(a, PLANET_SIZE))
        return;
    uint16_t owner = g16(g, a + P_OWNER);
    if (owner == 7)
        return;                                 /* enemy planet: text only */
    int e = owner == 1 ? 6 : g8(g, a + P_TYPE);
    if (e > 6)
        return;                                 /* not a real planet type */
    s8(g, G_ANIMPARAM, scene_param[e]);
    s32(g, G_VIEWSCRIPT, scene_script[e] + 4);  /* $4F0E: first picture shown */
}

/* $5AAC: a random name in the 9 bytes at a (enemy.c) */
static void random_name(Game *g, uint32_t a)
{
    random_name_at(g, a);
}

/* ---------------------------------------------------------------- $99D6 */
/* A planet the enemy has taken gets new stocks and a new name. */
static void reseed_planet(Game *g, uint32_t a)
{
    s16(g, a + P_POP, rng_below(g, 4500));
    s16(g, a + P_FOOD, (uint16_t)(rng_below(g, 700) + 300));
    s16(g, a + P_MINERALS, (uint16_t)(rng_below(g, 1000) + 200));
    s16(g, a + P_FUELS, (uint16_t)(rng_below(g, 900) + 350));
    s16(g, a + P_ENERGY, (uint16_t)(rng_below(g, 700) + 100));
    s32(g, a + P_CREDITS, (uint16_t)(rng_below(g, 2000) + 5000));   /* addi.w: 16-bit */
    s8(g, a + P_TAX, 25);
    s8(g, a + P_GROWTH, 30);
    s8(g, a + P_DECLINE, 15);
    s8(g, a + P_MORALE, 75);
    random_name(g, a + P_NAME);
    viewer_scene(g);                            /* $4E28 when $E425 = 0 */
}

/* ---------------------------------------------------------------- $5786 */
static uint32_t free_bay(Game *g)           /* $6530: first bay with no craft, or 0 */
{
    s8(g, G_FREEBAY, 0);
    for (int n = 0; n < NUM_BAYS; n++)
        if (g8(g, bay(n) + 0x1E) == 0)
            return bay(n);
    s8(g, G_FREEBAY, 0xFF);
    return 0;
}

static void captured_craft_one(Game *g)     /* $57B0 */
{
    uint32_t b = free_bay(g);
    if (!b)
        return;
    uint32_t a = g32(g, G_CURPLANET);
    if (!inblk(a, PLANET_SIZE))
        return;                                 /* stale $E3E4 = 0 (original reads low memory) */
    s8(g, b + 0x21, g8(g, a + P_INDEX));
    uint32_t t = T_FARMING;
    s8(g, b + 0x1F, 0x30);
    if (g8(g, a + P_TYPE) != 4) {
        t = T_MINING;
        if (g8(g, a + P_TYPE) != 5) {
            t = (rng_next(g) & 1) ? T_BATTLE : T_CARGO;
            s8(g, b + 0x1F, 0x10);
        }
    }
    s16(g, b + 0x0A, 0);
    s16(g, b + 0x0C, g16(g, t + 0x08));
    s16(g, b + 0x0E, 0);
    s16(g, b + 0x10, 0);
    s16(g, b + 0x12, 0);
    s16(g, b + 0x14, g16(g, t + 0x0A));
    s16(g, b + 0x18, 0);
    s8(g, b + 0x1E, g8(g, t + 0x12));
    s8(g, b + 0x20, g8(g, t + 0x13));
    s16(g, b + 0x22, 0);
    s16(g, b + 0x24, 0);
    if (g16(g, t + 0x0C) != g16(g, t + 0x0E))
        fix_hits |= 16;
    /* the long at +$0C is passed to $9DB4, which uses its low word (+$0E) */
    s16(g, b + 0x26, rng_below(g, g16(g, t + 0x0E)));   /* kept: WHDLoad behaviour */
    random_name(g, b);                          /* $5AAC: name at bay +$00 */
}

static void captured_craft(Game *g)         /* $5786 */
{
    uint16_t k = (uint16_t)(rng_next(g) & 7);
    k = (uint16_t)(k - (rng_next(g) & 3));
    if ((uint8_t)k >= 5)
        return;
    for (int i = 0; i <= k; i++)
        captured_craft_one(g);
}

/* ---------------------------------------------------------------- $9508 */
static void conquer(Game *g, uint32_t a)
{
    s16(g, a + P_ENEMY, 0);
    if (g16(g, a + P_OWNER) == OWNER_PLAYER)
        return;
    s32(g, G_RANKPLAT, a);
    uint32_t where = g->fix_bugs ? a : g32(g, G_SHOWN);                        /* F1 */
    if (a != g32(g, G_SHOWN))
        fix_hits |= 1;
    for (int k = 0; k < NUM_PLATOONS; k++) {
        uint32_t p = platoon(k);
        uint8_t f = g8(g, p + 6);
        if (!(f & 8) && (f & 2) && g32(g, p + 2) == where && g8(g, p + 7) < 0x5D)
            s8(g, p + 7, (uint8_t)(g8(g, p + 7) + 7));
    }
    or32(g, G_REDRAW, 0x80018);
    captured_craft(g);
    uint32_t c = g32(g, G_CURPLANET);           /* the original uses $E3E4 from here on */
    if (inblk(c, PLANET_SIZE))
        for (int i = 0; i < 9; i++)
            s8(g, NAME_WON + i, g8(g, c + P_NAME + i));
    post_message(g, MSG_WAR);
    post_message(g, MSG_WON);
    if (inblk(c, PLANET_SIZE))
        s16(g, c + P_OWNER, OWNER_PLAYER);
    s8(g, G_WARFLAG, 0xFF);
    sound(g, 0x1F);
    if (c == g32(g, G_SHOWN))
        viewer_scene(g);
}

/* ---------------------------------------------------------------- $9342 */
static void lost(Game *g, uint32_t a)
{
    s16(g, a + P_STRENGTH, 0);
    s16(g, a + P_TROOPS, 0);
    if (g16(g, a + P_OWNER) == 7)
        return;
    uint8_t scr = g8(g, G_SCREEN);
    if (scr != 0 && scr != 7 && scr != 5 && (scr == 4 || a == g32(g, G_SHOWN))) {   /* F3 */
        fix_hits |= 4;
        return;                                 /* captured on a later pass (kept) */
    }
    s32(g, G_LOSTPLANET, a);
    for (int n = 0; n < NUM_BAYS; n++) {
        uint32_t b = bay(n);
        if (g8(g, b + 0x21) != g8(g, a + P_INDEX))
            continue;
        uint8_t f = g8(g, b + 0x1F);
        if (!(f & 0x10) && (f & 4))
            continue;                           /* in flight */
        for (int k = 0; k < NUM_PLATOONS; k++) {
            uint32_t p = platoon(k);
            if (guard_stop(g, p))
                return;                         /* F4 */
            if (g32(g, p + 2) == b)
                wipe(g, p);
        }
        s8(g, b + 0x1E, 0);                     /* craft destroyed */
        uint8_t num = (uint8_t)(n + 1);
        if (g8(g, a + P_SLOT1) == num)
            s8(g, a + P_SLOT1, 0);
        else if (g8(g, a + P_SLOT2) == num)
            s8(g, a + P_SLOT2, 0);
        else if (g8(g, a + P_SLOT3) == num)
            s8(g, a + P_SLOT3, 0);
    }
    for (int i = 0; i < 9; i++)
        s8(g, NAME_TAKEN + i, g8(g, a + P_NAME + i));
    post_message(g, MSG_WAR);
    post_message(g, MSG_TAKEN);
    if (g8(g, G_SCREEN) == 7 && hooks.orbit_slots)
        hooks.orbit_slots(g);                   /* $86C4 */
    s16(g, a + P_OWNER, 7);
    reseed_planet(g, a);
    s8(g, G_WARFLAG, 0xFF);
    or32(g, G_REDRAW, 0x80018);
    sound(g, 2);
}

/* ---------------------------------------------------------------- $9206 */
static int stationed(Game *g, uint32_t p, uint32_t a)
{
    return g32(g, p + 2) == a && (g8(g, p + 6) & 2);
}

static void ground_planet(Game *g, uint32_t a)
{
    if (!inblk(a, PLANET_SIZE) || g8(g, a + P_TYPE) == 0)
        return;

    /* 1. strength = troops + (troops x training / 72) x (suit + weapon + aggression + 1) */
    uint16_t strength = 0, troops = 0;
    for (int k = 0; k < NUM_PLATOONS; k++) {
        uint32_t p = platoon(k);
        if (!stationed(g, p, a))
            continue;
        uint16_t t = g16(g, p);
        troops = (uint16_t)(troops + t);
        strength = (uint16_t)(strength + t);
        uint16_t q = (uint16_t)divu((uint32_t)g8(g, p + 7) * t, 0x48);
        uint16_t m = (uint16_t)(g16(g, p + 8) + g16(g, p + 10));
        m = (uint16_t)((m & 0xFF00) | (uint8_t)(m + g8(g, a + P_AGGR) + 1));   /* add.b, addq.b */
        strength = (uint16_t)(strength + (uint16_t)((uint32_t)q * m));
    }
    s16(g, a + P_STRENGTH, strength);
    s16(g, a + P_TROOPS, troops);
    if (a == g32(g, G_SHOWN))
        or32(g, G_REDRAW, 0x80010);

    /* 2. no enemy force */
    if (g16(g, a + P_ENEMY) == 0) {
        if (g16(g, a + P_STRENGTH) == 0)
            return;
        conquer(g, a);
        return;
    }

    /* 3. the enemy force grows, except on the enemy base (record 0) */
    if (a != g32(g, G_PLANET_TABLE)) {
        uint8_t ty = g8(g, a + P_TYPE);
        uint16_t inc = ty == 4 ? 4 : ty == 5 ? 3 : 1;
        s16(g, a + P_ENEMY, (uint16_t)(g16(g, a + P_ENEMY) + inc));
        if (g16(g, a + P_ENEMY) >= 0x3095)
            s16(g, a + P_ENEMY, 0x3095);
    }
    if (g16(g, a + P_STRENGTH) == 0) {
        if (g16(g, a + P_ENEMY) == 0)
            return;
        lost(g, a);                             /* undefended */
        return;
    }

    /* 4. battle */
    uint32_t prod = (uint32_t)g16(g, a + P_ENEMY) * (uint8_t)(2 + g8(g, a + P_AGGR));
    uint16_t enemy = (uint16_t)divu(prod, 100);
    if (enemy == 0)
        enemy = 1;
    uint16_t mine = g16(g, a + P_STRENGTH);
    if (mine <= enemy) {
        for (int k = 0; k < NUM_PLATOONS; k++) {
            uint32_t p = platoon(k);
            if (guard_stop(g, p))
                return;                         /* F4 */
            if (stationed(g, p, a))
                wipe(g, p);
        }
        lost(g, a);
        return;
    }
    uint16_t pct = (uint16_t)divu((uint32_t)(uint16_t)(mine - enemy) * 100u, mine);
    if (pct) {
        uint16_t left = 0;
        for (int k = 0; k < NUM_PLATOONS; k++) {
            uint32_t p = platoon(k);
            if (!stationed(g, p, a))
                continue;
            uint16_t t = (uint16_t)divu((uint32_t)g16(g, p) * pct, 100);
            left = (uint16_t)(left + t);
            if (t == 0) {
                s32(g, p + 2, 0);
                s8(g, p + 6, 0);
                s8(g, p + 7, 0);
                or32(g, G_REDRAW, 0x80000);
            }
            s16(g, p, t);
        }
        s16(g, a + P_TROOPS, left);
    }
    uint16_t loss = (uint16_t)divu((uint32_t)g16(g, a + P_STRENGTH) * 2u, 100);
    uint16_t force = g16(g, a + P_ENEMY);
    if (force >= loss) {
        s16(g, a + P_ENEMY, (uint16_t)(force - loss));
        return;
    }
    s16(g, a + P_ENEMY, (uint16_t)(force - loss));
    conquer(g, a);
}

/* $91EA: pass $38+N+i works on planet record i. */
void ground_forces(Game *g, uint16_t pass)
{
    uint16_t i = (uint16_t)(pass - 0x38 - g16(g, G_NUMPLANETS));
    uint32_t a = (uint32_t)i * PLANET_SIZE + g32(g, G_PLANET_TABLE);
    s32(g, G_CURPLANET, a);
    ground_planet(g, a);
}

/* $91E2: the same pass on $E3DC, run once by every combat-screen entry. */
void ground_forces_entry(Game *g)
{
    uint32_t a = g32(g, G_MAINPLANET);
    if (g32(g, G_CURPLANET) != a)
        fix_hits |= 2;
    if (g->fix_bugs)
        s32(g, G_CURPLANET, a);                 /* F2 */
    ground_planet(g, a);
}

/* ---------------------------------------------------------------- $7736 */
/* $778E: the platoon shown on the troops screen (rank text, soldier
 * animation, TRAIN lamp). An empty platoon's training is reset. */
static void show_platoon(Game *g, uint32_t p)
{
    s32(g, G_RANKPLAT, p);
    if (g16(g, p) == 0)
        s8(g, p + 7, 0);
    s8(g, G_ANIMPARAM, 0xFF);
    uint8_t tr = g8(g, p + 7);
    if (tr)
        s8(g, G_ANIMPARAM, (uint8_t)(9 - (tr >> 4)));
    s8(g, G_TRAINLAMP, 0);
    if (tr && !(g8(g, p + 6) & 2))
        s8(g, G_TRAINLAMP, 0xFF);
}

void platoon_pass(Game *g, uint16_t pass)
{
    uint32_t p = PLATOON_TABLE + (uint32_t)(uint16_t)(pass - 0x20) * PLATOON_SIZE;
    if (!inblk(p, PLATOON_SIZE) || (g8(g, p + 6) & 2))
        return;                                 /* equipped platoons don't train */
    uint8_t lv = g8(g, G_LEVEL);
    uint8_t tr = (uint8_t)(g8(g, p + 7) + 1);
    if (lv != 2) {
        tr++;
        if (lv == 0)
            tr++;
    }
    if (tr >= 100)
        tr = 100;
    s8(g, p + 7, tr);
    if (g8(g, G_SCREEN) == 1 && g32(g, G_SELPLAT) == p)
        show_platoon(g, p);
}

/* ---------------------------------------------------------------- troops screen */
static const uint16_t suit_cost[4]   = { 50, 100, 200, 295 };   /* GAM2 $59A16 +4, 30 bytes each */
static const uint16_t weapon_cost[3] = { 50, 120, 250 };        /* GAM2 $59A8E +4, 20 bytes each */
/* second-viewer animation of each suit picture (GAM2 $59A16 +$16 .. +$1D) */
static const uint32_t suit_view_script[4] = { 0, 0, 0, 0xFBDE };
static const uint8_t  suit_view_param[4]  = { 3, 4, 3, 7 };

static uint16_t kit_cost(const uint16_t *tab, int n, uint16_t i)
{
    return i < n ? tab[i] : 0;                  /* out of range only by poking */
}

static void select_ptr(Game *g)             /* $5F46: $E3A4 = platoon $E420 */
{
    s32(g, G_SELPLAT, platoon(g8(g, G_SELINDEX)));
}

uint32_t troops_equip_cost(Game *g)         /* $61FC */
{
    uint32_t p = g32(g, G_SELPLAT);
    if (!inblk(p, PLATOON_SIZE))
        return 0;
    uint16_t c = (uint16_t)(kit_cost(suit_cost, 4, g16(g, p + 8)) + kit_cost(weapon_cost, 3, g16(g, p + 10)));
    s16(g, G_HOTSPOTS, 11);
    uint32_t cost = (uint32_t)c * g16(g, p);
    s32(g, G_EQUIPCOST, cost);
    return cost;
}

static void redraw_platoon(Game *g)         /* $6106: the panel and the active hotspots */
{
    uint32_t p = g32(g, G_SELPLAT);
    if (!inblk(p, PLATOON_SIZE))
        return;
    uint8_t f = g8(g, p + 6);
    if (f & 1) {                                /* on a mission */
        if (g8(g, G_PANEL) != 2) {
            s16(g, G_HOTSPOTS, 4);
            s8(g, G_PANEL, 2);
        }
    } else if (f & 2) {                         /* equipped */
        if (g8(g, G_PANEL) != 3) {
            s16(g, G_HOTSPOTS, 4);
            s8(g, G_PANEL, 3);
        }
    } else {
        if (g8(g, G_PANEL) != 1)
            s8(g, G_PANEL, 1);
        troops_equip_cost(g);
    }
}

/* $EF2E: picture static, 118 rows of r16 calls (10 a row, 8 with $E441). */
static void picture_static(Game *g, int per_row)
{
    for (int row = 0; row < 118; row++)
        for (int i = 0; i < per_row; i++)
            rng_next(g);
}

static void suit_picture(Game *g)           /* $62A2 */
{
    for (int i = 0; i < 3; i++)
        picture_static(g, 10);
    uint16_t s = g16(g, g32(g, G_SELPLAT) + 8);
    if (s < 4) {
        s32(g, G_VIEW2SCRIPT, suit_view_script[s]);
        s16(g, G_VIEW2WAIT, 0);
        s8(g, G_VIEW2PARAM, suit_view_param[s]);
        s8(g, G_VIEW2FRAME, 0);
    }
}

static void weapon_picture(Game *g)         /* $63D8 ($E441 is set around the static) */
{
    for (int i = 0; i < 3; i++)
        picture_static(g, 8);
}

/* $31A6: the screen holds 44 passes after a message (no game time). */
static void hold(Game *g)
{
    s8(g, G_LOOPCOUNT, (uint8_t)(g8(g, G_LOOPCOUNT) + 44));
}

/* $380C / $3814: message, then hold with the pointer off ($3006 .. $301C). */
static void hold_msg(Game *g)
{
    hold(g);
    s8(g, 0xE42D, 0);
}

static void after_select(Game *g)           /* $6014..$6036 */
{
    select_ptr(g);
    suit_picture(g);
    weapon_picture(g);
    redraw_platoon(g);
    s8(g, G_VIEWFRAME, 0);
    s16(g, G_VIEWWAIT, 0);
    show_platoon(g, g32(g, G_SELPLAT));
}

void troops_select(Game *g, uint8_t index)  /* $5FE8 */
{
    if (index == g8(g, G_SELINDEX))
        return;
    s8(g, G_SELINDEX, index);
    after_select(g);
}

void troops_next(Game *g)                   /* $6082 */
{
    sound(g, 13);
    uint8_t i = (uint8_t)(g8(g, G_SELINDEX) + 1);
    if (i >= 24)
        i = 0;
    troops_select(g, i);
}

void troops_prev(Game *g)                   /* $5FC8 */
{
    sound(g, 13);
    uint8_t i = (uint8_t)(g8(g, G_SELINDEX) - 1);
    if (i >= 24)
        i = 23;
    troops_select(g, i);
}

int troops_disband(Game *g)                 /* $5C50: only at the starbase; 100 credits a soldier */
{
    sound(g, 13);
    uint32_t p = g32(g, G_SELPLAT), sb = starbase(g);
    if (!inblk(p, PLATOON_SIZE) || g32(g, p + 2) != sb)
        return CT_NOTHING;
    s32(g, sb + P_CREDITS, g32(g, sb + P_CREDITS) + (uint32_t)g16(g, p) * 100u);
    s32(g, p + 2, 0);
    s8(g, p + 6, 0);
    s8(g, G_TRAINLAMP, 0);
    select_ptr(g);
    redraw_platoon(g);                          /* $6020.. */
    s8(g, G_VIEWFRAME, 0);
    s16(g, G_VIEWWAIT, 0);
    show_platoon(g, g32(g, G_SELPLAT));
    return CT_OK;
}

int troops_equip(Game *g)                   /* $5DEA */
{
    uint32_t p = g32(g, G_SELPLAT);
    if (!inblk(p, PLATOON_SIZE) || (g8(g, p + 6) & 2) || g16(g, p) == 0)
        return CT_NOTHING;
    sound(g, 13);
    uint32_t sb = starbase(g);
    uint32_t credits = g32(g, sb + P_CREDITS), cost = g32(g, G_EQUIPCOST);
    if (cost > credits) {
        s8(g, G_PANEL, 0);                      /* $60EC */
        hold(g);
        s8(g, G_PANEL, 0);
        redraw_platoon(g);
        return CT_NOT_ENOUGH_CREDITS;
    }
    s32(g, sb + P_CREDITS, credits - cost);
    p = g32(g, G_SELPLAT);
    s8(g, p + 6, (uint8_t)(g8(g, p + 6) | 2));
    s8(g, F_EQUIPPED, 0xFF);
    s32(g, p + 2, sb);
    s8(g, G_PANEL, 0);
    s8(g, G_TRAINLAMP, 0);
    redraw_platoon(g);
    return CT_OK;
}

/* $5E92: recruit/dismiss with auto-repeat. A step on the press, then after
 * 11, 10, ... 1 skipped passes, then every pass ($E42C counts passes). */
static void recruit_dismiss(Game *g, int dismiss)
{
    s8(g, G_DISMISS, dismiss ? 0xFF : 0);
    s8(g, G_MOUSELATCH, 0);
    uint8_t last = g8(g, G_LASTPRESS);
    s8(g, G_LASTPRESS, g8(g, G_LOOPCOUNT));
    if ((uint8_t)(last + 1) != g8(g, G_LOOPCOUNT)) {
        s8(g, G_REPDELAY, 12);
        s8(g, G_REPWAIT, 0);
    }
    if (g8(g, G_REPWAIT)) {
        s8(g, G_REPWAIT, (uint8_t)(g8(g, G_REPWAIT) - 1));
        return;
    }
    uint8_t d = g8(g, G_REPDELAY);
    if (d)
        d--;
    s8(g, G_REPDELAY, d);
    s8(g, G_REPWAIT, d);
    uint32_t p = g32(g, G_SELPLAT), sb = starbase(g);
    if (!inblk(p, PLATOON_SIZE))
        return;
    if (!dismiss) {
        if (g16(g, sb + P_POP) == 0 || g16(g, p) == 200)
            return;
        s16(g, p, (uint16_t)(g16(g, p) + 1));
        s16(g, sb + P_POP, (uint16_t)(g16(g, sb + P_POP) - 1));
        s8(g, F_RECRUITED, 0xFF);
        if (g8(g, p + 7))
            s8(g, p + 7, (uint8_t)(g8(g, p + 7) - 1));
    } else {
        if (g16(g, p) == 0 || g16(g, sb + P_POP) == 30000)
            return;
        s16(g, sb + P_POP, (uint16_t)(g16(g, sb + P_POP) + 1));
        s16(g, p, (uint16_t)(g16(g, p) - 1));
    }
    s8(g, G_MOUSELATCH, 0);
    select_ptr(g);
    redraw_platoon(g);
}

void troops_recruit(Game *g) { recruit_dismiss(g, 0); }
void troops_dismiss(Game *g) { recruit_dismiss(g, 1); }

void troops_suit(Game *g, int up)           /* $6254 / $6276: 0..3, wrapping */
{
    sound(g, 13);
    uint32_t p = g32(g, G_SELPLAT);
    if (!inblk(p, PLATOON_SIZE))
        return;
    uint16_t s = (uint16_t)(g16(g, p + 8) + (up ? 1 : -1));
    if (s >= 4)
        s = up ? 0 : 3;
    s16(g, p + 8, s);
    suit_picture(g);
    redraw_platoon(g);
}

void troops_weapon(Game *g, int up)         /* $6382 / $63AC: 0..2, wrapping */
{
    sound(g, 13);
    uint32_t p = g32(g, G_SELPLAT);
    if (!inblk(p, PLATOON_SIZE))
        return;
    uint16_t w = (uint16_t)(g16(g, p + 10) + (up ? 1 : -1));
    if (w >= 3)
        w = up ? 0 : 2;
    s16(g, p + 10, w);
    weapon_picture(g);
    redraw_platoon(g);
}

/* ---------------------------------------------------------------- combat screen */
void combat_build_list(Game *g)             /* $372E */
{
    for (int i = 0; i < NUM_PLATOONS; i++)
        s32(g, G_PLATLIST + 4u * i, 0);
    uint32_t a = g32(g, G_SHOWN), w = G_PLATLIST;
    for (int k = 0; k < NUM_PLATOONS; k++) {
        uint32_t p = platoon(k);
        if (g32(g, p + 2) == a && !(g8(g, p + 6) & 8)) {
            s32(g, w, p);
            w += 4;
        }
    }
}

int combat_aboard(Game *g, uint32_t out[NUM_PLATOONS])   /* $33EC (the list is outside the block) */
{
    uint32_t c = g32(g, G_CRAFT);
    int n = 0;
    for (int k = 0; k < NUM_PLATOONS; k++) {
        uint32_t p = platoon(k);
        if (g32(g, p + 2) == c && (g8(g, p + 6) & 2)) {
            if (out)
                out[n] = p;
            n++;
        }
    }
    return n;
}

int combat_enter(Game *g)                   /* $3A2A */
{
    uint32_t a = g32(g, G_MAINPLANET);
    if (!inblk(a, PLANET_SIZE))
        return CT_NOTHING;
    uint16_t owner = g16(g, a + P_OWNER);
    if (owner != OWNER_PLAYER) {
        if (owner == 4 || owner == 1) {
            post_message(g, MSG_FORMAT);
            return CT_NEEDS_FORMATTING;
        }
        if (!(g8(g, a + P_SLOT1) | g8(g, a + P_SLOT2) | g8(g, a + P_SLOT3)) && g16(g, a + P_TROOPS) == 0) {
            if (owner == 7) {
                post_message(g, MSG_DENIED);    /* $8F0E, then back to the main screen ($60A6) */
                s8(g, 0xE43F, 0);
                s8(g, 0xE43D, 0);
                return CT_ACCESS_DENIED;
            }
            post_message(g, MSG_FORMAT);
            return CT_NEEDS_FORMATTING;
        }
    }
    sound(g, 0x1D);
    s32(g, G_SHOWN, a);
    s32(g, G_CRAFT, 0);
    s32(g, G_REDRAW, 0);
    s16(g, G_VIEWWAIT, 0);
    s32(g, G_VIEWSCRIPT, 0);
    s8(g, G_WARFLAG, 0xFF);                     /* $464A */
    s8(g, G_SCREEN, 7);
    s32(g, G_HOTTABLE, 0xF52C);
    s16(g, G_HOTSPOTS, 18);
    if (hooks.orbit_slots)
        hooks.orbit_slots(g);
    combat_build_list(g);
    /* $9714: bar scale = max(+$22, +$26) / 172, at least 1 */
    uint16_t m = g16(g, a + P_STRENGTH), e = g16(g, a + P_ENEMY);
    uint16_t sc = (uint16_t)divu(m >= e ? m : e, 0xAC);
    s16(g, G_BARSCALE, sc ? sc : 1);
    ground_forces_entry(g);
    return CT_OK;
}

void combat_aggression(Game *g, int up)     /* $3974 / $39D0 */
{
    uint32_t a = g32(g, G_SHOWN);
    if (!inblk(a, PLANET_SIZE))
        return;
    uint8_t v = g8(g, a + P_AGGR);
    if (up ? v == 3 : v == 0)
        return;
    v = (uint8_t)(up ? v + 1 : v - 1);
    s8(g, a + P_AGGR, v);
    int lv = up ? v : v + 1;                    /* the picture/sound of the level passed */
    sound(g, lv == 1 ? 0x1A : lv == 2 ? 0x1B : 0x1C);
}

int combat_select_craft(Game *g, int slot)  /* $37FC / $38AC / $38BC */
{
    static const uint8_t off[3] = { P_SLOT1, P_SLOT2, P_SLOT3 };
    uint32_t a = g32(g, slot == 0 ? G_MAINPLANET : G_SHOWN);
    if (slot < 0 || slot > 2 || !inblk(a, PLANET_SIZE))
        return CT_NOTHING;
    uint8_t n = g8(g, a + off[slot]);
    if (n == 0) {
        hold_msg(g);
        return CT_EMPTY;
    }
    uint32_t b = BAY_TABLE + (uint32_t)(uint8_t)(n - 1) * BAY_SIZE;
    if (!inblk(b, BAY_SIZE))
        return CT_NOTHING;
    if (g8(g, b + 0x1E) != 1) {
        hold_msg(g);
        return CT_CANT_CARRY;
    }
    s32(g, G_CRAFT, b);
    s8(g, 0xE434, 0xFF);
    if (hooks.orbit_slots)
        hooks.orbit_slots(g);
    return CT_OK;
}

int combat_unload(Game *g, int n)           /* $6D8C / $6D84 / $6D7C / $6D74 */
{
    uint32_t list[NUM_PLATOONS];
    int count = combat_aboard(g, list);
    if (n < 0 || n > 3 || n >= count) {
        hold_msg(g);
        return CT_EMPTY;
    }
    uint32_t p = list[n];
    s32(g, p + 2, g32(g, G_MAINPLANET));
    s8(g, p + 6, (uint8_t)(g8(g, p + 6) & 0xFB));
    combat_build_list(g);
    return CT_OK;
}

int combat_load(Game *g, int entry)         /* $8202..$824C */
{
    if (entry < 0 || entry >= NUM_PLATOONS)
        return CT_NOTHING;
    uint32_t p = g32(g, G_PLATLIST + 4u * (uint32_t)entry);
    if (p == 0) {
        hold_msg(g);
        return CT_EMPTY;
    }
    if (g32(g, G_CRAFT) == 0) {
        hold_msg(g);
        return CT_SELECT_SHIP;
    }
    if (combat_aboard(g, NULL) >= 4) {
        hold_msg(g);
        return CT_SHIP_FULL;
    }
    if (!inblk(p, PLATOON_SIZE))
        return CT_NOTHING;
    s32(g, p + 2, g32(g, G_CRAFT));
    s8(g, p + 6, (uint8_t)(g8(g, p + 6) | 4));
    s8(g, F_LOADED, 0xFF);
    combat_build_list(g);
    return CT_OK;
}

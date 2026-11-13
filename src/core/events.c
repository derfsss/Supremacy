/*
 * The event-script interpreter $B4DE and the Scorpion V4 answers. See
 * events.h.
 *
 * Bug fixes (only when g->fix_bugs; the checks run with 0):
 *   F1  KILLSHIPS (op $84) tried to clear the planet's dock slot holding the
 *       destroyed craft, but tested $EC74+$18/$1C/$20 (a2 had been reused
 *       for the platoon loop), so the slot kept pointing at the empty bay.
 *       Fixed: the planet's own +$18/+$1C/+$20.
 *   (not fixed) Script 16 (aracno-locusts) tests the year $3F6 = 1014, so
 *       it never fires. The intended year is unknown; the port plays it as
 *       the WHDLoad version does.
 */
#include "events.h"
#include "enemy.h"

#define D_PLANET    0xD444u        /* u32 event planet */
#define D_BAY       0xD448u        /* u32 event bay */
#define G_SCREEN    0xE425u
#define G_HOTSPOTS  0xE3ECu        /* u32 hotspot table */
#define G_NHOTSPOTS 0xE400u        /* u16 hotspot count */
#define MAIN_HOTSPOTS 0xF26Cu
#define YESNO_HOTSPOTS 0xF74Cu

static int in_block(uint32_t a)
{
    return a >= STATE_BASE && a < STATE_BASE + STATE_SIZE;
}

/* a byte at an original address: the block, the script data (from the
 * image), else 0 */
static uint8_t rd8(Game *g, uint32_t a, int screen)
{
    if (a == G_SCREEN)
        return (uint8_t)screen;
    if (in_block(a))
        return g8(g, a);
    if (!g->image)
        return 0;
    if (a >= EV_SCRIPTS_BASE && a - EV_SCRIPTS_BASE < EV_SCRIPTS_SIZE)
        return g->image[a];
    if (a >= EV_NAMES_BASE && a - EV_NAMES_BASE < EV_NAMES_SIZE)
        return g->image[a];
    return 0;
}

static uint16_t rd16(Game *g, uint32_t a, int sc)
{
    return (uint16_t)((rd8(g, a, sc) << 8) | rd8(g, a + 1, sc));
}

static uint32_t rd32(Game *g, uint32_t a, int sc)
{
    return ((uint32_t)rd16(g, a, sc) << 16) | rd16(g, a + 2, sc);
}

static void wr8(Game *g, uint32_t a, uint8_t v)
{
    if (in_block(a))
        s8(g, a, v);
}

static void wr16(Game *g, uint32_t a, uint16_t v)
{
    wr8(g, a, (uint8_t)(v >> 8));
    wr8(g, a + 1, (uint8_t)v);
}

static void wr32(Game *g, uint32_t a, uint32_t v)
{
    wr16(g, a, (uint16_t)(v >> 16));
    wr16(g, a + 2, (uint16_t)v);
}

static void clear_platoon(Game *g, uint32_t p)
{
    s16(g, p, 0);
    s32(g, p + 2, 0);
    s8(g, p + 6, 0);
    s8(g, p + 7, 0);
}

/* the byte-sized bset on $E39C: bit n of the long's top byte */
static void redraw_hi(Game *g, uint8_t bits)
{
    s8(g, G_REDRAW, (uint8_t)(g8(g, G_REDRAW) | bits));
}

/* op $64 DESTROYPLANET: 0 = stopped */
static int destroy_planet(Game *g, int screen)
{
    uint32_t a = g32(g, D_PLANET);
    for (int k = 0; k < NPLATOONS; k++) {
        uint32_t p = PLATOONS_ADDR + (uint32_t)k * PLATOON_SIZE;
        if (screen == 1 && p == g32(g, G_EDIT_PLATOON))
            return 0;
        if (g32(g, p + 2) == a)
            clear_platoon(g, p);
    }
    if (screen != 0 && screen != 5 && a == g32(g, G_SHOWN))
        return 0;
    s16(g, a + P_OWNER, OWNER_LIFELESS);
    s8(g, a + P_TYPE, 0);
    redraw_hi(g, 0x08);
    sup_viewer_refresh(g, screen);
    return 1;
}

/* op $84 KILLSHIPS: craft at planet [$D444] that are landed or docked (not
 * in orbit) are destroyed with the platoons aboard. 0 = stopped. */
static int kill_ships(Game *g, int screen)
{
    uint32_t a = g32(g, D_PLANET), shown = g32(g, G_SHOWN);
    if (shown == 0 || a == shown)
        return 0;
    uint8_t idx = g8(g, a + P_INDEX);
    uint8_t d5 = 1;
    for (int j = 0; j < NBAYS; j++, d5++) {
        uint32_t bay = BAYS_ADDR + (uint32_t)j * BAY_SIZE;
        uint8_t f = g8(g, bay + 0x1F);
        if (g8(g, bay + 0x21) != idx || (!(f & 0x10) && (f & 0x04)))
            continue;
        if (screen != 0 && screen != 5 && bay == g32(g, G_SHOWN_BAY))
            return 0;
        for (int k = 0; k < NPLATOONS; k++) {
            uint32_t p = PLATOONS_ADDR + (uint32_t)k * PLATOON_SIZE;
            if (g32(g, p + 2) != bay)
                continue;
            if (screen == 1 && p == g32(g, G_EDIT_PLATOON))
                return 0;
            clear_platoon(g, p);
        }
        s8(g, bay + 0x1E, 0);
        /* the original tests the bytes past the platoon table ($EC74 + off) */
        uint32_t slots = g->fix_bugs ? a : PLATOONS_ADDR + NPLATOONS * PLATOON_SIZE;
        static const uint8_t off[3] = { 0x18, 0x1C, 0x20 };
        for (int s = 0; s < 3; s++)
            if (g8(g, slots + off[s]) == d5) {
                s8(g, slots + off[s], 0);
                break;
            }
    }
    return 1;
}

/* op $88 KILLFARM: a running landed farming station not at the planet on show */
static int kill_farm(Game *g)
{
    uint32_t shown = g32(g, G_SHOWN);
    if (shown == 0)
        return 0;
    uint8_t here = g8(g, shown + P_INDEX);
    for (int j = 0; j < NBAYS; j++) {
        uint32_t bay = BAYS_ADDR + (uint32_t)j * BAY_SIZE;
        uint8_t f = g8(g, bay + 0x1F);
        if (g8(g, bay + 0x1E) == 7 && bay != g32(g, G_SHOWN_BAY) && g8(g, bay + 0x21) != here &&
            (f & 0x10) && (f & 0x20)) {
            s32(g, D_BAY, bay);
            s8(g, bay + 0x1E, 0);
            return 1;
        }
    }
    return 0;
}

/* op $9C PIRATES: the first cargo cruiser in orbit carrying >= 1,200 */
static int pirates(Game *g)
{
    for (int j = 0; j < NBAYS; j++) {
        uint32_t bay = BAYS_ADDR + (uint32_t)j * BAY_SIZE;
        if (g8(g, bay + 0x1E) == 5 && (g8(g, bay + 0x1F) & 0x04) && g16(g, bay + 0x0A) >= 1200) {
            s32(g, D_BAY, bay);
            s16(g, bay + 0x0A, 0);
            redraw_hi(g, 0x40);
            return 1;
        }
    }
    return 0;
}

int events_resume(Game *g, uint32_t pc, int sc, uint32_t *resume)
{
#define L() (pc += 4, rd32(g, pc - 4, sc))
#define W() (pc += 2, rd16(g, pc - 2, sc))
    for (;;) {
        uint16_t op = W();
        uint32_t a, b, v;
        switch (op) {
        case 0x00: if (rd8(g, L(), sc) != 0) return EV_STOP; break;
        case 0x04: a = L(); v = W(); if (rd8(g, a, sc) != (uint8_t)v) return EV_STOP; break;
        case 0x08:
            a = L(); v = W();
            if (rd16(g, a, sc) != v) return EV_STOP;
            break;
        case 0x0C: a = L(); v = L(); if (rd32(g, a, sc) != v) return EV_STOP; break;
        case 0x10: post_message(g, L()); break;
        case 0x14: return EV_END;
        case 0x18: wr8(g, L(), 0xFF); break;
        case 0x1C: wr8(g, L(), 0); break;
        case 0x20:
            post_message(g, g32(g, G_PLANET_TABLE) + (uint32_t)g8(g, 0xD44C) * PLANET_SIZE + P_NAME);
            break;
        case 0x24: post_message(g, g32(g, D_PLANET) + P_NAME); break;
        case 0x28: {                                        /* RANDPLANET, never the starbase */
            uint16_t last = g16(g, G_LAST), i;
            do
                i = (uint16_t)(rng_next(g) & last);
            while (i == last);
            s32(g, D_PLANET, planet_rec(g, i));
            break;
        }
        case 0x2C: case 0x30: case 0x34:
            a = rd32(g, L(), sc);
            a += (uint32_t)(int32_t)(int16_t)W();
            if (op == 0x2C) wr8(g, 0xD44D, rd8(g, a, sc));
            else if (op == 0x30) wr16(g, 0xD44E, rd16(g, a, sc));
            else wr32(g, 0xD450, rd32(g, a, sc));
            break;
        case 0x38: a = L(); b = L(); wr8(g, b, rd8(g, a, sc)); break;
        case 0x3C: a = L(); b = L(); wr16(g, b, rd16(g, a, sc)); break;
        case 0x40: a = L(); b = L(); wr32(g, b, rd32(g, a, sc)); break;
        case 0x44: v = W(); a = L(); wr8(g, a, (uint8_t)(rd8(g, a, sc) + v)); break;
        case 0x48: v = W(); a = L(); wr16(g, a, (uint16_t)(rd16(g, a, sc) + v)); break;
        case 0x4C: v = W(); a = L(); wr32(g, a, rd32(g, a, sc) + v); break;
        case 0x50: if (rd8(g, L(), sc) == 0) return EV_STOP; break;
        case 0x54: v = W(); a = L(); wr8(g, a, (uint8_t)v); break;
        case 0x58: v = W(); a = L(); wr16(g, a, (uint16_t)v); break;
        case 0x5C: v = L(); a = L(); wr32(g, a, v); break;
        case 0x64: if (!destroy_planet(g, sc)) return EV_STOP; break;
        case 0x68: case 0x6C: case 0x70: {
            uint16_t want = op == 0x68 ? OWNER_PLAYER : op == 0x6C ? OWNER_ENEMY : OWNER_LIFELESS;
            if (g16(g, g32(g, D_PLANET) + P_OWNER) != want) return EV_STOP;
            break;
        }
        case 0x74:                                          /* COUNTDOWN */
            a = L();
            if (rd16(g, a, sc)) {
                wr16(g, a, (uint16_t)(rd16(g, a, sc) - 1));
                return EV_STOP;
            }
            break;
        case 0x78:
            if (resume) *resume = pc;
            return EV_WAIT_MESSAGES;
        case 0x7C:
            s32(g, G_HOTSPOTS, YESNO_HOTSPOTS);
            s16(g, G_NHOTSPOTS, 2);
            if (resume) *resume = pc;
            return EV_YESNO;
        case 0x80:
            for (int j = 0; j < NBAYS; j++) {
                uint32_t bay = BAYS_ADDR + (uint32_t)j * BAY_SIZE;
                s8(g, bay + 0x1F, (uint8_t)(g8(g, bay + 0x1F) & ~0x20));
            }
            break;
        case 0x84: if (!kill_ships(g, sc)) return EV_STOP; break;
        case 0x88: if (!kill_farm(g)) return EV_STOP; break;
        case 0x8C: post_message(g, g32(g, D_BAY)); break;
        case 0x90:
            s16(g, g32(g, D_PLANET) + P_POP, 0);
            or32(g, G_REDRAW, 0x20);
            break;
        case 0x94:
            s16(g, g32(g, D_PLANET) + P_ENERGY, 0x74CC);
            or32(g, G_REDRAW, 0x20000);
            break;
        case 0x98:
            for (int j = 0; j < NBAYS; j++)
                s16(g, BAYS_ADDR + (uint32_t)j * BAY_SIZE + 0x0C, 0);
            or32(g, G_REDRAW, 0x800);
            break;
        case 0x9C: if (!pirates(g)) return EV_STOP; break;
        default:                       /* $60 (native code jump) and non-opcodes */
            return EV_STOP;
        }
    }
#undef L
#undef W
}

int events_run_script(Game *g, int idx, int screen, uint32_t *resume)
{
    if (idx < 0 || idx >= EV_NSCRIPTS)
        return EV_STOP;
    uint32_t pc = rd32(g, EV_SCRIPT_TABLE + 4u * (uint32_t)idx, screen);
    return events_resume(g, pc, screen, resume);
}

int events_pass(Game *g, uint16_t counter, int screen, uint32_t *resume)
{
    uint16_t n = g16(g, G_NPLANETS);
    uint8_t idx = (uint8_t)(counter - 0x38u - 2u * n);
    return events_run_script(g, idx, screen, resume);
}

/* $B7C2 up to its message wait ($5BE2) */
void events_answer_no_begin(Game *g)
{
    post_message(g, 0xAABA);                        /* "HAVE IT YOUR WAY!" (then waited for) */
}

/* $B7C2 after the wait, the sound $21 and the flash, then the
 * main hotspot table back ($B76A) */
void events_answer_no_end(Game *g)
{
    uint32_t sb = planet_rec(g, g16(g, G_LAST));
    post_message(g, 0xAAED);
    s16(g, sb + P_FOOD, (uint16_t)(g16(g, sb + P_FOOD) >> 1));
    redraw_hi(g, 0x40);
    s16(g, sb + P_FUELS, (uint16_t)(g16(g, sb + P_FUELS) >> 1));
    redraw_hi(g, 0x01);
    for (int k = 0; k < NPLATOONS; k++) {
        uint32_t p = PLATOONS_ADDR + (uint32_t)k * PLATOON_SIZE;
        if (g32(g, p + 2) == sb)
            clear_platoon(g, p);
    }
    s32(g, G_HOTSPOTS, MAIN_HOTSPOTS);
    s16(g, G_NHOTSPOTS, 0x11);
}

void events_answer_yesno(Game *g, int yes)
{
    if (!yes) {
        events_answer_no_begin(g);
        if (g->play_sound)
            g->play_sound(g, 0x21);                 /* then 66 frames of flashing */
        events_answer_no_end(g);
        return;
    }
    uint32_t sb = planet_rec(g, g16(g, G_LAST));    /* $B782: hand over the goods */
    post_message(g, 0xAA71);
    uint16_t food = g16(g, sb + P_FOOD), fuel = g16(g, sb + P_FUELS);
    s16(g, sb + P_FOOD, (uint16_t)(food - (food < 390 ? food : 390)));
    s16(g, sb + P_FUELS, (uint16_t)(fuel - (fuel < 900 ? fuel : 900)));
    s32(g, G_HOTSPOTS, MAIN_HOTSPOTS);
    s16(g, G_NHOTSPOTS, 0x11);
}

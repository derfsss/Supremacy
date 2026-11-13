/*
 * The enemy's daily jobs and the win/lose test. See enemy.h.
 *
 * Bug fixes (only when g->fix_bugs; the checks run with 0):
 *   F3  $EE24 scanned one record past the planet table (i = 1..N; record N
 *       is not a planet). Fixed: records 1..N-1.
 */
#include <stddef.h>

#include "enemy.h"
#include "events.h"

#define MSG_ATTACK      0xD64Eu    /* "ENEMY ATTACKING PLANET : <name>" */
#define MSG_ATTACK_NAME 0xD668u
#define G_TARGET_FAIL   0xE427u    /* u8 $FF: $985A found no target */
#define G_ROBIN         0xE402u    /* u16 level-0 round-robin index */
#define G_FMT_DAYS      0xE41Fu    /* u8 enemy formatting countdown */
#define G_FMT_PLANET    0xE3D0u    /* u32 planet the enemy is formatting */

static void (*viewer_hook)(Game *g);

void enemy_set_viewer_hook(void (*fn)(Game *g)) { viewer_hook = fn; }

void sup_viewer_refresh(Game *g, int screen)
{
    if (screen == 0 && viewer_hook)
        viewer_hook(g);
}

/* $973E: the enemy base's force grows, unless player troops are on it or it
 * has reached $A240. */
void enemy_base_growth(Game *g)
{
    uint32_t b = g32(g, G_PLANET_TABLE);
    if (g16(g, b + P_OWNER) != OWNER_ENEMY || g16(g, b + P_TROOPS) != 0 ||
        g16(g, b + P_FORCE) >= 0xA240)
        return;
    uint16_t add = 10;
    if (g8(g, G_LEVEL)) {
        add = 22;
        if (g8(g, G_LEVEL) != 1)
            add = g8(g, G_OPP3) ? 50 : 34;
    }
    s16(g, b + P_FORCE, (uint16_t)(g16(g, b + P_FORCE) + add));
}

static uint32_t target_fail(Game *g)
{
    s8(g, G_TARGET_FAIL, 0xFF);
    return 0;
}

static uint32_t target_random(Game *g)
{
    uint16_t last = g16(g, G_LAST);
    uint16_t i = (uint16_t)(rng_next(g) & last);
    if (i == last)
        return target_fail(g);
    return planet_rec(g, i);
}

/* $985A: pick the planet to attack. Returns 0 (and $E427 = $FF) for no
 * attack today. */
uint32_t enemy_choose_target(Game *g)
{
    s8(g, G_TARGET_FAIL, 0);
    uint16_t last = g16(g, G_LAST);
    uint16_t i;
    for (i = 0; i < last; i++)
        if (g16(g, planet_rec(g, i) + P_OWNER) == OWNER_PLAYER)
            break;
    if (i == last)
        return planet_rec(g, last);           /* only the starbase is left */

    uint8_t lv = g8(g, G_LEVEL);
    if (lv == 0) {                             /* round robin, records 1..last-1 */
        uint16_t r = g16(g, G_ROBIN);
        r = (uint16_t)((r & 0xFF00u) | ((r + 1u) & 0xFFu));   /* addq.b */
        if (r >= last) {
            s16(g, G_ROBIN, 0);
            return target_fail(g);
        }
        s16(g, G_ROBIN, r);
        return planet_rec(g, r);
    }
    if (lv == 1)
        return target_random(g);

    uint8_t k = (uint8_t)(g16(g, G_DAY) & 7);
    if (k == 0 || k == 1) {                    /* planet of a farming / mining station */
        uint8_t type = k == 0 ? 7 : 6;
        for (int j = 0; j < NBAYS; j++) {
            uint32_t bay = BAYS_ADDR + (uint32_t)j * BAY_SIZE;
            if (g8(g, bay + 0x1E) == type && (g8(g, bay + 0x1F) & 0x10)) {
                uint32_t r = planet_rec(g, g8(g, bay + 0x21));
                if (r == planet_rec(g, last))
                    return target_fail(g);
                return r;
            }
        }
        return target_fail(g);
    }
    if (k == 2) {                              /* first player planet with pop >= 5000 */
        for (i = 0; i < last; i++) {
            uint32_t r = planet_rec(g, i);
            if (g16(g, r + P_OWNER) == OWNER_PLAYER && g16(g, r + P_POP) >= 5000)
                return r;
        }
        return target_fail(g);
    }
    if (k == 3)
        return target_random(g);
    return target_fail(g);
}

/* $978A: once the base force is over the threshold, drop a force on a
 * player planet that has no enemy force yet. */
void enemy_attack(Game *g)
{
    uint32_t b = g32(g, G_PLANET_TABLE);
    uint8_t lv = g8(g, G_LEVEL);
    uint16_t at = lv == 0 ? 0x2C38 : lv == 1 ? 0x568C : 0x7D9D;
    if (g16(g, b + P_FORCE) < at)
        return;
    uint32_t t = enemy_choose_target(g);
    if (g8(g, G_TARGET_FAIL) || t == 0)
        return;
    if (g16(g, t + P_OWNER) != OWNER_PLAYER || g16(g, t + P_FORCE) != 0)
        return;
    uint16_t r = rng_next(g);
    uint16_t mask = 0x1FF, base = 120;
    if (lv) {
        mask = 0x7F; base = 55;
        if (lv != 1) {
            mask = 0x3FF;
            base = g8(g, G_OPP3) ? 1250 : 500;
        }
    }
    uint16_t f = (uint16_t)((r & mask) + base);
    s16(g, t + P_FORCE, f);
    b = g32(g, G_PLANET_TABLE);
    s16(g, b + P_FORCE, (uint16_t)(g16(g, b + P_FORCE) - f));
    for (int k = 0; k < 9; k++)
        s8(g, MSG_ATTACK_NAME + k, g8(g, t + P_NAME + k));
    post_message(g, MSG_ATTACK);
    if (g->play_sound)
        g->play_sound(g, 8);
}

/* $5AA6: a fresh random name for a planet record */
void planet_random_name(Game *g, uint32_t rec)
{
    random_name_at(g, rec + P_NAME);
}

/* $5AAC: a random name in the 9 bytes at a: 1-4 letters alternating
 * consonant/vowel, then a 3-letter ending whose first letter differs from
 * the last one written. The name parts are read from the game image. */
void random_name_at(Game *g, uint32_t a)
{
    const uint8_t *np = g->image ? g->image + EV_NAMES_BASE : NULL;   /* the name parts */
    for (int k = 0; k < 9; k++)
        s8(g, a + k, 0x20);
    if (!np)                                                 /* no image: no name parts */
        return;
    int n = rng_next(g) & 3;
    for (;;) {
        s8(g, a++, np[rng_next(g) & 15]);                    /* $1E1E consonants */
        if (n-- == 0)
            break;
        s8(g, a++, np[0x10 + (rng_next(g) & 7)]);            /* $1E2E vowels */
        if (n-- == 0)
            break;
    }
    unsigned e;
    do
        e = 0x18u + (unsigned)(rng_next(g) & 15) * 4u;      /* $1E36 endings */
    while (np[e] == g8(g, a - 1));
    for (int k = 0; k < 3; k++)
        s8(g, a + k, np[e + k]);
}

/* $99D6: the planet becomes a new enemy colony: random stores, standard
 * tax/growth/decline/morale, a random name; the viewer is refreshed on the
 * main screen. Shared with the enemy capture $9342. */
void enemy_new_colony(Game *g, uint32_t a, int screen)
{
    s16(g, a + P_POP, rng_below(g, 4500));
    s16(g, a + P_FOOD, (uint16_t)(rng_below(g, 700) + 300));
    s16(g, a + P_MINERALS, (uint16_t)(rng_below(g, 1000) + 200));
    s16(g, a + P_FUELS, (uint16_t)(rng_below(g, 900) + 350));
    s16(g, a + P_ENERGY, (uint16_t)(rng_below(g, 700) + 100));
    s32(g, a + P_CREDITS, (uint16_t)(rng_below(g, 2000) + 5000));
    s8(g, a + P_TAX, 25);
    s8(g, a + P_GROWTH, 30);
    s8(g, a + P_DECLINE, 15);
    s8(g, a + P_MORALE, 75);
    planet_random_name(g, a);
    sup_viewer_refresh(g, screen);
}

/* $EE24 (68000 code inside the state block; its bytes are left alone):
 * start formatting the first lifeless planet. */
static void colonise_start(Game *g)
{
    uint16_t y = g16(g, G_YEAR);
    if (y < 2011 || (g8(g, G_LEVEL) == 0 && y < 2013))
        return;
    uint16_t n = g16(g, G_NPLANETS);
    unsigned count = (unsigned)((n & 0xFF00u) | ((n - 1u) & 0xFFu)) + 1u;  /* subq.b + dbra */
    if (g->fix_bugs)
        count = n ? n - 1u : 0;                 /* F3: records 1..N-1 only */
    uint32_t a = g32(g, G_PLANET_TABLE) + PLANET_SIZE;
    for (unsigned k = 0; k < count; k++, a += PLANET_SIZE) {
        if (g16(g, a + P_OWNER) == OWNER_LIFELESS) {
            s32(g, G_FMT_PLANET, a);
            s16(g, a + P_OWNER, OWNER_FORMATTING);
            uint16_t q = (uint16_t)(g16(g, a + P_RADIUS) / 250u);
            s8(g, G_FMT_DAYS, (uint8_t)(q + 11u));
            return;
        }
    }
}

/* $9978: the enemy's formatting countdown; at 0 the planet is the enemy's. */
void enemy_colonise(Game *g, int screen)
{
    uint8_t d = g8(g, G_FMT_DAYS);
    if (d == 0) {
        colonise_start(g);
        return;
    }
    s8(g, G_FMT_DAYS, --d);
    if (d)
        return;
    uint32_t a = g32(g, G_FMT_PLANET);
    or32(g, G_REDRAW, 0x08);
    s16(g, a + P_OWNER, OWNER_ENEMY);
    uint8_t t;
    do
        t = (uint8_t)(rng_next(g) & 7);
    while (t == 0 || t >= 6);
    s8(g, a + P_TYPE, t);
    uint16_t f = rng_below(g, 1200);
    uint32_t b = g32(g, G_PLANET_TABLE);
    if (f < g16(g, b + P_FORCE)) {
        s16(g, b + P_FORCE, (uint16_t)(g16(g, b + P_FORCE) - f));
        s16(g, a + P_FORCE, f);
    }
    enemy_new_colony(g, a, screen);
}

/* $3B50: lost when the starbase is the enemy's or no planet is the
 * player's; won when the enemy base is the player's or no planet is the
 * enemy's. Records 0..last are counted. The ending itself is the platform's. */
int game_win_lose(Game *g)
{
    uint16_t last = g16(g, G_LAST);
    int win = (uint8_t)(g8(g, G_LEVEL) + (g8(g, G_OPP3) ? 1 : 0));
    if (g16(g, planet_rec(g, last) + P_OWNER) == OWNER_ENEMY)
        return 0xFF;
    if (g16(g, g32(g, G_PLANET_TABLE) + P_OWNER) == OWNER_PLAYER)
        return win;
    uint8_t mine = 0, theirs = 0;
    for (uint32_t i = 0; i <= last; i++) {
        uint16_t o = g16(g, planet_rec(g, (uint16_t)i) + P_OWNER);
        if (o == OWNER_PLAYER)
            mine++;
        if (o == OWNER_ENEMY)
            theirs++;
    }
    if (mine == 0)
        return 0xFF;
    if (theirs == 0)
        return win;
    return -1;
}

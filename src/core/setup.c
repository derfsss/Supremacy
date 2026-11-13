/*
 * Starting a game: new game and RESTART GAME. See setup.h.
 * The original's routines $900, $34C4 and $5BD4. Checked byte for byte
 * against the original game running in an emulator.
 */
#include <string.h>

#include "game.h"
#include "setup.h"

/* globals touched here */
#define G_PLANET_TABLE2 0xE3D0u    /* u32 copies of the planet table pointer */
#define G_SEL_PLANET_A  0xE3D4u    /* u32 selected planet record */
#define G_SEL_PLANET_B  0xE3DCu    /* u32 selected planet record */
#define G_LIST_CURSOR   0xE3BCu    /* u32 planet-list cursor (record index) */
#define G_HOTSPOTS      0xE3ECu    /* u32 hotspot table */
#define G_HOTSPOT_COUNT 0xE400u    /* u16 */
#define G_ANIM_SCRIPT   0xE3F0u    /* u32 planet-animation script pointer */
#define G_ANIM_WAIT     0xE3F8u    /* u16 planet-animation delay */
#define G_ANIM_FRAME    0xE42Eu    /* u8  planet-animation frame counter */
#define G_ANIM_RATE     0xE44Eu    /* u8  planet-animation frames per step */
#define G_PLANETS       0xE40Eu    /* u16 N */
#define G_LAST          0xE410u    /* u16 N - 1 */
#define G_LAST_B        0xE41Eu    /* u8  N - 1 */
#define G_LEVEL         0xE43Bu    /* u8  0..2 */
#define G_RORN          0xE43Cu    /* u8  $FF for opponent 3 */
#define G_E42B          0xE42Bu
#define G_E42D          0xE42Du
#define G_PIC_FLAG      0xE425u
#define MSG_LOGGING_ON  0x1392u    /* "CROSS CHECK ANALYSIS COMPLETED. LOGGING ON ..." */

#define P_PLOT     0x00            /* u32 star-map plot address */
#define P_SELECTED 0x05            /* u8 */
#define P_22       0x22            /* u16 */
#define P_FORCE    0x26            /* u16 */
#define P_INDEX    0x28            /* u8 */
#define P_AGGR     0x29            /* u8 */

#define SOUND_CLICK 13             /* $59AD0 with d7 = 13 */

/* $4E28's planet-animation table ($FED2 + type * 14, and $FF26 for owner 1),
 * outside the block: each entry's script address and its byte +12. The
 * first step ($4F0E) of every one of these scripts is a picture, so it
 * leaves the script pointer at script + 4. */
static const struct { uint32_t script; uint8_t rate; } anim_entry[7] = {
    { 0xFD8A, 5 }, { 0xFD02, 4 }, { 0xFE12, 5 },
    { 0xFEB2, 6 }, { 0xFEBA, 7 }, { 0xFA9E, 7 },
    { 0xFDBE, 5 },                                /* owner 1 ($FF26) */
};

static uint32_t record(Game *g, uint32_t index)
{
    /* mulu.w #$3A on the index's low word */
    return g32(g, G_PLANET_TABLE) + (uint32_t)(uint16_t)index * PLANET_SIZE;
}

/* $4B4C (state part): mark the cursor's record as selected. The original
 * indexes with the product's low word, sign-extended (d0.w). */
static void list_mark(Game *g)
{
    uint32_t off = (uint32_t)(uint16_t)g32(g, G_LIST_CURSOR) * PLANET_SIZE;
    s8(g, g32(g, G_PLANET_TABLE) + (uint32_t)(int32_t)(int16_t)off + P_SELECTED, 1);
}

/* $6E5C: clear the cursor record's selected flag. */
static void list_unmark(Game *g)
{
    uint32_t off = (uint32_t)(uint16_t)g32(g, G_LIST_CURSOR) * PLANET_SIZE;
    s8(g, g32(g, G_PLANET_TABLE) + (uint32_t)(int32_t)(int16_t)off + P_SELECTED, 0);
}

/* $4E28 (state part): start the selected planet's animation. */
static void planet_anim_start(Game *g)
{
    if (g8(g, G_PIC_FLAG))
        return;
    s16(g, G_ANIM_WAIT, 0);
    s8(g, G_ANIM_FRAME, 0);
    s32(g, G_ANIM_SCRIPT, 0);
    uint32_t p = g32(g, G_SEL_PLANET_B);
    uint16_t owner = g16(g, p + P_OWNER);
    if (owner == 7)                                   /* enemy: a text instead */
        return;
    unsigned k = owner == 1 ? 6u : g8(g, p + P_TYPE);
    if (k > 6)
        k = 6;   /* not reachable: types are 0..5 */
    s32(g, G_ANIM_SCRIPT, anim_entry[k].script + 4);  /* after $4F0E's first step */
    s8(g, G_ANIM_RATE, anim_entry[k].rate);
}

/* $4C12 then $4C36 (state part): move the planet-list cursor to the next
 * record (byte arithmetic, wrapping after the last) and select it. */
static void list_next(Game *g)
{
    if (g->play_sound)
        g->play_sound(g, SOUND_CLICK);
    list_unmark(g);
    uint32_t d0 = g32(g, G_LIST_CURSOR);
    uint8_t lo = (uint8_t)d0;
    lo = lo == g8(g, G_LAST_B) ? 0 : (uint8_t)(lo + 1);
    d0 = (d0 & 0xFFFFFF00u) | lo;
    s32(g, G_LIST_CURSOR, d0);
    uint32_t p = g32(g, G_PLANET_TABLE) + (uint32_t)(uint16_t)d0 * PLANET_SIZE;
    s32(g, G_SEL_PLANET_B, p);
    s32(g, G_SEL_PLANET_A, p);
    list_mark(g);
    planet_anim_start(g);
    s16(g, G_ANIM_WAIT, 0);
}

/* $34C4/$34B4/$34A4/$349E then $6A68: the start state ($BA5A snapshot) for
 * an opponent, built in the whole block from the image's block. */
static void start_state(Game *g, int opponent)
{
    uint32_t table;
    uint16_t n;
    uint8_t level;
    switch (opponent) {
    case 0:  table = 0xE1BE; n = 8;  level = 0; break;
    case 1:  table = 0xDE1E; n = 16; level = 1; break;
    case 2:  table = 0xD6DE; n = 32; level = 2; break;
    default: table = 0xD6DE; n = 32; level = 2; s8(g, G_RORN, 0xFF); break;
    }
    s32(g, G_SEL_PLANET_A, table);
    s32(g, G_SEL_PLANET_B, table);
    s32(g, G_PLANET_TABLE, table);
    s32(g, G_PLANET_TABLE2, table);
    s16(g, G_PLANETS, n);
    s16(g, G_LAST, (uint16_t)(n - 1));
    s8(g, G_LAST_B, (uint8_t)(n - 1));
    s8(g, G_LEVEL, level);
    /* $6A68 (first boot only) */
    s8(g, G_E42B, 0);
    s32(g, G_HOTSPOTS, 0xEF8C);
    s16(g, G_HOTSPOT_COUNT, 4);
}

/* $6AD6 up to the main loop $475E. */
static void setup_galaxy(Game *g)
{
    static const uint16_t force[3] = { 0x1F77, 0x3A2F, 0x5811 };
    static const uint32_t cursor[3] = { 6, 14, 30 };
    uint8_t level = g8(g, G_LEVEL);
    uint32_t e = g32(g, G_PLANET_TABLE);

    /* the code tests 0, 1, else */
    unsigned lv = level == 0 ? 0u : level == 1 ? 1u : 2u;
    s16(g, e + P_FORCE, force[lv]);
    s32(g, G_LIST_CURSOR, cursor[lv]);
    s32(g, e + P_CREDITS, 100250);
    s16(g, e + P_FOOD, 11000);
    s16(g, e + P_MINERALS, 4392);
    s16(g, e + P_FUELS, 7234);
    s16(g, e + P_ENERGY, 5941);
    s16(g, e + P_POP, 8913);
    s16(g, e + P_22, 0);
    s8(g, e + P_TYPE, 2);

    uint32_t sb = record(g, g16(g, G_LAST));          /* $3506 */
    s32(g, sb + P_CREDITS, (uint32_t)rng_below(g, 20000) + 50000u);
    uint16_t x = rng_below(g, 1000);
    uint16_t v = (uint16_t)(x + 2500u);
    s16(g, sb + P_FOOD, v);     v = (uint16_t)(v + x);
    s16(g, sb + P_MINERALS, v); v = (uint16_t)(v + x);
    s16(g, sb + P_FUELS, v);    v = (uint16_t)(v + x);
    s16(g, sb + P_ENERGY, v);
    s16(g, sb + P_POP, (uint16_t)(rng_below(g, 500) + 1500u));
    s16(g, sb + P_22, 0);
    s8(g, sb + P_TYPE, 2);

    /* records 1 .. last-1 are LIFELESS! (d4 = last - 2 as a byte, dbra) */
    uint16_t d4 = g16(g, G_LAST);
    d4 = (uint16_t)((d4 & 0xFF00u) | (uint8_t)(d4 - 2));
    uint32_t a = e + PLANET_SIZE;
    for (;;) {
        static const char lifeless[9] = { 'L','I','F','E','L','E','S','S','!' };
        for (int i = 0; i < 9; i++)
            s8(g, a + P_NAME + i, (uint8_t)lifeless[i]);
        a += PLANET_SIZE;
        if (d4-- == 0)
            break;
    }

    /* every record: +$28 = index, +$29 (aggression) = 0 */
    d4 = g16(g, G_LAST);
    a = e;
    uint8_t k = 0;
    for (;;) {
        s8(g, a + P_INDEX, k);
        s8(g, a + P_AGGR, 0);
        a += PLANET_SIZE;
        k++;
        if (d4-- == 0)
            break;
    }
    s8(g, G_E42D, 0);

    /* display steps; these are the ones that change the block */
    s8(g, G_PIC_FLAG, 0);
    d4 = g16(g, G_LAST);
    a = e;
    for (;;) {
        s32(g, a + P_PLOT, 0x72000);
        a += PLANET_SIZE;
        if (d4-- == 0)
            break;
    }
    list_mark(g);                                     /* $4B4C */
    list_next(g);                                     /* $4C12 */

    /* 51 frames of message-panel static ($46A8: 103 lines x 9 words) */
    for (int i = 0; i < 51 * 103 * 9; i++) {
        uint16_t v = rng_next(g);
        if (g->panel_static_hook)
            g->panel_static_hook(g, i % (103 * 9), v);
    }

    post_message(g, MSG_LOGGING_ON);
}

static int block_opponent(Game *g)
{
    uint8_t level = g8(g, G_LEVEL);
    if (level >= 2)
        return g8(g, G_RORN) ? 3 : 2;
    return level;
}

/* the block as the game image has it before any code runs (zeros without one) */
static void initial_block(const Game *g, uint8_t *blk)
{
    if (g->image) memcpy(blk, g->image + STATE_BASE, STATE_SIZE);
    else memset(blk, 0, STATE_SIZE);
}

void game_new_game(Game *g, int opponent, uint32_t seed)
{
    initial_block(g, g->blk);
    g->rng_seed = seed;                               /* $9BC: $802 -> $9DB0 */
    g->booted_opponent = opponent;                    /* RESTART restarts this one */
    start_state(g, opponent);
    setup_galaxy(g);
}

void game_restart_as(Game *g, int opponent)
{
    /* $3CF4: the snapshot back over $D440-$EE23; the rest and the seed stay */
    Game tmp;   /* only its block is used */
    initial_block(g, tmp.blk);
    start_state(&tmp, opponent);
    memcpy(g->blk, tmp.blk, SNAPSHOT_SIZE);
    setup_galaxy(g);
}

void game_restart(Game *g)
{
    game_restart_as(g, block_opponent(g));
}

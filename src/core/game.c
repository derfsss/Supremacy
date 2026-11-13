/* Core services: RNG and the message queue. See game.h. */
#include <stdlib.h>
#include <string.h>

#include "game.h"

/* $9DDE / $9DB4 core: seed = seed * 13 + 7 (32-bit); result = low word XOR
 * high word of the new seed. */
static uint16_t rng_step(Game *g)
{
    uint32_t s = g->rng_seed * 13u + 7u;
    g->rng_seed = s;
    return (uint16_t)(s ^ (s >> 16));
}

/* $9DDE: a 16-bit random number. */
uint16_t rng_next(Game *g)
{
    return rng_step(g);
}

/* $9DB4: a random number in 0..n, as ((n + 1) * r) >> 16 with 16-bit n + 1
 * (n = $FFFF gives 0, as on the 68000). */
uint16_t rng_below(Game *g, uint16_t n)
{
    uint16_t r = rng_step(g);
    uint16_t n1 = (uint16_t)(n + 1u);
    return (uint16_t)(((uint32_t)n1 * r) >> 16);
}

/* $4A9E: queue a message (text at an original address) for the panel.
 * Plays sound 1, sets the arrived flag, and appends to the ring at $E450
 * unless 128 messages are already waiting. A long $FE marks the ring's end. */
void post_message(Game *g, uint32_t text_addr)
{
    if (g->play_sound)
        g->play_sound(g, 1);
    s8(g, G_MSG_ARRIVED, 0xFF);
    if (g16(g, G_MSG_COUNT) == 0x80)
        return;
    uint32_t w = g32(g, G_MSG_WRITE);
    if (g32(g, w) == 0xFE)
        w = G_MSG_RING;
    s32(g, w, text_addr);
    s32(g, G_MSG_WRITE, w + 4);
    s16(g, G_MSG_COUNT, (uint16_t)(g16(g, G_MSG_COUNT) + 1));
}

/* ---- test helpers ---- */
Game *game_new(void)
{
    Game *g = calloc(1, sizeof(Game));
    return g;
}

void game_free(Game *g) { free(g); }
uint8_t *game_block(Game *g) { return g->blk; }
void game_set_seed(Game *g, uint32_t seed) { g->rng_seed = seed; }
void game_set_fix_bugs(Game *g, int on) { g->fix_bugs = on; }
uint32_t game_get_seed(Game *g) { return g->rng_seed; }

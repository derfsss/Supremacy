/*
 * Daily planet economy: one planet per pass ($756E).
 * Checked against the original game running in an emulator.
 */
#include "game.h"

#define MSG_FOOD_LOW   0xD517u     /* "**** WARNING! ****", food too low on <planet> */
#define MSG_FOOD_NAME  0xD55Cu     /* where the planet name is copied for it */
#define FLAG_FERTILITY 0xD481u     /* 2037 fertility drug: growth = morale / 2 */
#define FLAG_ACCIDENT  0xD475u     /* 2025 nuclear accident: wipes a planet's population */
#define ACCIDENT_PLANET 0xD476u    /* u32: which planet */
#define ACCIDENT_MSG   0xD47Au     /* "civilians dying" message latch */

void planet_economy(Game *g, uint32_t p)
{
    if (g16(g, p + P_OWNER) != OWNER_PLAYER)
        return;

    /* 1. food: the population eats pop / 240 a day */
    uint16_t food = g16(g, p + P_FOOD);
    if (food == 0) {
        s8(g, p + P_MORALE, 0);
        or32(g, G_REDRAW, 0x40);
    } else {
        uint16_t need = g16(g, p + P_POP) / 240u;
        if (food < need) {
            if ((g16(g, G_DAY) & 0x0F) == 0) {      /* every 16th day: warn */
                for (int i = 0; i < 9; i++)
                    s8(g, MSG_FOOD_NAME + i, g8(g, p + P_NAME + i));
                post_message(g, MSG_FOOD_LOW);
            }
            need = food;
            s8(g, p + P_MORALE, 0);
        }
        s16(g, p + P_FOOD, (uint16_t)(food - need));
        or32(g, G_REDRAW, 0x4000);
    }

    /* 2. morale drifts one point a day towards 100 - tax */
    uint8_t target = (uint8_t)(100 - g8(g, p + P_TAX));
    uint8_t morale = g8(g, p + P_MORALE);
    if (target != morale) {
        s8(g, p + P_MORALE, (uint8_t)(target < morale ? morale - 1 : morale + 1));
        or32(g, G_REDRAW, 0xC0);
    }

    if (g16(g, G_DAY) & 1) {
        /* 3. odd days: taxes. 16-bit quotient added to the 32-bit credits */
        uint16_t div = (g8(g, p + P_TYPE) == 2 || g8(g, p + P_TYPE) == 1) ? 125 : 200;
        uint32_t prod = (uint32_t)g8(g, p + P_TAX) * g16(g, p + P_POP);
        uint16_t inc = (uint16_t)(prod / div);
        s32(g, p + P_CREDITS, g32(g, p + P_CREDITS) + inc);
        or32(g, G_REDRAW, 0x04);
        return;
    }

    /* 4. even days: population */
    morale = g8(g, p + P_MORALE);
    s8(g, p + P_GROWTH, g8(g, FLAG_FERTILITY) ? (uint8_t)(morale >> 1) : (uint8_t)(morale / 3));
    or32(g, G_REDRAW, 0x40);
    s8(g, p + P_DECLINE, (uint8_t)((uint8_t)(100 - morale) / 4 + g8(g, p + P_TAX) / 4));
    or32(g, G_REDRAW, 0x40);

    uint16_t pop = g16(g, p + P_POP);
    uint8_t growth = g8(g, p + P_GROWTH), decline = g8(g, p + P_DECLINE);
    if (growth != decline) {
        uint8_t diff = (uint8_t)(growth - decline);
        if (!(diff & 0x80)) {                         /* growth wins */
            if (pop < 2)
                return;
            pop = (uint16_t)(pop + (uint16_t)(((uint32_t)pop * diff) / 400u) + 1u);
        } else {                                      /* decline wins */
            uint16_t loss = (uint16_t)(((uint32_t)pop * (uint8_t)(-diff)) / 400u + 1u);
            pop = (uint16_t)(pop - (loss >= pop ? pop : loss));
        }
        if (pop >= 30000)
            pop = 30000;
        s16(g, p + P_POP, pop);
        or32(g, G_REDRAW, 0x20);
    }
    if (g8(g, FLAG_ACCIDENT) && g16(g, p + P_POP) && g32(g, ACCIDENT_PLANET) == p) {
        s16(g, p + P_POP, 0);
        s8(g, ACCIDENT_MSG, 0);
    }
}

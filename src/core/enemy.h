/*
 * The computer opponent's daily jobs and the win/lose test.
 * Checked against the original game running in an emulator.
 *
 * Screen: functions that depend on the screen on show take it as `screen`
 * (the original's $E425: 0 main, 1 troops, 2 cargo, 3 statistics, 4 fleet,
 * 5 shipyard, 6 surface, 7 combat). The core never reads $E425 itself.
 */
#ifndef SUP_ENEMY_H
#define SUP_ENEMY_H

#include "game.h"

#define OWNER_ENEMY       7
#define OWNER_LIFELESS    4
#define OWNER_FORMATTING  1

#define P_FORCE        0x26        /* u16 enemy force on the planet */
#define P_TROOPS       0x2E        /* u16 player troops on the planet */
#define P_RADIUS       0x1A        /* u16 (formatting time) */
#define P_INDEX        0x28        /* u8 record index */

#define BAYS_ADDR      0xE654u     /* 32 docking bays, 40 bytes */
#define PLATOONS_ADDR  0xEB54u     /* 24 platoons, 12 bytes */

#define G_NPLANETS     0xE40Eu     /* u16 N */
#define G_LAST         0xE410u     /* u16 N-1: the player's starbase */
#define G_LEVEL        0xE43Bu     /* u8 0..2 */
#define G_OPP3         0xE43Cu     /* u8 $FF for opponent 3 */
#define G_SHOWN        0xE3D4u     /* u32 planet on show */
#define G_SHOWN_BAY    0xE3D8u     /* u32 bay on show */
#define G_EDIT_PLATOON 0xE3A4u     /* u32 platoon being edited (troops screen) */

/* Platform hook for the viewer refresh $4E28 (main screen only): the
 * planet picture/animation of the selected planet. The original's $4E28 also
 * writes the viewer animation state ($E3F0, $E3F8, $E42E, $E44E), which is
 * UI state the platform owns. May be NULL. */
SUP_API void enemy_set_viewer_hook(void (*fn)(Game *g));
void sup_viewer_refresh(Game *g, int screen);

static inline uint32_t planet_rec(Game *g, uint16_t i)
{
    return g32(g, G_PLANET_TABLE) + (uint32_t)i * PLANET_SIZE;
}

SUP_API void     enemy_base_growth(Game *g);                  /* $973E, pass $A9 */
SUP_API uint32_t enemy_choose_target(Game *g);                /* $985A: record or 0 ($E427 set) */
SUP_API void     enemy_attack(Game *g);                       /* $978A, pass $FB */
SUP_API void     enemy_colonise(Game *g, int screen);         /* $9978 + $EE24, pass $FE */
SUP_API void     enemy_new_colony(Game *g, uint32_t rec, int screen); /* $99D6 (+$4E28) */
SUP_API void     planet_random_name(Game *g, uint32_t rec);   /* $5AA6 */
SUP_API void     random_name_at(Game *g, uint32_t a);         /* $5AAC: 9 bytes at a */
/* $3B50, pass $FA: -1 play on; otherwise the value for $800:
 * $FF lost, 0..3 won (the opponent number, picks WIN0..WIN3). */
SUP_API int      game_win_lose(Game *g);

#endif

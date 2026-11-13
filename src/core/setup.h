/*
 * Supremacy port - starting a game: new game ($900 -> $34C4/$34B4/$34A4/
 * $349E -> $6A68 -> $6AD6 -> main loop $475E) and RESTART GAME ($5BD4).
 * Checked against the original game running in an emulator.
 */
#ifndef SUP_SETUP_H
#define SUP_SETUP_H

#include <stdint.h>

#include "game.h"

#define SNAPSHOT_SIZE 0x19E4u      /* $D440-$EE23: what $6AD2 saves to $BA5A */

/* Opponents ($800, set by the intro): 0 Wotok (8 planets, level 0),
 * 1 Smine (16, level 1), 2 Krart (32, level 2), 3 Rorn (as Krart, $E43C =
 * $FF). Any other value starts Rorn, as the original's dispatch at $9CA
 * does. */

/* A new game: the block and seed exactly as the original has them when its
 * main loop ($475E) first starts, the intro having handed over `opponent`
 * ($800) and `seed` ($802 -> $9DB0). Includes the 47,277 RNG calls of the
 * opening message-panel static. fix_bugs and the callbacks are kept.
 * Set the game image first (game_set_image): the block starts as the image's
 * $D440-$EF97 (the player's own GAM1, as it is before any code runs). */
SUP_API void game_new_game(Game *g, int opponent, uint32_t seed);

/* RESTART GAME ($5BD4): put back the start-of-game snapshot ($D440-$EE23;
 * $EE24-$EF97 and the seed are kept) and run the set-up again from $6AD6,
 * so the new galaxy differs from the first one. The opponent is read from
 * the current block ($E43B, $E43C); see game_restart_as. */
SUP_API void game_restart(Game *g);

/* As game_restart, for an explicit opponent. The original's snapshot is
 * taken once at boot, so after loading a save made against another
 * opponent it restarts the booted opponent; a caller that wants that exact
 * behaviour keeps the booted opponent and passes it here. */
SUP_API void game_restart_as(Game *g, int opponent);

#endif

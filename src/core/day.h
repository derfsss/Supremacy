/*
 * Supremacy port - one pass of game time ($71E0) and the job dispatcher
 * ($74B2). 256 passes make a day; the pass counter is $E426.
 * Checked pass by pass against the original game running in an emulator.
 */
#ifndef SUP_DAY_H
#define SUP_DAY_H

#include "game.h"

enum {
    PASS_OK = 0,
    PASS_GAME_OVER,          /* $3B50 decided: g->result */
    PASS_WAIT_MESSAGES,      /* event op $78: show the messages, then game_event_resume */
    PASS_YESNO               /* event op $7C (Scorpion V4): events_answer_yesno, then resume */
};

/* Point the core at the original's memory image (static tables: the tension
 * table $302A) and reset the outside-block state from it (banner at $1A79,
 * $2C60). */
SUP_API void game_set_image(Game *g, const uint8_t *image);

/* $71E0: one pass. `screen` is the screen on show ($E425). The screen loops'
 * own RNG call (the surface screen's twinkling star) is not part of this:
 * see game_screen_loop. */
SUP_API int  game_pass(Game *g, uint32_t *resume);
SUP_API int  game_event_resume(Game *g, uint32_t pc, uint32_t *resume);

/* The RNG calls a screen loop makes once per pass before $71E0 (surface
 * screen: one $9DDE for the twinkling star at $220C). */
SUP_API void game_screen_loop(Game *g);

/* The individual jobs, for the checks. */
SUP_API void day_tension(Game *g);       /* $2C00 */
SUP_API void day_banner_hide(Game *g);   /* $5BBE */
SUP_API void day_banner_reveal(Game *g); /* $95CC */
SUP_API void day_banner_blink(Game *g);  /* $9644 */
SUP_API void day_banner_clear(Game *g);  /* $96AC */
SUP_API void day_advance(Game *g);       /* $77E8 */

/* tests: the outside-block state */
SUP_API uint8_t *game_banner(Game *g);
SUP_API uint16_t game_get_tension_sound(Game *g);
SUP_API void     game_set_tension_sound(Game *g, uint16_t v);
SUP_API int      game_get_result(Game *g);

#endif

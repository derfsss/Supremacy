/*
 * The event-script interpreter ($B4DE): 30 one-shot scripts (the enemy's
 * threats, the calendar disasters and discoveries), one per pass
 * $38+2N .. $56+2N-1. Checked against the original game running in an emulator.
 *
 * The scripts stay in the original's bytecode, as data at their original
 * addresses, read from the game image (the player's own GAM1, set by
 * game_set_image). Operand addresses are original addresses;
 * accesses outside the state block (and the scripts' own data) read 0 and
 * are not written.
 *
 * Screen: `screen` is the screen on show (the original's $E425). The core
 * does not read $E425 from the block: a script's test of byte $E425 (op $00)
 * reads `screen`.
 *
 * Two ops wait for the player, running the original's own frame loop
 * without game time; here they return to the caller with a resume address:
 *   EV_WAIT_MESSAGES ($78): show every queued message, then call
 *       events_resume(g, resume, ...).
 *   EV_YESNO ($7C, only script 8, the Scorpion V4): show the YES/NO box,
 *       call events_answer_yesno(g, yes), then events_resume(g, resume, ...).
 */
#ifndef SUP_EVENTS_H
#define SUP_EVENTS_H

#include "game.h"

enum {
    EV_STOP = 0,             /* a test failed: the script ends for today */
    EV_END = 1,              /* the script ran to its END op */
    EV_WAIT_MESSAGES = 2,    /* op $78: resume after the messages are shown */
    EV_YESNO = 3             /* op $7C: answer, then resume */
};

#define EV_NSCRIPTS 30
#define EV_SCRIPT_TABLE 0x9EA8u

/* Static data the event and enemy code reads from the game image
 * (game_set_image: the player's own GAM1): the script pointers $9EA8 and
 * the scripts $9F20-$A499, and the name parts at $1E1E (consonants), $1E2E
 * (vowels) and $1E36 (endings, 4 bytes each). */
#define EV_SCRIPTS_BASE 0x9EA8u
#define EV_SCRIPTS_SIZE 0x5F2u
#define EV_NAMES_BASE   0x1E1Eu
#define EV_NAMES_SIZE   0x58u

/* $B4DE: the event pass for dispatcher counter `counter` (script
 * counter - $38 - 2N). */
SUP_API int events_pass(Game *g, uint16_t counter, int screen, uint32_t *resume);
SUP_API int events_run_script(Game *g, int idx, int screen, uint32_t *resume);
SUP_API int events_resume(Game *g, uint32_t pc, int screen, uint32_t *resume);
/* The player's answer to the Scorpion V4 box: yes = $B782, no = $B7C2.
 * NO posts "HAVE IT YOUR WAY!", plays sound $21 and posts the slaughter
 * message; between the two the original waits for the first message and
 * flashes the screen 66 frames: that wait and flash are the platform's. */
SUP_API void events_answer_yesno(Game *g, int yes);
/* NO in two halves, for a platform that shows the wait and the flash:
 * begin, show the messages ($5BE2), sound $21, flash 66 x 3 frames, end. */
SUP_API void events_answer_no_begin(Game *g);
SUP_API void events_answer_no_end(Game *g);

#endif

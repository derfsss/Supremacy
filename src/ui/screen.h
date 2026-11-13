/*
 * Supremacy port - the original's screen and its text routines.
 *
 * The first rendition keeps the original's display buffer exactly: 320x200,
 * 5 bitplanes interleaved per line (each line = 5 rows of 40 bytes, 200
 * bytes), at original address $72000 (line addresses: the table at $58F58).
 * Drawing code is translated routine by routine so the buffer can be checked
 * byte for byte against the original. The platform
 * layer converts it to pixels; high-resolution art replaces this layer later.
 */
#ifndef SUP_SCREEN_H
#define SUP_SCREEN_H

#include <stdint.h>

#include "game.h"

#if defined(_WIN32) && defined(SUP_BUILD_DLL)
#define UI_API __declspec(dllexport)
#else
#define UI_API
#endif

#define SCREEN_BASE  0x72000u
#define SCREEN_LINE  200u                  /* bytes per line (5 planes x 40) */
#define SCREEN_SIZE  40000u
#define FONT_ADDR    0x597C8u              /* 4x5 font, 10 bytes per char from $20 */
#define FONT_CHARS   224             /* codes $20-$FF, as $2904 indexes them */

typedef struct Screen {
    uint8_t bpl[SCREEN_SIZE];              /* original layout */
    uint8_t font[FONT_CHARS * 10];         /* copied from the game data */
    /* $26C8 with $E43F set runs game passes after each character
     * (the typewriter effect); the caller supplies them. */
    void  (*char_hook)(void *user);
    void   *hook_user;
} Screen;

UI_API void screen_init(Screen *s, const uint8_t *font);
UI_API void screen_load_raw(Screen *s, const uint8_t *raw); /* unpacked picture bytes */

/* $2904: one character at column `col` (4-pixel units) and pixel row `row`;
 * writes planes 0 and 1 only. */
UI_API void ui_draw_char(Screen *s, unsigned col, unsigned row, unsigned ch);
/* $3274 (from 10,000) / $3296 (from 10,000,000): decimal, no leading zeros.
 * Returns the column after the last digit. */
UI_API unsigned ui_draw_number(Screen *s, unsigned col, unsigned row, uint32_t value);
UI_API unsigned ui_draw_number_wide(Screen *s, unsigned col, unsigned row, uint32_t value);
/* $26C8: a string with control bytes ($01 n: row += n; $02 n: col += n;
 * $03 n c: c n times across; $04 n c: c n times down; $FF ends). `typing`
 * is the $E43F flag. Returns the end of the string. */
UI_API const uint8_t *ui_draw_string(Screen *s, unsigned col, unsigned row,
                                     const uint8_t *str, int typing);

/* The message panel, every pass ($4994 then $49EA). Message texts are read
 * from the state block (`text_at` resolves addresses outside it, may be
 * NULL). */
typedef const uint8_t *(*ui_text_fn)(void *user, uint32_t addr);
UI_API void ui_message_pass(Screen *s, Game *g, ui_text_fn text_at, void *user);
UI_API void ui_message_type(Screen *s, Game *g, ui_text_fn text_at, void *user); /* $49EA */
UI_API void ui_message_scroll(Screen *s);  /* $4AE8 */

/* $4850, every pass on the main screen: the star map. Erases and redraws
 * each planet's dot from its orbit fields and moves the selection arrow.
 * `sin_table` = the game's bytes at $58E58 (at least $200). `d1_hi` = the
 * upper word of d1 at the call (it ends up in the long at $E3C4). If a
 * planet is selected, *arrow gets the arrow image ($592D0 or $592F4; the
 * original stores it in the sprite list at $59BCA), otherwise it is left. */
UI_API void ui_star_map(Screen *s, Game *g, const uint8_t *sin_table, uint32_t d1_hi,
                        uint32_t *arrow);

/* The screen as colour indices (0-31), 320x200. */
UI_API void screen_to_indices(const Screen *s, uint8_t *out);

#endif

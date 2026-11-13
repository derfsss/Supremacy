/*
 * Supremacy port - the intro program (INT1 + INT2), translated from the
 * 68000: title, story pictures with their animations, credits over a
 * starfield, and the opponent selection that ends with the hand-over to the
 * game.
 *
 * The kernel unpacks INT1 (LZSS) to $900 and loads INT2 raw at $49CE0. The C
 * keeps a 512 KB copy of that memory and works on it at the original
 * addresses, like src/ui/ending.c, so a test can compare it
 * with the original running in the emulator.
 *
 * One intro_frame() is one 50 Hz frame:
 *   1. the front end's pointer is written to the variables the intro reads
 *      ($A90 / $A96), as the port's game does with $AB4 / $ABA;
 *   2. the vertical-blank interrupt ($EC4): mouse buttons ($A76 / $A77),
 *      sprite pointers from $B1C, the copper list, "any key or button down"
 *      -> the skip flag $2368, the frame counter $10D4;
 *   3. CIA-A timer B interrupts ($C78, ~199 Hz: the seed counter $B44, the
 *      keyboard, every 4th one ticks the music) for lines 0-5;
 *   4. one pass of the main program up to its next frame wait ($DE4 /
 *      $DF0 / $E92), unless it is still "busy" (see below);
 *   5. the CIA interrupts for lines 6-312.
 *
 * Timing model (deterministic, the emulator's): a PAL frame of 313 lines,
 * 453 CPU cycles a line; the CIA E-clock is the CPU clock / 10, i.e. 45.3
 * ticks a line, accumulated in a double.
 * Timer B reloads from $0DEB, so it underflows every 3,564 ticks: 3.978
 * times a frame, not a whole number. The timer starts in frame 2, line 167
 * (the boot's screen clear takes the 68000 two frames); interrupts are on
 * from then. The vertical-blank handler occupies lines 0-4, so timer
 * interrupts falling there are taken at line 5. The main program resumes
 * after the vertical blank; CHALLENGE reads $B44 at line 5, so the main pass
 * runs after the CIA interrupts of lines 0-5.
 *
 * Busy frames: unpacking a picture takes the 68000 9-36 frames (the screen
 * is blank meanwhile, the music plays on). The C does the unpack at once and
 * then idles for the same number of frames, measured in the emulator (table
 * in intro.c). Other overruns of the original (a big delta frame or a
 * credits page that needs 2-3 frames, ~44 frames over the whole intro) are
 * not modelled: the port runs every other pass of the main program in one
 * frame, so with no input it reaches the selection at frame 5,481 where the
 * original (in the emulator) does at 5,525.
 *
 * Music: the intro's music player is at $68000 in INT2 (not part of this
 * file). The intro calls its init (jsr $68000, d7 = 0) in frame 2, right
 * after the timer starts (`music_init`), clears the enable byte $10D5 at
 * once, and from then every 4th CIA timer interrupt calls the tick ($68006):
 * `music_ticks` says how many ticks fell in the last frame (0, 1 or 2;
 * 0.995 a frame on average). The intro never stops the music itself: at the
 * hand-over the loader it jumps to ($7C000) turns all interrupts off (no
 * more ticks) and, one vertical blank later, all DMA, which silences Paula.
 * So: when `handover` is set, stop ticking and silence the audio. (In the
 * original the hand-over comes 2 frames after the C's: clearing the screen
 * first takes the 68000 that long.)
 *
 * Not modelled (68000 cycle timing): the music tick's raster wait ($6839A,
 * "until VHPOSR's low byte is 4 more") that starts on lines 309-312 lasts
 * until line ~58 of the next frame, and the timer interrupt falling in it is
 * lost (one $B44 / $CDE count about every 139 frames in the emulator). The
 * C counts every timer underflow; a test harness tells it about the
 * ones the original lost (intro_lose_irq), the only thing it syncs.
 */
#ifndef SUP_INTRO_H
#define SUP_INTRO_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SUP_BUILD_DLL)
#define INTRO_API __declspec(dllexport)
#else
#define INTRO_API
#endif

#define INTRO_KEY_RAW 0x40          /* the raw key code a key press is given (space) */

typedef struct Intro {
    uint8_t *mem;                   /* 512 KB: INT1 unpacked at $900, INT2 at $49CE0 */
    uint16_t colour[32];            /* COLOR00-31 as the 68000 wrote them */
    uint16_t dmacon;                /* display DMA: $100 bitplanes, $80 copper, $20 sprites */
    uint32_t cop1lc;                /* copper list set at the last vertical blank */
    uint32_t sprpt[8];              /* sprite pointers set at the last vertical blank */
    long     frame;                 /* frames since the program started */

    /* the main program: where it waits, and the state of the routine it is in */
    int      pc, next, ret;
    int      count, sub;
    uint32_t pal;                   /* palette for the $DF0 that is waiting */
    uint32_t view_delta;            /* $1420: delta frame drawn after Rorn's palette */
    int      busy;                  /* frames the original still spends on the last pass */

    /* input of the current frame */
    int      in_mx, in_my, in_button, in_key;
    int      key_down;              /* last key state sent to the keyboard */

    /* CIA-A (timer B, ICR, serial port) */
    double   eclock_frac;
    int32_t  tb;
    int      timer_on, irq_on;
    uint8_t  icr, sdr;
    int      lose_line;             /* tests: a timer interrupt the original loses */

    /* music, for the front end's music player */
    int      music_init;            /* 1 in the frame the intro calls the init (song 0) */
    int      music_ticks;           /* ticks ($68006) in the last frame */
    int      music_on;              /* the enable byte $10D5 is 0 and interrupts run */

    /* results */
    int      challenge;             /* CHALLENGE was clicked ($3730) */
    int      handover;              /* the intro has jumped to the game loader: stop here */
    int      opponent;              /* $800: low byte of $249C (0-3) */
    uint32_t seed;                  /* $802: the long at $B44 when CHALLENGE was clicked */
} Intro;

/* Build the program's memory from INT1 (packed) and INT2 as read from
 * SupremacyData. 0, or -1 if they are not the intro. */
INTRO_API int  intro_init(Intro *in, const uint8_t *int1, size_t len1,
                          const uint8_t *int2, size_t len2);
/* The same from a ready 512 KB image (unpacked/amiga/intro_mem.bin). */
INTRO_API int  intro_init_image(Intro *in, const uint8_t *img, size_t len);
INTRO_API void intro_free(Intro *in);

/* One frame. mx, my: the pointer in the intro's coordinates (320 x 200
 * screen pixels, the pointer's top left corner), or negative to leave it
 * where it is; button: bit 0 left, bit 1 right; key: nonzero while a key is
 * held (sent as raw key INTRO_KEY_RAW). */
INTRO_API void intro_frame(Intro *in, int mx, int my, int button, int key);

/* The display as 320 x 200 0xAARRGGBB: the 5 bitplanes through the copper
 * list (per-line colours on the selection screen) and the sprites (the
 * selection screen's pointer). */
INTRO_API void intro_render(const Intro *in, uint32_t *argb);

/* ---- the parts of intro_frame, for tests ---- */
INTRO_API void intro_input(Intro *in, int mx, int my, int button, int key);
INTRO_API void intro_irq_head(Intro *in);   /* pointer, vertical blank, CIA lines 0-5 */
INTRO_API void intro_main(Intro *in);       /* one pass of the main program */
INTRO_API void intro_irq_tail(Intro *in);   /* CIA lines 6-312, next frame */

INTRO_API Intro    *intro_new_image(const uint8_t *img, size_t len);
INTRO_API void      intro_delete(Intro *in);
INTRO_API uint8_t  *intro_mem(Intro *in);
INTRO_API uint16_t *intro_colours(Intro *in);
INTRO_API int       intro_dma(Intro *in);
INTRO_API int       intro_state(Intro *in, int what);  /* see intro.c */
INTRO_API void      intro_lose_irq(Intro *in, int line);

#endif

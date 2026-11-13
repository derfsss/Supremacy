/*
 * Supremacy port - the end sequences (the separate programs WIN0-WIN3,
 * victory over opponent 0-3, and WIN_, defeat), translated from the 68000.
 *
 * GAM2's end loader ($5BD82) unpacks 'WIN0' + opponent (or WIN_ on defeat)
 * to $900 and loads the music (WINM / DIEM) raw to $4E000. All five programs
 * run the same few routines:
 *   victory: card "CONGRATULATIONS, YOU HAVE DEFEATED ..." 501 frames, fade,
 *            the door 151 frames, a palette, the animation (delta frames
 *            XORed onto the screen, one step every 6 frames), fade, "THE END"
 *            held for ever;
 *   defeat:  the card, fade, the temple throne with its animation, fade to
 *            white, fade out twice, "THE END".
 * The C keeps a copy of the program's memory and works on it at the
 * original addresses (the unpacker's ring buffer lies inside the program's
 * data), so it can be compared with the original.
 * The music is src/sound/endmusic.c.
 *
 * One ending_frame() is the code between two frame waits ($E2E / $E96).
 */
#ifndef SUP_ENDING_H
#define SUP_ENDING_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SUP_BUILD_DLL)
#define END_API __declspec(dllexport)
#else
#define END_API
#endif

#define ENDING_DEFEAT (-1)

typedef struct Ending {
    uint8_t *mem;                  /* 512 KB: the program unpacked at $900 */
    int      which;                /* 0-3 victory over that opponent, -1 defeat */
    uint32_t screen;               /* $6C000 (victory) / $72000 (defeat) */
    uint32_t shadow;               /* palette copy the fades work on */
    uint32_t ring;                 /* the unpacker's ring buffer */
    uint32_t anim_ptr, anim_delay; /* $13BC / $13C0 (defeat $13A8 / $13AC) */
    uint32_t card, scene, final;   /* pictures */
    uint32_t anim;                 /* the animation script */
    uint16_t colour[32];           /* COLOR00-31 */
    int      display;              /* bitplane DMA on */
    int      state, count;
    int      done;                 /* "THE END" is up (the program loops for ever) */
} Ending;

/* Build the program's memory from its packed file (WIN0-WIN3 / WIN_ as read
 * from SupremacyData). 0, or -1 if it is not one of the known programs. */
END_API int  ending_init(Ending *e, int which, const uint8_t *file, size_t len);
/* The same from an already unpacked image of the program (len bytes at $900). */
END_API int  ending_init_unpacked(Ending *e, int which, const uint8_t *prog, size_t len);
END_API void ending_free(Ending *e);
/* One frame (after a frame wait returned, up to the next one). */
END_API void ending_frame(Ending *e);
/* The picture as colour indices, 320 x 200 (all 0 while the display is off). */
END_API void ending_indices(const Ending *e, uint8_t *idx);
/* File names: "WIN0".."WIN3" / "WIN_", music "WINM" / "DIEM". */
END_API const char *ending_file(int which);
END_API const char *ending_music_file(int which);

/* ---- for tests ---- */
END_API Ending  *ending_new(int which, const uint8_t *prog, size_t len);
END_API uint8_t *ending_mem(Ending *e);
END_API uint16_t *ending_colours(Ending *e);
END_API int      ending_display(Ending *e);
END_API void     ending_delete(Ending *e);

#endif

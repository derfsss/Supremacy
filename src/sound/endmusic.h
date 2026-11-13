/*
 * Supremacy port - the music player of the end sequences and the intro
 * (Wim van Hees / Ralph Sens), translated from the 68000 code.
 *
 * Two copies of one player, both at $68000:
 *  - ENDMUS_END: in the music files WINM (victory, WIN0-WIN3) and DIEM
 *    (defeat, WIN_), loaded raw to $4E000 (player at file offset $1A000).
 *    $68000 = jmp $69330 (init), $68006 = jmp $6800C (tick). WINM and DIEM
 *    have the same code; they differ in data (and in leftovers in the
 *    player's state area, which init clears or overwrites, and in the
 *    self-modified wait byte $683F5).
 *  - ENDMUS_INTRO: in INT2, loaded raw to $49CE0 (player at file offset
 *    $1E320). $68000 = jmp $68DC4 (init), $68006 = jmp $6800E (tick). The
 *    same code with fewer routines (no arpeggio, fall-off, timbre sweep,
 *    note/wave sequences, wave sweep, $FA/$FB, external fade request), its
 *    state area $56E bytes lower (blocks at $68F12 instead of $69480).
 * The data tables at $6A000-$6AFFF are at the same addresses in both.
 *
 * The player keeps its own byte copy of the whole file, indexed by the
 * original addresses (base + offset), and writes into it like the original
 * (channel blocks, build buffers, two self-modified code bytes). A read
 * outside the copy returns 0 (and is counted); a write outside it is
 * dropped.
 *
 * Output: the Paula register writes in the original's order, reg = offset
 * from $DFF000 (see sequencer.h), to the callback and to the record of the
 * last call. Init also sets CIA-A PRA bit 1 (ori.b #2,$BFE001: LED off /
 * audio filter off); that is noted in `filter_off`, not sent.
 *
 * Callers (both programs): init with d7 = 0, then the tick from the CIA-A
 * timer B (latch $0DEB) level-2 interrupt every 4th interrupt, about
 * 49.76 Hz (WIN*: counter $D28, enable byte $11D0 / WIN_ $D2A; intro:
 * counter $CDE, enable byte $10D5).
 *
 * Stale registers: the tick routines share a0 and a3 between routines and
 * channels. a0 is set at the start of every channel ($68086), so it never
 * comes from outside while the credits check passes. a3 is set for each
 * active channel; it is read for an inactive channel only where the result
 * is overwritten. Port rule: a0 = a3 = 0 at the start of each tick.
 */
#ifndef SUP_ENDMUSIC_H
#define SUP_ENDMUSIC_H

#include <stddef.h>
#include <stdint.h>

#include "sequencer.h"
#include "paula.h"

#define ENDMUS_END      0           /* WINM / DIEM at $4E000 */
#define ENDMUS_INTRO    1           /* INT2 at $49CE0 */

#define ENDMUS_END_BASE   0x4E000u
#define ENDMUS_INTRO_BASE 0x49CE0u

#define ENDMUS_MAX_WRITES 256

typedef struct EndMusic {
    uint8_t  *mem;          /* own copy of the file (malloc'd) */
    uint32_t  base, len;    /* original address of mem[0], size */
    int       variant;      /* ENDMUS_END / ENDMUS_INTRO */
    /* layout (original addresses) */
    uint32_t  blocks;       /* 4 channel blocks of $80 bytes */
    uint32_t  credits;      /* "Music coding ..." ('M' is checked per channel) */
    uint32_t  cmp_imm;      /* self-modified cmpi.w immediate (low byte) */
    uint32_t  wait_imm;     /* self-modified cmpi.b #line,$DFF006 word */
    /* 68000 registers the routines share */
    uint32_t  a0, a3, a5;
    uint16_t  hwb;          /* current channel's Paula base, $A0+16n */
    int       keep_regs;    /* 0 (port rule): a0 = a3 = 0 at each tick */
    int       filter_off;   /* init set CIA-A PRA bit 1 */
    uint32_t  outside;      /* reads outside the copy (all calls) */
    /* Paula write output */
    seq_paula_fn paula;
    void     *paula_user;
    SeqWrite  writes[ENDMUS_MAX_WRITES];    /* writes of the last call */
    int       nwrites;
    int       overflow;
} EndMusic;

/* Copy the file (WINM/DIEM for ENDMUS_END, INT2 for ENDMUS_INTRO). Returns 0,
 * or -1 if it is too short, its entry jumps are not the expected ones, or
 * allocation fails. Plays nothing until endmus_start. */
SND_API int  endmus_init(EndMusic *e, const uint8_t *file, size_t len, int variant);
SND_API void endmus_free(EndMusic *e);
SND_API void endmus_set_paula(EndMusic *e, seq_paula_fn fn, void *user);
SND_API void endmus_start(EndMusic *e, int song);   /* jsr $68000 with d7 = song (the programs use 0) */
SND_API void endmus_tick(EndMusic *e);              /* jsr $68006 */

/* Pointer to the byte at original address addr in the copy (NULL outside). */
SND_API uint8_t *endmus_ptr(EndMusic *e, uint32_t addr);

/* ---- sound output: player + Paula mixer, ticking at SND_TICK_NUM/DEN Hz ---- */
typedef struct EndSound {
    EndMusic mus;
    Paula    paula;
    uint64_t acc;           /* tick phase, in 1/(DEN*rate) */
    int      hold;          /* no ticks (the program's enable byte set); Paula plays on */
} EndSound;

/* Copies the file, runs init (song 0) and starts ticking. 0 or -1. */
SND_API int  endsnd_init(EndSound *o, const uint8_t *file, size_t len, int variant, int rate);
SND_API void endsnd_free(EndSound *o);
/* Render n stereo frames (2n int16), running player ticks at their times. */
SND_API void endsnd_render(EndSound *o, int16_t *out, int n);

/* ---- for tests (ctypes) ---- */
SND_API EndMusic *endmus_new(void);
SND_API void      endmus_delete(EndMusic *e);
SND_API int       endmus_nwrites(EndMusic *e);
SND_API int       endmus_overflow(EndMusic *e);
SND_API SeqWrite *endmus_writes(EndMusic *e);
SND_API uint32_t  endmus_outside(EndMusic *e);
SND_API void      endmus_set_regs(EndMusic *e, uint32_t a0, uint32_t a3, int keep);
SND_API EndSound *endsnd_new(void);
SND_API void      endsnd_delete(EndSound *o);

#endif

/*
 * Supremacy port - sound sequencer (the original's player in GAM2,
 * $68000-$69295), translated from the original.
 *
 * The sequencer keeps its own byte copy of the original's memory from
 * $60000 to $70C9F (tables, scripts, instruments, samples) and indexes it
 * with the original 68000 addresses, exactly like the original. It writes
 * into that copy too (channel blocks at $692E2, $69600, the waveform build
 * buffers $69688-$69707), and the Paula mixer reads sample bytes from it.
 *
 * Addresses are masked to 19 bits like the model ($7FFFF). A read outside
 * the copy returns 0 and a write outside it is dropped. (The data region is
 * a byte slice of the player's GAM2 file: offset $16320, length $10CA0.)
 *
 * Output is the sequence of Paula register writes the original makes:
 * reg is the offset from $DFF000 ($096 DMACON, $A0+16n AUDnLC high word,
 * $A2+16n LC low word, $A4+16n LEN, $A6+16n PER, $A8+16n VOL), in the
 * original's order. Each write goes to the callback (if set) and to the
 * record of the last call (seq_stop / seq_play / seq_tick).
 *
 * Stale registers: the original leaves a0 and a3 over from the program the
 * interrupt broke into. Port rule (checked against the original): both are 0 at the start of every tick
 * and carry over from one channel to the next.
 */
#ifndef SUP_SEQUENCER_H
#define SUP_SEQUENCER_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SUP_BUILD_DLL)
#define SND_API __declspec(dllexport)
#else
#define SND_API
#endif

#define SEQ_DATA_BASE   0x60000u    /* original address of the data region */
#define SEQ_DATA_SIZE   0x10CA0u    /* $60000-$70C9F */
#define SEQ_GAM2_OFFSET 0x16320u    /* the region's offset in the GAM2 file */

#define SEQ_BLOCKS      0x692E2u    /* 4 channel blocks of $80 bytes */
#define SEQ_STATE_END   0x69708u    /* end of player state (build buffers) */
#define SEQ_SOUNDS      50          /* entries in the sound table $6AA40 */

#define SEQ_MAX_WRITES  256

typedef void (*seq_paula_fn)(void *user, uint16_t reg, uint16_t val);

typedef struct SeqWrite { uint16_t reg, val; } SeqWrite;

typedef struct Sequencer {
    uint8_t  *mem;          /* own copy of the data region (malloc'd) */
    uint32_t  base;         /* original address of mem[0] */
    uint32_t  len;
    /* the rest of the original's memory, read-only, for reads outside the
     * copy (sound 71 from the WHDLoad clamp points into GAM1); may be NULL */
    const uint8_t *ext;
    uint32_t  ext_len;
    /* 68000 registers the routines share */
    uint32_t  a0, a3;       /* stale registers (see above) */
    uint32_t  a5;           /* current channel block */
    uint16_t  hwb;          /* current channel's Paula base, $A0+16n */
    int       keep_regs;    /* 0 (port rule): a0 = a3 = 0 at each tick */
    /* Paula write output */
    seq_paula_fn paula;
    void     *paula_user;
    SeqWrite  writes[SEQ_MAX_WRITES];   /* writes of the last call */
    int       nwrites;
    int       overflow;     /* writes lost from the record (callback still got them) */
} Sequencer;

/* Copy the data region (len bytes for original addresses base..). Returns 0,
 * or -1 if it does not cover the player ($68000-$6AB00) or allocation fails.
 * Also runs no sound: call seq_stop or seq_play first. */
SND_API int  seq_init(Sequencer *s, const uint8_t *image, size_t len, uint32_t base);
SND_API void seq_free(Sequencer *s);
SND_API void seq_set_paula(Sequencer *s, seq_paula_fn fn, void *user);

SND_API void seq_stop(Sequencer *s);            /* $690F2 */
SND_API void seq_play(Sequencer *s, int sound); /* $59AD0: stop, then $69190 */
SND_API void seq_tick(Sequencer *s);            /* $68014; ~49.76 Hz */

/* Pointer to the byte at original address addr in the copy (NULL outside). */
SND_API uint8_t *seq_ptr(Sequencer *s, uint32_t addr);

/* ---- for tests (ctypes) ---- */
SND_API Sequencer *seq_new(void);
SND_API void       seq_delete(Sequencer *s);
SND_API int        seq_nwrites(Sequencer *s);
SND_API int        seq_overflow(Sequencer *s);
SND_API SeqWrite  *seq_writes(Sequencer *s);
SND_API void       seq_set_regs(Sequencer *s, uint32_t a0, uint32_t a3, int keep);

#endif

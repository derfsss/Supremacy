/*
 * Supremacy port - a small Paula mixer: four DMA audio channels rendered to
 * signed 16-bit stereo PCM. It takes the register writes the sequencer makes
 * (paula_write, usable directly as the sequencer's callback) and reads the
 * 8-bit signed sample bytes from the sequencer's live memory copy, since the
 * player rebuilds some waves while they play.
 *
 * Behaviour ("Paula behaviour the mixer must
 * copy"):
 *  - DMACON: bit 15 set/clear, bits 0-3 the channels. The master enable
 *    (bit 9) is treated as always on: the player never writes it.
 *  - A channel's DMA turning on latches AUDxLC/AUDxLEN and starts from the
 *    beginning. LC/LEN written while it runs take effect at the end of the
 *    current pass (the next reload). There is no one-shot logic here: the
 *    player queues the silence buffer itself.
 *  - PER and VOL take effect at once. Rate = 3,546,895 / period Hz (PAL);
 *    periods below 113 are clamped to 113 (as in the emulator). VOL 0-64
 *    (larger values count as 64). LEN 0 means 65,536 words.
 *  - Channels 0 and 3 go left, 1 and 2 right; each adds sample * vol * 2.
 *  - Sample-and-hold, like the hardware (no interpolation or filter).
 */
#ifndef SUP_PAULA_H
#define SUP_PAULA_H

#include <stdint.h>

#include "sequencer.h"

#define PAULA_CLOCK     3546895.0
#define PAULA_MIN_PER   113

typedef struct PaulaChannel {
    uint16_t lch, lcl, len, per, vol;   /* registers */
    int      on;                        /* DMA running */
    uint32_t ptr;                       /* current byte address */
    uint32_t left;                      /* bytes left in this pass */
    double   pos;                       /* fraction of a sample byte consumed */
    int      cur;                       /* current output byte (signed) */
} PaulaChannel;

typedef struct Paula {
    PaulaChannel ch[4];
    uint16_t dmacon;                    /* audio bits 0-3 */
    int      rate;                      /* output rate, Hz */
    const uint8_t *mem;                 /* sample memory (live) */
    uint32_t base, len;                 /* original address of mem[0], size */
    const uint8_t *ext;                 /* whole image, read-only fallback (may be NULL) */
    uint32_t ext_len;
} Paula;

/* mem/base/len: the memory the AUDxLC addresses point into (normally the
 * sequencer's copy: seq->mem, seq->base, seq->len). */
SND_API void paula_init(Paula *p, int rate, const uint8_t *mem, uint32_t base, uint32_t len);
SND_API void paula_write(void *paula, uint16_t reg, uint16_t val);   /* seq_paula_fn */
/* Render n stereo frames (2n int16 values, left then right) at p->rate. */
SND_API void paula_render(Paula *p, int16_t *out, int n);

/* ---- sound output: sequencer + mixer, ticking at 709,379 / 14,256 Hz ---- */
#define SND_TICK_NUM 709379u                /* E clock */
#define SND_TICK_DEN 14256u                 /* CIA-A timer B latch 3,564 x 4 */

typedef struct SoundOut {
    Sequencer seq;
    Paula     paula;
    uint64_t  acc;                          /* tick phase, in 1/(DEN*rate) */
    int       hold;                         /* the sound-off flag $E42B: no ticks
                                               ($2DC8), Paula plays on */
} SoundOut;

/* image/len/base as for seq_init (the $60000-$70C9F region). 0 or -1. */
SND_API int  snd_init(SoundOut *o, const uint8_t *image, uint32_t len, uint32_t base, int rate);
SND_API void snd_free(SoundOut *o);
SND_API void snd_play(SoundOut *o, int sound);   /* the game's $59AD0 */
SND_API void snd_stop(SoundOut *o);
/* Render n stereo frames, running sequencer ticks at their times. */
SND_API void snd_render(SoundOut *o, int16_t *out, int n);
/* Give the player and mixer the whole original memory image (read-only) for
 * addresses outside the sound region, as the original reads them. */
SND_API void snd_set_image(SoundOut *o, const uint8_t *image, uint32_t len);

/* ---- for tests (ctypes) ---- */
SND_API SoundOut *snd_new(void);
SND_API void      snd_delete(SoundOut *o);

#endif

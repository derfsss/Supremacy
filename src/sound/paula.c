/*
 * Supremacy port - Paula mixer and sound output. See paula.h.
 */
#include "paula.h"

#include <stdlib.h>
#include <string.h>

SND_API void paula_init(Paula *p, int rate, const uint8_t *mem, uint32_t base, uint32_t len)
{
    memset(p, 0, sizeof *p);
    p->rate = rate > 0 ? rate : 44100;
    p->mem = mem;
    p->base = base;
    p->len = len;
}

static void reload(PaulaChannel *c)
{
    c->ptr = (((uint32_t)c->lch << 16) | c->lcl) & 0x7FFFEu;
    c->left = (c->len ? (uint32_t)c->len : 0x10000u) * 2u;
}

SND_API void paula_write(void *paula, uint16_t reg, uint16_t val)
{
    Paula *p = (Paula *)paula;
    if (reg == 0x096) {
        uint16_t old = p->dmacon;
        int i;
        if (val & 0x8000)
            p->dmacon = (uint16_t)(old | (val & 0x000F));
        else
            p->dmacon = (uint16_t)(old & ~val & 0x000F);
        for (i = 0; i < 4; i++) {
            PaulaChannel *c = &p->ch[i];
            int on = (p->dmacon >> i) & 1;
            if (on && !c->on) {             /* DMA switched on: latch LC/LEN */
                reload(c);
                c->pos = 0.0;
            }
            c->on = on;
        }
        return;
    }
    if (reg >= 0xA0 && reg < 0xE0) {
        PaulaChannel *c = &p->ch[(reg - 0xA0) >> 4];
        switch (reg & 0x0F) {
        case 0x0: c->lch = val; break;
        case 0x2: c->lcl = val; break;
        case 0x4: c->len = val; break;
        case 0x6: c->per = val; break;
        case 0x8: c->vol = val; break;
        default: break;
        }
    }
}

static int fetch(const Paula *p, uint32_t addr)
{
    uint32_t o = (addr & 0x7FFFFu) - p->base;
    if (o < p->len) return (int)(int8_t)p->mem[o];
    addr &= 0x7FFFFu;
    return (p->ext && addr < p->ext_len) ? (int)(int8_t)p->ext[addr] : 0;
}

SND_API void paula_render(Paula *p, int16_t *out, int n)
{
    int i, k;
    for (k = 0; k < n; k++) {
        int mix[2] = {0, 0};
        for (i = 0; i < 4; i++) {
            PaulaChannel *c = &p->ch[i];
            int vol;
            unsigned per;
            if (!c->on)
                continue;
            per = c->per < PAULA_MIN_PER ? PAULA_MIN_PER : c->per;
            c->pos += PAULA_CLOCK / per / p->rate;
            while (c->pos >= 1.0) {
                c->pos -= 1.0;
                c->cur = fetch(p, c->ptr);
                c->ptr++;
                if (--c->left == 0)
                    reload(c);
            }
            vol = (c->vol & 0x7F) > 64 ? 64 : (c->vol & 0x7F);
            mix[(i == 0 || i == 3) ? 0 : 1] += c->cur * vol * 2;
        }
        out[2 * k]     = (int16_t)(mix[0] > 32767 ? 32767 : mix[0] < -32768 ? -32768 : mix[0]);
        out[2 * k + 1] = (int16_t)(mix[1] > 32767 ? 32767 : mix[1] < -32768 ? -32768 : mix[1]);
    }
}

/* ------------------------------------------------------------ sound output */
SND_API int snd_init(SoundOut *o, const uint8_t *image, uint32_t len, uint32_t base, int rate)
{
    memset(o, 0, sizeof *o);
    if (seq_init(&o->seq, image, len, base) != 0)
        return -1;
    paula_init(&o->paula, rate, o->seq.mem, o->seq.base, o->seq.len);
    seq_set_paula(&o->seq, paula_write, &o->paula);
    seq_stop(&o->seq);
    o->acc = (uint64_t)SND_TICK_DEN * (uint64_t)o->paula.rate;   /* first tick due at once */
    return 0;
}

SND_API void snd_set_image(SoundOut *o, const uint8_t *image, uint32_t len)
{
    o->seq.ext = image;
    o->seq.ext_len = len;
    o->paula.ext = image;
    o->paula.ext_len = len;
}

SND_API void snd_free(SoundOut *o)
{
    seq_free(&o->seq);
    o->paula.mem = NULL;
    o->paula.len = 0;
}

SND_API void snd_play(SoundOut *o, int sound)
{
    if (sound >= 0 && sound < SEQ_SOUNDS)
        seq_play(&o->seq, sound);
}

SND_API void snd_stop(SoundOut *o)
{
    seq_stop(&o->seq);
}

SND_API void snd_render(SoundOut *o, int16_t *out, int n)
{
    uint64_t thr = (uint64_t)SND_TICK_DEN * (uint64_t)o->paula.rate;
    while (n > 0) {
        uint64_t need;
        int m;
        if (o->acc >= thr) {
            o->acc -= thr;
            if (!o->hold)
                seq_tick(&o->seq);
        }
        need = (thr - o->acc + SND_TICK_NUM - 1) / SND_TICK_NUM;   /* frames to the next tick */
        m = need < (uint64_t)n ? (int)need : n;
        if (m < 1)
            m = 1;
        paula_render(&o->paula, out, m);
        o->acc += (uint64_t)m * SND_TICK_NUM;
        out += 2 * m;
        n -= m;
    }
}

SND_API SoundOut *snd_new(void)      { return (SoundOut *)calloc(1, sizeof(SoundOut)); }
SND_API void snd_delete(SoundOut *o) { if (o) { snd_free(o); free(o); } }

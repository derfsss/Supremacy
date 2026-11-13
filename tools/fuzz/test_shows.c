/*
 * Supremacy port - run the intro, the five end sequences and their music
 * once each under AddressSanitizer / UBSan (tools/fuzz: build_shows.sh),
 * as the front end does: frames, renders and audio, with random pointer
 * moves, clicks and keys on the intro.
 *
 * Usage: test_shows <SupremacyData>
 * Exit 0 = everything ran (a sanitizer report aborts the run).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "endmusic.h"
#include "ending.h"
#include "intro.h"

static uint8_t *load(const char *dir, const char *name, size_t *n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f && !strcmp(name, "WIN_")) { snprintf(p, sizeof p, "%s/WIN.", dir); f = fopen(p, "rb"); }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void audio(EndSound *s, int frames)
{
    static int16_t buf[2 * 960];
    for (int k = 0; k < frames; k++) endsnd_render(s, buf, 960);    /* 48 kHz / 50 Hz */
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *dir = argv[1];
    static uint32_t argb[320 * 200];
    static uint8_t idx[320 * 200];
    int bad = 0;
    size_t l1, l2;
    uint8_t *f1 = load(dir, "INT1", &l1), *f2 = load(dir, "INT2", &l2);
    for (int run = 0; run < 3 && f1 && f2; run++) {
        static Intro in;
        if (intro_init(&in, f1, l1, f2, l2)) { printf("intro: init failed\n"); bad++; break; }
        EndSound s;
        int snd = endsnd_init(&s, f2, l2, ENDMUS_INTRO, 48000) == 0;
        long fr = 0;
        for (; fr < 20000 && !in.handover; fr++) {
            int mx = -1, my = -1, b = 0, key = 0;
            if (run > 0 && fr > 300) {                     /* random input after the title */
                mx = (int)(rnd() % 340) - 10; my = (int)(rnd() % 220) - 10;
                uint32_t r = rnd() % 100;
                b = r < 5 ? 1 : r < 7 ? 2 : 0;
                key = run == 2 && r > 97;
            }
            if (run == 0 && fr == 400) key = 1;            /* to the selection; then pick one */
            if (run == 0 && fr > 600 && fr % 50 == 0) { mx = 60 + (int)(rnd() % 200); my = 60 + (int)(rnd() % 120); b = 1; }
            intro_frame(&in, mx, my, b, key);
            intro_render(&in, argb);
            if (snd) audio(&s, 1);
        }
        printf("intro run %d: %ld frames, handover %d challenge %d opponent %d seed %08X\n", run, fr,
               in.handover, in.challenge, in.opponent, (unsigned)in.seed);
        if (snd) endsnd_free(&s);
        else { printf("intro: music init failed\n"); bad++; }
        intro_free(&in);
    }
    if (!f1 || !f2) { printf("INT1/INT2 missing\n"); bad++; }
    free(f1);
    free(f2);
    for (int which = -1; which < 4; which++) {
        size_t n, mn;
        uint8_t *file = load(dir, ending_file(which), &n);
        uint8_t *mus = load(dir, ending_music_file(which), &mn);
        if (!file || !mus) { printf("ending %d: files missing\n", which); bad++; continue; }
        Ending e;
        if (ending_init(&e, which, file, n)) { printf("ending %d: init failed\n", which); bad++; continue; }
        EndSound s;
        int snd = endsnd_init(&s, mus, mn, ENDMUS_END, 48000) == 0;
        long fr = 0, after = 0;
        for (; fr < 60000 && after < 500; fr++) {
            ending_frame(&e);
            ending_indices(&e, idx);
            if (snd) audio(&s, 1);
            if (e.done) after++;
        }
        printf("ending %d: %ld frames, done %d, music %s\n", which, fr, e.done, snd ? "ok" : "FAILED");
        if (!e.done || !snd) bad++;
        if (snd) endsnd_free(&s);
        ending_free(&e);
        free(file);
        free(mus);
    }
    printf("RESULT %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

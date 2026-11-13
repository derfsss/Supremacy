/*
 * Supremacy port - fuzz the archive / disk extractor (src/platform/extract.c)
 * with damaged inputs, under AddressSanitizer and UBSan.
 *
 * Usage: fuzz_extract <out dir> <rounds> <seed> <file> [file ...]
 * Each round takes one of the given files (a real .lha / .zip / .ipf),
 * damages it (flipped bytes, cut short, a block copied over another, sizes
 * and lengths set to extremes) and extracts it into an empty folder. The
 * extractor must return (any result) without a crash or a sanitizer report.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "extract.h"

size_t fuzz_data_len;
unsigned char *fuzz_data;

static uint32_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void clear_dir(const char *d)
{
    DIR *dir = opendir(d);
    if (!dir) return;
    struct dirent *e;
    char p[1100];
    while ((e = readdir(dir)))
        if (e->d_name[0] != '.') { snprintf(p, sizeof p, "%s/%s", d, e->d_name); unlink(p); }
    closedir(dir);
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
    const char *out = argv[1];
    long rounds = atol(argv[2]);
    rng = (uint32_t)strtoul(argv[3], NULL, 0) | 1;
    int nf = argc - 4;
    unsigned char **orig = calloc((size_t)nf, sizeof *orig);
    size_t *olen = calloc((size_t)nf, sizeof *olen);
    for (int i = 0; i < nf; i++) {
        FILE *f = fopen(argv[4 + i], "rb");
        if (!f) { perror(argv[4 + i]); return 2; }
        fseek(f, 0, SEEK_END);
        olen[i] = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        orig[i] = malloc(olen[i]);
        if (fread(orig[i], 1, olen[i], f) != olen[i]) return 2;
        fclose(f);
    }
    long results[4] = {0};
    for (long r = 0; r < rounds; r++) {
        int k = (int)(rnd() % (uint32_t)nf);
        size_t n = olen[k];
        fuzz_data = malloc(n + 64);
        memcpy(fuzz_data, orig[k], n);
        int kind = (int)(rnd() % 6);
        int edits = 1 + (int)(rnd() % 16);
        for (int e = 0; e < edits; e++) {
            size_t at = n ? rnd() % n : 0;
            switch (kind) {
            case 0: fuzz_data[at] ^= (unsigned char)(1u << (rnd() % 8)); break;   /* bit flips */
            case 1: fuzz_data[at] = (unsigned char)rnd(); break;                  /* bytes */
            case 2: n = at; break;                                                /* cut short */
            case 3: {                                                             /* a block moved */
                size_t from = n ? rnd() % n : 0, len = rnd() % 4096;
                if (from + len < n && at + len < n) memmove(fuzz_data + at, fuzz_data + from, len);
                break;
            }
            case 4: {                                                             /* extreme words */
                static const uint32_t v[] = {0, 1, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFF, 0x10000};
                uint32_t x = v[rnd() % 7];
                if (at + 4 <= n) memcpy(fuzz_data + at, &x, 4);
                break;
            }
            default:                                                              /* header area */
                if (n) fuzz_data[rnd() % (n < 256 ? n : 256)] = (unsigned char)rnd();
                break;
            }
        }
        fuzz_data_len = n;
        clear_dir(out);
        char msg[200];
        int res = extract_game_data("fuzz", out, msg, sizeof msg);
        results[res + 1 < 0 ? 0 : (res + 1 > 3 ? 3 : res + 1)]++;
        free(fuzz_data);
        if (r % 500 == 0) { printf("round %ld: %d %s\n", r, res, msg); fflush(stdout); }
    }
    printf("rounds %ld: failed %ld, complete %ld, partial %ld\nRESULT PASS (no crash)\n", rounds,
           results[0], results[1], results[2]);
    return 0;
}

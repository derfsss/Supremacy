/* A minimal stand-in for the SDL functions src/platform/extract.c uses, so
 * the extractor can be fuzzed on Linux without SDL (tools/fuzz). */
#ifndef FUZZ_SDL_SHIM_H
#define FUZZ_SDL_SHIM_H
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define SDL_snprintf snprintf
#define SDL_strcasecmp strcasecmp
#define SDL_free free
typedef struct SDL_PathInfo { int type; long size; } SDL_PathInfo;

extern size_t fuzz_data_len;                 /* the file "loaded": the fuzzer's buffer */
extern unsigned char *fuzz_data;

static inline void *SDL_LoadFile(const char *path, size_t *n)
{
    (void)path;
    unsigned char *c = malloc(fuzz_data_len ? fuzz_data_len : 1);
    if (!c) return NULL;
    memcpy(c, fuzz_data, fuzz_data_len);
    *n = fuzz_data_len;
    return c;
}
static inline const char *SDL_GetError(void) { return "shim"; }
static inline bool SDL_CreateDirectory(const char *p) { return mkdir(p, 0777) == 0; }
static inline bool SDL_GetPathInfo(const char *p, SDL_PathInfo *i)
{
    struct stat st;
    if (stat(p, &st)) return false;
    if (i) { i->type = 0; i->size = (long)st.st_size; }
    return true;
}
static inline size_t SDL_strlcat(char *d, const char *s, size_t n)
{
    size_t a = strlen(d), b = strlen(s);
    if (a + 1 < n) { size_t c = b < n - a - 1 ? b : n - a - 1; memcpy(d + a, s, c); d[a + c] = 0; }
    return a + b;
}
#endif

/* Game data from a .lha / .zip of the WHDLoad install. See extract.h. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extract.h"

/* ------------------------------------------------------------ the files */

static const char *WANTED[] = {"GAM1", "GAM2", "INT1", "INT2", "Main", "WIN0", "WIN1",
                               "WIN2", "WIN3", "WIN.", "WINM", "DIEM"};
#define NWANTED ((int)(sizeof WANTED / sizeof WANTED[0]))
#define WIN_DEFEAT 9                       /* "WIN." (also accepted as WIN_) */

/* index in WANTED of an archive entry's name (its last path part), or -1 */
static int wanted(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p == 0xFF) b = p + 1;
    for (int k = 0; k < NWANTED; k++) {
        if (!SDL_strcasecmp(b, WANTED[k])) return k;
        if (k == WIN_DEFEAT && !SDL_strcasecmp(b, "WIN_")) return k;
    }
    return -1;
}

static int write_file(const char *dir, int k, const uint8_t *d, size_t n)
{
    char path[1200];
#ifdef _WIN32
    const char *name = k == WIN_DEFEAT ? "WIN_" : WANTED[k];
#else
    const char *name = WANTED[k];
#endif
    SDL_snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(d, 1, n, f);
    return (fclose(f) == 0 && w == n) ? 0 : -1;
}

/* ------------------------------------------------------------ CRCs */

static uint16_t crc16(const uint8_t *d, size_t n)    /* CRC-16/ARC, as LHA */
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    return c;
}

static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* ------------------------------------------------------------ -lh5- */

#define LH_NC 510                          /* 256 literals + lengths 3..256 */
#define LH_NT 19
#define LH_NP 14                           /* lh5: 13-bit window + 1 */
#define LH_CBIT 9
#define LH_TBIT 5
#define LH_PBIT 4

typedef struct Lh {
    const uint8_t *in;
    size_t in_n, in_pos;
    uint16_t bitbuf;
    uint8_t subbitbuf;
    int bitcount;
    uint16_t left[2 * LH_NC - 1], right[2 * LH_NC - 1];
    uint8_t c_len[LH_NC], pt_len[LH_NT > LH_NP ? LH_NT : LH_NP];
    uint16_t c_table[4096], pt_table[256];
    unsigned blocksize;
    int bad;
} Lh;

static void fillbuf(Lh *h, int n)
{
    if (n < 0 || n > 16) { h->bad = 1; return; }   /* a damaged table asked for this */
    while (n > h->bitcount) {
        n -= h->bitcount;
        h->bitbuf = (uint16_t)((h->bitbuf << h->bitcount) | (h->subbitbuf >> (8 - h->bitcount)));
        h->subbitbuf = h->in_pos < h->in_n ? h->in[h->in_pos++] : 0;
        h->bitcount = 8;
    }
    h->bitcount -= n;
    h->bitbuf = (uint16_t)((h->bitbuf << n) | (h->subbitbuf >> (8 - n)));
    h->subbitbuf = (uint8_t)(h->subbitbuf << n);
}

static unsigned getbits(Lh *h, int n)
{
    unsigned x = h->bitbuf >> (16 - n);
    fillbuf(h, n);
    return x;
}

static void make_table(Lh *h, int nchar, const uint8_t *bitlen, int tablebits, uint16_t *table)
{
    uint32_t count[17] = {0}, weight[17], start[18];
    for (int i = 0; i < nchar; i++) {
        if (bitlen[i] > 16) { h->bad = 1; return; }
        count[bitlen[i]]++;
    }
    start[1] = 0;
    for (int i = 1; i <= 16; i++) start[i + 1] = start[i] + (count[i] << (16 - i));
    if (start[17] != 0x10000u) { h->bad = 1; return; }   /* the code space must be exactly full */
    int jutbits = 16 - tablebits;
    for (int i = 1; i <= tablebits; i++) {
        start[i] >>= jutbits;
        weight[i] = 1u << (tablebits - i);
    }
    for (int i = tablebits + 1; i <= 16; i++) weight[i] = 1u << (16 - i);
    uint32_t i = (start[tablebits + 1] >> jutbits) & 0xFFFF;
    if (i != 0) {
        uint32_t k = 1u << tablebits;
        while (i != k) table[i++] = 0;
    }
    int avail = nchar;
    uint32_t mask = 1u << (15 - tablebits);
    for (int ch = 0; ch < nchar; ch++) {
        int len = bitlen[ch];
        if (!len) continue;
        uint32_t k = start[len], next = k + weight[len];
        if (len <= tablebits) {
            if (next > (1u << tablebits)) { h->bad = 1; return; }
            for (uint32_t j = k; j < next; j++) table[j] = (uint16_t)ch;
        } else {
            uint16_t *p = &table[k >> jutbits];
            int n = len - tablebits;
            while (n--) {
                if (*p == 0) {
                    if (avail >= 2 * LH_NC - 1) { h->bad = 1; return; }
                    h->right[avail] = h->left[avail] = 0;
                    *p = (uint16_t)avail++;
                }
                p = (k & mask) ? &h->right[*p] : &h->left[*p];
                k <<= 1;
            }
            *p = (uint16_t)ch;
        }
        start[len] = next;
    }
}

static void read_pt_len(Lh *h, int nn, int nbit, int special)
{
    int n = (int)getbits(h, nbit);
    if (n == 0) {
        unsigned c = getbits(h, nbit);
        memset(h->pt_len, 0, (size_t)nn);
        for (int i = 0; i < 256; i++) h->pt_table[i] = (uint16_t)c;
        return;
    }
    if (n > nn) { h->bad = 1; return; }
    int i = 0;
    while (i < n) {
        int c = h->bitbuf >> 13;
        if (c == 7) {
            uint16_t m = 1u << 12;
            while (m & h->bitbuf) { m >>= 1; c++; }
        }
        fillbuf(h, c < 7 ? 3 : c - 3);
        h->pt_len[i++] = (uint8_t)c;
        if (i == special) {
            int z = (int)getbits(h, 2);
            while (z-- > 0 && i < nn) h->pt_len[i++] = 0;
        }
    }
    while (i < nn) h->pt_len[i++] = 0;
    make_table(h, nn, h->pt_len, 8, h->pt_table);
}

static void read_c_len(Lh *h)
{
    int n = (int)getbits(h, LH_CBIT);
    if (n == 0) {
        unsigned c = getbits(h, LH_CBIT);
        memset(h->c_len, 0, LH_NC);
        for (int i = 0; i < 4096; i++) h->c_table[i] = (uint16_t)c;
        return;
    }
    if (n > LH_NC) { h->bad = 1; return; }
    int i = 0;
    while (i < n) {
        int c = h->pt_table[h->bitbuf >> 8];
        if (c >= LH_NT) {
            uint16_t m = 1u << 7;
            do {
                c = (h->bitbuf & m) ? h->right[c] : h->left[c];
                m >>= 1;
            } while (c >= LH_NT && m);
        }
        fillbuf(h, h->pt_len[c]);
        if (c <= 2) {
            if (c == 0) c = 1;
            else if (c == 1) c = (int)getbits(h, 4) + 3;
            else c = (int)getbits(h, LH_CBIT) + 20;
            while (c-- > 0 && i < LH_NC) h->c_len[i++] = 0;
        } else {
            h->c_len[i++] = (uint8_t)(c - 2);
        }
    }
    while (i < LH_NC) h->c_len[i++] = 0;
    make_table(h, LH_NC, h->c_len, 12, h->c_table);
}

static unsigned decode_c(Lh *h)
{
    if (h->blocksize == 0) {
        h->blocksize = getbits(h, 16);
        read_pt_len(h, LH_NT, LH_TBIT, 3);
        read_c_len(h);
        read_pt_len(h, LH_NP, LH_PBIT, -1);
    }
    h->blocksize--;
    unsigned j = h->c_table[h->bitbuf >> 4];
    if (j < LH_NC) {
        fillbuf(h, h->c_len[j]);
    } else {
        fillbuf(h, 12);
        uint16_t m = 1u << 15;
        do {
            j = (h->bitbuf & m) ? h->right[j] : h->left[j];
            m >>= 1;
        } while (j >= LH_NC && m);
        fillbuf(h, h->c_len[j] - 12);
    }
    return j;
}

static unsigned decode_p(Lh *h)
{
    unsigned j = h->pt_table[h->bitbuf >> 8];
    if (j < LH_NP) {
        fillbuf(h, h->pt_len[j]);
    } else {
        fillbuf(h, 8);                      /* the walk goes on from the top bit */
        uint16_t m = 1u << 15;
        do {
            j = (h->bitbuf & m) ? h->right[j] : h->left[j];
            m >>= 1;
        } while (j >= LH_NP && m);
        fillbuf(h, h->pt_len[j] - 8);
    }
    if (j != 0) j = (1u << (j - 1)) + getbits(h, (int)j - 1);
    return j;
}

static int lh5_decode(const uint8_t *in, size_t in_n, uint8_t *out, size_t out_n)
{
    Lh *h = calloc(1, sizeof *h);
    if (!h) return -1;
    h->in = in;
    h->in_n = in_n;
    fillbuf(h, 16);
    size_t o = 0;
    while (o < out_n && !h->bad) {
        unsigned c = decode_c(h);
        if (c < 256) {
            out[o++] = (uint8_t)c;
        } else {
            size_t len = c - 256 + 3, dist = decode_p(h) + 1;
            if (dist > o) { h->bad = 1; break; }
            while (len-- && o < out_n) { out[o] = out[o - dist]; o++; }
        }
    }
    int bad = h->bad;
    free(h);
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------ LHA */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static int lha_extract(const uint8_t *d, size_t n, const char *dir, int *got, char *msg, size_t ml)
{
    size_t o = 0;
    while (o + 24 < n && d[o] != 0) {
        int level = d[o + 20];
        const uint8_t *m = d + o + 2;
        uint32_t packed = le32(d + o + 7), size = le32(d + o + 11);
        char name[256] = "";
        size_t data;
        uint16_t crc = 0;
        if (level == 0 || level == 1) {
            size_t hs = d[o];
            int nl = d[o + 21];
            if (o + 22 + (size_t)nl + 2 > n || o + hs + 2 > n) break;
            memcpy(name, d + o + 22, (size_t)nl < sizeof name ? (size_t)nl : sizeof name - 1);
            crc = le16(d + o + 22 + nl);
            data = o + hs + 2;
            if (level == 1) {                       /* extended headers: in `packed` */
                size_t e = o + hs;                  /* the next-size word */
                uint16_t es = le16(d + e);
                size_t q = data;
                while (es && q + es <= n) {
                    if (es < 3 || es > packed) { SDL_snprintf(msg, ml, "THE ARCHIVE IS DAMAGED"); return -1; }
                    if (d[q] == 0x01 && es > 3) {   /* file name */
                        size_t l = (size_t)es - 3 < sizeof name - 1 ? (size_t)es - 3 : sizeof name - 1;
                        memcpy(name, d + q + 1, l);
                        name[l] = 0;
                    }
                    packed -= es;
                    q += es;
                    es = le16(d + q - 2);
                }
                data = q;
            }
        } else if (level == 2) {
            size_t hs = le16(d + o);
            if (hs < 26 || o + hs > n) { SDL_snprintf(msg, ml, "THE ARCHIVE IS DAMAGED"); return -1; }
            crc = le16(d + o + 21);
            size_t q = o + 26;                      /* extended headers after the size word at 24 */
            uint16_t es = le16(d + o + 24);
            while (es && q + es <= o + hs) {
                if (es < 3) { SDL_snprintf(msg, ml, "THE ARCHIVE IS DAMAGED"); return -1; }
                if (d[q] == 0x01 && es > 3) {
                    size_t l = (size_t)es - 3 < sizeof name - 1 ? (size_t)es - 3 : sizeof name - 1;
                    memcpy(name, d + q + 1, l);
                    name[l] = 0;
                }
                q += es;
                es = le16(d + q - 2);
            }
            data = o + hs;
        } else {
            SDL_snprintf(msg, ml, "UNSUPPORTED LHA HEADER LEVEL %d", level);
            return -1;
        }
        /* (compared so that it cannot overflow on a 32-bit size_t) */
        if (data > n || packed > n - data) { SDL_snprintf(msg, ml, "THE ARCHIVE IS CUT SHORT"); return -1; }
        int k = wanted(name);
        if (k >= 0 && !got[k]) {
            if (size > 4u << 20) { SDL_snprintf(msg, ml, "%s IS DAMAGED IN THE ARCHIVE", WANTED[k]); return -1; }
            uint8_t *buf = malloc(size ? size : 1);
            if (!buf) return -1;
            int r;
            if (!memcmp(m, "-lh0-", 5) && packed == size) { memcpy(buf, d + data, size); r = 0; }
            else if (!memcmp(m, "-lh5-", 5)) r = lh5_decode(d + data, packed, buf, size);
            else { SDL_snprintf(msg, ml, "%s: UNSUPPORTED METHOD %.5s", WANTED[k], (const char *)m); free(buf); return -1; }
            if (r || crc16(buf, size) != crc) {
                SDL_snprintf(msg, ml, "%s IS DAMAGED IN THE ARCHIVE", WANTED[k]);
                free(buf);
                return -1;
            }
            r = write_file(dir, k, buf, size);
            free(buf);
            if (r) { SDL_snprintf(msg, ml, "CANNOT WRITE %s", WANTED[k]); return -1; }
            got[k] = 1;
        }
        o = data + packed;
    }
    return 0;
}

/* ------------------------------------------------------------ inflate */

typedef struct Inf {
    const uint8_t *in;
    size_t n, pos;
    uint32_t bits;
    int nb;
    uint8_t *out;
    size_t on, op;
} Inf;

typedef struct Huff { uint16_t count[16], sym[288]; } Huff;

static int bit(Inf *s, int need)
{
    while (s->nb < need) {
        if (s->pos >= s->n) return -1;
        s->bits |= (uint32_t)s->in[s->pos++] << s->nb;
        s->nb += 8;
    }
    int v = (int)(s->bits & ((1u << need) - 1));
    s->bits >>= need;
    s->nb -= need;
    return v;
}

static void huff_build(Huff *h, const uint8_t *len, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}

static int huff_decode(Inf *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = bit(s, 1);
        if (b < 0) return -1;
        code |= b;
        int c = h->count[len];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_codes(Inf *s, const Huff *lit, const Huff *dist)
{
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35,
                                       43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4,
                                     4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
                                       385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
                                       12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9,
                                     9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->op >= s->on) return -1;
            s->out[s->op++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int e = bit(s, lext[sym]);
            if (e < 0) return -1;
            size_t len = (size_t)lbase[sym] + (size_t)e;
            int ds = huff_decode(s, dist);
            if (ds < 0 || ds >= 30) return -1;
            e = bit(s, dext[ds]);
            if (e < 0) return -1;
            size_t d = (size_t)dbase[ds] + (size_t)e;
            if (d > s->op || s->op + len > s->on) return -1;
            while (len--) { s->out[s->op] = s->out[s->op - d]; s->op++; }
        }
    }
}

static int inflate(const uint8_t *in, size_t n, uint8_t *out, size_t on)
{
    Inf s = {in, n, 0, 0, 0, out, on, 0};
    static Huff lit, dist;
    int last;
    do {
        last = bit(&s, 1);
        int type = bit(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {                                   /* stored */
            s.bits = 0;
            s.nb = 0;
            if (s.pos + 4 > n) return -1;
            size_t len = le16(in + s.pos);
            s.pos += 4;
            if (s.pos + len > n || s.op + len > on) return -1;
            memcpy(out + s.op, in + s.pos, len);
            s.pos += len;
            s.op += len;
        } else if (type == 1) {                            /* fixed codes */
            uint8_t l[320];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(&lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dist, l, 30);
            if (inflate_codes(&s, &lit, &dist)) return -1;
        } else if (type == 2) {                            /* dynamic codes */
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = bit(&s, 5) + 257, ndist = bit(&s, 5) + 1, ncode = bit(&s, 4) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            uint8_t l[320] = {0};
            for (int i = 0; i < ncode; i++) {
                int v = bit(&s, 3);
                if (v < 0) return -1;
                l[order[i]] = (uint8_t)v;
            }
            Huff code;
            huff_build(&code, l, 19);
            int i = 0;
            while (i < nlen + ndist) {
                int sym = huff_decode(&s, &code);
                if (sym < 0) return -1;
                if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
                int rep, val = 0;
                if (sym == 16) { if (!i) return -1; val = l[i - 1]; rep = 3 + bit(&s, 2); }
                else if (sym == 17) rep = 3 + bit(&s, 3);
                else rep = 11 + bit(&s, 7);
                if (i + rep > nlen + ndist) return -1;
                while (rep--) l[i++] = (uint8_t)val;
            }
            uint8_t dl[30];
            memcpy(dl, l + nlen, (size_t)ndist);
            huff_build(&lit, l, nlen);
            huff_build(&dist, dl, ndist);
            if (inflate_codes(&s, &lit, &dist)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return s.op == on ? 0 : -1;
}

/* ------------------------------------------------------------ IPF disks */
/*
 * The original disks as IPF images (SPS release 1384): each track is a list
 * of blocks of stream elements (sync and raw: MFM bits as they are; data:
 * bytes to MFM-encode; gaps), which rebuilds the track's MFM. The disks are
 * read as the WHDLoad install's Supremacy.Imager (Dark Angel, 1999) reads
 * them: trackdisk track T = cylinder x 2 + side;
 *   - every track but one: 11 sectors, sync $4489, marker $2AAAAAAA, then
 *     $104 words of odd bits and $104 of even bits: a header word (sector
 *     number in its low byte), 512 bytes, a checksum long (header + the 128
 *     longs);
 *   - track 2 ("Main", 4,096 bytes): one block after $4489 $2AAA, $804
 *     words odd/even, the file 4 bytes in;
 *   - a directory in sector 0 of track 4 (disk 1 side 0, "SUP.boot"),
 *     track 1 (side 1: "SUP.game", disk 2 "SUP.endw") or track 0 (disk 2
 *     side 0, "SUP.endd"): "SUP.xxxx", then up to 63 entries of ID (4; '/'
 *     is '.'), track, sector, length in sectors; a file runs on through the
 *     sectors of that side (track + 2).
 */

typedef struct Bits { uint8_t *b; size_t n, cap; } Bits;

static int bits_put(Bits *t, int v)
{
    if (t->n >= 1u << 18) return -1;                       /* a real track is ~100,000 bits */
    if (t->n == t->cap) {
        size_t c = t->cap ? t->cap * 2 : 131072;
        uint8_t *nb = realloc(t->b, c);
        if (!nb) return -1;
        t->b = nb;
        t->cap = c;
    }
    t->b[t->n++] = (uint8_t)v;
    return 0;
}

static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* the MFM bits of one track from its IMGE record and DATA block */
static int ipf_track(const uint8_t *imge, const uint8_t *ext, size_t extn, Bits *t)
{
    if (!imge || !ext) return -1;                          /* a track the image does not have */
    t->n = 0;
    uint32_t blocks = be32p(imge + 40);
    int prev = 0;
    for (uint32_t bi = 0; bi < blocks; bi++) {
        if ((size_t)bi * 32 + 32 > extn) return -1;
        const uint8_t *bd = ext + bi * 32;
        uint32_t gapbits = be32p(bd + 4), flags = be32p(bd + 20), dataoff = be32p(bd + 28);
        if (gapbits > 1u << 18) return -1;                  /* a track is ~100,000 bits */
        size_t p = dataoff;
        for (;;) {
            if (p >= extn) return -1;
            unsigned head = ext[p++], type = head & 0x1F, w = head >> 5;
            if (type == 0) break;
            if (p + w > extn) return -1;
            uint32_t size = 0;
            for (unsigned k = 0; k < w; k++) size = size << 8 | ext[p++];
            uint32_t nbits = (flags & 4) ? size : size * 8;
            size_t nbytes = (nbits + 7) / 8;
            if (p + nbytes > extn) return -1;
            const uint8_t *c = ext + p;
            p += nbytes;
            if (type == 1 || type == 4) {                   /* sync, raw: MFM as stored */
                for (uint32_t i = 0; i < nbits; i++) {
                    prev = (c[i / 8] >> (7 - i % 8)) & 1;
                    if (bits_put(t, prev)) return -1;
                }
            } else if (type == 2) {                         /* data: MFM-encode */
                for (uint32_t i = 0; i < nbits; i++) {
                    int b = (c[i / 8] >> (7 - i % 8)) & 1;
                    if (bits_put(t, !prev && !b) || bits_put(t, b)) return -1;
                    prev = b;
                }
            }                                               /* 3: gap marks; 5: fuzzy */
        }
        for (uint32_t i = 0; i + 1 < gapbits; i += 2)       /* the gap: $AA cells */
            if (bits_put(t, 1) || bits_put(t, 0)) return -1;
        prev = 0;
    }
    return 0;
}

static unsigned bw(const Bits *t, size_t i)                 /* 16 bits from bit i (wraps) */
{
    unsigned v = 0;
    for (int k = 0; k < 16; k++) v = v << 1 | t->b[(i + (size_t)k) % t->n];
    return v;
}

/* $7F6: the 11 sectors of a track into sec[11][512]; returns a bit mask of
 * the sectors found */
static unsigned ipf_sectors(const Bits *t, uint8_t (*sec)[512])   /* sec: 11 rows */
{
    unsigned found = 0;
    if (t->n < 64) return 0;
    for (size_t i = 0; i < t->n; i++) {
        if (bw(t, i) != 0x4489) continue;
        size_t j = i;
        while (bw(t, j) == 0x4489) j += 16;
        if (bw(t, j) != 0x2AAA || bw(t, j + 16) != 0xAAAA) { i = j - 1; continue; }
        j += 32;
        uint8_t raw[520];
        for (int k = 0; k < 0x104; k++) {
            unsigned v = (bw(t, j + 16 * (size_t)k) & 0x5555) | ((bw(t, j + 16 * (size_t)(0x104 + k)) & 0x5555) << 1);
            raw[2 * k] = (uint8_t)(v >> 8);
            raw[2 * k + 1] = (uint8_t)v;
        }
        uint32_t sum = (uint32_t)(raw[0] << 8 | raw[1]);
        for (int k = 0; k < 128; k++) sum += be32p(raw + 2 + 4 * k);
        unsigned s = raw[1];
        if (sum == be32p(raw + 514) && s < 11) {
            memcpy(sec[s], raw + 2, 512);
            found |= 1u << s;
        }
        i = j + 16 * 0x208 - 1;
    }
    return found;
}

/* $7A8: "Main" on track 2 */
static int ipf_main(const Bits *t, uint8_t *out)
{
    if (t->n < 64) return -1;
    for (size_t i = 0; i < t->n; i++) {
        if (bw(t, i) != 0x4489) continue;
        size_t j = i;
        while (bw(t, j) == 0x4489) j += 16;
        if (bw(t, j) != 0x2AAA) continue;
        j += 16;
        static uint8_t raw[0x804 * 2];
        for (int k = 0; k < 0x804; k++) {
            unsigned v = (bw(t, j + 16 * (size_t)k) & 0x5555) | ((bw(t, j + 16 * (size_t)(0x804 + k)) & 0x5555) << 1);
            raw[2 * k] = (uint8_t)(v >> 8);
            raw[2 * k + 1] = (uint8_t)v;
        }
        if (raw[0] || raw[1]) continue;                     /* tst.w (a4)+ */
        uint32_t sum = (uint32_t)(raw[2] << 8 | raw[3]);
        for (int k = 0; k < 0x400; k++) sum += be32p(raw + 4 + 4 * k);
        if (sum != be32p(raw + 4 + 0x1000)) continue;
        memcpy(out, raw + 4, 0x1000);
        return 0;
    }
    return -1;
}

typedef struct Ipf {
    const uint8_t *imge[84 * 2];                            /* by trackdisk track */
    const uint8_t *ext[84 * 2];
    size_t extn[84 * 2];
    Bits bits;
    int cached;                                             /* track in `sec`, or -1 */
    uint8_t sec[11][512];
    unsigned found;
} Ipf;

static const uint8_t *ipf_sector(Ipf *f, int T, int s)
{
    if (T < 0 || T >= 84 * 2 || !f->imge[T]) return NULL;
    if (f->cached != T) {
        f->cached = T;
        f->found = 0;
        if (ipf_track(f->imge[T], f->ext[T], f->extn[T], &f->bits) == 0)
            f->found = ipf_sectors(&f->bits, f->sec);
    }
    return (f->found >> s) & 1 ? f->sec[s] : NULL;
}

static int ipf_extract(const uint8_t *d, size_t n, const char *dir, int *got, char *msg, size_t ml)
{
    Ipf *f = calloc(1, sizeof *f);
    if (!f) return -1;
    f->cached = -1;
    /* records: type, length, crc; IMGE (track, side, ..., data key at +52);
     * DATA (its extra data follows the record) */
    const uint8_t *imges[84 * 2] = {0};
    uint32_t ikey[84 * 2] = {0};
    size_t o = 0;
    while (o + 12 <= n) {
        uint32_t len = be32p(d + o + 4);
        if (len < 12 || o + len > n) break;
        const uint8_t *body = d + o + 12;
        if (!memcmp(d + o, "IMGE", 4) && len >= 12 + 68) {
            uint32_t cyl = be32p(body), side = be32p(body + 4);
            if (cyl < 84 && side < 2) {
                imges[cyl * 2 + side] = body;
                ikey[cyl * 2 + side] = be32p(body + 52);
            }
        }
        if (!memcmp(d + o, "DATA", 4) && len >= 12 + 16) {
            uint32_t dl = be32p(body), key = be32p(body + 12);
            for (int T = 0; T < 84 * 2; T++)
                if (imges[T] && ikey[T] == key && o + len + dl <= n) {
                    f->imge[T] = imges[T];
                    f->ext[T] = d + o + len;
                    f->extn[T] = dl;
                }
            o += dl;
        }
        o += len;
    }
    static const struct { int base, T; } dirs[3] = {{0, 4}, {1, 1}, {0, 0}};
    int ndirs = 0, rc = 0;
    for (int di = 0; di < 3 && !rc; di++) {
        const uint8_t *d0 = ipf_sector(f, dirs[di].T, 0);
        if (!d0 || memcmp(d0, "SUP.", 4)) continue;
        uint8_t dirsec[512];
        memcpy(dirsec, d0, 512);
        ndirs++;
        if (!memcmp(dirsec, "SUP.boot", 8)) {               /* disk 1: Main on track 2 too */
            static uint8_t mainf[0x1000];
            int k = wanted("Main");
            if (k >= 0 && !got[k] && ipf_main((ipf_track(f->imge[2], f->ext[2], f->extn[2], &f->bits), &f->bits), mainf) == 0) {
                f->cached = -1;
                if (write_file(dir, k, mainf, sizeof mainf)) { SDL_snprintf(msg, ml, "CANNOT WRITE Main"); rc = -1; break; }
                got[k] = 1;
            }
            f->cached = -1;
        }
        for (int e = 0; e < 63; e++) {
            const uint8_t *en = dirsec + 8 + 8 * e;
            if (!en[0]) break;
            char id[5] = {(char)en[0], (char)en[1], (char)en[2], (char)en[3], 0};
            for (int c = 0; c < 4; c++) if (id[c] == '/') id[c] = '.';
            int k = wanted(id);
            if (k < 0 || got[k]) continue;
            unsigned len = (unsigned)(en[6] << 8 | en[7]), s = en[5] & 0x7F;
            int T = en[4] * 2 + dirs[di].base;
            uint8_t *buf = malloc((size_t)len * 512 + 1);
            if (!buf) { rc = -1; break; }
            int bad = 0;
            for (unsigned q = 0; q < len; q++) {
                const uint8_t *sp = ipf_sector(f, T, (int)s);
                if (!sp) { bad = 1; break; }
                memcpy(buf + (size_t)q * 512, sp, 512);
                if (++s == 11) { s = 0; T += 2; }
            }
            if (bad) {
                SDL_snprintf(msg, ml, "%s: A SECTOR CANNOT BE READ ON THIS DISK", WANTED[k]);
                free(buf);
                rc = -1;
                break;
            }
            int w = write_file(dir, k, buf, (size_t)len * 512);
            free(buf);
            if (w) { SDL_snprintf(msg, ml, "CANNOT WRITE %s", WANTED[k]); rc = -1; break; }
            got[k] = 1;
        }
    }
    free(f->bits.b);
    free(f);
    if (!rc && !ndirs) { SDL_snprintf(msg, ml, "NOT A SUPREMACY DISK"); rc = -1; }
    return rc;
}

/* ------------------------------------------------------------ ZIP */

static int zip_extract(const uint8_t *d, size_t n, const char *dir, int *got, char *msg, size_t ml)
{
    if (n < 22) { SDL_snprintf(msg, ml, "NOT A ZIP ARCHIVE"); return -1; }
    size_t e = n - 22;
    for (;; e--) {                                         /* end of central directory */
        if (le32(d + e) == 0x06054B50u) break;
        if (e == 0 || n - e > 22 + 65535) { SDL_snprintf(msg, ml, "NOT A ZIP ARCHIVE"); return -1; }
    }
    unsigned count = le16(d + e + 10);
    size_t c = le32(d + e + 16);
    for (unsigned i = 0; i < count; i++) {
        if (c + 46 > n || le32(d + c) != 0x02014B50u) { SDL_snprintf(msg, ml, "THE ZIP DIRECTORY IS DAMAGED"); return -1; }
        unsigned method = le16(d + c + 10), nl = le16(d + c + 28), xl = le16(d + c + 30), cl = le16(d + c + 32);
        if (c + 46 + nl > n) { SDL_snprintf(msg, ml, "THE ZIP DIRECTORY IS DAMAGED"); return -1; }
        uint32_t crc = le32(d + c + 16), packed = le32(d + c + 20), size = le32(d + c + 24);
        size_t lo = le32(d + c + 42);
        char name[256] = "";
        memcpy(name, d + c + 46, nl < sizeof name ? nl : sizeof name - 1);
        c += 46 + nl + xl + cl;
        size_t nlen = strlen(name);
        int is_ipf = nlen > 4 && !SDL_strcasecmp(name + nlen - 4, ".ipf");
        int k = is_ipf ? -2 : wanted(name);
        if (k == -1 || (k >= 0 && got[k])) continue;
        if (lo + 30 > n || le32(d + lo) != 0x04034B50u) { SDL_snprintf(msg, ml, "THE ZIP IS DAMAGED"); return -1; }
        size_t data = lo + 30 + le16(d + lo + 26) + le16(d + lo + 28);
        if (data > n || packed > n - data) { SDL_snprintf(msg, ml, "THE ARCHIVE IS CUT SHORT"); return -1; }
        if (size > 4u << 20) { SDL_snprintf(msg, ml, "THE ZIP IS DAMAGED"); return -1; }
        uint8_t *buf = malloc(size ? size : 1);
        if (!buf) return -1;
        int r;
        if (method == 0 && packed == size) { memcpy(buf, d + data, size); r = 0; }
        else if (method == 8) r = inflate(d + data, packed, buf, size);
        else { SDL_snprintf(msg, ml, "%s: UNSUPPORTED ZIP METHOD %u", is_ipf ? name : WANTED[k], method); free(buf); return -1; }
        if (r || crc32(buf, size) != crc) {
            SDL_snprintf(msg, ml, "%s IS DAMAGED IN THE ARCHIVE", is_ipf ? name : WANTED[k]);
            free(buf);
            return -1;
        }
        if (is_ipf) {                                      /* a disk image in the zip */
            r = ipf_extract(buf, size, dir, got, msg, ml);
            free(buf);
            if (r) return -1;
            continue;
        }
        r = write_file(dir, k, buf, size);
        free(buf);
        if (r) { SDL_snprintf(msg, ml, "CANNOT WRITE %s", WANTED[k]); return -1; }
        got[k] = 1;
    }
    return 0;
}

/* ------------------------------------------------------------ entry points */

static int kind(const uint8_t *d, size_t n)
{
    if (n >= 4 && !memcmp(d, "CAPS", 4)) return 3;
    if (n >= 7 && d[2] == '-' && d[3] == 'l' && (d[4] == 'h' || d[4] == 'z') && d[6] == '-') return 1;
    if (n >= 4 && le32(d) == 0x04034B50u) return 2;
    return 0;
}

int extract_is_archive(const char *path)
{
    uint8_t b[8] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    return kind(b, n) != 0;
}

int extract_game_data(const char *archive, const char *out_dir, char *msg, size_t ml)
{
    size_t n = 0;
    uint8_t *d = SDL_LoadFile(archive, &n);
    if (!d) { SDL_snprintf(msg, ml, "CANNOT READ THE ARCHIVE (%s)", SDL_GetError()); return -1; }
    int k = kind(d, n);
    if (!k && (n == 901120 || n == 1802240)) {               /* a plain ADF */
        SDL_free(d);
        /* Plain ADFs cannot hold the original disks (custom tracks); the
         * cracked ones (PDX, Angels) crunch or patch the files. */
        SDL_snprintf(msg, ml, "CRACKED ADF, NOT USABLE: USE IPF DISKS OR THE WHDLOAD .LHA");
        return -1;
    }
    if (!k) { SDL_free(d); SDL_snprintf(msg, ml, "NOT AN LHA, ZIP OR IPF FILE"); return -1; }
    SDL_CreateDirectory(out_dir);
    int got[NWANTED] = {0}, had = 0;
    for (int i = 0; i < NWANTED; i++) {                     /* files from an earlier disk count */
        char p[1200];
        SDL_snprintf(p, sizeof p, "%s/%s", out_dir, i == WIN_DEFEAT ? "WIN_" : WANTED[i]);
        int ex = SDL_GetPathInfo(p, NULL);
        if (!ex && i == WIN_DEFEAT) {
            SDL_snprintf(p, sizeof p, "%s/WIN.", out_dir);
            ex = SDL_GetPathInfo(p, NULL);
        }
        if (ex) { got[i] = 1; had++; }
    }
    msg[0] = 0;
    int r = k == 1 ? lha_extract(d, n, out_dir, got, msg, ml)
          : k == 2 ? zip_extract(d, n, out_dir, got, msg, ml)
                   : ipf_extract(d, n, out_dir, got, msg, ml);
    SDL_free(d);
    if (r) { if (!msg[0]) SDL_snprintf(msg, ml, "OUT OF MEMORY"); return -1; }
    char miss[160] = "";
    int nmiss = 0;
    for (int i = 0; i < NWANTED; i++)
        if (!got[i]) {
            SDL_strlcat(miss, nmiss++ ? " " : "", sizeof miss);
            SDL_strlcat(miss, WANTED[i], sizeof miss);
        }
    int now = 0;
    for (int i = 0; i < NWANTED; i++) now += got[i];
    if (nmiss) {
        if (k == 3 || now > had) {                          /* a disk: the other one is needed */
            SDL_snprintf(msg, ml, "GOT %d FILES. STILL NEEDED: %s (THE OTHER DISK)", now - had, miss);
            return 1;
        }
        if (now == 0)
            SDL_snprintf(msg, ml, "NO AMIGA SUPREMACY FILES IN THIS ARCHIVE");
        else
            SDL_snprintf(msg, ml, "NOT IN THE ARCHIVE: %s", miss);
        return -1;
    }
    SDL_snprintf(msg, ml, "EXTRACTED %d FILES", now - had);
    return 0;
}

/* The original's screen buffer and text routines. See screen.h. */
#include <string.h>

#include "screen.h"

void screen_init(Screen *s, const uint8_t *font)
{
    memset(s, 0, sizeof *s);
    if (font) memcpy(s->font, font, sizeof s->font);
}

void screen_load_raw(Screen *s, const uint8_t *raw)
{
    memcpy(s->bpl, raw, SCREEN_SIZE);
}

/* Glyph bytes for character codes $20..$FF ($2904 indexes $597C8 with
 * (c - $20) x 10; codes past the copied font read as blank). */
static const uint8_t *glyph(const Screen *s, unsigned ch)
{
    static const uint8_t blank[10];
    unsigned i = (ch - 0x20u) & 0xFFFFu;
    return i < FONT_CHARS ? s->font + i * 10 : blank;
}

void ui_draw_char(Screen *s, unsigned col, unsigned row, unsigned ch)
{
    if (row + 5 > 200) return;                 /* outside the line table */
    unsigned x = (col & 0xFFu) >> 1;           /* lsr.b #1,d0b */
    if (x >= 40) return;
    uint8_t *p = s->bpl + row * SCREEN_LINE + x;
    const uint8_t *gl = glyph(s, ch);
    for (int r = 0; r < 5; r++) {
        for (int pl = 0; pl < 2; pl++) {       /* planes 0 and 1 */
            uint8_t *b = p + r * SCREEN_LINE + pl * 40;
            uint8_t v = gl[r * 2 + pl];
            if (col & 1)                       /* odd column: right nibble */
                *b = (uint8_t)((*b & 0xF0) | (v >> 4));
            else                               /* even column ($29AA): left */
                *b = (uint8_t)((*b & 0x0F) | v);
        }
    }
}

/* d0.b + 1: the column is a byte inside a word. */
static unsigned col_inc(unsigned col, unsigned n)
{
    return (col & 0xFF00u) | ((col + n) & 0xFFu);
}

/* $32D4: one digit by repeated subtraction. */
static unsigned digit(Screen *s, unsigned col, unsigned row, uint32_t *v, uint32_t p)
{
    unsigned ch = 0x2F;
    uint32_t d = *v;
    for (;;) {
        ch = (ch + 1) & 0xFFFFu;
        uint32_t before = d;
        d -= p;
        if (before < p) break;                 /* borrow */
    }
    d += p;
    *v = d;
    ui_draw_char(s, col, row, ch);
    return col_inc(col, 1);
}

unsigned ui_draw_number(Screen *s, unsigned col, unsigned row, uint32_t v)
{
    static const uint32_t pw[] = {10000, 1000, 100, 10, 1};
    int k;
    int32_t sv = (int32_t)v;                   /* cmpi.l / bge: signed */
    if (sv >= 10000) k = 0;
    else if (sv >= 1000) k = 1;
    else if (sv >= 100) k = 2;
    else if (sv >= 10) k = 3;
    else k = 4;
    for (; k < 5; k++) col = digit(s, col, row, &v, pw[k]);
    return col;
}

unsigned ui_draw_number_wide(Screen *s, unsigned col, unsigned row, uint32_t v)
{
    static const uint32_t pw[] = {10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    for (int k = 0; k < 8; k++) col = digit(s, col, row, &v, pw[k]);
    return col;
}

const uint8_t *ui_draw_string(Screen *s, unsigned col, unsigned row, const uint8_t *a, int typing)
{
    for (;;) {
        unsigned c = *a++;
        if (c == 0xFF) return a;
        if (c >= 0x20) {
            ui_draw_char(s, col, row, c);
            if (typing && s->char_hook) s->char_hook(s->hook_user);
            col = col_inc(col, 1);
            continue;
        }
        switch (c) {
        case 1: row = (row & 0xFF00u) | ((row + *a++) & 0xFFu); break;   /* add.b */
        case 2: col = col_inc(col, *a++); break;
        case 3: {                                  /* across */
            unsigned n = (unsigned)((*a++ - 1) & 0xFF) + 1, ch = *a++;
            for (unsigned k = 0; k < n; k++) {
                ui_draw_char(s, col, row, ch);
                col = col_inc(col, 1);
            }
            break;
        }
        case 4: {                                  /* down */
            unsigned n = (unsigned)((*a++ - 1) & 0xFF) + 1, ch = *a++;
            for (unsigned k = 0; k < n; k++) {
                ui_draw_char(s, col, row, ch);
                row = (row + 6) & 0xFFFFu;
            }
            break;
        }
        default:                                   /* not used by the game */
            return a;
        }
    }
}

void ui_message_scroll(Screen *s)
{
    /* Rows 90..182 of planes 0-1, bytes 20..37 (x 160-303), take the rows
     * 6 lines below; then rows 183..189 are cleared. */
    uint8_t *p = s->bpl + (0x76664u - SCREEN_BASE);
    for (int r = 0; r < 0x5D; r++, p += SCREEN_LINE) {
        memmove(p, p + 0x4B0, 18);
        memmove(p + 40, p + 0x4D8, 18);
    }
    for (int r = 0; r < 7; r++, p += SCREEN_LINE) {
        memset(p, 0, 18);
        memset(p + 40, 0, 18);
    }
}

static int in_block(uint32_t a) { return a >= STATE_BASE && a < STATE_BASE + STATE_SIZE; }

static const uint8_t *text(Game *g, uint32_t a, ui_text_fn fn, void *user)
{
    if (in_block(a)) return gp(g, a);
    return fn ? fn(user, a) : NULL;
}

/* $4A26: drop the current message. a4 = the ring entry after it. */
static void end_message(Game *g, uint32_t a4)
{
    s8(g, 0xE43D, 0);
    if (g32(g, a4) == 0xFE) a4 = G_MSG_RING;
    s16(g, G_MSG_COUNT, (uint16_t)(g16(g, G_MSG_COUNT) - 1));
    s32(g, 0xE3B4, a4);
}

void ui_message_pass(Screen *s, Game *g, ui_text_fn text_at, void *user)
{
    if (g8(g, 0xE429)) {                           /* $4994: scrolling */
        if (!g8(g, 0xE447)) {
            if (g8(g, 0xE448) == 0x11) {           /* panel full: blink prompt */
                unsigned t = g8(g, 0xE42C) & 7;
                uint32_t a = t == 5 ? 0x1CDE : t == 0 ? 0x1CA5 : 0;
                const uint8_t *str = a ? text(g, a, text_at, user) : NULL;
                if (str) ui_draw_string(s, 0x4D, 0x67, str, g8(g, 0xE43F));
                return;
            }
            s8(g, 0xE448, (uint8_t)(g8(g, 0xE448) + 1));
        }
        s8(g, 0xE429, (uint8_t)(g8(g, 0xE429) - 1));
        ui_message_scroll(s);
        return;
    }
    ui_message_type(s, g, text_at, user);
}

void ui_message_type(Screen *s, Game *g, ui_text_fn text_at, void *user)
{
    if (!g16(g, G_MSG_COUNT)) return;              /* $49EA: typing */
    if (g8(g, 0xE42A)) {
        s8(g, 0xE42A, (uint8_t)(g8(g, 0xE42A) - 1));
        return;
    }
    uint32_t a4 = g32(g, 0xE3B4);
    uint32_t a5 = g32(g, a4);
    a4 += 4;
    const uint8_t *t = text(g, a5, text_at, user);
    if (!t) { end_message(g, a4); return; }        /* unreadable text: drop it */
    unsigned c = t[0];
    a5++;
    switch (c) {
    case 0xFD:                                     /* new line */
        s8(g, 0xE429, 1);
        s32(g, a4 - 4, a5);
        s16(g, 0xE418, 0x29);
        break;
    case 0xFC:                                     /* wait */
        s8(g, 0xE42A, t[1]);
        s32(g, a4 - 4, a5 + 1);
        break;
    case 0xFB: {                                   /* number, then end */
        end_message(g, a4);
        uint32_t v = (uint32_t)(t[1] << 8 | t[2]);
        unsigned col = ui_draw_number(s, g16(g, 0xE418), 0xB8, v);
        s16(g, 0xE418, (uint16_t)col);
        break;
    }
    case 0xFF:
        end_message(g, a4);
        break;
    default: {
        s32(g, a4 - 4, a5);
        unsigned col = g16(g, 0xE418);
        s16(g, 0xE418, (uint16_t)(col + 1));
        ui_draw_char(s, col, 0xB8, c);
        break;
    }
    }
}

static uint8_t *screen_at(Screen *s, uint32_t a)
{
    static uint8_t sink;
    return (a >= SCREEN_BASE && a < SCREEN_BASE + SCREEN_SIZE) ? &s->bpl[a - SCREEN_BASE] : &sink;
}

void ui_star_map(Screen *s, Game *g, const uint8_t *sin_table, uint32_t d1_hi, uint32_t *arrow)
{
    uint32_t r = g32(g, G_PLANET_TABLE);
    unsigned n = g16(g, 0xE410);
    s8(g, 0xE428, (uint8_t)(g8(g, 0xE428) + 2));
    unsigned t = g8(g, 0xE428);
    for (unsigned k = 0; k <= n; k++, r += PLANET_SIZE) {
        /* erase: the saved byte back into all 5 planes */
        uint32_t old = g32(g, r);
        uint8_t saved = g8(g, r + 4);
        for (int pl = 0; pl < 5; pl++) *screen_at(s, old + pl * 40u) = saved;
        /* position */
        uint16_t ph = g16(g, r + 6);
        uint16_t idx = (uint16_t)((ph & 0xFF00u) | ((ph + t) & 0xFFu));
        uint8_t ang = (uint8_t)idx;
        int16_t sn = (int16_t)(sin_table[(int16_t)idx] << 8 | sin_table[(int16_t)idx + 1]);
        int32_t prod = (int32_t)sn * (int16_t)g16(g, r + 8);
        uint32_t sh = (uint32_t)prod << 2;
        uint32_t d0 = (sh << 16) | (((sh >> 16) + 0x50u) & 0xFFFFu);   /* swap, add.w */
        uint16_t row = g16(g, r + 10);
        unsigned q = ang >> 6;
        if (g8(g, r + 5)) {                        /* selected: the arrow follows */
            s32(g, 0xE3C0, d0);
            s32(g, 0xE3C4, (d1_hi << 16) | row);
            uint32_t img = q == 2 ? 0x592D0u : 0x592F4u;
            if (arrow) *arrow = img;
            s32(g, 0xE3CC, img);
        }
        if (q == 3) q = 1;                         /* the far side shows as 1 */
        uint16_t owner = g16(g, r + 12);
        unsigned colour;
        if (owner != 1 && owner != 4 && g16(g, r + 0x26) && g16(g, r + 0x22) &&
            (g8(g, 0xE42C) & 15) >= 8)
            colour = (q + (owner ^ 0x0Du)) & 0xFF;     /* under attack: flashes */
        else
            colour = (q + owner) & 0xFF;
        /* draw */
        uint16_t x = (uint16_t)d0;
        uint32_t a = SCREEN_BASE + (16u + (row >> 2)) * SCREEN_LINE + (x >> 3);
        s32(g, r, a);
        unsigned bit = (x & 7) ^ 7;
        for (int pl = 0; pl < 4; pl++) {
            uint8_t *b = screen_at(s, a + pl * 40u);
            *b = (uint8_t)(*b & ~(1u << bit));
            if (colour & (1u << pl)) *b |= (uint8_t)(1u << bit);
        }
        uint8_t *b5 = screen_at(s, a + 160u);
        s8(g, r + 4, *b5);
        *b5 = (uint8_t)(*b5 & ~(1u << bit));
    }
}

void screen_to_indices(const Screen *s, uint8_t *out)
{
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            unsigned idx = 0, bit = 7 - (x & 7);
            for (int pl = 0; pl < 5; pl++)
                idx |= ((s->bpl[y * SCREEN_LINE + pl * 40 + (x >> 3)] >> bit) & 1u) << pl;
            out[y * 320 + x] = (uint8_t)idx;
        }
}

/*
 * Screen: planet statistics ($825A, or $825E without the panel save;
 * picture $1E924, hotspot table $F48C, $E425 = 3). Translated routine by
 * routine; checked frame by frame against the original game running in an emulator.
 *
 * One frame() is one iteration of the loop at $8308:
 *   $2FFA wait (the caller's), $4ED8 viewer tick, $9B66 pointer (platform),
 *   $9A6C hotspots + handler, $902A planet grid click, $71E0 game pass,
 *   $E42C + 1, $8338 redraw (one $E39C bit per pass), right button ->
 *   $3534, $60A6.
 *
 * Handlers that run frame loops of their own in the original are state
 * machines across frame() calls (each call = the code between two $2FFA
 * waits of the original):
 *   ST_HOLD  $31A6 (44 frames of $E42C + 1 and $4ED8; no game pass): after
 *            "ALL CASH SENT TO BASE!", "NOT YOURS!", RENAME's "ABORTED".
 *   ST_TYPE  $26C8 with $E43F set (RENAME's " ABORTED "): one character,
 *            $4ED8 and $71E0 per frame.
 *   ST_TEXT  the text entry $2272 / $2390 (RENAME here, the format prompt
 *            in scr_format.c). Keys come from the queue filled by
 *            ui_stats_key(). As in the WHDLoad version, time is frozen during
 *            name entry ($E44F = $FF, as in the WHDLoad version): each
 *            frame() only takes keys. With $E44F = 0 (an original disk
 *            copy's save) a frame() takes at most one key and runs the
 *            idle pass of $22B4 (viewer, $71E0, on the main screen the star
 *            map and $E42C + 1), as the original does.
 * When the inner loop ends, the handler's tail runs and the stats loop
 * goes on from the step after the handler, in the same frame() call.
 *
 * Outside the state block this screen keeps the words $865A / $865C /
 * $865E (the food trend: last food shown, food now, day of the last
 * trend); scr_stats_set_trend() sets them for the check.
 */
#include <string.h>

#include "ui.h"
#include "day.h"

#define STATS_PICTURE 0x1E924u
#define STATS_TABLE   0xF48Cu

/* ---- shared with scr_format.c / scr_starbase.c / scr_spy.c ---- */
void scr_select_e3d4(MainUI *m);
void scr_text_begin(MainUI *m, uint32_t buf, unsigned col, unsigned row, int preset);
int  scr_text_frame(MainUI *m, int *pass, uint32_t *status);
int  scr_text_aborted(void);
void scr_install_banner(MainUI *m);
UI_API int  ui_stats_key(int ascii);
UI_API int  ui_stats_text_active(void);
UI_API void scr_stats_set_trend(uint16_t last, uint16_t now, uint16_t day);
UI_API void scr_stats_get_trend(uint16_t out[3]);

static uint16_t food_last, food_now, trend_day;           /* $865A, $865C, $865E */

enum { ST_LOOP, ST_HOLD, ST_TYPE, ST_TEXT };
enum { A_CASH = 1, A_RENAME, A_RENAME_TYPED, A_RENAME_HELD, A_GRID };
enum { STEP_TOP, STEP_GRID, STEP_PASS };
static int st = ST_LOOP, after, hold_n;
static uint32_t type_at;
static unsigned type_col;

/* ---------------------------------------------------------------- drawing */

static unsigned cinc(unsigned col, unsigned n) { return (col & 0xFF00u) | ((col + n) & 0xFFu); }

static void wb(MainUI *m, uint32_t a, uint8_t v)
{
    if (a >= SCREEN_BASE && a < SCREEN_BASE + SCREEN_SIZE) m->s->bpl[a - SCREEN_BASE] = v;
}

/* $26C8, returning the column after the string (d0) */
static unsigned str_at(MainUI *m, unsigned col, unsigned row, const uint8_t *a)
{
    if (!a) return col;
    for (;;) {
        unsigned c = *a++;
        if (c == 0xFF) return col;
        if (c >= 0x20) { ui_draw_char(m->s, col, row, c); col = cinc(col, 1); continue; }
        switch (c) {
        case 1: row = (row & 0xFF00u) | ((row + *a++) & 0xFFu); break;
        case 2: col = cinc(col, *a++); break;
        case 3: case 4: {
            unsigned n = (unsigned)((*a++ - 1) & 0xFF) + 1, ch = *a++;
            for (unsigned k = 0; k < n; k++) {
                ui_draw_char(m->s, col, row, ch);
                if (c == 3) col = cinc(col, 1); else row = (row + 6) & 0xFFFFu;
            }
            break;
        }
        default: return col;
        }
    }
}
static unsigned str(MainUI *m, unsigned col, unsigned row, uint32_t a) { return str_at(m, col, row, ui_text(m, a)); }

static unsigned put(MainUI *m, unsigned col, unsigned row, unsigned ch)
{
    ui_draw_char(m->s, col, row, ch);
    return cinc(col, 1);
}

/* $31E0 (from = 0: 8 columns) / $3216 (from = 3: 5 columns): 32-bit,
 * right-aligned with spaces (signed compares, $32D4 digits) */
static unsigned num32(MainUI *m, unsigned col, unsigned row, uint32_t v, int k)
{
    static const uint32_t pw[8] = {10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    for (; k < 7; k++) {
        if ((int32_t)v >= (int32_t)pw[k]) break;
        col = put(m, col, row, 0x20);
    }
    for (; k < 8; k++) {
        unsigned ch = 0x2F;
        for (;;) {
            ch = (ch + 1) & 0xFFFFu;
            uint32_t b = v;
            v -= pw[k];
            if (b < pw[k]) break;
        }
        v += pw[k];
        col = put(m, col, row, ch);
    }
    return col;
}

/* $32F0 (k = 0: 5 columns) / $330C (k = 2: 3) / $331A (k = 3: 2): 16-bit */
static unsigned num16(MainUI *m, unsigned col, unsigned row, uint16_t v, int k)
{
    static const int32_t pw[5] = {10000, 1000, 100, 10, 1};
    for (; k < 4; k++) {
        if ((int16_t)v >= pw[k]) break;
        col = put(m, col, row, 0x20);
    }
    int32_t d = v;
    for (; k < 5; k++) {
        unsigned ch = 0x2F;
        do { ch++; d -= pw[k]; } while (d >= 0);
        d += pw[k];
        col = put(m, col, row, ch);
    }
    return col;
}

static uint32_t mk_base(unsigned col, unsigned row, int32_t off)
{
    uint16_t c = (uint16_t)((col & 0xFF00u) | ((col & 0xFFu) >> 1));    /* lsr.b, adda.w */
    return (uint32_t)((int32_t)(SCREEN_BASE + (row & 0xFFFFu) * SCREEN_LINE) + (int16_t)c + off);
}

static void marker_2a24(MainUI *m, unsigned col, unsigned row)     /* $2A24 (enemy etc.) */
{
    uint32_t a = mk_base(col, row, -0x24);
    for (int k = 0; k < 7; k++) { a += 200; wb(m, a - 120, 0); wb(m, a, 8); }
    a -= 4;
    for (int k = 0; k < 4; k++, a++) { wb(m, a - 120, 0); wb(m, a, 0xFF); }
    wb(m, a - 120, 0); wb(m, a, 0xF8);
}

static void marker_2a88(MainUI *m, unsigned col, unsigned row)     /* $2A88 (empty) */
{
    uint32_t a = mk_base(col, row, -0x24);
    for (int k = 0; k < 7; k++) { a += 200; wb(m, a, 8); wb(m, a - 120, 8); wb(m, a - 80, 0); }
    a -= 4;
    for (int k = 0; k < 4; k++, a++) { wb(m, a - 80, 0); wb(m, a - 120, 0xFF); wb(m, a, 0xFF); }
    wb(m, a - 80, 0); wb(m, a - 120, 0xF8); wb(m, a, 0xF8);
}

static void marker_2b10(MainUI *m, unsigned col, unsigned row)     /* $2B10 (the player's) */
{
    uint32_t a = mk_base(col, row, -0x74);
    for (int k = 0; k < 7; k++) { a += 200; wb(m, a + 80, 0); wb(m, a - 40, 0); wb(m, a, 8); }
    a -= 4;
    for (int k = 0; k < 4; k++, a++) { wb(m, a - 40, 0); wb(m, a + 80, 0); wb(m, a, 0xFF); }
    wb(m, a - 40, 0); wb(m, a + 80, 0); wb(m, a, 0xF8);
}

static void grid_blank(MainUI *m)                                  /* $8690 */
{
    unsigned row = 0x83;
    for (int r = 0; r < 8; r++) {
        unsigned col = 8;
        for (int c = 0; c < 4; c++) {
            marker_2a88(m, col, row);
            col = str(m, col, row, 0x0E12);
            col = cinc(col, 1);
        }
        row = (row & 0xFF00u) | ((row + 8) & 0xFFu);
    }
}

static void grid_names(MainUI *m)                                  /* $8F70 */
{
    Game *g = m->g;
    unsigned col = 8, row = 0x83;
    uint32_t rec = g32(g, G_PLANET_TABLE);
    for (unsigned k = 0; k <= g16(g, 0xE410); k++, rec += PLANET_SIZE) {
        uint16_t o = g16(g, rec + P_OWNER);
        if (o == 1 || o == 4) {
            marker_2a88(m, col, row);
        } else {
            if (o != 10) marker_2a24(m, col, row);
            else marker_2b10(m, col, row);
            col = str(m, col, row, rec + P_NAME);
            col = (col - 9) & 0xFFFFu;
        }
        row = (row & 0xFF00u) | ((row + 8) & 0xFFu);
        if ((row & 0xFF) == 0xC3) { row = (row & 0xFF00u) | 0x83; col = cinc(col, 10); }
    }
}

/* $9B2C: a 40x29 craft picture (5 planes) at the text position */
static void picture(MainUI *m, unsigned col, unsigned row, uint32_t src)
{
    uint8_t x = (uint8_t)(((col & 0xFFu) >> 1) - 4);
    uint32_t dst = SCREEN_BASE + ((row - 0x1D) & 0xFFFFu) * SCREEN_LINE
                 + (uint32_t)(int16_t)(uint16_t)((col & 0xFF00u) | x);
    for (int r = 0; r < 29; r++)
        for (int c = 0; c < 5; c++)
            for (int p = 0; p < 5; p++)
                wb(m, dst + (uint32_t)r * 200 + (uint32_t)p * 40 + (uint32_t)c,
                   ui_m8(m, src + (uint32_t)((r * 5 + c) * 5 + p)));
}

/* $35E2 (mode 0: on the surface), $35D4 (1: in orbit), $35C6 (2: in the
 * docking bays): the craft at $E3D4, up to six with pictures */
static void craft_list(MainUI *m, int mode)
{
    Game *g = m->g;
    static const uint32_t head[3] = {0x1A90, 0x1AA6, 0x1ABC};
    s8(g, 0xE423, (uint8_t)mode);
    uint32_t p = g32(g, 0xE3D4);
    if (p) {
        unsigned col = str(m, 0x30, 0x75, head[mode]);
        str(m, col, 0x75, p + P_NAME);
    }
    uint8_t idx = ui_m8(m, p + 0x28);
    unsigned col = 0x30, row = 0x9B, n = 0;
    for (int b = 0; b < NBAYS; b++) {
        uint32_t a = BAY_TABLE + (uint32_t)b * BAY_SIZE;
        if (!g8(g, a + 0x1E) || g8(g, a + 0x21) != idx) continue;
        uint8_t f = g8(g, a + 0x1F);
        if (mode == 2 ? (f & 0x14) != 0 : mode == 1 ? !(f & 4) : !(f & 0x10)) continue;
        col = str(m, col, row, a);
        picture(m, col, row, 0x59BEEu + (uint32_t)(uint8_t)(g8(g, a + 0x1E) - 1) * 0x2D5u);
        n++;
        col = cinc(col, 1);
        if ((col & 0xFF) == 0x4E) {
            col = (col & 0xFF00u) | 0x30;
            row = (row & 0xFF00u) | ((row + 0x23) & 0xFFu);
            if (n == 6) return;
        }
    }
    for (int k = 5 - (int)n; k >= 0; k--) {
        col = str(m, col, row, 0x0E12);
        picture(m, col, row, 0x5AFC1u);
        col = cinc(col, 1);
        if ((col & 0xFF) == 0x4E) {
            col = (col & 0xFF00u) | 0x30;
            row = (row & 0xFF00u) | ((row + 0x23) & 0xFFu);
        }
    }
}

static void list_redraw(MainUI *m)                                 /* $4626 */
{
    uint8_t mode = g8(m->g, 0xE423);
    if (mode <= 2) craft_list(m, mode);
}

static void clear_prompt(MainUI *m) { str(m, 0x0A, 0x61, 0x1AD2); }   /* $8660 */

static uint32_t starbase(Game *g)                                  /* $3506 */
{
    return g32(g, G_PLANET_TABLE) + (uint32_t)(uint16_t)(g16(g, 0xE410) * PLANET_SIZE);
}

/* $3534: make $E3D4 the selected planet */
void scr_select_e3d4(MainUI *m)
{
    Game *g = m->g;
    uint32_t p = g32(g, 0xE3D4);
    if (!p) return;
    uint16_t idx = (uint16_t)(g32(g, 0xE3BC) * 0x3Au);           /* $6E5C */
    s8(g, g32(g, G_PLANET_TABLE) + (uint32_t)(int16_t)idx + 5, 0);
    s32(g, 0xE3DC, p);
    uint32_t d = p - g32(g, G_PLANET_TABLE);
    uint32_t q = d / 0x3Au;
    if (q > 0xFFFFu) q = d;                                       /* divu overflow */
    s32(g, 0xE3BC, q & 0xFF);
    ui_mark_selected(m);
}

/* the "* INCOMING  MESSAGE! *" banner of $95CC / $9644 on screens other
 * than the main one (position table $59BCE by $E425) */
static void banner(Game *g, int k)
{
    MainUI *m = (MainUI *)g->user;
    if (!m) return;
    unsigned i = (unsigned)(uint8_t)(g8(g, 0xE425) << 2);
    unsigned col = ui_m16(m, 0x59BCEu + i), row = ui_m16(m, 0x59BCEu + i + 2);
    if (k >= 0) ui_draw_char(m->s, cinc(col, (unsigned)k), row, g->banner[k]);
    else if (!(g8(g, 0xE426) & 1)) str_at(m, col, row, g->banner);
    else str(m, col, row, 0x0E20);
}

void scr_install_banner(MainUI *m)
{
    if (!m->g->user) m->g->user = m;
    if (!m->g->banner_hook) m->g->banner_hook = banner;
}

/* ---------------------------------------------------------------- redraw */

static void clr(Game *g, int bit) { s32(g, G_REDRAW, g32(g, G_REDRAW) & ~(1u << bit)); }

static void tons(MainUI *m, unsigned row, uint16_t v)
{
    unsigned col = num16(m, 0x15, row, v, 0);
    str(m, col, row, 0x20DB);                                      /* " T." */
}

static void redraw(MainUI *m)                                      /* $8338 */
{
    Game *g = m->g;
    uint32_t d7 = g32(g, G_REDRAW);
    if (d7 & (1u << 3)) { clr(g, 3); grid_blank(m); grid_names(m); return; }
    uint32_t rec = g32(g, 0xE3D4);
    if (!rec) {
        str(m, 0x10, 0x61, (g8(g, 0xE42C) & 7) >= 5 ? 0x0E30 : 0x0F2F);   /* "SELECT A PLANET!" */
        return;
    }
    if (d7 & (1u << 9)) { clr(g, 9); str(m, 0x13, 0x0A, rec + P_NAME); return; }
    if (d7 & (1u << 1)) {                                          /* $4DDE */
        clr(g, 1);
        uint16_t o = g16(g, rec + P_OWNER);
        str(m, 0x13, 0x1A, o == 4 ? 0x16C4 : o == 1 ? 0x16CF : o == 7 ? 0x16B9 : 0x16AE);
        return;
    }
    if (d7 & (1u << 2)) { clr(g, 2); num32(m, 0x14, 0x22, g32(g, rec + P_CREDITS), 0); return; }
    if (d7 & (1u << 14)) {                                         /* food and its trend */
        clr(g, 14);
        uint16_t v = g16(g, rec + P_FOOD);
        food_now = v;
        unsigned col = num16(m, 0x15, 0x2E, v, 0);
        col = str(m, col, 0x2E, 0x20DB);
        if (trend_day == g16(g, G_DAY)) return;
        trend_day = g16(g, G_DAY);
        col = cinc(col, (unsigned)-10);
        uint32_t a = 0x0E1A;
        if (food_last != food_now) a = food_last < food_now ? 0x18DF : 0x18DD;
        food_last = food_now;
        str(m, col, 0x2E, a);
        return;
    }
    if (d7 & (1u << 15)) { clr(g, 15); tons(m, 0x36, g16(g, rec + P_MINERALS)); return; }
    if (d7 & (1u << 16)) { clr(g, 16); tons(m, 0x3E, g16(g, rec + P_FUELS)); return; }
    if (d7 & (1u << 17)) { clr(g, 17); tons(m, 0x46, g16(g, rec + P_ENERGY)); return; }
    if (d7 & (1u << 5)) { clr(g, 5); num16(m, 0x34, 0x0A, g16(g, rec + P_POP), 0); return; }
    if (d7 & (1u << 7)) {
        clr(g, 7);
        unsigned col = num16(m, 0x34, 0x1A, g8(g, rec + P_MORALE), 2);
        str(m, col, 0x1A, 0x1844);                                 /* " %" */
        return;
    }
    if (d7 & (1u << 6)) {                                          /* growth */
        clr(g, 6);
        uint16_t v = 0;
        uint32_t sign = 0x0E1A;
        if (g16(g, rec + P_POP) < 30000) {
            uint8_t gr = g8(g, rec + P_GROWTH), de = g8(g, rec + P_DECLINE);
            if (gr > de) { v = (uint8_t)(gr - de); sign = 0x18DF; }
            else if (gr < de) { v = (uint8_t)(de - gr); sign = 0x18DD; }
        }
        unsigned col = num16(m, 0x35, 0x12, v, 3);
        col = str(m, col, 0x12, 0x1844);
        str(m, cinc(col, (unsigned)-5), 0x12, sign);
        return;
    }
    if (d7 & (1u << 8)) {
        clr(g, 8);
        unsigned col = num16(m, 0x34, 0x28, g8(g, rec + P_TAX), 2);
        str(m, col, 0x28, 0x1844);
        return;
    }
    if (d7 & (1u << 12)) { clr(g, 12); clear_prompt(m); return; }
    if (d7 & (1u << 4)) { clr(g, 4); num32(m, 0x34, 0x3E, g16(g, rec + 0x22), 3); return; }
    if (d7 & (1u << 20)) { clr(g, 20); craft_list(m, 0); return; }
}

/* ---------------------------------------------------------------- text entry */

/* key codes of $2B8C: 0 ESC, 1 backspace, 2 RETURN, else the character */
static uint8_t keyq[32];
static int kq_head, kq_n;
static struct {
    uint32_t buf;
    unsigned col, row;
    unsigned count;                 /* $2269 */
    int active, aborted, idle;      /* $2270 = aborted */
} te;

UI_API int ui_stats_key(int ascii)
{
    static const char chars[] = "1234567890-=QWERTYUIOPASDFGHJKL;ZXCVBNM,./ ";   /* $2B8C */
    int c;
    if (!te.active) return 0;
    if (ascii == 27) c = 0;
    else if (ascii == 8 || ascii == 127) c = 1;
    else if (ascii == 13 || ascii == 10) c = 2;
    else {
        if (ascii >= 'a' && ascii <= 'z') ascii -= 32;
        if (ascii <= 0 || !strchr(chars, ascii)) return 0;
        c = ascii;
    }
    if (kq_n == (int)sizeof keyq) return 0;
    keyq[(kq_head + kq_n++) % sizeof keyq] = (uint8_t)c;
    return 1;
}

UI_API int ui_stats_text_active(void) { return te.active; }
int scr_text_aborted(void) { return te.aborted; }

/* preset 0: $2272 (the buffer becomes "UN-NAMED!", nothing typed yet);
 * preset 1: $2390 (the default name $E38E is drawn and copied, all 9
 * characters typed) */
void scr_text_begin(MainUI *m, uint32_t buf, unsigned col, unsigned row, int preset)
{
    Game *g = m->g;
    static const uint8_t unnamed[9] = {'U', 'N', '-', 'N', 'A', 'M', 'E', 'D', '!'};
    te.buf = buf;
    te.row = row;
    te.aborted = te.idle = 0;
    te.active = 1;
    kq_head = kq_n = 0;
    if (preset) {
        str(m, col, row, 0xE38E);
        for (int k = 0; k < 9; k++) s8(g, buf + k, g8(g, 0xE38E + k));
        te.count = 9;
        te.col = cinc(col, 9);
    } else {
        for (int k = 0; k < 9; k++) s8(g, buf + k, unnamed[k]);
        te.count = 0;
        te.col = col;
    }
}

static int te_key(MainUI *m, int c)                                 /* 1: the entry ends */
{
    Game *g = m->g;
    if (c == 0) { te.aborted = 1; return 1; }
    if (c == 2) {                                                  /* pad with spaces */
        if (te.count != 9 && te.count != 0)
            for (unsigned k = te.count; k < 9; k++) s8(g, te.buf + k, 0x20);
        return 1;
    }
    if (c == 1) {
        if (!te.count) return 0;
        te.count--;
        te.col = cinc(te.col, (unsigned)-1);
        s8(g, te.buf + te.count, 0x20);
        ui_draw_char(m->s, te.col, te.row, 0x20);
        return 0;
    }
    if (te.count == 9) return 0;
    s8(g, te.buf + te.count, (uint8_t)c);
    ui_draw_char(m->s, te.col, te.row, (unsigned)c);
    te.col = cinc(te.col, 1);
    te.count++;
    return 0;
}

/* one frame of the entry loop ($2296); 1 when it ended (scr_text_aborted).
 * *pass gets the result of the idle pass's $71E0 (UI_OK if none ran). */
int scr_text_frame(MainUI *m, int *pass, uint32_t *status)
{
    Game *g = m->g;
    *pass = UI_OK;
    if (!te.active) return 1;
    if (te.idle) {                                                 /* after $22C8's wait */
        te.idle = 0;
        int r = ui_game_pass(m, status);
        if (r != UI_OK) { *pass = r; return 0; }
        if (!g8(g, 0xE425)) {
            uint32_t arrow = 0;
            ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);
            s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        }
    }
    while (kq_n) {
        int c = keyq[kq_head];
        kq_head = (kq_head + 1) % (int)sizeof keyq;
        kq_n--;
        if (te_key(m, c)) { te.active = 0; return 1; }
        if (!g8(g, 0xE44F)) break;                                 /* the key is still down */
    }
    if (!g8(g, 0xE44F)) {                                          /* $22B4 */
        ui_viewer_tick(m);
        te.idle = 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- handlers */

static void hold(int then) { st = ST_HOLD; hold_n = 44; after = then; }   /* $31A6 */

static void tax(MainUI *m, int down)                               /* $5B28 / $5B20 */
{
    Game *g = m->g;
    s8(g, 0xE44D, down ? 0xFF : 0);
    if (!g32(g, 0xE3D4)) return;
    s8(g, 0xE422, 0);
    uint8_t d0 = g8(g, 0xE44A);
    s8(g, 0xE44A, g8(g, 0xE42C));
    if ((uint8_t)(d0 + 1) != g8(g, 0xE42C)) { s8(g, 0xE44B, 12); s8(g, 0xE44C, 0); }
    if (g8(g, 0xE44C)) { s8(g, 0xE44C, (uint8_t)(g8(g, 0xE44C) - 1)); return; }
    d0 = g8(g, 0xE44B);
    if (d0) d0--;
    s8(g, 0xE44B, d0);
    s8(g, 0xE44C, d0);
    uint32_t t = g32(g, 0xE3D4) + P_TAX;
    if (!g8(g, 0xE44D)) { if (g8(g, t) == 100) return; s8(g, t, (uint8_t)(g8(g, t) + 1)); }
    else { if (g8(g, t) == 0) return; s8(g, t, (uint8_t)(g8(g, t) - 1)); }
    or32(g, G_REDRAW, 0x100);
}

static void cash_to_base(MainUI *m)                                /* $4D5E */
{
    Game *g = m->g;
    uint32_t r = g32(g, G_PLANET_TABLE);
    uint8_t mine = 0;
    for (unsigned k = 0; k <= g16(g, 0xE410); k++, r += PLANET_SIZE)   /* $5A76 */
        if (g16(g, r + P_OWNER) == 10) mine++;
    if (mine < 2) {
        str(m, 0x0B, 0x61, 0x20E5);                                /* "YOU HAVE NO OTHER PLANETS!" */
    } else {
        str(m, 0x0D, 0x61, 0x1902);                                /* "ALL CASH SENT TO BASE!" */
        uint32_t sb = starbase(g);
        uint16_t n = g16(g, 0xE410);
        n = (uint16_t)((n & 0xFF00u) | ((n - 1) & 0xFFu));         /* subq.b */
        r = g32(g, G_PLANET_TABLE);
        for (uint32_t k = 0; k <= n; k++, r += PLANET_SIZE)
            if (g16(g, r + P_OWNER) == 10) {
                s32(g, sb + P_CREDITS, g32(g, sb + P_CREDITS) + g32(g, r + P_CREDITS));
                s32(g, r + P_CREDITS, 0);
            }
        or32(g, G_REDRAW, 4);
    }
    ui_sound(m, 10);
    hold(A_CASH);
}

static void rename_planet(MainUI *m)                               /* $27A2 */
{
    Game *g = m->g;
    uint32_t p = g32(g, 0xE3D4);
    if (!p) { ui_sound(m, 3); return; }                            /* jmp $59ACA */
    ui_sound(m, 13);
    s8(g, 0xE42D, 0xFF);                                           /* $3006 */
    str(m, 0x0A, 0x61, 0x1A2A);                                    /* "TYPE NEW NAME: >" */
    for (int k = 0; k < 9; k++) s8(g, 0xD5E6 + k, g8(g, p + P_NAME + k));
    scr_text_begin(m, p + P_NAME, 0x1A, 0x61, 0);
    st = ST_TEXT;
    after = A_RENAME;
}

/* $90F8: an owned planet picked in the grid (a0 = rec) */
static void grid_pick(MainUI *m, uint32_t rec)
{
    Game *g = m->g;
    str(m, 0x13, 0x2E, 0x0E1A);
    list_redraw(m);
    if (rec != g32(g, 0xE3D4)) ui_sound(m, 13);
    or32(g, G_REDRAW, 0x7D3F6);
    s32(g, 0xE3D4, rec);
}

static void grid_click(MainUI *m, const Mouse *ms)                 /* $902A, screen 3 */
{
    Game *g = m->g;
    if (!ms->button) return;
    uint16_t x = (uint16_t)ms->x, y = (uint16_t)ms->y;
    if ((uint8_t)y < 0x83 || x < 0x1C) return;
    uint16_t c = (uint16_t)((uint16_t)(x - 0x1C) / 0x28);
    if (c >= 4) return;
    uint16_t r = (uint16_t)((uint16_t)(y - 0x83) / 8);
    if (r >= 8) return;
    uint16_t i = (uint16_t)(c * 8 + r);
    uint16_t lim = !g8(g, 0xE43B) ? 8 : g8(g, 0xE43B) == 1 ? 16 : 32;
    if (i >= lim) return;
    uint32_t rec = g32(g, G_PLANET_TABLE) + (uint32_t)i * PLANET_SIZE;
    if (!g8(g, rec + P_TYPE)) return;
    if (g16(g, rec + P_OWNER) != 10) {
        clear_prompt(m);
        str(m, 0x13, 0x61, 0x1878);                                /* "NOT YOURS!" */
        hold(A_GRID);
        return;
    }
    grid_pick(m, rec);
}

/* ---------------------------------------------------------------- the loop */

static int text_step(MainUI *m, const Mouse *ms, uint32_t *next);

static int body(MainUI *m, const Mouse *ms, uint32_t *next, int from)
{
    Game *g = m->g;
    if (from == STEP_TOP) {
        ui_viewer_tick(m);
        uint32_t h = ui_hotspots(m, ms);
        switch (h) {
        case 0: break;
        case 0x5B28: tax(m, 0); break;
        case 0x5B20: tax(m, 1); break;
        case 0x4500: case 0x4620: case 0x8EF8:                     /* left column: other screens */
            *next = h;
            return UI_GOTO;
        case 0x4D5E: cash_to_base(m); return UI_OK;
        case 0x27A2:
            rename_planet(m);
            if (st == ST_TEXT) return text_step(m, ms, next);    /* the first poll */
            break;
        case 0x276C: ui_sound(m, 13); craft_list(m, 0); break;
        case 0x277E: ui_sound(m, 13); craft_list(m, 1); break;
        case 0x2790: ui_sound(m, 13); craft_list(m, 2); break;
        default: *next = h; return UI_HANDLER;
        }
        from = STEP_GRID;
    }
    if (from == STEP_GRID) {
        grid_click(m, ms);
        if (st != ST_LOOP) return UI_OK;
    }
    int r = ui_game_pass(m, next);
    if (r != UI_OK) return r;
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    redraw(m);
    if (ms->rbutton) {
        scr_select_e3d4(m);
        return UI_EXIT_TO_MAIN;
    }
    return UI_OK;
}

/* the tail of a handler after its inner loop, then the rest of the loop */
static int resume(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    st = ST_LOOP;
    switch (after) {
    case A_CASH:
        clear_prompt(m);
        return body(m, ms, next, STEP_GRID);
    case A_RENAME:
        if (scr_text_aborted()) {
            uint32_t p = g32(g, 0xE3D4);
            for (int k = 0; k < 9; k++) s8(g, p + P_NAME + k, g8(g, 0xD5E6 + k));
            s8(g, 0xE43F, 0xFF);
            st = ST_TYPE;
            type_at = 0x1086;                                       /* " ABORTED " at $1A,$61 */
            type_col = 0x1A;
            after = A_RENAME_TYPED;
            return UI_OK;                                           /* the caller types the first */
        }
        or32(g, G_REDRAW, 0x200);
        grid_names(m);
        clear_prompt(m);
        s8(g, 0xE42D, 0);                                          /* $301C */
        return body(m, ms, next, STEP_GRID);
    case A_RENAME_TYPED:
        s8(g, 0xE43F, 0);
        hold(A_RENAME_HELD);
        return UI_OK;
    case A_RENAME_HELD:
        clear_prompt(m);
        s8(g, 0xE42D, 0);
        return body(m, ms, next, STEP_GRID);
    case A_GRID:
        clear_prompt(m);
        if (g8(g, 0xE43A)) grid_pick(m, 0x1AD5);                   /* a0 is left after $8660's text */
        return body(m, ms, next, STEP_PASS);
    }
    return body(m, ms, next, STEP_TOP);
}

/* one character of $26C8 with $E43F set: the character, $4ED8, $71E0 */
static int type_step(MainUI *m, const Mouse *ms, uint32_t *next)
{
    const uint8_t *t = ui_text(m, type_at);
    if (!t || *t == 0xFF) return resume(m, ms, next);
    if (*t >= 0x20) ui_draw_char(m->s, type_col, 0x61, *t);
    type_col = cinc(type_col, 1);
    type_at++;
    ui_viewer_tick(m);
    return ui_game_pass(m, next);
}

/* one frame of the text entry; when it ends, the handler's tail */
static int text_step(MainUI *m, const Mouse *ms, uint32_t *next)
{
    int pass;
    if (!scr_text_frame(m, &pass, next)) return pass;
    int r = resume(m, ms, next);
    if (st == ST_TYPE) return type_step(m, ms, next);
    return r;
}

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    Game *g = m->g;
    (void)next;
    if (handler == 0x825A) ui_panel_save(m);    /* $47DC: only the main-screen icon; $4614 and $825E skip it */
    ui_sound(m, 13);
    s32(g, 0xE3F0, 0);
    s32(g, G_REDRAW, 0);
    uint32_t dc = g32(g, 0xE3DC);
    s32(g, 0xE3D4, g16(g, dc + P_OWNER) == 10 ? dc : starbase(g));
    or32(g, G_REDRAW, 0x7C3F6);
    s8(g, 0xE425, 3);
    s32(g, 0xE3EC, STATS_TABLE);
    s16(g, 0xE400, 10);
    ui_show_picture(m, STATS_PICTURE);
    grid_blank(m);
    grid_names(m);
    ui_draw_date(m);
    craft_list(m, 0);
    s16(g, 0xE3F8, 0);
    s8(g, 0xE42E, 0);
    s32(g, 0xE3F0, 0xFC72);
    s8(g, 0xE44E, 6);
    st = ST_LOOP;
    te.active = 0;
    scr_install_banner(m);
    return UI_OK;
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    switch (st) {
    case ST_HOLD:
        s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        ui_viewer_tick(m);
        if (--hold_n > 0) return UI_OK;
        return resume(m, ms, next);
    case ST_TYPE:
        return type_step(m, ms, next);
    case ST_TEXT:
        return text_step(m, ms, next);
    }
    return body(m, ms, next, STEP_TOP);
}

UI_API void scr_stats_set_trend(uint16_t last, uint16_t now, uint16_t day)
{
    food_last = last; food_now = now; trend_day = day;
}
UI_API void scr_stats_get_trend(uint16_t out[3]) { out[0] = food_last; out[1] = food_now; out[2] = trend_day; }

const ScreenDef scr_stats = {SCR_STATS, 0x825A, enter, frame};

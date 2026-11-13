/*
 * Screen: shipyard ($6944, table $F45C): the craft types, BUY with the
 * name prompt, the starfield ($561C). Addresses are the original's.
 * Checked frame by frame against the original game running in an emulator.
 *
 * This file also holds the pieces the fleet screen (scr_fleet.c) shares:
 * the "fy_" functions below (declared again at the top of scr_fleet.c):
 * the blocking routines of the original as resumable steps, the number and
 * string printers that carry the column on, the key queue.
 *
 * Frame model. One frame() = the original's code between two returns of
 * the frame wait $2FFA. Where a handler runs a loop of its own the screen's
 * iteration is a small state machine (protothread style: a switch on a
 * saved line, PT_* below) that is suspended at each frame wait and resumed
 * by the next frame():
 *  - message hold $31A6: 44 frames of $561C, $E42C + 1, $4ED8 (no game pass);
 *  - text typed with $E43F set ($26C8 -> $26E0): per character the draw,
 *    $4ED8, $71E0, the frame wait, $561C;
 *  - name entry $228A: time is frozen ($E44F = $FF, as in the WHDLoad version):
 *    the original polls the keyboard without a frame wait, so the step only
 *    takes keys from the queue (ui_shipyard_key / ui_fleet_key) and waits
 *    (returns) while it is empty;
 *  - waits for the button to be released (fly to) poll the mouse the same
 *    way;
 *  - showing a picture ($2C62) waits for one frame inside: the shipyard's
 *    type page is suspended there (the fleet screen's entry is not: enter()
 *    cannot yield, and nothing in it reads the mouse).
 * The last two are "busy" waits (fleetyard_busy() = 1): the original runs no
 * frame there, so after the key / the release the code runs on to the next
 * frame wait inside the same frame() call.
 */
#include <string.h>

#include "ui.h"
#include "craft.h"

/* ---- protothreads: l = the saved line of the routine ---- */
#if defined(__GNUC__) && __GNUC__ >= 7
#define FALL __attribute__((fallthrough))
#else
#define FALL
#endif
#define PT_BEGIN(l)    switch (l) { case 0:
#define PT_YIELD(l)    do { (l) = __LINE__; return 1; case __LINE__:; } while (0)
#define PT_AWAIT(l, c) do { (l) = __LINE__; FALL; case __LINE__: if (c) return 1; } while (0)
#define PT_EXIT(l)     do { (l) = 0; return 0; } while (0)
#define PT_END(l)      FALL; default: break; } (l) = 0; return 0

/* ---- shared with scr_fleet.c ---- */
void     fy_reset(void);
void     fy_install_banner(MainUI *m);
int      fy_res(uint32_t *next);
int      fy_peek(void);
void     fy_set_res(int r, uint32_t next);
void     fy_pass(MainUI *m);
void     fy_num(MainUI *m, unsigned *col, unsigned row, uint32_t v, uint32_t entry);
void     fy_str(MainUI *m, unsigned *col, unsigned *row, uint32_t a);
void     fy_str_at(MainUI *m, unsigned col, unsigned row, uint32_t a);
void     fy_starfield(MainUI *m);
void     fy_lock(MainUI *m);
void     fy_unlock(MainUI *m);
uint32_t fy_starbase(MainUI *m);
void     fy_select_shown(MainUI *m);
void     fy_hold_start(void);
int      fy_hold_step(MainUI *m);
void     fy_type_start(MainUI *m, unsigned col, unsigned row, uint32_t a);
int      fy_type_step(MainUI *m);
void     fy_name_start(uint32_t a0, unsigned col, unsigned row, uint16_t d5, uint8_t count);
int      fy_name_step(MainUI *m);
int      fy_name_escaped(void);
void     fy_key_push(int ascii);
int      fy_busy_wait(int waiting);
const Mouse *fy_mouse(void);
void     fy_set_mouse(const Mouse *ms);

/* ------------------------------------------------------------------ */
/* shared state                                                        */
/* ------------------------------------------------------------------ */

static int      res;               /* the frame's result (UI_*) */
static uint32_t res_next;          /* its handler / status */
static int      busy;              /* suspended at a busy wait */
static const Mouse *cur_ms;        /* the mouse of the current frame() */
static const Mouse no_mouse;

const Mouse *fy_mouse(void) { return cur_ms ? cur_ms : &no_mouse; }
void fy_set_mouse(const Mouse *ms) { cur_ms = ms; }

void fy_set_res(int r, uint32_t next) { res = r; res_next = next; }
int fy_peek(void) { return res; }
int fy_res(uint32_t *next)
{
    int r = res;
    if (next && (r == UI_GOTO || r == UI_HANDLER || r == UI_EVENT_WAIT)) *next = res_next;
    res = UI_OK;
    return r;
}

/* One game pass ($71E0) inside a screen's own loop: a game over or an event
 * wait is passed on as the frame's result; the loop carries on. */
void fy_pass(MainUI *m)
{
    uint32_t st = 0;
    int r = ui_game_pass(m, &st);
    if (r != UI_OK && res == UI_OK) fy_set_res(r, st);
}

/* A busy wait: `waiting` = the condition still holds. Returns 1 to suspend. */
int fy_busy_wait(int waiting)
{
    busy = waiting ? 1 : 0;
    return busy;
}

UI_API int fleetyard_busy(void) { return busy; }

/* ------------------------------------------------------------------ */
/* numbers ($31E0, $3284, $32F0, $32FE, $330C, $331A) and strings       */
/* ------------------------------------------------------------------ */

static unsigned col_inc(unsigned col, unsigned n) { return (col & 0xFF00u) | ((col + n) & 0xFFu); }

static void put(MainUI *m, unsigned *col, unsigned row, unsigned ch)   /* $335E / $32E0 */
{
    ui_draw_char(m->s, *col, row, ch & 0xFFFFu);
    *col = col_inc(*col, 1);
}

/* $32D4: one digit of a long (sub.l / bcc) */
static void digit_l(MainUI *m, unsigned *col, unsigned row, uint32_t *v, uint32_t p)
{
    unsigned ch = 0x2F;
    uint32_t d = *v;
    for (;;) {
        ch = (ch + 1) & 0xFFFFu;
        uint32_t before = d;
        d -= p;
        if (before < p) break;
    }
    *v = d + p;
    put(m, col, row, ch);
}

/* $3346: one digit of a word (andi.l #$FFFF, sub.l / bpl) */
static void digit_w(MainUI *m, unsigned *col, unsigned row, uint32_t *v, uint32_t p)
{
    uint32_t d = *v & 0xFFFFu;
    p &= 0xFFFFu;
    unsigned ch = 0x2F;
    do {
        ch = (ch + 1) & 0xFFFFu;
        d -= p;
    } while ((int32_t)d >= 0);
    *v = d + p;
    put(m, col, row, ch);
}

/* entry: the original's routine. $31E0: long, 8 wide, spaces before;
 * $3284: long, no padding, from 100; $32F0/$32FE/$330C/$331A: word (signed
 * compare), 5/4/3/2 wide with spaces before. */
void fy_num(MainUI *m, unsigned *col, unsigned row, uint32_t v, uint32_t entry)
{
    static const uint32_t L[8] = {10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    static const uint32_t W[5] = {10000, 1000, 100, 10, 1};
    int k;
    switch (entry) {
    case 0x31E0:
        for (k = 0; k < 7; k++) {
            if ((int32_t)v >= (int32_t)L[k]) break;
            put(m, col, row, 0x20);
        }
        for (; k < 8; k++) digit_l(m, col, row, &v, L[k]);
        return;
    case 0x3284:
        k = (int32_t)v >= 100 ? 5 : (int32_t)v >= 10 ? 6 : 7;
        for (; k < 8; k++) digit_l(m, col, row, &v, L[k]);
        return;
    default:
        k = entry == 0x32F0 ? 0 : entry == 0x32FE ? 1 : entry == 0x330C ? 2 : 3;
        for (; k < 4; k++) {
            if ((int16_t)v >= (int16_t)W[k]) break;
            put(m, col, row, 0x20);
        }
        for (; k < 5; k++) digit_w(m, col, row, &v, W[k]);
        return;
    }
}

/* $26C8 without the typewriter: the string at `a`; col / row carry on as
 * d0 / d1 do in the original. */
void fy_str(MainUI *m, unsigned *col, unsigned *row, uint32_t a)
{
    const uint8_t *p = ui_text(m, a);
    if (!p) return;
    for (;;) {
        unsigned c = *p++;
        if (c == 0xFF) return;
        if (c >= 0x20) { put(m, col, *row, c); continue; }
        switch (c) {
        case 1: *row = (*row & 0xFF00u) | ((*row + *p++) & 0xFFu); break;
        case 2: *col = col_inc(*col, *p++); break;
        case 3: {
            unsigned n = (unsigned)((*p++ - 1) & 0xFF) + 1, ch = *p++;
            for (unsigned k = 0; k < n; k++) put(m, col, *row, ch);
            break;
        }
        case 4: {
            unsigned n = (unsigned)((*p++ - 1) & 0xFF) + 1, ch = *p++;
            for (unsigned k = 0; k < n; k++) {
                ui_draw_char(m->s, *col, *row, ch);
                *row = (*row + 6) & 0xFFFFu;
            }
            break;
        }
        default:
            return;
        }
    }
}

void fy_str_at(MainUI *m, unsigned col, unsigned row, uint32_t a) { fy_str(m, &col, &row, a); }

/* The "INCOMING MESSAGE" banner the day's passes draw on the screens other
 * than the main one ($95CC: one character, $9644: the whole banner or its
 * blank $0E20 on odd passes), at the position $59BCE + $E425 x 4. The text
 * is the core's copy of $1A79. */
static void banner(Game *g, int k)
{
    MainUI *m = (MainUI *)g->user;
    if (!m) return;
    uint32_t i = (uint32_t)(uint8_t)(g8(g, 0xE425) << 2);
    unsigned col = ui_m16(m, 0x59BCEu + i), row = ui_m16(m, 0x59BCEu + i + 2);
    if (k >= 0) {
        ui_draw_char(m->s, col_inc(col, (unsigned)k), row, g->banner[k]);
    } else if (!(g8(g, 0xE426) & 1)) {
        for (unsigned j = 0; j < sizeof g->banner && g->banner[j] != 0xFF; j++)
            put(m, &col, row, g->banner[j]);
    } else {
        fy_str_at(m, col, row, 0x0E20);
    }
}

void fy_install_banner(MainUI *m)
{
    m->g->user = m;
    m->g->banner_hook = banner;
}

/* ------------------------------------------------------------------ */
/* small routines                                                      */
/* ------------------------------------------------------------------ */

void fy_lock(MainUI *m) { s8(m->g, 0xE42D, 0xFF); }      /* $3006 (+ the pointer, display) */
void fy_unlock(MainUI *m) { s8(m->g, 0xE42D, 0); }       /* $301C */

uint32_t fy_starbase(MainUI *m)                          /* $3506 */
{
    return g32(m->g, 0xE3B0) + (uint32_t)(uint16_t)(g16(m->g, 0xE410) * 0x3Au);
}

void fy_select_shown(MainUI *m)                          /* $3534 */
{
    Game *g = m->g;
    uint32_t p = g32(g, 0xE3D4);
    if (!p) return;
    uint16_t idx = (uint16_t)(g32(g, 0xE3BC) * 0x3Au);   /* $6E5C */
    s8(g, g32(g, 0xE3B0) + (uint32_t)(int16_t)idx + 5, 0);
    s32(g, 0xE3DC, p);
    s32(g, 0xE3BC, ((p - g32(g, 0xE3B0)) / 0x3Au) & 0xFFu);
    ui_mark_selected(m);                                 /* $4B4C */
}

/* ---- the starfield ($5600 / $561C): 72 stars of 12 bytes at $59318 ----
 * +0 screen address, +4/+5 the saved bytes of planes 0/1, +6 x, +8 speed,
 * +$A/+$B draw in plane 0/1. Outside the state block: kept here. */
#define STARS     0x59318u
#define NSTARS    72
static uint8_t stars[NSTARS * 12];
static int     stars_loaded;

static void load_stars(MainUI *m)
{
    if (stars_loaded) return;
    for (unsigned k = 0; k < sizeof stars; k++) stars[k] = ui_m8(m, STARS + k);
    stars_loaded = 1;
}

UI_API uint8_t *shipyard_stars(MainUI *m) { load_stars(m); return stars; }

static uint8_t *scr(MainUI *m, uint32_t a)
{
    static uint8_t sink;
    a &= 0xFFFFFFu;
    return (a >= SCREEN_BASE && a < SCREEN_BASE + SCREEN_SIZE) ? &m->s->bpl[a - SCREEN_BASE] : &sink;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void stars_reset(MainUI *m)                        /* $5600 */
{
    load_stars(m);
    for (int k = 0; k < NSTARS; k++) wr32(stars + k * 12, SCREEN_BASE);
}

void fy_starfield(MainUI *m)                              /* $561C */
{
    Game *g = m->g;
    if (!g8(g, 0xE432) || g8(g, 0xE425) != 5) return;
    load_stars(m);
    for (int k = 0; k < NSTARS; k++) {
        uint8_t *st = stars + k * 12;
        uint32_t a2 = rd32(st);
        *scr(m, a2) = st[4];
        *scr(m, a2 + 0x28) = st[5];
        uint16_t d3 = (uint16_t)((st[6] << 8 | st[7]) + (st[8] << 8 | st[9]));
        if (d3 >= 0x140) d3 = (uint16_t)(d3 + 0x140);
        st[6] = (uint8_t)(d3 >> 8);
        st[7] = (uint8_t)d3;
        a2 = ui_m32(m, 0x58F58u + (uint32_t)k * 8u) + (uint32_t)(d3 >> 3);
        wr32(st, a2);
        unsigned bit = (unsigned)((~d3) & 7u);
        st[4] = *scr(m, a2);
        st[5] = *scr(m, a2 + 0x28);
        uint8_t d5 = (uint8_t)(*scr(m, a2) | *scr(m, a2 + 0x28) | *scr(m, a2 + 0x50) |
                               *scr(m, a2 + 0x78) | *scr(m, a2 + 0xA0));
        if (!d5) {
            if (st[10]) *scr(m, a2) |= (uint8_t)(1u << bit);
            if (st[11]) *scr(m, a2 + 0x28) |= (uint8_t)(1u << bit);
        }
    }
}

/* ---- the message hold $31A6 / $31B8: 44 frames ---- */
static int hold_line, hold_k;

void fy_hold_start(void) { hold_line = 0; }

int fy_hold_step(MainUI *m)
{
    Game *g = m->g;
    PT_BEGIN(hold_line);
    for (hold_k = 0; hold_k < 0x2C; hold_k++) {
        PT_YIELD(hold_line);                              /* $2FFA */
        fy_starfield(m);                                  /* $561C */
        /* $E425 = 6 would run $FFC6 here (the surface screen) */
        s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        ui_viewer_tick(m);                                /* $4ED8 */
    }
    PT_END(hold_line);
}

/* ---- the typewriter: $26C8 with $E43F set ---- */
static int tw_line;
static const uint8_t *tw_p;
static unsigned tw_col, tw_row;

void fy_type_start(MainUI *m, unsigned col, unsigned row, uint32_t a)
{
    tw_line = 0;
    tw_p = ui_text(m, a);
    tw_col = col;
    tw_row = row;
}

int fy_type_step(MainUI *m)
{
    static unsigned c;
    PT_BEGIN(tw_line);
    if (!tw_p) PT_EXIT(tw_line);
    for (;;) {
        c = *tw_p++;
        if (c == 0xFF) break;
        if (c >= 0x20) {                                  /* $26E0 */
            ui_draw_char(m->s, tw_col, tw_row, c);
            ui_viewer_tick(m);                            /* $4ED8 */
            fy_pass(m);                                   /* $71E0 */
            PT_YIELD(tw_line);                            /* $2FFA */
            fy_starfield(m);                              /* $561C */
            tw_col = col_inc(tw_col, 1);
            continue;
        }
        if (c == 1) tw_row = (tw_row & 0xFF00u) | ((tw_row + *tw_p++) & 0xFFu);
        else if (c == 2) tw_col = col_inc(tw_col, *tw_p++);
        else if (c == 3) {                                /* $2720: no passes */
            unsigned n = (unsigned)((*tw_p++ - 1) & 0xFF) + 1, ch = *tw_p++;
            for (unsigned k = 0; k < n; k++) put(m, &tw_col, tw_row, ch);
        } else if (c == 4) {                              /* $2744 */
            unsigned n = (unsigned)((*tw_p++ - 1) & 0xFF) + 1, ch = *tw_p++;
            for (unsigned k = 0; k < n; k++) {
                ui_draw_char(m->s, tw_col, tw_row, ch);
                tw_row = (tw_row + 6) & 0xFFFFu;
            }
        } else break;
    }
    PT_END(tw_line);
}

/* ---- keys and the name entry $228A ($E44F = $FF: no frames) ----
 * The queue holds the codes of the original's key table $2B8C: 0 ESC,
 * 1 DEL, 2 RETURN, else the character. */
static uint8_t keyq[64];
static unsigned kq_head, kq_tail;

void fy_key_push(int ascii)
{
    int c;
    if (ascii == 27) c = 0;
    else if (ascii == 8 || ascii == 127) c = 1;
    else if (ascii == 13 || ascii == 10) c = 2;
    else {
        if (ascii >= 'a' && ascii <= 'z') ascii -= 32;
        if (!((ascii >= '0' && ascii <= '9') || (ascii >= 'A' && ascii <= 'Z') ||
              ascii == '-' || ascii == '=' || ascii == ';' || ascii == ',' ||
              ascii == '.' || ascii == '/' || ascii == ' '))
            return;                                       /* not in $2B8C */
        c = ascii;
    }
    if (((kq_tail + 1) & 63u) == kq_head) return;
    keyq[kq_tail] = (uint8_t)c;
    kq_tail = (kq_tail + 1) & 63u;
}

/* Platform: call with each key press while a shipyard / fleet name prompt
 * may be open (ASCII; 27 = ESC, 8 = DEL / backspace, 13 = RETURN). */
UI_API void ui_shipyard_key(int ascii) { fy_key_push(ascii); }

static struct {
    uint32_t a0;                   /* where the next character goes */
    unsigned d0, d1;               /* column, row */
    uint16_t d5;                   /* places left (byte arithmetic) */
    uint8_t  count;                /* $2269 */
    uint8_t  esc;                  /* $2270 */
} ne;

void fy_name_start(uint32_t a0, unsigned col, unsigned row, uint16_t d5, uint8_t count)
{
    ne.a0 = a0;
    ne.d0 = col;
    ne.d1 = row;
    ne.d5 = d5;
    ne.count = count;
    ne.esc = 0;
    kq_head = kq_tail;             /* keys typed before the prompt are not read */
}

int fy_name_escaped(void) { return ne.esc; }

int fy_name_step(MainUI *m)
{
    Game *g = m->g;
    while (kq_head != kq_tail) {
        uint8_t c = keyq[kq_head];
        kq_head = (kq_head + 1) & 63u;
        if (c == 0) {                                     /* ESC */
            ne.esc = 0xFF;
            return fy_busy_wait(0);
        }
        if (c == 2) {                                     /* RETURN: pad with spaces */
            for (;;) {
                if (ne.count == 9 || ne.count == 0) break;
                s8(g, ne.a0++, 0x20);
                ne.d5 = (uint16_t)(ne.d5 - 1);
                if (ne.d5 == 0xFFFF) break;
            }
            return fy_busy_wait(0);
        }
        if (c == 1) {                                     /* DEL */
            if (ne.count == 0) continue;
            ne.a0--;
            ne.d0 = col_inc(ne.d0, 0xFF);
            s8(g, ne.a0, 0x20);
            ui_draw_char(m->s, ne.d0, ne.d1, 0x20);
            ne.count--;
            ne.d5 = (uint16_t)((ne.d5 & 0xFF00u) | ((ne.d5 + 1) & 0xFFu));
            continue;
        }
        if (ne.count == 9) continue;
        s8(g, ne.a0++, c);
        ui_draw_char(m->s, ne.d0, ne.d1, c);
        ne.d0 = col_inc(ne.d0, 1);
        ne.count++;
        ne.d5 = (uint16_t)((ne.d5 & 0xFF00u) | ((ne.d5 - 1) & 0xFFu));
    }
    return fy_busy_wait(1);
}

void fy_reset(void)
{
    hold_line = 0;
    tw_line = 0;
    busy = 0;
    res = UI_OK;
}

/* ------------------------------------------------------------------ */
/* the shipyard                                                        */
/* ------------------------------------------------------------------ */

static int it_line, buy_line;
static uint32_t it_h;

static unsigned lvl_row(MainUI *m, unsigned r0, unsigned r1, unsigned r2)
{
    uint8_t l = g8(m->g, 0xE43B);
    return l == 0 ? r0 : l == 1 ? r1 : r2;
}

static void credits(MainUI *m)                            /* $68BE */
{
    unsigned col = 5;
    fy_num(m, &col, lvl_row(m, 0xB7, 0xB3, 0xB0), g32(m->g, fy_starbase(m) + 0x36), 0x31E0);
}

static void minerals(MainUI *m)                           /* $68F2 */
{
    if (g8(m->g, 0xE43B) != 2) return;
    unsigned col = 5;
    fy_num(m, &col, 0xB7, g16(m->g, fy_starbase(m) + 0x30), 0x32F0);
}

static void energy(MainUI *m)                             /* $6914 */
{
    if (!g8(m->g, 0xE43B)) return;
    unsigned col = 5;
    fy_num(m, &col, g8(m->g, 0xE43B) == 2 ? 0xBE : 0xBC, g16(m->g, fy_starbase(m) + 0x34), 0x32F0);
}

static void info(MainUI *m)                               /* $6676 */
{
    Game *g = m->g;
    uint8_t lv = g8(g, 0xE43B);
    uint32_t t = g32(g, 0xE3AC);
    unsigned col;
    s8(g, 0xE444, 0);
    fy_str_at(m, 0x19, 0x98, ui_m32(m, t + 0x14));
    fy_str_at(m, 0x12, 0x92, ui_m32(m, t + 0x2C));
    credits(m);
    minerals(m);
    energy(m);
    col = 0x18;
    fy_num(m, &col, lvl_row(m, 0xB7, 0xB3, 0xB0), ui_m16(m, t + 6), 0x32F0);           /* price */
    if (lv == 2) { col = 0x18; fy_num(m, &col, 0xB7, ui_m16(m, t + 0x10), 0x330C); }
    if (lv) { col = 0x18; fy_num(m, &col, lv == 2 ? 0xBE : 0xBC, ui_m16(m, t + 0x0E), 0x330C); }
    if (lv == 2) {                                                                      /* crew */
        if (ui_m16(m, t + 0x0A) == 0xFFFE) fy_str_at(m, 0x2A, 0xB0, 0x183C);
        else { col = 0x2C; fy_num(m, &col, 0xB0, ui_m16(m, t + 0x0A), 0x32FE); }
    }
    col = 0x29;
    fy_num(m, &col, lvl_row(m, 0xB7, 0xB3, 0xB7), ui_m16(m, t + 4), 0x32F0);           /* payload */
    if (lv) {                                                                           /* fuel */
        unsigned row = lv == 1 ? 0xBC : 0xBE;
        if (ui_m16(m, t + 0x0C) == 0xFFFF) fy_str_at(m, 0x29, row, 0x1833);
        else { col = 0x29; fy_num(m, &col, row, ui_m16(m, t + 0x0C), 0x32F0); }
    }
    {                                                                                   /* $655E */
        uint8_t type = ui_m8(m, t + 0x12);
        uint32_t n = 0;
        for (int i = 0; i < BAY_COUNT; i++)
            if (g8(g, BAY_TABLE + (uint32_t)i * BAY_SIZE + B_TYPE) == type) n++;
        col = 0x3D;
        fy_num(m, &col, lvl_row(m, 0xB7, 0xB3, 0xB0), n, 0x331A);
    }
    if (lv == 2) {                                                                      /* range */
        if (ui_m16(m, t + 0x0C) == 0xFFFF) fy_str_at(m, 0x3A, 0xB7, 0x1A20);
        else {
            col = 0x3A;
            fy_num(m, &col, 0xB7, ((uint32_t)ui_m8(m, t + 0x13) * ui_m16(m, t + 0x0C)) & 0xFFFFu, 0x32F0);
        }
    }
    if (lv) { col = 0x3B; fy_num(m, &col, lv == 1 ? 0xBC : 0xBE, ui_m16(m, t + 0x1E), 0x32FE); }
}

/* $6580, up to the picture: $2C62 waits for a frame inside ($2EE8), so the
 * iteration is suspended there and type_page_rest runs in the next frame. */
static void type_page(MainUI *m)                          /* $6580 */
{
    Game *g = m->g;
    s16(g, 0xE3F8, 0);
    s8(g, 0xE42E, 0);
    s8(g, 0xE443, 0);
    uint32_t t = 0xECD4u + (uint32_t)(uint16_t)(g8(g, 0xE442) * 0x30u);
    s32(g, 0xE3AC, t);
    s16(g, 0xE416, ui_m16(m, t + 6));
    s32(g, 0xE3F0, ui_m32(m, t + 0x18));
    s8(g, 0xE44E, ui_m8(m, t + 0x1C));
    ui_show_picture(m, ui_m32(m, t));
}

static void type_page_rest(MainUI *m)
{
    Game *g = m->g;
    uint32_t t = g32(g, 0xE3AC);
    /* $65E2: colours 1-8 of the picture's palette (its copy at $59B4E, made
     * by $2EDC) go into the copper list's first moves, so the top part keeps
     * the picture's own colours: m->colour already holds them. */
    if (ui_m8(m, t + 0x1D)) {
        s8(g, 0xE432, 0xFF);
        m->copper = 0xF974;                               /* $65FA */
        stars_reset(m);                                   /* $5600 */
    } else {
        m->copper = 0xF85C;                               /* $6622 */
        s8(g, 0xE432, 0);
    }
    ui_xor_delta(m, 0x524A8);
    fy_str_at(m, 0x12, 0x98, 0x11A5);
    uint8_t lv = g8(g, 0xE43B);
    fy_str_at(m, 0, 0xB0, lv == 0 ? 0x1177 : lv == 1 ? 0x111E : 0x109A);
    info(m);
}

static void redraw(MainUI *m)                             /* $6876 */
{
    Game *g = m->g;
    uint32_t d7 = g32(g, 0xE39C);
    if (d7 & 0x4) { credits(m); s32(g, 0xE39C, g32(g, 0xE39C) & ~0x4u); return; }
    if (d7 & 0x8000) { minerals(m); s32(g, 0xE39C, g32(g, 0xE39C) & ~0x8000u); return; }
    if (d7 & 0x20000) { energy(m); s32(g, 0xE39C, g32(g, 0xE39C) & ~0x20000u); }
}

static void type_sound(MainUI *m)                         /* $69B8 */
{
    Game *g = m->g;
    uint8_t d0 = g8(g, 0xE42C);
    uint8_t type = ui_m8(m, g32(g, 0xE3AC) + 0x12);
    int n = -1;
    if (type == 7) { if (!(d0 & 0x1F)) n = 0x26; }
    else if (type == 3) { if (!(d0 & 0x3F)) n = 0x29; }
    else if (type == 4) { if (!(d0 & 0x3F)) n = 0x0C; }
    else if (type == 6) { if (d0 == 0) n = 0x28; else if (d0 == 0x80) n = 0x2A; }
    if (n >= 0) ui_sound(m, n);
}

/* BUY ($9B88) */
static int buy(MainUI *m)
{
    static uint32_t t, bay, text;
    static unsigned tcol;
    Game *g = m->g;
    PT_BEGIN(buy_line);
    ui_sound(m, 7);
    fy_lock(m);
    t = g32(g, 0xE3AC);
    text = craft_buy_check(g, t);                         /* $9B98-$9C88, $6530 */
    if (text) { tcol = 0x12; goto refuse; }
    bay = 0;                                              /* $6530: the lowest free bay */
    for (int i = 0; i < BAY_COUNT && !bay; i++)
        if (!g8(g, BAY_TABLE + (uint32_t)i * BAY_SIZE + B_TYPE)) bay = BAY_TABLE + (uint32_t)i * BAY_SIZE;
    s32(g, 0xE3A8, bay);
    fy_str_at(m, 0x12, 0x92, 0x12F0);                     /* "HIT RETURN IF NAME OK ..." */
    fy_str_at(m, 0x39, 0x92, t + T_NAME);                 /* $23D6: the default name */
    for (int i = 0; i < 9; i++) s8(g, bay + i, g8(g, t + T_NAME + i));
    fy_name_start(bay + 9, 0x39 + 9, 0x92, 0x00FF, 9);
    PT_AWAIT(buy_line, fy_name_step(m));                  /* $228A */
    if (fy_name_escaped()) {
        text = 0x1086;                                    /* " ABORTED " */
        tcol = 0x39;
        goto typed;
    }
    {
        uint8_t name[9];
        for (int i = 0; i < 9; i++) name[i] = g8(g, bay + i);
        craft_buy(g, t, name);                            /* $242E, $9CD8-$9D80 */
    }
    fy_str_at(m, 0x12, 0x92, 0x0E39);
    s8(g, 0xE444, 0xFF);
    s32(g, 0xE3D4, fy_starbase(m));
    fy_select_shown(m);                                   /* $3534 */
    text = 0x1326;                                        /* "THIS SHIP HAS NOW BEEN TRANSFERRED ..." */
    tcol = 0x12;
refuse:                                                   /* $9BEC */
    fy_str_at(m, 0x12, 0x92, 0x0E39);
typed:
    s8(g, 0xE43F, 0xFF);
    fy_type_start(m, tcol, 0x92, text);
    PT_AWAIT(buy_line, fy_type_step(m));
    s8(g, 0xE43F, 0);
    fy_unlock(m);
    fy_hold_start();
    PT_AWAIT(buy_line, fy_hold_step(m));                  /* $31A6 */
    info(m);                                              /* $6676 */
    PT_END(buy_line);
}

static int handler(MainUI *m)
{
    Game *g = m->g;
    switch (it_h) {
    case 0x649C:                                          /* previous type */
        ui_sound(m, 13);
        if (!g8(g, 0xE442)) s8(g, 0xE442, 6);
        else do s8(g, 0xE442, (uint8_t)(g8(g, 0xE442) - 1)); while (g8(g, 0xE442) == 1);
        s8(g, 0xE443, 0xFF);
        s8(g, 0xE444, 0xFF);
        return 0;
    case 0x64E6:                                          /* next type */
        ui_sound(m, 13);
        if (g8(g, 0xE442) == 6) s8(g, 0xE442, 0);
        else do s8(g, 0xE442, (uint8_t)(g8(g, 0xE442) + 1)); while (g8(g, 0xE442) == 1);
        s8(g, 0xE443, 0xFF);
        s8(g, 0xE444, 0xFF);
        return 0;
    case 0x9B88:
        return buy(m);
    }
    return 0;
}

/* one iteration of the loop $6998 */
static int iteration(MainUI *m)
{
    Game *g = m->g;
    PT_BEGIN(it_line);
    fy_starfield(m);                                      /* $561C */
    if (g8(g, 0xE443)) {                                  /* $6580 (runs on into $6676) */
        type_page(m);
        PT_YIELD(it_line);                                /* the frame wait in $2C62 */
        type_page_rest(m);
    }
    if (g8(g, 0xE444)) info(m);                           /* $6676 */
    type_sound(m);
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    {
        uint32_t st = 0;
        int r = ui_game_pass(m, &st);                     /* $71E0 */
        if (r != UI_OK) { fy_set_res(r, st); PT_EXIT(it_line); }
    }
    ui_viewer_tick(m);                                    /* $4ED8 */
    it_h = ui_hotspots(m, fy_mouse());                    /* $9A6C */
    if (it_h) {
        buy_line = 0;
        PT_AWAIT(it_line, handler(m));
    }
    redraw(m);                                            /* $6876 */
    if (fy_mouse()->rbutton) {                            /* $A9A */
        m->copper = 0;                                    /* $6A52: COP1LC = $AD0 */
        s8(g, 0xE43F, 0);
        fy_set_res(UI_EXIT_TO_MAIN, 0);
    }
    PT_END(it_line);
}

static int enter(MainUI *m, uint32_t handler_addr, uint32_t *next)
{
    Game *g = m->g;
    (void)next;
    if (handler_addr == 0x6944) ui_panel_save(m);        /* $47DC ($460E -> $6948 skips it) */
    fy_reset();
    fy_install_banner(m);
    it_line = buy_line = 0;
    ui_sound(m, 13);
    s32(g, 0xE3D4, fy_starbase(m));
    s8(g, 0xE422, 0);
    s8(g, 0xE425, 5);
    s32(g, 0xE3EC, 0xF45C);
    s16(g, 0xE400, 3);
    s8(g, 0xE443, 0xFF);
    s8(g, 0xE444, 0xFF);
    s8(g, 0xE432, 0);
    return UI_OK;
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    fy_set_mouse(ms);
    iteration(m);
    return fy_res(next);
}

const ScreenDef scr_shipyard = {SCR_SHIPYARD, 0x6944, enter, frame};

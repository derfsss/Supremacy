/* The main screen. See mainscreen.h. Addresses are the original's. */
#include <string.h>

#include "mainscreen.h"
#include "day.h"
#include "setup.h"
#include "gamedata.h"
#include "events.h"
#include "ui.h"

#define MAIN_PICTURE  0x114A8u
#define MAIN_HOTSPOTS 0xF26Cu
#define PANEL_SAVE    0x5F426u
#define PANEL_ADDR    0x76664u

/* ---- memory: the block where it covers the address, else the image ---- */
static int in_block(uint32_t a) { return a >= STATE_BASE && a < STATE_BASE + STATE_SIZE; }

static uint8_t m8(MainUI *m, uint32_t a)
{
    a &= 0x7FFFFu;
    if (in_block(a)) return g8(m->g, a);
    return m->image ? m->image[a] : 0;
}
static uint16_t m16(MainUI *m, uint32_t a) { return (uint16_t)(m8(m, a) << 8 | m8(m, a + 1)); }
static uint32_t m32(MainUI *m, uint32_t a) { return (uint32_t)m16(m, a) << 16 | m16(m, a + 2); }

static const uint8_t *text(MainUI *m, uint32_t a)
{
    a &= 0x7FFFFu;
    if (in_block(a)) return gp(m->g, a);
    return m->image ? m->image + a : NULL;
}

static void draw_text(MainUI *m, unsigned col, unsigned row, uint32_t a)
{
    const uint8_t *t = text(m, a);
    if (t) ui_draw_string(m->s, col, row, t, g8(m->g, 0xE43F));
}

static void sound(MainUI *m, int n)
{
    if (m->g->play_sound) m->g->play_sound(m->g, n);
}

static uint8_t *scr(MainUI *m, uint32_t a)
{
    static uint8_t sink;
    return (a >= SCREEN_BASE && a < SCREEN_BASE + SCREEN_SIZE) ? &m->s->bpl[a - SCREEN_BASE] : &sink;
}

static const uint8_t *text_at(void *user, uint32_t a) { return text((MainUI *)user, a); }

/* ------------------------------------------------------------------ */

void main_init(MainUI *m, Screen *s, Game *g, const uint8_t *image)
{
    memset(m, 0, sizeof *m);
    m->s = s;
    m->g = g;
    m->image = image;
    if (image) memcpy(m->panel, image + PANEL_SAVE, sizeof m->panel);
}

void ui_xor_delta(MainUI *m, uint32_t a)
{
    uint32_t dst = SCREEN_BASE + m16(m, a);
    a += 2;
    uint16_t d1 = m16(m, a);
    a += 2;
    for (;;) {
        uint8_t v = m8(m, a++);
        if (v) {
            *scr(m, dst++) ^= v;
            if (d1-- == 0) return;                 /* dbf */
        } else {
            dst += m8(m, a++);
            d1 = (uint16_t)(d1 - 2);
        }
    }
}

void ui_clear_rect(Screen *s, unsigned col, unsigned line, unsigned words, unsigned lines)
{
    for (unsigned y = 0; y <= (lines & 0xFFFF); y++) {
        uint32_t row = (line + y) * SCREEN_LINE + col;
        for (unsigned w = 0; w <= (words & 0xFFFF); w++)
            for (int pl = 0; pl < 5; pl++) {
                uint32_t o = row + pl * 40u + w * 2u;
                if (o + 1 < SCREEN_SIZE) s->bpl[o] = s->bpl[o + 1] = 0;
            }
    }
}

void ui_planet_name(MainUI *m)
{
    if (g8(m->g, 0xE425)) return;
    uint32_t rec = g32(m->g, 0xE3DC);
    if (g16(m->g, rec + P_OWNER) == 1) {           /* formatting */
        draw_text(m, 0x33, 0x4A, 0x16CF);
        return;
    }
    ui_draw_char(m->s, 0x33, 0x4A, 0x3E);
    const uint8_t *name = gp(m->g, rec + P_NAME);
    ui_draw_string(m->s, 0x34, 0x4A, name, g8(m->g, 0xE43F));
}

void ui_viewer_step(MainUI *m)
{
    uint32_t a = g32(m->g, 0xE3F0);
    if (!a) return;
    for (;;) {
        uint32_t d0 = m32(m, a);
        a += 4;
        if (d0 == 0xFF) { s32(m->g, 0xE3F0, 0); return; }
        if (d0 == 4) {
            s16(m->g, 0xE3F8, (uint16_t)m32(m, a));
            s32(m->g, 0xE3F0, a + 4);
            return;
        }
        if (d0 == 0) { a = m32(m, a); continue; }
        s32(m->g, 0xE3F0, a);
        ui_xor_delta(m, d0);
        return;
    }
}

void ui_viewer_scene(MainUI *m)
{
    Game *g = m->g;
    if (g8(g, 0xE425)) return;
    s16(g, 0xE3F8, 0);
    s8(g, 0xE42E, 0);
    s32(g, 0xE3F0, 0);
    ui_clear_rect(m->s, 0x18, 0x13, 3, 0x33);
    ui_planet_name(m);
    uint32_t rec = g32(g, 0xE3DC);
    if (g16(g, rec + P_OWNER) == 7) {              /* enemy planet: no scene */
        draw_text(m, 0x33, 0x2A, 0x1851);
        return;
    }
    uint32_t t = g16(g, rec + P_OWNER) == 1 ? 0xFF26u
                                            : 0xFED2u + (uint32_t)g8(g, rec + P_TYPE) * 14u;
    s32(g, 0xE3F0, m32(m, t));
    m->colour[21] = m16(m, t + 4);
    m->colour[24] = m16(m, t + 6);
    m->colour[25] = m16(m, t + 8);
    m->colour[26] = m16(m, t + 10);
    s8(g, 0xE44E, m8(m, t + 12));
    ui_viewer_step(m);
}

void ui_viewer_tick(MainUI *m)
{
    Game *g = m->g;
    if (g16(g, 0xE3F8)) { s16(g, 0xE3F8, (uint16_t)(g16(g, 0xE3F8) - 1)); return; }
    if (g8(g, 0xE44E) == 0xFF) return;
    uint8_t d0 = (uint8_t)(g8(g, 0xE42E) + 1);
    if (d0 >= g8(g, 0xE44E)) d0 = 0;
    s8(g, 0xE42E, d0);
    if (d0) return;
    ui_viewer_step(m);
}

void ui_mark_selected(MainUI *m)
{
    Game *g = m->g;
    uint16_t idx = (uint16_t)(g32(g, 0xE3BC) * 0x3Au);         /* mulu.w, word index */
    s8(g, g32(g, G_PLANET_TABLE) + (uint32_t)(int16_t)idx + 5, 1);
    m->arrow_x = (int16_t)(g16(g, 0xE3C2) - 7);
    m->arrow_y = (int16_t)((uint16_t)((uint16_t)(g16(g, 0xE3C6) + 0x40) >> 2) - 3);
    m->arrow_img = g32(g, 0xE3CC);
}

static void unmark(MainUI *m)                                   /* $6E5C */
{
    uint16_t idx = (uint16_t)(g32(m->g, 0xE3BC) * 0x3Au);
    s8(m->g, g32(m->g, G_PLANET_TABLE) + (uint32_t)(int16_t)idx + 5, 0);
}

void ui_select_planet(MainUI *m, uint32_t d0)
{
    Game *g = m->g;
    s32(g, 0xE3BC, d0);
    uint32_t rec = g32(g, G_PLANET_TABLE) + (uint32_t)(uint16_t)(d0 * 0x3Au);
    s32(g, 0xE3DC, rec);
    s32(g, 0xE3D4, rec);
    ui_mark_selected(m);
    ui_viewer_scene(m);
    ui_planet_name(m);
    s16(g, 0xE3F8, 0);
}

void ui_list_click(MainUI *m, const Mouse *ms)
{
    Game *g = m->g;
    if (!ms->button) return;
    uint8_t x = (uint8_t)ms->x;
    if (x < 0xA9 || x >= 0xB5) return;
    uint32_t d3 = (uint16_t)ms->y;
    if ((uint8_t)d3 < 0x11) return;
    d3 = (d3 & 0xFFFF0000u) | (uint16_t)(d3 - 0x11);
    unsigned sh = 3;
    if (g8(g, 0xE43B)) { sh--; if (g8(g, 0xE43B) != 1) sh--; }
    d3 = (d3 & 0xFFFF0000u) | (uint16_t)((uint16_t)d3 >> sh);
    if ((uint8_t)d3 >= (uint8_t)(g8(g, 0xE41E) + 1)) return;
    if (g32(g, 0xE3BC) == d3) return;
    unmark(m);
    ui_select_planet(m, d3);
}

uint32_t ui_hotspots(MainUI *m, const Mouse *ms)
{
    Game *g = m->g;
    if (g8(g, 0xE42D)) return 0;
    if (!ms->button) {
        if (g8(g, 0xE422)) {
            s8(g, 0xE422, 0);
            uint32_t hl = g32(g, 0xE3E0);
            if (hl) ui_xor_delta(m, hl);
        }
        return 0;
    }
    if (g8(g, 0xE422)) return 0;
    uint32_t t = g32(g, 0xE3EC);
    uint16_t n = (uint16_t)(g16(g, 0xE400) - 1);
    for (;;) {
        int16_t x0 = (int16_t)m16(m, t), y0 = (int16_t)m16(m, t + 2);
        int16_t x1 = (int16_t)m16(m, t + 4), y1 = (int16_t)m16(m, t + 6);
        if (ms->x >= x0 && ms->y >= y0 && ms->x <= x1 && ms->y <= y1) {
            uint32_t hl = m32(m, t + 8);
            s32(g, 0xE3E0, hl);
            if (hl) ui_xor_delta(m, hl);
            s8(g, 0xE422, 0xFF);
            return m32(m, t + 12);
        }
        t += 16;
        if (n-- == 0) return 0;
    }
}

/* $3346: one digit (signed compare), then d0.b + 1 */
static unsigned digit16(Screen *s, unsigned col, unsigned row, uint32_t *v, uint32_t p)
{
    uint32_t d3 = *v & 0xFFFF;
    unsigned ch = 0x2F;
    do { ch++; d3 -= p; } while ((int32_t)d3 >= 0);
    d3 += p;
    *v = d3;
    ui_draw_char(s, col, row, ch);
    return (col & 0xFF00u) | ((col + 1) & 0xFFu);
}

void ui_draw_date(MainUI *m)
{
    Game *g = m->g;
    unsigned col, row, sc = g8(g, 0xE425);
    if (sc == 0) { col = 0x35; row = 0x0B; }
    else if (sc == 3) { col = 0x15; row = 0x12; }
    else if (sc == 4) { col = 0x1D; row = 0x3A; }
    else return;
    uint32_t v = g16(g, G_DAY);
    if ((int16_t)v < 10) {                         /* $331A: a space first */
        ui_draw_char(m->s, col, row, 0x20);
        col = (col & 0xFF00u) | ((col + 1) & 0xFFu);
    } else {
        col = digit16(m->s, col, row, &v, 10);
    }
    col = digit16(m->s, col, row, &v, 1);
    ui_draw_char(m->s, col, row, 0x2F);
    col = (col + 1) & 0xFFFFu;
    v = g16(g, G_YEAR);
    col = digit16(m->s, col, row, &v, 1000);
    col = digit16(m->s, col, row, &v, 100);
    col = digit16(m->s, col, row, &v, 10);
    digit16(m->s, col, row, &v, 1);
}

void ui_panel_save(MainUI *m)
{
    uint8_t *p = m->panel;
    for (int r = 0; r < 0x67; r++) {
        uint8_t *a = scr(m, PANEL_ADDR + r * SCREEN_LINE), *b = a + 40;
        for (int k = 0; k < 4; k++) {
            memcpy(p, a + k * 4, 4); p += 4;
            memcpy(p, b + k * 4, 4); p += 4;
        }
        memcpy(p, a + 16, 2); p += 2;
        memcpy(p, b + 16, 2); p += 2;
    }
}

void ui_panel_restore(MainUI *m)
{
    const uint8_t *p = m->panel;
    for (int r = 0; r < 0x67; r++) {
        uint8_t *a = scr(m, PANEL_ADDR + r * SCREEN_LINE), *b = a + 40;
        for (int k = 0; k < 4; k++) {
            memcpy(a + k * 4, p, 4); p += 4;
            memcpy(b + k * 4, p, 4); p += 4;
        }
        memcpy(a + 16, p, 2); p += 2;
        memcpy(b + 16, p, 2); p += 2;
    }
}

void main_reenter(MainUI *m)
{
    Game *g = m->g;
    m->screen = 0;
    m->colour[31] = g16(g, 0xE3FE);
    s32(g, 0xE3EC, MAIN_HOTSPOTS);
    s16(g, 0xE400, 0x11);
    s8(g, 0xE425, 0);
    ui_viewer_scene(m);
    ui_mark_selected(m);
    ui_draw_date(m);
}

static void panel_static(Game *g, int i, uint16_t v)          /* $46A8 */
{
    MainUI *m = (MainUI *)g->user;
    uint32_t a = PANEL_ADDR + (uint32_t)(i / 9) * SCREEN_LINE + (uint32_t)(i % 9) * 2u;
    for (int pl = 0; pl < 2; pl++) {
        *scr(m, a + pl * 40u) = (uint8_t)(v >> 8);
        *scr(m, a + pl * 40u + 1) = (uint8_t)v;
    }
}

void main_new_game_screen(MainUI *m)
{
    if (m->image) picture_unpack_raw(m->image, MAIN_PICTURE, m->s->bpl, m->colour);
    m->g->user = m;
    m->g->panel_static_hook = panel_static;
}

void main_new_game_done(MainUI *m)
{
    ui_clear_rect(m->s, 0x14, 0x5A, 8, 0x66);      /* $47C8: the static is cleared */
    main_reenter(m);
}

void main_enter(MainUI *m)
{
    if (m->image) picture_unpack_raw(m->image, MAIN_PICTURE, m->s->bpl, m->colour);
    ui_panel_restore(m);
    main_reenter(m);
}

/* ---- main-screen handlers (table $F26C) ---- */

static void planet_info(MainUI *m)                           /* $4CAE */
{
    Game *g = m->g;
    sound(m, 13);
    if ((uint8_t)g16(g, G_MSG_COUNT) >= 0x74) return;
    uint32_t rec = g32(g, 0xE3DC);
    post_message(g, 0x1677);
    post_message(g, rec + 0x0E);
    post_message(g, 0x1696);
    post_message(g, rec + 0x19);
    post_message(g, 0x16A0);
    post_message(g, 0x16A4);
    uint16_t o = g16(g, rec + P_OWNER);                       /* $4DDE */
    post_message(g, o == 4 ? 0x16C4 : o == 1 ? 0x16CF : o == 7 ? 0x16B9 : 0x16AE);
    uint32_t r = g32(g, G_PLANET_TABLE);                      /* $5A76 */
    uint8_t d5 = 0, d6 = 0;
    for (unsigned k = 0; k <= g16(g, 0xE410); k++, r += PLANET_SIZE) {
        if (g16(g, r + P_OWNER) == 10) d5++;
        if (g16(g, r + P_OWNER) == 7) d6++;
    }
    s16(g, 0xD5BC, d5);
    s16(g, 0xD5D0, d6);
    s16(g, 0xD5E4, (uint16_t)(g16(g, 0xE40E) - (d5 + d6)));
    post_message(g, 0xD5A9);
    post_message(g, 0xD5BE);
    post_message(g, 0xD5D2);
    post_message(g, 0x1642);
}

int main_handler(MainUI *m, uint32_t h)
{
    Game *g = m->g;
    switch (h) {
    case 0x4BEA: {                                            /* previous planet */
        sound(m, 13);
        unmark(m);
        uint32_t d0 = g32(g, 0xE3BC);
        if ((uint8_t)d0) d0 = (d0 & ~0xFFu) | (uint8_t)(d0 - 1);
        else d0 = (d0 & ~0xFFu) | g8(g, 0xE41E);
        ui_select_planet(m, d0);
        return 1;
    }
    case 0x4C12: {                                            /* next planet */
        sound(m, 13);
        unmark(m);
        uint32_t d0 = g32(g, 0xE3BC);
        if ((uint8_t)d0 == g8(g, 0xE41E)) d0 &= ~0xFFu;
        else d0 = (d0 & ~0xFFu) | (uint8_t)(d0 + 1);
        ui_select_planet(m, d0);
        return 1;
    }
    case 0x4E0A:                                              /* pause */
        sound(m, 6);
        m->pause = 1;
        return 1;
    case 0x4CAE:
        planet_info(m);
        return 1;
    case 0x497E:                                              /* acknowledge "continue" */
        s8(g, 0xE448, 0);
        draw_text(m, 0x4D, 0x67, 0x1CDE);
        return 1;
    case 0x59AE6:                                             /* sound effects on/off */
        sound(m, 13);
        s8(g, 0xE42B, (uint8_t)~g8(g, 0xE42B));
        post_message(g, g8(g, 0xE42B) ? 0x2180 : 0x2196);
        return 1;
    }
    return 0;
}

/* ---- event waits ($71E0 calls the script, which runs a loop of its own) ---- */
enum { EVW_NONE, EVW_DRAIN, EVW_BOX, EVW_NO_DRAIN, EVW_FLASH };

static void event_wait(MainUI *m, int st, uint32_t resume)
{
    m->ev_resume = resume;
    if (st == PASS_WAIT_MESSAGES) {                /* op $78: $B71A -> $5BE2 */
        scr_drain_begin(m);
        m->ev = EVW_DRAIN;
    } else if (st == PASS_YESNO) {                 /* op $7C: the box $B726 */
        m->ev = EVW_BOX;
    } else {
        m->ev = EVW_NONE;
    }
}

void main_event_wait(MainUI *m, int status, uint32_t resume)
{
    event_wait(m, status, resume);
}

/* One frame of the waiting script's loop (from one $2FFA to the next); 1 when
 * the script has finished and the interrupted iteration goes on after $71E0. */
static int event_frame(MainUI *m, const Mouse *ms)
{
    Game *g = m->g;
    switch (m->ev) {
    case EVW_DRAIN:
        if (!scr_drain_frame(m)) return 0;
        break;
    case EVW_BOX: {
        int yes = ui_yesno_frame(m, ms);
        if (yes < 0) return 0;
        if (!yes) {                                /* $B7C2 up to its wait */
            events_answer_no_begin(g);
            scr_drain_begin(m);
            m->ev = EVW_NO_DRAIN;
            return 0;
        }
        events_answer_yesno(g, 1);                 /* $B782, then $B76A */
        break;
    }
    case EVW_NO_DRAIN:
        if (!scr_drain_frame(m)) return 0;
        if (g->play_sound) g->play_sound(g, 0x21); /* $B7D6 */
        m->ev = EVW_FLASH;
        m->ev_flash = 0;
        m->ev_col0 = m->colour[0];
        return 0;
    case EVW_FLASH: {                              /* $B7E6: 66 x white, black, (black) */
        int k = m->ev_flash++;
        if (k % 3 == 0) m->colour[0] = 0xFFF;
        else if (k % 3 == 1) m->colour[0] = 0x000;
        if (k < 66 * 3 - 1) return 0;
        m->colour[0] = m->ev_col0;
        events_answer_no_end(g);                   /* $B806, then $B76A */
        break;
    }
    default:
        m->ev = EVW_NONE;
        return 1;
    }
    uint32_t resume = 0;
    int st = game_event_resume(g, m->ev_resume, &resume);
    event_wait(m, st, resume);
    return m->ev == EVW_NONE;
}

int main_event_step(MainUI *m, const Mouse *ms)
{
    return event_frame(m, ms);
}

static int rest_of_frame(MainUI *m)
{
    Game *g = m->g;
    uint32_t resume = 0;
    int st = game_pass(g, &resume);
    if (g8(g, 0xE426) == 0xFF)                 /* $77E8 runs on into the date redraw $7806 */
        ui_draw_date(m);
    if (st == PASS_GAME_OVER) return MAIN_GAME_OVER;
    if (st != PASS_OK) { event_wait(m, st, resume); return MAIN_EVENT_WAIT; }
    return MAIN_OK;
}

int main_frame(MainUI *m, const Mouse *ms, uint32_t *handler)
{
    Game *g = m->g;
    if (m->ev) {                                              /* inside $71E0 */
        if (!event_frame(m, ms)) return MAIN_OK;
        goto after_pass;
    }
    if (m->pause) {                                           /* $4E0A's wait loops */
        if (m->pause == 1) { if (!ms->button) m->pause = 2; return MAIN_PAUSED; }
        if (!ms->button) return MAIN_PAUSED;
        m->pause = 0;
        goto after_hotspots;
    }
    ui_viewer_tick(m);
    uint32_t h = ui_hotspots(m, ms);
    if (h && !main_handler(m, h)) {
        if (handler) *handler = h;
        return MAIN_HANDLER;
    }
    if (m->pause) return MAIN_PAUSED;
after_hotspots:;
    int st = rest_of_frame(m);
    if (st == MAIN_EVENT_WAIT) return MAIN_OK;                /* the script's loop from here */
    if (st != MAIN_OK) return st;
after_pass:;
    uint32_t arrow = 0;
    ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);
    ui_mark_selected(m);
    ui_list_click(m, ms);
    ui_message_pass(m->s, g, text_at, m);
    if (!g8(g, 0xE429)) ui_message_type(m->s, g, text_at, m);
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    m->colour[31] = g16(g, 0xE3FE);
    return MAIN_OK;
}

/* ---- for tests (ctypes) ---- */
#include <stdlib.h>

UI_API MainUI *mainui_new(Game *g, const uint8_t *image)
{
    MainUI *m = calloc(1, sizeof *m);
    Screen *s = calloc(1, sizeof *s);
    if (!m || !s) { free(m); free(s); return NULL; }
    screen_init(s, image ? image + FONT_ADDR : NULL);
    main_init(m, s, g, image);
    return m;
}
UI_API uint8_t *mainui_screen(MainUI *m) { return m->s->bpl; }
UI_API uint8_t *mainui_panel(MainUI *m) { return m->panel; }
UI_API int mainui_frame(MainUI *m, int x, int y, int button, uint32_t *handler)
{
    Mouse ms = {(int16_t)x, (int16_t)y, (uint8_t)(button & 1), (uint8_t)((button >> 1) & 1)};
    return main_frame(m, &ms, handler);
}
UI_API void mainui_new_game(MainUI *m, int opponent, uint32_t seed)
{
    main_new_game_screen(m);
    game_new_game(m->g, opponent, seed);
    main_new_game_done(m);
}

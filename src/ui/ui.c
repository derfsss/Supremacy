/* The screens and how control moves between them. See ui.h. */
#include <stddef.h>

#include "ui.h"
#include "day.h"
#include "gamedata.h"

/* every entry handler of a screen (the main screen's boxes, the left-column
 * screen switches) and the screen it starts */
static const struct { uint32_t handler; const ScreenDef *def; } entries[] = {
    {0x825A, &scr_stats}, {0x825E, &scr_stats}, {0x4614, &scr_stats},
    {0x6944, &scr_shipyard}, {0x460E, &scr_shipyard},
    {0x8C30, &scr_fleet}, {0x4620, &scr_fleet},
    {0x4F5C, &scr_format},
    {0x5CC2, &scr_troops}, {0x461A, &scr_troops},
    {0x7874, &scr_cargo}, {0x8EF8, &scr_cargo}, {0x8EFC, &scr_cargo}, {0x8F1A, &scr_cargo},
    {0x70C2, &scr_surface}, {0x59B08, &scr_surface},
    {0x3A2A, &scr_combat}, {0x4500, &scr_combat}, {0x44D2, &scr_combat}, {0x455A, &scr_combat},
    {0x5880, &scr_spy},
    {0x559A, &scr_options},
    {0x3518, &scr_starbase},
};

static const ScreenDef *by_id(int id)
{
    for (size_t k = 0; k < sizeof entries / sizeof entries[0]; k++)
        if (entries[k].def->id == id) return entries[k].def;
    return NULL;
}

void ui_exit_to_main(MainUI *m)
{
    ui_sound(m, 13);
    s8(m->g, 0xE43F, 0);
    s8(m->g, 0xE43D, 0);
    main_enter(m);
}

int ui_goto(MainUI *m, uint32_t h, uint32_t *out)
{
    for (int depth = 0; depth < 8; depth++) {
        const ScreenDef *d = NULL;
        for (size_t k = 0; k < sizeof entries / sizeof entries[0]; k++)
            if (entries[k].handler == h) { d = entries[k].def; break; }
        if (!d) { if (out) *out = h; return UI_HANDLER; }
        int prev = m->screen;
        m->screen = d->id;
        uint32_t next = 0;
        int r = d->enter(m, h, &next);
        if (r == UI_GOTO) { h = next; continue; }
        if (r == UI_EXIT_TO_MAIN) { ui_exit_to_main(m); return UI_OK; }
        if (r == UI_HANDLER) { m->screen = prev; if (out) *out = h; return UI_HANDLER; }
        return r;
    }
    return UI_OK;
}

int ui_frame(MainUI *m, const Mouse *ms, uint32_t *handler)
{
    uint32_t h = 0;
    int r;
    if (m->ev && m->screen != SCR_MAIN) {
        /* An event script waiting in another screen's game pass: only
         * script 8 waits and it needs $E425 = 0, which the options panel
         * keeps (it is drawn over the main screen). The original runs the
         * script's message wait and YES/NO box inside that screen's loop;
         * here they run until the script ends, then the screen goes on. */
        main_event_step(m, ms);
        return UI_OK;
    }
    if (m->screen == SCR_MAIN) {
        r = main_frame(m, ms, &h);
        if (r == MAIN_HANDLER) return ui_goto(m, h, handler);
        if (r == MAIN_EVENT_WAIT && handler) *handler = h;
        return r;
    }
    const ScreenDef *d = by_id(m->screen);
    if (!d) { main_enter(m); return UI_OK; }
    r = d->frame(m, ms, &h);
    if (r == UI_EXIT_TO_MAIN) { ui_exit_to_main(m); return UI_OK; }
    if (r == UI_GOTO || r == UI_HANDLER) return ui_goto(m, h, handler);
    if (r == UI_EVENT_WAIT && handler) *handler = h;
    return r;
}

int ui_game_pass(MainUI *m, uint32_t *status)
{
    uint32_t resume = 0;
    int st = game_pass(m->g, &resume);
    if (g8(m->g, 0xE426) == 0xFF)                  /* $77E8 runs on into $7806 */
        ui_draw_date(m);
    if (st == PASS_GAME_OVER) return UI_GAME_OVER;
    if (st != PASS_OK) {
        /* Only script 8 waits, and only on the main screen ($E425 = 0): the
         * main screen runs the wait (it keeps the resume address; before,
         * it was lost here and the YES/NO hotspots stayed with no box) */
        if (status) *status = (uint32_t)st;
        main_event_wait(m, st, resume);
        return UI_EVENT_WAIT;
    }
    return UI_OK;
}

void ui_show_picture(MainUI *m, uint32_t addr)
{
    if (m->image) picture_unpack_raw(m->image, addr, m->s->bpl, m->colour);
}

static int in_block(uint32_t a) { return a >= STATE_BASE && a < STATE_BASE + STATE_SIZE; }

uint8_t ui_m8(MainUI *m, uint32_t a)
{
    a &= 0x7FFFFu;
    if (in_block(a)) return g8(m->g, a);
    return m->image ? m->image[a] : 0;
}
uint16_t ui_m16(MainUI *m, uint32_t a) { return (uint16_t)(ui_m8(m, a) << 8 | ui_m8(m, a + 1)); }
uint32_t ui_m32(MainUI *m, uint32_t a) { return (uint32_t)ui_m16(m, a) << 16 | ui_m16(m, a + 2); }

/* ---- port mode: the texts that talk about the Amiga or disks ---- */
static int port_mode;
static const struct { uint32_t addr; const char *text; } port_texts[] = {
    {0x215E, " QUIT GAME / RESTART GAME / EXITÿ"},  /* RESET AMIGA / ...; same columns */
    {0x1DFE, "SAVES ARE IN THE 'SAVE' FOLDER.ÿ"},   /* PUT YOUR FORMATTED DISK IN DF0: */
    {0x1DD2, " ERROR!  CANNOT USE SAVE FILE.ÿ"},    /*  ERROR!  PLEASE CHECK YOUR DISK. */
};

void ui_set_port_mode(int on) { port_mode = on; }
int  ui_port_mode(void) { return port_mode; }

const uint8_t *ui_text(MainUI *m, uint32_t a)
{
    a &= 0x7FFFFu;
    if (port_mode)
        for (size_t k = 0; k < sizeof port_texts / sizeof port_texts[0]; k++)
            if (port_texts[k].addr == a) return (const uint8_t *)port_texts[k].text;
    if (in_block(a)) return gp(m->g, a);
    return m->image ? m->image + a : NULL;
}

void ui_draw_text(MainUI *m, unsigned col, unsigned row, uint32_t a)
{
    const uint8_t *t = ui_text(m, a);
    if (t) ui_draw_string(m->s, col, row, t, g8(m->g, 0xE43F));
}

void ui_sound(MainUI *m, int n)
{
    if (m->g->play_sound) m->g->play_sound(m->g, n);
}

/* ---- keyboard: name entry and the key prompts of the screens ---- */
UI_API void ui_shipyard_key(int ascii);
UI_API void ui_fleet_key(int ascii);
UI_API void scr_cargo_key(int ch);
UI_API int  ui_stats_key(int ascii);

void ui_key(MainUI *m, int ascii)
{
    switch (m->screen) {
    case SCR_SHIPYARD: ui_shipyard_key(ascii); break;
    case SCR_FLEET: ui_fleet_key(ascii); break;
    case SCR_CARGO: scr_cargo_key(ascii); break;
    case SCR_STATS:
    case SCR_FORMAT: ui_stats_key(ascii); break;     /* format's name prompt is the stats text entry */
    default: break;
    }
}

/* ---- for tests (ctypes) ---- */
UI_API int mainui_ui_frame(MainUI *m, int x, int y, int buttons, uint32_t *handler)
{
    Mouse ms = {(int16_t)x, (int16_t)y, (uint8_t)(buttons & 1), (uint8_t)((buttons >> 1) & 1)};
    return ui_frame(m, &ms, handler);
}
UI_API void mainui_set_screen(MainUI *m, int id) { m->screen = id; }
UI_API int  mainui_get_screen(MainUI *m) { return m->screen; }
UI_API int  mainui_goto(MainUI *m, uint32_t handler, uint32_t *out) { return ui_goto(m, handler, out); }
UI_API uint16_t *mainui_colours(MainUI *m) { return m->colour; }

/* $B742: the YES/NO box's loop (no game time, no message pass) */
int ui_yesno_frame(MainUI *m, const Mouse *ms)
{
    Game *g = m->g;
    ui_viewer_tick(m);                                             /* $4ED8 */
    uint32_t arrow = 0;
    ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);  /* $4850 */
    ui_mark_selected(m);                                           /* $4B4C */
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    uint32_t h = ui_hotspots(m, ms);                               /* $9A6C */
    if (h == 0xB782) return 1;
    if (h == 0xB7C2) return 0;
    return -1;
}

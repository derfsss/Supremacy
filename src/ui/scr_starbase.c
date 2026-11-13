/*
 * Main-screen box 2: the starbase ($3518). Not a screen of its own: the
 * handler selects the player's starbase ($3506, $3534) and shows it
 * ($4E28), then returns into the main loop, whose iteration goes on
 * ($71E0, $4850, $4B4C, $4B88, $4994/$49EA, $E42C + 1). enter() does both
 * and puts the main screen back, so one call is one iteration of the
 * original. Checked against the original game running in an emulator.
 *
 * scr_main_rest() is that rest of the main loop's iteration (the part of
 * mainscreen.c's main_frame after its hotspots, which is static there);
 * the spy report and format handlers use it too.
 */
#include "ui.h"
#include "day.h"

void scr_select_e3d4(MainUI *m);                                    /* scr_stats.c */
int  scr_main_rest(MainUI *m, const Mouse *ms, uint32_t *status);


static const uint8_t *text_at(void *user, uint32_t a) { return ui_text((MainUI *)user, a); }

/* The main loop after the handler returned to $9A6C. `ms` = NULL skips
 * $4B88 (the clicked box is outside the planet-list column, so it would do
 * nothing). Puts the main screen back. */
int scr_main_rest(MainUI *m, const Mouse *ms, uint32_t *status)
{
    Game *g = m->g;
    m->screen = SCR_MAIN;
    int r = ui_game_pass(m, status);
    if (r == UI_EVENT_WAIT) return UI_OK;     /* main_frame runs the script's wait from here */
    if (r != UI_OK) return r;
    uint32_t arrow = 0;
    ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);
    ui_mark_selected(m);
    if (ms) ui_list_click(m, ms);
    ui_message_pass(m->s, g, text_at, m);
    if (!g8(g, 0xE429)) ui_message_type(m->s, g, text_at, m);
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    m->colour[31] = g16(g, 0xE3FE);
    return UI_OK;
}

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    Game *g = m->g;
    (void)handler; (void)next;
    ui_sound(m, 13);
    s32(g, 0xE3D4, g32(g, G_PLANET_TABLE) + (uint32_t)(uint16_t)(g16(g, 0xE410) * PLANET_SIZE));
    scr_select_e3d4(m);
    ui_viewer_scene(m);
    uint32_t st = 0;
    return scr_main_rest(m, NULL, &st);   /* an event wait: main_frame runs it */
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    m->screen = SCR_MAIN;
    return main_frame(m, ms, next);
}

const ScreenDef scr_starbase = {SCR_STARBASE, 0x3518, enter, frame};

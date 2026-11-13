/*
 * Main-screen box 8: FORMAT the selected lifeless planet with an
 * atmosphere processor ($4F5C). Not a screen with a picture: the handler
 * posts its report and, if a format can start, waits for the messages and
 * asks for the planet's name in the message panel, then returns into the
 * main loop. The state changes are craft_format_check / start / abort
 * (src/core/craft.c); this file has the UI part ($E3A8, the input lock
 * $E42D, the waits, the name prompt, the main-screen redraw). Checked
 * frame by frame against the original game running in an emulator.
 *
 * State machine (each frame() = the code between two $2FFA waits):
 *   FM_DRAIN  $5BE2 (scr_spy.c): the panel types the report.
 *   FM_TEXT   $2390: the name entry at column $2A, row $B8, preset with
 *             the default name $E38E (scr_stats.c's text entry; keys from
 *             ui_stats_key; time frozen while $E44F = $FF).
 * The frame that ends it runs the rest of the main loop (scr_main_rest).
 * When the handler ends at once (no processor, a progress report, the
 * wrong planet) enter() runs that rest itself.
 */
#include "ui.h"
#include "day.h"
#include "craft.h"

int  scr_main_rest(MainUI *m, const Mouse *ms, uint32_t *status);  /* scr_starbase.c */
void scr_text_begin(MainUI *m, uint32_t buf, unsigned col, unsigned row, int preset);   /* scr_stats.c */
int  scr_text_frame(MainUI *m, int *pass, uint32_t *status);
int  scr_text_aborted(void);

enum { FM_DRAIN, FM_TEXT };
static int st;

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    Game *g = m->g;
    (void)handler; (void)next;
    ui_sound(m, 13);
    for (int b = 0; b < NBAYS; b++) {                              /* the first processor */
        uint32_t a = BAY_TABLE + (uint32_t)b * BAY_SIZE;
        if (g8(g, a + 0x1E) == 4) { s32(g, 0xE3A8, a); break; }
    }
    if (craft_format_check(g, g32(g, 0xE3DC)) == 0) {
        s8(g, 0xE42D, 0xFF);                                       /* $3006 */
        scr_drain_begin(m);
        st = FM_DRAIN;
        return UI_OK;
    }
    uint32_t status = 0;
    return scr_main_rest(m, NULL, &status);  /* an event wait: main_frame runs it */
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    switch (st) {
    case FM_DRAIN:
        if (!scr_drain_frame(m)) return UI_OK;
        s8(g, 0xE42D, 0xFF);                                       /* $505A */
        scr_text_begin(m, g32(g, 0xE3DC) + P_NAME, 0x2A, 0xB8, 1);
        st = FM_TEXT;
        break;                                                     /* the first poll */
    }
    int pass;
    if (!scr_text_frame(m, &pass, next)) return pass;
    uint32_t p = g32(g, 0xE3DC);
    if (scr_text_aborted()) {
        craft_format_abort(g, p);                                  /* "LIFELESS!", "ABORTED" */
    } else {
        uint8_t name[9];
        for (int k = 0; k < 9; k++) name[k] = g8(g, p + P_NAME + k);
        craft_format_start(g, p, name);
        ui_planet_name(m);                                         /* $4C68 */
        ui_viewer_scene(m);                                        /* $4E28 */
    }
    s8(g, 0xE42D, 0);                                              /* $301C */
    return scr_main_rest(m, ms, next);
}

const ScreenDef scr_format = {SCR_FORMAT, 0x4F5C, enter, frame};

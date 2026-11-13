/*
 * Supremacy port - the screens and how control moves between them.
 *
 * Each screen of the original is a loop of its own; here each is a module
 * (src/ui/scr_<name>.c) with an enter function (the handler the original
 * jumps to) and a frame function (one iteration of its loop). ui_frame()
 * runs the current screen; when a frame returns UI_HANDLER the handler is
 * looked up in the screen table and that screen is entered, as the
 * original's jump would.
 *
 * Rules every screen module follows (they keep the checks byte-exact):
 *  - translate the original routine by routine onto the screen buffer
 *    (screen.h) and the state block, with the original's addresses in the
 *    comments; display-only hardware state (palette, sprites) goes to
 *    MainUI fields;
 *  - one frame() = one iteration of the original's loop, in its order;
 *  - the common pieces are in mainscreen.h / ui.h: ui_viewer_tick,
 *    ui_hotspots, ui_game_pass (game pass + the date redraw), the panel
 *    save, the exit to the main screen.
 */
#ifndef SUP_UI_H
#define SUP_UI_H

#include <stddef.h>

#include "mainscreen.h"

enum {
    UI_OK = 0,
    UI_PAUSED = MAIN_PAUSED,
    UI_HANDLER = MAIN_HANDLER,      /* *handler: a handler not ported yet */
    UI_GAME_OVER = MAIN_GAME_OVER,
    UI_EVENT_WAIT = MAIN_EVENT_WAIT,
    UI_EXIT_TO_MAIN = 100,          /* screen modules: leave through $60A6 */
    UI_GOTO = 101,                  /* screen modules: jump to *handler (another screen) */
    UI_QUIT = 102                   /* options: RESET AMIGA (the slave's quit): end the program */
};

/* screen ids ($E425 values where the original has one) */
enum {
    SCR_MAIN = 0,
    SCR_TROOPS = 1,
    SCR_CARGO = 2,
    SCR_STATS = 3,
    SCR_FLEET = 4,
    SCR_SHIPYARD = 5,
    SCR_SURFACE = 6,
    SCR_COMBAT = 7,
    SCR_SPY = 8,                    /* port ids from here (not $E425 values) */
    SCR_OPTIONS = 9,
    SCR_FORMAT = 10,
    SCR_STARBASE = 11,
    SCR_COUNT
};

typedef struct ScreenDef {
    int      id;
    uint32_t handler;               /* the original's entry for this screen */
    /* enter: the code at `handler` up to the loop; may return UI_GOTO /
     * UI_EXIT_TO_MAIN (e.g. "ACCESS DENIED") or UI_OK */
    int    (*enter)(MainUI *m, uint32_t handler, uint32_t *next);
    int    (*frame)(MainUI *m, const Mouse *ms, uint32_t *next);
} ScreenDef;

/* One iteration of the current screen. *handler gets a handler that is not
 * ported (UI_HANDLER) or the event status (UI_EVENT_WAIT). */
UI_API int  ui_frame(MainUI *m, const Mouse *ms, uint32_t *handler);
/* Enter the screen whose entry is `handler`; UI_HANDLER if none. */
UI_API int  ui_goto(MainUI *m, uint32_t handler, uint32_t *out);

/* A key for the current screen (ASCII; 8 DEL, 13 RETURN, 27 ESC). */
UI_API void ui_key(MainUI *m, int ascii);

/* ---- helpers for screen modules ---- */
/* $71E0 with the date redraw after a day advance; returns UI_OK,
 * UI_GAME_OVER or UI_EVENT_WAIT (*status). */
UI_API int  ui_game_pass(MainUI *m, uint32_t *status);
/* $60A6: click sound 13, clear $E43F/$E43D, back to the main screen ($4748). */
UI_API void ui_exit_to_main(MainUI *m);
/* $2C62: show the picture at `addr` (its palette into m->colour). */
UI_API void ui_show_picture(MainUI *m, uint32_t addr);
/* the original's memory, read: the block where it covers the address,
 * else the image */
UI_API uint8_t  ui_m8(MainUI *m, uint32_t a);
UI_API uint16_t ui_m16(MainUI *m, uint32_t a);
UI_API uint32_t ui_m32(MainUI *m, uint32_t a);
/* a text at an original address (block or image), for ui_draw_string */
UI_API const uint8_t *ui_text(MainUI *m, uint32_t a);
UI_API void ui_draw_text(MainUI *m, unsigned col, unsigned row, uint32_t a);
UI_API void ui_sound(MainUI *m, int n);

/* $5BE2, the message wait (scr_spy.c): begin, then frames until 1 (the
 * queue empty). Event op $78 waits this way too. */
UI_API void scr_drain_begin(MainUI *m);
UI_API int  scr_drain_frame(MainUI *m);

/* Event op $7C ($B726): one frame of the Scorpion V4 YES/NO loop (the core
 * has set the hotspots to $F74C). -1 still waiting, 1 YES ($B782), 0 NO
 * ($B7C2). */
UI_API int  ui_yesno_frame(MainUI *m, const Mouse *ms);

/* Port mode (the exe turns it on; the checks against the original leave it
 * off): texts about the Amiga and disks are replaced (saves are files), and
 * the options panel's FORMAT button is painted out and does nothing. */
UI_API void ui_set_port_mode(int on);
UI_API int  ui_port_mode(void);

/* Game options (scr_options.c): saving and loading go through the platform.
 * slot 0-3 = files SSG1-SSG4 (n = 7,000 bytes, no header); return 0 on
 * success; load must fail on a missing or short file. */
typedef int (*ui_options_save_fn)(int slot, const uint8_t *blk, size_t n, void *user);
typedef int (*ui_options_load_fn)(int slot, uint8_t *blk, size_t n, void *user);
UI_API void ui_options_set_io(ui_options_save_fn save, ui_options_load_fn load, void *user);

/* the screen table (ui.c); each scr_*.c defines its ScreenDef */
extern const ScreenDef scr_stats, scr_shipyard, scr_fleet, scr_troops, scr_cargo,
                       scr_surface, scr_combat, scr_spy, scr_options, scr_format,
                       scr_starbase;

#endif

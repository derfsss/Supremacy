/*
 * Supremacy port - the main screen ($4748 / the loop at $478E), translated
 * routine by routine onto the original's screen buffer (screen.h).
 * Checked frame by frame against the original game running in an emulator.
 *
 * One call of main_frame() is one iteration of the original's loop:
 *   $2FFA wait frame (the caller's), $4ED8 viewer tick, $9B66 pointer (the
 *   platform's), $9A6C hotspots, $71E0 one game pass, $4850 star map,
 *   $4B4C selection arrow, $4B88 planet-list click, $4994 message panel,
 *   $49EA typing again unless scrolling, $E42C + 1.
 */
#ifndef SUP_MAINSCREEN_H
#define SUP_MAINSCREEN_H

#include "screen.h"

typedef struct Mouse {
    int16_t x, y;                  /* $AB4, $ABA */
    uint8_t button;                /* $A99: left button down */
    uint8_t rbutton;               /* $A9A: right button down (leaves a screen) */
} Mouse;

typedef struct MainUI {
    Screen  *s;
    Game    *g;
    const uint8_t *image;          /* the original's memory image (tables, texts) */
    uint16_t colour[32];           /* hardware palette, 12-bit $0RGB */
    uint8_t  panel[0x67 * 36];     /* the message panel saved at $5F426 */
    int16_t  arrow_x, arrow_y;     /* selection arrow sprite ($4B4C), pixels */
    uint32_t arrow_img;            /* $592D0 / $592F4 */
    int      pause;                /* $4E0A: 1 = waiting for release, 2 = for a click */
    int      screen;               /* ui.h SCR_*: the screen whose loop runs */
    uint32_t copper;               /* COP1LC when not the plain $AD0 (0): the shipyard's
                                      $F974 / $F85C, which change colours down the screen */
    /* an event script waiting inside the game pass (only script 8, the
     * Scorpion V4): the original runs its own loops there, one $2FFA each */
    int      ev;                   /* 0, or which loop (mainscreen.c EVW_*) */
    uint32_t ev_resume;            /* where the script goes on */
    int      ev_flash;             /* NO: frames of the flash done */
    uint16_t ev_col0;              /* NO: colour 0 before the flash */
} MainUI;

enum {
    MAIN_OK = 0,                   /* iteration done */
    MAIN_PAUSED,                   /* paused ($4E0A): nothing else ran */
    MAIN_HANDLER,                  /* a hotspot whose handler is not here: *handler */
    MAIN_GAME_OVER,                /* game_pass ended the game */
    MAIN_EVENT_WAIT                /* (not returned: event waits run inside main_frame) */
};

UI_API void main_init(MainUI *m, Screen *s, Game *g, const uint8_t *image);
/* $2C62 + $4816 + $475E: show the main picture, restore the panel, set the
 * hotspot table, screen 0, the viewer, the arrow and the date. */
UI_API void main_enter(MainUI *m);
/* The new game's screen: the main picture and the set-up's panel static
 * (call before game_new_game; it sets g->panel_static_hook and g->user). */
UI_API void main_new_game_screen(MainUI *m);
/* After game_new_game: clear the panel ($47C8) and enter the loop ($475E). */
UI_API void main_new_game_done(MainUI *m);
/* $475E only (the entry from the new-game set-up). */
UI_API void main_reenter(MainUI *m);
UI_API int  main_frame(MainUI *m, const Mouse *mouse, uint32_t *handler);
/* An event script waiting in a game pass run outside main_frame (the boxes
 * that finish the main loop's iteration themselves, scr_main_rest): the
 * main screen runs the wait from its next frame on, as if it had started
 * in main_frame. status: PASS_WAIT_MESSAGES / PASS_YESNO; resume: where the
 * script goes on. */
UI_API void main_event_wait(MainUI *m, int status, uint32_t resume);
/* One frame of a pending wait (m->ev != 0) outside main_frame; 1 when the
 * script has finished. */
UI_API int  main_event_step(MainUI *m, const Mouse *ms);

/* the routines, for checks and other screens */
UI_API void ui_xor_delta(MainUI *m, uint32_t addr);                 /* $9B08 */
UI_API void ui_clear_rect(Screen *s, unsigned col, unsigned line,
                          unsigned words, unsigned lines);        /* $336E */
UI_API void ui_viewer_scene(MainUI *m);                            /* $4E28 */
UI_API void ui_viewer_tick(MainUI *m);                             /* $4ED8 */
UI_API void ui_viewer_step(MainUI *m);                             /* $4F0E */
UI_API void ui_planet_name(MainUI *m);                             /* $4C68 */
UI_API void ui_select_planet(MainUI *m, uint32_t index);           /* $4C36 */
UI_API void ui_mark_selected(MainUI *m);                           /* $4B4C */
UI_API void ui_list_click(MainUI *m, const Mouse *mouse);          /* $4B88 */
UI_API uint32_t ui_hotspots(MainUI *m, const Mouse *mouse);        /* $9A6C: handler or 0 */
UI_API void ui_draw_date(MainUI *m);                               /* $7806 */
UI_API void ui_panel_save(MainUI *m);                              /* $47DC */
UI_API void ui_panel_restore(MainUI *m);                           /* $4816 */
/* main-screen handlers; returns 1 if `handler` is one of them */
UI_API int  main_handler(MainUI *m, uint32_t handler);

#endif

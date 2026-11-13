/*
 * Screen: the game options menu ($559A, table $F76C), as the WHDLoad
 * version runs it. Checked frame by frame against the original
 * game running in an emulator. Addresses are the original's.
 *
 * The menu is drawn over the main screen's message panel: NEW GAME
 * ($3D14), LOAD ($520A), FORMAT ($54C6), SAVE ($53D6); the right button
 * leaves. The original reaches $559A from the main loop's hotspot call
 * ($9A6C pushes the handler and returns into it), and every sub-menu is a
 * handler called the same way from the loop above it, with a loop of its
 * own. Here each loop is a state; one frame() runs from one frame wait
 * ($2FFA) to the next, so a frame that finishes a sub-menu also runs the
 * rest of the loop it returns to.
 *
 *   ST_FLUSH    $5BE2: type out every queued message (no game pass)
 *   ST_MENU     $55CE: the options loop
 *   ST_NEWGAME  $3D38: "RESET AMIGA / RESTART GAME / EXIT"
 *   ST_SLOT     $51EC: "PICK 1,2,3 OR 4 TO SAVE/LOAD FILE AS" (four boxes)
 *   ST_CONFIRM  $5232 (load) / $5400 (save) / $54EA (format): EXECUTE / EXIT
 *
 * The WHDLoad patches are what this follows: the drive choice ($538E) and
 * the disk routines ($4250, $4294, $557A) do nothing, the disk checks
 * ($3D58, $3D7C) succeed, load and save ($3DC0, $3E0E) are files
 * SSG1..SSG4 of 7,000 bytes, FORMAT does nothing and reports success, and
 * the RESET AMIGA box is the slave's quit (its pointer at $F7B8).
 *
 * No keys are read anywhere in this menu (the slots are click boxes), so
 * ui_options_key() is a no-op, there for the platform's key routing.
 *
 * ---- what the platform wires ----
 *
 * File I/O: the core never touches files. ui_options_set_io() gives the
 * menu two callbacks; both return 0 on success, anything else on failure
 * (failure takes the original's "ERROR! PLEASE CHECK YOUR DISK." path):
 *   save(slot, blk, n, u): write the n (= 7,000) bytes to file SSG<slot+1>
 *                          in the save directory (slot 0-3), replacing it;
 *   load(slot, blk, n, u): read file SSG<slot+1> into blk; fail if it is
 *                          missing or shorter than n.
 * Without callbacks every load and save fails. The files are the WHDLoad
 * version's own (the bytes at $D440-$EF97), so saves move both ways.
 *
 * Quit: frame() returns UI_QUIT (102, defined here) when RESET AMIGA is
 * clicked in the NEW GAME menu; ui_frame() passes it through. The platform
 * ends the program (the slave returns to the OS there).
 *
 * RESTART GAME: game_restart (setup.h) with the main picture and the
 * panel static, as main_new_game_screen / main_new_game_done do for a new
 * game; frame() returns UI_OK with the main screen running.
 */
#include <string.h>

#include "ui.h"
#include "setup.h"



enum { ST_FLUSH, ST_MENU, ST_NEWGAME, ST_SLOT, ST_CONFIRM };
enum { OP_LOAD, OP_SAVE, OP_FORMAT };
enum { DONE, SUSPEND };            /* a handler returned / waits in a loop of its own */

static struct {
    int      st, op;
    uint32_t slot;                 /* $53D2: 0-3, $FF while the slot boxes wait */
    ui_options_save_fn save;
    ui_options_load_fn load;
    void    *user;
} o;

UI_API void ui_options_set_io(ui_options_save_fn save, ui_options_load_fn load, void *user)
{
    o.save = save;
    o.load = load;
    o.user = user;
}

/* ---- small routines ---- */

static void line(MainUI *m, uint32_t a) { ui_draw_text(m, 0x2A, 0xB1, a); }  /* $556E */
static void clear_line(MainUI *m) { line(m, 0x1DF3); }                          /* $556A */

/* a pixel's colour index on the interleaved screen, and setting one */
static unsigned pix(const uint8_t *b, int x, int y)
{
    unsigned v = 0;
    for (int p = 0; p < 5; p++)
        v |= (unsigned)((b[y * 200 + p * 40 + x / 8] >> (7 - x % 8)) & 1) << p;
    return v;
}
static void set_pix(uint8_t *b, int x, int y, unsigned v)
{
    for (int p = 0; p < 5; p++) {
        uint8_t *q = &b[y * 200 + p * 40 + x / 8], bit = (uint8_t)(0x80 >> (x % 8));
        if (v >> p & 1) *q |= bit; else *q &= (uint8_t)~bit;
    }
}

/* Port mode: paint the FORMAT button (x 206-261 with its shadow, y 120-136;
 * x 204-205 are LOAD's right edge and shadow) with the panel behind it,
 * after the menu is drawn: each column gets its pixel from line 119 above
 * the buttons (the gold and the shaded groove at x 233-235). */
static void menu_drawn(MainUI *m)
{
    if (!ui_port_mode()) return;
    uint8_t *b = m->s->bpl;
    for (int y = 120; y <= 136; y++)
        for (int x = 206; x <= 261; x++)
            set_pix(b, x, y, pix(b, x, 119));
}

static void table(MainUI *m, uint32_t t, uint16_t n)
{
    s32(m->g, 0xE3EC, t);
    s16(m->g, 0xE400, n);
}
static void options_table(MainUI *m) { table(m, 0xF76C, 4); }

/* $3006 / $301C: input lock on/off (the busy pointer image, $B0C, is not kept) */
static void busy_on(MainUI *m) { s8(m->g, 0xE42D, 0xFF); }
static void busy_off(MainUI *m) { s8(m->g, 0xE42D, 0); }

static void clear_panel(MainUI *m) { ui_clear_rect(m->s, 0x14, 0x5A, 8, 0x66); } /* $47C8 */

static const uint8_t *text_at(void *user, uint32_t a) { return ui_text((MainUI *)user, a); }

/* ---- the options loop ($55CE) after its hotspot call ---- */

static int menu_tail(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    ui_viewer_tick(m);                                         /* $4ED8 */
    if (!ms->rbutton) return UI_OK;                            /* $A9A */
    clear_panel(m);                                            /* $55E4 */
    ui_panel_restore(m);                                       /* $4816 */
    table(m, 0xF26C, 0x11);
    o.st = ST_MENU;
    m->screen = SCR_MAIN;
    /* $55FE rts: back into the main loop's iteration at $479E */
    uint32_t status = 0;
    int r = ui_game_pass(m, &status);                          /* $71E0 */
    if (r == UI_GAME_OVER) return r;
    if (r == UI_EVENT_WAIT) { if (next) *next = status; return r; }
    uint32_t arrow = 0;
    ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);   /* $4850 */
    ui_mark_selected(m);                                       /* $4B4C */
    ui_list_click(m, ms);                                      /* $4B88 */
    ui_message_pass(m->s, g, text_at, m);                      /* $4994 */
    if (!g8(g, 0xE429)) ui_message_type(m->s, g, text_at, m);  /* $49EA */
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    m->colour[31] = g16(g, 0xE3FE);
    return UI_OK;
}

/* ---- handlers called from the options loop ---- */

static void slot_wait(MainUI *m, int op)                       /* $51D0 up to its loop */
{
    table(m, 0xF7DC, 4);
    o.slot = 0xFF;
    o.op = op;
    o.st = ST_SLOT;
}

static void confirm_wait(MainUI *m)                            /* clr $E431, the EXECUTE/EXIT loop */
{
    s8(m->g, 0xE431, 0);
    o.st = ST_CONFIRM;
}

static int menu_handler(MainUI *m, uint32_t h)
{
    switch (h) {
    case 0x3D14:                                               /* NEW GAME */
        clear_line(m);
        line(m, 0x215E);
        table(m, 0xF7AC, 3);
        s8(m->g, 0xE431, 0);
        o.st = ST_NEWGAME;
        return SUSPEND;
    case 0x520A:                                               /* LOAD */
        line(m, 0x1D02);
        slot_wait(m, OP_LOAD);
        return SUSPEND;
    case 0x53D6:                                               /* SAVE */
        line(m, 0x1CE2);
        slot_wait(m, OP_SAVE);
        return SUSPEND;
    case 0x54C6:                                               /* FORMAT ($538E: nothing) */
        if (ui_port_mode()) return DONE;                       /* saves are files: no FORMAT */
        line(m, 0x1D7A);
        table(m, 0xF81C, 2);
        o.op = OP_FORMAT;
        confirm_wait(m);
        return SUSPEND;
    }
    return DONE;
}

/* EXECUTE: the disk work. DONE = back in the options loop; SUSPEND = the
 * EXECUTE/EXIT loop again (after an error). */
static int execute(MainUI *m)
{
    Game *g = m->g;
    if (o.op == OP_FORMAT) {                                   /* $5510 */
        line(m, 0x1D22);
        /* $557A, $3D58 (succeeds) */
        line(m, 0x213F);                                       /* $553C */
        options_table(m);
        return DONE;
    }
    if (o.op == OP_LOAD) {                                     /* $525A */
        line(m, 0x1D41);
        busy_on(m);
        /* $557A, $3D7C (succeeds), $4294, $4250; $3DC0 loads slot $53D2 */
        static uint8_t buf[STATE_SIZE];
        int bad = !o.load || o.load((int)(o.slot & 3), buf, STATE_SIZE, o.user) != 0;
        if (bad) {                                             /* $527A */
            clear_line(m);
            line(m, 0x1DD2);
            line(m, 0x1D9C);
            busy_off(m);
            confirm_wait(m);
            return SUSPEND;
        }
        memcpy(gp(g, STATE_BASE), buf, STATE_SIZE);
        ui_show_picture(m, 0x114A8);                           /* $52B4 */
        ui_viewer_scene(m);                                    /* $4E28 */
        ui_xor_delta(m, 0x3D614);                              /* the menu */
        menu_drawn(m);
        busy_off(m);
        line(m, 0x211F);
        options_table(m);                                      /* $52D8 */
        return DONE;
    }
    /* SAVE, $5428 */
    line(m, 0x1D5E);
    busy_on(m);
    /* $557A, $3D7C (succeeds), the slot's directory entry ('1234' at
     * $53CE, outside the block), $4250; $3E0E saves $D440, 7,000 bytes */
    int bad = !o.save || o.save((int)(o.slot & 3), gp(g, STATE_BASE), STATE_SIZE, o.user) != 0;
    busy_off(m);                                               /* $5486 */
    if (bad) {                                                 /* $5448 */
        clear_line(m);
        line(m, 0x1DD2);
        line(m, 0x1D9C);
        busy_off(m);
        confirm_wait(m);
        return SUSPEND;
    }
    clear_line(m);                                             /* $5490 */
    line(m, 0x2100);
    options_table(m);
    return DONE;
}

/* ------------------------------------------------------------------ */

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    (void)handler; (void)next;
    ui_sound(m, 13);                                           /* $559A */
    busy_on(m);                                                /* $5BE2: $3006 */
    s8(m->g, 0xE447, 0xFF);
    o.st = ST_FLUSH;
    return UI_OK;                                              /* at the wait $5BEC */
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    uint32_t h;
    switch (o.st) {
    case ST_FLUSH:                                             /* $5BF0 */
        ui_message_pass(m->s, g, text_at, m);                  /* $4994 */
        for (int k = 0; k < 3; k++)                            /* $49EA x 3 */
            if (!g8(g, 0xE429)) ui_message_type(m->s, g, text_at, m);
        ui_viewer_tick(m);                                     /* $4ED8 */
        {
            uint32_t arrow = 0;
            ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);
        }
        ui_mark_selected(m);                                   /* $4B4C */
        s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        if (g16(g, 0xE41A)) return UI_OK;                      /* messages still queued */
        s8(g, 0xE447, 0);                                      /* $5C34 */
        busy_off(m);
        if (g8(g, 0xE425) == 0) main_handler(m, 0x497E);
        else s8(g, 0xE448, 0);
        ui_panel_save(m);                                      /* $55AA: $47DC */
        clear_panel(m);                                        /* $47C8 */
        options_table(m);
        ui_xor_delta(m, 0x3D614);                              /* the menu */
        menu_drawn(m);
        o.st = ST_MENU;
        return UI_OK;                                          /* at the wait $55CE */

    case ST_MENU:                                              /* $55D2 */
        h = ui_hotspots(m, ms);
        if (h && menu_handler(m, h) == SUSPEND) return UI_OK;
        return menu_tail(m, ms, next);

    case ST_NEWGAME:                                           /* $3D3C */
        h = ui_hotspots(m, ms);
        if (h == 0x5BD4) {                                     /* RESTART GAME */
            /* $3CF4, drop the three loops' return addresses, $6AD6 .. $475E */
            main_new_game_screen(m);
            game_restart_as(g, g->booted_opponent);    /* the opponent booted with */
            main_new_game_done(m);
            o.st = ST_MENU;
            return UI_OK;
        }
        if (h == 0x3D0C) s8(g, 0xE431, 0xFF);                  /* EXIT */
        else if (h) return UI_QUIT;                            /* RESET AMIGA ($F7B8: the slave's quit) */
        ui_viewer_tick(m);
        if (!g8(g, 0xE431)) return UI_OK;
        clear_line(m);                                         /* $3D50 */
        options_table(m);                                      /* $52D8 */
        o.st = ST_MENU;
        return menu_tail(m, ms, next);

    case ST_SLOT:                                              /* $51F0 */
        h = ui_hotspots(m, ms);
        if (h >= 0x52EC && h <= 0x532E && (h - 0x52EC) % 0x16 == 0)
            o.slot = (h - 0x52EC) / 0x16;                      /* $53CE = $B5C + 16 x slot */
        ui_viewer_tick(m);
        if (o.slot == 0xFF) return UI_OK;
        /* $538E (nothing), $51C0 */
        line(m, 0x1DFE);
        line(m, 0x1D9C);
        table(m, 0xF81C, 2);
        confirm_wait(m);
        return UI_OK;

    case ST_CONFIRM:                                           /* $5236 / $5404 / $54EE */
        h = ui_hotspots(m, ms);
        if (h == 0x54B0) { s8(g, 0xE431, 1); clear_line(m); }  /* EXECUTE */
        else if (h == 0x54BC) s8(g, 0xE431, 2);                /* EXIT */
        ui_viewer_tick(m);
        if (g8(g, 0xE431) == 1) {
            if (execute(m) == SUSPEND) return UI_OK;
        } else if (g8(g, 0xE431) == 2) {                       /* $5558 */
            options_table(m);
            clear_line(m);
        } else {
            return UI_OK;
        }
        o.st = ST_MENU;
        return menu_tail(m, ms, next);
    }
    o.st = ST_MENU;
    return UI_OK;
}

const ScreenDef scr_options = {SCR_OPTIONS, 0x559A, enter, frame};

/* keys for this menu: none are read ($3D14-$55FE never looks at $A30) */
UI_API void ui_options_key(int ascii) { (void)ascii; }

/* ---- for tests (ctypes) ---- */
UI_API int ui_options_state(int *op, uint32_t *slot)
{
    if (op) *op = o.op;
    if (slot) *slot = o.slot;
    return o.st;
}

/*
 * Supremacy port - the Scorpion V4 event (script 8, the only one that
 * waits: a message wait, then the YES/NO box) starting in a game pass that
 * is not main_frame's own: the main-screen boxes that finish the main
 * loop's iteration themselves (starbase $3518, spy $5880, format $4F5C:
 * scr_main_rest) and the options panel $559A (drawn over the main screen,
 * so $E425 stays 0 and script 8's screen test passes). Before the fixes the
 * resume address was lost there (the event was dropped and fired again
 * later), and on the options panel a wait could be left armed with nothing
 * running it (busy for good: no pointer, no clicks).
 *
 * Usage: test_evwait <SupremacyData>
 * For each place and both answers: 2017, every planet the player's (so
 * RANDPLANET's planet passes the gate), $D46A clear; the screen is opened
 * and the dispatch counter $E426 is kept one before script 8's pass
 * ($38 + 2N + 8) until the script starts; the YES/NO box must show, the
 * answer is clicked, and afterwards the game must not be busy ($E42D = 0),
 * the main hotspot table $F26C must be back and $D46A set. Exit 0 = all
 * passed.
 */
#include <stdio.h>
#include <string.h>

#include "day.h"
#include "gamedata.h"
#include "setup.h"
#include "ui.h"

#define A(x) ((x) - STATE_BASE)
static uint32_t rd32(const uint8_t *b, uint32_t a)
{
    return (uint32_t)(b[A(a)] << 24 | b[A(a + 1)] << 16 | b[A(a + 2)] << 8 | b[A(a + 3)]);
}

static int one(const GameData *gd, uint32_t handler, const char *name, int yes, int leave)
{
    static Screen scr;
    static MainUI ui;
    memset(&scr, 0, sizeof scr);
    memset(&ui, 0, sizeof ui);
    Game *g = game_new();
    game_set_image(g, gd->mem);
    screen_init(&scr, gd->mem + FONT_ADDR);
    main_init(&ui, &scr, g, gd->mem);
    main_new_game_screen(&ui);
    game_new_game(g, 1, 0x4321u);
    main_new_game_done(&ui);
    uint8_t *b = game_block(g);
    Mouse idle = {200, 100, 0, 0};
    uint32_t h = 0;
    for (int k = 0; k < 400; k++) ui_frame(&ui, &idle, &h);       /* the start-up messages */
    b[A(0xE414)] = 2017 >> 8;
    b[A(0xE415)] = 2017 & 255;
    b[A(0xD46A)] = 0;
    uint32_t t = rd32(b, 0xE3B0);
    unsigned last = (unsigned)(b[A(0xE410)] << 8 | b[A(0xE411)]);
    for (unsigned i = 0; i < last; i++) {
        b[A(t + i * 0x3Au + 0x0C)] = 0;
        b[A(t + i * 0x3Au + 0x0D)] = 10;
        b[A(t + i * 0x3Au + 0x24)] = 3;
    }
    unsigned n = (unsigned)(b[A(0xE40E)] << 8 | b[A(0xE40F)]);
    uint8_t trigger = (uint8_t)(0x38 + 2 * n + 8 - 1);
    ui_goto(&ui, handler, &h);
    int armed_screen = -1;
    long p = 0, box = -1;
    /* keep script 8 next until it starts; leave = 1: a right click now and then
     * (format; the options panel runs no game pass until it is left), 2: click
     * the spy menu's ABORT */
    for (; p < 6000 && armed_screen < 0; p++) {
        if (!ui.ev) b[A(0xE426)] = trigger;
        Mouse ms = idle;
        if (leave == 1 && p % 300 > 290) ms.rbutton = 1;
        if (leave == 2 && p % 300 > 290) { ms.x = 200; ms.y = 179; ms.button = 1; }
        ui_frame(&ui, &ms, &h);
        if (ui.ev) armed_screen = ui.screen;
    }
    for (; p < 12000 && box < 0; p++) {                              /* the message wait, then the box */
        if (rd32(b, 0xE3EC) == 0xF74Cu && ui.ev == 2) box = p;
        else ui_frame(&ui, &idle, &h);
    }
    int ok = box >= 0;
    if (ok) {                                                        /* click the answer */
        Mouse a = {yes ? 208 : 232, 178, 1, 0};
        for (int k = 0; k < 4; k++) ui_frame(&ui, &idle, &h);   /* the button up first */
        for (int k = 0; k < 4; k++) ui_frame(&ui, &a, &h);
        for (long k = 0; k < 3000 && ui.ev; k++) ui_frame(&ui, &idle, &h);   /* NO: its wait, the flash */
        for (int k = 0; k < 200; k++) ui_frame(&ui, &idle, &h);
    }
    int busy = b[A(0xE42D)];
    uint32_t table = rd32(b, 0xE3EC);
    int flag = b[A(0xD46A)];
    int pass = ok && !ui.ev && !busy && flag == 0xFF && (table == 0xF26Cu || ui.screen != SCR_MAIN);
    printf("%-9s %s: wait started on screen %d at frame %ld; box %s; after: ev %d busy %d table $%X "
           "screen %d $D46A %02X -> %s\n", name, yes ? "YES" : "NO ", armed_screen, p, ok ? "shown" : "NOT SHOWN",
           ui.ev, busy, (unsigned)table, ui.screen, flag, pass ? "ok" : "FAIL");
    game_free(g);
    return pass;
}

int main(int argc, char **argv)
{
    static GameData gd;
    if (argc < 2 || gamedata_load(&gd, argv[1]) != 0) { fprintf(stderr, "data?\n"); return 2; }
    static const struct { uint32_t h; const char *name; int leave; } where[] = {
        {0x3518, "starbase", 1}, {0x5880, "spy", 2}, {0x4F5C, "format", 1}, {0x559A, "options", 1}};
    int bad = 0;
    for (int w = 0; w < 4; w++)
        for (int yes = 0; yes < 2; yes++)
            bad += !one(&gd, where[w].h, where[w].name, yes, where[w].leave);
    printf("RESULT %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

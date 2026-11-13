/*
 * Main-screen box 13: the spy report ($5880). Not a screen with a picture:
 * the handler runs its own loops on the main screen and then returns into
 * the main loop. Checked frame by frame against the original game
 * running in an emulator.
 *
 * State machine (each frame() = the code between two $2FFA waits):
 *   SP_DRAIN  $5BE2: the input stays locked ($E42D) and the message panel
 *             types three characters a frame ($4994, $49EA x 3, $4ED8,
 *             $4850, $4B4C, $E42C + 1) until the queue is empty; then the
 *             handler goes on (D_FIRST: dead/formatting planet or the price
 *             list; D_INFO: the menu; D_END: back to the main loop).
 *   SP_MENU   the loop at $58EA with table $F6FC (5 boxes, $584E-$5876 set
 *             $E433 = 1..5): $9A6C, $4850, $4B4C, $4ED8, one RNG call
 *             ($9DDE), $E42C + 1, until $E433 is set.
 * The last frame runs the rest of the main loop (scr_main_rest).
 *
 * scr_drain_frame() is used by scr_format.c too.
 */
#include "ui.h"
#include "day.h"

int  scr_main_rest(MainUI *m, const Mouse *ms, uint32_t *status);  /* scr_starbase.c */

enum { SP_DRAIN, SP_MENU };
enum { D_FIRST, D_INFO, D_END };
static int st, after;

static const uint8_t *text_at(void *user, uint32_t a) { return ui_text((MainUI *)user, a); }

/* $5BE2 up to its first wait */
void scr_drain_begin(MainUI *m)
{
    s8(m->g, 0xE42D, 0xFF);                                        /* $3006 */
    s8(m->g, 0xE447, 0xFF);
}

/* one frame of $5BE2's loop; 1 when the queue is empty (lock undone) */
int scr_drain_frame(MainUI *m)
{
    Game *g = m->g;
    ui_message_pass(m->s, g, text_at, m);                          /* $4994 */
    for (int k = 0; k < 3; k++)
        if (!g8(g, 0xE429)) ui_message_type(m->s, g, text_at, m);  /* $49EA */
    ui_viewer_tick(m);
    uint32_t arrow = 0;
    ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);
    ui_mark_selected(m);
    s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
    if (g16(g, G_MSG_COUNT)) return 0;
    s8(g, 0xE447, 0);
    s8(g, 0xE42D, 0);                                              /* $301C */
    s8(g, 0xE448, 0);
    if (!g8(g, 0xE425)) ui_draw_text(m, 0x4D, 0x67, 0x1CDE);       /* $497E: the prompt off */
    return 1;
}

static void drain(MainUI *m, int then)
{
    scr_drain_begin(m);
    st = SP_DRAIN;
    after = then;
}

static void report_end(MainUI *m)                                  /* $5A3A */
{
    post_message(m->g, 0x1642);
    s8(m->g, 0xD454, 0xFF);
    drain(m, D_END);
}

static int pay(Game *g, uint32_t cost)                             /* $5A52: 1 = paid */
{
    uint32_t sb = g32(g, G_PLANET_TABLE) + (uint32_t)(uint16_t)(g16(g, 0xE410) * PLANET_SIZE);
    if (cost > g32(g, sb + P_CREDITS)) {
        post_message(g, 0x1B64);                                   /* "YOU CAN'T AFFORD TO PAY YOUR SPY!" */
        return 0;
    }
    s32(g, sb + P_CREDITS, g32(g, sb + P_CREDITS) - cost);
    return 1;
}

static void report(MainUI *m, uint8_t c)                           /* $593C */
{
    Game *g = m->g;
    s8(g, 0xE42D, 0xFF);
    uint32_t p = g32(g, 0xE3DC);
    s16(g, 0xD5FC, g16(g, p + P_FOOD));
    s16(g, 0xD60A, g16(g, p + P_MINERALS));
    s16(g, 0xD618, g16(g, p + P_FUELS));
    s16(g, 0xD626, g16(g, p + P_ENERGY));
    s16(g, 0xD636, g16(g, p + P_POP));
    s16(g, 0xD64C, g16(g, p + 0x26));
    post_message(g, 0x1C66);                                       /* "SPY INFORMATION FOR : " */
    post_message(g, p + P_NAME);
    post_message(g, 0x1B62);
    if (c == 1 || c == 4) {
        if (!pay(g, 1000)) { report_end(m); return; }
        post_message(g, 0xD5F0);
        post_message(g, 0xD5FE);
        post_message(g, 0xD60C);
        post_message(g, 0xD61A);
    }
    if (c == 3 || c == 4) {
        if (!pay(g, 2200)) { report_end(m); return; }
        post_message(g, 0xD638);
    }
    if (c == 2 || c == 4) {
        if (!pay(g, 1520)) { report_end(m); return; }
        post_message(g, 0xD628);
    }
    report_end(m);
}

static int enter(MainUI *m, uint32_t handler, uint32_t *next)
{
    (void)handler; (void)next;
    ui_sound(m, 13);
    s8(m->g, 0xE42D, 0xFF);                                        /* $3006 */
    drain(m, D_FIRST);
    return UI_OK;
}

static int frame(MainUI *m, const Mouse *ms, uint32_t *next)
{
    Game *g = m->g;
    if (st == SP_MENU) {
        uint32_t h = ui_hotspots(m, ms);
        if (h >= 0x584E && h <= 0x5876 && (h - 0x584E) % 10 == 0)
            s8(g, 0xE433, (uint8_t)((h - 0x584E) / 10 + 1));
        uint32_t arrow = 0;
        ui_star_map(m->s, g, m->image ? m->image + 0x58E58 : NULL, 0, &arrow);
        ui_mark_selected(m);
        ui_viewer_tick(m);
        rng_next(g);
        s8(g, 0xE42C, (uint8_t)(g8(g, 0xE42C) + 1));
        uint8_t c = g8(g, 0xE433);
        if (!c) return UI_OK;
        s32(g, 0xE3EC, 0xF26C);
        s16(g, 0xE400, 0x11);
        if (c == 5) {
            post_message(g, 0x1639);                               /* "ABORTED" */
            return scr_main_rest(m, ms, next);
        }
        report(m, c);
        return UI_OK;
    }
    if (!scr_drain_frame(m)) return UI_OK;
    switch (after) {
    case D_FIRST: {
        uint16_t o = g16(g, g32(g, 0xE3DC) + P_OWNER);
        if (o == 4) { post_message(g, 0x1B25); report_end(m); return UI_OK; }   /* dead planet */
        if (o == 1) { post_message(g, 0x1B42); report_end(m); return UI_OK; }   /* formatting */
        post_message(g, 0x1B87);                                   /* the price list */
        drain(m, D_INFO);
        return UI_OK;
    }
    case D_INFO:
        s32(g, 0xE3EC, 0xF6FC);
        s16(g, 0xE400, 5);
        s8(g, 0xE433, 0);
        st = SP_MENU;
        return UI_OK;
    }
    s8(g, 0xE42D, 0);                                              /* $5A4C */
    return scr_main_rest(m, ms, next);
}

const ScreenDef scr_spy = {SCR_SPY, 0x5880, enter, frame};

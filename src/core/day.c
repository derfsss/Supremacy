/* One pass of game time and the job dispatcher. See day.h. */
#include <string.h>

#include "day.h"
#include "combat.h"
#include "craft.h"
#include "enemy.h"
#include "events.h"

#define BANNER_ADDR  0x1A79u
#define TENSION_TAB  0x302Au
#define TENSION_SND  0x2C60u

static uint16_t img16(const Game *g, uint32_t a)
{
    return g->image ? (uint16_t)(g->image[a] << 8 | g->image[a + 1]) : 0;
}

void game_set_image(Game *g, const uint8_t *image)
{
    g->image = image;
    memset(g->banner, 0xFF, sizeof g->banner);
    if (image) {
        for (unsigned k = 0; k < sizeof g->banner; k++) {
            g->banner[k] = image[BANNER_ADDR + k];
            if (g->banner[k] == 0xFF) break;
        }
        g->tension_sound = img16(g, TENSION_SND);
    }
    g->result = -1;
}

static void sound(Game *g, int n)
{
    if (g->play_sound) g->play_sound(g, n);
}

static unsigned screen(Game *g) { return g8(g, 0xE425); }

/* $2C00 (pass $A8): the alarm colour and sound from the number of enemy
 * planets, main screen only. */
void day_tension(Game *g)
{
    if (screen(g)) return;
    uint32_t r = g32(g, G_PLANET_TABLE);
    unsigned n = g16(g, 0xE410);
    uint8_t enemy = 0;
    for (unsigned k = 0; k <= n; k++, r += PLANET_SIZE)    /* $5A76 */
        if (g16(g, r + P_OWNER) == 7) enemy++;
    uint16_t d6 = enemy;
    uint8_t lv = g8(g, 0xE43B);
    if (lv == 0) d6 = (uint16_t)(d6 * 8);
    else if (lv == 1) d6 = (uint16_t)(d6 * 4);
    else d6 = (uint16_t)((d6 & 0xFE) * 2);
    /* Bug: when the enemy owns every planet the index is entry 16, one past
     * the 16-entry table, and reads the code that follows it (colour $C79,
     * sound $49). The WHDLoad slave (V1.1) only clamps the sound to $47 at
     * $2C3C, which stops the access fault but still starts sound 71, read
     * from past the end of the 50-entry sound table. The port keeps the
     * slave's clamp as the base behaviour and, with fix_bugs, uses the last
     * real entry (15: full red, no new sound) instead. */
    if (g->fix_bugs && d6 > 15 * 4) d6 = 15 * 4;
    uint32_t e = TENSION_TAB + (int16_t)d6;
    s16(g, 0xE3FE, img16(g, e));
    uint16_t snd = img16(g, e + 2);
    if (snd > 0x47) snd = 0x47;            /* the WHDLoad slave's clamp at $2C3C */
    if (snd != g->tension_sound) {
        g->tension_sound = snd;
        if (snd) sound(g, snd);
    }
    /* COLOR31 = $E3FE is the platform's (the alarm colour) */
}

/* $5BBE (pass $AA): hide every banner character. */
void day_banner_hide(Game *g)
{
    for (unsigned k = 0; k < sizeof g->banner && g->banner[k] != 0xFF; k++)
        g->banner[k] |= 0x80;
}

/* $96AC */
void day_banner_clear(Game *g)
{
    s8(g, 0xE43D, 0);
    s8(g, 0xE43F, g8(g, 0xE43E));
}

/* $95CC (passes $AB-$C0): reveal one hidden character, chosen by the RNG,
 * while a message has arrived and a screen other than the main one is up. */
void day_banner_reveal(Game *g)
{
    s8(g, 0xE43E, g8(g, 0xE43F));
    if (!g8(g, 0xE43D) || !screen(g)) { day_banner_clear(g); return; }
    s8(g, 0xE43F, 0);
    unsigned k;
    for (;;) {
        k = rng_next(g) & 0x1F;
        if (k >= 0x16) continue;
        if (g->banner[k] & 0x80) break;
    }
    g->banner[k] &= 0x7F;
    if (g->banner_hook) g->banner_hook(g, (int)k);
    s8(g, 0xE43F, g8(g, 0xE43E));
}

/* $9644 (passes $C1-$F7): the banner blinks (drawn on even passes, blanked
 * on odd ones); state: only the typewriter flags. */
void day_banner_blink(Game *g)
{
    s8(g, 0xE43E, g8(g, 0xE43F));
    if (!g8(g, 0xE43D) || !screen(g)) { day_banner_clear(g); return; }
    s8(g, 0xE43F, 0);
    if (g->banner_hook) g->banner_hook(g, -1);
    s8(g, 0xE43F, g8(g, 0xE43E));
}

/* $77E8 (pass $FF). The date redraw $7806 that follows is display. */
void day_advance(Game *g)
{
    uint16_t d = (uint16_t)(g16(g, G_DAY) + 1);
    if (d == 0x41) {
        d = 1;
        s16(g, G_YEAR, (uint16_t)(g16(g, G_YEAR) + 1));
    }
    s16(g, G_DAY, d);
}

void game_screen_loop(Game *g)
{
    if (screen(g) == 6) rng_next(g);
}

static int event_status(int st)
{
    if (st == EV_WAIT_MESSAGES) return PASS_WAIT_MESSAGES;
    if (st == EV_YESNO) return PASS_YESNO;
    return PASS_OK;
}

/* $74B2: the compares are cmp.b against the low bytes of $38+N, $38+2N,
 * $56+2N (N = $E40E). */
static int dispatch(Game *g, uint8_t c, uint32_t *resume)
{
    uint16_t n = g16(g, 0xE40E);
    if (c < 0x38) { platoon_pass(g, c); return PASS_OK; }
    if (c < (uint8_t)(0x38 + n)) {
        planet_economy(g, g32(g, G_PLANET_TABLE) + (uint32_t)(c - 0x38) * PLANET_SIZE);
        return PASS_OK;
    }
    if (c < (uint8_t)(0x38 + 2 * n)) { ground_forces(g, c); return PASS_OK; }
    if (c < (uint8_t)(0x56 + 2 * n))
        return event_status(events_pass(g, c, (int)screen(g), resume));
    switch (c) {
    case 0xA7: case 0xFC: case 0xFD:       /* troops-screen redraws: display */
        return PASS_OK;
    case 0xA8: day_tension(g); return PASS_OK;
    case 0xA9: enemy_base_growth(g); return PASS_OK;
    case 0xAA: day_banner_hide(g); return PASS_OK;
    case 0xF8: day_banner_clear(g); return PASS_OK;
    case 0xF9:                             /* $245E: war-update scene */
        if (g8(g, 0xE449) && screen(g) == 7 && g32(g, 0xE3F0) == 0 && g->war_scene_hook)
            g->war_scene_hook(g);
        return PASS_OK;
    case 0xFA: {
        int r = game_win_lose(g);
        if (r >= 0) { g->result = r; return PASS_GAME_OVER; }
        return PASS_OK;
    }
    case 0xFB: enemy_attack(g); return PASS_OK;
    case 0xFE: enemy_colonise(g, (int)screen(g)); return PASS_OK;
    case 0xFF: day_advance(g); return PASS_OK;
    }
    if (c < 0xAB) return PASS_OK;
    if (c < 0xC1) { day_banner_reveal(g); return PASS_OK; }
    if (c < 0xF8) { day_banner_blink(g); return PASS_OK; }
    return PASS_OK;
}

int game_pass(Game *g, uint32_t *resume)
{
    if (g16(g, 0xE408)) {                  /* explosion requested (comet script) */
        sound(g, 0x1E);
        s16(g, 0xE408, 0);
    }
    uint8_t c = (uint8_t)(g8(g, 0xE426) + 1);
    s8(g, 0xE426, c);
    if (c < 0x20) { craft_bay_pass(g, c); return PASS_OK; }
    return dispatch(g, c, resume);
}

int game_event_resume(Game *g, uint32_t pc, uint32_t *resume)
{
    return event_status(events_resume(g, pc, (int)screen(g), resume));
}

uint8_t *game_banner(Game *g) { return g->banner; }
uint16_t game_get_tension_sound(Game *g) { return g->tension_sound; }
void     game_set_tension_sound(Game *g, uint16_t v) { g->tension_sound = v; }
int      game_get_result(Game *g) { return g->result; }

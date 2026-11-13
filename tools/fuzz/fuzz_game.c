/*
 * Supremacy port - random-input soak test (no window, no SDL).
 *
 * Usage: fuzz_game <SupremacyData> <out dir> [games] [passes per game] [seed]
 *
 * Plays whole games the way the front end does (src/platform/main.c's
 * run_game: ui_frame once per game pass, keys through ui_key, saves through
 * the options I/O) with random input: clicks mostly inside the current
 * hotspot table (so every screen gets visited), random clicks, right
 * clicks, and keys (RETURN / ESC / DEL often, letters and digits). Saves go
 * to four slots in memory, so LOAD and SAVE are exercised too. The sound
 * player gets every sound the game plays and is rendered as it goes.
 *
 * It reports, and writes each game as a session log (the exe's --replay
 * format) so a finding can be replayed in the real program:
 *   - FREEZE-BUSY: $E42D (busy: no pointer, no clicks) still set 3,000
 *     passes after every way out was tried (a click, a right click, RETURN
 *     and ESC all sent since it began; prompts like RENAME legitimately
 *     wait for a key);
 *   - FREEZE-TIME: the date unchanged, likewise 3,000 passes after all of
 *     those (and at least 20,000 passes in all);
 *   - anything the sanitizers catch (build with -fsanitize=address,undefined).
 *   - YESNO-LOST: the Scorpion V4 box's hotspots ($F74C) active while the
 *     main screen is not running the box (the game would take no clicks).
 * It also counts the screens visited and the results ui_frame returned.
 * FUZZ_SCORPION=1 (environment): each game starts in 2017 with the player
 * owning planets 1-3, so script 8 (the Scorpion V4 YES/NO) can fire.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "day.h"
#include "gamedata.h"
#include "paula.h"
#include "setup.h"
#include "ui.h"

static uint32_t rng = 1;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static int rr(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }

/* ---- saves in memory ---- */
static uint8_t slots[4][STATE_SIZE];
static int slot_used[4];
static int save_fn(int slot, const uint8_t *blk, size_t n, void *u)
{
    (void)u;
    if (slot < 0 || slot > 3 || n != STATE_SIZE) return -1;
    memcpy(slots[slot], blk, n);
    slot_used[slot] = 1;
    return 0;
}
static int load_fn(int slot, uint8_t *blk, size_t n, void *u)
{
    (void)u;
    if (slot < 0 || slot > 3 || n != STATE_SIZE || !slot_used[slot]) return -1;
    memcpy(blk, slots[slot], n);
    return 0;
}

/* ---- sound ---- */
static SoundOut sound;
static int sound_ok;
static void game_sound(Game *g, int n)
{
    (void)g;
    if (sound_ok) snd_play(&sound, n);
}

static const char *SCREEN[] = {"main", "troops", "cargo", "statistics", "fleet", "shipyard",
                               "surface", "combat", "spy", "options", "format", "starbase"};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: fuzz_game <SupremacyData> <out dir> [games] [passes] [seed]\n");
        return 2;
    }
    int games = argc > 3 ? atoi(argv[3]) : 4;
    long passes_max = argc > 4 ? atol(argv[4]) : 200000;
    rng = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 12345u;
    if (!rng) rng = 1;
    static GameData gd;
    if (gamedata_load(&gd, argv[1]) != 0) {
        fprintf(stderr, "data: %s\n", gd.error);
        return 2;
    }
    sound_ok = snd_init(&sound, gd.mem + 0x60000u, 0x10CA0u, 0x60000u, 22050) == 0;
    if (sound_ok) snd_set_image(&sound, gd.mem, AMIGA_MEM_SIZE);
    ui_options_set_io(save_fn, load_fn, NULL);
    ui_set_port_mode(1);
    int findings = 0;
    long seen[16] = {0}, results[128] = {0};
    for (int gi = 0; gi < games; gi++) {
        int opponent = rr(4);
        uint32_t seed = rnd();
        char path[1024];
        snprintf(path, sizeof path, "%s/fuzz_%08x_%d.log", argv[2], seed, gi);
        FILE *log = fopen(path, "w");
        if (log) fprintf(log, "# fuzz_game session\ngame %d %08X\n", opponent, (unsigned)seed);
        static Screen scr;
        static MainUI ui;
        memset(&scr, 0, sizeof scr);
        memset(&ui, 0, sizeof ui);
        Game *g = game_new();
        g->play_sound = game_sound;
        game_set_image(g, gd.mem);
        screen_init(&scr, gd.mem + FONT_ADDR);
        main_init(&ui, &scr, g, gd.mem);
        main_new_game_screen(&ui);
        game_new_game(g, opponent, seed);
        main_new_game_done(&ui);
        uint8_t *blk = game_block(g);
#define B8(a) blk[(a) - STATE_BASE]
#define B16(a) (unsigned)(blk[(a) - STATE_BASE] << 8 | blk[(a) - STATE_BASE + 1])
        if (getenv("FUZZ_SCORPION")) {             /* 2017, planets 1-3 the player's */
            blk[0xE414 - STATE_BASE] = 2017 >> 8;
            blk[0xE415 - STATE_BASE] = 2017 & 255;
            uint32_t t = (uint32_t)B16(0xE3B0) << 16 | B16(0xE3B2);
            for (uint32_t i = 1; i <= 3; i++) {
                uint32_t r = t + i * 0x3Au - STATE_BASE;
                blk[r + 0x0C] = 0; blk[r + 0x0D] = 10;          /* owner: the player */
                blk[r + 0x24] = (uint8_t)(2 + i);               /* a planet type */
            }
            if (log) fprintf(log, "# FUZZ_SCORPION: the replay needs the same pokes\n");
        }
        long yesno_boxes = 0;
        Mouse ms = {160, 100, 0, 0}, last = {-1, -1, 0, 0};
        int hold = 0, wait = 0, reported_busy = 0, reported_time = 0;
        int tried_b = 0, tried_t = 0;           /* ways out tried: 1 click 2 right 4 RETURN 8 ESC */
        long busy_run = 0, busy_after = 0, time_after = 0;
        long same_date = 0;
        unsigned date = B16(0xE412) | B16(0xE414) << 16;
        long p;
        const char *end = "passes";
        for (p = 0; p < passes_max; p++) {
            /* ---- the input of this pass ---- */
            if (hold > 0) {
                if (--hold == 0) { ms.button = 0; ms.rbutton = 0; wait = 1 + rr(40); }
            } else if (wait > 0) {
                wait--;
            } else {
                int a = rr(100);
                if (a < 62) {                               /* a hotspot of this screen */
                    uint32_t t = ((uint32_t)B16(0xE3EC) << 16 | B16(0xE3EE));
                    unsigned n = B16(0xE400);
                    if (n && n < 64) {
                        uint32_t e = t + 16u * (uint32_t)rr((int)n);
                        /* QUIT GAME ($FC02D2, the options' NEW GAME menu) and RESTART
                         * ($5BD4) end the game: rarely, so games run long */
                        for (int tries = 0; tries < 8; tries++) {
                            uint32_t hd = ui_m32(&ui, e + 12);
                            if (!((hd == 0xFC02D2u && rr(100) >= 2) || (hd == 0x5BD4u && rr(100) >= 10)))
                                break;
                            e = t + 16u * (uint32_t)rr((int)n);
                        }
                        int x0 = ui_m16(&ui, e), y0 = ui_m16(&ui, e + 2);
                        int x1 = ui_m16(&ui, e + 4), y1 = ui_m16(&ui, e + 6);
                        if (x1 >= x0 && y1 >= y0) {
                            ms.x = (int16_t)(x0 + rr(x1 - x0 + 1));
                            ms.y = (int16_t)(y0 + rr(y1 - y0 + 1));
                        }
                    }
                    ms.button = 1;
                    tried_b |= 1;
                    tried_t |= 1;
                    hold = 2 + rr(5);
                } else if (a < 78) {                        /* anywhere */
                    ms.x = (int16_t)rr(318);
                    ms.y = (int16_t)(1 + rr(197));
                    ms.button = 1;
                    hold = 2 + rr(5);
                } else if (a < 83) {                        /* right click: leave a screen */
                    ms.rbutton = 1;
                    tried_b |= 2;
                    tried_t |= 2;
                    hold = 2 + rr(4);
                } else if (a < 97) {                        /* a key */
                    static const int keys[] = {13, 13, 13, 27, 27, 8, 8, ' ', 'A', 'E', 'N', 'Y',
                                               'S', 'Z', '1', '5', '9'};
                    int k = keys[rr((int)(sizeof keys / sizeof keys[0]))];
                    if (k == 'A') k = 'A' + rr(26);
                    if (log) fprintf(log, "K %ld %d\n", p, k);
                    ui_key(&ui, k);
                    int bit = k == 13 ? 4 : k == 27 ? 8 : 0;
                    tried_b |= bit;
                    tried_t |= bit;
                    wait = 1 + rr(20);
                } else {                                    /* let time pass */
                    wait = 50 + rr(400);
                }
                /* the options' NEW GAME menu ($F7AC): QUIT is entry 0 (and any
                 * click there that is not EXIT quits): mostly click EXIT instead */
                if (ms.button && ((uint32_t)B16(0xE3EC) << 16 | B16(0xE3EE)) == 0xF7ACu && rr(100) >= 2) {
                    int x0 = ui_m16(&ui, 0xF7ACu), y0 = ui_m16(&ui, 0xF7AEu);
                    int x1 = ui_m16(&ui, 0xF7B0u), y1 = ui_m16(&ui, 0xF7B2u);
                    if (ms.x >= x0 && ms.x <= x1 && ms.y >= y0 && ms.y <= y1) {
                        ms.x = (int16_t)ui_m16(&ui, 0xF7CCu);      /* entry 2: EXIT */
                        ms.y = (int16_t)ui_m16(&ui, 0xF7CEu);
                    }
                }
                if (ms.x < 0) ms.x = 0;
                if (ms.x > 317) ms.x = 317;
                if (ms.y < 1) ms.y = 1;
                if (ms.y > 197) ms.y = 197;
            }
            if (log && (ms.x != last.x || ms.y != last.y || ms.button != last.button ||
                        ms.rbutton != last.rbutton)) {
                fprintf(log, "M %ld %d %d %d %d\n", p, ms.x, ms.y, ms.button, ms.rbutton);
                last = ms;
            }
            /* ---- one game pass ---- */
            sound.hold = B8(0xE42B) != 0;
            uint32_t h = 0;
            int r = ui_frame(&ui, &ms, &h);
            if (r >= 0 && r < 128) results[r]++;
            if (ui.screen >= 0 && ui.screen < 16) seen[ui.screen]++;
            if (sound_ok && p % 8 == 0) {
                static int16_t pcm[2 * 64];
                snd_render(&sound, pcm, 64);
            }
            if (r == UI_QUIT) { end = "quit"; break; }      /* the next game starts */
            if (r == MAIN_GAME_OVER) { end = g->result == 0xFF ? "lost" : "won"; break; }
            if (r == MAIN_HANDLER) {
                printf("game %d pass %ld: handler $%X not ported (screen %s)\n", gi, p, (unsigned)h,
                       SCREEN[ui.screen % 12]);
                findings++;
            }
            /* ---- the YES/NO box: hotspots $F74C only while the box runs ---- */
            if (((uint32_t)B16(0xE3EC) << 16 | B16(0xE3EE)) == 0xF74Cu) {
                if (ui.ev < 2 || ui.ev > 4) {             /* mainscreen.c: EVW_BOX, NO_DRAIN, FLASH */
                    printf("YESNO-LOST game %d (opp %d seed %08X) pass %ld: YES/NO hotspots with no box "
                           "(screen %s, ev %d)\n", gi, opponent, (unsigned)seed, p, SCREEN[ui.screen % 12], ui.ev);
                    findings++;
                    end = "yesno-lost";
                    break;
                }
                yesno_boxes++;
            }
            /* ---- freeze checks ---- */
            if (B8(0xE42D)) busy_run++;
            else { busy_run = 0; tried_b = 0; busy_after = 0; reported_busy = 0; }
            if (busy_run && tried_b == 15) busy_after++;
            if (busy_after == 3000 && !reported_busy) {
                printf("FREEZE-BUSY game %d (opp %d seed %08X) pass %ld: busy $E42D for %ld passes, "
                       "every way out tried, screen %s, ev %d, pause %d\n", gi, opponent,
                       (unsigned)seed, p, busy_run, SCREEN[ui.screen % 12], ui.ev, ui.pause);
                reported_busy = 1;
                findings++;
            }
            unsigned d = B16(0xE412) | B16(0xE414) << 16;
            if (d == date) same_date++;
            else { same_date = 0; date = d; reported_time = 0; tried_t = 0; time_after = 0; }
            if (tried_t == 15) time_after++;
            if (same_date >= 20000 && time_after >= 3000 && !reported_time) {
                printf("FREEZE-TIME game %d (opp %d seed %08X) pass %ld: date %u/%u unchanged for "
                       "%ld passes, screen %s, busy %d, ev %d, pause %d, $E44F %02X, $E429 %02X\n",
                       gi, opponent, (unsigned)seed, p, B16(0xE412), B16(0xE414), same_date,
                       SCREEN[ui.screen % 12], B8(0xE42D), ui.ev, ui.pause, B8(0xE44F), B8(0xE429));
                reported_time = 1;
                findings++;
            }
        }
        printf("game %d: opponent %d seed %08X: %ld passes, end %s, date %u/%u, YES/NO box passes %ld, "
               "log %s\n", gi, opponent, (unsigned)seed, p, end, B16(0xE412), B16(0xE414), yesno_boxes, path);
        fflush(stdout);
        if (log) fclose(log);
        game_free(g);
    }
    printf("screens (passes):");
    for (int k = 0; k < 12; k++) printf(" %s %ld", SCREEN[k], seen[k]);
    printf("\nui_frame results:");
    for (int k = 0; k < 128; k++) if (results[k]) printf(" %d:%ld", k, results[k]);
    printf("\nfindings: %d\n", findings);
    return findings ? 1 : 0;
}

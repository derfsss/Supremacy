/*
 * Supremacy port - SDL3 front end.
 *
 * Usage: supremacy <SupremacyData directory> [options]
 *   (no arguments at all, e.g. started from its icon: the launcher window
 *   with these settings and a PLAY GAME button; kept in supremacy.cfg)
 *   --opponent N   0-3: skip the intro and play against this opponent
 *   --seed S       game seed (skips the intro; default with --opponent: from the clock)
 *   --no-intro     start the game at once (opponent 0 unless --opponent)
 *   --speed P      passes per second (default 47, the original's measured rate)
 *   --pictures     only show the full-screen pictures (LEFT/RIGHT, UP/DOWN sounds)
 *   --scale N      window size: N x 320 by N x 200 (default 3)
 *   --fullscreen   start full screen (F11 or Alt+Enter switches at any time;
 *                  the picture is scaled by whole numbers only, with black borders)
 *   --log FILE     the session log (default supremacy.log next to the program)
 *   --replay FILE  play a session log again (skips the intro), then play on
 *   --ending N     only play an end sequence: 0-3 victory over that opponent, -1 defeat
 *
 * The game core and screens draw into a copy of the original's screen
 * buffer, which is shown each frame. Without options it starts like the
 * original: the intro (INT1/INT2: title and theme, a click or key for the
 * opponent selection, CHALLENGE starts the game with the intro's seed).
 * When the game is won or lost the end sequence (WIN0-3 / WIN_) plays;
 * after "THE END" a click or key quits. Close the window to quit.
 * Environment variables for tests: SUP_AUTOQUIT_FRAMES=n (quit after n
 * frames), SUP_DUMP_DIR=dir (write the last frame there as game.ppm).
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gamedata.h"
#include "paula.h"
#include "endmusic.h"
#include "ui.h"
#include "setup.h"
#include "day.h"
#include "events.h"
#include "ending.h"
#include "intro.h"
#include "launcher.h"
#include "os4_dialog.h"
#include "build_id.h"           /* SUP_VERSION, SUP_BUILD_ID, SUP_BUILD_DATE (cmake/build_id.cmake) */

/* The version banner (AmigaOS Version command; release staging checks the
 * commit in it). */
__attribute__((used)) static const char sup_banner[] =
    "$VER: Supremacy " SUP_VERSION " (" SUP_BUILD_DATE ") build " SUP_BUILD_ID;

/* an error for a player who started from the icon (no console to read) */
static void show_error(SDL_Window *win, const char *text)
{
#ifdef __amigaos4__
    (void)win;
    os4_message("Supremacy", text);         /* Intuition, not SDL's message box */
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Supremacy", text, win);
#endif
}

/* Sound: the sequencer region $60000-$70C9F of the image (from GAM2). */
#define SOUND_BASE 0x60000u
#define SOUND_LEN  0x10CA0u
#define AUDIO_RATE 44100

static SoundOut sound;
static EndSound music;                 /* intro / end-sequence music (endmusic.c) */
static int music_on;
static SDL_Mutex *sound_lock;

static void SDLCALL audio_feed(void *user, SDL_AudioStream *st, int additional, int total)
{
    int16_t buf[2048];
    (void)user; (void)total;
    while (additional > 0) {
        int frames = additional / 4;
        if (frames > 1024) frames = 1024;
        if (frames <= 0) break;
        SDL_LockMutex(sound_lock);
        if (music_on) endsnd_render(&music, buf, frames);
        else snd_render(&sound, buf, frames);
        SDL_UnlockMutex(sound_lock);
        SDL_PutAudioStreamData(st, buf, frames * 4);
        additional -= frames * 4;
    }
}

static void play_sound(int n)
{
    SDL_LockMutex(sound_lock);
    snd_play(&sound, n);
    SDL_UnlockMutex(sound_lock);
}

static void game_sound(Game *g, int n) { (void)g; play_sound(n); }

/* ---- drawing ---- */
static uint32_t rgb12(uint16_t c)
{
    unsigned r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return 0xFF000000u | (r * 17u) << 16 | (g * 17u) << 8 | (b * 17u);
}

/* A 16-pixel hardware sprite: control long, then (plane A, plane B) words
 * per line; colours 28 + value (sprites 6 and 7). */
static void draw_sprite(uint32_t *px, const uint16_t *colour, const uint8_t *mem,
                        uint32_t addr, int x, int y, int h)
{
    for (int r = 0; r < h; r++) {
        const uint8_t *d = mem + addr + 4 + r * 4;
        unsigned a = (unsigned)(d[0] << 8 | d[1]), b = (unsigned)(d[2] << 8 | d[3]);
        for (int k = 0; k < 16; k++) {
            unsigned v = ((a >> (15 - k)) & 1) | (((b >> (15 - k)) & 1) << 1);
            int xx = x + k, yy = y + r;
            if (v && xx >= 0 && xx < 320 && yy >= 0 && yy < 200)
                px[yy * 320 + xx] = rgb12(colour[28 + v]);
        }
    }
}

/* The picture through the copper list: colour moves after a WAIT change the
 * palette from that line (display line $2C = picture row 0); the shipyard's
 * lists also patch colours 1-8 at line $10 with the values the CPU set
 * (MainUI.colour already holds them). copper 0 = the plain list $AD0. */
static void render_copper(uint32_t *px, const uint8_t *idx, const uint16_t *colour,
                          uint32_t copper, const uint8_t *mem)
{
    uint16_t pal[32];
    memcpy(pal, colour, sizeof pal);
    uint32_t pc = copper;
    int wait = copper ? -1 : 1000;                   /* first instruction pending */
    for (int y = 0; y < 200; y++) {
        int line = y + 0x2C;
        while (copper && wait < 1000) {
            if (wait >= 0 && line < wait) break;
            wait = -1;
            unsigned a = (unsigned)(mem[pc] << 8 | mem[pc + 1]);
            unsigned b = (unsigned)(mem[pc + 2] << 8 | mem[pc + 3]);
            pc += 4;
            if (a == 0xFFFF && b == 0xFFFE) { wait = 1000; break; }
            if (a & 1) { wait = (int)(a >> 8); continue; }          /* WAIT */
            if (a >= 0x182 && a <= 0x1BE && line <= 0x2C) continue;  /* colours 1-8 at $10: the CPU's */
            if (a >= 0x180 && a <= 0x1BE) pal[(a - 0x180) >> 1] = (uint16_t)b;
        }
        for (int x = 0; x < 320; x++) px[y * 320 + x] = rgb12(pal[idx[y * 320 + x]]);
    }
}

typedef struct App {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
} App;

/* The window as the game uses it (after the launcher, which set its own size). */
static void app_game_mode(App *a, int scale, int fullscreen)
{
    if (scale < 1) scale = 1;
    SDL_SetWindowSize(a->win, 320 * scale, 200 * scale);
    SDL_SetWindowPosition(a->win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetRenderLogicalPresentation(a->ren, 320, 200, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetWindowFullscreen(a->win, fullscreen != 0);
}

static int app_open(App *a, int scale, int fullscreen)
{
    if (scale < 1) scale = 1;
    /* Windows: the window's icon is the exe's icon resource 1 (supremacy.rc) */
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    SDL_WindowFlags fl = SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    if (!SDL_CreateWindowAndRenderer("Supremacy", 320 * scale, 200 * scale, fl, &a->win, &a->ren)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderLogicalPresentation(a->ren, 320, 200, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    SDL_SetTextureScaleMode(a->tex, SDL_SCALEMODE_NEAREST);
    return 0;
}

/* F11 or Alt+Enter: full screen on/off (the picture keeps its shape, with
 * black borders). Returns 1 if the event was that key (the game must not see it). */
static int app_event(App *a, const SDL_Event *ev)
{
    if (ev->type != SDL_EVENT_KEY_DOWN && ev->type != SDL_EVENT_KEY_UP) return 0;
    SDL_Keycode k = ev->key.key;
    int toggle = k == SDLK_F11 || ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (ev->key.mod & SDL_KMOD_ALT));
    if (!toggle) return 0;
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat)
        SDL_SetWindowFullscreen(a->win, !(SDL_GetWindowFlags(a->win) & SDL_WINDOW_FULLSCREEN));
    return 1;
}

static void app_present(App *a, const uint32_t *px)
{
    SDL_UpdateTexture(a->tex, NULL, px, 320 * 4);
    SDL_RenderClear(a->ren);
    SDL_RenderTexture(a->ren, a->tex, NULL, NULL);
    SDL_RenderPresent(a->ren);
}

static void dump_ppm(const char *dir, const char *name, const uint32_t *px)
{
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (int k = 0; k < 320 * 200; k++) {
        fputc((px[k] >> 16) & 255, f); fputc((px[k] >> 8) & 255, f); fputc(px[k] & 255, f);
    }
    fclose(f);
}

/* ---- picture viewer (--pictures) ---- */
static const struct { uint32_t addr; const char *name; } screens[] = {
    {PIC_MAIN, "main"}, {PIC_STATISTICS, "statistics"}, {PIC_SHIPYARD, "shipyard"},
    {PIC_FLEET, "fleet"}, {PIC_TROOPS, "troops"}, {PIC_CARGO, "cargo"},
    {PIC_SURFACE, "surface"}, {PIC_COMBAT, "combat"}, {PIC_TITLE, "title"},
};
#define NSCREENS ((int)(sizeof screens / sizeof screens[0]))

static void picture_pixels(const GameData *gd, int i, uint32_t *px)
{
    static Picture pic;
    if (picture_decode(gd, screens[i].addr, &pic) != 0) return;
    for (int k = 0; k < 320 * 200; k++) px[k] = 0xFF000000u | pic.palette[pic.pixels[k] & 31];
}

static int run_pictures(App *a, const GameData *gd)
{
    static uint32_t px[320 * 200];
    const char *dump = SDL_getenv("SUP_DUMP_DIR");
    if (dump)
        for (int i = 0; i < NSCREENS; i++) { picture_pixels(gd, i, px); dump_ppm(dump, screens[i].name, px); }
    int cur = 0, sfx = 1, frames = 0, running = 1;
    const char *aq = SDL_getenv("SUP_AUTOQUIT_FRAMES");
    int limit = aq ? SDL_atoi(aq) : 0;
    picture_pixels(gd, cur, px);
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (app_event(a, &ev)) continue;
            if (ev.type == SDL_EVENT_QUIT) running = 0;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = ev.key.key;
                if (k == SDLK_ESCAPE) running = 0;
                if (k == SDLK_RIGHT) cur = (cur + 1) % NSCREENS;
                if (k == SDLK_LEFT) cur = (cur + NSCREENS - 1) % NSCREENS;
                if (k == SDLK_UP) { sfx = (sfx + 1) % 50; play_sound(sfx); }
                if (k == SDLK_DOWN) { sfx = (sfx + 49) % 50; play_sound(sfx); }
                if (k == SDLK_SPACE) play_sound(sfx);
                picture_pixels(gd, cur, px);
            }
        }
        app_present(a, px);
        SDL_Delay(20);
        if (limit && ++frames >= limit) running = 0;
    }
    return 0;
}

/* ---- saved games: SSG1-SSG4 (the WHDLoad slave's files, 7,000 bytes each) in
 * the folder "save" next to the program ---- */
static char save_dir[1024];

static int save_slot(int slot, const uint8_t *blk, size_t n, void *u)
{
    (void)u;
    char path[1024];
    SDL_CreateDirectory(save_dir);
    SDL_snprintf(path, sizeof path, "%s/SSG%d", save_dir, slot + 1);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(blk, 1, n, f);
    int ok = fclose(f) == 0 && w == n;
    SDL_Log("save to %s: %s", path, ok ? "ok" : "FAILED");
    return ok ? 0 : -1;
}

static int load_slot_file(int slot, uint8_t *blk, size_t n, void *u)
{
    (void)u;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/SSG%d", save_dir, slot + 1);
    FILE *f = fopen(path, "rb");
    if (!f) { SDL_Log("load from %s: no file", path); return -1; }
    size_t r = fread(blk, 1, n, f);
    fclose(f);
    SDL_Log("load from %s: %s", path, r == n ? "ok" : "FAILED (short file)");
    return r == n ? 0 : -1;
}

/* ---- the session log and replay ----
 * Every run writes a log (default supremacy.log next to the program, or
 * supremacy-replay.log when replaying): a "game <opponent> <seed>" line,
 * then one line per input change, numbered by game pass:
 *   M <pass> <x> <y> <left> <right>   the mouse from that pass on
 *   K <pass> <code>                   a key given to the game before that pass
 * and "#" lines for what happened (screens, saves, warnings, errors).
 * The game is deterministic, so --replay <log> plays the same session again
 * (fast), then hands over to the player. */
static FILE *log_file;
static SDL_LogOutputFunction log_default;
static void *log_default_user;

static void SDLCALL log_out(void *u, int cat, SDL_LogPriority pri, const char *msg)
{
    (void)u;
    if (log_file) {
        fprintf(log_file, "# %s%s\n", pri >= SDL_LOG_PRIORITY_WARN ? "WARNING: " : "", msg);
        fflush(log_file);
    }
    if (log_default) log_default(log_default_user, cat, pri, msg);
}

static void SDLCALL log_quiet(void *u, int cat, SDL_LogPriority pri, const char *msg)
{
    (void)u; (void)cat; (void)pri; (void)msg;      /* the session log file has them */
}

static void log_open(const char *path)
{
    log_file = fopen(path, "w");
    if (!log_file) return;
    time_t t = time(NULL);
    fprintf(log_file, "# Supremacy port session log, %s", ctime(&t));
    fprintf(log_file, "# %s\n", sup_banner + 6);
    SDL_GetLogOutputFunction(&log_default, &log_default_user);
    SDL_SetLogOutputFunction(log_out, NULL);
}

typedef struct Rec { uint32_t pass; char type; int a, b, c, d; } Rec;
typedef struct Replay {
    Rec *rec;
    size_t n, at;
    int opponent;
    uint32_t seed;
    int active;
    int load_slot;                 /* "load <slot>": the session began from that save */
} Replay;
static Replay replay = {.load_slot = -1};

static int replay_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int have_game = 0;
    while (fgets(line, sizeof line, f)) {
        Rec r = {0};
        unsigned long pass;
        if (!have_game && sscanf(line, "game %d %x", &replay.opponent, &replay.seed) == 2) {
            have_game = 1;
            continue;
        }
        if (sscanf(line, "load %d", &replay.load_slot) == 1) continue;
        if (line[0] == 'M' && sscanf(line + 1, "%lu %d %d %d %d", &pass, &r.a, &r.b, &r.c, &r.d) == 5)
            r.type = 'M';
        else if (line[0] == 'K' && sscanf(line + 1, "%lu %d", &pass, &r.a) == 2)
            r.type = 'K';
        else
            continue;
        r.pass = (uint32_t)pass;
        Rec *nr = SDL_realloc(replay.rec, (replay.n + 1) * sizeof *nr);
        if (!nr) break;
        replay.rec = nr;
        replay.rec[replay.n++] = r;
    }
    fclose(f);
    if (!have_game) return -1;
    replay.active = 1;
    return 0;
}

static uint32_t game_pass_no;                        /* passes run so far */

/* a key for the game: logged (live) or ignored (while a replay runs) */
static void key_in(MainUI *ui, int code)
{
    if (replay.active) return;
    if (log_file) { fprintf(log_file, "K %u %d\n", (unsigned)game_pass_no, code); fflush(log_file); }
    ui_key(ui, code);
}

static const char *screen_name(int s)
{
    static const char *n[] = {"main", "troops", "cargo", "statistics", "fleet", "shipyard",
                              "surface", "combat", "spy", "options", "format", "starbase"};
    return s >= 0 && s < (int)(sizeof n / sizeof n[0]) ? n[s] : "?";
}

/* ---- the game ---- */
static const char *handler_name(uint32_t h)
{
    switch (h) {
    case 0x3518: return "starbase";
    case 0x825A: return "planet statistics";
    case 0x6944: return "shipyard";
    case 0x8C30: return "fleet";
    case 0x4F5C: return "format";
    case 0x5CC2: return "troops";
    case 0x7874: return "cargo bays";
    case 0x70C2: return "planet surface";
    case 0x3A2A: return "combat";
    case 0x5880: return "spy report";
    case 0x559A: return "game options";
    }
    return "?";
}

/* Returns 0 (quit), or 1 with *ending = the end sequence to play. */
/* load_slot >= 0: the game starts from that save (SSG1-4), as the options
 * panel's LOAD leaves it: the state block replaced, then the main screen. */
static int run_game(App *a, const GameData *gd, int opponent, uint32_t seed, double speed,
                    int load_slot, int *ending)
{
    static Screen scr;
    static MainUI ui;
    static uint8_t idx[320 * 200];
    static uint32_t px[320 * 200];
    Game *g = game_new();
    if (!g) return 1;
    g->play_sound = game_sound;
    game_set_image(g, gd->mem);
    screen_init(&scr, gd->mem + FONT_ADDR);
    main_init(&ui, &scr, g, gd->mem);
    main_new_game_screen(&ui);
    game_new_game(g, opponent, seed);
    main_new_game_done(&ui);
    SDL_Log("new game: opponent %d, seed %08X, %.2f passes/s", opponent, (unsigned)seed, speed);
    if (log_file) fprintf(log_file, "game %d %08X\n", opponent, (unsigned)seed);
    if (load_slot >= 0) {
        static uint8_t blk[STATE_SIZE];
        if (load_slot_file(load_slot, blk, STATE_SIZE, NULL) == 0) {
            memcpy(game_block(g), blk, STATE_SIZE);
            /* A save is written from the options panel while it is busy:
             * $E42D (busy: no pointer, no clicks) is $FF in it, with the
             * panel's button latch $E422 and EXECUTE/EXIT state $E431. The
             * in-game LOAD clears busy right after reading ($5486) and the
             * panel tidies the rest on leaving; starting from the launcher
             * goes straight to the main screen, so clear them here. */
            uint8_t *b = game_block(g);
            b[0xE42D - STATE_BASE] = 0;
            b[0xE422 - STATE_BASE] = 0;
            b[0xE431 - STATE_BASE] = 0;
            /* RESTART restarts the opponent of the save ($E43B, Rorn: $E43C = $FF) */
            g->booted_opponent = blk[0xE43C - STATE_BASE] == 0xFF ? 3 : blk[0xE43B - STATE_BASE];
            /* the message panel the main screen puts back: empty, not the
             * set-up's static ($47C8 clears it, $47DC saves it) */
            ui_clear_rect(&scr, 0x14, 0x5A, 8, 0x66);
            ui_panel_save(&ui);
            main_enter(&ui);
            if (log_file) fprintf(log_file, "load %d\n", load_slot);
        }
    }
    game_pass_no = 0;
    Mouse last = {-1, -1, 0, 0};
    int last_screen = ui.screen;

    int running = 1, frames = 0, over = 0;
    const char *aq = SDL_getenv("SUP_AUTOQUIT_FRAMES");
    int limit = aq ? SDL_atoi(aq) : 0;
    uint64_t next = SDL_GetTicksNS();
    const uint64_t step = (uint64_t)(1e9 / (speed > 0 ? speed : 47.0));
    SDL_HideCursor();
    SDL_StartTextInput(a->win);
    int latch_l = 0, latch_r = 0;    /* a press not yet seen by a game pass (a click quicker than a frame) */
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (app_event(a, &ev)) continue;
            if (ev.type == SDL_EVENT_QUIT) running = 0;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (ev.button.button == SDL_BUTTON_LEFT) latch_l = 1;
                if (ev.button.button == SDL_BUTTON_RIGHT) latch_r = 1;
            }
            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                for (const char *t = ev.text.text; *t; t++)
                    if ((unsigned char)*t >= 0x20 && (unsigned char)*t < 0x7F)
                        key_in(&ui, SDL_toupper((unsigned char)*t));
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = ev.key.key;
                if (k == SDLK_ESCAPE) key_in(&ui, 27);
                if (k == SDLK_BACKSPACE || k == SDLK_DELETE) key_in(&ui, 8);
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) key_in(&ui, 13);
            }
        }
        /* mouse in the 320x200 picture ($AB4: 0-317, $ABA: 1-197) */
        float wx, wy, lx, ly;
        SDL_MouseButtonFlags bt = SDL_GetMouseState(&wx, &wy);
        SDL_RenderCoordinatesFromWindow(a->ren, wx, wy, &lx, &ly);
        Mouse ms;
        ms.x = (int16_t)SDL_clamp((int)lx, 0, 317);
        ms.y = (int16_t)SDL_clamp((int)ly, 1, 197);
        ms.button = (bt & SDL_BUTTON_LMASK) || latch_l ? 1 : 0;
        ms.rbutton = (bt & SDL_BUTTON_RMASK) || latch_r ? 1 : 0;

        uint64_t now = SDL_GetTicksNS();
        int passes = 0;
        while ((replay.active || now >= next) && passes < (replay.active ? 400 : 4) && !over) {
            if (!replay.active) next += step;
            passes++;
            if (replay.active) {                     /* the recorded inputs of this pass */
                while (replay.at < replay.n && replay.rec[replay.at].pass <= game_pass_no) {
                    const Rec *rc = &replay.rec[replay.at++];
                    if (rc->type == 'K') ui_key(&ui, rc->a);
                    else last = (Mouse){(int16_t)rc->a, (int16_t)rc->b, (uint8_t)rc->c, (uint8_t)rc->d};
                }
                ms = last;
                if (replay.at >= replay.n) {
                    replay.active = 0;
                    next = SDL_GetTicksNS();
                    SDL_Log("replay finished at pass %u: the player has control", (unsigned)game_pass_no);
                }
            } else if (log_file && (ms.x != last.x || ms.y != last.y || ms.button != last.button ||
                                    ms.rbutton != last.rbutton)) {
                fprintf(log_file, "M %u %d %d %d %d\n", (unsigned)game_pass_no, ms.x, ms.y, ms.button, ms.rbutton);
                last = ms;
            }
            sound.hold = g8(g, 0xE42B) != 0;
            uint32_t h = 0;                           /* event waits run inside ui_frame */
            int r = ui_frame(&ui, &ms, &h);
            if (!replay.active) {                     /* the press was seen: the real state from now */
                latch_l = latch_r = 0;
                ms.button = (bt & SDL_BUTTON_LMASK) ? 1 : 0;
                ms.rbutton = (bt & SDL_BUTTON_RMASK) ? 1 : 0;
            }
            game_pass_no++;
            if (ui.screen != last_screen) {
                if (log_file) fprintf(log_file, "# pass %u: screen %s\n", (unsigned)game_pass_no, screen_name(ui.screen));
                last_screen = ui.screen;
            }
            if (r == MAIN_HANDLER) {
                SDL_Log("no handler for '%s' ($%X)", handler_name(h), (unsigned)h);
            } else if (r == UI_QUIT) {
                running = 0;
                break;
            } else if (r == MAIN_GAME_OVER) {
                over = 1;
                SDL_Log("game over: %s", g->result == 0xFF ? "lost" : "won");
                /* $3B50: the end loader runs WIN0 + opponent, or WIN_ on defeat */
                *ending = g->result == 0xFF ? ENDING_DEFEAT : opponent;
                running = 0;
                break;
            }
        }
        if (now > next + 10 * step) next = now;      /* no catching up after a stall */

        screen_to_indices(&scr, idx);
        render_copper(px, idx, ui.colour, ui.copper, gd->mem);
        /* the vertical blank's sprite table $B0C: the main screen's ($59BAE)
         * has the pointer and the planet arrow, the other screens' ($59B8E)
         * the pointer only; while busy ($3006 sets $E42D) it is $B14, empty */
        if (!g8(g, 0xE42D)) {
            if (ui.screen == SCR_MAIN)
                draw_sprite(px, ui.colour, gd->mem, ui.arrow_img ? ui.arrow_img : 0x592F4u,
                            ui.arrow_x, ui.arrow_y, 7);
            draw_sprite(px, ui.colour, gd->mem, 0x59288u, ms.x, ms.y, 16);
        }
        app_present(a, px);
        SDL_Delay(1);
        if (frames == 30 && SDL_getenv("SUP_GOTO")) {    /* tests: open a screen */
            uint32_t h = 0;
            ui_goto(&ui, (uint32_t)SDL_strtoul(SDL_getenv("SUP_GOTO"), NULL, 0), &h);
        }
        if (limit && ++frames >= limit) {
            const char *dump = SDL_getenv("SUP_DUMP_DIR");
            if (dump) dump_ppm(dump, "game", px);
            running = 0;
        }
    }
    SDL_ShowCursor();
    game_free(g);
    return over;
}

/* ---- the intro ---- */
static void *load_file(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", dir, name);
    void *f = SDL_LoadFile(path, len);
    if (!f) SDL_Log("cannot load %s: %s", path, SDL_GetError());
    return f;
}

static void music_stop(void)
{
    SDL_LockMutex(sound_lock);
    if (music_on) { music_on = 0; endsnd_free(&music); }
    SDL_UnlockMutex(sound_lock);
}

/* Returns 1 with the opponent and seed picked (CHALLENGE), 0 to quit, -1 if
 * the intro files are missing. */
static int run_intro(App *a, const char *dir, int *opponent, uint32_t *seed)
{
    static uint32_t px[320 * 200];
    size_t l1 = 0, l2 = 0;
    void *f1 = load_file(dir, "INT1", &l1), *f2 = load_file(dir, "INT2", &l2);
    static Intro in;
    int r = (f1 && f2) ? intro_init(&in, f1, l1, f2, l2) : -1;
    if (r) { SDL_free(f1); SDL_free(f2); return -1; }
    SDL_free(f1);
    const char *aq = SDL_getenv("SUP_AUTOQUIT_FRAMES");
    int limit = aq ? SDL_atoi(aq) : 0, frames = 0, running = 1, keys = 0, result = 0, latch = 0;
    const uint64_t step = 20000000ull;               /* PAL frames */
    uint64_t next = SDL_GetTicksNS();
    SDL_HideCursor();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (app_event(a, &ev)) continue;
            if (ev.type == SDL_EVENT_QUIT) running = 0;
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) keys++;
            if (ev.type == SDL_EVENT_KEY_UP && keys > 0) keys--;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)          /* a click quicker than a frame */
                latch |= ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
        }
        float wx, wy, lx, ly;
        SDL_MouseButtonFlags bt = SDL_GetMouseState(&wx, &wy);
        SDL_RenderCoordinatesFromWindow(a->ren, wx, wy, &lx, &ly);
        int held = ((bt & SDL_BUTTON_LMASK) ? 1 : 0) | ((bt & SDL_BUTTON_RMASK) ? 2 : 0);
        uint64_t now = SDL_GetTicksNS();
        for (int n = 0; now >= next && n < 4 && running; n++) {
            next += step;
            intro_frame(&in, (int)lx, (int)ly, held | latch, keys > 0);
            latch = 0;
            frames++;
            if (in.music_init) {                     /* jsr $68000, d7 = 0 */
                SDL_LockMutex(sound_lock);
                if (!music_on && endsnd_init(&music, f2, l2, ENDMUS_INTRO, AUDIO_RATE) == 0)
                    music_on = 1;
                SDL_UnlockMutex(sound_lock);
            }
            if (in.handover) {                       /* the loader: interrupts and audio off */
                *opponent = in.opponent;
                *seed = in.seed;
                result = 1;
                running = 0;
            }
        }
        if (now > next + 10 * step) next = now;
        intro_render(&in, px);
        app_present(a, px);
        SDL_Delay(1);
        if (limit && frames >= limit) {
            const char *dump = SDL_getenv("SUP_DUMP_DIR");
            if (dump) dump_ppm(dump, "intro", px);
            running = 0;
        }
    }
    music_stop();
    SDL_free(f2);
    intro_free(&in);
    SDL_ShowCursor();
    return result;
}

/* ---- the end sequences ---- */
static int run_ending(App *a, const char *dir, int which)
{
    static uint8_t idx[320 * 200];
    static uint32_t px[320 * 200];
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", dir, ending_file(which));
    size_t len = 0;
    void *file = SDL_LoadFile(path, &len);
    if (!file && which == ENDING_DEFEAT) {           /* "WIN." on the Amiga (WIN_ on Windows) */
        SDL_snprintf(path, sizeof path, "%s/WIN.", dir);
        file = SDL_LoadFile(path, &len);
    }
    if (!file) { SDL_Log("no end sequence: %s", SDL_GetError()); return 1; }
    Ending e;
    int r = ending_init(&e, which, file, len);
    SDL_free(file);
    if (r) { SDL_Log("%s is not a known end sequence", path); return 1; }
    SDL_Log("end sequence %s", ending_file(which));
    /* the game's sound stops ($3B50); the end loader puts WINM / DIEM at
     * $4E000 and the program starts it (jsr $68000, d7 = 0) */
    SDL_snprintf(path, sizeof path, "%s/%s", dir, ending_music_file(which));
    void *mus = SDL_LoadFile(path, &len);
    SDL_LockMutex(sound_lock);
    snd_stop(&sound);
    if (mus && endsnd_init(&music, mus, len, ENDMUS_END, AUDIO_RATE) == 0) music_on = 1;
    else SDL_Log("no end music (%s)", path);
    SDL_UnlockMutex(sound_lock);
    SDL_free(mus);
    const char *aq = SDL_getenv("SUP_AUTOQUIT_FRAMES");
    int limit = aq ? SDL_atoi(aq) : 0, frames = 0, running = 1;
    const uint64_t step = 20000000ull;               /* PAL frames */
    uint64_t next = SDL_GetTicksNS();
    SDL_HideCursor();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (app_event(a, &ev)) continue;
            if (ev.type == SDL_EVENT_QUIT) running = 0;
            if (e.done && (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN))
                running = 0;                         /* the original waits for a reset */
        }
        uint64_t now = SDL_GetTicksNS();
        for (int n = 0; now >= next && n < 4; n++) {
            next += step;
            ending_frame(&e);
            frames++;
        }
        if (now > next + 10 * step) next = now;
        ending_indices(&e, idx);
        for (int k = 0; k < 320 * 200; k++) px[k] = rgb12(e.colour[idx[k]]);
        app_present(a, px);
        SDL_Delay(1);
        if (limit && frames >= limit) {
            const char *dump = SDL_getenv("SUP_DUMP_DIR");
            if (dump) dump_ppm(dump, "ending", px);
            running = 0;
        }
    }
    SDL_ShowCursor();
    music_stop();
    ending_free(&e);
    return 0;
}

int main(int argc, char **argv)
{
    const char *data_dir = "SupremacyData";
    int opponent = 0, pictures = 0, ending = -2, intro = 1, scale = 3, fullscreen = 0;
    double speed = 47.0;
    const char *log_path = NULL, *replay_path = NULL;
    uint32_t seed = (uint32_t)time(NULL) * 2654435761u;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--opponent") && i + 1 < argc) { opponent = SDL_atoi(argv[++i]) & 3; intro = 0; }
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { seed = (uint32_t)SDL_strtoul(argv[++i], NULL, 0); intro = 0; }
        else if (!strcmp(argv[i], "--no-intro")) intro = 0;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = SDL_atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = SDL_atof(argv[++i]);
        else if (!strcmp(argv[i], "--pictures")) pictures = 1;
        else if (!strcmp(argv[i], "--ending") && i + 1 < argc) ending = SDL_atoi(argv[++i]);
        else data_dir = argv[i];
    }
    const char *base = SDL_GetBasePath();
    SDL_snprintf(save_dir, sizeof save_dir, "%ssave", base ? base : "");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    App a = {0};
    static Settings set;
    int launched = argc <= 1, load_slot = -1;        /* no arguments; 0 = started from Workbench */
    if (argc == 0) {                                 /* Workbench: any output would open a console */
        SDL_SetLogOutputFunction(log_quiet, NULL);
    }
    if (launched) {                                  /* from the icon: the launcher */
        char cfg[1100];
        SDL_snprintf(cfg, sizeof cfg, "%ssupremacy.cfg", base ? base : "");
        settings_defaults(&set, base);
        settings_load(&set, cfg);
        if (app_open(&a, 2, 0) != 0) return 1;
        int lr = launcher_run(a.win, a.ren, &set, base);
        if (!lr) {
            settings_save(&set, cfg);
            SDL_Quit();
            return 0;
        }
        settings_save(&set, cfg);
        data_dir = set.data;
        intro = set.intro;
        opponent = set.opponent;
        speed = settings_passes(set.speed);
        scale = set.scale;
        fullscreen = set.fullscreen;
        if (set.replay[0]) replay_path = set.replay;
        if (lr == 2) { load_slot = set.load_slot; intro = 0; }   /* LOAD SAVED GAME */
        app_game_mode(&a, scale, fullscreen);
    }
    ui_set_port_mode(1);
    if (replay_path) {
        if (replay_load(replay_path) != 0) {
            fprintf(stderr, "supremacy: cannot read the replay %s\n", replay_path);
            if (launched)
                show_error(a.win, "That file is not a session log (no 'game' line).");
            return 1;
        }
        opponent = replay.opponent;
        seed = replay.seed;
        load_slot = replay.load_slot;
        intro = 0;
    }
    char lp[1024];
    if (!log_path) {
        SDL_snprintf(lp, sizeof lp, "%s%s", base ? base : "",
                     replay_path ? "supremacy-replay.log" : "supremacy.log");
        log_path = lp;
    }
    log_open(log_path);
    if (replay_path) SDL_Log("replaying %s (%u inputs)", replay_path, (unsigned)replay.n);
    ui_options_set_io(save_slot, load_slot_file, NULL);
    static GameData gd;
    if (gamedata_load(&gd, data_dir) != 0) {
        fprintf(stderr, "supremacy: %s\n(give the path of your SupremacyData drawer)\n", gd.error);
        if (launched)
            show_error(a.win, gd.error);
        return 1;
    }
    if (!launched && app_open(&a, scale, fullscreen) != 0) return 1;
    SDL_AudioStream *audio = NULL;
    sound_lock = SDL_CreateMutex();
    if (snd_init(&sound, gd.mem + SOUND_BASE, SOUND_LEN, SOUND_BASE, AUDIO_RATE) == 0) {
        snd_set_image(&sound, gd.mem, AMIGA_MEM_SIZE);
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, AUDIO_RATE};
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_feed, NULL);
        if (audio) SDL_ResumeAudioStreamDevice(audio);
        else SDL_Log("no audio: %s", SDL_GetError());
    }
    int rc;
    if (pictures) rc = run_pictures(&a, &gd);
    else if (ending >= -1 && ending <= 3) rc = run_ending(&a, data_dir, ending);
    else {
        rc = 0;
        int go = 1;
        if (intro) {
            int ir = run_intro(&a, data_dir, &opponent, &seed);
            if (ir == 0) go = 0;                     /* closed during the intro */
            else if (ir < 0) SDL_Log("no intro (INT1/INT2): starting the game");
        }
        if (go && run_game(&a, &gd, opponent, seed, speed, load_slot, &ending) == 1)
            rc = run_ending(&a, data_dir, ending);
    }
    if (audio) SDL_DestroyAudioStream(audio);
    snd_free(&sound);
    SDL_DestroyMutex(sound_lock);
    SDL_DestroyTexture(a.tex);
    SDL_DestroyRenderer(a.ren);
    SDL_DestroyWindow(a.win);
    SDL_Quit();
    gamedata_free(&gd);
    return rc;
}

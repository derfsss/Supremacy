/* The launcher window. See launcher.h. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "launcher.h"
#include "extract.h"
#include "os4_dialog.h"
#include "gamedata.h"
#include "mainscreen.h"

#define LW 640                      /* logical size of the launcher */
#define LH 400
#define CH 8                        /* SDL_RenderDebugText: 8x8 characters */

static const char *OPPONENTS[4] = {"WOTOK", "SMINE", "KRART", "RORN"};

/* ---- settings ---- */

void settings_defaults(Settings *s, const char *base)
{
    memset(s, 0, sizeof *s);
    SDL_snprintf(s->data, sizeof s->data, "%sSupremacyData", base ? base : "");
    s->intro = 1;
    s->opponent = 0;
    s->speed = 0;
    s->scale = 3;
    s->fullscreen = 0;
    s->win_w = 2 * LW;
    s->win_h = 2 * LH;
    s->load_slot = -1;
}

static void trim(char *t)
{
    size_t n = strlen(t);
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ')) t[--n] = 0;
}

void settings_load(Settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "data")) SDL_strlcpy(s->data, v, sizeof s->data);
        else if (!strcmp(k, "intro")) s->intro = SDL_atoi(v) != 0;
        else if (!strcmp(k, "opponent")) s->opponent = SDL_atoi(v) & 3;
        else if (!strcmp(k, "speed")) s->speed = SDL_clamp(SDL_atoi(v), 0, 4);
        else if (!strcmp(k, "scale")) s->scale = SDL_clamp(SDL_atoi(v), 1, 12);
        else if (!strcmp(k, "fullscreen")) s->fullscreen = SDL_atoi(v) != 0;
        else if (!strcmp(k, "launcher_w")) s->win_w = SDL_clamp(SDL_atoi(v), LW / 2, 8 * LW);
        else if (!strcmp(k, "launcher_h")) s->win_h = SDL_clamp(SDL_atoi(v), LH / 2, 8 * LH);
    }
    fclose(f);
}

int settings_save(const Settings *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Supremacy launcher settings (speed 0 = normal, 1-4 = x1.25 .. x2)\n"
               "data=%s\nintro=%d\nopponent=%d\nspeed=%d\n"
               "scale=%d\nfullscreen=%d\nlauncher_w=%d\nlauncher_h=%d\n",
            s->data, s->intro, s->opponent, s->speed, s->scale, s->fullscreen,
            s->win_w, s->win_h);
    return fclose(f) == 0 ? 0 : -1;
}

static const char *SPEEDS[5] = {"NORMAL", "X1.25", "X1.5", "X1.75", "X2"};

double settings_passes(int speed)
{
    return 47.0 * (1.0 + 0.25 * SDL_clamp(speed, 0, 4));
}

/* ---- drawing ---- */

typedef struct { float x, y, w, h; } Box;

static int inside(Box b, float x, float y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
static int hv(float x, float y, float w, float h, float mx, float my)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void colour(SDL_Renderer *r, unsigned rgb)
{
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
}

static void text(SDL_Renderer *r, float x, float y, unsigned rgb, const char *t)
{
    colour(r, rgb);
    SDL_RenderDebugText(r, x, y, t);
}

static void frame(SDL_Renderer *r, Box b, unsigned fill, unsigned edge)
{
    SDL_FRect f = {b.x, b.y, b.w, b.h};
    colour(r, fill);
    SDL_RenderFillRect(r, &f);
    colour(r, edge);
    SDL_RenderRect(r, &f);
}

#define C_BACK   0x10101Cu
#define C_PANEL  0x1C1A2Cu
#define C_GOLD   0xCCA040u
#define C_TEXT   0xE6D2A0u
#define C_VALUE  0xFFFFFFu
#define C_DIM    0x6E6A80u
#define C_OK     0x60D060u
#define C_BAD    0xE05050u
#define C_HOVER  0x3A3456u

/* a button: text centred; returns its box */
static Box button(SDL_Renderer *r, float x, float y, float w, const char *t, int hover, int on)
{
    Box b = {x, y, w, 16};
    frame(r, b, hover && on ? C_HOVER : C_PANEL, on ? C_GOLD : C_DIM);   /* dimmed: no hover */
    text(r, x + (w - (float)strlen(t) * CH) / 2, y + 4, on ? C_VALUE : C_DIM, t);
    return b;
}

/* a path shown in a field of `chars` characters: the end of it if too long */
static void path_text(SDL_Renderer *r, float x, float y, int chars, const char *p, unsigned rgb, int caret)
{
    char buf[200];
    size_t n = strlen(p);
    if ((int)n + caret > chars) p += n + (size_t)caret - (size_t)chars;
    SDL_snprintf(buf, sizeof buf, "%s%s", p, caret ? "_" : "");
    text(r, x, y, rgb, buf);
}

/* ---- the folder / file dialogs (their callbacks may come on another thread) ---- */

static SDL_Mutex *dlg_lock;
static char dlg_path[4 * 1024];      /* several files (the two disks): one per line */
static int dlg_target;              /* 0 none, 1 data folder, 2 replay file, 3 archive */
static int dlg_done, dlg_failed;

#ifndef __amigaos4__                /* SDL's dialogs: not used on AmigaOS 4 (see pick) */
static void SDLCALL dlg_result(void *user, const char *const *list, int filter)
{
    (void)filter;
    SDL_LockMutex(dlg_lock);
    dlg_target = (int)(intptr_t)user;
    if (!list) dlg_failed = 1;
    else if (list[0]) {
        dlg_path[0] = 0;
        for (int i = 0; list[i] && i < 4; i++) {
            if (i) SDL_strlcat(dlg_path, "\n", sizeof dlg_path);
            SDL_strlcat(dlg_path, list[i], sizeof dlg_path);
        }
        dlg_done = 1;
    }
    SDL_UnlockMutex(dlg_lock);
}
#endif

/* A file or folder picker: 1 the data folder, 2 a session log, 3 archives or
 * IPF disks (several). The result arrives like SDL's: dlg_path / dlg_done. */
static void pick(SDL_Window *win, int target, const char *initial)
{
#ifdef __amigaos4__
    /* the ASL requester (os4_dialog.c: SDL's dialog thread crashes on OS4) */
    static char buf[sizeof dlg_path];
    int r = target == 1 ? os4_request("Game data folder", initial, 1, 0, NULL, buf, sizeof buf)
          : target == 2 ? os4_request("Session log to replay", initial, 0, 0, "#?.log", buf, sizeof buf)
                        : os4_request("WHDLoad archive or IPF disks", initial, 0, 1,
                                      "#?.(lha|lzh|zip|ipf)", buf, sizeof buf);
    (void)win;
    SDL_LockMutex(dlg_lock);
    if (r == 1) {
        SDL_strlcpy(dlg_path, buf, sizeof dlg_path);
        dlg_target = target;
        dlg_done = 1;
    } else if (r < 0) {
        dlg_failed = 1;
    }
    SDL_UnlockMutex(dlg_lock);
#else
    static const SDL_DialogFileFilter logs[] = {{"Session logs", "log"}, {"All files", "*"}};
    static const SDL_DialogFileFilter arcs[] = {
        {"WHDLoad archives and IPF disks", "lha;lzh;zip;ipf"}, {"All files", "*"}};
    if (target == 1) SDL_ShowOpenFolderDialog(dlg_result, (void *)1, win, initial, false);
    else if (target == 2) SDL_ShowOpenFileDialog(dlg_result, (void *)2, win, logs, 2, initial, false);
    else SDL_ShowOpenFileDialog(dlg_result, (void *)3, win, arcs, 2, initial, true);
#endif
}

static int data_ok(const char *dir, char *why, size_t n)
{
    static const char *need[] = {"GAM1", "GAM2", "INT1", "INT2", "WINM", "DIEM"};
    for (size_t k = 0; k < sizeof need / sizeof need[0]; k++) {
        char p[1100];
        SDL_snprintf(p, sizeof p, "%s/%s", dir, need[k]);
        if (!SDL_GetPathInfo(p, NULL)) {
            SDL_snprintf(why, n, "%s NOT FOUND IN THIS FOLDER", need[k]);
            return 0;
        }
    }
    SDL_snprintf(why, n, "GAME FILES FOUND");
    return 1;
}

/* ---- the saved games ---- */

typedef struct SaveInfo {
    int  present;
    char line1[64], line2[64], line3[64];
} SaveInfo;

static unsigned be16(const uint8_t *b, uint32_t a) { a -= 0xD440u; return (unsigned)(b[a] << 8 | b[a + 1]); }
static uint32_t be32(const uint8_t *b, uint32_t a) { return (uint32_t)be16(b, a) << 16 | be16(b, a + 2); }

/* what a save holds (the state block $D440-$EF97) and when it was written */
static void save_info(const char *base, int slot, SaveInfo *si)
{
    char p[1100];
    memset(si, 0, sizeof *si);
    SDL_snprintf(p, sizeof p, "%ssave/SSG%d", base ? base : "", slot + 1);
    size_t n = 0;
    uint8_t *b = SDL_LoadFile(p, &n);
    if (!b) return;
    if (n == 7000) {
        si->present = 1;
        int level = b[0xE43B - 0xD440], rorn = b[0xE43C - 0xD440] == 0xFF;
        int opp = level > 2 ? 2 : level;
        if (rorn) opp = 3;
        SDL_snprintf(si->line1, sizeof si->line1, "%s  -  DAY %u / %u", OPPONENTS[opp & 3],
                     be16(b, 0xE412), be16(b, 0xE414));
        uint32_t table = be32(b, 0xE3B0);
        unsigned last = be16(b, 0xE410), own = 0;
        uint32_t credits = 0;
        if (table >= 0xD440u && table + (last + 1) * 0x3Au <= 0xD440u + 7000u) {
            for (unsigned k = 0; k <= last; k++)
                if (be16(b, table + k * 0x3Au + 0x0C) == 10) own++;
            credits = be32(b, table + last * 0x3Au + 0x36);
        }
        SDL_snprintf(si->line2, sizeof si->line2, "%u CREDITS  -  %u PLANET%s", (unsigned)credits,
                     own, own == 1 ? "" : "S");
        /* the C library's stat(): SDL_GetPathInfo gives no dates on AmigaOS 4 */
        struct stat st;
        SDL_DateTime dt;
        if (stat(p, &st) == 0 && st.st_mtime > 0 &&
            SDL_TimeToDateTime(SDL_SECONDS_TO_NS((Sint64)st.st_mtime), &dt, true))
            SDL_snprintf(si->line3, sizeof si->line3, "SAVED %02d/%02d/%04d %02d:%02d",
                         dt.day, dt.month, dt.year, dt.hour, dt.minute);
    }
    SDL_free(b);
}

/* The game's own slot buttons 1-4: the options panel (picture $114A8 with
 * the menu overlay $3D614) drawn as the game draws it, the four slot
 * hotspots ($F7DC) cut out of it. */
static SDL_Texture *slot_tex[4];
static int slot_w, slot_h;
static char slot_from[1024];

static void make_slot_pictures(SDL_Renderer *ren, const char *data)
{
    if (slot_tex[0] && !strcmp(slot_from, data)) return;
    for (int k = 0; k < 4; k++)
        if (slot_tex[k]) { SDL_DestroyTexture(slot_tex[k]); slot_tex[k] = NULL; }
    SDL_strlcpy(slot_from, data, sizeof slot_from);
    static GameData gd;
    if (gamedata_load(&gd, data) != 0) return;
    static Screen sc;
    static MainUI m;
    memset(&m, 0, sizeof m);
    m.s = &sc;
    m.image = gd.mem;
    if (picture_unpack_raw(gd.mem, 0x114A8u, sc.bpl, m.colour) == 0) {
        ui_xor_delta(&m, 0x3D614u);
        const uint8_t *t = gd.mem + 0xF7DCu;
        for (int k = 0; k < 4; k++, t += 16) {
            int x0 = t[0] << 8 | t[1], y0 = t[2] << 8 | t[3], x1 = t[4] << 8 | t[5], y1 = t[6] << 8 | t[7];
            slot_w = x1 - x0 + 1;
            slot_h = y1 - y0 + 1;
            SDL_Surface *sf = SDL_CreateSurface(slot_w, slot_h, SDL_PIXELFORMAT_ARGB8888);
            if (!sf) continue;
            for (int y = 0; y < slot_h; y++)
                for (int x = 0; x < slot_w; x++) {
                    int px = x0 + x, py = y0 + y;
                    unsigned v = 0;
                    for (int pl = 0; pl < 5; pl++)
                        v |= (unsigned)((sc.bpl[py * 200 + pl * 40 + px / 8] >> (7 - px % 8)) & 1) << pl;
                    unsigned c = m.colour[v];
                    ((Uint32 *)((Uint8 *)sf->pixels + y * sf->pitch))[x] =
                        0xFF000000u | ((c >> 8) & 15) * 0x110000u | ((c >> 4) & 15) * 0x1100u | (c & 15) * 0x11u;
                }
            slot_tex[k] = SDL_CreateTextureFromSurface(ren, sf);
            SDL_SetTextureScaleMode(slot_tex[k], SDL_SCALEMODE_NEAREST);
            SDL_DestroySurface(sf);
        }
    }
    gamedata_free(&gd);
}

/* a wide button like PLAY NEW GAME, text at 2x */
static Box big_button(SDL_Renderer *r, float y, const char *label, int hover, int on)
{
    Box b = {40, y, LW - 80, 36};
    frame(r, b, on && hover ? C_HOVER : C_PANEL, on ? C_GOLD : C_DIM);
    SDL_SetRenderScale(r, 2, 2);
    text(r, (LW / 2.0f - (float)strlen(label) * CH) / 2, (y + 10) / 2.0f, on ? C_VALUE : C_DIM, label);
    SDL_SetRenderScale(r, 1, 1);
    return b;
}

/* Extract the game data from `archives` (one path per line: an archive, or
 * one or both IPF disks) into <base>SupremacyData and point the data folder
 * there; the outcome goes to `note`. */
static void do_extract(Settings *s, const char *base, const char *archives, char *note, size_t n,
                       unsigned *note_rgb)
{
    char out[1100], msg[200], one[1024];
    SDL_snprintf(out, sizeof out, "%sSupremacyData", base ? base : "");
    int r = -1, total = 0;
    const char *p = archives;
    while (*p) {
        size_t l = strcspn(p, "\n");
        SDL_strlcpy(one, p, l + 1 < sizeof one ? l + 1 : sizeof one);
        p += l + (p[l] == '\n');
        r = extract_game_data(one, out, msg, sizeof msg);
        if (r < 0) break;
        int got = 0;                    /* "EXTRACTED n FILES" / "GOT n FILES. ..." */
        if (SDL_sscanf(msg, "EXTRACTED %d", &got) == 1 || SDL_sscanf(msg, "GOT %d", &got) == 1) total += got;
    }
    if (r == 0) {
        SDL_strlcpy(s->data, out, sizeof s->data);
        SDL_snprintf(note, n, "EXTRACTED %d FILES INTO THE SUPREMACYDATA FOLDER", total);
        *note_rgb = 0x60D060u;
    } else if (r == 1) {                    /* one disk done: the other is needed */
        SDL_strlcpy(s->data, out, sizeof s->data);
        SDL_snprintf(note, n, "%s", msg);
        *note_rgb = C_GOLD;
    } else {
        SDL_snprintf(note, n, "EXTRACT FAILED: %s", msg);
        *note_rgb = 0xE05050u;
    }
}

int launcher_run(SDL_Window *win, SDL_Renderer *ren, Settings *s, const char *base)
{
    if (!dlg_lock) dlg_lock = SDL_CreateMutex();
    SDL_SetWindowTitle(win, "Supremacy - options");
    {                               /* the remembered size, shrunk to fit the screen */
        int w = s->win_w > 0 ? s->win_w : 2 * LW, h = s->win_h > 0 ? s->win_h : 2 * LH;
        SDL_Rect ub;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &ub)) {
            int aw = ub.w - 16, ah = ub.h - 48;        /* room for the borders and title bar */
            if (w > aw || h > ah) {
                double f = SDL_min((double)aw / w, (double)ah / h);
                w = (int)(w * f);
                h = (int)(h * f);
            }
        }
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    int extract_from = 0;           /* 1: extract from dlg_path, 2: from the data field */
    unsigned note_rgb = 0xE05050u;
    SDL_SetRenderLogicalPresentation(ren, LW, LH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_ShowCursor();
    /* the biggest window size that fits the screen (whole multiples of 320 x 200) */
    int max_scale = 12;
    SDL_Rect ub;                    /* the usable area (no taskbar), in window coordinates */
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &ub))
        max_scale = SDL_max(1, SDL_min(ub.w / 320, ub.h / 200));
    if (s->scale > max_scale) s->scale = max_scale;
    int editing = 0;                /* 1 data folder, 2 replay: typing into the field */
    int page = 0;                   /* 0 the options, 1 the LOAD SAVED GAME window */
    s->load_slot = -1;
    char note[160] = "";
    int result = -1;
    while (result < 0) {
        float mx = -1, my = -1;
        SDL_GetMouseState(&mx, &my);
        SDL_RenderCoordinatesFromWindow(ren, mx, my, &mx, &my);
        int click = 0;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { result = 0; SDL_Log("launcher: quit event"); }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                click = 1;
                SDL_RenderCoordinatesFromWindow(ren, ev.button.x, ev.button.y, &mx, &my);
            }
            if (editing && ev.type == SDL_EVENT_TEXT_INPUT) {
                char *t = editing == 1 ? s->data : s->replay;
                SDL_strlcat(t, ev.text.text, sizeof s->data);
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = ev.key.key;
                if (editing && k == SDLK_BACKSPACE) {
                    char *t = editing == 1 ? s->data : s->replay;
                    size_t n = strlen(t);
                    if (n) t[n - 1] = 0;
                } else if (editing && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE)) {
                    editing = 0;
                    SDL_StopTextInput(win);
                } else if (!editing && k == SDLK_ESCAPE) {
                    if (page) page = 0;         /* GO BACK */
                    else { result = 0; SDL_Log("launcher: escape"); }
                } else if (!editing && !page && (k == SDLK_RETURN || k == SDLK_KP_ENTER)) {
                    click = 2;              /* PLAY NEW GAME */
                }
            }
        }
        SDL_LockMutex(dlg_lock);
        if (dlg_done) {
            if (dlg_target == 3) extract_from = 1;          /* an archive was picked */
            else SDL_strlcpy(dlg_target == 1 ? s->data : s->replay, dlg_path, sizeof s->data);
            dlg_done = 0;
        }
        if (dlg_failed) {
            SDL_snprintf(note, sizeof note, "NO FILE DIALOG HERE: TYPE THE PATH INTO THE FIELD");
            note_rgb = C_BAD;
            dlg_failed = 0;
        }
        SDL_UnlockMutex(dlg_lock);

        if (extract_from) {
            SDL_LockMutex(dlg_lock);
            char arc[sizeof dlg_path];
            SDL_strlcpy(arc, extract_from == 1 ? dlg_path : s->data, sizeof arc);
            SDL_UnlockMutex(dlg_lock);
            do_extract(s, base, arc, note, sizeof note, &note_rgb);
            extract_from = 0;
        }
        char why[80];
        int ok = data_ok(s->data, why, sizeof why);
        int is_arc = !ok && extract_is_archive(s->data);
        int replay_ok = !s->replay[0] || SDL_GetPathInfo(s->replay, NULL);

        SaveInfo saves[4];
        int nsaves = 0;
        for (int k = 0; k < 4; k++) nsaves += (save_info(base, k, &saves[k]), saves[k].present);

        colour(ren, C_BACK);
        SDL_RenderClear(ren);
        if (page == 1) {                /* ---- the LOAD SAVED GAME window ---- */
            make_slot_pictures(ren, s->data);
            Box win_box = {24, 16, LW - 48, LH - 32};
            frame(ren, win_box, C_PANEL, C_GOLD);
            SDL_SetRenderScale(ren, 2, 2);
            text(ren, (LW / 2.0f - 15 * CH) / 2, 14, C_GOLD, "LOAD SAVED GAME");
            SDL_SetRenderScale(ren, 1, 1);
            float sw = (float)(slot_w ? slot_w : 28) * 2, sh = (float)(slot_h ? slot_h : 28) * 2;
            for (int k = 0; k < 4; k++) {
                float ry = 60 + k * (sh + 10);
                Box sb = {64, ry, sw, sh};
                int on = saves[k].present, hover = on && inside(sb, mx, my);
                if (slot_tex[k]) {
                    SDL_FRect d = {sb.x, sb.y, sb.w, sb.h};
                    Uint8 v = on ? (hover ? 255 : 230) : 70;
                    SDL_SetTextureColorMod(slot_tex[k], v, v, v);
                    SDL_RenderTexture(ren, slot_tex[k], NULL, &d);
                    if (hover) { colour(ren, C_GOLD); SDL_FRect f = {sb.x - 2, sb.y - 2, sb.w + 4, sb.h + 4}; SDL_RenderRect(ren, &f); }
                } else {
                    char n1[4];
                    SDL_snprintf(n1, sizeof n1, "%d", k + 1);
                    button(ren, sb.x, sb.y + sh / 2 - 8, sw, n1, hover, on);
                }
                float tx = sb.x + sw + 24;
                if (on) {
                    text(ren, tx, ry + sh / 2 - 18, C_VALUE, saves[k].line1);
                    text(ren, tx, ry + sh / 2 - 4, C_TEXT, saves[k].line2);
                    text(ren, tx, ry + sh / 2 + 10, C_DIM, saves[k].line3);
                } else {
                    text(ren, tx, ry + sh / 2 - 4, C_DIM, "NO SAVE DATA");
                }
                if (on && click == 1 && inside(sb, mx, my)) { s->load_slot = k; result = 2; }
            }
            Box back = big_button(ren, LH - 62, "GO BACK", inside((Box){40, LH - 62, LW - 80, 36}, mx, my), 1);
            if (click == 1 && inside(back, mx, my)) page = 0;
            if (!nsaves) page = 0;
            {                           /* tests: the window's picture (SUP_LAUNCHER_SHOT) */
                static int pf;
                const char *shot = SDL_getenv("SUP_LAUNCHER_SHOT");
                if (shot && ++pf == 10) {
                    SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
                    if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); }
                    const char *ls = SDL_getenv("SUP_LAUNCHER_LOADSLOT");
                    if (ls && saves[SDL_atoi(ls) & 3].present) { s->load_slot = SDL_atoi(ls) & 3; result = 2; }
                    else result = 0;
                }
            }
            SDL_RenderPresent(ren);
            SDL_Delay(16);
            continue;
        }
        SDL_SetRenderScale(ren, 3, 3);
        text(ren, (LW / 3.0f - 9 * CH) / 2, 6, C_GOLD, "SUPREMACY");
        SDL_SetRenderScale(ren, 1, 1);
        text(ren, (LW - 22 * CH) / 2, 46, C_DIM, "YOUR WILL BE DONE ...");

        const float L = 40, V = 230;
        float y = 70;
        Box b;
#define HOVER(bx) inside((bx), mx, my)
#define CLICKED(bx) (click == 1 && inside((bx), mx, my))

        /* the game data folder */
        text(ren, L, y + 4, C_TEXT, "GAME DATA FOLDER");
        Box field = {V, y, 300, 16};
        frame(ren, field, editing == 1 ? C_HOVER : C_PANEL, editing == 1 ? C_GOLD : C_DIM);
        path_text(ren, V + 4, y + 4, 36, s->data, C_VALUE, editing == 1);
        b = button(ren, V + 308, y, 64, "BROWSE", hv(V + 308, y, 64, 16, mx, my), 1);
        if (CLICKED(b)) pick(win, 1, s->data);
        if (click == 1 && inside(field, mx, my)) { editing = 1; SDL_StartTextInput(win); }
        else if (click == 1 && editing == 1) { editing = 0; SDL_StopTextInput(win); }
        if (is_arc) {                   /* an archive's path typed in the field */
            text(ren, V, y + 22, C_TEXT, "THIS IS AN ARCHIVE:");
            b = button(ren, V + 168, y + 18, 204, "EXTRACT THE GAME FILES", hv(V + 168, y + 18, 204, 16, mx, my), 1);
            if (CLICKED(b)) extract_from = 2;
        } else if (!ok) {
            text(ren, V, y + 22, C_BAD, why);
            y += 20;
            text(ren, L, y + 22, C_DIM, "NO GAME FILES?");
            b = button(ren, V, y + 18, 372, "EXTRACT FROM WHDLOAD .LHA/.ZIP OR .IPF DISKS",
                       hv(V, y + 18, 372, 16, mx, my), 1);
            if (CLICKED(b)) pick(win, 3, base);  /* the program's folder; both disks at once */
        } else {
            text(ren, V, y + 22, C_OK, why);
        }
        y += 40;

        /* intro */
        text(ren, L, y + 4, C_TEXT, "INTRO");
        b = button(ren, V, y, 64, s->intro ? "ON" : "OFF", hv(V, y, 64, 16, mx, my), 1);
        if (CLICKED(b)) s->intro = !s->intro;
        text(ren, V + 76, y + 4, C_DIM, s->intro ? "PICK THE OPPONENT IN THE INTRO" : "START THE GAME AT ONCE");
        y += 24;

        /* opponent */
        text(ren, L, y + 4, s->intro ? C_DIM : C_TEXT, "OPPONENT");
        b = button(ren, V, y, 24, "<", hv(V, y, 24, 16, mx, my), !s->intro);
        if (!s->intro && CLICKED(b)) s->opponent = (s->opponent + 3) & 3;
        Box ov = {V + 28, y, 80, 16};
        frame(ren, ov, C_PANEL, C_DIM);
        text(ren, V + 28 + (80 - (float)strlen(OPPONENTS[s->opponent]) * CH) / 2, y + 4,
             s->intro ? C_DIM : C_VALUE, OPPONENTS[s->opponent]);
        b = button(ren, V + 112, y, 24, ">", hv(V + 112, y, 24, 16, mx, my), !s->intro);
        if (!s->intro && CLICKED(b)) s->opponent = (s->opponent + 1) & 3;
        y += 24;

        /* speed */
        char num[64];
        text(ren, L, y + 4, C_TEXT, "GAME SPEED");
        b = button(ren, V, y, 24, "<", hv(V, y, 24, 16, mx, my), s->speed > 0);
        if (CLICKED(b)) s->speed = SDL_max(0, s->speed - 1);
        SDL_snprintf(num, sizeof num, "%s", SPEEDS[SDL_clamp(s->speed, 0, 4)]);
        frame(ren, (Box){V + 28, y, 80, 16}, C_PANEL, C_DIM);
        text(ren, V + 28 + (80 - (float)strlen(num) * CH) / 2, y + 4, C_VALUE, num);
        b = button(ren, V + 112, y, 24, ">", hv(V + 112, y, 24, 16, mx, my), s->speed < 4);
        if (CLICKED(b)) s->speed = SDL_min(4, s->speed + 1);
        SDL_snprintf(num, sizeof num, "%.2f PASSES/S%s", settings_passes(s->speed),
                     s->speed ? "" : " (ORIGINAL)");
        text(ren, V + 148, y + 4, C_DIM, num);
        y += 24;

        /* window size */
        text(ren, L, y + 4, C_TEXT, "WINDOW SIZE");
        b = button(ren, V, y, 24, "<", hv(V, y, 24, 16, mx, my), s->scale > 1);
        if (CLICKED(b)) s->scale = SDL_max(1, s->scale - 1);
        SDL_snprintf(num, sizeof num, "%dX", s->scale);
        frame(ren, (Box){V + 28, y, 80, 16}, C_PANEL, C_DIM);
        text(ren, V + 28 + (80 - (float)strlen(num) * CH) / 2, y + 4, C_VALUE, num);
        b = button(ren, V + 112, y, 24, ">", hv(V + 112, y, 24, 16, mx, my), s->scale < max_scale);
        if (CLICKED(b)) s->scale = SDL_min(max_scale, s->scale + 1);
        SDL_snprintf(num, sizeof num, "%d X %d", 320 * s->scale, 200 * s->scale);
        text(ren, V + 148, y + 4, C_DIM, num);
        y += 24;

        /* full screen */
        text(ren, L, y + 4, C_TEXT, "FULL SCREEN");
        b = button(ren, V, y, 64, s->fullscreen ? "ON" : "OFF", hv(V, y, 64, 16, mx, my), 1);
        if (CLICKED(b)) s->fullscreen = !s->fullscreen;
        text(ren, V + 76, y + 4, C_DIM, "F11 OR ALT+ENTER SWITCHES IN THE GAME");
        y += 24;

        /* replay */
        text(ren, L, y + 4, C_TEXT, "REPLAY A SESSION LOG");
        Box rfield = {V, y, 236, 16};
        frame(ren, rfield, editing == 2 ? C_HOVER : C_PANEL, editing == 2 ? C_GOLD : C_DIM);
        path_text(ren, V + 4, y + 4, 28, s->replay[0] || editing == 2 ? s->replay : "(NONE)",
                  s->replay[0] ? C_VALUE : C_DIM, editing == 2);
        b = button(ren, V + 244, y, 64, "CHOOSE", hv(V + 244, y, 64, 16, mx, my), 1);
        if (CLICKED(b)) pick(win, 2, base);     /* the logs are next to the program */
        b = button(ren, V + 316, y, 56, "CLEAR", hv(V + 316, y, 56, 16, mx, my), s->replay[0] != 0);
        if (CLICKED(b)) s->replay[0] = 0;
        if (click == 1 && inside(rfield, mx, my)) { editing = 2; SDL_StartTextInput(win); }
        else if (click == 1 && editing == 2 && !inside(rfield, mx, my)) { editing = 0; SDL_StopTextInput(win); }
        if (!replay_ok) text(ren, V, y + 22, C_BAD, "THIS FILE DOES NOT EXIST");
        else if (s->replay[0]) text(ren, V, y + 22, C_DIM, "SKIPS THE INTRO, THEN YOU PLAY ON");
        y += 40;

        if (note[0]) text(ren, (LW - (float)strlen(note) * CH) / 2, y, note_rgb, note);

        if (SDL_getenv("SUP_LAUNCHER_LOADPAGE") && nsaves && ok) page = 1;   /* tests */
        /* LOAD SAVED GAME (only when there are saves) and PLAY NEW GAME */
        int can = ok && replay_ok;
        if (nsaves) {
            Box load = big_button(ren, LH - 98, "LOAD SAVED GAME", HOVER(((Box){40, LH - 98, LW - 80, 36})), ok);
            if (ok && click == 1 && inside(load, mx, my)) page = 1;
        }
        Box play = big_button(ren, LH - 54, "PLAY NEW GAME", HOVER(((Box){40, LH - 54, LW - 80, 36})), can);
        if (can && ((click == 1 && inside(play, mx, my)) || click == 2)) result = 1;
        else if (!can && click == 2) {
            SDL_snprintf(note, sizeof note, "SET THE GAME DATA FOLDER FIRST");
            note_rgb = C_BAD;
        }
#undef HOVER
#undef CLICKED
        {                                           /* tests: SUP_LAUNCHER_SHOT=file.bmp (SUP_LAUNCHER_PLAY: then PLAY;
                                               SUP_LAUNCHER_EXTRACT=archive) */
            static int frames;
            const char *shot = SDL_getenv("SUP_LAUNCHER_SHOT");
            const char *arc = SDL_getenv("SUP_LAUNCHER_EXTRACT");   /* tests: an archive picked */
            if (arc && frames == 2) { SDL_strlcpy(dlg_path, arc, sizeof dlg_path); extract_from = 1; }
            if (shot && ++frames == 10) {
                SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
                if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); }
                else SDL_Log("launcher shot: %s", SDL_GetError());
                result = SDL_getenv("SUP_LAUNCHER_PLAY") && can ? 1 : 0;   /* PLAY NEW GAME */

            }
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    if (editing) SDL_StopTextInput(win);
    if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)))
        SDL_GetWindowSize(win, &s->win_w, &s->win_h);   /* remembered for next time */
    SDL_SetWindowTitle(win, "Supremacy");
    return result;
}

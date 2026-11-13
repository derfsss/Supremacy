/*
 * Supremacy port - game core.
 *
 * The game state is kept in the original's exact byte layout: the 7,000-byte
 * block the original saves as SSG<n> ($D440-$EF97 in the 68000's memory),
 * big-endian, with pointers stored as the original 68000 addresses. That
 * keeps saves compatible with the original and lets every routine be checked
 * byte for byte against the original running in an emulator. Addresses in
 * this code are the original's.
 *
 * Accessors take the original address: g16(g, 0xE412) is the day.
 */
#ifndef SUP_GAME_H
#define SUP_GAME_H

#include <stdint.h>

#if defined(_WIN32) && defined(SUP_BUILD_DLL)
#define SUP_API __declspec(dllexport)
#else
#define SUP_API
#endif

#define STATE_BASE 0xD440u          /* original address of the block */
#define STATE_SIZE 0x1B58u          /* 7,000 bytes */

typedef struct Game {
    uint8_t  blk[STATE_SIZE];       /* the saved state block, original layout */
    uint32_t rng_seed;              /* $9DB0: outside the block in the original */
    /* 0 by default: the port matches the original as the WHDLoad version
     * plays it. Fixes for the original's bugs are guarded by this flag and
     * kept for a possible later edition with fixes and new options. */
    int      fix_bugs;
    /* platform callbacks (may be NULL) */
    void   (*play_sound)(struct Game *g, int number);
    void    *user;
    /* ---- daily state the original keeps outside the block (day.c) ---- */
    uint8_t  banner[23];            /* "* INCOMING  MESSAGE! *" at $1A79, $FF-ended;
                                       bit 7 = character hidden */
    uint16_t tension_sound;         /* $2C60: the last alarm sound started */
    int      result;                /* $800 after $3B50: -1 playing, $FF lost, 0-3 won */
    int      booted_opponent;       /* RESTART restores the opponent booted with */
    const uint8_t *image;           /* the original's memory image (static tables) */
    /* UI hooks for the daily jobs that draw (may be NULL):
     * banner(g, k): character k of the banner was revealed ($95CC), k = -1:
     * blink ($9644, shown on odd passes); war_scene: $245E on the combat
     * screen. */
    void   (*banner_hook)(struct Game *g, int k);
    void   (*war_scene_hook)(struct Game *g);
    /* new-game set-up: each word of the message-panel static ($46A8: 51
     * frames of 103 lines x 9 words, written to planes 0 and 1); index =
     * line * 9 + word */
    void   (*panel_static_hook)(struct Game *g, int index, uint16_t value);
} Game;

/* ---- block access by original address (big-endian) ---- */
static inline uint8_t *gp(Game *g, uint32_t addr) { return &g->blk[addr - STATE_BASE]; }
static inline uint8_t  g8(Game *g, uint32_t a)  { return *gp(g, a); }
static inline uint16_t g16(Game *g, uint32_t a) { uint8_t *p = gp(g, a); return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t g32(Game *g, uint32_t a)
{
    uint8_t *p = gp(g, a);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void s8(Game *g, uint32_t a, uint8_t v)   { *gp(g, a) = v; }
static inline void s16(Game *g, uint32_t a, uint16_t v) { uint8_t *p = gp(g, a); p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void s32(Game *g, uint32_t a, uint32_t v)
{
    uint8_t *p = gp(g, a);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void or32(Game *g, uint32_t a, uint32_t bits) { s32(g, a, g32(g, a) | bits); }

/* ---- well-known globals ---- */
#define G_DAY          0xE412u     /* u16 day of year, 1..64 */
#define G_YEAR         0xE414u     /* u16 year */
#define G_PLANET_TABLE 0xE3B0u     /* u32 address of planet record 0 */
#define G_REDRAW       0xE39Cu     /* u32 redraw bits (UI) */
#define G_MSG_WRITE    0xE3B8u     /* u32 message ring write pointer */
#define G_MSG_COUNT    0xE41Au     /* u16 messages queued */
#define G_MSG_ARRIVED  0xE43Du     /* u8  message-arrived flag */
#define G_MSG_RING     0xE450u     /* message ring (u32 text addresses), $FE ends it */

/* ---- planet record (58 bytes) offsets ---- */
#define PLANET_SIZE   0x3A
#define P_NAME        0x0E
#define P_OWNER       0x0C         /* u16: 10 player, 7 enemy, 4 lifeless, 1 formatting */
#define P_GROWTH      0x1D         /* u8 */
#define P_TAX         0x1E         /* u8 % */
#define P_MORALE      0x1F         /* u8 % */
#define P_DECLINE     0x21         /* u8 */
#define P_TYPE        0x24         /* u8 planet type 0-5 */
#define P_POP         0x2A         /* u16 */
#define P_FOOD        0x2C         /* u16 */
#define P_MINERALS    0x30         /* u16 */
#define P_FUELS       0x32         /* u16 */
#define P_ENERGY      0x34         /* u16 */
#define P_CREDITS     0x36         /* u32 */

#define OWNER_PLAYER  10

/* ---- craft bays and platoons ---- */
#define BAY_TABLE     0xE654u      /* 32 docking bays x 40 bytes */
#define BAY_SIZE      0x28u
#define NBAYS         32
#define PLATOON_TABLE 0xEB54u      /* 24 platoons x 12 bytes */
#define PLATOON_SIZE  12u
#define NPLATOONS     24

/* ---- core services (game.c) ---- */
SUP_API uint16_t rng_next(Game *g);                  /* $9DDE */
SUP_API uint16_t rng_below(Game *g, uint16_t n);     /* $9DB4: 0..n */
SUP_API void     post_message(Game *g, uint32_t text_addr);   /* $4A9E */

/* ---- daily rules ---- */
SUP_API void planet_economy(Game *g, uint32_t planet_addr);   /* $756E */

/* ---- for tests (ctypes) ---- */
SUP_API Game    *game_new(void);
SUP_API void     game_free(Game *g);
SUP_API uint8_t *game_block(Game *g);
SUP_API void     game_set_seed(Game *g, uint32_t seed);
SUP_API void     game_set_fix_bugs(Game *g, int on);
SUP_API uint32_t game_get_seed(Game *g);

#endif

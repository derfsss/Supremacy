/*
 * Supremacy port - craft: docking bays, the daily bay pass and the
 * state-changing actions of the shipyard, fleet, cargo and surface screens
 * and of the format icon. Checked against the original game running in an emulator.
 *
 * Addresses are the original's (see game.h): a bay is passed as its record
 * address ($E654 + i * $28), a planet as its record address, a template as
 * $ECD4 + (type - 1) * $30.
 *
 * Actions are plain functions; the UI picks the bay / planet and shows the
 * result. Most return a "text address": 0 = done, CRAFT_NOTHING = nothing
 * happened and nothing is shown, any other value = the original address of
 * the refusal text the original prints in the screen's message line (it is
 * not posted to the message queue). Messages the original posts ($4A9E) are
 * posted with post_message(); sounds go through g->play_sound.
 */
#ifndef SUP_CRAFT_H
#define SUP_CRAFT_H

#include "game.h"

#define BAY_COUNT      32
#define TPL_TABLE      0xECD4u     /* 7 craft templates, $30 bytes */
#define TPL_SIZE       0x30u

/* bay record offsets */
#define B_NAME     0x00
#define B_FOOD     0x0A
#define B_SCRAP    0x0C
#define B_MINERALS 0x0E
#define B_FUELS    0x10
#define B_ENERGY   0x12
#define B_CREW     0x14
#define B_TRAVEL   0x18
#define B_DEST     0x1A
#define B_TYPE     0x1E
#define B_FLAGS    0x1F
#define B_RANGE    0x20
#define B_LOC      0x21
#define B_FORMAT   0x22
#define B_CIVIL    0x24
#define B_TANK     0x26

/* bay flags (+$1F) */
#define BF_FORMAT  0x01
#define BF_ORBIT   0x04
#define BF_TRANSIT 0x08
#define BF_SURFACE 0x10
#define BF_RUNNING 0x20

/* template offsets */
#define T_PAYLOAD  0x04
#define T_PRICE    0x06
#define T_SCRAP    0x08
#define T_CREW     0x0A            /* $FFFE = none */
#define T_FUEL     0x0C            /* $FFFF = nuclear */
#define T_ENERGY   0x0E
#define T_MINERALS 0x10
#define T_TYPE     0x12
#define T_RANGE    0x13
#define T_SEATS    0x1E
#define T_NAME     0x22            /* 9 chars + $FF; +$2A is the counter digit */

#define CRAFT_NOTHING 1u

/* ---- daily pass ---- */
/* $71E0 from $7210: one bay (0..31) of the daily bay pass. The caller (the
 * time step) has already handled $E408 and stepped $E426 to `bay`. */
SUP_API void     craft_bay_pass(Game *g, int bay);

/* ---- shipyard ---- */
/* BUY ($9B88) checks: 0 = can buy, else the refusal text. Sets $E437. */
SUP_API uint32_t craft_buy_check(Game *g, uint32_t tpl);
/* BUY after the name prompt was confirmed: the name (9 characters; NULL =
 * the template's default) goes into the lowest free bay, the template's name
 * counter steps ($242E), the craft is docked at the starbase and paid for.
 * Returns the bay, 0 if none was free. Call craft_buy_check first. */
SUP_API uint32_t craft_buy(Game *g, uint32_t tpl, const uint8_t *name);
/* $242E: step the counter in a default name ("..1" -> "..2", "..9" -> ".10"). */
SUP_API void     craft_name_step(Game *g, uint32_t digit_addr);

/* ---- fleet screen ($8C30) ---- */
/* screen_planet = the planet the fleet screen shows ($E3D4). bay 0 = none. */
SUP_API uint32_t craft_launch(Game *g, uint32_t bay, uint32_t screen_planet);  /* $8882 */
SUP_API uint32_t craft_dock(Game *g, uint32_t bay, uint32_t screen_planet);    /* $8796 */
/* FLY TO, before the planet is picked ($8A02): 0 = pick a planet. */
SUP_API uint32_t craft_fly_check(Game *g, uint32_t bay);
/* FLY TO, the picked planet ($8B2E). 0 = on its way. */
SUP_API uint32_t craft_fly_to(Game *g, uint32_t bay, uint32_t dest);
SUP_API uint32_t craft_stop(Game *g, uint32_t bay);                           /* $5708 */
SUP_API void     craft_rename(Game *g, uint32_t bay, const uint8_t *name);    /* $285C, 9 chars */

/* ---- cargo screen ($7874); planet = the main-screen planet ($E3DC) ---- */
/* what: 0 food, 1 minerals, 2 fuels, 3 energy. One call = one frame held. */
SUP_API void     craft_load(Game *g, uint32_t bay, uint32_t planet, int what);    /* $7F64.. */
SUP_API void     craft_unload(Game *g, uint32_t bay, uint32_t planet, int what);  /* $80C0.. */
SUP_API void     craft_load_civilians(Game *g, uint32_t bay, uint32_t planet);    /* $7A18 */
SUP_API void     craft_unload_civilians(Game *g, uint32_t bay, uint32_t planet);  /* $7A60 */
/* Fill (empty = 0) or empty the tank, one frame held. The rate speeds up
 * while held: it uses the frame counter $E42C (the screen loop steps it each
 * frame) and $E44A-$E44D, as the original. */
SUP_API void     craft_fuel(Game *g, uint32_t bay, uint32_t planet, int empty);   /* $7AAC/$7AA4 */
SUP_API uint32_t craft_assign_crew(Game *g, uint32_t bay, uint32_t planet);       /* $7B88 */
/* Unload everything ($7BF4). limit_planet = the screen's planet ($E3D4),
 * whose stores are tested against 30,000 (the original's; see fix list). */
SUP_API void     craft_unload_all(Game *g, uint32_t bay, uint32_t planet, uint32_t limit_planet);
/* Scrap after "Y" ($0BDC): unload everything, platoons off, refunds, bay freed. */
SUP_API void     craft_scrap(Game *g, uint32_t bay, uint32_t planet, uint32_t limit_planet);

/* ---- planet surface screen ($70C2); planet = $E3DC ---- */
/* Fills out[0..5] with the craft on the planet's surface, in bay order
 * ($6FC4); returns how many there are (can exceed 6; only 6 are stored). */
SUP_API int      craft_surface_list(Game *g, uint32_t planet, uint32_t out[6]);
SUP_API uint32_t craft_land(Game *g, uint32_t planet, int slot);         /* $6ED4.., slot 0..2 */
SUP_API uint32_t craft_take_off(Game *g, uint32_t planet, uint32_t bay); /* $6E08.. */
SUP_API uint32_t craft_toggle_running(Game *g, uint32_t bay);            /* $6E9A.. */

/* ---- format icon ($4F5C); planet = $E3DC ---- */
/* Returns 0 when the format prompt should follow (messages posted, $D56C =
 * format days, $D570 = distance), CRAFT_NOTHING when the attempt ended with
 * the messages posted (no processor, progress report, wrong planet). */
SUP_API uint32_t craft_format_check(Game *g, uint32_t planet);
/* After the name prompt was confirmed: planet name (NULL = default at $E38E),
 * the default's counter steps, the processor is sent. */
SUP_API void     craft_format_start(Game *g, uint32_t planet, const uint8_t *name);
/* ESC at the prompt: the planet is named "LIFELESS!", "ABORTED" posted. */
SUP_API void     craft_format_abort(Game *g, uint32_t planet);

/* helpers */
SUP_API uint32_t craft_template(Game *g, uint32_t bay);   /* $4682 */
SUP_API uint32_t craft_location(Game *g, uint32_t bay);   /* $71CC */

#endif

/*
 * Supremacy port - platoons and ground combat.
 *
 * Daily passes: $7736 (platoon training, passes $20-$37) and $91EA (ground
 * forces, one planet per pass, with $9342 capture, $99D6 reseed, $5AA6
 * random name, $9508 conquest, $5786 captured craft and the state part of
 * $4E28, the main-screen viewer scene). Plus the state-changing actions of
 * the troops screen ($5CC2) and the combat screen ($3A2A) as plain
 * functions, including the combat-entry pass $91E2.
 * Checked against the original game running in an emulator.
 */
#ifndef SUP_COMBAT_H
#define SUP_COMBAT_H

#include "game.h"

#define NUM_PLATOONS  24
#define NUM_BAYS      32

/* Display callbacks for the parts of these routines that only draw (the
 * state changes are done by the core). May be left NULL. */
typedef struct CombatHooks {
    void (*viewer_scene)(Game *g);    /* $4E28: redraw the main-screen viewer for $E3DC */
    void (*orbit_slots)(Game *g);     /* $86C4: redraw the combat screen's orbit-slot names */
} CombatHooks;
SUP_API void combat_set_hooks(const CombatHooks *h);

/* For the checks: which fixed bugs' conditions were met since the last
 * clear (bits 1..16 = F1..F5, see combat.c). */
SUP_API unsigned combat_fix_hits(int clear);

/* ---- daily passes ---- */
SUP_API void platoon_pass(Game *g, uint16_t pass);      /* $7736, pass = $20..$37 */
SUP_API void ground_forces(Game *g, uint16_t pass);     /* $91EA, pass = $38+N+index */
SUP_API void ground_forces_entry(Game *g);              /* $91E2: the pass the combat screen runs on $E3DC */

/* Results of the screen actions (what the original prints or posts). */
enum {
    CT_OK = 0,             /* done */
    CT_NOTHING,            /* the action did not apply (no message) */
    CT_NOT_ENOUGH_CREDITS, /* "NOT ENOUGH CREDITS!" (screen held 44 passes) */
    CT_EMPTY,              /* "THAT'S EMPTY!" */
    CT_CANT_CARRY,         /* "CAN'T CARRY TROOPS!" */
    CT_SELECT_SHIP,        /* "SELECT A SHIP!" */
    CT_SHIP_FULL,          /* "SHIP IS FULL!" */
    CT_ACCESS_DENIED,      /* combat entry refused: owner 7 ($8F0E, posted; back to the main screen) */
    CT_NEEDS_FORMATTING    /* combat entry refused: "THIS PLANET NEEDS FORMATTING!" (posted) */
};

/* ---- troops screen ($5CC2); the platoon is $E3A4 (index $E420) ---- */
SUP_API void     troops_select(Game *g, uint8_t index);  /* $5FE8: select platoon 0..23 */
SUP_API void     troops_next(Game *g);                   /* $6082 */
SUP_API void     troops_prev(Game *g);                   /* $5FC8 */
SUP_API int      troops_disband(Game *g);                /* $5C50, box 3 */
SUP_API int      troops_equip(Game *g);                  /* $5DEA, box 4 */
SUP_API void     troops_recruit(Game *g);                /* $5E8C, box 5: one call per pass held */
SUP_API void     troops_dismiss(Game *g);                /* $5E84, box 6: one call per pass held */
SUP_API void     troops_suit(Game *g, int up);           /* $6254 (up) / $6276 (down) */
SUP_API void     troops_weapon(Game *g, int up);         /* $6382 (up) / $63AC (down) */
SUP_API uint32_t troops_equip_cost(Game *g);             /* $61FC: (suit + weapon cost) x troops */

/* ---- combat screen ($3A2A); the planet is $E3DC (= $E3D4 there) ---- */
SUP_API int  combat_enter(Game *g);                      /* $3A2A set-up, including the $91E2 pass */
SUP_API void combat_aggression(Game *g, int up);         /* $3974 (up) / $39D0 (down) */
SUP_API int  combat_select_craft(Game *g, int slot);     /* $37FC / $38AC / $38BC, slot 0..2 */
SUP_API int  combat_unload(Game *g, int n);              /* $6D8C.. : platoon n (0..3) aboard $E3D8 */
SUP_API int  combat_load(Game *g, int entry);            /* $8198: entry 0..23 of the list at $EC74 */
SUP_API void combat_build_list(Game *g);                 /* $372E: $EC74 = platoons at $E3D4 */
SUP_API int  combat_aboard(Game *g, uint32_t out[NUM_PLATOONS]); /* $33EC: platoons aboard $E3D8 */

#endif

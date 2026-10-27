/* cartwheel platformer controller: the feel of a side-view platformer on top of cw_mover2d.
 *
 * What it adds to the mover: acceleration and braking on the ground and in the air, gravity that is heavier when
 * falling and when a jump is let go early (variable jump height), corner correction (a head that clips a ceiling corner is carried round it), coyote time (a jump still works for a moment
 * after walking off a ledge), jump buffering (a press just before landing jumps on landing), air jumps, wall
 * slide and wall jump (with a grace window after leaving the wall), a dash, dropping through one-way platforms
 * (down + jump) and swimming (the game says how deep the character is and which way a current pushes).
 *
 *   cw_platformer2d p;
 *   cw_platformer2d_init(&p, world, NULL, feet);
 *   fixed_update:  cw_platformer2d_step(&p, &(cw_platformer2d_input){ .move_x = ax, .jump_held = held,
 *                                       .jump_pressed = pressed }, dt);
 *   then read p.events (CW_PF_*) for sounds and particles, p.body.position for the feet.
 *
 * The step is simulation code: + - * / and sqrt only, the same bits on every host. */
#ifndef CW_PLATFORMER2D_H
#define CW_PLATFORMER2D_H

#include "physics2d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_platformer2d_def {
  cw_mover2d_def mover;   /* the body (defaults: 0.3 radius, 1.8 tall; a game sets its own size and one_way_mask) */
  float run_speed;        /* m/s: 8 */
  float ground_accel, ground_brake; /* m/s^2 speeding up / slowing down on the ground: 70, 90 */
  float air_accel, air_brake;       /* 45, 25 */
  float gravity;          /* m/s^2: 38 */
  float fall_gravity;     /* multiplier while falling: 1.4 */
  float jump_cut_gravity; /* multiplier while rising with the button up: 3 */
  float apex_gravity;     /* multiplier near the top of a held jump (|vy| < apex_speed): 0.55 */
  float apex_speed;       /* 1.5 */
  float max_fall;         /* m/s: 22 */
  float jump_height;      /* metres of a full held jump: 3.2 */
  float coyote_time, jump_buffer; /* seconds: 0.1, 0.12 */
  int air_jumps;          /* extra jumps in the air: 0 */
  float air_jump_scale;   /* their strength relative to a ground jump: 0.9 */
  bool wall_jump;         /* wall slide + wall jump: false */
  float wall_slide_speed; /* max fall speed while sliding: 3 */
  float wall_jump_speed_x, wall_jump_speed_y; /* 8, 14.5 */
  float wall_jump_lock;   /* seconds the push away is not fought by the stick: 0.16 */
  float dash_speed;       /* 0 = no dash; m/s: 0 */
  float dash_time, dash_cooldown; /* 0.16, 0.35 */
  float swim_speed;       /* m/s on a stick: 4.5 */
  float swim_accel;       /* 18 */
  float swim_stroke;      /* upward speed of a jump press in water: 7 */
  float water_gravity;    /* gravity scale when fully submerged: 0.18 */
  float corner_correction; /* metres a rising head that clips a ceiling corner is nudged around it: 0.25 (0 = off). Copied to mover.corner_correction */
} cw_platformer2d_def;
cw_platformer2d_def cw_platformer2d_default_def(void);

typedef struct cw_platformer2d_input {
  float move_x;           /* -1..1 */
  float move_y;           /* -1..1, +1 up: swimming */
  bool jump_pressed;      /* went down this tick */
  bool jump_held;
  bool dash_pressed;
  bool down_held;         /* with jump_pressed on a one-way platform: drop through */
} cw_platformer2d_input;

enum {
  CW_PF_JUMP = 1 << 0,       /* ground jump (also coyote) */
  CW_PF_AIR_JUMP = 1 << 1,
  CW_PF_WALL_JUMP = 1 << 2,
  CW_PF_DASH = 1 << 3,
  CW_PF_LAND = 1 << 4,       /* touched down: land_speed is the impact */
  CW_PF_DROP = 1 << 5,       /* dropped through a one-way platform */
  CW_PF_ENTER_WATER = 1 << 6,
  CW_PF_LEAVE_WATER = 1 << 7,
  CW_PF_STROKE = 1 << 8,     /* a swim stroke */
  CW_PF_BONK = 1 << 9,       /* head hit a ceiling */
};

typedef struct cw_platformer2d {
  cw_mover2d body;        /* feet position, velocity, on_floor ... */
  cw_platformer2d_def def;
  uint32_t events;        /* CW_PF_* of the last step */
  int facing;             /* -1 or +1 */
  float coyote, buffer, wall_coyote, wall_lock, dash_t, dash_cd, drop_t;
  int wall_side;          /* +1 wall on the right, -1 on the left, 0 none (remembered for the grace window) */
  int air_jumps_left;
  bool dash_ready, was_wet;
  float land_speed;       /* downward speed at the last landing */
  /* set by the game before each step */
  float submerged;        /* 0 = dry .. 1 = fully under water */
  cw_vec2 current;        /* m/s the water carries the character */
  /* abilities the game can grant or take away at any time */
  bool can_dash, can_wall_jump;
  int max_air_jumps;
} cw_platformer2d;

/* def NULL = defaults. */
void cw_platformer2d_init(cw_platformer2d *p, cw_world2d w, const cw_platformer2d_def *def, cw_pos2 feet);
/* One fixed step: input, forces, then cw_mover2d_move. */
void cw_platformer2d_step(cw_platformer2d *p, const cw_platformer2d_input *in, float dt);
/* Teleport (a respawn): clears timers and velocity. */
void cw_platformer2d_teleport(cw_platformer2d *p, cw_pos2 feet);

#ifdef __cplusplus
}
#endif
#endif

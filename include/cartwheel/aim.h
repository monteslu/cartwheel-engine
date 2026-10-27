/* cartwheel/aim.h: stick aiming that feels right, for first and third person
 * cameras. Turns a raw stick vector into a turn for this tick:
 *
 *   - a radial dead zone, rescaled so the response starts at zero,
 *   - a response curve (power): below 1 is twitchy, above 1 gives fine
 *     control near the middle and full speed at the edge,
 *   - a top turn rate in radians a second per axis (pitch is usually slower),
 *   - acceleration: holding the stick near its edge ramps the turn rate up
 *     over ramp_seconds to (1 + ramp_gain) times, so a flick turns fast and
 *     small corrections stay precise,
 *   - an optional slow-down while the aim sits on a target (assist).
 *
 * Everything is + - * / sqrt and cartwheel/math.h's cw_pow, so the same
 * input gives the same turn on every host (replays and tests agree).
 *
 *   static cw_aim_state aim;                                  // one per player
 *   cw_aim_curve c = cw_aim_curve_default();
 *   cw_vec2 stick = cw_input_vector(p, look_l, look_r, look_u, look_d, CW_DEADZONE_RADIAL, 0);
 *   cw_vec2 turn = cw_aim_step(&aim, &c, stick, 1.0f / 60);   // radians this tick
 *   yaw -= turn.x;  pitch -= turn.y;
 */
#ifndef CW_AIM_H
#define CW_AIM_H

#include "math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_aim_curve {
  float deadzone;      /* 0.15: radial, of the stick's reach */
  float power;         /* 1.8: response curve exponent after the dead zone */
  float yaw_rate;      /* 3.4: radians a second at full stick, horizontally */
  float pitch_rate;    /* 2.4: the same vertically */
  float ramp_seconds;  /* 0.35: how long a held edge takes to reach the full boost (0 = no boost) */
  float ramp_gain;     /* 0.6: the boost: the turn rate grows by up to this share */
  float ramp_edge;     /* 0.9: stick length at or above which the boost builds */
  float assist;        /* 0: 0..1: slows the turn by up to this share while `on_target` is set */
} cw_aim_curve;
cw_aim_curve cw_aim_curve_default(void); /* the values noted above */

typedef struct cw_aim_state {
  float held; /* seconds the stick has been at its edge (the boost ramp) */
} cw_aim_state;

/* One tick: stick is x right, y down (as cw_input_vector gives it); the
 * result is radians to turn this tick, x to the right, y down. on_target
 * (0..1) is how much the crosshair sits on something to aim at (0 = none). */
cw_vec2 cw_aim_step(cw_aim_state *s, const cw_aim_curve *c, cw_vec2 stick, float dt, float on_target);

#ifdef __cplusplus
}
#endif
#endif

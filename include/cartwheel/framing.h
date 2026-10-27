/* cartwheel framing: a camera that keeps a set of things in view (two
 * fighters, a party of players, a boss and its arena) with a margin, from a
 * fixed viewing direction, and glides to the new framing instead of jumping.
 *
 *   cw_frame_params p = { .yaw = 0, .pitch = -0.15f, .fov_y = 0.7f, .aspect = 16.f / 9,
 *                         .margin = 0.12f, .min_dist = 4, .max_dist = 14 };
 *   cw_frame_fit(&p, pts, n, &fit);            // where the camera has to be
 *   cw_frame_follow(&state, &fit, dt, 6.0f);   // eased toward it, in fixed_update
 *
 * The fit is exact: with the margin at 0.1 the extreme points land 10 percent
 * of the half-view inside the frame edges (tests/unit/test_framing.c projects
 * them to check). Points are world positions; give each a radius to keep a
 * body, not just its pivot, in view. Everything is + - * / sqrt and cartwheel
 * math (cw_sincos), so a smoothed camera is the same on every host and may be
 * used by simulation-visible code; it is normally render-only.
 *
 * Direction: yaw turns about +Y (0 looks along -Z), pitch tilts (negative
 * looks down). The result is a position and a look-at point; pass them to
 * the camera's position and lookAt. */
#ifndef CW_FRAMING_H
#define CW_FRAMING_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_frame_point { float x, y, z, radius; } cw_frame_point;

typedef struct cw_frame_params {
  float yaw, pitch;       /* viewing direction, radians */
  float fov_y;            /* vertical field of view, radians */
  float aspect;           /* width / height of the view being framed */
  float margin;           /* 0..0.9: share of the half-view kept free at the edges */
  float min_dist, max_dist; /* the camera's distance from the look-at point (max 0 = no limit) */
} cw_frame_params;

typedef struct cw_frame_result {
  float pos[3];    /* camera position */
  float target[3]; /* look-at point (the middle of the points in the view plane) */
  float dist;      /* distance between them, after the limits */
  bool clamped;    /* a limit stopped the points from fitting */
} cw_frame_result;

/* Fit n points (n >= 1). Returns false for n < 1 or bad parameters. */
bool cw_frame_fit(const cw_frame_params *p, const cw_frame_point *pts, int n, cw_frame_result *out);

/* Smoothing: critically damped spring on the target and the distance. */
typedef struct cw_frame_state {
  float target[3], dist;
  float vel[3], dist_vel;
  bool init;
} cw_frame_state;
/* Moves `s` toward `fit` over dt seconds; speed is the spring's rate (6 is a
 * settle in about half a second; 0 or less snaps). The first call snaps.
 * Writes the camera position for the state's framing through `p`'s direction. */
void cw_frame_follow(cw_frame_state *s, const cw_frame_params *p, const cw_frame_result *fit, float dt, float speed,
                     cw_frame_result *out);

#ifdef __cplusplus
}
#endif
#endif

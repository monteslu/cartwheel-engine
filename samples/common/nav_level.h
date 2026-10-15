/* A navigation test level as navmesh input (cartwheel/nav.h): a 64 x 64 m
 * floor with two walls with two doorways each, a field of pillars,
 * and a ramp up to a raised deck. samples/nav_demo walks crowds through it
 * and tests/unit/test_nav.c builds and queries it. Header-only, deterministic:
 * the same calls in the same order on every host. */
#ifndef CW_SAMPLE_NAV_LEVEL_H
#define CW_SAMPLE_NAV_LEVEL_H

#include "cartwheel/nav.h"

typedef struct nav_level_box {
  t3_vec3 center, half;
} nav_level_box;

/* The level's solid boxes (floor first); returns how many (at most cap). */
static inline int nav_level_boxes(nav_level_box *out, int cap) {
  int n = 0;
#define NAV_LEVEL_BOX(cx, cy, cz, hx, hy, hz) \
  do { if (n < cap) out[n] = (nav_level_box){ { cx, cy, cz }, { hx, hy, hz } }; n++; } while (0)
  NAV_LEVEL_BOX(0, -0.5f, 0, 32, 0.5f, 32); /* floor, top at y = 0 */
  /* two walls across x = -10 and x = 10, each with doorways */
  for (int w = 0; w < 2; w++) {
    float x = w ? 10.0f : -10.0f;
    float d0 = w ? -18.0f : -6.0f, d1 = w ? 6.0f : 14.0f; /* doorway centers along z */
    /* wall pieces between z = -32 .. 32, leaving 8 m doorways at d0 and d1 */
    float ends[3] = { d0 - 4, d1 - 4, 32 };
    float starts[3] = { -32, d0 + 4, d1 + 4 };
    for (int k = 0; k < 3; k++) {
      float z0 = starts[k], z1 = ends[k];
      NAV_LEVEL_BOX(x, 1.5f, (z0 + z1) * 0.5f, 0.5f, 1.5f, (z1 - z0) * 0.5f);
    }
  }
  /* pillars between the walls */
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 6; j++) NAV_LEVEL_BOX(-6.0f + i * 4.0f, 1.5f, -25.0f + j * 10.0f, 0.6f, 1.5f, 0.6f);
  /* a raised deck in the far corner and the ramp up to it (made of steps a
   * little lower than the climb height) */
  NAV_LEVEL_BOX(24, 0.75f, 24, 6, 0.75f, 6);
  for (int s = 0; s < 6; s++) /* tops at 0.125 .. 1.375 m, the highest against the deck (z = 18) */
    NAV_LEVEL_BOX(24, 0.125f * (float)s + 0.0625f, 15.25f + 0.5f * (float)s, 3, 0.125f * (float)s + 0.0625f, 0.25f);
#undef NAV_LEVEL_BOX
  return n;
}

/* The level's triangles into g (area: CW_NAV_AREA_WALK). */
static inline void nav_level_geom(cw_nav_geom *g) {
  nav_level_box b[64];
  int n = nav_level_boxes(b, 64);
  for (int i = 0; i < n; i++) cw_nav_geom_add_box(g, b[i].center, b[i].half, CW_NAV_AREA_WALK);
}

#endif

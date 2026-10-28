/* A navigation test level for links (cartwheel/nav.h): a 40 x 24 m floor, a
 * ledge 1.2 m high that walking cannot climb (agent_max_climb is 0.9) and a
 * deck 3.5 m high. samples/nav_links bakes it with NavigationLink3D nodes
 * from its scene file; tests/unit/test_nav.c builds it with the same links
 * declared in code (nav_links_level_links). Header-only, deterministic. */
#ifndef CW_SAMPLE_NAV_LINKS_LEVEL_H
#define CW_SAMPLE_NAV_LINKS_LEVEL_H

#include "cartwheel/nav.h"

typedef struct nav_links_box {
  t3_vec3 center, half;
} nav_links_box;

#define NAV_LINKS_LEDGE_TOP 1.2f
#define NAV_LINKS_DECK_TOP 3.5f

/* The solid boxes (floor first); returns how many (at most cap). */
static inline int nav_links_boxes(nav_links_box *out, int cap) {
  static const nav_links_box b[] = {
    { { 0, -0.5f, 0 }, { 20, 0.5f, 12 } },      /* floor, top at y = 0 */
    { { 8, 0.6f, -7 }, { 4, 0.6f, 5 } },        /* the ledge: x 4..12, z -12..-2, top 1.2 */
    { { -12, 1.75f, -8 }, { 4, 1.75f, 4 } },    /* the deck: x -16..-8, z -12..-4, top 3.5 */
    { { -2, 1, -6 }, { 0.5f, 1, 6 } },          /* a low wall between them (z -12..0) */
  };
  int n = (int)(sizeof b / sizeof b[0]);
  for (int i = 0; i < n && i < cap; i++) out[i] = b[i];
  return n;
}

/* The links, as samples/nav_links/assets/main.cwscene declares them:
 *   1 a jump up the ledge's front (both ways),
 *   2 a jump pad on the floor to the deck (one way, cheaper than its length),
 *   3 a drop from the deck's front edge to the floor (one way). */
static inline int nav_links_level_links(cw_nav_link *out, int cap) {
  const cw_nav_link l[] = {
    { .start = { 8, 0, 0.2f }, .end = { 8, NAV_LINKS_LEDGE_TOP, -3.0f }, .bidirectional = true, .tag = CW_NAV_LINK_JUMP,
      .arc = 0.9f },
    { .start = { -12, 0, 1.5f }, .end = { -12, NAV_LINKS_DECK_TOP, -6 }, .tag = CW_NAV_LINK_PAD, .cost = 2,
      .duration = 0.9f, .arc = 2.5f },
    { .start = { -9.2f, NAV_LINKS_DECK_TOP, -9 }, .end = { -6.5f, 0, -9 }, .tag = CW_NAV_LINK_DROP, .arc = 0.4f },
  };
  int n = (int)(sizeof l / sizeof l[0]);
  for (int i = 0; i < n && i < cap; i++) out[i] = l[i];
  return n;
}

/* The level's triangles into g, with or without its links. */
static inline void nav_links_level_geom(cw_nav_geom *g, bool links) {
  nav_links_box b[8];
  int n = nav_links_boxes(b, 8);
  for (int i = 0; i < n; i++) cw_nav_geom_add_box(g, b[i].center, b[i].half, CW_NAV_AREA_WALK);
  if (!links) return;
  cw_nav_link l[8];
  int k = nav_links_level_links(l, 8);
  for (int i = 0; i < k; i++) cw_nav_geom_add_link(g, &l[i]);
}

#endif

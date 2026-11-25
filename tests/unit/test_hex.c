/* Hex grids (cartwheel/hex.h): every function against an independent answer (a breadth-first walk for distance and
 * reach, the nearest centre by brute force for a point's hex, adjacency for lines, a filter of the spiral for rings).
 * Control (CW_TEST_HEX_NAIVE=1): the late case checks the two mistakes hex code makes most, rounding q and r on
 * their own instead of as a cube and distance as max(|dq|, |dr|), against the same answers; both must fail. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/hex.h"
#include "test.h"

static bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* breadth-first distance on an unbounded grid (the independent answer for cw_hex_distance), within limit */
static int bfs_distance(cw_axial a, cw_axial b, int limit) {
  cw_hex_map *seen = cw_hex_map_new(sizeof(int));
  cw_axial q[4096];
  int head = 0, tail = 0, d0 = 0, found = -1;
  q[tail++] = a;
  cw_hex_map_set(seen, a, &d0);
  while (head < tail) {
    cw_axial h = q[head++];
    int d = *CW_HEX_MAP_GET(seen, h, int);
    if (cw_hex_eq(h, b)) { found = d; break; }
    if (d >= limit) continue;
    for (int i = 0; i < 6; i++) {
      cw_axial n = { h.q + (i == 0 || i == 1) - (i == 3 || i == 4), h.r - (i == 1 || i == 2) + (i == 4 || i == 5) };
      if (cw_hex_map_has(seen, n) || tail >= 4096) continue;
      int nd = d + 1;
      cw_hex_map_set(seen, n, &nd);
      q[tail++] = n;
    }
  }
  cw_hex_map_free(seen);
  return found;
}

static void test_coords(void) {
  cw_axial a = cw_hex_make(3, -5);
  CHECK_EQ_INT(cw_hex_s(a), 2);
  CHECK(cw_hex_eq(cw_hex_add(a, cw_hex_make(1, 1)), cw_hex_make(4, -4)));
  CHECK(cw_hex_eq(cw_hex_sub(a, cw_hex_make(1, 1)), cw_hex_make(2, -6)));
  CHECK(cw_hex_eq(cw_hex_scale(a, -2), cw_hex_make(-6, 10)));
  CHECK(!cw_hex_eq(a, cw_hex_make(3, -4)));
  /* the six directions: each one step, all different, opposite pairs cancel, any int wraps */
  for (int i = 0; i < 6; i++) {
    cw_axial d = cw_hex_direction(i);
    CHECK_EQ_INT(cw_hex_length(d), 1);
    CHECK(cw_hex_eq(cw_hex_add(d, cw_hex_direction(i + 3)), cw_hex_make(0, 0)));
    CHECK(cw_hex_eq(cw_hex_direction(i + 6), d) && cw_hex_eq(cw_hex_direction(i - 6), d));
    for (int j = i + 1; j < 6; j++) CHECK(!cw_hex_eq(d, cw_hex_direction(j)));
    CHECK(cw_hex_eq(cw_hex_neighbor(a, i), cw_hex_add(a, d)));
    /* a diagonal is two steps, the sum of the two directions it sits between */
    cw_axial g = cw_hex_diagonal(i);
    CHECK_EQ_INT(cw_hex_length(g), 2);
    CHECK(cw_hex_eq(g, cw_hex_add(cw_hex_direction(i), cw_hex_direction(i + 1))));
    CHECK(cw_hex_eq(cw_hex_diagonal_neighbor(a, i), cw_hex_add(a, g)));
    CHECK(cw_hex_eq(cw_hex_diagonal(i - 12), g));
  }
  CHECK(cw_hex_eq(cw_hex_direction(0), cw_hex_make(1, 0)) && cw_hex_eq(cw_hex_direction(1), cw_hex_make(1, -1)));
}

static void test_distance(void) {
  CHECK_EQ_INT(cw_hex_length(cw_hex_make(0, 0)), 0);
  CHECK_EQ_INT(cw_hex_distance(cw_hex_make(-2, 3), cw_hex_make(-2, 3)), 0);
  int checked = 0;
  for (int q = -4; q <= 4; q++)
    for (int r = -4; r <= 4; r++) {
      cw_axial b = cw_hex_make(q, r);
      int want = bfs_distance(cw_hex_make(0, 0), b, 12);
      CHECK_EQ_INT(cw_hex_distance(cw_hex_make(0, 0), b), want);
      CHECK_EQ_INT(cw_hex_distance(cw_hex_make(5, -7), cw_hex_add(cw_hex_make(5, -7), b)), want); /* the same anywhere */
      CHECK_EQ_INT(cw_hex_distance(b, cw_hex_make(0, 0)), want); /* and both ways */
      checked++;
    }
  CHECK_EQ_INT(checked, 81);
  /* direction_to: a step that way gets one closer; -1 for the same hex; a straight line gives its own direction */
  CHECK_EQ_INT(cw_hex_direction_to(cw_hex_make(1, 1), cw_hex_make(1, 1)), -1);
  for (int i = 0; i < 6; i++) CHECK_EQ_INT(cw_hex_direction_to(cw_hex_make(2, 2), cw_hex_add(cw_hex_make(2, 2), cw_hex_scale(cw_hex_direction(i), 3))), i);
  for (int q = -3; q <= 3; q++)
    for (int r = -3; r <= 3; r++) {
      cw_axial t = cw_hex_make(q, r);
      if (!q && !r) continue;
      int d = cw_hex_direction_to(cw_hex_make(0, 0), t);
      CHECK(d >= 0 && d < 6);
      CHECK_EQ_INT(cw_hex_distance(cw_hex_neighbor(cw_hex_make(0, 0), d), t), cw_hex_length(t) - 1);
    }
}

static void test_rotate_reflect(void) {
  cw_axial c = cw_hex_make(2, -1);
  for (int i = 0; i < 6; i++) {
    /* a direction turned one step is the next direction; six steps is a whole turn */
    CHECK(cw_hex_eq(cw_hex_rotate(cw_hex_direction(i), cw_hex_make(0, 0), 1), cw_hex_direction(i + 1)));
    CHECK(cw_hex_eq(cw_hex_rotate(cw_hex_direction(i), cw_hex_make(0, 0), -1), cw_hex_direction(i - 1)));
  }
  for (int q = -3; q <= 3; q++)
    for (int r = -3; r <= 3; r++) {
      cw_axial h = cw_hex_make(q, r);
      CHECK(cw_hex_eq(cw_hex_rotate(h, c, 6), h));
      CHECK(cw_hex_eq(cw_hex_rotate(h, c, 0), h));
      CHECK(cw_hex_eq(cw_hex_rotate(cw_hex_rotate(h, c, 2), c, -2), h));
      CHECK(cw_hex_eq(cw_hex_rotate(h, c, 7), cw_hex_rotate(h, c, 1)));
      CHECK_EQ_INT(cw_hex_distance(cw_hex_rotate(h, c, 1), c), cw_hex_distance(h, c)); /* turning keeps the distance */
      for (int ax = 0; ax < 3; ax++) {
        cw_axial m = cw_hex_reflect(h, c, (cw_hex_axis)ax);
        CHECK(cw_hex_eq(cw_hex_reflect(m, c, (cw_hex_axis)ax), h));
        CHECK_EQ_INT(cw_hex_distance(m, c), cw_hex_distance(h, c));
      }
      cw_axial v = cw_hex_sub(h, c);
      CHECK_EQ_INT(cw_hex_sub(cw_hex_reflect(h, c, CW_HEX_AXIS_Q), c).q, v.q); /* the axis coordinate stays */
      CHECK_EQ_INT(cw_hex_sub(cw_hex_reflect(h, c, CW_HEX_AXIS_R), c).r, v.r);
      CHECK_EQ_INT(cw_hex_s(cw_hex_sub(cw_hex_reflect(h, c, CW_HEX_AXIS_S), c)), cw_hex_s(v));
    }
  /* the two directions on a line of the axis swap: r = -1 holds directions 1 and 2, q = 1 holds 0 and 1 */
  CHECK(cw_hex_eq(cw_hex_reflect(cw_hex_direction(1), cw_hex_make(0, 0), CW_HEX_AXIS_R), cw_hex_direction(2)));
  CHECK(cw_hex_eq(cw_hex_reflect(cw_hex_direction(0), cw_hex_make(0, 0), CW_HEX_AXIS_Q), cw_hex_direction(1)));
  CHECK(cw_hex_eq(cw_hex_reflect(cw_hex_direction(0), cw_hex_make(0, 0), CW_HEX_AXIS_S), cw_hex_direction(5)));
}

static void test_round_lerp(void) {
  /* every centre rounds to itself, a point a little off rounds back */
  for (int q = -3; q <= 3; q++)
    for (int r = -3; r <= 3; r++) {
      CHECK(cw_hex_eq(cw_hex_round((cw_axialf){ (float)q, (float)r }), cw_hex_make(q, r)));
      CHECK(cw_hex_eq(cw_hex_round((cw_axialf){ q + 0.3f, r - 0.2f }), cw_hex_make(q, r)));
    }
  /* the case rounding q and r on their own gets wrong: (0.45, 0.45) is outside hex (0, 0) (s = -0.9), so one of q
   * and r must round up */
  cw_axial h = cw_hex_round((cw_axialf){ 0.45f, 0.45f });
  CHECK(cw_hex_eq(h, cw_hex_make(0, 1)) || cw_hex_eq(h, cw_hex_make(1, 0)));
  cw_axialf l = cw_hex_lerp(cw_hex_make(0, 0), cw_hex_make(4, -2), 0.25f);
  CHECK(near(l.q, 1, 1e-6f) && near(l.r, -0.5f, 1e-6f));
  l = cw_hex_lerp(cw_hex_make(-1, 2), cw_hex_make(3, 2), 1);
  CHECK(near(l.q, 3, 1e-6f) && near(l.r, 2, 1e-6f));
}

static void test_shapes(void) {
  cw_axial buf[512];
  /* lines: distance + 1 hexes, the ends right, each a neighbour of the last, every one on a shortest path */
  for (int q = -4; q <= 4; q++)
    for (int r = -4; r <= 4; r++) {
      cw_axial a = cw_hex_make(-1, 2), b = cw_hex_make(q, r);
      int n = cw_hex_line(a, b, buf, 512), d = cw_hex_distance(a, b);
      CHECK_EQ_INT(n, d + 1);
      CHECK(cw_hex_eq(buf[0], a) && cw_hex_eq(buf[n - 1], b));
      for (int i = 1; i < n; i++) {
        CHECK_EQ_INT(cw_hex_distance(buf[i - 1], buf[i]), 1);
        CHECK_EQ_INT(cw_hex_distance(a, buf[i]) + cw_hex_distance(buf[i], b), d);
      }
    }
  CHECK_EQ_INT(cw_hex_line(cw_hex_make(0, 0), cw_hex_make(5, 0), NULL, 0), 6);
  CHECK_EQ_INT(cw_hex_line(cw_hex_make(0, 0), cw_hex_make(5, 0), buf, 2), 6); /* the count past cap */
  /* rings: 6r hexes, all at distance r, all different, in order (each next to the last, the last next to the first) */
  cw_axial c = cw_hex_make(1, -3);
  for (int rad = 0; rad <= 5; rad++) {
    int n = cw_hex_ring(c, rad, buf, 512);
    CHECK_EQ_INT(n, rad ? 6 * rad : 1);
    for (int i = 0; i < n; i++) {
      CHECK_EQ_INT(cw_hex_distance(buf[i], c), rad);
      for (int j = i + 1; j < n; j++) CHECK(!cw_hex_eq(buf[i], buf[j]));
      if (rad) CHECK_EQ_INT(cw_hex_distance(buf[i], buf[(i + 1) % n]), 1);
    }
    if (rad) CHECK(cw_hex_eq(buf[0], cw_hex_add(c, cw_hex_scale(cw_hex_direction(4), rad))));
    /* every hex at that distance in a box around it is on the ring */
    int at = 0;
    for (int q = -6; q <= 6; q++)
      for (int r = -6; r <= 6; r++)
        if (cw_hex_distance(cw_hex_add(c, cw_hex_make(q, r)), c) == rad) at++;
    CHECK_EQ_INT(at, n);
  }
  CHECK_EQ_INT(cw_hex_ring(c, -1, buf, 512), 0);
  /* spirals: 1 + 3r(r+1), the centre first, ring by ring, no repeats */
  for (int rad = 0; rad <= 6; rad++) {
    int n = cw_hex_spiral(c, rad, buf, 512);
    CHECK_EQ_INT(n, cw_hex_area_count(rad));
    CHECK(cw_hex_eq(buf[0], c));
    for (int i = 1; i < n; i++) CHECK(cw_hex_distance(buf[i], c) >= cw_hex_distance(buf[i - 1], c));
    cw_hex_map *seen = cw_hex_map_new(0);
    for (int i = 0; i < n; i++) cw_hex_map_set(seen, buf[i], NULL);
    CHECK_EQ_INT(cw_hex_map_count(seen), n);
    cw_hex_map_free(seen);
  }
  CHECK_EQ_INT(cw_hex_area_count(-1), 0);
  CHECK_EQ_INT(cw_hex_spiral(c, 3, buf, 5), 37); /* the count past cap */
  CHECK_EQ_INT(cw_hex_spiral(c, 3, NULL, 0), 37);
}

/* a wall: the ring of radius 2 around (0, 0), with one gap at its first hex */
static bool open_ring(cw_axial h, void *user) {
  (void)user;
  cw_axial gap = cw_hex_scale(cw_hex_direction(4), 2);
  return cw_hex_length(h) != 2 || cw_hex_eq(h, gap);
}

static void test_reachable(void) {
  cw_axial buf[512];
  /* no walls: exactly the spiral's hexes, nearest first */
  int n = cw_hex_reachable(cw_hex_make(0, 0), 3, NULL, NULL, buf, 512);
  CHECK_EQ_INT(n, cw_hex_area_count(3));
  for (int i = 1; i < n; i++) CHECK(cw_hex_length(buf[i]) >= cw_hex_length(buf[i - 1]));
  /* a wall with one gap: inside costs the straight way, outside only through the gap; every hex a BFS of its own
   * finds in that many steps */
  n = cw_hex_reachable(cw_hex_make(0, 0), 4, open_ring, NULL, buf, 512);
  for (int i = 0; i < n; i++) {
    cw_axial h = buf[i];
    CHECK(open_ring(h, NULL));
    if (cw_hex_length(h) > 2) CHECK(cw_hex_distance(h, cw_hex_scale(cw_hex_direction(4), 2)) <= 2); /* through the gap */
  }
  CHECK_EQ_INT(n, 7 + 1 + 3 + 7); /* radius 1 (7), the gap, then 3 hexes one step beyond it and 7 two steps beyond */
  CHECK_EQ_INT(cw_hex_reachable(cw_hex_make(0, 0), 4, open_ring, NULL, buf, 3), n); /* the count past cap */
  CHECK_EQ_INT(cw_hex_reachable(cw_hex_make(9, 9), 0, NULL, NULL, buf, 512), 1);
}

static void test_offset(void) {
  for (int k = 0; k < 4; k++)
    for (int q = -5; q <= 5; q++)
      for (int r = -5; r <= 5; r++) {
        int col, row;
        cw_axial h = cw_hex_make(q, r);
        cw_hex_to_offset(h, (cw_hex_offset_kind)k, &col, &row);
        CHECK(cw_hex_eq(cw_hex_from_offset(col, row, (cw_hex_offset_kind)k), h));
      }
  /* the conventions as drawn: odd-r pushes odd rows right, so (0,1) in offset is axial (0,1) with q shifted */
  CHECK(cw_hex_eq(cw_hex_from_offset(0, 1, CW_HEX_ODD_R), cw_hex_make(0, 1)));
  CHECK(cw_hex_eq(cw_hex_from_offset(0, 2, CW_HEX_ODD_R), cw_hex_make(-1, 2)));
  CHECK(cw_hex_eq(cw_hex_from_offset(0, 1, CW_HEX_EVEN_R), cw_hex_make(-1, 1)));
  CHECK(cw_hex_eq(cw_hex_from_offset(2, 0, CW_HEX_ODD_Q), cw_hex_make(2, -1)));
  CHECK(cw_hex_eq(cw_hex_from_offset(1, 0, CW_HEX_EVEN_Q), cw_hex_make(1, -1)));
  /* a rectangle of offset cells: each cell's right-hand neighbour in the row is its axial direction 0 (pointy) */
  for (int row = -3; row <= 3; row++)
    for (int col = -3; col <= 3; col++)
      CHECK(cw_hex_eq(cw_hex_from_offset(col + 1, row, CW_HEX_ODD_R), cw_hex_neighbor(cw_hex_from_offset(col, row, CW_HEX_ODD_R), 0)));
}

/* the nearest centre by brute force: the independent answer for a point's hex */
static cw_axial nearest(const cw_hex_layout *l, cw_vec2 p) {
  cw_axial best = { 0, 0 };
  float bd = 1e30f;
  for (int q = -12; q <= 12; q++)
    for (int r = -12; r <= 12; r++) {
      cw_vec2 c = cw_hex_to_point(l, cw_hex_make(q, r));
      float d = ((p.x - c.x) * (p.x - c.x)) / (l->size.x * l->size.x) + ((p.y - c.y) * (p.y - c.y)) / (l->size.y * l->size.y);
      if (d < bd) bd = d, best = cw_hex_make(q, r);
    }
  return best;
}

static void test_layout(void) {
  cw_hex_layout pointy = { CW_HEX_POINTY, { 2, 2 }, { 10, -4 } }, flat = { CW_HEX_FLAT, { 1.5f, 1 }, { -3, 7 } };
  const cw_hex_layout *ls[2] = { &pointy, &flat };
  /* known places: a pointy hex's east neighbour is sqrt(3) size along x, a flat hex's is 1.5 size along x */
  cw_vec2 p = cw_hex_to_point(&pointy, cw_hex_make(1, 0));
  CHECK(near(p.x, 10 + 2 * 1.7320508f, 1e-4f) && near(p.y, -4, 1e-5f));
  p = cw_hex_to_point(&pointy, cw_hex_make(0, 1));
  CHECK(near(p.x, 10 + 1.7320508f, 1e-4f) && near(p.y, -4 + 3, 1e-5f));
  p = cw_hex_to_point(&flat, cw_hex_make(1, 0));
  CHECK(near(p.x, -3 + 2.25f, 1e-5f) && near(p.y, 7 + 0.8660254f, 1e-4f));
  p = cw_hex_to_point(&flat, cw_hex_make(0, 1));
  CHECK(near(p.x, -3, 1e-5f) && near(p.y, 7 + 1.7320508f, 1e-4f));
  /* a zero size is taken as 1 */
  cw_hex_layout unit = { CW_HEX_POINTY, { 0, 0 }, { 0, 0 } };
  CHECK(near(cw_hex_to_point(&unit, cw_hex_make(0, 1)).y, 1.5f, 1e-6f));
  for (int li = 0; li < 2; li++) {
    const cw_hex_layout *l = ls[li];
    for (int q = -4; q <= 4; q++)
      for (int r = -4; r <= 4; r++) {
        cw_axial h = cw_hex_make(q, r);
        cw_vec2 c = cw_hex_to_point(l, h);
        CHECK(cw_hex_eq(cw_hex_from_point(l, c), h));
        cw_axialf f = cw_hex_from_point_frac(l, c);
        CHECK(near(f.q, (float)q, 1e-4f) && near(f.r, (float)r, 1e-4f));
        /* corners: on the hex's edge, shared with the neighbours that touch them, a point just inside each is in h */
        cw_vec2 k[6];
        cw_hex_corners(l, h, k);
        for (int i = 0; i < 6; i++) {
          cw_vec2 one = cw_hex_corner(l, h, i);
          CHECK(near(one.x, k[i].x, 1e-6f) && near(one.y, k[i].y, 1e-6f));
          float dx = (k[i].x - c.x) / l->size.x, dy = (k[i].y - c.y) / l->size.y;
          CHECK(near(dx * dx + dy * dy, 1, 1e-4f));
          cw_vec2 in = { c.x + (k[i].x - c.x) * 0.9f, c.y + (k[i].y - c.y) * 0.9f };
          CHECK(cw_hex_eq(cw_hex_from_point(l, in), h));
        }
        cw_vec2 k6 = cw_hex_corner(l, h, 6);
        CHECK(near(k6.x, k[0].x, 1e-6f) && near(k6.y, k[0].y, 1e-6f));
      }
    /* a grid of points over the board: the hex found is the nearest centre (a brute-force search) */
    int agree = 0, total = 0;
    for (int i = 0; i < 40; i++)
      for (int j = 0; j < 40; j++) {
        cw_vec2 q = { l->origin.x - 9 + i * 0.4513f, l->origin.y - 9 + j * 0.4471f };
        total++;
        agree += cw_hex_eq(cw_hex_from_point(l, q), nearest(l, q));
      }
    CHECK_EQ_INT(agree, total);
    /* 3D: the same layout on the ground, x to X and y to Z, at the given height */
    t3_vec3 w = cw_hex_to_world(l, cw_hex_make(2, -1), 0.25f);
    cw_vec2 p2 = cw_hex_to_point(l, cw_hex_make(2, -1));
    CHECK(w.x == p2.x && w.z == p2.y && w.y == 0.25f);
    CHECK(cw_hex_eq(cw_hex_from_world(l, (t3_vec3){ w.x, 17, w.z }), cw_hex_make(2, -1)));
    cw_axialf wf = cw_hex_from_world_frac(l, w);
    CHECK(near(wf.q, 2, 1e-4f) && near(wf.r, -1, 1e-4f));
    t3_vec3 wc = cw_hex_world_corner(l, cw_hex_make(2, -1), 3, -1);
    cw_vec2 pc = cw_hex_corner(l, cw_hex_make(2, -1), 3);
    CHECK(wc.x == pc.x && wc.z == pc.y && wc.y == -1);
  }
  /* pointy: corner 1 is straight below the centre (y down), flat: corner 0 straight right */
  cw_vec2 c0 = cw_hex_to_point(&pointy, cw_hex_make(0, 0)), k1 = cw_hex_corner(&pointy, cw_hex_make(0, 0), 1);
  CHECK(near(k1.x, c0.x, 1e-5f) && near(k1.y, c0.y + 2, 1e-5f));
  cw_vec2 f0 = cw_hex_to_point(&flat, cw_hex_make(0, 0)), fk = cw_hex_corner(&flat, cw_hex_make(0, 0), 0);
  CHECK(near(fk.x, f0.x + 1.5f, 1e-5f) && near(fk.y, f0.y, 1e-5f));
  /* cross-host: a few positions and the hexes found for off-centre points, folded */
  for (int i = 0; i < 16; i++) {
    cw_vec2 pt = cw_hex_to_point(&flat, cw_hex_make(i - 8, 3 - i / 2));
    cw_axial hh = cw_hex_from_point(&pointy, (cw_vec2){ pt.x * 1.37f, pt.y * 0.71f });
    FOLD(pt);
    FOLD(hh);
  }
}

typedef struct { int owner, height; } tile;

static void test_map(void) {
  cw_hex_map *m = cw_hex_map_new(sizeof(tile));
  CHECK_EQ_INT(cw_hex_map_count(m), 0);
  CHECK_EQ_INT((int)cw_hex_map_value_size(m), (int)sizeof(tile));
  CHECK(cw_hex_map_get(m, cw_hex_make(0, 0)) == NULL);
  CHECK(!cw_hex_map_remove(m, cw_hex_make(0, 0)));
  cw_axial all[512];
  int n = cw_hex_spiral(cw_hex_make(0, 0), 8, all, 512); /* 217: grows the table several times */
  for (int i = 0; i < n; i++) {
    tile t = { i, -i };
    tile *s = cw_hex_map_set(m, all[i], &t);
    CHECK(s && s->owner == i);
  }
  CHECK_EQ_INT(cw_hex_map_count(m), n);
  bool ok = true;
  for (int i = 0; i < n; i++) {
    tile *t = CW_HEX_MAP_GET(m, all[i], tile);
    ok &= t && t->owner == i && t->height == -i;
  }
  CHECK(ok);
  CHECK(!cw_hex_map_has(m, cw_hex_make(9, 0)));
  /* replace keeps the count; NULL stores zeros */
  cw_hex_map_set(m, all[5], NULL);
  CHECK_EQ_INT(cw_hex_map_count(m), n);
  CHECK(CW_HEX_MAP_GET(m, all[5], tile)->owner == 0);
  /* remove every other one: the rest are all still found (backward-shift removal keeps probe chains) */
  for (int i = 0; i < n; i += 2) CHECK(cw_hex_map_remove(m, all[i]));
  ok = true;
  for (int i = 0; i < n; i++) ok &= cw_hex_map_has(m, all[i]) == (i % 2 == 1);
  CHECK(ok);
  CHECK_EQ_INT(cw_hex_map_count(m), n / 2);
  /* keys sorted by r then q, whatever order they went in; the hash the same for the same contents */
  cw_axial keys[512];
  int k = cw_hex_map_keys(m, keys, 512);
  CHECK_EQ_INT(k, n / 2);
  for (int i = 1; i < k; i++) CHECK(keys[i - 1].r < keys[i].r || (keys[i - 1].r == keys[i].r && keys[i - 1].q < keys[i].q));
  CHECK_EQ_INT(cw_hex_map_keys(m, NULL, 0), k);
  cw_hex_map *m2 = cw_hex_map_new(sizeof(tile));
  for (int i = n - 1; i >= 0; i--)
    if (i % 2) {
      tile t = { i, -i };
      cw_hex_map_set(m2, all[i], i == 5 ? NULL : &t); /* as m: hex 5 was set to zeros */
    }
  uint32_t h1 = cw_hex_map_hash(m, CW_HASH32_INIT), h2 = cw_hex_map_hash(m2, CW_HASH32_INIT);
  CHECK(h1 == h2);
  CW_HEX_MAP_GET(m2, all[1], tile)->height = 99;
  CHECK(cw_hex_map_hash(m2, CW_HASH32_INIT) != h1); /* a value change shows */
  FOLD(h1);
  cw_hex_map_clear(m);
  CHECK_EQ_INT(cw_hex_map_count(m), 0);
  CHECK(!cw_hex_map_has(m, all[1]));
  CHECK_EQ_INT(cw_hex_map_hash(m, 7), 7);
  cw_hex_map_set(m, all[1], NULL);
  CHECK(cw_hex_map_has(m, all[1]));
  cw_hex_map_free(m);
  cw_hex_map_free(m2);
  cw_hex_map_free(NULL);
  /* a set (value size 0) */
  cw_hex_map *set = cw_hex_map_new(0);
  CHECK(cw_hex_map_set(set, cw_hex_make(-3, 4), NULL) != NULL);
  CHECK(cw_hex_map_get(set, cw_hex_make(-3, 4)) != NULL && cw_hex_map_get(set, cw_hex_make(3, 4)) == NULL);
  CHECK_EQ_INT(cw_hex_map_count(set), 1);
  cw_hex_map_free(set);
}

/* the control's two mistakes */
static cw_axial naive_round(cw_axialf f) { return cw_hex_make((int)roundf(f.q), (int)roundf(f.r)); }
static int naive_distance(cw_axial a, cw_axial b) {
  int dq = abs(a.q - b.q), dr = abs(a.r - b.r);
  return dq > dr ? dq : dr;
}

/* late (cw_test_env is read from frame 2): the point-to-hex and distance answers once more, through the real calls
 * or, as the control, through the naive ones, which must fail */
void cw_test_late(void) {
  bool naive = cw_test_env("CW_TEST_HEX_NAIVE") != NULL;
  cw_hex_layout l = { CW_HEX_POINTY, { 1, 1 }, { 0, 0 } };
  int wrong_hex = 0, wrong_dist = 0;
  for (int i = 0; i < 30; i++)
    for (int j = 0; j < 30; j++) {
      cw_vec2 p = { -6 + i * 0.4137f, -6 + j * 0.4119f };
      cw_axial got = naive ? naive_round(cw_hex_from_point_frac(&l, p)) : cw_hex_from_point(&l, p);
      wrong_hex += !cw_hex_eq(got, nearest(&l, p));
    }
  for (int q = -4; q <= 4; q++)
    for (int r = -4; r <= 4; r++) {
      cw_axial b = cw_hex_make(q, r);
      int got = naive ? naive_distance(cw_hex_make(0, 0), b) : cw_hex_distance(cw_hex_make(0, 0), b);
      wrong_dist += got != bfs_distance(cw_hex_make(0, 0), b, 12);
    }
  if (naive) cw_info("hex control: %d points in the wrong hex, %d wrong distances", wrong_hex, wrong_dist);
  CHECK_EQ_INT(wrong_hex, 0);
  CHECK_EQ_INT(wrong_dist, 0);
}

CW_TEST_SUITE("hex", { "coords", test_coords }, { "distance", test_distance }, { "rotate_reflect", test_rotate_reflect },
              { "round_lerp", test_round_lerp }, { "shapes", test_shapes }, { "reachable", test_reachable },
              { "offset", test_offset }, { "layout", test_layout }, { "map", test_map })

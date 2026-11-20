/* 2D navigation (cartwheel/nav2d.h): grids and obstacles, jump point search held to plain A* on random maps,
 * straightened paths, budgets, cell costs, flow fields, batched paths, the crowd, and the nodes on a tile map.
 * Everything that must agree across hosts is folded into the checksum. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/nav2d.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene2d.h"
#include "test.h"

#define DT (1.0f / 60)

/* the engine's test hook: the cost of the cell path by jump points, or by plain A* over every cell */
uint32_t cw__navgrid_path_cost(cw_navgrid g, int sx, int sy, int ex, int ey, bool plain, int *expanded);

static cw_navgrid grid_from(const char *const *rows, int h, float cell) {
  int w = (int)strlen(rows[0]);
  cw_navgrid g = cw_navgrid_new(&(cw_navgrid_desc){ .width = w, .height = h, .cell = { cell, cell } });
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (rows[y][x] == '#') cw_navgrid_set_cost(g, x, y, 0);
      else if (rows[y][x] == '~') cw_navgrid_set_cost(g, x, y, 5);
  return g;
}

static void t_grid(void) {
  CHECK(!cw_navgrid_valid(cw_navgrid_new(&(cw_navgrid_desc){ .width = 0, .height = 4 })));
  cw_navgrid g = cw_navgrid_new(&(cw_navgrid_desc){ .width = 8, .height = 6, .cell = { 16, 16 }, .origin = { 32, 0 } });
  CHECK(cw_navgrid_valid(g));
  int w, h, x, y;
  cw_navgrid_size(g, &w, &h);
  CHECK(w == 8 && h == 6);
  cw_navgrid_cell_at(g, cw_v2(32 + 40, 20), &x, &y);
  CHECK(x == 2 && y == 1);
  cw_vec2 c = cw_navgrid_cell_center(g, 2, 1);
  CHECK(c.x == 32 + 40 && c.y == 24);
  CHECK(cw_navgrid_walkable(g, 3, 3) && !cw_navgrid_walkable(g, -1, 0) && !cw_navgrid_walkable(g, 8, 0));
  uint32_t v0 = cw_navgrid_version(g);
  uint64_t h0 = cw_navgrid_hash(g);
  cw_navgrid_set_cost(g, 3, 3, 0);
  CHECK(!cw_navgrid_walkable(g, 3, 3) && cw_navgrid_version(g) != v0 && cw_navgrid_hash(g) != h0);
  cw_navgrid_set_cost(g, 3, 3, 0); /* no change: no new version */
  CHECK(cw_navgrid_version(g) == v0 + 1);
  cw_navgrid_set_cost(g, 3, 3, 1);
  CHECK(cw_navgrid_hash(g) == h0);
  CHECK(cw_navgrid_get_stats(g).jump_points && cw_navgrid_get_stats(g).walkable == 48);
  cw_navgrid_set_cost(g, 1, 1, 7);
  CHECK(!cw_navgrid_get_stats(g).jump_points && cw_navgrid_cost(g, 1, 1) == 7);
  cw_navgrid_set_cost(g, 1, 1, 1);
  /* obstacles block the cells whose middle they cover, and add up */
  uint32_t a = cw_navgrid_obstacle_add_circle(g, cw_v2(32 + 72, 40), 12); /* the middle of cell (4, 2) */
  uint32_t b = cw_navgrid_obstacle_add_rect(g, cw_v2(32 + 72, 40), cw_v2(20, 4), 0);
  CHECK(a && b && a != b);
  CHECK(!cw_navgrid_walkable(g, 4, 2) && !cw_navgrid_walkable(g, 3, 2) && !cw_navgrid_walkable(g, 5, 2));
  CHECK(cw_navgrid_walkable(g, 4, 1) && cw_navgrid_walkable(g, 2, 2));
  CHECK(cw_navgrid_get_stats(g).walkable == 45);
  CHECK(cw_navgrid_obstacle_remove(g, b));
  CHECK(!cw_navgrid_walkable(g, 4, 2) && cw_navgrid_walkable(g, 3, 2));
  CHECK(cw_navgrid_obstacle_remove(g, a) && !cw_navgrid_obstacle_remove(g, a));
  CHECK(cw_navgrid_walkable(g, 4, 2) && cw_navgrid_hash(g) == h0);
  /* a rect turned a quarter covers the column instead of the row */
  uint32_t r = cw_navgrid_obstacle_add_rect(g, cw_v2(32 + 72, 40), cw_v2(20, 4), 1.5707963f);
  CHECK(!cw_navgrid_walkable(g, 4, 1) && !cw_navgrid_walkable(g, 4, 3) && cw_navgrid_walkable(g, 3, 2));
  cw_navgrid_obstacle_remove(g, r);
  /* nearest: the point itself in a free cell, else the middle of the nearest free cell */
  cw_navgrid_set_cost(g, 0, 0, 0);
  cw_vec2 out;
  CHECK(cw_navgrid_nearest(g, cw_v2(32 + 20, 20), 0, &out) && out.x == 32 + 20 && out.y == 20);
  CHECK(cw_navgrid_nearest(g, cw_v2(32 + 12, 4), 0, &out) && out.x == 32 + 24 && out.y == 8); /* (1, 0): nearer than (0, 1) */
  CHECK(!cw_navgrid_nearest(g, cw_v2(-500, -500), 32, &out));
  cw_navgrid_free(g);
  CHECK(!cw_navgrid_valid(g));
}

/* jump point search must find paths of exactly plain A*'s cost, on maps nobody chose */
static void t_jps_equals_astar(void) {
  cw_rng rng;
  cw_rng_seed(&rng, 20261119);
  int64_t jps_expanded = 0, astar_expanded = 0;
  int reachable = 0;
  for (int m = 0; m < 12; m++) {
    int w = 24 + m * 7, h = 20 + m * 5;
    uint32_t density = 10 + (uint32_t)m * 3; /* percent blocked: 10..43 */
    cw_navgrid g = cw_navgrid_new(&(cw_navgrid_desc){ .width = w, .height = h });
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        uint32_t roll = cw_rng_u32(&rng) % 100;
        if (roll < density) cw_navgrid_set_cost(g, x, y, 0);
      }
    for (int q = 0; q < 60; q++) {
      uint32_t r0 = cw_rng_u32(&rng), r1 = cw_rng_u32(&rng), r2 = cw_rng_u32(&rng), r3 = cw_rng_u32(&rng);
      int sx = (int)(r0 % (uint32_t)w), sy = (int)(r1 % (uint32_t)h), ex = (int)(r2 % (uint32_t)w), ey = (int)(r3 % (uint32_t)h);
      int ej = 0, ea = 0;
      uint32_t cj = cw__navgrid_path_cost(g, sx, sy, ex, ey, false, &ej);
      uint32_t ca = cw__navgrid_path_cost(g, sx, sy, ex, ey, true, &ea);
      CHECK_EQ_INT(cj, ca);
      FOLD(cj);
      if (ca != 0xffffffffu) {
        reachable++;
        jps_expanded += ej, astar_expanded += ea;
      }
    }
    cw_navgrid_free(g);
  }
  CHECK(reachable > 200);
  CHECK(jps_expanded < astar_expanded); /* fewer even on noise, the worst maps for it */
  /* the point of it: rooms. An open hall with a few pillars */
  cw_navgrid hall = cw_navgrid_new(&(cw_navgrid_desc){ .width = 96, .height = 96 });
  for (int k = 0; k < 6; k++)
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) cw_navgrid_set_cost(hall, 12 + k * 14 + x, 20 + (k % 3) * 22 + y, 0);
  int ej = 0, ea = 0;
  uint32_t cj = cw__navgrid_path_cost(hall, 2, 3, 93, 90, false, &ej), ca = cw__navgrid_path_cost(hall, 2, 3, 93, 90, true, &ea);
  CHECK_EQ_INT(cj, ca);
  CHECK(ej * 4 < ea);
  cw_info("nav2d: an open hall: %d jump point expansions against %d for A*", ej, ea);
  cw_navgrid_free(hall);
  cw_info("nav2d: %d reachable pairs, %lld jump point expansions against %lld for A*", reachable, (long long)jps_expanded,
          (long long)astar_expanded);
}

static const char *const MAZE[] = {
  "....................",
  "....................",
  "..########..........",
  "..#......#..........",
  "..#......#....#.....",
  "..#......#....#.....",
  "..#..##########.....",
  "..#.................",
  "..#.................",
  "....................",
};

static bool legs_clear(cw_navgrid g, const cw_vec2 *p, int n) {
  for (int i = 1; i < n; i++)
    if (!cw_navgrid_raycast(g, p[i - 1], p[i], 0, NULL)) return false;
  return true;
}

static void t_paths(void) {
  cw_navgrid g = grid_from(MAZE, 10, 16);
  cw_vec2 pts[32], from = cw_v2(5 * 16 + 3, 4 * 16 + 5), to = cw_v2(17 * 16 + 9, 1 * 16 + 2);
  bool partial = true;
  /* out of the pocket (its only opening is at the bottom left), round the walls */
  int n = cw_navgrid_find_path(g, from, to, NULL, pts, 32, &partial);
  CHECK(n >= 3 && n <= 8 && !partial);
  CHECK(pts[0].x == from.x && pts[0].y == from.y && pts[n - 1].x == to.x && pts[n - 1].y == to.y);
  CHECK(legs_clear(g, pts, n));
  float len = cw_navgrid_path_length(pts, n);
  CHECK(len > 300 && len < 520);
  for (int i = 0; i < n; i++) FOLD(pts[i]);
  /* the same path with its corners left on cell middles is longer or equal, and has more corners */
  cw_vec2 cells[64];
  int nc = cw_navgrid_find_path(g, from, to, &(cw_navgrid_query){ .keep_cells = true }, cells, 64, &partial);
  CHECK(nc > n && !partial && legs_clear(g, cells, nc));
  CHECK(cw_navgrid_path_length(cells, nc) >= len - 0.01f);
  /* straight line of sight: two waypoints */
  n = cw_navgrid_find_path(g, cw_v2(8, 8), cw_v2(300, 20), NULL, pts, 32, &partial);
  CHECK_EQ_INT(n, 2);
  /* a wide agent keeps its straightened legs off the corners */
  n = cw_navgrid_find_path(g, from, to, &(cw_navgrid_query){ .agent_radius = 6 }, pts, 32, &partial);
  CHECK(n >= 3 && !partial);
  for (int i = 1; i < n - 1; i++) CHECK(cw_navgrid_raycast(g, pts[i], pts[i + 1], 0, NULL));
  /* a goal inside a wall ends at the nearest free cell's middle */
  n = cw_navgrid_find_path(g, cw_v2(8, 8), cw_v2(5 * 16 + 8, 2 * 16 + 8), NULL, pts, 32, &partial);
  CHECK(n >= 2 && !partial && pts[n - 1].y == 1 * 16 + 8);
  /* too few waypoints: cut short, and it says so */
  n = cw_navgrid_find_path(g, from, to, NULL, pts, 2, &partial);
  CHECK(n == 2 && partial);
  /* a budget of a few expansions ends near the start, partial */
  n = cw_navgrid_find_path(g, from, to, &(cw_navgrid_query){ .max_iters = 3 }, pts, 32, &partial);
  CHECK(n >= 1 && partial);
  /* sealed off: the pocket closed, the path ends at the cell inside nearest the goal */
  cw_navgrid_set_cost(g, 3, 7, 0);
  cw_navgrid_set_cost(g, 3, 8, 0);
  cw_navgrid_set_cost(g, 3, 9, 0);
  cw_navgrid_set_cost(g, 4, 7, 0);
  for (int x = 4; x < 20; x++) cw_navgrid_set_cost(g, x, 9, 0);
  for (int y = 6; y < 10; y++) cw_navgrid_set_cost(g, 19, y, 0);
  cw_navgrid_set_cost(g, 15, 7, 0), cw_navgrid_set_cost(g, 15, 8, 0);
  n = cw_navgrid_find_path(g, from, to, NULL, pts, 32, &partial);
  CHECK(n >= 1 && partial);
  CHECK(pts[n - 1].x != to.x);
  /* raycast: the wall stops it short and reports where */
  cw_vec2 hit;
  CHECK(!cw_navgrid_raycast(g, cw_v2(8, 3 * 16 + 8), cw_v2(8 * 16, 3 * 16 + 8), 0, &hit));
  CHECK(hit.x > 30 && hit.x <= 32.01f);
  FOLD(hit);
  /* batches give the answers of single queries */
  cw_navgrid_path_req reqs[16];
  cw_vec2 many[16 * 8];
  int counts[16];
  for (int i = 0; i < 16; i++) reqs[i] = (cw_navgrid_path_req){ cw_v2(8 + (float)i * 4, 8), cw_v2(300 - (float)i * 9, 20 + (float)i * 7) };
  cw_navgrid_find_paths(g, reqs, 16, NULL, many, 8, counts);
  for (int i = 0; i < 16; i++) {
    cw_vec2 one[8];
    int k = cw_navgrid_find_path(g, reqs[i].from, reqs[i].to, NULL, one, 8, NULL);
    CHECK_EQ_INT(k, counts[i]);
    CHECK(k > 0 && memcmp(one, many + i * 8, sizeof(cw_vec2) * (size_t)k) == 0);
  }
  cw_navgrid_free(g);
}

static void t_costs(void) {
  /* a swamp across the middle with one dry crossing far to the right: wading straight through 3 cells of
   * cost 5 (150) is dearer than the 12-cell detour? No: the detour is longer here, so it wades; with the
   * swamp deeper it walks round */
  static const char *const rows[] = {
    "..........",
    "..........",
    "~~~~~~~~~.",
    "~~~~~~~~~.",
    "~~~~~~~~~.",
    "..........",
    "..........",
  };
  cw_navgrid g = grid_from(rows, 7, 10);
  CHECK(!cw_navgrid_get_stats(g).jump_points);
  /* from above the left end to below it: straight down costs 3 * 50 + 30; round the right end 2 * 9 cells */
  uint32_t straight = cw__navgrid_path_cost(g, 0, 1, 0, 5, true, NULL);
  CHECK_EQ_INT(straight, 3 * 50 + 10);
  /* near the dry column the detour wins */
  uint32_t near_gap = cw__navgrid_path_cost(g, 7, 1, 7, 5, true, NULL);
  CHECK(near_gap < 3 * 50 + 10);
  CHECK_EQ_INT(near_gap, 14 * 2 + 10 * 4); /* east, down-east into the dry column, down it, down-west, west */
  cw_vec2 pts[16];
  int n = cw_navgrid_find_path(g, cw_v2(75, 15), cw_v2(75, 55), NULL, pts, 16, NULL);
  bool dry = true;
  for (int i = 0; i < n; i++) dry = dry && !(pts[i].y > 20 && pts[i].y < 50 && pts[i].x < 90);
  CHECK(n >= 3 && dry);
  cw_navgrid_free(g);
  /* four neighbours only: a diagonal trip costs both legs */
  cw_navgrid g4 = cw_navgrid_new(&(cw_navgrid_desc){ .width = 8, .height = 8, .no_diagonals = true });
  CHECK(!cw_navgrid_get_stats(g4).jump_points);
  CHECK_EQ_INT(cw__navgrid_path_cost(g4, 0, 0, 5, 5, false, NULL), 100);
  cw_navgrid_free(g4);
}

static void t_flow(void) {
  cw_navgrid g = grid_from(MAZE, 10, 16);
  cw_flowfield f = cw_flowfield_new(g);
  CHECK(cw_flowfield_valid(f) && cw_flowfield_stale(f));
  cw_vec2 goal = cw_v2(17 * 16 + 8, 1 * 16 + 8);
  cw_flowfield_build(f, &goal, 1);
  CHECK(!cw_flowfield_stale(f));
  CHECK(cw_flowfield_distance(f, goal) == 0);
  cw_vec2 d = cw_flowfield_direction(f, goal);
  CHECK(d.x == 0 && d.y == 0);
  d = cw_flowfield_direction(f, cw_v2(10 * 16 + 8, 1 * 16 + 8)); /* the same row, to its left: east */
  CHECK(d.x == 1 && d.y == 0);
  d = cw_flowfield_direction(f, cw_v2(4 * 16 + 8, 2 * 16 + 8)); /* inside a wall */
  CHECK(d.x == 0 && d.y == 0);
  CHECK(cw_flowfield_distance(f, cw_v2(4 * 16 + 8, 2 * 16 + 8)) < 0);
  /* every free cell's distance is the cost of the shortest path from it, and walking the field cell by cell
   * reaches the goal in no more steps than there are cells */
  int walked = 0;
  for (int y = 0; y < 10; y++)
    for (int x = 0; x < 20; x++) {
      if (!cw_navgrid_walkable(g, x, y)) continue;
      cw_vec2 p = cw_navgrid_cell_center(g, x, y);
      uint32_t cost = cw__navgrid_path_cost(g, x, y, 17, 1, true, NULL);
      float dist = cw_flowfield_distance(f, p);
      CHECK(dist >= 0 && (uint32_t)(dist / 1.6f + 0.5f) == cost);
      int steps = 0;
      while (steps < 200) {
        cw_vec2 dir = cw_flowfield_direction(f, p);
        if (dir.x == 0 && dir.y == 0) break;
        int cx, cy;
        cw_navgrid_cell_at(g, p, &cx, &cy);
        p = cw_navgrid_cell_center(g, cx + (dir.x > 0) - (dir.x < 0), cy + (dir.y > 0) - (dir.y < 0));
        steps++;
      }
      int cx, cy;
      cw_navgrid_cell_at(g, p, &cx, &cy);
      CHECK(cx == 17 && cy == 1);
      walked++;
    }
  CHECK(walked > 150);
  uint64_t h1 = cw_flowfield_hash(f);
  FOLD(h1);
  /* the grid changes: stale until built again, and the field then differs */
  cw_navgrid_set_cost(g, 12, 1, 0);
  CHECK(cw_flowfield_stale(f));
  cw_flowfield_build(f, &goal, 1);
  CHECK(!cw_flowfield_stale(f) && cw_flowfield_hash(f) != h1);
  /* two goals: each cell heads for the nearer */
  cw_vec2 goals[2] = { goal, cw_v2(1 * 16 + 8, 8 * 16 + 8) };
  cw_flowfield_build(f, goals, 2);
  CHECK(cw_flowfield_distance(f, goals[1]) == 0);
  d = cw_flowfield_direction(f, cw_v2(1 * 16 + 8, 5 * 16 + 8));
  CHECK(d.x == 0 && d.y == 1);
  /* a sealed pocket has no way out */
  cw_navgrid_set_cost(g, 3, 7, 0), cw_navgrid_set_cost(g, 3, 8, 0), cw_navgrid_set_cost(g, 3, 9, 0);
  cw_navgrid_set_cost(g, 4, 7, 0), cw_navgrid_set_cost(g, 4, 8, 0), cw_navgrid_set_cost(g, 4, 9, 0);
  for (int x = 5; x < 20; x++) cw_navgrid_set_cost(g, x, 9, 0);
  for (int y = 6; y < 10; y++) cw_navgrid_set_cost(g, 19, y, 0);
  cw_navgrid_set_cost(g, 15, 7, 0), cw_navgrid_set_cost(g, 15, 8, 0);
  cw_flowfield_build(f, goals, 2);
  CHECK(cw_flowfield_distance(f, cw_v2(5 * 16 + 8, 4 * 16 + 8)) < 0);
  cw_flowfield_free(f);
  CHECK(!cw_flowfield_valid(f));
  cw_navgrid_free(g);
}

static const char *const HALL[] = {
  "####################",
  "#........#.........#",
  "#........#.........#",
  "#..................#",
  "#..................#",
  "#..................#",
  "#........#.........#",
  "#........#.........#",
  "#........#.........#",
  "####################",
};

static void t_crowd(void) {
  cw_navgrid g = grid_from(HALL, 10, 16);
  cw_crowd2d c = cw_crowd2d_new(g, 64);
  CHECK(cw_crowd2d_valid(c));
  CHECK(CW_HANDLE_IS_NULL(cw_crowd2d_add(c, cw_v2(-900, -900), NULL))); /* nothing walkable near */
  /* twelve agents on each side swap chambers through a doorway three cells wide */
  cw_crowd2d_agent a[24];
  cw_vec2 goal[24];
  for (int i = 0; i < 12; i++) {
    /* places an agent can walk between: 28 apart, radii of 6 */
    cw_vec2 left = cw_v2(26 + (float)(i % 4) * 28, 34 + (float)(i / 4) * 38), right = cw_v2(180 + (float)(i % 4) * 28, 34 + (float)(i / 4) * 38);
    a[i] = cw_crowd2d_add(c, left, NULL);
    a[12 + i] = cw_crowd2d_add(c, right, NULL);
    goal[i] = right, goal[12 + i] = left;
  }
  CHECK_EQ_INT(cw_crowd2d_count(c), 24);
  CHECK(cw_crowd2d_agent_slot(c, a[5]) >= 0 && cw_crowd2d_agent_valid(c, a[5]));
  for (int i = 0; i < 24; i++) cw_crowd2d_set_target(c, a[i], goal[i]);
  cw_crowd2d_agent_state s;
  CHECK(cw_crowd2d_get_state(c, a[0], &s) && s.status == CW_CROWD2D_WAITING);
  /* the plan budget: 8 a step, oldest first */
  cw_crowd2d_step(c, DT);
  int moving = 0;
  for (int i = 0; i < 24; i++) {
    cw_crowd2d_get_state(c, a[i], &s);
    moving += s.status == CW_CROWD2D_MOVING;
  }
  CHECK_EQ_INT(moving, 8);
  cw_crowd2d_get_state(c, a[0], &s);
  CHECK(s.status == CW_CROWD2D_MOVING && s.path_version == 1);
  float min_gap = 1e9f;
  int in_wall = 0;
  for (int t = 0; t < 1500; t++) {
    cw_crowd2d_step(c, DT);
    for (int i = 0; i < 24; i++) {
      cw_vec2 p = cw_crowd2d_position(c, a[i]);
      int x, y;
      cw_navgrid_cell_at(g, p, &x, &y);
      in_wall += !cw_navgrid_walkable(g, x, y);
      for (int j = i + 1; j < 24; j++) {
        cw_vec2 q = cw_crowd2d_position(c, a[j]);
        float dx = p.x - q.x, dy = p.y - q.y, d = sqrtf(dx * dx + dy * dy);
        if (d < min_gap) min_gap = d;
      }
    }
  }
  int arrived = 0;
  for (int i = 0; i < 24; i++) {
    cw_crowd2d_get_state(c, a[i], &s);
    float dx = s.position.x - goal[i].x, dy = s.position.y - goal[i].y;
    arrived += s.status == CW_CROWD2D_ARRIVED && dx * dx + dy * dy < 6 * 6;
  }
  CHECK_EQ_INT(arrived, 24);
  CHECK_EQ_INT(in_wall, 0);
  CHECK(min_gap > 7.0f); /* radii of 6: they brush but never stack */
  cw_info("nav2d crowd: closest two agents came %.2f apart", (double)min_gap);
  uint64_t hh = cw_crowd2d_hash(c);
  FOLD(hh);
  /* a wall across the doorway: the grid's version moves, paths are planned again, the far side is out of reach */
  for (int i = 0; i < 24; i++) cw_crowd2d_set_target(c, a[i], goal[(i + 12) % 24]);
  cw_navgrid_set_cost(g, 9, 3, 0), cw_navgrid_set_cost(g, 9, 4, 0), cw_navgrid_set_cost(g, 9, 5, 0);
  for (int t = 0; t < 600; t++) cw_crowd2d_step(c, DT);
  int stuck = 0;
  for (int i = 0; i < 24; i++) {
    cw_crowd2d_get_state(c, a[i], &s);
    bool left_side = s.position.x < 9 * 16;
    stuck += (i < 12) != left_side; /* each is still in the chamber it had swapped into */
    CHECK(s.partial || s.status == CW_CROWD2D_ARRIVED);
  }
  CHECK_EQ_INT(stuck, 24);
  /* velocity mode, removal, slots reused lowest first */
  cw_crowd2d_set_velocity(c, a[0], cw_v2(0, 20));
  cw_vec2 before = cw_crowd2d_position(c, a[0]);
  cw_crowd2d_step(c, DT);
  cw_crowd2d_get_state(c, a[0], &s);
  CHECK(s.status == CW_CROWD2D_VELOCITY && cw_crowd2d_position(c, a[0]).y > before.y);
  int slot3 = cw_crowd2d_agent_slot(c, a[3]), slot7 = cw_crowd2d_agent_slot(c, a[7]);
  cw_crowd2d_remove(c, a[7]);
  cw_crowd2d_remove(c, a[3]);
  CHECK(!cw_crowd2d_agent_valid(c, a[3]) && cw_crowd2d_count(c) == 22);
  cw_crowd2d_agent n1 = cw_crowd2d_add(c, cw_v2(40, 40), NULL), n2 = cw_crowd2d_add(c, cw_v2(60, 40), NULL);
  CHECK(cw_crowd2d_agent_slot(c, n1) == CW_MIN(slot3, slot7) && cw_crowd2d_agent_slot(c, n2) == CW_MAX(slot3, slot7));
  CHECK(!cw_crowd2d_agent_valid(c, a[3])); /* the old handle of a reused slot stays dead */

  /* a horde on a flow field: forty agents, one goal, no path searches */
  cw_navgrid_set_cost(g, 9, 3, 1), cw_navgrid_set_cost(g, 9, 4, 1), cw_navgrid_set_cost(g, 9, 5, 1);
  cw_crowd2d horde = cw_crowd2d_new(g, 64);
  cw_flowfield f = cw_flowfield_new(g);
  cw_vec2 exit_at = cw_v2(17 * 16 + 8, 4 * 16 + 8);
  cw_flowfield_build(f, &exit_at, 1);
  cw_crowd2d_agent hd[40];
  cw_crowd2d_agent_params small = cw_crowd2d_agent_params_default();
  small.radius = 4;
  for (int i = 0; i < 40; i++) {
    hd[i] = cw_crowd2d_add(horde, cw_v2(22 + (float)(i % 8) * 14, 24 + (float)(i / 8) * 24), &small);
    cw_crowd2d_set_flow(horde, hd[i], f);
  }
  for (int t = 0; t < 900; t++) cw_crowd2d_step(horde, DT);
  int near_goal = 0, east = 0;
  for (int i = 0; i < 40; i++) {
    cw_vec2 p = cw_crowd2d_position(horde, hd[i]);
    east += p.x > 10 * 16;
    float dx = p.x - exit_at.x, dy = p.y - exit_at.y;
    near_goal += dx * dx + dy * dy < 60 * 60;
  }
  CHECK_EQ_INT(east, 40); /* every one went through the doorway */
  CHECK(near_goal >= 36);
  uint64_t h2 = cw_crowd2d_hash(horde);
  FOLD(h2);
  cw_navgrid_free(g); /* its crowds and field go with it */
  CHECK(!cw_crowd2d_valid(c) && !cw_crowd2d_valid(horde) && !cw_flowfield_valid(f));
}

/* ── the nodes, on a tile map ───────────────────────────────────────── */

static int path_changes, reached, velocity_calls, baked;
static void on_path(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; path_changes++; }
static void on_reached(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; reached++; }
static void on_velocity(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; velocity_calls += a[0].type == CW_T_VEC2; }
static void on_baked(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; baked++; }

static void step(cw_tree *t, int n) {
  for (int i = 0; i < n; i++) cw_tree_fixed_update(t, 1.0 / 60);
}

static void t_nodes(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node map = cw_node_new("TileMap2D", "Map");
  cw_node_set(map, "tile_shapes", cw_val_string("1: full; 2: oneway"));
  cw_node_set(map, "tile_data", cw_val_string("mud: 3=6"));
  /* two rooms joined by doorways at rows 1 and 5 of the wall at column 7; a one-way plank and a tile with no
   * shape block nothing */
  cw_node_set(map, "cells0", cw_val_string("1*15 / 1 .*6 . .*6 1 / 1 .*6 1 .*6 1 / 1 . 2 0 .*3 1 .*6 1 / 1 .*6 1 .*6 1 / 1 .*6 . .*6 1 / 1*15"));
  cw_node region = cw_node_new("NavigationRegion2D", "Nav");
  cw_node_add_child(map, region);
  cw_connect(region, "baked", region, NULL, on_baked, NULL, 0);
  cw_node_add_child(root, map);
  cw_node walker = cw_node_new("Node2D", "Walker");
  cw_node2d_set_position(walker, cw_v2(2 * 16 + 8, 3 * 16 + 8));
  cw_node agent = cw_node_new("NavigationAgent2D", "Agent");
  cw_node_add_child(walker, agent);
  cw_node_add_child(root, walker);
  cw_connect(agent, "path_changed", agent, NULL, on_path, NULL, 0);
  cw_connect(agent, "target_reached", agent, NULL, on_reached, NULL, 0);
  cw_connect(agent, "velocity_computed", agent, NULL, on_velocity, NULL, 0);
  step(t, 1);
  CHECK_EQ_INT(baked, 1);
  cw_navgrid g = cw_nav_region2d_grid(region);
  CHECK(cw_navgrid_valid(g) && cw_crowd2d_valid(cw_nav_region2d_crowd(region)));
  int w, h;
  cw_navgrid_size(g, &w, &h);
  CHECK(w == 15 && h == 7);
  cw_value v;
  cw_node_get_prop(region, "walkable_count", &v);
  CHECK_EQ_INT(v.i, 15 * 7 - (15 * 2 + 5 * 2 + 3)); /* the frame and the wall between the doorways */
  CHECK(cw_navgrid_walkable(g, 2, 3) && cw_navgrid_walkable(g, 3, 3) && !cw_navgrid_walkable(g, 7, 3) && cw_navgrid_walkable(g, 7, 1));
  CHECK(!CW_HANDLE_IS_NULL(cw_nav_agent2d_handle(agent)));
  /* to the east room: through the upper doorway (the nearer one from a start above the middle) */
  cw_node2d_set_position(walker, cw_v2(2 * 16 + 8, 2 * 16 + 8));
  cw_node_set(agent, "target_position", cw_val_vec2(12 * 16 + 8, 2 * 16 + 8));
  float min_y = 1e9f;
  for (int i = 0; i < 400; i++) {
    step(t, 1);
    cw_vec2 p = cw_node2d_position(walker);
    if (p.x > 6.5f * 16 && p.x < 8.5f * 16 && p.y < min_y) min_y = p.y;
  }
  cw_vec2 p = cw_node2d_position(walker);
  CHECK(fabsf(p.x - (12 * 16 + 8)) < 4 && fabsf(p.y - (2 * 16 + 8)) < 4);
  CHECK(min_y < 2 * 16); /* it crossed the wall's column in row 1 */
  CHECK_EQ_INT(reached, 1);
  CHECK_EQ_INT(path_changes, 1);
  CHECK(velocity_calls > 50);
  cw_node_get_prop(agent, "status", &v);
  CHECK_EQ_INT(v.i, CW_CROWD2D_ARRIVED);
  cw_node_get_prop(agent, "is_navigation_finished", &v);
  CHECK(v.b);
  FOLD(p);
  /* the upper doorway is bricked up in the map: the region bakes that cell in, and the way back is the lower one */
  cw_tilemap2d_set_cell(map, 0, 7, 1, 1, 0);
  cw_node_set(agent, "target_position", cw_val_vec2(2 * 16 + 8, 2 * 16 + 8));
  float max_y = 0;
  for (int i = 0; i < 500; i++) {
    step(t, 1);
    cw_vec2 q = cw_node2d_position(walker);
    if (q.x > 6.5f * 16 && q.x < 8.5f * 16 && q.y > max_y) max_y = q.y;
  }
  CHECK(!cw_navgrid_walkable(g, 7, 1));
  CHECK_EQ_INT(baked, 1); /* no whole bake for one cell */
  p = cw_node2d_position(walker);
  CHECK(fabsf(p.x - (2 * 16 + 8)) < 4 && fabsf(p.y - (2 * 16 + 8)) < 4);
  CHECK(max_y > 5 * 16);
  CHECK_EQ_INT(reached, 2);
  FOLD(p);
  /* an obstacle node in the lower doorway: no way through, the agent stops short and says the path is partial */
  cw_node crate = cw_node_new("NavigationObstacle2D", "Crate");
  cw_node2d_set_position(crate, cw_v2(7 * 16 + 8, 5 * 16 + 8));
  cw_node_add_child(root, crate);
  cw_node_set(agent, "target_position", cw_val_vec2(12 * 16 + 8, 4 * 16 + 8));
  step(t, 300);
  CHECK(!cw_navgrid_walkable(g, 7, 5));
  CHECK(cw_node2d_position(walker).x < 7 * 16);
  cw_crowd2d_agent_state s;
  CHECK(cw_crowd2d_get_state(cw_nav_region2d_crowd(region), cw_nav_agent2d_handle(agent), &s) && s.partial);
  /* the crate is moved aside: the cell opens, the path is planned again, the agent arrives */
  cw_node2d_set_position(crate, cw_v2(3 * 16 + 8, 1 * 16 + 8));
  step(t, 500);
  CHECK(cw_navgrid_walkable(g, 7, 5) && !cw_navgrid_walkable(g, 3, 1));
  p = cw_node2d_position(walker);
  CHECK(fabsf(p.x - (12 * 16 + 8)) < 4 && fabsf(p.y - (4 * 16 + 8)) < 4);
  CHECK_EQ_INT(reached, 3);
  cw_node_free(crate);
  step(t, 1);
  CHECK(cw_navgrid_walkable(g, 3, 1));
  /* a wide agent: with agent_radius the cells beside walls are blocked too (a property change bakes again) */
  cw_node_set(region, "agent_radius", cw_val_float(12));
  step(t, 1);
  CHECK_EQ_INT(baked, 2);
  g = cw_nav_region2d_grid(region);
  CHECK(!cw_navgrid_walkable(g, 1, 1) && cw_navgrid_walkable(g, 3, 3) && !cw_navgrid_walkable(g, 7, 5));
  CHECK(!CW_HANDLE_IS_NULL(cw_nav_agent2d_handle(agent))); /* joined the new crowd */
  cw_node_set(region, "agent_radius", cw_val_float(0));
  /* costs from tile_data: a strip of mud */
  cw_node_set(region, "cost_data", cw_val_string("mud"));
  cw_tilemap2d_set_cell(map, 0, 3, 4, 3, 0);
  step(t, 2);
  g = cw_nav_region2d_grid(region);
  CHECK_EQ_INT(cw_navgrid_cost(g, 3, 4), 6);
  CHECK(!cw_navgrid_get_stats(g).jump_points);
  /* the flow field of the region, and an agent told to follow it */
  cw_nav_region2d_set_flow_target(region, cw_v2(2 * 16 + 8, 5 * 16 + 8));
  CHECK(cw_flowfield_valid(cw_nav_region2d_flow(region)));
  cw_node_call(agent, "follow_flow", NULL, 0, NULL);
  step(t, 600);
  p = cw_node2d_position(walker);
  CHECK(fabsf(p.x - (2 * 16 + 8)) < 9 && fabsf(p.y - (5 * 16 + 8)) < 9);
  FOLD(p);
  /* the region leaves: agents drop out of its crowd without a crash, and join again when it is back */
  cw_node_remove_child(map, region);
  step(t, 2);
  CHECK(CW_HANDLE_IS_NULL(cw_nav_agent2d_handle(agent)));
  cw_node_add_child(map, region);
  step(t, 2);
  CHECK(!CW_HANDLE_IS_NULL(cw_nav_agent2d_handle(agent)));
  cw_tree_free(t);
}

CW_TEST_SUITE("nav2d", { "grid", t_grid }, { "jps_equals_astar", t_jps_equals_astar }, { "paths", t_paths }, { "costs", t_costs },
              { "flow", t_flow }, { "crowd", t_crowd }, { "nodes", t_nodes })

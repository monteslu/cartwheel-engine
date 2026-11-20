/* TileMap2D tile shapes, custom data and TileCollision2D: merged outlines per chunk, slopes, partial shared
 * edges, one-way platforms, physics layers, chunk rebuilds on edits, rollback. Every position that must agree
 * across hosts is folded into the checksum. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/net.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"
#include "test.h"

#define DT (1.0 / 60)
#define SHAPES "1-4: full; 5: slope 0 16; 6: slope 16 0; 7: slope 0 8; 8: slope 8 16; 9: oneway; 10: rect 0 8 16 8; 11: poly 0 0, 16 0, 8 16"

static void step(cw_tree *t, int n) {
  for (int i = 0; i < n; i++) cw_tree_fixed_update(t, DT);
}

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

/* a map with the suite's tile shapes and a TileCollision2D child; one-way platforms on physics layer 2 */
static cw_node make_map(cw_node root, const char *cells, cw_node *col) {
  cw_node map = cw_node_new("TileMap2D", "Map");
  cw_node_set(map, "tile_shapes", cw_val_string(SHAPES));
  cw_node_set(map, "cells0", cw_val_string(cells));
  cw_node c = cw_node_new("TileCollision2D", "Solid");
  cw_node_set(c, "one_way_layer", cw_val_int(2));
  cw_node_add_child(map, c);
  cw_node_add_child(root, map);
  if (col) *col = c;
  return map;
}

static cw_node box(cw_node parent, const char *name, float x, float y, float size) {
  cw_node b = cw_node_new("RigidBody2D", name);
  cw_node2d_set_position(b, cw_v2(x, y));
  cw_node s = cw_node_new("CollisionShape2D", "Shape");
  cw_node_set(s, "shape", cw_val_enum("RECT"));
  cw_node_set(s, "size", cw_val_vec2(size, size));
  cw_node_add_child(b, s);
  cw_node_add_child(parent, b);
  return b;
}

typedef struct { float vx, jump; } walker;
static void walker_fixed(cw_node n, void *d, double dt) {
  walker *w = d;
  cw_value v;
  cw_node_get_prop(n, "velocity", &v);
  float vy = v.v[1] + 900 * (float)dt;
  if (w->jump != 0) vy = -w->jump, w->jump = 0;
  cw_node_set(n, "velocity", cw_val_vec2(w->vx, vy));
  cw_character2d_move_and_slide(n, (float)dt);
}
static cw_node_type walker_type = { .name = "TileWalker", .parent = "CharacterBody2D", .data_size = sizeof(walker),
                                    .fixed_update = walker_fixed };

/* a capsule 12 wide and 24 tall: the node's origin is 12 px over its feet */
static cw_node make_walker(cw_node root, float x, float feet_y) {
  cw_node c = cw_node_new("TileWalker", "Walker");
  cw_node2d_set_position(c, cw_v2(x, feet_y - 12));
  cw_node s = cw_node_new("CollisionShape2D", "Shape");
  cw_node_set(s, "shape", cw_val_enum("CAPSULE"));
  cw_node_set(s, "radius", cw_val_float(6));
  cw_node_set(s, "height", cw_val_float(24));
  cw_node_add_child(c, s);
  cw_node_set(c, "one_way_mask", cw_val_int(1 << 2));
  cw_node_add_child(root, c);
  return c;
}

static bool flag(cw_node n, const char *prop) {
  cw_value v;
  cw_node_get_prop(n, prop, &v);
  return v.b;
}

static void setup(void) {
  cw_scene_register_builtin_types();
  if (!cw_node_type_find("TileWalker")) cw_node_type_register(&walker_type);
}

/* ── the text forms ─────────────────────────────────────────────────── */

static void t_shapes(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node map = add(root, "TileMap2D", "Map");
  cw_node_set(map, "tile_shapes", cw_val_string(SHAPES));
  cw_node_set(map, "tile_data", cw_val_string("damage: 5=1, 6-7=2.5; sound: 1-4=stone, 9=wood"));
  cw_node_set(map, "cells0", cw_val_string("1 5 5x 9 / 10 11 . 0 / 5y 5d"));
  cw_vec2 p[12];
  int n = 0;
  CHECK(cw_tilemap2d_tile_shape(map, 1, p, 12, &n) == CW_TILE_SHAPE_SOLID && n == 4);
  CHECK(cw_tilemap2d_tile_shape(map, 5, p, 12, &n) == CW_TILE_SHAPE_SOLID && n == 3);
  CHECK(p[0].x == 16 && p[0].y == 0 && p[1].x == 16 && p[1].y == 16 && p[2].x == 0 && p[2].y == 16);
  CHECK(cw_tilemap2d_tile_shape(map, 7, p, 12, &n) == CW_TILE_SHAPE_SOLID && n == 3);
  CHECK(cw_tilemap2d_tile_shape(map, 8, p, 12, &n) == CW_TILE_SHAPE_SOLID && n == 4);
  CHECK(cw_tilemap2d_tile_shape(map, 9, p, 12, &n) == CW_TILE_SHAPE_ONE_WAY && n == 2 && p[1].x == 16 && p[1].y == 0);
  CHECK(cw_tilemap2d_tile_shape(map, 10, p, 12, &n) == CW_TILE_SHAPE_SOLID && p[0].y == 8);
  CHECK(cw_tilemap2d_tile_shape(map, 11, p, 12, &n) == CW_TILE_SHAPE_SOLID && n == 3);
  CHECK(cw_tilemap2d_tile_shape(map, 0, p, 12, &n) == CW_TILE_SHAPE_NONE && n == 0);
  CHECK(cw_tilemap2d_tile_shape(map, 99, NULL, 0, NULL) == CW_TILE_SHAPE_NONE);
  /* cells: a one-way platform and a shapeless tile are not solid */
  CHECK(cw_tilemap2d_cell_solid(map, 0, 0, 0) && cw_tilemap2d_cell_solid(map, 0, 1, 0));
  CHECK(!cw_tilemap2d_cell_solid(map, 0, 3, 0) && !cw_tilemap2d_cell_solid(map, 0, 3, 1) && !cw_tilemap2d_cell_solid(map, 0, 2, 1));
  CHECK(!cw_tilemap2d_cell_solid(map, 0, -1, 0) && !cw_tilemap2d_cell_solid(map, 0, 40, 40));
  /* a slope rising to the right is solid under its floor line; flipped, it rises to the left */
  CHECK(cw_tilemap2d_point_solid(map, 0, cw_v2(16 + 12, 8)) && !cw_tilemap2d_point_solid(map, 0, cw_v2(16 + 4, 8)));
  CHECK(cw_tilemap2d_point_solid(map, 0, cw_v2(32 + 4, 8)) && !cw_tilemap2d_point_solid(map, 0, cw_v2(32 + 12, 8)));
  /* flipped down it hangs from the ceiling; transposed it is mirrored about its diagonal (the same triangle) */
  CHECK(cw_tilemap2d_point_solid(map, 0, cw_v2(12, 32 + 4)) && !cw_tilemap2d_point_solid(map, 0, cw_v2(4, 32 + 12)));
  CHECK(cw_tilemap2d_point_solid(map, 0, cw_v2(16 + 12, 32 + 12)) && !cw_tilemap2d_point_solid(map, 0, cw_v2(16 + 4, 32 + 4)));
  /* the half block: its lower half */
  CHECK(cw_tilemap2d_point_solid(map, 0, cw_v2(8, 16 + 12)) && !cw_tilemap2d_point_solid(map, 0, cw_v2(8, 16 + 4)));
  /* custom data */
  float v = -1;
  CHECK(cw_tilemap2d_tile_data(map, 5, "damage", &v) && v == 1);
  CHECK(cw_tilemap2d_tile_data(map, 7, "damage", &v) && v == 2.5f);
  v = -1;
  CHECK(!cw_tilemap2d_tile_data(map, 8, "damage", &v) && v == -1);
  CHECK(!cw_tilemap2d_tile_data(map, 5, "nothing", &v));
  CHECK(strcmp(cw_tilemap2d_tile_data_text(map, 3, "sound"), "stone") == 0);
  CHECK(strcmp(cw_tilemap2d_tile_data_text(map, 9, "sound"), "wood") == 0);
  CHECK(strcmp(cw_tilemap2d_tile_data_text(map, 5, "sound"), "") == 0);
  CHECK(cw_tilemap2d_cell_data(map, 0, 1, 0, "damage", 0) == 1 && cw_tilemap2d_cell_data(map, 0, 0, 0, "damage", 7) == 7);
  /* raw cells keep their flips */
  CHECK(cw_tilemap2d_get_raw(map, 0, 2, 0) == (CW_TILE(5) | CW_TILE_FLIP_X));
  cw_tilemap2d_set_raw(map, 0, 3, 1, CW_TILE(2) | CW_TILE_ROT90);
  CHECK(cw_tilemap2d_get_raw(map, 0, 3, 1) == (CW_TILE(2) | CW_TILE_ROT90) && cw_tilemap2d_get_cell(map, 0, 3, 1) == 2);
  cw_vec2 cc = cw_tilemap2d_cell_center(map, 2, 1);
  CHECK(cc.x == 40 && cc.y == 24);
  /* both text forms round-trip through a scene file */
  char *text = cw_scene_save(root);
  CHECK(strstr(text, "tile_shapes = \"1-4: full; 5: slope 0 16;") != NULL);
  CHECK(strstr(text, "tile_data = \"damage: 5=1, 6-7=2.5; sound: 1-4=stone, 9=wood\"") != NULL);
  cw_free(text);
  cw_tree_free(t);
}

/* ── outlines ───────────────────────────────────────────────────────── */

static void t_merge(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t), col;
  /* a block 10 cells by 2: one loop of four segments, whatever the 20 cells */
  cw_node map = make_map(root, "1*10 / 2*10", &col);
  step(t, 1);
  cw_tile_collision2d_stats s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 1);
  CHECK_EQ_INT(s.segments, 4);
  CHECK_EQ_INT(s.chunks, 1);
  CHECK_EQ_INT(s.builds, 1);
  CHECK(cw_tile_collision2d_map(col).id == map.id);
  CHECK_EQ_INT(cw_world2d_stats(cw_physics2d_world(t)).bodies, 1);
  cw_tree_free(t);

  /* 40 cells by 2 crosses the chunk edge at 32: each chunk has one open chain of three segments, and the top
   * and bottom carry on into the next chunk through its ghost vertices */
  t = cw_tree_new();
  root = cw_tree_root(t);
  make_map(root, "1*40 / 1*40", &col);
  step(t, 1);
  s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 2);
  CHECK_EQ_INT(s.segments, 6);
  CHECK_EQ_INT(s.chunks, 2);
  cw_tree_free(t);

  /* slopes: a hill. Its outline is one loop of eight straight runs */
  t = cw_tree_new();
  root = cw_tree_root(t);
  make_map(root, ". . . . 5 1 1 1 6 / 1*12", &col);
  step(t, 1);
  s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 1);
  CHECK_EQ_INT(s.segments, 8);
  cw_tree_free(t);

  /* two half slopes and a block make one ramp: the ramp's two tiles are one straight run, and the sides the
   * tiles share (half a cell high, then a whole one) are gone */
  t = cw_tree_new();
  root = cw_tree_root(t);
  make_map(root, "7 8 1", &col);
  step(t, 1);
  s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 1);
  CHECK_EQ_INT(s.segments, 4);
  cw_tree_free(t);

  /* a half block beside a full one: the full block's side is shared over its lower half only */
  t = cw_tree_new();
  root = cw_tree_root(t);
  make_map(root, "10 1", &col);
  step(t, 1);
  s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 1);
  CHECK_EQ_INT(s.segments, 6);
  cw_tree_free(t);

  /* two blocks that touch at a corner stay two loops */
  t = cw_tree_new();
  root = cw_tree_root(t);
  make_map(root, "1 . / . 1", &col);
  step(t, 1);
  s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 2);
  CHECK_EQ_INT(s.segments, 8);
  cw_tree_free(t);

  /* a ring of walls: the outside and the inside outline */
  t = cw_tree_new();
  root = cw_tree_root(t);
  make_map(root, "1*5 / 1 .*3 1 / 1 .*3 1 / 1*5", &col);
  step(t, 1);
  s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 2);
  CHECK_EQ_INT(s.segments, 8);
  cw_tree_free(t);
}

/* ── bodies on tiles ────────────────────────────────────────────────── */

/* a box slides along 60 cells of floor without friction: it must not catch on a tile seam or the chunk seam */
static void t_slide(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t), col;
  make_map(root, ". / . / . / . / 1*60 / 1*60", &col);
  cw_node_set(col, "friction", cw_val_float(0));
  cw_node b = box(root, "Slider", 24, 64 - 5, 10);
  cw_node_set(b, "friction", cw_val_float(0));
  step(t, 30);
  CHECK(fabsf(cw_node2d_position(b).y - 59) < 0.5f); /* resting on the floor's top, y = 64 */
  cw_node_set(b, "linear_velocity", cw_val_vec2(300, 0));
  float max_dy = 0, max_rot = 0;
  for (int i = 0; i < 170; i++) {
    step(t, 1);
    max_dy = CW_MAX(max_dy, fabsf(cw_node2d_position(b).y - 59));
    max_rot = CW_MAX(max_rot, fabsf(cw_node2d_rotation(b)));
  }
  cw_value v;
  cw_node_get_prop(b, "linear_velocity", &v);
  CHECK(cw_node2d_position(b).x > 24 + 800); /* past the chunk seam at x = 512 */
  CHECK(fabsf(v.v[0] - 300) < 1.0f);
  CHECK(max_dy < 0.5f && max_rot < 0.01f);
  { cw_vec2 p = cw_node2d_position(b); FOLD(p); }
  /* a body outside the tiles' mask falls through them */
  cw_node ghost = box(root, "Ghost", 100, 40, 10);
  cw_node_set(ghost, "collision_layer", cw_val_int(3));
  cw_node_set(col, "collision_mask", cw_val_int(~(1 << 3)));
  step(t, 90);
  CHECK(cw_node2d_position(ghost).y > 120);
  cw_tree_free(t);
}

static void t_slopes(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t), col;
  /* a hill: up a slope at cells 4, a plateau one cell up, down a slope */
  make_map(root, ". / . / . / . / . / . / . / . / . / 1 . . . 5 1 1 1 6 / 1*12", &col);
  cw_node w = make_walker(root, 24, 160);
  step(t, 20);
  CHECK(flag(w, "on_floor"));
  CHECK(fabsf(cw_node2d_position(w).y - 148) < 0.6f);
  ((walker *)cw_node_data(w, "TileWalker"))->vx = 60;
  step(t, 80); /* 80 px: onto the plateau */
  cw_vec2 p = cw_node2d_position(w);
  CHECK(flag(w, "on_floor"));
  CHECK(p.x > 82 && p.x < 125); /* past the slope (64..80), which it climbs slower than it walks */
  CHECK(fabsf(p.y - 132) < 0.6f); /* the plateau's top is y = 144 */
  FOLD(p);
  step(t, 80); /* down the far slope, back on the floor */
  p = cw_node2d_position(w);
  CHECK(flag(w, "on_floor") && p.x > 150);
  CHECK(fabsf(p.y - 148) < 0.6f);
  FOLD(p);
  /* contacts name the TileCollision2D, and a contact point names the cell */
  int cx, cy;
  cw_tile_collision2d_cell_at(col, cw_v2(p.x, 160), cw_v2(0, -1), &cx, &cy);
  CHECK_EQ_INT(cy, 10);
  CHECK_EQ_INT(cx, (int)(p.x / 16));
  /* a ball rolls down the slope it is dropped on */
  cw_node ball = cw_node_new("RigidBody2D", "Ball");
  cw_node2d_set_position(ball, cw_v2(72, 120));
  cw_node bs = cw_node_new("CollisionShape2D", "Shape");
  cw_node_set(bs, "shape", cw_val_enum("CIRCLE"));
  cw_node_set(bs, "radius", cw_val_float(4));
  cw_node_add_child(ball, bs);
  cw_node_add_child(root, ball);
  step(t, 120);
  cw_vec2 bp = cw_node2d_position(ball);
  CHECK(bp.x < 60 && fabsf(bp.y - 156) < 0.6f); /* rolled left, off the slope, on the floor */
  FOLD(bp);
  cw_tree_free(t);
}

static void t_one_way(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t), col;
  /* a platform three cells wide at row 7 (its top y = 112) over a floor at row 10 (y = 160) */
  make_map(root, ". / . / . / . / . / . / . / . . . 9 9 9 / . / . / 1*12", &col);
  cw_node w = make_walker(root, 72, 160);
  step(t, 20);
  CHECK(flag(w, "on_floor") && !flag(w, "on_one_way"));
  cw_tile_collision2d_stats s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 2); /* the floor's loop and one platform */
  CHECK_EQ_INT(s.segments, 5);
  /* jump: up through the platform, down onto it */
  ((walker *)cw_node_data(w, "TileWalker"))->jump = 330;
  float top = 1e9f;
  for (int i = 0; i < 70; i++) {
    step(t, 1);
    top = CW_MIN(top, cw_node2d_position(w).y);
  }
  CHECK(top < 100 - 4); /* its feet rose above the platform */
  CHECK(flag(w, "on_floor") && flag(w, "on_one_way"));
  CHECK(fabsf(cw_node2d_position(w).y - 100) < 0.6f);
  { cw_vec2 p = cw_node2d_position(w); FOLD(p); }
  /* drop through it */
  cw_node_set(w, "drop_through", cw_val_bool(true));
  step(t, 8);
  cw_node_set(w, "drop_through", cw_val_bool(false));
  step(t, 50);
  CHECK(flag(w, "on_floor") && !flag(w, "on_one_way"));
  CHECK(fabsf(cw_node2d_position(w).y - 148) < 0.6f);
  /* rigid bodies: one dropped from above rests on the platform, one thrown up from below passes it */
  cw_node above = box(root, "Above", 60, 80, 10);
  cw_node below = box(root, "Below", 88, 140, 10);
  cw_node_set(below, "linear_velocity", cw_val_vec2(0, -420));
  float below_top = 1e9f;
  for (int i = 0; i < 40; i++) {
    step(t, 1);
    below_top = CW_MIN(below_top, cw_node2d_position(below).y);
  }
  CHECK(below_top < 100); /* its middle went above the platform's top at y = 112 */
  step(t, 80);
  CHECK(fabsf(cw_node2d_position(above).y - 107) < 0.6f);
  CHECK(fabsf(cw_node2d_position(below).y - 107) < 0.6f); /* and came down onto it */
  { cw_vec2 p = cw_node2d_position(above); FOLD(p); }
  /* a body that does not see the platforms' layer falls to the floor */
  cw_node past = box(root, "Past", 76, 60, 6);
  cw_node_set(past, "collision_mask", cw_val_int(~(1 << 2)));
  step(t, 120);
  CHECK(cw_node2d_position(past).y > 150);
  cw_tree_free(t);
}

/* ── edits ──────────────────────────────────────────────────────────── */

static void t_edits(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t), col;
  cw_node map = make_map(root, ". / . / . / . / 1*70 / 1*70", &col);
  step(t, 1);
  cw_tile_collision2d_stats s0 = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s0.builds, 3); /* 70 cells: three chunks */
  CHECK_EQ_INT(s0.chains, 4);   /* the middle chunk's top and bottom meet only in its neighbours */
  CHECK_EQ_INT(s0.segments, 8);
  step(t, 5);
  CHECK_EQ_INT(cw_tile_collision2d_stats_get(col).builds, 3); /* nothing changed: nothing rebuilt */
  /* a cell in the middle of a chunk: that chunk alone */
  cw_tilemap2d_set_cell(map, 0, 40, 4, -1, 0);
  step(t, 1);
  cw_tile_collision2d_stats s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.builds, 4);
  CHECK_EQ_INT(s.segments, s0.segments + 4); /* a notch in the top of chunk 1: five edges where one was */
  /* setting a cell to what it holds is no edit */
  cw_tilemap2d_set_cell(map, 0, 40, 4, -1, 0);
  step(t, 1);
  CHECK_EQ_INT(cw_tile_collision2d_stats_get(col).builds, 4);
  /* a cell on a chunk's edge: its chunk and the neighbour whose ghost vertices it feeds */
  cw_tilemap2d_set_cell(map, 0, 31, 4, -1, 0);
  step(t, 1);
  CHECK_EQ_INT(cw_tile_collision2d_stats_get(col).builds, 6);
  /* a box falls through a hole cut under it, and lands on the floor put back */
  cw_node b = box(root, "Faller", 168, 50, 10);
  step(t, 30);
  CHECK(fabsf(cw_node2d_position(b).y - 59) < 0.6f);
  for (int x = 9; x <= 11; x++)
    for (int y = 4; y <= 5; y++) cw_tilemap2d_set_cell(map, 0, x, y, -1, 0);
  step(t, 60);
  CHECK(cw_node2d_position(b).y > 100);
  { cw_vec2 p = cw_node2d_position(b); FOLD(p); }
  for (int x = 9; x <= 11; x++) cw_tilemap2d_set_cell(map, 0, x, 4, 1, 0);
  cw_tile_collision2d_update(col); /* now, not at the next step */
  int builds = cw_tile_collision2d_stats_get(col).builds;
  step(t, 1);
  CHECK_EQ_INT(cw_tile_collision2d_stats_get(col).builds, builds);
  cw_node b2 = box(root, "Lander", 168, 40, 10);
  step(t, 60);
  CHECK(fabsf(cw_node2d_position(b2).y - 59) < 0.6f);
  /* growing the map, or new shapes, rebuilds everything */
  builds = cw_tile_collision2d_stats_get(col).builds;
  cw_tilemap2d_set_cell(map, 0, 100, 4, 1, 0);
  step(t, 1);
  CHECK_EQ_INT(cw_tile_collision2d_stats_get(col).builds, builds + 4);
  cw_node_set(map, "tile_shapes", cw_val_string("1: rect 0 0 16 4"));
  step(t, 1);
  CHECK_EQ_INT(cw_tile_collision2d_stats_get(col).builds, builds + 8);
  step(t, 60);
  CHECK(fabsf(cw_node2d_position(b2).y - 59) < 0.6f); /* thin floors now: the box still rests on y = 64 */
  /* the layers bit mask: a map layer left out builds nothing */
  cw_node_set(col, "layers", cw_val_int(2));
  step(t, 1);
  CHECK_EQ_INT(cw_tile_collision2d_stats_get(col).chains, 0);
  step(t, 60);
  CHECK(cw_node2d_position(b2).y > 100);
  cw_tree_free(t);
}

/* a tree snapshot from before an edit brings back the colliders of that moment */
static void t_rollback(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t), col;
  cw_node map = make_map(root, ". / . / . / . / 1*20 / 1*20", &col);
  cw_node b = box(root, "Box", 100, 50, 10);
  step(t, 30);
  cw_state *st = cw_state_new();
  cw_state_save(st, t);
  cw_tile_collision2d_stats s0 = cw_tile_collision2d_stats_get(col);
  cw_vec2 p0 = cw_node2d_position(b);
  for (int x = 5; x <= 7; x++)
    for (int y = 4; y <= 5; y++) cw_tilemap2d_set_cell(map, 0, x, y, -1, 0);
  step(t, 40);
  CHECK(cw_node2d_position(b).y > 90);
  cw_state_load(st, t);
  step(t, 1);
  cw_tile_collision2d_stats s1 = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s1.chains, s0.chains);
  CHECK_EQ_INT(s1.segments, s0.segments);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 6, 4), 1);
  step(t, 40);
  CHECK(fabsf(cw_node2d_position(b).y - p0.y) < 0.6f);
  { cw_vec2 p = cw_node2d_position(b); FOLD(p); }
  cw_state_free(st);
  cw_tree_free(t);
}

/* ── terrains ───────────────────────────────────────────────────────── */

#define BLOB(mask) (100 + cw_tile_terrain_index(CW_TILE_TERRAIN_BLOB47, mask))
enum { N = 1, NE = 2, E = 4, SE = 8, S = 16, SW = 32, W = 64, NW = 128 };

static uint32_t map_hash(cw_node map, int w, int h) {
  uint32_t hh = CW_HASH32_INIT;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int c = cw_tilemap2d_get_cell(map, 0, x, y);
      hh = cw_hash32(&c, sizeof c, hh);
    }
  return hh;
}

static void t_terrain_sets(void) {
  setup();
  CHECK_EQ_INT(cw_tile_terrain_tiles(CW_TILE_TERRAIN_BLOB47), 47);
  CHECK_EQ_INT(cw_tile_terrain_tiles(CW_TILE_TERRAIN_EDGE16), 16);
  CHECK_EQ_INT(cw_tile_terrain_tiles(CW_TILE_TERRAIN_CORNER16), 16);
  CHECK_EQ_INT(cw_tile_terrain_tiles(CW_TILE_TERRAIN_SINGLE), 1);
  CHECK_EQ_INT(cw_tile_terrain_mask(CW_TILE_TERRAIN_BLOB47, 0), 0);
  CHECK_EQ_INT(cw_tile_terrain_mask(CW_TILE_TERRAIN_BLOB47, 4), 7);
  CHECK_EQ_INT(cw_tile_terrain_mask(CW_TILE_TERRAIN_BLOB47, 10), 28);
  CHECK_EQ_INT(cw_tile_terrain_mask(CW_TILE_TERRAIN_BLOB47, 46), 255);
  CHECK_EQ_INT(cw_tile_terrain_mask(CW_TILE_TERRAIN_BLOB47, 47), -1);
  /* every mask lands on a tile whose mask is the usable part of it, and the 47 are all different */
  int seen[47] = { 0 }, distinct = 0;
  for (int m = 0; m < 256; m++) {
    int i = cw_tile_terrain_index(CW_TILE_TERRAIN_BLOB47, m);
    CHECK(i >= 0 && i < 47);
    if (i < 0 || i >= 47) continue;
    if (!seen[i]++) distinct++;
    int back = cw_tile_terrain_mask(CW_TILE_TERRAIN_BLOB47, i);
    CHECK((back & m) == back && ((back ^ m) & (N | E | S | W)) == 0); /* the same sides, only unusable corners dropped */
  }
  CHECK_EQ_INT(distinct, 47);
  CHECK_EQ_INT(cw_tile_terrain_index(CW_TILE_TERRAIN_BLOB47, NE), 0);           /* a lone corner shows nothing */
  CHECK_EQ_INT(cw_tile_terrain_index(CW_TILE_TERRAIN_BLOB47, N | NE | E), 4);
  CHECK_EQ_INT(cw_tile_terrain_index(CW_TILE_TERRAIN_BLOB47, N | NE), 1);       /* a corner without both its sides */
  CHECK_EQ_INT(cw_tile_terrain_index(CW_TILE_TERRAIN_EDGE16, 5), 5);
}

static void t_terrain_paint(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node map = add(root, "TileMap2D", "Map");
  cw_node_set(map, "terrains", cw_val_string("grass: blob47 100; wall: edge16 200 outside; path: corner16 300; floor: single 400 x4 seed 3"));
  CHECK_EQ_INT(cw_tilemap2d_terrain_count(map), 4);
  int grass = cw_tilemap2d_terrain(map, "grass"), wall = cw_tilemap2d_terrain(map, "wall"), path = cw_tilemap2d_terrain(map, "path"),
      floor_t = cw_tilemap2d_terrain(map, "floor");
  CHECK(grass == 1 && wall == 2 && path == 3 && floor_t == 4 && cw_tilemap2d_terrain(map, "lava") == 0);
  CHECK(strcmp(cw_tilemap2d_terrain_name(map, 2), "wall") == 0 && strcmp(cw_tilemap2d_terrain_name(map, 9), "") == 0);
  cw_tilemap2d_set_cell(map, 0, 23, 23, -1, 0); /* a 24 x 24 map */

  /* blob: a 3 x 3 patch */
  cw_tilemap2d_set_terrain_rect(map, 0, 2, 2, 4, 4, grass);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 3, 3), 146);                       /* surrounded */
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 2, 2), BLOB(E | SE | S));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 2, 2), 110);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 3, 2), BLOB(W | E | S | SW | SE));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 4, 4), BLOB(W | NW | N));
  CHECK_EQ_INT(cw_tilemap2d_get_terrain(map, 0, 3, 3), grass);
  CHECK_EQ_INT(cw_tilemap2d_get_terrain(map, 0, 5, 3), 0);
  /* one more cell beside it: the patch's edge opens toward it */
  cw_tilemap2d_set_terrain(map, 0, 5, 3, grass);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 5, 3), BLOB(W));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 4, 3), BLOB(N | S | W | E | NW | SW));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 4, 2), BLOB(W | SW | S));           /* the corner cell: its SE is not between two sides */
  /* erase the middle: the ring closes round the hole */
  cw_tilemap2d_set_terrain(map, 0, 3, 3, 0);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 3, 3), -1);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 3, 2), BLOB(W | E));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 2, 2), BLOB(E | S));
  /* a lone cell */
  cw_tilemap2d_set_terrain(map, 0, 10, 2, grass);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 10, 2), 100);

  /* edge16 with "outside": a wall along the top row joins the map's edge */
  cw_tilemap2d_set_terrain_rect(map, 0, 8, 0, 10, 0, wall);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 8, 0), 200 + (1 | 2));             /* north (outside) and east */
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 9, 0), 200 + (1 | 2 | 8));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 10, 0), 200 + (1 | 8));
  cw_tilemap2d_set_terrain(map, 0, 9, 1, wall);                                  /* a T junction */
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 9, 0), 200 + 15);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 9, 1), 200 + 1);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 10, 2), 100);                       /* another terrain beside it is no neighbour */

  /* corner16: one painted cell fills its four corners: nine tiles */
  cw_tilemap2d_set_terrain(map, 0, 15, 15, path);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 15, 15), 315);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 14, 14), 300 + 2);                  /* its south-east corner */
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 15, 14), 300 + (2 | 4));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 16, 14), 300 + 4);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 14, 15), 300 + (1 | 2));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 16, 16), 300 + 8);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 13, 15), -1);
  /* a second cell two to the right: the cell between them is covered from both sides */
  cw_tilemap2d_set_terrain(map, 0, 17, 15, path);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 16, 15), 315);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 16, 14), 300 + (2 | 4));
  /* erasing the first takes its corners away, the second keeps its own */
  cw_tilemap2d_set_terrain(map, 0, 15, 15, 0);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 14, 14), -1);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 15, 15), -1);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 16, 15), 300 + (1 | 2));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 17, 15), 315);

  /* variants: the same cell gets the same one whatever the order, and they do vary */
  cw_tilemap2d_set_terrain_rect(map, 0, 0, 18, 7, 23, floor_t);
  int counts[4] = { 0 };
  for (int y = 18; y <= 23; y++)
    for (int x = 0; x <= 7; x++) {
      int c = cw_tilemap2d_get_cell(map, 0, x, y);
      CHECK(c >= 400 && c <= 403);
      if (c >= 400 && c <= 403) counts[c - 400]++;
    }
  CHECK(counts[0] > 3 && counts[1] > 3 && counts[2] > 3 && counts[3] > 3);
  uint32_t h1 = map_hash(map, 24, 24);
  cw_tilemap2d_set_terrain_rect(map, 0, 0, 18, 7, 23, 0);
  CHECK(map_hash(map, 24, 24) != h1);
  for (int y = 23; y >= 18; y--)
    for (int x = 7; x >= 0; x--) cw_tilemap2d_set_terrain(map, 0, x, y, floor_t);
  CHECK(map_hash(map, 24, 24) == h1);
  FOLD(h1);
  /* fixing again changes nothing */
  cw_tilemap2d_fix_terrain(map, 0, 0, 0, 23, 23);
  CHECK(map_hash(map, 24, 24) == h1);
  /* cells set raw by any tile of a terrain, then fixed: a generated level */
  for (int x = 12; x <= 16; x++) cw_tilemap2d_set_cell(map, 0, x, 6, 100, 0);
  cw_tilemap2d_fix_terrain(map, 0, 0, 0, 23, 23);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 12, 6), BLOB(E));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 14, 6), BLOB(E | W));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 16, 6), BLOB(W));
  cw_tree_free(t);
}

/* the scene form: cells name a terrain by any of its tiles and autotile picks; painting rebuilds colliders */
static void t_terrain_scene(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node map = cw_node_new("TileMap2D", "Map");
  /* cells before the rules, as a hand-written file may have them */
  cw_node_set(map, "cells0", cw_val_string("100*4 / 100*4 / 100*4 / . / . . 315 . / . / ."));
  cw_node_set(map, "terrains", cw_val_string("rock: blob47 100 x2; moss: corner16 300"));
  cw_node_set(map, "tile_shapes", cw_val_string("100-193: full"));
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 1, 1), 100);                        /* not yet: autotile is off */
  cw_node_set(map, "autotile", cw_val_bool(true));
  int mid = cw_tilemap2d_get_cell(map, 0, 1, 1);
  CHECK(mid == 146 || mid == 146 + 47);                                          /* one of the two variants of "surrounded" */
  int c00 = cw_tilemap2d_get_cell(map, 0, 0, 0);
  CHECK(c00 == BLOB(E | SE | S) || c00 == BLOB(E | SE | S) + 47);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 2, 4), 315);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 1, 3), 300 + 2);                    /* the whole tile grew its transitions */
  CHECK_EQ_INT(cw_tilemap2d_get_cell(map, 0, 3, 5), 300 + 8);
  cw_node col = cw_node_new("TileCollision2D", "Solid");
  cw_node_add_child(map, col);
  cw_node_add_child(root, map);
  step(t, 1);
  cw_tile_collision2d_stats s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 1);
  CHECK_EQ_INT(s.segments, 4); /* the 4 x 3 block, whichever variants drew it */
  /* paint a cell onto the block's side: one chunk rebuilt, a six-sided... an eight-cornered outline */
  cw_tilemap2d_set_terrain(map, 0, 4, 1, cw_tilemap2d_terrain(map, "rock"));
  step(t, 1);
  s = cw_tile_collision2d_stats_get(col);
  CHECK_EQ_INT(s.chains, 1);
  CHECK_EQ_INT(s.segments, 8);
  CHECK_EQ_INT(s.builds, 2);
  /* a saved scene holds the picked tiles and loads to the same map, autotile and all */
  uint32_t h = map_hash(map, 6, 7);
  char *text = cw_scene_save(map);
  CHECK(strstr(text, "terrains = \"rock: blob47 100 x2; moss: corner16 300\"") != NULL);
  CHECK(strstr(text, "autotile = true") != NULL);
  cw_packed_scene *ps = cw_scene_parse(text, "saved.cwscene");
  cw_free(text);
  cw_node again = ps ? cw_scene_instance(ps) : (cw_node){ 0 };
  CHECK(cw_node_valid(again));
  if (cw_node_valid(again)) {
    CHECK(map_hash(again, 6, 7) == h);
    cw_node_free(again);
  }
  if (ps) cw_packed_scene_free(ps);
  FOLD(h);
  cw_tree_free(t);
}

CW_TEST_SUITE("tiles2d", { "terrain_sets", t_terrain_sets }, { "terrain_paint", t_terrain_paint }, { "terrain_scene", t_terrain_scene }, { "shapes", t_shapes }, { "merge", t_merge }, { "slide", t_slide }, { "slopes", t_slopes },
              { "one_way", t_one_way }, { "edits", t_edits }, { "rollback", t_rollback })

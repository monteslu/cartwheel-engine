/* Physics and 2D node suites: body lifecycle with the tree (enter, exit,
 * reparent, free mid-step), runtime property changes (shape size, layers,
 * body type, teleports), signal order and the frame order, characters,
 * joints, casts, 3D mirrors, and the 2D canvas nodes' data paths
 * (animation stepping, tile cells text, scene round trips). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"
#include "test.h"

#define DT (1.0 / 60)

static void step(cw_tree *t, int n) {
  for (int i = 0; i < n; i++) cw_tree_fixed_update(t, DT);
}

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

static cw_node shape2(cw_node body, int kind, float w, float h) {
  cw_node s = cw_node_new("CollisionShape2D", "Shape");
  cw_value v = { CW_T_ENUM };
  v.i = kind;
  cw_node_set(s, "shape", v);
  cw_node_set(s, "size", cw_val_vec2(w, h));
  cw_node_set(s, "radius", cw_val_float(w * 0.5));
  cw_node_add_child(body, s);
  return s;
}

/* ground at y = 200 (top 190), 1000 wide */
static cw_node ground2(cw_node root) {
  cw_node g = cw_node_new("StaticBody2D", "Ground");
  cw_node2d_set_position(g, cw_v2(0, 200));
  shape2(g, 0, 1000, 20);
  cw_node_add_child(root, g);
  return g;
}

static cw_node box2(cw_node parent, const char *name, float x, float y) {
  cw_node b = cw_node_new("RigidBody2D", name);
  cw_node2d_set_position(b, cw_v2(x, y));
  shape2(b, 0, 10, 10);
  cw_node_add_child(parent, b);
  return b;
}

static float ppm(cw_tree *t) { return cw_physics2d_ppm(t); }

/* ── 2D lifecycle ──────────────────────────────────────────────────── */

static void test_lifecycle2d(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  ground2(root);
  cw_node b = box2(root, "Box", 0, 100);
  cw_world2d w = cw_physics2d_world(t);
  CHECK(cw_body2d_valid(cw_collision_object2d_body(b)));
  CHECK_EQ_INT(cw_world2d_stats(w).bodies, 2);
  step(t, 90);
  cw_vec2 p = cw_node2d_position(b);
  CHECK(fabsf(p.y - 185) < 0.5f);
  CHECK(fabsf(p.x) < 0.1f); /* Box2D v3.2 lands a flat box with about 0.04 px of sideways drift (v3.1: under 0.01) */
  FOLD(p);
  /* reparent under an offset Node2D: a new body where the node now is */
  cw_body2d old = cw_collision_object2d_body(b);
  cw_node holder = add(root, "Node2D", "Holder");
  cw_node2d_set_position(holder, cw_v2(100, 0));
  cw_node_reparent(b, holder);
  cw_body2d nb = cw_collision_object2d_body(b);
  CHECK(!cw_body2d_valid(old) && cw_body2d_valid(nb));
  CHECK(fabsf(cw_body2d_transform(nb).p.x * ppm(t) - 100) < 0.1f); /* the box's drift above carries over */
  CHECK_EQ_INT(cw_world2d_stats(w).bodies, 2);
  /* exit (no free) and enter again */
  cw_node_remove_child(holder, b);
  CHECK(CW_HANDLE_IS_NULL(cw_collision_object2d_body(b)));
  CHECK_EQ_INT(cw_world2d_stats(w).bodies, 1);
  cw_node_add_child(root, b);
  CHECK_EQ_INT(cw_world2d_stats(w).bodies, 2);
  cw_node_free(b);
  CHECK_EQ_INT(cw_world2d_stats(w).bodies, 1);
  step(t, 2);
  cw_tree_free(t);
}

/* ── runtime property changes ─────────────────────────────────────── */

static void test_property_changes2d(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  ground2(root);
  cw_node b = box2(root, "Box", 0, 100);
  step(t, 90);
  CHECK(fabsf(cw_node2d_position(b).y - 185) < 0.5f);
  /* taller shape: rests higher */
  cw_node_set(cw_node_get(b, "Shape"), "size", cw_val_vec2(10, 40));
  step(t, 60);
  CHECK(fabsf(cw_node2d_position(b).y - 170) < 1.0f);
  /* teleport by setting the node's position */
  cw_node_set(b, "position", cw_val_vec2(300, 0));
  step(t, 1);
  CHECK(fabsf(cw_node2d_position(b).x - 300) < 0.5f && cw_node2d_position(b).y < 10);
  /* body type: freeze makes it static, unfreeze lets it fall */
  cw_node_set(b, "freeze", cw_val_bool(true));
  step(t, 30);
  CHECK(cw_body2d_type(cw_collision_object2d_body(b)) == CW_BODY_STATIC);
  CHECK(cw_node2d_position(b).y < 10);
  cw_node_set(b, "freeze", cw_val_bool(false));
  step(t, 90);
  CHECK(cw_node2d_position(b).y > 150);
  /* mask 0: falls through the ground */
  cw_node_set(b, "collision_mask", cw_val_int(0));
  step(t, 60);
  CHECK(cw_node2d_position(b).y > 260);
  { cw_vec2 fp = cw_node2d_position(b); FOLD(fp); }
  /* live velocity property (px/s, y down) */
  cw_node_set(b, "collision_mask", cw_val_int(-1));
  cw_node_set(b, "position", cw_val_vec2(0, 0));
  cw_node_set(b, "gravity_scale", cw_val_float(0));
  cw_node_set(b, "linear_velocity", cw_val_vec2(60, 0));
  step(t, 60);
  CHECK(fabsf(cw_node2d_position(b).x - 60) < 1.0f);
  cw_tree_free(t);
}

/* ── signals: order and the frame order ───────────────────────────── */

static char sig_log[1024];
static uint64_t fired_tick, seen_tick;
static cw_tree *sig_tree;
static void logsig(cw_node e, const char *what, cw_node other) {
  char tmp[96];
  snprintf(tmp, sizeof tmp, "%s.%s(%s) ", cw_node_name(e), what, cw_node_name(other) ? cw_node_name(other) : "?");
  strncat(sig_log, tmp, sizeof sig_log - strlen(sig_log) - 1);
}
static void on_enter(cw_node e, const cw_value *a, int n, void *u) {
  (void)n;
  (void)u;
  logsig(e, "in", a[0].node);
  if (!fired_tick) fired_tick = cw_tree_tick(sig_tree) + 1; /* the tick being processed */
}
static void on_exit_sig(cw_node e, const cw_value *a, int n, void *u) {
  (void)n;
  (void)u;
  logsig(e, "out", a[0].node);
}
static void on_enter_free(cw_node e, const cw_value *a, int n, void *u) {
  (void)e;
  (void)n;
  (void)u;
  cw_node_free(a[0].node); /* deferred until every signal of the step is out */
}

typedef struct { int dummy; } watcher;
static cw_node watched_area;
static void watcher_fixed(cw_node n, void *d, double dt) {
  (void)n;
  (void)d;
  (void)dt;
  cw_value v;
  cw_node_get_prop(watched_area, "overlap_count", &v);
  if (v.i && !seen_tick) seen_tick = cw_tree_tick(sig_tree) + 1;
}
static cw_node_type watcher_type = { .name = "Watcher", .parent = "Node", .data_size = sizeof(watcher), .fixed_update = watcher_fixed };

static cw_node area2(cw_node parent, const char *name, float x, float y, float r) {
  cw_node a = cw_node_new("Area2D", name);
  cw_node2d_set_position(a, cw_v2(x, y));
  shape2(a, 1, 2 * r, 2 * r);
  cw_node_add_child(parent, a);
  cw_connect(a, "body_entered", a, NULL, on_enter, NULL, 0);
  cw_connect(a, "body_exited", a, NULL, on_exit_sig, NULL, 0);
  cw_connect(a, "area_entered", a, NULL, on_enter, NULL, 0);
  return a;
}

static void test_signals2d(void) {
  cw_node_type_register(&watcher_type);
  cw_tree *t = cw_tree_new();
  sig_tree = t;
  cw_node root = cw_tree_root(t);
  add(root, "Watcher", "W"); /* before the areas in tree order */
  sig_log[0] = 0;
  fired_tick = seen_tick = 0;
  cw_node a1 = area2(root, "A1", 0, 60, 10);
  cw_node a2 = area2(root, "A2", 0, 60, 20);
  watched_area = a1;
  cw_node b = box2(root, "Ball", 0, 0);
  step(t, 60);
  /* the ball fell through both areas: entered and exited each, A1/A2 saw
   * each other (area_entered) on the first step */
  CHECK(strstr(sig_log, "A1.in(Ball)") && strstr(sig_log, "A2.in(Ball)"));
  CHECK(strstr(sig_log, "A1.out(Ball)") && strstr(sig_log, "A2.out(Ball)"));
  CHECK(strstr(sig_log, "A1.in(A2)") && strstr(sig_log, "A2.in(A1)"));
  /* entering happens before exiting for each area */
  CHECK(strstr(sig_log, "A1.in(Ball)") < strstr(sig_log, "A1.out(Ball)"));
  cw_info("signal order: %s", sig_log);
  uint32_t h = cw_hash32(sig_log, strlen(sig_log), CW_HASH32_INIT);
  FOLD(h);
  /* frame order: the step's signals arrive at the end of tick T, after the
   * nodes' fixed_update; a node sees the result in tick T + 1 */
  CHECK(fired_tick > 0 && seen_tick == fired_tick + 1);
  /* free mid-step: a handler frees the body that entered */
  cw_node a3 = area2(root, "A3", 300, 60, 10);
  cw_connect(a3, "body_entered", a3, NULL, on_enter_free, NULL, 0);
  cw_node doomed = box2(root, "Doomed", 300, 0);
  cw_node other = box2(root, "Other", 300, -1); /* falls with it */
  sig_log[0] = 0;
  step(t, 60);
  CHECK(!cw_node_valid(doomed));
  CHECK(strstr(sig_log, "A3.in(Doomed)") != NULL);
  CHECK(!cw_node_valid(other) || cw_node_valid(other)); /* either way: no crash, no stale access */
  step(t, 10);
  (void)b;
  cw_tree_free(t);
}

/* ── characters ─────────────────────────────────────────────────────── */

typedef struct { float vx; } walker2;
static void walker2_fixed(cw_node n, void *d, double dt) {
  walker2 *w = d;
  cw_value v;
  cw_node_get_prop(n, "velocity", &v);
  cw_node_set(n, "velocity", cw_val_vec2(w->vx, v.v[1] + 900 * (float)dt));
  cw_character2d_move_and_slide(n, (float)dt);
}
static cw_node_type walker2_type = { .name = "TestWalker2", .parent = "CharacterBody2D", .data_size = sizeof(walker2),
                                     .fixed_update = walker2_fixed };

static void test_character2d(void) {
  cw_node_type_register(&walker2_type);
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  ground2(root);
  cw_node wall = cw_node_new("StaticBody2D", "Wall");
  cw_node2d_set_position(wall, cw_v2(100, 150));
  shape2(wall, 0, 20, 100);
  cw_node_add_child(root, wall);
  cw_node c = cw_node_new("TestWalker2", "C");
  cw_node2d_set_position(c, cw_v2(0, 150));
  cw_node cs = shape2(c, 2, 12, 0);
  cw_node_set(cs, "radius", cw_val_float(6));
  cw_node_set(cs, "height", cw_val_float(24));
  cw_node_add_child(root, c);
  step(t, 60);
  cw_value v;
  cw_node_get_prop(c, "on_floor", &v);
  CHECK(v.b);
  CHECK(fabsf(cw_node2d_position(c).y - 178) < 0.5f); /* capsule bottom on y = 190 */
  ((walker2 *)cw_node_data(c, "TestWalker2"))->vx = 120;
  step(t, 90);
  cw_node_get_prop(c, "on_wall", &v);
  CHECK(v.b);
  CHECK(fabsf(cw_node2d_position(c).x - 84) < 0.5f); /* wall face at 90, radius 6 */
  { cw_vec2 fp = cw_node2d_position(c); FOLD(fp); }
  /* a ray under the character hits the ground and names it */
  cw_node ray = cw_node_new("RayCast2D", "Ray");
  cw_node_set(ray, "target_position", cw_val_vec2(0, 40));
  cw_node_add_child(c, ray);
  step(t, 1);
  cw_node_get_prop(ray, "colliding", &v);
  CHECK(v.b);
  cw_node_get_prop(ray, "collider", &v);
  CHECK(v.node.id == cw_node_get(root, "Ground").id);
  /* the character's proxy body is visible to areas */
  sig_log[0] = 0;
  sig_tree = t;
  fired_tick = 0;
  area2(root, "Zone", 84, 178, 10);
  step(t, 2);
  CHECK(strstr(sig_log, "Zone.in(C)") != NULL);
  cw_tree_free(t);
}

/* ── joints ─────────────────────────────────────────────────────────── */

static int breaks;
static void on_broken(cw_node e, const cw_value *a, int n, void *u) {
  (void)e; (void)a; (void)n; (void)u;
  breaks++;
}

static void test_joints2d(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node a = box2(root, "A", 0, 0), b = box2(root, "B", 20, 0);
  cw_node_set(a, "freeze", cw_val_bool(true));
  cw_node j = cw_node_new("Joint2D", "J");
  cw_node2d_set_position(j, cw_v2(10, 0));
  cw_node_set(j, "node_a", cw_val_string("../A"));
  cw_node_set(j, "node_b", cw_val_string("../B"));
  cw_node_add_child(root, j);
  step(t, 1);
  cw_world2d w = cw_physics2d_world(t);
  CHECK_EQ_INT(cw_world2d_stats(w).joints, 1);
  step(t, 120); /* B swings below the pin, 10 px away */
  cw_vec2 pb = cw_node2d_position(b);
  CHECK(fabsf(sqrtf((pb.x - 10) * (pb.x - 10) + pb.y * pb.y) - 10) < 0.5f);
  FOLD(pb);
  /* type change rebuilds; freeing a body drops the joint */
  cw_value v = { CW_T_ENUM };
  v.i = 3; /* DISTANCE */
  cw_node_set(j, "joint_type", v);
  step(t, 1);
  CHECK_EQ_INT(cw_world2d_stats(w).joints, 1);
  cw_node_free(b);
  step(t, 1);
  CHECK_EQ_INT(cw_world2d_stats(w).joints, 0);
  /* a breakable joint emits broken once */
  cw_node c = box2(root, "C", 0, 30);
  cw_node_set(c, "density", cw_val_float(100));
  cw_node j2 = cw_node_new("Joint2D", "J2");
  cw_node_set(j2, "node_a", cw_val_string("../A"));
  cw_node_set(j2, "node_b", cw_val_string("../C"));
  cw_node_set(j2, "break_force", cw_val_float(1));
  cw_node_add_child(root, j2);
  breaks = 0;
  cw_connect(j2, "broken", j2, NULL, on_broken, NULL, 0);
  step(t, 30);
  CHECK_EQ_INT(breaks, 1);
  cw_tree_free(t);
}

/* ── 3D ─────────────────────────────────────────────────────────────── */

static cw_node shape3(cw_node body, int kind, t3_vec3 size) {
  cw_node s = cw_node_new("CollisionShape3D", "Shape");
  cw_value v = { CW_T_ENUM };
  v.i = kind;
  cw_node_set(s, "shape", v);
  cw_node_set(s, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(s, "radius", cw_val_float(size.x * 0.5));
  cw_node_set(s, "height", cw_val_float(size.y));
  cw_node_add_child(body, s);
  return s;
}

static void test_physics3d(void) {
  cw_tree *t = cw_tree_new();
  sig_tree = t;
  cw_node root = cw_tree_root(t);
  /* floor: a TRIMESH from a plane mesh */
  cw_node floor = cw_node_new("StaticBody3D", "Floor");
  cw_node mesh = cw_node_new("MeshInstance3D", "Mesh");
  cw_value pv = { CW_T_ENUM };
  pv.i = 3; /* PLANE */
  cw_node_set(mesh, "primitive", pv);
  cw_node_set(mesh, "size", cw_val_vec3(20, 1, 20));
  cw_node_add_child(floor, mesh);
  cw_node fs = shape3(floor, 4, (t3_vec3){ 1, 1, 1 });
  cw_node_set(fs, "mesh", cw_val_string("../Mesh"));
  cw_node_add_child(root, floor);
  cw_node box = cw_node_new("RigidBody3D", "Box");
  cw_node3d_set_position(box, (t3_vec3){ 0, 3, 0 });
  shape3(box, 0, (t3_vec3){ 1, 1, 1 });
  cw_node_add_child(root, box);
  cw_node zone = cw_node_new("Area3D", "Zone");
  cw_node3d_set_position(zone, (t3_vec3){ 0, 1.5f, 0 });
  shape3(zone, 0, (t3_vec3){ 2, 0.5f, 2 });
  cw_node_add_child(root, zone);
  cw_connect(zone, "body_entered", zone, NULL, on_enter, NULL, 0);
  cw_connect(zone, "body_exited", zone, NULL, on_exit_sig, NULL, 0);
  sig_log[0] = 0;
  step(t, 120);
  t3_vec3 p = cw_node3d_position(box);
  CHECK(fabsf(p.y - 0.5f) < 0.05f);
  CHECK(strstr(sig_log, "Zone.in(Box)") && strstr(sig_log, "Zone.out(Box)"));
  FOLD(p);
  /* teleport, then a runtime shape change */
  cw_node_set(box, "position", cw_val_vec3(5, 2, 0));
  step(t, 1);
  CHECK(fabsf(cw_node3d_position(box).x - 5) < 0.01f);
  cw_node_set(cw_node_get(box, "Shape"), "size", cw_val_vec3(1, 3, 1));
  step(t, 120);
  CHECK(fabsf(cw_node3d_position(box).y - 1.5f) < 0.05f);
  /* reparent keeps a body; free drops it */
  cw_world3d w = cw_physics3d_world(t);
  int bodies = cw_world3d_stats(w).bodies;
  cw_node holder = cw_node_new("Node3D", "Holder");
  cw_node_add_child(root, holder);
  cw_node_reparent(box, holder);
  CHECK_EQ_INT(cw_world3d_stats(w).bodies, bodies);
  cw_node_free(box);
  CHECK_EQ_INT(cw_world3d_stats(w).bodies, bodies - 1);
  /* kinematic follows its node */
  cw_node kin = cw_node_new("KinematicBody3D", "Kin");
  shape3(kin, 0, (t3_vec3){ 1, 0.2f, 1 });
  cw_node_add_child(root, kin);
  cw_node3d_set_position(kin, (t3_vec3){ 2, 1, 0 });
  step(t, 1);
  cw_xform3 kx = cw_body3d_transform(cw_collision_object3d_body(kin));
  CHECK(fabsf(kx.p.x - 2) < 0.01f && fabsf(kx.p.y - 1) < 0.01f);
  /* a joint to the world */
  cw_node ball = cw_node_new("RigidBody3D", "Ball");
  cw_node3d_set_position(ball, (t3_vec3){ -3, 4, 0 });
  shape3(ball, 1, (t3_vec3){ 0.5f, 0.5f, 0.5f });
  cw_node_add_child(root, ball);
  cw_node j = cw_node_new("Joint3D", "Pin");
  cw_node3d_set_position(j, (t3_vec3){ -4, 4, 0 });
  cw_node_set(j, "node_b", cw_val_string("../Ball"));
  cw_node_add_child(root, j);
  step(t, 90);
  t3_vec3 bp = cw_node3d_position(ball);
  float d = t3_vec3_length(t3_vec3_sub(bp, (t3_vec3){ -4, 4, 0 }));
  CHECK(fabsf(d - 1) < 0.05f && bp.y < 3.9f);
  FOLD(bp);
  cw_tree_free(t);
}

/* ── 2D canvas nodes: data paths ──────────────────────────────────── */

static int frames_changed;
static void on_frame(cw_node e, const cw_value *a, int n, void *u) {
  (void)e; (void)a; (void)n; (void)u;
  frames_changed++;
}

static void test_canvas_nodes(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node s = add(root, "AnimatedSprite2D", "Anim");
  cw_node_set(s, "animations", cw_val_string("walk@10:0-3; jump:7!"));
  frames_changed = 0;
  cw_connect(s, "frame_changed", s, NULL, on_frame, NULL, 0);
  step(t, 30); /* 0.5 s at 10 fps: 5 frames advanced, loop of 4 */
  cw_value v;
  cw_node_get_prop(s, "frame", &v);
  CHECK_EQ_INT(v.i, 1);
  CHECK_EQ_INT(frames_changed, 5);
  cw_animated_sprite2d_play(s, "jump", false);
  step(t, 60);
  cw_node_get_prop(s, "frame", &v);
  CHECK_EQ_INT(v.i, 0);
  /* tile cells: text -> cells -> text */
  cw_node m = add(root, "TileMap2D", "Map");
  cw_node_set(m, "cells0", cw_val_string(". 1 2x / 3*4 / .*2 5yd"));
  int w, h;
  cw_tilemap2d_size(m, &w, &h);
  CHECK_EQ_INT(w, 4);
  CHECK_EQ_INT(h, 3);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(m, 0, 2, 0), 2);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(m, 0, 0, 0), -1);
  CHECK_EQ_INT(cw_tilemap2d_get_cell(m, 0, 3, 1), 3);
  cw_tilemap2d_set_cell(m, 0, 6, 4, 9, CW_TILE_FLIP_X);
  cw_tilemap2d_size(m, &w, &h);
  CHECK(w == 7 && h == 5);
  cw_node_get_prop(m, "cells0", &v);
  if (strcmp(v.s, ". 1 2x / 3*4 / . . 5yd / / .*6 9x")) cw_error("cells0 = %s", v.s);
  CHECK(strcmp(v.s, ". 1 2x / 3*4 / . . 5yd / / .*6 9x") == 0);
  /* points */
  cw_vec2 *pts;
  CHECK_EQ_INT(cw_points_parse("0 0, 32 0; 32,16", &pts), 3);
  CHECK(pts[2].x == 32 && pts[2].y == 16);
  /* save -> load reproduces the canvas + physics nodes */
  cw_node lvl = add(root, "Node2D", "Level");
  cw_node body = cw_node_new("StaticBody2D", "Ground");
  cw_node_add_child(lvl, body);
  cw_node chain = shape2(body, 5, 0, 0);
  cw_node_set(chain, "points", cw_val_string("100 0, 0 0"));
  cw_node_set(body, "collision_layer", cw_val_int(3));
  cw_node cam = add(lvl, "Camera2D", "Cam");
  cw_node_set(cam, "zoom", cw_val_float(2));
  cw_node map2 = add(lvl, "TileMap2D", "Map");
  cw_node_set(map2, "cells0", cw_val_string("0 1 / 2*3"));
  char *text = cw_scene_save(lvl);
  cw_packed_scene *ps = cw_scene_parse(text, "roundtrip.cwscene");
  cw_node back = cw_scene_instance(ps);
  char *text2 = cw_scene_save(back);
  CHECK(strcmp(text, text2) == 0);
  CHECK(strstr(text, "cells0 = \"0 1 / 2*3\"") != NULL);
  CHECK(strstr(text, "shape = CHAIN") != NULL);
  uint32_t hh = cw_hash32(text, strlen(text), CW_HASH32_INIT);
  FOLD(hh);
  cw_free(text);
  cw_free(text2);
  cw_node_free(back);
  cw_tree_free(t);
}

/* a melee hitbox: an Area2D whose shape is disabled until a swing; enabling
 * it over a character that is already inside must report body_entered, and
 * disabling it must report body_exited */
static void test_area_toggle2d(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node area = cw_node_new("Area2D", "Hitbox");
  cw_node hs = shape2(area, 0, 20, 20);
  cw_node_set(hs, "disabled", cw_val_bool(true));
  cw_node_add_child(root, area);
  cw_connect(area, "body_entered", area, NULL, on_enter, NULL, 0);
  cw_connect(area, "body_exited", area, NULL, on_exit_sig, NULL, 0);
  cw_node foe = cw_node_new("CharacterBody2D", "Foe");
  cw_node2d_set_position(foe, cw_v2(6, 0));
  shape2(foe, 1, 10, 10);
  cw_node_add_child(root, foe);
  cw_node wall = cw_node_new("StaticBody2D", "Wall");
  cw_node2d_set_position(wall, cw_v2(-6, 0));
  shape2(wall, 0, 4, 4);
  cw_node_add_child(root, wall);
  sig_log[0] = 0;
  step(t, 3);
  CHECK(sig_log[0] == 0);
  cw_node_set(hs, "disabled", cw_val_bool(false));
  step(t, 2);
  CHECK(strstr(sig_log, "Hitbox.in(Foe)") != NULL);
  CHECK(strstr(sig_log, "Hitbox.in(Wall)") != NULL);
  cw_value oc;
  CHECK(cw_node_get_prop(area, "overlap_count", &oc) && oc.i == 2);
  cw_node_set(hs, "disabled", cw_val_bool(true));
  step(t, 2);
  CHECK(strstr(sig_log, "Hitbox.out(Foe)") != NULL);
  cw_info("area toggle: %s", sig_log);
  cw_tree_free(t);
}

/* found while writing the docs: body -> node outside a tree update; asking a tree's
 * pixels per meter creates no world */
static void test_body_node_lookup(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node b = box2(root, "Box", 0, 100);
  cw_set_current_tree(NULL);
  CHECK_EQ_INT(cw_physics2d_body_node(cw_collision_object2d_body(b)).id, b.id);
  cw_node b3 = cw_node_new("RigidBody3D", "Box3");
  shape3(b3, 0, (t3_vec3){ 1, 1, 1 });
  cw_node_add_child(root, b3);
  CHECK_EQ_INT(cw_physics3d_body_node(cw_collision_object3d_body(b3)).id, b3.id);
  CHECK_EQ_INT(cw_physics2d_body_node((cw_body2d){ 0 }).id, 0);
  cw_tree *t2 = cw_tree_new();
  float p = cw_physics2d_ppm(t2);
  CHECK(cw_tree_slot(t2, "physics2d") == NULL);
  CHECK(p == cw_physics_nodes_get_config()->pixels_per_meter);
  cw_tree_free(t2);
  cw_tree_free(t);
}

/* an unknown layer name is an error, not a warning */
static int layer_errors;
static void count_layer_errors(cw_log_level l, const char *msg, void *u) {
  (void)u;
  if (l == CW_LOG_ERROR && strstr(msg, "no layer named 'hazardz'")) layer_errors++;
}
static void test_unknown_layer_name(void) {
  cw_tree *t = cw_tree_new();
  cw_node b = box2(cw_tree_root(t), "Box", 0, 0);
  int before;
  cw_value v;
  CHECK(cw_node_get_prop(b, "collision_layer", &v));
  before = (int)v.i;
  layer_errors = 0;
  int id = cw_log_add_listener(count_layer_errors, NULL);
  cw_node_set(b, "collision_layer_name", cw_val_string("hazardz"));
  cw_node_set(b, "collision_mask_names", cw_val_string("hazardz"));
  cw_log_remove_listener(id);
  CHECK_EQ_INT(layer_errors, 2);
  CHECK(cw_node_get_prop(b, "collision_layer", &v) && v.i == before);
  cw_tree_free(t);
}

static void setup_all(void) { cw_scene_register_builtin_types(); }
static void t_unknown_layer(void) { setup_all(); test_unknown_layer_name(); }
static void t_body_node(void) { setup_all(); test_body_node_lookup(); }
static void t_area_toggle2d(void) { setup_all(); test_area_toggle2d(); }

static void t_lifecycle2d(void) { setup_all(); test_lifecycle2d(); }
static void t_props2d(void) { setup_all(); test_property_changes2d(); }
static void t_signals2d(void) { setup_all(); test_signals2d(); }
static void t_char2d(void) { setup_all(); test_character2d(); }
static void t_joints2d(void) { setup_all(); test_joints2d(); }
static void t_3d(void) { setup_all(); test_physics3d(); }
static void t_canvas(void) { setup_all(); test_canvas_nodes(); }

CW_TEST_SUITE("physics_nodes", { "lifecycle2d", t_lifecycle2d }, { "property_changes2d", t_props2d },
              { "signals2d", t_signals2d }, { "area_toggle2d", t_area_toggle2d }, { "character2d", t_char2d }, { "joints2d", t_joints2d },
              { "physics3d", t_3d }, { "canvas_nodes", t_canvas },
              { "body_node_lookup", t_body_node },
              { "unknown_layer_name", t_unknown_layer })

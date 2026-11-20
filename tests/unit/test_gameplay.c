/* Gameplay node suites (cartwheel/gameplay.h and its neighbours): Timer in fixed ticks (repeat, one shot, pause,
 * restart from a handler, tree snapshots), the lists of what is inside an Area2D / Area3D. Results that must
 * agree on every host fold into the checksum. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gameplay.h"
#include "cartwheel/net.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene.h"
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

/* ── Timer ─────────────────────────────────────────────────────────── */

static int32_t fires;
static uint64_t fire_ticks[64];
static cw_tree *timer_tree;
static void on_timeout(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  if (fires < 64) fire_ticks[fires] = cw_tree_tick(timer_tree);
  fires++;
}
static void on_timeout_restart(cw_node e, const cw_value *a, int argc, void *u) {
  (void)a, (void)argc, (void)u;
  cw_timer_start(e, 0.05); /* 3 ticks */
}
static void on_timeout_free(cw_node e, const cw_value *a, int argc, void *u) {
  (void)a, (void)argc, (void)u;
  cw_node_free(e);
}

static void t_timer(void) {
  cw_tree *t = cw_tree_new();
  timer_tree = t;
  cw_node root = cw_tree_root(t);
  cw_node tm = cw_node_new("Timer", "T");
  cw_node_set(tm, "wait_time", cw_val_float(0.5));
  cw_node_set(tm, "autostart", cw_val_bool(true));
  cw_connect(tm, "timeout", tm, NULL, on_timeout, NULL, 0);
  fires = 0;
  CHECK(cw_timer_is_stopped(tm));
  cw_node_add_child(root, tm); /* ready: autostart */
  CHECK(!cw_timer_is_stopped(tm));
  CHECK_EQ_INT(cw_timer_ticks_left(tm), 30);
  step(t, 29);
  CHECK_EQ_INT(fires, 0);
  CHECK_EQ_INT(cw_timer_ticks_left(tm), 1);
  step(t, 1);
  CHECK_EQ_INT(fires, 1);
  /* cw_tree_tick counts finished ticks: the 30th tick is running when the timer fires */
  CHECK_EQ_INT(fire_ticks[0], 29);
  step(t, 60); /* repeats every 30 */
  CHECK_EQ_INT(fires, 3);
  CHECK_EQ_INT(fire_ticks[2] - fire_ticks[1], 30);
  /* paused holds the count; the tree's pause too */
  step(t, 10);
  cw_node_set(tm, "paused", cw_val_bool(true));
  step(t, 100);
  CHECK_EQ_INT(fires, 3);
  CHECK_EQ_INT(cw_timer_ticks_left(tm), 20);
  cw_node_set(tm, "paused", cw_val_bool(false));
  cw_tree_set_paused(t, true);
  step(t, 100);
  CHECK_EQ_INT(fires, 3);
  cw_tree_set_paused(t, false);
  step(t, 20);
  CHECK_EQ_INT(fires, 4);
  /* time_left is whole ticks */
  cw_value v;
  step(t, 6);
  CHECK(cw_node_get_prop(tm, "time_left", &v) && v.type == CW_T_FLOAT);
  CHECK(v.f == 24 * DT);
  FOLD(v.f);
  /* stop; one_shot; the start method with a new wait */
  cw_timer_stop(tm);
  step(t, 100);
  CHECK_EQ_INT(fires, 4);
  CHECK(cw_timer_time_left(tm) == 0);
  cw_node_set(tm, "one_shot", cw_val_bool(true));
  cw_value arg = cw_val_float(0.1);
  CHECK(cw_node_call(tm, "start", &arg, 1, NULL));
  step(t, 5);
  CHECK_EQ_INT(fires, 4);
  step(t, 1);
  CHECK_EQ_INT(fires, 5);
  CHECK(cw_timer_is_stopped(tm));
  step(t, 60);
  CHECK_EQ_INT(fires, 5);
  CHECK(cw_node_call(tm, "start", NULL, 0, NULL));
  CHECK(cw_node_call(tm, "stop", NULL, 0, NULL));
  CHECK(cw_timer_is_stopped(tm));
  /* rounding: wait times that are not a whole number of ticks go to the nearest, never under one */
  static const struct { double w; int ticks; } R[] = { { 0.001, 1 }, { 1.0 / 60, 1 }, { 0.024, 1 }, { 0.026, 2 }, { 0.1, 6 },
                                                       { 1, 60 }, { 2.5, 150 }, { 0.3, 18 }, { 1.0 / 3, 20 } };
  for (size_t i = 0; i < CW_ARRAY_LEN(R); i++) {
    cw_timer_start(tm, R[i].w);
    CHECK_EQ_INT(cw_timer_ticks_left(tm), R[i].ticks);
    int64_t k = cw_timer_ticks_left(tm);
    FOLD(k);
  }
  cw_timer_stop(tm);
  /* a handler restarts its one-shot timer: a 3 tick repeat made by hand */
  cw_node rs = cw_node_new("Timer", "Restart");
  cw_node_set(rs, "one_shot", cw_val_bool(true));
  cw_node_set(rs, "wait_time", cw_val_float(0.05));
  cw_node_set(rs, "autostart", cw_val_bool(true));
  cw_connect(rs, "timeout", rs, NULL, on_timeout, NULL, 0);
  cw_connect(rs, "timeout", rs, NULL, on_timeout_restart, NULL, 0);
  cw_node_add_child(root, rs);
  fires = 0;
  step(t, 12);
  CHECK_EQ_INT(fires, 4);
  /* a handler frees its timer */
  cw_node doomed = cw_node_new("Timer", "Doomed");
  cw_node_set(doomed, "wait_time", cw_val_float(0.05));
  cw_node_set(doomed, "autostart", cw_val_bool(true));
  cw_connect(doomed, "timeout", doomed, NULL, on_timeout_free, NULL, 0);
  cw_node_add_child(root, doomed);
  step(t, 5);
  CHECK(!cw_node_valid(doomed));
  /* other node types: the calls answer like a stopped timer */
  CHECK(cw_timer_is_stopped(root) && cw_timer_ticks_left(root) == 0);
  cw_timer_start(root, 1);
  cw_tree_free(t);
}

/* scene text round trip; the live state is not saved */
static void t_timer_scene(void) {
  static const char *text = "[scene format=1]\n\n[node name=\"Level\" type=\"Node\"]\n\n"
                            "[node name=\"Spawn\" parent=\".\" type=\"Timer\"]\nwait_time = 2.5\none_shot = true\nautostart = true\n";
  cw_packed_scene *ps = cw_scene_parse(text, NULL);
  CHECK(ps != NULL);
  if (!ps) return;
  cw_node level = cw_scene_instance(ps);
  cw_node spawn = cw_node_get(level, "Spawn");
  CHECK(cw_node_is(spawn, "Timer"));
  cw_tree *t = cw_tree_new();
  cw_node_add_child(cw_tree_root(t), level);
  CHECK_EQ_INT(cw_timer_ticks_left(spawn), 150);
  step(t, 7);
  char *out = cw_scene_save(level);
  CHECK(out && !strcmp(out, text));
  if (out && strcmp(out, text)) cw_info("saved:\n%s", out);
  cw_free(out);
  cw_tree_free(t);
  cw_packed_scene_free(ps);
}

/* a tree snapshot carries a timer mid-count: restoring and running again fires on the same tick */
static void t_timer_snapshot(void) {
  cw_tree *t = cw_tree_new();
  timer_tree = t;
  cw_node tm = add(cw_tree_root(t), "Timer", "T");
  cw_node_set(tm, "wait_time", cw_val_float(0.25)); /* 15 ticks */
  cw_connect(tm, "timeout", tm, NULL, on_timeout, NULL, 0);
  cw_timer_start(tm, 0);
  step(t, 9);
  cw_state *st = cw_state_new();
  cw_state_save(st, t);
  uint32_t h0 = cw_state_hash(st);
  fires = 0;
  step(t, 20);
  CHECK_EQ_INT(fires, 1);
  uint64_t first = fire_ticks[0];
  cw_state_load(st, t);
  CHECK_EQ_INT(cw_timer_ticks_left(tm), 6);
  fires = 0;
  step(t, 20);
  CHECK_EQ_INT(fires, 1);
  CHECK_EQ_INT(fire_ticks[0], first);
  /* the state hash sees the count (a timer one tick apart is a different state) */
  cw_state_load(st, t);
  step(t, 1);
  cw_state *st2 = cw_state_new();
  cw_state_save(st2, t);
  CHECK(cw_state_hash(st2) != h0);
  FOLD(h0);
  cw_state_free(st);
  cw_state_free(st2);
  cw_tree_free(t);
}

/* ── what is inside an area ────────────────────────────────────────── */

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
static cw_node body2(cw_node parent, const char *type, const char *name, float x, float y, float w, float h) {
  cw_node b = cw_node_new(type, name);
  cw_node2d_set_position(b, cw_v2(x, y));
  shape2(b, 0, w, h);
  cw_node_add_child(parent, b);
  return b;
}
static void names(cw_node *list, int n, char *out, size_t cap) {
  out[0] = 0;
  for (int i = 0; i < n; i++) {
    strncat(out, cw_node_name(list[i]), cap - strlen(out) - 1);
    if (i + 1 < n) strncat(out, ",", cap - strlen(out) - 1);
  }
}

static void t_overlaps2d(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  /* a wide zone; kinematic bodies are moved in and out by hand (they do not fall) */
  cw_node zone = body2(root, "Area2D", "Zone", 0, 0, 200, 200);
  cw_node a = body2(root, "KinematicBody2D", "A", 500, 0, 10, 10);
  cw_node b = body2(root, "KinematicBody2D", "B", 500, 40, 10, 10);
  cw_node c = body2(root, "KinematicBody2D", "C", 500, -40, 10, 10);
  cw_node inner = body2(root, "Area2D", "Inner", 600, 0, 20, 20);
  cw_node hidden = body2(root, "Area2D", "Hidden", 20, 20, 20, 20);
  cw_node_set(hidden, "monitorable", cw_val_bool(false));
  step(t, 2);
  cw_node list[8];
  char txt[128];
  CHECK_EQ_INT(cw_area2d_overlapping_bodies(zone, list, 8), 0);
  CHECK_EQ_INT(cw_area2d_overlapping_areas(zone, list, 8), 0); /* Hidden is inside, and not monitorable */
  /* in one at a time: C, A, B. The list is in that order whatever the tree order */
  cw_node_set(c, "position", cw_val_vec2(0, -40));
  step(t, 2);
  cw_node_set(a, "position", cw_val_vec2(0, 0));
  step(t, 2);
  cw_node_set(b, "position", cw_val_vec2(0, 40));
  cw_node_set(inner, "position", cw_val_vec2(-50, 0));
  step(t, 2);
  int n = cw_area2d_overlapping_bodies(zone, list, 8);
  CHECK_EQ_INT(n, 3);
  names(list, n, txt, sizeof txt);
  CHECK(!strcmp(txt, "C,A,B"));
  uint32_t h = cw_hash32(txt, strlen(txt), CW_HASH32_INIT);
  FOLD(h);
  n = cw_area2d_overlapping_areas(zone, list, 8);
  CHECK_EQ_INT(n, 1);
  CHECK(n == 1 && list[0].id == inner.id);
  CHECK(cw_area2d_overlaps(zone, a) && cw_area2d_overlaps(zone, inner) && !cw_area2d_overlaps(zone, hidden));
  CHECK(cw_area2d_overlaps(inner, zone) && !cw_area2d_overlaps(a, zone)); /* a body is not an area */
  /* count only, and a short buffer: the total comes back either way */
  CHECK_EQ_INT(cw_area2d_overlapping_bodies(zone, NULL, 0), 3);
  list[1] = (cw_node){ 0 };
  CHECK_EQ_INT(cw_area2d_overlapping_bodies(zone, list, 1), 3);
  CHECK(list[0].id == c.id && CW_HANDLE_IS_NULL(list[1]));
  /* the methods (debug channel): names */
  cw_value r;
  CHECK(cw_node_call(zone, "get_overlapping_bodies", NULL, 0, &r) && r.type == CW_T_STRING && !strcmp(r.s, "C,A,B"));
  CHECK(cw_node_call(zone, "get_overlapping_areas", NULL, 0, &r) && !strcmp(r.s, "Inner"));
  cw_value arg = { CW_T_NODE };
  arg.node = a;
  CHECK(cw_node_call(zone, "overlaps", &arg, 1, &r) && r.type == CW_T_BOOL && r.b);
  /* one leaves: the others keep their order; it comes back at the end */
  cw_node_set(a, "position", cw_val_vec2(500, 0));
  step(t, 2);
  n = cw_area2d_overlapping_bodies(zone, list, 8);
  names(list, n, txt, sizeof txt);
  CHECK(!strcmp(txt, "C,B"));
  CHECK(!cw_area2d_overlaps(zone, a));
  cw_node_set(a, "position", cw_val_vec2(0, 0));
  step(t, 2);
  n = cw_area2d_overlapping_bodies(zone, list, 8);
  names(list, n, txt, sizeof txt);
  CHECK(!strcmp(txt, "C,B,A"));
  /* a snapshot restore keeps the order */
  cw_state *st = cw_state_new();
  cw_state_save(st, t);
  cw_node_set(c, "position", cw_val_vec2(500, -40));
  step(t, 2);
  CHECK_EQ_INT(cw_area2d_overlapping_bodies(zone, NULL, 0), 2);
  cw_state_load(st, t);
  n = cw_area2d_overlapping_bodies(zone, list, 8);
  names(list, n, txt, sizeof txt);
  CHECK(!strcmp(txt, "C,B,A"));
  cw_state_free(st);
  /* freed while inside: gone from the list */
  cw_node_free(b);
  step(t, 2);
  n = cw_area2d_overlapping_bodies(zone, list, 8);
  names(list, n, txt, sizeof txt);
  CHECK(!strcmp(txt, "C,A"));
  /* bodies that touch: a monitored rigid body resting on a floor lists the floor */
  cw_node floor = body2(root, "StaticBody2D", "Floor", 2000, 200, 400, 20);
  cw_node box = body2(root, "RigidBody2D", "Box", 2000, 170, 10, 10);
  cw_node_set(box, "contact_monitor", cw_val_bool(true));
  step(t, 60);
  n = cw_physics_body2d_colliding_bodies(box, list, 8);
  CHECK_EQ_INT(n, 1);
  CHECK(n == 1 && list[0].id == floor.id);
  CHECK_EQ_INT(cw_physics_body2d_colliding_bodies(floor, list, 8), 0); /* contact_monitor off */
  CHECK_EQ_INT(cw_area2d_overlapping_bodies(box, list, 8), 0);         /* not an area */
  cw_tree_free(t);
}

static cw_node body3(cw_node parent, const char *type, const char *name, t3_vec3 p, t3_vec3 size) {
  cw_node b = cw_node_new(type, name);
  cw_node3d_set_position(b, p);
  cw_node s = cw_node_new("CollisionShape3D", "Shape");
  cw_node_set(s, "shape", cw_val_enum("BOX"));
  cw_node_set(s, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_add_child(b, s);
  cw_node_add_child(parent, b);
  return b;
}

static void t_overlaps3d(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node zone = body3(root, "Area3D", "Zone", (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 6, 6, 6 });
  cw_node a = body3(root, "KinematicBody3D", "A", (t3_vec3){ 50, 0, 0 }, (t3_vec3){ 1, 1, 1 });
  cw_node b = body3(root, "KinematicBody3D", "B", (t3_vec3){ 50, 0, 2 }, (t3_vec3){ 1, 1, 1 });
  cw_node inner = body3(root, "Area3D", "Inner", (t3_vec3){ 60, 0, 0 }, (t3_vec3){ 1, 1, 1 });
  step(t, 30);
  cw_node list[8];
  char txt[128];
  CHECK_EQ_INT(cw_area3d_overlapping_bodies(zone, list, 8), 0);
  cw_node_set(b, "position", cw_val_vec3(0, 0, 2));
  step(t, 30);
  cw_node_set(a, "position", cw_val_vec3(0, 0, 0));
  cw_node_set(inner, "position", cw_val_vec3(-2, 0, 0));
  step(t, 30);
  int n = cw_area3d_overlapping_bodies(zone, list, 8);
  names(list, n, txt, sizeof txt);
  CHECK(!strcmp(txt, "B,A"));
  if (strcmp(txt, "B,A")) cw_info("3d list: %s", txt);
  uint32_t h = cw_hash32(txt, strlen(txt), CW_HASH32_INIT);
  FOLD(h);
  n = cw_area3d_overlapping_areas(zone, list, 8);
  CHECK(n == 1 && list[0].id == inner.id);
  CHECK(cw_area3d_overlaps(zone, a) && cw_area3d_overlaps(zone, inner));
  cw_value r;
  CHECK(cw_node_call(zone, "get_overlapping_bodies", NULL, 0, &r) && !strcmp(r.s, "B,A"));
  cw_node_set(b, "position", cw_val_vec3(50, 0, 2));
  step(t, 30);
  n = cw_area3d_overlapping_bodies(zone, list, 8);
  names(list, n, txt, sizeof txt);
  CHECK(!strcmp(txt, "A"));
  CHECK(!cw_area3d_overlaps(zone, b));
  /* a monitored rigid body on a floor */
  cw_node floor = body3(root, "StaticBody3D", "Floor", (t3_vec3){ 200, 0, 0 }, (t3_vec3){ 10, 1, 10 });
  cw_node box = body3(root, "RigidBody3D", "Box", (t3_vec3){ 200, 1.2f, 0 }, (t3_vec3){ 1, 1, 1 });
  cw_node_set(box, "contact_monitor", cw_val_bool(true));
  step(t, 60);
  n = cw_physics_body3d_colliding_bodies(box, list, 8);
  CHECK(n == 1 && list[0].id == floor.id);
  cw_tree_free(t);
}

/* ── on-screen notifiers ───────────────────────────────────────────── */

static int32_t entered, exited;
static void on_entered(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  entered++;
}
static void on_exited(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  exited++;
}

static void t_notifier3d(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node cam = add(root, "Camera3D", "Cam"); /* at the origin, looking down -Z, fov 70, the suite's square window */
  cw_node n = cw_node_new("VisibleOnScreenNotifier3D", "Seen");
  cw_connect(n, "screen_entered", n, NULL, on_entered, NULL, 0);
  cw_connect(n, "screen_exited", n, NULL, on_exited, NULL, 0);
  cw_node3d_set_position(n, (t3_vec3){ 0, 0, -10 });
  entered = exited = 0;
  cw_node_add_child(root, n);
  CHECK(!cw_notifier_is_on_screen(n)); /* not tested yet */
  step(t, 1);
  CHECK(cw_notifier_is_on_screen(n) && entered == 1 && exited == 0);
  step(t, 5);
  CHECK(entered == 1); /* once per change */
  /* behind the camera */
  cw_node3d_set_position(n, (t3_vec3){ 0, 0, 10 });
  step(t, 1);
  CHECK(!cw_notifier_is_on_screen(n) && exited == 1);
  /* the edge of the view at 10 m is tan(35 deg) x 10 = 7.0 m to the side; the box is 2 m wide */
  cw_node3d_set_position(n, (t3_vec3){ 7.9f, 0, -10 });
  step(t, 1);
  CHECK(cw_notifier_is_on_screen(n) && entered == 2);
  cw_node3d_set_position(n, (t3_vec3){ 8.8f, 0, -10 });
  step(t, 1);
  CHECK(!cw_notifier_is_on_screen(n) && exited == 2);
  /* a bigger box reaches back in; so does turning the camera toward it */
  cw_node_set(n, "size", cw_val_vec3(6, 2, 2));
  step(t, 1);
  CHECK(cw_notifier_is_on_screen(n) && entered == 3);
  cw_node_set(n, "size", cw_val_vec3(2, 2, 2));
  step(t, 1);
  CHECK(exited == 3);
  cw_node3d_look_at(cam, (t3_vec3){ 8.8f, 0, -10 }, (t3_vec3){ 0, 1, 0 });
  step(t, 1);
  CHECK(cw_notifier_is_on_screen(n) && entered == 4);
  /* a narrower lens loses a box at the side */
  cw_node3d_set_quaternion(cam, (t3_quat){ 0, 0, 0, 1 });
  cw_node3d_set_position(n, (t3_vec3){ 5, 0, -10 });
  step(t, 1);
  CHECK(cw_notifier_is_on_screen(n));
  cw_node_set(cam, "fov", cw_val_float(30));
  step(t, 1);
  CHECK(!cw_notifier_is_on_screen(n));
  cw_node_set(cam, "fov", cw_val_float(70));
  /* past the far plane, and before the near one */
  cw_node3d_set_position(n, (t3_vec3){ 0, 0, -2000 });
  step(t, 1);
  CHECK(!cw_notifier_is_on_screen(n));
  cw_node3d_set_position(n, (t3_vec3){ 0, 0, -10 });
  step(t, 1);
  CHECK(cw_notifier_is_on_screen(n));
  /* hidden is off screen; so is leaving the tree (the signal arrives) */
  int32_t e0 = exited;
  cw_node_set(n, "visible", cw_val_bool(false));
  step(t, 1);
  CHECK(!cw_notifier_is_on_screen(n) && exited == e0 + 1);
  cw_node_set(n, "visible", cw_val_bool(true));
  step(t, 1);
  CHECK(cw_notifier_is_on_screen(n));
  cw_node_remove_child(root, n);
  CHECK(!cw_notifier_is_on_screen(n) && exited == e0 + 2);
  cw_node_add_child(root, n);
  /* no camera: nothing is on screen */
  cw_node_free(cam);
  step(t, 2);
  CHECK(!cw_notifier_is_on_screen(n));
  int32_t counts[2] = { entered, exited };
  FOLD(counts);
  /* the 2D notifier needs a drawn frame for its view: with none, it stays off screen */
  cw_node n2 = cw_node_new("VisibleOnScreenNotifier2D", "Seen2");
  cw_node_add_child(root, n2);
  step(t, 2);
  CHECK(!cw_notifier_is_on_screen(n2));
  cw_value v;
  CHECK(cw_node_get_prop(n2, "size", &v) && v.v[0] == 20 && v.v[1] == 20);
  cw_tree_free(t);
}

/* ── spring arm ────────────────────────────────────────────────────── */

static void t_spring_arm(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  /* a wall across +Z whose near face is at z = 2 */
  cw_node wall = body3(root, "StaticBody3D", "Wall", (t3_vec3){ 0, 0, 2.5f }, (t3_vec3){ 10, 10, 1 });
  cw_node arm = add(root, "SpringArm3D", "Arm");
  cw_node cam = add(arm, "Camera3D", "Cam");
  CHECK(cw_node3d_position(cam).z == 0); /* loading a scene moves nothing: the first tick places it */
  step(t, 2);
  /* the 0.2 m sphere stops with its centre 0.2 m from the wall, and the arm 0.05 m short of that */
  float len = cw_spring_arm_length(arm);
  CHECK(fabsf(len - 1.75f) < 0.01f);
  CHECK(cw_node3d_position(cam).z == len && cw_node3d_position(cam).x == 0);
  FOLD(len);
  cw_value v;
  CHECK(cw_node_get_prop(arm, "hit_length", &v) && (float)v.f == len);
  /* a ray instead of a sphere reaches the wall itself */
  cw_node_set(arm, "radius", cw_val_float(0));
  step(t, 1);
  CHECK(fabsf(cw_spring_arm_length(arm) - 1.95f) < 0.01f);
  cw_node_set(arm, "radius", cw_val_float(0.2));
  /* a shorter arm than the gap is not touched */
  cw_node_set(arm, "spring_length", cw_val_float(1.5));
  step(t, 1);
  CHECK(cw_spring_arm_length(arm) == 1.5f);
  cw_node_set(arm, "spring_length", cw_val_float(4));
  step(t, 1);
  /* turned away from the wall: full length */
  cw_node3d_set_quaternion(arm, (t3_quat){ 0, 1, 0, 0 });
  step(t, 1);
  CHECK(cw_spring_arm_length(arm) == 4);
  cw_node3d_set_quaternion(arm, (t3_quat){ 0, 0, 0, 1 });
  step(t, 1);
  CHECK(fabsf(cw_spring_arm_length(arm) - 1.75f) < 0.01f);
  /* a layer the arm does not look at */
  cw_node_set(arm, "collision_mask", cw_val_int(2));
  step(t, 1);
  CHECK(cw_spring_arm_length(arm) == 4);
  cw_node_set(arm, "collision_mask", cw_val_int(-1));
  step(t, 1);
  /* the wall goes: back out at once by default, or at extend_speed */
  cw_node_set(arm, "extend_speed", cw_val_float(3));
  cw_node_free(wall);
  step(t, 2);
  step(t, 20);
  float growing = cw_spring_arm_length(arm);
  CHECK(growing > 1.75f + 0.9f && growing < 1.75f + 1.2f); /* about 22 ticks at 3 m/s */
  FOLD(growing);
  step(t, 60);
  CHECK(cw_spring_arm_length(arm) == 4);
  /* in the way again: it shortens at once whatever the speed */
  wall = body3(root, "StaticBody3D", "Wall", (t3_vec3){ 0, 0, 2.5f }, (t3_vec3){ 10, 10, 1 });
  step(t, 2);
  CHECK(fabsf(cw_spring_arm_length(arm) - 1.75f) < 0.01f);
  cw_node_free(wall);
  cw_node_free(arm);
  /* under a body: its own shape along the arm is ignored, unless exclude_parent is off */
  cw_node body = cw_node_new("KinematicBody3D", "Player");
  cw_node s = cw_node_new("CollisionShape3D", "Shape");
  cw_node_set(s, "shape", cw_val_enum("BOX"));
  cw_node_set(s, "size", cw_val_vec3(1, 1, 1));
  cw_node3d_set_position(s, (t3_vec3){ 0, 0, 2 });
  cw_node_add_child(body, s);
  cw_node_add_child(root, body);
  cw_node pivot = add(body, "Node3D", "Pivot");
  arm = add(pivot, "SpringArm3D", "Arm");
  cam = add(arm, "Camera3D", "Cam");
  step(t, 3);
  CHECK(cw_spring_arm_length(arm) == 4);
  cw_node_set(arm, "exclude_parent", cw_val_bool(false));
  step(t, 1);
  CHECK(cw_spring_arm_length(arm) < 1.5f);
  CHECK(cw_spring_arm_length(root) == 0);
  cw_tree_free(t);
}

/* What these nodes cost, printed (never checked): CW_TEST_COSTS=1 natively. */
#include <time.h>
#include "cartwheel/curve.h"
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }
static void t_costs(void) {
  if (!getenv("CW_TEST_COSTS")) return;
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  for (int i = 0; i < 1000; i++) {
    cw_node tm = add(root, "Timer", "T");
    cw_timer_start(tm, 0.5 + i * 0.001);
  }
  double a = now_ms();
  step(t, 600);
  cw_info("cost: 1000 running timers: %.4f ms per tick", (now_ms() - a) / 600);
  cw_tree_free(t);
  t = cw_tree_new();
  root = cw_tree_root(t);
  cw_node path = add(root, "Path3D", "P");
  cw_node_set(path, "points", cw_val_string("50 0 0  0 0 27.6  0 0 -27.6, 0 5 -50  27.6 0 0  -27.6 0 0, -50 0 0  0 0 -27.6  0 0 27.6, 0 -5 50  -27.6 0 0  27.6 0 0"));
  cw_node_set(path, "closed", cw_val_bool(true));
  a = now_ms();
  for (int i = 0; i < 200; i++) {
    cw_curve_set_bake_interval(cw_path_curve(path), 0.2f + (i & 1) * 0.001f);
    (void)cw_curve_length(cw_path_curve(path));
  }
  int pieces = cw_curve_baked(cw_path_curve(path), NULL, NULL);
  cw_info("cost: baking a 314 m closed curve at 0.2 m (%d pieces): %.4f ms", pieces, (now_ms() - a) / 200);
  for (int i = 0; i < 1000; i++) {
    cw_node f = add(path, "PathFollow3D", "F");
    cw_node_set(f, "rotation_mode", cw_val_enum("ORIENTED"));
    cw_node_set(f, "progress", cw_val_float(i * 0.3));
    cw_node_set(f, "speed", cw_val_float(5));
  }
  a = now_ms();
  step(t, 600);
  cw_info("cost: 1000 PathFollow3D with a speed: %.4f ms per tick", (now_ms() - a) / 600);
  a = now_ms();
  float acc = 0;
  for (int i = 0; i < 2000; i++) acc += cw_curve_closest_offset(cw_path_curve(path), (t3_vec3){ (float)(i % 100) - 50, 2, (float)(i % 77) - 38 });
  cw_info("cost: cw_curve_closest_offset on it: %.4f ms a call (%g)", (now_ms() - a) / 2000, (double)acc);
  cw_tree_free(t);
  t = cw_tree_new();
  root = cw_tree_root(t);
  add(root, "Camera3D", "Cam");
  for (int i = 0; i < 1000; i++) {
    cw_node n = add(root, "VisibleOnScreenNotifier3D", "N");
    cw_node3d_set_position(n, (t3_vec3){ (float)(i % 40) - 20, 0, -(float)(i / 40) });
  }
  a = now_ms();
  step(t, 600);
  cw_info("cost: 1000 VisibleOnScreenNotifier3D: %.4f ms per tick", (now_ms() - a) / 600);
  cw_tree_free(t);
  t = cw_tree_new();
  root = cw_tree_root(t);
  for (int i = 0; i < 200; i++) body3(root, "StaticBody3D", "W", (t3_vec3){ (float)(i % 20) * 3 - 30, 0, (float)(i / 20) * 3 }, (t3_vec3){ 1, 3, 1 });
  for (int i = 0; i < 100; i++) {
    cw_node arm = add(root, "SpringArm3D", "A");
    cw_node3d_set_position(arm, (t3_vec3){ (float)i * 0.5f - 25, 0.5f, -2 });
    add(arm, "Camera3D", "C");
  }
  step(t, 5);
  a = now_ms();
  step(t, 600);
  cw_info("cost: 100 SpringArm3D among 200 bodies: %.4f ms per tick (the physics step of the still world included)", (now_ms() - a) / 600);
  cw_tree_free(t);
}

CW_TEST_SUITE("gameplay", { "timer", t_timer }, { "timer_scene", t_timer_scene }, { "timer_snapshot", t_timer_snapshot },
              { "overlaps2d", t_overlaps2d }, { "overlaps3d", t_overlaps3d }, { "notifier3d", t_notifier3d },
              { "spring_arm", t_spring_arm }, { "costs", t_costs })

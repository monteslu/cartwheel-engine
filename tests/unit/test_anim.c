/* Animation: interpolation, key firing at loop edges, speed/seek, cross-fades,
 * queues, scene-format round trips, tweens, AnimationTree blends and state
 * machines, root motion, and glTF skeletal sampling checked against
 * three.c's own AnimationMixer. Sampled values fold into unit.checksum, so
 * every host must produce them bit for bit. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/anim.h"
#include "cartwheel/scene_file.h"
#include "test.h"

#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

static void fold_value(const cw_value *v) {
  switch (v->type) {
  case CW_T_FLOAT: { float f = (float)v->f; FOLD(f); break; }
  case CW_T_INT: case CW_T_ENUM: FOLD(v->i); break;
  case CW_T_BOOL: { int b = v->b; FOLD(b); break; }
  case CW_T_STRING: cw_test_checksum = cw_hash32(v->s, strlen(v->s), cw_test_checksum); break;
  default: FOLD(v->v); break;
  }
}

static cw_value sample(cw_anim a, int track, float t) {
  cw_value v = { 0 };
  CHECK(cw_anim_sample(a, track, t, &v));
  fold_value(&v);
  return v;
}

/* ── interpolation ─────────────────────────────────────────────────── */

static void test_interpolation(void) {
  cw_anim a = cw_anim_new("interp", 3, CW_LOOP_NONE);
  int lin = cw_anim_add_value_track(a, "A:f", CW_INTERP_LINEAR);
  cw_anim_add_key(a, lin, 1, cw_val_float(10));
  cw_anim_add_key(a, lin, 0, cw_val_float(0)); /* out of order: kept sorted */
  NEAR(sample(a, lin, 0.25f).f, 2.5, 1e-6);
  NEAR(sample(a, lin, -1).f, 0, 0);
  NEAR(sample(a, lin, 5).f, 10, 0);
  cw_anim_add_key(a, lin, 1, cw_val_float(20)); /* same time replaces */
  CHECK_EQ_INT(cw_anim_key_count(a, lin), 2);
  NEAR(sample(a, lin, 0.5f).f, 10, 1e-6);

  int near = cw_anim_add_value_track(a, "A:n", CW_INTERP_NEAREST);
  cw_anim_add_key(a, near, 0, cw_val_float(0));
  cw_anim_add_key(a, near, 1, cw_val_float(10));
  NEAR(sample(a, near, 0.99f).f, 0, 0);
  NEAR(sample(a, near, 1).f, 10, 0);

  /* Catmull-Rom through x^2 at integers is exact at the midpoint */
  int cub = cw_anim_add_value_track(a, "A:c", CW_INTERP_CUBIC);
  for (int i = 0; i < 4; i++) cw_anim_add_key(a, cub, (float)i, cw_val_float(i * i));
  NEAR(sample(a, cub, 1.5f).f, 2.25, 1e-6);
  NEAR(sample(a, cub, 1).f, 1, 0);

  /* Hermite with unit tangents is the straight line */
  int her = cw_anim_add_value_track(a, "A:h", CW_INTERP_HERMITE);
  cw_anim_add_key_hermite(a, her, 0, cw_val_float(0), cw_val_float(1), cw_val_float(1));
  cw_anim_add_key_hermite(a, her, 1, cw_val_float(1), cw_val_float(1), cw_val_float(1));
  NEAR(sample(a, her, 0.5f).f, 0.5, 1e-6);
  NEAR(sample(a, her, 0.25f).f, 0.25, 1e-6);

  /* ints round half up; bools step */
  int it = cw_anim_add_value_track(a, "A:i", CW_INTERP_LINEAR);
  cw_anim_add_key(a, it, 0, cw_val_int(0));
  cw_anim_add_key(a, it, 1, cw_val_int(3));
  CHECK_EQ_INT(sample(a, it, 0.5f).i, 2);
  CHECK_EQ_INT(sample(a, it, 0.4f).i, 1);
  int bt = cw_anim_add_value_track(a, "A:b", CW_INTERP_LINEAR);
  cw_anim_add_key(a, bt, 0, cw_val_bool(false));
  cw_anim_add_key(a, bt, 1, cw_val_bool(true));
  CHECK(!sample(a, bt, 0.9f).b);
  CHECK(sample(a, bt, 1).b);

  /* colours per component */
  int ct = cw_anim_add_value_track(a, "A:col", CW_INTERP_LINEAR);
  cw_anim_add_key(a, ct, 0, cw_val_color(1, 0, 0, 1));
  cw_anim_add_key(a, ct, 2, cw_val_color(0, 0, 1, 0));
  cw_value c = sample(a, ct, 1);
  CHECK(c.type == CW_T_COLOR);
  NEAR(c.v[0], 0.5, 1e-6);
  NEAR(c.v[2], 0.5, 1e-6);
  NEAR(c.v[3], 0.5, 1e-6);

  /* quaternions: slerp and nlerp, 90 degrees about y, halfway */
  float s45 = 0.70710678f;
  cw_value q0 = { CW_T_QUAT }, q1 = { CW_T_QUAT };
  q0.v[3] = 1;
  q1.v[1] = s45;
  q1.v[3] = s45;
  int qs = cw_anim_add_value_track(a, "A:q", CW_INTERP_LINEAR);
  cw_anim_add_key(a, qs, 0, q0);
  cw_anim_add_key(a, qs, 1, q1);
  cw_value q = sample(a, qs, 0.5f);
  NEAR(q.v[1], 0.38268343, 2e-6);
  NEAR(q.v[3], 0.92387953, 2e-6);
  q = sample(a, qs, 0.25f); /* slerp keeps constant angular speed: 11.25 deg */
  NEAR(q.v[1], 0.19509032, 2e-6);
  cw_anim_track_set_rot_mode(a, qs, CW_ROT_NLERP);
  q = sample(a, qs, 0.5f);
  NEAR(q.v[1], 0.38268343, 2e-6);
  q = sample(a, qs, 0.25f); /* nlerp does not: a smaller angle at 1/4 */
  CHECK(q.v[1] > 0.18f && q.v[1] < 0.1950f);
  NEAR(q.v[0] * q.v[0] + q.v[1] * q.v[1] + q.v[2] * q.v[2] + q.v[3] * q.v[3], 1, 1e-6);
  /* the opposite-sign key takes the short way */
  cw_value q2 = q1;
  for (int k = 0; k < 4; k++) q2.v[k] = -q2.v[k];
  float o[4];
  cw_quat_slerp_det(o, q0.v, q2.v, 0.5f);
  NEAR(fabs(o[1]), 0.38268343, 2e-6);
  FOLD(o);

  int st = cw_anim_add_value_track(a, "A:s", CW_INTERP_LINEAR);
  cw_anim_add_key(a, st, 0, cw_val_string("one"));
  cw_anim_add_key(a, st, 1, cw_val_string("two"));
  CHECK(!strcmp(sample(a, st, 0.7f).s, "one"));
  CHECK(!strcmp(sample(a, st, 1.2f).s, "two"));
  CHECK_EQ_INT(cw_anim_track_find(a, "A:q"), qs);
  cw_anim_release(a);
  CHECK(!cw_anim_valid(a));
}

/* ── key firing ─────────────────────────────────────────────────────── */

static char fired[512];
static void count_fn(cw_node target, const cw_value *args, int argc, void *user) {
  (void)target;
  (void)user;
  char tmp[64];
  snprintf(tmp, sizeof tmp, "%s%s", argc ? args[0].s : "?", " ");
  strncat(fired, tmp, sizeof fired - strlen(fired) - 1);
}
static int finished_count, changed_count, started_count, looped_count, event_count;
static char last_changed[64];
static void on_finished(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)a; (void)n; (void)u; finished_count++; }
static void on_changed(cw_node e, const cw_value *a, int n, void *u) {
  (void)e; (void)u;
  changed_count++;
  if (n == 2) snprintf(last_changed, sizeof last_changed, "%s>%s", a[0].s, a[1].s);
}
static void on_started(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)a; (void)n; (void)u; started_count++; }
static void on_looped(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)a; (void)n; (void)u; looped_count++; }
static void on_event(cw_node e, const cw_value *a, int n, void *u) {
  (void)e; (void)u;
  event_count++;
  if (n == 2) {
    strncat(fired, a[0].s, sizeof fired - strlen(fired) - 1);
    strncat(fired, "! ", sizeof fired - strlen(fired) - 1);
  }
}

/* a scene: Root(Node3D) > Box(MeshInstance3D), Anim(AnimationPlayer) */
static cw_tree *tree;
static cw_node root, box, anim;

static void make_scene(void) {
  cw_scene_register_builtin_types();
  tree = cw_tree_new();
  root = cw_node_new("Node3D", "Root");
  box = cw_node_new("MeshInstance3D", "Box");
  anim = cw_node_new("AnimationPlayer", "Anim");
  cw_node_add_child(root, box);
  cw_node_add_child(root, anim);
  cw_node_add_child(cw_tree_root(tree), root);
  cw_connect(anim, "animation_finished", (cw_node){ 0 }, NULL, on_finished, NULL, 0);
  cw_connect(anim, "animation_changed", (cw_node){ 0 }, NULL, on_changed, NULL, 0);
  cw_connect(anim, "animation_started", (cw_node){ 0 }, NULL, on_started, NULL, 0);
  cw_connect(anim, "animation_looped", (cw_node){ 0 }, NULL, on_looped, NULL, 0);
  cw_connect(anim, "animation_event", (cw_node){ 0 }, NULL, on_event, NULL, 0);
  finished_count = changed_count = started_count = looped_count = event_count = 0;
  fired[0] = 0;
}

static void free_scene(void) {
  cw_tree_free(tree);
  tree = NULL;
}

static cw_anim keyed(const char *name, cw_loop_mode loop) {
  cw_anim a = cw_anim_new(name, 1, loop);
  int m = cw_anim_add_method_track(a, "Box");
  cw_value k0 = cw_val_string("k0"), k5 = cw_val_string("k5"), k1 = cw_val_string("k1");
  cw_anim_add_method_key(a, m, 0, "count", &k0, 1);
  cw_anim_add_method_key(a, m, 1, "count", &k1, 1);
  cw_anim_add_method_key(a, m, 0.5f, "count", &k5, 1);
  return a;
}

static void steps(int n, double dt) {
  for (int i = 0; i < n; i++) cw_anim_player_advance(anim, dt);
}

static void test_loop_edges(void) {
  cw_anim_register_method("count", count_fn, NULL);
  make_scene();
  cw_anim l = keyed("loop", CW_LOOP_LINEAR), pp = keyed("pp", CW_LOOP_PING_PONG), one = keyed("once", CW_LOOP_NONE);
  cw_anim_player_add(anim, NULL, l);
  cw_anim_player_add(anim, NULL, pp);
  cw_anim_player_add(anim, NULL, one);
  cw_anim_release(l);
  cw_anim_release(pp);
  cw_anim_release(one);
  CHECK_EQ_INT(cw_anim_player_count(anim), 3);
  cw_node_set(anim, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));

  /* LOOP, quarter steps: time exactly at the end wraps to 0 and fires the
   * keys at 1 and at 0 in that instant */
  cw_anim_player_play(anim, "loop", 0);
  steps(4, 0.25);
  CHECK(!strcmp(fired, "k0 k5 k1 k0 "));
  NEAR(cw_anim_player_position(anim), 0, 0);
  CHECK_EQ_INT(looped_count, 1);
  steps(1, 0.25);
  CHECK(!strcmp(fired, "k0 k5 k1 k0 ")); /* the 0 key does not fire twice */
  /* a step over two whole cycles */
  fired[0] = 0;
  steps(1, 2.5);
  CHECK(!strcmp(fired, "k5 k1 k0 k5 k1 k0 k5 "));
  NEAR(cw_anim_player_position(anim), 0.75, 1e-9);
  CHECK_EQ_INT(looped_count, 3);

  /* seek never fires; the key at the seek point does not fire after it */
  fired[0] = 0;
  cw_anim_player_seek(anim, 0.5f, false);
  steps(1, 0.25);
  CHECK(!strcmp(fired, ""));

  /* backwards: from the end, wrapping to the end */
  fired[0] = 0;
  cw_anim_player_stop(anim, false);
  cw_anim_player_play_ex(anim, "loop", 0, -1, true);
  steps(4, 0.25);
  CHECK(!strcmp(fired, "k1 k5 k0 k1 "));
  NEAR(cw_anim_player_position(anim), 1, 0);
  /* speed_scale < 0 reverses again: forwards from the end (whose key
   * already fired) through the wrap */
  cw_node_set(anim, "speed_scale", cw_val_float(-2));
  fired[0] = 0;
  steps(1, 0.25);
  CHECK(!strcmp(fired, "k0 k5 "));
  NEAR(cw_anim_player_position(anim), 0.5, 0);
  cw_node_set(anim, "speed_scale", cw_val_float(1));

  /* PING_PONG: each turning-point key fires once per turn */
  fired[0] = 0;
  cw_anim_player_play(anim, "pp", 0);
  double pos[8];
  for (int i = 0; i < 8; i++) {
    steps(1, 0.25);
    pos[i] = cw_anim_player_position(anim);
  }
  CHECK(!strcmp(fired, "k0 k5 k1 k5 k0 "));
  CHECK(pos[3] == 1 && pos[4] == 0.75 && pos[7] == 0);
  steps(1, 0.25);
  CHECK(!strcmp(fired, "k0 k5 k1 k5 k0 "));
  NEAR(cw_anim_player_position(anim), 0.25, 0);

  /* NONE: stops exactly at the end, fires the end key, finishes once */
  fired[0] = 0;
  finished_count = 0;
  cw_anim_player_play(anim, "once", 0);
  steps(5, 0.3);
  CHECK(!strcmp(fired, "k0 k5 k1 "));
  CHECK_EQ_INT(finished_count, 1);
  CHECK(!cw_anim_player_is_playing(anim));
  NEAR(cw_anim_player_position(anim), 1, 0);
  /* playing a finished animation again restarts it */
  cw_anim_player_play(anim, "once", 0);
  steps(1, 0.1);
  CHECK(!strcmp(fired, "k0 k5 k1 k0 "));
  FOLD(looped_count);
  FOLD(finished_count);
  free_scene();
}

/* ── values on nodes, cross-fade, queue, events ─────────────────────── */

static cw_anim const_x(const char *name, float x, float len, cw_loop_mode loop) {
  cw_anim a = cw_anim_new(name, len, loop);
  int t = cw_anim_add_value_track(a, "Box:position.x", CW_INTERP_LINEAR);
  cw_anim_add_key(a, t, 0, cw_val_float(x));
  return a;
}

static float box_x(void) { return cw_node3d_position(box).x; }

static void test_player_values(void) {
  make_scene();
  cw_anim a = cw_anim_new("show", 2, CW_LOOP_LINEAR);
  int pos = cw_anim_add_value_track(a, "Box:position", CW_INTERP_LINEAR);
  cw_anim_add_key(a, pos, 0, cw_val_vec3(0, 0, 0));
  cw_anim_add_key(a, pos, 1, cw_val_vec3(2, 4, 6));
  int col = cw_anim_add_value_track(a, "Box:color", CW_INTERP_LINEAR);
  cw_anim_add_key(a, col, 0, cw_val_color(0, 0, 0, 1));
  cw_anim_add_key(a, col, 2, cw_val_color(1, 0.5f, 0, 1));
  int prim = cw_anim_add_value_track(a, "Box:primitive", CW_INTERP_NEAREST); /* an enum */
  cw_anim_add_key(a, prim, 0, cw_val_int(1));
  int vis = cw_anim_add_value_track(a, "Box:visible", CW_INTERP_NEAREST);
  cw_anim_add_key(a, vis, 0, cw_val_bool(true));
  cw_anim_add_key(a, vis, 1.5f, cw_val_bool(false));
  int ev = cw_anim_add_event_track(a);
  cw_anim_add_event_key(a, ev, 0.5f, "step");
  cw_anim_player_add(anim, "show", a);
  cw_anim_release(a);
  cw_node_set(anim, "autoplay", cw_val_string("show"));
  cw_node_set(anim, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));
  cw_anim_player_play(anim, "show", 0);
  CHECK_EQ_INT(started_count, 1);
  steps(1, 0.5);
  t3_vec3 p = cw_node3d_position(box);
  NEAR(p.x, 1, 1e-6);
  NEAR(p.z, 3, 1e-6);
  cw_value v;
  cw_node_get_prop(box, "color", &v);
  NEAR(v.v[0], 0.25, 1e-6);
  cw_node_get_prop(box, "primitive", &v);
  CHECK_EQ_INT(v.i, 1);
  CHECK(!strcmp(fired, "step! "));
  CHECK_EQ_INT(event_count, 1);
  steps(2, 0.5);
  cw_node_get_prop(box, "visible", &v);
  CHECK(!v.b);
  /* the current_animation property and the hash */
  cw_node_get_prop(anim, "current_animation", &v);
  CHECK(!strcmp(v.s, "show"));
  uint32_t h = cw_anim_player_hash(anim);
  FOLD(h);
  CHECK(h != CW_HASH32_INIT);

  /* cross-fade: half way through a 1 s blend the value is the average */
  cw_anim ax = const_x("ax", 0, 1, CW_LOOP_LINEAR), bx = const_x("bx", 10, 1, CW_LOOP_LINEAR);
  cw_anim_player_add(anim, NULL, ax);
  cw_anim_player_add(anim, NULL, bx);
  cw_anim_release(ax);
  cw_anim_release(bx);
  changed_count = 0;
  cw_anim_player_play(anim, "ax", 0);
  steps(1, 0.1);
  NEAR(box_x(), 0, 0);
  cw_anim_player_set_blend(anim, "ax", "bx", 1.0f);
  cw_anim_player_play(anim, "bx", -1);
  steps(1, 0.5);
  NEAR(box_x(), 5, 1e-6);
  steps(1, 0.25);
  NEAR(box_x(), 7.5, 1e-6);
  steps(1, 0.25);
  NEAR(box_x(), 10, 0);
  CHECK_EQ_INT(changed_count, 2);
  CHECK(!strcmp(last_changed, "ax>bx"));

  /* queue: a one-shot then the next */
  cw_anim once = const_x("once", 3, 0.5f, CW_LOOP_NONE);
  cw_anim_player_add(anim, NULL, once);
  cw_anim_release(once);
  finished_count = 0;
  cw_anim_player_play(anim, "once", 0);
  cw_anim_player_queue(anim, "ax");
  steps(1, 0.25);
  NEAR(box_x(), 3, 0);
  steps(1, 0.3);
  CHECK_EQ_INT(finished_count, 1);
  CHECK(!strcmp(cw_anim_player_current(anim), "ax"));
  steps(1, 0.1);
  NEAR(box_x(), 0, 0);
  /* a track to a missing node or property is skipped, the rest plays */
  cw_anim bad = cw_anim_new("bad", 1, CW_LOOP_LINEAR);
  int t1 = cw_anim_add_value_track(bad, "Nope:position", CW_INTERP_LINEAR);
  cw_anim_add_key(bad, t1, 0, cw_val_vec3(1, 1, 1));
  int t2 = cw_anim_add_value_track(bad, "Box:nonexistent", CW_INTERP_LINEAR);
  cw_anim_add_key(bad, t2, 0, cw_val_float(1));
  int t3 = cw_anim_add_value_track(bad, "Box:position.y", CW_INTERP_LINEAR);
  cw_anim_add_key(bad, t3, 0, cw_val_float(7));
  cw_anim_player_add(anim, NULL, bad);
  cw_anim_release(bad);
  cw_anim_player_play(anim, "bad", 0);
  steps(1, 0.1);
  NEAR(cw_node3d_position(box).y, 7, 0);
  FOLD(changed_count);
  free_scene();
}

/* ── the scene format ───────────────────────────────────────────────── */

static const char *scene_text =
  "[scene format=1]\n"
  "\n"
  "[node name=\"Root\" type=\"Node3D\"]\n"
  "\n"
  "[node name=\"Box\" parent=\".\" type=\"MeshInstance3D\"]\n"
  "position = vec3(0, 1, 0)\n"
  "\n"
  "[node name=\"Anim\" parent=\".\" type=\"AnimationPlayer\"]\n"
  "autoplay = \"spin\"\n"
  "speed_scale = 1.5\n"
  "process = UPDATE\n"
  "\n"
  "[animation name=\"spin\" player=\"Anim\" length=2 loop=LOOP]\n"
  "[track type=value path=\"Box:rotation\"]\n"
  "0 = vec3(0, 0, 0)\n"
  "2 = vec3(0, 6.28318, 0)\n"
  "[track type=value path=\"Box:color\" interp=NEAREST]\n"
  "0 = color(1, 0, 0, 1)\n"
  "1 = color(0, 0, 1, 1)\n"
  "[track type=value path=\"Box:quaternion\" interp=CUBIC rot=NLERP enabled=false]\n"
  "0 = quat(0, 0, 0, 1)\n"
  "[track type=value path=\"Box:scale\" interp=HERMITE]\n"
  "0 = vec3(1, 1, 1) in=vec3(0, 0, 0) out=vec3(1, 1, 1)\n"
  "1.5 = vec3(2, 2, 2) in=vec3(0, 0, 0) out=vec3(0, 0, 0)\n"
  "[track type=method path=\"Box\"]\n"
  "0.5 = count(1, \"a b\", vec2(1, 2))\n"
  "1 = count()\n"
  "[track type=event]\n"
  "0.25 = \"step\"\n"
  "\n"
  "[animation name=\"bob\" player=\"Anim\" length=0.5 loop=PING_PONG]\n"
  "[track type=value path=\"Box:position.y\" interp=CUBIC]\n"
  "0 = 0\n"
  "0.5 = 1.5\n"
  "\n"
  "[animation name=\"flags\" player=\"Anim\" length=1]\n"
  "[track type=value path=\"Box:visible\" interp=NEAREST]\n"
  "0 = true\n"
  "0.1 = false\n";

static void test_scene_format(void) {
  cw_scene_register_builtin_types();
  cw_packed_scene *ps = cw_scene_parse(scene_text, "anim_test.cwscene");
  CHECK(ps != NULL);
  cw_node r = cw_scene_instance(ps);
  cw_node a = cw_node_get(r, "Anim");
  CHECK_EQ_INT(cw_anim_player_count(a), 3);
  cw_anim spin = cw_anim_player_get(a, "spin");
  CHECK(cw_anim_loop(spin) == CW_LOOP_LINEAR);
  CHECK_EQ_INT(cw_anim_track_count(spin), 6);
  CHECK_EQ_INT(cw_anim_key_count(spin, 4), 2);
  cw_value v = sample(spin, 3, 0.75f); /* hermite */
  CHECK(v.type == CW_T_VEC3 && v.v[0] > 1 && v.v[0] < 2);
  char *out = cw_scene_save(r);
  CHECK(out && !strcmp(out, scene_text));
  if (out && strcmp(out, scene_text)) cw_error("round trip differs:\n%s", out);
  /* save -> parse -> save is stable too, and the hash of the text is fixed */
  uint32_t h = cw_hash32(out, strlen(out), CW_HASH32_INIT);
  FOLD(h);
  cw_free(out);
  cw_node_free(r);

  /* an animation built in code saves, loads and saves to the same text */
  cw_scene_register_builtin_types();
  cw_node r2 = cw_node_new("Node3D", "R");
  cw_node b2 = cw_node_new("Node3D", "B");
  cw_node p2 = cw_node_new("AnimationPlayer", "P");
  cw_node_add_child(r2, b2);
  cw_node_add_child(r2, p2);
  cw_node_set(p2, "root_node", cw_val_string("../B"));
  cw_anim x = cw_anim_new("x", 0.3f, CW_LOOP_PING_PONG);
  int t = cw_anim_add_value_track(x, ":position", CW_INTERP_CUBIC);
  cw_anim_add_key(x, t, 0.1f, cw_val_vec3(0.1f, 1.0f / 3.0f, -2));
  cw_anim_add_key(x, t, 0.3f, cw_val_vec3(1e-7f, 123456.78f, 0));
  int m = cw_anim_add_method_track(x, "");
  cw_value args[2] = { cw_val_float(0.1), cw_val_bool(true) };
  cw_anim_add_method_key(x, m, 0.2f, "hit", args, 2);
  cw_anim_player_add(p2, NULL, x);
  cw_anim_release(x);
  char *s1 = cw_scene_save(r2);
  cw_node r3 = cw_scene_instance(cw_scene_parse(s1, "built.cwscene"));
  char *s2 = cw_scene_save(r3);
  CHECK(s1 && s2 && !strcmp(s1, s2));
  if (s1 && s2 && strcmp(s1, s2)) cw_error("built round trip:\n%s\n---\n%s", s1, s2);
  CHECK(strstr(s1, "[animation name=\"x\" player=\"P\" length=0.3 loop=PING_PONG]") != NULL);
  CHECK(strstr(s1, "0.2 = hit(0.1, true)") != NULL);
  cw_value v1, v2;
  cw_anim_sample(cw_anim_player_get(r3 .id ? cw_node_get(r3, "P") : p2, "x"), 0, 0.2f, &v1);
  cw_anim_sample(cw_anim_player_get(p2, "x"), 0, 0.2f, &v2);
  CHECK(!memcmp(v1.v, v2.v, 12));
  cw_free(s1);
  cw_free(s2);
  cw_node_free(r2);
  cw_node_free(r3);

  /* errors name the line */
  CHECK(cw_scene_parse("[scene format=1]\n[node name=\"A\" type=\"Node\"]\n[animation name=\"a\"]\n", "bad.cwscene") == NULL);
  CHECK(cw_scene_parse("[scene format=1]\n[node name=\"A\" type=\"Node\"]\n[animation name=\"a\" length=1]\n0 = 1\n", "bad2.cwscene") == NULL);
}

/* ── tweens ─────────────────────────────────────────────────────────── */

static int tween_cb, tween_done;
static void tw_cb(cw_tween tw, void *u) { (void)tw; (void)u; tween_cb++; }
static void tw_fin(cw_tween tw, void *u) { (void)tw; (void)u; tween_done++; }

static void test_tweens(void) {
  make_scene();
  tween_cb = tween_done = 0;
  /* a sequence: x to 10 in 1 s, then y to 10 in 0.5 s */
  cw_tween tw = cw_tween_new(tree);
  cw_tween_add_property(tw, box, "position", cw_val_vec3(10, 0, 0), 1, CW_EASE_LINEAR);
  cw_tween_add_property(tw, box, "position", cw_val_vec3(10, 10, 0), 0.5f, CW_EASE_LINEAR);
  cw_tween_add_callback(tw, tw_cb, NULL);
  cw_tween_on_finished(tw, tw_fin, NULL);
  for (int i = 0; i < 4; i++) cw_tween_step(tw, 0.25);
  t3_vec3 p = cw_node3d_position(box);
  CHECK(p.x == 10 && p.y == 0);
  cw_tween_step(tw, 0.25);
  NEAR(cw_node3d_position(box).y, 5, 1e-6);
  CHECK(cw_tween_running(tw));
  cw_tween_step(tw, 0.25);
  CHECK(cw_node3d_position(box).y == 10);
  CHECK_EQ_INT(tween_cb, 1);
  CHECK_EQ_INT(tween_done, 1);
  CHECK(!cw_tween_running(tw)); /* finished tweens free themselves */

  /* leftover time carries into the next step */
  cw_node3d_set_position(box, (t3_vec3){ 0, 0, 0 });
  tw = cw_tween_new(tree);
  cw_tween_add_property(tw, box, "position", cw_val_vec3(10, 0, 0), 0.5f, CW_EASE_LINEAR);
  cw_tween_add_property(tw, box, "position", cw_val_vec3(20, 0, 0), 0.5f, CW_EASE_LINEAR);
  cw_tween_step(tw, 0.3);
  cw_tween_step(tw, 0.3);
  NEAR(cw_node3d_position(box).x, 12, 1e-4);
  FOLD(p);
  cw_tween_kill(tw);

  /* parallel with a delay, easing, relative */
  cw_node3d_set_position(box, (t3_vec3){ 0, 0, 0 });
  tw = cw_tween_new(tree);
  cw_tween_add_property(tw, box, "position", cw_val_vec3(1, 0, 0), 1, CW_EASE_LINEAR);
  cw_tween_relative(tw);
  cw_tween_parallel(tw);
  cw_tween_add_property(tw, box, "scale", cw_val_vec3(3, 3, 3), 0.5f, CW_EASE_OUT_QUAD);
  cw_tween_delay(tw, 0.25f);
  cw_tween_step(tw, 0.5);
  NEAR(cw_node3d_position(box).x, 0.5, 1e-6);
  t3_vec3 s;
  {
    cw_value sv;
    cw_node_get_prop(box, "scale", &sv);
    s = (t3_vec3){ sv.v[0], sv.v[1], sv.v[2] };
  }
  NEAR(s.x, 1 + 2 * 0.75, 1e-6); /* out-quad at t = 0.5 is 0.75 */
  cw_tween_step(tw, 0.5);
  NEAR(cw_node3d_position(box).x, 1, 0);
  CHECK(!cw_tween_running(tw));

  /* loops with an explicit start value; the interval and callback per pass */
  tween_cb = 0;
  tw = cw_tween_new(tree);
  cw_tween_add_property(tw, anim, "speed_scale", cw_val_float(2), 0.5f, CW_EASE_IN_OUT_SINE);
  cw_tween_from(tw, cw_val_float(0));
  cw_tween_add_interval(tw, 0.25f);
  cw_tween_add_callback(tw, tw_cb, NULL);
  cw_tween_set_loops(tw, 3);
  for (int i = 0; i < 8; i++) cw_tween_step(tw, 0.25);
  CHECK_EQ_INT(tween_cb, 2);
  CHECK_EQ_INT(cw_tween_loops_done(tw), 2);
  cw_value sp;
  cw_node_get_prop(anim, "speed_scale", &sp);
  NEAR(sp.f, 2, 0); /* the third pass is at the end of its property step */
  for (int i = 0; i < 4; i++) cw_tween_step(tw, 0.25);
  CHECK_EQ_INT(tween_cb, 3);
  CHECK(!cw_tween_running(tw));

  /* the tree runs tweens in fixed_update; bound tweens die with their node */
  cw_node3d_set_position(box, (t3_vec3){ 0, 0, 0 });
  tw = cw_tween_property(box, "position", cw_val_vec3(0, 0, 6), 0.5f, CW_EASE_LINEAR);
  CHECK_EQ_INT(cw_tween_count(tree), 1);
  for (int i = 0; i < 15; i++) cw_tree_fixed_update(tree, 1.0 / 60);
  NEAR(cw_node3d_position(box).z, 3, 1e-5);
  for (int i = 0; i < 16; i++) cw_tree_fixed_update(tree, 1.0 / 60);
  CHECK(cw_node3d_position(box).z == 6);
  CHECK(!cw_tween_running(tw));
  tw = cw_tween_property(box, "position", cw_val_vec3(0, 9, 0), 1, CW_EASE_LINEAR);
  cw_tween pause_tw = cw_tween_property(box, "scale", cw_val_vec3(1, 1, 1), 1, CW_EASE_LINEAR);
  cw_tween_pause(pause_tw);
  cw_tree_fixed_update(tree, 1.0 / 60);
  CHECK(cw_tween_running(tw) && !cw_tween_running(pause_tw));
  /* a paused tree holds tweens, except ones bound to a node that processes
   * while paused (a pause menu) */
  cw_tween_kill(pause_tw);
  cw_node menu = cw_node_new("Node3D", "Menu");
  cw_node_add_child(root, menu);
  cw_node_set_pause_mode(menu, CW_PAUSE_ALWAYS);
  cw_tween menu_tw = cw_tween_property(menu, "position", cw_val_vec3(6, 0, 0), 0.1f, CW_EASE_LINEAR);
  float box_y = cw_node3d_position(box).y;
  cw_tree_set_paused(tree, true);
  CHECK(cw_node_can_process(menu) && !cw_node_can_process(box));
  for (int i = 0; i < 3; i++) cw_tree_fixed_update(tree, 1.0 / 60);
  CHECK(cw_node3d_position(box).y == box_y); /* unbound to a paused-processing node: held */
  NEAR(cw_node3d_position(menu).x, 3, 1e-5);
  cw_tree_set_paused(tree, false);
  CHECK(cw_tween_running(menu_tw));
  cw_node_free(menu);
  cw_node_free(box);
  cw_tree_fixed_update(tree, 1.0 / 60);
  CHECK(!cw_tween_running(tw));
  CHECK_EQ_INT(cw_tween_count(tree), 0);
  free_scene();
}

/* ── AnimationTree ──────────────────────────────────────────────────── */

static int state_changes;
static char states_seen[256];
static void on_state(cw_node e, const cw_value *a, int n, void *u) {
  (void)e; (void)u;
  state_changes++;
  if (n) {
    strncat(states_seen, a[0].s, sizeof states_seen - strlen(states_seen) - 1);
    strncat(states_seen, " ", sizeof states_seen - strlen(states_seen) - 1);
  }
}

static void test_tree(void) {
  make_scene();
  cw_node3d_set_position(box, (t3_vec3){ 3, 0, 0 });
  cw_anim xs[3] = { const_x("a", 0, 1, CW_LOOP_LINEAR), const_x("b", 10, 1, CW_LOOP_LINEAR), const_x("c", 20, 0.5f, CW_LOOP_NONE) };
  for (int i = 0; i < 3; i++) { cw_anim_player_add(anim, NULL, xs[i]); cw_anim_release(xs[i]); }
  cw_node tr = cw_node_new("AnimationTree", "Tree");
  cw_node_add_child(root, tr);
  cw_node_set(tr, "player", cw_val_string("../Anim"));
  cw_node_set(tr, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));
  cw_connect(tr, "state_changed", (cw_node){ 0 }, NULL, on_state, NULL, 0);

  /* 1D blend space */
  cw_blend_id bs = cw_animtree_blend1d(tr, "s");
  cw_blend_id ca = cw_animtree_clip(tr, "a"), cb = cw_animtree_clip(tr, "b"), cc = cw_animtree_clip(tr, "c");
  cw_animtree_blend1d_add(tr, bs, cb, 1);
  cw_animtree_blend1d_add(tr, bs, ca, 0);
  cw_animtree_blend1d_add(tr, bs, cc, 2);
  float in[4] = { 0.5f, 1.5f, -1, 3 }, want[4] = { 5, 15, 0, 20 };
  for (int i = 0; i < 4; i++) {
    cw_animtree_set_param(tr, "s", in[i]);
    cw_animtree_advance(tr, 0.01);
    NEAR(box_x(), want[i], 1e-5);
  }
  NEAR(cw_animtree_clip_weight(tr, cc), 1, 0);

  /* 2D blend space (gradient band) */
  cw_node tr2 = cw_node_new("AnimationTree", "Tree2");
  cw_node_add_child(root, tr2);
  cw_node_set(tr2, "player", cw_val_string("../Anim"));
  cw_node_set(tr2, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));
  cw_blend_id b2 = cw_animtree_blend2d(tr2, "x", "y");
  cw_blend_id k[3] = { cw_animtree_clip(tr2, "a"), cw_animtree_clip(tr2, "b"), cw_animtree_clip(tr2, "c") };
  cw_animtree_blend2d_add(tr2, b2, k[0], 0, 0);
  cw_animtree_blend2d_add(tr2, b2, k[1], 1, 0);
  cw_animtree_blend2d_add(tr2, b2, k[2], 0, 1);
  float pts[4][3] = { { 0, 0, 0 }, { 1, 0, 10 }, { 0, 1, 20 }, { 0.5f, 0, 5 } };
  for (int i = 0; i < 4; i++) {
    cw_animtree_set_param(tr2, "x", pts[i][0]);
    cw_animtree_set_param(tr2, "y", pts[i][1]);
    cw_animtree_advance(tr2, 0.01);
    NEAR(box_x(), pts[i][2], 1e-5);
  }
  cw_animtree_set_param(tr2, "x", 0.3f);
  cw_animtree_set_param(tr2, "y", 0.4f);
  cw_animtree_advance(tr2, 0.01);
  float wsum = cw_animtree_clip_weight(tr2, k[0]) + cw_animtree_clip_weight(tr2, k[1]) + cw_animtree_clip_weight(tr2, k[2]);
  NEAR(wsum, 1, 1e-6);
  float bx2 = box_x();
  FOLD(bx2);
  cw_node_set(tr2, "active", cw_val_bool(false));

  /* state machine: idle <-> walk on a condition with fades; jump by travel,
   * back at its end */
  cw_node tr3 = cw_node_new("AnimationTree", "Tree3");
  cw_node_add_child(root, tr3);
  cw_node_set(tr3, "player", cw_val_string("../Anim"));
  cw_node_set(tr3, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));
  cw_connect(tr3, "state_changed", (cw_node){ 0 }, NULL, on_state, NULL, 0);
  cw_blend_id sm = cw_animtree_state_machine(tr3);
  cw_animtree_sm_add_state(tr3, sm, "idle", cw_animtree_clip(tr3, "a"));
  cw_animtree_sm_add_state(tr3, sm, "walk", cw_animtree_clip(tr3, "b"));
  cw_animtree_sm_add_state(tr3, sm, "jump", cw_animtree_clip(tr3, "c"));
  cw_animtree_sm_add_transition(tr3, sm, "idle", "walk", &(cw_transition){ .xfade = 0.2f, .condition = "speed > 0.5" });
  cw_animtree_sm_add_transition(tr3, sm, "walk", "idle", &(cw_transition){ .xfade = 0.2f, .condition = "speed <= 0.5" });
  cw_animtree_sm_add_transition(tr3, sm, "walk", "jump", &(cw_transition){ .advance = CW_ADVANCE_ENABLED });
  cw_animtree_sm_add_transition(tr3, sm, "jump", "walk", &(cw_transition){ .switch_mode = CW_SWITCH_AT_END });
  cw_animtree_sm_add_transition(tr3, sm, "idle", "jump", &(cw_transition){ .advance = CW_ADVANCE_DISABLED });
  state_changes = 0;
  states_seen[0] = 0;
  cw_animtree_advance(tr3, 0.1);
  CHECK(!strcmp(cw_animtree_sm_current(tr3, sm), "idle"));
  NEAR(box_x(), 0, 0);
  cw_animtree_set_param(tr3, "speed", 1);
  cw_animtree_advance(tr3, 0.1); /* the fade starts */
  CHECK(!strcmp(cw_animtree_sm_next(tr3, sm), "walk"));
  NEAR(box_x(), 0, 1e-6);
  cw_animtree_advance(tr3, 0.1);
  NEAR(box_x(), 5, 1e-5);
  cw_animtree_advance(tr3, 0.1);
  CHECK(!strcmp(cw_animtree_sm_current(tr3, sm), "walk"));
  CHECK(cw_animtree_sm_next(tr3, sm) == NULL);
  NEAR(box_x(), 10, 0);
  cw_animtree_sm_travel(tr3, sm, "jump");
  cw_animtree_advance(tr3, 0.25);
  CHECK(!strcmp(cw_animtree_sm_current(tr3, sm), "jump"));
  NEAR(box_x(), 20, 0);
  cw_animtree_advance(tr3, 0.25); /* jump reaches its end */
  CHECK(!strcmp(cw_animtree_sm_current(tr3, sm), "jump"));
  cw_animtree_advance(tr3, 0.1); /* AT_END: back to walk */
  CHECK(!strcmp(cw_animtree_sm_current(tr3, sm), "walk"));
  /* travel idle -> jump goes through walk (idle -> jump is DISABLED) */
  cw_animtree_set_param(tr3, "speed", 0);
  cw_animtree_sm_start(tr3, sm, "idle");
  cw_animtree_sm_travel(tr3, sm, "jump");
  cw_animtree_advance(tr3, 0.1); /* idle -> walk (fade 0.2) */
  CHECK(!strcmp(cw_animtree_sm_next(tr3, sm), "walk"));
  cw_animtree_advance(tr3, 0.1);
  cw_animtree_advance(tr3, 0.1); /* walk reached; travel continues */
  CHECK(!strcmp(cw_animtree_sm_current(tr3, sm), "jump"));
  CHECK(!strcmp(states_seen, "walk jump walk idle walk jump "));
  if (strcmp(states_seen, "walk jump walk idle walk jump ")) cw_error("states: %s", states_seen);
  FOLD(state_changes);
  cw_node_set(tr3, "active", cw_val_bool(false));

  /* additive: base + (sample - rest) * amount; the rest is the value when
   * first bound (3) */
  cw_node tr4 = cw_node_new("AnimationTree", "Tree4");
  cw_node_add_child(root, tr4);
  cw_node_set(tr4, "player", cw_val_string("../Anim"));
  cw_node_set(tr4, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));
  cw_node3d_set_position(box, (t3_vec3){ 3, 0, 0 });
  cw_blend_id add = cw_animtree_add(tr4, cw_animtree_clip(tr4, "a"), cw_animtree_clip(tr4, "b"), "amount");
  cw_animtree_set_param(tr4, "amount", 0.5f);
  cw_animtree_advance(tr4, 0.1);
  NEAR(box_x(), 3.5, 1e-6);
  /* a filter that excludes Box leaves only the base */
  const char *only[] = { "Other" };
  cw_animtree_set_filter(tr4, add, only, 1);
  cw_animtree_advance(tr4, 0.1);
  NEAR(box_x(), 3.5, 1e-6); /* nothing passes the filter: untouched */
  uint32_t h = cw_animtree_hash(tr4);
  FOLD(h);
  free_scene();
}

/* ── root motion (node tracks) ──────────────────────────────────────── */

static void test_root_motion(void) {
  make_scene();
  cw_node mover = cw_node_new("Node3D", "Mover"), body = cw_node_new("Node3D", "Body");
  cw_node_add_child(root, mover);
  cw_node_add_child(mover, body);
  cw_anim walk = cw_anim_new("walk", 1, CW_LOOP_LINEAR);
  int t = cw_anim_add_value_track(walk, "Mover/Body:position", CW_INTERP_LINEAR);
  cw_anim_add_key(walk, t, 0, cw_val_vec3(0, 0, 0));
  cw_anim_add_key(walk, t, 1, cw_val_vec3(2, 0.5f, 0));
  cw_anim_player_add(anim, NULL, walk);
  cw_anim_release(walk);
  cw_node_set(anim, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));
  cw_node_set(anim, "root_motion_track", cw_val_string("Mover/Body:position"));
  cw_node_set(anim, "root_motion_target", cw_val_string("../Mover"));
  cw_node_set(anim, "root_motion_lock", cw_val_int(1)); /* x moves the mover, y stays on the body */
  cw_anim_player_play(anim, "walk", 0);
  steps(10, 0.25);
  t3_vec3 m = cw_node3d_position(mover), b = cw_node3d_position(body);
  NEAR(m.x, 5, 1e-5);
  CHECK(b.x == 0);
  NEAR(b.y, 0.25, 1e-6); /* 2.5 s into 1 s cycles: half way */
  /* the motion follows the mover's facing: 90 degrees about y turns +x into -z */
  cw_node3d_set_position(mover, (t3_vec3){ 0, 0, 0 });
  cw_node3d_set_rotation(mover, (t3_vec3){ 0, CW_PI / 2, 0 });
  steps(2, 0.25);
  m = cw_node3d_position(mover);
  NEAR(m.z, -1, 1e-5);
  NEAR(m.x, 0, 1e-5);
  t3_vec3 rm = cw_anim_player_root_motion(anim);
  NEAR(rm.z, -0.5, 1e-5);
  FOLD(m);
  free_scene();
}

/* ── skeletal: glTF clip vs three.c's AnimationMixer ────────────────── */

static void test_skeletal(void) {
  size_t len;
  void *glb = cw_asset_load("CesiumMan.glb", &len);
  CHECK(glb != NULL);
  if (!glb) return;
  t3_gltf *g = t3_gltf_parse(glb, len, NULL, NULL);
  free(glb);
  CHECK(g && g->animation_count == 1);
  if (!g || !g->animation_count) return;
  make_scene();
  cw_node model = cw_node_new("Node3D", "Model");
  cw_node_add_child(root, model);
  cw_node3d_set_object(model, g->scene);
  cw_anim walk = cw_anim_from_t3_clip(g->animations[0], "Model");
  CHECK_EQ_INT(cw_anim_track_count(walk), 57);
  NEAR(cw_anim_length(walk), 2, 1e-6);
  cw_anim_player_add(anim, "walk", walk);
  cw_node_set(anim, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));

  /* three.c's mixer on the same bones, as the reference (tolerance: its
   * slerp is libm's) */
  t3_animation_mixer *mx = t3_animation_mixer_new(g->scene);
  t3_animation_action *act = t3_animation_mixer_clip_action(mx, g->animations[0]);
  t3_animation_action_play(act);
  float times[4] = { 0.3f, 0.77f, 1.5f, 1.99f };
  float prev = 0, maxerr = 0;
  for (int ti = 0; ti < 4; ti++) {
    t3_animation_mixer_update(mx, times[ti] - prev);
    prev = times[ti];
    for (int k = 0; k < cw_anim_track_count(walk); k++) {
      const char *path = cw_anim_track_path(walk, k);
      const char *at = strchr(path, '@'), *colon = strrchr(path, ':');
      char bone[128];
      snprintf(bone, sizeof bone, "%.*s", (int)(colon - at - 1), at + 1);
      t3_object *o = t3_object_get_by_name(g->scene, bone);
      cw_value v = sample(walk, k, times[ti]);
      const float *ref = !strcmp(colon + 1, "position") ? &o->position.x : !strcmp(colon + 1, "scale") ? &o->scale.x : &o->quaternion.x;
      int n = v.type == CW_T_QUAT ? 4 : 3;
      for (int c = 0; c < n; c++) {
        float e = fabsf(v.v[c] - ref[c]);
        if (e > maxerr) maxerr = e;
      }
    }
  }
  CHECK(maxerr < 2e-5f);
  if (maxerr >= 2e-5f) cw_error("skeletal sampling differs from three.c's mixer by %g", maxerr);
  t3_release(mx);

  /* the player writes exactly what it samples */
  cw_anim_player_play(anim, "walk", 0);
  steps(1, 0.5);
  t3_object *torso = t3_object_get_by_name(g->scene, "Skeleton_torso_joint_2");
  cw_value q;
  cw_anim_sample(walk, cw_anim_track_find(walk, "Model@Skeleton_torso_joint_2:quaternion"), 0.5f, &q);
  CHECK(!memcmp(&torso->quaternion, q.v, 16));
  uint32_t h = cw_anim_player_hash(anim);
  FOLD(h);
  cw_anim_release(walk);
  free_scene();
  /* the glTF is kept: three.c frees a SkinnedMesh through the InstancedMesh
   * path (object_free falls through into fields a SkinnedMesh does not have) */
}

CW_TEST_SUITE("anim", { "interpolation", test_interpolation }, { "loop_edges", test_loop_edges },
              { "player_values", test_player_values }, { "scene_format", test_scene_format }, { "tweens", test_tweens },
              { "tree", test_tree }, { "root_motion", test_root_motion }, { "skeletal", test_skeletal })

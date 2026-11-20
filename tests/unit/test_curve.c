/* Curves and paths (cartwheel/curve.h): Bezier points, the baked polyline, sampling by distance, the nearest
 * point, carried frames (a loop, a closed circle's seam, tilt), the text form, Path2D / Path3D and their
 * followers (progress, ratio, loop, speed, end_reached, rotation modes, scene text). Every baked curve folds
 * into the checksum: the gate holds each host to the native bits. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/curve.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "test.h"

#define DT (1.0 / 60)
#define V3(x, y, z) ((t3_vec3){ (x), (y), (z) })
static const t3_vec3 Z3 = { 0, 0, 0 };

static float dot3(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float dist3(t3_vec3 a, t3_vec3 b) {
  t3_vec3 d = { a.x - b.x, a.y - b.y, a.z - b.z };
  return sqrtf(dot3(d, d));
}
static bool near3(t3_vec3 a, t3_vec3 b, float tol) { return dist3(a, b) <= tol; }
static void fold_curve(cw_curve *c) {
  uint32_t h = cw_curve_hash(c);
  FOLD(h);
}

/* a circle of radius r in the XZ plane from four Bezier quarters, counter-clockwise seen from above, starting
 * at (r, 0, 0) and heading toward -Z */
static void circle_xz(cw_curve *c, float r) {
  float k = 0.5522847498f * r;
  cw_curve_add_point(c, V3(r, 0, 0), V3(0, 0, k), V3(0, 0, -k), -1);
  cw_curve_add_point(c, V3(0, 0, -r), V3(k, 0, 0), V3(-k, 0, 0), -1);
  cw_curve_add_point(c, V3(-r, 0, 0), V3(0, 0, -k), V3(0, 0, k), -1);
  cw_curve_add_point(c, V3(0, 0, r), V3(-k, 0, 0), V3(k, 0, 0), -1);
  cw_curve_set_closed(c, true);
}

static void t_line(void) {
  cw_curve *c = cw_curve_new();
  CHECK_EQ_INT(cw_curve_point_count(c), 0);
  CHECK(cw_curve_length(c) == 0);
  CHECK(near3(cw_curve_sample_baked(c, 3), Z3, 0));
  cw_curve_add_point(c, V3(1, 2, 3), Z3, Z3, -1);
  CHECK(cw_curve_length(c) == 0);
  CHECK(near3(cw_curve_sample_baked(c, 3), V3(1, 2, 3), 0)); /* one point: the point */
  CHECK(near3(cw_curve_closest_point(c, V3(9, 9, 9)), V3(1, 2, 3), 0));
  cw_curve_clear(c);
  /* an L: 4 along +X, then 3 along +Y */
  cw_curve_add_point(c, V3(0, 0, 0), Z3, Z3, -1);
  cw_curve_add_point(c, V3(4, 0, 0), Z3, Z3, -1);
  cw_curve_add_point(c, V3(4, 3, 0), Z3, Z3, -1);
  CHECK(cw_curve_length(c) == 7);
  CHECK(near3(cw_curve_sample_baked(c, 2), V3(2, 0, 0), 1e-6f));
  CHECK(near3(cw_curve_sample_baked(c, 5.5f), V3(4, 1.5f, 0), 1e-6f));
  CHECK(near3(cw_curve_sample_baked(c, -1), V3(0, 0, 0), 0)); /* clamped */
  CHECK(near3(cw_curve_sample_baked(c, 99), V3(4, 3, 0), 0));
  CHECK(near3(cw_curve_sample(c, 1, 0.5f), V3(4, 1.5f, 0), 1e-6f)); /* the Bezier itself */
  CHECK(fabsf(cw_curve_closest_offset(c, V3(1, 5, 0)) - 7) < 1e-5f); /* the far end is the nearest */
  CHECK(fabsf(cw_curve_closest_offset(c, V3(3, -2, 0)) - 3) < 1e-5f);
  CHECK(fabsf(cw_curve_closest_offset(c, V3(6, 2, 7)) - 6) < 1e-5f);
  CHECK(near3(cw_curve_closest_point(c, V3(6, 2, 7)), V3(4, 2, 0), 1e-5f));
  /* straight legs bake to their end points only (the corner twice: arriving and leaving): lengths are exact */
  const t3_vec3 *pos;
  const float *off;
  int n = cw_curve_baked(c, &pos, &off);
  CHECK_EQ_INT(n, 4);
  CHECK(n == 4 && off[0] == 0 && off[1] == 4 && off[2] == 4 && off[3] == 7 && pos[1].x == 4 && pos[2].x == 4);
  /* either side of the corner has its own leg's direction */
  CHECK(near3(cw_curve_sample_frame(c, 3.99f, false).forward, V3(1, 0, 0), 1e-6f));
  CHECK(near3(cw_curve_sample_frame(c, 4.01f, false).forward, V3(0, 1, 0), 1e-6f));
  fold_curve(c);
  /* insert, remove, set */
  cw_curve_add_point(c, V3(2, 1, 0), Z3, Z3, 1);
  CHECK_EQ_INT(cw_curve_point_count(c), 4);
  CHECK(cw_curve_get_point(c, 1).position.y == 1);
  CHECK(cw_curve_length(c) > 7);
  uint32_t v = cw_curve_version(c);
  cw_curve_remove_point(c, 1);
  CHECK(cw_curve_version(c) != v);
  CHECK(cw_curve_length(c) == 7);
  cw_curve_point p = cw_curve_get_point(c, 2);
  p.position.y = 6;
  cw_curve_set_point(c, 2, p);
  CHECK(cw_curve_length(c) == 10);
  cw_curve_remove_point(c, 9); /* ignored */
  CHECK(cw_curve_get_point(c, 9).position.x == 0);
  /* closed adds the way back: 4 + 6 + sqrt(52) */
  cw_curve_set_closed(c, true);
  CHECK(fabsf(cw_curve_length(c) - (10 + sqrtf(52))) < 1e-4f);
  fold_curve(c);
  cw_curve_free(c);
  cw_curve_free(NULL);
}

static void t_circle(void) {
  cw_curve *c = cw_curve_new();
  circle_xz(c, 5);
  float len = cw_curve_length(c);
  /* the four-Bezier circle is within 0.03% of a true one */
  CHECK(fabsf(len - 2 * 3.14159265f * 5) < 0.02f);
  /* every sample sits on the circle, and walking by distance turns at an even rate */
  float worst_r = 0, worst_step = 0, prev_ang = 0;
  for (int i = 0; i <= 64; i++) {
    t3_vec3 p = cw_curve_sample_baked(c, len * (float)i / 64);
    float r = sqrtf(p.x * p.x + p.z * p.z);
    if (fabsf(r - 5) > worst_r) worst_r = fabsf(r - 5);
    float ang = cw_atan2(-p.z, p.x);
    if (ang < -1e-4f) ang += 2 * 3.14159265f;
    if (i == 64) ang = 2 * 3.14159265f;
    if (i) {
      float step = fabsf((ang - prev_ang) - 2 * 3.14159265f / 64);
      if (step > worst_step) worst_step = step;
    }
    prev_ang = ang;
    FOLD(p);
  }
  CHECK(worst_r < 0.01f);
  CHECK(worst_step < 0.005f);
  /* the nearest point to something outside is the radial one */
  float off = cw_curve_closest_offset(c, V3(0, 3, -20));
  CHECK(fabsf(off - len / 4) < 0.05f);
  CHECK(near3(cw_curve_closest_point(c, V3(0, 3, -20)), V3(0, 0, -5), 0.02f));
  /* the bake interval is what sets the accuracy: a coarse one cuts the corners (the control for the checks above) */
  cw_curve_set_bake_interval(c, 100);
  float coarse = cw_curve_length(c);
  CHECK(coarse < len - 0.02f);
  cw_curve_set_bake_interval(c, 0.2f);
  CHECK(cw_curve_length(c) == len);
  fold_curve(c);
  cw_curve_free(c);
}

static void t_frames(void) {
  /* flat circle: forward is the tangent, up stays the world's, the seam closes */
  cw_curve *c = cw_curve_new();
  circle_xz(c, 5);
  float len = cw_curve_length(c);
  float worst = 0;
  for (int i = 0; i <= 100; i++) {
    cw_curve_frame f = cw_curve_sample_frame(c, len * (float)i / 100, true);
    float e = fabsf(dot3(f.forward, f.up)) + fabsf(dot3(f.forward, f.right)) + fabsf(dot3(f.up, f.right)) +
              fabsf(dot3(f.forward, f.forward) - 1) + fabsf(dot3(f.up, f.up) - 1) + fabsf(dot3(f.right, f.right) - 1);
    if (e > worst) worst = e;
    CHECK(f.up.y > 0.999f);
    FOLD(f);
  }
  CHECK(worst < 1e-4f);
  cw_curve_frame f0 = cw_curve_sample_frame(c, 0, true);
  CHECK(near3(f0.forward, V3(0, 0, -1), 1e-5f) && near3(f0.right, V3(1, 0, 0), 1e-5f));
  cw_curve_frame fq = cw_curve_sample_frame(c, len / 4, true);
  CHECK(near3(fq.forward, V3(-1, 0, 0), 0.01f));
  cw_curve_free(c);

  /* a vertical loop in the YZ plane (heading -Z at the bottom, over the top, back to the start): the carried
   * frame's up points at the loop's centre all the way round, upside down at the top. A look-at frame built
   * from the world's up cannot do that. */
  c = cw_curve_new();
  float r = 4, k = 0.5522847498f * r;
  cw_curve_add_point(c, V3(0, 0, 0), V3(0, 0, k), V3(0, 0, -k), -1);
  cw_curve_add_point(c, V3(0, r, -r), V3(0, -k, 0), V3(0, k, 0), -1);
  cw_curve_add_point(c, V3(0, 2 * r, 0), V3(0, 0, -k), V3(0, 0, k), -1);
  cw_curve_add_point(c, V3(0, r, r), V3(0, k, 0), V3(0, -k, 0), -1);
  cw_curve_set_closed(c, true);
  len = cw_curve_length(c);
  float worst_centre = 0;
  for (int i = 0; i <= 100; i++) {
    cw_curve_frame f = cw_curve_sample_frame(c, len * (float)i / 100, true);
    t3_vec3 to_centre = { 0 - f.position.x, r - f.position.y, 0 - f.position.z };
    float l = sqrtf(dot3(to_centre, to_centre));
    float d = dot3(f.up, to_centre) / l;
    if (1 - d > worst_centre) worst_centre = 1 - d;
    FOLD(f);
  }
  CHECK(worst_centre < 0.002f);
  cw_curve_frame top = cw_curve_sample_frame(c, len / 2, true);
  CHECK(top.up.y < -0.999f);             /* upside down at the top */
  CHECK(near3(top.forward, V3(0, 0, 1), 0.01f));
  cw_curve_frame end = cw_curve_sample_frame(c, len, true), start = cw_curve_sample_frame(c, 0, true);
  CHECK(near3(end.up, start.up, 1e-3f)); /* the seam */
  fold_curve(c);
  cw_curve_free(c);

  /* a corkscrew: a helix-like climb that turns out of every plane. The frames come back rolled; a closed curve
   * spreads that roll so the seam still meets. */
  c = cw_curve_new();
  cw_curve_add_point(c, V3(3, 0, 0), V3(0, -1, 2), V3(0, 1, -2), -1);
  cw_curve_add_point(c, V3(0, 2, -3), V3(2, -1, 0), V3(-2, 1, 0), -1);
  cw_curve_add_point(c, V3(-3, 4, 0), V3(0, 1, -2), V3(0, -1, 2), -1);
  cw_curve_add_point(c, V3(0, 2, 3), V3(-2, 1, 0), V3(2, -1, 0), -1);
  cw_curve_set_closed(c, true);
  len = cw_curve_length(c);
  end = cw_curve_sample_frame(c, len, true);
  start = cw_curve_sample_frame(c, 0, true);
  CHECK(near3(end.up, start.up, 1e-3f));
  /* neighbours never flip: the frame turns smoothly */
  float least = 1;
  cw_curve_frame prev = start;
  for (int i = 1; i <= 400; i++) {
    cw_curve_frame f = cw_curve_sample_frame(c, len * (float)i / 400, true);
    float d = dot3(f.up, prev.up);
    if (d < least) least = d;
    prev = f;
  }
  CHECK(least > 0.99f);
  fold_curve(c);
  cw_curve_free(c);

  /* a closed curve that leaves every plane (a smooth loop through four corners of a cube): carrying a frame
   * round it brings it back rolled. Closed, the roll is spread along the length and the seam meets; the same
   * points as an open curve that ends where it began are not corrected and do not meet (the control). */
  static const t3_vec3 Q[4] = { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 4, 4, 4 } };
  c = cw_curve_new();
  for (int i = 0; i < 4; i++) {
    t3_vec3 a = Q[(i + 3) % 4], b = Q[(i + 1) % 4];
    t3_vec3 h = { (b.x - a.x) / 6, (b.y - a.y) / 6, (b.z - a.z) / 6 };
    cw_curve_add_point(c, Q[i], V3(-h.x, -h.y, -h.z), h, -1);
  }
  cw_curve_set_closed(c, true);
  len = cw_curve_length(c);
  end = cw_curve_sample_frame(c, len, true);
  start = cw_curve_sample_frame(c, 0, true);
  CHECK(near3(end.up, start.up, 1e-3f));
  fold_curve(c);
  cw_curve_set_closed(c, false);
  cw_curve_add_point(c, Q[0], cw_curve_get_point(c, 0).in, cw_curve_get_point(c, 0).out, -1);
  CHECK(fabsf(cw_curve_length(c) - len) < 1e-3f);
  end = cw_curve_sample_frame(c, cw_curve_length(c), true);
  start = cw_curve_sample_frame(c, 0, true);
  CHECK(near3(end.forward, start.forward, 1e-3f) && !near3(end.up, start.up, 0.05f));
  cw_curve_free(c);

  /* tilt: a straight run along -Z with a quarter turn of tilt at its end rolls up toward right */
  c = cw_curve_new();
  cw_curve_add_point(c, V3(0, 0, 0), Z3, Z3, -1);
  cw_curve_add_point(c, V3(0, 0, -10), Z3, Z3, -1);
  cw_curve_point p = cw_curve_get_point(c, 1);
  p.tilt = 1.5707963f;
  cw_curve_set_point(c, 1, p);
  cw_curve_frame a = cw_curve_sample_frame(c, 0, true), m = cw_curve_sample_frame(c, 5, true), b = cw_curve_sample_frame(c, 10, true);
  CHECK(near3(a.up, V3(0, 1, 0), 1e-5f) && near3(a.right, V3(1, 0, 0), 1e-5f));
  CHECK(fabsf(m.tilt - 0.78539816f) < 1e-5f && fabsf(m.up.x - m.up.y) < 1e-4f && m.up.x > 0.7f);
  CHECK(near3(b.up, V3(1, 0, 0), 1e-4f));
  cw_curve_frame flat = cw_curve_sample_frame(c, 10, false);
  CHECK(near3(flat.up, V3(0, 1, 0), 1e-5f) && fabsf(flat.tilt - 1.5707963f) < 1e-6f);
  FOLD(m);
  /* a path that starts straight up: the reference up is replaced by something perpendicular */
  cw_curve_clear(c);
  cw_curve_add_point(c, V3(0, 0, 0), Z3, Z3, -1);
  cw_curve_add_point(c, V3(0, 5, 0), Z3, Z3, -1);
  a = cw_curve_sample_frame(c, 1, true);
  CHECK(near3(a.forward, V3(0, 1, 0), 1e-6f) && fabsf(dot3(a.up, a.forward)) < 1e-6f && fabsf(dot3(a.up, a.up) - 1) < 1e-5f);
  cw_curve_set_up(c, V3(1, 0, 0));
  a = cw_curve_sample_frame(c, 1, true);
  CHECK(near3(a.up, V3(1, 0, 0), 1e-6f));
  cw_curve_free(c);
}

static void t_text(void) {
  cw_curve *c = cw_curve_new();
  char buf[256];
  CHECK(cw_curve_parse(c, "0 0 0, 4 0 0  -1 0 0  1 0 0, 4 0 -4  0 0 1  0 0 -1  0.5", 3));
  CHECK_EQ_INT(cw_curve_point_count(c), 3);
  CHECK(cw_curve_get_point(c, 1).in.x == -1 && cw_curve_get_point(c, 1).out.x == 1);
  CHECK(cw_curve_get_point(c, 2).tilt == 0.5f && cw_curve_get_point(c, 2).out.z == -1);
  int n = cw_curve_format(c, buf, sizeof buf, 3);
  CHECK(!strcmp(buf, "0 0 0, 4 0 0  -1 0 0  1 0 0, 4 0 -4  0 0 1  0 0 -1  0.5"));
  CHECK_EQ_INT(n, (int)strlen(buf));
  CHECK_EQ_INT(cw_curve_format(c, NULL, 0, 3), n); /* measuring */
  char small[8];
  CHECK_EQ_INT(cw_curve_format(c, small, sizeof small, 3), n);
  CHECK(strlen(small) < sizeof small);
  /* what was written reads back to the same curve, awkward numbers included */
  cw_curve_clear(c);
  cw_curve_add_point(c, V3(0.1f, -2.5e-7f, 1234567.9f), V3(1.0f / 3, 0, 0), V3(0, 0, 1e9f), -1);
  cw_curve_add_point(c, V3(3, 4, 5), Z3, Z3, -1);
  cw_curve_format(c, buf, sizeof buf, 3);
  cw_curve *d = cw_curve_new();
  CHECK(cw_curve_parse(d, buf, 3));
  cw_curve_point a = cw_curve_get_point(c, 0), b = cw_curve_get_point(d, 0);
  CHECK(!memcmp(&a, &b, sizeof a));
  CHECK(cw_curve_hash(c) == cw_curve_hash(d));
  /* 2D */
  CHECK(cw_curve_parse(d, "0 0, 100 0  -20 0  20 0,\n 100 100", 2));
  CHECK_EQ_INT(cw_curve_point_count(d), 3);
  cw_curve_format(d, buf, sizeof buf, 2);
  CHECK(!strcmp(buf, "0 0, 100 0  -20 0  20 0, 100 100"));
  if (strcmp(buf, "0 0, 100 0  -20 0  20 0, 100 100")) cw_info("2d text: [%s]", buf);
  /* empty text is an empty curve; bad text is refused and leaves no points */
  CHECK(cw_curve_parse(d, "", 3) && cw_curve_point_count(d) == 0);
  CHECK(cw_curve_parse(d, NULL, 3) && cw_curve_point_count(d) == 0);
  cw_info("the two curve errors below are expected");
  CHECK(!cw_curve_parse(d, "0 0 0, 1 2", 3) && cw_curve_point_count(d) == 0);
  CHECK(!cw_curve_parse(d, "0 0 0, a b c", 3) && cw_curve_point_count(d) == 0);
  cw_curve_free(c);
  cw_curve_free(d);
}

static void t_2d(void) {
  cw_curve *c = cw_curve_new();
  cw_curve_set_bake_interval(c, 5);
  cw_curve_add_point2d(c, cw_v2(0, 0), cw_v2(0, 0), cw_v2(0, 0), -1);
  cw_curve_add_point2d(c, cw_v2(300, 0), cw_v2(0, 0), cw_v2(0, 0), -1);
  cw_curve_add_point2d(c, cw_v2(300, 400), cw_v2(0, 0), cw_v2(0, 0), -1);
  CHECK(cw_curve_length(c) == 700);
  cw_vec2 p = cw_curve_sample_baked2d(c, 500), dir;
  CHECK(p.x == 300 && p.y == 200);
  cw_curve_sample_dir2d(c, 100, &p, &dir);
  CHECK(p.x == 100 && dir.x == 1 && dir.y == 0);
  cw_curve_sample_dir2d(c, 500, &p, &dir);
  CHECK(dir.x == 0 && dir.y == 1);
  CHECK(fabsf(cw_curve_closest_offset2d(c, cw_v2(350, 100)) - 400) < 1e-3f);
  fold_curve(c);
  cw_curve_free(c);
}

/* ── nodes ─────────────────────────────────────────────────────────── */

static void step(cw_tree *t, int n) {
  for (int i = 0; i < n; i++) cw_tree_fixed_update(t, DT);
}
static int32_t ends, changes;
static void on_end(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  ends++;
}
static void on_changed(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  changes++;
}
static t3_vec3 fwd_of(cw_node n) { /* the node's -Z in its parent's space */
  t3_quat q = cw_node3d_quaternion(n);
  return V3(-(2 * (q.x * q.z + q.w * q.y)), -(2 * (q.y * q.z - q.w * q.x)), -(1 - 2 * (q.x * q.x + q.y * q.y)));
}
static t3_vec3 up_of(cw_node n) {
  t3_quat q = cw_node3d_quaternion(n);
  return V3(2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x));
}

static void t_path3d(void) {
  cw_tree *t = cw_tree_new();
  cw_node path = cw_node_new("Path3D", "Track");
  cw_node_add_child(cw_tree_root(t), path);
  cw_connect(path, "curve_changed", path, NULL, on_changed, NULL, 0);
  changes = 0;
  /* 10 along -Z, then 10 up a ramp of 45 degrees */
  CHECK(cw_node_set(path, "points", cw_val_string("0 0 0, 0 0 -10, 0 10 -20")));
  CHECK_EQ_INT(changes, 1);
  cw_value v;
  float ramp = sqrtf(200);
  CHECK(cw_node_get_prop(path, "length", &v) && fabsf((float)v.f - (10 + ramp)) < 1e-4f);
  cw_node f = cw_node_new("PathFollow3D", "Cart");
  cw_node_add_child(path, f);
  CHECK(near3(cw_node3d_position(f), V3(0, 0, 0), 0)); /* on the curve as soon as it enters */
  cw_node_set(f, "progress", cw_val_float(4));
  CHECK(near3(cw_node3d_position(f), V3(0, 0, -4), 1e-6f));
  CHECK(near3(fwd_of(f), V3(0, 0, -1), 1e-5f) && near3(up_of(f), V3(0, 1, 0), 1e-5f));
  /* on the ramp: XYZ pitches up it, Y stays level, NONE keeps what the node had */
  cw_node_set(f, "progress", cw_val_float(10 + ramp / 2));
  CHECK(near3(cw_node3d_position(f), V3(0, 5, -15), 1e-4f));
  CHECK(near3(fwd_of(f), V3(0, 0.70710678f, -0.70710678f), 1e-4f));
  cw_node_set(f, "rotation_mode", cw_val_enum("Y"));
  CHECK(near3(fwd_of(f), V3(0, 0, -1), 1e-5f));
  cw_node_set(f, "rotation_mode", cw_val_enum("NONE"));
  cw_node3d_set_quaternion(f, (t3_quat){ 0, 1, 0, 0 });
  cw_node_set(f, "progress", cw_val_float(2));
  CHECK(cw_node3d_quaternion(f).y == 1);
  cw_node_set(f, "rotation_mode", cw_val_enum("XYZ"));
  /* the model's +Z forward */
  cw_node_set(f, "use_model_front", cw_val_bool(true));
  CHECK(near3(fwd_of(f), V3(0, 0, 1), 1e-5f) && near3(up_of(f), V3(0, 1, 0), 1e-5f));
  cw_node_set(f, "use_model_front", cw_val_bool(false));
  /* offsets: sideways is +X while heading -Z, up is +Y */
  cw_node_set(f, "h_offset", cw_val_float(1.5));
  cw_node_set(f, "v_offset", cw_val_float(0.5));
  CHECK(near3(cw_node3d_position(f), V3(1.5f, 0.5f, -2), 1e-5f));
  cw_node_set(f, "h_offset", cw_val_float(0));
  cw_node_set(f, "v_offset", cw_val_float(0));
  /* ratio, wrap and clamp */
  float len = 10 + ramp;
  cw_node_set(f, "progress_ratio", cw_val_float(0.5));
  CHECK(cw_node_get_prop(f, "progress", &v) && fabsf((float)v.f - len / 2) < 1e-4f);
  cw_node_set(f, "progress", cw_val_float(len + 3));
  CHECK(near3(cw_node3d_position(f), V3(0, 0, -3), 1e-4f)); /* loop: wrapped */
  CHECK(cw_node_get_prop(f, "progress_ratio", &v) && fabsf((float)v.f - 3 / len) < 1e-5f);
  cw_node_set(f, "progress", cw_val_float(-2));
  CHECK(dist3(cw_node3d_position(f), V3(0, 10, -20)) < 2.01f && cw_node3d_position(f).y > 8);
  cw_node_set(f, "loop", cw_val_bool(false));
  cw_node_set(f, "progress", cw_val_float(len + 3));
  CHECK(near3(cw_node3d_position(f), V3(0, 10, -20), 1e-4f)); /* clamped to the end */
  cw_node_set(f, "progress", cw_val_float(-2));
  CHECK(near3(cw_node3d_position(f), V3(0, 0, 0), 0));
  /* speed: 6 m/s for 1 s of ticks, then on to the end, one end_reached */
  cw_connect(f, "end_reached", f, NULL, on_end, NULL, 0);
  ends = 0;
  cw_node_set(f, "progress", cw_val_float(0));
  cw_node_set(f, "speed", cw_val_float(6));
  step(t, 60);
  CHECK(cw_node_get_prop(f, "progress", &v) && fabsf((float)v.f - 6) < 1e-3f);
  float prog = (float)v.f;
  FOLD(prog);
  CHECK_EQ_INT(ends, 0);
  step(t, 300);
  CHECK_EQ_INT(ends, 1);
  CHECK(near3(cw_node3d_position(f), V3(0, 10, -20), 1e-4f));
  cw_node_set(f, "speed", cw_val_float(-30)); /* back down: the other end */
  step(t, 120);
  CHECK_EQ_INT(ends, 2);
  CHECK(near3(cw_node3d_position(f), V3(0, 0, 0), 0));
  /* looping with a speed never ends and stays on the curve */
  cw_node_set(f, "loop", cw_val_bool(true));
  cw_node_set(f, "speed", cw_val_float(11));
  step(t, 600);
  CHECK_EQ_INT(ends, 2);
  CHECK(cw_node_get_prop(f, "progress", &v) && v.f >= 0 && v.f < len);
  t3_vec3 pos = cw_node3d_position(f);
  FOLD(pos);
  cw_node_set(f, "speed", cw_val_float(0));
  /* changing the path moves its followers; so does the curve through its pointer */
  cw_node_set(f, "progress", cw_val_float(5));
  cw_node_set(path, "points", cw_val_string("0 0 0, 20 0 0"));
  CHECK(near3(cw_node3d_position(f), V3(5, 0, 0), 1e-5f));
  cw_curve *c = cw_path_curve(path);
  CHECK(c && cw_curve_point_count(c) == 2);
  cw_curve_point p = cw_curve_get_point(c, 1);
  p.position = V3(0, 0, 20);
  cw_curve_set_point(c, 1, p);
  changes = 0;
  cw_path_curve_changed(path);
  CHECK_EQ_INT(changes, 1);
  CHECK(near3(cw_node3d_position(f), V3(0, 0, 5), 1e-5f));
  CHECK(cw_node_get_prop(path, "points", &v) && !strcmp(v.s, "0 0 0, 0 0 20"));
  if (strcmp(v.s, "0 0 0, 0 0 20")) cw_info("points text: [%s]", v.s);
  /* ORIENTED through a vertical loop: upside down at the top, where XYZ (a look-at) stays upright */
  cw_node_set(path, "points", cw_val_string("0 0 0  0 0 2.2091  0 0 -2.2091, 0 4 -4  0 -2.2091 0  0 2.2091 0, "
                                            "0 8 0  0 0 -2.2091  0 0 2.2091, 0 4 4  0 2.2091 0  0 -2.2091 0"));
  cw_node_set(path, "closed", cw_val_bool(true));
  cw_node_get_prop(path, "length", &v);
  cw_node_set(f, "rotation_mode", cw_val_enum("ORIENTED"));
  cw_node_set(f, "progress", cw_val_float(v.f / 2));
  CHECK(near3(cw_node3d_position(f), V3(0, 8, 0), 1e-3f));
  CHECK(up_of(f).y < -0.999f && near3(fwd_of(f), V3(0, 0, 1), 0.01f));
  t3_quat q = cw_node3d_quaternion(f);
  FOLD(q);
  cw_node_set(f, "rotation_mode", cw_val_enum("XYZ"));
  CHECK(up_of(f).y > 0.999f);
  /* a follower that is not under a path stays put */
  cw_node stray = cw_node_new("PathFollow3D", "Stray");
  cw_node3d_set_position(stray, V3(7, 7, 7));
  cw_node_add_child(cw_tree_root(t), stray);
  cw_node_set(stray, "progress", cw_val_float(3));
  cw_node_set(stray, "speed", cw_val_float(3));
  step(t, 5);
  CHECK(near3(cw_node3d_position(stray), V3(7, 7, 7), 0));
  CHECK(cw_path_curve(stray) == NULL);
  cw_tree_free(t);
}

static void t_path2d(void) {
  cw_tree *t = cw_tree_new();
  cw_node path = cw_node_new("Path2D", "Rail");
  cw_node_add_child(cw_tree_root(t), path);
  cw_node_set(path, "points", cw_val_string("0 0, 300 0, 300 400"));
  cw_node f = cw_node_new("PathFollow2D", "Coin");
  cw_node_add_child(path, f);
  cw_node_set(f, "progress", cw_val_float(100));
  cw_vec2 p = cw_node2d_position(f);
  CHECK(p.x == 100 && p.y == 0 && cw_node2d_rotation(f) == 0);
  cw_node_set(f, "progress", cw_val_float(500));
  p = cw_node2d_position(f);
  CHECK(p.x == 300 && p.y == 200);
  CHECK(fabsf(cw_node2d_rotation(f) - 1.5707963f) < 1e-5f); /* heading down the screen (+y) */
  cw_node_set(f, "h_offset", cw_val_float(10));
  cw_node_set(f, "v_offset", cw_val_float(4));
  p = cw_node2d_position(f);
  CHECK(fabsf(p.x - 296) < 1e-4f && fabsf(p.y - 210) < 1e-4f);
  cw_node_set(f, "h_offset", cw_val_float(0));
  cw_node_set(f, "v_offset", cw_val_float(0));
  cw_node_set(f, "rotates", cw_val_bool(false));
  cw_node2d_set_rotation(f, 0.25f);
  cw_node_set(f, "progress", cw_val_float(50));
  CHECK(cw_node2d_rotation(f) == 0.25f);
  /* a patrol: speed, loop off, end_reached, turn round in the handler's place */
  cw_connect(f, "end_reached", f, NULL, on_end, NULL, 0);
  ends = 0;
  cw_node_set(f, "loop", cw_val_bool(false));
  cw_node_set(f, "progress", cw_val_float(0));
  cw_node_set(f, "speed", cw_val_float(350));
  step(t, 119);
  CHECK_EQ_INT(ends, 0);
  step(t, 2);
  CHECK_EQ_INT(ends, 1);
  p = cw_node2d_position(f);
  CHECK(p.x == 300 && p.y == 400);
  cw_value v;
  CHECK(cw_node_get_prop(f, "progress_ratio", &v) && v.f == 1);
  /* a closed loop keeps going round */
  cw_node_set(path, "closed", cw_val_bool(true));
  cw_node_set(f, "loop", cw_val_bool(true));
  step(t, 1000);
  p = cw_node2d_position(f);
  FOLD(p);
  CHECK(cw_node_get_prop(path, "length", &v) && v.f == 1200);
  cw_tree_free(t);
}

/* scene text: a path and its follower load, sit where they should, and save back byte for byte */
static void t_scene(void) {
  static const char *text =
    "[scene format=1]\n\n[node name=\"Level\" type=\"Node3D\"]\n\n"
    "[node name=\"Track\" parent=\".\" type=\"Path3D\"]\npoints = \"0 0 0, 0 0 -10  0 0 2  0 0 -2  0.3, 8 0 -18\"\nbake_interval = 0.1\n\n"
    "[node name=\"Cart\" parent=\"Track\" type=\"PathFollow3D\"]\nposition = vec3(0, 0, -4)\nrotation = vec3(0, 0, -0.14441739)\nprogress = 4\nloop = false\nspeed = 2\nrotation_mode = ORIENTED\n\n"
    "[node name=\"Rail\" parent=\".\" type=\"Path2D\"]\npoints = \"0 0, 64 0, 64 64\"\nclosed = true\n\n"
    "[node name=\"Coin\" parent=\"Rail\" type=\"PathFollow2D\"]\nposition = vec2(32, 0)\nprogress = 32\nrotates = false\n";
  cw_packed_scene *ps = cw_scene_parse(text, NULL);
  CHECK(ps != NULL);
  if (!ps) return;
  cw_node level = cw_scene_instance(ps);
  cw_tree *t = cw_tree_new();
  cw_node_add_child(cw_tree_root(t), level);
  cw_node cart = cw_node_get(level, "Track/Cart"), coin = cw_node_get(level, "Rail/Coin");
  CHECK(cw_node_is(cart, "PathFollow3D") && cw_node_is(coin, "PathFollow2D"));
  CHECK(near3(cw_node3d_position(cart), V3(0, 0, -4), 1e-6f));
  CHECK(cw_node2d_position(coin).x == 32);
  cw_value v;
  CHECK(cw_node_get_prop(cw_node_get(level, "Rail"), "length", &v) && fabsf((float)v.f - (128 + sqrtf(8192))) < 1e-3f);
  char *out = cw_scene_save(level);
  CHECK(out && !strcmp(out, text));
  if (out && strcmp(out, text)) cw_info("saved:\n%s", out);
  cw_free(out);
  step(t, 30); /* 1 m further */
  CHECK(cw_node_get_prop(cart, "progress", &v) && fabsf((float)v.f - 5) < 1e-4f);
  t3_vec3 p = cw_node3d_position(cart);
  FOLD(p);
  cw_tree_free(t);
  cw_packed_scene_free(ps);
}

CW_TEST_SUITE("curve", { "line", t_line }, { "circle", t_circle }, { "frames", t_frames }, { "text", t_text }, { "2d", t_2d },
              { "path3d", t_path3d }, { "path2d", t_path2d }, { "scene", t_scene })

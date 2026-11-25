/* The cinematic director (cartwheel/director.h), on synthetic people in a walled yard. Each shot type is cut and run
 * for three seconds while the measures (what the live camera sees) are read: the key point in frame, nothing across
 * the line to the face, a close-up never on the back of a head, no shake. The hard cases: a passer-by walking right
 * through a close-up's line, the subject turning away mid-shot, a wall behind the subject, a hand raised to the face,
 * the 180-degree line kept across two cuts, a release that eases back to the game's camera.
 * Control (CW_TEST_DIR_NAIVE=1, read late): the late case runs the passer-by close-up with no solving (the first
 * placement, never scored or re-solved), and its blocked frames must fail; CW_TEST_DIR_NOISE=1 adds handheld noise of
 * 1 cm to the output, and the shake check must fail. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/director.h"
#include "test.h"

/* the yard: four walls 6 m from the middle, a pillar at (2.2, 0, -1.6) */
typedef struct { float lo[3], hi[3]; } box_t;
static const box_t BOXES[] = {
  { { -6.5f, 0, -6.5f }, { 6.5f, 4, -6 } }, { { -6.5f, 0, 6 }, { 6.5f, 4, 6.5f } },
  { { -6.5f, 0, -6.5f }, { -6, 4, 6.5f } }, { { 6, 0, -6.5f }, { 6.5f, 4, 6.5f } },
  { { 1.9f, 0, -1.9f }, { 2.5f, 3, -1.3f } },
};
static bool ray_box(t3_vec3 a, t3_vec3 b, const box_t *x, float *t) {
  float o[3] = { a.x, a.y, a.z }, d[3] = { b.x - a.x, b.y - a.y, b.z - a.z }, t0 = 0, t1 = 1;
  for (int k = 0; k < 3; k++) {
    if (fabsf(d[k]) < 1e-9f) { if (o[k] < x->lo[k] || o[k] > x->hi[k]) return false; continue; }
    float u = (x->lo[k] - o[k]) / d[k], v = (x->hi[k] - o[k]) / d[k];
    if (u > v) { float w = u; u = v; v = w; }
    if (u > t0) t0 = u;
    if (v < t1) t1 = v;
    if (t0 > t1) return false;
  }
  *t = t0; return true;
}
static bool ray(t3_vec3 a, t3_vec3 b, float *t, void *u) {
  (void)u;
  float best = 2; bool hit = false; /* nearest hit */
  for (size_t i = 0; i < sizeof BOXES / sizeof *BOXES; i++) { float tt; if (ray_box(a, b, &BOXES[i], &tt) && tt < best) { best = tt; hit = true; } }
  if (b.y < 0 && a.y >= 0) { float tg = a.y / (a.y - b.y); if (tg < best) { best = tg; hit = true; } } /* the ground */
  if (hit) *t = best;
  return hit;
}

/* a person at (x, z) facing yaw, 1.75 m tall */
static cw_dir_subject person(float x, float z, float yaw) {
  float fx = sinf(yaw), fz = cosf(yaw);
  return (cw_dir_subject){ .eye = { x + fx * .08f, 1.64f, z + fz * .08f }, .chest = { x, 1.3f, z }, .feet = { x, 0, z },
                           .face = { fx, 0, fz }, .hand_l = { x + fz * .35f, .85f, z - fx * .35f }, .hand_r = { x - fz * .35f, .85f, z + fx * .35f },
                           .radius = .24f, .head_radius = .14f, .visible = true };
}

typedef struct { int32_t blocked, away, out, rev, resolves; } meas_t;
static cw_director *cur;
static meas_t measures(void) {
  return (meas_t){ (int32_t)cw_director_measure(cur, CW_DIR_M_BLOCKED), (int32_t)cw_director_measure(cur, CW_DIR_M_AWAY),
                   (int32_t)cw_director_measure(cur, CW_DIR_M_OUT), (int32_t)cw_director_measure(cur, CW_DIR_M_REVERSALS),
                   (int32_t)cw_director_measure(cur, CW_DIR_M_RESOLVES) };
}

/* runs a shot for secs; walk: the passer-by (subject 2) crosses along z = pz from x0 to x1; turn_away: the subject
 * turns its back halfway */
static void run_shot(cw_director *d, cw_shot shot, float secs, bool walk, float x0, float x1, float pz, bool turn_away) {
  cw_director_cut(d, &shot);
  cw_dir_camera cam;
  int n = (int)(secs * 60);
  for (int i = 0; i < n; i++) {
    float k = (float)i / (float)n;
    if (walk) { cw_dir_subject p = person(x0 + (x1 - x0) * k, pz, 1.57f); cw_director_set_subject(d, 2, &p); }
    if (turn_away && i == n / 2) { cw_dir_subject a = person(0, 0, 3.14159f); cw_director_set_subject(d, 0, &a); }
    cw_director_update(d, 1.f / 60, &cam);
  }
}

static cw_director *fresh(bool naive, float noise) {
  cw_director *d = cw_director_new(&(cw_director_desc){ .aspect = 16.f / 9, .ray = ray, .no_solve = naive, .test_noise = noise });
  cw_dir_subject a = person(0, 0, 0), b = person(0, 1.6f, 3.14159f); /* face to face, 1.6 m apart */
  cw_director_set_subject(d, 0, &a);
  cw_director_set_subject(d, 1, &b);
  cur = d;
  return d;
}

static void test_types(void) {
  static const cw_shot_type T[] = { CW_SHOT_WIDE, CW_SHOT_ESTABLISH, CW_SHOT_TWO, CW_SHOT_OTS, CW_SHOT_MCU, CW_SHOT_CU,
                                    CW_SHOT_PUSH, CW_SHOT_REACT, CW_SHOT_HERO, CW_SHOT_TOP };
  for (size_t i = 0; i < sizeof T / sizeof *T; i++) {
    cw_director *d = fresh(false, 0);
    cw_shot s = { .type = T[i], .subj = { 0, 1 }, .nsubj = T[i] == CW_SHOT_MCU || T[i] == CW_SHOT_CU || T[i] == CW_SHOT_PUSH || T[i] == CW_SHOT_HERO ? 1 : 2, .dur = 3 };
    run_shot(d, s, 3, false, 0, 0, 0, false);
    meas_t m = measures();
    if (m.blocked || m.away || m.out || m.rev) cw_error("director type %d: blocked %d away %d out %d reversals %d", T[i], m.blocked, m.away, m.out, m.rev);
    CHECK_EQ_INT(m.blocked, 0);
    CHECK_EQ_INT(m.away, 0);
    CHECK_EQ_INT(m.out, 0);
    CHECK_EQ_INT(m.rev, 0);
    CHECK(cw_director_active(d));
    CHECK(cw_director_shot_done(d));
    cw_director_free(d);
  }
}

static void test_insert(void) {
  cw_director *d = fresh(false, 0);
  /* a hand raised to work height: an insert on it */
  cw_dir_subject a = person(0, 0, 0); a.hand_r = (t3_vec3){ -.2f, 1.38f, .35f };
  cw_director_set_subject(d, 0, &a);
  run_shot(d, (cw_shot){ .type = CW_SHOT_INSERT, .subj = { 0 }, .nsubj = 1, .part = CW_SHOT_PART_HAND_R, .dur = 2 }, 2, false, 0, 0, 0, false);
  CHECK_EQ_INT(cw_director_solved_type(d), CW_SHOT_INSERT);
  meas_t m = measures();
  CHECK_EQ_INT(m.out, 0);
  CHECK_EQ_INT(m.blocked, 0);
  cw_director_free(d);
  /* a hand hanging at the hip would frame the hips: it becomes a medium close-up */
  d = fresh(false, 0);
  run_shot(d, (cw_shot){ .type = CW_SHOT_INSERT, .subj = { 0 }, .nsubj = 1, .part = CW_SHOT_PART_HAND_R, .dur = 1 }, 1, false, 0, 0, 0, false);
  CHECK_EQ_INT(cw_director_solved_type(d), CW_SHOT_MCU);
  cw_director_free(d);
}

/* a passer-by crossing a close-up's line, 0.7 m in front of the subject: the shot re-solves and nothing is ever
 * between the lens and the face */
static meas_t passerby(bool naive, float noise) {
  cw_director *d = fresh(naive, noise);
  cw_shot s = { .type = CW_SHOT_CU, .subj = { 0, 1 }, .nsubj = 2, .dur = 4 };
  run_shot(d, s, 4, true, -2.5f, 2.5f, .7f, false);
  meas_t m = measures();
  cw_director_free(d);
  return m;
}
static void test_passerby(void) {
  meas_t m = passerby(false, 0);
  if (m.blocked) cw_error("director passer-by: %d blocked frames, %d re-solves", m.blocked, m.resolves);
  CHECK_EQ_INT(m.blocked, 0);
  CHECK_EQ_INT(m.away, 0);
  CHECK_EQ_INT(m.rev, 0);
}

static void test_turn_away(void) {
  cw_director *d = fresh(false, 0);
  run_shot(d, (cw_shot){ .type = CW_SHOT_MCU, .subj = { 0 }, .nsubj = 1, .dur = 3 }, 3, false, 0, 0, 0, true);
  meas_t m = measures();
  /* the turn is instant: the next live checks find the back of the head and re-solve to the face (within 0.3 s) */
  CHECK(m.resolves >= 1);
  CHECK(m.away <= 18);
  cw_director_free(d);
}

static void test_release(void) {
  cw_director *d = fresh(false, 0);
  cw_dir_camera game = { { 0, 3, -5 }, { 0, 1, 0 }, 60 }, cam;
  cw_director_set_gameplay(d, &game);
  cw_director_cut(d, &(cw_shot){ .type = CW_SHOT_TWO, .subj = { 0, 1 }, .nsubj = 2 });
  for (int i = 0; i < 30; i++) cw_director_update(d, 1.f / 60, &cam);
  cw_director_release(d, .5f);
  int frames = 0;
  while (cw_director_update(d, 1.f / 60, &cam) && frames < 200) frames++;
  CHECK(!cw_director_active(d));
  CHECK(frames >= 29 && frames <= 31);
  CHECK(fabsf(cam.pos.x - game.pos.x) < 1e-3f && fabsf(cam.pos.y - game.pos.y) < 1e-3f && fabsf(cam.fov - 60) < 1e-3f);
  meas_t m = measures();
  CHECK_EQ_INT(m.rev, 0);
  cw_director_free(d);
}

static void test_line(void) {
  /* two two-shots in a row keep the camera on the same side of the line between the two people */
  cw_director *d = fresh(false, 0);
  cw_dir_camera c1, c2;
  cw_director_cut(d, &(cw_shot){ .type = CW_SHOT_TWO, .subj = { 0, 1 }, .nsubj = 2 });
  cw_director_update(d, 1.f / 60, &c1);
  cw_director_cut(d, &(cw_shot){ .type = CW_SHOT_OTS, .subj = { 0, 1 }, .nsubj = 2 });
  cw_director_update(d, 1.f / 60, &c2);
  /* the line runs along z at x = 0 */
  CHECK((c1.pos.x > 0) == (c2.pos.x > 0));
  cw_director_free(d);
}

void cw_test_late(void) {
  bool naive = cw_test_env("CW_TEST_DIR_NAIVE") != NULL, noise = cw_test_env("CW_TEST_DIR_NOISE") != NULL;
  meas_t m = passerby(naive, noise ? .01f : 0);
  if (naive || noise) cw_info("director control: %d blocked frames, %d reversals", m.blocked, m.rev);
  CHECK_EQ_INT(m.blocked, 0);
  CHECK_EQ_INT(m.rev, 0);
}

CW_TEST_SUITE("director", { "types", test_types }, { "insert", test_insert }, { "passerby", test_passerby },
              { "turn_away", test_turn_away }, { "release", test_release }, { "line", test_line })

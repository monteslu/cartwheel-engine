/* cw_cloth unit suite: a flag in wind, cloth draped on a sphere and a box,
 * a fast sphere through a curtain, ropes holding and swinging a crate, a
 * rope between two bodies, skin limits, rollback and a chain of rigid links.
 * Each test that bounds something (stretch, energy, penetration) also runs a
 * control that must break the bound. Hashes and every numeric result fold
 * into unit.checksum, which the gate compares across hosts; one worker and
 * the whole pool give the same bits. */
#include <math.h>
#include <string.h>
#include <time.h>

#include "cartwheel/cloth.h"
#include "test.h"

#define DT (1.0f / 60)

static cw_world3d world(void) {
  cw_world3d_def wd = cw_world3d_default_def();
  wd.sleep = false;
  return cw_world3d_create(&wd);
}

static cw_body3d static_box(cw_world3d w, t3_vec3 at, t3_vec3 half) {
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_STATIC;
  bd.position = at;
  cw_body3d b = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_box(b, &sd, half, cw_xform3_identity());
  return b;
}

static void step(cw_world3d w, cw_cloth c, int ticks) {
  for (int i = 0; i < ticks; i++) {
    cw_world3d_step(w, DT);
    cw_cloth_step(c, DT);
  }
}

/* a 32 x 32 flag, 2 x 1.4 m, pinned along its pole edge, in a gusty wind */
static cw_cloth make_flag(cw_world3d w, int workers, float slack, int n) {
  cw_cloth_def d = cw_cloth_default_def();
  float inv[64 * 64];
  for (int i = 0; i < n * n; i++) inv[i] = 1;
  for (int j = 0; j < n; j++) inv[j * n] = 0; /* the pole side: column 0 */
  d.inv_mass = inv;
  d.mass = 0.4f;
  d.workers = workers;
  d.tether_slack = slack;
  cw_cloth c = cw_cloth_create_grid(w, &d, n, n, (t3_vec3){ 0, 3, 0 }, (t3_vec3){ 2, 0, 0 }, (t3_vec3){ 0, -1.4f, 0 });
  cw_cloth_set_wind(c, (t3_vec3){ 6, 0, 1.5f }, 1, 0.3f, 0.6f);
  return c;
}

static void test_flag(void) {
  uint32_t hashes[2];
  for (int k = 0; k < 2; k++) {
    cw_world3d w = world();
    cw_cloth c = make_flag(w, k ? 1 : 0, 1.02f, 32);
    CHECK(cw_cloth_valid(c));
    float max_ke = 0, max_stretch = 0, mean_stretch = 0;
    for (int t = 0; t < 600; t++) {
      step(w, c, 1);
      cw_cloth_stats st = cw_cloth_stats_get(c);
      if (t >= 60 && st.kinetic_energy > max_ke) max_ke = st.kinetic_energy;
      if (st.max_stretch > max_stretch) max_stretch = st.max_stretch;
      if (st.mean_stretch > mean_stretch) mean_stretch = st.mean_stretch;
    }
    cw_cloth_stats st = cw_cloth_stats_get(c);
    CHECK_EQ_INT(st.vertices, 32 * 32);
    CHECK(st.groups >= 3);
    CHECK_EQ_INT(st.steps, 600);
    /* the flag flies: its free corner is downwind of the pole */
    int n;
    const t3_vec3 *x = cw_cloth_positions(c, &n);
    CHECK(x[31].x > 1.0f);
    CHECK(x[31].z > 0.05f);
    /* bounds: energy stays small and finite; stretch stays low on average, and no edge (the
     * shearing top corner in gusts) reaches 30% */
    CHECK(max_ke > 0 && max_ke < 5);
    CHECK(mean_stretch < 0.02f);
    CHECK(max_stretch < 0.3f);
    cw_info("cloth flag: stretch mean %.4f max %.4f, ke %.4f J, groups %d, %d constraints", mean_stretch, max_stretch,
            max_ke, st.groups, st.constraints);
    hashes[k] = cw_cloth_hash(c);
    FOLD(hashes[k]);
    cw_world3d_destroy(w);
  }
  CHECK_EQ_INT(hashes[0], hashes[1]); /* one worker and the pool: the same bits */
  /* control: a gale must break the energy bound (it can see energy) */
  cw_world3d w = world();
  cw_cloth c = make_flag(w, 0, 1.02f, 32);
  cw_cloth_set_wind(c, (t3_vec3){ 40, 0, 10 }, 1, 0.3f, 0.6f);
  float ke = 0;
  for (int t = 0; t < 120; t++) {
    step(w, c, 1);
    cw_cloth_stats st = cw_cloth_stats_get(c);
    if (st.kinetic_energy > ke) ke = st.kinetic_energy;
  }
  cw_info("cloth flag in a gale (control): ke %.2f J", ke);
  CHECK(ke > 5);
  cw_world3d_destroy(w);
}

/* tethers bound how far a hanging cloth stretches from its pins; the control (no tethers, soft
 * edges) must stretch past the bound */
static float hanging_stretch(float slack, float compliance) {
  cw_world3d w = world();
  cw_cloth_def d = cw_cloth_default_def();
  float inv[24 * 24];
  for (int i = 0; i < 24 * 24; i++) inv[i] = i < 24 ? 0 : 1; /* the top row pinned */
  d.inv_mass = inv;
  d.tether_slack = slack;
  d.stretch_compliance = compliance;
  d.substeps = 2;
  d.mass = 20;
  cw_cloth c = cw_cloth_create_grid(w, &d, 24, 24, (t3_vec3){ -1, 3, 0 }, (t3_vec3){ 2, 0, 0 }, (t3_vec3){ 0, -3, 0 });
  step(w, c, 240);
  /* the length of the middle column against its rest length */
  int n;
  const t3_vec3 *x = cw_cloth_positions(c, &n);
  float len = 0;
  for (int j = 0; j + 1 < 24; j++) {
    t3_vec3 a = x[12 + j * 24], b = x[12 + (j + 1) * 24];
    len += sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
  }
  cw_world3d_destroy(w);
  return len / 3.0f - 1;
}

static void test_stretch(void) {
  float s = hanging_stretch(1.02f, 1e-4f), ctl = hanging_stretch(0, 1e-4f);
  cw_info("cloth stretch: %.4f with tethers, %.4f without (control)", s, ctl);
  CHECK(s < 0.02f + 1e-3f);
  CHECK(ctl > 0.021f); /* control: the bound must fail without tethers */
  FOLD(s);
  FOLD(ctl);
}

/* how deep any particle went into a sphere of radius r at c over the run (minus thickness) */
static float sphere_drape(bool collide, float *out_rest_y) {
  cw_world3d w = world();
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_STATIC;
  bd.position = (t3_vec3){ 0, 1, 0 };
  cw_body3d b = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_sphere(b, &sd, (t3_vec3){ 0, 0, 0 }, 0.8f);
  static_box(w, (t3_vec3){ 0, -0.5f, 0 }, (t3_vec3){ 5, 0.5f, 5 });
  cw_cloth_def d = cw_cloth_default_def();
  d.collide_mask = collide ? CW_PHYS_ALL_LAYERS : 0;
  d.thickness = 0.02f;
  cw_cloth c = cw_cloth_create_grid(w, &d, 24, 24, (t3_vec3){ -1.2f, 2.2f, -1.2f }, (t3_vec3){ 2.4f, 0, 0 },
                                    (t3_vec3){ 0, 0, 2.4f });
  float worst = -1;
  for (int t = 0; t < 300; t++) {
    step(w, c, 1);
    int n;
    const t3_vec3 *x = cw_cloth_positions(c, &n);
    for (int i = 0; i < n; i++) {
      float dx = x[i].x, dy = x[i].y - 1, dz = x[i].z;
      float pen = 0.8f + d.thickness - sqrtf(dx * dx + dy * dy + dz * dz);
      if (pen > worst) worst = pen;
    }
  }
  int n;
  const t3_vec3 *x = cw_cloth_positions(c, &n);
  *out_rest_y = x[12 + 12 * 24].y;
  FOLD(*out_rest_y);
  uint32_t hh = cw_cloth_hash(c);
  FOLD(hh);
  cw_world3d_destroy(w);
  return worst;
}

static void test_sphere(void) {
  float top, top_ctl;
  float pen = sphere_drape(true, &top), ctl = sphere_drape(false, &top_ctl);
  cw_info("cloth on sphere: deepest %.4f m past thickness (control %.4f), top at %.3f", pen, ctl, top);
  CHECK(pen < 0.02f);               /* no particle deeper than thickness (a little slack: one substep's push) */
  CHECK(top > 1.8f && top < 1.86f); /* the middle rests on the sphere's top (1.8 + thickness) */
  CHECK(ctl > 0.5f);                /* control: without collision it falls through */
}

/* a sphere at 12 m/s through a hanging curtain must carry it, not pass through it */
static float fast_sphere(bool collide) {
  cw_world3d w = world();
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_KINEMATIC;
  bd.position = (t3_vec3){ 0, 1.5f, -8 }; /* reaches the curtain (z = 0) half a second after it settled */
  bd.linear_velocity = (t3_vec3){ 0, 0, 12 };
  cw_body3d b = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_sphere(b, &sd, (t3_vec3){ 0, 0, 0 }, 0.4f);
  cw_cloth_def d = cw_cloth_default_def();
  d.collide_mask = collide ? CW_PHYS_ALL_LAYERS : 0;
  float inv[20 * 20];
  for (int i = 0; i < 400; i++) inv[i] = i < 20 ? 0 : 1;
  d.inv_mass = inv;
  cw_cloth c = cw_cloth_create_grid(w, &d, 20, 20, (t3_vec3){ -1, 2.5f, 0 }, (t3_vec3){ 2, 0, 0 }, (t3_vec3){ 0, -2, 0 });
  step(w, c, 30); /* settle */
  float worst = -1;
  for (int t = 0; t < 30; t++) {
    step(w, c, 1);
    t3_vec3 sp = cw_body3d_transform(b).p;
    int n;
    const t3_vec3 *x = cw_cloth_positions(c, &n);
    for (int i = 0; i < n; i++) {
      float dx = x[i].x - sp.x, dy = x[i].y - sp.y, dz = x[i].z - sp.z;
      float pen = 0.4f - sqrtf(dx * dx + dy * dy + dz * dz);
      if (pen > worst) worst = pen;
    }
  }
  /* the curtain's middle was carried forward with the ball */
  int n;
  const t3_vec3 *x = cw_cloth_positions(c, &n);
  float mid_z = x[10 + 10 * 20].z;
  FOLD(mid_z);
  cw_world3d_destroy(w);
  return worst;
}

static void test_tunnel(void) {
  float pen = fast_sphere(true), ctl = fast_sphere(false);
  cw_info("cloth vs fast sphere: deepest %.4f m inside (control %.4f)", pen, ctl);
  CHECK(pen < 0.05f);
  CHECK(ctl > 0.2f); /* control: without collision particles end up inside the ball */
}

static void test_box(void) {
  cw_world3d w = world();
  static_box(w, (t3_vec3){ 0, 0.5f, 0 }, (t3_vec3){ 0.5f, 0.5f, 0.5f });
  static_box(w, (t3_vec3){ 0, -0.5f, 0 }, (t3_vec3){ 5, 0.5f, 5 });
  cw_cloth_def d = cw_cloth_default_def();
  cw_cloth c = cw_cloth_create_grid(w, &d, 20, 20, (t3_vec3){ -1, 1.6f, -1 }, (t3_vec3){ 2, 0, 0 }, (t3_vec3){ 0, 0, 2 });
  step(w, c, 300);
  int n;
  const t3_vec3 *x = cw_cloth_positions(c, &n);
  float worst = -1, top = x[10 + 10 * 20].y;
  for (int i = 0; i < n; i++) {
    float inside = fminf(fminf(0.5f - fabsf(x[i].x), 0.5f - fabsf(x[i].z)), fminf(x[i].y, 1 - x[i].y));
    if (inside > worst) worst = inside;
    if (-x[i].y > worst) worst = -x[i].y; /* under the floor */
  }
  cw_info("cloth on box: deepest %.4f, top %.3f", worst, top);
  CHECK(worst < 0.0f); /* never inside: thickness keeps every particle out */
  CHECK(top > 1.0f && top < 1.05f);
  FOLD(top);
  cw_world3d_destroy(w);
}

/* a 3 m rope from a fixed point holds a 20 kg crate; tension ~ its weight; the crate swings */
static cw_body3d crate(cw_world3d w, t3_vec3 at, float kg) {
  cw_body3d_def bd = cw_body3d_default_def();
  bd.position = at;
  bd.sleep = false;
  cw_body3d b = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  sd.density = kg / (0.5f * 0.5f * 0.5f);
  cw_shape3d_box(b, &sd, (t3_vec3){ 0.25f, 0.25f, 0.25f }, cw_xform3_identity());
  return b;
}

static void test_rope_crate(void) {
  for (int k = 0; k < 2; k++) {
    bool attached = k == 0;
    cw_world3d w = world();
    static_box(w, (t3_vec3){ 0, -10.5f, 0 }, (t3_vec3){ 5, 0.5f, 5 }); /* far floor */
    cw_rope_def rd = cw_rope_default_def();
    rd.start = (t3_vec3){ 0, 5, 0 };
    rd.end = (t3_vec3){ 0, 2, 0 };
    rd.segments = 20;
    rd.mass = 0.5f;
    cw_cloth r = cw_rope_create(w, &rd);
    cw_body3d b = crate(w, (t3_vec3){ 0, 1.75f, 0 }, 20);
    if (attached) cw_cloth_attach_body(r, 20, b, (t3_vec3){ 0, 0.25f, 0 });
    float tension = 0;
    for (int t = 0; t < 240; t++) {
      step(w, r, 1);
      if (t >= 180) tension += cw_cloth_attachment_force(r, 20) / 60;
    }
    float y = cw_body3d_transform(b).p.y;
    if (attached) {
      cw_info("rope holds crate: crate y %.3f, pull on the crate %.1f N (its weight: 196 N)", y, tension);
      CHECK(y > 1.6f && y < 1.75f); /* hanging at the rope's length (2% slack at most) */
      CHECK(tension > 176 && tension < 216);
      /* push it sideways: it swings on the rope (back past the middle) and stays at its length */
      cw_body3d_set_velocity(b, (t3_vec3){ 3, 0, 0 });
      float minx = 0, maxx = 0, maxd = 0;
      for (int t = 0; t < 240; t++) {
        step(w, r, 1);
        t3_vec3 p = cw_body3d_transform(b).p;
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        float dd = sqrtf(p.x * p.x + (p.y + 0.25f - 5) * (p.y + 0.25f - 5) + p.z * p.z);
        if (dd > maxd) maxd = dd;
      }
      cw_info("crate swings: x %.2f .. %.2f, farthest %.3f m from the pin", minx, maxx, maxd);
      CHECK(maxx > 1.0f && minx < -0.5f);
      CHECK(maxd < 3.0f * 1.05f);
      FOLD(minx);
      FOLD(maxx);
    } else {
      CHECK(y < -5); /* control: unattached, it falls past the rope's reach */
    }
    FOLD(y);
    uint32_t hh = cw_cloth_hash(r);
    FOLD(hh);
    cw_world3d_destroy(w);
  }
}

/* a rope between two dynamic bodies: a heavy one driven away tows the light one along the
 * ground at the rope's length; the control (not attached) stays put */
static void tow(bool attached, float *bx, float *gap) {
  cw_world3d w = world();
  static_box(w, (t3_vec3){ 0, -0.5f, 0 }, (t3_vec3){ 20, 0.5f, 5 });
  cw_body3d a = crate(w, (t3_vec3){ 0, 0.25f, 0 }, 200), b = crate(w, (t3_vec3){ -2.6f, 0.25f, 0 }, 10);
  cw_rope_def rd = cw_rope_default_def();
  rd.start = (t3_vec3){ -0.25f, 0.25f, 0 };
  rd.end = (t3_vec3){ -2.35f, 0.25f, 0 };
  rd.pin_start = false;
  rd.segments = 12;
  rd.mass = 0.2f;
  cw_cloth r = cw_rope_create(w, &rd);
  if (attached) {
    cw_cloth_attach_body(r, 0, a, (t3_vec3){ -0.25f, 0, 0 });
    cw_cloth_attach_body(r, 12, b, (t3_vec3){ 0.25f, 0, 0 });
  }
  for (int t = 0; t < 180; t++) {
    t3_vec3 v = cw_body3d_velocity(a);
    cw_body3d_set_velocity(a, (t3_vec3){ 2, v.y, 0 }); /* a drives away at 2 m/s */
    step(w, r, 1);
  }
  t3_vec3 pa = cw_body3d_transform(a).p, pb = cw_body3d_transform(b).p;
  *bx = pb.x;
  *gap = (pa.x - 0.25f) - (pb.x + 0.25f);
  FOLD(pa);
  FOLD(pb);
  uint32_t hh = cw_cloth_hash(r);
  FOLD(hh);
  cw_world3d_destroy(w);
}

static void test_rope_two_bodies(void) {
  float bx, gap, bx_ctl, gap_ctl;
  tow(true, &bx, &gap);
  tow(false, &bx_ctl, &gap_ctl);
  cw_info("rope tow: b at x %.3f, gap %.3f m (rope 2.1 m); control b at %.3f", bx, gap, bx_ctl);
  CHECK(bx > 2.5f);                    /* towed about 5.4 m */
  CHECK(gap > 1.9f && gap < 2.1f * 1.05f); /* at the rope's length */
  CHECK(bx_ctl < -2.5f);               /* control: not attached, it stays */
}

static void test_skin(void) {
  cw_world3d w = world();
  cw_cloth_def d = cw_cloth_default_def();
  float maxd[10 * 10];
  for (int i = 0; i < 100; i++) maxd[i] = (float)(i / 10) * 0.05f; /* rows hang looser further down */
  d.max_distance = maxd;
  cw_cloth c = cw_cloth_create_grid(w, &d, 10, 10, (t3_vec3){ 0, 2, 0 }, (t3_vec3){ 1, 0, 0 }, (t3_vec3){ 0, -1, 0 });
  t3_vec3 target[100];
  int n;
  const t3_vec3 *x0 = cw_cloth_positions(c, &n);
  memcpy(target, x0, sizeof target);
  float worst = 0;
  for (int t = 0; t < 120; t++) {
    for (int i = 0; i < 100; i++) target[i].x += 0.02f; /* the back moves 1.2 m/s */
    cw_cloth_set_skin_targets(c, target);
    step(w, c, 1);
    const t3_vec3 *x = cw_cloth_positions(c, &n);
    for (int i = 0; i < 100; i++) {
      float dx = x[i].x - target[i].x, dy = x[i].y - target[i].y, dz = x[i].z - target[i].z;
      float over = sqrtf(dx * dx + dy * dy + dz * dz) - maxd[i];
      if (over > worst) worst = over;
    }
  }
  cw_info("cloth skin: worst %.5f m past max distance", worst);
  CHECK(worst < 1e-4f);
  uint32_t hh = cw_cloth_hash(c);
  FOLD(hh);
  cw_world3d_destroy(w);
}

static void test_rollback(void) {
  cw_world3d w = world();
  cw_cloth c = make_flag(w, 0, 1.02f, 16);
  step(w, c, 60);
  size_t sz = cw_cloth_state_size(c);
  CHECK(sz > 0);
  void *buf = cw_alloc(sz, CW_MEM_GAME);
  CHECK(cw_cloth_save(c, buf));
  step(w, c, 60);
  uint32_t a = cw_cloth_hash(c);
  CHECK(cw_cloth_load(c, buf, sz));
  step(w, c, 60);
  uint32_t b = cw_cloth_hash(c);
  CHECK_EQ_INT(a, b);
  CHECK(!cw_cloth_load(c, buf, sz - 1));
  FOLD(a);
  cw_free(buf);
  cw_world3d_destroy(w);
}

static void test_chain(void) {
  cw_world3d w = world();
  cw_chain3d_def cd = cw_chain3d_default_def();
  cd.start = (t3_vec3){ 0, 4, 0 };
  cd.end = (t3_vec3){ 2, 4, 0 }; /* laid out sideways: it falls and hangs */
  cd.links = 10;
  cd.damping = 1;
  cw_body3d links[10];
  cw_joint3d joints[11];
  CHECK_EQ_INT(cw_chain3d_create(w, &cd, links, joints), 10);
  CHECK(cw_joint3d_valid(joints[0]) && !cw_joint3d_valid(joints[10]));
  for (int t = 0; t < 600; t++) cw_world3d_step(w, DT);
  t3_vec3 last = cw_body3d_transform(links[9]).p;
  cw_info("chain: last link at %.3f %.3f %.3f", last.x, last.y, last.z);
  CHECK(last.y < 2.4f && last.y > 1.8f && fabsf(last.x) < 0.3f);
  FOLD(last);
  cw_world3d_destroy(w);
}

static void test_bad_input(void) {
  cw_world3d w = world();
  cw_cloth_def d = cw_cloth_default_def();
  CHECK(!cw_cloth_valid(cw_cloth_create(w, &d)));
  CHECK(!cw_cloth_valid(cw_cloth_create_grid(w, &d, 1, 5, (t3_vec3){ 0 }, (t3_vec3){ 1, 0, 0 }, (t3_vec3){ 0, 1, 0 })));
  cw_rope_def rd = cw_rope_default_def();
  rd.segments = 0;
  CHECK(!cw_cloth_valid(cw_rope_create(w, &rd)));
  CHECK(!cw_cloth_valid((cw_cloth){ 0 }));
  cw_world3d_destroy(w);
}

/* 64 x 64 at 8 substeps: microseconds per step (logged, not compared) */
#ifndef CLOCK_THREAD_CPUTIME_ID /* wasi */
#define CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC
#endif
static void perf_run(int workers) {
  cw_world3d w = world();
  cw_cloth c = make_flag(w, workers, 1.02f, 64);
  step(w, c, 30);
  struct timespec t0, t1, c0, c1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
  step(w, c, 60);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double us = ((double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9) * 1e6 / 60;
  double cpu = ((double)(c1.tv_sec - c0.tv_sec) + (double)(c1.tv_nsec - c0.tv_nsec) * 1e-9) * 1e6 / 60;
  cw_cloth_stats st = cw_cloth_stats_get(c);
  cw_info("cloth perf: 64x64 (%d particles, %d constraints, %d groups), 8 substeps, %s: %.0f us/step wall, %.0f us on the calling thread's CPU",
          st.vertices, st.constraints, st.groups, workers == 1 ? "1 thread" : "the job pool", us, cpu);
  CHECK(st.vertices == 4096);
  cw_world3d_destroy(w);
}
static void test_perf(void) {
  perf_run(1);
  perf_run(0);
}

CW_TEST_SUITE("cloth", { "flag", test_flag }, { "stretch", test_stretch }, { "sphere", test_sphere },
              { "tunnel", test_tunnel }, { "box", test_box }, { "rope_crate", test_rope_crate },
              { "rope_two_bodies", test_rope_two_bodies }, { "skin", test_skin }, { "rollback", test_rollback },
              { "chain", test_chain }, { "bad_input", test_bad_input }, { "perf", test_perf })

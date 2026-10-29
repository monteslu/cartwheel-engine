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
#include "cartwheel/physics_nodes.h"
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

/* the node types: a Cloth3D pinned to a bone follows it (control: no bone, it stays); a Rope3D
 * holds a RigidBody3D crate and reports its pull; a LINKS Rope3D holds a weight */
static float bone_follow(bool use_bone) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node model = cw_node_new("Node3D", "Model");
  cw_node_add_child(root, model);
  cw_node3d_set_position(model, (t3_vec3){ 0, 2, 0 });
  t3_object *bone = t3_object_new();
  bone->name = "spine";
  t3_object_set_position(bone, 1, 0, 0);
  t3_object_add(cw_node3d_object(model), bone);
  cw_node cape = cw_node_new("Cloth3D", "Cape");
  cw_node_add_child(root, cape);
  cw_node3d_set_position(cape, (t3_vec3){ 1, 2, 0 });
  cw_node_set(cape, "columns", cw_val_int(4));
  cw_node_set(cape, "rows", cw_val_int(4));
  cw_node_set(cape, "attach_to", cw_val_string("../Model"));
  if (use_bone) cw_node_set(cape, "bone", cw_val_string("spine"));
  for (int i = 0; i < 10; i++) cw_tree_fixed_update(t, DT);
  t3_object_set_position(bone, 1, 0, 2); /* the bone moves 2 m along z */
  for (int i = 0; i < 30; i++) cw_tree_fixed_update(t, DT);
  int n;
  const t3_vec3 *x = cw_cloth_positions(cw_cloth3d_get(cape), &n);
  float z = n ? x[0].z : -1;
  FOLD(z);
  t3_release(bone);
  cw_tree_free(t);
  return z;
}

static void test_nodes(void) {
  float z = bone_follow(true), z_ctl = bone_follow(false);
  cw_info("Cloth3D on a bone: pin z %.3f (control without the bone %.3f)", z, z_ctl);
  CHECK(fabsf(z - 2) < 1e-3f);
  CHECK(fabsf(z_ctl) < 1e-3f);
  /* Rope3D holding a crate, and a LINKS chain holding a weight */
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  const char *modes[2] = { "XPBD", "LINKS" };
  cw_node bodies[2];
  for (int i = 0; i < 2; i++) {
    char rn[16], bn[16], path[24];
    snprintf(rn, sizeof rn, "Rope%d", i), snprintf(bn, sizeof bn, "Crate%d", i), snprintf(path, sizeof path, "../Crate%d", i);
    cw_node r = cw_node_new("Rope3D", rn);
    cw_node_add_child(root, r);
    cw_node3d_set_position(r, (t3_vec3){ (float)i * 3, 4, 0 });
    cw_node_set(r, "end", cw_val_vec3(0, -2, 0));
    cw_node_set(r, "mode", cw_val_enum(modes[i]));
    cw_node_set(r, "segments", cw_val_int(10));
    cw_node_set(r, "end_attach", cw_val_string(path));
    cw_node b = cw_node_new("RigidBody3D", bn);
    cw_node_add_child(root, b);
    cw_node3d_set_position(b, (t3_vec3){ (float)i * 3, 1.75f, 0 });
    cw_node_set(b, "mass", cw_val_float(10));
    cw_node_set(b, "can_sleep", cw_val_bool(false));
    cw_node s = cw_node_new("CollisionShape3D", "Shape");
    cw_node_set(s, "size", cw_val_vec3(0.5f, 0.5f, 0.5f));
    cw_node_add_child(b, s);
    bodies[i] = b;
  }
  for (int i = 0; i < 180; i++) cw_tree_fixed_update(t, DT);
  cw_value v;
  CHECK(cw_node_get_prop(cw_node_get(root, "Rope0"), "tension", &v));
  float tension = (float)v.f;
  float y0 = cw_node3d_position(bodies[0]).y, y1 = cw_node3d_position(bodies[1]).y;
  cw_info("Rope3D: crate at y %.3f pulled with %.1f N (weight 98 N); LINKS weight at y %.3f", y0, tension, y1);
  CHECK(y0 > 1.6f && y0 < 1.8f);
  CHECK(tension > 85 && tension < 110);
  CHECK(y1 > 1.6f && y1 < 1.9f);
  CHECK(cw_node_get_prop(cw_node_get(root, "Rope1"), "hash", &v) && v.i != 0);
  FOLD(y0);
  FOLD(y1);
  cw_tree_free(t);
}

/* cw_cloth_step_many steps what a loop of cw_cloth_step steps, bit for bit: a flag, a rope holding a
 * crate, a rope towing a second crate through the first's cloth-pushed ground, and a two-way cloth
 * on a falling ball (impulses on dynamic bodies are applied in list order either way).
 * Control: one tick fewer must change the hashes. Nothing here folds into unit.checksum. */
static void scene_hashes(bool many, int ticks, uint32_t *out, int *nout, float *bodies) {
  cw_world3d w = world();
  static_box(w, (t3_vec3){ 0, -10.5f, 0 }, (t3_vec3){ 20, 0.5f, 5 });
  cw_cloth cl[4];
  cl[0] = make_flag(w, 0, 1.02f, 32);
  cw_rope_def rd = cw_rope_default_def();
  rd.start = (t3_vec3){ 4, 5, 0 }, rd.end = (t3_vec3){ 4, 2, 0 }, rd.segments = 16, rd.mass = 0.5f;
  cl[1] = cw_rope_create(w, &rd);
  cw_body3d b1 = crate(w, (t3_vec3){ 4, 1.75f, 0 }, 20);
  cw_cloth_attach_body(cl[1], 16, b1, (t3_vec3){ 0, 0.25f, 0 });
  rd.start = (t3_vec3){ 6, 5, 0 }, rd.end = (t3_vec3){ 6, 2, 0 };
  cl[2] = cw_rope_create(w, &rd);
  cw_body3d b2 = crate(w, (t3_vec3){ 6, 1.75f, 0 }, 10);
  cw_cloth_attach_body(cl[2], 16, b2, (t3_vec3){ 0, 0.25f, 0 });
  cw_cloth_def d = cw_cloth_default_def();
  d.two_way = true;
  d.mass = 0.5f;
  cl[3] = cw_cloth_create_grid(w, &d, 24, 24, (t3_vec3){ 7, 3, -1 }, (t3_vec3){ 2, 0, 0 }, (t3_vec3){ 0, 0, 2 });
  cw_body3d_def bd = cw_body3d_default_def();
  bd.position = (t3_vec3){ 8, 5, 0 };
  cw_body3d ball = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_sphere(ball, &sd, (t3_vec3){ 0, 0, 0 }, 0.3f);
  for (int t = 0; t < ticks; t++) {
    if (t == 90) cw_body3d_set_velocity(b1, (t3_vec3){ 3, 0, 1 });
    cw_world3d_step(w, DT);
    if (many) cw_cloth_step_many(cl, 4, DT);
    else for (int i = 0; i < 4; i++) cw_cloth_step(cl[i], DT);
  }
  *nout = 4;
  for (int i = 0; i < 4; i++) out[i] = cw_cloth_hash(cl[i]);
  t3_vec3 p1 = cw_body3d_transform(b1).p, p2 = cw_body3d_transform(b2).p, p3 = cw_body3d_transform(ball).p;
  bodies[0] = p1.x, bodies[1] = p1.y, bodies[2] = p2.y, bodies[3] = p3.y, bodies[4] = p3.x;
  cw_world3d_destroy(w);
}

static void test_step_many(void) {
  uint32_t a[4], b[4], c[4];
  float ba[5], bb[5], bc[5];
  int n;
  scene_hashes(false, 240, a, &n, ba);
  scene_hashes(true, 240, b, &n, bb);
  scene_hashes(true, 239, c, &n, bc);
  for (int i = 0; i < 4; i++) CHECK_EQ_INT(a[i], b[i]);
  CHECK(memcmp(ba, bb, sizeof ba) == 0);
  bool differs = false;
  for (int i = 0; i < 4; i++) differs |= a[i] != c[i];
  CHECK(differs); /* control: a tick fewer is a different state */
  CHECK(fabsf(ba[0] - 4) > 0.5f); /* the shoved crate swung */
  CHECK(cw_cloth_hash((cw_cloth){ 0 }) == 0);
  cw_cloth_step_many(NULL, 0, DT);
  cw_cloth dup[2];
  cw_world3d w = world();
  dup[0] = dup[1] = make_flag(w, 0, 1.02f, 16);
  cw_cloth_step_many(dup, 2, DT); /* listed twice: steps twice */
  CHECK_EQ_INT(cw_cloth_stats_get(dup[0]).steps, 2);
  cw_world3d_destroy(w);
}

/* microseconds per tick of `count` flags of n x n particles at 8 substeps, wind and two pinned
 * corners: each stepped alone (cw_cloth_step in a loop), or all in one cw_cloth_step_many. workers = 1
 * keeps a cloth (and a batch of them) on the calling thread. Logged, not compared. */
static double perf_scene(int n, int count, int workers, bool many) {
  cw_world3d w = world();
  cw_cloth cl[8];
  for (int k = 0; k < count; k++) {
    cl[k] = make_flag(w, workers, 1.02f, n);
    cw_cloth_set_wind(cl[k], (t3_vec3){ 6, 0, 1.5f }, 1, 0.3f, 0.6f);
  }
  const int WARM = 20, RUN = 60;
  double best = 1e30;
  for (int rep = 0; rep < 3; rep++) { /* the best of three runs: the least disturbed */
    for (int i = 0; i < (rep ? 0 : WARM); i++) {
      cw_world3d_step(w, DT);
      if (many) cw_cloth_step_many(cl, count, DT);
      else for (int k = 0; k < count; k++) cw_cloth_step(cl[k], DT);
    }
    double total = 0;
    for (int i = 0; i < RUN; i++) {
      cw_world3d_step(w, DT);
      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      if (many) cw_cloth_step_many(cl, count, DT);
      else for (int k = 0; k < count; k++) cw_cloth_step(cl[k], DT);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      total += ((double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9) * 1e6;
    }
    if (total / RUN < best) best = total / RUN;
  }
  uint32_t hh = 0;
  for (int k = 0; k < count; k++) hh ^= cw_cloth_hash(cl[k]);
  (void)hh;
  cw_world3d_destroy(w);
  return best;
}

static void test_perf(void) {
  static const int sizes[] = { 16, 32, 64 };
  cw_info("cloth cost, microseconds per tick (8 substeps; best of 3 runs of 60 ticks; cloths x particles):");
  for (int s = 0; s < 3; s++)
    for (int cnt = 1; cnt <= 8; cnt *= 8) {
      int n = sizes[s];
      double one = perf_scene(n, cnt, 1, false), pool = perf_scene(n, cnt, 0, false), many = perf_scene(n, cnt, 0, true);
      cw_info("cloth cost: %d x %dx%d: one thread %.0f, pool %.0f, pool with cw_cloth_step_many %.0f", cnt, n, n, one, pool, many);
    }
  CHECK(1);
}

/* the geometries cw_cloth_geometry_new and cw_rope_geometry_new hand out own their attributes alone: a geometry
 * released frees every buffer (a second reference left on each attribute leaked them all, every rebuild) */
static void attr_refs(t3_geometry *g) {
  int slots[] = { T3_ATTR_POSITION, T3_ATTR_NORMAL, T3_ATTR_UV };
  for (int i = 0; i < 3; i++) if (g->attributes[slots[i]]) CHECK_EQ_INT(g->attributes[slots[i]]->_refs, 1);
  CHECK(g->index != NULL);
  if (g->index) CHECK_EQ_INT(g->index->_refs, 1);
}
static void test_geometry_refs(void) {
  cw_world3d w = world();
  cw_cloth c = make_flag(w, 0, 1.02f, 8);
  t3_geometry *g = cw_cloth_geometry_new(c);
  CHECK(g != NULL);
  if (g) { CHECK(g->attributes[T3_ATTR_POSITION] != NULL); attr_refs(g); t3_release(g); }
  cw_rope_def rd = cw_rope_default_def();
  rd.start = (t3_vec3){ 0, 5, 0 };
  rd.end = (t3_vec3){ 0, 2, 0 };
  rd.segments = 8;
  cw_cloth r = cw_rope_create(w, &rd);
  t3_geometry *rg = cw_rope_geometry_new(r, 0.03f, 6);
  CHECK(rg != NULL);
  if (rg) { attr_refs(rg); t3_release(rg); }
  cw_world3d_destroy(w);
}

CW_TEST_SUITE("cloth", { "flag", test_flag }, { "stretch", test_stretch }, { "sphere", test_sphere },
              { "tunnel", test_tunnel }, { "box", test_box }, { "rope_crate", test_rope_crate },
              { "rope_two_bodies", test_rope_two_bodies }, { "skin", test_skin }, { "rollback", test_rollback },
              { "chain", test_chain }, { "nodes", test_nodes }, { "step_many", test_step_many }, { "bad_input", test_bad_input }, { "geometry_refs", test_geometry_refs }, { "perf", test_perf })

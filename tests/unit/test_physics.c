/* Physics wrapper unit suite: queries in both dimensions (ray closest / all
 * / batched, shape cast, overlap, aabb), layer and sensor filtering, ignore
 * body and callback filters, mesh and chain shapes, joint breaking, the
 * event list, contact impulses, interpolation pairs and both movers on
 * simple courses. Every numeric result is folded into unit.checksum, which
 * the gate compares across hosts. */
#include "cartwheel/physics.h"
#include "test.h"

#define PHASE 80 /* frames at 3 m/s: x 0 -> 4 -> 8 */
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static void fold_hit3(const cw_ray_hit3d *h) {
  FOLD(h->shape.id);
  FOLD(h->point);
  FOLD(h->normal);
  FOLD(h->fraction);
  FOLD(h->material);
}
static void fold_hit2(const cw_ray_hit2d *h) {
  FOLD(h->shape.id);
  FOLD(h->point);
  FOLD(h->normal);
  FOLD(h->fraction);
  FOLD(h->material);
}

/* ── 3D ──────────────────────────────────────────────────────────────── */

typedef struct scene3 {
  cw_world3d w;
  cw_body3d ground, box, ball, trigger, mesh_body;
  cw_shape3d ground_s, box_s, ball_s, trigger_s, mesh_s;
  cw_mesh3d mesh;
} scene3;

static scene3 make3(void) {
  scene3 s = { 0 };
  cw_world3d_def wd = cw_world3d_default_def();
  wd.gravity = (t3_vec3){ 0, 0, 0 };
  wd.layers[0] = "world";
  wd.layers[1] = "props";
  wd.layers[2] = "triggers";
  s.w = cw_world3d_create(&wd);
  cw_body3d_def bd = cw_body3d_default_def();
  cw_shape3d_def sd = cw_shape3d_default_def();

  bd.type = CW_BODY_STATIC;
  bd.position = (t3_vec3){ 0, -0.5f, 0 };
  s.ground = cw_body3d_create(s.w, &bd);
  sd.layer = cw_world3d_layer(s.w, "world");
  sd.material.user_material = 7;
  s.ground_s = cw_shape3d_box(s.ground, &sd, (t3_vec3){ 20, 0.5f, 20 }, cw_xform3_identity());

  bd.type = CW_BODY_DYNAMIC;
  bd.position = (t3_vec3){ 0, 2, 0 };
  s.box = cw_body3d_create(s.w, &bd);
  sd.layer = cw_world3d_layer(s.w, "props");
  sd.material.user_material = 11;
  s.box_s = cw_shape3d_box(s.box, &sd, (t3_vec3){ 0.5f, 0.5f, 0.5f }, cw_xform3_identity());

  bd.position = (t3_vec3){ 3, 1, 0 };
  s.ball = cw_body3d_create(s.w, &bd);
  s.ball_s = cw_shape3d_sphere(s.ball, &sd, (t3_vec3){ 0, 0, 0 }, 0.5f);

  bd.type = CW_BODY_STATIC;
  bd.position = (t3_vec3){ -3, 1, 0 };
  s.trigger = cw_body3d_create(s.w, &bd);
  cw_shape3d_def trig = sd;
  trig.sensor = true;
  trig.layer = cw_world3d_layer(s.w, "triggers");
  s.trigger_s = cw_shape3d_box(s.trigger, &trig, (t3_vec3){ 0.5f, 0.5f, 0.5f }, cw_xform3_identity());

  /* a static mesh quad (2 triangles) at y = 5 over x 10..12, z -1..1 */
  static const t3_vec3 verts[4] = { { 10, 5, -1 }, { 12, 5, -1 }, { 12, 5, 1 }, { 10, 5, 1 } };
  static const int32_t idx[6] = { 0, 2, 1, 0, 3, 2 };
  s.mesh = cw_mesh3d_create(verts, 4, idx, 2);
  bd.position = (t3_vec3){ 0, 0, 0 };
  s.mesh_body = cw_body3d_create(s.w, &bd);
  sd.layer = cw_world3d_layer(s.w, "world");
  s.mesh_s = cw_shape3d_mesh(s.mesh_body, &sd, s.mesh, (t3_vec3){ 1, 1, 1 });
  cw_world3d_step(s.w, 1.0f / 60); /* zero gravity: nothing moves */
  return s;
}

static void free3(scene3 *s) {
  cw_world3d_destroy(s->w);
  cw_mesh3d_destroy(s->mesh);
}

static void test_ray3(void) {
  scene3 s = make3();
  cw_ray_hit3d h;
  CHECK(cw_world3d_raycast(s.w, (t3_vec3){ 0, 10, 0 }, (t3_vec3){ 0, -20, 0 }, NULL, &h));
  CHECK(h.shape.id == s.box_s.id && h.body.id == s.box.id);
  CHECK(NEAR(h.fraction, 0.375f, 1e-4f) && NEAR(h.point.y, 2.5f, 1e-3f) && NEAR(h.normal.y, 1, 1e-5f));
  CHECK_EQ_INT(h.material, 11);
  fold_hit3(&h);

  cw_ray_hit3d all[8];
  int n = cw_world3d_raycast_all(s.w, (t3_vec3){ 0, 10, 0 }, (t3_vec3){ 0, -20, 0 }, NULL, all, 8);
  CHECK_EQ_INT(n, 2);
  CHECK(all[0].shape.id == s.box_s.id && all[1].shape.id == s.ground_s.id);
  CHECK(all[0].fraction < all[1].fraction && NEAR(all[1].fraction, 0.5f, 1e-4f));
  CHECK_EQ_INT(all[1].material, 7);
  for (int i = 0; i < n; i++) fold_hit3(&all[i]);
  CHECK_EQ_INT(cw_world3d_raycast_all(s.w, (t3_vec3){ 0, 10, 0 }, (t3_vec3){ 0, -20, 0 }, NULL, all, 1), 1);
  CHECK(all[0].shape.id == s.box_s.id); /* cap keeps the closest */

  /* layers: only "world" */
  cw_phys_filter f = cw_phys_filter_layers(cw_world3d_layer_bits(s.w, "world"));
  CHECK(cw_world3d_raycast(s.w, (t3_vec3){ 0, 10, 0 }, (t3_vec3){ 0, -20, 0 }, &f, &h) && h.shape.id == s.ground_s.id);
  /* ignore the box's body */
  f = cw_phys_filter_all();
  f.ignore_body = s.box.id;
  CHECK(cw_world3d_raycast(s.w, (t3_vec3){ 0, 10, 0 }, (t3_vec3){ 0, -20, 0 }, &f, &h) && h.shape.id == s.ground_s.id);
  fold_hit3(&h);
  /* sensors: invisible by default, reported on request */
  CHECK(cw_world3d_raycast(s.w, (t3_vec3){ -3, 10, 0 }, (t3_vec3){ 0, -20, 0 }, NULL, &h) && h.shape.id == s.ground_s.id);
  f = cw_phys_filter_all();
  f.sensors = true;
  CHECK(cw_world3d_raycast(s.w, (t3_vec3){ -3, 10, 0 }, (t3_vec3){ 0, -20, 0 }, &f, &h) && h.shape.id == s.trigger_s.id);
  CHECK(NEAR(h.fraction, 0.425f, 1e-4f));
  fold_hit3(&h);
  /* miss */
  CHECK(!cw_world3d_raycast(s.w, (t3_vec3){ 0, 10, 0 }, (t3_vec3){ 0, 5, 0 }, NULL, &h) && h.shape.id == 0);
  /* mesh: hit from above, the triangle index is reported */
  CHECK(cw_world3d_raycast(s.w, (t3_vec3){ 11, 8, 0.5f }, (t3_vec3){ 0, -6, 0 }, NULL, &h));
  CHECK(h.shape.id == s.mesh_s.id && NEAR(h.fraction, 0.5f, 1e-4f) && h.triangle >= 0);
  fold_hit3(&h);
  free3(&s);
}

static bool skip_balls(uint64_t shape, void *ctx) { return shape != ((scene3 *)ctx)->ball_s.id; }

static void test_shapes3(void) {
  scene3 s = make3();
  cw_qshape3d q = cw_qshape3d_sphere((t3_vec3){ 3, 10, 0 }, 0.5f);
  cw_ray_hit3d h[4];
  int n = cw_world3d_shapecast(s.w, &q, (t3_vec3){ 0, -20, 0 }, NULL, h, 1);
  CHECK_EQ_INT(n, 1);
  CHECK(h[0].shape.id == s.ball_s.id && NEAR(h[0].fraction, 0.4f, 2e-3f));
  fold_hit3(&h[0]);
  /* callback filter drops the ball: the ground is next */
  cw_phys_filter f = cw_phys_filter_all();
  f.fn = skip_balls;
  f.ctx = &s;
  n = cw_world3d_shapecast(s.w, &q, (t3_vec3){ 0, -20, 0 }, &f, h, 4);
  CHECK(n >= 1 && h[0].shape.id == s.ground_s.id);
  /* box sweep along x hits the box side */
  cw_qshape3d bq = cw_qshape3d_box((t3_vec3){ -6, 2, 0 }, (t3_vec3){ 0.25f, 0.25f, 0.25f }, (t3_quat){ 0, 0, 0, 1 });
  n = cw_world3d_shapecast(s.w, &bq, (t3_vec3){ 12, 0, 0 }, NULL, h, 4);
  CHECK(n >= 1 && h[0].shape.id == s.box_s.id && NEAR(h[0].point.x, -0.5f, 2e-2f));
  for (int i = 0; i < n; i++) fold_hit3(&h[i]);

  cw_shape3d out[8];
  cw_qshape3d ob = cw_qshape3d_box((t3_vec3){ 0, 2, 0 }, (t3_vec3){ 0.6f, 0.6f, 0.6f }, (t3_quat){ 0, 0, 0, 1 });
  n = cw_world3d_overlap(s.w, &ob, NULL, out, 8);
  CHECK_EQ_INT(n, 1);
  CHECK(out[0].id == s.box_s.id);
  cw_qshape3d os = cw_qshape3d_sphere((t3_vec3){ -3, 1, 0 }, 0.2f);
  CHECK_EQ_INT(cw_world3d_overlap(s.w, &os, NULL, out, 8), 0);
  f = cw_phys_filter_all();
  f.sensors = true;
  CHECK_EQ_INT(cw_world3d_overlap(s.w, &os, &f, out, 8), 1);
  n = cw_world3d_overlap_aabb(s.w, (t3_vec3){ -50, -50, -50 }, (t3_vec3){ 50, 50, 50 }, NULL, out, 8);
  CHECK_EQ_INT(n, 4); /* ground, box, ball, mesh */
  for (int i = 1; i < n; i++) CHECK(out[i - 1].id < out[i].id);
  for (int i = 0; i < n; i++) FOLD(out[i].id);
  CHECK_EQ_INT(cw_world3d_overlap_aabb(s.w, (t3_vec3){ -50, -50, -50 }, (t3_vec3){ 50, 50, 50 }, &f, out, 8), 5);
  CHECK_EQ_INT(cw_world3d_overlap_aabb(s.w, (t3_vec3){ -50, -50, -50 }, (t3_vec3){ 50, 50, 50 }, &f, out, 2), 2);
  free3(&s);
}

static void test_batch3(void) {
  scene3 s = make3();
  enum { N = 256 };
  static cw_ray3d rays[N];
  static cw_ray_hit3d hb[N];
  for (int i = 0; i < N; i++) {
    float x = -8 + (float)(i % 16), z = -8 + (float)(i / 16);
    rays[i] = (cw_ray3d){ { x * 0.5f, 10, z * 0.5f }, { 0, -20, 0 } };
  }
  int hits = cw_world3d_raycast_batch(s.w, rays, N, NULL, hb);
  int same = 0, nh = 0;
  for (int i = 0; i < N; i++) {
    cw_ray_hit3d h;
    nh += cw_world3d_raycast(s.w, rays[i].origin, rays[i].translation, NULL, &h);
    same += h.shape.id == hb[i].shape.id && h.fraction == hb[i].fraction;
    fold_hit3(&hb[i]);
  }
  CHECK_EQ_INT(hits, nh);
  CHECK_EQ_INT(same, N);
  CHECK_EQ_INT(hits, N); /* the ground is under every ray */
  free3(&s);
}

static void test_joints_events3(void) {
  cw_world3d_def wd = cw_world3d_default_def();
  cw_world3d w = cw_world3d_create(&wd);
  cw_body3d_def bd = cw_body3d_default_def();
  cw_shape3d_def sd = cw_shape3d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body3d ground = cw_body3d_create(w, &bd);
  cw_shape3d_box(ground, &sd, (t3_vec3){ 10, 0.5f, 10 }, cw_xform3_identity());
  bd.type = CW_BODY_DYNAMIC;
  bd.position = (t3_vec3){ 0, 3, 0 };
  cw_body3d hang = cw_body3d_create(w, &bd);
  cw_shape3d_box(hang, &sd, (t3_vec3){ 0.25f, 0.25f, 0.25f }, cw_xform3_identity());
  cw_joint3d_def jd = cw_joint3d_default_def(CW_JOINT_SPHERICAL);
  jd.b = hang; /* a = null: the world */
  cw_joint3d_def_anchor(&jd, (t3_vec3){ 0, 3.5f, 0 }, (t3_vec3){ 0, 1, 0 });
  jd.break_force = 0.5f; /* weight 125 * 10 N: breaks at once */
  jd.user = (void *)0x1234;
  cw_joint3d j = cw_joint3d_create(w, &jd);
  CHECK(cw_joint3d_valid(j));
  int broke = 0, begins = 0, steps = 0;
  t3_vec3 prev = cw_body3d_transform(hang).p;
  for (; steps < 120; steps++) {
    cw_world3d_step(w, 1.0f / 60);
    int n;
    const cw_phys_event3d *ev = cw_world3d_events(w, &n);
    for (int i = 0; i < n; i++) {
      if (ev[i].type == CW_PHYS_JOINT_BREAK && ev[i].joint.id == j.id && ev[i].user == (void *)0x1234) broke++;
      if (ev[i].type == CW_PHYS_CONTACT_BEGIN) begins++;
    }
    if (steps == 10) {
      /* interpolation pair: prev/cur of the last step, alpha between */
      cw_xform3 a = cw_body3d_interpolated(hang, 0), b = cw_body3d_interpolated(hang, 1),
                m = cw_body3d_interpolated(hang, 0.5f);
      CHECK(a.p.y > b.p.y && NEAR(m.p.y, (a.p.y + b.p.y) * 0.5f, 1e-5f));
      CHECK(NEAR(b.p.y, cw_body3d_transform(hang).p.y, 0));
      CHECK(prev.y > b.p.y);
    }
  }
  CHECK_EQ_INT(broke, 1);
  CHECK(!cw_joint3d_valid(j));
  CHECK(begins >= 1);
  cw_phys_stats st = cw_world3d_stats(w);
  CHECK_EQ_INT(st.steps, 120);
  CHECK_EQ_INT(st.bodies, 3); /* ground, box, the internal world anchor */
  FOLD(st.hash);
  t3_vec3 rest = cw_body3d_transform(hang).p;
  CHECK(NEAR(rest.y, 0.75f, 0.02f));
  FOLD(rest);
  cw_world3d_destroy(w);
}

/* cw_world{2,3}d_tick_begin: a world stepped several times a tick
 * interpolates over the whole tick (the render's alpha), not its last step */
static void test_tick_span2(void) {
  cw_world2d w = cw_world2d_create(NULL);
  cw_body2d_def bd = cw_body2d_default_def();
  cw_shape2d_def sd = cw_shape2d_default_def();
  bd.position = (cw_pos2){ 0, 10 };
  bd.linear_velocity = (cw_vec2){ 6, 0 };
  cw_body2d b = cw_body2d_create(w, &bd);
  cw_shape2d_box(b, &sd, (cw_vec2){ 0.25f, 0.25f }, cw_xform2_identity());
  cw_world2d_step(w, 1.0f / 180);
  cw_vec2 before = cw_body2d_transform(b).p;
  cw_world2d_step(w, 1.0f / 180);
  CHECK(NEAR(cw_body2d_interpolated(b, 0).p.x, before.x, 0)); /* by default: the last step's pair */
  for (int tick = 0; tick < 3; tick++) {
    cw_world2d_tick_begin(w);
    cw_vec2 start = cw_body2d_transform(b).p;
    for (int i = 0; i < 3; i++) cw_world2d_step(w, 1.0f / 180);
    cw_vec2 end = cw_body2d_transform(b).p, a = cw_body2d_interpolated(b, 0).p, m = cw_body2d_interpolated(b, 0.5f).p;
    CHECK(NEAR(a.x, start.x, 0) && NEAR(a.y, start.y, 0));
    CHECK(NEAR(cw_body2d_interpolated(b, 1).p.x, end.x, 0));
    CHECK(NEAR(m.x, (start.x + end.x) * 0.5f, 1e-5f) && end.x - start.x > 0.09f);
  }
  /* a tick with no step: nothing moved, the body stands at its pose */
  cw_world2d_tick_begin(w);
  CHECK(NEAR(cw_body2d_interpolated(b, 0.3f).p.x, cw_body2d_transform(b).p.x, 0));
  /* a teleport inside the tick: no interpolation from the old place */
  cw_world2d_tick_begin(w);
  cw_world2d_step(w, 1.0f / 180);
  cw_body2d_set_transform(b, (cw_xform2){ { 50, 10 }, { 1, 0 } });
  cw_world2d_step(w, 1.0f / 180);
  CHECK(NEAR(cw_body2d_interpolated(b, 0).p.x, 50, 0));
  cw_world2d_destroy(w);
}

static void test_tick_span3(void) {
  cw_world3d w = cw_world3d_create(NULL);
  cw_body3d_def bd = cw_body3d_default_def();
  cw_shape3d_def sd = cw_shape3d_default_def();
  bd.position = (t3_vec3){ 0, 10, 0 };
  bd.linear_velocity = (t3_vec3){ 6, 0, 0 };
  cw_body3d b = cw_body3d_create(w, &bd);
  cw_shape3d_box(b, &sd, (t3_vec3){ 0.25f, 0.25f, 0.25f }, cw_xform3_identity());
  for (int tick = 0; tick < 3; tick++) {
    cw_world3d_tick_begin(w);
    t3_vec3 start = cw_body3d_transform(b).p;
    for (int i = 0; i < 3; i++) cw_world3d_step(w, 1.0f / 180);
    t3_vec3 end = cw_body3d_transform(b).p, a = cw_body3d_interpolated(b, 0).p, m = cw_body3d_interpolated(b, 0.5f).p;
    CHECK(NEAR(a.x, start.x, 0) && NEAR(a.y, start.y, 0));
    CHECK(NEAR(cw_body3d_interpolated(b, 1).p.x, end.x, 0));
    CHECK(NEAR(m.x, (start.x + end.x) * 0.5f, 1e-5f) && end.x - start.x > 0.09f);
  }
  cw_world3d_tick_begin(w);
  CHECK(NEAR(cw_body3d_interpolated(b, 0.3f).p.x, cw_body3d_transform(b).p.x, 0));
  cw_world3d_destroy(w);
}

static void test_mover3(void) {
  cw_world3d w = cw_world3d_create(NULL);
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body3d g = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_box(g, &sd, (t3_vec3){ 20, 0.5f, 20 }, cw_xform3_at(0, -0.5f, 0));
  cw_shape3d_box(g, &sd, (t3_vec3){ 5, 0.1f, 5 }, cw_xform3_at(8, 0.1f, 0));    /* 0.2 m ledge from x = 3: ridden */
  cw_shape3d_box(g, &sd, (t3_vec3){ 3, 0.275f, 5 }, cw_xform3_at(9, 0.275f, 0)); /* +0.35 m from x = 6: stepped */
  cw_shape3d_box(g, &sd, (t3_vec3){ 0.5f, 3, 5 }, cw_xform3_at(-4, 3, 0));  /* a wall at x = -3.5 */
  cw_mover3d m;
  cw_mover3d_def md = cw_mover3d_default_def();
  md.step_height = 0.4f;
  cw_mover3d_init(&m, w, &md, (t3_vec3){ 0, 0.5f, 0 });
  /* fall and land */
  for (int i = 0; i < 60; i++) {
    m.velocity.y -= 10.0f / 60;
    cw_mover3d_move(&m, 1.0f / 60);
  }
  CHECK(m.on_floor && NEAR(m.position.y, 0, 1e-3f));
  /* walk onto the ledges */
  int stepped = 0;
  for (int i = 0; i < PHASE; i++) {
    m.velocity.x = 3;
    m.velocity.y -= 10.0f / 60;
    cw_mover3d_move(&m, 1.0f / 60);
    stepped += m.stepped;
  }
  CHECK(stepped == 0 && m.on_floor && m.position.x > 3.5f && NEAR(m.position.y, 0.2f, 1e-3f));
  for (int i = 0; i < PHASE; i++) {
    m.velocity.x = 3;
    m.velocity.y -= 10.0f / 60;
    cw_mover3d_move(&m, 1.0f / 60);
    stepped += m.stepped;
  }
  CHECK(stepped >= 1 && m.on_floor && m.position.x > 7 && NEAR(m.position.y, 0.55f, 1e-3f));
  FOLD(m.position);
  /* walk back into the wall */
  int walled = 0;
  for (int i = 0; i < 240; i++) {
    m.velocity.x = -4;
    m.velocity.y -= 10.0f / 60;
    cw_mover3d_move(&m, 1.0f / 60);
    walled += m.on_wall;
  }
  CHECK(walled > 0 && m.on_wall && NEAR(m.position.x, -3.5f + 0.3f, 0.01f) && m.on_floor && NEAR(m.position.y, 0, 1e-3f));
  CHECK(NEAR(m.velocity.x, 0, 1e-3f));
  FOLD(m.position);
  cw_world3d_destroy(w);
}

/* ── 2D ──────────────────────────────────────────────────────────────── */

typedef struct scene2 {
  cw_world2d w;
  cw_body2d ground, box, ball, trigger;
  cw_shape2d ground_s, box_s, ball_s, trigger_s;
  cw_chain2d chain;
} scene2;

static scene2 make2(void) {
  scene2 s = { 0 };
  cw_world2d_def wd = cw_world2d_default_def();
  wd.gravity = (cw_vec2){ 0, 0 };
  wd.layers[0] = "world";
  wd.layers[1] = "props";
  wd.layers[2] = "triggers";
  s.w = cw_world2d_create(&wd);
  cw_body2d_def bd = cw_body2d_default_def();
  cw_shape2d_def sd = cw_shape2d_default_def();
  bd.type = CW_BODY_STATIC;
  bd.position = (cw_pos2){ 0, -0.5f };
  s.ground = cw_body2d_create(s.w, &bd);
  sd.layer = cw_world2d_layer(s.w, "world");
  sd.material.user_material = 7;
  s.ground_s = cw_shape2d_box(s.ground, &sd, (cw_vec2){ 20, 0.5f }, cw_xform2_identity());
  /* a chain (terrain) up at y = 5 over x 10..12 */
  static const cw_vec2 pts[4] = { { 13, 5 }, { 12, 5 }, { 10, 5 }, { 9, 5 } };
  bd.position = (cw_pos2){ 0, 0 };
  s.chain = cw_chain2d_create(cw_body2d_create(s.w, &bd), &sd, pts, 4, false);

  bd.type = CW_BODY_DYNAMIC;
  bd.position = (cw_pos2){ 0, 2 };
  s.box = cw_body2d_create(s.w, &bd);
  sd.layer = cw_world2d_layer(s.w, "props");
  sd.material.user_material = 11;
  s.box_s = cw_shape2d_box(s.box, &sd, (cw_vec2){ 0.5f, 0.5f }, cw_xform2_identity());
  bd.position = (cw_pos2){ 3, 1 };
  s.ball = cw_body2d_create(s.w, &bd);
  s.ball_s = cw_shape2d_circle(s.ball, &sd, (cw_vec2){ 0, 0 }, 0.5f);
  bd.type = CW_BODY_STATIC;
  bd.position = (cw_pos2){ -3, 1 };
  s.trigger = cw_body2d_create(s.w, &bd);
  cw_shape2d_def trig = sd;
  trig.sensor = true;
  trig.layer = cw_world2d_layer(s.w, "triggers");
  s.trigger_s = cw_shape2d_box(s.trigger, &trig, (cw_vec2){ 0.5f, 0.5f }, cw_xform2_identity());
  cw_world2d_step(s.w, 1.0f / 60);
  return s;
}

static void test_ray2(void) {
  scene2 s = make2();
  cw_ray_hit2d h;
  CHECK(cw_world2d_raycast(s.w, (cw_pos2){ 0, 10 }, (cw_vec2){ 0, -20 }, NULL, &h));
  CHECK(h.shape.id == s.box_s.id && h.body.id == s.box.id && NEAR(h.fraction, 0.375f, 1e-4f) && NEAR(h.normal.y, 1, 1e-5f));
  CHECK_EQ_INT(h.material, 11);
  fold_hit2(&h);
  cw_ray_hit2d all[8];
  int n = cw_world2d_raycast_all(s.w, (cw_pos2){ 0, 10 }, (cw_vec2){ 0, -20 }, NULL, all, 8);
  CHECK_EQ_INT(n, 2);
  CHECK(all[0].shape.id == s.box_s.id && all[1].shape.id == s.ground_s.id && NEAR(all[1].fraction, 0.5f, 1e-4f));
  for (int i = 0; i < n; i++) fold_hit2(&all[i]);
  cw_phys_filter f = cw_phys_filter_layers(cw_world2d_layer_bits(s.w, "world"));
  CHECK(cw_world2d_raycast(s.w, (cw_pos2){ 0, 10 }, (cw_vec2){ 0, -20 }, &f, &h) && h.shape.id == s.ground_s.id);
  f = cw_phys_filter_all();
  f.ignore_body = s.box.id;
  CHECK(cw_world2d_raycast(s.w, (cw_pos2){ 0, 10 }, (cw_vec2){ 0, -20 }, &f, &h) && h.shape.id == s.ground_s.id);
  CHECK(cw_world2d_raycast(s.w, (cw_pos2){ -3, 10 }, (cw_vec2){ 0, -20 }, NULL, &h) && h.shape.id == s.ground_s.id);
  f = cw_phys_filter_all();
  f.sensors = true;
  CHECK(cw_world2d_raycast(s.w, (cw_pos2){ -3, 10 }, (cw_vec2){ 0, -20 }, &f, &h) && h.shape.id == s.trigger_s.id);
  CHECK(NEAR(h.fraction, 0.425f, 1e-4f));
  fold_hit2(&h);
  /* chain: solid from above (points run right to left) */
  CHECK(cw_world2d_raycast(s.w, (cw_pos2){ 11, 8 }, (cw_vec2){ 0, -6 }, NULL, &h) && NEAR(h.fraction, 0.5f, 1e-4f));
  CHECK(h.body.id != s.ground.id && h.body.id != 0 && NEAR(h.normal.y, 1, 1e-5f));
  fold_hit2(&h);
  cw_world2d_destroy(s.w);
}

static void test_shapes2(void) {
  scene2 s = make2();
  cw_qshape2d q = cw_qshape2d_circle((cw_vec2){ 3, 10 }, 0.5f);
  cw_ray_hit2d h[4];
  int n = cw_world2d_shapecast(s.w, &q, (cw_vec2){ 0, -20 }, NULL, h, 1);
  CHECK_EQ_INT(n, 1);
  CHECK(h[0].shape.id == s.ball_s.id && NEAR(h[0].fraction, 0.4f, 2e-3f));
  fold_hit2(&h[0]);
  cw_qshape2d bq = cw_qshape2d_box((cw_vec2){ -6, 2 }, (cw_vec2){ 0.25f, 0.25f }, (cw_rot2){ 1, 0 });
  n = cw_world2d_shapecast(s.w, &bq, (cw_vec2){ 12, 0 }, NULL, h, 4);
  CHECK(n >= 1 && h[0].shape.id == s.box_s.id && NEAR(h[0].point.x, -0.5f, 2e-2f));
  for (int i = 0; i < n; i++) fold_hit2(&h[i]);
  cw_shape2d out[8];
  cw_qshape2d ob = cw_qshape2d_box((cw_vec2){ 0, 2 }, (cw_vec2){ 0.6f, 0.6f }, (cw_rot2){ 1, 0 });
  CHECK_EQ_INT(cw_world2d_overlap(s.w, &ob, NULL, out, 8), 1);
  CHECK(out[0].id == s.box_s.id);
  cw_qshape2d os = cw_qshape2d_circle((cw_vec2){ -3, 1 }, 0.2f);
  CHECK_EQ_INT(cw_world2d_overlap(s.w, &os, NULL, out, 8), 0);
  cw_phys_filter f = cw_phys_filter_all();
  f.sensors = true;
  CHECK_EQ_INT(cw_world2d_overlap(s.w, &os, &f, out, 8), 1);
  n = cw_world2d_overlap_aabb(s.w, (cw_pos2){ -50, -50 }, (cw_pos2){ 50, 50 }, NULL, out, 8);
  CHECK_EQ_INT(n, 4); /* ground, the chain's one real segment (the end points are ghosts), box, ball */
  for (int i = 1; i < n; i++) CHECK(out[i - 1].id < out[i].id);
  for (int i = 0; i < n; i++) FOLD(out[i].id);
  /* batch == single */
  enum { N = 128 };
  static cw_ray2d rays[N];
  static cw_ray_hit2d hb[N];
  for (int i = 0; i < N; i++) rays[i] = (cw_ray2d){ { -8 + 0.13f * (float)i, 10 }, { 0, -20 } };
  int hits = cw_world2d_raycast_batch(s.w, rays, N, NULL, hb);
  int same = 0;
  for (int i = 0; i < N; i++) {
    cw_ray_hit2d one;
    cw_world2d_raycast(s.w, rays[i].origin, rays[i].translation, NULL, &one);
    same += one.shape.id == hb[i].shape.id && one.fraction == hb[i].fraction;
    fold_hit2(&hb[i]);
  }
  CHECK_EQ_INT(same, N);
  CHECK_EQ_INT(hits, N);
  cw_world2d_destroy(s.w);
}

static void test_joints_events2(void) {
  cw_world2d w = cw_world2d_create(NULL);
  cw_body2d_def bd = cw_body2d_default_def();
  cw_shape2d_def sd = cw_shape2d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body2d ground = cw_body2d_create(w, &bd);
  cw_shape2d_box(ground, &sd, (cw_vec2){ 10, 0.5f }, cw_xform2_identity());
  bd.type = CW_BODY_DYNAMIC;
  bd.position = (cw_pos2){ 0, 3 };
  cw_body2d hang = cw_body2d_create(w, &bd);
  cw_shape2d_box(hang, &sd, (cw_vec2){ 0.25f, 0.25f }, cw_xform2_identity());
  cw_joint2d_def jd = cw_joint2d_default_def(CW_JOINT_REVOLUTE);
  jd.b = hang;
  cw_joint2d_def_anchor(&jd, (cw_pos2){ 0, 3.5f }, (cw_vec2){ 1, 0 });
  jd.break_force = 0.5f;
  cw_joint2d j = cw_joint2d_create(w, &jd);
  CHECK(cw_joint2d_valid(j));
  CHECK_EQ_INT(cw_joint2d_create(w, &(cw_joint2d_def){ .type = CW_JOINT_SPHERICAL, .b = hang }).id, 0);
  int broke = 0, begins = 0;
  for (int s = 0; s < 120; s++) {
    cw_world2d_step(w, 1.0f / 60);
    int n;
    const cw_phys_event2d *ev = cw_world2d_events(w, &n);
    for (int i = 0; i < n; i++) {
      broke += ev[i].type == CW_PHYS_JOINT_BREAK && ev[i].joint.id == j.id;
      begins += ev[i].type == CW_PHYS_CONTACT_BEGIN;
    }
  }
  CHECK_EQ_INT(broke, 1);
  CHECK(!cw_joint2d_valid(j));
  CHECK(begins >= 1);
  cw_vec2 rest = cw_body2d_transform(hang).p;
  CHECK(NEAR(rest.y, 0.75f, 0.02f));
  FOLD(rest);
  cw_phys_stats st = cw_world2d_stats(w);
  FOLD(st.hash);
  cw_world2d_destroy(w);
}

/* Box2D v3.2 large worlds: positions are doubles, so a scene 524 km out behaves as one at the origin (a float
 * position there has 6 cm steps; the checks below are millimetres) */
static void test_large_world2(void) {
  const double X = 524288.3;
  CHECK(b2IsDoublePrecision());
  cw_world2d w = cw_world2d_create(NULL);
  cw_body2d_def bd = cw_body2d_default_def();
  bd.type = CW_BODY_STATIC;
  bd.position = (cw_pos2){ X, 0 };
  cw_body2d ground = cw_body2d_create(w, &bd);
  cw_shape2d_def sd = cw_shape2d_default_def();
  cw_shape2d_box(ground, &sd, (cw_vec2){ 20, 0.5f }, cw_xform2_at(0, -0.5f));
  bd = cw_body2d_default_def();
  bd.position = (cw_pos2){ X, 3 };
  cw_body2d box = cw_body2d_create(w, &bd);
  cw_shape2d_box(box, &sd, (cw_vec2){ 0.5f, 0.5f }, cw_xform2_identity());
  CHECK(cw_body2d_position(box).x == X); /* a float would hold 524288.3125 */
  for (int i = 0; i < 180; i++) cw_world2d_step(w, 1.0f / 60);
  cw_pos2 p = cw_body2d_position(box);
  CHECK(fabs(p.x - X) < 0.02 && fabs(p.y - 0.5) < 0.02); /* 3.2 lands a flat box with a little sideways drift */
  FOLD(p.y);
  cw_ray_hit2d h;
  CHECK(cw_world2d_raycast(w, (cw_pos2){ X, 10 }, (cw_vec2){ 0, -20 }, NULL, &h) && h.body.id == box.id);
  CHECK(fabs(h.point.x - X) < 1e-3 && fabs(h.point.y - 1.0) < 0.02);
  cw_shape2d out[4];
  CHECK_EQ_INT(cw_world2d_overlap_aabb(w, (cw_pos2){ X - 5, -1 }, (cw_pos2){ X + 5, 5 }, NULL, out, 4), 2);
  cw_qshape2d q = cw_qshape2d_at(cw_qshape2d_circle((cw_vec2){ 0, 0.5f }, 0.1f), (cw_pos2){ X, 0 });
  CHECK_EQ_INT(cw_world2d_overlap(w, &q, NULL, out, 4), 1);
  /* a mover stands and walks 3 m out there */
  cw_mover2d m;
  cw_mover2d_init(&m, w, NULL, (cw_pos2){ X - 8, 0.5f });
  for (int i = 0; i < 60; i++) {
    m.velocity.y -= 10.0f / 60;
    cw_mover2d_move(&m, 1.0f / 60);
  }
  CHECK(m.on_floor && fabs(m.position.y) < 1e-3 && fabs(m.position.x - (X - 8)) < 1e-9);
  for (int i = 0; i < 60; i++) {
    m.velocity.x = 3;
    m.velocity.y -= 10.0f / 60;
    cw_mover2d_move(&m, 1.0f / 60);
  }
  CHECK(m.on_floor && fabs(m.position.x - (X - 8 + 3)) < 0.01);
  FOLD(m.position.x);
  cw_world2d_destroy(w);
}

static void test_mover2(void) {
  cw_world2d w = cw_world2d_create(NULL);
  cw_body2d_def bd = cw_body2d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body2d g = cw_body2d_create(w, &bd);
  cw_shape2d_def sd = cw_shape2d_default_def();
  cw_shape2d_box(g, &sd, (cw_vec2){ 20, 0.5f }, cw_xform2_at(0, -0.5f));
  cw_shape2d_box(g, &sd, (cw_vec2){ 5, 0.1f }, cw_xform2_at(8, 0.1f));
  cw_shape2d_box(g, &sd, (cw_vec2){ 3, 0.275f }, cw_xform2_at(9, 0.275f));
  cw_shape2d_box(g, &sd, (cw_vec2){ 0.5f, 3 }, cw_xform2_at(-4, 3));
  /* a sensor in the way never blocks the mover */
  cw_shape2d_def trig = sd;
  trig.sensor = true;
  cw_shape2d_box(g, &trig, (cw_vec2){ 0.5f, 1 }, cw_xform2_at(1.5f, 1));
  cw_mover2d m;
  cw_mover2d_def md = cw_mover2d_default_def();
  md.step_height = 0.4f;
  cw_mover2d_init(&m, w, &md, (cw_pos2){ 0, 0.5f });
  for (int i = 0; i < 60; i++) {
    m.velocity.y -= 10.0f / 60;
    cw_mover2d_move(&m, 1.0f / 60);
  }
  CHECK(m.on_floor && NEAR(m.position.y, 0, 1e-3f));
  int stepped = 0;
  for (int i = 0; i < PHASE; i++) {
    m.velocity.x = 3;
    m.velocity.y -= 10.0f / 60;
    cw_mover2d_move(&m, 1.0f / 60);
    stepped += m.stepped;
  }
  CHECK(stepped == 0 && m.on_floor && m.position.x > 3.5f && NEAR(m.position.y, 0.2f, 1e-3f));
  for (int i = 0; i < PHASE; i++) {
    m.velocity.x = 3;
    m.velocity.y -= 10.0f / 60;
    cw_mover2d_move(&m, 1.0f / 60);
    stepped += m.stepped;
  }
  CHECK(stepped >= 1 && m.on_floor && m.position.x > 7 && NEAR(m.position.y, 0.55f, 1e-3f));
  FOLD(m.position);
  for (int i = 0; i < 240; i++) {
    m.velocity.x = -4;
    m.velocity.y -= 10.0f / 60;
    cw_mover2d_move(&m, 1.0f / 60);
  }
  CHECK(m.on_wall && m.on_floor && NEAR(m.position.x, -3.5f + 0.3f, 0.01f) && NEAR(m.position.y, 0, 1e-3f));
  FOLD(m.position);
  cw_world2d_destroy(w);
}

/* cw_body3d_contacts: a box dropped on two ground slabs reports both, sorted,
 * with the landing's approach speed, then rests on them */
static void test_contacts3(void) {
  cw_world3d w = cw_world3d_create(NULL);
  cw_body3d_def bd = cw_body3d_default_def();
  cw_shape3d_def sd = cw_shape3d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body3d ground = cw_body3d_create(w, &bd);
  cw_shape3d left = cw_shape3d_box(ground, &sd, (t3_vec3){ 1, 0.5f, 2 }, cw_xform3_at(-1, -0.5f, 0));
  cw_shape3d right = cw_shape3d_box(ground, &sd, (t3_vec3){ 1, 0.5f, 2 }, cw_xform3_at(1, -0.5f, 0));
  bd.type = CW_BODY_DYNAMIC;
  bd.position = (t3_vec3){ 0, 1.5f, 0 };
  cw_body3d box = cw_body3d_create(w, &bd);
  cw_shape3d bs = cw_shape3d_box(box, &sd, (t3_vec3){ 0.5f, 0.5f, 0.5f }, cw_xform3_identity());
  cw_contact3d c[4];
  float approach = 0;
  int n = 0;
  for (int i = 0; i < 120; i++) {
    cw_world3d_step(w, 1.0f / 60);
    n = cw_body3d_contacts(box, c, 4);
    for (int k = 0; k < n; k++)
      if (c[k].approach > approach) approach = c[k].approach;
  }
  CHECK_EQ_INT(n, 2);
  CHECK(approach > 3); /* a 1 m fall lands at about 4.5 m/s */
  CHECK(c[0].shape.id == bs.id && c[1].shape.id == bs.id);
  CHECK(c[0].other.id == ground.id);
  CHECK(c[0].other_shape.id < c[1].other_shape.id);
  CHECK((c[0].other_shape.id == left.id && c[1].other_shape.id == right.id) ||
        (c[0].other_shape.id == right.id && c[1].other_shape.id == left.id));
  float sum = 0;
  for (int k = 0; k < 2; k++) {
    CHECK(NEAR(c[k].normal.y, -1, 1e-3f)); /* from the box toward the ground */
    CHECK(NEAR(c[k].point.y, 0, 0.02f) && fabsf(c[k].point.x) > 0.1f && fabsf(c[k].point.x) < 0.55f);
    CHECK(c[k].approach < 0.05f);
    CHECK(fabsf(c[k].slide.x) + fabsf(c[k].slide.z) < 0.01f);
    sum += c[k].impulse;
    FOLD(c[k].point);
    FOLD(c[k].impulse);
  }
  CHECK(sum > 0);
  /* the ground sees the box from its side, the normal the other way */
  n = cw_body3d_contacts(ground, c, 4);
  CHECK_EQ_INT(n, 2);
  CHECK(c[0].other.id == box.id && NEAR(c[0].normal.y, 1, 1e-3f));
  CHECK_EQ_INT(cw_body3d_contacts(box, c, 1), 1); /* the cap keeps the first in order */
  CHECK_EQ_INT(cw_body3d_contacts((cw_body3d){ 0 }, c, 4), 0);
  cw_world3d_destroy(w);
}

/* found while writing the docs: wheel value = measured spin (as 3D), NULL joint defs,
 * the 2D mover's skin default (as 3D) */
static void test_findings2(void) {
  cw_world2d w = cw_world2d_create(NULL);
  cw_body2d_def bd = cw_body2d_default_def();
  bd.type = CW_BODY_DYNAMIC;
  bd.gravity_scale = 0;
  cw_body2d car = cw_body2d_create(w, &bd);
  cw_shape2d_def sd = cw_shape2d_default_def();
  cw_shape2d_box(car, &sd, (cw_vec2){ 1, 0.25f }, cw_xform2_identity());
  bd.position = (cw_pos2){ 0, -1 };
  cw_body2d wheel = cw_body2d_create(w, &bd);
  cw_shape2d_circle(wheel, &sd, (cw_vec2){ 0, 0 }, 0.4f);
  cw_joint2d_def jd = cw_joint2d_default_def(CW_JOINT_WHEEL);
  jd.a = car;
  jd.b = wheel;
  cw_joint2d_def_anchor(&jd, (cw_pos2){ 0, -1 }, (cw_vec2){ 0, 1 });
  jd.motor = false;
  jd.motor_speed = 7; /* a setting, not what the wheel does */
  cw_joint2d j = cw_joint2d_create(w, &jd);
  CHECK(cw_joint2d_valid(j));
  cw_body2d_set_angular_velocity(wheel, 2);
  CHECK(NEAR(cw_joint2d_value(j), 2, 1e-4f));
  cw_world2d_step(w, 1.0f / 60);
  float spin = cw_body2d_angular_velocity(wheel) - cw_body2d_angular_velocity(car);
  CHECK(NEAR(cw_joint2d_value(j), spin, 1e-4f) && spin > 1);
  CHECK_EQ_INT(cw_joint2d_create(w, NULL).id, 0);
  cw_mover2d m;
  cw_mover2d_def md = cw_mover2d_default_def();
  md.skin = 0;
  cw_mover2d_init(&m, w, &md, (cw_pos2){ 0, 5 });
  CHECK(m.def.skin == 0.02f);
  cw_world2d_destroy(w);
  cw_world3d w3 = cw_world3d_create(NULL);
  CHECK_EQ_INT(cw_joint3d_create(w3, NULL).id, 0);
  cw_world3d_destroy(w3);
}

CW_TEST_SUITE("physics", { "ray3", test_ray3 }, { "shapes3", test_shapes3 }, { "batch3", test_batch3 },
              { "joints_events3", test_joints_events3 }, { "mover3", test_mover3 }, { "contacts3", test_contacts3 }, { "ray2", test_ray2 },
              { "shapes2", test_shapes2 }, { "joints_events2", test_joints_events2 }, { "mover2", test_mover2 }, { "large_world2", test_large_world2 },
              { "tick_span2", test_tick_span2 }, { "tick_span3", test_tick_span3 },
              { "findings2", test_findings2 })

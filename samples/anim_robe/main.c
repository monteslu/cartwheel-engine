/* anim_robe: robes and long coats on SpringBone3D's skirt mode (cartwheel/ik.h). Two generated people
 * (samples/anim_robe/make-figures.mjs): one in a long robe on eight chains of four bones round the waist
 * (skirt = RING), one in a long coat open at the front on five chains of three bones (skirt = OPEN). Capsules on
 * the thighs and shins keep the garments off the legs. Both run at 5.4 m/s, turn, stop and turn on the spot.
 *
 * A fixed script, tick by tick (both people, side by side):
 *     0 ..  89  stand (the garments settle)
 *    90 .. 403  run: up to 5.4 m/s in 0.3 s, a half turn to the left at speed (240 .. 330), on, and a stop
 *               (380 .. 403)
 *   404 .. 559  stand
 *   560 .. 589  a half turn on the spot
 *   590 .. 719  stand
 *   720 .. 823  run again and stop (800 .. 823)
 *   824 .. 959  stand
 *
 * Debug fields (rb.*), per garment (robe, coat):
 *   pen        the deepest a leg (capsules placed from the bones, not the collision shapes: hip to knee 8.5 cm,
 *              knee to ankle 7 cm, ankle to toes 4.5 cm) goes through the garment's surface (the chains' joints and the
 *              surface between neighbouring chains, sampled 5 x 5 a panel), over the whole run (m; <= 0: the
 *              surface is outside every leg)
 *   pen_run, pen_stand, pen_turn   the same in the run (90 .. 403), at the stands, in the turn on the spot
 *   swing_run  the most a tip strayed from its animated place in the runs (m)
 *   settle     per stop (the run's, the turn's, the last run's): ticks until every tip stays within 1 cm of
 *              where it hangs at the end of that stand (999: never)
 *   jit_max, jit_rms   at rest (the last 48 ticks of each stand): each tip's offset less its 5-tick running
 *              mean, the largest and the root mean square (mm)
 *   hash_win   both models' pose hashes after each 60-tick window
 *   us         modifier phase microseconds per character (host clock, never compared); us_min the quietest tick
 * and the oks the gates expect at the end: legs_ok (pen_run, pen_stand, pen_turn <= 1 cm), settle_ok (every settle
 * <= 60), rest_ok (every jit_max <= 1 mm), flip_ok (swing_run <= 0.6 m).
 *
 * Test switches (cw_test_env, read at tick 2): ROBE_SKIRT=OFF (chains on their own: the old way), ROBE_NO_COLLIDE,
 * ROBE_CLOTH=1 (instead of the spring bones, each garment is a Cloth3D-style XPBD cloth (cartwheel/cloth.h) on
 * the chains' joints, pinned at the waist, against kinematic Box3D capsules on the legs: the measured
 * alternative; ROBE_CLOTH_DAMPING sets its damping), ROBE_CROWD=n (n people, robes and coats in turn, for the
 * cost). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/cloth.h"
#include "cartwheel/debug.h"
#include "cartwheel/ik.h"
#include "cartwheel/physics3d.h"
#include "cartwheel/profile.h"
#include "cartwheel/scene_file.h"

#define MAXC 16
#define NG 2       /* garments measured: the robe (person 0), the coat (person 1) */
#define MAXCH 8    /* chains a garment */
#define MAXL 5     /* joint rows a chain, its root's head first */
#define TICKS 960
#define RUN 5.4f   /* m/s */
#define NLEG 6     /* thighs, shins, feet */

typedef struct person {
  cw_node mover, model, anim, springs;
  t3_gltf *gltf;
  int kind; /* 0 robe, 1 coat */
  float x, z, yaw;
  bool running;
} person;

typedef struct garment {
  cw_node chain[MAXCH];
  char root[MAXCH][24];
  int nch, rows; /* chains; joint rows (the root's head and each tail) */
  bool closed;
  /* the Cloth3D alternative */
  cw_cloth cloth;
  cw_body3d leg[NLEG];
} garment;

static person P[MAXC];
static garment GA[NG];
static int crowd = 2;
static bool cloth_mode;
static cw_world3d cloth_world;
static cw_node level, camera;

/* debug state */
static int32_t tick_i, settle[NG * 3], legs_ok, settle_ok, rest_ok, flip_ok;
static float pen[NG], pen_run[NG], pen_stand[NG], pen_turn[NG], swing_run[NG], jit_max[NG * 3], jit_rms[NG * 3];
static float us, us_min = 1e9f, cloth_us;
static uint32_t hash_win[16];
static double us_sum, cloth_sum;
static int us_n;
static t3_vec3 tips[NG][MAXCH][TICKS];

static t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }
static t3_vec3 vadd(t3_vec3 a, t3_vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static t3_vec3 vmul(t3_vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float vdot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float vlen(t3_vec3 a) { return sqrtf(vdot(a, a)); }
static t3_vec3 vlerp(t3_vec3 a, t3_vec3 b, float t) { return vadd(a, vmul(vsub(b, a), t)); }

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

/* another person's SpringBone3D: the scene's, property for property */
static void copy_props(cw_node from, cw_node to) {
  for (const cw_node_type *t = cw_node_get_type(from); t && strcmp(t->name, "Node"); t = t->base)
    for (const cw_prop *p = t->props; p && p->name; p++) {
      cw_value v;
      if (!(p->flags & CW_PROP_STORED) || (p->flags & CW_PROP_READONLY) || !cw_node_get_prop(from, p->name, &v)) continue;
      cw_node_set(to, p->name, v);
    }
}
static cw_node copy_tree(cw_node from, cw_node parent) {
  cw_node n = add(parent, cw_node_get_type(from)->name, cw_node_name(from));
  copy_props(from, n);
  for (int i = 0; i < cw_node_child_count(from); i++) copy_tree(cw_node_child(from, i), n);
  return n;
}

static void set_shadows(t3_object *o) {
  if (o->type == T3_SKINNED_MESH || o->type == T3_MESH) o->cast_shadow = true;
  for (int i = 0; i < o->child_count; i++) set_shadows(o->children[i]);
}

static void load_person(int i, const void *glb, size_t len) {
  person *p = &P[i];
  char path[32];
  snprintf(path, sizeof path, "%s/Model", cw_node_name(p->mover));
  p->gltf = t3_gltf_parse(glb, len, NULL, NULL);
  cw_node3d_set_object(p->model, p->gltf->scene);
  set_shadows(p->gltf->scene);
  for (int k = 0; k < p->gltf->animation_count; k++) {
    const char *name = p->gltf->animations[k]->name;
    cw_anim a = cw_anim_from_t3_clip(p->gltf->animations[k], path);
    cw_anim_set_loop(a, CW_LOOP_LINEAR);
    cw_anim_player_add(p->anim, !strcmp(name, "Run") ? "run" : "idle", a);
    cw_anim_release(a);
  }
  p->x = -8;
  p->z = -1.8f * (float)i;
  p->yaw = 1.5707963f;
  cw_anim_player_play(p->anim, "idle", 0);
  if (i >= 2) cw_anim_player_seek(p->anim, 0.29f * (float)i, false); /* the crowd out of step */
}

static void make_person(int i) {
  char name[32];
  snprintf(name, sizeof name, "Mover%d", i);
  person *p = &P[i];
  p->kind = i & 1;
  p->mover = add(level, "Node3D", name);
  p->model = add(p->mover, "Node3D", "Model");
  p->springs = copy_tree(P[p->kind].springs, p->model);
  snprintf(name, sizeof name, "Anim%d", i);
  p->anim = add(level, "AnimationPlayer", name);
}

static void init(void *u) {
  (void)u;
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  camera = cw_node_get(level, "Camera");
  static const char *movers[NG] = { "Robed", "Coated" }, *nodes[NG] = { "Robe", "Coat" };
  for (int g = 0; g < NG; g++) {
    person *p = &P[g];
    p->kind = g;
    p->mover = cw_node_get(level, movers[g]);
    p->model = cw_node_get(p->mover, "Model");
    p->springs = cw_node_get(p->model, nodes[g]);
    p->anim = add(level, "AnimationPlayer", g ? "Anim1" : "Anim0");
    garment *G = &GA[g];
    for (int i = 0; i < cw_node_child_count(p->springs) && G->nch < MAXCH; i++) {
      cw_node ch = cw_node_child(p->springs, i);
      if (!cw_node_is(ch, "SpringBoneChain3D")) continue;
      cw_value v;
      cw_node_get_prop(ch, "root_bone", &v);
      snprintf(G->root[G->nch], sizeof G->root[0], "%s", v.s);
      G->chain[G->nch++] = ch;
    }
    G->rows = g ? 4 : 5;
    G->closed = g == 0;
  }
  for (int i = 0; i < NG * 3; i++) settle[i] = -1;
  cw_debug_i32("rb.tick", &tick_i);
  cw_debug_f32_array("rb.pen", pen, NG);
  cw_debug_f32_array("rb.pen_run", pen_run, NG);
  cw_debug_f32_array("rb.pen_stand", pen_stand, NG);
  cw_debug_f32_array("rb.pen_turn", pen_turn, NG);
  cw_debug_f32_array("rb.swing_run", swing_run, NG);
  cw_debug_field("rb.settle", settle, CW_DBG_I32, NG * 3);
  cw_debug_f32_array("rb.jit_max", jit_max, NG * 3);
  cw_debug_f32_array("rb.jit_rms", jit_rms, NG * 3);
  cw_debug_field("rb.hash_win", hash_win, CW_DBG_U32, 16);
  cw_debug_f32("rb.us", &us);         /* host clock: never compared */
  cw_debug_f32("rb.us_min", &us_min); /* the quietest tick */
  cw_debug_f32("rb.cloth_us", &cloth_us);
  cw_debug_i32("rb.legs_ok", &legs_ok);
  cw_debug_i32("rb.settle_ok", &settle_ok);
  cw_debug_i32("rb.rest_ok", &rest_ok);
  cw_debug_i32("rb.flip_ok", &flip_ok);
  for (int g = 0; g < NG; g++) pen[g] = pen_run[g] = pen_stand[g] = pen_turn[g] = -1;
}

/* #region cloth */
/* The measured alternative: each garment as an XPBD cloth (cartwheel/cloth.h) whose particles start on the chains'
 * joints, its top row pinned to the roots' heads, against kinematic Box3D capsules that follow the leg bones. */
/* the legs as the probe and the cloth see them, from the bones: thighs (8.5 cm), shins (7 cm), feet (4.5 cm) */
static void leg_capsule(int person_i, int leg, t3_vec3 *a, t3_vec3 *b, float *r) {
  static const char *up[2] = { "UpperLegL", "UpperLegR" }, *lo[2] = { "LowerLegL", "LowerLegR" }, *ft[2] = { "FootL", "FootR" },
                    *toe[2] = { "ToesL", "ToesR" };
  cw_node m = P[person_i].model;
  int side = leg & 1;
  t3_vec3 hip = cw_skeleton_bone_world(m, up[side]), knee = cw_skeleton_bone_world(m, lo[side]), ankle = cw_skeleton_bone_world(m, ft[side]);
  if (leg < 2) {
    *a = vlerp(hip, knee, 0.15f);
    *b = knee;
    *r = 0.085f;
  } else if (leg < 4) {
    *a = vlerp(knee, ankle, 0.05f);
    *b = ankle;
    *r = 0.07f;
  } else {
    *a = ankle;
    *b = cw_skeleton_bone_world(m, toe[side]);
    *r = 0.045f;
  }
}

static cw_xform3 capsule_xform(t3_vec3 a, t3_vec3 b) {
  t3_vec3 d = vsub(b, a);
  float l = vlen(d);
  cw_xform3 x = { vlerp(a, b, 0.5f), cw_quat_from_to(v3(0, 1, 0), l > 0 ? vmul(d, 1 / l) : v3(0, 1, 0)) };
  return x;
}

static t3_vec3 row_point(int g, int k, int row);

static void cloth_build(int g) {
  garment *G = &GA[g];
  if (!cloth_world.id) {
    cw_world3d_def wd = cw_world3d_default_def();
    wd.gravity = v3(0, -9.8f, 0);
    cloth_world = cw_world3d_create(&wd);
  }
  static t3_vec3 pos[MAXCH * MAXL];
  static float inv[MAXCH * MAXL];
  static uint32_t tri[MAXCH * MAXL * 6];
  int nc = G->nch, nv = 0, nt = 0;
  for (int r = 0; r < G->rows; r++)
    for (int k = 0; k < nc; k++) {
      pos[nv] = row_point(g, k, r);
      inv[nv++] = r ? 1.0f : 0.0f;
    }
  int ncol = G->closed ? nc : nc - 1;
  for (int r = 0; r + 1 < G->rows; r++)
    for (int k = 0; k < ncol; k++) {
      uint32_t a = (uint32_t)(r * nc + k), b = (uint32_t)(r * nc + (k + 1) % nc), c = b + (uint32_t)nc, d = a + (uint32_t)nc;
      tri[nt * 3 + 0] = a; tri[nt * 3 + 1] = b; tri[nt * 3 + 2] = c; nt++;
      tri[nt * 3 + 0] = a; tri[nt * 3 + 1] = c; tri[nt * 3 + 2] = d; nt++;
    }
  cw_cloth_def cd = cw_cloth_default_def();
  cd.positions = pos;
  cd.vertex_count = nv;
  cd.triangles = tri;
  cd.triangle_count = nt;
  cd.inv_mass = inv;
  cd.mass = g ? 1.4f : 1.6f;
  cd.thickness = 0.025f;
  cd.workers = 1;
  if (cw_test_env("ROBE_CLOTH_DAMPING")) cd.damping = (float)atof(cw_test_env("ROBE_CLOTH_DAMPING"));
  G->cloth = cw_cloth_create(cloth_world, &cd);
  for (int l = 0; l < NLEG; l++) {
    t3_vec3 a, b;
    float r;
    leg_capsule(g, l, &a, &b, &r);
    cw_body3d_def bd = cw_body3d_default_def();
    bd.type = CW_BODY_KINEMATIC;
    cw_xform3 x = capsule_xform(a, b);
    bd.position = x.p;
    bd.rotation = x.q;
    G->leg[l] = cw_body3d_create(cloth_world, &bd);
    cw_shape3d_def sd = cw_shape3d_default_def();
    float h = vlen(vsub(b, a)) * 0.5f;
    cw_shape3d_capsule(G->leg[l], &sd, v3(0, -h, 0), v3(0, h, 0), r);
  }
}

static void cloth_tick(float dt) {
  uint64_t t0 = cw_profile_now_ns();
  for (int g = 0; g < NG; g++) {
    garment *G = &GA[g];
    for (int l = 0; l < NLEG; l++) {
      t3_vec3 a, b;
      float r;
      leg_capsule(g, l, &a, &b, &r);
      cw_body3d_set_target(G->leg[l], capsule_xform(a, b), dt);
    }
  }
  cw_world3d_step(cloth_world, dt);
  for (int g = 0; g < NG; g++) {
    garment *G = &GA[g];
    for (int k = 0; k < G->nch; k++) cw_cloth_set_pin_target(G->cloth, k, cw_skeleton_bone_world(P[g].model, G->root[k]));
    cw_cloth_step(G->cloth, dt);
  }
  float now = (float)((cw_profile_now_ns() - t0) / 1000.0) / NG;
  cloth_sum += now;
  cloth_us = (float)(cloth_sum / (us_n + 1));
}
/* #endregion */

/* the garment's joint row `row` of chain k (0: the root's head), in the world */
static t3_vec3 row_point(int g, int k, int row) {
  garment *G = &GA[g];
  if (cloth_mode && G->cloth.id) {
    int n;
    const t3_vec3 *x = cw_cloth_positions(G->cloth, &n);
    return x[row * G->nch + k];
  }
  if (!row) return cw_skeleton_bone_world(P[g].model, G->root[k]);
  t3_vec3 j = v3(0, 0, 0);
  cw_spring_chain3d_joint_world(G->chain[k], row - 1, &j);
  return j;
}

/* #region probe */
/* the deepest a leg goes through the garment: its joints and the surface between neighbouring chains, 5 x 5 a panel */
static float penetration(int g) {
  garment *G = &GA[g];
  t3_vec3 la[NLEG], lb[NLEG];
  float lr[NLEG];
  for (int l = 0; l < NLEG; l++) leg_capsule(g, l, &la[l], &lb[l], &lr[l]);
  t3_vec3 X[MAXCH][MAXL];
  for (int k = 0; k < G->nch; k++)
    for (int r = 0; r < G->rows; r++) X[k][r] = row_point(g, k, r);
  float worst = -1;
  int ncol = G->closed ? G->nch : G->nch - 1;
  for (int k = 0; k < ncol; k++)
    for (int r = 0; r + 1 < G->rows; r++)
      for (int iu = 0; iu <= 4; iu++)
        for (int iv = 0; iv <= 4; iv++) {
          float u = (float)iu / 4, v = (float)iv / 4;
          int k2 = (k + 1) % G->nch;
          t3_vec3 q = vlerp(vlerp(X[k][r], X[k2][r], u), vlerp(X[k][r + 1], X[k2][r + 1], u), v);
          for (int l = 0; l < NLEG; l++) {
            t3_vec3 ab = vsub(lb[l], la[l]);
            float ll = vdot(ab, ab), t = ll > 0 ? vdot(vsub(q, la[l]), ab) / ll : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float d = lr[l] - vlen(vsub(q, vadd(la[l], vmul(ab, t))));
            if (d > worst) worst = d;
          }
        }
  return worst;
}
/* #endregion */

static t3_vec3 tip_offset(int g, int k) {
  garment *G = &GA[g];
  if (cloth_mode && G->cloth.id) {
    t3_vec3 anim = v3(0, 0, 0);
    cw_spring_chain3d_joint_world(G->chain[k], G->rows - 2, &anim);
    return vsub(row_point(g, k, G->rows - 1), anim);
  }
  cw_value v;
  if (!cw_node_get_prop(G->chain[k], "tip_offset", &v)) return v3(0, 0, 0);
  return v3(v.v[0], v.v[1], v.v[2]);
}

/* settle and rest jitter of garment g over the stand from tick s0 to s1 (inclusive), result slot w */
static void stand_stats(int g, int w, int s0, int s1) {
  garment *G = &GA[g];
  t3_vec3 hang[MAXCH];
  for (int k = 0; k < G->nch; k++) {
    t3_vec3 m = v3(0, 0, 0);
    for (int t = s1 - 29; t <= s1; t++) m = vadd(m, tips[g][k][t]);
    hang[k] = vmul(m, 1.0f / 30);
  }
  int last_far = s0 - 1;
  for (int t = s0; t <= s1; t++)
    for (int k = 0; k < G->nch; k++)
      if (vlen(vsub(tips[g][k][t], hang[k])) > 0.01f) last_far = t;
  settle[g * 3 + w] = last_far >= s1 - 30 ? 999 : last_far + 1 - s0;
  float mx = 0;
  double ss = 0;
  int n = 0;
  for (int t = s1 - 49; t <= s1 - 2; t++)
    for (int k = 0; k < G->nch; k++) {
      t3_vec3 m = v3(0, 0, 0);
      for (int j = -2; j <= 2; j++) m = vadd(m, tips[g][k][t + j]);
      float d = vlen(vsub(tips[g][k][t], vmul(m, 0.2f))) * 1000;
      if (d > mx) mx = d;
      ss += (double)d * d;
      n++;
    }
  jit_max[g * 3 + w] = mx;
  jit_rms[g * 3 + w] = (float)sqrt(ss / n);
}

static void probe(int t) {
  for (int g = 0; g < NG; g++) {
    float d = penetration(g);
    if (t > 2 && d > pen[g]) pen[g] = d;
    if (t >= 90 && t <= 403 && d > pen_run[g]) pen_run[g] = d;
    if (t >= 720 && t <= 823 && d > pen_run[g]) pen_run[g] = d;
    if (((t >= 404 && t < 560) || (t >= 590 && t < 720) || t >= 824) && d > pen_stand[g]) pen_stand[g] = d;
    if (t >= 560 && t < 620 && d > pen_turn[g]) pen_turn[g] = d;
    for (int k = 0; k < GA[g].nch; k++) {
      t3_vec3 o = tip_offset(g, k);
      if (t < TICKS) tips[g][k][t] = o;
      float s = vlen(o);
      if (((t >= 90 && t <= 403) || (t >= 720 && t <= 823)) && s > swing_run[g]) swing_run[g] = s;
    }
    if (t == 559) stand_stats(g, 0, 404, 559);
    if (t == 719) stand_stats(g, 1, 590, 719);
    if (t == 959) stand_stats(g, 2, 824, 959);
  }
  /* the bounds the gates hold (tests/robe-skirt.mjs): legs at most 1 cm through, settled within 60 ticks of each stop
   * with at most 1 mm of rest jitter, tips at most 0.6 m off their animated places in the runs */
  if (t == 959) {
    legs_ok = settle_ok = rest_ok = flip_ok = 1;
    for (int g = 0; g < NG; g++) {
      if (!(pen_run[g] <= 0.01f && pen_stand[g] <= 0.01f && pen_turn[g] <= 0.01f)) legs_ok = 0;
      if (!(swing_run[g] <= 0.6f)) flip_ok = 0;
      for (int w = 0; w < 3; w++) {
        if (settle[g * 3 + w] < 0 || settle[g * 3 + w] > 60) settle_ok = 0;
        if (!(jit_max[g * 3 + w] <= 1)) rest_ok = 0;
      }
    }
  }
}

static void start(void) {
  const char *e = cw_test_env("ROBE_CROWD");
  crowd = e ? atoi(e) : 2;
  if (crowd < 2) crowd = 2;
  if (crowd > MAXC) crowd = MAXC;
  for (int i = 2; i < crowd; i++) make_person(i);
  static const char *files[2] = { "robed.glb", "coated.glb" };
  for (int kind = 0; kind < 2; kind++) {
    size_t len;
    void *glb = cw_asset_load(files[kind], &len);
    if (!glb) { cw_error("anim_robe: %s missing from the assets", files[kind]); return; }
    for (int i = 0; i < crowd; i++)
      if (P[i].kind == kind) load_person(i, glb, len);
    free(glb);
  }
  cloth_mode = cw_test_env("ROBE_CLOTH") != NULL;
  const char *sk = cw_test_env("ROBE_SKIRT");
  for (int i = 0; i < crowd; i++) {
    if (sk) cw_node_set(P[i].springs, "skirt", cw_val_enum(sk));
    if (cw_test_env("ROBE_NO_COLLIDE")) cw_node_set(P[i].springs, "collisions", cw_val_bool(false));
    if (cloth_mode) { /* the spring bones only report the animated joints: held on the pose, no collisions */
      cw_node_set(P[i].springs, "skirt", cw_val_enum("OFF"));
      cw_node_set(P[i].springs, "collisions", cw_val_bool(false));
      for (int k = 0; k < cw_node_child_count(P[i].springs); k++) {
        cw_node c = cw_node_child(P[i].springs, k);
        if (cw_node_is(c, "SpringBoneChain3D")) {
          cw_node_set(c, "stiffness", cw_val_float(3600));
          cw_node_set(c, "gravity_strength", cw_val_float(0));
        }
      }
    }
  }
}

static void move(person *p, int t, float dt) {
  float pace = 0;
  if (t >= 90 && t < 404) pace = cw_smoothstep(90, 108, (float)t) * (1 - cw_smoothstep(380, 404, (float)t));
  if (t >= 720 && t < 824) pace = cw_smoothstep(720, 738, (float)t) * (1 - cw_smoothstep(800, 824, (float)t));
  bool run = pace > 0.3f;
  if (run != p->running) cw_anim_player_play(p->anim, run ? "run" : "idle", 0.15f);
  p->running = run;
  if (t >= 240 && t <= 330) p->yaw = 1.5707963f + 3.1415927f * cw_smoothstep(240, 330, (float)t);
  if (t >= 560 && t <= 590) p->yaw = 4.712389f + 3.1415927f * cw_smoothstep(560, 590, (float)t);
  float s, c;
  cw_sincos(p->yaw, &s, &c);
  p->x += s * RUN * pace * dt;
  p->z += c * RUN * pace * dt;
  cw_node3d_set_position(p->mover, v3(p->x, 0, p->z));
  cw_node3d_set_rotation(p->mover, v3(0, cw_wrap_angle(p->yaw), 0));
}

static void fixed(void *u, double dt) {
  (void)u;
  int t = (int)cw_tree_tick(cw_scene_app_tree());
  tick_i = t;
  if (t == 2) start();
  if (!P[0].gltf) return;
  /* this tick's modifiers already ran (after the nodes, before this callback) */
  if (cloth_mode) {
    if (t == 3)
      for (int g = 0; g < NG; g++) cloth_build(g);
    else if (t > 3)
      cloth_tick((float)dt);
  }
  if (t > 3) probe(t);
  if (t % 60 == 59 && t / 60 < 16) hash_win[t / 60] = cw_skeleton_hash(P[0].model) * 31u + cw_skeleton_hash(P[1].model);
  if (t > 30) {
    float now = 0;
    for (int i = 0; i < crowd; i++) now += cw_skeleton_last_us(P[i].model);
    now /= (float)crowd;
    us_sum += now;
    us_n++;
    us = (float)(us_sum / us_n);
    if (now < us_min) us_min = now;
  }
  for (int i = 0; i < crowd; i++) move(&P[i], t, (float)dt);
  /* the camera keeps both people in view from their left, easing after them */
  t3_vec3 mid = vlerp(v3(P[0].x, 0.9f, P[0].z), v3(P[1].x, 0.9f, P[1].z), 0.5f);
  t3_vec3 cp = cw_node3d_position(camera), want = vadd(mid, v3(0, 0.9f, 6.5f));
  float k = t < 4 ? 1 : 1 - cw_exp(-3 * (float)dt);
  cw_node3d_set_position(camera, vlerp(cp, want, k));
  cw_node3d_look_at(camera, mid, v3(0, 1, 0));
}

static void shutdown(void *u) {
  (void)u;
  for (int g = 0; g < NG; g++)
    if (GA[g].cloth.id) cw_cloth_destroy(GA[g].cloth);
  if (cloth_world.id) cw_world3d_destroy(cloth_world);
  for (int i = 0; i < crowd; i++)
    if (P[i].gltf) {
      t3_release(P[i].gltf);
      P[i].gltf = NULL;
    }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "anim_robe", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}

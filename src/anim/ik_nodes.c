/* Skeleton modifier nodes and the modifier phase (cartwheel/ik.h): after
 * every node's fixed_update and the physics step, each model with modifier
 * children captures its animated pose, runs the modifiers in child order and
 * flushes the pose back to its bones once. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "anim_internal.h"
#include "cartwheel/profile.h"
#include "cartwheel/debug.h"
#include "cartwheel/ik.h"
#include "cartwheel/jobs.h"
#include "cartwheel/physics3d.h"
#include "cartwheel/physics_nodes.h"

#define MAX_LEGS 8
#define MAX_MODS 32
#define PARALLEL_MIN 8 /* models before the solves go to the job pool */

/* ── small math ─────────────────────────────────────────────────────── */

static inline t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }
static inline t3_vec3 vadd(t3_vec3 a, t3_vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline t3_vec3 vmul(t3_vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vlen(t3_vec3 a) { return sqrtf(vdot(a, a)); }
static inline t3_vec3 vnorm(t3_vec3 a) {
  float l = vlen(a);
  return l > 0 ? vmul(a, 1 / l) : v3(0, 0, 0);
}
static inline t3_quat qmul(t3_quat a, t3_quat b) {
  return (t3_quat){ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static inline t3_quat qconj(t3_quat q) { return (t3_quat){ -q.x, -q.y, -q.z, q.w }; }
static inline t3_vec3 qrot(t3_quat q, t3_vec3 v) {
  float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
  return v3(v.x + q.w * tx + q.y * tz - q.z * ty, v.y + q.w * ty + q.z * tx - q.x * tz, v.z + q.w * tz + q.x * ty - q.y * tx);
}

/* ── per-model state ───────────────────────────────────────────────── */

typedef struct skel {
  cw_node model;           /* the node whose children are the modifiers */
  cw_node frame;           /* the Node3D whose space is model space (model, or a Skeleton3D's model) */
  t3_object *obj;          /* the object the pose is bound to */
  bool bound, bind_failed;
  uint32_t gen;            /* bumped on every bind: modifiers re-resolve bones */
  cw_pose pose;            /* the working pose */
  cw_pose scratch;         /* a modifier's input, for influence < 1 */
  t3_vec3 *anim_pos, *fl_pos;
  t3_quat *anim_rot, *fl_rot;
  t3_vec3 *anim_scl, *fl_scl;
  bool flushed;
  /* model <-> world (uniform scale) */
  t3_mat4 world;
  t3_quat world_rot;
  float world_scale;
  uint32_t hash;
  float last_us;
  cw_node mods[MAX_MODS];
  int nmods;
} skel;

typedef struct ikw {
  cw_tree *tree;
  CW_ARRAY(skel *) skels;
  double dt;
} ikw;

static const cw_node_type *mod_t, *two_t, *look_t, *ccd_t, *fab_t, *foot_t;

/* ── node data ─────────────────────────────────────────────────────── */

typedef struct modbase {
  bool active;
  float influence;
} modbase;

typedef struct target_ref {
  const char *target;
  t3_vec3 target_position;
} target_ref;

typedef struct two_bone {
  const char *root_bone, *middle_bone, *end_bone, *target;
  t3_vec3 target_position;
  const char *pole;
  t3_vec3 pole_position;
  bool use_pole;
  float soften;
  bool use_target_rotation;
  t3_quat target_rotation_offset;
  bool reached;
  /* resolved */
  uint32_t gen;
  int idx[3];
  t3_vec3 t_model, pole_model;
  t3_quat end_model;
} two_bone;

typedef struct look_at {
  const char *bone, *target;
  t3_vec3 target_position;
  t3_vec3 forward_axis, offset;
  float limit_angle;
  bool in_limit;
  uint32_t gen;
  int idx;
  t3_vec3 t_model;
} look_at;

typedef struct ccd_node {
  const char *root_bone, *end_bone, *target;
  t3_vec3 target_position;
  int32_t max_iterations;
  float min_angle, max_angle;
  cw_ccdik_link joints[CW_IK_MAX_CHAIN];
  bool joint_set[CW_IK_MAX_CHAIN];
  uint32_t gen;
  int end, nlinks;
  int links[CW_IK_MAX_CHAIN];
  t3_vec3 t_model;
} ccd_node;

typedef struct fab_node {
  const char *root_bone, *end_bone, *target;
  t3_vec3 target_position;
  int32_t max_iterations;
  float min_distance;
  cw_ik_joint joints[CW_IK_MAX_CHAIN];
  uint32_t gen;
  int chain[CW_IK_MAX_CHAIN], n;
  t3_vec3 t_model;
} fab_node;

typedef struct leg {
  int upper, lower, end, foot;
  float gh;          /* smoothed ground height (m, along world up) */
  float dy;          /* gh relative to the model origin, clamped */
  bool primed;
  cw_foot_state st;
  /* this tick */
  t3_vec3 ankle_model;  /* animated */
  float h_anim;         /* animated ankle height above the model's ground plane (m) */
  cw_ray3d ray;
} leg;

typedef struct foot_node {
  const char *legs, *pelvis;
  float ray_up, max_step_down, foot_height, max_slope, blend_speed;
  int32_t collision_mask;
  uint32_t gen;
  const char *parsed_legs;
  int nlegs, pelvis_idx;
  leg L[MAX_LEGS];
  float pelvis_off;
  bool pelvis_primed;
  t3_vec3 up_model;   /* world up in model space (unit) */
} foot_node;

static modbase *MB(cw_node n) { return cw_node_data_t(n, mod_t); }

/* ── world <-> model ───────────────────────────────────────────────── */

static void model_frame(skel *s) {
  const t3_mat4 *g = cw_node3d_global_transform(s->frame);
  if (g) s->world = *g;
  else t3_mat4_identity(&s->world);
  t3_quat q = { 0, 0, 0, 1 };
  for (cw_node n = s->frame; !CW_HANDLE_IS_NULL(n); n = cw_node_parent(n))
    if (cw_node_is(n, "Node3D")) q = qmul(cw_node3d_quaternion(n), q);
  s->world_rot = q;
  const float *e = s->world.e;
  s->world_scale = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  if (s->world_scale <= 0) s->world_scale = 1;
}
static t3_vec3 to_model(const skel *s, t3_vec3 w) {
  t3_vec3 d = vsub(w, v3(s->world.e[12], s->world.e[13], s->world.e[14]));
  return vmul(qrot(qconj(s->world_rot), d), 1 / s->world_scale);
}
static t3_vec3 dir_to_model(const skel *s, t3_vec3 w) { return qrot(qconj(s->world_rot), w); }
static t3_vec3 to_world(const skel *s, t3_vec3 m) {
  return vadd(v3(s->world.e[12], s->world.e[13], s->world.e[14]), qrot(s->world_rot, vmul(m, s->world_scale)));
}

/* a target node's world position (and rotation), or the fallback */
static bool target_world(cw_node mod, const char *path, t3_vec3 fallback, t3_vec3 *pos, t3_quat *rot) {
  *pos = fallback;
  if (rot) *rot = (t3_quat){ 0, 0, 0, 1 };
  if (!path || !path[0]) return true;
  cw_node t = cw_node_get(mod, path);
  if (!cw_node_valid(t) || !cw_node_is(t, "Node3D")) return false;
  const t3_mat4 *g = cw_node3d_global_transform(t);
  *pos = v3(g->e[12], g->e[13], g->e[14]);
  if (rot) {
    t3_quat q = { 0, 0, 0, 1 };
    for (cw_node n = t; !CW_HANDLE_IS_NULL(n); n = cw_node_parent(n))
      if (cw_node_is(n, "Node3D")) q = qmul(cw_node3d_quaternion(n), q);
    *rot = q;
  }
  return true;
}

static int bone_or_warn(const skel *s, cw_node mod, const char *prop, const char *name) {
  int i = cw_pose_find(&s->pose, name);
  if (i < 0 && name && name[0])
    cw_warn("%s '%s': %s '%s' is not a bone of '%s'", cw_node_get_type(mod)->name, cw_node_name(mod), prop, name,
            cw_node_name(s->model));
  return i;
}

/* rest-pose model rotation of a bone */
static t3_quat rest_mrot(const cw_pose *p, int i) {
  t3_quat q = { 0, 0, 0, 1 };
  int last = i;
  for (int b = i; b >= 0; b = p->parent[b]) {
    q = qmul(p->rest_rot[b], q);
    last = b;
  }
  return qmul(p->base_rot[last], q);
}

/* ── TwoBoneIK3D ───────────────────────────────────────────────────── */

static void two_prepare(cw_node n, two_bone *d, skel *s) {
  if (d->gen != s->gen) {
    d->gen = s->gen;
    d->idx[0] = bone_or_warn(s, n, "root_bone", d->root_bone);
    d->idx[1] = bone_or_warn(s, n, "middle_bone", d->middle_bone);
    d->idx[2] = bone_or_warn(s, n, "end_bone", d->end_bone);
  }
  t3_vec3 tp;
  t3_quat tr;
  target_world(n, d->target, d->target_position, &tp, &tr);
  d->t_model = to_model(s, tp);
  t3_quat er = qmul(tr, d->target_rotation_offset);
  d->end_model = qmul(qconj(s->world_rot), er);
  t3_vec3 pp;
  target_world(n, d->pole, d->pole_position, &pp, NULL);
  d->pole_model = to_model(s, pp);
}

static void two_solve(cw_node n, two_bone *d, skel *s) {
  (void)n;
  if (d->idx[0] < 0 || d->idx[1] < 0 || d->idx[2] < 0) return;
  cw_ik2_opts o = CW_IK2_OPTS_DEFAULT;
  o.use_pole = d->use_pole;
  o.pole = d->pole_model;
  o.soften = d->soften;
  o.use_end_rotation = d->use_target_rotation;
  o.end_rotation = d->end_model;
  d->reached = cw_ik_two_bone(&s->pose, d->idx[0], d->idx[1], d->idx[2], d->t_model, &o);
}

/* ── LookAtModifier3D ──────────────────────────────────────────────── */

static void look_prepare(cw_node n, look_at *d, skel *s) {
  if (d->gen != s->gen) {
    d->gen = s->gen;
    d->idx = bone_or_warn(s, n, "bone", d->bone);
  }
  t3_vec3 tp;
  target_world(n, d->target, d->target_position, &tp, NULL);
  d->t_model = to_model(s, tp);
}

static void look_solve(cw_node n, look_at *d, skel *s) {
  (void)n;
  if (d->idx < 0) return;
  cw_aim_opts o = CW_AIM_OPTS_DEFAULT;
  t3_quat rr = qconj(rest_mrot(&s->pose, d->idx));
  o.forward = vnorm(qrot(rr, d->forward_axis));
  const float *e = s->pose.model[d->idx].e;
  float bs = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
  o.offset = vmul(qrot(rr, d->offset), bs > 0 ? 1 / bs : 1);
  o.limit_angle = d->limit_angle;
  d->in_limit = cw_ik_aim(&s->pose, d->idx, d->t_model, &o);
}

/* ── CCDIK3D ───────────────────────────────────────────────────────── */

static void ccd_prepare(cw_node n, ccd_node *d, skel *s) {
  if (d->gen != s->gen) {
    d->gen = s->gen;
    int root = bone_or_warn(s, n, "root_bone", d->root_bone);
    d->end = bone_or_warn(s, n, "end_bone", d->end_bone);
    d->nlinks = 0;
    if (root >= 0 && d->end >= 0) {
      if (!cw_pose_is_descendant(&s->pose, d->end, root) || root == d->end)
        cw_warn("CCDIK3D '%s': end_bone '%s' is not below root_bone '%s'", cw_node_name(n), d->end_bone, d->root_bone);
      else
        for (int b = s->pose.parent[d->end]; b >= 0 && d->nlinks < CW_IK_MAX_CHAIN; b = s->pose.parent[b]) {
          d->links[d->nlinks++] = b;
          if (b == root) break;
        }
    }
  }
  t3_vec3 tp;
  target_world(n, d->target, d->target_position, &tp, NULL);
  d->t_model = to_model(s, tp);
}

static void ccd_solve(cw_node n, ccd_node *d, skel *s) {
  (void)n;
  if (d->nlinks <= 0) return;
  cw_ccdik_link links[CW_IK_MAX_CHAIN];
  for (int j = 0; j < d->nlinks; j++) {
    links[j] = d->joint_set[j] ? d->joints[j] : (cw_ccdik_link){ 0 };
    links[j].index = d->links[j];
  }
  cw_ccdik ik = { .target = -1, .target_position = d->t_model, .effector = d->end, .links = links,
                  .link_count = d->nlinks, .iteration = d->max_iterations,
                  .has_min_angle = d->min_angle > 0, .min_angle = d->min_angle,
                  .has_max_angle = d->max_angle > 0, .max_angle = d->max_angle };
  cw_ccdik_solve(&s->pose, &ik, 1, 1);
}

/* ── FABRIK3D ──────────────────────────────────────────────────────── */

static void fab_prepare(cw_node n, fab_node *d, skel *s) {
  if (d->gen != s->gen) {
    d->gen = s->gen;
    int root = bone_or_warn(s, n, "root_bone", d->root_bone);
    int end = bone_or_warn(s, n, "end_bone", d->end_bone);
    d->n = 0;
    if (root >= 0 && end >= 0) {
      int tmp[CW_IK_MAX_CHAIN], k = 0;
      int b = end;
      for (; b >= 0 && k < CW_IK_MAX_CHAIN; b = s->pose.parent[b]) {
        tmp[k++] = b;
        if (b == root) break;
      }
      if (b != root || k < 2)
        cw_warn("FABRIK3D '%s': end_bone '%s' is not below root_bone '%s'", cw_node_name(n), d->end_bone, d->root_bone);
      else
        for (int i = 0; i < k; i++) d->chain[i] = tmp[k - 1 - i];
      if (b == root && k >= 2) d->n = k;
    }
  }
  t3_vec3 tp;
  target_world(n, d->target, d->target_position, &tp, NULL);
  d->t_model = to_model(s, tp);
}

static void fab_solve(cw_node n, fab_node *d, skel *s) {
  (void)n;
  if (d->n < 2) return;
  cw_chain_opts o = CW_CHAIN_OPTS_DEFAULT;
  o.max_iterations = d->max_iterations;
  o.min_distance = d->min_distance / s->world_scale;
  o.joints = d->joints;
  cw_ik_fabrik(&s->pose, d->chain, d->n, d->t_model, &o);
}

/* ── FootPlacement3D ───────────────────────────────────────────────── */

static void foot_parse(cw_node n, foot_node *d, skel *s) {
  d->nlegs = 0;
  d->parsed_legs = d->legs;
  d->pelvis_idx = bone_or_warn(s, n, "pelvis", d->pelvis);
  const char *p = d->legs ? d->legs : "";
  while (*p && d->nlegs < MAX_LEGS) {
    char names[4][96] = { { 0 } };
    int k = 0;
    size_t len = 0;
    for (; *p && *p != ';'; p++) {
      if (*p == ',') { if (k < 3) k++; len = 0; continue; }
      if (*p == ' ' && len == 0) continue;
      if (len < sizeof names[0] - 1) { names[k][len++] = *p; names[k][len] = 0; }
    }
    if (*p == ';') p++;
    if (!names[0][0]) continue;
    leg *l = &d->L[d->nlegs];
    memset(l, 0, sizeof *l);
    l->upper = bone_or_warn(s, n, "legs", names[0]);
    l->lower = bone_or_warn(s, n, "legs", names[1]);
    l->end = bone_or_warn(s, n, "legs", names[2]);
    l->foot = names[3][0] ? bone_or_warn(s, n, "legs", names[3]) : -1;
    if (l->upper < 0 || l->lower < 0 || l->end < 0) {
      cw_warn("FootPlacement3D '%s': a leg is \"upper,lower,end[,foot]\" (bone names); legs separate with ';'", cw_node_name(n));
      continue;
    }
    d->nlegs++;
  }
}

/* serial: rays from the animated pose, one batch per node */
static void foot_prepare(cw_node n, foot_node *d, skel *s, cw_tree *tree, double dt) {
  if (d->gen != s->gen || d->parsed_legs != d->legs) {
    d->gen = s->gen;
    foot_parse(n, d, s);
  }
  if (!d->nlegs) return;
  t3_vec3 up = v3(0, 1, 0);
  d->up_model = vnorm(dir_to_model(s, up));
  t3_vec3 origin = to_world(s, v3(0, 0, 0));
  cw_ray3d rays[MAX_LEGS];
  for (int i = 0; i < d->nlegs; i++) {
    leg *l = &d->L[i];
    l->ankle_model = cw_pose_model_position(&s->pose, l->end);
    t3_vec3 aw = to_world(s, l->ankle_model);
    l->h_anim = vdot(vsub(aw, origin), up);
    float above = l->h_anim > 0 ? l->h_anim : 0;
    rays[i].origin = vadd(aw, vmul(up, d->ray_up));
    rays[i].translation = vmul(up, -(d->ray_up + above + d->max_step_down));
  }
  cw_ray_hit3d hits[MAX_LEGS];
  memset(hits, 0, sizeof hits);
  cw_world3d w = cw_physics3d_world(tree);
  cw_phys_filter f = cw_phys_filter_layers((uint32_t)d->collision_mask);
  if (cw_world3d_valid(w)) cw_world3d_raycast_batch(w, rays, d->nlegs, &f, hits);
  float k = d->blend_speed > 0 ? 1 - cw_exp(-d->blend_speed * (float)dt) : 1;
  float lowest = 0;
  for (int i = 0; i < d->nlegs; i++) {
    leg *l = &d->L[i];
    bool hit = !CW_HANDLE_IS_NULL(hits[i].shape);
    /* the ground height is smoothed in the world, so the model moving over
     * it (a body following stairs) adds no lag under a planted foot */
    float base = vdot(origin, up);
    float want = hit ? vdot(hits[i].point, up) : base;
    /* a foot on the ground follows it at once (exact contact); a swinging
     * foot eases toward the ground under it, so it does not pop mid-air */
    float swing = cw_smoothstep(d->foot_height, d->foot_height * 3, l->h_anim);
    float kl = 1 + (k - 1) * swing;
    if (!l->primed) { l->gh = want; l->primed = true; }
    else l->gh += (want - l->gh) * kl;
    l->dy = l->gh - base;
    if (l->dy < -d->max_step_down) l->dy = -d->max_step_down;
    if (l->dy > d->ray_up) l->dy = d->ray_up;
    l->st.hit = hit;
    l->st.ground = hit ? hits[i].point : v3(0, 0, 0);
    l->st.normal = hit ? hits[i].normal : up;
    l->st.offset = l->dy;
    l->st.lift = l->h_anim;
    if (l->dy < lowest) lowest = l->dy;
  }
  /* the pelvis eases (it carries the whole body) */
  if (!d->pelvis_primed) { d->pelvis_off = lowest; d->pelvis_primed = true; }
  else {
    /* down six times as fast as up: a leg must not run out of reach */
    float kp = lowest < d->pelvis_off && d->blend_speed > 0 ? 1 - cw_exp(-6 * d->blend_speed * (float)dt) : k;
    d->pelvis_off += (lowest - d->pelvis_off) * kp;
  }
}

static void foot_solve(cw_node n, foot_node *d, skel *s) {
  (void)n;
  if (!d->nlegs) return;
  cw_pose *p = &s->pose;
  float inv_scale = 1 / s->world_scale;
  if (d->pelvis_idx >= 0 && d->pelvis_off < 0) cw_pose_translate_model(p, d->pelvis_idx, vmul(d->up_model, d->pelvis_off * inv_scale));
  float cos_max, sin_max;
  cw_sincos(d->max_slope > 0 ? d->max_slope : 0, &sin_max, &cos_max);
  for (int i = 0; i < d->nlegs; i++) {
    leg *l = &d->L[i];
    t3_vec3 target = vadd(l->ankle_model, vmul(d->up_model, l->dy * inv_scale));
    cw_ik_two_bone(p, l->upper, l->lower, l->end, target, NULL);
    t3_vec3 end_now = cw_pose_model_position(p, l->end);
    int foot = l->foot;
    if (foot >= 0 && !cw_pose_is_descendant(p, foot, l->end)) {
      /* a foot bone outside the chain (an IK-rigged glTF): carry it with the ankle */
      cw_pose_translate_model(p, foot, vsub(end_now, l->ankle_model));
    }
    if (foot < 0) foot = l->end;
    /* align to the ground normal while the foot is near the ground */
    if (!l->st.hit || d->max_slope <= 0) continue;
    float h = l->h_anim;
    float w = 1 - cw_smoothstep(d->foot_height, d->foot_height * 2.5f, h);
    if (w <= 0) continue;
    t3_vec3 nm = vnorm(dir_to_model(s, l->st.normal));
    float c = vdot(nm, d->up_model);
    if (c < cos_max) {
      t3_vec3 side = vnorm(vsub(nm, vmul(d->up_model, c)));
      nm = vnorm(vadd(vmul(d->up_model, cos_max), vmul(side, sin_max)));
    }
    t3_quat r = cw_quat_from_to(d->up_model, nm);
    if (w < 1) {
      float o4[4], id[4] = { 0, 0, 0, 1 }, rq[4] = { r.x, r.y, r.z, r.w };
      cw_quat_slerp_det(o4, id, rq, w);
      r = (t3_quat){ o4[0], o4[1], o4[2], o4[3] };
    }
    cw_pose_rotate_model(p, foot, r);
  }
}

/* ── SpringBone3D ──────────────────────────────────────────────────────
 * Verlet on each joint's tail, kept in world space across ticks: the model
 * moving or turning leaves the tails behind, a spring pulls each one back
 * toward where the animated pose (below the already simulated parent) puts
 * it, gravity sags it, the bone length holds it, and collision shapes on
 * the body push it out. Each bone then turns by the shortest rotation onto
 * its simulated tail. */

#define SPRING_MAX_CHAINS 16
#define SPRING_MAX_SHAPES 16
#define SPRING_J CW_SPRING_MAX_JOINTS

typedef struct spring_chain {
  const char *root_bone, *end_bone;
  float stiffness, damping;
  t3_vec3 gravity;
  float gravity_strength, radius;
  t3_vec3 tip_offset;           /* read-only: the tip from its animated place (world, m) */
  float tip_distance;           /* read-only: its length */
  /* resolved */
  uint32_t gen;
  const char *r_root, *r_end;
  int n;                        /* simulated joints: bone[i] turns, its tail is tail[i] (or the extension) */
  int bone[SPRING_J], tail[SPRING_J];
  t3_vec3 ext_dir;              /* the end bone's tail when it has no child: in its frame, unit */
  float ext_len;                /* model units */
  /* state across ticks (world): each tail now and a tick ago, and where the animation put it a tick ago */
  t3_vec3 cur[SPRING_J], prev[SPRING_J], aprev[SPRING_J];
  bool primed;
  /* this tick (model space) */
  bool snap, step;
  t3_vec3 mcur[SPRING_J], mprev[SPRING_J], maprev[SPRING_J];
  t3_vec3 g;                    /* gravity times dt^2, model units */
  float keep, pull, r;
} spring_chain;

typedef struct spring_shape {
  const char *bone;
  int32_t shape;                /* 0 SPHERE, 1 CAPSULE */
  float radius;
  t3_vec3 offset, tail;         /* from the bone's origin, the model's rest orientation, metres */
  /* resolved */
  uint32_t gen;
  const char *r_bone;
  int idx;
  t3_vec3 off_local, tail_local; /* in the bone's frame, metres */
  /* this tick */
  t3_vec3 a, b;                 /* model space */
  float r;
  t3_vec3 wa, wb;               /* world, for cw_spring_collision3d_world */
  float wr;
  bool placed;
} spring_shape;

typedef struct spring_node {
  bool reset;                   /* set: start again from the animated pose next tick (clears itself) */
  float teleport_distance, teleport_angle;
  bool collisions;
  int32_t resets;               /* read-only */
  /* this tick */
  int nch, nsh;
  spring_chain *ch[SPRING_MAX_CHAINS];
  spring_shape *sh[SPRING_MAX_SHAPES];
  /* the model's world transform last tick */
  bool primed;
  uint64_t last_tick;
  t3_vec3 last_org;
  t3_quat last_rot;
  float last_scale;
} spring_node;

static const cw_node_type *spring_t, *chain_t, *shape_t;

static t3_vec3 to_model_with(t3_vec3 org, t3_quat rot, float scale, t3_vec3 w) {
  return vmul(qrot(qconj(rot), vsub(w, org)), 1 / scale);
}

static void chain_resolve(cw_node n, spring_chain *c, skel *s) {
  c->gen = s->gen;
  c->r_root = c->root_bone;
  c->r_end = c->end_bone;
  c->n = 0;
  c->primed = false;
  int root = bone_or_warn(s, n, "root_bone", c->root_bone);
  int end = bone_or_warn(s, n, "end_bone", c->end_bone);
  if (root < 0 || end < 0) return;
  const cw_pose *p = &s->pose;
  int tmp[SPRING_J], k = 0, b = end;
  for (; b >= 0 && k < SPRING_J; b = p->parent[b]) {
    tmp[k++] = b;
    if (b == root) break;
  }
  if (b != root) {
    cw_warn("SpringBoneChain3D '%s': end_bone '%s' is not below root_bone '%s' (or the chain is longer than %d bones)", cw_node_name(n),
            c->end_bone, c->root_bone, SPRING_J);
    return;
  }
  for (int i = 0; i < k; i++) c->bone[i] = tmp[k - 1 - i];
  for (int i = 0; i + 1 < k; i++) c->tail[i] = c->bone[i + 1];
  /* the end bone's tail: its first child (a glTF "_end" marker, the next bone), else its own length
   * again along the incoming direction, fixed in its frame */
  c->tail[k - 1] = -1;
  for (int j = end + 1; j < p->count; j++)
    if (p->parent[j] == end) { c->tail[k - 1] = j; break; }
  c->ext_len = 0;
  if (c->tail[k - 1] < 0) {
    int pa = p->parent[end];
    if (pa < 0) {
      cw_warn("SpringBoneChain3D '%s': end_bone '%s' has no child and no parent to give it a length", cw_node_name(n), c->end_bone);
      return;
    }
    t3_vec3 d = vsub(cw_pose_model_position(p, end), cw_pose_model_position(p, pa));
    c->ext_len = vlen(d);
    c->ext_dir = vnorm(qrot(qconj(p->mrot[end]), d));
  }
  c->n = k;
}

static void shape_resolve(cw_node n, spring_shape *h, skel *s) {
  h->gen = s->gen;
  h->r_bone = h->bone;
  h->idx = bone_or_warn(s, n, "bone", h->bone);
  if (h->idx < 0) return;
  t3_quat rr = qconj(rest_mrot(&s->pose, h->idx));
  h->off_local = qrot(rr, h->offset);
  h->tail_local = qrot(rr, h->tail);
}

/* the tail of joint i where the pose puts it now (model space) */
static t3_vec3 chain_tail(const cw_pose *p, const spring_chain *c, int i) {
  if (c->tail[i] >= 0) return cw_pose_model_position(p, c->tail[i]);
  int b = c->bone[i];
  return vadd(cw_pose_model_position(p, b), vmul(qrot(p->mrot[b], c->ext_dir), c->ext_len));
}

/* serial: gather the chains and shapes, resolve bones, carry the state into model space */
static void spring_prepare(cw_node n, spring_node *d, skel *s, cw_tree *tree, double dt) {
  d->nch = d->nsh = 0;
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node c = cw_node_child(n, i);
    spring_chain *ch = cw_node_data_t(c, chain_t);
    if (ch) {
      if (d->nch >= SPRING_MAX_CHAINS) continue;
      if (ch->gen != s->gen || ch->r_root != ch->root_bone || ch->r_end != ch->end_bone) chain_resolve(c, ch, s);
      d->ch[d->nch++] = ch;
      continue;
    }
    spring_shape *sh = cw_node_data_t(c, shape_t);
    if (sh && d->nsh < SPRING_MAX_SHAPES) {
      if (sh->gen != s->gen || sh->r_bone != sh->bone) shape_resolve(c, sh, s);
      d->sh[d->nsh++] = sh;
    }
  }
  uint64_t tick = cw_tree_tick(tree);
  t3_vec3 org = v3(s->world.e[12], s->world.e[13], s->world.e[14]);
  /* a tick or more without this node (inactive, just added), or a reset: start from the animated pose */
  bool snap = d->reset || !d->primed || (tick != d->last_tick && tick != d->last_tick + 1);
  bool step = tick != d->last_tick;
  /* a jump: the tails move with the model (no whip) */
  bool carry = false;
  if (!snap && step) {
    if (d->teleport_distance > 0 && vlen(vsub(org, d->last_org)) > d->teleport_distance) carry = true;
    if (d->teleport_angle > 0) {
      t3_quat a = d->last_rot, b = s->world_rot;
      float c = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
      if (c < 0) c = -c;
      if (c < cw_cos(d->teleport_angle * 0.5f)) carry = true;
    }
  }
  if ((snap && d->primed) || carry) d->resets++; /* (the first tick is a start, not a reset) */
  float fdt = (float)dt;
  for (int k = 0; k < d->nch; k++) {
    spring_chain *c = d->ch[k];
    c->snap = snap || !c->primed;
    c->step = step;
    if (!c->snap)
      for (int i = 0; i < c->n; i++) {
        if (carry) {
          c->mcur[i] = to_model_with(d->last_org, d->last_rot, d->last_scale, c->cur[i]);
          c->mprev[i] = to_model_with(d->last_org, d->last_rot, d->last_scale, c->prev[i]);
          c->maprev[i] = to_model_with(d->last_org, d->last_rot, d->last_scale, c->aprev[i]);
        } else {
          c->mcur[i] = to_model(s, c->cur[i]);
          c->mprev[i] = to_model(s, c->prev[i]);
          c->maprev[i] = to_model(s, c->aprev[i]);
        }
      }
    c->keep = c->damping > 0 ? cw_exp(-c->damping * fdt) : 1;
    c->pull = c->stiffness * fdt * fdt;
    if (c->pull > 1) c->pull = 1;
    if (c->pull < 0) c->pull = 0;
    t3_vec3 gd = vnorm(c->gravity);
    c->g = vmul(dir_to_model(s, gd), c->gravity_strength * fdt * fdt / s->world_scale);
    c->r = c->radius / s->world_scale;
  }
  d->primed = true;
  d->reset = false;
  d->last_tick = tick;
  d->last_org = org;
  d->last_rot = s->world_rot;
  d->last_scale = s->world_scale;
}

/* push x out of every shape by its radius plus r */
static t3_vec3 spring_collide(const spring_node *d, t3_vec3 x, float r) {
  for (int k = 0; k < d->nsh; k++) {
    const spring_shape *h = d->sh[k];
    if (!h->placed) continue;
    t3_vec3 c = h->a, ab = vsub(h->b, h->a);
    float ll = vdot(ab, ab);
    if (ll > 0) {
      float t = vdot(vsub(x, h->a), ab) / ll;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      c = vadd(h->a, vmul(ab, t));
    }
    t3_vec3 dv = vsub(x, c);
    float R = h->r + r, dd = vdot(dv, dv);
    if (dd >= R * R || dd <= 0) continue;
    x = vadd(c, vmul(dv, R / sqrtf(dd)));
  }
  return x;
}

static void spring_solve(cw_node n, spring_node *d, skel *s) {
  (void)n;
  cw_pose *p = &s->pose;
  for (int k = 0; k < d->nsh; k++) {
    spring_shape *h = d->sh[k];
    h->placed = h->idx >= 0;
    if (!h->placed) continue;
    t3_vec3 o = cw_pose_model_position(p, h->idx);
    t3_quat q = p->mrot[h->idx];
    float inv = 1 / s->world_scale;
    h->a = vadd(o, vmul(qrot(q, h->off_local), inv));
    h->b = h->shape == 1 ? vadd(o, vmul(qrot(q, h->tail_local), inv)) : h->a;
    h->r = h->radius * inv;
    h->wa = to_world(s, h->a);
    h->wb = to_world(s, h->b);
    h->wr = h->radius;
  }
  for (int k = 0; k < d->nch; k++) {
    spring_chain *c = d->ch[k];
    if (c->n <= 0) continue;
    t3_vec3 tip_in = chain_tail(p, c, c->n - 1);
    for (int i = 0; i < c->n; i++) {
      int b = c->bone[i];
      t3_vec3 head = cw_pose_model_position(p, b);
      t3_vec3 at = chain_tail(p, c, i); /* where the animation puts the tail below the simulated parent */
      t3_vec3 ad = vsub(at, head);
      float L = vlen(ad);
      if (L <= 1e-7f) { c->mcur[i] = c->mprev[i] = c->maprev[i] = at; continue; }
      t3_vec3 x;
      if (c->snap) {
        x = at;
        c->mprev[i] = at;
      } else if (c->step) {
        /* the tail keeps its velocity, less the drag on what it moves apart from where the animation
         * moves it (so a steady walk leaves no lag, a start, stop or turn does) */
        t3_vec3 cur = c->mcur[i], v = vsub(cur, c->mprev[i]), va = vsub(at, c->maprev[i]);
        t3_vec3 pred = vadd(cur, vadd(va, vmul(vsub(v, va), c->keep)));
        x = vadd(vadd(pred, vmul(vsub(at, pred), c->pull)), c->g);
        c->mprev[i] = cur;
      } else {
        x = c->mcur[i];
      }
      c->maprev[i] = at;
      for (int it = 0; it < 2; it++) {
        t3_vec3 dx = vsub(x, head);
        float l = vlen(dx);
        x = l > 0 ? vadd(head, vmul(dx, L / l)) : at;
        if (d->collisions && d->nsh) x = spring_collide(d, x, c->r);
      }
      t3_vec3 dx = vsub(x, head);
      float l = vlen(dx);
      x = l > 0 ? vadd(head, vmul(dx, L / l)) : at;
      c->mcur[i] = x;
      cw_pose_rotate_model(p, b, cw_quat_from_to(vmul(ad, 1 / L), vmul(vsub(x, head), 1 / L)));
    }
    t3_vec3 tip = c->mcur[c->n - 1];
    c->tip_offset = qrot(s->world_rot, vmul(vsub(tip, tip_in), s->world_scale));
    c->tip_distance = vlen(c->tip_offset);
    for (int i = 0; i < c->n; i++) {
      c->cur[i] = to_world(s, c->mcur[i]);
      c->prev[i] = to_world(s, c->mprev[i]);
      c->aprev[i] = to_world(s, c->maprev[i]);
    }
    c->snap = false;
    c->primed = true;
  }
}

/* ── the phase ─────────────────────────────────────────────────────── */

static void skel_arrays_free(skel *s) {
  void *a[] = { s->anim_pos, s->fl_pos, s->anim_rot, s->fl_rot, s->anim_scl, s->fl_scl };
  for (size_t i = 0; i < 6; i++) cw_free(a[i]);
  s->anim_pos = s->fl_pos = s->anim_scl = s->fl_scl = NULL;
  s->anim_rot = s->fl_rot = NULL;
}

static void skel_unbind(skel *s) {
  if (s->bound) {
    cw_pose_free(&s->pose);
    cw_pose_free(&s->scratch);
    skel_arrays_free(s);
  }
  s->bound = false;
  s->flushed = false;
}

static void skel_free(skel *s) {
  skel_unbind(s);
  cw_free(s);
}

/* The rest pose the modifiers measure against (LookAtModifier3D's forward_axis and offset, SpringBoneCollision3D's
 * offsets): the skin's bind pose, from its inverse bind matrices, whatever the bones hold when the modifiers bind
 * (a game that advances its AnimationTree before the first modifier phase would otherwise give its first animated
 * frame as rest). A bone no skin lists keeps its local transform from the bind under its parent's bind pose.
 * CW_TEST_IK_REST_CURRENT=1 (a gate's control): the transforms at bind time, as before. */
static void collect_skins(t3_object *o, t3_mat4 m, t3_object *root, t3_skinned_mesh **out, t3_mat4 *mm, int *n, int cap) {
  if (o != root) {
    t3_mat4 l;
    t3_mat4_compose(&l, o->position, o->quaternion, o->scale);
    t3_mat4_multiply(&m, &m, &l);
  }
  if (o->type == T3_SKINNED_MESH && *n < cap) {
    out[*n] = (t3_skinned_mesh *)o;
    mm[(*n)++] = m;
  }
  for (int i = 0; i < o->child_count; i++) collect_skins(o->children[i], m, root, out, mm, n, cap);
}
static void bind_rest(cw_pose *p, t3_object *root) {
  if (getenv("CW_TEST_IK_REST_CURRENT")) return; /* (native: the unit suite binds in its first frame) */
  enum { MAXSK = 16 };
  t3_skinned_mesh *sk[MAXSK];
  t3_mat4 skm[MAXSK], id;
  t3_mat4_identity(&id);
  int nsk = 0;
  collect_skins(root, id, root, sk, skm, &nsk, MAXSK);
  if (!nsk) return;
  t3_mat4 *bind = cw_calloc((size_t)p->count, sizeof(t3_mat4), CW_MEM_ANIM);
  bool *have = cw_calloc((size_t)p->count, sizeof(bool), CW_MEM_ANIM);
  for (int k = 0; k < nsk; k++) {
    const t3_skeleton *s = sk[k]->skeleton;
    if (!s || !s->bone_inverses) continue;
    /* bind world -> model: the mesh's model transform after the inverse of its bind matrix (the mesh's world when it
     * was bound; bind_matrix_inverse follows the mesh's current world in ATTACHED mode, so it is not used) */
    t3_mat4 to_model, binv;
    t3_mat4_invert(&binv, &sk[k]->bind_matrix);
    t3_mat4_multiply(&to_model, &skm[k], &binv);
    for (int j = 0; j < s->bone_count; j++)
      for (int b = 0; b < p->count; b++)
        if (p->bone[b] == s->bones[j] && !have[b]) {
          t3_mat4 inv;
          t3_mat4_invert(&inv, &s->bone_inverses[j]);
          t3_mat4_multiply(&bind[b], &to_model, &inv);
          have[b] = true;
        }
  }
  for (int b = 0; b < p->count; b++) {
    const t3_mat4 *pm = p->parent[b] >= 0 ? &bind[p->parent[b]] : &p->base[b];
    if (!have[b]) { /* the bind-time local transform under the parent's bind pose */
      t3_mat4 l;
      t3_mat4_compose(&l, p->rest_pos[b], p->rest_rot[b], p->rest_scl[b]);
      t3_mat4_multiply(&bind[b], pm, &l);
      continue;
    }
    t3_mat4 pinv, l;
    t3_mat4_invert(&pinv, pm);
    t3_mat4_multiply(&l, &pinv, &bind[b]);
    t3_mat4_decompose(&l, &p->rest_pos[b], &p->rest_rot[b], &p->rest_scl[b]);
  }
  cw_free(bind);
  cw_free(have);
}

static bool skel_bind(skel *s) {
  /* a Skeleton3D holder: the bones of its model (cartwheel/skeleton.h); any other Node3D: its own object */
  t3_object *obj = cw__skeleton_bone_root(s->model, &s->frame);
  if (s->bound && obj == s->obj) return true;
  if (!s->bound && s->bind_failed && obj == s->obj) return false;
  skel_unbind(s);
  s->obj = obj;
  if (!obj || !cw_pose_bind(&s->pose, obj)) { s->bind_failed = true; return false; }
  bind_rest(&s->pose, obj);
  s->bind_failed = false;
  s->bound = true;
  s->gen++;
  int n = s->pose.count;
  cw_pose_init(&s->scratch, n);
  memcpy(s->scratch.parent, s->pose.parent, sizeof(int) * (size_t)n);
  s->anim_pos = cw_calloc((size_t)n, sizeof(t3_vec3), CW_MEM_ANIM);
  s->fl_pos = cw_calloc((size_t)n, sizeof(t3_vec3), CW_MEM_ANIM);
  s->anim_scl = cw_calloc((size_t)n, sizeof(t3_vec3), CW_MEM_ANIM);
  s->fl_scl = cw_calloc((size_t)n, sizeof(t3_vec3), CW_MEM_ANIM);
  s->anim_rot = cw_calloc((size_t)n, sizeof(t3_quat), CW_MEM_ANIM);
  s->fl_rot = cw_calloc((size_t)n, sizeof(t3_quat), CW_MEM_ANIM);
  return true;
}

/* the animated pose: a bone still holding what we flushed was not animated
 * this tick, so it gets its last animated value back */
static void skel_capture(skel *s) {
  cw_pose *p = &s->pose;
  cw__pose_capture_locals(p);
  for (int i = 0; i < p->count; i++) {
    if (s->flushed && !memcmp(&p->pos[i], &s->fl_pos[i], sizeof(t3_vec3)) &&
        !memcmp(&p->rot[i], &s->fl_rot[i], sizeof(t3_quat)) && !memcmp(&p->scl[i], &s->fl_scl[i], sizeof(t3_vec3))) {
      p->pos[i] = s->anim_pos[i];
      p->rot[i] = s->anim_rot[i];
      p->scl[i] = s->anim_scl[i];
    } else {
      s->anim_pos[i] = p->pos[i];
      s->anim_rot[i] = p->rot[i];
      s->anim_scl[i] = p->scl[i];
    }
  }
  cw_pose_update_model(p);
}

static void skel_flush(skel *s) {
  cw_pose *p = &s->pose;
  cw_pose_flush(p);
  /* what the bones hold now (t3_object_set_quaternion stores it as given) */
  for (int i = 0; i < p->count; i++) {
    t3_object *o = p->bone[i];
    s->fl_pos[i] = o->position;
    s->fl_rot[i] = o->quaternion;
    s->fl_scl[i] = o->scale;
  }
  s->flushed = true;
  s->hash = cw_pose_hash(p, s->hash);
}

static void mod_solve(cw_node n, skel *s) {
  const cw_node_type *t = cw_node_get_type(n);
  if (cw_node_type_is(t, "TwoBoneIK3D")) two_solve(n, cw_node_data_t(n, two_t), s);
  else if (cw_node_type_is(t, "LookAtModifier3D")) look_solve(n, cw_node_data_t(n, look_t), s);
  else if (cw_node_type_is(t, "CCDIK3D")) ccd_solve(n, cw_node_data_t(n, ccd_t), s);
  else if (cw_node_type_is(t, "FABRIK3D")) fab_solve(n, cw_node_data_t(n, fab_t), s);
  else if (cw_node_type_is(t, "FootPlacement3D")) foot_solve(n, cw_node_data_t(n, foot_t), s);
  else if (cw_node_type_is(t, "SpringBone3D")) spring_solve(n, cw_node_data_t(n, spring_t), s);
}

static void solve_skel(skel *s) {
  for (int m = 0; m < s->nmods; m++) {
    cw_node n = s->mods[m];
    float w = MB(n)->influence;
    if (w <= 0) continue;
    if (w < 1) cw_pose_copy_locals(&s->scratch, &s->pose);
    mod_solve(n, s);
    if (w < 1) cw_pose_blend(&s->pose, &s->scratch, 1 - w); /* the result back toward its input */
  }
  skel_flush(s);
}

static void solve_range(int begin, int end, int worker, void *ctx) {
  (void)worker;
  skel **list = ctx;
  for (int i = begin; i < end; i++)
    if (list[i]) solve_skel(list[i]);
}

/* wall clock for the profiling figure only (never game state) */
static double now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

static void phase_hook(cw_tree *tree, double dt, void *u) {
  CW_PROFILE_SCOPE("anim.ik");
  (void)u;
  ikw *W = cw_tree_slot(tree, "ik");
  if (!W || !W->skels.count) return;
  double t0 = now_us();
  skel *run[256];
  int nrun = 0;
  for (int i = 0; i < W->skels.count; i++) {
    skel *s = W->skels.data[i];
    if (!cw_node_valid(s->model)) {
      skel_free(s);
      memmove(W->skels.data + i, W->skels.data + i + 1, sizeof(skel *) * (size_t)(W->skels.count - i - 1));
      W->skels.count--;
      i--;
      continue;
    }
    s->nmods = 0;
    for (int c = 0; c < cw_node_child_count(s->model) && s->nmods < MAX_MODS; c++) {
      cw_node ch = cw_node_child(s->model, c);
      modbase *b = cw_node_data_t(ch, mod_t);
      if (b && b->active && cw_node_can_process(ch)) s->mods[s->nmods++] = ch;
    }
    if (!s->nmods) {
      /* every modifier off: give the bones their animated pose back once */
      if (s->bound && s->flushed && cw__skeleton_bone_root(s->model, NULL) == s->obj) {
        skel_capture(s);
        cw_pose_flush(&s->pose);
        s->flushed = false;
      }
      continue;
    }
    if (!skel_bind(s)) continue;
    model_frame(s);
    skel_capture(s);
    for (int m = 0; m < s->nmods; m++) {
      cw_node n = s->mods[m];
      const cw_node_type *t = cw_node_get_type(n);
      if (cw_node_type_is(t, "TwoBoneIK3D")) two_prepare(n, cw_node_data_t(n, two_t), s);
      else if (cw_node_type_is(t, "LookAtModifier3D")) look_prepare(n, cw_node_data_t(n, look_t), s);
      else if (cw_node_type_is(t, "CCDIK3D")) ccd_prepare(n, cw_node_data_t(n, ccd_t), s);
      else if (cw_node_type_is(t, "FABRIK3D")) fab_prepare(n, cw_node_data_t(n, fab_t), s);
      else if (cw_node_type_is(t, "FootPlacement3D")) foot_prepare(n, cw_node_data_t(n, foot_t), s, tree, dt);
      else if (cw_node_type_is(t, "SpringBone3D")) spring_prepare(n, cw_node_data_t(n, spring_t), s, tree, dt);
    }
    if (nrun < 256) run[nrun++] = s;
    else solve_skel(s);
  }
  /* a job per model only pays off for a crowd: waking the pool costs more
   * than a few models' solves (measured: 4 robots in a cart, 44 us serial
   * against 147 us through 7 workers). Same results either way. */
  if (nrun >= PARALLEL_MIN) cw_parallel_for(nrun, 1, solve_range, run);
  else solve_range(0, nrun, 0, run);
  float us = (float)(now_us() - t0);
  for (int i = 0; i < nrun; i++) run[i]->last_us = us / (float)nrun;
}

static void ikw_free(void *p) {
  ikw *W = p;
  for (int i = 0; i < W->skels.count; i++) skel_free(W->skels.data[i]);
  cw_array_free(&W->skels);
  cw_free(W);
}

static ikw *world_of(cw_tree *t, bool create) {
  if (!t) return NULL;
  ikw *W = cw_tree_slot(t, "ik");
  if (W || !create) return W;
  W = cw_calloc(1, sizeof *W, CW_MEM_ANIM);
  W->tree = t;
  cw_tree_set_slot(t, "ik", W, ikw_free);
  /* after the nodes (0), tweens (100) and the physics sync (101) */
  cw_tree_add_hook(t, 0, 150, phase_hook, NULL);
  return W;
}

static skel *skel_of(cw_node model, bool create) {
  ikw *W = world_of(cw_node_tree(model), create);
  if (!W) return NULL;
  for (int i = 0; i < W->skels.count; i++)
    if (W->skels.data[i]->model.id == model.id) return W->skels.data[i];
  if (!create) return NULL;
  skel *s = cw_calloc(1, sizeof *s, CW_MEM_ANIM);
  s->model = model;
  s->frame = model;
  s->hash = CW_HASH32_INIT;
  cw_array_push(&W->skels, s);
  return s;
}

/* ── public queries ────────────────────────────────────────────────── */

const cw_pose *cw_skeleton_pose(cw_node model) {
  skel *s = skel_of(model, false);
  return s && s->bound ? &s->pose : NULL;
}
uint32_t cw_skeleton_hash(cw_node model) {
  skel *s = skel_of(model, false);
  return s ? s->hash : 0;
}
float cw_skeleton_last_us(cw_node model) {
  skel *s = skel_of(model, false);
  return s ? s->last_us : 0;
}
t3_vec3 cw_skeleton_bone_world(cw_node model, const char *bone) {
  skel *s = skel_of(model, false);
  if (!s || !s->bound) return v3(0, 0, 0);
  int i = cw_pose_find(&s->pose, bone);
  if (i < 0) return v3(0, 0, 0);
  model_frame(s);
  return to_world(s, cw_pose_model_position(&s->pose, i));
}

void cw_ccdik3d_set_joint(cw_node n, int joint, const cw_ccdik_link *limits) {
  ccd_node *d = cw_node_data_t(n, ccd_t);
  if (!d || joint < 0 || joint >= CW_IK_MAX_CHAIN) { cw_warn("cw_ccdik3d_set_joint: not a CCDIK3D or joint out of range"); return; }
  d->joints[joint] = limits ? *limits : (cw_ccdik_link){ 0 };
  d->joint_set[joint] = limits != NULL;
}
void cw_fabrik3d_set_joint(cw_node n, int joint, const cw_ik_joint *j) {
  fab_node *d = cw_node_data_t(n, fab_t);
  if (!d || joint < 0 || joint >= CW_IK_MAX_CHAIN) { cw_warn("cw_fabrik3d_set_joint: not a FABRIK3D or joint out of range"); return; }
  d->joints[joint] = j ? *j : (cw_ik_joint){ 0 };
}
bool cw_foot_placement_leg(cw_node n, int i, cw_foot_state *out) {
  foot_node *d = cw_node_data_t(n, foot_t);
  if (!d || i < 0 || i >= d->nlegs) return false;
  if (out) *out = d->L[i].st;
  return true;
}
float cw_foot_placement_pelvis_offset(cw_node n) {
  foot_node *d = cw_node_data_t(n, foot_t);
  return d ? d->pelvis_off : 0;
}

void cw_spring_bone3d_reset(cw_node n) {
  spring_node *d = cw_node_data_t(n, spring_t);
  if (!d) { cw_warn("cw_spring_bone3d_reset: not a SpringBone3D"); return; }
  d->reset = true;
}
bool cw_spring_chain3d_joint_world(cw_node chain, int joint, t3_vec3 *out) {
  spring_chain *c = cw_node_data_t(chain, chain_t);
  if (!c || !c->primed || joint < 0 || joint >= c->n) return false;
  if (out) *out = c->cur[joint];
  return true;
}
bool cw_spring_collision3d_world(cw_node shape, t3_vec3 *a, t3_vec3 *b, float *radius) {
  spring_shape *h = cw_node_data_t(shape, shape_t);
  if (!h || !h->placed) return false;
  if (a) *a = h->wa;
  if (b) *b = h->wb;
  if (radius) *radius = h->wr;
  return true;
}

/* ── node types ────────────────────────────────────────────────────── */

static void mod_init(cw_node n, void *data) {
  (void)n;
  modbase *b = data;
  b->active = true;
  b->influence = 1;
}
static void mod_enter(cw_node n, void *data) {
  (void)data;
  cw_node model = cw_node_parent(n);
  if (CW_HANDLE_IS_NULL(model) || !cw_node_is(model, "Node3D")) {
    cw_warn("%s '%s': a skeleton modifier is a child of the Node3D holding the skinned model, or of its Skeleton3D", cw_node_get_type(n)->name,
            cw_node_name(n));
    return;
  }
  skel_of(model, true);
}

static const cw_prop mod_props[] = {
  CW_PROP_FIELD(modbase, active, CW_T_BOOL, CW_PROP_DEFAULT, "Runs each tick when true."),
  CW_PROP_FIELD_RANGE(modbase, influence, CW_T_FLOAT, 0, 1, "Blend of this modifier's result with the pose it was given."),
  CW_PROP_END,
};
static cw_node_type mod_type = {
  .name = "SkeletonModifier3D", .parent = "Node", .data_size = sizeof(modbase), .props = mod_props,
  .doc = "Base of the skeleton modifiers: children of a skinned model's Node3D, or of its Skeleton3D, that adjust its animated pose each tick.",
  .init = mod_init, .enter_tree = mod_enter,
};

static void two_init(cw_node n, void *data) {
  (void)n;
  two_bone *d = data;
  d->soften = 1;
  d->target_rotation_offset = (t3_quat){ 0, 0, 0, 1 };
  d->gen = (uint32_t)-1;
}
#define RO (CW_PROP_READONLY)
static const cw_prop two_props[] = {
  CW_PROP_FIELD(two_bone, root_bone, CW_T_STRING, CW_PROP_DEFAULT, "Upper bone (shoulder, hip)."),
  CW_PROP_FIELD(two_bone, middle_bone, CW_T_STRING, CW_PROP_DEFAULT, "Middle bone (elbow, knee)."),
  CW_PROP_FIELD(two_bone, end_bone, CW_T_STRING, CW_PROP_DEFAULT, "End bone (wrist, ankle) placed at the target."),
  CW_PROP_FIELD(two_bone, target, CW_T_STRING, CW_PROP_DEFAULT, "Node3D (path from this node) the end reaches for."),
  CW_PROP_FIELD(two_bone, target_position, CW_T_VEC3, CW_PROP_DEFAULT, "World target when no target node is set."),
  CW_PROP_FIELD(two_bone, pole, CW_T_STRING, CW_PROP_DEFAULT, "Node3D the middle joint bends toward (use_pole)."),
  CW_PROP_FIELD(two_bone, pole_position, CW_T_VEC3, CW_PROP_DEFAULT, "World pole point when no pole node is set."),
  CW_PROP_FIELD(two_bone, use_pole, CW_T_BOOL, CW_PROP_DEFAULT, "Bend toward the pole; false keeps the animation's bend plane."),
  CW_PROP_FIELD_RANGE(two_bone, soften, CW_T_FLOAT, 0, 1, "Reach eases off from this fraction of the chain's length (1 = off)."),
  CW_PROP_FIELD(two_bone, use_target_rotation, CW_T_BOOL, CW_PROP_DEFAULT, "End bone takes the target node's rotation (times the offset)."),
  CW_PROP_FIELD(two_bone, target_rotation_offset, CW_T_QUAT, CW_PROP_DEFAULT, "Rotation in the target's frame applied to the end bone."),
  { .name = "reached", .type = CW_T_BOOL, .flags = RO, .offset = offsetof(two_bone, reached), .size = sizeof(bool),
    .doc = "The target was within reach last tick." },
  CW_PROP_END,
};
static cw_node_type two_type = {
  .name = "TwoBoneIK3D", .parent = "SkeletonModifier3D", .data_size = sizeof(two_bone), .props = two_props,
  .doc = "Two-bone IK: an arm or leg reaches a target, bending toward a pole.", .init = two_init,
};

static void look_init(cw_node n, void *data) {
  (void)n;
  look_at *d = data;
  d->forward_axis = v3(0, 0, 1);
  d->gen = (uint32_t)-1;
}
static const cw_prop look_props[] = {
  CW_PROP_FIELD(look_at, bone, CW_T_STRING, CW_PROP_DEFAULT, "Bone that turns (head, eye, neck, turret)."),
  CW_PROP_FIELD(look_at, target, CW_T_STRING, CW_PROP_DEFAULT, "Node3D (path from this node) to look at."),
  CW_PROP_FIELD(look_at, target_position, CW_T_VEC3, CW_PROP_DEFAULT, "World target when no target node is set."),
  CW_PROP_FIELD(look_at, forward_axis, CW_T_VEC3, CW_PROP_DEFAULT, "The way the bone faces at rest, in model space."),
  CW_PROP_FIELD(look_at, offset, CW_T_VEC3, CW_PROP_DEFAULT, "Aim origin offset from the joint at rest, model space (eyes)."),
  CW_PROP_FIELD_RANGE(look_at, limit_angle, CW_T_FLOAT, 0, 3.14159265, "Most the bone turns from its animated direction (radians, 0 = no limit)."),
  { .name = "in_limit", .type = CW_T_BOOL, .flags = RO, .offset = offsetof(look_at, in_limit), .size = sizeof(bool),
    .doc = "The target was inside the limit last tick." },
  CW_PROP_END,
};
static cw_node_type look_type = {
  .name = "LookAtModifier3D", .parent = "SkeletonModifier3D", .data_size = sizeof(look_at), .props = look_props,
  .doc = "Turns a bone toward a target within an angle limit.", .init = look_init,
};

static void ccd_init(cw_node n, void *data) {
  (void)n;
  ccd_node *d = data;
  d->max_iterations = 4;
  d->gen = (uint32_t)-1;
}
static const cw_prop ccd_props[] = {
  CW_PROP_FIELD(ccd_node, root_bone, CW_T_STRING, CW_PROP_DEFAULT, "Top of the chain (the last link)."),
  CW_PROP_FIELD(ccd_node, end_bone, CW_T_STRING, CW_PROP_DEFAULT, "Effector: the bone brought to the target."),
  CW_PROP_FIELD(ccd_node, target, CW_T_STRING, CW_PROP_DEFAULT, "Node3D (path from this node) the effector reaches for."),
  CW_PROP_FIELD(ccd_node, target_position, CW_T_VEC3, CW_PROP_DEFAULT, "World target when no target node is set."),
  CW_PROP_FIELD_RANGE(ccd_node, max_iterations, CW_T_INT, 1, 64, "CCD passes (r187 iteration); stops early when nothing turns."),
  CW_PROP_FIELD_RANGE(ccd_node, min_angle, CW_T_FLOAT, 0, 3.14159265, "Smallest step per link (radians, 0 = none)."),
  CW_PROP_FIELD_RANGE(ccd_node, max_angle, CW_T_FLOAT, 0, 3.14159265, "Largest step per link (radians, 0 = none)."),
  CW_PROP_END,
};
static cw_node_type ccd_type = {
  .name = "CCDIK3D", .parent = "SkeletonModifier3D", .data_size = sizeof(ccd_node), .props = ccd_props,
  .doc = "Cyclic coordinate descent over a chain, as three.js r187's CCDIKSolver.", .init = ccd_init,
};

static void fab_init(cw_node n, void *data) {
  (void)n;
  fab_node *d = data;
  d->max_iterations = 4;
  d->min_distance = 0.001f;
  d->gen = (uint32_t)-1;
}
static const cw_prop fab_props[] = {
  CW_PROP_FIELD(fab_node, root_bone, CW_T_STRING, CW_PROP_DEFAULT, "Chain root (stays in place)."),
  CW_PROP_FIELD(fab_node, end_bone, CW_T_STRING, CW_PROP_DEFAULT, "Chain end brought to the target."),
  CW_PROP_FIELD(fab_node, target, CW_T_STRING, CW_PROP_DEFAULT, "Node3D (path from this node) the end reaches for."),
  CW_PROP_FIELD(fab_node, target_position, CW_T_VEC3, CW_PROP_DEFAULT, "World target when no target node is set."),
  CW_PROP_FIELD_RANGE(fab_node, max_iterations, CW_T_INT, 1, 64, "Backward/forward passes."),
  CW_PROP_FIELD_RANGE(fab_node, min_distance, CW_T_FLOAT, 0, 1, "Stop when the end is this close (metres)."),
  CW_PROP_END,
};
static cw_node_type fab_type = {
  .name = "FABRIK3D", .parent = "SkeletonModifier3D", .data_size = sizeof(fab_node), .props = fab_props,
  .doc = "FABRIK over a chain (tails, spines, cables) with cone and hinge limits.", .init = fab_init,
};

static void foot_init(cw_node n, void *data) {
  (void)n;
  foot_node *d = data;
  d->ray_up = 0.5f;
  d->max_step_down = 0.5f;
  d->foot_height = 0.1f;
  d->max_slope = 0.7f;
  d->blend_speed = 12;
  d->collision_mask = (int32_t)CW_PHYS_ALL_LAYERS;
  d->gen = (uint32_t)-1;
}
static const cw_prop foot_props[] = {
  CW_PROP_FIELD(foot_node, legs, CW_T_STRING, CW_PROP_DEFAULT, "\"upper,lower,end[,foot]\" bone names per leg, legs separated by ';'."),
  CW_PROP_FIELD(foot_node, pelvis, CW_T_STRING, CW_PROP_DEFAULT, "Bone lowered so the lowest foot reaches its ground."),
  CW_PROP_FIELD_RANGE(foot_node, ray_up, CW_T_FLOAT, 0, 10, "Rays start this far above the animated ankle (m): the highest step."),
  CW_PROP_FIELD_RANGE(foot_node, max_step_down, CW_T_FLOAT, 0, 10, "Lowest ground a foot reaches below the model's origin (m)."),
  CW_PROP_FIELD_RANGE(foot_node, foot_height, CW_T_FLOAT, 0, 1, "Ankle height above the sole (m): feet align below it."),
  CW_PROP_FIELD_RANGE(foot_node, max_slope, CW_T_FLOAT, 0, 1.5, "Most a foot tilts onto the ground normal (radians)."),
  CW_PROP_FIELD_RANGE(foot_node, blend_speed, CW_T_FLOAT, 0, 1000, "How fast offsets follow the ground (1/s; 0 = at once)."),
  CW_PROP_FIELD(foot_node, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Physics layers the rays hit."),
  CW_PROP_END,
};
static cw_node_type foot_type = {
  .name = "FootPlacement3D", .parent = "SkeletonModifier3D", .data_size = sizeof(foot_node), .props = foot_props,
  .doc = "Plants feet on the physics ground: raycasts, a lowered pelvis, two-bone legs, feet on the ground normal.",
  .init = foot_init,
};

static void spring_init(cw_node n, void *data) {
  (void)n;
  spring_node *d = data;
  d->teleport_distance = 1;
  d->teleport_angle = 1.5f;
  d->collisions = true;
}
static const cw_prop spring_props[] = {
  CW_PROP_FIELD(spring_node, reset, CW_T_BOOL, CW_PROP_EDITOR, "Set true: the chains start again from the animated pose next tick (clears itself)."),
  CW_PROP_FIELD_RANGE(spring_node, teleport_distance, CW_T_FLOAT, 0, 1000, "A model moving further than this in one tick teleported: the chains move with it, no whip (m, 0 = off)."),
  CW_PROP_FIELD_RANGE(spring_node, teleport_angle, CW_T_FLOAT, 0, 3.14159265, "A model turning further than this in one tick teleported (radians, 0 = off)."),
  CW_PROP_FIELD(spring_node, collisions, CW_T_BOOL, CW_PROP_DEFAULT, "The SpringBoneCollision3D children push the chains out of the body."),
  { .name = "resets", .type = CW_T_INT, .flags = RO, .offset = offsetof(spring_node, resets), .size = sizeof(int32_t),
    .doc = "Resets and teleports so far (each starts the chains again)." },
  CW_PROP_END,
};
static cw_node_type spring_type = {
  .name = "SpringBone3D", .parent = "SkeletonModifier3D", .data_size = sizeof(spring_node), .props = spring_props,
  .doc = "Secondary motion: hair, braids, tails and coat panels lag, sway and settle back to the animated pose (its SpringBoneChain3D children), kept out of the body by its SpringBoneCollision3D children.",
  .init = spring_init,
};

static void chain_init(cw_node n, void *data) {
  (void)n;
  spring_chain *c = data;
  c->stiffness = 120;
  c->damping = 8;
  c->gravity = v3(0, -1, 0);
  c->radius = 0.02f;
  c->gen = (uint32_t)-1;
}
static void child_enter(cw_node n, void *data) {
  (void)data;
  cw_node p = cw_node_parent(n);
  if (CW_HANDLE_IS_NULL(p) || !cw_node_is(p, "SpringBone3D"))
    cw_warn("%s '%s': a child of a SpringBone3D", cw_node_get_type(n)->name, cw_node_name(n));
}
static const cw_prop chain_props[] = {
  CW_PROP_FIELD(spring_chain, root_bone, CW_T_STRING, CW_PROP_DEFAULT, "First bone that swings (its origin stays on its parent)."),
  CW_PROP_FIELD(spring_chain, end_bone, CW_T_STRING, CW_PROP_DEFAULT, "Last bone of the chain (below root_bone); its child, or its own length again, is the tip."),
  CW_PROP_FIELD_RANGE(spring_chain, stiffness, CW_T_FLOAT, 0, 3600, "Pull back toward the animated pose (1/s^2: 120 sways about 1.7 times a second; 3600 holds it)."),
  CW_PROP_FIELD_RANGE(spring_chain, damping, CW_T_FLOAT, 0, 1000, "Drag: how fast the swing dies away (1/s)."),
  CW_PROP_FIELD(spring_chain, gravity, CW_T_VEC3, CW_PROP_DEFAULT, "Direction gravity pulls the chain (world)."),
  CW_PROP_FIELD_RANGE(spring_chain, gravity_strength, CW_T_FLOAT, 0, 100, "Gravity's pull (m/s^2; 0 = none): the chain sags by about strength / stiffness metres."),
  CW_PROP_FIELD_RANGE(spring_chain, radius, CW_T_FLOAT, 0, 10, "Hit radius of each joint and the tip against the collision shapes (m)."),
  { .name = "tip_offset", .type = CW_T_VEC3, .flags = RO, .offset = offsetof(spring_chain, tip_offset), .size = sizeof(t3_vec3),
    .doc = "The tip from where the animated pose puts it, last tick (world, m)." },
  { .name = "tip_distance", .type = CW_T_FLOAT, .flags = RO, .offset = offsetof(spring_chain, tip_distance), .size = sizeof(float),
    .doc = "Length of tip_offset (m)." },
  CW_PROP_END,
};
static cw_node_type chain_type = {
  .name = "SpringBoneChain3D", .parent = "Node", .data_size = sizeof(spring_chain), .props = chain_props,
  .doc = "One chain of a SpringBone3D (a ponytail, a braid, a coat tail): root_bone to end_bone with its own spring.",
  .init = chain_init, .enter_tree = child_enter,
};

static void shape_init(cw_node n, void *data) {
  (void)n;
  spring_shape *h = data;
  h->radius = 0.1f;
  h->gen = (uint32_t)-1;
  h->idx = -1;
}
static const char *const shape_names[] = { "SPHERE", "CAPSULE", NULL };
static const cw_prop shape_props[] = {
  CW_PROP_FIELD(spring_shape, bone, CW_T_STRING, CW_PROP_DEFAULT, "Bone the shape rides (head, chest, thigh)."),
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(spring_shape, shape), .size = sizeof(int32_t),
    .enum_names = shape_names, .doc = "SPHERE at offset, or CAPSULE from offset to tail." },
  CW_PROP_FIELD_RANGE(spring_shape, radius, CW_T_FLOAT, 0, 10, "Radius (m)."),
  CW_PROP_FIELD(spring_shape, offset, CW_T_VEC3, CW_PROP_DEFAULT, "Centre (a capsule's first end) from the bone's origin, in the model's rest (bind) orientation (m)."),
  CW_PROP_FIELD(spring_shape, tail, CW_T_VEC3, CW_PROP_DEFAULT, "A capsule's other end, like offset (m)."),
  CW_PROP_END,
};
static cw_node_type shape_type = {
  .name = "SpringBoneCollision3D", .parent = "Node", .data_size = sizeof(spring_shape), .props = shape_props,
  .doc = "A sphere or capsule on a bone that keeps its SpringBone3D's chains out of the body.",
  .init = shape_init, .enter_tree = child_enter,
};

/* ── debug channel ─────────────────────────────────────────────────── */

static void cmd_ik(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: ik <model path> (the Node3D whose children are skeleton modifiers)"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "Node3D");
  if (!cw_node_valid(n)) return;
  skel *s = skel_of(n, false);
  if (!s) { cw_reply_error(r, "'%s' has no skeleton modifiers", argv[1]); return; }
  cw_reply_printf(r, "{\"bound\":%s,\"bones\":%d,\"hash\":%u,\"us\":%.3f,\"modifiers\":[", s->bound ? "true" : "false",
                  s->bound ? s->pose.count : 0, s->hash, s->last_us);
  for (int m = 0; m < s->nmods; m++) {
    cw_node c = s->mods[m];
    if (m) cw_reply_raw(r, ",");
    cw_reply_raw(r, "{\"name\":");
    cw_reply_str(r, cw_node_name(c));
    cw_reply_raw(r, ",\"type\":");
    cw_reply_str(r, cw_node_get_type(c)->name);
    cw_reply_printf(r, ",\"influence\":%.6g", MB(c)->influence);
    if (cw_node_is(c, "TwoBoneIK3D")) cw_reply_printf(r, ",\"reached\":%s", ((two_bone *)cw_node_data_t(c, two_t))->reached ? "true" : "false");
    if (cw_node_is(c, "LookAtModifier3D")) cw_reply_printf(r, ",\"in_limit\":%s", ((look_at *)cw_node_data_t(c, look_t))->in_limit ? "true" : "false");
    if (cw_node_is(c, "FootPlacement3D")) {
      foot_node *d = cw_node_data_t(c, foot_t);
      cw_reply_printf(r, ",\"pelvis\":%.6g,\"legs\":[", d->pelvis_off);
      for (int i = 0; i < d->nlegs; i++)
        cw_reply_printf(r, "%s{\"hit\":%s,\"offset\":%.6g}", i ? "," : "", d->L[i].st.hit ? "true" : "false", d->L[i].dy);
      cw_reply_raw(r, "]");
    }
    if (cw_node_is(c, "SpringBone3D")) {
      spring_node *d = cw_node_data_t(c, spring_t);
      cw_reply_printf(r, ",\"resets\":%d,\"chains\":[", d->resets);
      for (int i = 0; i < d->nch; i++) {
        const spring_chain *ch = d->ch[i];
        cw_reply_printf(r, "%s{\"joints\":%d,\"tip_offset\":[%.6g,%.6g,%.6g],\"tip_distance\":%.6g}", i ? "," : "", ch->n,
                        ch->tip_offset.x, ch->tip_offset.y, ch->tip_offset.z, ch->tip_distance);
      }
      cw_reply_raw(r, "]");
    }
    cw_reply_raw(r, "}");
  }
  cw_reply_raw(r, "]}");
}

void cw__register_ik_types(void) {
  cw_node_type_register(&mod_type);
  mod_t = &mod_type;
  cw_node_type_register(&two_type);
  two_t = &two_type;
  cw_node_type_register(&look_type);
  look_t = &look_type;
  cw_node_type_register(&ccd_type);
  ccd_t = &ccd_type;
  cw_node_type_register(&fab_type);
  fab_t = &fab_type;
  cw_node_type_register(&foot_type);
  foot_t = &foot_type;
  cw_node_type_register(&spring_type);
  spring_t = &spring_type;
  cw_node_type_register(&chain_type);
  chain_t = &chain_type;
  cw_node_type_register(&shape_type);
  shape_t = &shape_type;
  cw_debug_command("ik", "ik <model path>: skeleton modifiers, last results, pose hash", cmd_ik);
}

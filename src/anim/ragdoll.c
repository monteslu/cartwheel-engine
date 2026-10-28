/* Ragdolls from a skeleton pose (cartwheel/ragdoll.h): one Box3D body and
 * capsule per part bone, a spherical (cone + twist) or revolute (hinge) joint
 * to the nearest ancestor part with limits measured from the rest pose, and
 * the bodies written back as model rotations. Only + - * / sqrt and
 * cartwheel/math.h: the same inputs give the same bits on every host. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/math.h"
#include "cartwheel/ragdoll.h"

static inline t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }
static inline t3_vec3 vadd(t3_vec3 a, t3_vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline t3_vec3 vmul(t3_vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline t3_vec3 vcross(t3_vec3 a, t3_vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float vlen(t3_vec3 a) { return sqrtf(vdot(a, a)); }
static inline t3_vec3 vnorm(t3_vec3 a) { float l = vlen(a); return l > 0 ? vmul(a, 1 / l) : v3(0, 0, 0); }
static inline t3_quat qmul(t3_quat a, t3_quat b) {
  return (t3_quat){ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static inline t3_quat qconj(t3_quat q) { return (t3_quat){ -q.x, -q.y, -q.z, q.w }; }
static inline t3_quat qnorm(t3_quat q) {
  float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l == 0) return (t3_quat){ 0, 0, 0, 1 };
  l = 1 / l;
  return (t3_quat){ q.x * l, q.y * l, q.z * l, q.w * l };
}
static inline t3_vec3 qrot(t3_quat q, t3_vec3 v) {
  float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
  return v3(v.x + q.w * tx + q.y * tz - q.z * ty, v.y + q.w * ty + q.z * tx - q.x * tz, v.z + q.w * tz + q.x * ty - q.y * tx);
}

typedef struct rd_part {
  int bone, parent;            /* pose bone; parent part (-1: root) */
  cw_ragdoll_joint_kind kind;
  cw_body3d body;
  cw_joint3d joint;
  t3_quat rest_mrot;           /* the bone's model rotation at rest */
  t3_quat fa, fb;              /* joint frame rotations in the parent's and this body's frames */
  t3_vec3 a, c;                /* capsule segment, body local */
  char name[48];               /* the bone name (cw_ragdoll_find) */
  bool attached;               /* hung from a part that is not its skeleton ancestor: its position is written too */
  t3_vec3 base_pos, last_pos;  /* the bone's local transform when the ragdoll was made, and as blend_pose last left it */
  t3_quat base_rot, last_rot;
} rd_part;

struct cw_ragdoll {
  cw_world3d world;
  int count;
  rd_part p[CW_RAGDOLL_MAX_PARTS];
  int order[CW_RAGDOLL_MAX_PARTS]; /* parts by pose bone index: parents first */
  cw_pose scratch;                 /* blend_pose's ragdoll copy of the pose */
  bool have_last;                  /* blend_pose has run: last_* hold what it left in the pose */
};

cw_ragdoll_def cw_ragdoll_default_def(const cw_ragdoll_part *parts, int count) {
  return (cw_ragdoll_def){ .parts = parts, .count = count, .density = 1000, .friction = 0.7f, .restitution = 0.05f,
                           .joint_friction = 2, .linear_damping = 0.05f, .angular_damping = 0.6f, .mask = CW_PHYS_ALL_LAYERS };
}

/* model to world: rotation q, translation t, uniform scale s */
typedef struct mw { t3_quat q; t3_vec3 t; float s; } mw;
static mw mw_of(const t3_mat4 *m) {
  mw o = { { 0, 0, 0, 1 }, { 0, 0, 0 }, 1 };
  if (!m) return o;
  t3_vec3 s;
  t3_mat4_decompose(m, &o.t, &o.q, &s);
  o.s = s.x;
  return o;
}
static t3_vec3 to_world(const mw *m, t3_vec3 p) { return vadd(m->t, qrot(m->q, vmul(p, m->s))); }
static t3_vec3 to_model(const mw *m, t3_vec3 p) { return vmul(qrot(qconj(m->q), vsub(p, m->t)), 1 / m->s); }

static int part_of_bone(const cw_ragdoll *r, int bone) {
  for (int i = 0; i < r->count; i++) if (r->p[i].bone == bone) return i;
  return -1;
}

/* the rest pose's model rotations and positions (a free copy with the rest locals) */
static void rest_model(const cw_pose *src, cw_pose *rest) {
  cw_pose_init(rest, src->count);
  memcpy(rest->parent, src->parent, sizeof(int) * (size_t)src->count);
  memcpy(rest->base, src->base, sizeof(t3_mat4) * (size_t)src->count);
  memcpy(rest->base_rot, src->base_rot, sizeof(t3_quat) * (size_t)src->count);
  memcpy(rest->pos, src->rest_pos, sizeof(t3_vec3) * (size_t)src->count);
  memcpy(rest->rot, src->rest_rot, sizeof(t3_quat) * (size_t)src->count);
  memcpy(rest->scl, src->rest_scl, sizeof(t3_vec3) * (size_t)src->count);
  cw_pose_update_model(rest);
}

cw_ragdoll *cw_ragdoll_create(cw_world3d w, const cw_ragdoll_def *def, const cw_pose *pose, const t3_mat4 *model_to_world) {
  if (!def || !pose || !cw_world3d_valid(w) || def->count <= 0 || def->count > CW_RAGDOLL_MAX_PARTS) {
    cw_error("ragdoll: needs a world, a pose and 1..%d parts", CW_RAGDOLL_MAX_PARTS);
    return NULL;
  }
  int bone[CW_RAGDOLL_MAX_PARTS], tip[CW_RAGDOLL_MAX_PARTS];
  for (int i = 0; i < def->count; i++) {
    const cw_ragdoll_part *d = &def->parts[i];
    bone[i] = d->bone ? cw_pose_find(pose, d->bone) : -1;
    tip[i] = d->tip ? cw_pose_find(pose, d->tip) : -1;
    if (bone[i] < 0 || (d->tip && tip[i] < 0)) {
      cw_error("ragdoll: bone '%s' (tip '%s') is not in the pose", d->bone ? d->bone : "(null)", d->tip ? d->tip : "-");
      return NULL;
    }
  }
  cw_ragdoll *r = cw_calloc(1, sizeof *r, CW_MEM_PHYSICS);
  r->world = w;
  r->count = def->count;
  for (int i = 0; i < def->count; i++) {
    r->p[i].bone = bone[i];
    snprintf(r->p[i].name, sizeof r->p[i].name, "%s", def->parts[i].bone);
  }
  /* parents: the nearest ancestor bone that is a part */
  for (int i = 0; i < r->count; i++) {
    r->p[i].parent = -1;
    r->p[i].attached = false;
    if (def->parts[i].attach) { /* hung from a named part: a bone outside its skeleton chain (an IK foot) */
      int k = cw_ragdoll_find(r, def->parts[i].attach);
      if (k < 0 || k == i) { cw_error("ragdoll: part %s attaches to %s, which is not a part", def->parts[i].bone, def->parts[i].attach); cw_free(r); return NULL; }
      r->p[i].parent = k, r->p[i].attached = true;
      continue;
    }
    for (int b = pose->parent[bone[i]]; b >= 0; b = pose->parent[b]) {
      int k = part_of_bone(r, b);
      if (k >= 0) { r->p[i].parent = k; break; }
    }
  }
  /* parents first: sort part indices by bone index (the pose orders parents first) */
  for (int i = 0; i < r->count; i++) r->order[i] = i;
  for (int i = 1; i < r->count; i++)
    for (int j = i; j > 0 && r->p[r->order[j]].bone < r->p[r->order[j - 1]].bone; j--) {
      int t = r->order[j]; r->order[j] = r->order[j - 1]; r->order[j - 1] = t;
    }
  static int next_group = 0;
  int group = def->group ? def->group : -(1 + (next_group++ & 0x3fffffff));
  mw M = mw_of(model_to_world);
  cw_pose rest;
  rest_model(pose, &rest);
  for (int i = 0; i < r->count; i++) {
    const cw_ragdoll_part *d = &def->parts[i];
    rd_part *P = &r->p[i];
    int b = bone[i];
    P->kind = d->joint;
    P->rest_mrot = cw_pose_model_rotation(&rest, b);
    P->base_pos = pose->pos[b], P->base_rot = pose->rot[b];
    t3_quat mr = cw_pose_model_rotation(pose, b);
    t3_vec3 o = cw_pose_model_position(pose, b);
    t3_vec3 tipm = tip[i] >= 0 ? cw_pose_model_position(pose, tip[i]) : vadd(o, qrot(qmul(mr, qconj(P->rest_mrot)), d->tip_offset));
    t3_vec3 tl = vmul(qrot(qconj(mr), vsub(tipm, o)), M.s); /* the tip in the body's frame, world units */
    float L = vlen(tl), in = d->radius * 0.5f < L * 0.25f ? d->radius * 0.5f : L * 0.25f;
    t3_vec3 dir = L > 0 ? vmul(tl, 1 / L) : v3(0, 1, 0);
    P->a = vmul(dir, in), P->c = vsub(tl, vmul(dir, in));
    cw_body3d_def bd = cw_body3d_default_def();
    bd.type = CW_BODY_DYNAMIC;
    bd.position = to_world(&M, o);
    bd.rotation = qnorm(qmul(M.q, mr));
    bd.linear_damping = def->linear_damping;
    bd.angular_damping = def->angular_damping;
    bd.name = d->bone;
    bd.interpolate = false;
    P->body = cw_body3d_create(w, &bd);
    cw_shape3d_def sd = cw_shape3d_default_def();
    sd.density = def->density * (d->density_scale > 0 ? d->density_scale : 1);
    sd.material.friction = def->friction;
    sd.material.restitution = def->restitution;
    sd.material.rolling_resistance = 0.1f;
    sd.layer = def->layer;
    sd.mask = def->mask;
    sd.group = group;
    sd.contact_events = false;
    sd.sensor_events = false;
    cw_shape3d_capsule(P->body, &sd, P->a, P->c, d->radius);
  }
  /* joints at each child's bone origin; frames coincide at rest, so 0 is the rest pose */
  for (int i = 0; i < r->count; i++) {
    rd_part *P = &r->p[i];
    if (P->parent < 0) continue;
    const cw_ragdoll_part *d = &def->parts[i];
    rd_part *Q = &r->p[P->parent];
    t3_vec3 z = v3(0, 0, 1), fdir;
    if (P->kind == CW_RAGDOLL_HINGE) fdir = vnorm(d->axis);
    else {
      t3_vec3 o = cw_pose_model_position(&rest, P->bone);
      t3_vec3 tp = tip[i] >= 0 ? cw_pose_model_position(&rest, tip[i]) : vadd(o, d->tip_offset);
      fdir = vnorm(vsub(tp, o));
    }
    if (vdot(fdir, fdir) == 0) fdir = v3(0, 1, 0);
    t3_quat qf = cw_quat_from_to(z, fdir);
    P->fa = qnorm(qmul(qconj(Q->rest_mrot), qf));
    P->fb = qnorm(qmul(qconj(P->rest_mrot), qf));
    t3_quat mrq = cw_pose_model_rotation(pose, Q->bone);
    t3_vec3 anchor = vmul(qrot(qconj(mrq), vsub(cw_pose_model_position(pose, P->bone), cw_pose_model_position(pose, Q->bone))), M.s);
    cw_joint3d_def jd = cw_joint3d_default_def(P->kind == CW_RAGDOLL_HINGE ? CW_JOINT_REVOLUTE : CW_JOINT_SPHERICAL);
    jd.a = Q->body, jd.b = P->body;
    jd.frame_a = (cw_xform3){ anchor, P->fa };
    jd.frame_b = (cw_xform3){ { 0, 0, 0 }, P->fb };
    jd.collide_connected = false;
    jd.spring = false;
    jd.limit = true;
    jd.lower = d->lower, jd.upper = d->upper;
    jd.cone_angle = P->kind == CW_RAGDOLL_BALL ? d->swing : 0;
    jd.motor = def->joint_friction > 0;
    jd.motor_speed = 0;
    jd.max_motor = def->joint_friction;
    P->joint = cw_joint3d_create(w, &jd);
  }
  cw_pose_free(&rest);
  return r;
}

void cw_ragdoll_destroy(cw_ragdoll *r) {
  if (!r) return;
  for (int i = 0; i < r->count; i++) if (cw_joint3d_valid(r->p[i].joint)) cw_joint3d_destroy(r->p[i].joint);
  for (int i = 0; i < r->count; i++) if (cw_body3d_valid(r->p[i].body)) cw_body3d_destroy(r->p[i].body);
  cw_pose_free(&r->scratch);
  cw_free(r);
}

int cw_ragdoll_part_count(const cw_ragdoll *r) { return r ? r->count : 0; }
cw_body3d cw_ragdoll_body(const cw_ragdoll *r, int part) {
  return r && part >= 0 && part < r->count ? r->p[part].body : (cw_body3d){ 0 };
}
int cw_ragdoll_find(const cw_ragdoll *r, const char *bone) {
  if (!r || !bone) return -1;
  for (int i = 0; i < r->count; i++) if (!strcmp(r->p[i].name, bone)) return i;
  return -1;
}
cw_xform3 cw_ragdoll_part_transform(const cw_ragdoll *r, int part) {
  if (!r || part < 0 || part >= r->count) return cw_xform3_identity();
  return cw_body3d_transform(r->p[part].body);
}
t3_vec3 cw_ragdoll_axis(const cw_ragdoll *r, int part, t3_vec3 rest_model_dir) {
  if (!r || part < 0 || part >= r->count) return rest_model_dir;
  cw_xform3 x = cw_body3d_transform(r->p[part].body);
  /* body = model_to_world * model rotation now; model rotation now * conj(rest) carries a rest direction */
  return qrot(qmul(x.q, qconj(r->p[part].rest_mrot)), rest_model_dir);
}

void cw_ragdoll_set_velocity(cw_ragdoll *r, t3_vec3 v, t3_vec3 angular) {
  if (!r) return;
  t3_vec3 c = cw_body3d_transform(r->p[r->order[0]].body).p;
  for (int i = 0; i < r->count; i++) {
    t3_vec3 p = cw_body3d_transform(r->p[i].body).p;
    cw_body3d_set_velocity(r->p[i].body, vadd(v, vcross(angular, vsub(p, c))));
    cw_body3d_set_angular_velocity(r->p[i].body, angular);
  }
}

static float seg_dist2(t3_vec3 a, t3_vec3 b, t3_vec3 p) {
  t3_vec3 ab = vsub(b, a);
  float l = vdot(ab, ab), t = l > 0 ? vdot(vsub(p, a), ab) / l : 0;
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  t3_vec3 d = vsub(p, vadd(a, vmul(ab, t)));
  return vdot(d, d);
}
int cw_ragdoll_apply_impulse(cw_ragdoll *r, int part, t3_vec3 impulse, t3_vec3 point) {
  if (!r) return -1;
  if (part < 0 || part >= r->count) {
    float best = 1e30f;
    for (int i = 0; i < r->count; i++) {
      cw_xform3 x = cw_body3d_transform(r->p[i].body);
      float d = seg_dist2(vadd(x.p, qrot(x.q, r->p[i].a)), vadd(x.p, qrot(x.q, r->p[i].c)), point);
      if (d < best) best = d, part = i;
    }
  }
  cw_body3d_apply_impulse(r->p[part].body, impulse, &point);
  return part;
}
void cw_ragdoll_apply_force(cw_ragdoll *r, t3_vec3 force) {
  if (!r) return;
  cw_body3d_apply_force(r->p[r->order[0]].body, force, NULL);
}

void cw_ragdoll_write_pose(const cw_ragdoll *r, cw_pose *pose, const t3_mat4 *model_to_world) {
  if (!r || !pose) return;
  mw M = mw_of(model_to_world);
  for (int k = 0; k < r->count; k++) {
    const rd_part *P = &r->p[r->order[k]];
    if (P->bone >= pose->count) continue;
    cw_xform3 x = cw_body3d_transform(P->body);
    cw_pose_set_model_rotation(pose, P->bone, qnorm(qmul(qconj(M.q), x.q)));
    if (P->parent < 0 || P->attached) cw_pose_translate_model(pose, P->bone, vsub(to_model(&M, x.p), cw_pose_model_position(pose, P->bone)));
  }
}

static void blend_into(cw_ragdoll *rw, cw_pose *pose, const t3_mat4 *model_to_world, float w);
void cw_ragdoll_blend_pose(cw_ragdoll *r, cw_pose *pose, const t3_mat4 *model_to_world, float w) {
  if (!r || !pose) return;
  /* A part bone the animation did not write since the last call still holds exactly what this call left there (bit for
   * bit): give it back the value it had when the ragdoll was made, so a bone no clip keys never keeps a ragdoll angle. */
  bool touched = false;
  for (int i = 0; r->have_last && i < r->count; i++) {
    rd_part *P = &r->p[i];
    if (P->bone >= pose->count) continue;
    if (!memcmp(&pose->rot[P->bone], &P->last_rot, sizeof P->last_rot)) pose->rot[P->bone] = P->base_rot, touched = true;
    if (!memcmp(&pose->pos[P->bone], &P->last_pos, sizeof P->last_pos)) pose->pos[P->bone] = P->base_pos, touched = true;
  }
  if (touched) cw_pose_update_model(pose);
  if (w >= 1) cw_ragdoll_write_pose(r, pose, model_to_world);
  else if (w > 0) blend_into(r, pose, model_to_world, w);
  for (int i = 0; i < r->count; i++) {
    rd_part *P = &r->p[i];
    if (P->bone >= pose->count) continue;
    P->last_rot = pose->rot[P->bone], P->last_pos = pose->pos[P->bone];
  }
  r->have_last = true;
}
static void blend_into(cw_ragdoll *rw, cw_pose *pose, const t3_mat4 *model_to_world, float w) {
  const cw_ragdoll *r = rw;
  if (rw->scratch.count != pose->count) { cw_pose_free(&rw->scratch); cw_pose_init(&rw->scratch, pose->count); }
  cw_pose *s = &rw->scratch;
  memcpy(s->parent, pose->parent, sizeof(int) * (size_t)pose->count);
  memcpy(s->base, pose->base, sizeof(t3_mat4) * (size_t)pose->count);
  memcpy(s->base_rot, pose->base_rot, sizeof(t3_quat) * (size_t)pose->count);
  cw_pose_copy_locals(s, pose);
  cw_pose_update_model(s);
  cw_ragdoll_write_pose(r, s, model_to_world);
  cw_pose_blend(pose, s, w);
}

bool cw_ragdoll_resting(const cw_ragdoll *r, float speed) {
  if (!r) return true;
  for (int i = 0; i < r->count; i++) {
    if (vlen(cw_body3d_velocity(r->p[i].body)) >= speed) return false;
    if (vlen(cw_body3d_angular_velocity(r->p[i].body)) >= speed * 2) return false;
  }
  return true;
}
float cw_ragdoll_max_speed(const cw_ragdoll *r) {
  float m = 0;
  for (int i = 0; r && i < r->count; i++) { float v = vlen(cw_body3d_velocity(r->p[i].body)); if (v > m) m = v; }
  return m;
}
float cw_ragdoll_joint_angle(const cw_ragdoll *r, int part) {
  if (!r || part < 0 || part >= r->count || r->p[part].parent < 0) return 0;
  const rd_part *P = &r->p[part];
  if (P->kind == CW_RAGDOLL_HINGE) return cw_joint3d_value(P->joint);
  t3_quat qa = cw_body3d_transform(r->p[P->parent].body).q, qb = cw_body3d_transform(P->body).q;
  t3_vec3 za = qrot(qmul(qa, P->fa), v3(0, 0, 1)), zb = qrot(qmul(qb, P->fb), v3(0, 0, 1));
  return cw_acos(vdot(za, zb));
}
uint32_t cw_ragdoll_hash(const cw_ragdoll *r, uint32_t h) {
  for (int i = 0; r && i < r->count; i++) {
    cw_xform3 x = cw_body3d_transform(r->p[i].body);
    h = cw_hash32(&x, sizeof x, h);
  }
  return h;
}

/* The pose layer and the IK solvers (cartwheel/ik.h). Only + - * / sqrt and
 * cartwheel/math.h: every result is bit-identical across hosts.
 *
 * Algorithms: two-bone and aim after ozz-animation's IkTwoBoneJob /
 * IkAimJob (MIT; re-derived trig-free here, exact square roots), CCD as
 * three.js r186's CCDIKSolver (MIT), FABRIK after Aristidou and Lasenby
 * (2011) with cone and hinge constraints as caliko (MIT) applies them. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/ik.h"
#include "cartwheel/anim.h"

/* ── vector and quaternion helpers (deterministic) ─────────────────── */

static inline t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }
static inline t3_vec3 vadd(t3_vec3 a, t3_vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline t3_vec3 vmul(t3_vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline t3_vec3 vcross(t3_vec3 a, t3_vec3 b) {
  return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
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
static inline t3_quat qnorm(t3_quat q) {
  float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l == 0) return (t3_quat){ 0, 0, 0, 1 };
  l = 1 / l;
  return (t3_quat){ q.x * l, q.y * l, q.z * l, q.w * l };
}
/* three.js Vector3.applyQuaternion */
static inline t3_vec3 qrot(t3_quat q, t3_vec3 v) {
  float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
  return v3(v.x + q.w * tx + q.y * tz - q.z * ty, v.y + q.w * ty + q.z * tx - q.x * tz, v.z + q.w * tz + q.x * ty - q.y * tx);
}
static t3_quat qslerp(t3_quat a, t3_quat b, float t) {
  float o[4], fa[4] = { a.x, a.y, a.z, a.w }, fb[4] = { b.x, b.y, b.z, b.w };
  cw_quat_slerp_det(o, fa, fb, t);
  return (t3_quat){ o[0], o[1], o[2], o[3] };
}
static t3_vec3 any_perp(t3_vec3 v) {
  t3_vec3 a = (v.x < 0 ? -v.x : v.x) < 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
  return vnorm(vcross(v, a));
}

t3_quat cw_quat_from_to(t3_vec3 from, t3_vec3 to) {
  /* three.js Quaternion.setFromUnitVectors */
  float r = vdot(from, to) + 1;
  t3_quat q;
  if (r < 1e-6f) {
    r = 0;
    if ((from.x < 0 ? -from.x : from.x) > (from.z < 0 ? -from.z : from.z)) q = (t3_quat){ -from.y, from.x, 0, r };
    else q = (t3_quat){ 0, -from.z, from.y, r };
  } else {
    t3_vec3 c = vcross(from, to);
    q = (t3_quat){ c.x, c.y, c.z, r };
  }
  return qnorm(q);
}

t3_quat cw_quat_axis_angle_det(t3_vec3 axis, float angle) {
  float s, c;
  cw_sincos(angle * 0.5f, &s, &c);
  return (t3_quat){ axis.x * s, axis.y * s, axis.z * s, c };
}

/* ── matrices (column-major, as three.c) ───────────────────────────── */

static void compose(t3_mat4 *m, t3_vec3 p, t3_quat q, t3_vec3 s) {
  /* three.js Matrix4.compose */
  float x = q.x, y = q.y, z = q.z, w = q.w;
  float x2 = x + x, y2 = y + y, z2 = z + z;
  float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
  float wx = w * x2, wy = w * y2, wz = w * z2;
  float *e = m->e;
  e[0] = (1 - (yy + zz)) * s.x; e[1] = (xy + wz) * s.x; e[2] = (xz - wy) * s.x; e[3] = 0;
  e[4] = (xy - wz) * s.y; e[5] = (1 - (xx + zz)) * s.y; e[6] = (yz + wx) * s.y; e[7] = 0;
  e[8] = (xz + wy) * s.z; e[9] = (yz - wx) * s.z; e[10] = (1 - (xx + yy)) * s.z; e[11] = 0;
  e[12] = p.x; e[13] = p.y; e[14] = p.z; e[15] = 1;
}
/* affine product (the bottom rows are 0 0 0 1) */
static void mmul(t3_mat4 *o, const t3_mat4 *a, const t3_mat4 *b) {
  t3_mat4 r;
  const float *x = a->e, *y = b->e;
  for (int c = 0; c < 3; c++)
    for (int rr = 0; rr < 3; rr++)
      r.e[c * 4 + rr] = x[rr] * y[c * 4] + x[4 + rr] * y[c * 4 + 1] + x[8 + rr] * y[c * 4 + 2];
  for (int rr = 0; rr < 3; rr++) r.e[12 + rr] = x[rr] * y[12] + x[4 + rr] * y[13] + x[8 + rr] * y[14] + x[12 + rr];
  r.e[3] = r.e[7] = r.e[11] = 0;
  r.e[15] = 1;
  *o = r;
}
static t3_vec3 mpos(const t3_mat4 *m) { return v3(m->e[12], m->e[13], m->e[14]); }
static t3_vec3 mdir(const t3_mat4 *m, t3_vec3 v) {
  return v3(m->e[0] * v.x + m->e[4] * v.y + m->e[8] * v.z, m->e[1] * v.x + m->e[5] * v.y + m->e[9] * v.z,
            m->e[2] * v.x + m->e[6] * v.y + m->e[10] * v.z);
}
static void identity(t3_mat4 *m) {
  memset(m, 0, sizeof *m);
  m->e[0] = m->e[5] = m->e[10] = m->e[15] = 1;
}
/* inverse of an affine matrix whose 3x3 part is rotation times uniform scale */
static t3_vec3 inv_point(const t3_mat4 *m, t3_vec3 p) {
  t3_vec3 d = vsub(p, mpos(m));
  t3_vec3 c0 = v3(m->e[0], m->e[1], m->e[2]), c1 = v3(m->e[4], m->e[5], m->e[6]), c2 = v3(m->e[8], m->e[9], m->e[10]);
  float s2 = vdot(c0, c0);
  return s2 > 0 ? v3(vdot(c0, d) / s2, vdot(c1, d) / vdot(c1, c1), vdot(c2, d) / vdot(c2, c2)) : d;
}

/* ── poses ─────────────────────────────────────────────────────────── */

static void pose_alloc(cw_pose *p, int n) {
  memset(p, 0, sizeof *p);
  p->count = n;
  size_t c = (size_t)(n ? n : 1);
  p->parent = cw_calloc(c, sizeof(int), CW_MEM_ANIM);
  p->name = cw_calloc(c, sizeof(char *), CW_MEM_ANIM);
  p->bone = cw_calloc(c, sizeof(t3_object *), CW_MEM_ANIM);
  p->pos = cw_calloc(c, sizeof(t3_vec3), CW_MEM_ANIM);
  p->rot = cw_calloc(c, sizeof(t3_quat), CW_MEM_ANIM);
  p->scl = cw_calloc(c, sizeof(t3_vec3), CW_MEM_ANIM);
  p->rest_pos = cw_calloc(c, sizeof(t3_vec3), CW_MEM_ANIM);
  p->rest_rot = cw_calloc(c, sizeof(t3_quat), CW_MEM_ANIM);
  p->rest_scl = cw_calloc(c, sizeof(t3_vec3), CW_MEM_ANIM);
  p->base = cw_calloc(c, sizeof(t3_mat4), CW_MEM_ANIM);
  p->base_rot = cw_calloc(c, sizeof(t3_quat), CW_MEM_ANIM);
  p->model = cw_calloc(c, sizeof(t3_mat4), CW_MEM_ANIM);
  p->mrot = cw_calloc(c, sizeof(t3_quat), CW_MEM_ANIM);
  for (int i = 0; i < n; i++) {
    p->parent[i] = -1;
    p->rot[i] = p->rest_rot[i] = p->base_rot[i] = p->mrot[i] = (t3_quat){ 0, 0, 0, 1 };
    p->scl[i] = p->rest_scl[i] = v3(1, 1, 1);
    identity(&p->base[i]);
    identity(&p->model[i]);
  }
}

void cw_pose_init(cw_pose *p, int count) { pose_alloc(p, count < 0 ? 0 : count); }

void cw_pose_free(cw_pose *p) {
  if (!p) return;
  void *arrs[] = { p->parent, p->name, p->bone, p->pos, p->rot, p->scl, p->rest_pos, p->rest_rot,
                   p->rest_scl, p->base, p->base_rot, p->model, p->mrot };
  for (size_t i = 0; i < sizeof arrs / sizeof arrs[0]; i++) cw_free(arrs[i]);
  memset(p, 0, sizeof *p);
}

static bool is_mesh_like(const t3_object *o) {
  switch (o->type) {
  case T3_MESH: case T3_INSTANCED_MESH: case T3_SKINNED_MESH: case T3_LINE: case T3_LINE_SEGMENTS: case T3_LINE_LOOP:
  case T3_SPRITE: case T3_POINTS: case T3_LOD: case T3_PERSPECTIVE_CAMERA: case T3_ORTHOGRAPHIC_CAMERA:
  case T3_AMBIENT_LIGHT: case T3_HEMISPHERE_LIGHT: case T3_DIRECTIONAL_LIGHT: case T3_POINT_LIGHT: case T3_SPOT_LIGHT:
    return true;
  default: return false;
  }
}

/* depth first: bones, and non-mesh objects under a bone */
static int collect(t3_object *o, bool under_bone, t3_object **out, int *parent, int pidx, int n, int cap) {
  bool take = o->type == T3_BONE || (under_bone && !is_mesh_like(o));
  int me = pidx;
  if (take) {
    if (out && n < cap) { out[n] = o; parent[n] = pidx; }
    me = n++;
  }
  for (int i = 0; i < o->child_count; i++)
    n = collect(o->children[i], take || under_bone, out, parent, me, n, cap);
  return n;
}

bool cw_pose_bind(cw_pose *p, t3_object *root) {
  memset(p, 0, sizeof *p);
  if (!root) return false;
  int n = 0;
  for (int i = 0; i < root->child_count; i++) n = collect(root->children[i], false, NULL, NULL, -1, n, 0);
  if (!n) {
    cw_warn("cw_pose_bind: '%s' has no bones (skeleton modifiers need a skinned glTF model under the node)",
            root->name ? root->name : "(unnamed)");
    return false;
  }
  pose_alloc(p, n);
  int k = 0;
  for (int i = 0; i < root->child_count; i++) k = collect(root->children[i], false, p->bone, p->parent, -1, k, n);
  p->root = root;
  for (int i = 0; i < n; i++) p->name[i] = p->bone[i]->name;
  cw_pose_capture(p);
  cw_pose_set_rest(p);
  return true;
}

int cw_pose_find(const cw_pose *p, const char *name) {
  if (!p || !name || !name[0]) return -1;
  for (int i = 0; i < p->count; i++)
    if (p->name[i] && !strcmp(p->name[i], name)) return i;
  return -1;
}

void cw__pose_capture_locals(cw_pose *p) {
  for (int i = 0; i < p->count; i++) {
    t3_object *o = p->bone[i];
    if (!o) continue;
    p->pos[i] = o->position;
    p->rot[i] = o->quaternion;
    p->scl[i] = o->scale;
    if (p->parent[i] < 0) {
      /* the objects between the root and this bone, from their fields */
      t3_object *chain[64];
      int c = 0;
      for (t3_object *a = o->parent; a && a != p->root && c < 64; a = a->parent) chain[c++] = a;
      identity(&p->base[i]);
      p->base_rot[i] = (t3_quat){ 0, 0, 0, 1 };
      for (int j = c - 1; j >= 0; j--) {
        t3_mat4 l;
        compose(&l, chain[j]->position, chain[j]->quaternion, chain[j]->scale);
        mmul(&p->base[i], &p->base[i], &l);
        p->base_rot[i] = qmul(p->base_rot[i], chain[j]->quaternion);
      }
    }
  }
}

void cw_pose_capture(cw_pose *p) {
  cw__pose_capture_locals(p);
  cw_pose_update_model(p);
}

void cw_pose_flush(const cw_pose *p) {
  for (int i = 0; i < p->count; i++) {
    t3_object *o = p->bone[i];
    if (!o) continue;
    o->position = p->pos[i];
    o->scale = p->scl[i];
    t3_object_set_quaternion(o, p->rot[i]);
  }
}

void cw_pose_set_rest(cw_pose *p) {
  memcpy(p->rest_pos, p->pos, sizeof(t3_vec3) * (size_t)p->count);
  memcpy(p->rest_rot, p->rot, sizeof(t3_quat) * (size_t)p->count);
  memcpy(p->rest_scl, p->scl, sizeof(t3_vec3) * (size_t)p->count);
}

static void model_one(cw_pose *p, int i) {
  t3_mat4 l;
  compose(&l, p->pos[i], p->rot[i], p->scl[i]);
  int pa = p->parent[i];
  if (pa >= 0) {
    mmul(&p->model[i], &p->model[pa], &l);
    p->mrot[i] = qmul(p->mrot[pa], p->rot[i]);
  } else {
    mmul(&p->model[i], &p->base[i], &l);
    p->mrot[i] = qmul(p->base_rot[i], p->rot[i]);
  }
}

void cw_pose_update_model(cw_pose *p) {
  for (int i = 0; i < p->count; i++) model_one(p, i);
}

void cw_pose_update_from(cw_pose *p, int bone) {
  if (bone < 0 || bone >= p->count) return;
  model_one(p, bone);
  /* descendants follow their parent in the array: one pass marking them */
  uint8_t mark[512];
  uint8_t *m = p->count - bone <= (int)sizeof mark ? mark : cw_calloc((size_t)(p->count - bone), 1, CW_MEM_ANIM);
  m[0] = 1;
  for (int i = bone + 1; i < p->count; i++) {
    int pa = p->parent[i];
    m[i - bone] = pa >= bone && m[pa - bone];
    if (m[i - bone]) model_one(p, i);
  }
  if (m != mark) cw_free(m);
}

t3_vec3 cw_pose_model_position(const cw_pose *p, int bone) {
  return bone >= 0 && bone < p->count ? mpos(&p->model[bone]) : v3(0, 0, 0);
}
t3_quat cw_pose_model_rotation(const cw_pose *p, int bone) {
  return bone >= 0 && bone < p->count ? p->mrot[bone] : (t3_quat){ 0, 0, 0, 1 };
}

bool cw_pose_is_descendant(const cw_pose *p, int bone, int ancestor) {
  for (int b = bone; b >= 0; b = p->parent[b])
    if (b == ancestor) return true;
  return false;
}

static t3_quat parent_mrot(const cw_pose *p, int i) {
  return p->parent[i] >= 0 ? p->mrot[p->parent[i]] : p->base_rot[i];
}

void cw_pose_set_model_rotation(cw_pose *p, int bone, t3_quat q) {
  if (bone < 0 || bone >= p->count) return;
  p->rot[bone] = qnorm(qmul(qconj(parent_mrot(p, bone)), q));
  cw_pose_update_from(p, bone);
}

void cw_pose_rotate_model(cw_pose *p, int bone, t3_quat delta) {
  if (bone < 0 || bone >= p->count) return;
  cw_pose_set_model_rotation(p, bone, qmul(delta, p->mrot[bone]));
}

void cw_pose_translate_model(cw_pose *p, int bone, t3_vec3 delta) {
  if (bone < 0 || bone >= p->count) return;
  const t3_mat4 *pm = p->parent[bone] >= 0 ? &p->model[p->parent[bone]] : &p->base[bone];
  /* the offset in the parent's space: inverse of its rotation-and-scale */
  t3_vec3 o = mpos(pm);
  t3_vec3 local = inv_point(pm, vadd(o, delta));
  p->pos[bone] = vadd(p->pos[bone], local);
  cw_pose_update_from(p, bone);
}

void cw_pose_blend(cw_pose *p, const cw_pose *to, float w) {
  if (w <= 0) return;
  int n = p->count < to->count ? p->count : to->count;
  /* only bones that differ change; model transforms of them and below */
  uint8_t mark[512];
  uint8_t *dirty = n <= (int)sizeof mark ? mark : cw_calloc((size_t)n, 1, CW_MEM_ANIM);
  for (int i = 0; i < n; i++) {
    bool dp = memcmp(&p->pos[i], &to->pos[i], sizeof(t3_vec3)) != 0;
    bool dr = memcmp(&p->rot[i], &to->rot[i], sizeof(t3_quat)) != 0;
    bool ds = memcmp(&p->scl[i], &to->scl[i], sizeof(t3_vec3)) != 0;
    if (w >= 1) {
      p->pos[i] = to->pos[i];
      p->rot[i] = to->rot[i];
      p->scl[i] = to->scl[i];
    } else {
      if (dp) p->pos[i] = vadd(p->pos[i], vmul(vsub(to->pos[i], p->pos[i]), w));
      if (ds) p->scl[i] = vadd(p->scl[i], vmul(vsub(to->scl[i], p->scl[i]), w));
      if (dr) p->rot[i] = qslerp(p->rot[i], to->rot[i], w);
    }
    dirty[i] = dp || dr || ds || (p->parent[i] >= 0 && dirty[p->parent[i]]);
    if (dirty[i]) model_one(p, i);
  }
  if (dirty != mark) cw_free(dirty);
}

void cw_pose_copy_locals(cw_pose *dst, const cw_pose *src) {
  int n = dst->count < src->count ? dst->count : src->count;
  memcpy(dst->pos, src->pos, sizeof(t3_vec3) * (size_t)n);
  memcpy(dst->rot, src->rot, sizeof(t3_quat) * (size_t)n);
  memcpy(dst->scl, src->scl, sizeof(t3_vec3) * (size_t)n);
}

uint32_t cw_pose_hash(const cw_pose *p, uint32_t h) {
  h = cw_hash32(p->pos, sizeof(t3_vec3) * (size_t)p->count, h);
  h = cw_hash32(p->rot, sizeof(t3_quat) * (size_t)p->count, h);
  return cw_hash32(p->scl, sizeof(t3_vec3) * (size_t)p->count, h);
}

/* ── two-bone ──────────────────────────────────────────────────────── */

/* turn bone i (model space) so the direction from it to `from` points at `to` */
static void aim_dir(cw_pose *p, int i, t3_vec3 from_dir, t3_vec3 to_dir) {
  t3_vec3 a = vnorm(from_dir), b = vnorm(to_dir);
  if (vdot(a, a) == 0 || vdot(b, b) == 0) return;
  cw_pose_rotate_model(p, i, cw_quat_from_to(a, b));
}

static t3_quat local_rot_of(const cw_pose *p, int i) { return p->rot[i]; }

bool cw_ik_two_bone(cw_pose *p, int a, int b, int c, t3_vec3 target, const cw_ik2_opts *o) {
  cw_ik2_opts def = CW_IK2_OPTS_DEFAULT;
  if (!o) o = &def;
  if (a < 0 || b < 0 || c < 0 || a >= p->count || b >= p->count || c >= p->count) return false;
  if (o->weight <= 0) return false;
  t3_quat in_a = local_rot_of(p, a), in_b = local_rot_of(p, b), in_c = local_rot_of(p, c);
  t3_vec3 pa = cw_pose_model_position(p, a), pb = cw_pose_model_position(p, b), pc = cw_pose_model_position(p, c);
  float lab = vlen(vsub(pb, pa)), lbc = vlen(vsub(pc, pb));
  t3_vec3 at = vsub(target, pa);
  float dist = vlen(at);
  float maxr = lab + lbc, minr = lab > lbc ? lab - lbc : lbc - lab;
  bool reached = dist <= maxr * (1 + 1e-5f) && dist >= minr * (1 - 1e-5f);
  if (lab <= 0 || lbc <= 0) return false;
  t3_vec3 u = dist > 1e-8f ? vmul(at, 1 / dist) : vnorm(vsub(pc, pa));
  if (vdot(u, u) == 0) return false;
  /* soften: the reach eases toward full extension (ozz IkTwoBoneJob) */
  float d = dist;
  if (o->soften > 0 && o->soften < 1) {
    float start = o->soften * maxr;
    if (d > start) {
      float range = maxr - start;
      d = start + range * (1 - cw_exp(-(d - start) / range));
    }
  }
  float eps = maxr * 1e-6f;
  if (d > maxr - eps) d = maxr - eps;
  if (d < minr + eps) d = minr + eps;
  /* the bend plane: the pole, else the incoming middle joint */
  t3_vec3 pole = o->use_pole ? vsub(o->pole, pa) : vsub(pb, pa);
  t3_vec3 v = vsub(pole, vmul(u, vdot(pole, u)));
  if (vdot(v, v) < 1e-12f) {
    /* pole on the reach line: the incoming bend, then any perpendicular */
    t3_vec3 alt = vsub(pb, pa);
    v = vsub(alt, vmul(u, vdot(alt, u)));
    if (vdot(v, v) < 1e-12f) v = any_perp(u);
  }
  v = vnorm(v);
  /* law of cosines, no trig: the middle joint's new position */
  float cos_a = (lab * lab + d * d - lbc * lbc) / (2 * lab * d);
  if (cos_a > 1) cos_a = 1;
  if (cos_a < -1) cos_a = -1;
  float sin_a = sqrtf(1 - cos_a * cos_a);
  t3_vec3 nb = vadd(pa, vadd(vmul(u, lab * cos_a), vmul(v, lab * sin_a)));
  t3_vec3 nc = vadd(pa, vmul(u, d));
  aim_dir(p, a, vsub(pb, pa), vsub(nb, pa));
  pb = cw_pose_model_position(p, b);
  pc = cw_pose_model_position(p, c);
  aim_dir(p, b, vsub(pc, pb), vsub(nc, pb));
  if (o->use_end_rotation) cw_pose_set_model_rotation(p, c, qnorm(o->end_rotation));
  if (o->weight < 1) {
    p->rot[a] = qslerp(in_a, p->rot[a], o->weight);
    p->rot[b] = qslerp(in_b, p->rot[b], o->weight);
    p->rot[c] = qslerp(in_c, p->rot[c], o->weight);
    cw_pose_update_from(p, a);
  }
  return reached;
}

/* ── aim ───────────────────────────────────────────────────────────── */

bool cw_ik_aim(cw_pose *p, int bone, t3_vec3 target, const cw_aim_opts *o) {
  cw_aim_opts def = CW_AIM_OPTS_DEFAULT;
  if (!o) o = &def;
  if (bone < 0 || bone >= p->count || o->weight <= 0) return false;
  t3_quat in = p->rot[bone];
  t3_quat mr = p->mrot[bone];
  t3_vec3 fwd = vnorm(qrot(mr, o->forward));
  if (vdot(fwd, fwd) == 0) return false;
  /* the aim origin: the joint plus the offset (scaled as the bone is) */
  const t3_mat4 *m = &p->model[bone];
  t3_vec3 origin = vadd(mpos(m), mdir(m, o->offset));
  t3_vec3 want = vnorm(vsub(target, origin));
  if (vdot(want, want) == 0) return false;
  bool inside = true;
  if (o->limit_angle > 0 && o->limit_angle < 3.14159265f) {
    float cl, sl;
    cw_sincos(o->limit_angle, &sl, &cl);
    float c = vdot(fwd, want);
    if (c < cl) {
      inside = false;
      /* the direction at the limit, in the plane of fwd and want */
      t3_vec3 side = vsub(want, vmul(fwd, c));
      if (vdot(side, side) < 1e-12f) side = any_perp(fwd);
      side = vnorm(side);
      want = vnorm(vadd(vmul(fwd, cl), vmul(side, sl)));
    }
  }
  cw_pose_rotate_model(p, bone, cw_quat_from_to(fwd, want));
  if (o->weight < 1) {
    p->rot[bone] = qslerp(in, p->rot[bone], o->weight);
    cw_pose_update_from(p, bone);
  }
  return inside;
}

/* ── CCD (three.js r186 CCDIKSolver) ───────────────────────────────── */

/* three.js Euler.setFromQuaternion(q, 'XYZ') via the rotation matrix */
static t3_vec3 euler_xyz(t3_quat q) {
  float x = q.x, y = q.y, z = q.z, w = q.w;
  float x2 = x + x, y2 = y + y, z2 = z + z;
  float xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
  float wx = w * x2, wy = w * y2, wz = w * z2;
  float m11 = 1 - (yy + zz), m12 = xy - wz, m13 = xz + wy;
  float m22 = 1 - (xx + zz), m23 = yz - wx;
  float m32 = yz + wx, m33 = 1 - (xx + yy);
  t3_vec3 e;
  float c13 = m13 < -1 ? -1 : m13 > 1 ? 1 : m13;
  e.y = cw_asin(c13);
  if ((m13 < 0 ? -m13 : m13) < 0.9999999f) {
    e.x = cw_atan2(-m23, m33);
    e.z = cw_atan2(-m12, m11);
  } else {
    e.x = cw_atan2(m32, m22);
    e.z = 0;
  }
  return e;
}
/* three.js Quaternion.setFromEuler, order XYZ */
static t3_quat quat_xyz(t3_vec3 e) {
  float s1, c1, s2, c2, s3, c3;
  cw_sincos(e.x / 2, &s1, &c1);
  cw_sincos(e.y / 2, &s2, &c2);
  cw_sincos(e.z / 2, &s3, &c3);
  return (t3_quat){ s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3,
                    c1 * c2 * c3 - s1 * s2 * s3 };
}

static void ccd_one(cw_pose *p, const cw_ccdik *ik, float override_blend) {
  float chain_blend = ik->has_blend_factor ? ik->blend_factor : override_blend;
  if (ik->effector < 0 || ik->effector >= p->count) return;
  t3_vec3 target = ik->target >= 0 && ik->target < p->count ? cw_pose_model_position(p, ik->target) : ik->target_position;
  int iteration = ik->iteration > 0 ? ik->iteration : 1;
  t3_quat initial[CW_IK_MAX_CHAIN];
  int nl = ik->link_count < CW_IK_MAX_CHAIN ? ik->link_count : CW_IK_MAX_CHAIN;
  if (chain_blend < 1)
    for (int j = 0; j < nl; j++) initial[j] = p->rot[ik->links[j].index];
  for (int it = 0; it < iteration; it++) {
    bool rotated = false;
    for (int j = 0; j < nl; j++) {
      const cw_ccdik_link *lk = &ik->links[j];
      int li = lk->index;
      if (lk->disabled) break;
      if (li < 0 || li >= p->count) continue;
      t3_vec3 link_pos = cw_pose_model_position(p, li);
      t3_quat inv = qconj(p->mrot[li]);
      t3_vec3 eff = cw_pose_model_position(p, ik->effector);
      t3_vec3 ev = vnorm(qrot(inv, vsub(eff, link_pos)));
      t3_vec3 tv = vnorm(qrot(inv, vsub(target, link_pos)));
      /* r186: acos(clamp(dot)). Its three.js runs in doubles; in floats acos
       * near 1 loses half the bits (~3e-4 rad at 1e-7 from 1), so the same
       * angle comes from atan2(|cross|, dot), exact in float at small angles. */
      float angle = cw_atan2(vlen(vcross(ev, tv)), vdot(tv, ev));
      if (angle < 1e-5f) continue;
      if (ik->has_min_angle && angle < ik->min_angle) angle = ik->min_angle;
      if (ik->has_max_angle && angle > ik->max_angle) angle = ik->max_angle;
      t3_vec3 axis = vnorm(vcross(ev, tv));
      t3_quat q = qmul(p->rot[li], cw_quat_axis_angle_det(axis, angle));
      if (lk->has_limitation) {
        float c = q.w;
        if (c > 1) c = 1;
        float dot = q.x * lk->limitation.x + q.y * lk->limitation.y + q.z * lk->limitation.z;
        float sign = dot < 0 ? -1.0f : 1.0f;
        float c2 = sign * sqrtf(1 - c * c);
        q = (t3_quat){ lk->limitation.x * c2, lk->limitation.y * c2, lk->limitation.z * c2, c };
      }
      if (lk->has_rotation_min) {
        t3_vec3 e = euler_xyz(q);
        e = v3(e.x > lk->rotation_min.x ? e.x : lk->rotation_min.x, e.y > lk->rotation_min.y ? e.y : lk->rotation_min.y,
               e.z > lk->rotation_min.z ? e.z : lk->rotation_min.z);
        q = quat_xyz(e);
      }
      if (lk->has_rotation_max) {
        t3_vec3 e = euler_xyz(q);
        e = v3(e.x < lk->rotation_max.x ? e.x : lk->rotation_max.x, e.y < lk->rotation_max.y ? e.y : lk->rotation_max.y,
               e.z < lk->rotation_max.z ? e.z : lk->rotation_max.z);
        q = quat_xyz(e);
      }
      p->rot[li] = q;
      cw_pose_update_from(p, li);
      rotated = true;
    }
    if (!rotated) break;
  }
  if (chain_blend < 1) {
    for (int j = 0; j < nl; j++) {
      int li = ik->links[j].index;
      if (li < 0 || li >= p->count) continue;
      p->rot[li] = qslerp(initial[j], p->rot[li], chain_blend);
      cw_pose_update_from(p, li);
    }
  }
}

void cw_ccdik_solve(cw_pose *p, const cw_ccdik *iks, int n, float blend) {
  for (int i = 0; i < n; i++) ccd_one(p, &iks[i], blend);
}

/* ── FABRIK ────────────────────────────────────────────────────────── */

/* keep direction d within the cone of half-angle h around unit axis ref */
static t3_vec3 cone_limit(t3_vec3 d, t3_vec3 ref, float h) {
  float sl, cl;
  cw_sincos(h, &sl, &cl);
  float c = vdot(d, ref);
  if (c >= cl) return d;
  t3_vec3 side = vsub(d, vmul(ref, c));
  if (vdot(side, side) < 1e-12f) side = any_perp(ref);
  side = vnorm(side);
  return vnorm(vadd(vmul(ref, cl), vmul(side, sl)));
}

int cw_ik_fabrik(cw_pose *p, const int *chain, int n, t3_vec3 target, const cw_chain_opts *o) {
  cw_chain_opts def = CW_CHAIN_OPTS_DEFAULT;
  if (!o) o = &def;
  if (n < 2 || n > CW_IK_MAX_CHAIN || o->weight <= 0) return 0;
  for (int i = 0; i < n; i++)
    if (chain[i] < 0 || chain[i] >= p->count) return 0;
  t3_vec3 pt[CW_IK_MAX_CHAIN];
  float len[CW_IK_MAX_CHAIN];
  t3_quat in[CW_IK_MAX_CHAIN];
  t3_vec3 hinge[CW_IK_MAX_CHAIN];
  float total = 0;
  for (int i = 0; i < n; i++) {
    pt[i] = cw_pose_model_position(p, chain[i]);
    in[i] = p->rot[chain[i]];
    if (i) total += len[i - 1] = vlen(vsub(pt[i], pt[i - 1]));
  }
  /* hinge axes in model space, from the incoming pose */
  for (int i = 0; i < n; i++) {
    hinge[i] = v3(0, 0, 0);
    if (o->joints && o->joints[i].kind == CW_IK_JOINT_HINGE) {
      int par = i > 0 ? chain[i - 1] : p->parent[chain[0]];
      t3_quat pr = par >= 0 ? p->mrot[par] : p->base_rot[chain[0]];
      hinge[i] = vnorm(qrot(pr, o->joints[i].axis));
    }
  }
  t3_vec3 root = pt[0];
  t3_vec3 root_dir_in = vnorm(vsub(pt[1], pt[0]));
  int max_it = o->max_iterations > 0 ? o->max_iterations : 4;
  float tol = o->min_distance > 0 ? o->min_distance : 0.001f;
  int it = 0;
  float reach = vlen(vsub(target, root));
  if (reach >= total) {
    /* out of reach: stretch straight toward the target (constraints applied below) */
    t3_vec3 dir = vnorm(vsub(target, root));
    for (int i = 1; i < n; i++) pt[i] = vadd(pt[i - 1], vmul(dir, len[i - 1]));
    it = 1;
  }
  for (; it < max_it; it++) {
    if (vlen(vsub(pt[n - 1], target)) <= tol) break;
    /* backward: from the end at the target toward the root */
    pt[n - 1] = target;
    for (int i = n - 2; i >= 0; i--) {
      t3_vec3 dir = vnorm(vsub(pt[i], pt[i + 1]));
      if (vdot(dir, dir) == 0) dir = vnorm(vsub(pt[i], pt[n - 1]));
      pt[i] = vadd(pt[i + 1], vmul(dir, len[i]));
    }
    /* forward: from the root back out, with constraints */
    pt[0] = root;
    t3_vec3 prev = root_dir_in;
    for (int i = 0; i < n - 1; i++) {
      t3_vec3 dir = vnorm(vsub(pt[i + 1], pt[i]));
      if (vdot(dir, dir) == 0) dir = prev;
      if (o->joints) {
        const cw_ik_joint *j = &o->joints[i];
        if (j->kind == CW_IK_JOINT_CONE && i > 0) dir = cone_limit(dir, prev, j->half_angle);
        else if (j->kind == CW_IK_JOINT_HINGE && vdot(hinge[i], hinge[i]) > 0) {
          t3_vec3 pr = vsub(dir, vmul(hinge[i], vdot(dir, hinge[i])));
          dir = vdot(pr, pr) > 1e-12f ? vnorm(pr) : prev;
        }
      }
      pt[i + 1] = vadd(pt[i], vmul(dir, len[i]));
      prev = dir;
    }
  }
  /* positions -> rotations, root first */
  for (int i = 0; i < n - 1; i++) {
    t3_vec3 cur = vsub(cw_pose_model_position(p, chain[i + 1]), cw_pose_model_position(p, chain[i]));
    aim_dir(p, chain[i], cur, vsub(pt[i + 1], pt[i]));
  }
  if (o->weight < 1) {
    for (int i = 0; i < n; i++) p->rot[chain[i]] = qslerp(in[i], p->rot[chain[i]], o->weight);
    cw_pose_update_from(p, chain[0]);
  }
  return it;
}

/* Animation resources: tracks, keys, deterministic sampling, key sweeps. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"

static cw_pool pool;
static bool pool_ready;

anim_rec *cw__anim(cw_anim a) { return pool_ready ? cw_pool_get(&pool, a.id) : NULL; }

cw_anim cw_anim_new(const char *name, float length, cw_loop_mode loop) {
  if (!pool_ready) { cw_pool_init(&pool, sizeof(anim_rec), CW_MEM_ANIM); pool_ready = true; }
  uint64_t id;
  anim_rec *r = cw_pool_alloc(&pool, &id);
  r->refs = 1;
  r->name = cw_intern(name ? name : "");
  r->length = length > 0 ? length : 0;
  r->loop = loop;
  r->stored = true;
  return (cw_anim){ id };
}

void cw_anim_retain(cw_anim a) {
  anim_rec *r = cw__anim(a);
  if (r) r->refs++;
}

void cw_anim_release(cw_anim a) {
  anim_rec *r = cw__anim(a);
  if (!r || --r->refs > 0) return;
  for (int i = 0; i < r->tracks.count; i++) {
    anim_track *t = &r->tracks.data[i];
    cw_array_free(&t->times);
    cw_array_free(&t->values);
    cw_array_free(&t->tangents);
    cw_array_free(&t->mkeys);
  }
  cw_array_free(&r->tracks);
  cw_pool_release(&pool, a.id);
}

bool cw_anim_valid(cw_anim a) { return cw__anim(a) != NULL; }
const char *cw_anim_name(cw_anim a) { anim_rec *r = cw__anim(a); return r ? r->name : NULL; }
float cw_anim_length(cw_anim a) { anim_rec *r = cw__anim(a); return r ? r->length : 0; }
void cw_anim_set_length(cw_anim a, float len) { anim_rec *r = cw__anim(a); if (r) r->length = len > 0 ? len : 0; }
cw_loop_mode cw_anim_loop(cw_anim a) { anim_rec *r = cw__anim(a); return r ? r->loop : CW_LOOP_NONE; }
void cw_anim_set_loop(cw_anim a, cw_loop_mode l) { anim_rec *r = cw__anim(a); if (r) r->loop = l; }
void cw_anim_set_stored(cw_anim a, bool s) { anim_rec *r = cw__anim(a); if (r) r->stored = s; }

/* ── tracks ─────────────────────────────────────────────────────────── */

static anim_track *track_of(cw_anim a, int i, const char *fn) {
  anim_rec *r = cw__anim(a);
  if (!r) { cw_warn("%s: stale animation handle", fn); return NULL; }
  if (i < 0 || i >= r->tracks.count) { cw_warn("%s: '%s' has no track %d (it has %d)", fn, r->name, i, r->tracks.count); return NULL; }
  return &r->tracks.data[i];
}

/* "A/B@bone:prop" -> node "A/B", bone, prop */
static void split_path(anim_track *t, const char *path) {
  const char *colon = strrchr(path, ':');
  const char *nodeend = colon ? colon : path + strlen(path);
  const char *at = memchr(path, '@', (size_t)(nodeend - path));
  t->node = cw_intern_n(path, (size_t)((at ? at : nodeend) - path));
  t->bone = at ? cw_intern_n(at + 1, (size_t)(nodeend - at - 1)) : NULL;
  t->prop = colon ? cw_intern(colon + 1) : NULL;
}

static int add_track(cw_anim a, cw_track_kind kind, const char *path, cw_interp interp) {
  anim_rec *r = cw__anim(a);
  if (!r) { cw_warn("cw_anim_add_*_track: stale animation handle"); return -1; }
  anim_track t = { 0 };
  t.kind = kind;
  t.path = cw_intern(path ? path : "");
  t.interp = interp;
  t.enabled = true;
  split_path(&t, t.path);
  if (kind == CW_TRACK_VALUE && !t.prop)
    cw_warn("animation '%s': value track '%s' has no ':property' (write \"Node:position\")", r->name, t.path);
  if (kind != CW_TRACK_VALUE) t.prop = NULL;
  cw_array_push(&r->tracks, t);
  r->version++;
  return r->tracks.count - 1;
}

int cw_anim_add_value_track(cw_anim a, const char *path, cw_interp interp) { return add_track(a, CW_TRACK_VALUE, path, interp); }
int cw_anim_add_method_track(cw_anim a, const char *node_path) { return add_track(a, CW_TRACK_METHOD, node_path, CW_INTERP_NEAREST); }
int cw_anim_add_event_track(cw_anim a) { return add_track(a, CW_TRACK_EVENT, "", CW_INTERP_NEAREST); }

void cw_anim_track_set_rot_mode(cw_anim a, int i, cw_rot_mode m) {
  anim_track *t = track_of(a, i, "cw_anim_track_set_rot_mode");
  if (t) t->rot = m;
}
void cw_anim_track_set_enabled(cw_anim a, int i, bool e) {
  anim_track *t = track_of(a, i, "cw_anim_track_set_enabled");
  if (t) t->enabled = e;
}
int cw_anim_track_count(cw_anim a) { anim_rec *r = cw__anim(a); return r ? r->tracks.count : 0; }
cw_track_kind cw_anim_track_kind(cw_anim a, int i) { anim_track *t = track_of(a, i, "cw_anim_track_kind"); return t ? t->kind : CW_TRACK_VALUE; }
const char *cw_anim_track_path(cw_anim a, int i) { anim_track *t = track_of(a, i, "cw_anim_track_path"); return t ? t->path : NULL; }
int cw_anim_track_find(cw_anim a, const char *path) {
  anim_rec *r = cw__anim(a);
  if (!r) return -1;
  const char *p = cw_intern(path);
  for (int i = 0; i < r->tracks.count; i++)
    if (r->tracks.data[i].path == p) return i;
  return -1;
}
int cw_anim_key_count(cw_anim a, int i) { anim_track *t = track_of(a, i, "cw_anim_key_count"); return t ? t->times.count : 0; }
float cw_anim_key_time(cw_anim a, int i, int k) {
  anim_track *t = track_of(a, i, "cw_anim_key_time");
  return t && k >= 0 && k < t->times.count ? t->times.data[k] : 0;
}

/* index where a key at `time` goes; *exists when one is already there */
static int key_slot(anim_track *t, float time, bool *exists) {
  int n = t->times.count;
  *exists = false;
  if (!n || t->times.data[n - 1] < time) return n;
  int lo = 0, hi = n;
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    if (t->times.data[mid] < time) lo = mid + 1;
    else hi = mid;
  }
  *exists = lo < n && t->times.data[lo] == time;
  return lo;
}

#define INSERT_AT(arr, i, val) \
  do { \
    cw_array_push((arr), (val)); \
    memmove((arr)->data + (i) + 1, (arr)->data + (i), sizeof *(arr)->data * (size_t)((arr)->count - 1 - (i))); \
    (arr)->data[(i)] = (val); \
  } while (0)

static void insert_value(cw_anim a, int i, float time, cw_value v, const cw_value *tan, const char *fn) {
  anim_track *t = track_of(a, i, fn);
  if (!t) return;
  if (t->kind != CW_TRACK_VALUE) { cw_warn("%s: track %d of '%s' is not a value track", fn, i, cw_anim_name(a)); return; }
  bool exists;
  int k = key_slot(t, time, &exists);
  if (v.type == CW_T_STRING) v.s = cw_intern(v.s ? v.s : "");
  cw_value in = tan ? tan[0] : (cw_value){ 0 }, out = tan ? tan[1] : (cw_value){ 0 };
  if (exists) {
    t->values.data[k] = v;
    if (t->tangents.count) { t->tangents.data[2 * k] = in; t->tangents.data[2 * k + 1] = out; }
  } else {
    INSERT_AT(&t->times, k, time);
    INSERT_AT(&t->values, k, v);
    if (tan || t->tangents.count) {
      /* tangents are kept for every key once any key has them */
      while (t->tangents.count < 2 * (t->values.count - 1)) cw_array_push(&t->tangents, (cw_value){ 0 });
      INSERT_AT(&t->tangents, 2 * k, out);
      INSERT_AT(&t->tangents, 2 * k, in);
    }
  }
  cw__anim(a)->version++;
}

void cw_anim_add_key(cw_anim a, int i, float time, cw_value v) { insert_value(a, i, time, v, NULL, "cw_anim_add_key"); }
void cw_anim_add_key_hermite(cw_anim a, int i, float time, cw_value v, cw_value in, cw_value out) {
  cw_value tan[2] = { in, out };
  insert_value(a, i, time, v, tan, "cw_anim_add_key_hermite");
}

static void insert_mkey(cw_anim a, int i, float time, const char *name, const cw_value *args, int argc, const char *fn) {
  anim_track *t = track_of(a, i, fn);
  if (!t) return;
  if (t->kind == CW_TRACK_VALUE) { cw_warn("%s: track %d of '%s' is a value track", fn, i, cw_anim_name(a)); return; }
  if (argc > CW_ANIM_MAX_ARGS) { cw_warn("%s: at most %d arguments per key", fn, CW_ANIM_MAX_ARGS); argc = CW_ANIM_MAX_ARGS; }
  anim_mkey mk = { cw_intern(name ? name : ""), argc };
  for (int k = 0; k < argc; k++) {
    mk.args[k] = args[k];
    if (mk.args[k].type == CW_T_STRING) mk.args[k].s = cw_intern(mk.args[k].s ? mk.args[k].s : "");
  }
  bool exists;
  int k = key_slot(t, time, &exists);
  if (exists) t->mkeys.data[k] = mk;
  else {
    INSERT_AT(&t->times, k, time);
    INSERT_AT(&t->mkeys, k, mk);
  }
  cw__anim(a)->version++;
}

void cw_anim_add_method_key(cw_anim a, int i, float time, const char *method, const cw_value *args, int argc) {
  insert_mkey(a, i, time, method, args, argc, "cw_anim_add_method_key");
}
int cw_anim_add_event(cw_anim a, float time, const char *name) {
  anim_rec *r = cw__anim(a);
  if (!r) return -1;
  int track = -1;
  for (int i = 0; i < r->tracks.count; i++)
    if (r->tracks.data[i].kind == CW_TRACK_EVENT) { track = i; break; }
  if (track < 0) track = cw_anim_add_event_track(a);
  cw_anim_add_event_key(a, track, time, name);
  return track;
}

int cw_anim_event_count(cw_anim a, const char *name) {
  anim_rec *r = cw__anim(a);
  if (!r) return 0;
  const char *in = cw_intern(name);
  int n = 0;
  for (int i = 0; i < r->tracks.count; i++)
    if (r->tracks.data[i].kind == CW_TRACK_EVENT)
      for (int k = 0; k < r->tracks.data[i].mkeys.count; k++) n += r->tracks.data[i].mkeys.data[k].name == in;
  return n;
}

float cw_anim_event_time(cw_anim a, const char *name, int nth) {
  anim_rec *r = cw__anim(a);
  if (!r) return -1;
  const char *in = cw_intern(name);
  float best = -1;
  int seen = 0;
  /* in time order across the event tracks: repeated passes pick the smallest time above the last one */
  float last = -1e30f;
  for (;;) {
    float next = 1e30f;
    bool found = false;
    for (int i = 0; i < r->tracks.count; i++) {
      const anim_track *t = &r->tracks.data[i];
      if (t->kind != CW_TRACK_EVENT) continue;
      for (int k = 0; k < t->mkeys.count; k++)
        if (t->mkeys.data[k].name == in && t->times.data[k] > last && t->times.data[k] < next) { next = t->times.data[k]; found = true; }
    }
    if (!found) return -1;
    if (seen++ == nth) { best = next; break; }
    last = next;
  }
  return best;
}

void cw_anim_add_event_key(cw_anim a, int i, float time, const char *name) { insert_mkey(a, i, time, name, NULL, 0, "cw_anim_add_event_key"); }

/* ── value math ─────────────────────────────────────────────────────── */

int cw__value_ncomp(cw_type t) {
  switch (t) {
  case CW_T_VEC2: return 2;
  case CW_T_VEC3: return 3;
  case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR: return 4;
  default: return 1;
  }
}

bool cw__value_interpolable(cw_type t) {
  return t == CW_T_FLOAT || t == CW_T_INT || t == CW_T_VEC2 || t == CW_T_VEC3 || t == CW_T_VEC4 || t == CW_T_QUAT ||
         t == CW_T_COLOR;
}

bool cw__value_coerce(cw_value *v, cw_type want) {
  if (want == CW_T_NONE || v->type == want) return true;
  if (want == CW_T_FLOAT && (v->type == CW_T_INT || v->type == CW_T_ENUM)) { v->f = (double)v->i; v->type = want; return true; }
  if ((want == CW_T_INT || want == CW_T_ENUM) && (v->type == CW_T_INT || v->type == CW_T_ENUM)) { v->type = want; return true; }
  if ((want == CW_T_INT || want == CW_T_ENUM) && v->type == CW_T_FLOAT) { v->i = (int64_t)floor(v->f + 0.5); v->type = want; return true; }
  if (want == CW_T_BOOL && v->type == CW_T_INT) { v->b = v->i != 0; v->type = want; return true; }
  if (want == CW_T_COLOR && (v->type == CW_T_VEC3 || v->type == CW_T_VEC4)) { if (v->type == CW_T_VEC3) v->v[3] = 1; v->type = want; return true; }
  if ((want == CW_T_VEC3 || want == CW_T_VEC4) && v->type == CW_T_COLOR) { v->type = want; return true; }
  if (want == CW_T_QUAT && v->type == CW_T_VEC4) { v->type = want; return true; }
  if (want == CW_T_VEC4 && v->type == CW_T_QUAT) { v->type = want; return true; }
  return false;
}

static void to_f(const cw_value *v, float f[4]) {
  switch (v->type) {
  case CW_T_FLOAT: f[0] = (float)v->f; break;
  case CW_T_INT: case CW_T_ENUM: f[0] = (float)v->i; break;
  case CW_T_BOOL: f[0] = v->b ? 1.0f : 0.0f; break;
  default: memcpy(f, v->v, 16); return;
  }
  f[1] = f[2] = f[3] = 0;
}

static void from_f(cw_value *v, cw_type t, const float f[4]) {
  v->type = t;
  if (t == CW_T_FLOAT) v->f = f[0];
  else if (t == CW_T_INT || t == CW_T_ENUM) v->i = (int64_t)floorf(f[0] + 0.5f);
  else memcpy(v->v, f, 16);
}

void cw_quat_slerp_det(float out[4], const float a[4], const float b[4], float t) {
  /* three.js Quaternion.slerpFlat with cw_atan2 / cw_sin (deterministic) */
  float x0 = a[0], y0 = a[1], z0 = a[2], w0 = a[3];
  const float x1 = b[0], y1 = b[1], z1 = b[2], w1 = b[3];
  if (t == 0) { out[0] = x0; out[1] = y0; out[2] = z0; out[3] = w0; return; }
  if (t == 1) { out[0] = x1; out[1] = y1; out[2] = z1; out[3] = w1; return; }
  if (w0 != w1 || x0 != x1 || y0 != y1 || z0 != z1) {
    float s = 1 - t;
    const float cs = x0 * x1 + y0 * y1 + z0 * z1 + w0 * w1, dir = cs >= 0 ? 1.0f : -1.0f, sqr_sin = 1 - cs * cs;
    if (sqr_sin > 2.220446049250313e-16f) {
      const float sn = sqrtf(sqr_sin), len = cw_atan2(sn, cs * dir);
      s = cw_sin(s * len) / sn;
      t = cw_sin(t * len) / sn;
    }
    const float tdir = t * dir;
    x0 = x0 * s + x1 * tdir;
    y0 = y0 * s + y1 * tdir;
    z0 = z0 * s + z1 * tdir;
    w0 = w0 * s + w1 * tdir;
    if (s == 1 - t) {
      const float f = 1 / sqrtf(x0 * x0 + y0 * y0 + z0 * z0 + w0 * w0);
      x0 *= f; y0 *= f; z0 *= f; w0 *= f;
    }
  }
  out[0] = x0; out[1] = y0; out[2] = z0; out[3] = w0;
}

static void quat_normalize(float q[4]) {
  float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (l == 0) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
  l = 1 / l;
  for (int i = 0; i < 4; i++) q[i] *= l;
}

void cw_quat_nlerp(float out[4], const float a[4], const float b[4], float t) {
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sb = d < 0 ? -1.0f : 1.0f, r[4];
  for (int i = 0; i < 4; i++) r[i] = a[i] + (b[i] * sb - a[i]) * t;
  quat_normalize(r);
  memcpy(out, r, sizeof r);
}

void cw__value_mix(cw_value *dst, const cw_value *src, float w, cw_rot_mode rot) {
  if (w <= 0) return;
  if (w >= 1) { *dst = *src; return; }
  cw_value s = *src;
  if (!cw__value_coerce(&s, dst->type) || !cw__value_interpolable(dst->type)) {
    if (w >= 0.5f) *dst = *src;
    return;
  }
  float a[4], b[4], r[4];
  to_f(dst, a);
  to_f(&s, b);
  if (dst->type == CW_T_QUAT) {
    if (rot == CW_ROT_NLERP) cw_quat_nlerp(r, a, b, w);
    else cw_quat_slerp_det(r, a, b, w);
  } else {
    for (int i = 0; i < 4; i++) r[i] = a[i] * (1 - w) + b[i] * w;
  }
  from_f(dst, dst->type, r);
}

/* ── sampling ───────────────────────────────────────────────────────── */

static float catmull(float p0, float p1, float p2, float p3, float t) {
  float t2 = t * t, t3 = t2 * t;
  return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

bool cw__track_sample(const anim_track *t, float time, cw_type want, cw_value *out) {
  int n = t->values.count;
  if (t->kind != CW_TRACK_VALUE || !n) return false;
  cw_type type = want != CW_T_NONE ? want : t->values.data[0].type;
#define KEYV(k, dst) \
  do { \
    dst = t->values.data[(k)]; \
    cw__value_coerce(&dst, type); \
  } while (0)
  if (n == 1 || time <= t->times.data[0]) { KEYV(0, *out); return true; }
  if (time >= t->times.data[n - 1]) { KEYV(n - 1, *out); return true; }
  int lo = 0, hi = n; /* first key with time > t */
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    if (t->times.data[mid] <= time) lo = mid + 1;
    else hi = mid;
  }
  int i1 = lo, i0 = lo - 1;
  cw_value a, b;
  KEYV(i0, a);
  KEYV(i1, b);
  if (t->interp == CW_INTERP_NEAREST || !cw__value_interpolable(type) || a.type != type || b.type != type) { *out = a; return true; }
  float t0 = t->times.data[i0], t1 = t->times.data[i1], td = t1 - t0, alpha = (time - t0) / td;
  float fa[4], fb[4], r[4];
  to_f(&a, fa);
  to_f(&b, fb);
  int nc = type == CW_T_QUAT ? 4 : cw__value_ncomp(type);
  if (t->interp == CW_INTERP_LINEAR) {
    if (type == CW_T_QUAT) {
      if (t->rot == CW_ROT_NLERP) cw_quat_nlerp(r, fa, fb, alpha);
      else cw_quat_slerp_det(r, fa, fb, alpha);
    } else {
      for (int k = 0; k < 4; k++) r[k] = fa[k] * (1 - alpha) + fb[k] * alpha;
    }
  } else if (t->interp == CW_INTERP_CUBIC) {
    cw_value pv, nv;
    KEYV(i0 > 0 ? i0 - 1 : i0, pv);
    KEYV(i1 + 1 < n ? i1 + 1 : i1, nv);
    float fp[4], fn[4];
    to_f(&pv, fp);
    to_f(&nv, fn);
    if (type == CW_T_QUAT) { /* keep neighbours in a's hemisphere */
      float *qs[3] = { fp, fb, fn };
      for (int q = 0; q < 3; q++) {
        float d = fa[0] * qs[q][0] + fa[1] * qs[q][1] + fa[2] * qs[q][2] + fa[3] * qs[q][3];
        if (d < 0) for (int k = 0; k < 4; k++) qs[q][k] = -qs[q][k];
      }
    }
    for (int k = 0; k < 4; k++) r[k] = catmull(fp[k], fa[k], fb[k], fn[k], alpha);
    if (type == CW_T_QUAT) quat_normalize(r);
  } else { /* HERMITE: value + tangent * td */
    float m0[4] = { 0 }, m1[4] = { 0 };
    if (t->tangents.count >= 2 * n) {
      cw_value o0 = t->tangents.data[2 * i0 + 1], in1 = t->tangents.data[2 * i1];
      cw__value_coerce(&o0, type);
      cw__value_coerce(&in1, type);
      to_f(&o0, m0);
      to_f(&in1, m1);
    }
    float p = alpha, p2 = p * p, p3 = p2 * p;
    float s2 = -2 * p3 + 3 * p2, s3 = p3 - p2, s0 = 1 - s2, s1 = s3 - p2 + p;
    for (int k = 0; k < 4; k++) r[k] = s0 * fa[k] + s1 * (m0[k] * td) + s2 * fb[k] + s3 * (m1[k] * td);
    if (type == CW_T_QUAT) quat_normalize(r);
  }
  (void)nc;
  from_f(out, type, r);
  if (type == CW_T_COLOR || type == CW_T_VEC2 || type == CW_T_VEC3) {
    for (int k = cw__value_ncomp(type); k < 4; k++) out->v[k] = type == CW_T_COLOR ? out->v[k] : 0;
  }
#undef KEYV
  return true;
}

bool cw_anim_sample(cw_anim a, int i, float time, cw_value *out) {
  anim_track *t = track_of(a, i, "cw_anim_sample");
  return t && cw__track_sample(t, time, CW_T_NONE, out);
}

/* ── key sweeps ─────────────────────────────────────────────────────── */

static void fire_range(anim_rec *a, double from, double to, bool incl_from, anim_fired_list *out) {
  if (!out) return;
  bool fwd = to >= from;
  double lo = fwd ? from : to, hi = fwd ? to : from;
  bool incl_lo = fwd ? incl_from : true, incl_hi = fwd ? true : incl_from;
  for (int ti = 0; ti < a->tracks.count; ti++) {
    anim_track *t = &a->tracks.data[ti];
    if (t->kind == CW_TRACK_VALUE || !t->enabled) continue;
    int n = t->times.count;
    for (int j = 0; j < n; j++) {
      int k = fwd ? j : n - 1 - j;
      double kt = t->times.data[k];
      if (kt < lo || kt > hi || (kt == lo && !incl_lo) || (kt == hi && !incl_hi)) continue;
      anim_fired f = { 0, ti, k, NULL, NULL };
      cw_array_push(out, f);
    }
  }
}

static void add_seg(anim_cursor *c, double from, double to) {
  if (from == to) return;
  if (c->seg_count < 8) {
    c->seg[c->seg_count][0] = (float)from;
    c->seg[c->seg_count][1] = (float)to;
    c->seg_count++;
  }
}

/* keys in a sweep fire in time order across tracks only within a track; for
 * one ordering over all tracks, sort the new entries by key time (stable) */
static void sort_fired(anim_rec *a, anim_fired_list *out, int from, bool fwd) {
  if (!out) return;
  for (int i = from + 1; i < out->count; i++) {
    anim_fired f = out->data[i];
    if (f.kind != 0) continue;
    float ft = a->tracks.data[f.track].times.data[f.key];
    int j = i;
    while (j > from) {
      anim_fired g = out->data[j - 1];
      if (g.kind != 0) break;
      float gt = a->tracks.data[g.track].times.data[g.key];
      if (fwd ? gt <= ft : gt >= ft) break;
      out->data[j] = g;
      j--;
    }
    out->data[j] = f;
  }
}

static void sweep(anim_rec *a, anim_cursor *c, double from, double to, anim_fired_list *out) {
  int start = out ? out->count : 0;
  fire_range(a, from, to, c->fire_start, out);
  sort_fired(a, out, start, to >= from);
  c->fire_start = false;
  add_seg(c, from, to);
}

static void push_signal(anim_fired_list *out, int kind) {
  if (out) cw_array_push(out, ((anim_fired){ kind, -1, -1, NULL, NULL }));
}

void cw__cursor_advance(anim_rec *a, anim_cursor *c, double delta, anim_fired_list *out) {
  c->seg_count = 0;
  c->ended = false;
  double len = a->length;
  if (c->dir == 0) c->dir = 1;
  if (delta == 0) return;
  if (len <= 0) {
    sweep(a, c, 0, 0, out);
    c->time = 0;
    if (a->loop == CW_LOOP_NONE && !c->finished) { c->finished = c->ended = true; push_signal(out, 2); }
    return;
  }
  if (a->loop == CW_LOOP_NONE) {
    if (c->finished) return;
    double nt = c->time + delta;
    if (delta > 0 && nt >= len) {
      sweep(a, c, c->time, len, out);
      c->time = len;
      c->finished = c->ended = true;
      push_signal(out, 2);
    } else if (delta < 0 && nt <= 0) {
      sweep(a, c, c->time, 0, out);
      c->time = 0;
      c->finished = c->ended = true;
      push_signal(out, 2);
    } else {
      sweep(a, c, c->time, nt, out);
      c->time = nt;
    }
    return;
  }
  /* LOOP / PING_PONG: walk the motion, splitting at the turning points */
  double rem = fabs(delta);
  int sign = delta > 0 ? 1 : -1;
  if (a->loop == CW_LOOP_PING_PONG) sign *= c->dir;
  for (int guard = 0; rem > 0; guard++) {
    if (guard >= 64) { /* a huge step: drop whole cycles (no keys for them) */
      double cycle = a->loop == CW_LOOP_PING_PONG ? 2 * len : len;
      rem = fmod(rem, cycle);
      guard = 0;
      if (rem == 0) break;
    }
    if (sign > 0) {
      if (c->time + rem < len) { sweep(a, c, c->time, c->time + rem, out); c->time += rem; break; }
      rem -= len - c->time;
      sweep(a, c, c->time, len, out);
      c->ended = true;
      push_signal(out, 1);
      if (a->loop == CW_LOOP_LINEAR) { /* keys at 0 fire at the wrap instant */
        c->time = 0;
        c->fire_start = true;
        sweep(a, c, 0, 0, out);
      }
      else { c->time = len; c->dir = -c->dir; sign = -1; }
    } else {
      if (c->time - rem > 0) { sweep(a, c, c->time, c->time - rem, out); c->time -= rem; break; }
      rem -= c->time;
      sweep(a, c, c->time, 0, out);
      c->ended = true;
      push_signal(out, 1);
      if (a->loop == CW_LOOP_LINEAR) {
        c->time = len;
        c->fire_start = true;
        sweep(a, c, len, len, out);
      }
      else { c->time = 0; c->dir = -c->dir; sign = 1; }
    }
  }
}

/* ── glTF clips ─────────────────────────────────────────────────────── */

cw_anim cw_anim_from_t3_clip(const t3_animation_clip *clip, const char *model_path) {
  cw_anim a = cw_anim_new(clip->name, clip->duration, CW_LOOP_LINEAR);
  cw_anim_set_stored(a, false);
  for (int i = 0; i < clip->track_count; i++) {
    const t3_keyframe_track *k = clip->tracks[i];
    const char *dot = strrchr(k->name, '.');
    if (!dot) continue;
    const char *prop = dot + 1;
    if (strcmp(prop, "position") && strcmp(prop, "quaternion") && strcmp(prop, "scale")) {
      cw_warn("glTF clip '%s': track '%s' skipped (only position/quaternion/scale tracks are sampled)", clip->name, k->name);
      continue;
    }
    char path[512];
    snprintf(path, sizeof path, "%s@%.*s:%s", model_path, (int)(dot - k->name), k->name, prop);
    cw_interp in = k->interpolation == T3_INTERPOLATE_DISCRETE ? CW_INTERP_NEAREST
                   : k->interpolation == T3_INTERPOLATE_CUBIC_SPLINE_GLTF ? CW_INTERP_HERMITE : CW_INTERP_LINEAR;
    int ti = cw_anim_add_value_track(a, path, in);
    bool quat = k->type == T3_TRACK_QUATERNION;
    int vs = k->value_size;
    if ((quat && vs != 4) || (!quat && vs != 3)) continue;
    for (int j = 0; j < k->count; j++) {
      cw_value v[3];
      int per = in == CW_INTERP_HERMITE ? 3 : 1;
      for (int s = 0; s < per; s++) {
        const float *src = k->values + ((size_t)j * per + s) * vs;
        v[s] = (cw_value){ quat ? CW_T_QUAT : CW_T_VEC3 };
        memcpy(v[s].v, src, sizeof(float) * (size_t)vs);
      }
      if (in == CW_INTERP_HERMITE) cw_anim_add_key_hermite(a, ti, k->times[j], v[1], v[0], v[2]);
      else cw_anim_add_key(a, ti, k->times[j], v[0]);
    }
  }
  return a;
}

/* ── method callbacks ───────────────────────────────────────────────── */

typedef struct { const char *name; cw_anim_method_fn fn; void *user; } method_reg;
static CW_ARRAY(method_reg) methods;

void cw_anim_register_method(const char *name, cw_anim_method_fn fn, void *user) {
  const char *in = cw_intern(name);
  for (int i = 0; i < methods.count; i++)
    if (methods.data[i].name == in) { methods.data[i].fn = fn; methods.data[i].user = user; return; }
  cw_array_push(&methods, ((method_reg){ in, fn, user }));
}

void cw__fire_method_key(cw_node root, const anim_track *t, const anim_mkey *k) {
  cw_node target = t->node[0] ? cw_node_get(root, t->node) : root;
  if (!cw_node_valid(target)) {
    cw_warn("animation method track: no node at '%s' (from '%s')", t->node, cw_node_name(root));
    return;
  }
  for (int i = 0; i < methods.count; i++)
    if (methods.data[i].name == k->name) { methods.data[i].fn(target, k->args, k->argc, methods.data[i].user); return; }
  cw_emit(target, k->name, k->args, k->argc);
}

/* ── text form ──────────────────────────────────────────────────────── */

typedef struct { char *buf; size_t len, cap; } tbuf;
static void tput(tbuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2 + 256;
    b->buf = cw_realloc(b->buf, b->cap, CW_MEM_ANIM);
  }
  memcpy(b->buf + b->len, s, n + 1);
  b->len += n;
}
static void tval(tbuf *b, const cw_value *v) {
  char tmp[2048];
  cw_value_format(v, tmp, sizeof tmp);
  tput(b, tmp);
}
static void tfloat(tbuf *b, float f) {
  cw_value v = cw_val_float(f);
  tval(b, &v);
}

static const char *const loop_names[] = { "NONE", "LOOP", "PING_PONG" };
static const char *const interp_names[] = { "NEAREST", "LINEAR", "CUBIC", "HERMITE" };
static const char *const kind_names[] = { "value", "method", "event" };

const char *cw__anim_loop_name(int i) { return loop_names[i]; }
const char *cw__anim_interp_name(int i) { return interp_names[i]; }
const char *cw__anim_kind_name(int i) { return kind_names[i]; }

static void tquoted(tbuf *b, const char *s) {
  cw_value v = { CW_T_STRING };
  v.s = s;
  tval(b, &v);
}

char *cw_anim_to_text(cw_anim a, const char *player_path) { return cw__anim_to_text_named(a, NULL, player_path); }

char *cw__anim_to_text_named(cw_anim a, const char *name, const char *player_path) {
  anim_rec *r = cw__anim(a);
  if (!r) return NULL;
  tbuf b = { 0 };
  tput(&b, "\n[animation name=");
  tquoted(&b, name ? name : r->name);
  tput(&b, " player=");
  tquoted(&b, player_path ? player_path : ".");
  tput(&b, " length=");
  tfloat(&b, r->length);
  if (r->loop != CW_LOOP_NONE) { tput(&b, " loop="); tput(&b, loop_names[r->loop]); }
  tput(&b, "]\n");
  for (int i = 0; i < r->tracks.count; i++) {
    anim_track *t = &r->tracks.data[i];
    tput(&b, "[track type=");
    tput(&b, kind_names[t->kind]);
    if (t->kind != CW_TRACK_EVENT) { tput(&b, " path="); tquoted(&b, t->path); }
    if (t->kind == CW_TRACK_VALUE && t->interp != CW_INTERP_LINEAR) { tput(&b, " interp="); tput(&b, interp_names[t->interp]); }
    if (t->rot == CW_ROT_NLERP) tput(&b, " rot=NLERP");
    if (!t->enabled) tput(&b, " enabled=false");
    tput(&b, "]\n");
    for (int k = 0; k < t->times.count; k++) {
      tfloat(&b, t->times.data[k]);
      tput(&b, " = ");
      if (t->kind == CW_TRACK_VALUE) {
        tval(&b, &t->values.data[k]);
        if (t->tangents.count >= 2 * (k + 1)) {
          tput(&b, " in=");
          tval(&b, &t->tangents.data[2 * k]);
          tput(&b, " out=");
          tval(&b, &t->tangents.data[2 * k + 1]);
        }
      } else if (t->kind == CW_TRACK_METHOD) {
        anim_mkey *mk = &t->mkeys.data[k];
        tput(&b, mk->name);
        tput(&b, "(");
        for (int j = 0; j < mk->argc; j++) {
          if (j) tput(&b, ", ");
          tval(&b, &mk->args[j]);
        }
        tput(&b, ")");
      } else {
        tquoted(&b, t->mkeys.data[k].name);
      }
      tput(&b, "\n");
    }
  }
  return b.buf;
}

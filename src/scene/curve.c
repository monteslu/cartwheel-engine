/* cw_curve (cartwheel/curve.h): Bezier points, the baked polyline with frames, queries by distance.
 * Float + - * / and sqrt in a fixed order, cw_sincos / cw_atan2 for angles: the same bits on every host. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/curve.h"

typedef t3_vec3 v3;

typedef struct baked {
  v3 pos, fwd, up;
  float dist, tilt;
} baked;

struct cw_curve {
  CW_ARRAY(cw_curve_point) pts;
  bool closed;
  float interval;
  v3 up;
  uint32_t version;
  /* the bake */
  bool dirty;
  CW_ARRAY(baked) bk;
  CW_ARRAY(v3) bk_pos;      /* cw_curve_baked's view */
  CW_ARRAY(float) bk_dist;
  float length;
};

static inline v3 V(float x, float y, float z) { return (v3){ x, y, z }; }
static inline v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 sub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 mul(v3 a, float s) { return V(a.x * s, a.y * s, a.z * s); }
static inline float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float len(v3 a) { return sqrtf(dot(a, a)); }
static inline v3 lerp3(v3 a, v3 b, float t) { return add(a, mul(sub(b, a), t)); }
/* unit vector, or fallback when a is too short to have a direction */
static inline v3 norm_or(v3 a, v3 fallback) {
  float l = len(a);
  return l > 1e-12f ? mul(a, 1.0f / l) : fallback;
}

cw_curve *cw_curve_new(void) {
  cw_curve *c = cw_alloc(sizeof *c, CW_MEM_SCENE);
  memset(c, 0, sizeof *c);
  c->interval = 0.2f;
  c->up = V(0, 1, 0);
  c->dirty = true;
  return c;
}
void cw_curve_free(cw_curve *c) {
  if (!c) return;
  cw_array_free(&c->pts);
  cw_array_free(&c->bk);
  cw_array_free(&c->bk_pos);
  cw_array_free(&c->bk_dist);
  cw_free(c);
}
static void touch(cw_curve *c) {
  c->dirty = true;
  c->version++;
}
void cw_curve_clear(cw_curve *c) {
  cw_array_clear(&c->pts);
  touch(c);
}
int cw_curve_point_count(const cw_curve *c) { return c->pts.count; }
void cw_curve_add_point(cw_curve *c, v3 position, v3 in, v3 out, int index) {
  cw_curve_point p = { position, in, out, 0 };
  cw_array_push(&c->pts, p);
  int n = c->pts.count;
  if (index >= 0 && index < n - 1) {
    memmove(&c->pts.data[index + 1], &c->pts.data[index], sizeof p * (size_t)(n - 1 - index));
    c->pts.data[index] = p;
  }
  touch(c);
}
void cw_curve_remove_point(cw_curve *c, int index) {
  int n = c->pts.count;
  if (index < 0 || index >= n) return;
  memmove(&c->pts.data[index], &c->pts.data[index + 1], sizeof(cw_curve_point) * (size_t)(n - 1 - index));
  c->pts.count--;
  touch(c);
}
cw_curve_point cw_curve_get_point(const cw_curve *c, int index) {
  if (index < 0 || index >= c->pts.count) return (cw_curve_point){ 0 };
  return c->pts.data[index];
}
void cw_curve_set_point(cw_curve *c, int index, cw_curve_point p) {
  if (index < 0 || index >= c->pts.count) return;
  c->pts.data[index] = p;
  touch(c);
}
void cw_curve_set_closed(cw_curve *c, bool closed) {
  if (c->closed == closed) return;
  c->closed = closed;
  touch(c);
}
bool cw_curve_closed(const cw_curve *c) { return c->closed; }
void cw_curve_set_bake_interval(cw_curve *c, float interval) {
  if (!(interval >= 0.0001f)) interval = 0.0001f;
  if (c->interval == interval) return;
  c->interval = interval;
  touch(c);
}
float cw_curve_bake_interval(const cw_curve *c) { return c->interval; }
void cw_curve_set_up(cw_curve *c, v3 up) {
  if (c->up.x == up.x && c->up.y == up.y && c->up.z == up.z) return;
  c->up = up;
  touch(c);
}
uint32_t cw_curve_version(const cw_curve *c) { return c->version; }

static int segment_count(const cw_curve *c) {
  int n = c->pts.count;
  return n < 2 ? 0 : c->closed ? n : n - 1;
}
static void segment_ctrl(const cw_curve *c, int s, v3 k[4]) {
  const cw_curve_point *a = &c->pts.data[s], *b = &c->pts.data[(s + 1) % c->pts.count];
  k[0] = a->position;
  k[1] = add(a->position, a->out);
  k[2] = add(b->position, b->in);
  k[3] = b->position;
}
static v3 bezier(const v3 k[4], float t) {
  float u = 1 - t, uu = u * u, tt = t * t;
  float b0 = uu * u, b1 = 3 * uu * t, b2 = 3 * u * tt, b3 = tt * t;
  return add(add(mul(k[0], b0), mul(k[1], b1)), add(mul(k[2], b2), mul(k[3], b3)));
}
static v3 bezier_deriv(const v3 k[4], float t) {
  float u = 1 - t;
  v3 d0 = sub(k[1], k[0]), d1 = sub(k[2], k[1]), d2 = sub(k[3], k[2]);
  return add(add(mul(d0, 3 * u * u), mul(d1, 6 * u * t)), mul(d2, 3 * t * t));
}

v3 cw_curve_sample(const cw_curve *c, int segment, float t) {
  int n = c->pts.count, segs = segment_count(c);
  if (!n) return V(0, 0, 0);
  if (!segs) return c->pts.data[0].position;
  if (segment < 0) segment = 0, t = 0;
  if (segment >= segs) segment = segs - 1, t = 1;
  v3 k[4];
  segment_ctrl(c, segment, k);
  return bezier(k, cw_clampf(t, 0, 1));
}

/* rotate up about the unit axis fwd (perpendicular to up) by angle */
static v3 roll(v3 up, v3 fwd, float angle) {
  float s, co;
  cw_sincos(angle, &s, &co);
  return add(mul(up, co), mul(cross(fwd, up), s));
}

static void bake(cw_curve *c) {
  c->dirty = false;
  cw_array_clear(&c->bk);
  cw_array_clear(&c->bk_pos);
  cw_array_clear(&c->bk_dist);
  c->length = 0;
  int n = c->pts.count, segs = segment_count(c);
  if (!n) return;
  if (!segs) {
    baked b = { c->pts.data[0].position, V(0, 0, -1), V(0, 1, 0), 0, c->pts.data[0].tilt };
    cw_array_push(&c->bk, b);
  }
  float dist = 0;
  for (int s = 0; s < segs; s++) {
    v3 k[4];
    segment_ctrl(c, s, k);
    const cw_curve_point *pa = &c->pts.data[s], *pb = &c->pts.data[(s + 1) % n];
    bool straight = dot(pa->out, pa->out) == 0 && dot(pb->in, pb->in) == 0;
    float est = len(sub(k[1], k[0])) + len(sub(k[2], k[1])) + len(sub(k[3], k[2]));
    float fsteps = est / c->interval;
    /* a straight segment is one piece: its length is exact, and tilt is linear along it either way */
    int steps = straight ? 1 : fsteps > 4096 ? 4096 : (int)fsteps + 1;
    if (!straight && steps < 8) steps = 8;
    v3 chord = norm_or(sub(k[3], k[0]), V(0, 0, -1));
    if (s && c->bk.count) {
      /* a corner between two segments: the point is baked twice, arriving and leaving, a piece of no length
       * apart, so a sample just before it and one just after get the right direction (and the frame is carried
       * round the corner like any other step) */
      baked last = c->bk.data[c->bk.count - 1];
      v3 leave = norm_or(bezier_deriv(k, 0), chord);
      if (dot(leave, last.fwd) < 0.999999f) {
        last.fwd = leave;
        cw_array_push(&c->bk, last);
      }
    }
    for (int i = s ? 1 : 0; i <= steps; i++) {
      float t = (float)i / (float)steps;
      v3 p = i == steps ? k[3] : i == 0 ? k[0] : bezier(k, t);
      baked b = { .pos = p, .tilt = pa->tilt + (pb->tilt - pa->tilt) * t };
      /* the direction of travel: the derivative, or the chord where a zero handle makes it vanish */
      b.fwd = norm_or(bezier_deriv(k, t), chord);
      if (c->bk.count) {
        float d = len(sub(p, c->bk.data[c->bk.count - 1].pos));
        if (d <= 1e-9f) { /* a repeated point: keep one, with the later direction and tilt */
          c->bk.data[c->bk.count - 1].fwd = b.fwd;
          c->bk.data[c->bk.count - 1].tilt = b.tilt;
          continue;
        }
        dist += d;
      }
      b.dist = dist;
      cw_array_push(&c->bk, b);
    }
  }
  c->length = dist;
  int m = c->bk.count;
  baked *B = c->bk.data;
  /* frames: the first up is the reference made perpendicular to the first direction; each next one is carried
   * by two reflections (Wang et al., "Computation of Rotation Minimizing Frames", 2008): no trig, no drift */
  v3 up0 = sub(c->up, mul(B[0].fwd, dot(c->up, B[0].fwd)));
  if (dot(up0, up0) < 1e-10f) {
    v3 alt = fabsf(B[0].fwd.z) < 0.9f ? V(0, 0, 1) : V(1, 0, 0);
    up0 = sub(alt, mul(B[0].fwd, dot(alt, B[0].fwd)));
  }
  B[0].up = norm_or(up0, V(0, 1, 0));
  for (int i = 0; i + 1 < m; i++) {
    v3 v1 = sub(B[i + 1].pos, B[i].pos);
    float c1 = dot(v1, v1);
    v3 rl = B[i].up, tl = B[i].fwd;
    if (c1 > 0) {
      rl = sub(B[i].up, mul(v1, 2 / c1 * dot(v1, B[i].up)));
      tl = sub(B[i].fwd, mul(v1, 2 / c1 * dot(v1, B[i].fwd)));
    }
    v3 v2 = sub(B[i + 1].fwd, tl);
    float c2 = dot(v2, v2);
    v3 r = c2 > 1e-20f ? sub(rl, mul(v2, 2 / c2 * dot(v2, rl))) : rl;
    /* keep it exactly perpendicular and unit: the errors of thousands of steps do not add up */
    r = sub(r, mul(B[i + 1].fwd, dot(r, B[i + 1].fwd)));
    B[i + 1].up = norm_or(r, B[i].up);
  }
  /* a closed curve: the carried frame comes back rolled by some angle about the first direction; spread the
   * opposite roll along the length so the frames meet at the seam */
  if (c->closed && m > 2 && c->length > 0) {
    /* the last frame carried on to the first direction (a seam with a corner turns there) */
    v3 f0 = B[0].fwd, v2 = sub(f0, B[m - 1].fwd), endup = B[m - 1].up;
    float c2 = dot(v2, v2);
    if (c2 > 1e-20f) endup = sub(endup, mul(v2, 2 / c2 * dot(v2, endup)));
    endup = norm_or(sub(endup, mul(f0, dot(endup, f0))), B[0].up);
    float twist = cw_atan2(dot(cross(endup, B[0].up), f0), dot(endup, B[0].up));
    if (twist != 0)
      for (int i = 1; i < m; i++) B[i].up = norm_or(roll(B[i].up, B[i].fwd, twist * (B[i].dist / c->length)), B[i].up);
  }
  for (int i = 0; i < m; i++) {
    cw_array_push(&c->bk_pos, B[i].pos);
    cw_array_push(&c->bk_dist, B[i].dist);
  }
}
static inline void ensure(cw_curve *c) {
  if (c->dirty) bake(c);
}

float cw_curve_length(cw_curve *c) {
  ensure(c);
  return c->length;
}

/* the baked piece holding offset: index i with dist[i] <= offset <= dist[i + 1], and the share along it */
static int locate(const cw_curve *c, float offset, float *frac) {
  int m = c->bk.count;
  const baked *B = c->bk.data;
  if (m < 2 || !(offset > 0)) { *frac = 0; return 0; }
  if (offset >= c->length) { *frac = 1; return m - 2; }
  int lo = 0, hi = m - 1;
  while (hi - lo > 1) {
    int mid = (lo + hi) / 2;
    if (B[mid].dist <= offset) lo = mid;
    else hi = mid;
  }
  float span = B[lo + 1].dist - B[lo].dist;
  *frac = span > 0 ? (offset - B[lo].dist) / span : 0;
  return lo;
}

v3 cw_curve_sample_baked(cw_curve *c, float offset) {
  ensure(c);
  if (!c->bk.count) return V(0, 0, 0);
  if (c->bk.count == 1) return c->bk.data[0].pos;
  float f;
  int i = locate(c, offset, &f);
  return lerp3(c->bk.data[i].pos, c->bk.data[i + 1].pos, f);
}

cw_curve_frame cw_curve_sample_frame(cw_curve *c, float offset, bool apply_tilt) {
  ensure(c);
  cw_curve_frame fr = { V(0, 0, 0), V(0, 0, -1), V(0, 1, 0), V(1, 0, 0), 0 };
  int m = c->bk.count;
  if (!m) return fr;
  const baked *B = c->bk.data;
  if (m == 1) {
    fr.position = B[0].pos;
    fr.tilt = B[0].tilt;
  } else {
    float f;
    int i = locate(c, offset, &f);
    fr.position = lerp3(B[i].pos, B[i + 1].pos, f);
    fr.forward = norm_or(lerp3(B[i].fwd, B[i + 1].fwd, f), B[i].fwd);
    v3 up = lerp3(B[i].up, B[i + 1].up, f);
    up = sub(up, mul(fr.forward, dot(up, fr.forward)));
    fr.up = norm_or(up, B[i].up);
    fr.tilt = B[i].tilt + (B[i + 1].tilt - B[i].tilt) * f;
  }
  if (apply_tilt && fr.tilt != 0) fr.up = norm_or(roll(fr.up, fr.forward, fr.tilt), fr.up);
  fr.right = cross(fr.forward, fr.up);
  return fr;
}

float cw_curve_closest_offset(cw_curve *c, v3 p) {
  ensure(c);
  int m = c->bk.count;
  const baked *B = c->bk.data;
  if (m < 2) return 0;
  float best = -1, best_off = 0;
  for (int i = 0; i + 1 < m; i++) {
    v3 ab = sub(B[i + 1].pos, B[i].pos);
    float l2 = dot(ab, ab);
    float t = l2 > 0 ? cw_clampf(dot(sub(p, B[i].pos), ab) / l2, 0, 1) : 0;
    v3 d = sub(p, add(B[i].pos, mul(ab, t)));
    float d2 = dot(d, d);
    if (best < 0 || d2 < best) { /* the first of equally near pieces wins: a fixed answer */
      best = d2;
      best_off = B[i].dist + (B[i + 1].dist - B[i].dist) * t;
    }
  }
  return best_off;
}
v3 cw_curve_closest_point(cw_curve *c, v3 p) {
  ensure(c);
  if (c->bk.count == 1) return c->bk.data[0].pos;
  return cw_curve_sample_baked(c, cw_curve_closest_offset(c, p));
}

int cw_curve_baked(cw_curve *c, const v3 **positions, const float **offsets) {
  ensure(c);
  if (positions) *positions = c->bk_pos.data;
  if (offsets) *offsets = c->bk_dist.data;
  return c->bk.count;
}

uint32_t cw_curve_hash(cw_curve *c) {
  ensure(c);
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < c->bk.count; i++) {
    const baked *b = &c->bk.data[i];
    float f[11] = { b->pos.x, b->pos.y, b->pos.z, b->fwd.x, b->fwd.y, b->fwd.z, b->up.x, b->up.y, b->up.z, b->dist, b->tilt };
    h = cw_hash32(f, sizeof f, h);
  }
  return cw_hash32(&c->length, sizeof c->length, h);
}

/* ── text ───────────────────────────────────────────────────────────── */

bool cw_curve_parse(cw_curve *c, const char *text, int dims) {
  cw_array_clear(&c->pts);
  touch(c);
  if (!text) return true;
  const char *s = text;
  int index = 0;
  for (;;) {
    float f[10];
    int k = 0;
    for (;;) {
      while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
      if (!*s || *s == ',') break;
      char *end;
      double d = strtod(s, &end);
      if (end == s || k >= 10) {
        cw_error("curve: point %d of \"%.40s...\" is not numbers (points are \"x y%s\" separated by commas, optionally followed by the in and out handles%s)",
                 index, text, dims == 3 ? " z" : "", dims == 3 ? " and a tilt" : "");
        cw_array_clear(&c->pts);
        return false;
      }
      f[k++] = (float)d;
      s = end;
    }
    if (k) {
      cw_curve_point p = { 0 };
      bool ok = true;
      if (dims == 2) {
        ok = k == 2 || k == 6;
        p.position = V(f[0], f[1], 0);
        if (k == 6) p.in = V(f[2], f[3], 0), p.out = V(f[4], f[5], 0);
      } else {
        ok = k == 3 || k == 9 || k == 10;
        p.position = V(f[0], f[1], f[2]);
        if (k >= 9) p.in = V(f[3], f[4], f[5]), p.out = V(f[6], f[7], f[8]);
        if (k == 10) p.tilt = f[9];
      }
      if (!ok) {
        cw_error("curve: point %d has %d numbers (a %s point is %s)", index, k, dims == 2 ? "2D" : "3D",
                 dims == 2 ? "2: x y, or 6 with the in and out handles" : "3: x y z, 9 with the in and out handles, or 10 with a tilt");
        cw_array_clear(&c->pts);
        return false;
      }
      cw_array_push(&c->pts, p);
      index++;
    }
    if (!*s) break;
    s++; /* the comma */
  }
  return true;
}

/* an snprintf-style appender: counts the full length, writes what fits */
typedef struct { char *buf; int cap, len; } outbuf;
static void put_s(outbuf *o, const char *s) {
  for (; *s; s++, o->len++)
    if (o->buf && o->len < o->cap - 1) o->buf[o->len] = *s;
}
/* shortest text that reads back as the same float */
static void put_f(outbuf *o, float f) {
  char tmp[40];
  if (f == floorf(f) && fabsf(f) < 1e15f) { /* whole numbers as digits, never 1e+02 */
    snprintf(tmp, sizeof tmp, "%.0f", (double)f);
    put_s(o, f == 0 ? "0" : tmp); /* and never -0 */
    return;
  }
  for (int prec = 1; prec <= 9; prec++) {
    snprintf(tmp, sizeof tmp, "%.*g", prec, (double)f);
    if (strtof(tmp, NULL) == f) break;
  }
  put_s(o, tmp);
}
static void put_v(outbuf *o, v3 v, int dims) {
  put_f(o, v.x);
  put_s(o, " ");
  put_f(o, v.y);
  if (dims == 3) put_s(o, " "), put_f(o, v.z);
}

int cw_curve_format(const cw_curve *c, char *buf, int cap, int dims) {
  outbuf o = { cap > 0 ? buf : NULL, cap, 0 };
  for (int i = 0; i < c->pts.count; i++) {
    const cw_curve_point *p = &c->pts.data[i];
    if (i) put_s(&o, ", ");
    put_v(&o, p->position, dims);
    bool tilt = dims == 3 && p->tilt != 0;
    if (dot(p->in, p->in) != 0 || dot(p->out, p->out) != 0 || tilt) {
      put_s(&o, "  ");
      put_v(&o, p->in, dims);
      put_s(&o, "  ");
      put_v(&o, p->out, dims);
      if (tilt) put_s(&o, "  "), put_f(&o, p->tilt);
    }
  }
  if (o.buf) o.buf[o.len < cap - 1 ? o.len : cap - 1] = 0;
  return o.len;
}

/* ── 2D ─────────────────────────────────────────────────────────────── */

void cw_curve_add_point2d(cw_curve *c, cw_vec2 position, cw_vec2 in, cw_vec2 out, int index) {
  cw_curve_add_point(c, V(position.x, position.y, 0), V(in.x, in.y, 0), V(out.x, out.y, 0), index);
}
cw_vec2 cw_curve_sample_baked2d(cw_curve *c, float offset) {
  v3 p = cw_curve_sample_baked(c, offset);
  return cw_v2(p.x, p.y);
}
void cw_curve_sample_dir2d(cw_curve *c, float offset, cw_vec2 *position, cw_vec2 *direction) {
  ensure(c);
  v3 p = V(0, 0, 0), d = V(1, 0, 0);
  int m = c->bk.count;
  if (m == 1) p = c->bk.data[0].pos;
  else if (m > 1) {
    float f;
    int i = locate(c, offset, &f);
    const baked *B = c->bk.data;
    p = lerp3(B[i].pos, B[i + 1].pos, f);
    d = norm_or(lerp3(B[i].fwd, B[i + 1].fwd, f), B[i].fwd);
  }
  if (position) *position = cw_v2(p.x, p.y);
  if (direction) *direction = cw_v2(d.x, d.y);
}
float cw_curve_closest_offset2d(cw_curve *c, cw_vec2 p) { return cw_curve_closest_offset(c, V(p.x, p.y, 0)); }

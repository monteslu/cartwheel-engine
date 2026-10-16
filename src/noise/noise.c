/* cw_noise: FastNoiseLite wrapped (cartwheel/noise.h), and the gen://noise
 * texture generator.
 *
 * FastNoiseLite's C header uses no libm (it carries its own square root),
 * so the samples are the same bits on every host. Its hashes multiply
 * signed ints and rely on wrap-around, which C leaves undefined: this file
 * is built with -fwrapv (CMakeLists.txt), which defines it as the wrap the
 * library expects (and keeps UBSan quiet). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/jobs.h"
#include "cartwheel/noise.h"
#include "noise_internal.h"

#define FNL_IMPL
#include "third_party/fastnoiselite/FastNoiseLite.h"

cw_noise cw_noise_default(void) {
  return (cw_noise){ .seed = 1337, .frequency = 0.01f, .type = CW_NOISE_OPENSIMPLEX2, .rotation = CW_NOISE_ROTATION_NONE,
                     .fractal = CW_NOISE_FRACTAL_NONE, .octaves = 3, .lacunarity = 2.0f, .gain = 0.5f,
                     .weighted_strength = 0.0f, .ping_pong_strength = 2.0f,
                     .cellular_distance = CW_NOISE_DIST_EUCLIDEAN_SQ, .cellular_return = CW_NOISE_CELL_DISTANCE,
                     .cellular_jitter = 1.0f, .warp = CW_NOISE_WARP_OPENSIMPLEX2, .warp_amp = 30.0f };
}

/* our enums follow FastNoiseLite's order; WARP_NONE is ours (one before) */
static fnl_state to_fnl(const cw_noise *n) {
  fnl_state s = fnlCreateState();
  s.seed = n->seed;
  s.frequency = n->frequency;
  s.noise_type = (fnl_noise_type)((unsigned)n->type < CW_NOISE_TYPE_COUNT ? n->type : 0);
  s.rotation_type_3d = (fnl_rotation_type_3d)((unsigned)n->rotation <= CW_NOISE_ROTATION_XZ ? n->rotation : 0);
  s.fractal_type = (fnl_fractal_type)((unsigned)n->fractal < CW_NOISE_FRACTAL_COUNT ? n->fractal : 0);
  s.octaves = n->octaves < 1 ? 1 : n->octaves > 16 ? 16 : n->octaves;
  s.lacunarity = n->lacunarity;
  s.gain = n->gain;
  s.weighted_strength = n->weighted_strength;
  s.ping_pong_strength = n->ping_pong_strength;
  s.cellular_distance_func = (fnl_cellular_distance_func)((unsigned)n->cellular_distance < CW_NOISE_DIST_COUNT ? n->cellular_distance : 1);
  s.cellular_return_type = (fnl_cellular_return_type)((unsigned)n->cellular_return < CW_NOISE_CELL_RETURN_COUNT ? n->cellular_return : 1);
  s.cellular_jitter_mod = n->cellular_jitter;
  s.domain_warp_type = (fnl_domain_warp_type)(n->warp > CW_NOISE_WARP_NONE && n->warp < CW_NOISE_WARP_COUNT ? n->warp - 1 : 0);
  s.domain_warp_amp = n->warp_amp;
  return s;
}

float cw_noise_2d(const cw_noise *n, float x, float y) {
  fnl_state s = to_fnl(n);
  return fnlGetNoise2D(&s, x, y);
}

float cw_noise_3d(const cw_noise *n, float x, float y, float z) {
  fnl_state s = to_fnl(n);
  return fnlGetNoise3D(&s, x, y, z);
}

void cw_noise_warp_2d(const cw_noise *n, float *x, float *y) {
  if (n->warp == CW_NOISE_WARP_NONE) return;
  fnl_state s = to_fnl(n);
  fnlDomainWarp2D(&s, x, y);
}

void cw_noise_warp_3d(const cw_noise *n, float *x, float *y, float *z) {
  if (n->warp == CW_NOISE_WARP_NONE) return;
  fnl_state s = to_fnl(n);
  fnlDomainWarp3D(&s, x, y, z);
}

typedef struct fill_job {
  fnl_state s, w;
  bool warp, three;
  float *out;
  int width;
  float x0, y0, z, step;
} fill_job;

static void fill_rows(int begin, int end, int worker, void *ctx) {
  const fill_job *j = ctx;
  for (int r = begin; r < end; r++)
    for (int c = 0; c < j->width; c++) {
      float x = j->x0 + (float)c * j->step, y = j->y0 + (float)r * j->step, z = j->z;
      if (j->three) {
        if (j->warp) fnlDomainWarp3D(&j->w, &x, &y, &z);
        j->out[(size_t)r * (size_t)j->width + (size_t)c] = fnlGetNoise3D(&j->s, x, y, z);
      } else {
        if (j->warp) fnlDomainWarp2D(&j->w, &x, &y);
        j->out[(size_t)r * (size_t)j->width + (size_t)c] = fnlGetNoise2D(&j->s, x, y);
      }
    }
}

static void fill(const cw_noise *n, const cw_noise *warp, float *out, int w, int h, float x0, float y0, float z,
                 float step, bool three) {
  if (w <= 0 || h <= 0) return;
  fill_job j = { .s = to_fnl(n), .warp = warp && warp->warp != CW_NOISE_WARP_NONE, .three = three, .out = out,
                 .width = w, .x0 = x0, .y0 = y0, .z = z, .step = step };
  if (j.warp) j.w = to_fnl(warp);
  cw_parallel_for(h, 4, fill_rows, &j);
}

void cw_noise_fill_2d(const cw_noise *n, const cw_noise *warp, float *out, int w, int h, float x0, float y0, float step) {
  fill(n, warp, out, w, h, x0, y0, 0, step, false);
}

void cw_noise_fill_3d_slice(const cw_noise *n, const cw_noise *warp, float *out, int w, int h, float x0, float y0,
                            float z, float step) {
  fill(n, warp, out, w, h, x0, y0, z, step, true);
}

uint64_t cw_noise_hash(const float *v, size_t count) { return cw_hash_bytes(v, count * sizeof *v, 0); }

void cw_noise_gpu_params(const cw_noise *n, float out[16]) {
  out[0] = (float)n->seed;
  out[1] = n->frequency;
  out[2] = (float)n->type;
  out[3] = (float)n->rotation;
  out[4] = (float)n->fractal;
  out[5] = (float)(n->octaves < 1 ? 1 : n->octaves > 16 ? 16 : n->octaves);
  out[6] = n->lacunarity;
  out[7] = n->gain;
  out[8] = n->weighted_strength;
  out[9] = n->ping_pong_strength;
  out[10] = (float)n->cellular_distance;
  out[11] = (float)n->cellular_return;
  out[12] = n->cellular_jitter;
  out[13] = out[14] = out[15] = 0;
}

/* ── names ─────────────────────────────────────────────────────────── */

static const char *const type_names[] = { "opensimplex2", "opensimplex2s", "cellular", "perlin", "value_cubic", "value" };
static const char *const fractal_names[] = { "none", "fbm", "ridged", "pingpong", "progressive", "independent" };
static const char *const rot_names[] = { "none", "xy", "xz" };
static const char *const dist_names[] = { "euclidean", "euclidean_sq", "manhattan", "hybrid" };
static const char *const ret_names[] = { "value", "distance", "distance2", "distance2_add", "distance2_sub", "distance2_mul",
                                         "distance2_div" };
static const char *const warp_names[] = { "none", "opensimplex2", "opensimplex2_reduced", "basic_grid" };
static const char *const out_names[] = { "gray", "rgba", "normal" };

const char *cw_noise_type_name(cw_noise_type t) { return (unsigned)t < CW_NOISE_TYPE_COUNT ? type_names[t] : "?"; }

static int find_name(const char *const *names, int n, const char *s, size_t len) {
  for (int i = 0; i < n; i++)
    if (strlen(names[i]) == len && !strncmp(names[i], s, len)) return i;
  return -1;
}

int cw_noise_type_from_name(const char *s) { return s ? find_name(type_names, CW_NOISE_TYPE_COUNT, s, strlen(s)) : -1; }

/* ── the gen://noise query ─────────────────────────────────────────── */

typedef struct noise_query {
  cw_noise n, warp;
  int w, h;
  float x, y, z;
  bool three, seamless;
  int out; /* 0 gray, 1 rgba, 2 normal */
  float strength, lo, hi;
} noise_query;

static bool num(const char *v, size_t len, float *out) {
  char b[48];
  if (!len || len >= sizeof b) return false;
  memcpy(b, v, len);
  b[len] = 0;
  char *e;
  /* strtod is exact for decimal input on every libc we build with */
  double d = strtod(b, &e);
  if (*e) return false;
  *out = (float)d;
  return true;
}

static bool parse(const char *q, noise_query *o) {
  memset(o, 0, sizeof *o);
  o->n = cw_noise_default();
  o->w = o->h = 256;
  o->strength = 2.0f;
  o->lo = -1.0f;
  o->hi = 1.0f;
  bool warp_seed = false, warp_freq = false;
  o->warp = cw_noise_default();
  o->warp.warp = CW_NOISE_WARP_NONE;
  const char *p = q;
  while (p && *p) {
    const char *amp = strchr(p, '&');
    size_t len = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = memchr(p, '=', len);
    if (!eq) {
      cw_error("gen://noise: \"%.*s\" has no value (key=value)", (int)len, p);
      return false;
    }
    size_t kl = (size_t)(eq - p), vl = len - kl - 1;
    const char *v = eq + 1;
    float f = 0;
    int e = -1;
#define KEY(s) (kl == sizeof(s) - 1 && !strncmp(p, s, kl))
#define NUM(dst)                                                                                                  \
  do {                                                                                                            \
    if (!num(v, vl, &f)) { cw_error("gen://noise: %.*s=%.*s is not a number", (int)kl, p, (int)vl, v); return false; } \
    dst;                                                                                                          \
  } while (0)
#define ENUM(names, count, dst)                                                                                   \
  do {                                                                                                            \
    e = find_name(names, count, v, vl);                                                                           \
    if (e < 0) { cw_error("gen://noise: %.*s=%.*s is not one of its names (see cartwheel/noise.h)", (int)kl, p, (int)vl, v); return false; } \
    dst;                                                                                                          \
  } while (0)
    if (KEY("size")) {
      const char *x = memchr(v, 'x', vl);
      float a, b;
      if (x ? !(num(v, (size_t)(x - v), &a) && num(x + 1, vl - (size_t)(x - v) - 1, &b)) : !num(v, vl, &a)) {
        cw_error("gen://noise: size=%.*s: write 256 or 512x256", (int)vl, v);
        return false;
      }
      if (!x) b = a;
      o->w = (int)a;
      o->h = (int)b;
    } else if (KEY("type")) ENUM(type_names, CW_NOISE_TYPE_COUNT, o->n.type = (cw_noise_type)e);
    else if (KEY("seed")) NUM(o->n.seed = (int32_t)f);
    else if (KEY("freq")) NUM(o->n.frequency = f);
    else if (KEY("fractal")) ENUM(fractal_names, 4, o->n.fractal = (cw_noise_fractal)e);
    else if (KEY("octaves")) NUM(o->n.octaves = (int32_t)f);
    else if (KEY("lacunarity")) NUM(o->n.lacunarity = f);
    else if (KEY("gain")) NUM(o->n.gain = f);
    else if (KEY("weighted")) NUM(o->n.weighted_strength = f);
    else if (KEY("pingpong")) NUM(o->n.ping_pong_strength = f);
    else if (KEY("rotation")) ENUM(rot_names, 3, o->n.rotation = (cw_noise_rotation)e);
    else if (KEY("cell_dist")) ENUM(dist_names, CW_NOISE_DIST_COUNT, o->n.cellular_distance = (cw_noise_cellular_distance)e);
    else if (KEY("cell_return")) ENUM(ret_names, CW_NOISE_CELL_RETURN_COUNT, o->n.cellular_return = (cw_noise_cellular_return)e);
    else if (KEY("jitter")) NUM(o->n.cellular_jitter = f);
    else if (KEY("warp")) ENUM(warp_names, CW_NOISE_WARP_COUNT, o->warp.warp = (cw_noise_warp)e);
    else if (KEY("warp_amp")) NUM(o->warp.warp_amp = f);
    else if (KEY("warp_freq")) NUM((o->warp.frequency = f, warp_freq = true));
    else if (KEY("warp_seed")) NUM((o->warp.seed = (int32_t)f, warp_seed = true));
    else if (KEY("warp_fractal")) {
      static const char *const wf[] = { "none", "progressive", "independent" };
      ENUM(wf, 3, o->warp.fractal = e ? (cw_noise_fractal)(CW_NOISE_FRACTAL_WARP_PROGRESSIVE + e - 1) : CW_NOISE_FRACTAL_NONE);
    } else if (KEY("warp_octaves")) NUM(o->warp.octaves = (int32_t)f);
    else if (KEY("x")) NUM(o->x = f);
    else if (KEY("y")) NUM(o->y = f);
    else if (KEY("z")) NUM((o->z = f, o->three = true));
    else if (KEY("seamless")) NUM(o->seamless = f != 0);
    else if (KEY("out")) ENUM(out_names, 3, o->out = e);
    else if (KEY("strength")) NUM(o->strength = f);
    else if (KEY("min")) NUM(o->lo = f);
    else if (KEY("max")) NUM(o->hi = f);
    else {
      cw_error("gen://noise: unknown key \"%.*s\" (see cartwheel/noise.h)", (int)kl, p);
      return false;
    }
#undef KEY
#undef NUM
#undef ENUM
    p = amp ? amp + 1 : NULL;
  }
  if (o->w < 1 || o->h < 1 || o->w > 4096 || o->h > 4096) {
    cw_error("gen://noise: size %dx%d is out of range (1..4096)", o->w, o->h);
    return false;
  }
  if (o->hi == o->lo) {
    cw_error("gen://noise: min and max are equal");
    return false;
  }
  if (!warp_seed) o->warp.seed = o->n.seed + 100;
  if (!warp_freq) o->warp.frequency = o->n.frequency * 0.5f;
  o->warp.rotation = o->n.rotation;
  return true;
}

/* one layer of floats, w x h, seamless when asked */
static float *layer(const noise_query *q, int32_t seed) {
  int w = q->w, h = q->h;
  cw_noise n = q->n;
  n.seed = seed;
  const cw_noise *wp = q->warp.warp != CW_NOISE_WARP_NONE ? &q->warp : NULL;
  int fw = q->seamless ? w * 2 : w, fh = q->seamless ? h * 2 : h;
  float *f = cw_alloc((size_t)fw * (size_t)fh * sizeof *f, CW_MEM_RESOURCE);
  /* seamless: the same field over twice the size, starting a period before;
   * texel (c, r) blends (c, r), (c + w, r), (c, r + h), (c + w, r + h) */
  float x0 = q->seamless ? q->x - (float)w : q->x, y0 = q->seamless ? q->y - (float)h : q->y;
  if (q->three) cw_noise_fill_3d_slice(&n, wp, f, fw, fh, x0, y0, q->z, 1.0f);
  else cw_noise_fill_2d(&n, wp, f, fw, fh, x0, y0, 1.0f);
  if (!q->seamless) return f;
  float *o = cw_alloc((size_t)w * (size_t)h * sizeof *o, CW_MEM_RESOURCE);
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++) {
      /* tile(x) = n(x) (1 - u) + n(x - w) u: at u = 1 it is n(x - w) at
       * x = w, which is n(0), where u = 0 starts: the edges meet */
      float u = ((float)c + 0.5f) / (float)w, v = ((float)r + 0.5f) / (float)h;
      float a = f[(size_t)(r + h) * (size_t)fw + (size_t)(c + w)], b = f[(size_t)(r + h) * (size_t)fw + (size_t)c];
      float cc = f[(size_t)r * (size_t)fw + (size_t)(c + w)], d = f[(size_t)r * (size_t)fw + (size_t)c];
      float top = a * (1 - u) + b * u, bot = cc * (1 - u) + d * u;
      float x = top * (1 - v) + bot * v;
      /* blending two uncorrelated values shrinks the spread most in the middle: restore it */
      float k = (u * u + (1 - u) * (1 - u)) * (v * v + (1 - v) * (1 - v));
      o[(size_t)r * (size_t)w + (size_t)c] = x / sqrtf(k);
    }
  cw_free(f);
  return o;
}

static uint8_t to8(float v, float lo, float hi) {
  float t = (v - lo) / (hi - lo);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return (uint8_t)(int)(t * 255.0f + 0.5f);
}

bool cw__noise_image_query(const char *query, uint8_t **rgba, int *wo, int *ho) {
  noise_query q;
  *rgba = NULL;
  if (!parse(query, &q)) return false;
  int w = q.w, h = q.h;
  size_t n = (size_t)w * (size_t)h;
  uint8_t *px = cw_alloc(n * 4, CW_MEM_RESOURCE);
  if (q.out == 1) {
    for (int ch = 0; ch < 4; ch++) {
      float *f = layer(&q, q.n.seed + ch);
      for (size_t i = 0; i < n; i++) px[i * 4 + (size_t)ch] = to8(f[i], q.lo, q.hi);
      cw_free(f);
    }
  } else if (q.out == 2) {
    float *f = layer(&q, q.n.seed);
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++) {
        /* central differences, wrapping (a seamless map stays seamless) */
        int cl = (c + w - 1) % w, cr = (c + 1) % w, ru = (r + h - 1) % h, rd = (r + 1) % h;
        float s = q.strength * 0.5f;
        float dx = (f[(size_t)r * (size_t)w + (size_t)cr] - f[(size_t)r * (size_t)w + (size_t)cl]) * s;
        float dy = (f[(size_t)rd * (size_t)w + (size_t)c] - f[(size_t)ru * (size_t)w + (size_t)c]) * s;
        float il = 1.0f / sqrtf(dx * dx + dy * dy + 1.0f);
        uint8_t *o = px + ((size_t)r * (size_t)w + (size_t)c) * 4;
        o[0] = to8(-dx * il, -1, 1);
        o[1] = to8(-dy * il, -1, 1);
        o[2] = to8(il, -1, 1);
        o[3] = to8(f[(size_t)r * (size_t)w + (size_t)c], q.lo, q.hi);
      }
    cw_free(f);
  } else {
    float *f = layer(&q, q.n.seed);
    for (size_t i = 0; i < n; i++) {
      uint8_t v = to8(f[i], q.lo, q.hi);
      px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
      px[i * 4 + 3] = 255;
    }
    cw_free(f);
  }
  *rgba = px;
  *wo = w;
  *ho = h;
  return true;
}

bool cw_noise_image(const char *query, uint8_t **rgba, int *w, int *h) {
  if (query && !strncmp(query, "gen://noise", 11)) query += 11;
  else if (query && !strncmp(query, "gen:/noise", 10)) query += 10;
  if (query && *query == '?') query++;
  return cw__noise_image_query(query ? query : "", rgba, w, h);
}

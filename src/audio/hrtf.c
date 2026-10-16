/* HRTF: head-related filtering for spatial voices in headphones mode.
 *
 * Data: a .cwhrtf table (tools/hrtf/import-kemar.mjs writes it; the format
 * is described there): directions on the unit sphere, a minimum-phase FIR
 * per ear per direction (the spectrum), a delay per ear (the interaural time
 * difference, split out of the FIR), and a triangulation of the directions.
 * The built-in table is the MIT Media Lab KEMAR set (Gardner and Martin,
 * 1994, diffuse-field equalized; NOTICE), embedded below.
 *
 * Per voice and block: the direction in listener space selects a triangle
 * (barycentric weights from three dot products with the triangle's
 * precomputed inverse: no trig), the three FIRs and delays are blended by
 * the weights, each ear's input is the voice's mono signal delayed by the
 * ear's delay (linear interpolation of the fraction), and a 64-tap FIR runs
 * per ear. When the filter changed since the last block, the output of the
 * old filter and of the new one are crossfaded linearly across the block.
 *
 * Determinism: only + - * / and float compares (no FMA: -ffp-contract=off).
 * The FIR computes four output samples at once (simd4.h: SSE2, wasm SIMD128,
 * NEON or plain C); each lane is one output sample summing its 64 products
 * in tap order, exactly as the scalar tail does, so every ISA and the
 * scalar build give the same bits. */
#include <string.h>

#include "internal.h"
#include "../nav/simd4.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

/* CW_EMBED_INC: a compiler without #embed gets the same bytes generated as an initializer list (CMakeLists.txt) */
static const unsigned char kemar_cwhrtf[] = {
#if defined(CW_EMBED_INC)
#include "kemar.cwhrtf.inc"
#else
#embed "hrtf/kemar.cwhrtf"
#endif
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t rd16(const uint8_t *p) { return (int16_t)(uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8); }

const void *cw__hrtf_builtin(size_t *len) {
  *len = sizeof kemar_cwhrtf;
  return kemar_cwhrtf;
}

cw__hrtf *cw__hrtf_parse(const void *data, size_t len, int mix_rate) {
  const uint8_t *b = data;
  if (len < 32 || memcmp(b, "CWHRTF01", 8)) {
    cw_error("audio: not a .cwhrtf table (make one with tools/hrtf/import-kemar.mjs)");
    return NULL;
  }
  uint32_t rate = rd32(b + 8), taps = rd32(b + 12), count = rd32(b + 16), ntris = rd32(b + 20);
  float scale;
  memcpy(&scale, b + 24, 4);
  size_t need = 32 + (size_t)count * 12 + (size_t)count * 2 * taps * 2 + (size_t)ntris * 6;
  if (taps < 8 || taps > 512 || count < 4 || count > 65535 || ntris < 4 || rate < 8000 || len < need) {
    cw_error("audio: .cwhrtf table is truncated or out of range (%u taps, %u directions)", taps, count);
    return NULL;
  }
  cw__hrtf *h = cw_calloc(1, sizeof *h, CW_MEM_AUDIO);
  h->count = (int)count;
  h->ntris = (int)ntris;
  h->taps = cw_calloc((size_t)count * 2 * CW__HRTF_TAPS, sizeof(float), CW_MEM_AUDIO);
  h->delay = cw_calloc((size_t)count * 2, sizeof(float), CW_MEM_AUDIO);
  h->dirs = cw_calloc((size_t)count * 3, sizeof(float), CW_MEM_AUDIO);
  h->tris = cw_calloc((size_t)ntris * 3, sizeof(uint16_t), CW_MEM_AUDIO);
  h->inv = cw_calloc((size_t)ntris * 9, sizeof(float), CW_MEM_AUDIO);
  /* the table's rate vs the mixer's: taps are resampled linearly (scaled so
   * the response keeps its level) and delays scaled; equal rates copy exactly */
  float r = (float)rate / (float)mix_rate;
  const uint8_t *dp = b + 32, *tp = b + 32 + (size_t)count * 12;
  for (uint32_t d = 0; d < count; d++, dp += 12) {
    for (int c = 0; c < 3; c++) h->dirs[d * 3 + c] = (float)rd16(dp + c * 2) * (1.0f / 32767.0f);
    for (int e = 0; e < 2; e++) {
      float dl = (float)rd16(dp + 6 + e * 2) * (1.0f / 256.0f) / r;
      h->delay[d * 2 + e] = cw_clampf(dl, 0, (float)(CW__HRTF_MAXDELAY - 1));
      const uint8_t *src = tp + ((size_t)d * 2 + (size_t)e) * taps * 2;
      float *dst = h->taps + ((size_t)d * 2 + (size_t)e) * CW__HRTF_TAPS;
      for (int k = 0; k < CW__HRTF_TAPS; k++) { /* stored reversed: dst[63 - k] = tap k */
        float v;
        if (rate == (uint32_t)mix_rate) v = k < (int)taps ? (float)rd16(src + k * 2) * scale : 0.0f;
        else {
          float t = (float)k * r;
          int i0 = (int)t;
          float f = t - (float)i0;
          float a = i0 < (int)taps ? (float)rd16(src + i0 * 2) * scale : 0.0f;
          float c = i0 + 1 < (int)taps ? (float)rd16(src + (i0 + 1) * 2) * scale : 0.0f;
          v = (a + (c - a) * f) * r;
        }
        dst[CW__HRTF_TAPS - 1 - k] = v;
      }
    }
  }
  const uint8_t *rp = tp + (size_t)count * 2 * taps * 2;
  for (uint32_t t = 0; t < ntris; t++) {
    for (int c = 0; c < 3; c++) {
      uint16_t i = (uint16_t)rd16(rp + (t * 3 + (uint32_t)c) * 2);
      if (i >= count) {
        cw_error("audio: .cwhrtf triangle %u names direction %u of %u", t, i, count);
        cw__hrtf_free(h);
        return NULL;
      }
      h->tris[t * 3 + (uint32_t)c] = i;
    }
    /* rows of the inverse of [a b c]: w = inv * d gives d = wa a + wb b + wc c */
    const float *a = h->dirs + h->tris[t * 3] * 3, *bb = h->dirs + h->tris[t * 3 + 1] * 3, *c = h->dirs + h->tris[t * 3 + 2] * 3;
    float bc[3] = { bb[1] * c[2] - bb[2] * c[1], bb[2] * c[0] - bb[0] * c[2], bb[0] * c[1] - bb[1] * c[0] };
    float ca[3] = { c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0] };
    float ab[3] = { a[1] * bb[2] - a[2] * bb[1], a[2] * bb[0] - a[0] * bb[2], a[0] * bb[1] - a[1] * bb[0] };
    float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    float id = det != 0 ? 1.0f / det : 0.0f;
    float *m = h->inv + t * 9;
    for (int k = 0; k < 3; k++) {
      m[k] = bc[k] * id;
      m[3 + k] = ca[k] * id;
      m[6 + k] = ab[k] * id;
    }
  }
  /* neighbours: across the edge opposite vertex k of triangle t (-1 on an open edge) */
  h->adj = cw_alloc((size_t)ntris * 3 * sizeof(int32_t), CW_MEM_AUDIO);
  for (uint32_t i = 0; i < ntris * 3; i++) h->adj[i] = -1;
  for (uint32_t t = 0; t < ntris; t++)
    for (int k = 0; k < 3; k++) {
      if (h->adj[t * 3 + (uint32_t)k] >= 0) continue;
      uint16_t a = h->tris[t * 3 + (uint32_t)(k + 1) % 3], b = h->tris[t * 3 + (uint32_t)(k + 2) % 3];
      for (uint32_t u = 0; u < ntris && h->adj[t * 3 + (uint32_t)k] < 0; u++) {
        if (u == t) continue;
        for (int j = 0; j < 3; j++) {
          uint16_t c = h->tris[u * 3 + (uint32_t)(j + 1) % 3], d = h->tris[u * 3 + (uint32_t)(j + 2) % 3];
          if ((a == c && b == d) || (a == d && b == c)) {
            h->adj[t * 3 + (uint32_t)k] = (int32_t)u;
            h->adj[u * 3 + (uint32_t)j] = (int32_t)t;
            break;
          }
        }
      }
    }
  return h;
}

void cw__hrtf_free(cw__hrtf *h) {
  if (!h) return;
  cw_free(h->taps);
  cw_free(h->delay);
  cw_free(h->dirs);
  cw_free(h->tris);
  cw_free(h->inv);
  cw_free(h->adj);
  cw_free(h);
}

/* Barycentric weights of d in triangle t; returns the smallest. */
static float tri_weights(const cw__hrtf *h, int t, t3_vec3 d, float w[3]) {
  const float *m = h->inv + t * 9;
  w[0] = m[0] * d.x + m[1] * d.y + m[2] * d.z;
  w[1] = m[3] * d.x + m[4] * d.y + m[5] * d.z;
  w[2] = m[6] * d.x + m[7] * d.y + m[8] * d.z;
  float lo = w[0] < w[1] ? w[0] : w[1];
  return lo < w[2] ? lo : w[2];
}

int cw__hrtf_find(const cw__hrtf *h, t3_vec3 d, int hint, float w[3]) {
  const float eps = -1e-5f;
  /* walk from the last triangle toward d: cross the edge opposite the most
   * negative weight (a moving source is found in a step or two) */
  if (hint >= 0 && hint < h->ntris) {
    int t = hint;
    for (int step = 0; step < 24; step++) {
      float lo = tri_weights(h, t, d, w);
      if (lo >= eps) return t;
      int k = w[0] <= w[1] && w[0] <= w[2] ? 0 : w[1] <= w[2] ? 1 : 2;
      int next = h->adj[t * 3 + k];
      if (next < 0) break;
      t = next;
    }
  }
  int best = 0;
  float best_lo = -1e30f, bw[3] = { 1, 0, 0 };
  for (int t = 0; t < h->ntris; t++) {
    float lo = tri_weights(h, t, d, w);
    if (lo >= eps) return t;
    if (lo > best_lo) { best_lo = lo; best = t; memcpy(bw, w, sizeof bw); }
  }
  memcpy(w, bw, sizeof bw);
  return best;
}

void cw__hrtf_set_direction(const cw__hrtf *h, cw__hstate *s, t3_vec3 d) {
  float w[3];
  int t = cw__hrtf_find(h, d, s->tri, w);
  for (int k = 0; k < 3; k++) w[k] = w[k] > 0 ? w[k] : 0.0f;
  float sum = w[0] + w[1] + w[2];
  if (sum > 0) for (int k = 0; k < 3; k++) w[k] = w[k] / sum;
  else w[0] = 1, w[1] = w[2] = 0;
  if (s->valid && t == s->tri && !memcmp(w, s->w, sizeof w)) { s->changed = false; return; }
  int nxt = s->valid ? s->cur ^ 1 : s->cur;
  const uint16_t *tri = h->tris + t * 3;
  for (int e = 0; e < 2; e++) {
    const float *a = h->taps + ((size_t)tri[0] * 2 + (size_t)e) * CW__HRTF_TAPS;
    const float *b = h->taps + ((size_t)tri[1] * 2 + (size_t)e) * CW__HRTF_TAPS;
    const float *c = h->taps + ((size_t)tri[2] * 2 + (size_t)e) * CW__HRTF_TAPS;
    float *o = s->taps[nxt][e];
    for (int k = 0; k < CW__HRTF_TAPS; k++) o[k] = (a[k] * w[0] + b[k] * w[1]) + c[k] * w[2];
    s->delay[nxt][e] = (h->delay[tri[0] * 2 + e] * w[0] + h->delay[tri[1] * 2 + e] * w[1]) + h->delay[tri[2] * 2 + e] * w[2];
  }
  s->changed = s->valid;
  s->cur = nxt;
  s->tri = t;
  memcpy(s->w, w, sizeof w);
  s->valid = true;
}

/* u[i] = x[i - delay] for i in [-(TAPS-1), n): the ear's delayed input. x
 * points at the block's first sample inside the history. */
static void delayed(const float *x, int n, float delay, float *u) {
  int di = (int)delay;
  float f = delay - (float)di;
  for (int i = -(CW__HRTF_TAPS - 1); i < n; i++) {
    const float *p = x + i - di;
    u[i + CW__HRTF_TAPS - 1] = p[0] + (p[-1] - p[0]) * f;
  }
}

/* y[i] = sum over k in tap order of hr[k] * u[i + k] (hr reversed taps) */
void cw__hrtf_fir(const float *hr, const float *u, int n, float *y) {
  int i = 0;
  for (; i + 16 <= n; i += 16) { /* 16 outputs per pass: one tap splat feeds four vectors */
    v4 a0 = v4_splat(0), a1 = a0, a2 = a0, a3 = a0;
    const float *p = u + i;
    for (int k = 0; k < CW__HRTF_TAPS; k++, p++) {
      v4 hk = v4_splat(hr[k]);
      a0 = v4_add(a0, v4_mul(hk, v4_load(p)));
      a1 = v4_add(a1, v4_mul(hk, v4_load(p + 4)));
      a2 = v4_add(a2, v4_mul(hk, v4_load(p + 8)));
      a3 = v4_add(a3, v4_mul(hk, v4_load(p + 12)));
    }
    v4_store(y + i, a0);
    v4_store(y + i + 4, a1);
    v4_store(y + i + 8, a2);
    v4_store(y + i + 12, a3);
  }
  for (; i + 4 <= n; i += 4) {
    v4 acc = v4_splat(0);
    for (int k = 0; k < CW__HRTF_TAPS; k++) acc = v4_add(acc, v4_mul(v4_splat(hr[k]), v4_load(u + i + k)));
    v4_store(y + i, acc);
  }
  for (; i < n; i++) {
    float acc = 0;
    for (int k = 0; k < CW__HRTF_TAPS; k++) acc = acc + hr[k] * u[i + k];
    y[i] = acc;
  }
}

/* The plain C reference (the unit test checks the SIMD path against it). */
void cw__hrtf_fir_scalar(const float *hr, const float *u, int n, float *y) {
  for (int i = 0; i < n; i++) {
    float acc = 0;
    for (int k = 0; k < CW__HRTF_TAPS; k++) acc = acc + hr[k] * u[i + k];
    y[i] = acc;
  }
}

void cw__hrtf_render(cw__hstate *s, int n, float *out, float m0, float m1) {
  float *x = s->hist + CW__HRTF_PRE;
  float u[CW__HRTF_TAPS - 1 + CW_AUDIO_BLOCK];
  float yc[2][CW_AUDIO_BLOCK], yp[2][CW_AUDIO_BLOCK];
  for (int e = 0; e < 2; e++) {
    delayed(x, n, s->delay[s->cur][e], u);
    cw__hrtf_fir(s->taps[s->cur][e], u, n, yc[e]);
    if (s->changed) {
      delayed(x, n, s->delay[s->cur ^ 1][e], u);
      cw__hrtf_fir(s->taps[s->cur ^ 1][e], u, n, yp[e]);
    }
  }
  float dm = (m1 - m0) / (float)n, m = m0;
  if (s->changed) {
    float inv = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
      float t = (float)(i + 1) * inv;
      m += dm;
      out[2 * i] += (yp[0][i] + (yc[0][i] - yp[0][i]) * t) * m;
      out[2 * i + 1] += (yp[1][i] + (yc[1][i] - yp[1][i]) * t) * m;
    }
    s->changed = false;
  } else {
    for (int i = 0; i < n; i++) {
      m += dm;
      out[2 * i] += yc[0][i] * m;
      out[2 * i + 1] += yc[1][i] * m;
    }
  }
  /* keep the last PRE input samples for the next block */
  memmove(s->hist, s->hist + n, CW__HRTF_PRE * sizeof(float));
}

void cw__hrtf_reset(cw__hstate *s) {
  memset(s, 0, sizeof *s);
  s->tri = -1;
}

/* FDN reverb (Jot and Chaigne 1991): 8 delay lines with mutually prime
 * lengths, fed back through an 8x8 Hadamard matrix (adds and subtracts, one
 * scale), a per-line gain that sets the decay time and a one-pole low-pass
 * per line for high-frequency damping. A pre-delay line sits in front.
 *
 * Used by CW_FX_FDN_REVERB on a bus and by every reverb zone. The line
 * lengths (room size) and the pre-delay are fixed when it is made (changing
 * them while it rings would click); decay, damping, wet and dry follow
 * cw__fdn_set.
 *
 * Determinism: + - * / only, every transcendental from cartwheel/math.h
 * (and only when parameters change), fixed loop order. */
#include <string.h>

#include "internal.h"

/* prime line lengths at 48 kHz for size 1 (about 21 to 60 ms) */
static const int base_len[CW__FDN_LINES] = { 1031, 1327, 1523, 1801, 2017, 2311, 2579, 2861 };

static int is_prime(int n) {
  if (n < 2) return 0;
  for (int d = 2; d * d <= n; d++)
    if (n % d == 0) return 0;
  return 1;
}

bool cw__fdn_init(cw__fdn *f, float size, float predelay_s, int rate) {
  memset(f, 0, sizeof *f);
  f->rate = rate;
  float scale = (0.35f + 1.25f * cw_clampf(size, 0, 1)) * ((float)rate / 48000.0f);
  int prev = 0;
  for (int i = 0; i < CW__FDN_LINES; i++) {
    int n = (int)((float)base_len[i] * scale);
    if (n <= prev) n = prev + 1;
    while (!is_prime(n)) n++; /* mutually prime: distinct primes */
    prev = n;
    f->len[i] = n;
    f->line[i] = cw_calloc((size_t)n, sizeof(float), CW_MEM_AUDIO);
  }
  f->pre_len = (int)(cw_clampf(predelay_s, 0, 0.5f) * (float)rate);
  if (f->pre_len > 0) f->pre = cw_calloc((size_t)f->pre_len, sizeof(float), CW_MEM_AUDIO);
  f->wet = 1;
  return true;
}

void cw__fdn_free(cw__fdn *f) {
  for (int i = 0; i < CW__FDN_LINES; i++) cw_free(f->line[i]);
  cw_free(f->pre);
  memset(f, 0, sizeof *f);
}

void cw__fdn_set(cw__fdn *f, float decay_s, float hf_damp_hz, float wet, float dry) {
  float t = decay_s > 0.05f ? decay_s : 0.05f;
  /* gain per pass so a line's loop loses 60 dB in t seconds: 10^(-3 len / (rate t)) */
  for (int i = 0; i < CW__FDN_LINES; i++) f->g[i] = cw_exp(-6.9077553f * (float)f->len[i] / ((float)f->rate * t));
  f->damp = hf_damp_hz > 0 ? 1.0f - cw_exp(-CW_TAU * cw_clampf(hf_damp_hz, 20, 0.45f * (float)f->rate) / (float)f->rate) : 1.0f;
  f->wet = wet;
  f->dry = dry;
}

void cw__fdn_clear(cw__fdn *f) {
  for (int i = 0; i < CW__FDN_LINES; i++) memset(f->line[i], 0, (size_t)f->len[i] * sizeof(float));
  if (f->pre) memset(f->pre, 0, (size_t)f->pre_len * sizeof(float));
  memset(f->lp, 0, sizeof f->lp);
}

/* One sample: mono in, stereo wet out. */
static inline void fdn_tick(cw__fdn *f, float in, float *ol, float *orr) {
  float x = in;
  if (f->pre) {
    float d = f->pre[f->pre_pos];
    f->pre[f->pre_pos] = in;
    if (++f->pre_pos == f->pre_len) f->pre_pos = 0;
    x = d;
  }
  float o[CW__FDN_LINES], h[CW__FDN_LINES];
  for (int i = 0; i < CW__FDN_LINES; i++) {
    float v = f->line[i][f->pos[i]];
    float lp = f->lp[i] + f->damp * (v - f->lp[i]);
    f->lp[i] = (lp > -1e-20f && lp < 1e-20f) ? 0.0f : lp; /* no denormals in the loop */
    o[i] = f->lp[i];
  }
  /* fast Walsh-Hadamard transform, scaled by 1/sqrt(8): orthogonal */
  for (int i = 0; i < CW__FDN_LINES; i++) h[i] = o[i];
  for (int span = 1; span < CW__FDN_LINES; span <<= 1)
    for (int i = 0; i < CW__FDN_LINES; i += span << 1)
      for (int j = i; j < i + span; j++) {
        float a = h[j], b = h[j + span];
        h[j] = a + b;
        h[j + span] = a - b;
      }
  const float norm = 0.35355339f;
  for (int i = 0; i < CW__FDN_LINES; i++) {
    float in_i = (i & 1) ? -x : x;
    f->line[i][f->pos[i]] = in_i + f->g[i] * (h[i] * norm);
    if (++f->pos[i] == f->len[i]) f->pos[i] = 0;
  }
  *ol = ((o[0] - o[2]) + (o[4] - o[6])) * 0.5f;
  *orr = ((o[1] - o[3]) + (o[5] - o[7])) * 0.5f;
}

void cw__fdn_process(cw__fdn *f, float *buf, int frames) {
  for (int i = 0; i < frames; i++) {
    float l = buf[2 * i], r = buf[2 * i + 1], wl, wr;
    fdn_tick(f, (l + r) * 0.5f, &wl, &wr);
    buf[2 * i] = l * f->dry + wl * f->wet;
    buf[2 * i + 1] = r * f->dry + wr * f->wet;
  }
}

float cw__fdn_process_mono(cw__fdn *f, const float *in, float g0, float g1, int frames, float *out) {
  float g = g0, dg = (g1 - g0) / (float)frames, peak = 0;
  for (int i = 0; i < frames; i++) {
    g += dg;
    float wl, wr;
    fdn_tick(f, in[i] * g, &wl, &wr);
    wl *= f->wet;
    wr *= f->wet;
    out[2 * i] += wl;
    out[2 * i + 1] += wr;
    float a = wl < 0 ? -wl : wl, b = wr < 0 ? -wr : wr;
    if (a > peak) peak = a;
    if (b > peak) peak = b;
  }
  return peak;
}

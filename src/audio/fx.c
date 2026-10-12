/* Bus effects. Filters run on miniaudio's biquad and the echo on its
 * ma_delay; reverb (Freeverb), compressor and limiter are here. Every
 * coefficient comes from cartwheel/math.h (cw_sin, cw_exp, cw_pow), never
 * libm, so the output is bit-identical on every host. Buffers are allocated
 * in cw__fx_create on the game thread; processing never allocates. */
#include <string.h>

#include "internal.h"

static float db_to_lin(float db) { return cw_pow(10.0f, db / 20.0f); }
/* one-pole smoothing coefficient for a time constant in seconds */
static float time_coef(float seconds, int rate) {
  if (seconds <= 0) return 0;
  return cw_exp(-1.0f / (seconds * (float)rate));
}

static ma_biquad_config filter_config(const cw_fx_desc *d, int rate) {
  float fc = cw_clampf(d->filter.cutoff_hz, 10.0f, 0.49f * (float)rate);
  float q = d->filter.q > 0 ? d->filter.q : 0.70710678f;
  float s, c;
  cw_sincos(CW_TAU * fc / (float)rate, &s, &c);
  float alpha = s / (2.0f * q);
  double b0, b1, b2;
  if (d->type == CW_FX_LOWPASS) {
    b0 = (1.0f - c) * 0.5f;
    b1 = 1.0f - c;
    b2 = b0;
  } else {
    b0 = (1.0f + c) * 0.5f;
    b1 = -(1.0f + c);
    b2 = b0;
  }
  return ma_biquad_config_init(ma_format_f32, 2, b0, b1, b2, 1.0f + alpha, -2.0f * c, 1.0f - alpha);
}

/* Freeverb's tunings at 44.1 kHz */
static const int comb_tuning[CW__RV_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int ap_tuning[CW__RV_ALLPASSES] = { 556, 441, 341, 225 };
#define RV_SPREAD 23

static void reverb_params(cw__fx *fx) {
  const cw_fx_desc *d = &fx->desc;
  float room = cw_clampf(d->reverb.room_size, 0, 1), damp = cw_clampf(d->reverb.damping, 0, 1);
  float wet = cw_clampf(d->reverb.wet, 0, 1) * 3.0f, width = cw_clampf(d->reverb.width, 0, 1);
  fx->rv.feedback = room * 0.28f + 0.7f;
  fx->rv.damp1 = damp * 0.4f;
  fx->rv.damp2 = 1.0f - fx->rv.damp1;
  fx->rv.wet1 = wet * (width * 0.5f + 0.5f);
  fx->rv.wet2 = wet * ((1.0f - width) * 0.5f);
  fx->rv.dry = cw_clampf(d->reverb.dry, 0, 1) * 2.0f;
  fx->rv.gain = 0.015f;
}

static void dyn_params(cw__fx *fx) {
  const cw_fx_desc *d = &fx->desc;
  if (d->type == CW_FX_COMPRESSOR) {
    fx->dyn.thr = db_to_lin(d->compressor.threshold_db);
    float ratio = d->compressor.ratio >= 1 ? d->compressor.ratio : 4.0f;
    fx->dyn.slope = 1.0f - 1.0f / ratio;
    fx->dyn.att = time_coef(d->compressor.attack_s > 0 ? d->compressor.attack_s : 0.005f, fx->rate);
    fx->dyn.rel = time_coef(d->compressor.release_s > 0 ? d->compressor.release_s : 0.1f, fx->rate);
    fx->dyn.makeup = db_to_lin(d->compressor.makeup_db);
  } else {
    fx->dyn.ceil = db_to_lin(d->limiter.ceiling_db < 0 ? d->limiter.ceiling_db : -0.1f);
    fx->dyn.rel = time_coef(d->limiter.release_s > 0 ? d->limiter.release_s : 0.05f, fx->rate);
  }
}

cw__fx *cw__fx_create(const cw_fx_desc *d, int rate) {
  cw__fx *fx = cw_calloc(1, sizeof *fx, CW_MEM_AUDIO);
  fx->desc = *d;
  fx->rate = rate;
  switch (d->type) {
  case CW_FX_LOWPASS:
  case CW_FX_HIGHPASS: {
    ma_biquad_config c = filter_config(d, rate);
    if (ma_biquad_init(&c, NULL, &fx->bq) != MA_SUCCESS) goto fail;
    break;
  }
  case CW_FX_DELAY: {
    float t = d->delay.time_s > 0 ? d->delay.time_s : 0.25f;
    ma_uint32 frames = (ma_uint32)(t * (float)rate);
    if (frames < 1) frames = 1;
    /* delayStart: the buffer holds the input, the output is the echo only;
     * dry and wet are mixed in cw__fx_process */
    ma_delay_config c = ma_delay_config_init(2, (ma_uint32)rate, frames, cw_clampf(d->delay.feedback, 0, 0.98f));
    c.delayStart = MA_TRUE;
    if (ma_delay_init(&c, NULL, &fx->delay) != MA_SUCCESS) goto fail;
    break;
  }
  case CW_FX_REVERB:
    for (int ch = 0; ch < 2; ch++) {
      int spread = ch ? RV_SPREAD : 0;
      for (int i = 0; i < CW__RV_COMBS; i++) {
        int n = (int)((int64_t)(comb_tuning[i] + spread) * rate / 44100);
        fx->rv.comb_len[ch][i] = n;
        fx->rv.comb[ch][i] = cw_calloc((size_t)n, sizeof(float), CW_MEM_AUDIO);
      }
      for (int i = 0; i < CW__RV_ALLPASSES; i++) {
        int n = (int)((int64_t)(ap_tuning[i] + spread) * rate / 44100);
        fx->rv.ap_len[ch][i] = n;
        fx->rv.ap[ch][i] = cw_calloc((size_t)n, sizeof(float), CW_MEM_AUDIO);
      }
    }
    reverb_params(fx);
    break;
  case CW_FX_COMPRESSOR:
  case CW_FX_LIMITER: dyn_params(fx); break;
  default: goto fail;
  }
  return fx;
fail:
  cw_error("audio: bad effect description (type %d)", (int)d->type);
  cw_free(fx);
  return NULL;
}

void cw__fx_destroy(cw__fx *fx) {
  if (!fx) return;
  switch (fx->desc.type) {
  case CW_FX_LOWPASS:
  case CW_FX_HIGHPASS: ma_biquad_uninit(&fx->bq, NULL); break;
  case CW_FX_DELAY: ma_delay_uninit(&fx->delay, NULL); break;
  case CW_FX_REVERB:
    for (int ch = 0; ch < 2; ch++) {
      for (int i = 0; i < CW__RV_COMBS; i++) cw_free(fx->rv.comb[ch][i]);
      for (int i = 0; i < CW__RV_ALLPASSES; i++) cw_free(fx->rv.ap[ch][i]);
    }
    break;
  default: break;
  }
  cw_free(fx);
}

void cw__fx_set(cw__fx *fx, const cw_fx_desc *d) {
  if (d->type != fx->desc.type) return;
  float keep_time = fx->desc.delay.time_s;
  fx->desc = *d;
  switch (d->type) {
  case CW_FX_LOWPASS:
  case CW_FX_HIGHPASS: {
    ma_biquad_config c = filter_config(d, fx->rate);
    ma_biquad_reinit(&c, &fx->bq); /* keeps the filter state: no click */
    break;
  }
  case CW_FX_DELAY:
    fx->desc.delay.time_s = keep_time;
    ma_delay_set_decay(&fx->delay, cw_clampf(d->delay.feedback, 0, 0.98f));
    break;
  case CW_FX_REVERB: reverb_params(fx); break;
  default: dyn_params(fx); break;
  }
}

/* Flush values too small to hear, so long reverb tails never decay into
 * denormals (slow on x86; identical results either way). */
static inline float undenormal(float x) { return (x > -1e-20f && x < 1e-20f) ? 0.0f : x; }

static void reverb_process(cw__fx *fx, float *buf, int frames) {
  for (int i = 0; i < frames; i++) {
    float in = (buf[2 * i] + buf[2 * i + 1]) * fx->rv.gain;
    float out[2] = { 0, 0 };
    for (int ch = 0; ch < 2; ch++) {
      float acc = 0;
      for (int c = 0; c < CW__RV_COMBS; c++) {
        float *b = fx->rv.comb[ch][c];
        int p = fx->rv.comb_pos[ch][c];
        float y = b[p];
        float st = undenormal(y * fx->rv.damp2 + fx->rv.comb_store[ch][c] * fx->rv.damp1);
        fx->rv.comb_store[ch][c] = st;
        b[p] = undenormal(in + st * fx->rv.feedback);
        if (++p >= fx->rv.comb_len[ch][c]) p = 0;
        fx->rv.comb_pos[ch][c] = p;
        acc += y;
      }
      for (int a = 0; a < CW__RV_ALLPASSES; a++) {
        float *b = fx->rv.ap[ch][a];
        int p = fx->rv.ap_pos[ch][a];
        float bo = b[p];
        float y = -acc + bo;
        b[p] = undenormal(acc + bo * 0.5f);
        if (++p >= fx->rv.ap_len[ch][a]) p = 0;
        fx->rv.ap_pos[ch][a] = p;
        acc = y;
      }
      out[ch] = acc;
    }
    float l = buf[2 * i], r = buf[2 * i + 1];
    buf[2 * i] = out[0] * fx->rv.wet1 + out[1] * fx->rv.wet2 + l * fx->rv.dry;
    buf[2 * i + 1] = out[1] * fx->rv.wet1 + out[0] * fx->rv.wet2 + r * fx->rv.dry;
  }
}

static void delay_process(cw__fx *fx, float *buf, int frames) {
  float echo[CW_AUDIO_BLOCK * 2];
  float wet = fx->desc.delay.wet, dry = fx->desc.delay.dry;
  for (int off = 0; off < frames; off += CW_AUDIO_BLOCK) {
    int n = CW_MIN(CW_AUDIO_BLOCK, frames - off);
    float *b = buf + off * 2;
    ma_delay_process_pcm_frames(&fx->delay, echo, b, (ma_uint32)n);
    for (int i = 0; i < n * 2; i++) b[i] = b[i] * dry + undenormal(echo[i]) * wet;
  }
}

static void compressor_process(cw__fx *fx, float *buf, int frames) {
  float env = fx->dyn.env;
  for (int i = 0; i < frames; i++) {
    float l = buf[2 * i], r = buf[2 * i + 1];
    float pk = CW_MAX(l < 0 ? -l : l, r < 0 ? -r : r);
    float k = pk > env ? fx->dyn.att : fx->dyn.rel;
    env = undenormal(pk + (env - pk) * k);
    float g = env > fx->dyn.thr ? cw_pow(fx->dyn.thr / env, fx->dyn.slope) : 1.0f;
    g *= fx->dyn.makeup;
    buf[2 * i] = l * g;
    buf[2 * i + 1] = r * g;
  }
  fx->dyn.env = env;
}

static void limiter_process(cw__fx *fx, float *buf, int frames) {
  float env = fx->dyn.env, ceil = fx->dyn.ceil;
  for (int i = 0; i < frames; i++) {
    float l = buf[2 * i], r = buf[2 * i + 1];
    float pk = CW_MAX(l < 0 ? -l : l, r < 0 ? -r : r);
    env = pk > env ? pk : undenormal(env * fx->dyn.rel); /* instant attack */
    if (env < pk) env = pk;
    float g = env > ceil ? ceil / env : 1.0f;
    buf[2 * i] = cw_clampf(l * g, -ceil, ceil);
    buf[2 * i + 1] = cw_clampf(r * g, -ceil, ceil);
  }
  fx->dyn.env = env;
}

void cw__fx_process(cw__fx *fx, float *buf, int frames) {
  if (fx->bypass) return;
  switch (fx->desc.type) {
  case CW_FX_LOWPASS:
  case CW_FX_HIGHPASS: ma_biquad_process_pcm_frames(&fx->bq, buf, buf, (ma_uint64)frames); break;
  case CW_FX_DELAY: delay_process(fx, buf, frames); break;
  case CW_FX_REVERB: reverb_process(fx, buf, frames); break;
  case CW_FX_COMPRESSOR: compressor_process(fx, buf, frames); break;
  case CW_FX_LIMITER: limiter_process(fx, buf, frames); break;
  default: break;
  }
}

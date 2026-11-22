/* Tempo with the pitch kept (cw_voice_set_tempo, cw_voices_set_tempo, cw_play_desc.tempo), offline. Each check
 * publishes a result field (tempo.*) and has a gate control that must fail it (tests/gates.json, unit-audio-tempo*):
 *   pitch_ok   a tone and a chord keep their pitch (within 2 cents) at tempo 0.75 and 1.5 while the duration scales;
 *              control CW_TEST_TEMPO_AS_PITCH plays the same speeds as pitch
 *   lock_ok    five stems (three samples, two streams) started together and given tempo changes as a group stay
 *              locked to the sample (their mix equals one voice's, every clock equal); control CW_TEST_TEMPO_SPLIT
 *              gives each stem the changes a block apart
 *   clock_ok   cw_voice_frames_played is the source position heard: a click track's clicks are heard within 128 source
 *              frames of where the clock says, through a pre-fed start (from the top and from an offset), changes
 *              while stretching, and in and out of the bypass; control
 *              CW_TEST_TEMPO_CLOCK_OUTPUT reads the frames played out instead
 *   bypass_ok  tempo 1 (never set, set in the play, set again) mixes bit for bit what no tempo does; control
 *              CW_TEST_TEMPO_FORCE runs the stretcher at tempo 1
 *   click_ok   no click across tempo changes in and out of the bypass (the largest step between samples of a
 *              440 Hz tone stays under 0.05); control CW_TEST_TEMPO_NAIVE switches with no warm up or cross-fade
 * Timings (tempo.ms10_*: ms of one core per 10 ms of sound, five stereo streams stretching) are never compared. */
#include <math.h>
#include <string.h>

#include "cartwheel/audio.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"
#include "test.h"
#include "../../src/audio/internal.h"

#define R 48000
#define MAXF (R * 12)
static float out[MAXF * 2];
static int32_t pitch_ok, lock_ok, clock_ok, bypass_ok, click_ok, done;
static float cents[4], lock_diff, clock_err, click_delta;
static uint32_t clock_hash = CW_HASH32_INIT, out_hash = CW_HASH32_INIT;
static float ms10[5]; /* tempo 0.5, 0.8, 1.25, 2, and 1 (the bypass) */

static void start(void) {
  cw_audio_init(&(cw_audio_desc){ .sample_rate = R, .offline = true, .max_voices = 16 });
}
static void stop(void) {
  cw_audio_shutdown();
  cw_mem_stats ms;
  cw_mem_get_stats(&ms);
  CHECK_EQ_INT(ms.bytes[CW_MEM_AUDIO], 0);
}

/* a 16-bit WAV in memory (streams and samples decode the same floats from it) */
static uint8_t *wav16(const float *pcm, int frames, int ch, size_t *len) {
  size_t n = (size_t)frames * (size_t)ch;
  uint8_t *w = cw_alloc(44 + n * 2, CW_MEM_GENERAL);
  uint32_t u32;
  uint16_t u16;
#define P32(o, v) (u32 = (uint32_t)(v), memcpy(w + (o), &u32, 4))
#define P16(o, v) (u16 = (uint16_t)(v), memcpy(w + (o), &u16, 2))
  memcpy(w, "RIFF", 4);
  P32(4, 36 + n * 2);
  memcpy(w + 8, "WAVEfmt ", 8);
  P32(16, 16); P16(20, 1); P16(22, ch); P32(24, R); P32(28, R * ch * 2); P16(32, ch * 2); P16(34, 16);
  memcpy(w + 36, "data", 4);
  P32(40, n * 2);
  for (size_t i = 0; i < n; i++) {
    float x = cw_clampf(pcm[i], -1, 1);
    int16_t s = (int16_t)(x * 32767.0f);
    memcpy(w + 44 + i * 2, &s, 2);
  }
  *len = 44 + n * 2;
  return w;
}
static cw_sound from_wav(const float *pcm, int frames, int ch, bool stream) {
  size_t len;
  uint8_t *w = wav16(pcm, frames, ch, &len);
  cw_sound s = cw_sound_load_memory(w, len, &(cw_sound_desc){ .mode = stream ? CW_SOUND_STREAM : CW_SOUND_SAMPLE });
  cw_free(w);
  return s;
}

/* Hann-windowed Goertzel power at f over out[from..from+n) (left channel) */
static double power_at(int from, int n, double f) {
  double w = 2.0 * 3.14159265358979323846 * f / R, c = 2.0 * cos(w), s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) {
    double h = 0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * i / (n - 1));
    double s0 = out[2 * (from + i)] * h + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;
}
/* the dominant frequency between lo and hi Hz: a 1 Hz scan, then golden-section to 0.001 Hz */
static double dominant(int from, int n, double lo, double hi) {
  double best = lo, bp = -1;
  for (double f = lo; f <= hi; f += 1.0) {
    double p = power_at(from, n, f);
    if (p > bp) bp = p, best = f;
  }
  double a = best - 1.0, b = best + 1.0, g = 0.6180339887498949;
  double x1 = b - g * (b - a), x2 = a + g * (b - a), p1 = power_at(from, n, x1), p2 = power_at(from, n, x2);
  while (b - a > 0.001) {
    if (p1 > p2) b = x2, x2 = x1, p2 = p1, x1 = b - g * (b - a), p1 = power_at(from, n, x1);
    else a = x1, x1 = x2, p1 = p2, x2 = a + g * (b - a), p2 = power_at(from, n, x2);
  }
  return (a + b) * 0.5;
}

/* ── pitch kept, duration scaled ─────────────────────────────────────── */
static void test_pitch(bool as_pitch) {
  const int len = R * 2;
  static float pcm[R * 2 * 2];
  bool ok = true;
  for (int k = 0; k < 2; k++) { /* a tone, then a chord whose root is the strongest partial */
    double root = k == 0 ? 440.0 : 220.0;
    for (int i = 0; i < len; i++) {
      float t = (float)i / R, x;
      if (k == 0) x = 0.5f * cw_sin(CW_TAU * 440.0f * t);
      else x = 0.4f * cw_sin(CW_TAU * 220.0f * t) + 0.2f * cw_sin(CW_TAU * 277.18f * t) + 0.2f * cw_sin(CW_TAU * 329.63f * t);
      pcm[2 * i] = pcm[2 * i + 1] = x;
    }
    for (int j = 0; j < 2; j++) {
      float tempo = j == 0 ? 0.75f : 1.5f;
      start();
      cw_sound s = from_wav(pcm, len, 2, true);
      cw_voice v = cw_audio_play(s, &(cw_play_desc){ .tempo = as_pitch ? 0 : tempo, .pitch = as_pitch ? tempo : 0 });
      int end = -1, frames = (int)((float)len / tempo) + 4096;
      for (int f = 0; f < frames; f += 256) {
        cw_audio_render(out + 2 * f, 256);
        if (end < 0 && !cw_voice_is_playing(v)) end = f;
      }
      int expect = (int)((float)len / tempo);
      double fm = dominant(R / 4, R / 2, root - 20, root + 20);
      double c = 1200.0 * log2(fm / root);
      cents[k * 2 + j] = (float)c;
      ok &= fabs(c) <= 2.0;
      ok &= end >= expect - 512 && end <= expect + 512; /* the duration scales (to the block) */
      cw_info("tempo pitch: %s %.2f: %.3f Hz (%+.2f cents), ended at %d (expected %d)", k ? "chord" : "tone", tempo, fm,
              c, end, expect);
      stop();
    }
  }
  pitch_ok = ok;
}

/* ── five stems locked to the sample ─────────────────────────────────── */
static float stem[R * 4 * 2];
static void make_stem(int frames) {
  cw_rng r;
  cw_rng_seed(&r, 7);
  for (int i = 0; i < frames; i++) {
    float t = (float)i / R;
    float x = 0.3f * cw_sin(CW_TAU * 110.0f * t) + 0.15f * cw_sin(CW_TAU * 659.0f * t + 1.0f);
    if ((i % 12000) < 600) x += 0.4f * (cw_rng_float(&r) * 2 - 1) * (1.0f - (float)(i % 12000) / 600.0f); /* hits */
    stem[2 * i] = x;
    stem[2 * i + 1] = 0.8f * x + 0.1f * cw_sin(CW_TAU * 220.0f * t);
  }
}
typedef struct { int frames; float tempo; } step_t;
static const step_t steps[] = { { 24000, 1.25f }, { 19200, 0.8f }, { 14400, 2.0f }, { 14400, 0.5f }, { 14400, 1.0f }, { 24000, 0 } };

static int render_steps(cw_voice *v, int nv, bool split, int64_t clocks[][8]) {
  int f = 0;
  for (size_t k = 0; k < CW_ARRAY_LEN(steps); k++) {
    int left = steps[k].frames;
    if (steps[k].tempo > 0) {
      if (!split) cw_voices_set_tempo(v, nv, steps[k].tempo);
      else
        for (int i = 0; i < nv; i++) { /* the control: each stem a block after the one before */
          cw_voice_set_tempo(v[i], steps[k].tempo);
          if (i < nv - 1) {
            cw_audio_render(out + 2 * f, 256);
            f += 256;
            left -= 256;
          }
        }
    }
    cw_audio_render(out + 2 * f, left);
    f += left;
    for (int i = 0; i < nv; i++) clocks[k][i] = cw_voice_frames_played(v[i]);
  }
  return f;
}

static void test_lock(bool split) {
  const int len = R * 4;
  make_stem(len);
  static float ref[MAXF * 2];
  int64_t rclk[CW_ARRAY_LEN(steps)][8], clk[CW_ARRAY_LEN(steps)][8];
  /* the reference: one voice at full volume */
  start();
  cw_sound s = from_wav(stem, len, 2, false);
  cw_voice one = cw_audio_play(s, &(cw_play_desc){ .start_frame = 1000 });
  int n = render_steps(&one, 1, false, rclk);
  memcpy(ref, out, (size_t)n * 2 * sizeof(float));
  stop();
  /* five stems at a fifth: three samples, two streams, started on the same frame */
  start();
  cw_sound smp = from_wav(stem, len, 2, false), str[2] = { from_wav(stem, len, 2, true), from_wav(stem, len, 2, true) };
  cw_voice v[5];
  for (int i = 0; i < 5; i++)
    v[i] = cw_audio_play(i < 3 ? smp : str[i - 3], &(cw_play_desc){ .start_frame = 1000, .volume = 0.2f });
  int n5 = render_steps(v, 5, split, clk);
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  stop();
  bool ok = n5 == n && st.underruns == 0;
  float md = 0;
  for (int i = 0; i < n * 2; i++) md = CW_MAX(md, fabsf(out[i] - ref[i]));
  for (size_t k = 0; k < CW_ARRAY_LEN(steps); k++)
    for (int i = 0; i < 5; i++) {
      ok &= clk[k][i] == rclk[k][0];
      FOLD(clk[k][i]);
      clock_hash = cw_hash32(&clk[k][i], sizeof clk[k][i], clock_hash);
    }
  lock_diff = md;
  ok &= md < 1e-4f;
  out_hash = cw_hash32(ref, (size_t)n * 2 * sizeof(float), out_hash);
  cw_info("tempo lock: largest difference from one voice %.3g; clocks %lld %lld %lld (one voice %lld)", md,
          (long long)clk[3][0], (long long)clk[3][3], (long long)clk[3][4], (long long)rclk[3][0]);
  lock_ok = ok;
}

/* ── the clock is the source position heard ──────────────────────────── */
#define CLICK_EVERY 4800
static float clicks[R * 8 * 2];
static void make_clicks(int frames) {
  memset(clicks, 0, (size_t)frames * 2 * sizeof(float));
  for (int c = CLICK_EVERY; c + 96 < frames; c += CLICK_EVERY)
    for (int i = 0; i < 96; i++) {
      float w = 0.5f - 0.5f * cw_cos(CW_TAU * (float)i / 96.0f);
      clicks[2 * (c + i)] = clicks[2 * (c + i) + 1] = 0.8f * w * cw_sin(CW_TAU * 2000.0f * (float)i / R);
    }
}
/* Plays the clicks with a tempo schedule, a block at a time, and checks each click heard against the clock read
 * around it. Returns the largest error in source frames. */
static float clock_run(bool stream, float t0, int64_t offset, const float *sched, int nsched, bool use_output) {
  const int len = R * 8;
  start();
  cw_sound s = from_wav(clicks, len, 2, stream);
  cw_voice v = cw_audio_play(s, &(cw_play_desc){ .tempo = t0, .offset = offset });
  static int64_t clk[MAXF / 256 + 2];
  int nb = 0, f = 0, total = R * 6, seg = total / (nsched + 1) / 256 * 256;
  clk[nb++] = use_output ? 0 : offset;
  while (f < total) {
    if (f > 0 && f % seg == 0 && f / seg <= nsched) cw_voice_set_tempo(v, sched[f / seg - 1]);
    cw_audio_render(out + 2 * f, 256);
    f += 256;
    clk[nb++] = use_output ? offset + f : cw_voice_frames_played(v);
    clock_hash = cw_hash32(&clk[nb - 1], sizeof clk[nb - 1], clock_hash);
  }
  stop();
  /* clicks heard: energy peaks over 96 frames */
  float worst = 0;
  int heard = 0;
  for (int i = 96; i + 96 < f;) {
    float e = 0;
    for (int j = 0; j < 96; j++) e += out[2 * (i + j)] * out[2 * (i + j)];
    if (e < 2.0f) {
      i += 16;
      continue;
    }
    int best = i;
    float be = e;
    for (int j = i; j < i + 600 && j + 96 < f; j++) { /* the peak of this click */
      float ej = 0;
      for (int q = 0; q < 96; q++) ej += out[2 * (j + q)] * out[2 * (j + q)];
      if (ej > be) be = ej, best = j;
    }
    /* where it is heard: the centre of its energy (a stretched click spreads a little either side) */
    double sw = 0, sx = 0;
    for (int j = CW_MAX(0, best - 1200); j < CW_MIN(f, best + 1296); j++) {
      double e2 = (double)out[2 * j] * out[2 * j];
      sw += e2;
      sx += e2 * j;
    }
    double c = sx / sw;
    int b = (int)c / 256;
    double at = (double)clk[b] + (double)(clk[b + 1] - clk[b]) * (c - b * 256) / 256.0; /* the clock there */
    at -= 48; /* the click's own centre is 48 frames in */
    double nearest = floor(at / CLICK_EVERY + 0.5) * CLICK_EVERY;
    float err = (float)fabs(at - nearest);
    if (nearest >= CLICK_EVERY) {
      worst = CW_MAX(worst, err);
      heard++;
    }
    i = best + 2400;
  }
  cw_info("tempo clock: %s from %.2f: %d clicks heard, worst %.1f frames off the clock", stream ? "stream" : "sample", t0,
          heard, worst);
  return heard >= 15 ? worst : 1e9f;
}
static void test_clock(bool use_output) {
  make_clicks(R * 8);
  const float a[] = { 0.8f, 1.5f }, b[] = { 1.25f, 1.0f, 0.75f };
  float w = 0;
  w = CW_MAX(w, clock_run(true, 1.25f, 0, a, 2, use_output)); /* a pre-fed start, then changes while stretching */
  w = CW_MAX(w, clock_run(false, 0, 0, b, 3, use_output));     /* from the bypass, back to it, in again */
  w = CW_MAX(w, clock_run(true, 0.8f, CLICK_EVERY * 3 + 1234, a, 2, use_output)); /* a pre-fed start from an offset */
  clock_err = w;
  clock_ok = w <= 128.0f;
}

/* ── tempo 1 is the bypass, bit for bit ──────────────────────────────── */
static uint32_t bypass_run(int how, bool force) {
  start();
  make_stem(R * 2);
  cw_sound smp = from_wav(stem, R * 2, 2, false), str = from_wav(stem, R * 2, 2, true);
  cw_play_desc d = { .pitch = 0.9f, .loop = true };
  if (how == 1) d.tempo = 1.0f;
  cw_voice a = cw_audio_play(smp, &d), b = cw_audio_play(str, &(cw_play_desc){ .pan = 0.3f });
  cw_voice c = cw_audio_play(smp, &(cw_play_desc){ .spatial = true, .space = { .position = { 3, 0, -2 }, .model = CW_ATTEN_INVERSE } });
  cw_audio_render(out, R / 2);
  if (force) cw__tempo_test = CW__TEMPO_TEST_FORCE;
  if (how == 2) {
    cw_voice_set_tempo(a, 1);
    cw_voices_set_tempo((cw_voice[]){ b, c }, 2, 1);
  }
  cw_audio_render(out + R, R * 2);
  cw__tempo_test = 0;
  uint32_t h = cw_audio_analyze(out, (size_t)(R / 2 + R * 2) * 2).hash;
  stop();
  return h;
}
static void test_bypass(bool force) {
  uint32_t h0 = bypass_run(0, false), h1 = bypass_run(1, false), h2 = bypass_run(2, force);
  FOLD(h0);
  bypass_ok = h0 == h1 && h1 == h2;
  cw_info("tempo bypass: %08x %08x %08x", h0, h1, h2);
}

/* ── no clicks across tempo changes ──────────────────────────────────── */
static void test_clicks(bool naive) {
  static float pcm[R * 2];
  for (int i = 0; i < R; i++) pcm[2 * i] = pcm[2 * i + 1] = 0.5f * cw_sin(CW_TAU * 440.0f * (float)i / R);
  start();
  cw_sound s = cw_sound_from_pcm(pcm, R, 2, R, NULL); /* a second of a whole number of cycles, looped */
  cw_voice v = cw_audio_play(s, &(cw_play_desc){ .loop = true });
  const float sched[] = { 1.25f, 0.8f, 2.0f, 0.5f, 1.0f, 1.1f, 1.0f, 0.9f, 1.0f };
  int f = 0;
  if (naive) cw__tempo_test = CW__TEMPO_TEST_NAIVE;
  for (size_t k = 0; k < CW_ARRAY_LEN(sched); k++) {
    cw_voice_set_tempo(v, sched[k]);
    cw_audio_render(out + 2 * f, R / 4);
    f += R / 4;
  }
  cw__tempo_test = 0;
  stop();
  float md = 0;
  for (int i = 1; i < f; i++) md = CW_MAX(md, fabsf(out[2 * i] - out[2 * i - 2]));
  click_delta = md;
  click_ok = md < 0.05f; /* the tone itself steps by up to 0.029 */
  cw_info("tempo clicks: largest step %.4f (the tone's own: %.4f)", md, 0.5f * CW_TAU * 440.0f / R);
}

/* ── cost ────────────────────────────────────────────────────────────── */
static void bench(void) {
  const int len = R * 4;
  make_stem(len);
  const float tempos[5] = { 0.5f, 0.8f, 1.25f, 2.0f, 1.0f };
  for (int k = 0; k < 5; k++) {
    start();
    cw_voice v[5];
    for (int i = 0; i < 5; i++) {
      cw_sound s = from_wav(stem, len, 2, true);
      v[i] = cw_audio_play(s, &(cw_play_desc){ .loop = true, .tempo = tempos[k], .offset = i * 4321 });
    }
    cw_audio_render(out, R / 2); /* started and settled */
    uint64_t t0 = cw_profile_now_ns();
    const int secs = 5;
    for (int b = 0; b < secs * 100; b++) cw_audio_render(out, R / 100);
    ms10[k] = (float)((double)(cw_profile_now_ns() - t0) / 1e6 / (secs * 100));
    (void)v;
    stop();
  }
  cw_info("tempo cost, five stereo streams (ms per 10 ms of sound): 0.5 %.3f, 0.8 %.3f, 1.25 %.3f, 2 %.3f, bypass %.3f",
          ms10[0], ms10[1], ms10[2], ms10[3], ms10[4]);
}

static void test_all(void) {
  test_pitch(cw_test_env("CW_TEST_TEMPO_AS_PITCH") != NULL);
  test_lock(cw_test_env("CW_TEST_TEMPO_SPLIT") != NULL);
  test_clock(cw_test_env("CW_TEST_TEMPO_CLOCK_OUTPUT") != NULL);
  test_bypass(cw_test_env("CW_TEST_TEMPO_FORCE") != NULL);
  test_clicks(cw_test_env("CW_TEST_TEMPO_NAIVE") != NULL);
  bench();
  CHECK(pitch_ok && lock_ok && clock_ok && bypass_ok && click_ok);
  done = 1;
}

static void test_fields(void) {
  cw_debug_i32("tempo.pitch_ok", &pitch_ok);
  cw_debug_i32("tempo.lock_ok", &lock_ok);
  cw_debug_i32("tempo.clock_ok", &clock_ok);
  cw_debug_i32("tempo.bypass_ok", &bypass_ok);
  cw_debug_i32("tempo.click_ok", &click_ok);
  cw_debug_i32("tempo.done", &done);
  cw_debug_u32("tempo.clock_hash", &clock_hash);
  cw_debug_u32("tempo.out_hash", &out_hash);
  cw_debug_f32("tempo.cents_tone_075", &cents[0]);
  cw_debug_f32("tempo.cents_tone_150", &cents[1]);
  cw_debug_f32("tempo.cents_chord_075", &cents[2]);
  cw_debug_f32("tempo.cents_chord_150", &cents[3]);
  cw_debug_f32("tempo.lock_diff", &lock_diff);
  cw_debug_f32("tempo.clock_err", &clock_err);
  cw_debug_f32("tempo.click_delta", &click_delta);
  cw_debug_f32("tempo.ms10_050", &ms10[0]);
  cw_debug_f32("tempo.ms10_080", &ms10[1]);
  cw_debug_f32("tempo.ms10_125", &ms10[2]);
  cw_debug_f32("tempo.ms10_200", &ms10[3]);
  cw_debug_f32("tempo.ms10_bypass", &ms10[4]);
}

void cw_test_late(void) { test_all(); }

CW_TEST_SUITE("audio_tempo", { "fields", test_fields })

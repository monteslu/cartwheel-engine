/* Audio module unit suite: every test runs the mixer OFFLINE (no device) and
 * checks exact behaviour sample by sample; rendered buffers are folded into
 * the checksum the gate compares across hosts. */
#include <math.h>
#include <string.h>

#include "cartwheel/audio.h"
#include "cartwheel/math.h"
#include "test.h"
#include "../../src/audio/internal.h"
#include "../../src/platform/internal.h"

#define R 48000
static float buf[R * 2 * 2]; /* 2 s stereo */

static void start(int max_voices) {
  cw_audio_init(&(cw_audio_desc){ .sample_rate = R, .offline = true, .max_voices = max_voices });
}
static void stop(void) {
  cw_audio_shutdown();
  cw_mem_stats ms;
  cw_mem_get_stats(&ms);
  CHECK_EQ_INT(ms.bytes[CW_MEM_AUDIO], 0); /* everything the module allocated came back */
}
static void render(int frames) {
  memset(buf, 0, sizeof buf);
  cw_audio_render(buf, frames);
  uint32_t h = cw_audio_analyze(buf, (size_t)frames * 2).hash;
  FOLD(h);
}
static bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* a constant (DC) mono sample: easy to measure gains on */
static cw_sound dc(float v, int frames, int rate) {
  static float pcm[R];
  for (int i = 0; i < frames; i++) pcm[i] = v;
  return cw_sound_from_pcm(pcm, frames, 1, rate, NULL);
}
static cw_sound sine(float hz, int frames, float amp) {
  static float pcm[R];
  for (int i = 0; i < frames; i++) pcm[i] = amp * cw_sin(CW_TAU * hz * (float)i / (float)R);
  return cw_sound_from_pcm(pcm, frames, 1, R, NULL);
}
static float rms_ch(int from, int to, int ch) {
  double s = 0;
  for (int i = from; i < to; i++) s += (double)buf[2 * i + ch] * buf[2 * i + ch];
  return (float)sqrt(s / (double)(to - from));
}

static void test_spsc(void) {
  cw__spsc q;
  cw__spsc_init(&q, sizeof(int), 8);
  int v, ok = 1;
  for (int round = 0; round < 5; round++) { /* wraps several times */
    for (int i = 0; i < 8; i++) ok &= cw__spsc_push(&q, &(int){ round * 100 + i });
    CHECK(!cw__spsc_push(&q, &(int){ -1 }));
    CHECK_EQ_INT(cw__spsc_free_slots(&q), 0);
    for (int i = 0; i < 8; i++) ok &= cw__spsc_pop(&q, &v) && v == round * 100 + i;
    CHECK(!cw__spsc_pop(&q, &v));
  }
  CHECK(ok);
  cw__spsc_free(&q);
}

/* start_frame lands on the exact sample; mono at center = equal-power 0.7071 */
static void test_schedule_and_pan(void) {
  start(16);
  cw_sound s = dc(1.0f, 1, R);
  cw_audio_play(s, &(cw_play_desc){ .start_frame = 1000 });
  render(2048);
  int first = -1;
  for (int i = 0; i < 2048; i++)
    if (buf[2 * i] != 0) { first = i; break; }
  CHECK_EQ_INT(first, 1000);
  float sn, cs;
  cw_sincos(CW_PI * 0.25f, &sn, &cs);
  CHECK(buf[2000] == cs && buf[2001] == sn);
  CHECK(near(buf[2000], 0.70710678f, 1e-6f));
  /* hard pan right */
  cw_audio_play(s, &(cw_play_desc){ .pan = 1, .start_frame = 2048 + 10 });
  render(256);
  CHECK(near(buf[2 * 10], 0, 1e-6f) && near(buf[2 * 10 + 1], 1, 1e-6f));
  stop();
}

static void test_resample_loop_clock(void) {
  start(16);
  cw_sound s = dc(0.5f, 100, 24000); /* resampled at load to the mix rate */
  CHECK_EQ_INT(cw_sound_length_frames(s), 200);
  CHECK_EQ_INT(cw_sound_source_rate(s), R);
  cw_voice v = cw_audio_play(s, &(cw_play_desc){ .loop = true });
  render(1000);
  CHECK_EQ_INT(cw_voice_frames_played(v), 1000);
  CHECK(cw_voice_is_playing(v));
  cw_voice_set_pitch(v, 2.0f);
  render(500);
  CHECK_EQ_INT(cw_voice_frames_played(v), 2000); /* the clock counts every loop pass */
  cw_voice_set_paused(v, true);
  render(500);
  CHECK_EQ_INT(cw_voice_frames_played(v), 2000);
  CHECK(rms_ch(0, 500, 0) == 0);
  stop();
}

static void test_one_shot_end(void) {
  start(16);
  cw_sound s = dc(0.5f, 100, R);
  cw_voice v = cw_audio_play(s, NULL);
  render(256);
  CHECK(buf[2 * 99] != 0 && buf[2 * 100] == 0);
  CHECK(!cw_voice_is_playing(v));
  CHECK_EQ_INT(cw_voice_frames_played(v), -1);
  cw_voice_set_volume(v, 1); /* stale: a no-op */
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.voices, 0);
  stop();
}

static void test_limits_priority(void) {
  start(4);
  cw_sound s = dc(0.1f, R, R);
  cw_voice v[4];
  for (int i = 0; i < 4; i++) v[i] = cw_audio_play(s, &(cw_play_desc){ .priority = 1, .loop = true });
  CHECK(CW_HANDLE_IS_NULL(cw_audio_play(s, &(cw_play_desc){ .priority = 0 }))); /* lower priority: refused */
  cw_voice hi = cw_audio_play(s, &(cw_play_desc){ .priority = 2 });              /* higher: steals the oldest */
  CHECK(!CW_HANDLE_IS_NULL(hi) && !cw_voice_is_playing(v[0]) && cw_voice_is_playing(v[1]));
  render(256);
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.voices, 4);
  stop();
  /* per-sound limit: the sound's own oldest goes */
  start(8);
  cw_sound lim = cw_sound_from_pcm((float[]){ 0.2f, 0.2f, 0.2f, 0.2f }, 4, 1, R, &(cw_sound_desc){ .max_voices = 1 });
  cw_voice a = cw_audio_play(lim, &(cw_play_desc){ .loop = true });
  cw_voice b = cw_audio_play(lim, &(cw_play_desc){ .loop = true });
  CHECK(!cw_voice_is_playing(a) && cw_voice_is_playing(b));
  render(64);
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.voices, 1);
  /* freeing a sound with live voices: memory goes after the mixer lets go */
  cw_sound_free(lim);
  render(64);
  CHECK(!cw_voice_is_playing(b));
  CHECK(CW_HANDLE_IS_NULL(cw_audio_play(lim, NULL)));
  stop();
}

static void test_fades(void) {
  start(8);
  cw_sound s = dc(1.0f, R, R);
  cw_voice v = cw_audio_play(s, &(cw_play_desc){ .loop = true, .fade_in = 0.01f }); /* 480 frames */
  render(1024);
  CHECK(buf[0] < 0.01f && near(buf[2 * 600], 0.70710678f, 1e-5f));
  CHECK(buf[2 * 100] < buf[2 * 200] && buf[2 * 200] < buf[2 * 300]);
  cw_voice_stop(v, 0.01f);
  render(1024);
  CHECK(buf[2 * 1000] == 0 && buf[2 * 10] > 0.5f);
  CHECK(!cw_voice_is_playing(v));
  stop();
}

static void test_buses(void) {
  start(8);
  cw_bus a = cw_bus_create("a", CW_AUDIO_MASTER), b = cw_bus_create("b", CW_AUDIO_MASTER);
  cw_bus a2 = cw_bus_create("a2", a);
  CHECK(cw_bus_find("a2") == a2 && cw_bus_find("master") == 0 && cw_bus_find("nope") == -1);
  CHECK_EQ_INT(cw_bus_create("bad", 99), -1);
  cw_sound s = dc(0.25f, R, R);
  cw_audio_play(s, &(cw_play_desc){ .loop = true, .bus = a2 });
  render(512);
  float base = buf[2 * 400];
  CHECK(near(base, 0.25f * 0.70710678f, 1e-6f));
  cw_bus_set_volume(a, 0.5f); /* ramps over one block, then holds */
  render(512);
  CHECK(near(buf[2 * 400], base * 0.5f, 1e-6f));
  cw_bus_set_solo(b, true); /* a2 is not on b's path: silent */
  render(512);
  CHECK(buf[2 * 400] == 0);
  cw_bus_set_solo(b, false);
  cw_bus_set_solo(a2, true); /* soloing a2 keeps its ancestors audible */
  render(512);
  CHECK(near(buf[2 * 400], base * 0.5f, 1e-6f));
  cw_bus_set_solo(a2, false);
  cw_bus_set_mute(a, true); /* muting a parent silences the child */
  render(512);
  CHECK(buf[2 * 400] == 0);
  cw_bus_set_mute(a, false);
  cw_bus_set_volume(a, 1);
  cw_bus_set_send(a2, 0, b, 1.0f); /* a2 -> a -> master and a2 -> b -> master: double */
  render(512);
  render(512);
  CHECK(near(buf[2 * 400], base * 2, 1e-6f));
  cw_bus_set_send(a2, 0, b, 0);
  cw_bus_set_send(a, 1, a2, 1.0f); /* a2 is newer than a: refused (it could close a cycle) */
  render(512);
  render(512);
  CHECK(near(buf[2 * 400], base, 1e-6f));
  stop();
}

static void test_3d(void) {
  start(8);
  cw_sound s = dc(0.5f, R, R);
  cw_audio_set_listener((t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0 }, (t3_vec3){ 0, 0, -1 }, (t3_vec3){ 0, 1, 0 });
  struct { cw_atten_model m; float d, maxd, expect; } cases[] = {
    { CW_ATTEN_INVERSE, 4, 100, 0.25f },     /* 1 / (1 + 3) */
    { CW_ATTEN_LINEAR, 5, 9, 0.5f },         /* 1 - 4/8 */
    { CW_ATTEN_EXPONENTIAL, 5, 100, 0.2f },  /* 5^-1 */
    { CW_ATTEN_INVERSE, 500, 100, 0.01f },   /* clamped at max distance */
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(cases); i++) {
    cw_voice v = cw_audio_play(s, &(cw_play_desc){
        .loop = true, .spatial = true,
        .space = { .position = { 0, 0, -cases[i].d }, .model = cases[i].m, .ref_distance = 1, .max_distance = cases[i].maxd } });
    render(256);
    float g = buf[2 * 100] / (0.5f * 0.70710678f);
    if (!near(g, cases[i].expect, 1e-4f)) cw_error("3d case %d: gain %f expected %f", (int)i, g, cases[i].expect);
    CHECK(near(g, cases[i].expect, 1e-4f));
    CHECK(near(buf[2 * 100], buf[2 * 100 + 1], 1e-6f)); /* straight ahead: centred */
    cw_voice_stop(v, 0);
  }
  /* to the right of the listener: right channel only */
  cw_voice v = cw_audio_play(s, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 3, 0, 0 } } });
  render(256);
  CHECK(near(buf[2 * 100], 0, 1e-6f) && near(buf[2 * 100 + 1], 0.5f, 1e-6f));
  /* cone pointing away from the listener: outer gain */
  cw_voice_set_3d(v, &(cw_voice_3d){ .position = { 0, 0, -2 }, .direction = { 0, 0, -1 }, .cone_inner_deg = 60,
                                     .cone_outer_deg = 120, .cone_outer_gain = 0.25f });
  render(256);
  render(256);
  CHECK(near(buf[2 * 100] / (0.5f * 0.70710678f), 0.25f, 1e-5f));
  cw_voice_stop(v, 0);
  /* doppler: approaching at a tenth of the speed of sound -> pitch c/(c - v) */
  cw_voice d = cw_audio_play(s, &(cw_play_desc){
      .loop = true, .spatial = true, .space = { .position = { 0, 0, -10 }, .velocity = { 0, 0, 34.3f }, .doppler = 1 } });
  render(R);
  float expect = 343.0f / (343.0f - 34.3f) * (float)R;
  int64_t got = cw_voice_frames_played(d);
  if (fabsf((float)got - expect) > 2) cw_error("doppler: %lld frames, expected %f", (long long)got, expect);
  CHECK(fabsf((float)got - expect) <= 2);
  cw_voice_stop(d, 0);
  /* occlusion low-passes: a 6 kHz sine loses most of its level */
  cw_sound hi = sine(6000, R, 0.5f);
  cw_voice o = cw_audio_play(hi, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 0, 0, -1 } } });
  render(4096);
  float clear = rms_ch(1024, 4096, 0);
  cw_voice_set_occlusion(o, 1);
  render(4096);
  float blocked = rms_ch(1024, 4096, 0);
  CHECK(clear > 0.2f && blocked < clear * 0.1f);
  stop();
}

/* a deterministic white noise sample */
static cw_sound noise(int frames, float amp) {
  static float pcm[R];
  uint32_t x = 12345;
  for (int i = 0; i < frames; i++) {
    x = x * 1664525u + 1013904223u;
    pcm[i] = amp * ((float)(x >> 8) * (1.0f / 8388608.0f) - 1.0f);
  }
  return cw_sound_from_pcm(pcm, frames, 1, R, NULL);
}
/* energy above ~6 kHz of channel ch (first difference: a crude high-pass) */
static float hf_energy(int from, int to, int ch) {
  double e = 0;
  for (int i = from + 1; i < to; i++) {
    double d = (double)buf[2 * i + ch] - buf[2 * (i - 1) + ch];
    e += d * d;
  }
  return (float)e;
}

static void test_hrtf(void) {
  /* the built-in table */
  size_t len;
  const void *data = cw__hrtf_builtin(&len);
  cw__hrtf *h = cw__hrtf_parse(data, len, R);
  CHECK(h && h->count == 711 && h->ntris == 1418);
  if (!h) return;
  /* every probe direction lands in a triangle with weights >= 0 summing to 1 */
  int bad = 0;
  for (int i = 0; i < 2000; i++) {
    float sn, cs, se, ce;
    cw_sincos((float)i * 2.39996f, &sn, &cs); /* golden-angle spiral over the sphere */
    float y = 1.0f - 2.0f * ((float)i + 0.5f) / 2000.0f, r = sqrtf(1.0f - y * y);
    (void)se, (void)ce;
    float w[3];
    int t = cw__hrtf_find(h, (t3_vec3){ r * cs, y, r * sn }, -1, w);
    float lo = w[0] < w[1] ? (w[0] < w[2] ? w[0] : w[2]) : (w[1] < w[2] ? w[1] : w[2]);
    if (t < 0 || lo < -1e-4f) bad++;
  }
  CHECK_EQ_INT(bad, 0);
  /* the SIMD FIR gives the plain C loop's bits */
  static float u[CW__HRTF_TAPS - 1 + 253], ya[253], yb[253];
  uint32_t x = 7;
  for (size_t i = 0; i < CW_ARRAY_LEN(u); i++) { x = x * 1664525u + 1013904223u; u[i] = (float)(int32_t)x * (1.0f / 2147483648.0f); }
  const float *hr = h->taps + 100 * 2 * CW__HRTF_TAPS;
  cw__hrtf_fir(hr, u, 253, ya);
  cw__hrtf_fir_scalar(hr, u, 253, yb);
  CHECK(!memcmp(ya, yb, sizeof ya));
  FOLD(ya);
  cw__hrtf_free(h);

  start(8);
  cw_audio_set_listener((t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0 }, (t3_vec3){ 0, 0, -1 }, (t3_vec3){ 0, 1, 0 });
  CHECK(cw_audio_get_output() == CW_AUDIO_SPEAKERS);
  cw_audio_set_output(CW_AUDIO_HEADPHONES);
  CHECK(cw_audio_get_output() == CW_AUDIO_HEADPHONES);
  cw_sound n = noise(R, 0.5f);
  /* straight ahead: both ears the same (the KEMAR set is symmetric at 0 degrees) */
  cw_voice v = cw_audio_play(n, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 0, 0, -2 } } });
  render(4096);
  float maxd = 0, peak = 0;
  for (int i = 0; i < 4096; i++) {
    maxd = fmaxf(maxd, fabsf(buf[2 * i] - buf[2 * i + 1]));
    peak = fmaxf(peak, fabsf(buf[2 * i]));
  }
  if (maxd > 1e-6f) cw_error("hrtf front: L-R %g", (double)maxd);
  CHECK(peak > 0.05f && maxd <= 1e-6f);
  /* 90 degrees left: an impulse reaches the left ear first, by the dataset's ITD */
  cw_voice_stop(v, 0);
  cw_sound imp = dc(1.0f, 1, R);
  cw_audio_play(imp, &(cw_play_desc){ .spatial = true, .start_frame = cw_audio_mixed_frames() + 512 + 100,
                                     .space = { .position = { -2, 0, 0 } } });
  render(2048);
  int on[2] = { -1, -1 };
  float pk[2] = { 0, 0 };
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < 2048; i++) pk[c] = fmaxf(pk[c], fabsf(buf[2 * i + c]));
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < 2048 && on[c] < 0; i++)
      if (fabsf(buf[2 * i + c]) >= 0.2f * pk[c]) on[c] = i;
  /* KEMAR at 270 degrees: an interaural delay of about 28 frames at 48 kHz
   * (0.58 ms), plus a few frames of the far ear's duller (slower) filter; the
   * left ear is far louder */
  cw_info("hrtf left: onsets L %d R %d, peaks %g %g", on[0], on[1], (double)pk[0], (double)pk[1]);
  CHECK(on[0] >= 0 && on[1] - on[0] >= 26 && on[1] - on[0] <= 36);
  CHECK(pk[0] > 4 * pk[1]);
  /* front vs behind: the pinna dulls sound from behind (less high-frequency energy) */
  v = cw_audio_play(n, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 0, 0, -2 } } });
  render(8192);
  float front = hf_energy(1024, 8192, 0) + hf_energy(1024, 8192, 1);
  cw_voice_set_position(v, (t3_vec3){ 0, 0, 2 }, (t3_vec3){ 0 });
  render(8192);
  float back = hf_energy(1024, 8192, 0) + hf_energy(1024, 8192, 1);
  cw_info("hrtf hf energy: front %g back %g", (double)front, (double)back);
  CHECK(back < front * 0.8f);
  /* a source circling fast stays smooth (the filters crossfade): no step
   * bigger than the noise's own */
  cw_sound tone = sine(440, R, 0.5f);
  cw_voice_stop(v, 0);
  v = cw_audio_play(tone, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 0, 0, -2 } } });
  float maxstep = 0;
  for (int b = 0; b < 40; b++) {
    float sn, cs;
    cw_sincos((float)b * 0.6f, &sn, &cs);
    cw_voice_set_position(v, (t3_vec3){ 2 * sn, 0, -2 * cs }, (t3_vec3){ 0 });
    render(256);
    for (int i = 1; i < 256; i++)
      for (int c = 0; c < 2; c++) maxstep = fmaxf(maxstep, fabsf(buf[2 * i + c] - buf[2 * (i - 1) + c]));
  }
  cw_info("hrtf orbit: max step %g", (double)maxstep);
  CHECK(maxstep < 0.12f); /* 440 Hz at 0.5 moves at most 0.029 per frame; filters raise it a few times */
  cw_voice_stop(v, 0);
  /* the voice limit: with 1, only the nearest of three is filtered */
  cw_audio_set_hrtf_voice_limit(1);
  cw_voice far1 = cw_audio_play(n, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 5, 0, 0 } } });
  cw_voice near1 = cw_audio_play(n, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 0, 1, -1 } } });
  cw_voice far2 = cw_audio_play(n, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 0, 0, 9 } } });
  render(512);
  cw__mixer *m = &cw__au.mix;
  int on_count = 0, on_slot = -1;
  for (int s = 0; s < m->max_voices; s++)
    if (m->hrtf_on[s]) on_count++, on_slot = s;
  CHECK_EQ_INT(on_count, 1);
  CHECK(on_slot >= 0 && m->voices[on_slot].sp.position.y == 1.0f);
  /* speakers: nobody is filtered; the mix is the plain pan again */
  cw_audio_set_output(CW_AUDIO_SPEAKERS);
  render(512);
  render(512);
  on_count = 0;
  for (int s = 0; s < m->max_voices; s++)
    on_count += m->voices[s].state == CW__V_PLAYING && (m->hrtf_on[s] || m->hstate[s].mix > 0);
  CHECK_EQ_INT(on_count, 0);
  cw_voice_stop(far1, 0);
  cw_voice_stop(near1, 0);
  cw_voice_stop(far2, 0);
  /* air absorption dulls a far source */
  v = cw_audio_play(n, &(cw_play_desc){ .loop = true, .spatial = true, .space = { .position = { 0, 0, -40 }, .air_absorption = 0.1f } });
  render(8192);
  float dull = hf_energy(1024, 8192, 0) / (rms_ch(1024, 8192, 0) * rms_ch(1024, 8192, 0) + 1e-20f);
  cw_voice_set_3d(v, &(cw_voice_3d){ .position = { 0, 0, -40 } });
  render(8192);
  float bright = hf_energy(1024, 8192, 0) / (rms_ch(1024, 8192, 0) * rms_ch(1024, 8192, 0) + 1e-20f);
  cw_info("air absorption: hf/energy %g vs %g", (double)dull, (double)bright);
  CHECK(dull < bright * 0.5f);
  stop();
}

static void test_fx(void) {
  start(8);
  cw_bus b = cw_bus_create("fx", CW_AUDIO_MASTER);
  cw_sound hi = sine(10000, R, 0.5f), lo = sine(100, R, 0.5f);
  cw_audio_play(hi, &(cw_play_desc){ .loop = true, .bus = b, .pan = -1 });
  render(4096);
  float clear = rms_ch(2048, 4096, 0);
  int slot = cw_bus_add_fx(b, &(cw_fx_desc){ .type = CW_FX_LOWPASS, .filter = { 1000, 0.707f } });
  CHECK_EQ_INT(slot, 0);
  render(4096);
  float lp = rms_ch(2048, 4096, 0);
  CHECK(clear > 0.3f && lp < clear * 0.05f);
  cw_bus_set_fx_bypass(b, 0, true);
  render(4096);
  CHECK(near(rms_ch(2048, 4096, 0), clear, 1e-3f));
  cw_bus_remove_fx(b, 0);
  render(256);
  stop();

  start(8);
  b = cw_bus_create("fx", CW_AUDIO_MASTER);
  lo = sine(100, R, 0.5f);
  cw_audio_play(lo, &(cw_play_desc){ .loop = true, .bus = b, .pan = -1 });
  cw_bus_add_fx(b, &(cw_fx_desc){ .type = CW_FX_HIGHPASS, .filter = { 2000, 0.707f } });
  render(R / 2);
  CHECK(rms_ch(R / 4, R / 2, 0) < 0.5f * 0.7071f * 0.02f);
  stop();

  /* limiter: a full-scale x2 signal never passes the ceiling */
  start(8);
  cw_sound loud = sine(440, R, 2.0f);
  cw_audio_play(loud, &(cw_play_desc){ .loop = true, .pan = -1 });
  cw_bus_add_fx(CW_AUDIO_MASTER, &(cw_fx_desc){ .type = CW_FX_LIMITER, .limiter = { -3.0f, 0.05f } });
  render(R / 2);
  float pk = 0;
  for (int i = 0; i < R; i++) pk = CW_MAX(pk, fabsf(buf[i]));
  CHECK(pk <= cw_pow(10.0f, -3.0f / 20.0f) + 1e-6f && pk > 0.5f);
  stop();

  /* delay: an impulse comes back after time_s at wet * level, then decays by feedback */
  start(8);
  b = cw_bus_create("echo", CW_AUDIO_MASTER);
  cw_bus_add_fx(b, &(cw_fx_desc){ .type = CW_FX_DELAY, .delay = { 0.01f, 0.5f, 0.5f, 1.0f } }); /* 480 frames */
  cw_audio_play(cw_sound_from_pcm((float[]){ 1.0f }, 1, 1, R, NULL), &(cw_play_desc){ .bus = b, .pan = -1 });
  render(2048);
  CHECK(buf[0] == 1.0f && buf[2 * 480] == 0.5f && buf[2 * 960] == 0.25f && buf[2 * 100] == 0);
  stop();

  /* reverb: an impulse leaves a tail; compressor: a loud steady tone comes down */
  start(8);
  b = cw_bus_create("verb", CW_AUDIO_MASTER);
  cw_bus_add_fx(b, &(cw_fx_desc){ .type = CW_FX_REVERB, .reverb = { 0.8f, 0.3f, 1.0f, 0.0f, 1.0f } });
  cw_audio_play(cw_sound_from_pcm((float[]){ 1.0f }, 1, 1, R, NULL), &(cw_play_desc){ .bus = b });
  render(R);
  CHECK(rms_ch(R / 4, R / 2, 0) > 1e-4f && rms_ch(R / 4, R / 2, 1) > 1e-4f);
  CHECK(buf[0] == 0); /* fully wet: nothing before the first comb returns */
  stop();
  start(8);
  b = cw_bus_create("comp", CW_AUDIO_MASTER);
  cw_audio_play(sine(440, R, 0.9f), &(cw_play_desc){ .loop = true, .bus = b, .pan = -1 });
  render(R / 4);
  float before = rms_ch(R / 8, R / 4, 0);
  cw_bus_add_fx(b, &(cw_fx_desc){ .type = CW_FX_COMPRESSOR, .compressor = { -20, 8, 0.002f, 0.1f, 0 } });
  render(R / 2);
  CHECK(rms_ch(R / 4, R / 2, 0) < before * 0.3f);
  stop();
}

/* A WAV built in memory, loaded as a SAMPLE and as a STREAM: the mixer must
 * produce identical samples either way (the ring is transparent). */
static void *wav16(int frames, int ch, int rate, size_t *len) {
  static uint8_t w[44 + R * 4];
  int n = frames * ch;
  memcpy(w, "RIFF", 4);
  uint32_t u32;
  uint16_t u16;
#define P32(o, v) (u32 = (uint32_t)(v), memcpy(w + (o), &u32, 4))
#define P16(o, v) (u16 = (uint16_t)(v), memcpy(w + (o), &u16, 2))
  P32(4, 36 + n * 2);
  memcpy(w + 8, "WAVEfmt ", 8);
  P32(16, 16); P16(20, 1); P16(22, ch); P32(24, rate); P32(28, rate * ch * 2); P16(32, ch * 2); P16(34, 16);
  memcpy(w + 36, "data", 4);
  P32(40, n * 2);
  for (int i = 0; i < n; i++) {
    int16_t s = (int16_t)(8000.0f * cw_sin((float)i * 0.0371f) + 3000.0f * cw_sin((float)i * 0.0013f));
    memcpy(w + 44 + i * 2, &s, 2);
  }
  *len = 44 + (size_t)n * 2;
  return w;
}

static void test_stream_equals_sample(void) {
  size_t len;
  void *w = wav16(10000, 2, R, &len);
  uint32_t hashes[2];
  int64_t clocks[2];
  for (int mode = 0; mode < 2; mode++) {
    start(8);
    cw_sound s = cw_sound_load_memory(w, len, &(cw_sound_desc){ .mode = mode ? CW_SOUND_STREAM : CW_SOUND_SAMPLE });
    CHECK(cw_sound_is_stream(s) == (mode == 1));
    CHECK_EQ_INT(cw_sound_length_frames(s), 10000);
    cw_voice v = cw_audio_play(s, &(cw_play_desc){ .loop = true, .pitch = 0.75f, .start_frame = 333 });
    cw_audio_render(buf, R); /* loops the 10000 frames several times at 0.75x */
    hashes[mode] = cw_audio_analyze(buf, R * 2).hash;
    clocks[mode] = cw_voice_frames_played(v);
    cw_audio_stats st;
    cw_audio_get_stats(&st);
    CHECK_EQ_INT(st.underruns, 0);
    stop();
  }
  CHECK(hashes[0] == hashes[1]);
  CHECK_EQ_INT(clocks[0], clocks[1]);
  CHECK_EQ_INT(clocks[0], (int64_t)(((uint64_t)((double)0.75f * 4294967296.0) * (uint64_t)(R - 333)) >> 32));
  FOLD(hashes[0]);
  /* a non-looping stream ends and frees its slot */
  start(8);
  cw_sound s = cw_sound_load_memory(w, len, &(cw_sound_desc){ .mode = CW_SOUND_STREAM });
  cw_voice v = cw_audio_play(s, NULL);
  render(12000);
  CHECK(!cw_voice_is_playing(v));
  CHECK(buf[2 * 9999] != 0 && buf[2 * 10000] == 0);
  stop();
}

static void test_queue_overflow(void) {
  cw_audio_init(&(cw_audio_desc){ .sample_rate = R, .offline = true, .command_capacity = 16 });
  for (int i = 0; i < 100; i++) cw_audio_set_speed_of_sound(300);
  cw_audio_update();
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.dropped_commands, 84);
  render(256); /* the 16 that fit are applied; nothing breaks */
  stop();
}

/* closing the platform device forgets the fill callback on
 * every host (native kept it), so nothing can call into a freed mixer */
static void dev_fill(void *user, float *out, int frames) {
  (void)user;
  memset(out, 0, (size_t)frames * 2 * sizeof(float));
}
static void test_device_close(void) {
  static int tag;
  cw_audio_open(48000, dev_fill, &tag);
  CHECK(cw__plat.audio_fill == dev_fill);
  cw_audio_close();
  CHECK(cw__plat.audio_fill == NULL && cw__plat.audio_user == NULL);
}

CW_TEST_SUITE("audio", { "spsc", test_spsc }, { "schedule_and_pan", test_schedule_and_pan },
              { "resample_loop_clock", test_resample_loop_clock }, { "one_shot_end", test_one_shot_end },
              { "limits_priority", test_limits_priority }, { "fades", test_fades }, { "buses", test_buses },
              { "3d", test_3d }, { "hrtf", test_hrtf }, { "fx", test_fx }, { "stream_equals_sample", test_stream_equals_sample },
              { "queue_overflow", test_queue_overflow },
              { "device_close", test_device_close })

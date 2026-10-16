/* audio_3d: 3D sound for headphones, rendered offline one 1/60 s slice per
 * frame (6 seconds) so every host must reproduce it bit for bit.
 *
 *   - headphones mode: spatial voices are filtered with the HRTF (the
 *     built-in KEMAR table), so direction is heard above, behind and below,
 *     not only left and right;
 *   - a bell orbits the listener in the horizontal plane (front, right,
 *     behind, left), a hum orbits through the vertical plane (front,
 *     overhead, behind, below), a shaker sits behind, up and to the left;
 *   - 24 crickets scattered 6 to 20 units away with a distance low-pass and
 *     an HRTF voice limit of 8 (the nearest 8 are filtered, the rest pan);
 *   - seconds 3 to 4 switch to speakers (plain panning) and back: each voice
 *     crossfades over one block.
 *
 * When done it publishes a hash, peak and RMS of all 288000 stereo frames,
 * per-second hashes and how many voices had the HRTF. It also times the
 * mixer (not compared across hosts): microseconds per voice per 256-frame
 * block panned and with the HRTF. The picture is a top-down radar
 * (listener yellow in the middle, the near voices bright in headphones mode
 * and dim on speakers, the crickets blue) and the hash bits. CW_AUDIO_3D_WAV=path (native) writes the mix as a float WAV. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/audio.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"

#define RATE 48000
#define FRAMES_PER_TICK (RATE / 60)
#define TICKS 360
#define TOTAL (FRAMES_PER_TICK * TICKS)
#define CRICKETS 24

typedef struct {
  float *buf;
  int tick;
  cw_sound bell, hum, shaker, cricket;
  cw_voice v_bell, v_hum, v_shaker, v_cricket[CRICKETS];
  t3_vec3 p_bell, p_hum, p_shaker, p_cricket[CRICKETS];
  /* published */
  uint32_t hash, seg[6];
  float peak, rms;
  int32_t done, hrtf_max, hrtf_speakers, voices;
  float us_pan, us_hrtf, us_hrtf_moving; /* per voice per block (timing: never compared) */
} app3d;

static app3d app;

/* ── generated sounds ─────────────────────────────────────────────────── */

static uint32_t lcg(uint32_t *x) { return *x = *x * 1664525u + 1013904223u; }
static float white(uint32_t *x) { return (float)(lcg(x) >> 8) * (1.0f / 8388608.0f) - 1.0f; }

static cw_sound make(int frames, float (*fn)(int i, uint32_t *rng)) {
  float *pcm = cw_alloc((size_t)frames * sizeof(float), CW_MEM_GAME);
  uint32_t rng = 99;
  for (int i = 0; i < frames; i++) pcm[i] = fn(i, &rng);
  cw_sound s = cw_sound_from_pcm(pcm, frames, 1, RATE, NULL);
  cw_free(pcm);
  return s;
}
/* a bell ping every half second: 880 Hz + 2.76x partial, exponential decay */
static float bell_fn(int i, uint32_t *r) {
  (void)r;
  int k = i % (RATE / 2);
  float t = (float)k / RATE, e = cw_exp(-t * 9.0f);
  return 0.5f * e * (cw_sin(CW_TAU * 880.0f * t) + 0.4f * cw_sin(CW_TAU * 2428.8f * t));
}
/* a buzzy hum: 150 Hz with odd harmonics, a little noise */
static float hum_fn(int i, uint32_t *r) {
  float t = (float)i / RATE, s = 0;
  for (int h = 1; h <= 9; h += 2) s += cw_sin(CW_TAU * 150.0f * (float)h * t) / (float)h;
  return 0.3f * s + 0.03f * white(r);
}
/* shaker: noise bursts four times a second */
static float shaker_fn(int i, uint32_t *r) {
  int k = i % (RATE / 4);
  float t = (float)k / RATE;
  return 0.4f * white(r) * cw_exp(-t * 30.0f);
}
/* cricket: a 4.5 kHz chirp train */
static float cricket_fn(int i, uint32_t *r) {
  (void)r;
  int k = i % (RATE / 3);
  if (k > RATE / 12) return 0;
  float t = (float)i / RATE, g = (k / 480) % 2 ? 0.0f : 0.25f;
  return g * cw_sin(CW_TAU * 4500.0f * t);
}

/* ── scene ────────────────────────────────────────────────────────────── */

/* #region orbit */
static void place(app3d *a, int t) {
  float sn, cs;
  /* the bell: clockwise seen from above (front, right, behind, left), 4 s a turn */
  cw_sincos(CW_TAU * (float)t / 240.0f, &sn, &cs);
  a->p_bell = (t3_vec3){ 3 * sn, 0, -3 * cs };
  /* the hum: front, overhead, behind, below, 3 s a turn */
  cw_sincos(CW_TAU * (float)t / 180.0f, &sn, &cs);
  a->p_hum = (t3_vec3){ 0, 3 * sn, -3 * cs };
  a->p_shaker = (t3_vec3){ -2, 1.5f, 2 };
}
/* #endregion */

static void init(void *user) {
  app3d *a = user;
  a->buf = cw_calloc(TOTAL * 2, sizeof(float), CW_MEM_GAME);
  cw_audio_init(&(cw_audio_desc){ .sample_rate = RATE, .offline = true, .max_voices = 48 });
  a->bell = make(RATE / 2, bell_fn);
  a->hum = make(RATE, hum_fn);
  a->shaker = make(RATE / 4, shaker_fn);
  a->cricket = make(RATE, cricket_fn);

  /* #region setup */
  cw_audio_set_listener((t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0 }, (t3_vec3){ 0, 0, -1 }, (t3_vec3){ 0, 1, 0 });
  /* CW_AUDIO_3D_SPEAKERS=1 (the gate's control) stays on speakers: the mix must differ */
  bool speakers = getenv("CW_AUDIO_3D_SPEAKERS") != NULL;
  cw_audio_set_output(speakers ? CW_AUDIO_SPEAKERS : CW_AUDIO_HEADPHONES); /* headphones loads the built-in HRTF table */
  cw_audio_set_hrtf_voice_limit(8);
  place(a, 0);
  cw_voice_3d near = { .model = CW_ATTEN_INVERSE, .ref_distance = 1, .max_distance = 50 };
  near.position = a->p_bell;
  a->v_bell = cw_audio_play(a->bell, &(cw_play_desc){ .loop = true, .spatial = true, .space = near });
  near.position = a->p_hum;
  a->v_hum = cw_audio_play(a->hum, &(cw_play_desc){ .loop = true, .volume = 0.6f, .spatial = true, .space = near });
  near.position = a->p_shaker;
  a->v_shaker = cw_audio_play(a->shaker, &(cw_play_desc){ .loop = true, .spatial = true, .space = near });
  /* crickets: far, dull with distance (air absorption), most of them panned by the limit */
  uint32_t r = 7;
  for (int i = 0; i < CRICKETS; i++) {
    float sn, cs, d = 6.0f + 14.0f * (float)(lcg(&r) >> 8) / 16777216.0f;
    cw_sincos(CW_TAU * (float)i / CRICKETS + 0.3f, &sn, &cs);
    a->p_cricket[i] = (t3_vec3){ d * sn, -1.0f + (float)(i % 3), -d * cs };
    a->v_cricket[i] = cw_audio_play(a->cricket, &(cw_play_desc){
        .loop = true, .volume = 0.5f, .offset = (int64_t)(lcg(&r) % RATE), .spatial = true,
        .space = { .position = a->p_cricket[i], .model = CW_ATTEN_INVERSE, .ref_distance = 2, .max_distance = 40,
                   .air_absorption = 0.03f } });
  }
  /* #endregion */

  cw_debug_u32("sp.hash", &a->hash);
  cw_debug_f32("sp.peak", &a->peak);
  cw_debug_f32("sp.rms", &a->rms);
  cw_debug_field("sp.seg_hash", a->seg, CW_DBG_U32, 6);
  cw_debug_i32("sp.done", &a->done);
  cw_debug_i32("sp.hrtf_max", &a->hrtf_max);
  cw_debug_i32("sp.hrtf_speakers", &a->hrtf_speakers);
  cw_debug_i32("sp.voices", &a->voices);
  cw_debug_f32("sp.us_pan", &a->us_pan);
  cw_debug_f32("sp.us_hrtf", &a->us_hrtf);
  cw_debug_f32("sp.us_hrtf_moving", &a->us_hrtf_moving);
}

static void script(app3d *a, int t) {
  place(a, t);
  /* velocities feed doppler only; none is set here */
  cw_voice_set_position(a->v_bell, a->p_bell, (t3_vec3){ 0 });
  cw_voice_set_position(a->v_hum, a->p_hum, (t3_vec3){ 0 });
  /* #region switch */
  if (t == 180) cw_audio_set_output(CW_AUDIO_SPEAKERS);
  if (t == 240 && !getenv("CW_AUDIO_3D_SPEAKERS")) cw_audio_set_output(CW_AUDIO_HEADPHONES);
  /* #endregion */
}

static double now_s(void) { /* wall clock (cw_time is the game's clock: it stops in test runs) */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Microseconds per voice per block: n spatial voices for 0.5 s, panned and
 * with the HRTF (timing only; after the scene, on a fresh mixer). */
static void bench(app3d *a) {
  cw_audio_shutdown();
  cw_audio_init(&(cw_audio_desc){ .sample_rate = RATE, .offline = true, .max_voices = 32 });
  cw_sound s = make(RATE, hum_fn);
  cw_voice v[32];
  for (int i = 0; i < 32; i++) {
    float sn, cs;
    cw_sincos((float)i, &sn, &cs);
    v[i] = cw_audio_play(s, &(cw_play_desc){ .loop = true, .volume = 0.05f, .spatial = true,
                                     .space = { .position = { 3 * sn, (float)(i % 5) - 2, 3 * cs } } });
  }
  static float out[RATE];
  cw_audio_set_hrtf_voice_limit(32);
  cw_audio_render(out, 2048); /* warm up */
  const int blocks = 94;   /* ~0.5 s */
  /* pass 0 panned, 1 HRTF, 2 HRTF with every voice moving every block (filters crossfade) */
  for (int pass = 0; pass < 3; pass++) {
    cw_audio_set_output(pass ? CW_AUDIO_HEADPHONES : CW_AUDIO_SPEAKERS);
    cw_audio_render(out, 512);
    double t0 = now_s();
    for (int b = 0; b < blocks; b++) {
      if (pass == 2)
        for (int i = 0; i < 32; i++) {
          float sn, cs;
          cw_sincos((float)i + (float)b * 0.05f, &sn, &cs);
          cw_voice_set_position(v[i], (t3_vec3){ 3 * sn, (float)(i % 5) - 2, 3 * cs }, (t3_vec3){ 0 });
        }
      cw_audio_render(out, 256);
    }
    float us = (float)((now_s() - t0) * 1e6 / (double)blocks / 32.0);
    if (pass == 2) a->us_hrtf_moving = us;
    else if (pass) a->us_hrtf = us;
    else a->us_pan = us;
  }
  cw_info("audio_3d: %.2f us per voice per 256-frame block panned, %.2f with the HRTF, %.2f moving", (double)a->us_pan,
          (double)a->us_hrtf, (double)a->us_hrtf_moving);
}

static void write_wav(const char *path, const float *buf, int frames) {
#if !CW_CART
  FILE *f = fopen(path, "wb");
  if (!f) return;
  uint32_t data = (uint32_t)frames * 8, rate = RATE, byte_rate = RATE * 8, riff = 36 + data, fmt_len = 16;
  uint16_t fmt = 3, ch = 2, align = 8, bits = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt_len, 4, 1, f);
  fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  fwrite(buf, 8, (size_t)frames, f);
  fclose(f);
#else
  (void)path; (void)buf; (void)frames;
#endif
}

static void finish(app3d *a) {
  cw_audio_analysis r = cw_audio_analyze(a->buf, TOTAL * 2);
  a->hash = r.hash;
  a->peak = r.peak;
  a->rms = r.rms;
  for (int s = 0; s < 6; s++) a->seg[s] = cw_audio_analyze(a->buf + s * RATE * 2, RATE * 2).hash;
  if (getenv("CW_AUDIO_3D_WAV")) write_wav(getenv("CW_AUDIO_3D_WAV"), a->buf, TOTAL);
  cw_info("audio_3d: hash %08x peak %.4f rms %.5f, %d voices, HRTF on %d (speakers: %d)", a->hash, a->peak, a->rms,
          a->voices, a->hrtf_max, a->hrtf_speakers);
  bench(a);
  a->done = 1;
}

static void update(void *user, double dt) {
  (void)dt;
  app3d *a = user;
  if (a->tick >= TICKS) return;
  script(a, a->tick);
  cw_audio_render(a->buf + (size_t)a->tick * FRAMES_PER_TICK * 2, FRAMES_PER_TICK);
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  if (a->tick == 100) a->voices = st.voices;
  if (a->tick < 180 || a->tick >= 250) a->hrtf_max = CW_MAX(a->hrtf_max, st.hrtf_voices);
  if (a->tick == 200) a->hrtf_speakers = st.hrtf_voices;
  if (++a->tick == TICKS) finish(a);
}

static void rect(int x, int y, int w, int h, float r, float g, float b) {
  glScissor(x, y, w, h);
  glClearColor(r, g, b, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void dot(float sx, float sy, t3_vec3 p, float scale, float r, float g, float b) {
  int x = (int)((240.0f + p.x * scale) * sx), y = (int)((150.0f - p.z * scale) * sy); /* -z (forward) is up */
  rect(x - (int)(3 * sx), y - (int)(3 * sy), (int)(6 * sx), (int)(6 * sy), r, g, b);
}

static void render(void *user, double alpha) {
  (void)alpha;
  app3d *a = user;
  int w, h;
  cw_drawable_size(&w, &h);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, w, h);
  glClearColor(0.06f, 0.07f, 0.09f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  float sx = (float)w / 480.0f, sy = (float)h / 270.0f;
  bool hp = a->tick < 180 || a->tick >= 240;
  float c = hp ? 1.0f : 0.4f;
  dot(sx, sy, (t3_vec3){ 0, 0, 0 }, 1, 1.0f, 0.8f, 0.2f); /* the listener */
  dot(sx, sy, a->p_bell, 20, c, c, 1.0f);
  dot(sx, sy, a->p_hum, 20, c, 1.0f, c);
  dot(sx, sy, a->p_shaker, 20, 1.0f, c, c);
  for (int i = 0; i < CRICKETS; i++) dot(sx, sy, a->p_cricket[i], 5, 0.3f, 0.5f, 0.9f);
  for (int i = 0; i < 32; i++) {
    float v = (a->hash >> i) & 1 ? 1.0f : 0.2f;
    rect((int)((8 + i * 14) * sx), (int)(8 * sy), (int)(12 * sx), (int)(16 * sy), v, a->done ? v : 0.2f, v);
  }
  /* progress */
  rect(0, (int)(264 * sy), (int)((float)a->tick / TICKS * 480.0f * sx), (int)(6 * sy), 0.3f, 0.7f, 1.0f);
  glDisable(GL_SCISSOR_TEST);
}

static void shutdown(void *user) {
  app3d *a = user;
  cw_audio_shutdown();
  cw_free(a->buf);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel 3D audio", .width = 480, .height = 270, .debug = true,
                        .init = init, .update = update, .render = render, .shutdown = shutdown, .user = &app };
}

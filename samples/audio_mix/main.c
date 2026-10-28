/* audio_mix: the audio module's cross-host gate. A scripted scene is
 * rendered OFFLINE (no device) one 1/60 s slice per frame, 4 seconds in all:
 *
 *   - a looping Ogg Vorbis music stream (44.1 kHz: stream resampling) on a
 *     "music" bus whose low-pass sweeps down, then back up;
 *   - an Ogg Opus ambience stream that fades in, then fades out and stops;
 *   - a tracker module (MOD, libxmp) on its own bus, soloed for half a second;
 *   - one-shots: WAV kick (48 kHz), WAV hat (44.1 kHz: load-time resampling)
 *     through a high-pass, FLAC blip scheduled on an exact mixer frame, MP3
 *     zap at three pitches, a voice-limited kick burst that steals;
 *   - a looping engine WAV as a 3D voice flying past the listener (inverse
 *     distance, doppler, a cone) with occlusion switched on halfway;
 *   - an "sfx" bus with a delay and a send into a "verb" bus (Freeverb);
 *     a compressor on the music bus and a limiter on the master.
 *
 * When done (frame 240) it publishes a hash, peak and RMS of all 192000
 * stereo frames, per-second hashes, every decoder's own hash, and the music
 * clock checked against the exact 32.32 arithmetic. The picture shows the
 * waveform and the hash bits, so even a frame-only host sees a mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/audio.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

#define RATE 48000
#define FRAMES_PER_TICK (RATE / 60)
#define TICKS 240
#define TOTAL (FRAMES_PER_TICK * TICKS)

typedef struct {
  float *buf;
  int tick;
  cw_sound kick, hat, blip, zap, engine, music, ambience, tune;
  cw_voice v_music, v_amb, v_tune, v_engine;
  cw_bus b_music, b_sfx, b_verb, b_hp, b_tune;
  int fx_music_lp;
  /* published */
  uint32_t hash, seg[4], dec_hash[8];
  int32_t dec_frames[8];
  float peak, rms;
  int32_t done, clock_ok, steal_ok, loaded;
  uint32_t music_clock, tune_clock;
  float col_peak[TICKS];
} mix_app;

static mix_app app;
static const char *dec_names[] = { "kick.wav", "hat.wav", "blip.flac", "zap.mp3", "music.ogg", "ambience.opus",
                                   "tune.mod", "engine.wav" };

static void decoder_hashes(mix_app *a) {
  for (int i = 0; i < 8; i++) {
    float *pcm;
    int ch, rate;
    int64_t n = cw_audio_decode_asset(dec_names[i], &pcm, &ch, &rate);
    if (n > 0 && !strcmp(dec_names[i], "tune.mod")) n = CW_MIN(n, (int64_t)rate * 4); /* 4 s of the module */
    a->dec_frames[i] = (int32_t)n;
    a->dec_hash[i] = n > 0 ? cw_audio_analyze(pcm, (size_t)n * ch).hash : 0;
    cw_free(pcm);
  }
}

static void init(void *user) {
  mix_app *a = user;
  a->buf = cw_calloc(TOTAL * 2, sizeof(float), CW_MEM_GAME);
  cw_audio_init(&(cw_audio_desc){ .sample_rate = RATE, .offline = true, .max_voices = 32 });
  decoder_hashes(a);

  /* #region buses */
  /* buses: master <- music, sfx, verb, hp, tune; sfx -> verb (send) */
  a->b_verb = cw_bus_create("verb", CW_AUDIO_MASTER);
  a->b_music = cw_bus_create("music", CW_AUDIO_MASTER);
  a->b_sfx = cw_bus_create("sfx", CW_AUDIO_MASTER);
  a->b_hp = cw_bus_create("hp", a->b_sfx);
  a->b_tune = cw_bus_create("tune", CW_AUDIO_MASTER);
  cw_bus_add_fx(a->b_verb, &(cw_fx_desc){ .type = CW_FX_REVERB, .reverb = { 0.7f, 0.4f, 1.0f, 0.0f, 1.0f } });
  a->fx_music_lp = cw_bus_add_fx(a->b_music, &(cw_fx_desc){ .type = CW_FX_LOWPASS, .filter = { 8000, 0.9f } });
  cw_bus_add_fx(a->b_music, &(cw_fx_desc){ .type = CW_FX_COMPRESSOR, .compressor = { -18, 4, 0.01f, 0.15f, 3 } });
  cw_bus_add_fx(a->b_sfx, &(cw_fx_desc){ .type = CW_FX_DELAY, .delay = { 0.18f, 0.45f, 0.5f, 1.0f } });
  cw_bus_add_fx(a->b_hp, &(cw_fx_desc){ .type = CW_FX_HIGHPASS, .filter = { 3000, 0.7f } });
  cw_bus_add_fx(CW_AUDIO_MASTER, &(cw_fx_desc){ .type = CW_FX_LIMITER, .limiter = { -1.0f, 0.08f } });
  cw_bus_set_send(a->b_sfx, 0, a->b_verb, 0.35f);
  cw_bus_set_volume(a->b_tune, 0.6f);
  /* #endregion */

  /* #region sounds */
  a->kick = cw_sound_load("kick.wav", &(cw_sound_desc){ .max_voices = 3, .bus = a->b_sfx });
  a->hat = cw_sound_load("hat.wav", &(cw_sound_desc){ .bus = a->b_hp, .volume = 0.7f });
  a->blip = cw_sound_load("blip.flac", &(cw_sound_desc){ .bus = a->b_sfx });
  a->zap = cw_sound_load("zap.mp3", &(cw_sound_desc){ .bus = a->b_sfx, .volume = 0.6f });
  a->engine = cw_sound_load("engine.wav", NULL);
  a->music = cw_sound_load("music.ogg", &(cw_sound_desc){ .mode = CW_SOUND_STREAM, .bus = a->b_music });
  a->ambience = cw_sound_load("ambience.opus", &(cw_sound_desc){ .mode = CW_SOUND_STREAM, .volume = 0.5f });
  a->tune = cw_sound_load("tune.mod", &(cw_sound_desc){ .bus = a->b_tune });
  /* #endregion */
  a->loaded = !CW_HANDLE_IS_NULL(a->kick) + !CW_HANDLE_IS_NULL(a->hat) + !CW_HANDLE_IS_NULL(a->blip) +
              !CW_HANDLE_IS_NULL(a->zap) + !CW_HANDLE_IS_NULL(a->engine) + !CW_HANDLE_IS_NULL(a->music) +
              !CW_HANDLE_IS_NULL(a->ambience) + !CW_HANDLE_IS_NULL(a->tune);

  cw_audio_set_listener((t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 0, -1 }, (t3_vec3){ 0, 1, 0 });

  cw_debug_u32("mix.hash", &a->hash);
  cw_debug_f32("mix.peak", &a->peak);
  cw_debug_f32("mix.rms", &a->rms);
  cw_debug_field("mix.seg_hash", a->seg, CW_DBG_U32, 4);
  cw_debug_field("mix.dec_hash", a->dec_hash, CW_DBG_U32, 8);
  cw_debug_field("mix.dec_frames", a->dec_frames, CW_DBG_I32, 8);
  cw_debug_i32("mix.done", &a->done);
  cw_debug_i32("mix.loaded", &a->loaded);
  cw_debug_i32("mix.clock_ok", &a->clock_ok);
  cw_debug_i32("mix.steal_ok", &a->steal_ok);
  cw_debug_u32("mix.music_clock", &a->music_clock);
  cw_debug_u32("mix.tune_clock", &a->tune_clock);
}

/* The engine's flight: x from -30 to +30 over ticks 20..200 at z = -4. */
static t3_vec3 engine_pos(int tick) {
  float t = (float)(tick - 20) / 180.0f;
  return (t3_vec3){ -30.0f + 60.0f * t, 0, -4 };
}

static void script(mix_app *a, int t) {
  if (t == 0) {
    a->v_music = cw_audio_play(a->music, &(cw_play_desc){ .loop = true });
    a->v_amb = cw_audio_play(a->ambience, &(cw_play_desc){ .loop = true, .fade_in = 0.5f, .pan = -0.3f });
    a->v_tune = cw_audio_play(a->tune, &(cw_play_desc){ .loop = true });
    /* sample-accurate scheduling: a blip at an exact mixer frame */
    cw_audio_play(a->blip, &(cw_play_desc){ .start_frame = 2 * RATE + 123 });
  }
  if (t % 15 == 5) cw_audio_play(a->hat, &(cw_play_desc){ .pan = (t % 30 == 5) ? -0.6f : 0.6f });
  if (t % 30 == 10) cw_audio_play(a->kick, NULL);
  if (t == 40 || t == 70 || t == 100)
    cw_audio_play(a->zap, &(cw_play_desc){ .pitch = t == 40 ? 1.0f : t == 70 ? 1.5f : 0.75f, .pan = 0.4f });
  /* #region voice3d */
  if (t == 20) {
    a->v_engine = cw_audio_play(a->engine, &(cw_play_desc){
        .loop = true, .volume = 0.8f, .spatial = true,
        .space = { .position = engine_pos(t), .velocity = { 20, 0, 0 }, .direction = { 1, 0, 0 },
                   .model = CW_ATTEN_INVERSE, .ref_distance = 2, .max_distance = 60, .rolloff = 1,
                   .cone_inner_deg = 120, .cone_outer_deg = 240, .cone_outer_gain = 0.4f, .doppler = 1 } });
  }
  if (t > 20 && t <= 200) cw_voice_set_position(a->v_engine, engine_pos(t), (t3_vec3){ 20, 0, 0 });
  if (t == 110) cw_voice_set_occlusion(a->v_engine, 0.8f);
  if (t == 200) cw_voice_stop(a->v_engine, 0.25f);
  /* #endregion */
  /* music low-pass sweep */
  if (t >= 60 && t < 180 && t % 10 == 0) {
    float cutoff = t < 120 ? 8000.0f - (float)(t - 60) * 120.0f : 800.0f + (float)(t - 120) * 120.0f;
    cw_bus_set_fx(a->b_music, a->fx_music_lp, &(cw_fx_desc){ .type = CW_FX_LOWPASS, .filter = { cutoff, 0.9f } });
  }
  if (t == 130) cw_bus_set_solo(a->b_tune, true);
  if (t == 160) cw_bus_set_solo(a->b_tune, false);
  if (t == 150) cw_voice_stop(a->v_amb, 0.6f);
  /* voice limit: 5 kicks at once with max_voices 3 steal the oldest */
  if (t == 215) {
    cw_voice k[5];
    for (int i = 0; i < 5; i++) k[i] = cw_audio_play(a->kick, &(cw_play_desc){ .pitch = 1.0f + 0.1f * (float)i });
    a->steal_ok = !cw_voice_is_playing(k[0]) && !cw_voice_is_playing(k[1]) && cw_voice_is_playing(k[2]) &&
                  cw_voice_is_playing(k[4]);
  }
  if (t == 225) cw_voice_set_pitch(a->v_music, 1.25f);
}

/* Native: CW_AUDIO_MIX_WAV=path writes the rendered scene as a float WAV, to
 * listen to or inspect (e.g. ffmpeg's showspectrumpic). */
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

static void finish(mix_app *a) {
  cw_audio_analysis r = cw_audio_analyze(a->buf, TOTAL * 2);
  a->hash = r.hash;
  a->peak = r.peak;
  a->rms = r.rms;
  for (int s = 0; s < 4; s++) a->seg[s] = cw_audio_analyze(a->buf + s * RATE * 2, RATE * 2).hash;
  /* music clock: 225 ticks at ratio 44100/48000, then 15 at 1.25x, in 32.32 */
  int64_t clock = cw_voice_frames_played(a->v_music);
  uint64_t step1 = (uint64_t)((double)(44100.0f / 48000.0f) * 4294967296.0);
  uint64_t step2 = (uint64_t)((double)((44100.0f / 48000.0f) * 1.25f) * 4294967296.0);
  /* the pitch change lands at the start of the block after tick 225's commands */
  uint64_t f1 = (uint64_t)225 * FRAMES_PER_TICK;
  uint64_t pos = step1 * f1 + step2 * (uint64_t)(TOTAL - f1);
  a->clock_ok = clock == (int64_t)(pos >> 32);
  a->music_clock = (uint32_t)clock;
  a->tune_clock = (uint32_t)cw_voice_frames_played(a->v_tune);
  a->done = 1;
  if (getenv("CW_AUDIO_MIX_WAV")) write_wav(getenv("CW_AUDIO_MIX_WAV"), a->buf, TOTAL);
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  cw_info("audio_mix: hash %08x peak %.4f rms %.5f clock %lld (%s) underruns %u", a->hash, a->peak, a->rms,
          (long long)clock, a->clock_ok ? "exact" : "OFF", st.underruns);
}

/* #region render */
static void update(void *user, double dt) {
  (void)dt;
  mix_app *a = user;
  if (a->tick >= TICKS) return;
  script(a, a->tick);
  float *out = a->buf + (size_t)a->tick * FRAMES_PER_TICK * 2;
  cw_audio_render(out, FRAMES_PER_TICK);
  float pk = 0;
  for (int i = 0; i < FRAMES_PER_TICK * 2; i++) pk = CW_MAX(pk, out[i] < 0 ? -out[i] : out[i]);
  a->col_peak[a->tick] = pk;
  if (++a->tick == TICKS) finish(a);
}
/* #endregion */

/* a filled rectangle at (x, y, w, h) in drawable pixels, bottom-left origin
 * (the layout this sample was drawn in), through the 2D renderer */
static void rect(int x, int y, int w, int h, float r, float g, float b) {
  if (w <= 0 || h <= 0) return;
  int dw, dh;
  cw_drawable_size(&dw, &dh);
  cw_vec2 c = cw_r2d_canvas_size();
  float kx = c.x / (float)dw, ky = c.y / (float)dh;
  cw_r2d_rect((cw_rect){ (float)x * kx, (float)(dh - y - h) * ky, (float)w * kx, (float)h * ky }, cw_rgba(r, g, b, 1));
}

static void render(void *user, double alpha) {
  (void)alpha;
  mix_app *a = user;
  int w, h;
  cw_drawable_size(&w, &h);
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_rgba(0.06f, 0.07f, 0.09f, 1) });
  float sx = (float)w / 480.0f, sy = (float)h / 270.0f;
  /* waveform peaks: one column per rendered tick */
  for (int t = 0; t < a->tick; t++) {
    int bh = (int)(a->col_peak[t] * 180.0f * sy);
    rect((int)((float)t * 2 * sx), (int)(60 * sy), CW_MAX(1, (int)(2 * sx)), CW_MAX(1, bh), 0.3f, 0.7f, 1.0f);
  }
  /* the hash, one square per bit (white = 1) */
  for (int i = 0; i < 32; i++) {
    float v = (a->hash >> i) & 1 ? 1.0f : 0.2f;
    rect((int)((8 + i * 14) * sx), (int)(16 * sy), (int)(12 * sx), (int)(24 * sy), v, a->done ? v : 0.2f, v);
  }
  cw_r2d_end();
}

static void shutdown(void *user) {
  mix_app *a = user;
  cw_audio_shutdown();
  cw_free(a->buf);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel audio mix", .width = 480, .height = 270, .debug = true,
                        .init = init, .update = update, .render = render, .shutdown = shutdown, .user = &app };
}

/* audio_play: the audio module through the REAL output path. Natively the
 * mixer runs on SDL's audio thread; in carts on the job pool while the frame
 * renders, filling the wasmcart audio ring that wc_render hands the host.
 *
 * A looping Ogg Vorbis stream (decoded on the job pool), a FLAC blip every
 * half second panned left/right, and a generated tone (cw_sound_from_pcm)
 * circling the listener as a 3D voice. How much gets mixed depends on the
 * host's clock, so the published state is only what must hold everywhere:
 * a device is open, the mixer is producing frames, they are not silent, the
 * stream is playing and its clock moves. The picture is a pure function of
 * the frame number. CW_PLAY_NO_AUDIO=1 skips audio (the gate's control).
 *
 * A native test run (CW_FRAMES) does not pace frames in real time, so 120
 * frames can be over before SDL's audio thread has asked for much. Natively
 * the sample therefore lets the device run (up to 3 s) at frame 100 until the
 * stream clock has moved. A cart's wc_render hands over each frame's share,
 * so there the frames themselves are the clock and no wait is needed (or
 * possible). */
#include <stdlib.h>
#include <time.h>

#include "cartwheel/audio.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"

typedef struct {
  bool on;
  cw_sound music, blip, tone;
  cw_voice v_music, v_tone;
  int frame;
  int32_t device, mixing, audible, music_playing, clock_moves;
  int64_t first_clock;
} play_app;

static play_app app;

static t3_vec3 orbit(int frame) {
  float s, c;
  cw_sincos((float)frame * 0.05f, &s, &c);
  return (t3_vec3){ 6.0f * s, 0, -6.0f * c };
}

static void init(void *user) {
  play_app *a = user;
  cw_debug_i32("play.device", &a->device);
  cw_debug_i32("play.mixing", &a->mixing);
  cw_debug_i32("play.audible", &a->audible);
  cw_debug_i32("play.music_playing", &a->music_playing);
  cw_debug_i32("play.clock_moves", &a->clock_moves);
  if (getenv("CW_PLAY_NO_AUDIO")) return;
  /* #region init */
  a->on = cw_audio_init(NULL);
  a->music = cw_sound_load("music.ogg", &(cw_sound_desc){ .mode = CW_SOUND_STREAM, .volume = 0.5f });
  a->blip = cw_sound_load("blip.flac", &(cw_sound_desc){ .volume = 0.4f });
  /* a 330 Hz tone, one second, generated here */
  static float pcm[48000];
  for (int i = 0; i < 48000; i++) pcm[i] = 0.3f * cw_sin(CW_TAU * 330.0f * (float)i / 48000.0f);
  a->tone = cw_sound_from_pcm(pcm, 48000, 1, 48000, NULL);
  a->v_music = cw_audio_play(a->music, &(cw_play_desc){ .loop = true, .fade_in = 0.3f });
  a->v_tone = cw_audio_play(a->tone, &(cw_play_desc){
      .loop = true, .spatial = true,
      .space = { .position = orbit(0), .model = CW_ATTEN_INVERSE, .ref_distance = 2, .doppler = 1 } });
  /* #endregion */
}

static double wall_seconds(void) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void observe(play_app *a) {
  cw_audio_update();
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  a->device = st.device;
  a->mixing |= st.mixed_frames > 0;
  a->audible |= st.peak > 0.01f;
  a->music_playing = cw_voice_is_playing(a->v_music);
  int64_t clock = cw_voice_frames_played(a->v_music);
  if (a->first_clock == 0 && clock > 0) a->first_clock = clock;
  a->clock_moves |= a->first_clock > 0 && clock > a->first_clock;
}

static void update(void *user, double dt) {
  (void)dt;
  play_app *a = user;
  a->frame++;
  if (!a->on) return;
  if (a->frame % 30 == 0) cw_audio_play(a->blip, &(cw_play_desc){ .pan = (a->frame / 30) % 2 ? -0.7f : 0.7f });
  t3_vec3 p = orbit(a->frame), q = orbit(a->frame + 1);
  cw_voice_set_position(a->v_tone, p, (t3_vec3){ (q.x - p.x) * 60.0f, 0, (q.z - p.z) * 60.0f });
  observe(a);
#if !CW_CART
  if (a->frame == 100 && a->device)
    for (double t0 = wall_seconds(); !(a->clock_moves && a->audible) && wall_seconds() - t0 < 3.0;) observe(a);
#endif
}

static void rect(int x, int y, int w, int h, float r, float g, float b) {
  glScissor(x, y, w, h);
  glClearColor(r, g, b, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void render(void *user, double alpha) {
  (void)alpha;
  play_app *a = user;
  int w, h;
  cw_drawable_size(&w, &h);
  glViewport(0, 0, w, h);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.08f, 0.08f, 0.1f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  float sx = (float)w / 320.0f, sy = (float)h / 240.0f;
  /* listener at the centre, the orbiting voice around it */
  rect((int)(156 * sx), (int)(116 * sy), (int)(8 * sx), (int)(8 * sy), 1, 1, 1);
  t3_vec3 p = orbit(a->frame);
  rect((int)((160 + p.x * 15 - 5) * sx), (int)((120 - p.z * 15 - 5) * sy), (int)(10 * sx), (int)(10 * sy), 1, 0.6f, 0.2f);
  glDisable(GL_SCISSOR_TEST);
}

static void shutdown(void *user) {
  (void)user;
  cw_audio_shutdown();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel audio play", .width = 320, .height = 240, .debug = true,
                        .init = init, .update = update, .render = render, .shutdown = shutdown, .user = &app };
}

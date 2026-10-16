/* video_cutscene: an MPEG-1 cutscene (assets/clip.mpg, made by
 * make-clip.mjs: 4 s, 320x180, 30 fps, MP2 sound) played from the tick
 * count, drawn as a sprite (GL or WebGPU: the YUV to RGB conversion runs in
 * a shader on either), with its sound through a stream voice.
 *
 * The script: play from the start (ticks 0..239: frames 0..119), hold past
 * the end (ticks 240..269: a non-looping video keeps its last frame and
 * reports ended), then jump back to 1 s and play on (ticks 270..329: a
 * rewind and decode forward). The audio is mixed offline, one 1/60 s slice
 * per tick, so its hash is the same on every host.
 *
 * Published: the frame shown, the hash of its decoded planes, a hash over
 * every frame shown, the hash of the whole mix, the stream voice's clock and
 * the video's info. The picture is the frame letterboxed in the window with
 * a progress bar under it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/audio.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/video.h"

#define RATE 48000
#define FRAMES_PER_TICK (RATE / 60)
#define TICKS 330

typedef struct {
  cw_video video;
  float *mix;
  int32_t tick, frame, ended, width, height, frames, done, has_audio, ended_seen, max_frame;
  uint32_t frame_hash, frames_hash, audio_hash, voice_clock;
  float fps, peak;
  float decode_ms; /* mean per decoded frame (timing: never compared) */
  double decode_s;
  int32_t decoded;
} app_t;
static app_t app;

static void init(void *user) {
  app_t *a = user;
  if (!cw_r2d_init()) CW_FATAL("render2d init failed");
  a->mix = cw_calloc((size_t)TICKS * FRAMES_PER_TICK * 2, sizeof(float), CW_MEM_GAME);
  cw_audio_init(&(cw_audio_desc){ .sample_rate = RATE, .offline = true });
  /* #region open */
  a->video = cw_video_open("clip.mpg", &(cw_video_desc){ .volume = 0.8f });
  cw_video_info info = { 0 };
  cw_video_get_info(a->video, &info);
  /* #endregion */
  a->width = info.width;
  a->height = info.height;
  a->frames = info.frames;
  a->fps = (float)info.fps;
  a->has_audio = info.has_audio;
  a->frames_hash = CW_HASH32_INIT;
  a->frame = -1;
  cw_debug_i32("video.frame", &a->frame);
  cw_debug_u32("video.frame_hash", &a->frame_hash);
  cw_debug_u32("video.frames_hash", &a->frames_hash);
  cw_debug_i32("video.ended", &a->ended);
  cw_debug_i32("video.ended_seen", &a->ended_seen);
  cw_debug_i32("video.max_frame", &a->max_frame);
  cw_debug_i32("video.width", &a->width);
  cw_debug_i32("video.height", &a->height);
  cw_debug_i32("video.frames", &a->frames);
  cw_debug_f32("video.fps", &a->fps);
  cw_debug_i32("video.has_audio", &a->has_audio);
  cw_debug_u32("video.audio_hash", &a->audio_hash);
  cw_debug_f32("video.peak", &a->peak);
  cw_debug_u32("video.voice_clock", &a->voice_clock);
  cw_debug_i32("video.done", &a->done);
  cw_debug_f32("video.decode_ms", &a->decode_ms);
}

/* #region time */
/* the video's time from the tick count: never a clock, so replays match */
static double video_time(int tick) {
  if (tick < 270) return (double)tick / 60.0;   /* play, then hold past the end */
  return 1.0 + (double)(tick - 270) / 60.0;      /* jump back to 1 s */
}
/* #endregion */

static void fixed_update(void *user, double dt) {
  (void)dt;
  app_t *a = user;
  if (a->tick >= TICKS) return;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int before = a->frame;
  /* #region update */
  a->frame = cw_video_update(a->video, video_time(a->tick));
  a->ended = cw_video_ended(a->video);
  /* #endregion */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (a->frame == before + 1) { /* one frame decoded this tick */
    a->decode_s += (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    a->decoded++;
    a->decode_ms = (float)(a->decode_s * 1e3 / a->decoded);
  }
  a->ended_seen |= a->ended;
  a->max_frame = a->frame > a->max_frame ? a->frame : a->max_frame;
  cw_video_info info;
  if (cw_video_get_info(a->video, &info)) a->frames = info.frames; /* exact once the end was decoded */
  a->frame_hash = cw_video_frame_hash(a->video);
  a->frames_hash = cw_hash32(&a->frame_hash, 4, a->frames_hash);
  cw_audio_render(a->mix + (size_t)a->tick * FRAMES_PER_TICK * 2, FRAMES_PER_TICK);
  int64_t clock = cw_voice_frames_played(cw_video_voice(a->video));
  if (clock >= 0) a->voice_clock = (uint32_t)clock;
  if (++a->tick == TICKS) {
    cw_audio_analysis r = cw_audio_analyze(a->mix, (size_t)TICKS * FRAMES_PER_TICK * 2);
    a->audio_hash = r.hash;
    a->peak = r.peak;
    a->done = 1;
    cw_info("video_cutscene: frames hash %08x, audio hash %08x peak %.3f, voice clock %u", a->frames_hash, a->audio_hash,
            (double)a->peak, a->voice_clock);
  }
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  /* #region draw */
  cw_texture tex = cw_video_texture(a->video); /* converts a new frame on the GPU */
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = { 0.02f, 0.02f, 0.03f, 1 } });
  cw_vec2 size = cw_r2d_canvas_size();
  float s = fminf(size.x / (float)a->width, (size.y - 16) / (float)a->height);
  cw_vec2 drawn = { (float)a->width * s, (float)a->height * s };
  cw_r2d_sprite(&(cw_sprite){ .texture = tex, .pos = { (size.x - drawn.x) * 0.5f, 0 }, .size = drawn });
  /* #endregion */
  float p = a->frames > 0 ? (float)(a->frame + 1) / (float)a->frames : 0;
  cw_r2d_rect((cw_rect){ 0, size.y - 10, size.x, 10 }, (cw_color){ 0.15f, 0.15f, 0.2f, 1 });
  cw_r2d_rect((cw_rect){ 0, size.y - 10, size.x * p, 10 },
              a->ended ? (cw_color){ 0.9f, 0.4f, 0.2f, 1 } : (cw_color){ 0.3f, 0.7f, 1.0f, 1 });
  cw_r2d_end();
}

static void shutdown(void *user) {
  app_t *a = user;
  cw_video_close(a->video);
  cw_audio_shutdown();
  cw_free(a->mix);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel video cutscene", .width = 480, .height = 270, .debug = true,
                        .init = init, .fixed_update = fixed_update, .render = render, .shutdown = shutdown,
                        .user = &app };
}

/* cartwheel video: MPEG-1 playback for cutscenes (pl_mpeg).
 *
 * A cw_video plays an MPEG-1 program stream (.mpg: MPEG-1 video, MP2
 * audio). Make one with ffmpeg, e.g.
 *   ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 128k -f mpeg out.mpg
 *
 * ── Time ────────────────────────────────────────────────────────────
 * The game says which moment to show: cw_video_update(v, t) with t in
 * seconds since the video started, taken from the fixed tick count (e.g.
 * (cw_tick_index() - start_tick) * cw_fixed_dt()), never from a clock. The
 * frame shown is floor(t * fps), so a replay, a slow machine and every host
 * show the same frame on the same tick. Decoding moves forward frame by
 * frame; a t earlier than the current frame (or a loop) restarts from the
 * beginning. MPEG-1 decoding is integer, so frames are identical everywhere.
 *
 * ── Picture ─────────────────────────────────────────────────────────
 * cw_video_texture returns a render target (cartwheel/render2d.h) holding
 * the current frame as RGB: draw it as a sprite (cw_r2d_sprite), GL or
 * WebGPU alike. The decoder produces Y, Cb and Cr planes; they are uploaded
 * and converted to RGB by a shader on the GPU, only when a new frame was
 * decoded and only when the texture is asked for, so call cw_video_texture
 * where you draw (render), not in update. On GL a conversion changes GL
 * state the way a 2D pass does (cartwheel/render2d.h): call it before your
 * 2D pass, or reset three.c's bindings after it (t3_renderer_reset_bindings).
 *
 * ── Sound ───────────────────────────────────────────────────────────
 * A file with audio plays it as a stream voice (cartwheel/audio.h) started
 * by the first cw_video_update, on the bus and volume of the desc (needs
 * cw_audio_init). Its MP2 frames are decoded on the audio job pool like
 * any stream. The audio runs on the mixer's clock: over a long video it may
 * drift from the tick clock by the difference between the two (a few ms a
 * minute), which a cutscene does not notice.
 *
 * Cost: decoding is on the game thread, in cw_video_update: about 0.22 ms
 * per 320x180 frame on a Ryzen AI 9 HX 370 core (about 0.9 ms at 640x360);
 * the GPU conversion is one triangle over the target. */
#ifndef CW_VIDEO_H
#define CW_VIDEO_H

#include "audio.h"
#include "base.h"
#include "render2d.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_video);

typedef struct cw_video_desc {
  bool loop;     /* start over after the last frame (the audio loops too) */
  bool mute;     /* no audio voice even when the file has sound */
  float volume;  /* audio voice volume; 0 = 1 */
  cw_bus bus;    /* audio bus; 0 = master */
} cw_video_desc;

typedef struct cw_video_info {
  int width, height;  /* picture size in pixels */
  double fps;
  int frames;         /* frames in the file: from its time stamps, exact once decoding has reached the end */
  double duration;    /* seconds: frames / fps */
  bool has_audio;
  int audio_rate;     /* Hz; 0 without audio */
} cw_video_info;

/* Opens a video from the asset bundle (the file is read into memory and
 * kept; the first frame is decoded here, nothing else). Logs and returns the null handle
 * when the asset is missing or not an MPEG-1 program stream. Main thread. */
cw_video cw_video_open(const char *asset_path, const cw_video_desc *desc);
/* Stops its audio voice and frees everything. Stale handle: no-op. */
void cw_video_close(cw_video v);
/* Shows the frame for t seconds since the start (see Time). Returns the
 * frame index now shown (the last one once a non-looping video has ended),
 * -1 for a stale handle. */
int cw_video_update(cw_video v, double t);
/* The current frame as an RGB render target, converted on the GPU if a new
 * frame was decoded since the last call (see Picture). Owned by the video. */
cw_texture cw_video_texture(cw_video v);
bool cw_video_get_info(cw_video v, cw_video_info *out); /* false for a stale handle */
/* A non-looping video's t has reached its end. */
bool cw_video_ended(cw_video v);
/* The audio voice (the null handle without sound or before the first update). */
cw_voice cw_video_voice(cw_video v);
/* FNV-1a over the current frame's Y, Cb and Cr planes (what the decoder
 * produced, before the GPU): what the cross-host tests compare. */
uint32_t cw_video_frame_hash(cw_video v);

#ifdef __cplusplus
}
#endif
#endif

/* Video playback (cartwheel/video.h): pl_mpeg decodes MPEG-1 video frames
 * on the game thread, render2d converts them to RGB on the GPU (yuv.c),
 * and the MP2 sound plays as an ordinary stream voice (the audio module's
 * MPEG decoder). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/platform.h"
#include "cartwheel/video.h"
#include "render2d/yuv.h"
#include <stddef.h>
#include <stdio.h>

#include "pl_mpeg.h"

typedef struct cw__video {
  uint8_t *data; /* the file (pl_mpeg reads it in place) */
  size_t len;
  plm_t *plm;
  plm_frame_t *frame; /* current, valid until the next decode */
  int index;          /* current frame, -1 = none */
  int count;          /* frames in the file */
  cw_video_desc desc;
  cw_video_info info;
  cw__yuv *yuv;
  bool dirty, ended, started, exact;
  cw_sound sound;
  cw_voice voice;
} cw__video;

static cw_pool videos;
static bool pool_ready;

static cw__video *get(cw_video v) { return pool_ready ? cw_pool_get(&videos, v.id) : NULL; }

/* One decoded frame forward; false at the end. */
static bool step(cw__video *vd) {
  plm_frame_t *f = plm_decode_video(vd->plm);
  if (!f) return false;
  vd->frame = f;
  vd->index++;
  vd->dirty = true;
  return true;
}

static void restart(cw__video *vd) {
  plm_rewind(vd->plm);
  vd->frame = NULL;
  vd->index = -1;
}

cw_video cw_video_open(const char *asset_path, const cw_video_desc *desc) {
  size_t len = 0;
  void *data = cw_asset_load(asset_path, &len);
  if (!data) {
    cw_error("video: asset '%s' not found", asset_path);
    return (cw_video){ 0 };
  }
  plm_t *plm = plm_create_with_memory(data, len, 0);
  if (!plm || !plm_has_headers(plm) || plm_get_num_video_streams(plm) < 1) {
    cw_error("video: '%s' is not an MPEG-1 program stream with video (make one with ffmpeg: -c:v mpeg1video "
             "-c:a mp2 -f mpeg)", asset_path);
    if (plm) plm_destroy(plm);
    free(data);
    return (cw_video){ 0 };
  }
  if (!pool_ready) {
    cw_pool_init(&videos, sizeof(cw__video), CW_MEM_RESOURCE);
    pool_ready = true;
  }
  uint64_t id;
  cw__video *vd = cw_pool_alloc(&videos, &id);
  memset(vd, 0, sizeof *vd);
  vd->data = data;
  vd->len = len;
  vd->plm = plm;
  vd->desc = desc ? *desc : (cw_video_desc){ 0 };
  vd->index = -1;
  plm_set_audio_enabled(plm, 0); /* the audio voice has its own decoder */
  plm_set_loop(plm, 0);
  vd->info.width = plm_get_width(plm);
  vd->info.height = plm_get_height(plm);
  vd->info.fps = plm_get_framerate(plm);
  vd->info.has_audio = plm_get_num_audio_streams(plm) > 0;
  vd->info.audio_rate = vd->info.has_audio ? plm_get_samplerate(plm) : 0;
  /* frames from the stream's time stamps (no decode); exact once decoding reaches the end */
  double dur = plm_get_duration(plm);
  vd->count = vd->info.fps > 0 && dur > 0 ? (int)floor(dur * vd->info.fps + 0.5) : 1;
  if (vd->count < 1) vd->count = 1;
  vd->info.frames = vd->count;
  vd->info.duration = vd->info.fps > 0 ? (double)vd->count / vd->info.fps : 0;
  if (!step(vd)) {
    cw_error("video: '%s' has no decodable frame", asset_path);
    cw_video_close((cw_video){ id });
    return (cw_video){ 0 };
  }
  if (vd->info.has_audio && !vd->desc.mute && cw_audio_sample_rate() > 0) {
    vd->sound = cw_sound_load_memory(data, len, &(cw_sound_desc){ .mode = CW_SOUND_STREAM, .bus = vd->desc.bus });
    if (CW_HANDLE_IS_NULL(vd->sound)) cw_warn("video: '%s': its audio could not be opened; playing silent", asset_path);
  }
  return (cw_video){ id };
}

void cw_video_close(cw_video v) {
  cw__video *vd = get(v);
  if (!vd) return;
  if (!CW_HANDLE_IS_NULL(vd->sound)) cw_sound_free(vd->sound); /* stops its voice */
  cw__yuv_free(vd->yuv);
  plm_destroy(vd->plm);
  free(vd->data);
  cw_pool_release(&videos, v.id);
}

int cw_video_update(cw_video v, double t) {
  cw__video *vd = get(v);
  if (!vd) return -1;
  if (!vd->started) {
    vd->started = true;
    if (!CW_HANDLE_IS_NULL(vd->sound))
      vd->voice = cw_audio_play(vd->sound, &(cw_play_desc){ .loop = vd->desc.loop, .volume = vd->desc.volume,
                                                             .bus = vd->desc.bus });
  }
  /* the frame for t: floor(t * fps), a hair of slack so t = k / fps lands on k */
  double f = t * vd->info.fps + 1e-6;
  int k = f > 0 ? (int)floor(f) : 0;
  /* count is the time stamps' estimate until decoding has met the end (exact) */
  int want = vd->desc.loop && vd->exact ? k % vd->count : k;
  if (want < vd->index) restart(vd);
  while (vd->index < want && step(vd)) {}
  if (vd->index < want && vd->index >= 0) { /* the end: now the count is exact */
    vd->count = vd->index + 1;
    vd->exact = true;
    vd->info.frames = vd->count;
    vd->info.duration = (double)vd->count / vd->info.fps;
    if (vd->desc.loop) {
      want = k % vd->count;
      if (want < vd->index) restart(vd);
      while (vd->index < want && step(vd)) {}
    }
  }
  vd->ended = !vd->desc.loop && vd->exact && k >= vd->count;
  return vd->index;
}

cw_texture cw_video_texture(cw_video v) {
  cw__video *vd = get(v);
  if (!vd) return (cw_texture){ 0 };
  if (!vd->yuv) {
    vd->yuv = cw__yuv_new(vd->info.width, vd->info.height, (int)vd->frame->y.width, (int)vd->frame->y.height);
    if (!vd->yuv) return (cw_texture){ 0 };
  }
  if (vd->dirty && vd->frame) {
    const uint8_t *planes[3] = { vd->frame->y.data, vd->frame->cb.data, vd->frame->cr.data };
    cw__yuv_convert(vd->yuv, planes);
    vd->dirty = false;
  }
  return cw__yuv_target(vd->yuv);
}

bool cw_video_get_info(cw_video v, cw_video_info *out) {
  cw__video *vd = get(v);
  if (!vd) return false;
  *out = vd->info;
  return true;
}

bool cw_video_ended(cw_video v) {
  cw__video *vd = get(v);
  return vd && vd->ended;
}

cw_voice cw_video_voice(cw_video v) {
  cw__video *vd = get(v);
  return vd ? vd->voice : (cw_voice){ 0 };
}

uint32_t cw_video_frame_hash(cw_video v) {
  cw__video *vd = get(v);
  if (!vd || !vd->frame) return 0;
  const plm_frame_t *f = vd->frame;
  uint32_t h = CW_HASH32_INIT;
  h = cw_hash32(f->y.data, (size_t)f->y.width * f->y.height, h);
  h = cw_hash32(f->cb.data, (size_t)f->cb.width * f->cb.height, h);
  h = cw_hash32(f->cr.data, (size_t)f->cr.width * f->cr.height, h);
  return h;
}

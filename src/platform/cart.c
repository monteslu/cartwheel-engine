/* Cart loop owner: the wasmcart exports. wc_init() runs cw_main and the
 * platform's init (the game's init runs in the first frame); every wc_render()
 * is one cw__frame. SDL (wasmcart-sdl2) is
 * pointed at the shared-memory regions the host writes each frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WC_USE_GL
#include "wasmcart.h"
#ifndef WC_HOST_FLAG_DETERMINISTIC
#define WC_HOST_FLAG_DETERMINISTIC (1u << 0) /* wasmcart SPEC.md "Deterministic replay" */
#endif
#ifndef WC_HOST_FLAG_GPU_WGPU
#define WC_HOST_FLAG_GPU_WGPU (1u << 1) /* wasmcart SPEC.md "WebGPU": the host selected WebGPU for a dual cart */
#endif

#include <wasi/libc-busywait.h>

#include "internal.h"
#include "cartwheel/jobs.h"
#include "cartwheel/profile.h"
#include "cartwheel/plugin.h"

/* wasmcart-sdl2's video backend */
extern void SDL_WASMCART_SetJoystickPads(void *pads);
extern void SDL_WASMCART_SetPads(void *pads);
extern void SDL_WASMCART_SetKeys(uint8_t *keys);
extern void SDL_WASMCART_SetPointers(void *pointers);
extern void SDL_WASMCART_SetWheel(void *wheel);

/* The backend can host a gl4es bridge for GL1 ports; cartwheel calls GLES3
 * directly through the gl imports, so the bridge is empty. */
void gl4es_bridge_init(int w, int h) { (void)w; (void)h; }
void gl4es_bridge_set_size(int w, int h) { (void)w; (void)h; }
void *wc_gl4es_GetProcAddress(const char *p) { (void)p; return NULL; }
/* software SDL surfaces blitted to GL: never used (cartwheel draws with GL) */
void wc_sdl_gl_blit(const void *pixels, int w, int h) { (void)pixels; (void)w; (void)h; }

#define AUDIO_CAP 4096
static uint32_t fb_dummy[4]; /* GL cart: the host presents the GL surface */
static float audio_ring[AUDIO_CAP * 2];
static uint32_t audio_write_cursor;
static wc_pad_t pads[4];
static wc_pad_t no_pads[4]; /* always neutral: see wc_init */
static uint8_t keys[32];
static wc_pointer_t pointers[10];
static wc_wheel_t wheel;
static wc_time_t time_info;
static wc_host_info_t host_info;
static wc_info_t info;
static bool app_loaded;
static double audio_ms_owed; /* host time not yet turned into audio frames */
static void audio_kick(void); /* below: the frame's audio mix */

static void load_app(void) {
  if (app_loaded) return;
  app_loaded = true;
  static char arg0[] = "cart";
  static char *argv[] = { arg0, NULL };
  cw__plat.app = cw_main(1, argv);
  if (cw__plat.app.width <= 0) cw__plat.app.width = 1280;
  if (cw__plat.app.height <= 0) cw__plat.app.height = 720;
  cw__resolve_scale(); /* the framebuffer the cart declares is the scaled size (until wc_init knows the host's preference) */
}

__attribute__((export_name("wc_set_seed"))) void wc_set_seed(uint32_t seed) { cw__plat.host_seed = seed; }

/* src/jobs/jobs.c sizes the pool from it: 0 = the host does not say */
int cw__cart_hardware_threads(void);
int cw__cart_hardware_threads(void) { return host_info.hardware_threads > 1024 ? 1024 : (int)host_info.hardware_threads; }

/* The host's preferred size (wc_host_info_t) sizes an auto render scale
 * (cw_app_desc.render_scale). A deterministic run (a test) keeps the default
 * 1080 lines whatever the host prefers, so its pixels do not depend on the
 * host. */
static void take_host_pref(void) {
  cw_platform *p = &cw__plat;
  bool det = (host_info.flags & WC_HOST_FLAG_DETERMINISTIC) != 0;
  if (!det && host_info.preferred_height <= 16384 && host_info.preferred_width <= 16384) {
    p->host_pref_w = (int)host_info.preferred_width;
    p->host_pref_h = (int)host_info.preferred_height;
  } else p->host_pref_w = p->host_pref_h = 0;
  cw__resolve_scale();
}

__attribute__((export_name("wc_get_info"))) wc_info_t *wc_get_info(void) {
  load_app();
  /* a host that wrote its preferences before asking (wasmcart-native's
   * deferred init: a libretro core sizes the frontend's video at load) gets
   * the size the cart will have; one that writes them later (only before
   * wc_init, as the spec asks) gets it after wc_init */
  if (!cw__plat.window && (host_info.preferred_width || host_info.preferred_height)) take_host_pref();
  info.version = WC_ABI_VERSION;
  info.width = (uint32_t)cw__plat.px_w;
  info.height = (uint32_t)cw__plat.px_h;
  info.fb_ptr = (uint32_t)(uintptr_t)fb_dummy;
  info.audio_ptr = (uint32_t)(uintptr_t)audio_ring;
  info.audio_cap = AUDIO_CAP;
  info.audio_write_ptr = (uint32_t)(uintptr_t)&audio_write_cursor;
  info.audio_sample_rate = 48000;
  info.input_ptr = (uint32_t)(uintptr_t)pads;
  info.time_ptr = (uint32_t)(uintptr_t)&time_info;
  info.host_info_ptr = (uint32_t)(uintptr_t)&host_info;
  info.pointer_ptr = (uint32_t)(uintptr_t)pointers;
  info.keys_ptr = (uint32_t)(uintptr_t)keys;
  info.wheel_ptr = (uint32_t)(uintptr_t)&wheel;
  info.save_ptr = (uint32_t)(uintptr_t)cw__plat.app.save;
  info.save_size = cw__plat.app.save ? (uint32_t)cw__plat.app.save_size : 0;
#if CW_WGPU
  info.gpu_api = 2; /* a dual cart: imports gl AND WebGPU, the host picks (WC_HOST_FLAG_GPU_WGPU) */
#else
  info.gpu_api = 1; /* WebGL2 / GLES 3.0 */
#endif
  /* WC_FLAG_HOST_THREADS: host_info has hardware_threads, which sizes the job pool */
  info.flags = WC_FLAG_AUDIO_F32 | WC_FLAG_POINTER | WC_FLAG_KEYBOARD | WC_FLAG_HOST_THREADS;
  if (cw__plat.app.deterministic) info.flags |= WC_FLAG_DETERMINISTIC;
  if (cw__plat.app.debug) info.flags |= WC_FLAG_DEBUG;
  if (cw__plat.app.net) info.flags |= WC_FLAG_NET_PEER;
  return &info;
}

void cw__programs_register(void); /* src/render/programs.c */

__attribute__((export_name("wc_init"))) void wc_init(void) {
  cw_platform *p = &cw__plat;
  cw__programs_register(); /* the app's three.c program tables, before any renderer */
  /* A browser runs a cart's main thread on the page thread, where
   * Atomics.wait throws: a contended mutex or condition variable would trap.
   * wasi-libc can spin instead of waiting on this thread (as Emscripten does
   * on the browser main thread); the workers still sleep properly. */
  __wasilibc_enable_futex_busywait_on_current_thread();
  load_app();
  p->deterministic_run = (host_info.flags & WC_HOST_FLAG_DETERMINISTIC) != 0;
  /* The host's preferred frame size (written before wc_init) settles the
   * size here, before the window and GPU exist: a RetroArch "Internal
   * Resolution" of 960x540 gets a 960x540 frame. wc_info is updated for hosts
   * that read it after wc_init or after the first wc_render (wasmcart SPEC.md,
   * "Resolution changes"). */
  take_host_pref();
  info.width = (uint32_t)p->px_w;
  info.height = (uint32_t)p->px_h;
  /* a test starts from no save data (the host restored the region before wc_init) */
  if (p->deterministic_run && p->app.save) memset(p->app.save, 0, p->app.save_size);
  p->fixed_dt = 1.0 / (p->app.fixed_hz > 0 ? p->app.fixed_hz : 60);
  /* pads reach SDL as game controllers only (SDL_WASMCART_SetPads would also
   * synthesize arrow keys from the d-pad, which the input module would then
   * count twice) */
  SDL_WASMCART_SetJoystickPads(pads);
  /* wasmcart-sdl2 delivers the raw keyboard only from inside its pad-to-key
   * pump, which returns early when no pads were given to SDL_WASMCART_SetPads
   * (so keys never reached SDL). Hand it a pad array that stays neutral:
   * the keyboard passthrough runs, and the pump synthesizes nothing. */
  SDL_WASMCART_SetPads(no_pads);
  SDL_WASMCART_SetKeys(keys);
  SDL_WASMCART_SetPointers(pointers);
  SDL_WASMCART_SetWheel(&wheel);
  cw_jobs_init(p->app.workers == 0 ? -1 : p->app.workers < 0 ? 0 : p->app.workers);
  cw__update_done_hook = audio_kick; /* the frame's audio mix starts after the update */
  if (!cw__gpu_select(p->app.gpu, (host_info.flags & WC_HOST_FLAG_GPU_WGPU) != 0)) CW_FATAL("GPU selection failed");
  if (!cw__sdl_init()) CW_FATAL("platform init failed");
  cw_plugins_init(); /* the build's runtime plugins register before the app (and its scenes) start */
  /* the app's init runs in the first frame (cw__frame): wc_init stays light */
}

/* ── audio: mixed on the job pool while the frame renders ───────────
 *
 * Every wc_render hands the host's ring the frames of its delta. They are
 * counted in whole milliseconds: rounding each frame's delta (16.67 -> 17)
 * over-produced 2% (48960 frames/s at 60 Hz), so the remainder is carried
 * (48 frames per ms at 48 kHz in the long run). The fill callback runs in
 * blocks of AUDIO_FILL frames (what SDL's wasmcart pump ran it with before);
 * the frames of a block not handed over yet wait in `left` for the next
 * frame.
 *
 * The frame's mix starts as a job when the app's update returns
 * (cw__update_done_hook): the game has sent the frame's commands, and the
 * job runs on a worker while the main thread renders. wc_render waits for it
 * at the end and only then moves the write cursor, so the host never sees a
 * partly written block and gets exactly the frames and samples it got when
 * the mix ran there. The audio module's commands are fenced while the job
 * runs: one sent during the render (no app does) waits for the next frame's
 * mix instead of racing this one. A frame with no update (warm-up, the
 * debugger's held pause), or audio opened during the render, mixes inline at
 * the end. With no workers the job runs inline where it starts. */

#define AUDIO_FILL 512
static struct {
  bool open;
  cw_job *job;     /* the frame's mix, started after the update */
  bool planned;    /* this wc_render's frames are settled (audio_plan) */
  double owed;     /* audio_ms_owed once they are handed over */
  uint32_t frames; /* to hand over this wc_render */
  uint32_t cursor; /* the write cursor after them (the mix's output) */
  cw_audio_fill_fn fill;
  void *user;
  float left[AUDIO_FILL * 2]; /* the last block's frames not handed over yet */
  int left_n;
  float work[AUDIO_FILL * 2];
} audio;

/* this wc_render's frames, from the host's delta (main thread, once a frame) */
static void audio_plan(void) {
  if (audio.planned) return;
  cw_platform *p = &cw__plat;
  audio.planned = true;
  double owed = audio_ms_owed + (time_info.delta_ms > 0 ? time_info.delta_ms : 1000.0 / 60.0);
  uint32_t ms = (uint32_t)owed;
  uint64_t frames = (uint64_t)(uint32_t)p->audio_rate * ms / 1000;
  audio.owed = owed - ms;
  audio.frames = frames > 8192 ? 8192 : (uint32_t)frames;
  audio.cursor = audio_write_cursor;
  audio.fill = p->audio_fill;
  audio.user = p->audio_user;
}

static void ring_put(const float *src, int n) {
  for (int i = 0; i < n; i++) {
    uint32_t at = (audio.cursor % AUDIO_CAP) * 2;
    audio_ring[at] = src[2 * i];
    audio_ring[at + 1] = src[2 * i + 1];
    audio.cursor++;
  }
}

/* The frame's frames into the ring past the host's write cursor: the job, or
 * inline on the main thread, never both. */
static void audio_mix(void *ctx) {
  (void)ctx;
  int need = (int)audio.frames;
  int n = audio.left_n < need ? audio.left_n : need;
  ring_put(audio.left, n);
  audio.left_n -= n;
  memmove(audio.left, audio.left + n * 2, (size_t)audio.left_n * 2 * sizeof(float));
  need -= n;
  while (need > 0) {
    memset(audio.work, 0, sizeof audio.work);
    audio.fill(audio.user, audio.work, AUDIO_FILL);
    n = need < AUDIO_FILL ? need : AUDIO_FILL;
    ring_put(audio.work, n);
    audio.left_n = AUDIO_FILL - n;
    memcpy(audio.left, audio.work + n * 2, (size_t)audio.left_n * 2 * sizeof(float));
    need -= n;
  }
}

static void audio_kick(void) {
  if (!cw__plat.audio_fill || audio.job) return;
  audio_plan();
  if (!audio.frames) return; /* the delta rounds to no frames: nothing to mix */
  if (cw__audio_fence_hook) cw__audio_fence_hook(true);
  audio.job = cw_job_single(audio_mix, NULL);
}

/* waits for the frame's mix job, if one runs; !keep drops its frames */
static void audio_join(bool keep) {
  if (!audio.job) return;
  cw_job_wait(audio.job);
  audio.job = NULL;
  if (cw__audio_fence_hook) cw__audio_fence_hook(false);
  if (!keep) audio.planned = false;
}

static void audio_hand_off(void) {
  if (audio.job) audio_join(true);
  else {
    if (!cw__plat.audio_fill) return;
    audio_plan();
    if (audio.frames) audio_mix(NULL);
  }
  audio_ms_owed = audio.owed;
  audio_write_cursor = audio.cursor;
}

/* Opening again swaps the callback once the running mix is done (a frame's
 * frames come from the callback set when its mix started); a first open
 * starts with no frames left over. */
bool cw_audio_open(int rate, cw_audio_fill_fn fill, void *user) {
  cw_platform *p = &cw__plat;
  (void)rate; /* the ring's declared rate decides */
  audio_join(true);
  p->audio_fill = fill;
  p->audio_user = user;
  if (!audio.open) {
    audio.open = true;
    audio.left_n = 0;
    p->audio_rate = (int)info.audio_sample_rate;
  }
  return true;
}
int cw_audio_rate(void) { return cw__plat.audio_rate ? cw__plat.audio_rate : (int)info.audio_sample_rate; }
/* A mix running when the audio closes is finished and dropped: the host gets
 * no frames from a closed device. */
void cw_audio_close(void) {
  audio_join(false);
  audio.open = false;
  cw__plat.audio_fill = NULL;
  cw__plat.audio_user = NULL;
}

__attribute__((export_name("wc_render"))) void wc_render(void) {
  cw_platform *p = &cw__plat;
  cw__frame(time_info.time_ms / 1000.0);
  CW_PROFILE_BEGIN("present");
  cw__wgpu_frame_end(true);
  CW_PROFILE_END();
  cw__perf_mark(CW_PERF_PRESENT);
  if (p->audio_fill) {
    CW_PROFILE_SCOPE("audio.hand_off");
    audio_hand_off();
    cw__perf_mark(CW_PERF_AUDIO);
  }
  audio.planned = false;
}

/* ── assets: the cart's own bundle ─────────────────────────────────── */

long cw_asset_size(const char *path) { return wc_asset_size(path, (uint32_t)strlen(path)); }
long cw_asset_read(const char *path, void *dst, size_t cap) {
  return wc_load_asset(path, (uint32_t)strlen(path), dst, (uint32_t)cap);
}
void cw_asset_remount(void) {}
bool cw_asset_set_root(const char *dir) { (void)dir; return false; }
const char *cw_asset_pack_path(void) { return NULL; }
/* a cart cannot list its bundle: cw_vfs_list_dir reads the bundle manifest instead */
int cw__asset_list_all(void (*cb)(const char *path, int64_t size, void *user), void *user);
int cw__asset_list_all(void (*cb)(const char *path, int64_t size, void *user), void *user) {
  (void)cb, (void)user;
  return -1;
}

void cw_debug_mark(uint32_t id) { wc_debug_mark(id); }

void cw__platform_log(cw_log_level level, const char *msg) {
  static const char *names[] = { "debug", "info", "warn", "error" };
  char line[1100];
  int n = snprintf(line, sizeof line, "[%s] %s", names[level], msg);
  wc_log(line, (uint32_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
}

/* ── text input (wasmcart text ABI: wc_on_text + wc_text_input_begin/end) ── */

static bool text_active;
void cw_text_input_start(void) {
  if (text_active) return;
  text_active = true;
  wc_text_input_begin();
}
void cw_text_input_stop(void) {
  if (!text_active) return;
  text_active = false;
  wc_text_input_end();
}
bool cw_text_input_active(void) { return text_active; }

/* The host calls this before wc_render with text the OS composed; the
 * pointer is valid only during the call, so it is copied into the queue the
 * next poll turns into CW_EVENT_TEXT. */
__attribute__((export_name("wc_on_text"))) void wc_on_text(const char *utf8, uint32_t len) {
  cw__platform_queue_text(utf8, (int)len);
}

void cw_save_commit(void) {} /* the host keeps the save region */

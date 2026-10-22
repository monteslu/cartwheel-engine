/* Shared between sdl_common.c and the two loop owners (native.c, cart.c). */
#ifndef CW_PLATFORM_INTERNAL_H
#define CW_PLATFORM_INTERNAL_H

#include <SDL.h>

#include "cartwheel/platform.h"

typedef struct cw_platform {
  cw_app_desc app;
  SDL_Window *window;
  SDL_GLContext gl;
  int win_w, win_h;      /* logical: the SDL window's size / scale */
  int px_w, px_h;        /* the output in GPU pixels: the SDL window's drawable (a HiDPI
                          * display gives dpi pixels per window point), a cart's framebuffer */
  float scale;           /* pixels per logical unit: cw_app_desc.render_scale resolved
                          * (cw__resolve_scale, in window points) times dpi once a window exists */
  float scale_pts;       /* the resolved scale in window points (what the window was sized with) */
  float dpi;             /* drawable pixels per window point (1 without a window or HiDPI) */
  /* A cart host's preferred frame size (wc_host_info_t preferred_width/height,
   * read at wc_init): an auto render scale fits the logical size into it.
   * 0 = no preference; never set in a deterministic run, or natively. */
  int host_pref_w, host_pref_h;
  /* pixel_scale / crisp UI (src/platform/pixel.c). The output is px_w x
   * px_h; the scene renders at scene_w x scene_h and is scaled up k times
   * (nearest) into the view rect of the output (centred, black bars around
   * it when the output is not a multiple). k == 1: one resolution, nothing
   * extra is drawn. */
  int pixel_n;           /* cw_app_desc.pixel_scale resolved (>= 1) */
  int ui_floor;          /* cw_app_desc.ui_min_lines resolved (0 = not offered) */
  bool crisp_ui;         /* crisp UI on (runtime) */
  bool crisp_ui_chosen;  /* set by cw_crisp_ui_set / restore (the save byte) */
  bool pixel_host;       /* a cart whose host gave a size: the output is the picture, k fixed */
  int scene_w, scene_h, k;
  int view_x, view_y, view_w, view_h;
  /* the render resolution setting (cartwheel/quality.h): k == 1 and a scale
   * under 1 renders the scene at scene_w x scene_h and scales it up with a
   * filter (upscale) into the whole output, the UI over it at full size */
  float res_scale;       /* 1 (or 0) = the output's size */
  bool upscale;
  cw_raw_input input;
  SDL_GameController *pads[CW_MAX_PADS];
  bool quit;
  /* time */
  double now, last;      /* seconds */
  double accumulator;
  double fixed_dt;
  uint64_t frame, tick;
  bool deterministic_run;
  uint32_t host_seed;
  /* audio */
  SDL_AudioDeviceID audio_dev; /* native */
  int audio_rate;
  cw_audio_fill_fn audio_fill;
  void *audio_user;
} cw_platform;

extern cw_platform cw__plat;
extern void (*cw__res_frame_hook)(void);    /* cartwheel/resource.h, once used */
/* cw__frame calls this between the app's update and its render (a cart
 * starts the frame's audio mix there, on the job pool) */
extern void (*cw__update_done_hook)(void);
/* The audio module, once initialized: on (game thread, no mix running) holds
 * every command sent from now on out of the mixes until it is called with
 * off. A cart fences the commands while its mix job runs, so a command sent
 * during the render lands in the next frame's mix whatever the timing. */
extern void (*cw__audio_fence_hook)(bool on);
extern void (*cw__res_shutdown_hook)(void);

/* sdl_common.c */
void cw__resolve_scale(void);       /* app.render_scale -> scale, px_w/px_h (after cw_main) */
/* pixel.c: the scene / UI targets of pixel_scale and crisp UI */
void cw__pixel_frame_begin(void);   /* start of a frame: the scene's target */
void cw__pixel_ui_begin(void);      /* the first 2D window pass: the UI's target */
void cw__pixel_frame_end(void);     /* end of a frame: the picture into the output */
int cw__pixel_target(void);         /* what draws now: 0 output, 1 scene, 2 UI (view-sized) */
void cw__pixel_shutdown(void);
void cw__render_scale_apply(float s); /* sdl_common.c: the render resolution setting's scale (quality.c) */
bool cw__sdl_init(void);            /* SDL_Init + window + GL context */
void cw__sdl_poll(void);            /* events -> raw input + app events */
void cw__frame(double now_seconds); /* one rendered frame: ticks, update, render */
void cw__shutdown(void);
void cw__input_script_apply(cw_raw_input *in, uint64_t frame); /* native test hook */
void cw__platform_queue_text(const char *utf8, int len); /* emitted as CW_EVENT_TEXT by the next poll */

/* gpu.c: the GPU device module (cartwheel/gpu.h) */
bool cw__gpu_select(cw_gpu_api want, bool host_wgpu); /* before cw__sdl_init */
bool cw__wgpu_init(void);                  /* WebGPU device + surface (cw__sdl_init, after the window) */
/* a native run that picked WebGPU itself (CW_GPU_AUTO) and could not start it:
 * release what started and switch to GL; false when GL is no fallback */
bool cw__gpu_fallback_to_gl(void);
void cw__wgpu_frame_end(bool present);     /* end of a frame: present / hand back, rotate error scopes */
bool cw__wgpu_readback(unsigned char **px, int *w, int *h); /* native: the frame, top-down RGBA8 */
void cw__wgpu_shutdown(void);
void cw__wgpu_drawable_size(int *w, int *h);
void cw__gpu_debug_register(void);
void cw__host_debug_update(void);   /* host.* debug fields, end of every frame */

/* debug/perf.c: frame phase timing (the "perf" debug command) */
enum { CW_PERF_INPUT, CW_PERF_SIM, CW_PERF_UPDATE, CW_PERF_RENDER, CW_PERF_PRESENT, CW_PERF_AUDIO };
void cw__perf_frame_begin(void);
void cw__perf_mark(int phase);

#endif

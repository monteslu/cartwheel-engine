/* The SDL2 half of the platform layer, identical in native and cart builds:
 * window + GLES 3.0 context, event translation, game controllers, the raw
 * input snapshot, and the shared frame step (fixed ticks + update + render). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/gl.h"
#include "cartwheel/jobs.h"
#include "cartwheel/debug.h"
#include "cartwheel/core.h"
#include "hooks.h"
#include "render/quality_internal.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"
#if !CW_CART && defined(__APPLE__) && !defined(SDL_VIDEO_DRIVER_UIKIT)
#include <dlfcn.h>
#endif

cw_platform cw__plat;
/* set by the resource system when first used, so apps that never load a
 * resource do not link it (and its decoders) */
void (*cw__res_frame_hook)(void);
void (*cw__update_done_hook)(void);
void (*cw__audio_fence_hook)(bool on);
void (*cw__res_shutdown_hook)(void);

/* The input module (src/input) registers itself here the first time a game
 * defines an action, so a game that never uses actions links no input code. */
static cw__input_hooks input_hooks;
static cw_raw_input_filter raw_filter; /* cw_set_raw_input_filter */
static void *raw_filter_user;

/* ── loading progress and the warm-up (cw_app_desc.warmup) ─────────── */
static int32_t loading[3]; /* debug field cw.loading: done, total, stage */
static bool warmed;        /* the warm-up has finished (or there is none) */
static bool app_inited;    /* the app's init has run (cw__frame) */
/* one warm-up step per frame (the loading screen shows), or the whole
 * warm-up inside the first frame. Spread by default on every host, so the
 * loading screen is the first thing a player sees; a test does it all in the
 * first frame so its frames line up with a run without one: natively a test
 * run (CW_FRAMES; CW_WARMUP_SPREAD=1 spreads it too), in a cart a
 * deterministic run (WC_HOST_FLAG_DETERMINISTIC, romdev's test loads) or a
 * host that asks with the debug request `warmup_spread 0` before the first
 * frame (tools/run-cart.mjs does on Chromium and wasmcart-run, which have no
 * deterministic mode) */
static bool warmup_spread;
static void cmd_warmup_spread(int argc, const char **argv, cw_reply *r) {
  if (argc > 1) warmup_spread = atoi(argv[1]) != 0;
  cw_reply_printf(r, "{\"warmup_spread\":%s}", warmup_spread ? "true" : "false");
}
void cw_loading_set(int done, int total, cw_load_stage stage) {
  loading[0] = done < 0 ? 0 : done;
  loading[1] = total < 0 ? 0 : total;
  loading[2] = stage;
}
void cw_loading_get(int *done, int *total, cw_load_stage *stage) {
  if (done) *done = loading[0];
  if (total) *total = loading[1];
  if (stage) *stage = (cw_load_stage)loading[2];
}
bool cw_loading_active(void) { return cw__plat.app.warmup && !warmed; }
static const char *const LOAD_LABEL[CW_LOAD_STAGE_COUNT] = { "Loading", "Loading assets", "Making textures",
                                                             "Building meshes", "Compiling shaders", "Building the scene" };
/* the built-in loading screen: the stage and a thin bar, centred on a dark
 * page (the web export's panel looks the same, so the hand-off is seamless) */
static void loading_screen(void) {
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_rgba(0.063f, 0.075f, 0.102f, 1) });
  cw_vec2 cs = cw_r2d_canvas_size();
  float s = cs.y / 540.0f, bw = fminf(cs.x * 0.6f, 420 * s), x = (cs.x - bw) * 0.5f, y = cs.y * 0.5f;
  float frac = loading[1] > 0 ? (float)loading[0] / (float)loading[1] : 0;
  if (frac > 1) frac = 1;
  cw_r2d_rect(cw_rect_make(x, y, bw, 6 * s), cw_rgba(0.165f, 0.188f, 0.271f, 1));
  cw_r2d_rect(cw_rect_make(x, y, bw * frac, 6 * s), cw_rgba(1, 0.698f, 0.243f, 1));
  char label[64];
  int stage = loading[2] >= 0 && loading[2] < CW_LOAD_STAGE_COUNT ? loading[2] : 0;
  snprintf(label, sizeof label, "%s\xe2\x80\xa6 %d%%", LOAD_LABEL[stage], (int)(frac * 100));
  cw_r2d_text(cw_ui_font_sans(), label, cw_v2(x, y + 14 * s),
              &(cw_text_style){ .size = 16 * s, .color = cw_rgba(0.604f, 0.631f, 0.714f, 1) });
  cw_r2d_end();
}

/* cw_platform_touch_primary: a phone or tablet with no pad in use. Natively
 * Android / iOS say so; CW_TOUCH_PRIMARY=1|0 overrides (desktops default to
 * false: SDL2 cannot tell whether a mouse is attached). A cart has no way to
 * ask its host, so the host tells it through the debug channel
 * (`touch_primary 1`: the web export's player does when the browser's
 * pointer is coarse; run-cart --touch-primary for tests). */
static bool touch_primary_hint, pad_used;
bool cw_platform_touch_primary(void) { return touch_primary_hint && !pad_used; }
static void cmd_crisp_ui(int argc, const char **argv, cw_reply *r) {
  cw_platform *p = &cw__plat;
  if (argc > 1) cw_crisp_ui_set(atoi(argv[1]) != 0);
  cw_reply_printf(r, "{\"crisp_ui\":%s,\"ui_min_lines\":%d,\"pixel_scale\":%d,\"k\":%d,\"scene\":[%d,%d],\"view\":[%d,%d,%d,%d]}",
                  p->crisp_ui ? "true" : "false", p->ui_floor, p->pixel_n, p->k, p->scene_w, p->scene_h, p->view_x, p->view_y,
                  p->view_w, p->view_h);
}
static void cmd_touch_primary(int argc, const char **argv, cw_reply *r) {
  if (argc > 1) touch_primary_hint = atoi(argv[1]) != 0;
  cw_reply_printf(r, "{\"touch_primary\":%s,\"hint\":%s,\"pad_used\":%s}", cw_platform_touch_primary() ? "true" : "false",
                  touch_primary_hint ? "true" : "false", pad_used ? "true" : "false");
}
/* cw_platform_vibrate: the request count and its length, read by the host */
static uint32_t vibrate_req[2];
void cw_platform_vibrate(int ms) {
  vibrate_req[0]++;
  vibrate_req[1] = ms > 0 ? (uint32_t)ms : 0;
}
/* any button or stick on any pad ends touch-primary (a pad merely present,
 * as test hosts and some browsers report one, does not) */
static void note_pad_use(const cw_raw_input *in) {
  for (int i = 0; i < CW_MAX_PADS && !pad_used; i++) {
    const cw_pad_state *ps = &in->pads[i];
    if (!ps->connected) continue;
    if (ps->buttons || ps->pressed) pad_used = true;
    for (int a = 0; a < CW_AXIS_COUNT; a++)
      if (fabsf(ps->axes[a]) > 0.5f) pad_used = true;
  }
}

/* cw_app_desc.touch_pointer: the primary finger is the mouse's left button */
static int touch_primary = -1;
static void touch_pointer(cw_raw_input *in) {
  in->mouse_from_touch = false;
  if (!cw__plat.app.touch_pointer) return;
  if (touch_primary >= 0 && !in->touches[touch_primary].active) { /* lifted: release */
    in->mouse_buttons &= ~1u;
    touch_primary = -1;
  }
  int pick = touch_primary;
  if (pick < 0)
    for (int i = 0; i < CW_MAX_TOUCHES; i++)
      if (in->touches[i].tapped || in->touches[i].active) {
        pick = i;
        break;
      }
  if (pick < 0) return;
  const cw_touch_state *t = &in->touches[pick];
  bool fresh = pick != touch_primary;
  in->mouse_present = true;
  in->mouse_from_touch = true;
  in->mouse_x = t->x;
  in->mouse_y = t->y;
  if (fresh) in->mouse_pressed |= 1u;
  /* held while the finger is down; a finger that landed and lifted within one
   * frame holds for this frame and releases on the next: a click */
  in->mouse_buttons |= 1u;
  touch_primary = pick;
}

void cw__platform_hook_input(const cw__input_hooks *h) { input_hooks = *h; }

static cw__ui_hooks ui_hooks;
void cw__platform_hook_ui(const cw__ui_hooks *h) { ui_hooks = *h; }

static void emit(const cw_event *e) {
  if (ui_hooks.event) ui_hooks.event(e);
  if (cw__plat.app.event) cw__plat.app.event(cw__plat.app.user, e);
}

/* text queued outside the SDL event loop, emitted by the next poll */
static char pending_text[64][32];
static int pending_text_n;
void cw__platform_queue_text(const char *utf8, int len) {
  /* split into event-sized pieces on UTF-8 boundaries */
  while (len > 0 && pending_text_n < 64) {
    int n = len < 31 ? len : 31;
    while (n < len && n > 0 && ((unsigned char)utf8[n] & 0xc0) == 0x80) n--;
    memcpy(pending_text[pending_text_n], utf8, (size_t)n);
    pending_text[pending_text_n++][n] = 0;
    utf8 += n;
    len -= n;
  }
}

/* ── clipboard ──────────────────────────────────────────────────────── */
static char *private_clip;
static bool clip_private(void) {
#if CW_CART
  return false;
#else
  return getenv("CW_FRAMES") != NULL;
#endif
}
void cw_clipboard_set(const char *utf8) {
  if (!utf8) utf8 = "";
  if (clip_private()) {
    cw_free(private_clip);
    private_clip = cw_strdup(utf8, CW_MEM_GENERAL);
    return;
  }
  SDL_SetClipboardText(utf8);
}
const char *cw_clipboard_get(void) {
  if (clip_private()) {
    const char *s = private_clip ? private_clip : "";
    char *c = cw_frame_alloc(strlen(s) + 1);
    memcpy(c, s, strlen(s) + 1);
    return c;
  }
  char *t = SDL_GetClipboardText();
  const char *s = t ? t : "";
  char *c = cw_frame_alloc(strlen(s) + 1);
  memcpy(c, s, strlen(s) + 1);
  SDL_free(t);
  return c;
}

static int pad_slot_of(SDL_JoystickID id) {
  for (int i = 0; i < CW_MAX_PADS; i++)
    if (cw__plat.pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(cw__plat.pads[i])) == id) return i;
  return -1;
}

/* SDL reports every controller present at start twice: the device list read
 * after init, and an SDL_CONTROLLERDEVICEADDED event queued by its init. Opening
 * both gave one controller two slots (one pad was pad 0 and pad 1 at once, and
 * a third controller found the slots full), so a device already open is
 * skipped. */
static void open_pad(int device_index) {
  if (!SDL_IsGameController(device_index)) return;
  if (pad_slot_of(SDL_JoystickGetDeviceInstanceID(device_index)) >= 0) return;
  /* the slot is the controller's player index when SDL has one and it is free
   * (a wasmcart host's pad id, the XInput slot, the index SDL handed out at
   * connect), so pads that come back in another order keep their players;
   * otherwise the first free slot */
  int want = SDL_JoystickGetDevicePlayerIndex(device_index);
  for (int k = -1; k < CW_MAX_PADS; k++) {
    int i = k < 0 ? want : k;
    if (i < 0 || i >= CW_MAX_PADS || cw__plat.pads[i]) continue;
    cw__plat.pads[i] = SDL_GameControllerOpen(device_index);
    if (!cw__plat.pads[i]) { cw_warn("pad open: %s", SDL_GetError()); return; }
    cw_event e = { .type = CW_EVENT_PAD_ADDED, .pad = { i } };
    emit(&e);
    return;
  }
}

/* ── render scale (cw_app_desc.render_scale) ─────────────────────────── */

/* cw.view: logical w, h, then the picture's rect in the framebuffer: w, h, x, y
 * (pixel_scale centres it with bars when the output is not a multiple) */
static int32_t view_dbg[6];

void cw__resolve_scale(void) {
  cw_platform *p = &cw__plat;
  float s = p->app.render_scale;
#if !CW_CART
  const char *env = getenv("CW_RENDER_SCALE");
  if (env && *env) s = strtof(env, NULL);
#endif
  /* auto with a host preference (a cart, cw_app_desc.render_scale): the
   * largest scale whose frame fits the preferred size, so the host's 540, 720
   * or 1080 lines are the frame's lines for a game of the host's aspect, and a
   * game of another aspect fits inside it (16:9 in a 640x480 preference is
   * 640x360) and never declares a frame larger than the host asked for */
  bool fit = !(s > 0) && (p->host_pref_w > 0 || p->host_pref_h > 0);
  if (fit) {
    float sw = p->host_pref_w > 0 ? (float)p->host_pref_w / (float)p->app.width : 1e9f;
    float sh = p->host_pref_h > 0 ? (float)p->host_pref_h / (float)p->app.height : 1e9f;
    s = CW_MIN(sw, sh);
  }
  if (!(s > 0)) s = p->app.height < 1080 ? 1080.0f / (float)p->app.height : 1.0f;
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(SDL_VIDEO_DRIVER_UIKIT))
  s = 1; /* a phone's window is the screen: its density is the scale */
#endif
  if (s > 8) s = 8;
  p->scale = s;
  p->px_w = CW_MAX(1, (int)lroundf((float)p->app.width * s));
  p->px_h = CW_MAX(1, (int)lroundf((float)p->app.height * s));
  if (fit) { /* rounding never takes a side past the preference */
    if (p->host_pref_w > 0) p->px_w = CW_MIN(p->px_w, p->host_pref_w);
    if (p->host_pref_h > 0) p->px_h = CW_MIN(p->px_h, p->host_pref_h);
  }
  /* pixel_scale and crisp UI (cw_app_desc.pixel_scale, ui_min_lines) */
  int n = p->app.pixel_scale, floor_lines = p->app.ui_min_lines;
#if !CW_CART
  const char *pe = getenv("CW_PIXEL_SCALE"), *ue = getenv("CW_UI_MIN_LINES");
  if (pe && *pe) n = atoi(pe);
  if (ue && *ue) {
    floor_lines = atoi(ue);
    p->crisp_ui = floor_lines > 0;
  }
#endif
  p->pixel_n = CW_CLAMP(n, 1, 16);
  p->ui_floor = CW_CLAMP(floor_lines, 0, 4320);
  if (!p->ui_floor) p->crisp_ui = false;
  /* A cart whose host gave a size hands the host the low frame (the host
   * scales it: RetroArch's Integer Scale), made a whole multiple k of the
   * scene only to reach the crisp-UI floor; fixed for the run (a libretro
   * frontend sizes its video once). Everywhere else the output keeps its
   * size and the scene renders at 1/N of it (set_window_px). */
  p->pixel_host = fit && (p->pixel_n > 1 || p->ui_floor > 0);
  if (p->pixel_host) {
    int sw = CW_MAX(1, p->px_w / p->pixel_n), sh = CW_MAX(1, p->px_h / p->pixel_n);
    int k = p->ui_floor > 0 && sh < p->ui_floor ? (p->ui_floor + sh - 1) / sh : 1;
    p->k = k;
    p->px_w = sw * k;
    p->px_h = sh * k;
    p->scale = (float)p->px_h / (float)p->app.height;
  }
}

/* the scene and the picture for an output of w x h (cw_app_desc.pixel_scale) */
static void pixel_layout(int w, int h) {
  cw_platform *p = &cw__plat;
  if (p->pixel_host) { /* k fixed at resolve: the output is the picture */
    p->scene_w = CW_MAX(1, w / p->k);
    p->scene_h = CW_MAX(1, h / p->k);
  } else {
    p->k = p->pixel_n;
    p->scene_w = CW_MAX(1, w / p->k);
    p->scene_h = CW_MAX(1, h / p->k);
  }
  p->view_w = p->scene_w * p->k;
  p->view_h = p->scene_h * p->k;
  p->view_x = (w - p->view_w) / 2;
  p->view_y = (h - p->view_h) / 2;
}

/* the SDL window's size changed (or is first known): logical = pixels / scale */
static void set_window_px(int w, int h) {
  cw_platform *p = &cw__plat;
  p->px_w = w;
  p->px_h = h;
  pixel_layout(w, h);
  /* logical = the picture's pixels / scale (the whole output unless pixel_scale left bars) */
  p->win_w = CW_MAX(1, (int)lroundf((float)p->view_w / p->scale));
  p->win_h = CW_MAX(1, (int)lroundf((float)p->view_h / p->scale));
  view_dbg[0] = p->win_w;
  view_dbg[1] = p->win_h;
  view_dbg[2] = p->view_w;
  view_dbg[3] = p->view_h;
  view_dbg[4] = p->view_x;
  view_dbg[5] = p->view_y;
}

bool cw__sdl_init(void) {
  cw_platform *p = &cw__plat;
  if (!(p->scale > 0)) cw__resolve_scale();
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(SDL_VIDEO_DRIVER_UIKIT))
  touch_primary_hint = true;
#endif
#if !CW_CART
  if (getenv("CW_TOUCH_PRIMARY")) touch_primary_hint = atoi(getenv("CW_TOUCH_PRIMARY")) != 0;
#endif
  cw_debug_field("cw.loading", loading, CW_DBG_I32, 3);
  cw_debug_field("cw.view", view_dbg, CW_DBG_I32, 6);
  cw__quality_init(); /* cartwheel/quality.h: High and fixed in a deterministic run */
  cw_debug_command("warmup_spread", "warmup_spread [1|0]: one warm-up step per frame (cw_app_desc.warmup)", cmd_warmup_spread);
#if !CW_CART
  /* CW_WARMUP_SPREAD=1 or 0 decides; otherwise spread unless it is a test run.
   * A game the editor launches gets 0: the editor steps it tick by tick from
   * a paused start, so its first frames must be playable (src/remote/native.c) */
  const char *spread_env = getenv("CW_WARMUP_SPREAD");
  warmup_spread = spread_env && *spread_env ? atoi(spread_env) != 0 : !getenv("CW_FRAMES");
#else
  warmup_spread = !p->deterministic_run;
#endif
  cw_debug_command("crisp_ui", "crisp_ui [1|0]: the UI at the picture's resolution (cw_app_desc.ui_min_lines; a no-op unless the app offers it)", cmd_crisp_ui);
  cw_debug_command("touch_primary", "touch_primary [1|0]: the device is touch-first (cw_platform_touch_primary)", cmd_touch_primary);
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
#if !CW_CART && defined(__APPLE__)
#define CW_WGPU_WINDOW_FLAGS SDL_WINDOW_METAL
#else
#define CW_WGPU_WINDOW_FLAGS 0
#endif
  if (!wgpu) SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(SDL_VIDEO_DRIVER_UIKIT))
  /* a resizable window would make SDL request any orientation (overriding
   * the manifest / Info.plist): keep the game's own aspect */
  SDL_SetHint(SDL_HINT_ORIENTATIONS, p->app.width >= p->app.height ? "LandscapeLeft LandscapeRight" : "Portrait PortraitUpsideDown");
#endif
#if !CW_CART && !defined(_WIN32)
  /* A test run (CW_FRAMES) never opens a window on the desktop: SDL's offscreen
   * driver gives it a virtual window (GL: EGL surfaceless; WebGPU: no surface
   * at all, see gpu.c). Hundreds of short-lived X11 windows per suite went
   * through the user's Xwayland, which aborted once mid-suite (2026-10-06).
   * CW_TEST_WINDOW=1 or an explicit SDL_VIDEODRIVER keeps a real window. */
  if (getenv("CW_FRAMES") && !getenv("SDL_VIDEODRIVER") && !(getenv("CW_TEST_WINDOW") && atoi(getenv("CW_TEST_WINDOW"))))
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
#endif
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
    cw_error("SDL_Init: %s", SDL_GetError());
    return false;
  }
  bool no_msaa = false;
  for (;;) { /* another pass when WebGPU falls back to GL, or GL to no multisampling */
    if (!wgpu) {
#if !CW_CART && defined(__APPLE__) && !defined(SDL_VIDEO_DRIVER_UIKIT)
      /* macOS: GLES is ANGLE (Metal), linked through @rpath. SDL opens
       * libGLESv2 / libEGL by leaf name, which dyld does not look up through
       * the app's rpath: give it the files beside the libGLESv2 this process
       * linked (Contents/Frameworks in a bundle) */
      Dl_info di;
      if (!getenv("SDL_VIDEO_EGL_DRIVER") && dladdr((const void *)glGetString, &di) && di.dli_fname) {
        char dir[1024], path[1100];
        snprintf(dir, sizeof dir, "%s", di.dli_fname);
        char *slash = strrchr(dir, '/');
        if (slash) {
          *slash = 0;
          snprintf(path, sizeof path, "%s/libGLESv2.dylib", dir);
          setenv("SDL_VIDEO_GL_DRIVER", path, 0);
          snprintf(path, sizeof path, "%s/libEGL.dylib", dir);
          setenv("SDL_VIDEO_EGL_DRIVER", path, 0);
        }
      }
#endif
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
      SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
      if (p->app.msaa > 0 && !no_msaa) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, p->app.msaa);
      }
    }
    /* WebGPU: no GL context (a dual cart's host traps every gl import); on
     * macOS a Metal view for Dawn's surface */
    Uint32 wflags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE | (wgpu ? CW_WGPU_WINDOW_FLAGS : SDL_WINDOW_OPENGL);
#if !CW_CART
    { /* a desktop window larger than the display: lower the scale to fit (the
       * logical size stays), never below 1 */
      const char *drv = SDL_GetCurrentVideoDriver();
      SDL_Rect r;
      if (p->scale > 1 && drv && strcmp(drv, "offscreen") != 0 && SDL_GetDisplayUsableBounds(0, &r) == 0 && r.w > 0 && r.h > 0) {
        float f = CW_MIN((float)r.w / (float)p->px_w, (float)(r.h - 40) / (float)p->px_h); /* room for a title bar */
        if (f < 1) {
          p->scale = CW_MAX(1.0f, p->scale * f);
          p->px_w = (int)lroundf((float)p->app.width * p->scale);
          p->px_h = (int)lroundf((float)p->app.height * p->scale);
        }
      }
    }
#endif
    p->window = SDL_CreateWindow(p->app.title ? p->app.title : "cartwheel", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, p->px_w, p->px_h, wflags);
    if (!p->window && !wgpu && p->app.msaa > 0 && !no_msaa) {
      /* a multisampled window surface the GLES driver cannot make (ANGLE
       * on some Macs): the same window without multisampling */
      cw_warn("SDL_CreateWindow with %dx MSAA: %s; trying without multisampling", p->app.msaa, SDL_GetError());
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
      no_msaa = true;
      continue;
    }
    if (!p->window) {
      cw_error("SDL_CreateWindow: %s", SDL_GetError());
      return false;
    }
    if (wgpu) {
      if (cw__wgpu_init()) break;
      /* WebGPU the run picked itself and could not start (no adapter, no
       * surface): a new window, on GL */
      if (!cw__gpu_fallback_to_gl()) return false;
      SDL_DestroyWindow(p->window);
      p->window = NULL;
      wgpu = false;
      SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
      continue;
    } else {
      p->gl = SDL_GL_CreateContext(p->window);
      if (!p->gl) {
        cw_error("SDL_GL_CreateContext (GLES 3.0): %s", SDL_GetError());
        return false;
      }
      SDL_GL_MakeCurrent(p->window, p->gl);
      SDL_GL_SetSwapInterval(p->app.vsync_off ? 0 : 1);
      cw_log(CW_LOG_INFO, "GL: %s | %s | %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
             (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    }
    break;
  }
  {
    int w, h;
    SDL_GetWindowSize(p->window, &w, &h);
    set_window_px(w, h);
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
  if (p->app.debug || cw__debug_remote_requested()) { /* the platform's own state, readable by name */
    cw__debug_channel_init();
    cw_debug_field("cw.frame", &p->frame, CW_DBG_U32, 1);
    cw_debug_field("cw.tick", &p->tick, CW_DBG_U32, 1);
    /* every pad the platform delivers (cw_pad_state, after the input script) */
    static const char *const names[CW_MAX_PADS][3] = {
      { "cw.pad0.buttons", "cw.pad0.axes", "cw.pad0.connected" }, { "cw.pad1.buttons", "cw.pad1.axes", "cw.pad1.connected" },
      { "cw.pad2.buttons", "cw.pad2.axes", "cw.pad2.connected" }, { "cw.pad3.buttons", "cw.pad3.axes", "cw.pad3.connected" } };
    for (int i = 0; i < CW_MAX_PADS; i++) {
      cw_debug_u32(names[i][0], &p->input.pads[i].buttons);
      cw_debug_f32_array(names[i][1], p->input.pads[i].axes, CW_AXIS_COUNT);
      cw_debug_field(names[i][2], &p->input.pads[i].connected, CW_DBG_U8, 1);
    }
    cw_debug_field("cw.vibrate", vibrate_req, CW_DBG_U32, 2);
    cw__gpu_debug_register();
  }
  return true;
}

static int mouse_button_index(Uint8 b) {
  return b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : -1;
}

void cw__sdl_poll(void) {
  cw_platform *p = &cw__plat;
  cw_raw_input *in = &p->input;
  in->wheel_x = in->wheel_y = 0;
  /* "went down since the last frame" bits: a press released inside this
   * event batch still reaches the input module */
  memset(in->keys_pressed, 0, sizeof in->keys_pressed);
  in->mouse_pressed = 0;
  for (int i = 0; i < CW_MAX_TOUCHES; i++) in->touches[i].tapped = false;
  uint32_t pad_pressed[CW_MAX_PADS] = { 0 };
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    cw_event e = { 0 };
    switch (ev.type) {
    case SDL_QUIT:
      p->quit = true;
      e.type = CW_EVENT_QUIT;
      break;
    case SDL_WINDOWEVENT:
      if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        set_window_px(ev.window.data1, ev.window.data2);
        e.type = CW_EVENT_RESIZE;
        e.size.w = p->win_w;
        e.size.h = p->win_h;
      } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        e.type = CW_EVENT_FOCUS;
        e.focus.gained = ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
      }
      break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      e.type = ev.type == SDL_KEYDOWN ? CW_EVENT_KEY_DOWN : CW_EVENT_KEY_UP;
      e.key.scancode = ev.key.keysym.scancode;
      e.key.repeat = ev.key.repeat != 0;
      if (e.key.scancode >= 0 && e.key.scancode < CW_KEY_COUNT) {
        in->keys[e.key.scancode] = ev.type == SDL_KEYDOWN;
        if (ev.type == SDL_KEYDOWN && !e.key.repeat) in->keys_pressed[e.key.scancode] = 1;
      }
      break;
    case SDL_TEXTINPUT:
      e.type = CW_EVENT_TEXT;
      SDL_strlcpy(e.text.utf8, ev.text.text, sizeof e.text.utf8);
      break;
    case SDL_MOUSEMOTION:
      if (ev.motion.which == SDL_TOUCH_MOUSEID) break;
      in->mouse_present = true;
      in->mouse_x = (float)(ev.motion.x - p->view_x) / p->scale; /* window pixels -> logical (the picture's origin) */
      in->mouse_y = (float)(ev.motion.y - p->view_y) / p->scale;
      e.type = CW_EVENT_MOUSE_MOVE;
      e.mouse.x = in->mouse_x;
      e.mouse.y = in->mouse_y;
      break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
      if (ev.button.which == SDL_TOUCH_MOUSEID) break;
      int b = mouse_button_index(ev.button.button);
      if (b < 0) break;
      in->mouse_present = true;
      if (ev.type == SDL_MOUSEBUTTONDOWN) {
        in->mouse_buttons |= 1u << b;
        in->mouse_pressed |= 1u << b;
      } else {
        in->mouse_buttons &= ~(1u << b);
      }
      e.type = ev.type == SDL_MOUSEBUTTONDOWN ? CW_EVENT_MOUSE_DOWN : CW_EVENT_MOUSE_UP;
      e.mouse.button = b;
      e.mouse.x = (float)(ev.button.x - p->view_x) / p->scale;
      e.mouse.y = (float)(ev.button.y - p->view_y) / p->scale;
      break;
    }
    case SDL_MOUSEWHEEL:
      in->wheel_x += ev.wheel.preciseX;
      in->wheel_y += ev.wheel.preciseY;
      e.type = CW_EVENT_WHEEL;
      e.wheel.x = ev.wheel.preciseX;
      e.wheel.y = ev.wheel.preciseY;
      break;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP: {
      int slot = (int)(ev.tfinger.fingerId % CW_MAX_TOUCHES);
      if (slot < 0) slot = -slot;
      in->touches[slot].active = ev.type != SDL_FINGERUP;
      if (ev.type == SDL_FINGERDOWN) in->touches[slot].tapped = true;
      /* SDL normalizes by (size - 1) (X11, wasmcart-sdl2); snapping to 1/8
       * window pixel removes the float round trip so every host reports the
       * same point, then window pixels -> logical */
      in->touches[slot].x = (roundf(ev.tfinger.x * (float)(p->px_w - 1) * 8.0f) / 8.0f - (float)p->view_x) / p->scale;
      in->touches[slot].y = (roundf(ev.tfinger.y * (float)(p->px_h - 1) * 8.0f) / 8.0f - (float)p->view_y) / p->scale;
      break;
    }
    case SDL_CONTROLLERBUTTONDOWN: {
      int slot = pad_slot_of(ev.cbutton.which);
      if (slot >= 0 && ev.cbutton.button < CW_PAD_BUTTON_COUNT) pad_pressed[slot] |= 1u << ev.cbutton.button;
      break;
    }
    case SDL_CONTROLLERDEVICEADDED:
      open_pad(ev.cdevice.which);
      break;
    case SDL_CONTROLLERDEVICEREMOVED: {
      int slot = pad_slot_of(ev.cdevice.which);
      if (slot >= 0) {
        SDL_GameControllerClose(p->pads[slot]);
        p->pads[slot] = NULL;
        e.type = CW_EVENT_PAD_REMOVED;
        e.pad.index = slot;
      }
      break;
    }
    default:
      break;
    }
    if (e.type != CW_EVENT_NONE) emit(&e);
  }
  for (int i = 0; i < CW_MAX_PADS; i++) {
    cw_pad_state *ps = &in->pads[i];
    SDL_GameController *gc = p->pads[i];
    memset(ps, 0, sizeof *ps);
    if (!gc) continue;
    ps->connected = true;
    ps->pressed = pad_pressed[i];
    for (int b = 0; b < CW_PAD_BUTTON_COUNT; b++)
      if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) ps->buttons |= 1u << b;
    for (int a = 0; a < CW_AXIS_COUNT; a++) {
      float v = SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)a) / 32767.0f;
      ps->axes[a] = v < -1 ? -1 : v;
    }
  }
#if !CW_CART
  cw__input_script_apply(in, p->frame);
#endif
  /* text that arrived outside SDL (cart wc_on_text, the native input script) */
  for (int i = 0; i < pending_text_n; i++) {
    cw_event e = { .type = CW_EVENT_TEXT };
    SDL_strlcpy(e.text.utf8, pending_text[i], sizeof e.text.utf8);
    emit(&e);
  }
  pending_text_n = 0;
  note_pad_use(in);
  touch_pointer(in);
  if (raw_filter) raw_filter(in, p->frame, raw_filter_user);
}

void cw_set_raw_input_filter(cw_raw_input_filter fn, void *user) {
  raw_filter = fn;
  raw_filter_user = user;
}

/* the debugger's pause: steps left, and the wall time spent paused (taken
 * out of cw_time() so the game's clock stops with its simulation) */
static struct { bool paused; uint32_t steps; double held; } dbg;

void cw__frame(double now) {
  cw_platform *p = &cw__plat;
  double raw = p->frame == 0 ? p->fixed_dt : now - p->last;
  double dt = raw;
  if (dt < 0) dt = 0;
  if (dt > 0.25) dt = 0.25; /* a stall (debugger, tab switch) is not simulated */
  /* A frame that lasted one fixed step arrives with rounding noise that
   * depends on the host's clock (native: frame / hz, a cart: the host's
   * milliseconds / 1000), so the same run summed different dts per host and
   * time-based state (a caption cue starting at 0.5 s) flipped a frame
   * between hosts. Snap it: every host passes the same dt bit for bit. */
  if (fabs(dt - p->fixed_dt) < p->fixed_dt * 1e-6) dt = p->fixed_dt;
  p->last = now;
  p->now = now - dbg.held;
  cw__perf_frame_begin();
  cw__pixel_frame_begin(); /* pixel_scale / crisp UI: the scene's target (pixel.c) */
  cw_frame_arena_begin(p->frame);
  if (cw__res_frame_hook) cw__res_frame_hook(); /* deferred frees, async loads finishing under the budget */
  cw__debug_channel_poll();
  if (!app_inited) {
    /* The app's init runs here, in the first frame, not at startup: with a
     * spread warm-up the first frame is only the loading screen (no app work,
     * no game shaders), so a player sees something as soon as the platform is
     * up, and the init runs in the next frame under that screen. A test (no
     * spread) runs it in this frame, before the warm-up and the first tick, as
     * a run without a warm-up does: the same order either way. Requests that
     * arrived in this frame (warmup_spread 0) are answered first; one naming
     * a command the app registers in its init is held until the init has run
     * (cw__debug_channel_app_ready), never answered as unknown. */
    if (p->app.warmup && warmup_spread && p->frame == 0) { /* events wait for the next frame's poll */
      loading_screen();
      cw__host_debug_update();
      p->frame++;
      cw__pixel_frame_end();
      cw__debug_frame_end();
      return;
    }
    app_inited = true;
    if (p->app.init) p->app.init(p->app.user);
    cw__debug_channel_app_ready();
  }
  if (dbg.paused) {
    /* a step frame runs exactly one tick and an update of one fixed dt, as an
     * unpaused 1/60 s frame runs them; a held frame runs neither */
    bool step = dbg.steps > 0;
    dbg.held += step ? raw - p->fixed_dt : raw;
    p->now = now - dbg.held;
    cw__sdl_poll();
    if (!step) {
      if (p->app.render) p->app.render(p->app.user, p->accumulator / p->fixed_dt);
      p->frame++;
      cw__pixel_frame_end();
      cw__debug_frame_end();
      return;
    }
    dbg.steps--;
    dt = p->fixed_dt;
    p->accumulator = 0;
  } else {
    p->now = now - dbg.held;
    cw__sdl_poll();
  }
  if (input_hooks.frame_begin) input_hooks.frame_begin(&p->input);
  if (p->app.warmup && !warmed) {
    /* one step per frame, the loading screen between them; a test does it
     * all in this frame (the same work, and frames line up with no warm-up) */
    do warmed = p->app.warmup(p->app.user);
    while (!warmed && !warmup_spread);
    if (!warmed) {
      /* the shader stage renders the app under the loading screen: its
       * first renders build the programs the scene needs (a scene app
       * renders a slice of the scene per frame, src/scene/runner.c) */
      if (loading[2] == CW_LOAD_SHADERS && p->app.render) p->app.render(p->app.user, 0);
      loading_screen();
      cw__host_debug_update();
      p->frame++;
      cw__pixel_frame_end();
      cw__debug_frame_end();
      return;
    }
    if (loading[1] > 0) loading[0] = loading[1];
    p->accumulator = 0; /* the warm-up's frames are not game time */
    dt = p->fixed_dt;
  }
  if (ui_hooks.frame_begin) ui_hooks.frame_begin(dt);
  cw__perf_mark(CW_PERF_INPUT);
  int max_sub = p->app.max_substeps > 0 ? p->app.max_substeps : 5;
  p->accumulator += dt;
  int steps = 0;
  /* hosts hand over frame times as doubles in ms; k*16.666..ms differences can
   * land a hair under fixed_dt, so a tolerance keeps exactly one tick per
   * 1/60 s frame on every host (native, romdev, browser) */
  const double eps = 1e-6;
  while (p->accumulator >= p->fixed_dt - eps && steps < max_sub) {
    if (input_hooks.tick_begin) input_hooks.tick_begin(); /* the per-tick action snapshot */
    if (p->app.fixed_update) p->app.fixed_update(p->app.user, p->fixed_dt);
    p->accumulator -= p->fixed_dt;
    p->tick++;
    steps++;
  }
  /* behind by more than the cap: drop the backlog instead of spiralling */
  if (p->accumulator >= p->fixed_dt - eps) p->accumulator = 0;
  if (p->accumulator < 0) p->accumulator = 0;
  if (input_hooks.ticks_end) input_hooks.ticks_end(); /* queries read the frame snapshot again */
  cw__perf_mark(CW_PERF_SIM);
  if (p->app.update) p->app.update(p->app.user, dt);
  cw__perf_mark(CW_PERF_UPDATE);
  if (cw__update_done_hook) cw__update_done_hook();
  if (p->app.render) p->app.render(p->app.user, p->accumulator / p->fixed_dt);
  cw__perf_mark(CW_PERF_RENDER);
  cw__host_debug_update();
  p->frame++;
  cw__pixel_frame_end();
  cw__debug_frame_end();
}

/* ── the debugger's pause (cartwheel/debug.h) ──────────────────────── */
void cw_debug_pause(bool paused) {
  dbg.paused = paused;
  if (!paused) dbg.steps = 0;
}
bool cw_debug_paused(void) { return dbg.paused; }
void cw_debug_step(uint32_t ticks) {
  dbg.paused = true;
  dbg.steps += ticks;
}
uint32_t cw_debug_steps_left(void) { return dbg.steps; }

void cw__shutdown(void) {
  cw_platform *p = &cw__plat;
  if (p->app.shutdown && app_inited) p->app.shutdown(p->app.user);
  if (cw__res_shutdown_hook) cw__res_shutdown_hook(); /* while the GL context is still current */
  cw_audio_close();
  cw_jobs_shutdown();
  cw__pixel_shutdown();
  cw__wgpu_shutdown();
  for (int i = 0; i < CW_MAX_PADS; i++)
    if (p->pads[i]) SDL_GameControllerClose(p->pads[i]);
  if (p->gl) SDL_GL_DeleteContext(p->gl);
  if (p->window) SDL_DestroyWindow(p->window);
  SDL_Quit();
}

/* ── queries ───────────────────────────────────────────────────────── */

const cw_raw_input *cw_input_raw(void) { return &cw__plat.input; }
void cw_window_size(int *w, int *h) {
  if (w) *w = cw__plat.win_w;
  if (h) *h = cw__plat.win_h;
}
void cw_drawable_size(int *w, int *h) {
  cw_platform *p = &cw__plat;
  int t = cw__pixel_target();
  if (t == 1) { /* the scene's target */
    *w = p->scene_w;
    *h = p->scene_h;
  } else if (t == 2) { /* the UI's: the picture's size */
    *w = p->view_w;
    *h = p->view_h;
  } else if (cw_gpu_backend() == CW_GPU_WEBGPU) cw__wgpu_drawable_size(w, h);
  else SDL_GL_GetDrawableSize(p->window, w, h);
}
void cw_render_size(int *w, int *h) {
  cw_platform *p = &cw__plat;
  if (p->k > 1) {
    if (w) *w = p->scene_w;
    if (h) *h = p->scene_h;
    return;
  }
  int dw = 0, dh = 0;
  if (p->window) {
    if (cw_gpu_backend() == CW_GPU_WEBGPU) cw__wgpu_drawable_size(&dw, &dh);
    else SDL_GL_GetDrawableSize(p->window, &dw, &dh);
  }
  if (w) *w = dw;
  if (h) *h = dh;
}
void cw_crisp_ui_set(bool on) {
  cw_platform *p = &cw__plat;
  if (!p->ui_floor) return;
  p->crisp_ui = on;
  p->crisp_ui_chosen = true;
}
bool cw_crisp_ui(void) { return cw__plat.crisp_ui; }
bool cw_crisp_ui_offered(void) { return cw__plat.ui_floor > 0; }
uint8_t cw_crisp_ui_saved(void) { return cw__plat.crisp_ui_chosen ? (uint8_t)(cw__plat.crisp_ui ? 2 : 1) : 0; }
void cw_crisp_ui_restore(uint8_t saved) {
  if (saved == 1 || saved == 2) cw_crisp_ui_set(saved == 2);
}
float cw_pixel_ratio(void) {
  int dw = 0, dh = 0;
  cw_drawable_size(&dw, &dh);
  return cw__plat.win_w > 0 && dw > 0 ? (float)dw / (float)cw__plat.win_w : 1.0f;
}
double cw_time(void) { return cw__plat.now; }
uint64_t cw_frame_index(void) { return cw__plat.frame; }
uint64_t cw_tick_index(void) { return cw__plat.tick; }
double cw_fixed_dt(void) { return cw__plat.fixed_dt; }
bool cw_is_deterministic_run(void) { return cw__plat.deterministic_run; }
uint32_t cw_host_seed(void) { return cw__plat.host_seed; }
void cw_quit(void) { cw__plat.quit = true; }
void cw_set_title(const char *title) {
  if (cw__plat.window) SDL_SetWindowTitle(cw__plat.window, title);
}

/* declared in platform.h since the start; both loop owners provide
 * cw_asset_size/cw_asset_read, so the whole-file helper lives here */
void *cw_asset_load(const char *path, size_t *len) {
  long n = cw_asset_size(path);
  if (n < 0) return NULL;
  char *buf = malloc((size_t)n + 1);
  if (!buf) return NULL;
  long got = n ? cw_asset_read(path, buf, (size_t)n) : 0;
  if (got != n) {
    free(buf);
    return NULL;
  }
  buf[n] = 0;
  if (len) *len = (size_t)n;
  return buf;
}

void cw__platform_system_locale(char *out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  SDL_Locale *ls = SDL_GetPreferredLocales();
  if (ls && ls[0].language)
    snprintf(out, cap, "%s%s%s", ls[0].language, ls[0].country ? "-" : "", ls[0].country ? ls[0].country : "");
  if (ls) SDL_free(ls);
}

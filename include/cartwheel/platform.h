/* cartwheel platform layer: the app loop, window + GL context, raw input,
 * time, audio output, assets.
 *
 * One SDL2 code path serves both builds: native links real SDL2, carts link
 * wasmcart-sdl2 (SDL2 on the wasmcart ABI). The only differences live in
 * src/platform/native.c and src/platform/cart.c: who owns the loop, where time
 * comes from, how audio reaches the device and how assets are read.
 *
 * The engine owns the frame. A game never writes a main loop: it returns a
 * cw_app_desc from cw_main() and is called back. Natively cw_main runs from
 * main(); in a cart from wc_init(), and each wc_render() is one frame. */
#ifndef CW_PLATFORM_H
#define CW_PLATFORM_H

#include "base.h"
#include "gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── raw input (one snapshot per rendered frame) ────────────────────────
 * Games normally read actions (cartwheel/input.h), not this. */

#define CW_MAX_PADS 4
#define CW_MAX_TOUCHES 10
#define CW_KEY_COUNT 512 /* SDL scancodes (USB HID usage ids) */

/* Pad buttons, in SDL_GameControllerButton order. */
typedef enum {
  CW_PAD_SOUTH, CW_PAD_EAST, CW_PAD_WEST, CW_PAD_NORTH, /* A B X Y on an Xbox pad */
  CW_PAD_BACK, CW_PAD_GUIDE, CW_PAD_START,
  CW_PAD_LSTICK, CW_PAD_RSTICK, CW_PAD_LSHOULDER, CW_PAD_RSHOULDER,
  CW_PAD_UP, CW_PAD_DOWN, CW_PAD_LEFT, CW_PAD_RIGHT,
  CW_PAD_BUTTON_COUNT
} cw_pad_button;

/* Pad axes, in SDL_GameControllerAxis order. Sticks -1..1 (y down positive,
 * as SDL reports), triggers 0..1. */
typedef enum { CW_AXIS_LX, CW_AXIS_LY, CW_AXIS_RX, CW_AXIS_RY, CW_AXIS_LT, CW_AXIS_RT, CW_AXIS_COUNT } cw_pad_axis;

typedef struct cw_pad_state {
  bool connected;
  uint32_t buttons; /* bit i = cw_pad_button i held */
  uint32_t pressed; /* bit i = went down at least once since the last frame (a tap released
                     * within one frame shows here and not in buttons) */
  float axes[CW_AXIS_COUNT];
} cw_pad_state;

typedef struct cw_touch_state {
  bool active;
  bool tapped; /* touched down since the last frame (even if already lifted) */
  float x, y;  /* logical window coordinates, snapped to 1/8 px */
} cw_touch_state;

typedef struct cw_raw_input {
  uint8_t keys[CW_KEY_COUNT]; /* 1 = held, indexed by SDL scancode */
  uint8_t keys_pressed[CW_KEY_COUNT]; /* 1 = went down since the last frame (not repeats) */
  float mouse_x, mouse_y;     /* logical window coordinates */
  uint32_t mouse_buttons;     /* bit0 left, bit1 right, bit2 middle */
  uint32_t mouse_pressed;     /* buttons that went down since the last frame */
  bool mouse_present;
  bool mouse_from_touch;      /* this frame's left button / position came from a finger (cw_app_desc.touch_pointer) */
  float wheel_x, wheel_y;     /* notches this frame, +y = up */
  cw_touch_state touches[CW_MAX_TOUCHES];
  cw_pad_state pads[CW_MAX_PADS];
} cw_raw_input;

/* ── events (forwarded raw; the input module builds actions on top) ───── */

typedef enum {
  CW_EVENT_NONE,
  CW_EVENT_QUIT,
  CW_EVENT_RESIZE,     /* size.w/h: new logical size */
  CW_EVENT_KEY_DOWN,   /* key.scancode, key.repeat */
  CW_EVENT_KEY_UP,
  CW_EVENT_TEXT,       /* text.utf8 */
  CW_EVENT_MOUSE_DOWN, /* mouse.button (0 left, 1 right, 2 middle), mouse.x/y */
  CW_EVENT_MOUSE_UP,
  CW_EVENT_MOUSE_MOVE,
  CW_EVENT_WHEEL,      /* wheel.x/y in notches */
  CW_EVENT_PAD_ADDED,  /* pad.index */
  CW_EVENT_PAD_REMOVED,
  CW_EVENT_FOCUS,      /* focus.gained */
} cw_event_type;

typedef struct cw_event {
  cw_event_type type;
  union {
    struct { int w, h; } size;
    struct { int scancode; bool repeat; } key;
    struct { char utf8[32]; } text;
    struct { int button; float x, y; } mouse;
    struct { float x, y; } wheel;
    struct { int index; } pad;
    struct { bool gained; } focus;
  };
} cw_event;

/* ── the app ────────────────────────────────────────────────────────── */

/* How the logical size meets a screen of another aspect (cw_app_desc.fit). */
typedef enum {
  CW_FIT_LETTERBOX = 0, /* the logical size is fixed; a screen of another aspect shows bars */
  CW_FIT_HEIGHT,        /* Hor+: the height is the design height, the width follows the screen's aspect */
  CW_FIT_WIDTH,         /* Vert+: the width is the design width, the height follows the screen's aspect */
} cw_fit_mode;

typedef struct cw_app_desc {
  const char *title;
  int width, height;       /* logical size: the coordinate space of input, the 2D canvas and UI */
  /* How that size meets a screen of another aspect: a host's preferred size
   * (RetroArch's Internal Resolution, a handheld's 640x480 or 1280x800
   * panel; CW_HOST_PREF=WxH pretends to be one natively).
   * LETTERBOX (the default): width x height is the logical size everywhere
   * and the frame is the largest of that aspect that fits, so a 16:9 game
   * on a 4:3 handheld shows bars.
   * FIT_HEIGHT: the height stays the design height and the logical width
   * follows the screen's aspect (Hor+): a 960x540 game is 720x540 on 4:3,
   * 864x540 on 16:10, 960x540 on 16:9 and 1280x540 on 21:9. The 3D camera
   * sees more or less to the sides at the same vertical field of view, a
   * plain 2D canvas is the wider or narrower logical size, UI anchored to
   * the edges stays at the edges, and there are no bars for any aspect
   * between aspect_min and aspect_max (0 = 4:3 and 2.4, the 21:9 screens);
   * beyond them the width is clamped and the host shows bars.
   * FIT_WIDTH is the same with the axes swapped (Vert+): a vertical shooter
   * keeps its width and gains lines on a 4:3 screen.
   * The size the run settled on is cw_window_size (ask every frame: a
   * native window also resizes); the design size is cw_design_size, so a
   * HUD scales by height / design height without a literal; and
   * cw_fit_logical_size answers the size for any screen without running,
   * for tests and tools. */
  cw_fit_mode fit;
  float aspect_min, aspect_max;
  /* Framebuffer pixels per logical unit. The 3D scene, post and the 2D
   * batcher all render at the framebuffer's resolution while game code keeps
   * its logical coordinates (cw_window_size, cw_r2d_canvas_size, pointer
   * positions): text and shapes rasterize at full resolution.
   *
   * 0 = auto. A cart follows the host's preferred size (wc_host_info_t
   * preferred_width/height, e.g. RetroArch's Internal Resolution): the
   * largest scale whose frame fits inside it, so a 960x540 game renders
   * 960x540, 1280x720, 1920x1080 or 3840x2160 for a 540p, 720p, 1080p or 4K
   * preference, and fits a preference of another aspect (640x480 gives
   * 640x360). With no preference (0), natively, and in a deterministic run
   * (a test's pixels never depend on the host) auto outputs 1080 lines:
   * 1080 / height when height < 1080, else 1.
   *
   * > 0 is fixed and wins over a host preference too: an app that sets it
   * depends on the pixel count (a benchmark timing a size, a test reading
   * back W x H, the editor), so the host's preference never clamps it; the
   * host scales the frame to its screen as it does any cart's.
   *
   * A cart declares the scaled size as its framebuffer (wc_info
   * width/height, settled in wc_init once the host's preference is known).
   * Natively the window opens at the scaled size (clamped to the display),
   * and CW_RENDER_SCALE in the environment wins. Render-only: the simulation
   * never sees it. */
  float render_scale;
  /* Integer pixel scale, for a chunky pixel look: the frame renders at 1/N
   * of the output and is scaled up N times with nearest-neighbour, so pixels
   * stay square and crisp. 0 or 1 = off; N >= 2. The whole frame is
   * pixelated (3D, post and the 2D window passes) unless crisp UI is on
   * (ui_min_lines). Where the output size is fixed (a native window, a cart
   * the host shows at its own size: the browser, wasmcart-native without a
   * preference) the output keeps its size, the frame renders at
   * floor(output / N) and the picture is centred with black bars when the
   * output is not a multiple of N. A cart whose host states a preferred size
   * (RetroArch's Internal Resolution) instead hands the host the low frame
   * itself (the preference's frame / N) and leaves the scaling to the host
   * (RetroArch: Integer Scale on, bilinear filtering off), so it is never
   * scaled twice. Logical coordinates, pointer positions and the simulation
   * never see it. CW_PIXEL_SCALE in the environment wins (native). */
  int pixel_scale;
  /* Crisp UI: the scene (3D, post, the game's world) renders at the render
   * resolution while the 2D window passes (HUD, menus, text) render at a
   * resolution of at least this many lines, so text stays readable at a low
   * internal resolution (270p, 360p) or under pixel_scale. 0 = the app offers
   * none (the default). An app that sets it offers the choice; it starts off
   * (UI pixelated with the scene) and cw_crisp_ui_set turns it on, from a
   * graphics menu for instance (keep it in the save with
   * cw_crisp_ui_saved / cw_crisp_ui_restore).
   * When the render height is below it, the frame is the smallest whole
   * multiple k of the render size that reaches it: the scene is scaled up k
   * times with nearest-neighbour (still chunky) and the UI draws on top at
   * the frame's resolution (a cart at 270p offering 540 hands its host a
   * 540-line frame, whether crisp UI is on or off, so the size never changes
   * while it runs). Under pixel_scale the UI draws at the upscaled picture's
   * resolution. "The UI" is every 2D window pass from the first one of a
   * frame on: draw the 3D scene first. CW_UI_MIN_LINES=N in the environment
   * offers N lines and turns crisp UI on (0 = off; native). */
  int ui_min_lines;
  int fixed_hz;            /* simulation ticks per second; 0 = 60 */
  int max_substeps;        /* catch-up cap per rendered frame; 0 = 5 */
  int msaa;                /* default framebuffer samples (native); 0 = none */
  bool vsync_off;          /* native only */
  bool deterministic;      /* cart: declare WC_FLAG_DETERMINISTIC */
  bool debug;              /* cart: export the debug channel (wc_debug_state) */
  bool net;                /* cart: declare WC_FLAG_NET_PEER (wasmcart peer connections, cartwheel/net.h) */
  int workers;             /* job threads besides main: 0 = auto, <0 = none */
  cw_gpu_api gpu;          /* graphics API (cartwheel/gpu.h): AUTO: see cw_gpu_api; CW_GPU in the environment wins (native) */
  /* The primary touch drives the mouse: the first finger down becomes the
   * left button at its position (press, drag, release) and stays the
   * pointer until it lifts; other fingers stay in touches[] (pinch). A tap
   * that lands and lifts within one frame is a click (held for that frame,
   * released the next). For games that read
   * the mouse; same on every host (native SDL touch, carts' pointer slots
   * 1-9, the CW_INPUT script), done here after the devices are read. */
  bool touch_pointer;
  /* Warm-up: work done before the first playable frame (decoding assets,
   * generating textures, building meshes, compiling shaders) in small steps
   * across frames instead of one long blocking first frame. Called once per
   * frame, before any tick, until it returns true; report progress with
   * cw_loading_set. Meanwhile the engine draws a minimal loading screen (a
   * bar and the stage) and runs no ticks. A deterministic run (a test) calls
   * it until done inside the first frame, so frame counts and input timing
   * are the same as without it. NULL = none. */
  bool (*warmup)(void *user);
  /* Save data: a buffer of the app's that persists between runs (best
   * scores, settings, progress). Its contents are restored before init and
   * kept by the host: a cart's region is the wasmcart save region (the host
   * restores it at load and writes it back); natively it is a file (CW_SAVE_FILE,
   * else save.dat in SDL's per-user preference directory for the title),
   * written at exit and by cw_save_commit. A deterministic run (a test:
   * CW_FRAMES natively, the host's deterministic mode in a cart) starts from
   * zeros and never writes, so tests do not depend on earlier runs. */
  void *save;
  size_t save_size;
  void (*init)(void *user);
  void (*fixed_update)(void *user, double dt); /* simulation at fixed dt */
  void (*update)(void *user, double dt);       /* once per rendered frame */
  void (*render)(void *user, double alpha);    /* alpha: interpolation 0..1 */
  void (*shutdown)(void *user);
  void (*event)(void *user, const cw_event *e); /* optional */
  void *user;
} cw_app_desc;

/* Provided by the game (or by the engine's scene runner). */
cw_app_desc cw_main(int argc, char **argv);

/* ── raw input filter ─────────────────────────────────────────────────
 * Rewrite the raw snapshot after the platform polled the devices (natively
 * also after a CW_INPUT test script applied) and before the input module
 * builds actions from it. frame is the engine frame about to run (0-based,
 * cw_frame_index). Same place in the loop on every host, so an autopilot,
 * a bot or demo playback driven from here produces the same action stream
 * as real devices would: the way an input-free host (wasmcart-run) plays a
 * scripted run. One filter; NULL removes it. Cheap: one call per frame. */
typedef void (*cw_raw_input_filter)(cw_raw_input *in, uint64_t frame, void *user);
void cw_set_raw_input_filter(cw_raw_input_filter fn, void *user); /* replaces any previous filter; main thread */

/* ── queries, valid from init onward ───────────────────────────────── */

const cw_raw_input *cw_input_raw(void);
/* True when the device is touch-first and no gamepad has been used: start in
 * touch mode (draw touch prompts / controls from the first frame). Android
 * and iOS builds say yes; a desktop says no unless CW_TOUCH_PRIMARY=1. A
 * cart learns it from its host through the debug channel request
 * `touch_primary 1` (sent before the first frame: the web export's player
 * does so for a coarse-pointer browser, run-cart --touch-primary for tests);
 * other hosts leave it false. Any pad button or stick turns it false for the
 * rest of the run. */
bool cw_platform_touch_primary(void);
/* A short buzz on a phone (feedback for a touch control's press), ms long.
 * Presentation only. A cart publishes it as the debug field `cw.vibrate`
 * (a request count, then ms) and the web export's player calls
 * navigator.vibrate where the browser has it; other hosts ignore it. */
void cw_platform_vibrate(int ms);

/* ── loading progress (cw_app_desc.warmup) ─────────────────────────────
 * Published as the debug field cw.loading = [done, total, stage] (the web
 * export's loading panel reads it to carry its bar past the download) and
 * drawn by the engine's built-in loading screen. */
typedef enum {
  CW_LOAD_NONE, CW_LOAD_ASSETS, CW_LOAD_TEXTURES, CW_LOAD_MESHES, CW_LOAD_SHADERS, CW_LOAD_SCENE, CW_LOAD_STAGE_COUNT
} cw_load_stage;
/* Report warm-up progress: done of total steps, in stage. */
void cw_loading_set(int done, int total, cw_load_stage stage);
/* The progress last reported (any pointer may be NULL). */
void cw_loading_get(int *done, int *total, cw_load_stage *stage);
/* True from start-up until a warm-up (if any) has finished. */
bool cw_loading_active(void);
void cw_window_size(int *w, int *h);   /* logical */
void cw_design_size(int *w, int *h);   /* cw_app_desc width x height: what the game was laid out for */
/* The logical size the app settles on for a screen of screen_w x screen_h
 * pixels (cw_app_desc.fit and aspect bounds): what cw_window_size answers
 * under that host preference. Pure, for tests and tools; 0 x 0 = no
 * preference, the design size. */
void cw_fit_logical_size(const cw_app_desc *app, int screen_w, int screen_h, int *w, int *h);
/* Pixels of what is being drawn right now: the window's, or with
 * pixel_scale / crisp UI the scene's while the scene draws and the UI's once
 * the first 2D window pass of the frame began. Outside a frame, the output's. */
void cw_drawable_size(int *w, int *h);
/* The scene's render size in pixels (the render resolution; with
 * pixel_scale the size before the upscale). */
void cw_render_size(int *w, int *h);
/* Crisp UI (cw_app_desc.ui_min_lines): on or off at runtime. A no-op unless
 * the app offers it; starts off. Render-only. */
void cw_crisp_ui_set(bool on);
bool cw_crisp_ui(void);          /* crisp UI is on now */
bool cw_crisp_ui_offered(void); /* the app set ui_min_lines */
/* Save data: one byte (0 = none chosen, 1 = off, 2 = on); restore applies a
 * saved byte (0 leaves the default). */
uint8_t cw_crisp_ui_saved(void);
void cw_crisp_ui_restore(uint8_t saved); /* apply a cw_crisp_ui_saved byte (at init, after the save loaded) */
/* Framebuffer pixels per logical unit right now (drawable / window size:
 * the render scale times any display density). */
float cw_pixel_ratio(void);
double cw_time(void);                  /* seconds since start (host clock in carts) */
uint64_t cw_frame_index(void);         /* rendered frames since start */
uint64_t cw_tick_index(void);          /* fixed ticks since start */
double cw_fixed_dt(void); /* seconds per fixed tick (1 / fixed_hz) */
bool cw_is_deterministic_run(void);    /* cart: host requested a replay run */
/* Write the app's save data now (natively; a cart's host keeps the region
 * itself). No effect in a deterministic run or without save data. */
void cw_save_commit(void);
uint32_t cw_host_seed(void);           /* seed the host delivered (0 = none) */
void cw_quit(void);                    /* native: leave the loop; cart: no-op */
void cw_set_title(const char *title); /* window title (native); ignored before the window exists */

/* Text input: while started, typed text arrives as CW_EVENT_TEXT (already
 * composed by the OS: layout, dead keys, IME). Native: SDL_StartTextInput;
 * carts: wc_text_input_begin (hosts raise an on-screen keyboard). The UI
 * starts it while a text field has focus. */
void cw_text_input_start(void);
void cw_text_input_stop(void); /* no-op when not started */
bool cw_text_input_active(void); /* between start and stop */

/* Clipboard (UTF-8). Native: the system clipboard through SDL, except in test
 * runs (CW_FRAMES), which use a private buffer so a run never reads what
 * another program copied. Carts: SDL's in-process clipboard (wasmcart has no
 * clipboard ABI), so copy and paste work inside the cart. get returns a
 * frame-arena copy ("" when empty). */
void cw_clipboard_set(const char *utf8);
const char *cw_clipboard_get(void); /* frame-arena copy, valid until next frame */

/* ── audio output ─────────────────────────────────────────────────────
 * One stereo float32 stream. The callback fills `frames` interleaved stereo
 * frames. Native: called on SDL's audio thread. Cart: called on a job-pool
 * thread (cartwheel/jobs.h) while the frame renders, or on the main thread
 * at the end of a frame with no update. Either way it must not block. */
typedef void (*cw_audio_fill_fn)(void *user, float *out, int frames);
/* Starts output (or swaps fill/user when already open). Native: false and silent when there is no
 * device. Cart: sample_rate is ignored, the host's ring rate is used; read cw_audio_rate after. */
bool cw_audio_open(int sample_rate, cw_audio_fill_fn fill, void *user);
int cw_audio_rate(void); /* rate fill runs at (the device's, maybe not the one asked for) */
void cw_audio_close(void); /* stops the device; the fill callback is not called afterwards */

/* ── assets (read-only, shipped with the game) ────────────────────────
 * Paths are relative to the asset root ("fonts/a.ttf", never "assets/..."),
 * '/'-separated, never containing "..".
 * Native: the .cwpak named by CW_PAK, else <executable>.cwpak when it exists
 * (cwpack writes it), then loose files for paths the pack does not hold: the
 * root is CW_ASSETS if set, else <exe>.assets beside the executable
 * (cw_add_app's ASSETS dir is linked there), else ./assets.
 * Cart: the cart's own bundle (the same ASSETS dir or PACK output, packed).
 * Safe from any thread natively; in a cart call them from the main thread (a
 * lazily loaded web cart suspends there while the bytes arrive). Most code
 * loads through cartwheel/resource.h instead. cw_asset_size returns -1 when
 * the asset does not exist; cw_asset_read returns the bytes copied (at most
 * cap), -1 when missing. */
long cw_asset_size(const char *path);
/* Bytes copied (a larger asset is cut at cap), -1 when missing. */
long cw_asset_read(const char *path, void *dst, size_t cap);
/* Whole asset, malloc'd with a trailing NUL (not counted in *len). NULL when
 * missing. Free with free(). */
void *cw_asset_load(const char *path, size_t *len);
/* Native dev: close and reopen the pack (after cwpack rewrote it; hot reload
 * calls this). Cart: no-op. */
void cw_asset_remount(void);
/* Native: read loose files from `dir` from now on (what CW_ASSETS sets at
 * startup): an editor opening a project directory makes the project's
 * fonts, textures and scenes the assets. A mounted pack still answers first.
 * Caches keyed by path (fonts, textures, scenes) keep what they loaded.
 * Cart: no-op, returns false (a cart's assets are its bundle). */
bool cw_asset_set_root(const char *dir);
/* The mounted pack's file name, or NULL (loose files only; always NULL in a
 * cart). */
const char *cw_asset_pack_path(void);

/* ── debug marks (cart: wc_debug_mark; native: logged at debug level) ─ */
void cw_debug_mark(uint32_t id);

#ifdef __cplusplus
}
#endif
#endif

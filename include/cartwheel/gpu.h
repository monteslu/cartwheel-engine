/* cartwheel GPU device: which graphics API this run renders with, and (on
 * WebGPU) the instance / adapter / device / surface the renderers share.
 *
 * Two backends, chosen once at startup, never per frame:
 *   GL      OpenGL ES 3.0 / WebGL2 (cartwheel/gl.h): every target, every host.
 *   WebGPU  webgpu.h (Dawn): natively through native-dawn, in carts through
 *           the wasmcart WebGPU tier (Dawn's emdawnwebgpu port, compiled into
 *           the cart with wasmcart's wgpu-wasi kit).
 *
 * Selection
 *   Native: CW_GPU=gl|webgpu in the environment, else cw_app_desc.gpu, else
 *   GL (the default for now). A run that asks for WebGPU and cannot get it
 *   (no Dawn in this build, no adapter, no device) is an error that ends the
 *   run, never a silent GL fallback.
 *   Carts: a cart built with CW_WGPU (build/cart-wgpu) imports BOTH APIs and
 *   follows the host: WC_HOST_FLAG_GPU_WGPU in wc_host_info_t.flags at
 *   wc_init means WebGPU, else GL. A plain cart build is GL only.
 *
 * What runs on WebGPU today: the 2D renderer (cartwheel/render2d.h: sprites,
 * shapes, text, tilemaps, lit layers with lights and shadows, render
 * targets, virtual canvases) and everything drawn through it (the UI, the
 * scene tree's 2D canvas). 3D (three.c), the post stack and the 3D fx wait
 * on their ports; a WebGPU run that reaches
 * them logs an error once and draws nothing for them.
 *
 * WebGPU rules this module keeps for every renderer:
 *   - core WebGPU where the adapter has it, compatibility mode (the wasmcart
 *     tier's floor) where it does not: natively the engine asks for a core
 *     adapter first; a cart gets the host's device, whatever its level.
 *     Renderers ask cw__wgpu_core() and keep a compatibility-mode path
 *     (CW_WGPU_FEATURE_LEVEL=compat forces it natively, for tests);
 *   - main thread only: workers never call wgpu*;
 *   - nothing blocks in a cart: asynchronous results (errors) arrive
 *     between frames;
 *   - every WebGPU error (the device's uncaptured-error handler; in a cart
 *     error scopes too while the handler is being installed at startup) goes
 *     to cw_error with WebGPU's message and counts in
 *     cw_gpu_error_count(), which test runs and gates read (debug field
 *     "cw.gpu.errors").
 *
 * GPU safety (native test runs, CW_FRAMES): a test run opens no surface and no
 * desktop window (an offscreen texture; SDL's offscreen video driver). The
 * harness pins the GPU (tools/lib/wgpu-pin.mjs): Mesa shows Dawn one device,
 * CW_WGPU_POWER=high|low picks the power preference to match its class, and
 * CW_WGPU_EXPECT_DEVICE (hex, e.g. 0x7480) ends the run before a device exists
 * on any other adapter. */
#ifndef CW_GPU_H
#define CW_GPU_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cw_gpu_api {
  CW_GPU_AUTO = 0, /* the platform default: GL in a development build; an exported game (and one built
                    * on a prebuilt SDK) runs WebGPU (Dawn), GL where WebGPU does not start; a dual
                    * cart follows its host */
  CW_GPU_GL,
  CW_GPU_WEBGPU,
} cw_gpu_api;

/* The API this run renders with (valid from the app's init on). */
cw_gpu_api cw_gpu_backend(void);
const char *cw_gpu_backend_name(void); /* "gl" or "webgpu" */
/* This run renders with a reversed depth buffer (cw_app_desc.reversed_depth, WebGPU only).
 * A pass that records its own WebGPU render passes against the scene's depth clears to 0,
 * compares with greater-equal, and writes clip z as (w - z) / 2 from a GL clip position. Its WGSL
 * has CW_REV (bool: this mode) and CW_FAR (the far plane's depth, 0 or 1) declared for it. */
bool cw_gpu_reversed_depth(void);

/* The adapter a WebGPU run got (all zero on GL). Also published as debug
 * fields when the debug channel is on: cw.gpu.api and cw.gpu.errors (compared
 * across hosts by the gates), host.gpu.vendor_id / device_id / adapter_type /
 * backend (machine facts, never compared), so a run says which GPU it used. */
typedef struct cw_gpu_adapter {
  char vendor[64], architecture[64], device[128], description[128];
  uint32_t vendor_id, device_id;
  uint32_t backend;      /* WGPUBackendType */
  uint32_t adapter_type; /* WGPUAdapterType: 1 discrete, 2 integrated, 3 CPU */
  bool core;             /* the device is core WebGPU (not compatibility mode): HDR scenes multisample
                          * and passes may read depth with textureLoad; false on GL */
} cw_gpu_adapter;
const cw_gpu_adapter *cw_gpu_adapter_info(void); /* never NULL; zeroed on GL */

/* WebGPU errors seen so far (validation, out of memory, internal, device
 * lost). Always 0 on GL. */
uint32_t cw_gpu_error_count(void);

/* Clear the whole window to (r, g, b, a) (both backends): what an app that draws its
 * own background before any pass does instead of glClear. */
void cw_gpu_clear_window(float r, float g, float b, float a);

struct cw_rt; /* cartwheel/pipeline.h */
/* Read the window back, on either backend. cw_gpu_read_window asks for the
 * region (x, y, w, h: window pixels, top-left origin) of the picture this
 * frame puts in the window; the read is taken at the end of the frame, after
 * everything has drawn (the UI, a pixel_scale or render resolution upscale),
 * and returns a ticket (0: a bad region, or eight reads already waiting).
 * cw_gpu_read_result copies the pixels out once they have arrived, as
 * top-down RGBA8 (w * h * 4 bytes): 1 (the ticket is spent), 0 (not yet:
 * ask again next frame) or -1 (an unknown ticket, or a read that failed).
 * On GL the pixels arrive at the end of the frame they were asked in, so
 * they can be taken from the next frame on; on WebGPU a frame or more later
 * (a cart never waits for the GPU). Reads made in the same frame arrive in
 * the order they were asked. A read is a stall on GL and costs a copy on
 * WebGPU: for checks and tools, not for every frame of a game. */
uint32_t cw_gpu_read_window(int x, int y, int w, int h);
/* The same, taken now instead of at the end of the frame: what has been drawn
 * to the window so far (between two passes, to compare them). */
uint32_t cw_gpu_read_window_now(int x, int y, int w, int h);
/* Take a read's pixels (top-down RGBA8) once they arrived: 1, 0 (not yet) or -1 (above). */
int cw_gpu_read_result(uint32_t ticket, void *rgba);
/* A read of a 3D pipeline target (cw_rt_new; a cw_target_rt cannot be read): the
 * region (x, y, w, h, top-left origin) as it is now, top-down RGBA8 like the window's.
 * Taken at once; it arrives as the window's reads do. */
uint32_t cw_gpu_read_rt(const struct cw_rt *rt, int x, int y, int w, int h);
/* cw_gpu_read_result, but natively it waits for a WebGPU read to arrive (a tool or an
 * editor measuring a picture). On GL the read is there already; a cart on WebGPU
 * cannot wait (0 until a later frame). */
int cw_gpu_read_wait(uint32_t ticket, void *rgba);
/* Give up a read: its slot is free again (at once, or when it lands). */
void cw_gpu_read_drop(uint32_t ticket);

/* Wait until the GPU has finished all the work submitted so far (a perf
 * build timing whole frames): natively on both backends, and in a cart on
 * GL (a one-pixel read). A cart on WebGPU cannot wait: false, nothing done. */
bool cw_gpu_finish(void);

#ifdef __cplusplus
}
#endif
#endif

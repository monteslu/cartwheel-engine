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
 *   - compatibility feature level (the wasmcart tier's floor, enforced by
 *     Dawn on every backend);
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
  CW_GPU_AUTO = 0, /* the platform default (GL for now) */
  CW_GPU_GL,
  CW_GPU_WEBGPU,
} cw_gpu_api;

/* The API this run renders with (valid from the app's init on). */
cw_gpu_api cw_gpu_backend(void);
const char *cw_gpu_backend_name(void); /* "gl" or "webgpu" */

/* The adapter a WebGPU run got (all zero on GL). Also published as debug
 * fields when the debug channel is on: cw.gpu.api and cw.gpu.errors (compared
 * across hosts by the gates), host.gpu.vendor_id / device_id / adapter_type /
 * backend (machine facts, never compared), so a run says which GPU it used. */
typedef struct cw_gpu_adapter {
  char vendor[64], architecture[64], device[128], description[128];
  uint32_t vendor_id, device_id;
  uint32_t backend;      /* WGPUBackendType */
  uint32_t adapter_type; /* WGPUAdapterType: 1 discrete, 2 integrated, 3 CPU */
} cw_gpu_adapter;
const cw_gpu_adapter *cw_gpu_adapter_info(void); /* never NULL; zeroed on GL */

/* WebGPU errors seen so far (validation, out of memory, internal, device
 * lost). Always 0 on GL. */
uint32_t cw_gpu_error_count(void);

/* Clear the whole window to (r, g, b, a) (both backends): what an app that draws its
 * own background before any pass does instead of glClear. */
void cw_gpu_clear_window(float r, float g, float b, float a);

#ifdef __cplusplus
}
#endif
#endif

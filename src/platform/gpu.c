/* The GPU device module (cartwheel/gpu.h): backend selection, and on WebGPU
 * the instance / adapter / device / surface, the frame's target texture,
 * error routing, presenting and (native) readback.
 *
 * Native (native-dawn): instance -> adapter (compatibility feature level,
 * the power preference CW_WGPU_POWER names, low power by default; the adapter is checked before a device exists in test runs) ->
 * device with uncaptured-error and device-lost callbacks -> a surface on the
 * SDL window (native_dawn/sdl2.h), or, in test runs (CW_FRAMES), no surface at
 * all: an offscreen RGBA8 texture (and SDL's offscreen video driver, so no
 * window reaches the desktop).
 * Asynchronous steps are AllowProcessEvents callbacks pumped with
 * wgpuInstanceProcessEvents (natively the main thread may wait).
 *
 * Native runs skip Dawn's command validation (the "skip_validation"
 * toggle) except in test runs (CW_FRAMES) and debug builds;
 * CW_WGPU_VALIDATION=1 / 0 overrides either way.
 *
 * Cart (wasmcart WebGPU tier): the host's device
 * (emscripten_webgpu_get_device), a surface on "#canvas" configured once at
 * the cart's size, no wgpuSurfacePresent (the host presents after
 * wc_render). Errors: the device's uncaptured-error handler, installed by
 * requesting the host's device once more with the handler (the host answers
 * with the same device; webgpu.h cannot set one on a device it already
 * has). Until that request lands, at startup where the cart creates its
 * resources, a set of error scopes is open, popped and re-pushed at the end
 * of each frame; after it no scopes are pushed. Callbacks run between frames
 * (the host's event loop), never inside wc_render. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/jobs.h"

static cw_gpu_api backend = CW_GPU_GL;
static bool auto_pick __attribute__((unused)); /* the native run chose WebGPU itself (CW_GPU_AUTO): GL is its fallback */
static cw_gpu_adapter adapter_info;
static uint32_t gpu_errors;
static uint32_t dbg_api; /* debug field: 1 GL, 2 WebGPU */
static uint32_t host_threads, host_worker_chunks;

cw_gpu_api cw_gpu_backend(void) { return backend; }
const char *cw_gpu_backend_name(void) { return backend == CW_GPU_WEBGPU ? "webgpu" : "gl"; }
const cw_gpu_adapter *cw_gpu_adapter_info(void) { return &adapter_info; }
uint32_t cw_gpu_error_count(void) { return gpu_errors; }

/* the debug fields every run publishes when the channel is on */
void cw__gpu_debug_register(void);
void cw__gpu_debug_register(void) {
  dbg_api = backend == CW_GPU_WEBGPU ? 2 : 1;
  cw_debug_u32("cw.gpu.api", &dbg_api);
  cw_debug_u32("cw.gpu.errors", &gpu_errors);
  /* host.*: facts about the machine a run used, never compared across hosts
   * (tests/gate.mjs skips them): the adapter, and the job pool's threads and
   * the chunks its workers (not the main thread) ran, which shows a threaded
   * cart really ran jobs off the main thread */
  cw_debug_u32("host.gpu.vendor_id", &adapter_info.vendor_id);
  cw_debug_u32("host.gpu.device_id", &adapter_info.device_id);
  cw_debug_u32("host.gpu.adapter_type", &adapter_info.adapter_type);
  cw_debug_u32("host.gpu.backend", &adapter_info.backend);
  cw_debug_u32("host.jobs.threads", &host_threads);
  cw_debug_u32("host.jobs.worker_chunks", &host_worker_chunks);
}

/* end of every frame (cw__frame): refresh the host.* fields */
void cw__host_debug_update(void);
void cw__host_debug_update(void) {
  cw_jobs_stats st;
  cw_jobs_get_stats(&st);
  uint64_t w = 0;
  int n = cw_jobs_thread_count();
  for (int i = 1; i < n && i < 64; i++) w += st.chunks_by_thread[i];
  host_threads = (uint32_t)n;
  host_worker_chunks = (uint32_t)w;
}

/* true in an app built with cw_add_app(... GL_ONLY): it calls GL itself, so
 * it has no WebGPU build (the cart-wgpu tree makes no dual cart for it). A
 * native run asked for WebGPU refuses at startup, as asking for WebGPU where it
 * cannot be had always does, rather than trapping at the app's first GL call. */
bool cw__app_gl_only(void);
__attribute__((weak)) bool cw__app_gl_only(void) { return false; }

#if !CW_CART
/* the API a native run asks for: CW_GPU (gl | webgpu | wgpu) over the app's
 * desc; false (logged) on an unknown CW_GPU or WebGPU for a GL_ONLY app */
static bool native_want(cw_gpu_api *want) {
  const char *env = getenv("CW_GPU");
  if (env && *env) {
    if (!strcmp(env, "webgpu") || !strcmp(env, "wgpu")) *want = CW_GPU_WEBGPU;
    else if (!strcmp(env, "gl")) *want = CW_GPU_GL;
    else {
      cw_error("CW_GPU=%s: use gl or webgpu", env);
      return false;
    }
  }
  if (*want == CW_GPU_WEBGPU && cw__app_gl_only()) {
    cw_error("WebGPU was asked for (%s%s), but this app draws with GL itself (cw_add_app GL_ONLY) and has no "
             "WebGPU build, as the cart-wgpu tree makes no dual cart for it: run it on GL (CW_GPU=gl)",
             env && *env ? "CW_GPU=" : "", env && *env ? env : "cw_app_desc.gpu");
    return false;
  }
  return true;
}
#endif

#if !CW_WGPU
/* a build without WebGPU (exports, the GL cart build): GL only */
bool cw__gpu_select(cw_gpu_api want, bool host_wgpu) {
  (void)host_wgpu;
#if !CW_CART
  if (!native_want(&want)) return false;
#endif
  if (want == CW_GPU_WEBGPU) {
    const char *env = NULL;
#if !CW_CART
    env = getenv("CW_GPU");
#endif
    cw_error("WebGPU was asked for (%s), but this build has no WebGPU backend (configure with CW_WGPU=ON)",
             env && *env ? "CW_GPU" : "cw_app_desc.gpu");
    return false;
  }
  backend = CW_GPU_GL;
  return true;
}
bool cw__gpu_fallback_to_gl(void) { return false; }
bool cw__wgpu_init(void) { return false; }
void cw__wgpu_frame_end(bool present) { (void)present; }
bool cw__wgpu_readback(unsigned char **px, int *w, int *h) { (void)px, (void)w, (void)h; return false; }
void cw__wgpu_shutdown(void) {}
void cw__wgpu_drawable_size(int *w, int *h) { (void)w, (void)h; }
void cw_gpu_clear_window(float r, float g, float b, float a) {
  int w, h;
  cw_drawable_size(&w, &h);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, w, h);
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
#else

#include "wgpu.h"

/* GL on a WebGPU run (cartwheel/gl.h checks every GL entry point): stop with
 * the GL function, the call site and the feature, instead of the host's bare
 * import trap (a cart) or a GL call with no context (native) */
bool cw__gl_banned;
const char *cw__gl_feature;
void cw__gl_trap(const char *fn, const char *file, int line) {
  cw_fatal(file, line,
           "'%s' called GL (%s) on a WebGPU run: this path has no WebGPU implementation, and a dual cart traps on any GL "
           "call while the host selected WebGPU",
           cw__gl_feature && *cw__gl_feature ? cw__gl_feature : "engine or game code", fn);
}
static void set_backend(cw_gpu_api b) {
  backend = b;
  cw__gl_banned = b == CW_GPU_WEBGPU;
}
#if !CW_CART
#include <SDL_syswm.h>
#include "native_dawn/sdl2.h"
#endif

static struct {
  WGPUInstance instance;
  WGPUAdapter adapter;
  WGPUDevice device;
  WGPUQueue queue;
  WGPULimits limits;
  WGPUSurface surface;
  WGPUTextureFormat format;
  int surf_w, surf_h; /* configured surface size (0 = not yet) */
  bool headless;      /* offscreen frame texture (native test runs) */
  WGPUTexture offscreen;
  int off_w, off_h;
  WGPUTexture frame_tex; /* this frame's target (surface texture or offscreen) */
  WGPUTextureView frame_view;
  bool frame_from_surface;
  bool lost;
  bool timestamps;
#if CW_CART
  WGPUAdapter err_adapter; /* the second request for the host's device (errors) */
  WGPUDevice err_device;   /* the host's device again, with the uncaptured-error handler */
  bool err_handler;        /* that handler is installed: no more per-frame scopes */
  bool scopes_open;
#endif
#if !CW_CART
  NativeDawnSDL2Window native_win;
#endif
} G;

bool cw__wgpu_on(void) { return backend == CW_GPU_WEBGPU; }
WGPUDevice cw__wgpu_device(void) { return G.device; }
WGPUQueue cw__wgpu_queue(void) { return G.queue; }
const WGPULimits *cw__wgpu_limits(void) { return &G.limits; }
WGPUTextureFormat cw__wgpu_frame_format(void) { return G.format; }
bool cw__wgpu_timestamps(void) { return G.timestamps; }

static void sv_copy(char *dst, size_t cap, WGPUStringView v) {
  size_t n = v.data ? (v.length == WGPU_STRLEN ? strlen(v.data) : v.length) : 0;
  if (n >= cap) n = cap - 1;
  if (n) memcpy(dst, v.data, n);
  dst[n] = 0;
}

static const char *error_type_name(WGPUErrorType t) {
  switch (t) {
  case WGPUErrorType_Validation: return "validation";
  case WGPUErrorType_OutOfMemory: return "out of memory";
  case WGPUErrorType_Internal: return "internal";
  default: return "unknown";
  }
}

static void report(const char *kind, WGPUStringView msg) {
  char m[2048];
  sv_copy(m, sizeof m, msg);
  gpu_errors++;
  cw_error("WebGPU %s error: %s", kind, m);
}

void cw__wgpu_fail(const char *fmt, ...) {
  char m[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m, sizeof m, fmt, ap);
  va_end(ap);
  gpu_errors++;
  cw_error("WebGPU: %s", m);
}

WGPUShaderModule cw__wgpu_shader(const char *label, const char *wgsl) {
  WGPUShaderSourceWGSL src = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = CW_WGPU_SV(wgsl) };
  WGPUShaderModuleDescriptor d = { .nextInChain = &src.chain, .label = CW_WGPU_SV(label) };
  return wgpuDeviceCreateShaderModule(G.device, &d);
}

static void read_adapter_info(WGPUAdapterInfo *ai) {
  sv_copy(adapter_info.vendor, sizeof adapter_info.vendor, ai->vendor);
  sv_copy(adapter_info.architecture, sizeof adapter_info.architecture, ai->architecture);
  sv_copy(adapter_info.device, sizeof adapter_info.device, ai->device);
  sv_copy(adapter_info.description, sizeof adapter_info.description, ai->description);
  adapter_info.vendor_id = ai->vendorID;
  adapter_info.device_id = ai->deviceID;
  adapter_info.backend = (uint32_t)ai->backendType;
  adapter_info.adapter_type = (uint32_t)ai->adapterType;
}

static void log_adapter(void) {
  cw_log(CW_LOG_INFO, "WebGPU adapter: %s %s (%04x:%04x, %s, backend %u, type %u)", adapter_info.vendor,
         adapter_info.device[0] ? adapter_info.device : adapter_info.description, adapter_info.vendor_id,
         adapter_info.device_id, adapter_info.architecture, adapter_info.backend, adapter_info.adapter_type);
}

bool cw__gpu_select(cw_gpu_api want, bool host_wgpu) {
#if CW_CART
  (void)want;
  set_backend(host_wgpu ? CW_GPU_WEBGPU : CW_GPU_GL); /* a dual cart follows the host */
#else
  (void)host_wgpu;
  if (!native_want(&want)) return false;
  /* AUTO in an export build (CW_GPU_AUTO_WEBGPU: the export toolchains and
   * the prebuilt SDKs): WebGPU (Dawn), and GL if WebGPU does not start on
   * this machine (cw__gpu_fallback_to_gl); GL_ONLY apps draw with GL. A
   * development build keeps GL. An explicit request (CW_GPU,
   * cw_app_desc.gpu) gets that API or fails. */
#ifndef CW_GPU_AUTO_WEBGPU
#define CW_GPU_AUTO_WEBGPU 0
#endif
  auto_pick = CW_GPU_AUTO_WEBGPU && want == CW_GPU_AUTO && !cw__app_gl_only();
  set_backend(want == CW_GPU_WEBGPU || auto_pick ? CW_GPU_WEBGPU : CW_GPU_GL);
#endif
  return true;
}

#if !CW_CART
bool cw__gpu_fallback_to_gl(void) {
  if (!auto_pick || backend != CW_GPU_WEBGPU) return false;
  cw__wgpu_shutdown();
  set_backend(CW_GPU_GL);
  cw_log(CW_LOG_WARN, "WebGPU did not start on this machine: rendering with GL (CW_GPU=webgpu makes that an error)");
  return true;
}
#else
bool cw__gpu_fallback_to_gl(void) { return false; }
#endif

/* ── frame target ─────────────────────────────────────────────────────── */

void cw__wgpu_drawable_size(int *w, int *h) {
#if CW_CART
  *w = cw__plat.px_w; /* the declared framebuffer (render scale applied) */
  *h = cw__plat.px_h;
#else
  SDL_GetWindowSizeInPixels(cw__plat.window, w, h);
  if (*w <= 0 || *h <= 0) SDL_GetWindowSize(cw__plat.window, w, h);
#endif
}

/* what draws now: the window's frame, or pixel.c's scene / UI target */
void cw__wgpu_frame_size(int *w, int *h) {
  int t = cw__pixel_target();
  if (t == 1) { *w = cw__plat.scene_w; *h = cw__plat.scene_h; }
  else if (t == 2) { *w = cw__plat.view_w; *h = cw__plat.view_h; }
  else cw__wgpu_drawable_size(w, h);
}

static void configure_surface(int w, int h) {
  WGPUSurfaceCapabilities caps = { 0 };
  wgpuSurfaceGetCapabilities(G.surface, G.adapter, &caps);
  WGPUTextureUsage usage = WGPUTextureUsage_RenderAttachment;
  if (caps.usages & WGPUTextureUsage_CopySrc) usage |= WGPUTextureUsage_CopySrc; /* native screenshots */
  if (!G.format) {
    G.format = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
    /* a non-sRGB format: GL's default framebuffer stores what the shaders write */
    for (size_t i = 0; i < caps.formatCount; i++)
      if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
        G.format = caps.formats[i];
        break;
      }
  }
  /* opaque where the surface offers it; Android's Vulkan surfaces may offer
   * only inherit (Auto lets Dawn pick from what the surface supports) */
  WGPUCompositeAlphaMode alpha = WGPUCompositeAlphaMode_Auto;
  for (size_t i = 0; i < caps.alphaModeCount; i++)
    if (caps.alphaModes[i] == WGPUCompositeAlphaMode_Opaque) alpha = WGPUCompositeAlphaMode_Opaque;
  WGPUSurfaceConfiguration cfg = { .device = G.device, .format = G.format, .usage = usage, .width = (uint32_t)w,
                                   .height = (uint32_t)h, .alphaMode = alpha, .presentMode = WGPUPresentMode_Fifo };
#if !CW_CART
  if (cw__plat.app.vsync_off)
    for (size_t i = 0; i < caps.presentModeCount; i++)
      if (caps.presentModes[i] == WGPUPresentMode_Immediate) cfg.presentMode = WGPUPresentMode_Immediate;
#endif
  wgpuSurfaceConfigure(G.surface, &cfg);
  wgpuSurfaceCapabilitiesFreeMembers(caps);
  G.surf_w = w;
  G.surf_h = h;
}

static void acquire(void) {
  if (G.frame_tex || G.lost) return;
  int w, h;
  cw__wgpu_drawable_size(&w, &h);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (G.headless) {
    if (!G.offscreen || G.off_w != w || G.off_h != h) {
      if (G.offscreen) wgpuTextureRelease(G.offscreen);
      WGPUTextureDescriptor d = { .label = CW_WGPU_SV("cw frame (offscreen)"),
                                  .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
                                  .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, 1 },
                                  .format = G.format, .mipLevelCount = 1, .sampleCount = 1 };
      G.offscreen = wgpuDeviceCreateTexture(G.device, &d);
      G.off_w = w;
      G.off_h = h;
    }
    wgpuTextureAddRef(G.offscreen);
    G.frame_tex = G.offscreen;
    G.frame_from_surface = false;
  } else {
#if CW_CART
    if (!G.surf_w) configure_surface(w, h); /* the cart's own size, once */
#else
    if (G.surf_w != w || G.surf_h != h) configure_surface(w, h);
#endif
    WGPUSurfaceTexture st = { 0 };
    wgpuSurfaceGetCurrentTexture(G.surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
      if (st.texture) wgpuTextureRelease(st.texture);
      /* outdated (a resize raced us): reconfigure once and retry */
      configure_surface(w, h);
      wgpuSurfaceGetCurrentTexture(G.surface, &st);
      if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
          st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (st.texture) wgpuTextureRelease(st.texture);
        cw__wgpu_fail("the surface gave no texture this frame (status %d)", (int)st.status);
        return;
      }
    }
    G.frame_tex = st.texture;
    G.frame_from_surface = true;
  }
  G.frame_view = wgpuTextureCreateView(G.frame_tex, NULL);
}

WGPUTexture cw__wgpu_frame_texture(void) {
  acquire();
  return G.frame_tex;
}
WGPUTextureView cw__pixel_wgpu_view(void); /* pixel.c */
/* the view engine code draws "the window" into: pixel.c's target while one
 * stands in (cw_app_desc.pixel_scale, crisp UI), else the frame's */
WGPUTextureView cw__wgpu_frame_view(void) {
  WGPUTextureView v = cw__pixel_wgpu_view();
  if (v) return v;
  acquire();
  return G.frame_view;
}
WGPUTextureView cw__wgpu_output_view(void); /* the frame's own view (pixel.c composes into it) */
WGPUTextureView cw__wgpu_output_view(void) {
  acquire();
  return G.frame_view;
}

void cw_gpu_clear_window(float r, float g, float b, float a) {
  if (!cw__wgpu_on()) {
    int w, h;
    cw_drawable_size(&w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return;
  }
  WGPUTextureView v = cw__wgpu_frame_view();
  if (!v) return;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(G.device, NULL);
  WGPURenderPassColorAttachment ca = { .view = v, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                       .storeOp = WGPUStoreOp_Store, .clearValue = { r, g, b, a } };
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = 1, .colorAttachments = &ca };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(G.queue, 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
}

/* ── errors ───────────────────────────────────────────────────────────── */

static void on_uncaptured(WGPUDevice const *d, WGPUErrorType type, WGPUStringView msg, void *u1, void *u2) {
  (void)d, (void)u1, (void)u2;
  report(error_type_name(type), msg);
}
static void on_lost(WGPUDevice const *d, WGPUDeviceLostReason reason, WGPUStringView msg, void *u1, void *u2) {
  (void)d, (void)u1, (void)u2;
  if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled) return;
  G.lost = true;
  report("device lost", msg);
}

#if CW_CART
static void on_scope(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView msg, void *u1, void *u2) {
  (void)u2;
  if (status != WGPUPopErrorScopeStatus_Success) return; /* the device is gone (reported by lost) */
  if (type != WGPUErrorType_NoError) report((const char *)u1, msg);
}
static const WGPUErrorFilter scope_filters[3] = { WGPUErrorFilter_Validation, WGPUErrorFilter_OutOfMemory,
                                                  WGPUErrorFilter_Internal };
static const char *scope_names[3] = { "validation", "out of memory", "internal" };
static void push_scopes(void) {
  for (int i = 0; i < 3; i++) wgpuDevicePushErrorScope(G.device, scope_filters[i]);
  G.scopes_open = true;
}
static void pop_scopes(void) {
  if (!G.scopes_open) return;
  G.scopes_open = false;
  for (int i = 2; i >= 0; i--) {
    WGPUPopErrorScopeCallbackInfo ci = { .mode = WGPUCallbackMode_AllowSpontaneous, .callback = on_scope,
                                         .userdata1 = (void *)scope_names[i] };
    wgpuDevicePopErrorScope(G.device, ci);
  }
}
/* the uncaptured-error handler: the host's device requested again with it */
static void on_err_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void *u1, void *u2) {
  (void)u1, (void)u2;
  if (st != WGPURequestDeviceStatus_Success || !d) {
    char m[512];
    sv_copy(m, sizeof m, msg);
    cw_info("WebGPU: no uncaptured-error handler (%s): error scopes stay open every frame", m);
    return;
  }
  G.err_device = d;
  G.err_handler = true;
}
static void on_err_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void *u1, void *u2) {
  (void)msg, (void)u1, (void)u2;
  if (st != WGPURequestAdapterStatus_Success || !a) {
    cw_info("WebGPU: no adapter for the uncaptured-error handler: error scopes stay open every frame");
    return;
  }
  G.err_adapter = a;
  WGPUDeviceDescriptor dd = {
    .label = CW_WGPU_SV("cartwheel errors"),
    .deviceLostCallbackInfo = { .mode = WGPUCallbackMode_AllowSpontaneous, .callback = on_lost },
    .uncapturedErrorCallbackInfo = { .callback = on_uncaptured },
  };
  wgpuAdapterRequestDevice(a, &dd, (WGPURequestDeviceCallbackInfo){ .mode = WGPUCallbackMode_AllowSpontaneous, .callback = on_err_device });
}
#else

/* ── native bring-up ──────────────────────────────────────────────────── */

typedef struct { bool done; WGPURequestAdapterStatus st; WGPUAdapter a; char msg[512]; } adapter_req;
static void on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void *u1, void *u2) {
  (void)u2;
  adapter_req *r = u1;
  r->st = st;
  r->a = a;
  sv_copy(r->msg, sizeof r->msg, msg);
  r->done = true;
}
typedef struct { bool done; WGPURequestDeviceStatus st; WGPUDevice d; char msg[512]; } device_req;
static void on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void *u1, void *u2) {
  (void)u2;
  device_req *r = u1;
  r->st = st;
  r->d = d;
  sv_copy(r->msg, sizeof r->msg, msg);
  r->done = true;
}
static void pump_until(volatile bool *done) {
  for (int i = 0; !*done && i < 60000; i++) {
    wgpuInstanceProcessEvents(G.instance);
    if (!*done) SDL_Delay(1);
  }
}

static void on_work_done(WGPUQueueWorkDoneStatus st, WGPUStringView msg, void *u1, void *u2) {
  (void)st, (void)msg, (void)u1, (void)u2;
}

static void wait_future(WGPUFuture f) {
  WGPUFutureWaitInfo wi = { .future = f };
  WGPUWaitStatus ws = wgpuInstanceWaitAny(G.instance, 1, &wi, 10ull * 1000 * 1000 * 1000);
  if (ws != WGPUWaitStatus_Success) cw__wgpu_fail("waiting for the GPU failed (status %d)", (int)ws);
  wgpuInstanceProcessEvents(G.instance); /* and deliver what else completed */
}

static bool test_run(void) { return getenv("CW_FRAMES") != NULL; }

/* Dawn's command validation costs the CPU on every call (a game's scene pass
 * measured 2.64 ms on WebGPU against 1.54 on GL with it). A player's run skips
 * it; test runs (CW_FRAMES) and debug builds keep it, so every gate still
 * catches an invalid call. CW_WGPU_VALIDATION=1 keeps it in a release run,
 * =0 drops it in a test run (perf measurement, tools/perf-frame.mjs). */
static bool wgpu_validate(void) {
  const char *v = getenv("CW_WGPU_VALIDATION");
  if (v && *v) return atoi(v) != 0;
#ifndef NDEBUG
  return true;
#else
  return test_run();
#endif
}

/* GPU safety: a test run is pinned to one device by the harness
 * (tools/lib/wgpu-pin.mjs sets CW_WGPU_EXPECT_DEVICE to its PCI device id);
 * any other adapter is refused before a device exists. Without it a test run
 * takes whatever the power preference gets and logs it. */
static bool adapter_allowed(void) {
  const char *want = getenv("CW_WGPU_EXPECT_DEVICE");
  if (!want || !*want) return true;
  unsigned long id = strtoul(want, NULL, 16);
  if (adapter_info.device_id == id) return true;
  cw_error("WebGPU: adapter %04x:%04x (%s) is not CW_WGPU_EXPECT_DEVICE=%s: refusing to run on it",
           adapter_info.vendor_id, adapter_info.device_id, adapter_info.device, want);
  return false;
}
#endif

bool cw__wgpu_init(void) {
  if (!cw__wgpu_on()) return true;
#if CW_CART
  G.device = emscripten_webgpu_get_device();
  if (!G.device) {
    cw_error("WebGPU: the host selected WebGPU but gave no device");
    return false;
  }
  G.queue = wgpuDeviceGetQueue(G.device);
  WGPUAdapterInfo ai = { 0 };
  if (wgpuDeviceGetAdapterInfo(G.device, &ai) == WGPUStatus_Success) {
    read_adapter_info(&ai);
    wgpuAdapterInfoFreeMembers(ai);
  }
  wgpuDeviceGetLimits(G.device, &G.limits);
  G.instance = wgpuCreateInstance(NULL);
  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {
    .chain = { .sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector }, .selector = CW_WGPU_SV("#canvas") };
  WGPUSurfaceDescriptor sd = { .nextInChain = &canvas.chain };
  G.surface = wgpuInstanceCreateSurface(G.instance, &sd);
  push_scopes();
  wgpuInstanceRequestAdapter(G.instance, &(WGPURequestAdapterOptions){ .featureLevel = WGPUFeatureLevel_Compatibility },
                             (WGPURequestAdapterCallbackInfo){ .mode = WGPUCallbackMode_AllowSpontaneous, .callback = on_err_adapter });
#else
  G.headless = test_run();
  /* timed WaitAny: frame pacing and perf waits block on a GPU future instead
   * of polling (a 1 ms sleep loop measured 15 ms a frame on a loaded box) */
  static const WGPUInstanceFeatureName inst_features[1] = { WGPUInstanceFeatureName_TimedWaitAny };
  WGPUInstanceDescriptor id = { .requiredFeatureCount = 1, .requiredFeatures = inst_features };
  G.instance = wgpuCreateInstance(&id);
  if (!G.instance) {
    cw_error("WebGPU: wgpuCreateInstance failed");
    return false;
  }
  if (!G.headless) {
    if (nativeDawnSDL2Window(cw__plat.window, &G.native_win) != 0) {
      cw_error("WebGPU: no surface for this SDL window: %s", SDL_GetError());
      return false;
    }
    G.surface = nativeDawnCreateSurface(G.instance, &G.native_win.window);
    if (!G.surface) {
      cw_error("WebGPU: could not create a surface on the window");
      return false;
    }
  }
  const char *pw = getenv("CW_WGPU_POWER");
  WGPURequestAdapterOptions ao = { .featureLevel = WGPUFeatureLevel_Compatibility,
                                   .powerPreference = pw && !strcmp(pw, "high")
                                                          ? WGPUPowerPreference_HighPerformance
                                                          : WGPUPowerPreference_LowPower,
                                   .compatibleSurface = G.surface };
  adapter_req ar = { 0 };
  wgpuInstanceRequestAdapter(G.instance, &ao,
                             (WGPURequestAdapterCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents,
                                                               .callback = on_adapter, .userdata1 = &ar });
  pump_until(&ar.done);
  if (!ar.done || ar.st != WGPURequestAdapterStatus_Success || !ar.a) {
    cw_error("WebGPU: no adapter (%s)", ar.done ? ar.msg : "timed out");
    return false;
  }
  G.adapter = ar.a;
  WGPUAdapterInfo ai = { 0 };
  wgpuAdapterGetInfo(G.adapter, &ai);
  read_adapter_info(&ai);
  wgpuAdapterInfoFreeMembers(ai);
  log_adapter();
  if (!adapter_allowed()) return false;
  /* GPU pass timing natively (cw_pipeline_pass_stats): timestamp queries
   * when the adapter has them */
  bool validate = wgpu_validate();
  static const char *const skip_validation[1] = { "skip_validation" };
  WGPUDawnTogglesDescriptor toggles = { .chain = { .sType = WGPUSType_DawnTogglesDescriptor },
                                        .enabledToggleCount = 1, .enabledToggles = skip_validation };
  const char *tse = getenv("CW_WGPU_TIMESTAMPS");
  WGPUFeatureName feats[1];
  size_t nfeats = 0;
  if (!(tse && !strcmp(tse, "0")) && wgpuAdapterHasFeature(G.adapter, WGPUFeatureName_TimestampQuery))
    feats[nfeats++] = WGPUFeatureName_TimestampQuery;
  WGPUDeviceDescriptor dd = {
    .nextInChain = validate ? NULL : &toggles.chain,
    .label = CW_WGPU_SV("cartwheel"),
    .requiredFeatureCount = nfeats,
    .requiredFeatures = feats,
    .deviceLostCallbackInfo = { .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_lost },
    .uncapturedErrorCallbackInfo = { .callback = on_uncaptured },
  };
  device_req dr = { 0 };
  wgpuAdapterRequestDevice(G.adapter, &dd,
                           (WGPURequestDeviceCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents,
                                                            .callback = on_device, .userdata1 = &dr });
  pump_until(&dr.done);
  if (!dr.done || dr.st != WGPURequestDeviceStatus_Success || !dr.d) {
    cw_error("WebGPU: no device (%s)", dr.done ? dr.msg : "timed out");
    return false;
  }
  G.device = dr.d;
  G.queue = wgpuDeviceGetQueue(G.device);
  wgpuDeviceGetLimits(G.device, &G.limits);
  G.timestamps = nfeats > 0;
  if (!G.headless) { /* a surface this adapter cannot present to: not started (GL takes over when it may) */
    WGPUSurfaceCapabilities caps = { 0 };
    bool ok = wgpuSurfaceGetCapabilities(G.surface, G.adapter, &caps) == WGPUStatus_Success && caps.formatCount > 0;
    if (ok) wgpuSurfaceCapabilitiesFreeMembers(caps);
    if (!ok) {
      cw_error("WebGPU: the window's surface has no format this adapter can present");
      return false;
    }
  }
  cw_info("WebGPU: Dawn validation %s", validate ? "on" : "off (skip_validation: a release run; CW_WGPU_VALIDATION=1 keeps it)");
  if (G.headless) G.format = WGPUTextureFormat_RGBA8Unorm;
#endif
#if CW_CART
  log_adapter();
#endif
  return true;
}

/* End of a frame: hand the frame's texture back (natively present it when
 * it came from the surface), deliver async results, rotate the cart's error
 * scopes. present=false natively skips the present (the last frame of a test
 * run, after its readback). */
static void (*frame_end_hooks[8])(void);
static int n_frame_end_hooks;
void cw__wgpu_add_frame_end_hook(void (*fn)(void)) {
  for (int i = 0; i < n_frame_end_hooks; i++)
    if (frame_end_hooks[i] == fn) return;
  if (n_frame_end_hooks < 8) frame_end_hooks[n_frame_end_hooks++] = fn;
}

void cw__wgpu_frame_end(bool present) {
  if (!cw__wgpu_on() || !G.device) return;
  for (int i = 0; i < n_frame_end_hooks; i++) frame_end_hooks[i]();
#if CW_CART
  (void)present;
  /* startup's scopes rotate each frame until the uncaptured-error handler is
   * in; then the open set is popped and no more are pushed */
  pop_scopes();
  if (!G.err_handler) push_scopes();
#else
  if (G.frame_tex && G.frame_from_surface && present && !G.lost) wgpuSurfacePresent(G.surface);
  /* at most two frames in flight, like a swapchain: an offscreen test run
   * has no present to pace it, and the GPU queue (and every asynchronous
   * result behind it) would fall further behind each frame */
  static WGPUFuture fut[3];
  static bool pending[3];
  static int slot;
  fut[slot] = wgpuQueueOnSubmittedWorkDone(G.queue, (WGPUQueueWorkDoneCallbackInfo){
    .mode = WGPUCallbackMode_WaitAnyOnly, .callback = on_work_done, .userdata1 = (void *)&pending[slot] });
  pending[slot] = true;
  slot = (slot + 1) % 3;
  if (pending[slot]) wait_future(fut[slot]);
  pending[slot] = false;
#endif
  if (G.frame_view) wgpuTextureViewRelease(G.frame_view);
  if (G.frame_tex) wgpuTextureRelease(G.frame_tex);
  G.frame_view = NULL;
  G.frame_tex = NULL;
}

/* Native: wait until the GPU has finished everything submitted (perf
 * measurements). A no-op in carts, which never wait. */
void cw__wgpu_wait_idle(void) {
#if !CW_CART
  if (!G.device) return;
  wait_future(wgpuQueueOnSubmittedWorkDone(G.queue, (WGPUQueueWorkDoneCallbackInfo){
    .mode = WGPUCallbackMode_WaitAnyOnly, .callback = on_work_done }));
#endif
}

#if !CW_CART
typedef struct { bool done; WGPUMapAsyncStatus st; } map_req;
static void on_map(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) {
  (void)msg, (void)u2;
  map_req *r = u1;
  r->st = st;
  r->done = true;
}
#endif

/* Native: this frame's picture as top-down RGBA8 (malloc'd). */
bool cw__wgpu_readback(unsigned char **out, int *ow, int *oh) {
#if CW_CART
  (void)out, (void)ow, (void)oh;
  return false;
#else
  WGPUTexture tex = cw__wgpu_frame_texture();
  if (!tex) return false;
  int w = (int)wgpuTextureGetWidth(tex), h = (int)wgpuTextureGetHeight(tex);
  uint32_t row = ((uint32_t)w * 4 + 255) & ~255u;
  WGPUBufferDescriptor bd = { .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, .size = (uint64_t)row * h };
  WGPUBuffer buf = wgpuDeviceCreateBuffer(G.device, &bd);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(G.device, NULL);
  WGPUTexelCopyTextureInfo src = { .texture = tex, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferInfo dst = { .layout = { .bytesPerRow = row, .rowsPerImage = (uint32_t)h }, .buffer = buf };
  WGPUExtent3D ext = { (uint32_t)w, (uint32_t)h, 1 };
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(G.queue, 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
  map_req mr = { 0 };
  wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size,
                     (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map,
                                                  .userdata1 = &mr });
  pump_until(&mr.done);
  bool ok = mr.done && mr.st == WGPUMapAsyncStatus_Success;
  if (ok) {
    const uint8_t *p = wgpuBufferGetConstMappedRange(buf, 0, bd.size);
    unsigned char *px = malloc((size_t)w * h * 4);
    bool bgra = G.format == WGPUTextureFormat_BGRA8Unorm || G.format == WGPUTextureFormat_BGRA8UnormSrgb;
    for (int y = 0; y < h; y++) {
      const uint8_t *s = p + (size_t)y * row;
      unsigned char *d = px + (size_t)y * w * 4;
      for (int x = 0; x < w; x++, s += 4, d += 4) {
        d[0] = bgra ? s[2] : s[0];
        d[1] = s[1];
        d[2] = bgra ? s[0] : s[2];
        d[3] = s[3];
      }
    }
    wgpuBufferUnmap(buf);
    *out = px;
    *ow = w;
    *oh = h;
  } else {
    cw__wgpu_fail("screenshot readback failed (map status %d)", (int)mr.st);
  }
  wgpuBufferRelease(buf);
  return ok;
#endif
}

void cw__wgpu_shutdown(void) {
  if (!G.device) { /* an init that stopped part way (no adapter, no device) */
#if !CW_CART
    if (G.surface) wgpuSurfaceRelease(G.surface);
    if (G.adapter) wgpuAdapterRelease(G.adapter);
    if (G.instance) wgpuInstanceRelease(G.instance);
    if (G.native_win.metalView) nativeDawnSDL2Release(&G.native_win);
    memset(&G, 0, sizeof G);
#endif
    return;
  }
  if (G.frame_view) wgpuTextureViewRelease(G.frame_view);
  if (G.frame_tex) wgpuTextureRelease(G.frame_tex);
  if (G.offscreen) wgpuTextureRelease(G.offscreen);
#if !CW_CART
  if (G.surface) wgpuSurfaceUnconfigure(G.surface);
#endif
  if (G.surface) wgpuSurfaceRelease(G.surface);
  if (G.queue) wgpuQueueRelease(G.queue);
#if !CW_CART
  wgpuDeviceDestroy(G.device);
  wgpuInstanceProcessEvents(G.instance);
#endif
  wgpuDeviceRelease(G.device);
  if (G.adapter) wgpuAdapterRelease(G.adapter);
  if (G.instance) wgpuInstanceRelease(G.instance);
#if !CW_CART
  nativeDawnSDL2Release(&G.native_win);
#endif
  memset(&G, 0, sizeof G);
}
#endif

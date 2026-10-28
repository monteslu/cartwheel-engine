/* Window reads and GPU waits on either backend (cartwheel/gpu.h):
 * cw_gpu_read_window, cw_gpu_read_result, cw_gpu_finish.
 *
 * A read is queued when asked for and taken at the end of the frame
 * (cw__gpu_reads_frame_end, after pixel.c composed the picture into the
 * window), so it sees everything the frame put in the window whatever drew
 * it. GL reads at once (glReadPixels, rows flipped to top-down). WebGPU
 * copies the region of the frame's texture into a mappable buffer in an
 * encoder of its own, submitted after the frame's work, and maps it: the
 * pixels arrive when the map completes (natively at the next frame's events,
 * in a cart between frames; a cart never waits). */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/core.h"
#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#undef glBindFramebuffer /* reads the real window (0), after pixel.c's composite */
#if CW_WGPU
#include "wgpu.h"
#endif

enum { R_FREE, R_QUEUED, R_MAPPING, R_DONE, R_FAILED };
#define NREADS 8

typedef struct gpu_read {
  int state;
  uint32_t ticket;
  int x, y, w, h;
  uint8_t *px; /* top-down RGBA8, w * h * 4 */
  const cw_rt *rt; /* cw_gpu_read_rt: the target (rows bottom-up on both APIs), else the window */
#if CW_WGPU
  WGPUBuffer buf;
  uint32_t row; /* bytes per buffer row (256-aligned) */
  bool bgra, flip;
#endif
  bool dropped; /* cw_gpu_read_drop while it was on its way: released when it lands */
} gpu_read;

static gpu_read reads[NREADS];
static uint32_t next_ticket = 1;

static gpu_read *find(uint32_t ticket) {
  if (!ticket) return NULL;
  for (int i = 0; i < NREADS; i++)
    if (reads[i].state != R_FREE && reads[i].ticket == ticket) return &reads[i];
  return NULL;
}

static void release(gpu_read *r) {
#if CW_WGPU
  if (r->buf) wgpuBufferRelease(r->buf);
  r->buf = NULL;
#endif
  free(r->px);
  r->px = NULL;
  r->state = R_FREE;
  r->ticket = 0;
  r->rt = NULL;
  r->dropped = false;
}

uint32_t cw_gpu_read_window(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0 || x < 0 || y < 0) return 0;
  for (int i = 0; i < NREADS; i++) {
    gpu_read *r = &reads[i];
    if (r->state != R_FREE) continue;
    r->px = malloc((size_t)w * (size_t)h * 4);
    if (!r->px) return 0;
    r->state = R_QUEUED;
    r->ticket = next_ticket++;
    if (!next_ticket) next_ticket = 1;
    r->x = x, r->y = y, r->w = w, r->h = h;
    return r->ticket;
  }
  return 0; /* eight reads already waiting: the reader is behind */
}

static void take(gpu_read *r);

uint32_t cw_gpu_read_window_now(int x, int y, int w, int h) {
  uint32_t t = cw_gpu_read_window(x, y, w, h);
  if (t) take(find(t));
  return t;
}

uint32_t cw_gpu_read_rt(const cw_rt *rt, int x, int y, int w, int h) {
  if (!rt) return 0;
  uint32_t t = cw_gpu_read_window(x, y, w, h);
  if (!t) return 0;
  gpu_read *r = find(t);
  r->rt = rt;
  take(r);
  r->rt = NULL; /* taken: the target may go now */
  return t;
}

int cw_gpu_read_result(uint32_t ticket, void *rgba) {
  gpu_read *r = find(ticket);
  if (!r) return -1;
  if (r->state == R_FAILED) {
    release(r);
    return -1;
  }
  if (r->state != R_DONE) return 0;
  if (rgba) memcpy(rgba, r->px, (size_t)r->w * (size_t)r->h * 4);
  release(r);
  return 1;
}

void cw_gpu_read_drop(uint32_t ticket) {
  gpu_read *r = find(ticket);
  if (!r) return;
  if (r->state == R_MAPPING) r->dropped = true;
  else release(r);
}

#if CW_WGPU
static void on_map(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) {
  (void)msg;
  gpu_read *r = u1;
  uint32_t ticket = (uint32_t)(uintptr_t)u2;
  if (r->ticket != ticket || r->state != R_MAPPING) return;
  if (r->dropped) {
    if (st == WGPUMapAsyncStatus_Success) wgpuBufferUnmap(r->buf);
    release(r);
    return;
  }
  if (st != WGPUMapAsyncStatus_Success) {
    r->state = R_FAILED;
    return;
  }
  const uint8_t *p = wgpuBufferGetConstMappedRange(r->buf, 0, (size_t)r->row * (size_t)r->h);
  if (!p) {
    wgpuBufferUnmap(r->buf);
    r->state = R_FAILED;
    return;
  }
  for (int yy = 0; yy < r->h; yy++) {
    const uint8_t *s = p + (size_t)(r->flip ? r->h - 1 - yy : yy) * r->row;
    uint8_t *d = r->px + (size_t)yy * (size_t)r->w * 4;
    if (!r->bgra) memcpy(d, s, (size_t)r->w * 4);
    else
      for (int xx = 0; xx < r->w; xx++) {
        d[4 * xx + 0] = s[4 * xx + 2];
        d[4 * xx + 1] = s[4 * xx + 1];
        d[4 * xx + 2] = s[4 * xx + 0];
        d[4 * xx + 3] = s[4 * xx + 3];
      }
  }
  wgpuBufferUnmap(r->buf);
  r->state = R_DONE;
}

WGPUTexture cw__pw_texture(unsigned id); /* render/pipeline_wgpu.c: a cw_rt's texture */
static void wgpu_take(gpu_read *r) {
  WGPUTexture tex = r->rt ? cw__pw_texture(r->rt->tex) : cw__wgpu_frame_texture();
  if (!tex) {
    if (r->rt) cw_error("cw_gpu_read_rt: the target has no texture to read (a cw_rt_new target can be read)");
    r->state = R_FAILED;
    return;
  }
  int tw = (int)wgpuTextureGetWidth(tex), th = (int)wgpuTextureGetHeight(tex);
  if (r->x + r->w > tw || r->y + r->h > th) {
    cw_error("%s: %d x %d at (%d, %d) is outside the %d x %d %s", r->rt ? "cw_gpu_read_rt" : "cw_gpu_read_window", r->w, r->h, r->x,
             r->y, tw, th, r->rt ? "target" : "window");
    r->state = R_FAILED;
    return;
  }
  /* a target keeps its rows bottom-up (GL's order): read the rows under the region, flipped */
  r->flip = r->rt != NULL;
  uint32_t oy = (uint32_t)(r->flip ? th - r->y - r->h : r->y);
  WGPUTextureFormat f = wgpuTextureGetFormat(tex);
  r->bgra = f == WGPUTextureFormat_BGRA8Unorm || f == WGPUTextureFormat_BGRA8UnormSrgb;
  r->row = ((uint32_t)r->w * 4 + 255) & ~255u;
  WGPUDevice dev = cw__wgpu_device();
  r->buf = wgpuDeviceCreateBuffer(dev, &(WGPUBufferDescriptor){ .label = CW_WGPU_SV("cw_gpu_read_window"),
                                                              .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                                              .size = (uint64_t)r->row * (uint64_t)r->h });
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, NULL);
  WGPUTexelCopyTextureInfo src = { .texture = tex,
                                   .origin = { (uint32_t)r->x, oy, 0 },
                                   .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferInfo dst = { .buffer = r->buf,
                                  .layout = { .bytesPerRow = r->row, .rowsPerImage = (uint32_t)r->h } };
  WGPUExtent3D ext = { (uint32_t)r->w, (uint32_t)r->h, 1 };
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(cw__wgpu_queue(), 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
  r->state = R_MAPPING;
  WGPUBufferMapCallbackInfo ci = {
#if CW_CART
    .mode = WGPUCallbackMode_AllowSpontaneous,
#else
    .mode = WGPUCallbackMode_AllowProcessEvents,
#endif
    .callback = on_map, .userdata1 = r, .userdata2 = (void *)(uintptr_t)r->ticket };
  wgpuBufferMapAsync(r->buf, WGPUMapMode_Read, 0, (size_t)r->row * (size_t)r->h, ci);
}
#endif

static void gl_take(gpu_read *r) {
  int ww, wh;
  if (r->rt) ww = r->rt->w, wh = r->rt->h;
  else cw_drawable_size(&ww, &wh);
  if (r->x + r->w > ww || r->y + r->h > wh) {
    cw_error("%s: %d x %d at (%d, %d) is outside the %d x %d %s", r->rt ? "cw_gpu_read_rt" : "cw_gpu_read_window", r->w, r->h, r->x, r->y,
             ww, wh, r->rt ? "target" : "window");
    r->state = R_FAILED;
    return;
  }
  uint8_t *tmp = malloc((size_t)r->w * (size_t)r->h * 4);
  if (!tmp) {
    r->state = R_FAILED;
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, r->rt ? r->rt->fbo : 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(r->x, wh - r->y - r->h, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, tmp); /* GL rows are bottom-up */
  if (r->rt) glBindFramebuffer(GL_FRAMEBUFFER, 0);
  size_t row = (size_t)r->w * 4;
  for (int yy = 0; yy < r->h; yy++) memcpy(r->px + (size_t)yy * row, tmp + (size_t)(r->h - 1 - yy) * row, row);
  free(tmp);
  r->state = R_DONE;
}

static void take(gpu_read *r) {
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    wgpu_take(r);
    return;
  }
#endif
  gl_take(r);
}

void cw__gpu_reads_frame_end(void) {
  for (int i = 0; i < NREADS; i++)
    if (reads[i].state == R_QUEUED) take(&reads[i]);
}

void cw__wgpu_process_events(void); /* gpu.c */
int cw_gpu_read_wait(uint32_t ticket, void *rgba) {
  gpu_read *r = find(ticket);
  if (!r) return -1;
#if CW_WGPU && !CW_CART
  /* natively the GPU may be waited for: deliver the map (10 s at most) */
  for (int i = 0; i < 10000 && r->state == R_MAPPING; i++) {
    cw__wgpu_process_events();
    if (r->state == R_MAPPING) SDL_Delay(1);
  }
#endif
  return cw_gpu_read_result(ticket, rgba);
}

bool cw_gpu_finish(void) {
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
#if CW_CART
    return false; /* a cart never waits for the GPU */
#else
    cw__wgpu_wait_idle();
    return true;
#endif
  }
#endif
  uint8_t px[4];
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); /* waits for everything before it */
  return true;
}

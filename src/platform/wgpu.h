/* Engine-internal side of the GPU device module (cartwheel/gpu.h): the
 * WebGPU handles the renderers share. Only compiled with CW_WGPU. */
#ifndef CW_PLATFORM_WGPU_H
#define CW_PLATFORM_WGPU_H

#include <webgpu/webgpu.h>

#include "cartwheel/gpu.h"

#define CW_WGPU_SV(s) ((WGPUStringView){ (s), WGPU_STRLEN })

bool cw__wgpu_on(void); /* this run renders with WebGPU */
WGPUDevice cw__wgpu_device(void);
WGPUQueue cw__wgpu_queue(void);
const WGPULimits *cw__wgpu_limits(void);
/* the window's texture this frame (acquired on first use, presented or
 * handed to the host at the end of the frame), its format and size */
WGPUTexture cw__wgpu_frame_texture(void);
WGPUTextureView cw__wgpu_frame_view(void);
WGPUTextureFormat cw__wgpu_frame_format(void);
void cw__wgpu_frame_size(int *w, int *h);
/* a WebGPU error found by the engine itself (not reported by Dawn) */
void cw__wgpu_fail(const char *fmt, ...);
/* native runs only: the device has the timestamp-query feature (requested
 * when the adapter offers it; CW_WGPU_TIMESTAMPS=0 turns it off). Never in a
 * cart: the tier keeps timestamp queries off unless requested. */
bool cw__wgpu_timestamps(void);
/* WGSL -> shader module; NULL (logged) on a compile error */
WGPUShaderModule cw__wgpu_shader(const char *label, const char *wgsl);

/* run fn at the end of every frame, after the frame's work is submitted
 * and before it is presented (asynchronous readbacks map here) */
void cw__wgpu_add_frame_end_hook(void (*fn)(void));
/* native: wait for the GPU to finish all submitted work (perf runs); no-op in carts */
void cw__wgpu_wait_idle(void);

#endif

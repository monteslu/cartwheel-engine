/* The render pipeline on WebGPU (pipeline_wgpu.c): engine-internal. Only
 * compiled with CW_WGPU; other modules that record their own WebGPU passes
 * (effects, sky, visibility) include it.
 *
 * Model:
 *   - every cw_rt is a set of ids in one registry: rt->fbo names the target
 *     (its attachments), rt->tex the colour texture, rt->depth_tex the depth
 *     ATTACHMENT (Depth32Float). Ids are never 0; fbo 0 / tex 0 is the output
 *     when it is the window (the frame texture of cartwheel/gpu.h).
 *   - one command encoder per pipeline frame (cw__pw_encoder): every pass
 *     records into it; queue writes (uniform blocks, uploads) made during the
 *     frame land before its submit (cw__pw_flush: after POST, before the
 *     OVERLAY phase whose 2D renderer submits its own work, and at the end).
 *   - offscreen targets keep GL's row order: full-screen passes into them
 *     negate clip y (as render2d does), so UVs, gl_FragCoord-style math and
 *     the texture memory match GL row for row. The window is not flipped.
 *   - depth: the scene's Depth32Float attachment holds GL-convention window
 *     depth ([0, 1]: draws remap clip z to (z + w) / 2). Passes that SAMPLE
 *     depth read an R32Float linear depth (view-space distance along -z,
 *     positive; 3e38 where nothing was drawn) made by a copy pass after the
 *     opaque phase (ctx->depth_tex). */
#ifndef CW_PIPELINE_WGPU_H
#define CW_PIPELINE_WGPU_H

#include "cartwheel/pipeline.h"

#if CW_WGPU
#include "platform/wgpu.h"

/* What a pass that records its own render passes needs (SKY / EFFECTS /
 * OPAQUE): the frame encoder and the phase target's views. */
typedef struct cw__wgpu_pass_targets {
  WGPUCommandEncoder enc;          /* the frame encoder: open and end your render passes on it */
  WGPUTextureView color;           /* the phase target's colour attachment */
  WGPUTextureFormat color_format;
  WGPUTextureView depth;           /* its Depth32Float attachment (LOAD / STORE; never sample it) */
  WGPUTextureFormat depth_format;
  WGPUTextureView linear_depth;    /* R32Float linear depth to sample (SKY / EFFECTS / POST; NULL before) */
  int w, h;
  bool flip_y;                     /* true: offscreen rows bottom-up (negate clip y) */
  int samples;                     /* 4 in OPAQUE with MSAA (color / depth multisampled), else 1 */
  WGPUTextureView resolve;         /* MSAA: the scene colour to resolve into, else NULL */
} cw__wgpu_pass_targets;
const cw__wgpu_pass_targets *cw__pass_wgpu_targets(cw_pass_ctx *ctx);

/* the registry */
WGPUTexture cw__pw_texture(unsigned id);
WGPUTextureView cw__pw_view(unsigned id);   /* sampled view */
WGPUTextureView cw__pw_attach(unsigned id); /* render-attachment view */
WGPUTextureFormat cw__pw_format(unsigned id);
/* a sampled texture of your own in the registry (cw_pass_bind_texture takes
 * the id); the registry owns tex / view from then on */
unsigned cw__pw_register(WGPUTexture tex, WGPUTextureView view);
void cw__pw_release(unsigned id);
/* a target drawing into layer 0 of an embedder's colour texture (no depth), and its release */
unsigned cw__pw_target_wrap(WGPUTexture color, int w, int h);
void cw__pw_target_unwrap(unsigned target);
/* upload a 2D texture (rows top-down as given; for GL parity pass rows the
 * way glTexImage2D would get them: row 0 = v 0) */
unsigned cw__pw_texture_2d(int w, int h, WGPUTextureFormat f, const void *data, uint32_t bytes_per_row);
/* a size^3 RGBA16Float 3D texture (a LUT; r fastest, as glTexImage3D takes it) */
unsigned cw__pw_texture_3d(int size, const uint16_t *rgba_half);

/* the frame encoder (made on demand) and its submit */
WGPUCommandEncoder cw__pw_encoder(void);
/* a 256-aligned uniform block for this frame's submit: returns the buffer
 * and offset to bind (valid until the flush) */
WGPUBuffer cw__pw_ubo(const void *data, size_t size, uint64_t *offset);
/* a bind group / view to release after the flush */
void cw__pw_defer_bind_group(WGPUBindGroup g);
#endif

/* backend-neutral entry points pipeline.c / pipeline_gl.c call (stubs
 * without CW_WGPU) */
bool cw__pw_rt_create(cw_rt *rt, int w, int h, cw_rt_format f, bool depth);
void cw__pw_rt_destroy(cw_rt *rt);
/* the scene's 4x MSAA target resolving into scene->tex (a cw_rt whose fbo
 * names the MSAA attachments) */
bool cw__pw_msaa_create(cw_rt *msaa, const cw_rt *scene);
/* the same MSAA attachments resolving into another scene colour of their size */
void cw__pw_msaa_retarget(cw_rt *msaa, const cw_rt *scene);
/* core WebGPU: the MSAA depth's sample 0 into scene->depth_tex (as GL's resolving blit) */
void cw__pw_depth_resolve(const cw_rt *msaa, const cw_rt *scene);
/* a core WebGPU device (false in compatibility mode and without WebGPU) */
bool cw__pw_core(void);
/* R32F linear depth from the depth attachment of src (multisampled: core WebGPU only) */
void cw__pw_depth_copy(cw_pass_ctx *ctx, const cw_rt *src, const cw_rt *dst_r32f, const float proj_z[4]);
void cw__pw_bind_target(const cw_rt *rt);
void cw__pw_state_reset(void);
void cw__pw_frame_begin(void);
/* submit the frame encoder (after POST, before OVERLAY; and at the end) */
void cw__pw_flush(void);
void cw__pw_probe_caps(cw_pipeline_caps *caps);
/* GPU timing (native, the adapter's timestamp-query feature): the pass the
 * following render passes belong to (NULL = none), and delivery of results
 * that arrived (1-3 frames late) */
void cw__pw_timing_pass(cw_pipeline *p, const char *pass_name, const char *lane_name);
void cw__pw_timing_poll(void);
/* hold: the flushes keep the frame's timestamps unread (splitscreen's views
 * submit one by one; the frame's last submit reads them all back) */
void cw__pw_timing_hold(bool hold);
/* a timed span round work recorded on enc by someone else (three.c's scene
 * and shadow passes): an empty compute pass writes its start (end false),
 * another its end; the span is the current pass's (cw__pw_timing_pass) */
void cw__pw_timing_span(void *enc, bool end);
/* the current pass timed whole, as one span on the frame encoder (begin: end false); the profiler's GPU lane */
void cw__pw_timing_whole(bool end);
void cw__pw_forget(cw_pipeline *p);
/* pipeline.c: a timed pass's GPU milliseconds arrived */
void cw__pipeline_gpu_time(cw_pipeline *p, const char *pass_name, float ms);

#endif

/* Render pipeline internals shared by pipeline.c (graph, pool, timing) and
 * pipeline_post.c (the built-in passes). */
#ifndef CW_PIPELINE_INTERNAL_H
#define CW_PIPELINE_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/gpu.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"

/* program states no table had (src/render/programs.c) */
extern int32_t cw__missing_programs;
const char *cw__programs_fix_command(void);

#define CW__QUERY_RING 64 /* a pass's queries in flight: up to four views a frame, many frames deep (a run flat out queues a lot) */

/* GPU pass timers (GL timer queries, WebGPU timestamps) are issued and polled
 * only when someone reads them: CW_GPU_TIMERS=1, or after the debug channel's
 * "pipeline" command. Natively every availability check is a full driver sync
 * (Mesa's glthread), which cost a frame 1-3 ms of main-thread CPU when the
 * timers ran unconditionally. pipeline.c. */
bool cw__gpu_timing_wanted(void);
void cw__gpu_timing_request(void); /* turn them on (Auto quality measures the GPU: quality.c) */

typedef struct pass_rec {
  cw_pass_desc d;
  const char *name; /* interned */
  int seq;          /* registration order (stable sort key) */
  bool enabled;     /* user switch */
  bool active;      /* this frame */
  bool wgpu_warned; /* skipped on WebGPU (no webgpu flag): logged once */
  /* timing */
  unsigned q[CW__QUERY_RING];
  bool q_busy[CW__QUERY_RING];
  uint64_t q_frame[CW__QUERY_RING]; /* the frame each query timed */
  uint64_t q_eframe[CW__QUERY_RING]; /* the same as the engine's frame index (cw_frame_index, for the profiler) */
  bool q_init;
  /* a frame's GPU time summed over its runs (splitscreen times a pass once
   * per view): the frame being summed, and the sum so far */
  uint64_t acc_frame;
  uint64_t missed[8]; /* frame + 1 of recent frames with a run left untimed (never published) */
  float acc_ms;
  bool acc_on;
  float frame_cpu; /* this frame's CPU ms, every run */
  cw_pass_stat stat;
} pass_rec;

typedef struct pool_rt {
  cw_rt rt;
  bool depth;
  bool used;
  uint64_t last_frame;
} pool_rt;

typedef struct persist_rt {
  const char *key; /* interned */
  cw_rt rt;
  bool depth;
} persist_rt;

struct cw_pipeline {
  cw_tree *tree;
  CW_ARRAY(pass_rec *) passes;
  bool order_dirty;
  int next_seq;
  cw_post_settings s;
  char *missing_states;    /* the states three.c's generated table lacked last frame */
  CW_ARRAY(pool_rt *) pool;
  CW_ARRAY(persist_rt *) persist;
  /* the scene's MSAA target (renderbuffers, resolved into the scene rt) */
  unsigned msaa_fbo, msaa_color, msaa_depth;
  int msaa_w, msaa_h, msaa_samples;
  cw_rt_format msaa_format;
  int msaa_failed; /* format + 1 the host could not multisample */
  cw_rt msaa_rt;   /* WebGPU: the 4x MSAA attachments (resolve into the scene) */
  cw_rt screen;              /* the default framebuffer as a target */
  const cw_rt *output_user;  /* cw_pipeline_set_output */
  void (*app_render)(void *user, double alpha);
  void *app_user;
  uint64_t frame;
  void (*view_hook)(int view, void *user); /* cw_pipeline_set_view_hook */
  void *view_hook_user;
  int run; /* render runs so far this frame (splitscreen: one per view, then the overlays) */
  float views_cpu_ms; /* splitscreen: the views' 3D CPU this frame (cw_render3d_stats.cpu_ms) */
  t3_mat4 prev_view_proj;
  bool have_prev;
  int prev_w, prev_h;
  /* splitscreen: each view's own previous view-projection (motion vectors and
   * reprojection follow that view's camera, not the view drawn before it) */
  struct cw__view_prev { t3_mat4 vp; bool have; int w, h; } view_prev[CW_MAX_VIEWS];
  bool velocity_wanted, depth_copy_wanted, prepass_ran;
  bool drew3d; /* the scene pass ran this frame (the canvas pass clears when not) */
  /* stats for cw_pipeline_pass_stats, in run order */
  CW_ARRAY(cw_pass_stat) stats;
  float gpu_total_ms;
  /* the jittered inverse view-projection, for the velocity pass */
  t3_mat4 inv_view_proj_jittered, inv_proj_jittered;
  unsigned bound_fbo; /* the FBO the current pass was handed */
  bool taa_reset;     /* drop the TAA history next frame */
  cw_view views[CW_MAX_VIEWS]; /* cw_pipeline_set_views (nviews >= 2: splitscreen) */
  int nviews;
  int view_i;               /* splitscreen: the view being drawn (-1: none) */
  const cw_rt *view_pic;    /* a shared post chain's view: its scene target is its picture (no copy) */
  bool views_shared_post; /* cw_pipeline_set_views_shared_post */
  bool view_post_split;   /* this frame: the views run the depth-reading post passes, the overlay run the rest */
  const cw_rt *post_src;  /* the overlay run's input: the views laid out (shared post) */
  int taa_frames;     /* frames accumulated in the TAA history */
  /* post fusion this frame, GL and WebGPU (pipeline.c decides,
   * pipeline_post.c draws): the height fog inside the SSAO composite (not
   * under TAA), bloom's composite and the LUT grade inside the tonemap pass,
   * the lens effects inside the anti-aliasing pass (SMAA's blend, FXAA) or, with no
   * anti-aliasing, inside the tonemap pass; each fused step rounds its value
   * as the target the separate pass wrote would (RGBA16F, RGBA8), so the
   * picture is the same with up to five full-screen passes fewer */
  bool fuse_fog, fuse_bloom, fuse_grade, fuse_lens, fuse_lens_tm;
  const cw_rt *bloom_glow;   /* fuse_bloom: bloom's glow, released by the tonemap pass */
  float bloom_texel[2], bloom_norm;
};

/* CW_TEST_FUSION_RAW=1 (native test control): the fused post passes skip
 * the rounding that keeps them equal to the separate passes */
bool cw__fusion_raw(void);
/* the fused passes' rounding helpers and the lens step, GLSL and WGSL
 * (pipeline_post.c); the lens step needs the common helpers (luma, hash01) */
const char *cw__fuse_round_glsl(void);
const char *cw__fuse_round_wgsl(void);
const char *cw__post_common_glsl(void);
const char *cw__fuse_lens_glsl(void);
const char *cw__fuse_lens_wgsl(void);
/* the lens uniforms (u_on, u_amt, u_frame) into a program in use; ca: chromatic aberration on */
void cw__lens_uniforms(cw_pass_ctx *ctx, unsigned prog, bool ca);
/* break a fused program too: CW_TEST_BREAK_PROGRAM=<step> names a step that
 * runs inside another pass's program (pipeline_wgpu.c) */
bool cw__program_has_step(const char *program, const char *step);
/* the height fog fused into the SSAO composite (fog.c): its uniform
 * declarations and fog_eval(uv, depth) in GLSL, and its uniforms into the
 * fused program in use */
const char *cw__fog_glsl(void);
void cw__fog_fused_uniforms(cw_pass_ctx *ctx, unsigned prog);
/* engine-internal target formats: R32F (the WebGPU linear depth), RG8 (SMAA's edges) */
#define CW__RT_R32F ((cw_rt_format)CW_RT_FORMAT_COUNT)
#define CW__RT_RG8 ((cw_rt_format)(CW_RT_FORMAT_COUNT + 1))

extern cw_pipeline_caps cw__caps;
/* WebGPU side of the pass helpers (pipeline_wgpu.c; stubs without CW_WGPU) */
unsigned cw__pw_program(const char *name, const char *wgsl);
void cw__pw_use(unsigned prog);
void cw__pw_fullscreen(void);
void cw__pw_draw(int vertices);
void cw__pw_bind_texture(int unit, unsigned tex);
void cw__pw_clear(float r, float g, float b, float a, bool depth);
void cw__pw_set_blend(cw_pass_blend b);
void cw__pw_set_depth(bool test, bool write);
/* n floats (n < 0: one int iv) into a member of the program's block */
void cw__pw_uniform(unsigned prog, const char *name, int n, const float *f, int iv);
unsigned cw__pw_lut(int size, const uint16_t *half);
unsigned cw__pw_rg8(int w, int h, const uint8_t *rg);
/* cw_pass_texture_new on WebGPU: texels as glTexImage2D takes them */
unsigned cw__pw_pass_texture(int w, int h, cw_rt_format f, const void *texels);
void cw__pw_tex_free(unsigned id);
/* a program for this run's backend: GLSL on GL, WGSL on WebGPU */
unsigned cw__pass_program2(const char *name, const char *glsl, const char *wgsl);
void cw__pipeline_probe_caps(void);
/* built-in passes (pipeline_post.c) */
void cw__pipeline_add_builtin_passes(cw_pipeline *p);
/* GL helpers (pipeline_gl.c) */
bool cw__rt_create(cw_rt *rt, int w, int h, cw_rt_format f, bool depth);
/* the persistent target `key` as it is now (any size and format), NULL when there is none:
 * for a reader that must not resize or recreate what its writer kept */
const cw_rt *cw__pipeline_rt_find(cw_pipeline *p, const char *key);
void cw__rt_destroy(cw_rt *rt);
void cw__pass_state_reset(void);
/* copy a texture into a target with a plain shader (any formats) */
void cw__pass_copy(cw_pass_ctx *ctx, unsigned src_tex, const cw_rt *dst);
/* the LUT texture of a LUT id (0 when unknown) */
unsigned cw__lut_texture(uint64_t lut, int *size, float dmin[3], float dmax[3]);


#endif

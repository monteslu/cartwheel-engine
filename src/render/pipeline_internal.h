/* Render pipeline internals shared by pipeline.c (graph, pool, timing) and
 * pipeline_post.c (the built-in passes). */
#ifndef CW_PIPELINE_INTERNAL_H
#define CW_PIPELINE_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/gpu.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"

#define CW__QUERY_RING 4

/* GPU pass timers (GL timer queries, WebGPU timestamps) are issued and polled
 * only when someone reads them: CW_GPU_TIMERS=1, or after the debug channel's
 * "pipeline" command. Natively every availability check is a full driver sync
 * (Mesa's glthread), which cost a frame 1-3 ms of main-thread CPU when the
 * timers ran unconditionally. pipeline.c. */
bool cw__gpu_timing_wanted(void);

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
  bool q_init;
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
  t3_mat4 prev_view_proj;
  bool have_prev;
  int prev_w, prev_h;
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
  int taa_frames;     /* frames accumulated in the TAA history */
  /* GL post fusion this frame (pipeline.c decides, pipeline_post.c draws):
   * bloom's composite and the LUT grade run inside the tonemap pass, the
   * lens effects inside SMAA's blend; each fused step rounds its value as
   * the target the separate pass wrote would (RGBA16F, RGBA8), so the
   * picture is the same with two to three full-screen passes fewer */
  bool fuse_bloom, fuse_grade, fuse_lens;
  const cw_rt *bloom_glow;   /* fuse_bloom: bloom's glow, released by the tonemap pass */
  float bloom_texel[2], bloom_norm;
};

/* CW_TEST_FUSION_RAW=1 (native test control): the fused post passes skip
 * the rounding that keeps them equal to the separate passes */
bool cw__fusion_raw(void);
/* the fused passes' rounding helpers, GLSL (pipeline_post.c) */
const char *cw__fuse_round_glsl(void);
/* an engine-internal target format: R32F (the WebGPU linear depth) */
#define CW__RT_R32F ((cw_rt_format)CW_RT_FORMAT_COUNT)

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

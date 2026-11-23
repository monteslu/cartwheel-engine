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
/* A warm-up slice is rendering (src/scene/runner.c): only part of the scene is shown, so what the gi pass would capture
 * (the probe grid's bake, reflection probes) would keep a partial scene's light for good: it waits for a full render. */
extern bool cw__warm_partial;
const char *cw__programs_fix_command(void);
/* an extension hook setting a value one variant of its part lacks on purpose: on around those sets, the pipeline's
 * report of values a program lacks (a table older than its extension) leaves them out */
extern bool cw__ext_miss_quiet;
const char *cw__app_target_name(void); /* the app's target (its generated <app>_programs.c), src/render/programs.c */

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
  const char *q_pname[CW__QUERY_RING]; /* the profiler's name for the run (split screen: "ssao@v1") */
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

/* one-pass split screen: the view whose pass runs now (-1: none; src/render/pipeline.c array_view) */
extern int cw__array_view_now;

struct cw_pipeline {
  cw_tree *tree;
  CW_ARRAY(pass_rec *) passes;
  CW_ARRAY(pass_rec *) steps; /* GPU work between passes (the views' layout, MSAA resolves, depth copies): timed, never run as passes */
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
  /* WebGPU, this run: the scene pass leaves its multisampled colour unresolved because the viewmodel pass draws
   * into it next and resolves at its end (one resolve of the scene colour, not two) */
  bool msaa_keep;
  int32_t msaa_resolves, msaa_resolves_frame; /* the scene colour's resolves this frame (all views), and last frame's (cw.msaa_resolves) */
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
  unsigned bound_depth_tex, bound_depth_rb; /* GL: its depth (a texture, or the multisampled scene's renderbuffer), 0 for none */
  bool taa_reset;     /* drop the TAA history next frame */
  /* the last frame's velocity target (the velocity pass alternates two), NULL when there is none: TAA tells
   * a surface the history saw move away (disocclusion) from one it follows */
  const cw_rt *velocity_prev;
  uint64_t velocity_frame;
  /* automatic exposure (pipeline_post.c): the state the tonemap pass reads this run (0: none), the frame each view's
   * state was last moved on, and cw_pipeline_exposure_reset */
  unsigned auto_exposure_tex;
  uint64_t auto_exposure_seen[CW_MAX_VIEWS];
  bool auto_exposure_reset;
  cw_view views[CW_MAX_VIEWS]; /* cw_pipeline_set_views (nviews >= 2: splitscreen) */
  int nviews;
  int view_i;               /* splitscreen: the view being drawn (-1: none); an extra view (below) draws as a later one */
  /* extra views (cartwheel/subviewport.h): cameras drawn into targets of their own after the frame's views, as
   * later splitscreen views are (the frame's shadow maps and world work are reused). Queued each frame. */
  struct cw__extra_view { cw_node camera; const cw_rt *rt; int slot; void (*done)(void *user, float cpu_ms); void *user; } extra[16];
  int nextra;
  const char *rt_scope;     /* an extra view's run: its persistent targets are its own (the key gets this suffix) */
  const cw_rt *view_pic;    /* a shared post chain's view: its scene target is its picture (no copy) */
  bool views_shared_post; /* cw_pipeline_set_views_shared_post */
  bool view_post_split;   /* this frame: the views run the depth-reading post passes, the overlay run the rest */
  /* split screen in one pass (cw_pipeline_set_views_array) */
  bool views_array, arr_used;
  bool views_submit; /* WebGPU: each view of a one-pass run submitted as it is drawn (arr_part; "pipeline views_submit 0|1") */
  const char *arr_why;      /* the first active pass that could not (interned), last frame */
  t3_array_camera *arr_cam;
  t3_camera *arr_cams[CW_MAX_VIEWS];
  cw_node arr_nodes[CW_MAX_VIEWS];
  int arr_rect[CW_MAX_VIEWS][4]; /* pixels from the bottom left */
  int arr_n;
  /* WebGPU, one-pass split screen: the views' shadow-phase passes and scene drawn view by view in the one target, each
   * view's scene submitted as it is recorded (the GPU starts on view 0 while the CPU prepares view 1): the view being
   * drawn so plus one (0: not so), the array camera of that one view, and each view's viewport as the array camera has it */
  int arr_part;
  t3_array_camera *arr_cam1;
  int arr_vp[CW_MAX_VIEWS][4];
  const char *arr_refused;  /* set by a RUN_ARRAY run that found such a pass (nothing drawn) */
  const cw_rt *post_src;  /* the overlay run's input: the views laid out (shared post) */
  int taa_frames;     /* frames accumulated in the TAA history */
  /* post fusion this frame, GL and WebGPU (pipeline.c decides,
   * pipeline_post.c draws): the height fog inside the SSAO composite (not
   * under TAA), bloom's composite and the LUT grade inside the tonemap pass,
   * the lens effects inside the anti-aliasing pass (SMAA's blend, FXAA) or, with no
   * anti-aliasing, inside the tonemap pass; each fused step rounds its value
   * as the target the separate pass wrote would (RGBA16F, RGBA8), so the
   * picture is the same with up to five full-screen passes fewer */
  bool fuse_fog, fuse_bloom, fuse_grade, fuse_lens, fuse_lens_tm, fuse_distort;
  bool fuse_aa_tm;  /* FXAA inside the tonemap pass (effects detail Very Low) */
  bool fuse_fog_tm; /* the height fog inside the tonemap pass (effects detail Very Low) */
  const cw_rt *bloom_glow;   /* fuse_bloom: bloom's glow, released by the tonemap pass */
  float bloom_texel[2], bloom_norm;
  /* backdrops (src/render/backdrop.c, cartwheel/backdrop.h): bd_frame renders this frame's pictures at the start of
   * cw_pipeline_render (set while the tree has Backdrop nodes); bd the pictures in draw order, bd_bg_weight the share
   * of the tree's own background left under them; bd_drawn: this run's "backdrop" pass drew them, so the scene pass
   * keeps the colour and draws no background */
  void (*bd_frame)(cw_tree *t, cw_pipeline *p, int w, int h, double alpha);
  struct cw__backdrop_pic { unsigned tex; float opacity, scale; } bd[8];
  int bd_n;
  float bd_bg_weight;
  bool bd_drawn;
  int bd_w, bd_h; /* the frame's size, which the pictures have */
  /* the backdrops whose worlds light the tree (backdrop.h environment): the backdrop, its world and camera, its picture
   * in bd[]; then (the "backdrop_env" pass, which captures them) its last whole capture (six faces of size x size, as
   * cube faces; NULL before the first) and whether it finished this frame */
  struct cw__backdrop_env { void *bd; cw_tree *world; cw_node camera; int pic; const cw_rt *faces; int size; bool fresh; } bd_env[8];
  int bd_env_n;
  /* the screen-space reflections' view of the backdrops (cw_post_settings.ssr_backdrop): the pictures blended at half
   * size, made this frame when bd_ssr_frame is the frame */
  const cw_rt *bd_ssr;
  uint64_t bd_ssr_frame;
  /* this pipeline is drawing a backdrop's picture (cw__pipeline_render_backdrop): the passes up to the colour-only
   * post chain, the picture left in bd_pic */
  bool backdrop_run;
  const cw_rt *bd_pic;
};
/* a backdrop's world into its picture at w x h through camera (null: the world's own), its HDR scene target or the
 * last depth-reading post pass's; NULL when the world has nothing to draw (pipeline.c) */
const cw_rt *cw__pipeline_render_backdrop(cw_tree *world, cw_node camera, int w, int h, double alpha);
/* the share of the tree's own background its backdrops leave this frame (1: none drawn; backdrop.c) */
float cw__backdrop_bg_weight(const cw_pipeline *p);
/* the backdrops as screen-space reflections see them this frame (backdrop.c): the half-size composite (premultiplied,
 * alpha their coverage) and the rectangle of this run's view in it (uv, from the bottom left); false when there is none */
bool cw__backdrop_ssr(const cw_pass_ctx *ctx, unsigned *tex, float rect[4]);
/* the tree's environment while its backdrops light it (their blended captures, prefiltered), NULL otherwise: what a
 * planar reflection (Water3D) draws where its mirrored view finds nothing of the scene (backdrop.c) */
struct t3_texture *cw__backdrop_env_texture(cw_tree *t);

/* CW_TEST_FUSION_RAW=1 (native test control): the fused post passes skip
 * the rounding that keeps them equal to the separate passes */
bool cw__fusion_raw(void);
/* the fused passes' rounding helpers and the lens step, GLSL and WGSL
 * (pipeline_post.c); the lens step needs the common helpers (luma, hash01) */
const char *cw__fuse_round_glsl(void);
/* src/fx/distort.c: the distortion buffer this view made (NULL when none), taken by the tonemap pass when
 * the distortion is fused into it (PostProcess fuse_distortion), and the apply's uniforms */
const cw_rt *cw__fx_take_distortion(cw_tree *t);
void cw__fx_distortion_uniforms(cw_pass_ctx *ctx, unsigned prog, bool on);
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
/* and the small HDR scene (effects detail Very Low): R11G11B10 float, no alpha, half RGBA16F's bytes, where the device
 * can render and blend it (GL: an FBO with one is complete; WebGPU: the rg11b10ufloat-renderable feature) */
#define CW__RT_RG11B10F ((cw_rt_format)(CW_RT_FORMAT_COUNT + 2))
extern bool cw__small_hdr_render;
/* (GLES 3.0 names some GL headers leave out: the cart's) */
#ifndef GL_R11F_G11F_B10F
#define GL_R11F_G11F_B10F 0x8C3A
#endif
#ifndef GL_UNSIGNED_INT_10F_11F_11F_REV
#define GL_UNSIGNED_INT_10F_11F_11F_REV 0x8C3B
#endif

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
/* GL uniform values the engine's own programs (cw_pass_program*, the fx programs) already hold:
 * cw__gl_uniform_held is true when (prog, loc) was last set to these bytes (at most 64), so the
 * glUniform call is skipped (a cart pays a wasm -> host crossing per GL call; a pass or a particle
 * system sets the same samplers and constants every frame). It records the bytes when they differ.
 * cw__gl_program_born forgets a program id's values (GL reuses ids): every program those setters
 * write is made through it. */
bool cw__gl_uniform_held(unsigned prog, int loc, const void *bytes, int n);
void cw__gl_program_born(unsigned prog);
uint32_t cw__gl_program_epoch(unsigned prog);
void cw__pipeline_probe_caps(void);
/* velocity.c: the meshes that moved since the last frame, drawn over the camera's motion in the velocity
 * target (the scene's depth tests them); returns how many were drawn */
int cw__velocity_objects(cw_pass_ctx *ctx, const cw_rt *vel);
/* built-in passes (pipeline_post.c) */
void cw__pipeline_add_builtin_passes(cw_pipeline *p);
/* GL helpers (pipeline_gl.c) */
bool cw__rt_create(cw_rt *rt, int w, int h, cw_rt_format f, bool depth);
/* the persistent target `key` as it is now (any size and format), NULL when there is none:
 * for a reader that must not resize or recreate what its writer kept */
const cw_rt *cw__pipeline_rt_find(cw_pipeline *p, const char *key);
/* Queue an extra view for this frame (drawn at the end of cw_pipeline_render, see struct cw_pipeline): camera into
 * rt (its size), slot naming its own set of persistent targets; done runs after it with the CPU ms it took to
 * issue. false when the frame's queue is full. */
bool cw__pipeline_queue_view(cw_pipeline *p, cw_node camera, const cw_rt *rt, int slot, void (*done)(void *user, float cpu_ms), void *user);
void cw__rt_destroy(cw_rt *rt);
void cw__pass_state_reset(void);
/* split screen in one pass: the view rectangle the pass helpers scissor to (pixels of a full_w x full_h target,
 * from the bottom left); on = false outside a view run. A bound target of another size gets the rectangle scaled
 * (a half-size intermediate: half the rectangle). */
typedef struct cw__view_clip { bool on; int rect[4]; int full_w, full_h; int bound_w, bound_h; } cw__view_clip;
extern cw__view_clip cw__vclip;
/* the rectangle scaled to a target of w x h */
void cw__view_clip_rect(int w, int h, int out[4]);
/* the view's rectangle in a w x h target's framebuffer rows (top_down: row 0 at the top, as a WebGPU pass on a target
 * not drawn flipped); the whole target outside a one-pass split screen's view */
void cw__view_fb_rect(const cw_pass_ctx *ctx, int w, int h, bool top_down, int out[4]);
void cw__pw_set_scissor(const int *rect); /* WebGPU: NULL = the whole target */
/* a pass's split screen capability, from the next frame (cw_pass_desc.views: a pass that can only sometimes) */
void cw__pipeline_set_pass_views(cw_pipeline *p, const char *name, uint8_t views);
/* change a pass's reads (looks.c: the DISPLAY stage under the shared split screen post chain) */
void cw__pipeline_set_pass_reads(cw_pipeline *p, const char *name, uint32_t reads);
/* a one-pass split screen's pass that finds it cannot do this frame (in the SHADOW phase, before anything is drawn):
 * the frame is drawn view by view instead. Does nothing outside such a run. */
void cw__pipeline_array_refuse(cw_pass_ctx *ctx, const char *why);
/* copy a texture into a target with a plain shader (any formats) */
void cw__pass_copy(cw_pass_ctx *ctx, unsigned src_tex, const cw_rt *dst);
/* the LUT texture of a LUT id (0 when unknown) */
unsigned cw__lut_texture(uint64_t lut, int *size, float dmin[3], float dmax[3]);


#endif

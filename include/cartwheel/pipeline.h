/* The render pipeline: a small per-tree frame graph of passes.
 *
 * Every tree that draws 3D owns one pipeline (created on first use). The
 * scene runner (cw_scene_app) renders through it every frame, so scene apps
 * get it without doing anything; other apps call cw_pipeline_render. A frame
 * runs the active passes phase by phase, and inside a phase by `order`
 * (ascending; ties in registration order):
 *
 *   CW_PHASE_SHADOW    targets of the pass's own: nothing is bound for it.
 *                      (three.c still renders its own light shadows inside
 *                      the scene pass.) Module
 *                      pass: "visibility" (2000, visibility.h: culling, LOD,
 *                      impostor bakes; it hides culled meshes for the frame).
 *   CW_PHASE_PREPASS   depth-only draws into the scene FBO. The built-in
 *                      "depth_prepass" (settings.depth_prepass) is one. When
 *                      any prepass ran, the scene pass keeps that depth and
 *                      clears colour only.
 *   CW_PHASE_OPAQUE    the scene: the built-in "scene" pass (order 0) renders
 *                      the tree's three.c scene (opaque, background,
 *                      transparent, as three.c orders them) into the scene
 *                      FBO (HDR colour + depth texture, or the MSAA
 *                      renderbuffers). Passes after it here draw more
 *                      geometry with the same depth. Module passes:
 *                      "impostors" (100) and "occlusion_queries" (900:
 *                      bounding boxes against the finished depth),
 *                      visibility.h.
 *   CW_PHASE_SKY       background / atmosphere after the opaque scene, into
 *                      the scene FBO, depth-tested against the scene.
 *   CW_PHASE_EFFECTS   particles, decals, fog volumes: the scene FBO with its
 *                      depth attached for testing; ctx->depth_tex is a copy
 *                      to SAMPLE. (With MSAA the scene is resolved before
 *                      SKY, so these phases draw single-sampled.) Module
 *                      passes: "decals" (100, decals.h), "particles" (200,
 *                      particles.h: GPU particle simulation and drawing,
 *                      trails).
 *   CW_PHASE_POST      the full-screen chain. A pass that WRITES CW_RES_COLOR
 *                      reads ctx->src and writes ctx->dst; the pipeline
 *                      ping-pongs pooled targets and hands the last writer
 *                      the output as dst. A pass that only reads sees the
 *                      current image as src (after the last writer that is
 *                      the output: tex 0 when it is the screen). Built-ins:
 *                      "velocity" (0, when something reads CW_RES_VELOCITY),
 *                      "ssao" (100), "taa" (200), "bloom" (300), "tonemap"
 *                      (500, HDR to display), "grade" (550, 3D LUT), "aa"
 *                      (600, FXAA or SMAA), "lens" (700: chromatic
 *                      aberration, vignette, film grain), "debug_view"
 *                      (10000).
 *   CW_PHASE_OVERLAY   2D, UI and debug draw into the output (display
 *                      values, no depth). Built-ins: "canvas" (-100: the
 *                      tree's 2D pass and its cw_canvas_add_pass UI overlays,
 *                      cartwheel/scene2d.h; it clears first when no 3D was
 *                      drawn) and "app" (0: the app's render callback, so a
 *                      HUD lands on top). Debug draw at 1000 is the
 *                      convention. render2d draws to the window: with a
 *                      custom output (cw_pipeline_set_output) an overlay pass
 *                      targets ctx->output itself.
 *   CW_PHASE_PRESENT   last: readbacks, captures, a final scale when the
 *                      output is not the screen. Module pass:
 *                      "visibility_restore" (1000) shows what "visibility"
 *                      hid.
 *
 * The scene is always linear HDR: three.c (three.js r186's generated
 * programs, physical light units, colour management) renders it into the
 * RGBA16F scene target, and the "tonemap" POST pass applies exposure, the
 * tone curve and the sRGB transfer function.
 *
 * ── adding a pass (what another module does) ─────────────────────────
 *
 *   static void fog_exec(cw_pass_ctx *ctx, void *user) {
 *     // EFFECTS: the scene FBO is bound (colour + depth for testing)
 *     glUseProgram(fog_prog);
 *     cw_pass_bind_texture(0, ctx->depth_tex); // the copy: safe to sample
 *     glEnable(GL_BLEND);
 *     glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
 *     cw_pass_fullscreen();                    // one triangle, no buffers
 *   }
 *   if (!cw_pipeline_has_pass(cw_pipeline_of(tree), "height_fog"))
 *     cw_pipeline_add_pass(cw_pipeline_of(tree), &(cw_pass_desc){
 *         .name = "height_fog", .phase = CW_PHASE_EFFECTS, .order = 300,
 *         .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR,
 *         .active = fog_active, .execute = fog_exec, .user = my_state });
 *
 * A module registers its passes once per tree: from a node type's
 * enter_tree (a HeightFog node), from cw_tree_add_hook, or from app init.
 * Rules a pass follows:
 *   - Raw GL is fine. The pipeline forgets three.c's cached GL state
 *     (t3_renderer_reset_bindings) before every three.c render it makes. A
 *     pass that calls t3_renderer_render itself (shadow casters, probes)
 *     calls t3_renderer_reset_bindings(ctx->renderer) first and puts the
 *     renderer's framebuffer, render target and size back afterwards.
 *   - Before execute the pipeline binds the phase's target and viewport (see
 *     above), and leaves depth test, blending, culling and scissor OFF and
 *     the colour mask on; a pass enables what it uses. A pass may bind other
 *     targets (cw_pass_bind_target); POST writers must leave their result in
 *     ctx->dst, or in a target of their own named by ctx->result.
 *   - Declare what you read and write (CW_RES_*). The graph uses it: reading
 *     DEPTH or VELOCITY moves the scene offscreen, VELOCITY schedules the
 *     velocity pass, DEPTH in SKY / EFFECTS makes the depth copy, COLOR
 *     writes in POST join the ping-pong chain.
 *   - active() switches a pass per frame from settings (cheap, no GL); the
 *     user switch is cw_pipeline_set_pass_enabled.
 *   - Scratch targets come from the pool: cw_pass_rt(ctx, w, h, format) is
 *     yours until the frame ends (or cw_pass_rt_release). Targets that
 *     outlive a frame (history) are cw_pipeline_rt_persistent.
 *   - Every pass is timed on the GPU (GL_TIME_ELAPSED where the host has
 *     EXT_disjoint_timer_query; natively on WebGPU through timestamp
 *     queries, never in a cart) and on the CPU (its execute):
 *     cw_pipeline_pass_stats, the debug channel's "pipeline" command.
 *   - WebGPU (cartwheel/gpu.h): a pass runs there only with webgpu = true in
 *     its desc. It draws through the same helpers with a WGSL program
 *     (cw_pass_program_wgsl, cw_pass_use, cw_pass_set_blend / _depth,
 *     cw_pass_clear, cw_pass_fullscreen / cw_pass_draw); every pass records
 *     into one command encoder per pipeline frame. Offscreen targets keep
 *     GL's row order and depth is GL's [0, 1] window depth, so GLSL ports
 *     line for line. ctx->depth_tex is an R32F LINEAR depth there (view-space
 *     distance, 3e38 where nothing was drawn; read it with textureLoad /
 *     cw_load); MSAA serves RGBA8 scenes without depth readers
 *     (compatibility mode). three.c has no WebGPU backend yet: the "scene"
 *     pass is skipped, and an OPAQUE pass of the app's (webgpu = true) can
 *     draw the scene (samples/post_synth).
 *   - Under TAA the camera's projection_matrix carries this frame's jitter
 *     while the passes run (it is restored after); ctx->proj / view_proj are
 *     the unjittered ones.
 *   - Fragment shaders get highp samplers from cw_pass_program; GLSL ES
 *     defaults sampler2D to lowp in fragment shaders, which quantizes depth
 *     and HDR on some GPUs.
 *
 * The workflow is three.js r186's: material and environment colours are
 * linear, lights are physical units (see "Light units" below), and the post
 * chain works on scene-linear HDR until the tonemap pass.
 */
#ifndef CW_PIPELINE_H
#define CW_PIPELINE_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_pipeline cw_pipeline;

typedef enum {
  CW_PHASE_SHADOW, CW_PHASE_PREPASS, CW_PHASE_OPAQUE, CW_PHASE_SKY, CW_PHASE_EFFECTS, CW_PHASE_POST,
  CW_PHASE_OVERLAY, CW_PHASE_PRESENT, CW_PHASE_COUNT
} cw_pass_phase;

/* What a pass reads / writes (bit mask). */
enum {
  CW_RES_COLOR = 1 << 0,    /* the scene HDR colour (POST: the chain's current image) */
  CW_RES_DEPTH = 1 << 1,    /* the scene depth texture */
  CW_RES_VELOCITY = 1 << 2, /* screen-space motion, RG16F, uv units (current - previous) */
  CW_RES_OUTPUT = 1 << 3,   /* the final image (overlays, present) */
  CW_RES_SHADOW = 1 << 4,   /* shadow maps a SHADOW pass made (for documentation / ordering) */
};

/* Render target formats (all GLES 3.0 core except the float ones, which need
 * EXT_color_buffer_float or EXT_color_buffer_half_float to be rendered to;
 * cw_pipeline_caps says what the host has). */
typedef enum {
  CW_RT_RGBA8, CW_RT_RGBA16F, CW_RT_RG16F, CW_RT_R16F, CW_RT_R8, CW_RT_FORMAT_COUNT
} cw_rt_format;

/* A render target: an FBO with one colour texture (linear filtered, clamped)
 * and optionally a depth texture. Read-only to users. fbo 0 with tex 0 is the
 * output when it is the screen (never sample it). */
typedef struct cw_rt {
  unsigned fbo, tex, depth_tex;
  int w, h;
  cw_rt_format format;
} cw_rt;

/* Tone curves, three.js r186's (ToneMappingFunctions): NONE clamps; AGX
 * (Blender's AgX, three.js's minimal fit) is the default; NEUTRAL is Khronos
 * PBR Neutral. The sRGB transfer follows every curve, as in r186. */
typedef enum {
  CW_TONEMAP_NONE, CW_TONEMAP_LINEAR, CW_TONEMAP_REINHARD, CW_TONEMAP_CINEON, CW_TONEMAP_ACES_FILMIC,
  CW_TONEMAP_AGX, CW_TONEMAP_NEUTRAL, CW_TONEMAP_COUNT
} cw_tonemap;

/* Post antialiasing on display values. FXAA: 3.11, quality preset 12. SMAA:
 * 1x with orthogonal pattern search (16 steps) and corner rounding; its
 * diagonal pattern search is not built yet. TAA is a separate switch. */
typedef enum { CW_AA_NONE, CW_AA_FXAA, CW_AA_SMAA } cw_aa_mode;

/* Show an intermediate buffer instead of the image (the "debug_view" POST
 * pass, last in the chain): linear depth (1 / (1 + z / 4)), view-space
 * normals rebuilt from depth, the GTAO visibility (needs ssao), the velocity
 * (x 20, 0.5 = still), or the bloom chain's result (needs bloom). */
typedef enum {
  CW_VIEW_NONE, CW_VIEW_DEPTH, CW_VIEW_NORMALS, CW_VIEW_AO, CW_VIEW_VELOCITY, CW_VIEW_BLOOM, CW_VIEW_COUNT
} cw_debug_view;

/* Every post setting, with its default. The PostProcess node (docs/nodes.json)
 * mirrors these as properties for scene files; code can write
 * cw_pipeline_settings(p) directly. Changing a field takes effect on the next
 * frame; nothing is ever lowered automatically (quality is the developer's). */
typedef struct cw_post_settings {
  bool hdr;                 /* the scene target is RGBA16F (linear HDR); off: RGBA8, where linear
                             * values band in the darks (WebGPU compatibility mode's MSAA needs it); true */
  int msaa;                 /* samples on the scene pass (0, 2, 4, 8; clamped to GL_MAX_SAMPLES); 0 */
  bool depth_prepass;       /* depth-only pass before the scene (early-z for heavy shaders) */
  /* exposure: a linear multiplier (three.js's toneMappingExposure) times
   * 2^exposure_ev (photographic stops) */
  float exposure;           /* 1 */
  float exposure_ev;        /* 0 */
  cw_tonemap tonemap;       /* AGX */
  /* bloom: dual-filter chain; threshold/knee pick what blooms (0 = all
   * light, the physical default), intensity is the share of that energy that
   * is spread (energy conserving: what blooms is taken from the source) */
  bool bloom;
  float bloom_intensity;    /* 0.05 */
  float bloom_threshold;    /* 0 (scene-linear units) */
  float bloom_knee;         /* 0.5: soft transition width, as a fraction of threshold */
  float bloom_radius;       /* 1: upsample tent radius in texels of each level */
  int bloom_levels;         /* 6 */
  /* ambient occlusion: GTAO at half resolution, depth-aware upsample */
  bool ssao;
  float ssao_radius;        /* 0.5 world units */
  float ssao_intensity;     /* 1 */
  float ssao_power;         /* 1.5: contrast curve on the visibility */
  int ssao_slices;          /* 2 directions per pixel (x2 sides) */
  int ssao_steps;           /* 6 horizon steps per side */
  /* temporal antialiasing: jittered projection (Halton 2,3), reprojection by
   * the velocity target (camera motion rebuilt from depth; moving objects
   * have no velocity of their own yet and rely on the YCoCg neighbourhood
   * clip), Catmull-Rom history fetch */
  bool taa;
  float taa_feedback;       /* 0.9: history weight */
  int taa_samples;          /* 8 jitter positions */
  cw_aa_mode aa;            /* NONE */
  /* colour grading: a 3D LUT (cw_lut_load_cube) applied to display values */
  bool grade;
  float grade_intensity;    /* 1: blend with the ungraded image */
  uint64_t grade_lut;       /* the LUT, from cw_lut_* (0 = none) */
  bool vignette;
  float vignette_intensity; /* 0.35 */
  float vignette_smoothness; /* 0.5 */
  bool grain;
  float grain_intensity;    /* 0.04 */
  bool chromatic_aberration;
  float ca_intensity;       /* 0.004 (uv offset at the corners) */
  cw_debug_view debug_view; /* NONE */
  /* screen-space reflections: three.js r186's SSRNode (one mirror ray a
   * pixel, softened by roughness) at half resolution over a G-buffer of the
   * surfaces carrying the GI shading part (cartwheel/gi.h: every Standard /
   * Physical material of a tree with SSR, a probe or a grid); off on the Low
   * effects preset */
  bool ssr;
  float ssr_max_distance;   /* 1: how far a reflection reaches (view units, r186's maxDistance) */
  float ssr_thickness;      /* 0.1: how deep a surface counts as hit behind the depth */
  float ssr_quality;        /* 0.5: march steps per pixel of the ray's screen length (0..1) */
  float ssr_intensity;      /* 1 */
  /* screen-space bounce light and AO: three.js r186's SSGINode (SSILVB) at
   * half resolution over the same G-buffer plus the surfaces' diffuse
   * colour; composited as r186's example does (colour x AO + diffuse x GI);
   * the Ultra effects preset only */
  bool ssgi;
  float ssgi_radius;        /* 12: r186's radius (screen-space sampling) */
  float ssgi_intensity;     /* 10: r186's giIntensity */
  float ssgi_ao_intensity;  /* 1 */
  float ssgi_thickness;     /* 1 */
  int ssgi_slices;          /* 2 (r186's example) */
  int ssgi_steps;           /* 8 (r186's example) */
} cw_post_settings;

/* ── the pass context ──────────────────────────────────────────────── */

typedef struct cw_pass_ctx {
  cw_tree *tree;
  cw_pipeline *pipeline;
  const cw_post_settings *settings;
  t3_renderer *renderer;    /* the tree's three.c renderer (NULL before the first scene draw) */
  t3_scene *scene3d;
  t3_camera *camera;        /* projection jittered when TAA is on */
  cw_node camera_node;
  int width, height;        /* the frame in pixels */
  uint64_t frame;           /* frames this pipeline has rendered (0 first) */
  double time;              /* seconds (cw_time) */
  double alpha;             /* fixed-step interpolation of this frame */
  float jitter[2];          /* this frame's projection jitter, pixels (0 without TAA) */
  bool hdr;                 /* colour is scene-linear HDR (the scene target is RGBA16F) */
  bool offscreen;           /* false on the direct path (no scene target) */
  /* the scene target: HDR colour + depth texture (NULL on the direct path) */
  const cw_rt *scene;
  const cw_rt *velocity;    /* RG16F, when an enabled pass reads CW_RES_VELOCITY */
  /* the depth texture that is safe to SAMPLE in this phase: in SKY / EFFECTS
   * a copy of the scene depth (the scene FBO's own depth is attached for
   * depth testing; sampling it would be a feedback loop), made only when an
   * active pass of those phases reads CW_RES_DEPTH; in POST the scene depth
   * itself. 0 when there is none. Depth is non-linear [0, 1] window depth;
   * view-space z = linearize with ctx->proj (see cw_pass_linear_depth_params). */
  unsigned depth_tex;
  const cw_rt *output;      /* the final image (fbo 0 = the screen) */
  /* POST phase: the image to read and the target to write */
  const cw_rt *src, *dst;
  /* camera matrices (column major, three.js layout), unjittered, this frame
   * and the previous one (equal on the first frame) */
  t3_mat4 view, proj, view_proj, inv_proj, inv_view_proj, prev_view_proj;
  /* a POST pass that wrote its result somewhere else than dst (a history
   * buffer) sets this: the chain continues from it. */
  const cw_rt *result;
  void *user;
} cw_pass_ctx;

typedef struct cw_pass_desc {
  const char *name;         /* unique in the pipeline (copied); "bloom" */
  cw_pass_phase phase;
  int order;                /* within the phase, ascending */
  uint32_t reads, writes;   /* CW_RES_* */
  bool disabled;            /* start disabled (cw_pipeline_set_pass_enabled turns it on) */
  /* POST writers: dst has the format of src unless the pass turns HDR into
   * display values (the tonemap pass): then dst is RGBA8 */
  bool ldr_output;
  /* optional: called each frame before scheduling; false skips the pass this
   * frame (and its reads do not count). For passes driven by settings. */
  bool (*active)(const cw_pass_ctx *ctx, void *user);
  void (*execute)(cw_pass_ctx *ctx, void *user);
  void (*destroy)(void *user); /* when removed or the pipeline is freed */
  void *user;
  /* execute works on WebGPU (cartwheel/gpu.h): it draws through the
   * backend-neutral helpers below (cw_pass_use, cw_pass_fullscreen, ...) with
   * a WGSL program on WebGPU, or records its own WebGPU passes. On a WebGPU
   * run a pass without it is skipped (logged once): GL calls there would
   * trap. Ignored on GL. */
  bool webgpu;
} cw_pass_desc;

/* The tree's pipeline (created on first call; freed with the tree). */
cw_pipeline *cw_pipeline_of(cw_tree *t);
/* Add a pass. Returns false (and logs) when the name is taken. */
bool cw_pipeline_add_pass(cw_pipeline *p, const cw_pass_desc *d);
/* Remove a pass (its destroy runs). False when there is none by that name. */
bool cw_pipeline_remove_pass(cw_pipeline *p, const char *name);
/* The user switch of a pass (built-ins included: "scene", "bloom", ...);
 * active() still decides per frame. False (logged) for an unknown name. */
bool cw_pipeline_set_pass_enabled(cw_pipeline *p, const char *name, bool on);
/* Whether a pass of that name is registered (enabled or not). */
bool cw_pipeline_has_pass(cw_pipeline *p, const char *name);

/* Settings (read/write). A PostProcess node in the tree overwrites them every
 * frame: set its properties instead when there is one. */
cw_post_settings *cw_pipeline_settings(cw_pipeline *p);
/* The program states the tree's last frame needed and none of the app's
 * program tables has, one per line ("" when none). A cart skips those draws;
 * a native build with CW_THREE_BUILDER builds them at run time. Either way
 * each line belongs in the app's table (cmake/programs.cmake: run
 * tools/programs.mjs). Also the debug fields "cw.three.missing" (the count)
 * and "cw.three.missing_states" (the lines; the gates fail on them), and the
 * "pipeline" command. */
const char *cw_pipeline_missing_states(cw_tree *t);

/* Light units: physical, three.js r186's (r155+ made them its only ones):
 *   DirectionalLight3D.energy   illuminance, lux-like (E; diffuse radiance =
 *                               albedo x E x cos / PI)
 *   Omni/SpotLight3D.energy     luminous intensity, candela-like: E = I / d^2
 *                               (decay 2), windowed to 0 at range when > 0
 *   WorldEnvironment ambient / hemi energies: irradiance, as directional
 * Sensible values with exposure 1 and AgX: a sun of 3-5, room lamps of
 * 10-50 a few metres away, ambient 0.05-0.3. Real photometric values (a sun
 * of 100000 lux) work too with exposure_ev around -14 (a camera's EV100). */

/* Render one frame of the tree into the output (the bound default
 * framebuffer unless cw_pipeline_set_output), w x h pixels. The scene runner
 * calls this; apps without the runner call it from their render callback. */
void cw_pipeline_render(cw_tree *t, int w, int h, double alpha);
/* Render into this target instead of the screen (NULL = the screen). The
 * target is not owned. */
void cw_pipeline_set_output(cw_pipeline *p, const cw_rt *rt);
/* The app's render callback, run as the "app" OVERLAY pass (order 0). */
void cw_pipeline_set_app_render(cw_pipeline *p, void (*render)(void *user, double alpha), void *user);

/* ── views (splitscreen) ───────────────────────────────────────────── */

/* A view: a Camera3D drawn into a rectangle of the output (fractions of
 * it, x / y from the top left). With two or more views the pipeline renders
 * the 3D scene once per view (each through its own camera, at its own size,
 * with the post chain minus TAA, whose history one camera owns; the shadow
 * maps are drawn once, for the first view, and the others reuse them), lays the
 * pictures into their rectangles, then runs the overlays (the app's HUD,
 * debug draw) once over the whole output: a HUD reads the rectangles back
 * with cw_pipeline_views. */
#define CW_MAX_VIEWS 4
typedef struct cw_view {
  cw_node camera;
  float x, y, w, h;
} cw_view;
/* Set the views (n <= 1: the tree's current camera fills the output, as
 * without views). Copied. */
void cw_pipeline_set_views(cw_pipeline *p, const cw_view *views, int n);
/* The views in use (0 when a single camera fills the output). */
int cw_pipeline_views(cw_pipeline *p, const cw_view **views);
/* A split of the output for n players: 2 side by side (top / bottom when
 * the output is taller than wide), 3 and 4 as quadrants (3 leaves the last
 * quadrant to the HUD), cameras taken in order. Returns the count set. */
int cw_pipeline_split(cw_pipeline *p, const cw_node *cameras, int n);
/* Called before each view's 3D picture is drawn, with the view's index:
 * something can be posed one way in one player's view and another way in
 * the rest (a player's own replay, say). NULL: none. Shared work is done
 * with the first view, as the hook left it: the shadow maps, and the
 * scene's world matrices (what a hook moves for a later view, it updates
 * itself, t3_object_update_world_matrix). */
void cw_pipeline_set_view_hook(cw_pipeline *p, void (*hook)(int view, void *user), void *user);
/* Splitscreen's post chain, shared: each view runs only the post passes that
 * read its depth or motion (SSAO, say) and keeps its picture in HDR; the
 * views are laid out first and the rest of the chain (bloom, tone mapping,
 * the grade, anti-aliasing, lens effects) runs once over the whole output.
 * A view then costs its 3D and those passes, not a whole chain (the CPU
 * records each pass once a frame instead of once a view). The vignette and
 * the bloom are the screen's then, not each view's: a glow can cross a seam.
 * Off by default. */
void cw_pipeline_set_views_shared_post(cw_pipeline *p, bool shared);

/* ── targets ───────────────────────────────────────────────────────── */

/* A target of your own (a sub-viewport, a render-to-texture output for
 * cw_pipeline_set_output): colour of format f, plus a DEPTH_COMPONENT24
 * texture when with_depth. NULL (logged) when the host cannot render that
 * format. Needs the GL context. */
cw_rt *cw_rt_new(int w, int h, cw_rt_format f, bool with_depth);
/* Free a target from cw_rt_new (NULL is fine); not one from the pool. */
void cw_rt_free(cw_rt *rt);

/* A pooled target, the caller's until the end of the frame. Same size and
 * format targets are reused across frames, so this costs no allocation in a
 * steady state. with_depth adds a DEPTH_COMPONENT24 texture. */
const cw_rt *cw_pass_rt(cw_pass_ctx *ctx, int w, int h, cw_rt_format f, bool with_depth);
/* Hand a pooled target back early (it can be reused by the next request). */
void cw_pass_rt_release(cw_pass_ctx *ctx, const cw_rt *rt);
/* A target owned by the pipeline under a name ("taa.history0"), (re)created
 * at this size and format; *fresh is set true when its contents are new
 * (undefined) and false when they survive from the previous frame. */
const cw_rt *cw_pipeline_rt_persistent(cw_pipeline *p, const char *key, int w, int h, cw_rt_format f, bool *fresh);

/* ── helpers for pass code ─────────────────────────────────────────── */

/* Compile + link a full-screen program: fs is GLSL ES 3.00 fragment source
 * after the version line (precision highp is prepended; `in vec2 v_uv` is
 * provided). Returns 0 and logs the compiler output on failure. GL only (a
 * WebGPU run logs an error and returns 0: use cw_pass_program_wgsl). */
unsigned cw_pass_program(const char *name, const char *fs);
/* The WebGPU counterpart: a full-screen program from a WGSL fragment module.
 * The engine supplies the vertex stage (one triangle) and prepends:
 *
 *   struct CwTarget { size: vec2f, flip: f32, pad: f32 };
 *   @group(1) @binding(0) var<uniform> cw_target: CwTarget;
 *   @group(1) @binding(1) var cw_linear: sampler;   // linear, clamp to edge
 *   @group(1) @binding(2) var cw_nearest: sampler;  // nearest, clamp to edge
 *   fn cw_frag_coord(p: vec4f) -> vec2f  // GL's gl_FragCoord.xy (y from the bottom)
 *   fn cw_load(t: texture_2d<f32>, uv: vec2f) -> vec4f  // GL's NEAREST texture(t, uv)
 *
 * The module defines `@fragment fn fs(@builtin(position) pos: vec4f,
 * @location(0) uv: vec2f) -> @location(0) vec4f` (uv is GL's v_uv: (0, 0) at
 * the bottom left of the image) and its own resources in group 0:
 *   @group(0) @binding(0) var<uniform> u: <a struct of your own>
 *       the uniform block: cw_pass_uniform* write its members by name
 *       (f32 / i32 / u32 / vec2f / vec3f / vec4f / mat4x4f), values persist
 *       per program as GL uniforms do;
 *   @group(0) @binding(k + 1) var <name>: texture_2d<f32> | texture_3d<f32>
 *       texture unit k (cw_pass_bind_texture / cw_pass_bind_texture_3d); a
 *       texture read with textureLoad / cw_load and never given to a
 *       textureSample* call is bound unfilterable (ctx->depth_tex, R32F).
 * Sample with textureSampleLevel(..., 0.0) (WGSL needs uniform control flow
 * for textureSample). Offscreen targets keep GL's row order, so a GLSL
 * program ports line for line. Returns 0 (logged) on a WGSL error; GL runs
 * get 0 and an error. */
unsigned cw_pass_program_wgsl(const char *name, const char *wgsl);
/* Use a program for the following cw_pass_fullscreen calls (glUseProgram on
 * GL). */
void cw_pass_use(unsigned prog);
/* Blending of the following draws (reset to NONE before every pass). */
typedef enum {
  CW_PASS_BLEND_NONE,
  CW_PASS_BLEND_PREMUL, /* ONE, ONE_MINUS_SRC_ALPHA */
  CW_PASS_BLEND_ADD,    /* ONE, ONE */
  CW_PASS_BLEND_ALPHA,  /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
} cw_pass_blend;
/* Blend the following draws with b (GL glBlendFunc; part of the pipeline key on WebGPU). */
void cw_pass_set_blend(cw_pass_blend b);
/* Depth of the following draws on a target with depth: test (LESS) and
 * write (reset to off before every pass). write without test always writes
 * (a full-screen pass that outputs depth). */
void cw_pass_set_depth(bool test, bool write);
/* Clear the bound target's colour (and depth to 1). */
void cw_pass_clear(float r, float g, float b, float a, bool depth);
/* One full-screen triangle (no buffers; needs a bound program). */
void cw_pass_fullscreen(void);
/* Procedural geometry without buffers: `vertices` vertices as a triangle
 * list, positioned by the program's own vertex stage from the vertex index
 * (gl_VertexID / @builtin(vertex_index)): a GLSL program from
 * cw_pass_program_vs, or a WGSL module that defines `@vertex fn vs`. On
 * WebGPU that vertex stage writes clip z as (z + w) / 2 for GL's depth range
 * and multiplies clip y by cw_target.flip (offscreen targets keep GL's row
 * order). */
void cw_pass_draw(int vertices);
/* cw_pass_program with a vertex stage of its own (GLSL ES 3.00 after the
 * version line) for cw_pass_draw. */
unsigned cw_pass_program_vs(const char *name, const char *vs, const char *fs);
/* Bind tex to texture unit `unit` (GL_TEXTURE_2D). On WebGPU tex is the
 * registry id a cw_rt carries (rt->tex), ctx->depth_tex, or a LUT's. */
void cw_pass_bind_texture(int unit, unsigned tex);
/* The same for a 3D texture (GL_TEXTURE_3D: a colour-grading LUT's). */
void cw_pass_bind_texture_3d(int unit, unsigned tex);
/* A texture of your own for pass programs (a lookup table, a noise image, a
 * CPU-made mask): w x h texels of format f, uploaded once. RGBA8 and R8 take
 * bytes, RGBA16F / RG16F / R16F take IEEE halves, packed rows (no padding),
 * row 0 first as glTexImage2D takes it (row 0 is v = 0, the bottom, in
 * GLSL's texture() and in WGSL through the offscreen row order). Linear
 * filtered, clamped to the edge, no mips. Returns an id for
 * cw_pass_bind_texture on GL and WebGPU alike, 0 (logged) on bad arguments.
 * Needs the GPU (init or later). Free it with cw_pass_texture_free. */
unsigned cw_pass_texture_new(int w, int h, cw_rt_format f, const void *texels);
/* Free a texture from cw_pass_texture_new (0 is fine). */
void cw_pass_texture_free(unsigned tex);
/* Bind and set the viewport to a target (NULL = the output). */
void cw_pass_bind_target(cw_pass_ctx *ctx, const cw_rt *rt);
/* (near, far, is_orthographic) of ctx->camera, for shaders that linearize
 * window depth: perspective z_view = n*f / (f - d*(f - n)) (negated). */
void cw_pass_linear_depth_params(const cw_pass_ctx *ctx, float out[3]);
/* Uniform setters by name, cached per program (the location lookup happens
 * once). */
void cw_pass_uniform1f(unsigned prog, const char *name, float v);
/* vec2 */
void cw_pass_uniform2f(unsigned prog, const char *name, float x, float y);
/* vec3 */
void cw_pass_uniform3f(unsigned prog, const char *name, float x, float y, float z);
/* vec4 */
void cw_pass_uniform4f(unsigned prog, const char *name, float x, float y, float z, float w);
/* int or sampler unit */
void cw_pass_uniform1i(unsigned prog, const char *name, int v);
/* mat4, column major (t3_mat4's layout) */
void cw_pass_uniform_mat4(unsigned prog, const char *name, const t3_mat4 *m);

/* ── colour grading LUTs ───────────────────────────────────────────── */

/* Load an Adobe/Resolve .cube 3D LUT (LUT_3D_SIZE 2..64, DOMAIN_MIN/MAX
 * honoured) from an asset into a GL_TEXTURE_3D (RGBA16F, linear filtered).
 * Returns a LUT id for settings.grade_lut, 0 on failure (logged). Needs the
 * GL context (call from init or later). */
uint64_t cw_lut_load_cube(const char *asset_path);
/* The same from text in memory. */
uint64_t cw_lut_parse_cube(const char *text, size_t len, const char *name_for_errors);
/* A LUT from size^3 RGB float triples (r fastest), e.g. a generated grade. */
uint64_t cw_lut_from_rgb(int size, const float *rgb);
/* Free a LUT's texture; settings still naming it grade nothing. */
void cw_lut_free(uint64_t lut);

/* ── capabilities and timing ───────────────────────────────────────── */

typedef struct cw_pipeline_caps {
  bool float_render;        /* RGBA16F colour-renderable (EXT_color_buffer_float / _half_float) */
  bool float_blend;         /* blending into float targets */
  bool timer_query;         /* EXT_disjoint_timer_query(_webgl2) */
  int max_samples;          /* GL_MAX_SAMPLES */
  const char *hdr_format;   /* "RGBA16F", or "RGBA8" when float targets are missing */
} cw_pipeline_caps;
/* Probed on first use (needs the GL context: call from init or later).
 * Measured on Linux with Mesa (2026-10): RGBA16F renders and timer
 * queries work natively (Mesa) and over ANGLE. RGBA16F also renders in
 * romdev's wasmcart host and in Chromium (wasmcart's web host enables
 * EXT_color_buffer_float). Carts (wasm) never use timer queries (timer_query
 * false, pass gpu_ms -1): in Chromium on ANGLE's GL backend they made
 * rendering nondeterministic and coincided with GPU page faults (see
 * pipeline_gl.c). */
const cw_pipeline_caps *cw_pipeline_caps_get(void);

typedef struct cw_pass_stat {
  const char *name;
  cw_pass_phase phase;
  bool ran;                 /* ran last frame */
  bool fused;               /* ran inside another pass (GL post fusion: the grade in the tonemap
                             * pass, the lens in SMAA's blend); its time is that pass's */
  float gpu_ms;             /* latest available GPU time (GL: read in a batch every 8 frames, 3 to 11 frames late; WebGPU: 1 to 3 frames late); -1 = none
                             * (natively the timers run only when asked for: CW_GPU_TIMERS=1,
                             * or the "pipeline" debug command).
                             * A pipeline rendered from inside another one's pass is not
                             * timed (GL has one timer query open at a time): the
                             * enclosing pass's time includes it */
  float gpu_ms_avg;         /* exponential average (1/16) */
  /* CPU time of the pass's execute (recording), last frame and averaged */
  float cpu_ms, cpu_ms_avg;
} cw_pass_stat;
/* Every pass in run order, as of the last complete frame (read during a
 * frame, it is the previous one's); returns the count. */
int cw_pipeline_pass_stats(cw_pipeline *p, const cw_pass_stat **out);
/* Total GPU ms of the passes that ran in the latest timed frame. */
float cw_pipeline_gpu_ms(cw_pipeline *p);

#ifdef __cplusplus
}
#endif
#endif

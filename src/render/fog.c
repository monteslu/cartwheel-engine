/* HeightFog (cartwheel/environment.h): exponential height fog applied in
 * the post chain from the depth buffer, with a sun lobe. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"

#include "cartwheel/shading.h"
#include "environment_internal.h"
#include "pipeline_internal.h"
#define S(...) #__VA_ARGS__ "\n"
#include "pipeline_post_wgsl.h"

typedef cw__height_fog fog_node;

typedef struct fog_rt {
  fog_node p;
  bool present;             /* a HeightFog is in the tree (exit_tree clears it) */
} fog_rt;

/* the tree's transparent surfaces in fog (environment_internal.h): a slot of every tree whose transparent queue a
 * fog pass ever deferred */
typedef struct fog_trans {
  t3_renderer *renderer;    /* the tree's (the hfog hook finds its values by it) */
  CW_ARRAY(t3_mesh *) meshes; /* every drawable of the scene, at graph version ver */
  const t3_scene *scene;
  uint32_t ver;
  bool have;
  uint64_t frame;           /* cw_frame_index() of the last walk */
  int count;                /* its transparent draws (materials of visible drawables) */
  float hf[5][4];           /* the hfog part's values for this run */
  struct fog_trans *next;
} fog_trans;
static fog_trans *trans_list; /* every tree's, for the hook's lookup */

/* the fog's uniforms (all but the depth) and its evaluation: the fog pass
 * draws it over the scene, and the SSAO composite runs it fused (pipeline.c
 * plan_fusion; pipeline_internal.h) */
static const char FOG_DECL[] =
    "uniform mat4 u_inv_view_proj;\n"
    "uniform vec3 u_cam;\n"
    "uniform vec4 u_fog; /* density, falloff, base height, max opacity */\n"
    "uniform vec4 u_dist; /* start distance, sky distance */\n"
    "uniform vec3 u_color;\n"
    "uniform vec3 u_sun_dir; /* toward the sun */\n"
    "uniform vec3 u_sun_color; /* sunlight x sun_scatter */\n"
    "uniform float u_g;\n";
/* fog_eval: (in-scattered light, opacity) premultiplied, or alpha -1 where
 * there is no fog (the pass discards there) */
static const char FOG_FN[] =
    /* the fog's optical depth from the camera along v for distance l */
    "float opticalDepth( vec3 v, float l ) {\n"
    "\tfloat a = u_fog.x * exp( -u_fog.y * ( u_cam.y - u_fog.z ) );\n"
    "\tfloat k = u_fog.y * v.y * l;\n"
    "\treturn a * l * ( abs( k ) > 1e-4 ? ( 1.0 - exp( -k ) ) / k : 1.0 - 0.5 * k );\n"
    "}\n"
    "vec4 fog_eval( vec2 uv, float d ) {\n"
    "\tvec4 a = u_inv_view_proj * vec4( uv * 2.0 - 1.0, -1.0, 1.0 );\n"
    "\tvec4 b = u_inv_view_proj * vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );\n"
    "\tvec3 p0 = a.xyz / a.w;\n"
    "\tvec3 ray = b.xyz / b.w - u_cam;\n"
    "\tfloat len = length( ray );\n"
    "\tvec3 v = ray / max( len, 1e-6 );\n"
    "\tif ( d >= 1.0 ) {\n"
    "\t\tif ( u_dist.y <= 0.0 ) return vec4( -1.0 );\n"
    "\t\tv = normalize( b.xyz / b.w - p0 );\n"
    "\t\tlen = u_dist.y;\n"
    "\t}\n"
    "\tfloat l = max( len - u_dist.x, 0.0 );\n"
    "\tif ( l <= 0.0 ) return vec4( -1.0 );\n"
    "\tfloat tau = opticalDepth( v, l );\n"
    "\tfloat fog = min( 1.0 - exp( -tau ), u_fog.w );\n"
    "\tfloat mu = dot( v, u_sun_dir ), g = u_g, g2 = g * g;\n"
    "\tfloat hg = ( 1.0 - g2 ) / ( 4.0 * 3.14159265 * pow( max( 1.0 + g2 - 2.0 * g * mu, 1e-4 ), 1.5 ) );\n"
    "\tvec3 sun = u_sun_color * hg * 4.0 * 3.14159265;\n"
    "\tvec3 inscatter = ( u_color + sun ) * fog;\n"
    "\treturn vec4( inscatter, fog );\n"
    "}\n";
static const char FOG_MAIN[] =
    "uniform sampler2D u_depth;\n"
    "uniform vec4 u_view; /* the view's rectangle in uv (a one-pass split screen's; 0, 0, 1, 1 otherwise) */\n"
    "out vec4 o;\n"
    "void main() {\n"
    "\tvec4 f = fog_eval( ( v_uv - u_view.xy ) / u_view.zw, texture( u_depth, v_uv ).r );\n"
    "\tif ( f.a < 0.0 ) discard;\n"
    "\to = f;\n"
    "}\n";
const char *cw__fog_glsl(void) {
  static char buf[4096];
  if (!buf[0]) snprintf(buf, sizeof buf, "%s%s", FOG_DECL, FOG_FN);
  return buf;
}

static void fog_rt_free(void *p) { cw_free(p); }

static void fog_trans_free(void *p) {
  fog_trans *t = p;
  for (fog_trans **q = &trans_list; *q; q = &(*q)->next)
    if (*q == t) {
      *q = t->next;
      break;
    }
  cw_array_free(&t->meshes);
  cw_free(t);
}

/* CW_TEST_FOG_OVER_TRANSPARENTS=1 (a gate control): the fog drawn as it was before transparent surfaces took their
 * own: after the transparent queue and the particles (EFFECTS 300), over them, by the depth behind them */
bool cw__fog_over_transparents(void) {
  const char *e = cw_test_env("CW_TEST_FOG_OVER_TRANSPARENTS");
  return e && atoi(e) != 0;
}

static bool fog_active(const cw_pass_ctx *ctx, void *user) {
  fog_rt *f = user;
  (void)ctx;
  /* volumetric mode: the froxel pass draws this fog (volumetric_fog.c); on effects detail Low and Very Low, which draw no
   * volumetric fog, this pass draws it as plain height fog */
  return f->present && f->p.density > 0 && !(f->p.volumetric && cw__vfog_on());
}

const cw__height_fog *cw__height_fog_of(cw_tree *t) {
  fog_rt *f = cw_tree_slot(t, "fog");
  return f && f->present ? &f->p : NULL;
}

static unsigned fog_program(void) {
  static unsigned prog;
  if (!prog) {
    if (cw_gpu_backend() == CW_GPU_WEBGPU) {
      static char w[8192];
      snprintf(w, sizeof w, "%s%s%s", WGSL_COMMON, WGSL_FOG, WGSL_FOG_FN);
      prog = cw_pass_program_wgsl("fog", w);
    } else {
      static char g[8192];
      snprintf(g, sizeof g, "%s%s%s", FOG_DECL, FOG_FN, FOG_MAIN);
      prog = cw_pass_program("fog", g);
    }
  }
  return prog;
}

/* the fog's uniforms (not its depth) into a program in use: the fog pass's or the fused SSAO composite's */
static void fog_uniforms(cw_pass_ctx *ctx, unsigned pr, const fog_node *p, t3_color color, t3_vec3 sun, t3_color sun_color) {
  t3_vec3 cam = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  cw_pass_uniform_mat4(pr, "u_inv_view_proj", &ctx->inv_view_proj);
  const float *e = ctx->camera->projection_matrix.e;
  cw_pass_uniform4f(pr, "u_pz", e[10], e[14], e[11], e[15]);
  cw_pass_uniform3f(pr, "u_cam", cam.x, cam.y, cam.z);
  cw_pass_uniform4f(pr, "u_fog", p->density, CW_MAX(p->height_falloff, 0.0f), p->base_height, CW_CLAMP(p->max_opacity, 0.0f, 1.0f));
  cw_pass_uniform4f(pr, "u_dist", p->start_distance, p->sky_distance, 0, 0);
  cw_pass_uniform3f(pr, "u_color", color.r, color.g, color.b);
  cw_pass_uniform3f(pr, "u_sun_dir", sun.x, sun.y, sun.z);
  float sc = p->sun_scatter / (4.0f * CW_PI);
  cw_pass_uniform3f(pr, "u_sun_color", sun_color.r * sc, sun_color.g * sc, sun_color.b * sc);
  cw_pass_uniform1f(pr, "u_g", CW_CLAMP(p->sun_g, -0.95f, 0.95f));
}

static void fog_exec(cw_pass_ctx *ctx, void *user) {
  fog_rt *f = user;
  const fog_node *p = &f->p;
  if (!ctx->depth_tex) return;
  /* colours: the sky's when asked and present */
  t3_color color = p->color, sun_color = { 0, 0, 0 }, horizon;
  t3_vec3 sun = { 0, 1, 0 };
  if (cw__sky_env(ctx->tree, NULL, &sun, &sun_color, &horizon) && p->use_sky_color) color = horizon;
  unsigned pr = fog_program();
  if (!pr) return;
  cw_pass_use(pr);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(pr, "u_depth", 0);
  fog_uniforms(ctx, pr, p, color, sun, sun_color);
  float vu[4];
  cw_pass_view_uv(ctx, vu);
  cw_pass_uniform4f(pr, "u_view", vu[0], vu[1], vu[2], vu[3]);
  cw_pass_set_blend(CW_PASS_BLEND_PREMUL);
  cw_pass_fullscreen();
  cw_pass_set_blend(CW_PASS_BLEND_NONE);
}

/* the fused SSAO composite (pipeline_post.c): the fog's uniforms into its program, in use */
void cw__fog_fused_uniforms(cw_pass_ctx *ctx, unsigned prog) {
  fog_rt *f = cw_tree_slot(ctx->tree, "fog");
  if (!f) return;
  const fog_node *p = &f->p;
  t3_color color = p->color, sun_color = { 0, 0, 0 }, horizon;
  t3_vec3 sun = { 0, 1, 0 };
  if (cw__sky_env(ctx->tree, NULL, &sun, &sun_color, &horizon) && p->use_sky_color) color = horizon;
  fog_uniforms(ctx, prog, p, color, sun, sun_color);
}

static bool fog_pass_active(const cw_pass_ctx *ctx, void *user) { return !cw__fog_over_transparents() && fog_active(ctx, user); }
static bool fog_old_active(const cw_pass_ctx *ctx, void *user) { return cw__fog_over_transparents() && fog_active(ctx, user); }

static fog_rt *fog_of(cw_tree *t) {
  fog_rt *f = cw_tree_slot(t, "fog");
  if (f) return f;
  f = cw_calloc(1, sizeof *f, CW_MEM_RENDER);
  cw_tree_set_slot(t, "fog", f, fog_rt_free);
  /* over the opaque picture (the volumetric grid at 110 and its apply at 120 instead in volumetric mode), before the
   * transparent queue (130) and the particles (200), which take the fog in their own shaders at their own distance
   * (environment_internal.h) */
  cw_pipeline_add_pass(cw_pipeline_of(t), &(cw_pass_desc){ .name = "height_fog", .phase = CW_PHASE_EFFECTS, .order = 120,
                                                           .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                                           .active = fog_pass_active, .execute = fog_exec, .user = f,
                                                           .webgpu = true, .views = CW_PASS_VIEWS_EACH });
  cw_pipeline_add_pass(cw_pipeline_of(t), &(cw_pass_desc){ .name = "height_fog.over", .phase = CW_PHASE_EFFECTS, .order = 300,
                                                           .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                                           .active = fog_old_active, .execute = fog_exec, .user = f,
                                                           .webgpu = true, .views = CW_PASS_VIEWS_EACH });
  return f;
}

/* ── transparent surfaces in fog (environment_internal.h) ───────────── */

/* the fog a surface takes at its own distance, in the engine's own shaders: u_ff (cw__fog_fwd's layout)
 *   [0] density, height falloff, base height, max opacity
 *   [1] start distance, mode (0 none, 1 analytic, 2 grid then analytic), sun g, analytic past the grid (mode 2)
 *   [2] in-scattered colour (the fog's, or the grid's ambient)   [3] toward the sun   [4] sunlight x scatter
 *   [5] the camera   [6] grid: slices, distance, detail spread   [7] the view's rectangle in uv   [8] 1 / target size */
const char CW__FFOG_GLSL[] =
    "uniform vec4 u_ff[9];\n"
    "uniform highp sampler3D u_ffvol;\n"
    "float cw_ff_od(vec3 v, float l) {\n"
    "  float a = u_ff[0].x * exp(-u_ff[0].y * (u_ff[5].y - u_ff[0].z));\n"
    "  float k = u_ff[0].y * v.y * l;\n"
    "  return a * l * (abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k);\n"
    "}\n"
    "float cw_ff_hg(float mu, float g) {\n"
    "  float g2 = g * g;\n"
    "  return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));\n"
    "}\n"
    "vec4 cw_ffog(vec3 wp, float vz) {\n"
    "  int mode = int(u_ff[1].y + 0.5);\n"
    "  if (mode == 0) return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  vec3 ray = wp - u_ff[5].xyz;\n"
    "  float len = max(length(ray), 1e-6);\n"
    "  vec3 v = ray / len;\n"
    "  vec3 light = u_ff[2].xyz + u_ff[4].xyz * cw_ff_hg(dot(v, u_ff[3].xyz), u_ff[1].z);\n"
    "  if (mode == 1) {\n"
    "    float l = max(len - u_ff[1].x, 0.0);\n"
    "    float f = l > 0.0 ? min(1.0 - exp(-cw_ff_od(v, l)), u_ff[0].w) : 0.0;\n"
    "    return vec4(light * f, 1.0 - f);\n"
    "  }\n"
    "  vec2 q = (gl_FragCoord.xy * u_ff[8].xy - u_ff[7].xy) / u_ff[7].zw;\n"
    "  float c = max(u_ff[6].x * pow(clamp(vz / u_ff[6].y, 1e-10, 1.0), u_ff[6].z) - 1.0, 0.0);\n"
    "  vec4 vol = texture(u_ffvol, vec3(q, (c - 0.5) / u_ff[6].x));\n"
    "  float k = clamp(c, 0.0, 1.0);\n"
    "  vec3 rgb = vol.rgb * k;\n"
    "  float tr = mix(1.0, vol.a, k);\n"
    "  if (u_ff[1].w > 0.5) {\n"
    "    float l0 = u_ff[6].y * len / max(vz, 1e-4);\n"
    "    if (len > l0) {\n"
    "      float f = min(1.0 - exp(-(cw_ff_od(v, len) - cw_ff_od(v, l0))), u_ff[0].w);\n"
    "      rgb += tr * light * f;\n"
    "      tr *= 1.0 - f;\n"
    "    }\n"
    "  }\n"
    "  return vec4(rgb, tr);\n"
    "}\n";
const char CW__FFOG_WGSL[] =
    "fn cw_ff_od(f: array<vec4f, 9>, v: vec3f, l: f32) -> f32 {\n"
    "  let a = f[0].x * exp(-f[0].y * (f[5].y - f[0].z));\n"
    "  let k = f[0].y * v.y * l;\n"
    "  let ks = select(1.0, k, abs(k) > 1e-4);\n"
    "  return a * l * select(1.0 - 0.5 * k, (1.0 - exp(-ks)) / ks, abs(k) > 1e-4);\n"
    "}\n"
    "fn cw_ff_hg(mu: f32, g: f32) -> f32 {\n"
    "  let g2 = g * g;\n"
    "  return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));\n"
    "}\n"
    "fn cw_ffog(f: array<vec4f, 9>, vol: texture_3d<f32>, s: sampler, frag: vec2f, wp: vec3f, vz: f32) -> vec4f {\n"
    "  let mode = i32(f[1].y + 0.5);\n"
    "  if (mode == 0) { return vec4f(0.0, 0.0, 0.0, 1.0); }\n"
    "  let ray = wp - f[5].xyz;\n"
    "  let len = max(length(ray), 1e-6);\n"
    "  let v = ray / len;\n"
    "  let light = f[2].xyz + f[4].xyz * cw_ff_hg(dot(v, f[3].xyz), f[1].z);\n"
    "  let q = (frag * f[8].xy - f[7].xy) / f[7].zw;\n"
    "  let c = max(f[6].x * pow(clamp(vz / max(f[6].y, 1e-6), 1e-10, 1.0), f[6].z) - 1.0, 0.0);\n"
    "  let g = textureSampleLevel(vol, s, vec3f(q, (c - 0.5) / max(f[6].x, 1.0)), 0.0);\n"
    "  if (mode == 1) {\n"
    "    let l = max(len - f[1].x, 0.0);\n"
    "    let fa = select(0.0, min(1.0 - exp(-cw_ff_od(f, v, l)), f[0].w), l > 0.0);\n"
    "    return vec4f(light * fa, 1.0 - fa);\n"
    "  }\n"
    "  let k = clamp(c, 0.0, 1.0);\n"
    "  var rgb = g.rgb * k;\n"
    "  var tr = mix(1.0, g.a, k);\n"
    "  if (f[1].w > 0.5) {\n"
    "    let l0 = f[6].y * len / max(vz, 1e-4);\n"
    "    if (len > l0) {\n"
    "      let fa = min(1.0 - exp(-(cw_ff_od(f, v, len) - cw_ff_od(f, v, l0))), f[0].w);\n"
    "      rgb += tr * light * fa;\n"
    "      tr *= 1.0 - fa;\n"
    "    }\n"
    "  }\n"
    "  return vec4f(rgb, tr);\n"
    "}\n";

/* the colours the fog pass draws with (the sky's when asked and present) */
static void fog_colors(cw_tree *t, const fog_node *p, t3_color *color, t3_vec3 *sun, t3_color *sun_color) {
  t3_color horizon;
  *color = p->color, *sun_color = (t3_color){ 0, 0, 0 }, *sun = (t3_vec3){ 0, 1, 0 };
  if (cw__sky_env(t, NULL, sun, sun_color, &horizon) && p->use_sky_color) *color = horizon;
}

bool cw__vfog_draws(const cw_pass_ctx *ctx); /* volumetric_fog.c */
bool cw__fog_draws(const cw_pass_ctx *ctx) {
  fog_rt *f = cw_tree_slot(ctx->tree, "fog");
  return (f && fog_active(ctx, f)) || cw__vfog_draws(ctx);
}

void cw__fog_forward(cw_pass_ctx *ctx, cw__fog_fwd *o) {
  memset(o, 0, sizeof *o);
  fog_rt *f = cw_tree_slot(ctx->tree, "fog");
  if (!ctx->camera || cw__fog_over_transparents()) return;
  if (!cw__vfog_forward(ctx, o)) {
    if (!f || !fog_active(ctx, f)) return;
    const fog_node *p = &f->p;
    t3_color color, sun_color;
    t3_vec3 sun;
    fog_colors(ctx->tree, p, &color, &sun, &sun_color);
    float *u = o->u;
    o->mode = 1;
    u[0] = p->density, u[1] = CW_MAX(p->height_falloff, 0.0f), u[2] = p->base_height, u[3] = CW_CLAMP(p->max_opacity, 0.0f, 1.0f);
    u[4] = p->start_distance, u[5] = 1, u[6] = CW_CLAMP(p->sun_g, -0.95f, 0.95f);
    u[8] = color.r, u[9] = color.g, u[10] = color.b;
    u[12] = sun.x, u[13] = sun.y, u[14] = sun.z;
    u[16] = sun_color.r * p->sun_scatter, u[17] = sun_color.g * p->sun_scatter, u[18] = sun_color.b * p->sun_scatter;
    t3_vec3 cam = t3_mat4_get_position(&ctx->camera->base.matrix_world);
    u[20] = cam.x, u[21] = cam.y, u[22] = cam.z;
  }
  float vu[4];
  cw_pass_view_uv(ctx, vu);
  memcpy(o->u + 28, vu, sizeof vu);
  o->u[32] = 1.0f / (float)CW_MAX(ctx->width, 1), o->u[33] = 1.0f / (float)CW_MAX(ctx->height, 1);
}

/* the scene's drawables (every kind three.c draws with a material), cached per graph version */
static bool drawable(const t3_object *o) {
  switch (o->type) {
  case T3_MESH: case T3_INSTANCED_MESH: case T3_SKINNED_MESH: case T3_BATCHED_MESH:
  case T3_LINE: case T3_LINE_SEGMENTS: case T3_LINE_LOOP: case T3_SPRITE: case T3_POINTS: return true;
  default: return false;
  }
}
static void collect(fog_trans *t, t3_object *o) {
  if (drawable(o)) cw_array_push(&t->meshes, (t3_mesh *)o);
  for (int i = 0; i < o->child_count; i++) collect(t, o->children[i]);
}

/* what a material's extension says, remembered by its id (ids are small: three.c numbers names as first seen):
 * bit 0 known, bit 1 carries hfog, bit 2 every part of it composes (a shading part some renderer knows) */
static uint8_t *ext_memo;
static int ext_memo_cap;
static int ext_info(const t3_material *m) {
  uint16_t id = m->extension;
  if (!id) return 1 | 4;
  if (id < ext_memo_cap && ext_memo[id] & 1) return ext_memo[id];
  const char *ext = t3_material_get_extension(m);
  int v = 1 | 4;
  for (const char *p = ext; p && *p;) {
    const char *e = strchr(p, '_');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char part[16] = "";
    if (len < sizeof part) memcpy(part, p, len), part[len] = 0;
    if (!strcmp(part, "hfog")) v |= 2;
    else if (!part[0] || !cw_shading_part_known(part)) v &= ~4;
    if (!e) break;
    p = e + 1;
  }
  if (id >= ext_memo_cap) {
    int cap = CW_MAX(64, id * 2 + 1);
    ext_memo = cw_realloc(ext_memo, (size_t)cap, CW_MEM_RENDER);
    memset(ext_memo + ext_memo_cap, 0, (size_t)(cap - ext_memo_cap));
    ext_memo_cap = cap;
  }
  ext_memo[id] = (uint8_t)v;
  return v;
}

/* CW_TEST_FOG_IGNORE_OPTOUT=1 (a gate control): a material with fog = false takes the fog of its own anyway, as v0.70.1
 * to v0.77.0 did */
static bool test_env_on(const char *name) { const char *e = cw_test_env(name); return e && atoi(e); }
/* (read once a frame, never cached from frame 0: cw_test_env answers nothing there, before a cart host delivers the env) */
static bool test_ignore_optout(void) {
  static uint64_t at = UINT64_MAX;
  static bool v;
  if (at != cw_frame_index()) at = cw_frame_index(), v = test_env_on("CW_TEST_FOG_IGNORE_OPTOUT");
  return v;
}

static bool test_no_hfog(void) { /* CW_TEST_FOG_NO_HFOG=1: transparent materials take no fog of their own (a gate control) */
  static uint64_t at = UINT64_MAX;
  static bool v;
  if (at != cw_frame_index()) at = cw_frame_index(), v = test_env_on("CW_TEST_FOG_NO_HFOG");
  return v;
}

/* every transparent material of the scene that takes fog (fog = true, three.js's Material.fog) carries the hfog part, no
 * other material does (a fog = false one, a sky ring or a backdrop card, draws as it is: before v0.70.1 the fog pass
 * fogged it by the depth behind it, the sky, which sky_distance 0 leaves alone); the count of transparent draws of
 * visible drawables (every one, fogged or not, draws after the fog) */
static int mark_transparents(fog_trans *t, t3_scene *sc) {
  uint32_t ver = t3_graph_version();
  if (!t->have || t->scene != sc || t->ver != ver) {
    cw_array_clear(&t->meshes);
    collect(t, &sc->base);
    t->scene = sc, t->ver = ver, t->have = true;
  }
  int n = 0;
  static bool warned;
  for (int i = 0; i < t->meshes.count; i++) {
    t3_mesh *m = t->meshes.data[i];
    for (int k = 0; k < m->material_count; k++) {
      t3_material *mt = m->materials[k];
      if (!mt) continue;
      bool tr = mt->transparent || (mt->type == T3_MESH_PHYSICAL_MATERIAL && mt->transmission > 0);
      int info = ext_info(mt);
      bool want = tr && (mt->fog || test_ignore_optout()) && !test_no_hfog();
      if (tr) n += m->base.visible;
      if (want && !(info & 2)) {
        if (info & 4) cw_shading_add(mt, "hfog");
        else if (!warned) {
          warned = true;
          cw_warn("fog: transparent material \"%s\" (extension x%s) cannot take the fog's shading part: it draws unfogged "
                  "over the fogged scene", mt->name ? mt->name : "", t3_material_get_extension(mt));
        }
      } else if (!want && (info & 2)) cw_shading_remove(mt, "hfog");
    }
  }
  return n;
}

static fog_trans *trans_of(cw_tree *t) {
  fog_trans *f = cw_tree_slot(t, "fog.trans");
  if (f) return f;
  f = cw_calloc(1, sizeof *f, CW_MEM_RENDER);
  cw_tree_set_slot(t, "fog.trans", f, fog_trans_free);
  f->next = trans_list;
  trans_list = f;
  return f;
}

bool cw__fog_transparents_wanted(const cw_pass_ctx *ctx) {
  if (!ctx->scene3d || !ctx->renderer || cw__fog_over_transparents() || !cw__fog_draws(ctx)) return false;
  fog_trans *t = trans_of(ctx->tree);
  uint64_t fr = cw_frame_index();
  if (t->frame != fr || t->scene != ctx->scene3d) {
    t->frame = fr;
    t->count = mark_transparents(t, ctx->scene3d);
  }
  return t->count > 0;
}

void cw__fog_transparents_begin(cw_pass_ctx *ctx) {
  fog_trans *t = trans_of(ctx->tree);
  t->renderer = ctx->renderer;
  cw__fog_fwd fw;
  cw__fog_forward(ctx, &fw);
  memset(t->hf, 0, sizeof t->hf);
  /* three.c's materials take the analytic fog (a volumetric fog's analytic medium, lit by its ambient and its sun:
   * the grid is a 3D texture three.c's programs cannot sample) */
  bool analytic = fw.mode == 1 || (fw.mode == 2 && fw.u[7] > 0.5f);
  if (!analytic) return;
  memcpy(t->hf[0], fw.u, 4 * sizeof(float));
  t->hf[1][0] = fw.mode == 1 ? fw.u[4] : 0, t->hf[1][2] = fw.u[6];
  memcpy(t->hf[2], fw.u + 8, 3 * sizeof(float));
  memcpy(t->hf[3], fw.u + 12, 3 * sizeof(float));
  memcpy(t->hf[4], fw.u + 16, 3 * sizeof(float));
}

/* the hfog part's hook: the drawing tree's fog, and how the material blends (an additive surface's light fades in
 * fog, an alpha-blended one's turns into the fog's) */
static void hfog_fill(t3_extension_ctx *c, void *user) {
  static const float none[4] = { 0, 0, 0, 0 };
  const fog_trans *t = NULL;
  for (const fog_trans *q = trans_list; q && !t; q = q->next)
    if (q->renderer == user) t = q;
  const t3_material *m = t3_extension_material(c);
  float b[4] = { 0, 1, 0, 0 };
  if (t) b[0] = t->hf[1][0], b[2] = t->hf[1][2];
  if (m && (m->blending == T3_ADDITIVE_BLENDING || m->blending == T3_SUBTRACTIVE_BLENDING)) b[1] = 0;
  if (m && m->blending == T3_MULTIPLY_BLENDING) b[1] = 0, b[3] = 1;
  t3_extension_set_uniform(c, "hfogA", t ? t->hf[0] : none, 4);
  t3_extension_set_uniform(c, "hfogB", b, 4);
  float col[4] = { 0, 0, 0, m && m->premultiplied_alpha ? 1.0f : 0.0f };
  if (t) memcpy(col, t->hf[2], 3 * sizeof(float));
  t3_extension_set_uniform(c, "hfogC", col, 4);
  t3_extension_set_uniform(c, "hfogD", t ? t->hf[3] : none, 4);
  t3_extension_set_uniform(c, "hfogE", t ? t->hf[4] : none, 4);
}

void cw__fog_renderer_init(t3_renderer *r) {
  if (!r) return;
  cw_shading_part(r, "hfog", hfog_fill, r);
  cw_shading_part_batching(r, "hfog", true); /* (per-frame values and the material's blending) */
}

static void fog_init(cw_node n, void *data) {
  (void)n;
  fog_node *f = data;
  f->density = 0.02f;
  f->height_falloff = 0.15f;
  f->color = (t3_color){ 0.5f, 0.6f, 0.7f };
  f->use_sky_color = true;
  f->sun_scatter = 0.5f;
  f->sun_g = 0.6f;
  f->max_opacity = 1;
  f->sky_distance = 400;
  f->volumetric_distance = 64;
  f->volumetric_detail_spread = 2;
  f->volumetric_temporal = 0.9f;
  f->volumetric_ambient = 1;
  f->volumetric_albedo = (t3_color){ 1, 1, 1 };
}

static void fog_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  fog_rt *f = fog_of(t);
  f->p = *(fog_node *)data;
  f->present = true;
  if (f->p.volumetric) cw__vfog_touch(t);
}

static void fog_exit(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  fog_rt *f = t ? cw_tree_slot(t, "fog") : NULL;
  if (f) f->present = false;
}

#define FF(field, lo, hi, docs) \
  { .name = #field, .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(fog_node, field), \
    .size = (int)sizeof(float), .min = lo, .max = hi, .doc = docs }

static const cw_prop fog_props[] = {
  FF(density, 0, 10, "Extinction per world unit at base_height."),
  FF(height_falloff, 0, 10, "How fast the density falls with height (per unit; 0 = uniform fog)."),
  FF(base_height, -1e5, 1e5, "World height where density is `density`."),
  CW_PROP_FIELD(fog_node, color, CW_T_COLOR, CW_PROP_DEFAULT, "In-scattered colour (linear radiance) when there is no sky or use_sky_color is off."),
  { .name = "use_sky_color", .type = CW_T_BOOL, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(fog_node, use_sky_color),
    .size = (int)sizeof(bool), .doc = "Take the colour from the ProceduralSky's horizon (when there is one)." },
  FF(sun_scatter, 0, 100, "Share of sunlight the fog scatters toward the camera (the glow around the sun)."),
  FF(sun_g, -0.95, 0.95, "Forward scattering of the sun lobe (Henyey-Greenstein g)."),
  FF(max_opacity, 0, 1, "Fog never hides more than this."),
  FF(start_distance, 0, 1e6, "No fog closer than this to the camera."),
  FF(sky_distance, 0, 1e6, "Distance the sky is fogged as (0 = the sky is never fogged)."),
  { .name = "volumetric", .type = CW_T_BOOL, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(fog_node, volumetric),
    .size = (int)sizeof(bool),
    .doc = "Volumetric fog: the fog near the camera is lit per cell of a 3D grid, with the lights' shadows (light shafts), "
           "FogVolume3D nodes and each light's fog_energy; the analytic fog continues beyond volumetric_distance." },
  FF(volumetric_distance, 1, 10000, "Volumetric: how far the grid reaches from the camera (world units)."),
  FF(volumetric_detail_spread, 1, 4, "Volumetric: how the grid's slices crowd toward the camera (1 = even, 2 = finer near)."),
  FF(volumetric_temporal, 0, 0.98, "Volumetric: share of the previous frames kept (hides the sampling noise; 0 = off)."),
  FF(volumetric_ambient, 0, 100, "Volumetric: scale of the fog colour as light the fog scatters from everywhere."),
  CW_PROP_FIELD(fog_node, volumetric_albedo, CW_T_COLOR, CW_PROP_DEFAULT, "Volumetric: share of the light the fog scatters (per channel)."),
  CW_PROP_END,
};

static cw_node_type fog_type = {
  .name = "HeightFog", .parent = "Node", .data_size = sizeof(fog_node), .props = fog_props,
  .doc = "Exponential height fog in the post chain (from depth): sky-coloured, with a sun lobe.",
  .init = fog_init, .pre_render = fog_pre_render, .exit_tree = fog_exit,
};

void cw__register_fog_types(void) { cw_node_type_register(&fog_type); }

/* HeightFog (cartwheel/environment.h): exponential height fog applied in
 * the post chain from the depth buffer, with a sun lobe. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "environment_internal.h"
#include "pipeline_internal.h"
#define S(...) #__VA_ARGS__ "\n"
#include "pipeline_post_wgsl.h"

typedef cw__height_fog fog_node;

typedef struct fog_rt {
  fog_node p;
  bool present;             /* a HeightFog is in the tree (exit_tree clears it) */
} fog_rt;

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
    "out vec4 o;\n"
    "void main() {\n"
    "\tvec4 f = fog_eval( v_uv, texture( u_depth, v_uv ).r );\n"
    "\tif ( f.a < 0.0 ) discard;\n"
    "\to = f;\n"
    "}\n";
const char *cw__fog_glsl(void) {
  static char buf[4096];
  if (!buf[0]) snprintf(buf, sizeof buf, "%s%s", FOG_DECL, FOG_FN);
  return buf;
}

static void fog_rt_free(void *p) { cw_free(p); }

static bool fog_active(const cw_pass_ctx *ctx, void *user) {
  fog_rt *f = user;
  (void)ctx;
  /* volumetric mode: the froxel pass draws this fog (volumetric_fog.c) */
  return f->present && f->p.density > 0 && !(f->p.volumetric && cw__vfog_supported());
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

static fog_rt *fog_of(cw_tree *t) {
  fog_rt *f = cw_tree_slot(t, "fog");
  if (f) return f;
  f = cw_calloc(1, sizeof *f, CW_MEM_RENDER);
  cw_tree_set_slot(t, "fog", f, fog_rt_free);
  cw_pipeline_add_pass(cw_pipeline_of(t), &(cw_pass_desc){ .name = "height_fog", .phase = CW_PHASE_EFFECTS, .order = 300,
                                                           .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                                           .active = fog_active, .execute = fog_exec, .user = f,
                                                           .webgpu = true });
  return f;
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

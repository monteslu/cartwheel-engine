/* HeightFog (cartwheel/environment.h): exponential height fog applied in
 * the post chain from the depth buffer, with a sun lobe. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "environment_internal.h"
#include "pipeline_internal.h"
#define S(...) #__VA_ARGS__ "\n"
#include "pipeline_post_wgsl.h"

typedef struct fog_node {
  float density, height_falloff, base_height;
  t3_color color;
  bool use_sky_color;
  float sun_scatter, sun_g, max_opacity, start_distance, sky_distance;
} fog_node;

typedef struct fog_rt {
  fog_node p;
  bool present;             /* a HeightFog is in the tree (exit_tree clears it) */
} fog_rt;

static const char FOG_FS[] =
    "uniform sampler2D u_depth;\n"
    "uniform mat4 u_inv_view_proj;\n"
    "uniform vec3 u_cam;\n"
    "uniform vec4 u_fog; /* density, falloff, base height, max opacity */\n"
    "uniform vec4 u_dist; /* start distance, sky distance */\n"
    "uniform vec3 u_color;\n"
    "uniform vec3 u_sun_dir; /* toward the sun */\n"
    "uniform vec3 u_sun_color; /* sunlight x sun_scatter */\n"
    "uniform float u_g;\n"
    "out vec4 o;\n"
    /* the fog's optical depth from the camera along v for distance l */
    "float opticalDepth( vec3 v, float l ) {\n"
    "\tfloat a = u_fog.x * exp( -u_fog.y * ( u_cam.y - u_fog.z ) );\n"
    "\tfloat k = u_fog.y * v.y * l;\n"
    "\treturn a * l * ( abs( k ) > 1e-4 ? ( 1.0 - exp( -k ) ) / k : 1.0 - 0.5 * k );\n"
    "}\n"
    "void main() {\n"
    "\tfloat d = texture( u_depth, v_uv ).r;\n"
    "\tvec4 a = u_inv_view_proj * vec4( v_uv * 2.0 - 1.0, -1.0, 1.0 );\n"
    "\tvec4 b = u_inv_view_proj * vec4( v_uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );\n"
    "\tvec3 p0 = a.xyz / a.w;\n"
    "\tvec3 ray = b.xyz / b.w - u_cam;\n"
    "\tfloat len = length( ray );\n"
    "\tvec3 v = ray / max( len, 1e-6 );\n"
    "\tif ( d >= 1.0 ) {\n"
    "\t\tif ( u_dist.y <= 0.0 ) discard;\n"
    "\t\tv = normalize( b.xyz / b.w - p0 );\n"
    "\t\tlen = u_dist.y;\n"
    "\t}\n"
    "\tfloat l = max( len - u_dist.x, 0.0 );\n"
    "\tif ( l <= 0.0 ) discard;\n"
    "\tfloat tau = opticalDepth( v, l );\n"
    "\tfloat fog = min( 1.0 - exp( -tau ), u_fog.w );\n"
    "\tfloat mu = dot( v, u_sun_dir ), g = u_g, g2 = g * g;\n"
    "\tfloat hg = ( 1.0 - g2 ) / ( 4.0 * 3.14159265 * pow( max( 1.0 + g2 - 2.0 * g * mu, 1e-4 ), 1.5 ) );\n"
    "\tvec3 sun = u_sun_color * hg * 4.0 * 3.14159265;\n"
    "\tvec3 inscatter = ( u_color + sun ) * fog;\n"
    "\to = vec4( inscatter, fog );\n"
    "}\n";

static void fog_rt_free(void *p) { cw_free(p); }

static bool fog_active(const cw_pass_ctx *ctx, void *user) {
  fog_rt *f = user;
  (void)ctx;
  return f->present && f->p.density > 0;
}

static unsigned fog_program(void) {
  static unsigned prog;
  if (!prog) {
    if (cw_gpu_backend() == CW_GPU_WEBGPU) {
      static char w[8192];
      snprintf(w, sizeof w, "%s%s", WGSL_COMMON, WGSL_FOG);
      prog = cw_pass_program_wgsl("fog", w);
    } else {
      prog = cw_pass_program("fog", FOG_FS);
    }
  }
  return prog;
}

/* the uniforms every variant shares */
static void fog_uniforms(cw_pass_ctx *ctx, unsigned pr, const fog_node *p, t3_color color, t3_vec3 sun, t3_color sun_color) {
  t3_vec3 cam = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  cw_pass_use(pr);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(pr, "u_depth", 0);
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
  fog_uniforms(ctx, pr, p, color, sun, sun_color);
  cw_pass_set_blend(CW_PASS_BLEND_PREMUL);
  cw_pass_fullscreen();
  cw_pass_set_blend(CW_PASS_BLEND_NONE);
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
}

static void fog_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  fog_rt *f = fog_of(t);
  f->p = *(fog_node *)data;
  f->present = true;
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
  CW_PROP_END,
};

static cw_node_type fog_type = {
  .name = "HeightFog", .parent = "Node", .data_size = sizeof(fog_node), .props = fog_props,
  .doc = "Exponential height fog in the post chain (from depth): sky-coloured, with a sun lobe.",
  .init = fog_init, .pre_render = fog_pre_render, .exit_tree = fog_exit,
};

void cw__register_fog_types(void) { cw_node_type_register(&fog_type); }

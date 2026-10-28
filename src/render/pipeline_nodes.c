/* The PostProcess node: the render pipeline's settings (pipeline.h) as
 * reflected properties, so scene files and the debug channel can set them. */
#include <string.h>

#include "cartwheel/pipeline.h"
#include "pipeline_internal.h"

typedef struct post_node {
  cw_post_settings s;
  const char *lut;       /* asset path of a .cube LUT (interned) */
  const char *lut_built; /* the path grade_lut was loaded from */
} post_node;

static const char *const tonemap_names[] = { "NONE", "LINEAR", "REINHARD", "CINEON", "ACES_FILMIC", "AGX", "NEUTRAL", NULL };
static const char *const aa_names[] = { "NONE", "FXAA", "SMAA", NULL };
static const char *const view_names[] = { "NONE", "DEPTH", "NORMALS", "AO", "VELOCITY", "BLOOM", NULL };

static void post_init(cw_node n, void *data) {
  (void)n;
  post_node *p = data;
  /* the pipeline's defaults */
  cw_post_settings *s = &p->s;
  memset(s, 0, sizeof *s);
  s->hdr = true;
  s->exposure = 1;
  s->tonemap = CW_TONEMAP_AGX;
  s->bloom_intensity = 0.05f;
  s->bloom_knee = 0.5f;
  s->bloom_radius = 1;
  s->bloom_levels = 6;
  s->ssao_radius = 0.5f;
  s->ssao_intensity = 1;
  s->ssao_power = 1.5f;
  s->ssao_slices = 2;
  s->ssao_steps = 6;
  s->ssr_max_distance = 1;
  s->ssr_thickness = 0.1f;
  s->ssr_quality = 0.5f;
  s->ssr_intensity = 1;
  s->ssgi_radius = 12;
  s->dof_focus_distance = 3;
  s->dof_focus_range = 1;
  s->dof_blur = 8;
  s->dof_near_blur = 0.5f;
  s->ssgi_intensity = 10;
  s->ssgi_ao_intensity = 1;
  s->ssgi_thickness = 1;
  s->ssgi_slices = 2;
  s->ssgi_steps = 8;
  s->taa_feedback = 0.9f;
  s->taa_samples = 8;
  s->grade_intensity = 1;
  s->vignette_intensity = 0.35f;
  s->vignette_smoothness = 0.5f;
  s->grain_intensity = 0.04f;
  s->ca_intensity = 0.004f;
  p->lut = cw_intern("");
}

static void post_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  post_node *p = data;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  if (p->lut != p->lut_built) {
    if (p->s.grade_lut) cw_lut_free(p->s.grade_lut);
    p->s.grade_lut = p->lut && p->lut[0] ? cw_lut_load_cube(p->lut) : 0;
    p->lut_built = p->lut;
  }
  cw_pipeline *pl = cw_pipeline_of(t);
  *cw_pipeline_settings(pl) = p->s;
}

static void post_destroy(cw_node n, void *data) {
  (void)n;
  post_node *p = data;
  if (p->s.grade_lut) cw_lut_free(p->s.grade_lut);
  p->s.grade_lut = 0;
}

#define FR(field, lo, hi, docs) \
  { .name = #field, .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(post_node, s.field), \
    .size = (int)sizeof(float), .min = lo, .max = hi, .doc = docs }
#define FI(field, lo, hi, docs) \
  { .name = #field, .type = CW_T_INT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(post_node, s.field), \
    .size = (int)sizeof(int), .min = lo, .max = hi, .doc = docs }
#define FB(field, docs) \
  { .name = #field, .type = CW_T_BOOL, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(post_node, s.field), \
    .size = (int)sizeof(bool), .doc = docs }
#define FE(field, names, docs) \
  { .name = #field, .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(post_node, s.field), \
    .size = (int)sizeof(int), .enum_names = names, .doc = docs }

CW_STATIC_ASSERT(sizeof(cw_debug_view) == sizeof(int) && sizeof(cw_tonemap) == sizeof(int) && sizeof(cw_aa_mode) == sizeof(int),
                 "enum properties are int-sized");

static const cw_prop post_props[] = {
  FB(hdr, "Render the scene into RGBA16F (linear HDR; off: RGBA8, banding in the darks, for MSAA on a compatibility-mode WebGPU device)."),
  FI(msaa, 0, 8, "MSAA samples on the scene pass (0 = off; WebGPU multisamples 4x for any of 2-8)."),
  FB(depth_prepass, "Depth-only pass before the scene (early-z for heavy fragment shaders; GL and WebGPU, the same picture)."),
  FR(exposure, 0, 1000, "Linear exposure multiplier (toneMappingExposure)."),
  FR(exposure_ev, -20, 20, "Exposure in stops (multiplies by 2^ev)."),
  FE(tonemap, tonemap_names, "Tone curve (three.js r186's: AGX by default)."),
  FB(bloom, "Bloom (dual-filter, energy conserving)."),
  FR(bloom_intensity, 0, 1, "Share of the bright energy spread by bloom."),
  FR(bloom_threshold, 0, 1e6, "Scene-linear brightness where bloom starts (0 = all light)."),
  FR(bloom_knee, 0, 1, "Soft transition around the threshold, as a fraction of it."),
  FR(bloom_radius, 0.1, 4, "Upsample tent radius in texels of each level."),
  FI(bloom_levels, 1, 10, "Downsample levels (each halves the size)."),
  FB(ssao, "Ground-truth ambient occlusion (half resolution, depth-aware upsample)."),
  FR(ssao_radius, 0.01, 100, "AO radius in world units."),
  FR(ssao_intensity, 0, 1, "AO strength."),
  FR(ssao_power, 0.1, 8, "Contrast curve on the visibility."),
  FI(ssao_slices, 1, 8, "Directions per pixel."),
  FI(ssao_steps, 1, 16, "Horizon steps per direction side."),
  FB(ssr, "Screen-space reflections (half resolution) of surfaces with the GI shading part (gi.h)."),
  FR(ssr_max_distance, 0.01, 1000, "How far a screen-space reflection reaches (view units)."),
  FR(ssr_thickness, 0.001, 100, "How deep a surface counts as hit behind the depth."),
  FR(ssr_quality, 0, 1, "March steps per pixel of the ray's screen length."),
  FR(ssr_intensity, 0, 10, "Screen-space reflection strength."),
  FB(ssgi, "Screen-space bounce light and AO (half resolution) of surfaces with the GI shading part (gi.h)."),
  FR(ssgi_radius, 0.1, 100, "SSGI sampling radius (r186's, screen space)."),
  FR(ssgi_intensity, 0, 100, "Bounce light strength."),
  FR(ssgi_ao_intensity, 0, 10, "Its ambient occlusion strength."),
  FR(ssgi_thickness, 0.01, 100, "How thick a surface is assumed behind the depth."),
  FI(ssgi_slices, 1, 8, "Directions per pixel."),
  FI(ssgi_steps, 1, 64, "Steps per direction side."),
  FB(taa, "Temporal antialiasing (camera jitter, reprojection, history clipping)."),
  FR(taa_feedback, 0, 0.98, "History weight."),
  FI(taa_samples, 1, 64, "Jitter positions (Halton 2,3)."),
  FE(aa, aa_names, "Post antialiasing on display values."),
  FB(grade, "Colour grading with the 3D LUT in `lut`."),
  FR(grade_intensity, 0, 1, "Blend between the ungraded and graded image."),
  CW_PROP_FIELD(post_node, lut, CW_T_STRING, CW_PROP_DEFAULT, "Asset path of a .cube 3D LUT."),
  FB(dof, "Depth of field: blur away from the focus distance (off on the Low effects preset)."),
  FR(dof_focus_distance, 0, 1e6, "View distance that is sharp (world units)."),
  FR(dof_focus_range, 0, 1e6, "Depth around the focus distance that stays sharp."),
  FR(dof_blur, 0, 32, "Largest blur radius in pixels of a 1080 line image."),
  FR(dof_near_blur, 0, 1, "Blur in front of the focus, as a share of dof_blur."),
  FB(vignette, "Darken toward the corners."),
  FR(vignette_intensity, 0, 1, "Vignette strength."),
  FR(vignette_smoothness, 0.01, 1, "Vignette falloff width."),
  FB(grain, "Film grain (integer hash noise, new every frame)."),
  FR(grain_intensity, 0, 1, "Grain strength."),
  FB(chromatic_aberration, "Radial red / blue channel offset."),
  FR(ca_intensity, 0, 0.1, "Channel offset at the corners (uv)."),
  FE(debug_view, view_names, "Show an intermediate buffer instead of the image (depth, normals, AO, velocity, bloom)."),
  CW_PROP_END,
};

static cw_node_type post_type = {
  .name = "PostProcess", .parent = "Node", .data_size = sizeof(post_node), .props = post_props,
  .doc = "The render pipeline's settings for this tree (one per tree; the last in tree order wins): HDR, "
         "MSAA, exposure, tone curve, bloom, SSAO, TAA, FXAA/SMAA, LUT grading, vignette, grain, chromatic aberration.",
  .init = post_init, .pre_render = post_pre_render, .destroy = post_destroy,
};

void cw__register_pipeline_types(void);
void cw__register_pipeline_types(void) { cw_node_type_register(&post_type); }

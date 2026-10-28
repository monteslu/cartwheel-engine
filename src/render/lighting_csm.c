/* Cascaded shadow maps for the sun (cartwheel/lighting.h), three.js r186's
 * CSMShadowNode (examples/jsm/csm/CSMShadowNode.js, CSMFrustum.js) on the
 * engine's side: the practical split, each cascade's slice of the view
 * fitted with r186's bounding width and texel snapping, the cascades rendered
 * with three.c's caster programs (t3_renderer_render_depth_to) into the
 * quarters of one depth atlas, and the values the "csm" shading part reads
 * (src/render/extensions/csm_part.mjs).
 *
 * A cascade is re-rendered only when its light matrix or any shadow caster
 * changed (r186 renders them every frame; the image is the same). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/math.h"
#include "cartwheel/profile.h"
#include "lighting_internal.h"
#if CW_WGPU
#include "pipeline_wgpu.h"
#include "platform/wgpu.h"
#endif

#define LIGHT_MARGIN 200.0f /* r186 CSMShadowNode lightMargin */

struct cw__csm {
  int size, count;            /* one cascade's texels, cascades in use */
  bool fade;
  unsigned gl_tex, gl_fbo;    /* GL: the atlas and its depth-only FBO */
#if CW_WGPU
  WGPUTexture wg_tex;
  WGPUTextureView wg_view;
#endif
  t3_texture *tex;            /* the atlas for three.c (adopted) */
  t3_camera *cam;             /* the cascade's orthographic camera */
  float mats[4][16];          /* world -> atlas uv (before r186's y flip) and depth */
  float ranges[8];
  float near, far;
  float dir[4], color[4], bias[4];
  uint64_t key[4];
  bool valid[4];
  int rendered;
  /* the shadow casters' key, once a frame (split screen fits the cascades once a
   * view; the casters are the same for every view) */
  uint64_t casters, casters_frame;
  const void *casters_pipe;
};

cw__csm *cw__csm_new(void) {
  cw__csm *c = cw_calloc(1, sizeof *c, CW_MEM_RENDER);
  c->cam = t3_orthographic_camera_new(-1, 1, 1, -1, 0.5f, 500);
  return c;
}

static void free_maps(cw__csm *c) {
  if (c->tex) t3_release(c->tex);
  c->tex = NULL;
  if (c->gl_fbo) glDeleteFramebuffers(1, &c->gl_fbo);
  if (c->gl_tex) glDeleteTextures(1, &c->gl_tex);
  c->gl_fbo = c->gl_tex = 0;
#if CW_WGPU
  if (c->wg_view) wgpuTextureViewRelease(c->wg_view);
  if (c->wg_tex) wgpuTextureRelease(c->wg_tex);
  c->wg_view = NULL;
  c->wg_tex = NULL;
#endif
  memset(c->valid, 0, sizeof c->valid);
}

void cw__csm_free(cw__csm *c) {
  if (!c) return;
  free_maps(c);
  t3_release(c->cam);
  cw_free(c);
}

void cw__csm_stats(const cw__csm *c, cw__csm_info *out) {
  out->cascades = c->count;
  out->rendered = c->rendered;
  out->size = c->size;
}

/* the atlas: 2 x 2 cascades of size texels, a depth texture sampled with compare */
static bool make_maps(cw__csm *c, int size) {
  free_maps(c);
  int n = size * 2;
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    WGPUTextureDescriptor d = { .label = CW_WGPU_SV("csm.atlas"),
                                .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
                                .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)n, (uint32_t)n, 1 },
                                .format = WGPUTextureFormat_Depth32Float, .mipLevelCount = 1, .sampleCount = 1 };
    c->wg_tex = wgpuDeviceCreateTexture(cw__wgpu_device(), &d);
    if (!c->wg_tex) return false;
    c->wg_view = wgpuTextureCreateView(c->wg_tex, NULL);
    /* (the wrapper does not own them: free_maps releases them after it) */
    c->tex = t3_texture_from_wgpu(c->wg_tex, c->wg_view, n, n, 1, false, false);
  } else
#endif
  {
    glGenTextures(1, &c->gl_tex);
    glBindTexture(GL_TEXTURE_2D, c->gl_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, n, n, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &c->gl_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c->gl_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, c->gl_tex, 0);
    GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) return false;
    c->tex = t3_texture_from_gl(c->gl_tex, n, n, 1, false, false);
  }
  if (!c->tex) return false;
  /* a texture the GPU makes, as three.js's DepthTexture: sampled the way
   * three.c samples its own shadow maps */
  c->tex->gpu_made = true;
  c->tex->mag_filter = c->tex->min_filter = T3_LINEAR;
  c->size = size;
  return true;
}

#if CW_WGPU
/* A WebGPU render pass clears its whole attachment (loadOp), not a rect:
 * a shadow map tile is cleared by drawing the far depth over it instead
 * (one triangle, depth always written), so the other tiles keep theirs. */
static const char CLEAR_WGSL[] =
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;\n"
    "  return vec4f(p, 1.0, 1.0);\n"
    "}\n"
    "@fragment fn fs() {}\n";

void cw__shadow_clear_rect_wgpu(void *view, int x, int y, int w, int h) {
  static WGPURenderPipeline pipe;
  if (!pipe) {
    WGPUShaderModule sm = cw__wgpu_shader("shadow.clear", CLEAR_WGSL);
    if (!sm) return;
    WGPUDepthStencilState ds = { .format = WGPUTextureFormat_Depth32Float, .depthWriteEnabled = WGPUOptionalBool_True,
                                 .depthCompare = WGPUCompareFunction_Always,
                                 .stencilFront = { .compare = WGPUCompareFunction_Always },
                                 .stencilBack = { .compare = WGPUCompareFunction_Always } };
    WGPUFragmentState fs = { .module = sm, .entryPoint = CW_WGPU_SV("fs") };
    WGPURenderPipelineDescriptor d = { .label = CW_WGPU_SV("shadow.clear"),
                                       .vertex = { .module = sm, .entryPoint = CW_WGPU_SV("vs") },
                                       .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
                                       .depthStencil = &ds, .multisample = { .count = 1, .mask = 0xffffffffu },
                                       .fragment = &fs };
    pipe = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
    wgpuShaderModuleRelease(sm);
    if (!pipe) return;
  }
  WGPURenderPassDepthStencilAttachment da = { .view = (WGPUTextureView)view, .depthLoadOp = WGPULoadOp_Load,
                                              .depthStoreOp = WGPUStoreOp_Store };
  WGPURenderPassDescriptor rp = { .label = CW_WGPU_SV("shadow.clear"), .depthStencilAttachment = &da };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(cw__pw_encoder(), &rp);
  wgpuRenderPassEncoderSetViewport(p, (float)x, (float)y, (float)w, (float)h, 0, 1);
  wgpuRenderPassEncoderSetScissorRect(p, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
  wgpuRenderPassEncoderSetPipeline(p, pipe);
  wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
}
#endif

/* ── r186 CSMFrustum / CSMShadowNode ────────────────────────────────── */

typedef struct frust { t3_vec3 n[4], f[4]; } frust;

static uint64_t mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}
static uint64_t mixf(uint64_t h, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return mix64(h, u);
}

/* every shadow caster's state (r186 renders the cascades every frame; a
 * cascade is redrawn here when this or its matrix changed) */
static uint64_t casters_key(const t3_object *o, uint64_t h) {
  if (!o->visible) return h;
  if ((o->type == T3_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SKINNED_MESH) && o->cast_shadow) {
    const t3_mesh *m = (const t3_mesh *)o;
    h = mix64(h, (uint64_t)(uintptr_t)o);
    for (int i = 0; i < 16; i++) h = mixf(h, o->matrix_world.e[i]);
    if (m->geometry) h = mix64(mix64(h, (uint64_t)(uintptr_t)m->geometry), m->geometry->version);
    for (int i = 0; i < m->material_count; i++)
      if (m->materials[i]) h = mix64(h, m->materials[i]->version);
    if (o->type == T3_INSTANCED_MESH) {
      const t3_instanced_mesh *im = (const t3_instanced_mesh *)o;
      h = mix64(mix64(h, (uint64_t)im->count), im->instance_matrix ? im->instance_matrix->version : 0);
    }
    if (o->type == T3_SKINNED_MESH) {
      const t3_skinned_mesh *sm = (const t3_skinned_mesh *)o;
      if (sm->skeleton)
        for (int b = 0; b < sm->skeleton->bone_count; b++)
          for (int i = 0; i < 16; i++) h = mixf(h, sm->skeleton->bones[b]->matrix_world.e[i]);
    }
    for (int i = 0; i < m->morph_influence_count; i++) h = mixf(h, m->morph_influences[i]);
  }
  for (int i = 0; i < o->child_count; i++) h = casters_key(o->children[i], h);
  return h;
}

/* Matrix4.lookAt(eye, target, up) as three.js builds it (rotation only):
 * z = eye - target, x = up x z, y = z x x */
static void look_rotation(t3_mat4 *m, t3_vec3 eye, t3_vec3 target, t3_vec3 up) {
  t3_vec3 z = t3_vec3_sub(eye, target);
  if (t3_vec3_length_sq(z) == 0) z.z = 1;
  z = t3_vec3_normalize(z);
  t3_vec3 x = t3_vec3_cross(up, z);
  if (t3_vec3_length_sq(x) == 0) {
    if (fabsf(up.z) == 1) z.x += 0.0001f;
    else z.z += 0.0001f;
    z = t3_vec3_normalize(z);
    x = t3_vec3_cross(up, z);
  }
  x = t3_vec3_normalize(x);
  t3_vec3 y = t3_vec3_cross(z, x);
  float e[16] = { x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, 0, 0, 0, 1 };
  memcpy(m->e, e, sizeof e);
}

static t3_vec3 lerp3(t3_vec3 a, t3_vec3 b, float t) { return t3_vec3_add(a, t3_vec3_scale(t3_vec3_sub(b, a), t)); }

bool cw__csm_update(cw__csm *c, cw_pass_ctx *ctx, t3_light *sun, const cw__csm_cfg *cfg) {
  CW_PROFILE_SCOPE("lighting.csm");
  c->rendered = 0;
  int n = CW_CLAMP(cfg->cascades, 1, 4);
  int size = CW_CLAMP(cfg->size, 64, 8192);
  if (!c->tex || c->size != size)
    if (!make_maps(c, size)) {
      cw_error("lighting: a %dx%d depth texture for the sun's cascades cannot be made here", size * 2, size * 2);
      free_maps(c);
      return false;
    }
  if (n != c->count || cfg->fade != c->fade) memset(c->valid, 0, sizeof c->valid);
  c->count = n;
  c->fade = cfg->fade;
  t3_camera *cam = ctx->camera;
  const t3_light_shadow *ls = sun->shadow;
  /* r186 _getBreaks, practical (lambda 0.5) over [near, min(far, maxFar)] */
  float near = cam->near, far = cfg->distance > 0 ? CW_MIN(cam->far, cfg->distance) : cam->far;
  if (far <= near * 1.01f) far = near * 2;
  float breaks[4];
  for (int i = 1; i < n; i++) {
    float u = (near + (far - near) * (float)i / (float)n) / far;
    float l = (near * cw_pow(far / near, (float)i / (float)n)) / far;
    breaks[i - 1] = u + (l - u) * 0.5f;
  }
  breaks[n - 1] = 1;
  for (int i = 0; i < 4; i++) {
    c->ranges[2 * i] = i < n ? (i ? breaks[i - 1] : 0) : 2;
    c->ranges[2 * i + 1] = i < n ? breaks[i] : 2;
  }
  c->near = near;
  c->far = far;
  /* r186 CSMFrustum.setFromProjectionMatrix (WebGL clip z -1 .. 1: three.c's
   * matrices), the far corners pulled in to maxFar */
  t3_mat4 ip;
  t3_mat4_invert(&ip, &ctx->proj);
  bool ortho = ctx->proj.e[11] == 0;
  frust main;
  static const float cx[4] = { 1, 1, -1, -1 }, cy[4] = { 1, -1, -1, 1 };
  for (int k = 0; k < 4; k++) {
    main.n[k] = t3_vec3_apply_mat4((t3_vec3){ cx[k], cy[k], -1 }, &ip);
    t3_vec3 f = t3_vec3_apply_mat4((t3_vec3){ cx[k], cy[k], 1 }, &ip);
    float s = CW_MIN(far / fabsf(f.z), 1.0f);
    if (ortho) f.z *= s;
    else f = t3_vec3_scale(f, s);
    main.f[k] = f;
  }
  /* split */
  frust fr[4];
  float fn = main.n[0].z, ff = main.f[0].z;
  for (int i = 0; i < n; i++) {
    float a0 = i == 0 ? 0 : (breaks[i - 1] * ff - fn) / (ff - fn);
    float a1 = i == n - 1 ? 1 : (breaks[i] * ff - fn) / (ff - fn);
    for (int k = 0; k < 4; k++) {
      fr[i].n[k] = i == 0 ? main.n[k] : lerp3(main.n[k], main.f[k], a0);
      fr[i].f[k] = i == n - 1 ? main.f[k] : lerp3(main.n[k], main.f[k], a1);
    }
  }
  /* the light: where it shines, its orientation (r186 _lightOrientationMatrix) */
  t3_vec3 lp = t3_mat4_get_position(&sun->base.matrix_world);
  t3_vec3 tp = sun->target ? t3_mat4_get_position(&sun->target->matrix_world) : t3_vec3_add(lp, (t3_vec3){ 0, -1, 0 });
  t3_vec3 ldir = t3_vec3_normalize(t3_vec3_sub(tp, lp));
  t3_mat4 orient, orient_inv, cam_to_light;
  look_rotation(&orient, lp, tp, (t3_vec3){ 0, 1, 0 });
  t3_mat4_invert(&orient_inv, &orient);
  t3_mat4_multiply(&cam_to_light, &orient_inv, &cam->base.matrix_world);
  float bias = ls ? ls->bias : 0, nbias = ls ? ls->normal_bias : 0;
  float snear = ls && ls->camera ? ls->camera->near : 0.5f, sfar = ls && ls->camera ? ls->camera->far : 500;
  if (c->casters_frame != ctx->frame + 1 || c->casters_pipe != ctx->pipeline) {
    c->casters = casters_key(&ctx->scene3d->base, 1469598103934665603ull);
    c->casters_frame = ctx->frame + 1;
    c->casters_pipe = ctx->pipeline;
  }
  uint64_t ck = c->casters;
  bool webgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  for (int i = 0; i < n; i++) {
    /* r186 _updateShadowBounds: the slice's longest diagonal, widened by the fade margin */
    t3_vec3 p1 = fr[i].f[0];
    t3_vec3 p2 = t3_vec3_distance(p1, fr[i].f[2]) > t3_vec3_distance(p1, fr[i].n[2]) ? fr[i].f[2] : fr[i].n[2];
    float width = t3_vec3_distance(p1, p2);
    if (c->fade) {
      float fmax = CW_MAX(cam->far, cfg->distance > 0 ? cfg->distance : cam->far);
      float lin = fr[i].f[0].z / (fmax - near);
      width += 0.25f * lin * lin * (fmax - near);
    }
    float half = width / 2, texel = width / (float)size;
    /* r186 updateBefore: the slice's box in light space, its centre snapped to texels */
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (int k = 0; k < 8; k++) {
      t3_vec3 v = t3_vec3_apply_mat4(k < 4 ? fr[i].n[k] : fr[i].f[k - 4], &cam_to_light);
      float a[3] = { v.x, v.y, v.z };
      for (int q = 0; q < 3; q++) mn[q] = CW_MIN(mn[q], a[q]), mx[q] = CW_MAX(mx[q], a[q]);
    }
    t3_vec3 ctr = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, mx[2] + LIGHT_MARGIN };
    ctr.x = floorf(ctr.x / texel) * texel;
    ctr.y = floorf(ctr.y / texel) * texel;
    ctr = t3_vec3_apply_mat4(ctr, &orient);
    /* the cascade's camera at ctr looking along the light */
    t3_camera *oc = c->cam;
    oc->left = -half, oc->right = half, oc->top = half, oc->bottom = -half;
    oc->near = snear, oc->far = sfar, oc->zoom = 1;
    t3_camera_update_projection_matrix(oc);
    oc->base.position = ctr;
    oc->base.up = (t3_vec3){ 0, 1, 0 };
    t3_object_look_at(oc, ctr.x + ldir.x, ctr.y + ldir.y, ctr.z + ldir.z);
    t3_object_update_matrix_world(oc, false);
    /* LightShadow.updateMatrices: bias * projection * view; then the quarter
     * (before the y flip) and r186's per-cascade bias, bias * (i + 1) */
    t3_mat4 b = { { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0.5f, 0.5f, 0.5f, 1 } }, t, m;
    t3_mat4 qm = { { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, (float)(i & 1) * 0.5f, (float)(1 - (i >> 1)) * 0.5f,
                     bias * (float)(i + 1), 1 } };
    t3_mat4_multiply(&t, &b, &oc->projection_matrix);
    t3_mat4_multiply(&m, &t, &oc->matrix_world_inverse);
    t3_mat4_multiply(&t, &qm, &m);
    memcpy(c->mats[i], t.e, sizeof c->mats[i]);
    uint64_t key = ck;
    for (int k = 0; k < 16; k++) key = mixf(key, t.e[k]);
    if (c->valid[i] && c->key[i] == key) continue;
    /* render the cascade into its quarter (GL counts rows from the bottom,
     * and three.c flips a GPU-made texture there; WebGPU from the top) */
    int row = i >> 1;
    t3_external_target et = { 0 };
    et.x = (i & 1) * size;
    et.y = (webgpu ? row : 1 - row) * size;
    et.w = et.h = size;
    et.samples = 1;
    et.clear_depth = 1;
    et.load_depth = false; /* GL clears the scissored rect */
#if CW_WGPU
    if (webgpu) {
      et.depth = (uintptr_t)c->wg_view;
      et.depth_format = WGPUTextureFormat_Depth32Float;
      /* recorded on the frame's encoder, ahead of the scene pass that samples it;
       * the quarter cleared by a draw (a cleared pass would clear the others too) */
      cw__shadow_clear_rect_wgpu(c->wg_view, et.x, et.y, et.w, et.h);
      t3_renderer_set_external_encoder(ctx->renderer, (uintptr_t)cw__pw_encoder());
      et.load_depth = true;
    }
#endif
    if (!webgpu) et.color = c->gl_fbo;
    t3_renderer_render_depth_to(ctx->renderer, ctx->scene3d, oc, &et);
    c->key[i] = key;
    /* (not kept from the renderer's first frame: three.c's GL depth pass filled no
     * camera for casters drawn before its first scene render, so the map was
     * empty; the next frame draws it again) */
    c->valid[i] = ctx->frame > 0;
    c->rendered++;
  }
  if (!webgpu) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    t3_renderer_reset_bindings(ctx->renderer);
  }
  /* the light as the part shades it: r186 lightTargetDirection (to the
   * light, view space), colour x intensity, shadow intensity */
  t3_vec3 d = t3_vec3_normalize(t3_vec3_transform_direction(t3_vec3_sub(lp, tp), &ctx->view));
  c->dir[0] = d.x, c->dir[1] = d.y, c->dir[2] = d.z, c->dir[3] = ls ? ls->intensity : 1;
  c->color[0] = sun->color.r * sun->intensity, c->color[1] = sun->color.g * sun->intensity;
  c->color[2] = sun->color.b * sun->intensity, c->color[3] = (float)n;
  float radius = ls ? ls->radius : 1;
  c->bias[0] = 0, c->bias[1] = nbias, c->bias[2] = 1.0f / (float)(size * 2), c->bias[3] = radius;
  return true;
}

void cw__csm_fill(cw__csm *c, t3_extension_ctx *x) {
  static const char *const names[4] = { "csmMatrix0", "csmMatrix1", "csmMatrix2", "csmMatrix3" };
  for (int i = 0; i < 4; i++) t3_extension_set_uniform(x, names[i], c->mats[i < c->count ? i : 0], 16);
  t3_extension_set_uniform(x, "csmDir", c->dir, 4);
  t3_extension_set_uniform(x, "csmColor", c->color, 4);
  t3_extension_set_uniform(x, "csmBias", c->bias, 4);
  float depth[4] = { c->near, c->far, c->fade ? 1.0f : 0.0f, 0 };
  t3_extension_set_uniform(x, "csmDepth", depth, 4);
  t3_extension_set_uniform(x, "csmRanges0", c->ranges, 4);
  t3_extension_set_uniform(x, "csmRanges1", c->ranges + 4, 4);
  const t3_object *o = t3_extension_object(x);
  float rec = o && o->receive_shadow ? 1.0f : 0.0f;
  t3_extension_set_uniform(x, "csmReceive", &rec, 1);
  t3_extension_set_texture(x, "csmMap", c->tex);
}


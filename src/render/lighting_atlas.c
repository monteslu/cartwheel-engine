/* The shadow atlas for clustered spot and omni lights (cartwheel/lighting.h):
 * one depth texture, a tile per spot light and a 3 x 2 block of cube faces
 * per omni light, each light's tile sized by how big the light is on screen
 * (at most its shadow_size), packed largest first. Tiles are drawn with
 * three.c's caster programs (t3_renderer_render_depth_to); a tile is redrawn
 * only when its light moved or a shadow caster inside the light's range
 * changed, and at most `budget` faces a frame are redrawn (the most
 * important lights first; the rest keep the picture and matrix they were
 * drawn with). The "cluster" shading part samples the atlas with r187's PCF
 * filter, comparing by hand (textureLoad: the reads sit inside the per-light
 * loop, where WGSL forbids textureSampleCompare). */
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

/* the cube faces: where each looks and its up (three.js lookAt), the same
 * table the cluster part's TSL builds its face bases from */
static const float FACE_DIR[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float FACE_UP[6][3] = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };

typedef struct caster { float x, y, z, r; uint64_t key; } caster;

typedef struct tile {
  const t3_light *light;
  int size, x, y, w, h;          /* texels (top-down); omni: the 3 x 2 block */
  bool omni;
  uint64_t light_key, face_key[6];
  bool valid[6];
  /* what the tile holds (kept while it is not redrawn) */
  float mat[16];                 /* spot: world -> atlas uv (before the y flip) and depth */
  t3_vec3 pos;
  float near, far;
} tile;

struct cw__atlas {
  int size;
  unsigned gl_tex, gl_fbo;
#if CW_WGPU
  WGPUTexture wg_tex, wg_r32;      /* the depth the casters draw; its R32Float copy the shader reads */
  WGPUTextureView wg_view, wg_r32_view;
  WGPUBindGroup wg_copy_bg;
#endif
  t3_texture *tex;
  t3_camera *pcam;
  CW_ARRAY(tile) tiles;
  CW_ARRAY(caster) casters;
  uint64_t layout_key;
  cw__atlas_info info;
  bool warned_full;
};

cw__atlas *cw__atlas_new(void) {
  cw__atlas *a = cw_calloc(1, sizeof *a, CW_MEM_RENDER);
  a->pcam = t3_perspective_camera_new(90, 1, 0.1f, 10);
  return a;
}

static void free_tex(cw__atlas *a) {
  if (a->tex) t3_release(a->tex);
  a->tex = NULL;
  if (a->gl_fbo) glDeleteFramebuffers(1, &a->gl_fbo);
  if (a->gl_tex) glDeleteTextures(1, &a->gl_tex);
  a->gl_fbo = a->gl_tex = 0;
#if CW_WGPU
  if (a->wg_copy_bg) wgpuBindGroupRelease(a->wg_copy_bg);
  if (a->wg_r32_view) wgpuTextureViewRelease(a->wg_r32_view);
  if (a->wg_r32) wgpuTextureRelease(a->wg_r32);
  if (a->wg_view) wgpuTextureViewRelease(a->wg_view);
  if (a->wg_tex) wgpuTextureRelease(a->wg_tex);
  a->wg_view = a->wg_r32_view = NULL;
  a->wg_tex = a->wg_r32 = NULL;
  a->wg_copy_bg = NULL;
#endif
  a->size = 0;
}

void cw__atlas_free(cw__atlas *a) {
  if (!a) return;
  free_tex(a);
  cw_array_free(&a->tiles);
  cw_array_free(&a->casters);
  t3_release(a->pcam);
  cw_free(a);
}

t3_texture *cw__atlas_texture(cw__atlas *a) { return a->tex; }
void cw__atlas_get_info(const cw__atlas *a, cw__atlas_info *out) { *out = a->info; }

/* a depth texture read by hand (no compare mode), drawn into with an FBO on GL */
static bool make_tex(cw__atlas *a, int n) {
  free_tex(a);
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    WGPUTextureDescriptor d = { .label = CW_WGPU_SV("shadow.atlas"),
                                .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
                                .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)n, (uint32_t)n, 1 },
                                .format = WGPUTextureFormat_Depth32Float, .mipLevelCount = 1, .sampleCount = 1 };
    a->wg_tex = wgpuDeviceCreateTexture(cw__wgpu_device(), &d);
    if (!a->wg_tex) return false;
    a->wg_view = wgpuTextureCreateView(a->wg_tex, NULL);
    /* compatibility mode reads a depth texture only through a comparison
     * sampler, which WGSL allows only in uniform control flow (not in the
     * per-light loop): the shader reads an R32Float copy with textureLoad */
    d.label = CW_WGPU_SV("shadow.atlas.r32");
    d.format = WGPUTextureFormat_R32Float;
    a->wg_r32 = wgpuDeviceCreateTexture(cw__wgpu_device(), &d);
    if (!a->wg_r32) return false;
    a->wg_r32_view = wgpuTextureCreateView(a->wg_r32, NULL);
    a->tex = t3_texture_from_wgpu(a->wg_r32, a->wg_r32_view, n, n, 1, false, false);
    /* clear it once: tiles are cleared one by one from then on */
    cw__shadow_clear_rect_wgpu(a->wg_view, 0, 0, n, n);
  } else
#endif
  {
    glGenTextures(1, &a->gl_tex);
    glBindTexture(GL_TEXTURE_2D, a->gl_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, n, n, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glGenFramebuffers(1, &a->gl_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, a->gl_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, a->gl_tex, 0);
    GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
      glViewport(0, 0, n, n);
      glDisable(GL_SCISSOR_TEST);
      glDepthMask(GL_TRUE);
      glClearDepthf(1);
      glClear(GL_DEPTH_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) return false;
    a->tex = t3_texture_from_gl(a->gl_tex, n, n, 1, false, false);
  }
  if (!a->tex) return false;
  /* drawn by the GPU: three.c reads its rows as it reads its own shadow maps' */
  a->tex->gpu_made = true;
  a->tex->mag_filter = a->tex->min_filter = T3_NEAREST;
  a->size = n;
  a->layout_key = 0;
  return true;
}

/* ── casters ────────────────────────────────────────────────────────── */

static uint64_t mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}
static uint64_t mixf(uint64_t h, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return mix64(h, u);
}

static void gather_casters(cw__atlas *a, const t3_object *o) {
  if (!o->visible) return;
  if ((o->type == T3_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SKINNED_MESH) && o->cast_shadow) {
    const t3_mesh *m = (const t3_mesh *)o;
    t3_geometry *g = m->geometry;
    if (g && g->attributes[T3_ATTR_POSITION]) {
      if (!g->has_bounding_sphere) t3_geometry_compute_bounding_sphere(g);
      caster c;
      uint64_t k = mix64((uint64_t)(uintptr_t)o, (uint64_t)(uintptr_t)g);
      for (int i = 0; i < 16; i++) k = mixf(k, o->matrix_world.e[i]);
      k = mix64(k, g->version);
      for (int i = 0; i < m->material_count; i++)
        if (m->materials[i]) k = mix64(k, m->materials[i]->version);
      t3_vec3 cc = t3_vec3_apply_mat4(g->bounding_sphere.center, &o->matrix_world);
      c.x = cc.x, c.y = cc.y, c.z = cc.z;
      c.r = g->bounding_sphere.radius * t3_mat4_max_scale_on_axis(&o->matrix_world);
      if (o->type == T3_INSTANCED_MESH) {
        const t3_instanced_mesh *im = (const t3_instanced_mesh *)o;
        k = mix64(mix64(k, (uint64_t)im->count), im->instance_matrix ? im->instance_matrix->version : 0);
        c.r = -1; /* instances spread anywhere */
      }
      if (o->type == T3_SKINNED_MESH) {
        const t3_skinned_mesh *sm = (const t3_skinned_mesh *)o;
        if (sm->skeleton)
          for (int b = 0; b < sm->skeleton->bone_count; b++)
            for (int i = 0; i < 16; i++) k = mixf(k, sm->skeleton->bones[b]->matrix_world.e[i]);
        c.r = -1; /* a pose can leave the bind-pose bounds */
      }
      for (int i = 0; i < m->morph_influence_count; i++) k = mixf(k, m->morph_influences[i]);
      c.key = k;
      cw_array_push(&a->casters, c);
    }
  }
  for (int i = 0; i < o->child_count; i++) gather_casters(a, o->children[i]);
}

/* the casters a sphere reaches, folded (what makes its shadow change) */
static uint64_t casters_in(const cw__atlas *a, t3_vec3 c, float r) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < a->casters.count; i++) {
    const caster *k = &a->casters.data[i];
    if (k->r >= 0) {
      float dx = k->x - c.x, dy = k->y - c.y, dz = k->z - c.z, rr = k->r + r;
      if (dx * dx + dy * dy + dz * dz > rr * rr) continue;
    }
    h = mix64(h, k->key);
  }
  return h;
}

/* ── layout ─────────────────────────────────────────────────────────── */

typedef struct want { int req, size; float priority; } want;

static int want_cmp(const void *pa, const void *pb) {
  const want *x = pa, *y = pb;
  if (x->size != y->size) return x->size > y->size ? -1 : 1;
  if (x->priority != y->priority) return x->priority > y->priority ? -1 : 1;
  return x->req < y->req ? -1 : x->req > y->req;
}

/* shelf packing, tallest first: false when a tile does not fit */
static bool pack(int atlas, const cw__shadow_req *req, want *ws, int n, int *xy) {
  qsort(ws, (size_t)n, sizeof *ws, want_cmp);
  int sx = 0, sy = 0, shelf = 0;
  for (int i = 0; i < n; i++) {
    bool omni = req[ws[i].req].omni;
    int w = omni ? ws[i].size * 3 : ws[i].size, h = omni ? ws[i].size * 2 : ws[i].size;
    if (sx + w > atlas) sy += shelf, sx = 0, shelf = 0;
    if (sy + h > atlas || w > atlas) return false;
    xy[2 * i] = sx, xy[2 * i + 1] = sy;
    sx += w;
    shelf = CW_MAX(shelf, h);
  }
  return true;
}

static tile *find_tile(cw__atlas *a, const t3_light *l) {
  for (int i = 0; i < a->tiles.count; i++)
    if (a->tiles.data[i].light == l) return &a->tiles.data[i];
  return NULL;
}

static void layout(cw__atlas *a, const cw__shadow_req *req, int n) {
  /* each light's size, halved for everyone smallest-priority first until it fits */
  want *ws = cw_frame_alloc((size_t)CW_MAX(n, 1) * sizeof *ws);
  int *xy = cw_frame_alloc((size_t)CW_MAX(n, 1) * 2 * sizeof(int));
  for (int i = 0; i < n; i++) ws[i] = (want){ i, req[i].size, req[i].priority };
  int placed = n;
  while (placed > 0) {
    want *tmp = cw_frame_alloc((size_t)placed * sizeof *tmp);
    memcpy(tmp, ws, (size_t)placed * sizeof *tmp);
    if (pack(a->size, req, tmp, placed, xy)) {
      memcpy(ws, tmp, (size_t)placed * sizeof *tmp);
      break;
    }
    /* shrink the least important tile that can still shrink; else drop the least important light */
    int pick = -1;
    for (int i = 0; i < placed; i++)
      if (ws[i].size > 64 && (pick < 0 || ws[i].priority < ws[pick].priority)) pick = i;
    if (pick >= 0) ws[pick].size /= 2;
    else {
      int least = 0;
      for (int i = 1; i < placed; i++)
        if (ws[i].priority < ws[least].priority) least = i;
      ws[least] = ws[--placed];
      if (!a->warned_full) {
        a->warned_full = true;
        cw_warn("lighting: the shadow atlas (%d) is full: some lights render without shadows. Raise "
                "LightingSettings.shadow_atlas_size, or shadow fewer lights",
                a->size);
      }
    }
  }
  /* keep what the old tiles hold where a light kept its place and size */
  CW_ARRAY(tile) old = { 0 };
  old.data = a->tiles.data, old.count = a->tiles.count, old.cap = a->tiles.cap;
  memset(&a->tiles, 0, sizeof a->tiles);
  for (int i = 0; i < placed; i++) {
    const cw__shadow_req *r = &req[ws[i].req];
    tile t;
    memset(&t, 0, sizeof t);
    t.light = r->light;
    t.omni = r->omni;
    t.size = ws[i].size;
    t.x = xy[2 * i], t.y = xy[2 * i + 1];
    t.w = t.omni ? t.size * 3 : t.size, t.h = t.omni ? t.size * 2 : t.size;
    for (int k = 0; k < old.count; k++)
      if (old.data[k].light == t.light && old.data[k].x == t.x && old.data[k].y == t.y && old.data[k].size == t.size &&
          old.data[k].omni == t.omni)
        t = old.data[k];
    cw_array_push(&a->tiles, t);
  }
  cw_array_free(&old);
}

/* ── drawing ────────────────────────────────────────────────────────── */

#if CW_WGPU
/* the depth of a rect into the R32Float copy: each texel recovered exactly by
 * a binary search over the bit patterns of [0, 1] floats with nearest
 * LessEqual compares (the only way compatibility mode reads depth; the
 * engine's depth copy does the same, src/render/pipeline_wgpu.c) */
static const char COPY_WGSL[] =
    "@group(0) @binding(0) var t_depth: texture_depth_2d;\n"
    "@group(0) @binding(1) var s_cmp: sampler_comparison;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;\n"
    "  return vec4f(p, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "  let tc = pos.xy / vec2f(textureDimensions(t_depth));\n"
    "  var lo = 0u;\n"
    "  var hi = 0x3f800000u;\n"
    "  for (var i = 0; i < 31; i++) {\n"
    "    if (lo >= hi) { break; }\n"
    "    let mid = lo + (hi - lo + 1u) / 2u;\n"
    "    if (textureSampleCompareLevel(t_depth, s_cmp, tc, bitcast<f32>(mid)) > 0.5) { lo = mid; } else { hi = mid - 1u; }\n"
    "  }\n"
    "  return vec4f(bitcast<f32>(lo), 0.0, 0.0, 1.0);\n"
    "}\n";

static void copy_rect_wgpu(cw__atlas *a, int x, int y, int s) {
  static WGPURenderPipeline pipe;
  static WGPUBindGroupLayout bgl;
  static WGPUSampler cmp;
  WGPUDevice dev = cw__wgpu_device();
  if (!pipe) {
    WGPUShaderModule sm = cw__wgpu_shader("shadow.copy", COPY_WGSL);
    if (!sm) return;
    WGPUBindGroupLayoutEntry e[2] = {
      { .binding = 0, .visibility = WGPUShaderStage_Fragment,
        .texture = { .sampleType = WGPUTextureSampleType_Depth, .viewDimension = WGPUTextureViewDimension_2D } },
      { .binding = 1, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Comparison } },
    };
    bgl = wgpuDeviceCreateBindGroupLayout(dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 2, .entries = e });
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 1, .bindGroupLayouts = &bgl });
    WGPUColorTargetState ct = { .format = WGPUTextureFormat_R32Float, .writeMask = WGPUColorWriteMask_All };
    WGPUFragmentState fs = { .module = sm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
    WGPURenderPipelineDescriptor d = { .label = CW_WGPU_SV("shadow.copy"), .layout = pl,
                                       .vertex = { .module = sm, .entryPoint = CW_WGPU_SV("vs") },
                                       .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
                                       .multisample = { .count = 1, .mask = 0xffffffffu }, .fragment = &fs };
    pipe = wgpuDeviceCreateRenderPipeline(dev, &d);
    wgpuPipelineLayoutRelease(pl);
    wgpuShaderModuleRelease(sm);
    WGPUSamplerDescriptor sd = { .addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge,
                                 .addressModeW = WGPUAddressMode_ClampToEdge, .magFilter = WGPUFilterMode_Nearest,
                                 .minFilter = WGPUFilterMode_Nearest, .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                                 .lodMinClamp = 0, .lodMaxClamp = 32, .compare = WGPUCompareFunction_LessEqual, .maxAnisotropy = 1 };
    cmp = wgpuDeviceCreateSampler(dev, &sd);
    if (!pipe) return;
  }
  if (!a->wg_copy_bg) {
    WGPUBindGroupEntry be[2] = { { .binding = 0, .textureView = a->wg_view }, { .binding = 1, .sampler = cmp } };
    a->wg_copy_bg = wgpuDeviceCreateBindGroup(dev, &(WGPUBindGroupDescriptor){ .layout = bgl, .entryCount = 2, .entries = be });
  }
  WGPURenderPassColorAttachment ca = { .view = a->wg_r32_view, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                                       .loadOp = WGPULoadOp_Load, .storeOp = WGPUStoreOp_Store };
  WGPURenderPassDescriptor rp = { .label = CW_WGPU_SV("shadow.copy"), .colorAttachmentCount = 1, .colorAttachments = &ca };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(cw__pw_encoder(), &rp);
  wgpuRenderPassEncoderSetViewport(p, (float)x, (float)y, (float)s, (float)s, 0, 1);
  wgpuRenderPassEncoderSetScissorRect(p, (uint32_t)x, (uint32_t)y, (uint32_t)s, (uint32_t)s);
  wgpuRenderPassEncoderSetPipeline(p, pipe);
  wgpuRenderPassEncoderSetBindGroup(p, 0, a->wg_copy_bg, 0, NULL);
  wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
}
#endif

void cw__render_shadow_depth(t3_renderer *r, t3_scene *scene, t3_camera *cam, const t3_external_target *et) {
  bool rev = cw_gpu_reversed_depth();
  if (rev) t3_renderer_set_reversed_depth(r, false);
  t3_renderer_render_depth_to(r, scene, cam, et);
  if (rev) t3_renderer_set_reversed_depth(r, true);
}

static void draw_rect(cw__atlas *a, cw_pass_ctx *ctx, t3_camera *cam, int x, int y, int s) {
  bool webgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  t3_external_target et = { 0 };
  et.samples = 1;
  et.clear_depth = 1;
  et.w = et.h = s;
  et.x = x;
  /* top-down rows in the texture (WebGPU's own; three.c flips a GPU-made
   * texture's rows on GL, where rects count from the bottom) */
  et.y = webgpu ? y : a->size - y - s;
#if CW_WGPU
  if (webgpu) {
    et.depth = (uintptr_t)a->wg_view;
    et.depth_format = WGPUTextureFormat_Depth32Float;
    et.load_depth = true;
    cw__shadow_clear_rect_wgpu(a->wg_view, et.x, et.y, s, s);
    t3_renderer_set_external_encoder(ctx->renderer, (uintptr_t)cw__pw_encoder());
  }
#endif
  if (!webgpu) et.color = a->gl_fbo;
  cw__render_shadow_depth(ctx->renderer, ctx->scene3d, cam, &et);
#if CW_WGPU
  if (webgpu) copy_rect_wgpu(a, et.x, et.y, s);
#endif
}

/* a camera at p looking along dir with up, perspective fov (degrees) */
static void aim(t3_camera *c, t3_vec3 p, t3_vec3 dir, t3_vec3 up, float fov, float near, float far) {
  c->fov = fov, c->aspect = 1, c->near = near, c->far = far, c->zoom = 1;
  t3_camera_update_projection_matrix(c);
  c->base.position = p;
  c->base.up = up;
  t3_object_look_at(c, p.x + dir.x, p.y + dir.y, p.z + dir.z);
  t3_object_update_matrix_world(c, false);
}

void cw__atlas_update(cw__atlas *a, cw_pass_ctx *ctx, const cw__shadow_req *req, int n, int atlas_size, int budget,
                      bool cache, cw__shadow_out *out) {
  CW_PROFILE_SCOPE("lighting.atlas");
  memset(&a->info, 0, sizeof a->info);
  for (int i = 0; i < n; i++) out[i].type = 0;
  /* (with no shadowed light a small atlas still binds: the shader reads none of it) */
  atlas_size = n ? CW_CLAMP(atlas_size, 512, 16384) : (a->size ? a->size : 512);
  if (a->size != atlas_size && !make_tex(a, atlas_size)) {
    cw_error("lighting: a %dx%d depth texture for the shadow atlas cannot be made here", atlas_size, atlas_size);
    free_tex(a);
    return;
  }
  if (!n) return;
  cw_array_clear(&a->casters);
  gather_casters(a, &ctx->scene3d->base);
  /* the layout follows the set of lights and their sizes */
  uint64_t lk = mix64(1469598103934665603ull, (uint64_t)a->size);
  for (int i = 0; i < n; i++) lk = mix64(mix64(mix64(lk, (uint64_t)(uintptr_t)req[i].light), (uint64_t)req[i].size), req[i].omni);
  if (lk != a->layout_key) {
    layout(a, req, n);
    a->layout_key = lk;
  }
  /* the order tiles are brought up to date: the most important first */
  int *order = cw_frame_alloc((size_t)n * sizeof *order);
  for (int i = 0; i < n; i++) order[i] = i;
  for (int i = 1; i < n; i++) {
    int v = order[i], j = i - 1;
    while (j >= 0 && req[order[j]].priority < req[v].priority) order[j + 1] = order[j], j--;
    order[j + 1] = v;
  }
  if (budget <= 0) budget = 1 << 30;
  float inv = 1.0f / (float)a->size;
  for (int oi = 0; oi < n; oi++) {
    int i = order[oi];
    const cw__shadow_req *r = &req[i];
    const t3_light *l = r->light;
    tile *t = find_tile(a, l);
    if (!t) continue;
    a->info.shadowed++;
    t3_vec3 pos = t3_mat4_get_position(&l->base.matrix_world);
    float near = l->shadow && l->shadow->camera ? CW_MAX(l->shadow->camera->near, 0.01f) : 0.5f;
    float far = r->range;
    float fov = 90;
    t3_vec3 dir = { 0, 0, -1 };
    if (!t->omni) {
      t3_vec3 tp = l->target ? t3_mat4_get_position(&l->target->matrix_world) : t3_vec3_add(pos, (t3_vec3){ 0, -1, 0 });
      dir = t3_vec3_normalize(t3_vec3_sub(tp, pos));
      /* SpotLightShadow: fov = 2 x angle x focus */
      fov = CW_MIN(2.0f * l->angle * (l->shadow ? l->shadow->focus : 1) * 57.2957795f, 179.0f);
    }
    uint64_t key = mix64(mix64(1469598103934665603ull, (uint64_t)t->size), t->omni);
    key = mixf(mixf(mixf(key, pos.x), pos.y), pos.z);
    key = mixf(mixf(mixf(key, dir.x), dir.y), dir.z);
    key = mixf(mixf(mixf(key, far), near), fov);
    if (key != t->light_key) {
      memset(t->valid, 0, sizeof t->valid);
      t->light_key = key;
    }
    uint64_t ck = casters_in(a, pos, far);
    int faces = t->omni ? 6 : 1, todo = 0;
    for (int f = 0; f < faces; f++)
      if (!t->valid[f] || !cache || t->face_key[f] != ck) todo++;
    if (todo && todo <= budget) {
      budget -= todo;
      for (int f = 0; f < faces; f++) {
        if (t->valid[f] && cache && t->face_key[f] == ck) continue;
        if (t->omni) {
          aim(a->pcam, pos, (t3_vec3){ FACE_DIR[f][0], FACE_DIR[f][1], FACE_DIR[f][2] },
              (t3_vec3){ FACE_UP[f][0], FACE_UP[f][1], FACE_UP[f][2] }, 90, near, far);
          draw_rect(a, ctx, a->pcam, t->x + (f % 3) * t->size, t->y + (f / 3) * t->size, t->size);
        } else {
          t3_vec3 up = fabsf(dir.y) > 0.99f ? (t3_vec3){ 1, 0, 0 } : (t3_vec3){ 0, 1, 0 };
          aim(a->pcam, pos, dir, up, fov, near, far);
          draw_rect(a, ctx, a->pcam, t->x, t->y, t->size);
          /* LightShadow.matrix (bias x projection x view), its rect in the atlas
           * folded in (before r187's y flip: rows counted from the bottom) */
          t3_mat4 b = { { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0.5f, 0.5f, 0.5f, 1 } }, tm, m;
          float s = (float)t->size * inv;
          t3_mat4 q = { { s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1, 0, (float)t->x * inv, 1 - (float)(t->y + t->size) * inv, 0, 1 } };
          t3_mat4_multiply(&tm, &b, &a->pcam->projection_matrix);
          t3_mat4_multiply(&m, &tm, &a->pcam->matrix_world_inverse);
          t3_mat4_multiply(&tm, &q, &m);
          memcpy(t->mat, tm.e, sizeof t->mat);
        }
        t->face_key[f] = ck;
        t->valid[f] = ctx->frame > 0; /* (not from the first frame: see lighting_csm.c) */
        a->info.faces_rendered++;
      }
      t->pos = pos, t->near = near, t->far = far;
    } else
      a->info.faces_cached += faces;
    bool all = true;
    for (int f = 0; f < faces; f++) all = all && t->valid[f];
    if (!all) continue;
    cw__shadow_out *o = &out[i];
    o->type = t->omni ? 2 : 1;
    memcpy(o->mat, t->mat, sizeof o->mat);
    o->pos = t->pos;
    o->near = t->near;
    o->far = t->far;
    o->rect[0] = (float)t->x, o->rect[1] = (float)t->y, o->rect[2] = (float)t->size, o->rect[3] = 0;
  }
  if (cw_gpu_backend() != CW_GPU_WEBGPU) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    t3_renderer_reset_bindings(ctx->renderer);
  }
}

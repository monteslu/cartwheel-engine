/* Octahedral impostors: a mesh seen from 64 directions (an 8x8 grid over the
 * full sphere in octahedral layout), rendered once by the tree's own three.c
 * renderer into two atlases, albedo (with coverage) and normals, then drawn
 * as camera-facing quads that blend the four nearest views (each re-projected
 * into its own view plane) and are lit at draw time by the tree's
 * directional, ambient and hemisphere lights. Lighting at draw time is what
 * lets every rotated instance of a mesh share one atlas. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/render3d.h"
#include "vis_internal.h"
#if CW_WGPU
#include "fx/fx_wgpu.h"
#endif

#define IMP_G 8
#define IMP_CELL 128
#define IMP_SIZE (IMP_G * IMP_CELL)
#define IMP_MAX_DIR 4
#define IMP_MIPS 5 /* 1024 down to 64 texels */

typedef struct imp_atlas {
  uint64_t key;
  GLuint albedo, normal, fbo, depth_rb;
  float radius, center[3]; /* the bounding sphere in bake space */
  bool ok;
  CW_ARRAY(vis_impostor_inst) inst;
  GLuint vbo;
  int vbo_cap;
#if CW_WGPU
  WGPUTextureView walbedo, wnormal;
  WGPUBuffer wvbo; /* opaque instances, then fading ones, at wvbo_split */
  int wvbo_cap;
#endif
} imp_atlas;

struct vis_impostors {
  CW_ARRAY(imp_atlas *) atlases;
};

static GLuint prog, quad_vbo, vao;
static bool prog_failed;

static const char *const vs =
    "#version 300 es\n"
    "precision highp float;\n"
    "layout(location = 0) in vec2 a_corner;\n"
    "layout(location = 1) in vec4 a_cr;\n"   /* centre, radius */
    "layout(location = 2) in vec4 a_quat;\n" /* object rotation */
    "layout(location = 3) in float a_alpha;\n"
    "uniform mat4 u_view, u_proj;\n"
    "uniform vec3 u_campos;\n"
    "out vec3 v_off;\n"   /* bake-space offset from the centre, on the plane through it */
    "out vec3 v_dir;\n"   /* bake-space direction from the centre to the camera */
    "flat out vec4 v_quat;\n"
    "out float v_alpha;\n"
    "out float v_r;\n"
    "out float v_depth;\n"
    "vec3 qrot_inv(vec4 q, vec3 v) { vec3 u = -q.xyz; vec3 t = 2.0 * cross(u, v); return v + q.w * t + cross(u, t); }\n"
    "void main() {\n"
    "  vec3 c = a_cr.xyz; float r = a_cr.w;\n"
    "  vec3 tc = u_campos - c; float dist = length(tc); vec3 dir = tc / max(dist, 1e-6);\n"
    /* a quad facing the camera position (not its view plane): it covers the
     * sphere's silhouette cone wherever on screen the object is; pulled
     * toward the camera by the radius so its own mesh never hides it */
    "  vec3 upr = abs(dir.y) > 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);\n"
    "  vec3 right = normalize(cross(upr, dir)); vec3 up = cross(dir, right);\n"
    "  float pull = min(r, dist * 0.5);\n"
    "  vec3 cq = c + dir * pull;\n"
    "  float half_size = (dist - pull) * r / sqrt(max(dist * dist - r * r, 1e-6)) * 1.03;\n"
    "  vec3 pq = cq + (right * a_corner.x + up * a_corner.y) * half_size;\n"
    "  vec3 p = u_campos + (pq - u_campos) * (dist / max(dist - pull, 1e-4));\n"
    "  v_off = qrot_inv(a_quat, p - c);\n"
    "  v_dir = qrot_inv(a_quat, dir);\n"
    "  v_quat = a_quat;\n"
    "  v_alpha = a_alpha; v_r = r;\n"
    "  v_depth = -(u_view * vec4(c, 1.0)).z;\n"
    "  gl_Position = u_proj * (u_view * vec4(pq, 1.0));\n"
    "}\n";

static const char *const fs =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp sampler2D;\n"
    "in vec3 v_off;\n"
    "in vec3 v_dir;\n"
    "flat in vec4 v_quat;\n"
    "in float v_alpha;\n"
    "in float v_r;\n"
    "in float v_depth;\n"
    "uniform sampler2D u_albedo, u_normal;\n"
    "uniform vec3 u_bake_center;\n"
    "uniform int u_ndir;\n"
    "uniform vec3 u_dir_l[4];\n" /* toward the light, world */
    "uniform vec3 u_dir_c[4];\n" /* colour x intensity */
    "uniform vec3 u_ambient;\n"
    "uniform vec3 u_hemi_sky, u_hemi_ground, u_hemi_up;\n"
    "uniform float u_light_scale;\n" /* 1 / PI: physical light units (three.js r186's) */
    "uniform int u_fog;\n"
    "uniform vec4 u_fog_par;\n" /* near, far, density */
    "uniform vec3 u_fog_color;\n"
    "out vec4 frag;\n"
    "const float G = 8.0;\n"
    "vec3 qrot(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }\n"
    "vec2 oct_enc(vec3 n) {\n"
    "  n /= abs(n.x) + abs(n.y) + abs(n.z);\n"
    "  vec2 p = n.xz;\n"
    "  if (n.y < 0.0) p = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);\n"
    "  return p * 0.5 + 0.5;\n"
    "}\n"
    "vec3 oct_dec(vec2 uv) {\n"
    "  vec2 p = uv * 2.0 - 1.0;\n"
    "  vec3 n = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);\n"
    "  if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);\n"
    "  return normalize(n);\n"
    "}\n"
    /* one view: albedo (rgb, coverage) and the bake-space normal (xyz, used) */
    "void cell(vec2 ij, vec3 off, float w, inout vec4 alb, inout vec3 nrm) {\n"
    "  vec3 d = oct_dec((ij + 0.5) / G);\n"
    "  vec3 upr = abs(d.y) > 0.999 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);\n"
    "  vec3 x = normalize(cross(upr, d)); vec3 y = cross(d, x);\n"
    "  vec3 o = off - u_bake_center;\n"
    "  vec2 uv = vec2(dot(o, x), dot(o, y)) / (2.0 * v_r) + 0.5;\n"
    "  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return;\n"
    "  uv = clamp(uv, 0.5 / 128.0, 1.0 - 0.5 / 128.0);\n"
    "  vec2 t = (ij + uv) / G;\n"
    "  vec4 a = texture(u_albedo, t);\n"
    "  vec3 nv = texture(u_normal, t).xyz * 2.0 - 1.0;\n"
    "  alb += a * w;\n"
    "  nrm += (x * nv.x + y * nv.y + d * nv.z) * (a.a * w);\n"
    "}\n"
    "void main() {\n"
    "  vec2 g = oct_enc(normalize(v_dir)) * G - 0.5;\n"
    "  vec2 c0 = floor(g); vec2 f = g - c0;\n"
    "  vec2 a = clamp(c0, 0.0, G - 1.0), b = clamp(c0 + 1.0, 0.0, G - 1.0);\n"
    "  vec3 off = v_off + u_bake_center;\n"
    "  vec4 alb = vec4(0.0); vec3 nrm = vec3(0.0);\n"
    "  cell(a, off, (1.0 - f.x) * (1.0 - f.y), alb, nrm);\n"
    "  cell(vec2(b.x, a.y), off, f.x * (1.0 - f.y), alb, nrm);\n"
    "  cell(vec2(a.x, b.y), off, (1.0 - f.x) * f.y, alb, nrm);\n"
    "  cell(b, off, f.x * f.y, alb, nrm);\n"
    "  if (alb.a < 0.5) discard;\n"
    "  vec3 albedo = alb.rgb / alb.a;\n"
    "  vec3 n = qrot(v_quat, normalize(nrm + vec3(0.0, 1e-5, 0.0)));\n"
    "  vec3 irr = u_ambient + mix(u_hemi_ground, u_hemi_sky, dot(n, u_hemi_up) * 0.5 + 0.5);\n"
    "  for (int i = 0; i < 4; i++) { if (i >= u_ndir) break; irr += u_dir_c[i] * max(dot(n, u_dir_l[i]), 0.0); }\n"
    "  vec3 rgb = albedo * irr * u_light_scale;\n"
    "  if (u_fog == 1) rgb = mix(rgb, u_fog_color, smoothstep(u_fog_par.x, u_fog_par.y, v_depth));\n"
    "  else if (u_fog == 2) rgb = mix(rgb, u_fog_color, 1.0 - exp(-u_fog_par.z * u_fog_par.z * v_depth * v_depth));\n"
    "  frag = vec4(rgb * v_alpha, v_alpha);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei n = 0;
    glGetShaderInfoLog(s, sizeof log, &n, log);
    cw_error("impostor shader failed to compile: %.*s", (int)n, log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static bool gl_init(void) {
  if (prog) return true;
  if (prog_failed) return false;
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) {
    prog_failed = true;
    return false;
  }
  prog = glCreateProgram();
  glAttachShader(prog, v);
  glAttachShader(prog, f);
  glLinkProgram(prog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    cw_error("impostor program failed to link");
    prog = 0;
    prog_failed = true;
    return false;
  }
  static const float q[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
  glGenBuffers(1, &quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glGenVertexArrays(1, &vao);
  return true;
}

vis_impostors *cw__vis_impostors_new(void) { return cw_calloc(1, sizeof(vis_impostors), CW_MEM_RENDER); }

void cw__vis_impostors_free(vis_impostors *im) {
  if (!im) return;
  for (int i = 0; i < im->atlases.count; i++) {
    imp_atlas *a = im->atlases.data[i];
    if (a->albedo) glDeleteTextures(1, &a->albedo);
    if (a->normal) glDeleteTextures(1, &a->normal);
    if (a->fbo) glDeleteFramebuffers(1, &a->fbo);
    if (a->depth_rb) glDeleteRenderbuffers(1, &a->depth_rb);
    if (a->vbo) glDeleteBuffers(1, &a->vbo);
#if CW_WGPU
    if (a->walbedo) wgpuTextureViewRelease(a->walbedo);
    if (a->wnormal) wgpuTextureViewRelease(a->wnormal);
    if (a->wvbo) wgpuBufferRelease(a->wvbo);
#endif
    cw_array_free(&a->inst);
    cw_free(a);
  }
  cw_array_free(&im->atlases);
  cw_free(im);
}

int cw__vis_impostor_atlas_count(const vis_impostors *im) { return im ? im->atlases.count : 0; }

void cw__vis_impostors_begin_frame(vis_impostors *im) {
  for (int i = 0; i < im->atlases.count; i++) cw_array_clear(&im->atlases.data[i]->inst);
}

void cw__vis_impostor_add(vis_impostors *im, int atlas, const vis_impostor_inst *inst) {
  if (atlas < 0 || atlas >= im->atlases.count) return;
  cw_array_push(&im->atlases.data[atlas]->inst, *inst);
}

/* ── baking ─────────────────────────────────────────────────────────── */

/* CW_TEST_IMPOSTOR_BAKE_MS=1: each bake waits for the GPU before and after
 * (cw_gpu_finish; on WebGPU the frame's encoder is submitted first, so the
 * bake goes to the GPU at once) and logs its time, so bake cost can be
 * measured on either backend. A WebGPU cart cannot wait: there the time is the
 * CPU side only. */
static bool bake_timing(void) {
  const char *e = getenv("CW_TEST_IMPOSTOR_BAKE_MS");
  return e && *e == '1';
}
static bool bake_finish(t3_renderer *r) {
#if CW_WGPU
  if (cw__wgpu_on()) {
    cw__pw_flush();
    t3_renderer_wgpu_submit(r);
  }
#endif
  (void)r;
  return cw_gpu_finish();
}
static double bake_clock_start(t3_renderer *r) {
  bake_finish(r);
  double t = cw__render3d_now_ms();
  return t > 0 ? t : 1e-9;
}
static void bake_clock_report(t3_renderer *r, double t0, const char *api) {
  bool waited = bake_finish(r);
  cw_info("impostor bake (%s): %.2f ms%s", api, cw__render3d_now_ms() - t0, waited ? "" : " (CPU only: this host cannot wait for the GPU)");
}

typedef struct mesh_list { CW_ARRAY(t3_mesh *) m; } mesh_list;
static void collect_meshes(t3_object *o, void *u) {
  mesh_list *l = u;
  if ((o->type == T3_MESH || o->type == T3_SKINNED_MESH) && o->visible) cw_array_push(&l->m, (t3_mesh *)o);
}

static uint64_t mixk(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}

static void oct_dec(float u, float v, float out[3]) {
  float px = u * 2 - 1, py = v * 2 - 1;
  float nx = px, ny = 1 - fabsf(px) - fabsf(py), nz = py;
  if (ny < 0) {
    float ox = (1 - fabsf(nz)) * (nx >= 0 ? 1.0f : -1.0f), oz = (1 - fabsf(nx)) * (nz >= 0 ? 1.0f : -1.0f);
    nx = ox;
    nz = oz;
  }
  float l = sqrtf(nx * nx + ny * ny + nz * nz);
  out[0] = nx / l;
  out[1] = ny / l;
  out[2] = nz / l;
}

static GLuint atlas_tex(void) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, IMP_SIZE, IMP_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, IMP_MIPS - 1);
  return t;
}

/* where a bake draws: GL, the atlas framebuffer with tex attached; WebGPU,
 * mip 0 of the atlas texture and a depth view of the atlas's size */
typedef struct bake_dst {
  GLuint fbo, tex;
#if CW_WGPU
  WGPUTextureView color, depth;
#endif
} bake_dst;

/* the first texel row (counted from row 0, where texture coordinate 0 is) of
 * view row j: the draw shaders read view (i, j) at (ij + uv) / 8. Both
 * backends place every view through this one function, so the atlases hold the
 * same texels in the same rows (the WebGPU bake draws with flip_y: GL's
 * bottom-up rows). three.js's setViewport counts y from the top, so the GL
 * bake asks for IMP_SIZE - row - IMP_CELL; it once asked for row itself, which
 * put view row j where row 7 - j is read: every impostor showed each view
 * mirrored front to back (z), its far side lit as its near side. */
static int tile_row(int j) { return j * IMP_CELL; }

/* render the 64 views of scene sc into the bake destination */
static void render_views(t3_renderer *r, t3_scene *sc, imp_atlas *a, const bake_dst *d) {
  bool wg = false;
#if CW_WGPU
  wg = d->color != NULL;
#endif
  if (!wg) {
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->tex, 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    t3_renderer_reset_bindings(r);
    t3_renderer_set_framebuffer(r, d->fbo);
    t3_renderer_set_size(r, IMP_SIZE, IMP_SIZE);
    t3_renderer_set_clear_color(r, 0x000000, 0);
    t3_renderer_set_scissor_test(r, true);
  }
  float R = a->radius;
  t3_camera *cam = t3_orthographic_camera_new(-R, R, R, -R, 0.01f, 4 * R);
  t3_vec3 c = { a->center[0], a->center[1], a->center[2] };
  for (int j = 0; j < IMP_G; j++)
    for (int i = 0; i < IMP_G; i++) {
      float dir[3];
      oct_dec((i + 0.5f) / IMP_G, (j + 0.5f) / IMP_G, dir);
      cam->base.up = fabsf(dir[1]) > 0.999f ? (t3_vec3){ 0, 0, -1 } : (t3_vec3){ 0, 1, 0 };
      cam->base.position = (t3_vec3){ c.x + dir[0] * 2 * R, c.y + dir[1] * 2 * R, c.z + dir[2] * 2 * R };
      t3_object_look_at(cam, c.x, c.y, c.z);
      t3_camera_update_projection_matrix(cam);
#if CW_WGPU
      if (wg) {
        /* the first view clears the whole atlas (a WebGPU pass clears its
         * whole attachment), the rest load it; flip_y: GL's row order, so
         * y counts from the bottom row */
        bool first = i == 0 && j == 0;
        t3_external_target et = {
          .color = (uintptr_t)d->color, .depth = (uintptr_t)d->depth,
          .color_format = (int)WGPUTextureFormat_RGBA8Unorm, .depth_format = (int)WGPUTextureFormat_Depth24Plus,
          .samples = 1, .load_color = !first, .load_depth = !first, .clear_rgba = { 0, 0, 0, 0 }, .clear_depth = 1,
          .x = i * IMP_CELL, .y = tile_row(j), .w = IMP_CELL, .h = IMP_CELL, .flip_y = true,
        };
        t3_renderer_set_external_target(r, &et);
        t3_renderer_render(r, sc, cam);
        continue;
      }
#endif
      int top = IMP_SIZE - tile_row(j) - IMP_CELL;
      t3_renderer_set_viewport(r, i * IMP_CELL, top, IMP_CELL, IMP_CELL);
      t3_renderer_set_scissor(r, i * IMP_CELL, top, IMP_CELL, IMP_CELL);
      t3_renderer_render(r, sc, cam);
    }
  t3_release(cam);
#if CW_WGPU
  if (wg) {
    t3_renderer_set_external_target(r, NULL);
    return;
  }
#endif
  t3_renderer_set_scissor_test(r, false);
  glBindTexture(GL_TEXTURE_2D, d->tex);
  glGenerateMipmap(GL_TEXTURE_2D);
}

#if CW_WGPU
/* ── baking on WebGPU: three.c draws the views into mip 0, mips by 2x2 box
 * filtering on the GPU (glGenerateMipmap's filter) ─────────────────────── */

static const char WGSL_MIP[] =
    "@group(0) @binding(0) var src: texture_2d<f32>;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  return vec4f(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "  let q = vec2i(pos.xy) * 2;\n"
    "  return (textureLoad(src, q, 0) + textureLoad(src, q + vec2i(1, 0), 0) + textureLoad(src, q + vec2i(0, 1), 0) +\n"
    "          textureLoad(src, q + vec2i(1, 1), 0)) * 0.25;\n"
    "}\n";

static struct {
  WGPUShaderModule sm;
  WGPUBindGroupLayout bgl;
  WGPURenderPipeline p;
} M;

static bool mip_init(void) {
  if (M.p) return true;
  const char *const parts[] = { WGSL_MIP, NULL };
  M.sm = cw__wg_module("impostor mips", parts);
  if (!M.sm) return false;
  const cw__wg_slot sl[1] = { { CW_WB_TEX, WGPUShaderStage_Fragment, 0, 0 } };
  M.bgl = cw__wg_layout(sl, 1);
  WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA8Unorm, .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = M.sm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
  WGPURenderPipelineDescriptor d = {
    .label = CW_WGPU_SV("impostor mips"), .layout = cw__wg_pipeline_layout(&M.bgl, 1),
    .vertex = { .module = M.sm, .entryPoint = CW_WGPU_SV("vs") },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
    .multisample = { .count = 1, .mask = 0xffffffffu },
    .fragment = &fs,
  };
  M.p = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
  wgpuPipelineLayoutRelease(d.layout);
  return M.p != NULL;
}

/* levels 1 .. IMP_MIPS-1 of t from level 0, recorded on enc. Each level is
 * drawn into a texture of its own and copied in, so no pass samples the
 * texture it draws into (compatibility mode allows no feedback loop) */
static void wg_mips(WGPUCommandEncoder enc, WGPUTexture t) {
  if (!mip_init()) return;
  WGPUTexture tmp[IMP_MIPS] = { 0 };
  WGPUTextureView views[IMP_MIPS] = { 0 };
  WGPUBindGroup groups[IMP_MIPS] = { 0 };
  views[0] = cw__wg_view(t, WGPUTextureViewDimension_2D, 0, IMP_MIPS, 0, 1); /* textureLoad level 0 */
  for (int k = 1; k < IMP_MIPS; k++) {
    uint32_t s = (uint32_t)(IMP_SIZE >> k);
    tmp[k] = cw__wg_texture(WGPUTextureDimension_2D, (int)s, (int)s, 1, 1, WGPUTextureFormat_RGBA8Unorm,
                            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
                            WGPUTextureViewDimension_Undefined, "impostor mip");
    views[k] = cw__wg_view(tmp[k], WGPUTextureViewDimension_2D, 0, 1, 0, 1);
    cw__wg_entry e = { .view = views[k - 1] };
    groups[k] = cw__wg_group(M.bgl, &e, 1);
    WGPURenderPassColorAttachment ca = { .view = views[k], .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                                         .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store };
    WGPURenderPassDescriptor rp = { .label = CW_WGPU_SV("impostor mip"), .colorAttachmentCount = 1, .colorAttachments = &ca };
    WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(p, M.p);
    wgpuRenderPassEncoderSetBindGroup(p, 0, groups[k], 0, NULL);
    wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(p);
    wgpuRenderPassEncoderRelease(p);
    WGPUTexelCopyTextureInfo src = { .texture = tmp[k], .aspect = WGPUTextureAspect_All };
    WGPUTexelCopyTextureInfo dst = { .texture = t, .mipLevel = (uint32_t)k, .aspect = WGPUTextureAspect_All };
    WGPUExtent3D ext = { s, s, 1 };
    wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &ext);
  }
  for (int k = 0; k < IMP_MIPS; k++) {
    if (groups[k]) wgpuBindGroupRelease(groups[k]);
    if (views[k]) wgpuTextureViewRelease(views[k]);
    if (tmp[k]) wgpuTextureRelease(tmp[k]);
  }
}

/* the two atlases of a: three.c draws the views on the frame's encoder (as
 * the shadow passes do: one submit, before the passes that sample them),
 * linear and untoned as the GL bake (the 3D pipeline's renderer state) */
static bool wg_bake(t3_renderer *r, t3_scene *sa, t3_scene *sn, imp_atlas *a) {
  WGPUTexture tex[2];
  for (int k = 0; k < 2; k++)
    tex[k] = cw__wg_texture(WGPUTextureDimension_2D, IMP_SIZE, IMP_SIZE, 1, IMP_MIPS, WGPUTextureFormat_RGBA8Unorm,
                            WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_RenderAttachment,
                            WGPUTextureViewDimension_Undefined, k ? "impostor normal" : "impostor albedo");
  WGPUTexture depth = cw__wg_texture(WGPUTextureDimension_2D, IMP_SIZE, IMP_SIZE, 1, 1, WGPUTextureFormat_Depth24Plus,
                                     WGPUTextureUsage_RenderAttachment, WGPUTextureViewDimension_Undefined, "impostor bake depth");
  if (!tex[0] || !tex[1] || !depth) {
    cw_error("impostor atlas: the WebGPU bake textures could not be made");
    for (int k = 0; k < 2; k++)
      if (tex[k]) wgpuTextureRelease(tex[k]);
    if (depth) wgpuTextureRelease(depth);
    return false;
  }
  float exposure = 1;
  t3_tone_mapping tm = t3_renderer_get_tone_mapping(r, &exposure);
  t3_color_space cs = t3_renderer_get_output_color_space(r);
  t3_renderer_set_tone_mapping(r, T3_NO_TONE_MAPPING, 1);
  t3_renderer_set_output_color_space(r, T3_LINEAR_SRGB_COLOR_SPACE);
  WGPUCommandEncoder enc = cw__pw_encoder();
  t3_renderer_set_external_encoder(r, (uintptr_t)enc);
  bake_dst d = { 0 };
  d.depth = cw__wg_view(depth, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  for (int k = 0; k < 2; k++) {
    d.color = cw__wg_view(tex[k], WGPUTextureViewDimension_2D, 0, 1, 0, 1);
    render_views(r, k ? sn : sa, a, &d);
    wgpuTextureViewRelease(d.color);
    wg_mips(enc, tex[k]);
  }
  wgpuTextureViewRelease(d.depth);
  wgpuTextureRelease(depth);
  t3_renderer_set_tone_mapping(r, tm, exposure);
  t3_renderer_set_output_color_space(r, cs);
  a->walbedo = cw__wg_view(tex[0], WGPUTextureViewDimension_2D, 0, IMP_MIPS, 0, 1);
  a->wnormal = cw__wg_view(tex[1], WGPUTextureViewDimension_2D, 0, IMP_MIPS, 0, 1);
  wgpuTextureRelease(tex[0]);
  wgpuTextureRelease(tex[1]);
  return true;
}
#endif

/* CW_TEST_IMPOSTOR_WGPU_OFF=1: WebGPU runs bake nothing and draw the meshes
 * (the gates' control: impostors off on WebGPU only) */
static bool wgpu_bake_off(void) {
  const char *e = getenv("CW_TEST_IMPOSTOR_WGPU_OFF");
  return e && *e == '1';
}

int cw__vis_impostor_atlas(vis_impostors *im, cw_pass_ctx *ctx, cw_node src, const cw__mesh_info *info, float *radius,
                           float center_local[3]) {
  if (!info->obj) return -1;
  bool wg = false;
#if CW_WGPU
  wg = cw__wgpu_on();
#endif
  if (wg && wgpu_bake_off()) return -1;
  /* bake space: the node's world transform without its rotation and
   * translation (its scale and every sub-transform stay in) */
  t3_object *group = cw_node3d_object(src);
  t3_vec3 gp, gs;
  t3_quat gq;
  t3_mat4_decompose(&group->matrix_world, &gp, &gq, &gs);
  t3_mat4 unrot, tr, base;
  t3_mat4_compose(&unrot, (t3_vec3){ 0, 0, 0 }, (t3_quat){ -gq.x, -gq.y, -gq.z, gq.w }, (t3_vec3){ 1, 1, 1 });
  t3_mat4_identity(&tr);
  tr.e[12] = -gp.x;
  tr.e[13] = -gp.y;
  tr.e[14] = -gp.z;
  t3_mat4_multiply(&base, &unrot, &tr);
  t3_mat4 rel_obj;
  t3_mat4_multiply(&rel_obj, &base, &info->obj->matrix_world);
  uint64_t key = info->look_key;
  static const int lin[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
  for (int i = 0; i < 12; i++) key = mixk(key, (uint64_t)(int64_t)lrintf(rel_obj.e[lin[i]] * 4096.0f));
  for (int i = 0; i < im->atlases.count; i++)
    if (im->atlases.data[i]->key == key) {
      imp_atlas *a = im->atlases.data[i];
      *radius = a->radius;
      memcpy(center_local, a->center, sizeof a->center);
      return a->ok ? i : -1;
    }
  imp_atlas *a = cw_calloc(1, sizeof *a, CW_MEM_RENDER);
  a->key = key;
  cw_array_push(&im->atlases, a);
  int index = im->atlases.count - 1;
  t3_renderer *r = ctx->renderer;
  if (!r || (!wg && !gl_init())) return -1;

  /* the meshes at full detail, in bake space, twice: unlit albedo and
   * view-space normals */
  cw__mesh3d_set_lod(src, 0);
  mesh_list ml = { 0 };
  t3_object_traverse(info->obj, collect_meshes, &ml);
  if (!ml.m.count) {
    cw_array_free(&ml.m);
    return -1;
  }
  float bmn[3] = { 1e30f, 1e30f, 1e30f }, bmx[3] = { -1e30f, -1e30f, -1e30f };
  t3_scene *sa = t3_scene_new(), *sn = t3_scene_new();
  t3_material *nm = t3_mesh_normal_material_new();
  nm->tone_mapped = false;
  for (int i = 0; i < ml.m.count; i++) {
    t3_mesh *m = ml.m.data[i];
    t3_mat4 rel;
    t3_mat4_multiply(&rel, &base, &m->base.matrix_world);
    t3_geometry *g = m->geometry;
    if (!g->has_bounding_box) t3_geometry_compute_bounding_box(g);
    for (int c = 0; c < 8; c++) {
      t3_vec3 p = { c & 1 ? g->bounding_box.max.x : g->bounding_box.min.x, c & 2 ? g->bounding_box.max.y : g->bounding_box.min.y,
                    c & 4 ? g->bounding_box.max.z : g->bounding_box.min.z };
      p = t3_vec3_apply_mat4(p, &rel);
      bmn[0] = fminf(bmn[0], p.x);
      bmn[1] = fminf(bmn[1], p.y);
      bmn[2] = fminf(bmn[2], p.z);
      bmx[0] = fmaxf(bmx[0], p.x);
      bmx[1] = fmaxf(bmx[1], p.y);
      bmx[2] = fmaxf(bmx[2], p.z);
    }
    t3_material *mats[8];
    int nmat = CW_MIN(m->material_count, 8);
    for (int k = 0; k < nmat; k++) {
      const t3_material *o = m->materials[k];
      t3_material *b = t3_mesh_basic_material_new(0xffffff);
      b->color = o->color;
      if (o->map) t3_material_set_map(b, o->map);
      b->side = o->side;
      b->alpha_test = o->alpha_test;
      b->vertex_colors = o->vertex_colors;
      b->tone_mapped = false;
      mats[k] = b;
    }
    double e[16];
    for (int k = 0; k < 16; k++) e[k] = rel.e[k];
    t3_mesh *ca = t3_mesh_new_multi(g, mats, nmat);
    t3_object_set_matrix_d(ca, e);
    t3_object_add(&sa->base, ca);
    t3_release(ca);
    t3_material *nmats[8];
    for (int k = 0; k < nmat; k++) nmats[k] = nm;
    t3_mesh *cn = t3_mesh_new_multi(g, nmats, nmat);
    t3_object_set_matrix_d(cn, e);
    t3_object_add(&sn->base, cn);
    t3_release(cn);
    for (int k = 0; k < nmat; k++) t3_release(mats[k]);
  }
  t3_release(nm);
  cw_array_free(&ml.m);
  for (int k = 0; k < 3; k++) a->center[k] = (bmn[k] + bmx[k]) * 0.5f;
  float dx = bmx[0] - bmn[0], dy = bmx[1] - bmn[1], dz = bmx[2] - bmn[2];
  a->radius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz) * 1.02f;
  double t0 = bake_timing() ? bake_clock_start(r) : 0;

#if CW_WGPU
  if (wg) {
    bool ok = wg_bake(r, sa, sn, a);
    t3_release(sa);
    t3_release(sn);
    if (!ok) return -1;
    if (t0) bake_clock_report(r, t0, "WebGPU");
    a->ok = true;
    *radius = a->radius;
    memcpy(center_local, a->center, sizeof a->center);
    return index;
  }
#endif
  a->albedo = atlas_tex();
  a->normal = atlas_tex();
  glGenRenderbuffers(1, &a->depth_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, a->depth_rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, IMP_SIZE, IMP_SIZE);
  glGenFramebuffers(1, &a->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a->albedo, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, a->depth_rb);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    cw_error("impostor atlas: the RGBA8 bake target is not complete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    t3_release(sa);
    t3_release(sn);
    return -1;
  }
  float ca = 1;
  t3_color cc = t3_renderer_get_clear_color(r, &ca);
  render_views(r, sa, a, &(bake_dst){ .fbo = a->fbo, .tex = a->albedo });
  render_views(r, sn, a, &(bake_dst){ .fbo = a->fbo, .tex = a->normal });
  t3_renderer_set_size(r, ctx->width, ctx->height);
  t3_renderer_set_clear_color(r, ((uint32_t)(fminf(fmaxf(cc.r, 0), 1) * 255 + 0.5f) << 16) |
                                     ((uint32_t)(fminf(fmaxf(cc.g, 0), 1) * 255 + 0.5f) << 8) |
                                     (uint32_t)(fminf(fmaxf(cc.b, 0), 1) * 255 + 0.5f),
                              ca);
  t3_renderer_reset_bindings(r);
  t3_release(sa);
  t3_release(sn);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (t0) bake_clock_report(r, t0, "GL");
  a->ok = true;
  *radius = a->radius;
  memcpy(center_local, a->center, sizeof a->center);
  return index;
}

/* tests (and tools): an atlas from pixels (IMP_SIZE^2 RGBA8 albedo with
 * coverage in alpha, and normals), with its bake-space bounding sphere;
 * mipmaps by 2x2 box filtering. Returns the atlas index. */
int cw__vis_impostor_atlas_from_pixels(vis_impostors *im, const uint8_t *albedo, const uint8_t *normal, float radius,
                                       const float center[3]);
int cw__vis_impostor_atlas_from_pixels(vis_impostors *im, const uint8_t *albedo, const uint8_t *normal, float radius,
                                       const float center[3]) {
  imp_atlas *a = cw_calloc(1, sizeof *a, CW_MEM_RENDER);
  a->key = 0;
  a->radius = radius;
  memcpy(a->center, center, sizeof a->center);
#if CW_WGPU
  if (cw__wgpu_on()) {
    for (int k = 0; k < 2; k++) {
      WGPUTexture t = cw__wg_texture(WGPUTextureDimension_2D, IMP_SIZE, IMP_SIZE, 1, IMP_MIPS, WGPUTextureFormat_RGBA8Unorm,
                                     WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                     WGPUTextureViewDimension_Undefined, k ? "impostor normal" : "impostor albedo");
      const uint8_t *px = k ? normal : albedo;
      cw__wg_tex_write(t, 0, 0, 0, 0, IMP_SIZE, IMP_SIZE, 4, px);
      cw__fx_wg_write_mips(t, IMP_SIZE, IMP_SIZE, IMP_MIPS, px);
      WGPUTextureView v = cw__wg_view(t, WGPUTextureViewDimension_2D, 0, IMP_MIPS, 0, 1);
      wgpuTextureRelease(t);
      if (k) a->wnormal = v;
      else a->walbedo = v;
    }
    a->ok = true;
    cw_array_push(&im->atlases, a);
    return im->atlases.count - 1;
  }
#endif
  a->albedo = atlas_tex();
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, IMP_SIZE, IMP_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, albedo);
  glGenerateMipmap(GL_TEXTURE_2D);
  a->normal = atlas_tex();
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, IMP_SIZE, IMP_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, normal);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  a->ok = true;
  cw_array_push(&im->atlases, a);
  return im->atlases.count - 1;
}

/* ── lights ─────────────────────────────────────────────────────────── */

typedef struct imp_lights {
  int ndir;
  float dir[IMP_MAX_DIR][3], dcol[IMP_MAX_DIR][3];
  float amb[3], sky[3], ground[3], up[3];
} imp_lights;

static void collect_lights(t3_object *o, void *u) {
  imp_lights *L = u;
  if (!o->visible) return;
  t3_light *l = (t3_light *)o;
  if (o->type == T3_AMBIENT_LIGHT) {
    L->amb[0] += l->color.r * l->intensity;
    L->amb[1] += l->color.g * l->intensity;
    L->amb[2] += l->color.b * l->intensity;
  } else if (o->type == T3_HEMISPHERE_LIGHT) {
    L->sky[0] += l->color.r * l->intensity;
    L->sky[1] += l->color.g * l->intensity;
    L->sky[2] += l->color.b * l->intensity;
    L->ground[0] += l->ground_color.r * l->intensity;
    L->ground[1] += l->ground_color.g * l->intensity;
    L->ground[2] += l->ground_color.b * l->intensity;
    t3_vec3 up = t3_vec3_normalize(t3_mat4_get_position(&o->matrix_world));
    if (t3_vec3_length_sq(up) < 1e-12f) up = (t3_vec3){ 0, 1, 0 };
    L->up[0] = up.x;
    L->up[1] = up.y;
    L->up[2] = up.z;
  }
}

/* directional lights from their nodes (the light shines along the node's -Z) */
static bool dir_node(cw_node n, void *u) {
  imp_lights *L = u;
  cw_value vis, col, en;
  if (cw_node_get_prop(n, "visible", &vis) && vis.type == CW_T_BOOL && !vis.b) return false;
  if (!cw_node_is(n, "DirectionalLight3D") || L->ndir >= IMP_MAX_DIR) return true;
  if (!cw_node_get_prop(n, "color", &col) || !cw_node_get_prop(n, "energy", &en)) return true;
  t3_vec3 d = t3_vec3_normalize(t3_vec3_transform_direction((t3_vec3){ 0, 0, 1 }, cw_node3d_global_transform(n)));
  L->dir[L->ndir][0] = d.x;
  L->dir[L->ndir][1] = d.y;
  L->dir[L->ndir][2] = d.z;
  for (int k = 0; k < 3; k++) L->dcol[L->ndir][k] = col.v[k] * (float)en.f;
  L->ndir++;
  return true;
}

/* ── drawing ────────────────────────────────────────────────────────── */

static t3_vec3 sort_cam;
static int far_first(const void *a, const void *b) {
  const vis_impostor_inst *x = a, *y = b;
  float dx = t3_vec3_distance_sq((t3_vec3){ x->center[0], x->center[1], x->center[2] }, sort_cam);
  float dy = t3_vec3_distance_sq((t3_vec3){ y->center[0], y->center[1], y->center[2] }, sort_cam);
  return dx > dy ? -1 : dx < dy ? 1 : 0;
}

#if CW_WGPU
static int draw_wgpu(vis_impostors *im, cw_pass_ctx *ctx, const imp_lights *L);
#endif

int cw__vis_impostors_draw(vis_impostors *im, cw_pass_ctx *ctx) {
#if CW_WGPU
  if (im && cw__wgpu_on()) {
    t3_object_update_world_matrix(ctx->camera, true, false);
    imp_lights L = { 0 };
    L.up[1] = 1;
    if (ctx->scene3d) t3_object_traverse(&ctx->scene3d->base, collect_lights, &L);
    cw_node_walk(cw_tree_root(ctx->tree), dir_node, &L);
    return draw_wgpu(im, ctx, &L);
  }
#endif
  if (!im || !gl_init()) return 0;
  int draws = 0;
  t3_mat4 view;
  t3_object_update_world_matrix(ctx->camera, true, false);
  t3_mat4_invert(&view, &ctx->camera->base.matrix_world);
  t3_vec3 cp = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  t3_scene *sc = ctx->scene3d;
  imp_lights L = { 0 };
  L.up[1] = 1;
  t3_object_traverse(&sc->base, collect_lights, &L);
  cw_node_walk(cw_tree_root(ctx->tree), dir_node, &L);
  for (int pass = 0; pass < 2; pass++) /* opaque first, then the fading ones over the meshes */
    for (int ai = 0; ai < im->atlases.count; ai++) {
      imp_atlas *a = im->atlases.data[ai];
      if (!a->ok || !a->inst.count) continue;
      int n = 0;
      vis_impostor_inst *sel = cw_frame_alloc(sizeof *sel * (size_t)a->inst.count);
      for (int i = 0; i < a->inst.count; i++)
        if ((a->inst.data[i].alpha >= 1) == (pass == 0)) sel[n++] = a->inst.data[i];
      if (!n) continue;
      if (pass == 1) { /* blended ones back to front: they do not write depth */
        sort_cam = cp;
        qsort(sel, (size_t)n, sizeof *sel, far_first);
      }
      if (!a->vbo) glGenBuffers(1, &a->vbo);
      glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
      if (a->vbo_cap < n) {
        a->vbo_cap = n + 64;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof *sel * (size_t)a->vbo_cap), NULL, GL_DYNAMIC_DRAW);
      }
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(sizeof *sel * (size_t)n), sel);
      glUseProgram(prog);
      glUniformMatrix4fv(glGetUniformLocation(prog, "u_view"), 1, GL_FALSE, view.e);
      glUniformMatrix4fv(glGetUniformLocation(prog, "u_proj"), 1, GL_FALSE, ctx->camera->projection_matrix.e);
      glUniform3f(glGetUniformLocation(prog, "u_campos"), cp.x, cp.y, cp.z);
      glUniform3f(glGetUniformLocation(prog, "u_bake_center"), a->center[0], a->center[1], a->center[2]);
      glUniform1i(glGetUniformLocation(prog, "u_albedo"), 0);
      glUniform1i(glGetUniformLocation(prog, "u_normal"), 1);
      glUniform1i(glGetUniformLocation(prog, "u_ndir"), L.ndir);
      if (L.ndir) {
        glUniform3fv(glGetUniformLocation(prog, "u_dir_l"), L.ndir, &L.dir[0][0]);
        glUniform3fv(glGetUniformLocation(prog, "u_dir_c"), L.ndir, &L.dcol[0][0]);
      }
      glUniform3fv(glGetUniformLocation(prog, "u_ambient"), 1, L.amb);
      glUniform3fv(glGetUniformLocation(prog, "u_hemi_sky"), 1, L.sky);
      glUniform3fv(glGetUniformLocation(prog, "u_hemi_ground"), 1, L.ground);
      glUniform3fv(glGetUniformLocation(prog, "u_hemi_up"), 1, L.up);
      glUniform1f(glGetUniformLocation(prog, "u_light_scale"), 1.0f / CW_PI);
      int fog = sc->fog.type == T3_FOG_LINEAR ? 1 : sc->fog.type == T3_FOG_EXP2 ? 2 : 0;
      glUniform1i(glGetUniformLocation(prog, "u_fog"), fog);
      glUniform4f(glGetUniformLocation(prog, "u_fog_par"), sc->fog.near, sc->fog.far, sc->fog.density, 0);
      glUniform3f(glGetUniformLocation(prog, "u_fog_color"), sc->fog.color.r, sc->fog.color.g, sc->fog.color.b);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, a->albedo);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, a->normal);
      glActiveTexture(GL_TEXTURE0);
      glBindVertexArray(vao);
      glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, 0);
      glVertexAttribDivisor(0, 0);
      glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
      const GLsizei st = sizeof(vis_impostor_inst);
      glEnableVertexAttribArray(1);
      glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, st, (const void *)offsetof(vis_impostor_inst, center));
      glVertexAttribDivisor(1, 1);
      glEnableVertexAttribArray(2);
      glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (const void *)offsetof(vis_impostor_inst, quat));
      glVertexAttribDivisor(2, 1);
      glEnableVertexAttribArray(3);
      glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, st, (const void *)offsetof(vis_impostor_inst, alpha));
      glVertexAttribDivisor(3, 1);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LEQUAL);
      glDisable(GL_CULL_FACE);
      if (pass == 0) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
      } else {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glDepthMask(GL_FALSE);
      }
      glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
      draws++;
      for (int k = 0; k < 4; k++) {
        glVertexAttribDivisor((GLuint)k, 0);
        glDisableVertexAttribArray((GLuint)k);
      }
      glBindVertexArray(0);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  return draws;
}

#if CW_WGPU
/* ── drawing on WebGPU: the same quads and shading in WGSL ─────────────── */

static const char WGSL_IMP[] =
    "struct U { view: mat4x4f, proj: mat4x4f, campos: vec4f, bake: vec4f, ndir: vec4i, dir_l: array<vec4f, 4>,\n"
    "  dir_c: array<vec4f, 4>, ambient: vec4f, sky: vec4f, ground: vec4f, up: vec4f, fog: vec4i, fog_par: vec4f, fog_col: vec4f }\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var albedo: texture_2d<f32>;\n"
    "@group(0) @binding(2) var normal: texture_2d<f32>;\n"
    "@group(0) @binding(3) var samp: sampler;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) off: vec3f, @location(1) dir: vec3f,\n"
    "  @location(2) @interpolate(flat, either) quat: vec4f, @location(3) alpha: f32, @location(4) r: f32, @location(5) depth: f32 }\n"
    "fn qrot_inv(q: vec4f, v: vec3f) -> vec3f { let uu = -q.xyz; let t = 2.0 * cross(uu, v); return v + q.w * t + cross(uu, t); }\n"
    "fn qrot(q: vec4f, v: vec3f) -> vec3f { let t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }\n"
    "@vertex fn vs(@location(0) corner: vec2f, @location(1) cr: vec4f, @location(2) quat: vec4f, @location(3) alpha: f32) -> VO {\n"
    "  var o: VO;\n"
    "  let c = cr.xyz; let r = cr.w;\n"
    "  let tc = u.campos.xyz - c; let dist = length(tc); let dir = tc / max(dist, 1e-6);\n"
    "  let upr = select(vec3f(0.0, 1.0, 0.0), vec3f(0.0, 0.0, 1.0), abs(dir.y) > 0.99);\n"
    "  let right = normalize(cross(upr, dir)); let up = cross(dir, right);\n"
    "  let pull = min(r, dist * 0.5);\n"
    "  let cq = c + dir * pull;\n"
    "  let half_size = (dist - pull) * r / sqrt(max(dist * dist - r * r, 1e-6)) * 1.03;\n"
    "  let pq = cq + (right * corner.x + up * corner.y) * half_size;\n"
    "  let p = u.campos.xyz + (pq - u.campos.xyz) * (dist / max(dist - pull, 1e-4));\n"
    "  o.off = qrot_inv(quat, p - c);\n"
    "  o.dir = qrot_inv(quat, dir);\n"
    "  o.quat = quat; o.alpha = alpha; o.r = r;\n"
    "  o.depth = -(u.view * vec4f(c, 1.0)).z;\n"
    "  o.pos = cw_clip(u.proj * (u.view * vec4f(pq, 1.0)), u.bake.w);\n"
    "  return o;\n"
    "}\n"
    "const G = 8.0;\n"
    "fn oct_enc(n0: vec3f) -> vec2f {\n"
    "  let n = n0 / (abs(n0.x) + abs(n0.y) + abs(n0.z));\n"
    "  var p = n.xz;\n"
    "  if (n.y < 0.0) { p = (1.0 - abs(n.zx)) * vec2f(select(-1.0, 1.0, n.x >= 0.0), select(-1.0, 1.0, n.z >= 0.0)); }\n"
    "  return p * 0.5 + 0.5;\n"
    "}\n"
    "fn oct_dec(uv: vec2f) -> vec3f {\n"
    "  let p = uv * 2.0 - 1.0;\n"
    "  var n = vec3f(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);\n"
    "  if (n.y < 0.0) { let xz = (1.0 - abs(n.zx)) * vec2f(select(-1.0, 1.0, n.x >= 0.0), select(-1.0, 1.0, n.z >= 0.0)); n.x = xz.x; n.z = xz.y; }\n"
    "  return normalize(n);\n"
    "}\n"
    /* one view; sampled unconditionally (implicit-lod sampling stays in
     * uniform control flow), its weight is 0 outside the view\x27s square */
    "fn cell(ij: vec2f, off: vec3f, w: f32, r: f32, alb: ptr<function, vec4f>, nrm: ptr<function, vec3f>) {\n"
    "  let d = oct_dec((ij + 0.5) / G);\n"
    "  let upr = select(vec3f(0.0, 1.0, 0.0), vec3f(0.0, 0.0, -1.0), abs(d.y) > 0.999);\n"
    "  let x = normalize(cross(upr, d)); let y = cross(d, x);\n"
    "  let o = off - u.bake.xyz;\n"
    "  let uv0 = vec2f(dot(o, x), dot(o, y)) / (2.0 * r) + 0.5;\n"
    "  let inside = all(uv0 >= vec2f(0.0)) && all(uv0 <= vec2f(1.0));\n"
    "  let uv = clamp(uv0, vec2f(0.5 / 128.0), vec2f(1.0 - 0.5 / 128.0));\n"
    "  let t = (ij + uv) / G;\n"
    /* the atlas holds GL\x27s rows in memory order, and both APIs put
     * texture coordinate 0 at the first row: the same coordinates */
    "  let a = textureSample(albedo, samp, t);\n"
    "  let nv = textureSample(normal, samp, t).xyz * 2.0 - 1.0;\n"
    "  let ww = select(0.0, w, inside);\n"
    "  *alb += a * ww;\n"
    "  *nrm += (x * nv.x + y * nv.y + d * nv.z) * (a.a * ww);\n"
    "}\n"
    "@fragment fn fs(in: VO) -> @location(0) vec4f {\n"
    "  let g = oct_enc(normalize(in.dir)) * G - 0.5;\n"
    "  let c0 = floor(g); let f = g - c0;\n"
    "  let a = clamp(c0, vec2f(0.0), vec2f(G - 1.0)); let b = clamp(c0 + 1.0, vec2f(0.0), vec2f(G - 1.0));\n"
    "  let off = in.off + u.bake.xyz;\n"
    "  var alb = vec4f(0.0); var nrm = vec3f(0.0);\n"
    "  cell(a, off, (1.0 - f.x) * (1.0 - f.y), in.r, &alb, &nrm);\n"
    "  cell(vec2f(b.x, a.y), off, f.x * (1.0 - f.y), in.r, &alb, &nrm);\n"
    "  cell(vec2f(a.x, b.y), off, (1.0 - f.x) * f.y, in.r, &alb, &nrm);\n"
    "  cell(b, off, f.x * f.y, in.r, &alb, &nrm);\n"
    "  if (alb.a < 0.5) { discard; }\n"
    "  let albedo_c = alb.rgb / alb.a;\n"
    "  let n = qrot(in.quat, normalize(nrm + vec3f(0.0, 1e-5, 0.0)));\n"
    "  var irr = u.ambient.rgb + mix(u.ground.rgb, u.sky.rgb, dot(n, u.up.xyz) * 0.5 + 0.5);\n"
    "  for (var i = 0; i < 4; i++) { if (i >= u.ndir.x) { break; } irr += u.dir_c[i].rgb * max(dot(n, u.dir_l[i].xyz), 0.0); }\n"
    "  var rgb = albedo_c * irr * u.ambient.w;\n"
    "  if (u.fog.x == 1) { rgb = mix(rgb, u.fog_col.rgb, smoothstep(u.fog_par.x, u.fog_par.y, in.depth)); }\n"
    "  else if (u.fog.x == 2) { rgb = mix(rgb, u.fog_col.rgb, 1.0 - exp(-u.fog_par.z * u.fog_par.z * in.depth * in.depth)); }\n"
    "  return vec4f(rgb * in.alpha, in.alpha);\n"
    "}\n";

typedef struct imp_pipe { uint32_t key; WGPURenderPipeline p; } imp_pipe;
static struct {
  WGPUShaderModule sm;
  WGPUBindGroupLayout bgl;
  CW_ARRAY(imp_pipe) pipes;
  WGPUBuffer quad, ubo;
  int ubo_cap;
} W;

static WGPURenderPipeline imp_pipe_for(int pass, const cw__wgpu_pass_targets *tg) {
  uint32_t key = (uint32_t)pass | (uint32_t)tg->flip_y << 1 | (uint32_t)(tg->samples > 1 ? tg->samples : 1) << 2 |
                 (uint32_t)tg->color_format << 8 | (uint32_t)tg->depth_format << 20;
  for (int i = 0; i < W.pipes.count; i++)
    if (W.pipes.data[i].key == key) return W.pipes.data[i].p;
  if (!W.sm) {
    const char *const parts[] = { WGSL_IMP, NULL };
    W.sm = cw__wg_module("impostors", parts);
    WGPUShaderStage VF = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, F = WGPUShaderStage_Fragment;
    const cw__wg_slot sl[4] = { { CW_WB_UNIFORM_DYN, VF, 0, 432 }, { CW_WB_TEX, F, 0, 0 }, { CW_WB_TEX, F, 0, 0 },
                                { CW_WB_SAMPLER, F, 0, 0 } };
    W.bgl = cw__wg_layout(sl, 4);
    static const float q[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    W.quad = cw__wg_buffer(sizeof q, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "impostor quad");
    cw__wg_write(W.quad, 0, q, sizeof q);
  }
  WGPUVertexAttribute qa = { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 };
  WGPUVertexAttribute ia[3] = {
    { .format = WGPUVertexFormat_Float32x4, .offset = offsetof(vis_impostor_inst, center), .shaderLocation = 1 },
    { .format = WGPUVertexFormat_Float32x4, .offset = offsetof(vis_impostor_inst, quat), .shaderLocation = 2 },
    { .format = WGPUVertexFormat_Float32, .offset = offsetof(vis_impostor_inst, alpha), .shaderLocation = 3 } };
  WGPUVertexBufferLayout vb[2] = {
    { .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 8, .attributeCount = 1, .attributes = &qa },
    { .stepMode = WGPUVertexStepMode_Instance, .arrayStride = sizeof(vis_impostor_inst), .attributeCount = 3, .attributes = ia } };
  WGPUColorTargetState ct = { .format = tg->color_format, .blend = pass ? cw__wg_blend(CW_WG_BLEND_PREMUL) : NULL,
                              .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = W.sm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
  WGPUDepthStencilState ds = { .format = tg->depth_format,
                               .depthWriteEnabled = pass ? WGPUOptionalBool_False : WGPUOptionalBool_True,
                               .depthCompare = WGPUCompareFunction_LessEqual,
                               .stencilFront = { .compare = WGPUCompareFunction_Always },
                               .stencilBack = { .compare = WGPUCompareFunction_Always } };
  WGPURenderPipelineDescriptor d = {
    .label = CW_WGPU_SV("impostors"), .layout = cw__wg_pipeline_layout(&W.bgl, 1),
    .vertex = { .module = W.sm, .entryPoint = CW_WGPU_SV("vs"), .bufferCount = 2, .buffers = vb },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleStrip, .cullMode = WGPUCullMode_None },
    .depthStencil = tg->depth ? &ds : NULL,
    .multisample = { .count = (uint32_t)(tg->samples > 1 ? tg->samples : 1), .mask = 0xffffffffu },
    .fragment = &fs,
  };
  WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
  wgpuPipelineLayoutRelease(d.layout);
  cw_array_push(&W.pipes, ((imp_pipe){ key, p }));
  return p;
}

static int draw_wgpu(vis_impostors *im, cw_pass_ctx *ctx, const imp_lights *L) {
  const cw__wgpu_pass_targets *tg = cw__fx_pass_targets(ctx);
  if (!tg || !tg->enc || !tg->color) {
    static bool warned;
    if (!warned) cw_error("impostors: the pipeline gave this pass no WebGPU targets; impostors are not drawn");
    warned = true;
    return 0;
  }
  t3_mat4 view;
  t3_mat4_invert(&view, &ctx->camera->base.matrix_world);
  t3_vec3 cp = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  t3_scene *sc = ctx->scene3d;
  /* one uniform block per atlas (the bake centre differs) */
  int na = im->atlases.count;
  if (!na) return 0;
  if (W.ubo_cap < na) {
    if (W.ubo) wgpuBufferRelease(W.ubo);
    W.ubo_cap = na + 8;
    W.ubo = cw__wg_buffer((size_t)W.ubo_cap * 512, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "impostor u");
  }
  imp_pipe_for(0, tg); /* makes the module and layout */
  uint8_t *ub = cw_frame_calloc((size_t)na * 512, 1);
  for (int ai = 0; ai < na; ai++) {
    const imp_atlas *a = im->atlases.data[ai];
    float *o = (float *)(ub + 512 * ai);
    int32_t *oi = (int32_t *)o;
    memcpy(o, view.e, 64);
    memcpy(o + 16, ctx->camera->projection_matrix.e, 64);
    o[32] = cp.x;
    o[33] = cp.y;
    o[34] = cp.z;
    memcpy(o + 36, a->center, 12);
    o[39] = tg->flip_y ? -1.0f : 1.0f;
    oi[40] = L->ndir;
    for (int i = 0; i < L->ndir; i++) {
      memcpy(o + 44 + 4 * i, L->dir[i], 12);
      memcpy(o + 60 + 4 * i, L->dcol[i], 12);
    }
    memcpy(o + 76, L->amb, 12);
    o[79] = 1.0f / CW_PI; /* physical light units */
    memcpy(o + 80, L->sky, 12);
    memcpy(o + 84, L->ground, 12);
    memcpy(o + 88, L->up, 12);
    oi[92] = sc ? (sc->fog.type == T3_FOG_LINEAR ? 1 : sc->fog.type == T3_FOG_EXP2 ? 2 : 0) : 0;
    if (sc) {
      o[96] = sc->fog.near;
      o[97] = sc->fog.far;
      o[98] = sc->fog.density;
      o[100] = sc->fog.color.r;
      o[101] = sc->fog.color.g;
      o[102] = sc->fog.color.b;
    }
  }
  cw__wg_write(W.ubo, 0, ub, (size_t)na * 512);
  int draws = 0;
  WGPURenderPassEncoder rp = NULL;
  for (int pass = 0; pass < 2; pass++) /* opaque first, then the fading ones over the meshes */
    for (int ai = 0; ai < na; ai++) {
      imp_atlas *a = im->atlases.data[ai];
      if (!a->ok || !a->inst.count || !a->walbedo) continue;
      int n = 0;
      vis_impostor_inst *sel = cw_frame_alloc(sizeof *sel * (size_t)a->inst.count);
      for (int i = 0; i < a->inst.count; i++)
        if ((a->inst.data[i].alpha >= 1) == (pass == 0)) sel[n++] = a->inst.data[i];
      if (!n) continue;
      if (pass == 1) {
        sort_cam = cp;
        qsort(sel, (size_t)n, sizeof *sel, far_first);
      }
      /* both passes' instances live in one buffer: opaque ones at 0, fading ones after */
      size_t base = pass == 0 ? 0 : (size_t)a->inst.count;
      if (a->wvbo_cap < 2 * a->inst.count) {
        if (a->wvbo) wgpuBufferRelease(a->wvbo);
        a->wvbo_cap = 2 * a->inst.count + 64;
        a->wvbo = cw__wg_buffer(sizeof *sel * (size_t)a->wvbo_cap, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                "impostor instances");
      }
      cw__wg_write(a->wvbo, sizeof *sel * base, sel, sizeof *sel * (size_t)n);
      if (!rp) rp = cw__wg_scene_pass(tg, true, "impostors");
      cw__wg_entry e[4] = { { .buffer = W.ubo, .size = 432 }, { .view = a->walbedo }, { .view = a->wnormal },
                            { .sampler = cw__wg_sampler(true, false, true) } };
      WGPUBindGroup bg = cw__wg_group(W.bgl, e, 4);
      uint32_t off = (uint32_t)(512 * ai);
      wgpuRenderPassEncoderSetPipeline(rp, imp_pipe_for(pass, tg));
      wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &off);
      wgpuRenderPassEncoderSetVertexBuffer(rp, 0, W.quad, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderSetVertexBuffer(rp, 1, a->wvbo, sizeof *sel * base, sizeof *sel * (size_t)n);
      wgpuRenderPassEncoderDraw(rp, 4, (uint32_t)n, 0, 0);
      wgpuBindGroupRelease(bg);
      draws++;
    }
  if (rp) {
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
  }
  return draws;
}
#endif

/* Decal3D: box-projected decals from a shared atlas, drawn in the "decals"
 * EFFECTS pass by rebuilding each pixel's position from the scene depth.
 * See cartwheel/decals.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/decals.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "fx_internal.h"
#include "fx_wgpu.h"

unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file,
                                     int desired_channels);
void stbi_image_free(void *retval_from_stbi_load);

enum { DB_MIX, DB_MULTIPLY, DB_ADD };
static const char *const dblend_names[] = { "MIX", "MULTIPLY", "ADD", NULL };
enum { DM_SCREEN, DM_MESH };
static const char *const dmode_names[] = { "SCREEN", "MESH", NULL };

/* mesh mode: what one decal built (the clipped pieces under their source
 * meshes, one material for them all) and what it was built from */
typedef struct mesh_decal {
  CW_ARRAY(t3_object *) pieces; /* retained; each a child of its source mesh */
  t3_material *mat;
  bool built, dirty, in_order;
  int tris, sources;
  bool truncated;
  uint32_t hash;
  float build_ms;
  uint64_t seq;
  /* the decal transform it was built at (the node's own, not interpolated) */
  t3_vec3 at_pos, at_scale;
  t3_quat at_quat;
} mesh_decal;

typedef struct decal3d {
  t3_vec3 size;
  const char *texture;
  float region[4];
  float color[4];
  float energy;
  int blend;
  float normal_fade, edge_fade;
  int priority;
  /* mesh mode */
  int mode;
  const char *target;     /* node path from the decal; "" = every mesh the box overlaps */
  int max_triangles;
  float mesh_offset;
  int max_mesh_decals;
  mesh_decal mesh;
} decal3d;

static const cw_node_type *decal_t;

/* ── the atlas ──────────────────────────────────────────────────────── */

#define ATLAS 2048
#define PAD 4
typedef struct atlas_img { const char *path; int x, y, w, h; bool ok; } atlas_img;
static struct {
  GLuint tex;
  CW_ARRAY(atlas_img) imgs;
  int shelf_x, shelf_y, shelf_h;
  bool dirty;
#if CW_WGPU
  /* WebGPU: the atlas (3 mip levels, as GL's MAX_LEVEL 2) and its texels on
   * the CPU, so mips are rebuilt by box filtering (glGenerateMipmap's) */
  WGPUTexture wtex;
  WGPUTextureView wview;
  uint8_t *cpu;
#endif
} atlas;

static bool atlas_alloc(int w, int h, int *x, int *y) {
  int pw = w + 2 * PAD, ph = h + 2 * PAD;
  if (pw > ATLAS || ph > ATLAS) return false;
  if (atlas.shelf_x + pw > ATLAS) {
    atlas.shelf_y += atlas.shelf_h;
    atlas.shelf_x = 0;
    atlas.shelf_h = 0;
  }
  if (atlas.shelf_y + ph > ATLAS) return false;
  *x = atlas.shelf_x + PAD;
  *y = atlas.shelf_y + PAD;
  atlas.shelf_x += pw;
  if (ph > atlas.shelf_h) atlas.shelf_h = ph;
  return true;
}

/* upload w x h RGBA at (x, y) with the border extruded PAD pixels */
static void atlas_upload(int x, int y, int w, int h, const uint8_t *px) {
  int pw = w + 2 * PAD, ph = h + 2 * PAD;
  uint8_t *buf = cw_alloc((size_t)pw * ph * 4, CW_MEM_RENDER);
  for (int j = 0; j < ph; j++)
    for (int i = 0; i < pw; i++) {
      int sx = CW_CLAMP(i - PAD, 0, w - 1), sy = CW_CLAMP(j - PAD, 0, h - 1);
      memcpy(buf + 4 * ((size_t)j * pw + i), px + 4 * ((size_t)sy * w + sx), 4);
    }
#if CW_WGPU
  if (cw__fx_wgpu_on()) {
    cw__wg_tex_write(atlas.wtex, 0, x - PAD, y - PAD, 0, pw, ph, 4, buf);
    for (int j = 0; j < ph; j++) memcpy(atlas.cpu + 4 * ((size_t)(y - PAD + j) * ATLAS + (x - PAD)), buf + 4 * (size_t)j * pw, (size_t)pw * 4);
    cw_free(buf);
    atlas.dirty = true;
    return;
  }
#endif
  glBindTexture(GL_TEXTURE_2D, atlas.tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x - PAD, y - PAD, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  cw_free(buf);
  atlas.dirty = true;
}

static void atlas_init(void) {
#if CW_WGPU
  if (cw__fx_wgpu_on()) {
    if (atlas.wtex) return;
    atlas.wtex = cw__wg_texture(WGPUTextureDimension_2D, ATLAS, ATLAS, 1, 3, WGPUTextureFormat_RGBA8Unorm,
                                WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, WGPUTextureViewDimension_Undefined,
                                "decal atlas");
    atlas.wview = cw__wg_view(atlas.wtex, WGPUTextureViewDimension_2D, 0, 3, 0, 1);
    atlas.cpu = cw_calloc((size_t)ATLAS * ATLAS * 4, 1, CW_MEM_RENDER);
    return;
  }
#endif
  if (atlas.tex) return;
  glGenTextures(1, &atlas.tex);
  glBindTexture(GL_TEXTURE_2D, atlas.tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS, ATLAS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  /* PAD = 4 keeps neighbours apart down to mip 2 */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
  glBindTexture(GL_TEXTURE_2D, 0);
}

/* RGBA8 texels of an interned image path ("" = the built-in soft splat);
 * NULL (logged) when missing. *stb: free with stbi_image_free, else cw_free */
static uint8_t *decode_image(const char *ip, int *w, int *h, bool *stb) {
  uint8_t *px = NULL;
  *stb = false;
  if (!*ip) {
    enum { S = 128 };
    *w = *h = S;
    px = cw_alloc(S * S * 4, CW_MEM_RENDER);
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        float dx = (x + 0.5f) / S * 2 - 1, dy = (y + 0.5f) / S * 2 - 1;
        float r = sqrtf(dx * dx + dy * dy);
        float a = CW_CLAMP((1 - r) * 4, 0, 1);
        uint8_t *o = px + 4 * (y * S + x);
        o[0] = o[1] = o[2] = 255;
        o[3] = (uint8_t)(a * 255 + 0.5f);
      }
  } else {
    size_t len = 0;
    void *bytes = cw_asset_load(ip, &len);
    if (!bytes) cw_error("Decal3D: texture '%s' not found in the assets", ip);
    else {
      int c = 0;
      px = stbi_load_from_memory(bytes, (int)len, w, h, &c, 4);
      free(bytes);
      *stb = px != NULL;
      if (!px) cw_error("Decal3D: texture '%s' could not be decoded (PNG / JPEG expected)", ip);
    }
  }
  return px;
}

/* the image of a path (cached; "" = the built-in splat) */
static const atlas_img *atlas_get(const char *path) {
  atlas_init();
  const char *ip = cw_intern(path ? path : "");
  for (int i = 0; i < atlas.imgs.count; i++)
    if (atlas.imgs.data[i].path == ip) return &atlas.imgs.data[i];
  atlas_img e = { .path = ip };
  bool from_stb = false;
  uint8_t *px = decode_image(ip, &e.w, &e.h, &from_stb);
  if (px) {
    if (atlas_alloc(e.w, e.h, &e.x, &e.y)) {
      atlas_upload(e.x, e.y, e.w, e.h, px);
      e.ok = true;
    } else
      cw_error("Decal3D: the decal atlas (%dx%d) has no room for '%s' (%dx%d); use smaller images or sheets with "
               "regions",
               ATLAS, ATLAS, ip, e.w, e.h);
    if (from_stb) stbi_image_free(px);
    else cw_free(px);
  }
  cw_array_push(&atlas.imgs, e);
  return &atlas.imgs.data[atlas.imgs.count - 1];
}

/* ── GL ─────────────────────────────────────────────────────────────── */

static const char *const vs =
    "layout(location = 0) in vec4 a_w0;\n"
    "layout(location = 1) in vec4 a_w1;\n"
    "layout(location = 2) in vec4 a_w2;\n"
    "layout(location = 3) in vec4 a_w3;\n"
    "layout(location = 4) in vec4 a_i0;\n"
    "layout(location = 5) in vec4 a_i1;\n"
    "layout(location = 6) in vec4 a_i2;\n"
    "layout(location = 7) in vec4 a_i3;\n"
    "layout(location = 8) in vec4 a_rect;\n"
    "layout(location = 9) in vec4 a_color;\n"
    "layout(location = 10) in vec4 a_par;\n"
    "layout(location = 11) in vec3 a_pos;\n"
    "uniform mat4 u_vp;\n"
    "flat out mat4 v_inv;\n"
    "flat out vec4 v_rect;\n"
    "flat out vec4 v_color;\n"
    "flat out vec4 v_par;\n"
    "flat out vec3 v_axis;\n"
    "void main() {\n"
    "  mat4 W = mat4(a_w0, a_w1, a_w2, a_w3);\n"
    "  v_inv = mat4(a_i0, a_i1, a_i2, a_i3);\n"
    "  v_rect = a_rect; v_color = a_color; v_par = a_par;\n"
    "  v_axis = normalize(W[2].xyz);\n"
    "  gl_Position = u_vp * (W * vec4(a_pos, 1.0));\n"
    "}\n";

static const char *const fs =
    "flat in mat4 v_inv;\n"
    "flat in vec4 v_rect;\n"
    "flat in vec4 v_color;\n"
    "flat in vec4 v_par;\n" /* normal_fade, edge_fade, energy, blend */
    "flat in vec3 v_axis;\n"
    "uniform sampler2D u_depth;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform mat4 u_ivp;\n"
    "uniform vec2 u_inv_size;\n"
    "uniform vec3 u_campos;\n"
    "uniform int u_linear;\n"
    "out vec4 frag;\n"
    "vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
    "void main() {\n"
    "  vec2 uv = gl_FragCoord.xy * u_inv_size;\n"
    "  float d = texture(u_depth, uv).r;\n"
    "  if (d >= 1.0) discard;\n"
    "  vec4 w = u_ivp * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "  vec3 wp = w.xyz / w.w;\n"
    "  vec3 lp = (v_inv * vec4(wp, 1.0)).xyz;\n"
    "  if (any(greaterThan(abs(lp), vec3(0.5)))) discard;\n"
    "  vec3 n = cross(dFdx(wp), dFdy(wp));\n"
    "  n = dot(n, n) > 0.0 ? normalize(n) : v_axis;\n"
    "  if (dot(n, u_campos - wp) < 0.0) n = -n;\n"
    "  float facing = dot(n, v_axis);\n"
    "  float a = facing <= 0.0 ? 0.0 : clamp((facing - v_par.x) / max(1.0 - v_par.x, 1e-4), 0.0, 1.0);\n"
    "  if (v_par.y > 0.0) a *= clamp((0.5 - abs(lp.z)) / v_par.y, 0.0, 1.0);\n"
    "  vec2 tuv = vec2(lp.x + 0.5, 0.5 - lp.y);\n"
    "  vec4 c = texture(u_atlas, mix(v_rect.xy, v_rect.zw, tuv));\n"
    "  if (u_linear == 1) c.rgb = to_linear(c.rgb);\n"
    "  c *= v_color;\n"
    "  c.a *= a;\n"
    "  int blend = int(v_par.w + 0.5);\n"
    "  if (blend == 1) frag = vec4(mix(vec3(1.0), c.rgb, c.a), 1.0);\n"
    "  else if (blend == 2) frag = vec4(c.rgb * c.a * v_par.z, 0.0);\n"
    "  else frag = vec4(c.rgb * c.a, c.a);\n"
    "}\n";

static GLuint prog, cube_vbo, inst_vbo, vao;
static int inst_cap;
static bool prog_failed;

static bool gl_init(void) {
  if (prog) return true;
  if (prog_failed) return false;
  prog = cw__fx_program("decals", vs, fs, NULL, 0);
  if (!prog) {
    prog_failed = true;
    return false;
  }
  /* a unit cube, outward CCW triangles */
  static const float f[6][4][3] = {
    { { 0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f } },
    { { -0.5f, -0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f }, { -0.5f, 0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f } },
    { { -0.5f, 0.5f, -0.5f }, { -0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, -0.5f } },
    { { -0.5f, -0.5f, 0.5f }, { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, 0.5f } },
    { { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f } },
    { { 0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f }, { -0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f } },
  };
  float v[36 * 3];
  int k = 0;
  static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
  for (int s = 0; s < 6; s++)
    for (int t = 0; t < 6; t++)
      for (int c = 0; c < 3; c++) v[k++] = f[s][tri[t]][c];
  glGenBuffers(1, &cube_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, cube_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
  glGenBuffers(1, &inst_vbo);
  glGenVertexArrays(1, &vao);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
}

/* ── per tree ───────────────────────────────────────────────────────── */

typedef struct mesh_tex { const char *path; t3_texture *tex; int w, h; } mesh_tex;

typedef struct decal_tree {
  CW_ARRAY(cw_node) decals;
  bool pass_added;
  cw_decal_stats stats;
  /* mesh mode */
  bool hook_added;
  CW_ARRAY(cw_node) mesh_order; /* mesh decals with geometry, oldest first */
  CW_ARRAY(mesh_tex) mesh_texs; /* decal images as three.c textures (per tree, as its geometry) */
  int mesh_limit;
  uint64_t mesh_seq;
  int mesh_recycled, mesh_builds;
#if CW_WGPU
  WGPUBuffer winst, wu; /* this tree's instances and uniforms (one write a frame) */
  int winst_cap;
#endif
} decal_tree;

static void dtree_free(void *p) {
  decal_tree *dt = p;
  for (int i = 0; i < dt->mesh_texs.count; i++) t3_release(dt->mesh_texs.data[i].tex);
  cw_array_free(&dt->mesh_texs);
  cw_array_free(&dt->mesh_order);
#if CW_WGPU
  if (dt->winst) wgpuBufferRelease(dt->winst);
  if (dt->wu) wgpuBufferRelease(dt->wu);
#endif
  cw_array_free(&dt->decals);
  cw_free(dt);
}

static decal_tree *dtree(cw_tree *t) {
  decal_tree *dt = cw_tree_slot(t, "decals");
  if (!dt) {
    dt = cw_calloc(1, sizeof *dt, CW_MEM_RENDER);
    cw_tree_set_slot(t, "decals", dt, dtree_free);
  }
  return dt;
}

void cw_decals_get_stats(cw_tree *t, cw_decal_stats *out) {
  decal_tree *dt = t ? cw_tree_slot(t, "decals") : NULL;
  if (!dt) memset(out, 0, sizeof *out);
  else *out = dt->stats;
  out->atlas_images = atlas.imgs.count;
  if (!dt) return;
  /* mesh mode: what exists now, in the order the decals entered the tree */
  out->mesh_recycled = dt->mesh_recycled;
  out->mesh_builds = dt->mesh_builds;
  uint32_t h = 2166136261u;
  for (int i = 0; i < dt->decals.count; i++) {
    const decal3d *d = cw_node_data_t(dt->decals.data[i], decal_t);
    if (!d || d->mode != DM_MESH || !d->mesh.tris) continue;
    out->mesh_decals++;
    out->mesh_triangles += d->mesh.tris;
    out->mesh_pieces += d->mesh.pieces.count;
    h = (h ^ d->mesh.hash) * 16777619u;
  }
  out->mesh_hash = h;
}

typedef struct vis { cw_node n; decal3d *d; int order; t3_mat4 w; } vis;

static int vis_cmp(const void *a, const void *b) {
  const vis *x = a, *y = b;
  if (x->d->priority != y->d->priority) return x->d->priority < y->d->priority ? -1 : 1;
  return x->order < y->order ? -1 : x->order > y->order;
}

/* box (unit cube through w) against the frustum: false when every corner is
 * outside one plane */
static bool box_visible(const t3_frustum *f, const t3_mat4 *w) {
  t3_vec3 c[8];
  for (int i = 0; i < 8; i++)
    c[i] = t3_vec3_apply_mat4((t3_vec3){ i & 1 ? 0.5f : -0.5f, i & 2 ? 0.5f : -0.5f, i & 4 ? 0.5f : -0.5f }, w);
  for (int p = 0; p < 6; p++) {
    int out = 0;
    for (int i = 0; i < 8; i++)
      if (t3_vec3_dot(f->planes[p].normal, c[i]) + f->planes[p].constant < 0) out++;
    if (out == 8) return false;
  }
  return true;
}

#if CW_WGPU
static const char WGSL_DECALS[] =
    "struct U { vp: mat4x4f, ivp: mat4x4f, proj: mat4x4f, campos: vec4f, p: vec4f }\n" /* p: 1/w, 1/h, flip, linear */
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var depth: texture_2d<f32>;\n"
    "@group(0) @binding(2) var atlas: texture_2d<f32>;\n"
    "@group(0) @binding(3) var samp: sampler;\n"
    "struct VI { @location(0) w0: vec4f, @location(1) w1: vec4f, @location(2) w2: vec4f, @location(3) w3: vec4f,\n"
    "  @location(4) i0: vec4f, @location(5) i1: vec4f, @location(6) i2: vec4f, @location(7) i3: vec4f,\n"
    "  @location(8) rect: vec4f, @location(9) color: vec4f, @location(10) par: vec4f, @location(11) pos: vec3f }\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) @interpolate(flat, either) i0: vec4f,\n"
    "  @location(1) @interpolate(flat, either) i1: vec4f, @location(2) @interpolate(flat, either) i2: vec4f,\n"
    "  @location(3) @interpolate(flat, either) i3: vec4f, @location(4) @interpolate(flat, either) rect: vec4f,\n"
    "  @location(5) @interpolate(flat, either) color: vec4f, @location(6) @interpolate(flat, either) par: vec4f,\n"
    "  @location(7) @interpolate(flat, either) axis: vec3f }\n"
    "@vertex fn vs(v: VI) -> VO {\n"
    "  var o: VO;\n"
    "  let W = mat4x4f(v.w0, v.w1, v.w2, v.w3);\n"
    "  o.i0 = v.i0; o.i1 = v.i1; o.i2 = v.i2; o.i3 = v.i3;\n"
    "  o.rect = v.rect; o.color = v.color; o.par = v.par;\n"
    "  o.axis = normalize(W[2].xyz);\n"
    "  o.pos = cw_clip(u.vp * (W * vec4f(v.pos, 1.0)), select(1.0, -1.0, u.p.z > 0.5));\n"
    "  return o;\n"
    "}\n"
    "fn to_linear(c: vec3f) -> vec3f { return select(c / 12.92, pow((c + 0.055) / 1.055, vec3f(2.4)), c >= vec3f(0.04045)); }\n"
    "@fragment fn fs(in: VO) -> @location(0) vec4f {\n"
    /* GL\x27s window coordinates (y up): the row is the GL y on a bottom-up target */
    "  var uv = in.pos.xy * u.p.xy;\n"
    "  if (u.p.z < 0.5) { uv.y = 1.0 - uv.y; }\n"
    "  let z = textureLoad(depth, vec2i(in.pos.xy), 0).r;\n"
    "  let empty = z <= 0.0 || z > 1e30;\n"
    "  let cz = u.proj[2].z * -z + u.proj[3].z; let cw = u.proj[2].w * -z + u.proj[3].w;\n"
    "  let d = select((cz / cw) * 0.5 + 0.5, 1.0, empty);\n"
    "  let w = u.ivp * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "  let wp = w.xyz / w.w;\n"
    "  let inv = mat4x4f(in.i0, in.i1, in.i2, in.i3);\n"
    "  let lp = (inv * vec4f(wp, 1.0)).xyz;\n"
    /* derivatives and the atlas fetch before any discard (uniform control flow) */
    "  var n = cross(dpdx(wp), dpdy(wp));\n"
    "  if (u.p.z < 0.5) { n = -n; }\n" /* a top-down target: framebuffer y runs against GL\x27s, dpdy is GL\x27s negated */
    "  let tuv = vec2f(lp.x + 0.5, 0.5 - lp.y);\n"
    "  var c = textureSample(atlas, samp, mix(in.rect.xy, in.rect.zw, tuv));\n"
    "  if (d >= 1.0) { discard; }\n"
    "  if (any(abs(lp) > vec3f(0.5))) { discard; }\n"
    "  n = select(in.axis, normalize(n), dot(n, n) > 0.0);\n"
    "  if (dot(n, u.campos.xyz - wp) < 0.0) { n = -n; }\n"
    "  let facing = dot(n, in.axis);\n"
    "  var a = select(clamp((facing - in.par.x) / max(1.0 - in.par.x, 1e-4), 0.0, 1.0), 0.0, facing <= 0.0);\n"
    "  if (in.par.y > 0.0) { a *= clamp((0.5 - abs(lp.z)) / in.par.y, 0.0, 1.0); }\n"
    "  if (u.p.w > 0.5) { c = vec4f(to_linear(c.rgb), c.a); }\n"
    "  c *= in.color;\n"
    "  c.a *= a;\n"
    "  let blend = i32(in.par.w + 0.5);\n"
    "  if (blend == 1) { return vec4f(mix(vec3f(1.0), c.rgb, c.a), 1.0); }\n"
    "  if (blend == 2) { return vec4f(c.rgb * c.a * in.par.z, 0.0); }\n"
    "  return vec4f(c.rgb * c.a, c.a);\n"
    "}\n";

typedef struct dpipe { uint32_t key; WGPURenderPipeline p; } dpipe;
static CW_ARRAY(dpipe) dpipes;
static WGPUShaderModule dsm;
static WGPUBindGroupLayout dbgl;
static WGPUBuffer dcube;

static WGPURenderPipeline decal_pipe(int blend, const cw__wgpu_pass_targets *tg) {
  uint32_t key = (uint32_t)blend | (uint32_t)tg->flip_y << 2 | (uint32_t)tg->color_format << 8;
  for (int i = 0; i < dpipes.count; i++)
    if (dpipes.data[i].key == key) return dpipes.data[i].p;
  if (!dsm) {
    const char *const parts[] = { WGSL_DECALS, NULL };
    dsm = cw__wg_module("decals", parts);
    WGPUShaderStage VF = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, F = WGPUShaderStage_Fragment;
    const cw__wg_slot sl[4] = { { CW_WB_UNIFORM, VF, 0, 224 }, { CW_WB_TEX_UNFILT, F, 0, 0 }, { CW_WB_TEX, F, 0, 0 },
                                { CW_WB_SAMPLER, F, 0, 0 } };
    dbgl = cw__wg_layout(sl, 4);
  }
  WGPUVertexAttribute ia[11], ca = { .format = WGPUVertexFormat_Float32x3, .offset = 0, .shaderLocation = 11 };
  for (int a = 0; a < 11; a++)
    ia[a] = (WGPUVertexAttribute){ .format = WGPUVertexFormat_Float32x4, .offset = (uint64_t)(16 * a), .shaderLocation = (uint32_t)a };
  WGPUVertexBufferLayout vb[2] = {
    { .stepMode = WGPUVertexStepMode_Instance, .arrayStride = 44 * 4, .attributeCount = 11, .attributes = ia },
    { .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 12, .attributeCount = 1, .attributes = &ca } };
  int bp = blend == DB_MULTIPLY ? CW_WG_BLEND_MULTIPLY : blend == DB_ADD ? CW_WG_BLEND_ADD : CW_WG_BLEND_PREMUL;
  WGPUColorTargetState ct = { .format = tg->color_format, .blend = cw__wg_blend(bp), .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = dsm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
  WGPURenderPipelineDescriptor d = {
    .label = CW_WGPU_SV("decals"), .layout = cw__wg_pipeline_layout(&dbgl, 1),
    .vertex = { .module = dsm, .entryPoint = CW_WGPU_SV("vs"), .bufferCount = 2, .buffers = vb },
    /* back faces only (GL culls the front): a camera inside a box still sees it */
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .frontFace = tg->flip_y ? WGPUFrontFace_CW : WGPUFrontFace_CCW,
                   .cullMode = WGPUCullMode_Front },
    .multisample = { .count = 1, .mask = 0xffffffffu },
    .fragment = &fs,
  };
  WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
  wgpuPipelineLayoutRelease(d.layout);
  cw_array_push(&dpipes, ((dpipe){ key, p }));
  return p;
}

typedef struct vis vis;
static void draw_wgpu(cw_pass_ctx *ctx, decal_tree *dt, const struct vis *list, int nv, const float *buf, int stride,
                      const t3_mat4 *vp, bool linear);
#endif

static bool decals_active(const cw_pass_ctx *ctx, void *user) {
  (void)user;
  decal_tree *dt = cw_tree_slot(ctx->tree, "decals");
  return dt && dt->decals.count && ctx->camera;
}

static void set_blend(int b) {
  glEnable(GL_BLEND);
  if (b == DB_MULTIPLY) glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
  else if (b == DB_ADD) glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
  else glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
}

static void decals_exec(cw_pass_ctx *ctx, void *user) {
  (void)user;
  decal_tree *dt = dtree(ctx->tree);
  memset(&dt->stats, 0, sizeof dt->stats);
  dt->stats.decals = dt->decals.count;
#if CW_WGPU
  if (cw__fx_wgpu_on()) {
    const cw__wgpu_pass_targets *tg = cw__fx_pass_targets(ctx);
    if (!tg || !tg->linear_depth) return;
  } else
#endif
  if (!ctx->depth_tex || !gl_init()) return;
  t3_frustum fr;
  t3_mat4 vp;
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &ctx->view);
  t3_frustum_from_matrix(&fr, &ctx->view_proj);
  vis *list = cw_frame_alloc(sizeof(vis) * (size_t)dt->decals.count);
  int nv = 0;
  for (int i = 0; i < dt->decals.count; i++) {
    cw_node n = dt->decals.data[i];
    decal3d *d = cw_node_data_t(n, decal_t);
    if (!d || d->color[3] <= 0 || d->mode == DM_MESH) continue;
    t3_object *o = cw_node3d_object(n);
    if (!o || !o->visible) continue;
    t3_object_update_world_matrix(o, true, false);
    t3_mat4 s, w;
    t3_mat4_identity(&s);
    s.e[0] = d->size.x;
    s.e[5] = d->size.y;
    s.e[10] = d->size.z;
    t3_mat4_multiply(&w, &o->matrix_world, &s);
    if (!box_visible(&fr, &w)) {
      dt->stats.culled++;
      continue;
    }
    list[nv++] = (vis){ n, d, i, w };
  }
  dt->stats.visible = nv;
  if (!nv) return;
  qsort(list, (size_t)nv, sizeof *list, vis_cmp);
  const int stride = 44;
  float *buf = cw_frame_alloc(sizeof(float) * stride * (size_t)nv);
  bool linear = ctx->hdr; /* the scene is linear HDR: images decode from sRGB */
  for (int i = 0; i < nv; i++) {
    float *o = buf + stride * i;
    const vis *v = &list[i];
    t3_mat4 inv;
    t3_mat4_invert(&inv, &v->w);
    memcpy(o, v->w.e, 64);
    memcpy(o + 16, inv.e, 64);
    const atlas_img *img = atlas_get(v->d->texture);
    float rx = 0, ry = 0, rw = (float)img->w, rh = (float)img->h;
    if (v->d->region[2] > 0 && v->d->region[3] > 0) {
      rx = v->d->region[0];
      ry = v->d->region[1];
      rw = v->d->region[2];
      rh = v->d->region[3];
    }
    o[32] = (img->x + rx) / ATLAS;
    o[33] = (img->y + ry) / ATLAS;
    o[34] = (img->x + rx + rw) / ATLAS;
    o[35] = (img->y + ry + rh) / ATLAS;
    if (!img->ok) o[32] = o[33] = o[34] = o[35] = 0;
    float c[4];
    memcpy(c, v->d->color, sizeof c);
    memcpy(o + 36, c, sizeof c);
    o[40] = CW_CLAMP(v->d->normal_fade, 0, 0.999f);
    o[41] = CW_CLAMP(v->d->edge_fade, 0, 0.5f);
    o[42] = v->d->energy;
    o[43] = (float)v->d->blend;
  }
#if CW_WGPU
  if (cw__fx_wgpu_on()) {
    draw_wgpu(ctx, dt, list, nv, buf, stride, &vp, linear);
    return;
  }
#endif
  if (atlas.dirty) {
    glBindTexture(GL_TEXTURE_2D, atlas.tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    atlas.dirty = false;
  }
  glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
  if (inst_cap < nv) {
    inst_cap = nv + 64;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * stride * (size_t)inst_cap), NULL, GL_DYNAMIC_DRAW);
  }
  glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(sizeof(float) * stride * (size_t)nv), buf);

  cw_pass_bind_target(ctx, ctx->scene);
  glUseProgram(prog);
  cw__fx_mat4(prog, "u_vp", vp.e);
  cw__fx_mat4(prog, "u_ivp", ctx->inv_view_proj.e);
  cw__fx_u2f(prog, "u_inv_size", 1.0f / (float)ctx->width, 1.0f / (float)ctx->height);
  t3_vec3 cp = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  cw__fx_u3f(prog, "u_campos", cp.x, cp.y, cp.z);
  cw__fx_u1i(prog, "u_linear", linear ? 1 : 0);
  cw__fx_u1i(prog, "u_depth", 0);
  cw__fx_u1i(prog, "u_atlas", 1);
  cw__fx_bind_tex(0, GL_TEXTURE_2D, ctx->depth_tex);
  cw__fx_bind_tex(1, GL_TEXTURE_2D, atlas.tex);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, cube_vbo);
  glEnableVertexAttribArray(11);
  glVertexAttribPointer(11, 3, GL_FLOAT, GL_FALSE, 12, 0);
  glVertexAttribDivisor(11, 0);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT);
  for (int start = 0; start < nv;) {
    int b = list[start].d->blend, end = start + 1;
    while (end < nv && list[end].d->blend == b) end++;
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    for (int a = 0; a < 11; a++) {
      glEnableVertexAttribArray((GLuint)a);
      glVertexAttribPointer((GLuint)a, 4, GL_FLOAT, GL_FALSE, stride * 4,
                            (const void *)(uintptr_t)(sizeof(float) * ((size_t)stride * start + 4 * a)));
      glVertexAttribDivisor((GLuint)a, 1);
    }
    set_blend(b);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 36, end - start);
    dt->stats.draw_calls++;
    start = end;
  }
  glCullFace(GL_BACK);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void cw__decals_test_exec(cw_pass_ctx *ctx);
void cw__decals_test_exec(cw_pass_ctx *ctx) {
  if (decals_active(ctx, NULL)) decals_exec(ctx, NULL);
}

#if CW_WGPU
static void draw_wgpu(cw_pass_ctx *ctx, decal_tree *dt, const struct vis *list, int nv, const float *buf, int stride,
                      const t3_mat4 *vp, bool linear) {
  const cw__wgpu_pass_targets *tg = cw__fx_pass_targets(ctx);
  if (atlas.dirty) {
    cw__fx_wg_write_mips(atlas.wtex, ATLAS, ATLAS, 3, atlas.cpu);
    atlas.dirty = false;
  }
  if (!dcube) {
    /* the GL path's unit cube */
    static const float f[6][4][3] = {
      { { 0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f } },
      { { -0.5f, -0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f }, { -0.5f, 0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f } },
      { { -0.5f, 0.5f, -0.5f }, { -0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, -0.5f } },
      { { -0.5f, -0.5f, 0.5f }, { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, 0.5f } },
      { { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f } },
      { { 0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f }, { -0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, -0.5f } },
    };
    float v[36 * 3];
    int k = 0;
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int s6 = 0; s6 < 6; s6++)
      for (int t = 0; t < 6; t++)
        for (int c = 0; c < 3; c++) v[k++] = f[s6][tri[t]][c];
    dcube = cw__wg_buffer(sizeof v, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "decal cube");
    cw__wg_write(dcube, 0, v, sizeof v);
  }
  if (dt->winst_cap < nv) {
    if (dt->winst) wgpuBufferRelease(dt->winst);
    dt->winst_cap = nv + 64;
    dt->winst = cw__wg_buffer(sizeof(float) * (size_t)stride * (size_t)dt->winst_cap,
                              WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "decal instances");
  }
  cw__wg_write(dt->winst, 0, buf, sizeof(float) * (size_t)stride * (size_t)nv);
  if (!dt->wu) dt->wu = cw__wg_buffer(224, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "decal u");
  float u[56] = { 0 };
  memcpy(u, vp->e, 64);
  memcpy(u + 16, ctx->inv_view_proj.e, 64);
  memcpy(u + 32, ctx->proj.e, 64);
  t3_vec3 cp = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  u[48] = cp.x;
  u[49] = cp.y;
  u[50] = cp.z;
  u[52] = 1.0f / (float)tg->w;
  u[53] = 1.0f / (float)tg->h;
  u[54] = tg->flip_y ? 1.0f : 0.0f;
  u[55] = linear ? 1.0f : 0.0f;
  cw__wg_write(dt->wu, 0, u, sizeof u);
  decal_pipe(DB_MIX, tg); /* makes the module and layout */
  cw__wg_entry e[4] = { { .buffer = dt->wu }, { .view = tg->linear_depth }, { .view = atlas.wview },
                        { .sampler = cw__wg_sampler(true, false, true) } };
  WGPUBindGroup bg = cw__wg_group(dbgl, e, 4);
  WGPURenderPassEncoder rp = cw__wg_scene_pass(tg, false, "decals");
  wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, NULL);
  wgpuRenderPassEncoderSetVertexBuffer(rp, 1, dcube, 0, WGPU_WHOLE_SIZE);
  for (int start = 0; start < nv;) {
    int b = list[start].d->blend, end = start + 1;
    while (end < nv && list[end].d->blend == b) end++;
    wgpuRenderPassEncoderSetPipeline(rp, decal_pipe(b, tg));
    wgpuRenderPassEncoderSetVertexBuffer(rp, 0, dt->winst, sizeof(float) * (size_t)stride * (size_t)start,
                                         sizeof(float) * (size_t)stride * (size_t)(end - start));
    wgpuRenderPassEncoderDraw(rp, 36, (uint32_t)(end - start), 0, 0);
    dt->stats.draw_calls++;
    start = end;
  }
  wgpuRenderPassEncoderEnd(rp);
  wgpuRenderPassEncoderRelease(rp);
  wgpuBindGroupRelease(bg);
}
#endif

/* ── mesh mode ──────────────────────────────────────────────────────── */

/* the pieces' name (by pointer): never a source of another decal */
static const char DECAL_PIECE[] = "Decal3D mesh";
double cw__render3d_now_ms(void); /* src/scene/render3d.c */

static void mesh_fill(t3_extension_ctx *c, void *u) {
  (void)c;
  (void)u; /* the meshdecal program reads only the material and the geometry */
}
/* every renderer that may draw a tree's scene knows the meshdecal extension
 * (src/scene/render3d.c calls this as it makes one) */
void cw__decals_renderer_init(t3_renderer *r);
void cw__decals_renderer_init(t3_renderer *r) {
  if (r) t3_renderer_set_extension(r, "meshdecal", mesh_fill, NULL);
}

static void mesh_clear(decal3d *d) {
  for (int i = 0; i < d->mesh.pieces.count; i++) {
    t3_object *o = d->mesh.pieces.data[i];
    if (o->parent) t3_object_remove(o->parent, o);
    t3_release(o);
  }
  cw_array_clear(&d->mesh.pieces);
  d->mesh.tris = d->mesh.sources = 0;
  d->mesh.truncated = false;
  d->mesh.hash = 0;
  d->mesh.built = false;
}

static void order_remove(decal_tree *dt, cw_node n, decal3d *d) {
  if (!d->mesh.in_order) return;
  for (int i = 0; i < dt->mesh_order.count; i++)
    if (dt->mesh_order.data[i].id == n.id) {
      memmove(dt->mesh_order.data + i, dt->mesh_order.data + i + 1, sizeof(cw_node) * (size_t)(dt->mesh_order.count - i - 1));
      dt->mesh_order.count--;
      break;
    }
  d->mesh.in_order = false;
}

static mesh_tex *mesh_texture(decal_tree *dt, const char *path) {
  const char *ip = cw_intern(path ? path : "");
  for (int i = 0; i < dt->mesh_texs.count; i++)
    if (dt->mesh_texs.data[i].path == ip) return &dt->mesh_texs.data[i];
  mesh_tex e = { .path = ip };
  bool stb = false;
  uint8_t *px = decode_image(ip, &e.w, &e.h, &stb);
  if (px) {
    /* three.js Texture defaults: flipY, mipmaps, linear, clamp; sRGB like a colour map */
    e.tex = t3_texture_new(e.w, e.h, px);
    e.tex->color_space = T3_SRGB_COLOR_SPACE;
    if (stb) stbi_image_free(px);
    else cw_free(px);
  }
  cw_array_push(&dt->mesh_texs, e);
  return &dt->mesh_texs.data[dt->mesh_texs.count - 1];
}

/* the screen-space pass's blends with the material's own factors: the
 * program writes (rgb * a, a) (src/fx/extensions/meshdecal.mjs) and the
 * target's alpha is kept */
static void mesh_material_apply(decal3d *d) {
  t3_material *m = d->mesh.mat;
  if (!m) return;
  float e = d->blend == DB_ADD ? d->energy : 1;
  m->color = (t3_color){ d->color[0] * e, d->color[1] * e, d->color[2] * e };
  m->opacity = d->color[3];
  m->blending = T3_CUSTOM_BLENDING;
  m->blend_equation = m->blend_equation_alpha = T3_ADD_EQUATION;
  m->blend_src_alpha = T3_ZERO_FACTOR;
  m->blend_dst_alpha = T3_ONE_FACTOR;
  if (d->blend == DB_MULTIPLY) {
    m->blend_src = T3_DST_COLOR_FACTOR; /* dst * (c * a) + dst * (1 - a) = dst * mix(1, c, a) */
    m->blend_dst = T3_ONE_MINUS_SRC_ALPHA_FACTOR;
  } else if (d->blend == DB_ADD) {
    m->blend_src = T3_ONE_FACTOR;
    m->blend_dst = T3_ONE_FACTOR;
  } else {
    m->blend_src = T3_ONE_FACTOR;
    m->blend_dst = T3_ONE_MINUS_SRC_ALPHA_FACTOR;
  }
  m->version++;
}

typedef CW_ARRAY(t3_mesh *) mesh_list;

static bool usable_source(const t3_mesh *m) {
  if (m->base.type != T3_MESH && m->base.type != T3_SKINNED_MESH) return false;
  const t3_geometry *g = m->geometry;
  const t3_attribute *p = g ? g->attributes[T3_ATTR_POSITION] : NULL;
  if (!p || p->type != T3_FLOAT32 || p->item_size != 3 || p->count < 3) return false;
  if (g->index && g->index->type == T3_FLOAT32) return false;
  if (!m->material_count || !m->materials[0]) return false;
  const t3_material *mt = m->materials[0];
  /* an extension program may move its vertices (terrain, water): the CPU
   * cannot see that surface */
  return !mt->extension && !mt->wireframe;
}

static void collect_sources(t3_object *o, mesh_list *out) {
  if (o->name == DECAL_PIECE) return;
  if ((o->type == T3_MESH || o->type == T3_SKINNED_MESH) && usable_source((t3_mesh *)o)) cw_array_push(out, (t3_mesh *)o);
  for (int i = 0; i < o->child_count; i++) collect_sources(o->children[i], out);
}

static bool attr4(const t3_attribute *a, int count) {
  return a && a->type == T3_FLOAT32 && a->item_size == 4 && a->count >= count;
}

static void mat_apply(const float *m, const float *v, float *o) {
  o[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12];
  o[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13];
  o[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14];
}
static void mat_dir(const float *m, const float *v, float *o) {
  o[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2];
  o[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2];
  o[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2];
}

/* the source's vertices posed in world space: skinned as three.js r186's
 * skinning node does (bindMatrixInverse * sum(w * bone.matrixWorld *
 * boneInverse) * bindMatrix), then the mesh's matrixWorld */
static void pose_source(t3_mesh *m, bool skinned, float *posed, float *posed_nrm) {
  const t3_geometry *g = m->geometry;
  const t3_attribute *pa = g->attributes[T3_ATTR_POSITION], *na = g->attributes[T3_ATTR_NORMAL];
  const float *pos = pa->array, *nrm = na ? na->array : NULL;
  int vc = pa->count;
  t3_object_update_world_matrix(m, true, false);
  if (!skinned) {
    t3_mat3 nm;
    t3_mat3_normal_matrix(&nm, &m->base.matrix_world);
    for (int i = 0; i < vc; i++) {
      mat_apply(m->base.matrix_world.e, pos + 3 * i, posed + 3 * i);
      if (posed_nrm) {
        const float *n = nrm + 3 * i;
        float *o = posed_nrm + 3 * i;
        o[0] = nm.e[0] * n[0] + nm.e[3] * n[1] + nm.e[6] * n[2];
        o[1] = nm.e[1] * n[0] + nm.e[4] * n[1] + nm.e[7] * n[2];
        o[2] = nm.e[2] * n[0] + nm.e[5] * n[1] + nm.e[8] * n[2];
      }
    }
    return;
  }
  t3_skinned_mesh *sm = (t3_skinned_mesh *)m;
  t3_skeleton *sk = sm->skeleton;
  t3_mat4 *bm = cw_alloc(sizeof(t3_mat4) * (size_t)sk->bone_count, CW_MEM_RENDER);
  for (int j = 0; j < sk->bone_count; j++) {
    t3_object_update_world_matrix(sk->bones[j], true, false);
    t3_mat4_multiply(&bm[j], &sk->bones[j]->matrix_world, &sk->bone_inverses[j]);
  }
  t3_mat4 a;
  t3_mat4_multiply(&a, &m->base.matrix_world, &sm->bind_matrix_inverse);
  const float *ji = g->attributes[T3_ATTR_SKIN_INDEX]->array, *wi = g->attributes[T3_ATTR_SKIN_WEIGHT]->array;
  for (int i = 0; i < vc; i++) {
    float bv[3], acc[3] = { 0, 0, 0 }, bn[3] = { 0, 0, 0 }, accn[3] = { 0, 0, 0 };
    mat_apply(sm->bind_matrix.e, pos + 3 * i, bv);
    if (nrm) mat_dir(sm->bind_matrix.e, nrm + 3 * i, bn);
    for (int k = 0; k < 4; k++) {
      float w = wi[4 * i + k];
      int j = (int)ji[4 * i + k];
      if (w == 0 || j < 0 || j >= sk->bone_count) continue;
      float t[3];
      mat_apply(bm[j].e, bv, t);
      acc[0] += t[0] * w;
      acc[1] += t[1] * w;
      acc[2] += t[2] * w;
      if (nrm) {
        mat_dir(bm[j].e, bn, t);
        accn[0] += t[0] * w;
        accn[1] += t[1] * w;
        accn[2] += t[2] * w;
      }
    }
    mat_apply(a.e, acc, posed + 3 * i);
    if (posed_nrm) mat_dir(a.e, accn, posed_nrm + 3 * i);
  }
  cw_free(bm);
}

/* a source box entirely outside one of the decal box's planes (rigid meshes) */
static bool source_outside(t3_mesh *m, const t3_mat4 *world_to_box) {
  t3_geometry *g = m->geometry;
  if (!g->has_bounding_box) t3_geometry_compute_bounding_box(g);
  t3_object_update_world_matrix(m, true, false);
  t3_mat4 mb;
  t3_mat4_multiply(&mb, world_to_box, &m->base.matrix_world);
  uint8_t all = 0x3f;
  for (int i = 0; i < 8; i++) {
    float c[3] = { i & 1 ? g->bounding_box.max.x : g->bounding_box.min.x, i & 2 ? g->bounding_box.max.y : g->bounding_box.min.y,
                   i & 4 ? g->bounding_box.max.z : g->bounding_box.min.z };
    float l[3];
    mat_apply(mb.e, c, l);
    uint8_t code = 0;
    for (int k = 0; k < 3; k++) {
      if (l[k] > 0.5f) code |= (uint8_t)(1u << (2 * k));
      if (l[k] < -0.5f) code |= (uint8_t)(2u << (2 * k));
    }
    all &= code;
  }
  return all != 0;
}

/* a new piece: the clipped slice [v0, v1) / [i0, i1) of out, under src */
static void add_piece(decal3d *d, t3_mesh *src, bool skinned, const cw__decal_out *out, int v0, int i0) {
  int nv = out->pos.count / 3 - v0, ni = out->index.count - i0;
  t3_geometry *g = t3_geometry_new();
  t3_attribute *a = t3_attribute_new(T3_FLOAT32, out->pos.data + 3 * v0, nv, 3);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, a);
  t3_release(a);
  a = t3_attribute_new(T3_FLOAT32, out->uv.data + 2 * v0, nv, 2);
  t3_geometry_set_attribute(g, T3_ATTR_UV, a);
  t3_release(a);
  a = t3_attribute_new(T3_FLOAT32, out->nrm.data + 3 * v0, nv, 3);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, a);
  t3_release(a);
  /* color.x: the fade the CPU baked (normal_fade, edge_fade); the material
   * does not use vertex colours, the meshdecal program reads it itself */
  float *f3 = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  for (int i = 0; i < nv; i++) {
    f3[3 * i] = out->fade.data[v0 + i];
    f3[3 * i + 1] = f3[3 * i + 2] = 0;
  }
  a = t3_attribute_new(T3_FLOAT32, f3, nv, 3);
  cw_free(f3);
  t3_geometry_set_attribute(g, T3_ATTR_COLOR, a);
  t3_release(a);
  if (skinned) {
    a = t3_attribute_new(T3_FLOAT32, out->joints.data + 4 * v0, nv, 4);
    t3_geometry_set_attribute(g, T3_ATTR_SKIN_INDEX, a);
    t3_release(a);
    a = t3_attribute_new(T3_FLOAT32, out->weights.data + 4 * v0, nv, 4);
    t3_geometry_set_attribute(g, T3_ATTR_SKIN_WEIGHT, a);
    t3_release(a);
  }
  uint32_t *ix = cw_alloc(sizeof(uint32_t) * (size_t)ni, CW_MEM_RENDER);
  for (int i = 0; i < ni; i++) ix[i] = out->index.data[i0 + i] - (uint32_t)v0;
  a = t3_attribute_new(T3_UINT32, ix, ni, 1);
  cw_free(ix);
  t3_geometry_set_index(g, a);
  t3_release(a);
  /* the source's bound: culled with it, and (origin and bound shared) sorted
   * right after it among transparent draws, by id */
  if (!src->geometry->has_bounding_sphere) t3_geometry_compute_bounding_sphere(src->geometry);
  g->bounding_sphere = src->geometry->bounding_sphere;
  g->has_bounding_sphere = true;
  t3_mesh *pm;
  if (skinned) {
    t3_skinned_mesh *ss = (t3_skinned_mesh *)src;
    t3_skinned_mesh *k = t3_skinned_mesh_new(g, d->mesh.mat);
    k->bind_mode = ss->bind_mode;
    t3_skinned_mesh_bind(k, ss->skeleton, &ss->bind_matrix);
    pm = &k->mesh;
  } else {
    pm = t3_mesh_new(g, d->mesh.mat);
  }
  t3_release(g);
  t3_object *po = &pm->base;
  po->name = DECAL_PIECE;
  po->cast_shadow = po->receive_shadow = false;
  po->frustum_culled = src->base.frustum_culled;
  po->render_order = d->priority;
  if (po->layers != src->base.layers) t3_object_set_layers(po, src->base.layers);
  t3_object_add(&src->base, po);
  cw_array_push(&d->mesh.pieces, po); /* keeps the reference _new gave */
}

static void mesh_snapshot(cw_node n, decal3d *d) {
  const cw_node3d *nd = cw_node_data(n, "Node3D");
  if (!nd) return;
  d->mesh.at_pos = nd->position;
  d->mesh.at_quat = nd->quaternion;
  d->mesh.at_scale = nd->scale;
}

static bool mesh_moved(cw_node n, const decal3d *d) {
  const cw_node3d *nd = cw_node_data(n, "Node3D");
  return nd && (memcmp(&nd->position, &d->mesh.at_pos, sizeof nd->position) ||
                memcmp(&nd->quaternion, &d->mesh.at_quat, sizeof nd->quaternion) ||
                memcmp(&nd->scale, &d->mesh.at_scale, sizeof nd->scale));
}

static void mesh_build(cw_tree *t, decal_tree *dt, cw_node n, decal3d *d) {
  double t0 = cw__render3d_now_ms();
  mesh_clear(d);
  d->mesh.dirty = false;
  d->mesh.built = true; /* (also when nothing comes of it: no retry every frame) */
  d->mesh.build_ms = 0;
  dt->mesh_builds++;
  mesh_snapshot(n, d);
  t3_object *o = cw_node3d_object(n);
  if (!o) return;
  t3_object_update_world_matrix(o, true, false);
  t3_mat4 s, w, inv;
  t3_mat4_identity(&s);
  s.e[0] = d->size.x;
  s.e[5] = d->size.y;
  s.e[10] = d->size.z;
  t3_mat4_multiply(&w, &o->matrix_world, &s);
  if (!(d->size.x > 0 && d->size.y > 0 && d->size.z > 0)) return;
  t3_mat4_invert(&inv, &w);
  /* the sources */
  mesh_list srcs = { 0 };
  if (d->target && *d->target) {
    cw_node tn = cw_node_get(n, d->target);
    t3_object *to = cw_node_valid(tn) ? cw_node3d_object(tn) : NULL;
    if (to) collect_sources(to, &srcs);
    /* (a mesh resource still loading has nothing yet: try again next frame) */
    if (!srcs.count) d->mesh.dirty = true;
  } else {
    collect_sources(&cw_render3d_scene(t)->base, &srcs);
  }
  /* the image and its region as uv (three.js textures flip Y: v = 1 is the
   * image's top row, which lands at the box's +Y like the screen-space pass) */
  mesh_tex *mt = mesh_texture(dt, d->texture);
  if (!d->mesh.mat) {
    d->mesh.mat = t3_mesh_basic_material_new(0xffffff);
    d->mesh.mat->transparent = true;
    d->mesh.mat->depth_write = false;
    d->mesh.mat->fog = false;
    t3_material_set_extension(d->mesh.mat, "meshdecal");
  }
  t3_material_set_map(d->mesh.mat, mt->tex);
  mesh_material_apply(d);
  float iw = mt->w > 0 ? (float)mt->w : 1, ih = mt->h > 0 ? (float)mt->h : 1;
  float rx = 0, ry = 0, rw = iw, rh = ih;
  if (d->region[2] > 0 && d->region[3] > 0) {
    rx = d->region[0];
    ry = d->region[1];
    rw = d->region[2];
    rh = d->region[3];
  }
  cw__decal_clip_params p = { .min_facing = 0, .normal_fade = CW_CLAMP(d->normal_fade, 0, 0.999f),
                              .edge_fade = CW_CLAMP(d->edge_fade, 0, 0.5f), .max_tris = CW_MAX(d->max_triangles, 1),
                              .uv_rect = { rx / iw, 1 - (ry + rh) / ih, (rx + rw) / iw, 1 - ry / ih } };
  memcpy(p.world_to_box, inv.e, sizeof p.world_to_box);
  float ax = w.e[8], ay = w.e[9], az = w.e[10], al = sqrtf(ax * ax + ay * ay + az * az);
  if (al > 0) {
    p.axis[0] = ax / al;
    p.axis[1] = ay / al;
    p.axis[2] = az / al;
  }
  cw__decal_out out = { 0 };
  for (int i = 0; i < srcs.count && !out.truncated; i++) {
    t3_mesh *m = srcs.data[i];
    const t3_geometry *g = m->geometry;
    int vc = g->attributes[T3_ATTR_POSITION]->count;
    const t3_attribute *na = g->attributes[T3_ATTR_NORMAL];
    if (na && (na->type != T3_FLOAT32 || na->item_size != 3 || na->count < vc)) na = NULL;
    const t3_attribute *ja = g->attributes[T3_ATTR_SKIN_INDEX], *wa = g->attributes[T3_ATTR_SKIN_WEIGHT];
    bool skinned = m->base.type == T3_SKINNED_MESH && ((t3_skinned_mesh *)m)->skeleton &&
                   ((t3_skinned_mesh *)m)->skeleton->bone_count > 0 && attr4(ja, vc) && attr4(wa, vc);
    if (!skinned && source_outside(m, &inv)) continue;
    float *posed = cw_alloc(sizeof(float) * 3 * (size_t)vc, CW_MEM_RENDER);
    float *posed_nrm = na ? cw_alloc(sizeof(float) * 3 * (size_t)vc, CW_MEM_RENDER) : NULL;
    pose_source(m, skinned, posed, posed_nrm);
    /* (the geometry's scale to world turns the offset into geometry units) */
    t3_mat4 gw = m->base.matrix_world;
    if (skinned) t3_mat4_multiply(&gw, &m->base.matrix_world, &((t3_skinned_mesh *)m)->bind_matrix);
    p.offset = d->mesh_offset;
    for (int c = 0; c < 3; c++)
      for (int r = 0; r < 3; r++) p.geo_to_world[3 * c + r] = gw.e[4 * c + r];
    cw__decal_src src = {
      .pos = g->attributes[T3_ATTR_POSITION]->array, .nrm = na ? na->array : NULL,
      .joints = skinned ? ja->array : NULL, .weights = skinned ? wa->array : NULL, .vertex_count = vc,
      .index = g->index ? g->index->array : NULL, .index16 = g->index && g->index->type == T3_UINT16,
      .index_count = g->index ? g->index->count : 0, .posed = posed, .posed_nrm = posed_nrm,
    };
    int v0 = out.pos.count / 3, i0 = out.index.count;
    if (cw__decal_clip(&src, &p, &out) > 0) {
      add_piece(d, m, skinned, &out, v0, i0);
      d->mesh.sources++;
    }
    cw_free(posed);
    cw_free(posed_nrm);
  }
  cw_array_free(&srcs);
  if (out.truncated)
    cw_warn("Decal3D '%s': clipped to max_triangles (%d); raise it or make the box smaller", cw_node_name(n),
            d->max_triangles);
  d->mesh.tris = out.tris;
  d->mesh.truncated = out.truncated;
  d->mesh.hash = cw__decal_out_hash(&out, 0);
  cw__decal_out_free(&out);
  d->mesh.build_ms = (float)(cw__render3d_now_ms() - t0);
}

/* pre_render, after every node's (order 1000): build what is new, changed or
 * moved, then recycle the oldest past the tree's limit */
static void mesh_hook(cw_tree *t, double unused, void *user) {
  (void)unused;
  (void)user;
  decal_tree *dt = cw_tree_slot(t, "decals");
  if (!dt) return;
  for (int i = 0; i < dt->decals.count; i++) {
    cw_node n = dt->decals.data[i];
    decal3d *d = cw_node_data_t(n, decal_t);
    if (!d) continue;
    if (d->mode != DM_MESH) {
      if (d->mesh.built) {
        mesh_clear(d);
        order_remove(dt, n, d);
      }
      continue;
    }
    if (!d->mesh.dirty && d->mesh.built && mesh_moved(n, d)) d->mesh.dirty = true;
    if (!d->mesh.dirty && d->mesh.built) continue;
    mesh_build(t, dt, n, d);
    if (d->mesh.tris > 0 && d->max_mesh_decals > 0 && !d->mesh.in_order) {
      cw_array_push(&dt->mesh_order, n);
      d->mesh.in_order = true;
      d->mesh.seq = ++dt->mesh_seq;
      dt->mesh_limit = CW_MAX(d->max_mesh_decals, 1);
    } else if (d->mesh.tris == 0 || d->max_mesh_decals <= 0) {
      order_remove(dt, n, d);
    }
  }
  /* oldest first: the decal node is freed (bullet holes, footprints) */
  while (dt->mesh_order.count > dt->mesh_limit) {
    cw_node old = dt->mesh_order.data[0];
    decal3d *d = cw_node_data_t(old, decal_t);
    if (d) {
      order_remove(dt, old, d);
      mesh_clear(d);
    } else {
      memmove(dt->mesh_order.data, dt->mesh_order.data + 1, sizeof(cw_node) * (size_t)(dt->mesh_order.count - 1));
      dt->mesh_order.count--;
    }
    cw_node_free(old);
    dt->mesh_recycled++;
  }
}

/* ── the node ───────────────────────────────────────────────────────── */

static void decal_init(cw_node n, void *data) {
  (void)n;
  decal3d *d = data;
  d->size = (t3_vec3){ 1, 1, 1 };
  d->texture = cw_intern("");
  d->color[0] = d->color[1] = d->color[2] = d->color[3] = 1;
  d->energy = 1;
  d->normal_fade = 0.2f;
  d->edge_fade = 0.1f;
  d->target = cw_intern("");
  d->max_triangles = 1024;
  d->mesh_offset = 0.002f;
}

static void decal_enter(cw_node n, void *data) {
  decal3d *d = data;
  cw_tree *t = cw_node_tree(n);
  decal_tree *dt = dtree(t);
  cw_array_push(&dt->decals, n);
  d->mesh.dirty = true;
  if (!dt->hook_added) {
    cw_tree_add_hook(t, 2, 1000, mesh_hook, NULL);
    dt->hook_added = true;
    dt->mesh_limit = 128;
  }
  if (!dt->pass_added) {
    cw_pipeline *p = cw_pipeline_of(t);
    if (!cw_pipeline_has_pass(p, "decals"))
      cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "decals", .phase = CW_PHASE_EFFECTS, .order = 100,
                                               .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                               .active = decals_active, .execute = decals_exec });
    dt->pass_added = true;
  }
}

static void decal_exit(cw_node n, void *data) {
  decal3d *d = data;
  cw_tree *t = cw_node_tree(n);
  mesh_clear(d);
  if (t) {
    order_remove(dtree(t), n, d);
    cw__fx_remove_node(&dtree(t)->decals, n);
  }
}

static void decal_destroy(cw_node n, void *data) {
  (void)n;
  decal3d *d = data;
  mesh_clear(d);
  cw_array_free(&d->mesh.pieces);
  if (d->mesh.mat) t3_release(d->mesh.mat);
  d->mesh.mat = NULL;
}

/* mesh mode: colour, energy and blend change the material; priority the
 * draw order; anything else that shapes the geometry builds it again */
static void decal_prop_changed(cw_node n, void *data, const cw_prop *p) {
  decal3d *d = data;
  if (!strcmp(p->name, "color") || !strcmp(p->name, "energy") || !strcmp(p->name, "blend")) {
    mesh_material_apply(d);
    return;
  }
  if (!strcmp(p->name, "priority")) {
    for (int i = 0; i < d->mesh.pieces.count; i++) d->mesh.pieces.data[i]->render_order = d->priority;
    return;
  }
  if (!strcmp(p->name, "max_mesh_decals")) {
    if (d->max_mesh_decals <= 0 && d->mesh.in_order) {
      cw_tree *t = cw_node_tree(n);
      if (t) order_remove(dtree(t), n, d);
    }
    return;
  }
  d->mesh.dirty = true;
}

bool cw_decal_get_mesh_info(cw_node decal, cw_decal_mesh_info *out) {
  memset(out, 0, sizeof *out);
  decal3d *d = decal_t ? cw_node_data_t(decal, decal_t) : NULL;
  if (!d || d->mode != DM_MESH) return false;
  out->built = d->mesh.built;
  out->triangles = d->mesh.tris;
  out->pieces = d->mesh.pieces.count;
  out->sources = d->mesh.sources;
  out->truncated = d->mesh.truncated;
  out->hash = d->mesh.hash;
  out->build_ms = d->mesh.build_ms;
  return true;
}

void cw_decal_restamp(cw_node decal) {
  decal3d *d = decal_t ? cw_node_data_t(decal, decal_t) : NULL;
  if (d) d->mesh.dirty = true;
}

static const cw_prop decal_props[] = {
  CW_PROP_FIELD(decal3d, size, CW_T_VEC3, CW_PROP_DEFAULT, "Box size: x width, y height, z projection depth."),
  CW_PROP_FIELD(decal3d, texture, CW_T_STRING, CW_PROP_DEFAULT, "Image asset (empty = a soft round splat)."),
  CW_PROP_FIELD(decal3d, region, CW_T_VEC4, CW_PROP_DEFAULT, "x y w h in image pixels (0 = the whole image)."),
  CW_PROP_FIELD(decal3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Tint; alpha = opacity."),
  CW_PROP_FIELD_RANGE(decal3d, energy, CW_T_FLOAT, 0, 1e6, "ADD: brightness."),
  { .name = "blend", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(decal3d, blend),
    .size = sizeof(int), .enum_names = dblend_names, .doc = "MIX (over, unlit), MULTIPLY (darkens, lit) or ADD (glow)." },
  CW_PROP_FIELD_RANGE(decal3d, normal_fade, CW_T_FLOAT, 0, 1, "Fade on surfaces turned away from the projection axis."),
  CW_PROP_FIELD_RANGE(decal3d, edge_fade, CW_T_FLOAT, 0, 0.5, "Fraction of the depth fading out at the box ends."),
  CW_PROP_FIELD(decal3d, priority, CW_T_INT, CW_PROP_DEFAULT, "Draw order (higher on top)."),
  { .name = "mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(decal3d, mode), .size = sizeof(int),
    .enum_names = dmode_names,
    .doc = "SCREEN (projected on the scene's depth each frame) or MESH (clipped from the target meshes once: lands on "
           "characters and transparent surfaces, follows them)." },
  CW_PROP_FIELD(decal3d, target, CW_T_STRING, CW_PROP_DEFAULT,
                "MESH: node path from the decal to the node to stick to (it and everything under it); empty = every mesh "
                "the box overlaps."),
  CW_PROP_FIELD_RANGE(decal3d, max_triangles, CW_T_INT, 1, 1000000, "MESH: the most triangles one decal keeps."),
  CW_PROP_FIELD_RANGE(decal3d, mesh_offset, CW_T_FLOAT, 0, 1, "MESH: metres the decal floats off the surface."),
  CW_PROP_FIELD_RANGE(decal3d, max_mesh_decals, CW_T_INT, 0, 1000000,
                      "MESH: above 0 the decal joins the tree's pool, which keeps this many (the newest one's value "
                      "counts) and frees the oldest past it; 0 = never recycled."),
  CW_PROP_END,
};

static cw_node_type decal_type = {
  .name = "Decal3D", .parent = "Node3D", .data_size = sizeof(decal3d), .props = decal_props,
  .doc = "A box-projected image on the scene's geometry, or (mode MESH) clipped onto the meshes it lands on (decals.h).",
  .init = decal_init,
  .enter_tree = decal_enter, .exit_tree = decal_exit, .destroy = decal_destroy, .prop_changed = decal_prop_changed,
};

void cw__register_decal_types(void) {
  cw_node_type_register(&decal_type);
  decal_t = &decal_type;
}

/* A 2D render target as a three.c texture (cartwheel/render3d.h, cw_target_texture3d): a card face, a sign, a
 * screen drawn with the 2D renderer once and worn by any number of materials, with no SubViewport per face.
 *
 * The target holds display-encoded colour, premultiplied, in the 2D renderer's row order. three.c samples an
 * embedder's texture as it is, so each target that has a 3D texture keeps a linear copy (RGBA16F where the host
 * renders half floats, else RGBA8): one fullscreen pass divides the alpha out, decodes the sRGB curve and keeps the
 * alpha, at the end of every 2D pass into the target (cw_r2d_end calls back) and on cw_target_texture3d_refresh.
 * Rows go the way three.c expects for a texture's v (v = 0 at the picture's bottom), whichever renderer drew them. */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "pipeline_internal.h"
#include "pipeline_wgpu.h"

void *cw__r2d_target_wgpu_texture(cw_texture h); /* src/render2d/texture.c */
unsigned cw__r2d_target_gl_array(cw_texture h);
bool cw__r2d_target_rows_up(cw_texture h);
extern void (*cw__r2d_target_drawn_hook)(cw_texture t);   /* src/render2d/batch.c: after a 2D pass into a target */
extern void (*cw__r2d_target_freed_hook)(cw_texture t);   /* src/render2d/texture.c: cw_texture_destroy */

typedef struct tt_entry {
  cw_texture target;
  int w, h;
  cw_rt *lin;        /* the linear copy three.c samples */
  t3_texture *t3;    /* three.c's texture over lin */
  void *src_tex;     /* WebGPU: the target's texture the registration below is for (a resize makes a new one) */
  unsigned src_id;   /* WebGPU: the target registered for the pass to sample */
  int32_t refreshes;
} tt_entry;

static CW_ARRAY(tt_entry) entries;
static int32_t dbg_refreshes, dbg_targets;
static bool hooked;

static bool wgpu(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }

static tt_entry *find(cw_texture t) {
  for (int i = 0; i < entries.count; i++)
    if (entries.data[i].target.id == t.id) return &entries.data[i];
  return NULL;
}

static void src_drop(tt_entry *e) {
#if CW_WGPU
  if (e->src_id) cw__pw_unregister(e->src_id);
#endif
  e->src_id = 0;
  e->src_tex = NULL;
}

/* the linear copy at the target's size, three.c's texture pointed at it */
static bool ensure_copy(tt_entry *e) {
  int w = 0, h = 0;
  cw_texture_size(e->target, &w, &h);
  if (w <= 0 || h <= 0) return false;
  if (e->lin && e->w == w && e->h == h) return true;
  cw_rt *old = e->lin;
  e->lin = cw_rt_new(w, h, cw_pipeline_caps_get()->float_render ? CW_RT_RGBA16F : CW_RT_RGBA8, false);
  if (!e->lin) {
    e->lin = old;
    return false;
  }
  e->w = w, e->h = h;
#if CW_WGPU
  if (wgpu()) {
    if (e->t3) t3_texture_adopt_wgpu(e->t3, cw__pw_texture(e->lin->tex), cw__pw_view(e->lin->tex), w, h, 1, false);
    else e->t3 = t3_texture_from_wgpu(cw__pw_texture(e->lin->tex), cw__pw_view(e->lin->tex), w, h, 1, false, false);
  } else
#endif
  {
    if (e->t3) t3_texture_adopt_gl(e->t3, e->lin->tex, w, h, 1, false);
    else e->t3 = t3_texture_from_gl(e->lin->tex, w, h, 1, false, false);
  }
  cw_rt_free(old);
  return e->t3 != NULL;
}

/* the decode pass: unpremultiply, sRGB to linear, alpha kept; `flip` for rows three.c drew top-down */
static unsigned program(bool flip) {
  static unsigned prog[2];
  if (prog[flip]) return prog[flip];
  const char *glsl = flip
    ? "uniform highp sampler2DArray u_src;\nout vec4 o;\n"
      "vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
      "void main() { vec4 c = texture(u_src, vec3(v_uv.x, 1.0 - v_uv.y, 0.0));\n"
      "  o = vec4(lin(c.a > 0.0 ? clamp(c.rgb / c.a, 0.0, 1.0) : vec3(0.0)), c.a); }\n"
    : "uniform highp sampler2DArray u_src;\nout vec4 o;\n"
      "vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
      "void main() { vec4 c = texture(u_src, vec3(v_uv, 0.0));\n"
      "  o = vec4(lin(c.a > 0.0 ? clamp(c.rgb / c.a, 0.0, 1.0) : vec3(0.0)), c.a); }\n";
  const char *wgsl = flip
    ? "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
      "fn lin(c: vec3f) -> vec3f { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3f(2.4)), step(vec3f(0.04045), c)); }\n"
      "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
      "  let c = textureSampleLevel(u_src, cw_linear, vec2f(uv.x, 1.0 - uv.y), 0.0);\n"
      "  var s = vec3f(0.0);\n  if (c.a > 0.0) { s = clamp(c.rgb / c.a, vec3f(0.0), vec3f(1.0)); }\n"
      "  return vec4f(lin(s), c.a);\n}\n"
    : "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
      "fn lin(c: vec3f) -> vec3f { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3f(2.4)), step(vec3f(0.04045), c)); }\n"
      "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
      "  let c = textureSampleLevel(u_src, cw_linear, uv, 0.0);\n"
      "  var s = vec3f(0.0);\n  if (c.a > 0.0) { s = clamp(c.rgb / c.a, vec3f(0.0), vec3f(1.0)); }\n"
      "  return vec4f(lin(s), c.a);\n}\n";
  prog[flip] = cw__pass_program2(flip ? "target_texture3d.decode_flip" : "target_texture3d.decode", glsl, wgsl);
  return prog[flip];
}

static void decode(tt_entry *e) {
  if (!ensure_copy(e)) return;
  bool flip = !cw__r2d_target_rows_up(e->target);
  unsigned prog = program(flip);
  if (!prog) return;
  cw_tree *tree = cw_current_tree();
  cw_pass_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.tree = tree;
  ctx.pipeline = tree ? cw_pipeline_of(tree) : NULL;
  ctx.width = e->w;
  ctx.height = e->h;
  ctx.output = e->lin;
  cw_pass_bind_target(&ctx, e->lin);
  cw__pass_state_reset();
  cw_pass_use(prog);
#if CW_WGPU
  if (wgpu()) {
    WGPUTexture tex = cw__r2d_target_wgpu_texture(e->target);
    if (!tex) return;
    if (e->src_tex != (void *)tex) { /* new storage (a resize): register it again */
      src_drop(e);
      WGPUTextureView view = wgpuTextureCreateView(tex, &(WGPUTextureViewDescriptor){
        .format = wgpuTextureGetFormat(tex), .dimension = WGPUTextureViewDimension_2D, .mipLevelCount = 1, .arrayLayerCount = 1,
        .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_TextureBinding });
      e->src_id = cw__pw_register(tex, view);
      e->src_tex = tex;
    }
    cw_pass_bind_texture(0, e->src_id);
    cw_pass_fullscreen();
    cw__pw_flush();
    e->refreshes++, dbg_refreshes++;
    return;
  }
#endif
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, cw__r2d_target_gl_array(e->target));
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_fullscreen();
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, cw__gl_window_fbo);
  t3_renderer *mr = tree ? cw_render3d_renderer(tree) : NULL;
  if (mr) t3_renderer_reset_bindings(mr);
  e->refreshes++, dbg_refreshes++;
}

static void drawn(cw_texture t) {
  tt_entry *e = find(t);
  if (e) decode(e);
}

static void freed(cw_texture t) {
  tt_entry *e = find(t);
  if (!e) return;
  src_drop(e);
  if (e->t3) t3_release(e->t3);
  cw_rt_free(e->lin);
  *e = entries.data[--entries.count];
  dbg_targets = entries.count;
}

t3_texture *cw_target_texture3d(cw_texture target) {
  if (!cw__r2d_target_gl_array(target) && !cw__r2d_target_wgpu_texture(target)) {
    cw_error("cw_target_texture3d: not a live render target (cw_target_create)");
    return NULL;
  }
  if (!hooked) {
    hooked = true;
    cw__r2d_target_drawn_hook = drawn;
    cw__r2d_target_freed_hook = freed;
    cw_debug_i32("render.target_texture3d.refreshes", &dbg_refreshes);
    cw_debug_i32("render.target_texture3d.targets", &dbg_targets);
  }
  tt_entry *e = find(target);
  if (!e) {
    cw_array_push(&entries, ((tt_entry){ .target = target }));
    e = &entries.data[entries.count - 1];
    dbg_targets = entries.count;
    decode(e); /* what the target holds now */
  } else if (!ensure_copy(e)) return NULL;
  return e->t3;
}

void cw_target_texture3d_refresh(cw_texture target) {
  tt_entry *e = find(target);
  if (e) decode(e);
}

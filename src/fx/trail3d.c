/* Trail3D: a camera-facing ribbon through the node's last positions (one
 * sample per fixed tick; the head follows the rendered position). */
#include "cartwheel/gpu.h"
#include <math.h>
#include <string.h>

#include "fx_internal.h"
#include "fx_wgpu.h"

#define TRAIL3D_MAX 1024
#define RAMP_N 64

enum { TB_ADD, TB_ALPHA };
static const char *const tblend_names[] = { "ADD", "ALPHA", NULL };

typedef struct trail3d {
  int length;
  float width;
  float color[4];
  const char *color_ramp;
  float energy;
  int blend;
  bool emitting;
  /* runtime */
  t3_vec3 *pts;
  int cap, head, count;
  GLuint vbo;
  int vbo_cap;
  void *wvbo, *wubo; /* WebGPU: vertices, uniforms */
  int wvbo_cap;
  const char *baked;
  float ramp[RAMP_N * 4];
} trail3d;

static const cw_node_type *trail_t;
static GLuint prog;

static const char *const vs =
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec4 a_col;\n"
    "uniform mat4 u_vp;\n"
    "out vec4 v_col;\n"
    "out float v_side;\n"
    "void main() { v_col = a_col; v_side = float(gl_VertexID & 1) * 2.0 - 1.0; gl_Position = u_vp * vec4(a_pos, 1.0); }\n";
static const char *const fs =
    "in vec4 v_col;\n"
    "in float v_side;\n"
    "uniform int u_blend;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  vec4 c = v_col; c.a *= smoothstep(0.0, 0.35, 1.0 - abs(v_side));\n"
    "  frag = vec4(c.rgb * c.a, u_blend == 1 ? c.a : 0.0);\n"
    "}\n";

static void trail_init(cw_node n, void *data) {
  (void)n;
  trail3d *t = data;
  t->length = 32;
  t->width = 0.2f;
  t->color[0] = t->color[1] = t->color[2] = t->color[3] = 1;
  t->color_ramp = cw_intern("");
  t->energy = 1;
  t->emitting = true;
}

static void trail_destroy(cw_node n, void *data) {
  (void)n;
  trail3d *t = data;
  cw_free(t->pts);
  t->pts = NULL;
  if (t->vbo) glDeleteBuffers(1, &t->vbo);
  t->vbo = 0;
#if CW_WGPU
  if (t->wvbo) wgpuBufferRelease(t->wvbo);
  if (t->wubo) wgpuBufferRelease(t->wubo);
  t->wvbo = t->wubo = NULL;
#endif
}

static void trail_enter(cw_node n, void *data) {
  trail3d *t = data;
  t->count = 0;
  cw_tree *tr = cw_node_tree(n);
  cw_array_push(&cw__fx_tree_of(tr)->trails, n);
  cw__fx_ensure_pass(tr);
}

static void trail_exit(cw_node n, void *data) {
  (void)data;
  if (cw_node_tree(n)) cw__fx_remove_node(&cw__fx_tree_of(cw_node_tree(n))->trails, n);
}

static void trail_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  trail3d *t = data;
  int len = CW_CLAMP(t->length, 2, TRAIL3D_MAX);
  if (t->cap != len) {
    cw_free(t->pts);
    t->pts = cw_alloc(sizeof(t3_vec3) * (size_t)len, CW_MEM_RENDER);
    t->cap = len;
    t->head = t->count = 0;
  }
  if (!t->emitting) {
    /* the tail catches up: drop the oldest sample each tick */
    if (t->count > 0) t->count--;
    return;
  }
  t->head = (t->head + 1) % t->cap;
  t->pts[t->head] = cw_node3d_global_position(n);
  if (t->count < t->cap) t->count++;
}

#if CW_WGPU
static const char WGSL_TRAIL3D[] =
    "struct U { vp: mat4x4f, p: vec4f }\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) col: vec4f, @location(1) side: f32 }\n"
    "@vertex fn vs(@builtin(vertex_index) vid: u32, @location(0) p: vec3f, @location(1) c: vec4f) -> VO {\n"
    "  var o: VO; o.col = c; o.side = f32(vid & 1u) * 2.0 - 1.0; o.pos = cw_clip(u.vp * vec4f(p, 1.0), u.p.y); return o; }\n"
    "@fragment fn fs(in: VO) -> @location(0) vec4f {\n"
    "  var c = in.col; c.a *= smoothstep(0.0, 0.35, 1.0 - abs(in.side));\n"
    "  return vec4f(c.rgb * c.a, select(0.0, c.a, i32(u.p.x) == 1)); }\n";

typedef struct tpipe { uint32_t key; WGPURenderPipeline p; } tpipe;
static CW_ARRAY(tpipe) tpipes;
static WGPUBindGroupLayout tbgl;
static WGPUShaderModule tsm;

static WGPURenderPipeline trail_pipe(int blend, const cw__wgpu_pass_targets *tg) {
  uint32_t key = (uint32_t)blend | (uint32_t)tg->flip_y << 1 | (uint32_t)(tg->samples > 1) << 2 |
                 (uint32_t)tg->color_format << 8 | (uint32_t)tg->depth_format << 20;
  for (int i = 0; i < tpipes.count; i++)
    if (tpipes.data[i].key == key) return tpipes.data[i].p;
  if (!tsm) {
    const char *const parts[] = { WGSL_TRAIL3D, NULL };
    tsm = cw__wg_module("trail3d", parts);
    const cw__wg_slot sl = { CW_WB_UNIFORM, WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, 0, 80 };
    tbgl = cw__wg_layout(&sl, 1);
  }
  WGPUVertexAttribute at[2] = { { .format = WGPUVertexFormat_Float32x3, .offset = 0, .shaderLocation = 0 },
                                { .format = WGPUVertexFormat_Float32x4, .offset = 12, .shaderLocation = 1 } };
  WGPUVertexBufferLayout vb = { .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 28, .attributeCount = 2, .attributes = at };
  WGPUColorTargetState ct = { .format = tg->color_format, .blend = cw__wg_blend(blend == TB_ADD ? CW_WG_BLEND_ADD : CW_WG_BLEND_PREMUL),
                              .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = tsm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
  WGPUDepthStencilState ds = { .format = tg->depth_format, .depthWriteEnabled = WGPUOptionalBool_False,
                               .depthCompare = cw_gpu_reversed_depth() ? WGPUCompareFunction_GreaterEqual : WGPUCompareFunction_LessEqual,
                               .stencilFront = { .compare = WGPUCompareFunction_Always },
                               .stencilBack = { .compare = WGPUCompareFunction_Always } };
  WGPURenderPipelineDescriptor d = {
    .label = CW_WGPU_SV("trail3d"), .layout = cw__wg_pipeline_layout(&tbgl, 1),
    .vertex = { .module = tsm, .entryPoint = CW_WGPU_SV("vs"), .bufferCount = 1, .buffers = &vb },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleStrip, .cullMode = WGPUCullMode_None },
    .depthStencil = tg->depth ? &ds : NULL,
    .multisample = { .count = (uint32_t)(tg->samples > 1 ? tg->samples : 1), .mask = 0xffffffffu },
    .fragment = &fs,
  };
  WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
  wgpuPipelineLayoutRelease(d.layout);
  cw_array_push(&tpipes, ((tpipe){ key, p }));
  return p;
}

static void draw_wgpu(trail3d *t, cw_pass_ctx *ctx, const float *v, int nv) {
  const cw__wgpu_pass_targets *tg = cw__fx_pass_targets(ctx);
  if (!tg || !tg->enc) return;
  WGPURenderPipeline pipe = trail_pipe(t->blend, tg);
  if (!pipe) return;
  if (t->wvbo_cap < nv) {
    if (t->wvbo) wgpuBufferRelease(t->wvbo);
    t->wvbo_cap = nv + 64;
    t->wvbo = cw__wg_buffer(sizeof(float) * 7 * (size_t)t->wvbo_cap, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "trail3d");
  }
  if (!t->wubo) t->wubo = cw__wg_buffer(80, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "trail3d u");
  cw__wg_write(t->wvbo, 0, v, sizeof(float) * 7 * (size_t)nv);
  float u[20] = { 0 };
  t3_mat4 vp;
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &ctx->view);
  memcpy(u, vp.e, 64);
  u[16] = (float)t->blend;
  u[17] = tg->flip_y ? -1.0f : 1.0f;
  cw__wg_write(t->wubo, 0, u, sizeof u);
  cw__wg_entry e = { .buffer = t->wubo };
  WGPUBindGroup bg = cw__wg_group(tbgl, &e, 1);
  WGPURenderPassEncoder rp = cw__wg_scene_pass(tg, true, "trail3d");
  wgpuRenderPassEncoderSetPipeline(rp, pipe);
  wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, NULL);
  wgpuRenderPassEncoderSetVertexBuffer(rp, 0, t->wvbo, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(rp, (uint32_t)nv, 1, 0, 0);
  wgpuRenderPassEncoderEnd(rp);
  wgpuRenderPassEncoderRelease(rp);
  wgpuBindGroupRelease(bg);
}
#endif

void cw__trail3d_draw(cw_node n, cw_pass_ctx *ctx, cw__fx_tree *ft) {
  trail3d *t = cw_node_data_t(n, trail_t);
  if (!t || t->count < 2 || !ctx->camera) return;
  if (!prog && !cw__fx_wgpu_on()) {
    prog = cw__fx_program("trail3d", vs, fs, NULL, 0);
    if (!prog) return;
  }
  if (t->baked != t->color_ramp) {
    cw__fx_curve_bake(t->color_ramp, 4, 1, RAMP_N, t->ramp, "Trail3D.color_ramp");
    t->baked = t->color_ramp;
  }
  int nv = 2 * t->count;
  float *v = cw_frame_alloc(sizeof(float) * 7 * (size_t)nv);
  t3_vec3 cam = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  /* the head is the rendered position (interpolated), older samples are ticks */
  t3_object *o = cw_node3d_object(n);
  t3_object_update_world_matrix(o, true, false);
  t3_vec3 headp = t3_mat4_get_position(&o->matrix_world);
  for (int j = 0; j < t->count; j++) {
    t3_vec3 p = j == 0 ? headp : t->pts[(t->head - j + t->cap) % t->cap];
    int ja = j > 0 ? j - 1 : 0, jb = j + 1 < t->count ? j + 1 : t->count - 1;
    t3_vec3 a = ja == 0 ? headp : t->pts[(t->head - ja + t->cap) % t->cap];
    t3_vec3 b = t->pts[(t->head - jb + t->cap) % t->cap];
    t3_vec3 tg = t3_vec3_sub(a, b);
    if (t3_vec3_length_sq(tg) < 1e-12f) tg = (t3_vec3){ 0, 1e-3f, 0 };
    t3_vec3 sd = t3_vec3_cross(t3_vec3_normalize(tg), t3_vec3_normalize(t3_vec3_sub(cam, p)));
    sd = t3_vec3_length_sq(sd) > 1e-10f ? t3_vec3_normalize(sd) : (t3_vec3){ 1, 0, 0 };
    float u = (float)j / (float)(t->count - 1);
    int ri = CW_CLAMP((int)(u * (RAMP_N - 1) + 0.5f), 0, RAMP_N - 1);
    const float *rc = t->ramp + 4 * ri;
    float w = t->width * 0.5f * (1 - u);
    for (int s = 0; s < 2; s++) {
      float *q = v + 7 * (2 * j + s);
      float sg = s ? 1.0f : -1.0f;
      q[0] = p.x + sd.x * w * sg;
      q[1] = p.y + sd.y * w * sg;
      q[2] = p.z + sd.z * w * sg;
      q[3] = rc[0] * t->color[0] * t->energy;
      q[4] = rc[1] * t->color[1] * t->energy;
      q[5] = rc[2] * t->color[2] * t->energy;
      q[6] = rc[3] * t->color[3];
    }
  }
#if CW_WGPU
  if (cw__wgpu_on()) {
    draw_wgpu(t, ctx, v, nv);
    ft->stats.draw_calls++;
    return;
  }
#endif
  if (!t->vbo) glGenBuffers(1, &t->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, t->vbo);
  if (t->vbo_cap < nv) {
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 7 * (size_t)nv), v, GL_DYNAMIC_DRAW);
    t->vbo_cap = nv;
  } else glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(sizeof(float) * 7 * (size_t)nv), v);
  cw_pass_bind_target(ctx, ctx->scene);
  glBindVertexArray(cw__fx_empty_vao());
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, 0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (const void *)12);
  glUseProgram(prog);
  t3_mat4 vp;
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &ctx->view);
  cw__fx_mat4(prog, "u_vp", vp.e);
  cw__fx_u1i(prog, "u_blend", t->blend);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  if (t->blend == TB_ADD) glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
  else glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, nv);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  ft->stats.draw_calls++;
}

static const cw_prop trail_props[] = {
  CW_PROP_FIELD(trail3d, length, CW_T_INT, CW_PROP_DEFAULT, "Samples (one per fixed tick), 2..1024."),
  CW_PROP_FIELD_RANGE(trail3d, width, CW_T_FLOAT, 0, 1e6, "Width at the head (tapers to 0 at the tail)."),
  CW_PROP_FIELD(trail3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Tint."),
  CW_PROP_FIELD(trail3d, color_ramp, CW_T_STRING, CW_PROP_DEFAULT, "Colour along the trail, t = 0 at the head: \"t r g b a, ...\"."),
  CW_PROP_FIELD_RANGE(trail3d, energy, CW_T_FLOAT, 0, 1e6, "Colour multiplier (HDR)."),
  { .name = "blend", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(trail3d, blend),
    .size = sizeof(int), .enum_names = tblend_names, .doc = "ADD or ALPHA (premultiplied)." },
  CW_PROP_FIELD(trail3d, emitting, CW_T_BOOL, CW_PROP_DEFAULT, "Recording; off lets the tail shrink away."),
  CW_PROP_END,
};

static cw_node_type trail_type = {
  .name = "Trail3D", .parent = "Node3D", .data_size = sizeof(trail3d), .props = trail_props,
  .doc = "A ribbon following the node (swords, projectiles); drawn in the particles pass.",
  .init = trail_init, .destroy = trail_destroy, .enter_tree = trail_enter, .exit_tree = trail_exit,
  .fixed_update = trail_fixed,
};

void cw__register_trail3d_types(void) {
  cw_node_type_register(&trail_type);
  trail_t = &trail_type;
}

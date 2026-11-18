/* overlay_full_res: a stylized post look's frame. The 3D scene (scene3d's
 * level) renders at a quarter of the output (cw_quality_scene_scale(0.25),
 * below the Resolution setting's 50% floor), and an OVERLAY pass of this app
 * ("pattern", order -200: before the 2D canvas pass) draws a one-pixel
 * checker over the middle half of the output with raw pass code
 * (cw_pass_bind_target(ctx, NULL), cw_pass_fullscreen). The overlay phase
 * starts on the output at its full size, so the checker alternates pixel by
 * pixel in the captured frame (gate overlay-full-res).
 *
 * Debug fields: overlay.size (ctx->width, ctx->height and ctx->output's size
 * as the pattern pass saw them), render.size (cw_render_size), scene.scale
 * (cw_quality_scene_scale_get x 1000). Debug command: scene_scale <s> (0
 * gives the scene back to the Resolution setting). */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/scene_file.h"

static int32_t overlay_size[4], render_size[2], scene_scale_k;

static const char *const PATTERN_GLSL =
  "uniform vec4 u_rect;\n" /* x0, y0, x1, y1 in the target's pixels */
  "out vec4 o;\n"
  "void main() {\n"
  "  vec2 p = floor(gl_FragCoord.xy);\n"
  "  if (p.x < u_rect.x || p.y < u_rect.y || p.x >= u_rect.z || p.y >= u_rect.w) discard;\n"
  "  float c = mod(p.x + p.y, 2.0);\n"
  "  o = vec4(vec3(c), 1.0);\n"
  "}\n";
static const char *const PATTERN_WGSL =
  "struct U { u_rect: vec4f };\n"
  "@group(0) @binding(0) var<uniform> u: U;\n"
  "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
  "  let p = floor(cw_frag_coord(pos));\n"
  "  if (p.x < u.u_rect.x || p.y < u.u_rect.y || p.x >= u.u_rect.z || p.y >= u.u_rect.w) { discard; }\n"
  "  let c = (p.x + p.y) - 2.0 * floor((p.x + p.y) * 0.5);\n"
  "  return vec4f(vec3f(c), 1.0);\n"
  "}\n";

static void pattern_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  static unsigned prog;
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  if (!prog) prog = wg ? cw_pass_program_wgsl("pattern", PATTERN_WGSL) : cw_pass_program("pattern", PATTERN_GLSL);
  overlay_size[0] = ctx->width;
  overlay_size[1] = ctx->height;
  overlay_size[2] = ctx->output ? ctx->output->w : 0;
  overlay_size[3] = ctx->output ? ctx->output->h : 0;
  if (!prog) return;
  cw_pass_bind_target(ctx, NULL); /* the output, as raw pass code binds it */
  cw_pass_use(prog);
  int w = ctx->output->w, h = ctx->output->h;
  cw_pass_uniform4f(prog, "u_rect", (float)(w / 4), (float)(h / 4), (float)(w - w / 4), (float)(h - h / 4));
  cw_pass_fullscreen();
}

static void cmd_scene_scale(int argc, const char **argv, cw_reply *r) {
  float s = argc > 1 ? (float)atof(argv[1]) : 0.25f;
  cw_quality_scene_scale(s);
  cw_reply_printf(r, "{\"scene_scale\":%g}", (double)cw_quality_scene_scale_get());
}

static void init(void *u) {
  (void)u;
  cw_debug_field("overlay.size", overlay_size, CW_DBG_I32, 4);
  cw_debug_field("render.size", render_size, CW_DBG_I32, 2);
  cw_debug_i32("scene.scale", &scene_scale_k);
  cw_debug_command("scene_scale", "scene_scale <s>: the scene's share of the output (0: the Resolution setting)", cmd_scene_scale);
  cw_quality_scene_scale(0.25f);
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  if (!cw_pipeline_has_pass(p, "pattern"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "pattern", .phase = CW_PHASE_OVERLAY, .order = -200,
                                             .execute = pattern_exec, .webgpu = true });
  cw_render_size(&render_size[0], &render_size[1]);
  scene_scale_k = (int32_t)(cw_quality_scene_scale_get() * 1000 + 0.5f);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "overlay_full_res", .width = 1280, .height = 720, .debug = true, .init = init, .update = update };
  return cw_scene_app("main.cwscene", &base);
}

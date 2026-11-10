/* The "particles" EFFECTS pass (GPUParticles3D, Trail3D) and node type
 * registration for the effects module. */
#include <string.h>

#include "fx_internal.h"
#if CW_WGPU
#include "platform/wgpu.h"
#include "render/pipeline_wgpu.h"
#endif

static bool particles_active(const cw_pass_ctx *ctx, void *user) {
  (void)user;
  cw__fx_tree *ft = cw_tree_slot(ctx->tree, "fx");
  return ft && (ft->systems.count || ft->trails.count) && ctx->camera;
}

static void particles_exec(cw_pass_ctx *ctx, void *user) {
  (void)user;
  cw__fx_tree *ft = cw__fx_tree_of(ctx->tree);
  memset(&ft->stats, 0, sizeof ft->stats);
#if CW_WGPU
  /* the particles pass records its own compute and render passes: time them as one span */
  if (cw__wgpu_on()) cw__pw_timing_span((void *)cw__pw_encoder(), false);
#endif
  cw__psys_frames(ctx, ft);
  for (int i = 0; i < ft->trails.count; i++) cw__trail3d_draw(ft->trails.data[i], ctx, ft);
  ft->last_stats = ft->stats;
#if CW_WGPU
  if (cw__wgpu_on()) {
    cw__pw_timing_span((void *)cw__pw_encoder(), true);
    return;
  }
#endif
  glBindVertexArray(0);
  glBindFramebuffer(GL_FRAMEBUFFER, ctx->scene ? ctx->scene->fbo : 0);
}

/* tests: the EFFECTS passes of the module (decals, then particles and
 * trails) on a context the caller built, without the pipeline */
void cw__fx_test_exec(cw_pass_ctx *ctx) {
  void cw__decals_test_exec(cw_pass_ctx *ctx);
  cw__decals_test_exec(ctx);
  particles_exec(ctx, NULL);
}

void cw__fx_mip_box(const uint8_t *src, int w, int h, uint8_t *dst) {
  int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
  for (int y = 0; y < nh; y++)
    for (int x = 0; x < nw; x++) {
      int x0 = CW_MIN(2 * x, w - 1), x1 = CW_MIN(2 * x + 1, w - 1), y0 = CW_MIN(2 * y, h - 1), y1 = CW_MIN(2 * y + 1, h - 1);
      const uint8_t *a = src + 4 * ((size_t)y0 * w + x0), *b = src + 4 * ((size_t)y0 * w + x1);
      const uint8_t *c = src + 4 * ((size_t)y1 * w + x0), *d = src + 4 * ((size_t)y1 * w + x1);
      uint8_t *o = dst + 4 * ((size_t)y * nw + x);
      for (int k = 0; k < 4; k++) o[k] = (uint8_t)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
    }
}

bool cw__fx_wgpu_on(void) {
#if CW_WGPU
  return cw__wgpu_on();
#else
  return false;
#endif
}

void cw__fx_ensure_pass(cw_tree *t) {
  cw__fx_tree *ft = cw__fx_tree_of(t);
  if (ft->pass_added) return;
  cw_pipeline *p = cw_pipeline_of(t);
  cw__fx_ensure_distort(t);
  if (!cw_pipeline_has_pass(p, "particles"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "particles", .phase = CW_PHASE_EFFECTS, .order = 200,
                                             .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                             .active = particles_active, .execute = particles_exec });
  ft->pass_added = true;
}

unsigned cw__fx_empty_vao(void) {
  static GLuint vao;
  if (!vao) glGenVertexArrays(1, &vao);
  return vao;
}

void cw__register_fx_types(void) {
  cw__register_particles3d_types();
  cw__register_particles2d_types();
  cw__register_trail3d_types();
  cw__register_decal_types();
  cw__register_distort_types();
  cw__register_explosion_types();
}

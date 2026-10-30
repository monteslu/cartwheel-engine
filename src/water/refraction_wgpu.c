/* Water3D's refraction on WebGPU: the scene drawn from the view's camera with
 * everything above (or, from under the water, below) the rest level clipped,
 * into a colour + depth target of the engine's own, then the depth copied to
 * an R32Float texture the surface reads.
 *
 * Why not three.c's own render target, as on GL: compatibility-mode WebGPU
 * (the wasmcart tier) reads a depth texture only through a comparison
 * sampler, so a material cannot read the refraction's depth texture. The
 * engine's depth copy (pipeline_wgpu.c, an exact binary search over compares)
 * makes the R32Float copy, and is given the projection z rows (2, 1, 0, -1),
 * which make it store the window depth itself (GL's [0, 1]: what the surface
 * reads on GL from the depth texture), 3e38 where nothing was drawn.
 *
 * Everything is recorded on the pipeline frame's encoder, in order, so a
 * split screen's views each read their own refraction. Rows are top-down
 * (flip_y off), as three.c's own targets: the surface samples both alike. */
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "water_internal.h"

#if CW_WGPU
#include "../render/pipeline_internal.h"
#include "../render/pipeline_wgpu.h"

static bool ensure(water3d *d, int w, int h) {
  cw_rt *c = d->wg_col, *z = d->wg_dep;
  if (c && c->w == w && c->h == h && z) return true;
  if (d->wg_col_tex) t3_release(d->wg_col_tex);
  if (d->wg_dep_tex) t3_release(d->wg_dep_tex);
  d->wg_col_tex = d->wg_dep_tex = NULL;
  cw_rt_free(c);
  cw_rt_free(z);
  d->wg_col = d->wg_dep = NULL;
  c = cw_rt_new(w, h, CW_RT_RGBA16F, true);
  z = cw_rt_new(w, h, CW__RT_R32F, false);
  if (!c || !z) {
    cw_rt_free(c);
    cw_rt_free(z);
    return false;
  }
  d->wg_col = c, d->wg_dep = z;
  d->wg_col_tex = t3_texture_from_wgpu(cw__pw_texture(c->tex), cw__pw_view(c->tex), w, h, 1, false, false);
  d->wg_dep_tex = t3_texture_from_wgpu(cw__pw_texture(z->tex), cw__pw_view(z->tex), w, h, 1, false, false);
  d->wg_fresh = true;
  return d->wg_col_tex && d->wg_dep_tex;
}

bool cw__water_refr_wgpu(cw_pass_ctx *ctx, water3d *d, t3_camera *cam, int w, int h) {
  if (!cw__wgpu_on()) return false;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (!ensure(d, w, h)) return false;
  cw_rt *c = d->wg_col, *z = d->wg_dep;
  if (!cam) {
    /* off: cleared once (never sampled garbage: a NaN would survive the shader's mix) */
    if (d->wg_fresh) {
      cw_pass_bind_target(ctx, c);
      cw_pass_clear(0, 0, 0, 1, true);
      cw_pass_bind_target(ctx, z);
      cw_pass_clear(1, 1, 1, 1, false);
      d->wg_fresh = false;
    }
    return true;
  }
  t3_renderer *r = ctx->renderer;
  /* cleared to black: where nothing is under the water the surface shows its own colour (an engine target gets no
   * sky, and the renderer's clear colour is not the scene's background) */
  t3_external_target et = {
    .color = (uintptr_t)cw__pw_attach(c->tex), .depth = (uintptr_t)cw__pw_attach(c->depth_tex),
    .color_format = (int)cw__pw_format(c->tex), .depth_format = (int)cw__pw_format(c->depth_tex), .samples = 1,
    .load_color = false, .load_depth = false, .clear_rgba = { 0, 0, 0, 1 }, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
    .x = 0, .y = 0, .w = w, .h = h, .flip_y = false,
  };
  t3_renderer_set_external_encoder(r, (uintptr_t)cw__pw_encoder());
  t3_renderer_set_external_target(r, &et);
  t3_renderer_render(r, ctx->scene3d, cam);
  t3_renderer_set_external_target(r, NULL);
  t3_renderer_set_external_encoder(r, 0);
  /* z rows (2, 1, 0, -1): the copy's view distance comes out as the window depth itself */
  static const float raw[4] = { 2, 1, 0, -1 };
  cw__pw_depth_copy(ctx, c, z, raw);
  d->wg_fresh = false;
  return true;
}

void cw__water_refr_wgpu_free(water3d *d) {
  if (d->wg_col_tex) t3_release(d->wg_col_tex);
  if (d->wg_dep_tex) t3_release(d->wg_dep_tex);
  cw_rt_free(d->wg_col);
  cw_rt_free(d->wg_dep);
  d->wg_col_tex = d->wg_dep_tex = NULL;
  d->wg_col = d->wg_dep = NULL;
}
#else
bool cw__water_refr_wgpu(cw_pass_ctx *ctx, water3d *d, t3_camera *cam, int w, int h) {
  (void)ctx, (void)d, (void)cam, (void)w, (void)h;
  return false;
}
void cw__water_refr_wgpu_free(water3d *d) { (void)d; }
#endif

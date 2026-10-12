/* render2d lights: the light buffer of a run of lit layers. Ambient clear,
 * then per light an optional stencil shadow pass (occluder edges extruded
 * away from the light) and one additive quad. */
#include <string.h>

#include "internal.h"

void cw_r2d_light(const cw_light2d *l) {
  CW_ASSERT(R.in_pass, "submit lights between cw_r2d_begin and cw_r2d_end");
  if (!R.layers[R.cur_layer].lit) {
    cw_warn("cw_r2d_light: layer %d is not lit (cw_r2d_layer_desc.lit); the light is ignored", R.cur_layer);
    return;
  }
  cw_array_push(&R.lights, ((r2d_light){ *l, R.has_xform ? R.xform : cw_xform2d_identity(), R.cur_layer }));
}

void cw_r2d_occluder(const cw_vec2 *pts, int n, bool closed) {
  CW_ASSERT(R.in_pass, "submit occluders between cw_r2d_begin and cw_r2d_end");
  if (n < 2) return;
  int first = R.occ_pts.count;
  for (int i = 0; i < n; i++) {
    cw_vec2 p = pts[i];
    if (R.has_xform) p = cw_xform2d_apply(&R.xform, p);
    cw_array_push(&R.occ_pts, p);
  }
  cw_array_push(&R.occluders, ((r2d_occluder){ first, n, closed, R.cur_layer }));
}

static float xform_scale(const cw_xform2d *m) {
  float det = m->a * m->d - m->b * m->c;
  return sqrtf(det < 0 ? -det : det);
}

/* shadow volume triangles of every occluder in [lo, hi] for a light at L
 * (canvas px), extruded `reach` px */
int r2d_shadow_tris(int lo, int hi, cw_vec2 L, float reach, float **out) {
  int edges = 0;
  for (int i = 0; i < R.occluders.count; i++) {
    const r2d_occluder *o = &R.occluders.data[i];
    if (o->layer >= lo && o->layer <= hi) edges += o->closed ? o->count : o->count - 1;
  }
  if (!edges) return 0;
  float *v = cw_frame_alloc(sizeof(float) * 4 * 6 * (size_t)edges);
  int nv = 0;
  for (int i = 0; i < R.occluders.count; i++) {
    const r2d_occluder *o = &R.occluders.data[i];
    if (o->layer < lo || o->layer > hi) continue;
    const cw_xform2d *m = &R.layer_view[o->layer];
    cw_vec2 *p = cw_frame_alloc(sizeof(cw_vec2) * (size_t)o->count);
    float area = 0;
    for (int k = 0; k < o->count; k++) p[k] = cw_xform2d_apply(m, R.occ_pts.data[o->first + k]);
    for (int k = 0, j = o->count - 1; k < o->count; j = k++) area += p[j].x * p[k].y - p[k].x * p[j].y;
    int ne = o->closed ? o->count : o->count - 1;
    for (int e = 0; e < ne; e++) {
      cw_vec2 a = p[e], b = p[(e + 1) % o->count];
      if (o->closed) {
        /* outward normal from the winding; only edges with the light behind
         * them cast (the occluder's lit face stays lit) */
        cw_vec2 d = cw_v2_sub(b, a);
        cw_vec2 nout = area > 0 ? (cw_vec2){ d.y, -d.x } : (cw_vec2){ -d.y, d.x };
        if (cw_v2_dot(nout, cw_v2_sub(L, a)) > 0) continue;
      }
      cw_vec2 da = cw_v2_scale(cw_v2_normalize(cw_v2_sub(a, L)), reach);
      cw_vec2 db = cw_v2_scale(cw_v2_normalize(cw_v2_sub(b, L)), reach);
      cw_vec2 a2 = cw_v2_add(a, da), b2 = cw_v2_add(b, db);
      cw_vec2 q[6] = { a, b, b2, a, b2, a2 };
      for (int k = 0; k < 6; k++) {
        float *w = v + 4 * nv++;
        w[0] = q[k].x;
        w[1] = q[k].y;
        w[2] = w[3] = 0;
      }
    }
  }
  *out = v;
  return nv;
}

void r2d_lights_render(int lo, int hi) {
  static cw_texture *rt = &R.light_rt;
  if (CW_HANDLE_IS_NULL(*rt) || !r2d_tex_get(*rt)) *rt = r2d_texture_new_internal(R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, true);
  else cw_target_resize(*rt, R.canvas_w, R.canvas_h);
  r2d_tex *lt = r2d_tex_get(*rt);
  r2d_set_target_fbo(lt->fbo, R.canvas_w, R.canvas_h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  cw_color a = R.pass.ambient;
  glClearColor(a.r * 0.5f, a.g * 0.5f, a.b * 0.5f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE);
  if (R.normals_used) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, R.arrays.data[r2d_tex_get(R.normal_rt)->array].tex);
    glActiveTexture(GL_TEXTURE0);
  }
  float diag = sqrtf((float)R.canvas_w * (float)R.canvas_w + (float)R.canvas_h * (float)R.canvas_h);
  for (int i = 0; i < R.lights.count; i++) {
    const r2d_light *rl = &R.lights.data[i];
    if (rl->layer < lo || rl->layer > hi) continue;
    cw_xform2d m = cw_xform2d_mul(&R.layer_view[rl->layer], &rl->local);
    cw_vec2 c = cw_xform2d_apply(&m, rl->l.pos);
    float k = xform_scale(&m), r = rl->l.radius * k;
    if (r <= 0) continue;
    if (c.x + r < 0 || c.y + r < 0 || c.x - r > (float)R.canvas_w || c.y - r > (float)R.canvas_h) continue;
    R.stats.lights++;
    float *sv = NULL;
    int snv = rl->l.shadows ? r2d_shadow_tris(lo, hi, c, 2 * r + diag, &sv) : 0;
    if (snv) {
      int sx0 = CW_MAX(0, (int)(c.x - r) - 1), sy1 = CW_MIN(R.canvas_h, (int)(c.y + r) + 2);
      int sx1 = CW_MIN(R.canvas_w, (int)(c.x + r) + 2), sy0 = CW_MAX(0, (int)(c.y - r) - 1);
      glEnable(GL_SCISSOR_TEST);
      glScissor(sx0, R.canvas_h - sy1, CW_MAX(0, sx1 - sx0), CW_MAX(0, sy1 - sy0));
      glStencilMask(0xff);
      glClearStencil(0);
      glClear(GL_STENCIL_BUFFER_BIT);
      glEnable(GL_STENCIL_TEST);
      glStencilFunc(GL_ALWAYS, 1, 0xff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glUseProgram(R.shadow.prog);
      glUniform2f(R.shadow.u_target, (float)R.canvas_w, (float)R.canvas_h);
      r2d_scratch_draw(sv, snv, GL_TRIANGLES);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glStencilFunc(GL_EQUAL, 0, 0xff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    }
    cw_color col = r2d_default_white(rl->l.color);
    float e = (rl->l.energy != 0 ? rl->l.energy : 1) * col.a * 0.5f;
    glUseProgram(R.light.prog);
    glUniform2f(R.light.u_target, (float)R.canvas_w, (float)R.canvas_h);
    glUniform2f(R.light.u[U_CENTER], c.x, (float)R.canvas_h - c.y);
    glUniform4f(R.light.u[U_LIGHT], col.r * e, col.g * e, col.b * e, r);
    float h = rl->l.height > 0 ? rl->l.height * k : r * 0.25f;
    glUniform3f(R.light.u[U_PARAMS], rl->l.falloff > 0 ? rl->l.falloff : 2.0f, h, R.normals_used ? 1.0f : 0.0f);
    r2d_draw_quad(c.x - r, c.y - r, c.x + r, c.y + r, 0, 0, 1, 1);
    if (snv) {
      glDisable(GL_STENCIL_TEST);
      glDisable(GL_SCISSOR_TEST);
    }
  }
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
}

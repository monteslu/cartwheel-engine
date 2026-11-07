/* render2d depth culling: the opaque fast path of the offscreen canvases.
 *
 * Layers draw back to front with blending, so a pixel under a full screen
 * of art is shaded once per layer: the sky, every parallax layer, the back
 * wall and the tiles in front of it. Here every submission gets a depth from
 * its place in the sorted stream (later = nearer), and a group of batches
 * that draw into the same offscreen buffers first draws its opaque texels
 * (alpha exactly 1, from art drawn with the alpha blend: textures that are
 * mostly opaque, solid colours, opaque tilesets) nearest first into
 * a depth buffer, colour off; then the group draws as always, back to front
 * with blending, testing that depth: a pixel behind an opaque one is never
 * shaded. Blending an opaque texel replaces what is under it, so the picture
 * is the same to the bit (and the normal and material buffers of a lit run
 * too): only work that could not show is skipped.
 *
 * A group ends wherever something reads what was drawn so far: a screen_read
 * or distortion layer, the end of a lit run (its lights and global
 * illumination read the run's buffers whole), the HUD layer of the chain.
 * Only offscreen buffers take part (the HDR canvas, a lit run's buffers);
 * a window canvas without a chain draws as before.
 *
 * Depths: triangle k of the sorted index stream is at t = 2k + 1, a tilemap
 * draw (no indices) at t = 2 x (triangles before it); depth d(t) = (T - t) /
 * (T + 1), T = 2 x triangles + 2, so nearer is smaller and a draw's own
 * depth range never meets its neighbours'. The group's draws test LESS
 * against their depth less half a step: a texel's own opaque write passes,
 * anything behind it (a step or more further) fails. Particles and sprite
 * instances are never culled and never cull (depth 0). */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/platform.h"

static r2d_cull_plan PL;
static CW_ARRAY(int) grp_store;
static CW_ARRAY(uint8_t) gcull_store, occ_store;
static CW_ARRAY(float) depth_store;

/* CW_R2D_CULL (native tests): 0 = off (the reference picture), "control" =
 * the test without its half step, so every opaque texel fails against its
 * own depth and the picture loses them (a control that must fail) */
static int cull_mode(void) {
  static int on = -1;
  if (on < 0) {
    on = 1;
#if !CW_CART
    const char *e = getenv("CW_R2D_CULL");
    if (e && *e == '0') on = 0;
    if (e && !strcmp(e, "control")) on = 2;
#endif
  }
  return on;
}
static bool cull_enabled(void) { return cull_mode() != 0; }

/* a tilemap draw's tileset is opaque art and its tint is not see-through (tilemap.c) */
static bool tilemap_opaque(const r2d_batch *b) { return r2d_tilemap_opaque(&R.tdraws.data[b->payload]); }

float r2d_cull_depth_t(float t) { return (PL.T - t) / (PL.T + 1.0f); }

const r2d_cull_plan *r2d_cull_build(const r2d_batch *b, int n, const uint32_t *idx, int nidx, bool hdr, int hud) {
  memset(&PL, 0, sizeof PL);
  int ntri = nidx / 3;
  PL.T = 2.0f * (float)ntri + 2.0f;
  PL.bias = cull_mode() == 2 ? 0.0f : 0.5f / (PL.T + 1.0f);
  if (!cull_enabled() || n < 2 || PL.T > 4.0e6f) return &PL;
  cw_array_reserve(&grp_store, n);
  cw_array_reserve(&occ_store, n);
  grp_store.count = occ_store.count = n;
  int *grp = grp_store.data;
  uint8_t *occ = occ_store.data;
  int g = -1, prev_layer = -1;
  bool prev_ok = false, prev_lit = false;
  for (int i = 0; i < n; i++) {
    const r2d_batch *c = &b[i];
    const cw_r2d_layer_desc *ld = &R.layers[c->layer];
    bool dist = ld->distortion > 0;
    bool ok = !dist && ((hdr && c->layer < hud) || c->lit);
    bool boundary = !prev_ok;
    if (c->layer != prev_layer &&
        (ld->screen_read || dist || (prev_layer >= 0 && R.layers[prev_layer].distortion > 0)))
      boundary = true;
    if (prev_lit && !c->lit) boundary = true;
    if (!ok) grp[i] = -1;
    else {
      if (boundary) g++;
      grp[i] = g;
    }
    uint32_t kind = R2D_ST_KIND(c->state);
    /* occluders: alpha-blended draws worth the pre-pass, opaque art (a texture mostly alpha 1, a solid
     * colour) and tilemaps of an opaque tileset; translucent art never hides anything, so drawing it twice is waste */
    bool opq = kind == R2D_KIND_STREAM ? c->opq : kind == R2D_KIND_TILEMAP ? tilemap_opaque(c) : false;
    occ[i] = ok && opq && R2D_ST_BLEND(c->state) == CW_BLEND_ALPHA;
    prev_ok = ok;
    prev_lit = c->lit;
    prev_layer = c->layer;
  }
  PL.ngroups = g + 1;
  if (!PL.ngroups) return &PL;
  /* a group culls when an occluder has something drawn before it */
  cw_array_reserve(&gcull_store, PL.ngroups);
  gcull_store.count = PL.ngroups;
  memset(gcull_store.data, 0, (size_t)PL.ngroups);
  int *first = cw_frame_alloc(sizeof(int) * (size_t)PL.ngroups);
  for (int k = 0; k < PL.ngroups; k++) first[k] = -1;
  for (int i = 0; i < n; i++) {
    if (grp[i] < 0) continue;
    if (first[grp[i]] < 0) first[grp[i]] = i;
    else if (occ[i]) gcull_store.data[grp[i]] = 1;
  }
  bool any = false;
  for (int k = 0; k < PL.ngroups; k++) any |= gcull_store.data[k] != 0;
  if (!any) return &PL;
  /* each vertex of a culled group's stream batches: the depth of the first triangle that uses it */
  cw_array_reserve(&depth_store, R.verts.count);
  depth_store.count = R.verts.count;
  memset(depth_store.data, 0, sizeof(float) * (size_t)R.verts.count);
  float *d = depth_store.data;
  for (int i = 0; i < n; i++) {
    if (grp[i] < 0 || !gcull_store.data[grp[i]] || R2D_ST_KIND(b[i].state) != R2D_KIND_STREAM) continue;
    for (uint32_t p = b[i].first, e = b[i].first + b[i].count; p < e; p++) {
      uint32_t v = idx[p];
      if (d[v] == 0) d[v] = r2d_cull_depth_t(2.0f * (float)(p / 3) + 1.0f);
    }
  }
  PL.grp = grp;
  PL.gcull = gcull_store.data;
  PL.occ = occ;
  PL.vdepth = d;
  PL.any = true;
  return &PL;
}

void r2d_cull_shutdown(void) {
  cw_array_free(&grp_store);
  cw_array_free(&gcull_store);
  cw_array_free(&occ_store);
  cw_array_free(&depth_store);
  memset(&PL, 0, sizeof PL);
}

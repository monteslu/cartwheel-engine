/* Decals: images box-projected onto whatever geometry the scene drew.
 *
 * ── node type ──────────────────────────────────────────────────────────
 *   Decal3D   (Node3D) a box of `size` (x width, y height, z depth) centred
 *             on the node; the image covers the box's XY face and projects
 *             along the node's -Z (the cartwheel convention: lights, cameras
 *             and decals face their -Z; aim one at a floor with rotation
 *             (-90 deg, 0, 0) or cw_node3d_look_at). Properties:
 *     texture       image asset (PNG / JPEG; empty = a soft round splat);
 *                   every decal image lives in one shared 2048x2048 atlas
 *     region        vec4 x y w h in image pixels (0 = the whole image): one
 *                   image can hold a sheet of decals
 *     color         tint (alpha = opacity)
 *     energy        ADD: brightness (HDR: the scene is linear HDR)
 *     blend         MIX (premultiplied over the lit scene: unlit, for signs,
 *                   paint, posters), MULTIPLY (scene x colour: lighting stays
 *                   correct; dirt, scorch, cracks, wet patches) or ADD (glow)
 *     normal_fade   0..1: fades where the surface turns away from the
 *                   projection axis (0 = only back-facing surfaces reject;
 *                   0.5 = gone at 60 degrees off axis)
 *     edge_fade     0..0.5: fraction of the depth fading out at the box's
 *                   near and far ends
 *     priority      draw order (higher draws later, on top); ties in tree
 *                   order
 *
 * How it draws. The pipeline's "decals" EFFECTS pass (order 100, before the
 * particles) draws every visible decal's box with instancing (one draw per
 * run of equal blend in priority order), back faces, without depth testing,
 * so a camera inside a box still sees it. Each pixel rebuilds its world
 * position from the scene depth (ctx->depth_tex), moves it into the decal's
 * box and samples the atlas there, so decals wrap uneven geometry: steps,
 * slopes, spheres, props. Decals are frustum culled on the CPU (box against
 * the six planes); the rasterized box is the per-pixel cull. Surface normals for normal_fade are rebuilt from
 * depth derivatives. Images are decoded from sRGB to linear.
 *
 * Limits: decals land on everything the scene drew into depth (characters
 * walking through a decal box get it too: keep boxes shallow), not on
 * transparent surfaces, particles or the sky. Costs: one pass over decals on
 * the CPU per frame, an instance upload, a few draws; the GPU cost is the
 * screen area the boxes cover. */
#ifndef CW_DECALS_H
#define CW_DECALS_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_decal_stats {
  int decals;      /* Decal3D nodes in the tree */
  int visible;     /* drawn last frame (in the frustum, opacity > 0) */
  int culled;      /* outside the frustum */
  int draw_calls;  /* instanced draws */
  int atlas_images; /* images in the shared atlas */
} cw_decal_stats;
/* last frame's counts; zeroed when the tree has no decals or t is NULL */
void cw_decals_get_stats(cw_tree *t, cw_decal_stats *out);

#ifdef __cplusplus
}
#endif
#endif

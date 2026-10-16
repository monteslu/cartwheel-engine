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
 *     mode          SCREEN (the default: projected on the scene's depth every
 *                   frame, below) or MESH (clipped from the meshes once, see
 *                   "Mesh mode")
 *     target        MESH: a node path from the decal ("../Hero"); the decal
 *                   sticks to that node's meshes and everything under it.
 *                   Empty: every mesh in the tree whose geometry crosses the
 *                   box (MeshInstance3D primitives and resources, glTF
 *                   scenes, any three.c mesh a node shows)
 *     max_triangles MESH: the most triangles one decal keeps (1024); clipping
 *                   stops there, with a warning
 *     mesh_offset   MESH: metres the decal floats off the surface (0.002),
 *                   against depth fighting
 *     max_mesh_decals MESH: 0 (the default) keeps the decal for good. Above
 *                   0 it joins the tree's pool of recycled decals (bullet
 *                   holes, footprints: spawn and forget), which keeps this
 *                   many (the newest pooled decal's value counts): one more
 *                   frees the OLDEST pooled decal's node
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
 * screen area the boxes cover.
 *
 * Mesh mode (mode = MESH). When the decal enters the tree, and again when
 * its box changes (size, the node's own position, rotation or scale, target,
 * texture, region, the fades), the CPU clips the target meshes' triangles to
 * the box (Sutherland-Hodgman against its six planes) and keeps the result
 * as small meshes parented under each source mesh, so the decal moves, bends
 * and hides with it. Characters: the clip sees the skinned pose of that
 * moment, and every new vertex is kept as weights of its source triangle, so
 * it is rebuilt in the bind pose with the four strongest joints of its
 * corners and drawn as a skinned mesh on the same skeleton: a decal on an
 * arm swings with the arm. Faces turned away from the projection (-Z) are
 * dropped; normal_fade and edge_fade are baked per vertex. The pieces draw
 * with the transparent objects, right after their surface (depth test on,
 * no depth write), so they also land on glass and other transparent meshes;
 * blends match the screen-space ones (MIX unlit over, MULTIPLY, ADD with
 * energy). The decal does not follow later changes of the surface itself
 * (morph targets, a mesh edited after the stamp, a different LOD level is
 * drawn as is). Not sources: instanced meshes (forests, particles), lines
 * and points, meshes drawn with an extension program (Terrain3D), other
 * decals. Costs: the clip touches every vertex of every candidate mesh once
 * (rigid meshes whose bounds miss the box are skipped; skinned ones are
 * posed first): 0.4 to 0.7 ms per decal on a 4.7k-triangle skinned
 * character (Ryzen AI 9 HX 370, machine under load); a built decal costs
 * nothing on the CPU per frame and one small draw per source mesh. Clipping
 * is deterministic: the same scene gives the same bits on every host. */
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
  /* mesh mode */
  int mesh_decals;    /* mesh decals with geometry, now */
  int mesh_triangles; /* their triangles */
  int mesh_pieces;    /* their meshes (one per source mesh) */
  int mesh_recycled;  /* pooled decals freed by max_mesh_decals so far */
  int mesh_builds;    /* clips run so far */
  uint32_t mesh_hash; /* FNV of every mesh decal's geometry hash, in the order they entered the tree */
} cw_decal_stats;
/* last frame's counts; zeroed when the tree has no decals or t is NULL */
void cw_decals_get_stats(cw_tree *t, cw_decal_stats *out);

/* One mesh-mode decal's geometry. */
typedef struct cw_decal_mesh_info {
  bool built;      /* clipped at least once */
  bool truncated;  /* max_triangles stopped the clip */
  int triangles;
  int pieces;      /* meshes made (one per source mesh it landed on) */
  int sources;     /* source meshes it landed on */
  uint32_t hash;   /* FNV-1a of the vertex data (positions, uv, fades, skin), equal on every host */
  float build_ms;  /* CPU time of the last clip */
} cw_decal_mesh_info;
/* false (out zeroed) when decal is not a Decal3D in MESH mode. The geometry
 * is built in the frame's pre-render, after the nodes' own: a decal added
 * this frame reports built from the next render on. */
bool cw_decal_get_mesh_info(cw_node decal, cw_decal_mesh_info *out);
/* Clip again at the next render (a character in a new pose, a mesh that
 * changed): mesh mode only, otherwise nothing. */
void cw_decal_restamp(cw_node decal);

#ifdef __cplusplus
}
#endif
#endif

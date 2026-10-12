/* Visibility: culling, LOD and impostors for the tree's 3D meshes.
 *
 * A tree opts in with a VisibilityManager node (the first one in tree order
 * counts); without one, three.c's own per-object frustum test is all there
 * is. Everything below works on MeshInstance3D nodes.
 *
 * ── node types (properties: docs/nodes.json) ────────────────────────────
 *   VisibilityManager  (Node) the switches and the read-only statistics:
 *     frustum_cull      BVH over static meshes (O(visible): a BVH4 walked
 *                       four child boxes at a time with SIMD, its top levels
 *                       spread over the job pool) plus a flat SIMD pass over
 *                       moving ones
 *     occlusion_cull    GPU occlusion queries (ANY_SAMPLES_PASSED_CONSERVATIVE)
 *                       on each candidate's bounding box against the depth the
 *                       scene just drew; results are read a frame later, so a
 *                       frame draws the PREVIOUS frame's visible set. Boxes are
 *                       inflated by occlusion_margin plus the camera's motion
 *                       over the query latency, objects new to the frustum
 *                       start visible, and a result that is late more than
 *                       two frames counts as visible: disocclusion is seen
 *                       before the object is, so nothing pops in. Visible
 *                       objects are re-queried every occlusion_interval frames
 *                       (staggered), occluded ones every frame.
 *                       occlusion_wait reads each result as soon as the next
 *                       frame starts, waiting for it if the GPU is behind (the
 *                       same counts on every host: tests)
 *     occlusion_guard   (true) what keeps queries from popping a moving
 *                       camera: boxes also grow by tan(6 x turn + 6 x step /
 *                       distance to the nearest drawn static box) per metre
 *                       of distance, a box the grown query would clip or
 *                       straddle the frustum edge with is treated as
 *                       visible, and while the camera is closer to a drawn
 *                       static box than occlusion_margin + 3 x its step this
 *                       frame no result is trusted and NO queries are
 *                       issued. So a fast camera brushing past props skips
 *                       occlusion culling altogether: vis_city's street run
 *                       (up to 0.9 m a frame, 0.6 m from the street trees)
 *                       issues no queries from about tick 10 to 220, and
 *                       its occlusion counts only appear once the camera
 *                       slows. Off: plain one-frame-late queries, which pop.
 *     portals           Room3D / Portal3D culling (below), when rooms exist
 *     lod               pick each mesh's LOD: mesh resources and glTF scenes
 *                       use their cwpack LOD chains (the coarsest level whose
 *                       simplification error stays within lod_error_px pixels
 *                       on screen); SPHERE and CYLINDER primitives have
 *                       built-in coarser tessellations picked the same way
 *     lod_error_px      the screen-space error budget (1 pixel)
 *     impostors         far meshes with `impostor` set draw as octahedral
 *                       impostors (below)
 *     shadow_distance   how far a shadow caster's box is swept along the
 *                       light when frustum culling (a caster outside the view
 *                       still shadows what is inside); casters are never
 *                       occlusion or portal culled while a DirectionalLight3D
 *                       casts shadows; with an omni or spot light casting
 *                       shadows, casters are not culled at all
 *     stats (read-only) meshes, static_meshes, visible, frustum_culled,
 *                       occlusion_culled, portal_culled, distance_culled,
 *                       queries (issued last frame), query_latency (frames
 *                       the last result took), lod_counts (vec4: meshes drawn
 *                       at LOD 0, 1, 2, 3 and coarser), impostors (drawn),
 *                       impostor_fading (drawn with their mesh, crossfading),
 *                       bvh_nodes, rooms_visible, cull_ms (CPU)
 *   MeshInstance3D gains:
 *     static            the mesh does not move: it joins the BVH (moving it
 *                       later is allowed; the BVH is refitted)
 *     lod_bias          scales the error budget for this mesh (2 = coarser)
 *     visibility_range  hidden beyond this distance (0 = no limit)
 *     occlusion_cull    may be hidden by occlusion queries (true)
 *     impostor          beyond impostor_distance the mesh draws as an
 *                       impostor: its look from 64 directions (an 8x8
 *                       octahedral grid) baked into an atlas the first time
 *                       it is needed, lit by the tree's directional, ambient
 *                       and hemisphere lights; instances with the same
 *                       geometry, materials and scale share one atlas. Over
 *                       impostor_fade metres the impostor fades in over the
 *                       still-drawn mesh (which goes only once the impostor
 *                       is opaque), so the switch never pops
 *     lod (read-only)   the level drawn last frame (-1 culled, 100 impostor)
 *   Room3D             (Node3D) a box (extents = half sizes) of interior
 *                      space; static meshes whose box centre is inside belong
 *                      to it
 *   Portal3D           (Node3D) a size.x by size.y opening in the node's XY
 *                      plane joining the rooms on its two sides (no room on a
 *                      side = the outside); enabled = false closes it (a shut
 *                      door). From a room, the camera sees other rooms only
 *                      through portals, each narrowing the view to the
 *                      portal's screen rectangle; from outside, rooms are seen
 *                      through their portals to the outside.
 *
 * Where it runs: the pipeline's "visibility" SHADOW-phase pass (order 2000,
 * after engine shadow maps, which so still see every caster)
 * culls, picks LODs and bakes impostors before the scene draws; culled meshes
 * are hidden for the frame and shown again by the "visibility_restore"
 * PRESENT pass, so game code never sees the flags change. "impostors" (OPAQUE,
 * 100) draws the impostors; "occlusion_queries" (OPAQUE, 900) issues the
 * queries against the finished depth.
 *
 * Costs per frame: the BVH walk (visible boxes plus their near misses), a
 * pass over moving meshes, a pass over visible meshes for LOD, one query
 * draw per queried mesh, one instanced draw per impostor atlas. The first use
 * of an impostor atlas renders the mesh 64 times. */
#ifndef CW_VISIBILITY_H
#define CW_VISIBILITY_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_visibility_stats {
  int meshes, static_meshes, visible;
  int frustum_culled, occlusion_culled, portal_culled, distance_culled;
  int queries, query_latency;
  int queries_late; /* results not ready when read (counted visible meanwhile) */
  int lod_counts[4];
  int impostors, impostor_fading, impostor_atlases;
  int bvh_nodes, rooms, rooms_visible;
  float cull_ms;
} cw_visibility_stats;
/* The last frame's numbers (zero without a VisibilityManager). */
void cw_visibility_get_stats(cw_tree *t, cw_visibility_stats *out);

/* Whether the visibility pass drew this MeshInstance3D last frame (as a mesh
 * or an impostor). */
bool cw_visibility_drawn(cw_node mesh);

#ifdef __cplusplus
}
#endif
#endif

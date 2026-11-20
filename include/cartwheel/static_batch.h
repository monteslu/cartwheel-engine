/* cartwheel/static_batch.h: many different static meshes in a few draws.
 *
 * A level dressed with thousands of props (rocks, crates, fence posts,
 * rubble, market stalls) pays one draw call a prop, however cheap each one
 * is, and draw calls are what a weak GPU's driver and a WebGPU host run out
 * of first. Automatic instancing only helps props that share a geometry. A
 * MeshInstance3D with `batch = true` joins its tree's static batches instead:
 * every batched mesh that shares a material (and its shadow flags) lives in
 * one three.js r187 BatchedMesh, whatever its geometry, and the batch draws
 * a run of visible props that follow each other in its buffer as ONE draw.
 * Five thousand different props are then a handful of draws. Each prop is
 * still frustum culled on its own (the camera's frustum, and each shadow
 * pass's), and hiding a node or freeing it takes its prop out.
 *
 * What a batched mesh gives up, since it is no longer a draw of its own:
 * level of detail, occlusion culling, impostors and visibility_range (it
 * draws its full mesh whenever it is in view), and a lightmap of its own. It
 * may move (its matrix is written to the batch's texture when its world
 * transform changes, a frame late), but the feature is meant for props that
 * stay put: moving one costs an upload of the batch's matrix texture.
 *
 * Runs are per batch in the order the props joined: props that are near each
 * other in the scene file end up near each other in the buffer, and a view
 * then draws few, long runs. A hidden prop in the middle of a run splits it
 * in two; a few props that are merely out of view do not (they are drawn
 * and clipped, which is cheaper than another draw call).
 *
 * Memory: a batch holds a copy of each prop's vertices (four more bytes a
 * vertex for the prop's id) and 64 bytes a prop for its matrix. The source
 * meshes keep their geometry (collision, baking and picking still read it).
 *
 * Program tables: a batched material draws states with the `batch` feature
 * (standard+batch+..., depth+batch): tools/programs.mjs finds and adds them
 * like any other state.
 *
 * From code, without nodes: three.c's t3_batched_mesh_new and friends are
 * the same thing by hand. */
#ifndef CW_STATIC_BATCH_H
#define CW_STATIC_BATCH_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_static_batch_stats {
  int batches;    /* BatchedMeshes in the tree (one a material and shadow setting) */
  int meshes;     /* meshes drawn through them */
  int geometries; /* distinct geometries they hold */
  int draws;      /* draw calls the batches took in the last pass that drew them */
  int drawn;      /* meshes those draws covered (the rest were out of view or hidden) */
} cw_static_batch_stats;
/* The tree's static batches now (zeros when it has none). */
void cw_static_batch_get_stats(cw_tree *t, cw_static_batch_stats *out);

#ifdef __cplusplus
}
#endif
#endif

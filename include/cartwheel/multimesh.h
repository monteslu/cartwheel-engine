/* cartwheel MultiMeshInstance3D: many instances of one mesh from an instance
 * buffer the game fills: a forest, a field of rocks, a crowd of props, a
 * swarm. One node instead of one MeshInstance3D each: no node, no three.c
 * object and no transform update per instance.
 *
 * THE MESH. `source` names a Node3D (a path from this node; empty: the first
 * MeshInstance3D child). Every mesh under it is drawn per instance, each
 * with its own material: a MeshInstance3D primitive (its colour and
 * roughness properties apply), a mesh resource, or a glTF scene of several
 * meshes. The source's own transform applies to every instance (scale a
 * model once there). The source node is hidden: it is the stamp, not a copy.
 * Skinned meshes are not instanced.
 *
 * THE BUFFER. cw_multimesh_set_instance_count allocates `count` instances
 * (identity transforms, white, zero custom data); cw_multimesh_set_transform
 * / _set_color / _set_custom write one; cw_multimesh_transforms hands back
 * the transform array to fill in a loop (16 floats each, column major, the
 * node's space), then cw_multimesh_touch. Changes are taken at the next
 * render. `visible_instance_count` draws only the first N.
 * A scene file can fill a grid without code (`fill = GRID`, `fill_count`,
 * `fill_spacing`): block-outs, tests, a parade.
 *
 * CHUNKS AND CULLING. The instances are sorted into cubic cells of
 * `chunk_size` metres; each cell is one instanced draw per mesh part with its
 * own bounds, which three.c culls against the camera and against each shadow
 * light, so what is off screen costs nothing but the cell test. Small cells
 * cull tightly and cost more draws; 16 to 32 m suits ground cover seen from
 * eye height. `chunk_size = 0` keeps one draw for everything (a swarm that
 * moves every frame and is all on screen: its buffer is rewritten in place,
 * no sort).
 * Rebuilding the chunks after the buffer changed costs a sort and a copy of
 * every instance (see the guide for the figure); a buffer that does not
 * change costs nothing per frame beyond the cell tests.
 *
 * LIGHTING. Instances are ordinary three.c instanced meshes: they take the
 * scene's lights (forward or clustered), cast and receive shadows from single
 * maps, the atlas and cascades (`cast_shadow`, `receive_shadow`), and take
 * reflection probes and the probe grid as any dynamic mesh does. They are not
 * lightmapped.
 *
 * COLOUR AND CUSTOM DATA. `use_colors` multiplies each instance's colour into
 * the material's (three.js instanceColor). Custom data (4 floats an instance)
 * is stored for game code (a health value, a phase); no built-in material
 * reads it.
 *
 * Debug command: `multimesh <node>` (instances, cells, cells and instances in
 * view, draws, last rebuild ms).
 */
#ifndef CW_MULTIMESH_H
#define CW_MULTIMESH_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MultiMeshInstance3D properties (a Node3D):
 *   source (string)                 the stamp (see above)
 *   instance_count (int)            instances in the buffer
 *   visible_instance_count (int)    draw only the first N (-1: all)
 *   chunk_size (float, 16)          cell size in metres (0: one draw)
 *   cast_shadow, receive_shadow (bool)
 *   use_colors (bool)               per-instance colour
 *   fill (enum NONE, GRID), fill_count (vec3), fill_spacing (vec3)
 *   visible_instances, chunks, visible_chunks (int, read-only) */

/* Allocate the buffer: count instances, identity, white. Keeps the first
 * min(old, new) instances. */
void cw_multimesh_set_instance_count(cw_node n, int count);
/* Instances in the buffer (0 for another node type). */
int cw_multimesh_instance_count(cw_node n);
/* One instance's transform in the node's space. Out of range: ignored (logged once). */
void cw_multimesh_set_transform(cw_node n, int i, const t3_mat4 *m);
/* false when out of range */
bool cw_multimesh_get_transform(cw_node n, int i, t3_mat4 *out);
/* One instance's colour (linear; drawn when use_colors). */
void cw_multimesh_set_color(cw_node n, int i, t3_color c);
/* One instance's custom data: 4 floats for game code. */
void cw_multimesh_set_custom(cw_node n, int i, const float v[4]);
/* false when out of range */
bool cw_multimesh_get_custom(cw_node n, int i, float out[4]);
/* The transform array (instance_count x 16 floats) to fill in place; NULL
 * when empty. Valid until the count changes. Call cw_multimesh_touch after. */
float *cw_multimesh_transforms(cw_node n);
/* The buffer changed: rebuild at the next render. */
void cw_multimesh_touch(cw_node n);

typedef struct cw_multimesh_stats {
  int instances;          /* drawn at most (the buffer, or visible_instance_count) */
  int chunks;             /* cells holding instances */
  int visible_chunks;     /* cells inside the main camera's view last frame */
  int visible_instances;  /* instances in those cells */
  int draws;              /* instanced draws those cells make (cells x mesh parts) */
  int parts;              /* meshes under the source */
  float build_ms;         /* the last rebuild (host clock: never game state) */
  int builds;             /* rebuilds so far */
} cw_multimesh_stats;
/* Zeroed for another node type. */
void cw_multimesh_get_stats(cw_node n, cw_multimesh_stats *out);

#ifdef __cplusplus
}
#endif
#endif

/* CSG: block-out shapes for laying out a level (greyboxing). Boxes,
 * cylinders and spheres combine by union, subtraction and intersection into
 * one mesh: a room is a box with a smaller box subtracted, a door is one more
 * subtracted box, a pillar is a cylinder.
 *
 * ── node types ─────────────────────────────────────────────────────────
 *   CSGShape3D      (Node3D) the base of every CSG node. Properties:
 *     operation     how this shape combines with what its CSG parent has
 *                   built so far: UNION, INTERSECTION or SUBTRACTION (the
 *                   root's own operation is ignored)
 *     color         this shape's surface colour (linear). A face cut by a
 *                   subtracted or intersected shape takes the colour of the
 *                   shape that cut it
 *   CSGCombiner3D   (CSGShape3D) no shape of its own: groups its children
 *   CSGBox3D        (CSGShape3D) size (x, y, z extents, centred)
 *   CSGCylinder3D   (CSGShape3D) radius, height (along Y, centred), sides,
 *                   cone (radius at the bottom, a point at the top)
 *   CSGSphere3D     (CSGShape3D) radius, segments
 * The topmost CSG node of a branch (one whose parent is not a CSG node) is
 * the ROOT: it owns the result. It starts from its own shape (nothing for a
 * combiner), then applies each CSG child in child order with the child's
 * operation; a child that has CSG children of its own is built the same way
 * first. A child's position, rotation and scale place it relative to its
 * parent. Root-only properties (ignored elsewhere):
 *     bake_file     the baked mesh asset ("levels/room.cwcsg"); empty = preview
 *     roughness, metalness, cast_shadow, receive_shadow   the surface
 *     ghosts        preview: draw subtracted / intersected shapes as
 *                   see-through boxes (default on)
 *   Read-only on the root: baked (the bake file is drawn), stale (the
 *   shapes changed since that bake), triangles, file_hash and mesh_hash
 *   (FNV-1a 64 of the file bytes and of the geometry as loaded, hex).
 *
 * ── baking ─────────────────────────────────────────────────────────────
 * The booleans run at tool time, never in a game: the cwcsg tool (native
 * only, tools/cwcsg) loads a scene, runs every root's shapes through
 * Manifold (robust, always-closed mesh booleans) and writes each root's
 * bake_file:
 *   cwcsg <scene.cwscene> [--assets DIR] [--check] [--watch]
 * DIR is the asset root bake_file is relative to (default: the scene's
 * directory). --check writes nothing and exits 1 when a bake file is missing
 * or differs from what the shapes give (a build or CI step); --watch bakes
 * again whenever the scene file changes. The same scene gives the same bytes
 * on every run: positions are snapped to 0.1 mm, faces are flat shaded,
 * and triangles and vertices are written in a sorted order, not Manifold's.
 * Round shapes use exactly `sides` / `segments` facets. The game loads the
 * file like any other asset, so every host draws the same mesh.
 *
 * ── preview ────────────────────────────────────────────────────────────
 * A root without a bake file (or whose file is missing) draws each shape as
 * it is, with no booleans: unioned shapes solid in their colour, subtracted
 * shapes as red see-through ghosts and intersected shapes as blue ones, so
 * you see where the cuts will go while you lay out. The cut itself only
 * appears after a bake. A root whose bake file is older than its shapes
 * (stale) still draws the file and logs a warning once naming the fix.
 *
 * ── collision ──────────────────────────────────────────────────────────
 * A CollisionShape3D with shape TRIMESH (or CONVEX) and `mesh` pointing at a
 * baked CSG root collides with the baked mesh (physics_nodes.h); put it on a
 * StaticBody3D. Preview shapes have no collision.
 *
 * ── the file (.cwcsg, little-endian) ───────────────────────────────────
 *   "CWCSG001", u32 vertex_count, u32 index_count, u64 input_hash,
 *   f32 bounds[6] (min xyz, max xyz),
 *   f32 positions[3V], f32 normals[3V], f32 colors[3V], f32 uvs[2V],
 *   u32 indices[I]
 * in the root's local space. uvs are box-projected world metres (one texture
 * repeat per metre). input_hash is cw_csg_input_hash of the shapes it was
 * baked from (how a stale bake is noticed).
 *
 * Costs: a baked root is one mesh and one draw. The preview is one draw per
 * shape. Baking the sample level takes a few milliseconds. */
#ifndef CW_CSG_H
#define CW_CSG_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_CSG_UNION, CW_CSG_INTERSECTION, CW_CSG_SUBTRACTION } cw_csg_op;
typedef enum { CW_CSG_COMBINER, CW_CSG_BOX, CW_CSG_CYLINDER, CW_CSG_SPHERE } cw_csg_kind;

/* n is a CSG node whose parent is not one */
bool cw_csg_is_root(cw_node n);
/* the root of n's CSG branch; null when n is not a CSG node */
cw_node cw_csg_root_of(cw_node n);

/* A hash of everything a bake depends on: every shape's type, operation,
 * colour, shape properties and position / rotation / scale properties, in
 * tree order. Pure property bits (no maths), so it is the same in the tool
 * and in every game build. 0 when root is not a CSG root. O(shapes). */
uint64_t cw_csg_input_hash(cw_node root);

/* One shape of a branch, flattened for a bake tool: its transform into the
 * root's space (parent chain composed; the root's own is identity) and its
 * parent's index (-1 for the root). Shapes come in tree order, so a parent
 * precedes its children and siblings keep their order. */
typedef struct cw_csg_shape {
  cw_csg_kind kind;
  cw_csg_op op;
  int parent;
  t3_mat4 to_root;
  t3_color color;
  t3_vec3 size;       /* BOX */
  float radius, height; /* CYLINDER (radius, height), SPHERE (radius) */
  int segments;       /* CYLINDER sides, SPHERE segments */
  bool cone;
} cw_csg_shape;
/* Writes up to cap shapes and returns how many the branch has (call with
 * cap 0 to size the array). Reads the node transforms as of the last
 * cw_tree_pre_render. */
int cw_csg_flatten(cw_node root, cw_csg_shape *out, int cap);

/* A baked mesh, as the file stores it. */
typedef struct cw_csg_mesh {
  uint32_t vertex_count, index_count;
  uint64_t input_hash;
  float bounds[6];
  const float *positions, *normals, *colors, *uvs; /* 3, 3, 3, 2 floats per vertex */
  const uint32_t *indices;
} cw_csg_mesh;
/* The file bytes for m (cw_alloc'd, CW_MEM_RESOURCE; *len set). */
uint8_t *cw_csg_mesh_encode(const cw_csg_mesh *m, size_t *len);
/* Points out's arrays into bytes (which must stay alive and be 4-byte
 * aligned). false (with the reason logged) on a bad or truncated file. */
bool cw_csg_mesh_decode(const uint8_t *bytes, size_t len, cw_csg_mesh *out);
/* FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3) of bytes: the
 * file_hash and mesh_hash properties are this, as 16 hex digits. */
uint64_t cw_csg_fnv1a(const void *bytes, size_t len, uint64_t h);

/* The baked mesh a root draws (owned by the node), NULL when it draws the
 * preview. CollisionShape3D's mesh path reads it. */
t3_mesh *cw_csg_baked_mesh(cw_node root);

#ifdef __cplusplus
}
#endif
#endif

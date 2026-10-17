/* MeshInstance3D internals the visibility module (src/visibility) uses:
 * its culling / LOD / impostor properties and LOD switching. */
#ifndef CW_RENDER3D_INTERNAL_H
#define CW_RENDER3D_INTERNAL_H

#include "cartwheel/scene.h"

/* The visibility properties of a MeshInstance3D (cartwheel/visibility.h),
 * stored in its data: the pointer stays valid while the node lives. */
typedef struct cw__mesh_vis {
  bool is_static;          /* "static" */
  bool occlusion_cull;
  bool impostor;
  float lod_bias;
  float visibility_range;
  float impostor_distance;
  float impostor_fade;
  int lod;                 /* read-only: the level drawn last frame (-1 culled, 100 impostor) */
} cw__mesh_vis;

enum { CW__MESH_PRIMITIVE, CW__MESH_RESOURCE, CW__MESH_GLTF };

typedef struct cw__mesh_info {
  cw__mesh_vis *vis;
  t3_object *obj;          /* what draws (the primitive's mesh, a resource mesh or a glTF instance); NULL = nothing */
  int kind;                /* CW__MESH_* */
  int lod_levels;          /* 1 = no chain */
  bool cast_shadow;
  uint64_t look_key;       /* equal for instances that look the same (geometry, materials, colours) */
} cw__mesh_info;

const cw_node_type *cw__mesh3d_type(void);
/* cw_render3d_stats.cpu_ms: the render pipeline (and cw_render3d_draw)
 * report the CPU time their 3D phases took; no-op without 3D state. */
void cw__render3d_set_cpu_ms(cw_tree *t, float ms);
double cw__render3d_now_ms(void);
/* keep: cw_render3d_prepare leaves the shadow maps drawn by the last view,
 * and the scene's world matrices as the last view updated them (splitscreen
 * draws the maps and updates the matrices once per frame, for the first view) */
void cw__render3d_keep_shadows(cw_tree *t, bool keep);
/* true while the shadow maps are being kept (a later view of a split screen: it draws no maps of its own) */
bool cw__render3d_shadows_kept(cw_tree *t);
/* the frame's draw counts (cw_render3d_get_stats): start over, and add the
 * render just made (the pipeline's scene and prepass renders, every view) */
void cw__render3d_frame_begin(cw_tree *t);
void cw__render3d_count(cw_tree *t);
/* cw_render3d_prepare without the renderer: the camera's matrices only */
t3_camera *cw__render3d_camera_prepare(cw_tree *t, int w, int h); /* monotonic clock, milliseconds */
/* Fill info for a MeshInstance3D (builds what is pending first); false when
 * n is not one. */
bool cw__mesh3d_info(cw_node n, cw__mesh_info *out);
/* The object-space error of a level (mesh units before the node's scale; 0
 * for level 0). glTF scenes answer per mesh: use cw_gltf_instance_auto_lod. */
float cw__mesh3d_lod_error(cw_node n, int level);
/* Draw level `level` (clamped to the chain). */
void cw__mesh3d_set_lod(cw_node n, int level);

#endif

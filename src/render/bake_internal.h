/* What the lightmap baker (src/render/lightmap.c) reads from the 3D node
 * types (src/scene/render3d.c). */
#ifndef CW_BAKE_INTERNAL_H
#define CW_BAKE_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/render3d.h"

/* One Light3D with `bake` set, in world space. */
typedef struct cw__light {
  cw_node node;
  int kind;                 /* 0 directional, 1 omni, 2 spot */
  t3_vec3 pos, dir;         /* dir: where the light shines (the node's -Z) */
  t3_color color;           /* colour x energy */
  float range, decay;
  float angle, penumbra;    /* spot */
  bool direct;              /* bake_direct: the lightmaps keep its direct light too */
} cw__light;
bool cw__light3d_bake_info(cw_node n, cw__light *out);

/* a MeshInstance3D for the lightmapper */
typedef struct cw__mesh_bake {
  bool is_static;           /* gi_mode STATIC */
  t3_mesh *mesh;            /* its three.c mesh (world matrix: mesh->base.matrix_world) */
  t3_color albedo, emissive;
  float scale;              /* lightmap_scale */
  bool instanced;           /* an instanced mesh: occludes and bounces in the bake, gets no lightmap */
  bool uv0;                 /* lightmap_uv UV0: the lightmap is laid out on the mesh's own uv0 */
} cw__mesh_bake;
bool cw__mesh3d_bake_info(cw_node n, cw__mesh_bake *out);
/* the baked geometry (with uv1) and lightmap for a mesh */
void cw__mesh3d_set_lightmap(cw_node n, t3_geometry *g, t3_texture *lm, float intensity, bool baked);
/* the WorldEnvironment's ambient and hemisphere light (colour x energy) */
void cw__world_env_bake(cw_tree *t, t3_color *ambient, t3_color *sky, t3_color *ground);

#endif

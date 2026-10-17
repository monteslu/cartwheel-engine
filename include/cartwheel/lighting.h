/* Lighting for a tree's 3D scene beyond three.c's per-material lights:
 * hundreds of omni and spot lights through clustered shading, their shadows
 * in one atlas, and the sun's cascaded shadow maps.
 *
 * FORWARD (the default): every Light3D is three.c's (three.js r186's lights,
 * at most 8 of each kind light a surface; more are dropped with a warning).
 *
 * CLUSTERED: three.js r186's ClusteredLighting, on GLES3 and WebGPU alike.
 * Omni lights (and spot lights, unless cluster_spot_lights is off) leave
 * three.c's per-material light list and are shaded through clusters instead:
 *   - every frame the CPU bins them into a froxel grid over the view
 *     (cartwheel/froxel.h: r186's grid, tile_px tiles x z_slices exponential
 *     slices, at most max_per_cluster lights a cell), on the job pool;
 *     lights are listed nearest first, so a full cell keeps its nearest
 *     (effects detail keeps fewer a cell on Low and Medium: cartwheel/quality.h);
 *   - each lit material (Lambert, Phong, Standard, Physical) draws with
 *     the "cluster" shading part (src/render/extensions/cluster_part.mjs):
 *     a fragment finds its cell and loops its lights with r186's own BRDF
 *     and distance attenuation (r186 ClusteredLightsNode's loop).
 *   - with shadow_atlas (the default) the shadow-casting ones are clustered
 *     too: each gets a tile of one depth atlas (a spot light one tile, an
 *     omni light a 3 x 2 block of cube faces), as big as the light is on
 *     screen and at most its shadow_size, packed largest first; a tile is
 *     redrawn only when its light or a shadow caster inside the light's
 *     range changed (shadow_cache), at most shadow_updates_per_frame faces a
 *     frame, the biggest lights first. The loop filters a light's tile with
 *     r186's PCF (five Vogel-disk taps, each a 2 x 2 compare) and its bias,
 *     normal bias, radius and intensity. Without shadow_atlas they stay on
 *     three.c's path, as r186 keeps them.
 * Directional, hemisphere and ambient lights stay on three.c's path. Beyond
 * r186: r186 clusters only shadowless point lights; spot lights here get SpotLightNode's cone in the same loop, and
 * are binned with a cone test (Filament's Froxelizer). A light with range 0
 * (r186: no cutoff, so in every cluster) gets the range where its intensity
 * falls under light_cutoff, and its falloff ends there as a range's does.
 * A light whose energy or colour is zero costs nothing.
 *
 * Costs: binning is O(cells each light reaches) on the CPU (about 0.4 ms
 * for 200 street lights at 1080p on the job pool); the GPU pays a cell lookup
 * per fragment plus each light of the fragment's cell. Uploads per frame: 64
 * bytes a light, 4 bytes a cell, 2 bytes a light reference. Materials with
 * an extension of their own (terrain) keep three.c's lights only. A mesh
 * whose material carries a shading part is drawn on its own (three.c does
 * not auto-instance it).
 *
 * Cascaded sun shadows (FORWARD or CLUSTERED): a DirectionalLight3D with
 * shadow and shadow_cascades 1-4 is three.js r186's CSMShadowNode: the view
 * from the camera's near to shadow_distance (r186's maxFar) split by r186's
 * practical split, each cascade an orthographic map of shadow_size fitted
 * around its slice with r186's width and texel snapping (no shimmer as the
 * camera moves), the four in one depth atlas, picked per fragment by depth
 * and cross-faded (cascade_fade, r186's fade). The sun leaves three.c's
 * list and the "csm" shading part draws it, with r186's PCF on its
 * cascade. A cascade is redrawn only when its matrix or a shadow caster
 * changed. The Shadows setting (cartwheel/quality.h) keeps 2 cascades on
 * Low and 3 on Medium, and the maps' size follows it as any shadow map's.
 *
 * Costs, shadows: each redrawn tile or cascade is a caster pass over the
 * scene (frustum culled); WebGPU also copies a redrawn atlas tile into an
 * R32Float texture (compatibility mode reads depth only through a
 * comparison sampler, which WGSL allows only outside the light loop). The
 * GPU pays 20 depth reads per shadowed light in a fragment's cell and 10
 * compare samples per fragment for the sun. Memory: the atlas
 * shadow_atlas_size^2 x 4 bytes (x 2 on WebGPU), the sun (2 x shadow_size)^2
 * x 4 bytes.
 *
 * The froxel grid is binned for the tree's camera once a frame: other
 * views of the same tree in that frame see the first view's clusters.
 *
 * Settings live per tree: cw_lighting_settings_of, or a LightingSettings
 * node in the scene (its properties are these fields). The debug command
 * `lighting` reports the stats. */
#ifndef CW_LIGHTING_H
#define CW_LIGHTING_H

#include "froxel.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_LIGHTING_FORWARD, CW_LIGHTING_CLUSTERED } cw_lighting_mode;

typedef struct cw_lighting_settings {
  cw_lighting_mode mode;     /* FORWARD */
  int max_lights;            /* 1024 (r186): clustered lights a frame; more are dropped (warned once) */
  int tile_px;               /* 32 (r186) */
  int z_slices;              /* 24 (r186) */
  int max_per_cluster;       /* 64 (r186), at most 255 */
  bool cluster_spot_lights;  /* true: shadowless spot lights are clustered too (beyond r186) */
  float cluster_far;         /* where the last slice ends; 0 = the camera's far */
  float light_cutoff;        /* 0.005: the intensity / distance^2 where a range-0 light ends */
  bool shadow_atlas;         /* true: shadowed omni and spot lights are clustered too, their shadows in one atlas */
  int shadow_atlas_size;     /* 4096: the atlas's texels a side (Shadows Low and Medium: at most 2048) */
  int shadow_updates_per_frame; /* 24: atlas faces redrawn a frame at most (an omni light is 6) */
  bool shadow_cache;         /* true: a tile is redrawn only when its light or a caster in its range changed */
  bool batching;             /* false: one draw a mesh (front to back). true: meshes that share geometry and
                              * material are one instanced draw under the cluster and csm parts
                              * (cw_shading_part_batching): far fewer draws, but the instances lose the
                              * front-to-back order, so the clustered shading runs on more hidden pixels.
                              * On the 890M (vis_city_lights, 1080p) 892 draws become 19, draw submission
                              * costs about the same and the scene pass 0.9 ms more GPU: on for a
                              * CPU-bound frame with many small meshes, off otherwise */
} cw_lighting_settings;

/* The tree's settings (created with the defaults above on first call). */
cw_lighting_settings *cw_lighting_settings_of(cw_tree *t);

typedef struct cw_lighting_stats {
  int clustered;             /* omni + spot lights shaded through clusters this frame */
  int clustered_spots;       /* of which spot lights */
  int forward;               /* omni / spot lights CLUSTERED mode left on three.c's path (shadowed ones) */
  int dropped;               /* clustered lights over max_lights */
  int materials;             /* materials drawing with the cluster part */
  float cpu_ms;              /* CPU time of the frame's lighting pass: gathering, binning, uploads (report only) */
  int cascades;              /* the cascaded sun's cascades (0: none) */
  int cascades_rendered;     /* of which re-rendered this frame (the rest were unchanged) */
  int shadowed;              /* clustered lights with an atlas tile */
  int shadow_faces_rendered; /* atlas faces redrawn this frame */
  int shadow_faces_cached;   /* atlas faces kept from earlier frames */
  cw_froxel_stats grid;      /* cells, references, overflow, bin time */
} cw_lighting_stats;
/* Stats of the last frame the tree's pipeline rendered (zeros before). */
void cw_lighting_get_stats(cw_tree *t, cw_lighting_stats *out);
/* The froxel grid the tree's clustered lights were binned into this frame
 * (NULL in FORWARD mode or before the first frame): its dimensions and slice
 * mapping for other passes that want the same cells (froxel.h). */
cw_froxel_grid *cw_lighting_froxels(cw_tree *t);

#ifdef __cplusplus
}
#endif
#endif

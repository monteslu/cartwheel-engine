/* Lighting modes for a tree's 3D scene: hundreds of omni and spot lights
 * through clustered shading.
 *
 * FORWARD (the default): every Light3D is three.c's (three.js r186's lights,
 * at most 8 of each kind light a surface; more are dropped with a warning).
 *
 * CLUSTERED: three.js r186's ClusteredLighting, on GLES3 and WebGPU alike.
 * Omni lights without a shadow (and spot lights without one, unless
 * cluster_spot_lights is off) leave three.c's per-material light list and
 * are shaded through clusters instead:
 *   - every frame the CPU bins them into a froxel grid over the view
 *     (cartwheel/froxel.h: r186's grid, tile_px tiles x z_slices exponential
 *     slices, at most max_per_cluster lights a cell), on the job pool;
 *     lights are listed nearest first, so a full cell keeps its nearest
 *     (effects detail keeps fewer a cell on Low and Medium: cartwheel/quality.h);
 *   - each lit material (Lambert, Phong, Standard, Physical) draws with
 *     the "cluster" shading part (src/render/extensions/cluster_part.mjs):
 *     a fragment finds its cell and loops its lights with r186's own BRDF
 *     and distance attenuation (r186 ClusteredLightsNode's loop).
 * Directional, hemisphere and ambient lights, and every shadow-casting
 * light, stay on three.c's path. Beyond r186: r186 clusters only point
 * lights; spot lights here get SpotLightNode's cone in the same loop, and
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

/* cartwheel/gi.h: reflections of the real surroundings (reflection probes)
 * and bounce light (a light probe grid).
 *
 * ReflectionProbe3D captures the scene around a point into a prefiltered
 * radiance map (three.js r186's PMREMGenerator.fromScene with its size,
 * position and renderTarget options) and every lit surface in its box
 * reflects that instead of the sky: a chrome ball shows the room's red
 * wall, a polished floor the furniture standing on it.
 *
 *   size            the box the probe affects, world units (axis aligned:
 *                   the node's rotation is ignored, its scale applies)
 *   origin_offset   where the capture is taken, from the box centre (Godot's
 *                   origin_offset: lift it off the floor, out of a pillar)
 *   box_projection  reflections are traced to the box's faces (r186's
 *                   getParallaxCorrectNormal) instead of being at infinity:
 *                   right for rooms and corridors, where the walls are near;
 *                   leave it off outdoors
 *   blend_distance  inside the box the probe's weight falls from 1 to 0 over
 *                   this distance at its faces, so overlapping probes and the
 *                   sky outside fade into each other (Godot's blending)
 *   intensity       multiplier of what it reflects
 *   ambient         the probe also gives the diffuse ambient light (its
 *                   blurriest level along the surface normal), in place of
 *                   the sky's
 *   resolution      the capture's face size in pixels (16 .. 1024, a power of
 *                   two; 128). The quality preset scales it (below).
 *   update_mode     ONCE (on its first frame, and again when the sky's
 *                   environment is re-rendered), ON_DEMAND (after
 *                   cw_reflection_probe_update / `call <probe> update`) or
 *                   EVERY_N_FRAMES (every `update_every` frames)
 *   near, far       the capture's clip range (0.1, 100)
 *   cull_mask       what the capture sees: objects whose three.c layers
 *                   share a bit with it (-1, all, by default). A probe can
 *                   see a cheap stand-in of a scene by giving the stand-in a
 *                   layer the view camera does not draw (t3_object_set_layers
 *                   on the camera and the stand-in): the capture draws what
 *                   the mask keeps whatever its layers.
 *
 * Shading. Every Standard and Physical material in a tree that has a probe
 * gets the "gi" shading part (cartwheel/shading.h): r186 TSL that samples up
 * to two probes per object, box projected, weighted per pixel, over the sky.
 * Each object takes the two probes whose boxes reach it (one containing its
 * centre first, then the smaller box). The app's program tables need the
 * xgi states its materials draw with (tools/programs.mjs adds them).
 *
 * Captures run in the pipeline's SHADOW phase (the "gi" pass, order 150:
 * after the sky's environment is made; it also draws the G-buffer of SSR and
 * SSGI, cartwheel/pipeline.h), at most a budget per frame (the quality preset's), the most overdue
 * first, then the nearest to the camera, then by node id. A capture renders
 * the scene six times at `resolution` and runs PMREM's filter passes; each
 * probe keeps two targets (it is drawn into one while the scene samples the
 * other), so nothing is allocated after its first capture. Render only:
 * nothing here reaches the simulation.
 *
 * Quality (effects detail): Low captures at half `resolution` (at least 32),
 * refreshes EVERY_N_FRAMES probes a quarter as often, and makes one capture a
 * frame; Medium at half resolution, half as often, one a frame; High as
 * authored, two a frame; Ultra at twice the resolution (up to 512), four a
 * frame.
 *
 * LightProbeGrid3D is three.js r186's LightProbeGrid: bounce light. A grid of
 * probes over a box (size; probes along x, y, z, 2 .. 32 each, 0 = one a
 * metre plus one), each rendering the scene into six cube_size faces (8
 * pixels) that an extension material ("giproj") projects onto L2 spherical
 * harmonics on the GPU (r186's LightProbeGenerator over the faces' texels,
 * solid-angle weighted), packed as r186 packs them (seven RGBA groups) into
 * a 2D atlas: three.c has no 3D textures, so probe (x, y, z)'s group g is
 * texel (z * nx + x, g * ny + y), read with bilinear filtering in x and y and
 * mixed by hand across z (14 fetches a pixel where r186 makes 7). The gi
 * shading part adds the irradiance at each lit surface (offset along the
 * normal by normal_offset probe spacings, r186's 0.5) to its light, as r186's
 * LightProbeGridNode does. update_mode BAKED captures the grid at load, in
 * 1 + bounces sweeps (each sweep sees the light the previous one stored, so
 * each adds a bounce; bake_per_frame spreads a sweep over frames, 0 = the
 * whole grid in one frame), and again when the sky's environment is
 * re-rendered; INCREMENTAL then keeps refreshing probes_per_frame probes
 * every frame (moving lights, opening doors). cw_light_probe_grid_rebake
 * (or `call <grid> rebake`) sweeps it again. Like r186's it has no
 * visibility term: light leaks through walls thinner than the probe spacing.
 * One grid lights a tree (the first in it). Cost per probe: six renders of
 * the scene at cube_size and seven one-texel draws. Quality: Low refreshes
 * nothing after the bake and caps faces at 8 pixels; Medium refreshes half of
 * probes_per_frame, High as authored, Ultra twice. */
#ifndef CW_GI_H
#define CW_GI_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_PROBE_UPDATE_ONCE, CW_PROBE_UPDATE_ON_DEMAND, CW_PROBE_UPDATE_EVERY_N_FRAMES } cw_probe_update;

/* Capture this probe again at the next opportunity (any update mode). */
void cw_reflection_probe_update(cw_node probe);
/* Capture every probe of a LightProbeGrid3D again (one sweep, at once). */
void cw_light_probe_grid_rebake(cw_node grid);

typedef struct cw_gi_stats {
  int reflection_probes;   /* ReflectionProbe3D nodes in the tree */
  int captures;            /* captures made in the last frame */
  int captures_total;      /* since the tree started */
  int materials;           /* materials carrying the gi part */
  int probes_bound_max;    /* the most probes one draw had bound in the last frame (0..2) */
  int grid_updates;        /* light probe grid probes captured in the last frame */
} cw_gi_stats;
/* The tree's GI counters (zeros for a tree without GI nodes). */
void cw_gi_get_stats(cw_tree *t, cw_gi_stats *out);

#ifdef __cplusplus
}
#endif
#endif

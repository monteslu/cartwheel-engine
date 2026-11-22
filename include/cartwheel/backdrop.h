/* cartwheel Backdrop: a whole 3D world of its own drawn BEHIND the scene, in
 * place of the scene's background, with an opacity: a concert stage behind a
 * rhythm game's note highway, a wall of fire, a sea at sunset. Two backdrops
 * cross-fade by their opacities while the game's own scene stays on top.
 *
 * WHAT IT DRAWS. A Backdrop node of the main scene owns a world, as a
 * SubViewport with `scene` does (cartwheel/subviewport.h): `scene` is a
 * .cwscene asset instanced into a tree of the node's own, with its own
 * camera, lights and shadows, WorldEnvironment, ProceduralSky, HeightFog
 * (volumetric fog included: up to three shadowed spot lights through haze),
 * FogVolume3D, particles, decals and water. Nothing in it lights or shadows
 * the main scene and nothing of the main scene shows in it. It ticks with the
 * main tree (also at opacity 0, so a faded-in world is where it would be).
 * `camera` names the Camera3D of the world it is seen through (a path from
 * the root of its scene, "Camera" for a child of it; empty: the world's
 * current camera). cw_backdrop_tree
 * and cw_backdrop_root give game code the world.
 *
 * HOW IT IS COMPOSITED. Each frame, before the main scene draws, every
 * backdrop with an opacity above 0 renders its world at the main scene's
 * own size (the full resolution: never a lower one) into a linear HDR
 * picture, through its own pipeline's passes up to the post chain: its
 * shadows, sky, its scene, decals, particles, fog and volumetric fog, and
 * the post passes that read its own depth (ambient occlusion, depth of
 * field, motion blur). The main scene then starts from those pictures
 * instead of clearing to its background: they are blended into its colour
 * target in `order` (lowest first, so the furthest back), each over what is
 * below by its opacity, over the main scene's own background (its clear
 * colour or sky) where the opacities leave some of it. The main scene draws
 * over that with its own depth, so its geometry, including transparent and
 * additive surfaces, is exactly what it is over its own background. The
 * main post chain (bloom, exposure, tone mapping, grading, antialiasing,
 * the lens) runs over the composite, so a bright stage light blooms and the
 * whole picture is tone mapped once. A backdrop's own PostProcess counts
 * for its exposure (its picture is scaled by its exposure over the main
 * scene's, so a world authored at exposure_ev -2 looks as it does alone)
 * and its depth-reading passes; its bloom, tone curve, grade and lens are
 * the main chain's. TAA is off in a backdrop (its history would be its
 * camera's); the main scene's TAA runs over the composite.
 *
 * COST. A backdrop costs what its world costs to draw at the screen's size,
 * plus one full-screen blend into the main scene's target (two, with the
 * main background under a partial opacity and a sky). Opacity 0 costs
 * nothing: its world is not culled, drawn or blended and nothing is
 * allocated, and with every backdrop at 0 the frame is the one without
 * them. A backdrop renders every frame on every preset (it is the
 * background); its world's effects follow the quality presets as the main
 * scene's do. Nothing is allocated per frame: each picture is one target
 * kept by its world's pipeline.
 *
 * SPLIT SCREEN. A backdrop is one picture behind the whole screen, from its
 * own camera, and each view shows its part of it: a stage behind every
 * player's lane, drawn once however many views there are. (A backdrop's
 * camera is not a player's, so a picture per view would be the same world
 * from the same camera at a narrower aspect: as many renders for no gain.)
 * A world that must follow each player's camera belongs in the main scene.
 *
 * WHAT DOES NOT SEE IT. A SubViewport's camera view of the main scene (a
 * monitor, a minimap) draws the world without the backdrop. The main
 * scene's own fog covers the backdrop as it covers its sky (HeightFog
 * sky_distance; 0 keeps the backdrop clear of it); the main sun disk is
 * part of the main background and fades with it.
 *
 * Debug command: `backdrop` (every backdrop: opacity, order, renders, the
 * passes its world ran last frame, CPU ms). Program tables: a backdrop's
 * world draws with the app's tables like everything else; run
 * tools/programs.mjs after adding one. */
#ifndef CW_BACKDROP_H
#define CW_BACKDROP_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backdrop properties:
 *   scene (string)              a .cwscene asset: the world it draws
 *   camera (string)             the world's Camera3D (path from the root of its scene; empty: its current camera)
 *   opacity (float, 1)          0 (not drawn, free) to 1 (covers what is below it)
 *   order (int, 0)              draw order among the backdrops: lower is further back
 *   renders (int, read-only)    pictures rendered so far
 * At most CW_BACKDROP_MAX backdrops with an opacity above 0 are drawn in a
 * frame (the first by order; the rest are skipped, logged once). */
#define CW_BACKDROP_MAX 8

/* Set the opacity (clamped to 0..1). Same as setting the property. */
void cw_backdrop_set_opacity(cw_node backdrop, float opacity);
/* The opacity (0 for a node that is not a Backdrop). */
float cw_backdrop_opacity(cw_node backdrop);
/* Set the draw order (lower is further back; ties keep the tree order). */
void cw_backdrop_set_order(cw_node backdrop, int order);
/* The backdrop's world (`scene` set, or made on this call so code can build
 * the world itself: add nodes, a Camera3D among them, under cw_tree_root of
 * it). NULL for a node that is not a Backdrop. Owned by the node. */
cw_tree *cw_backdrop_tree(cw_node backdrop);
/* The root of the instanced `scene` in that tree (null handle when none). */
cw_node cw_backdrop_root(cw_node backdrop);
/* Pictures rendered so far (0 for a node that is not a Backdrop). */
int cw_backdrop_renders(cw_node backdrop);
/* CPU milliseconds its last render took to issue (a profiling figure from
 * the host clock: never feed it into game state). */
float cw_backdrop_last_cpu_ms(cw_node backdrop);

#ifdef __cplusplus
}
#endif
#endif

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
 * REFLECTIONS. With `environment` on, a backdrop's world is the main
 * scene's image-based light, as a ProceduralSky with `environment` is: every
 * glossy and rough Standard / Physical surface of the scene reflects it (a
 * dark glass road shows the stage's lamps and lasers). Its world is captured
 * as a cube from its camera's position (`env_resolution` texels a face, 128
 * by default; linear HDR), by its world's renderer, after its picture each
 * frame (the world's lights and shadow maps as the picture has them); the six
 * faces of a capture are spread over `env_every` frames (4 by default; the
 * lower effects presets spread them wider: Very Low x8, Low x4, Medium x2),
 * the faces in all but the last of them and the filter in the last, and the
 * scene reflects the last whole capture. The captures of every backdrop
 * with environment on are blended, each by the share of the picture it has
 * (its opacity times what the backdrops in front of it leave, times its
 * exposure over the main scene's), over the main scene's own environment by
 * the share of the background they leave (so a cross-fade fades the
 * reflections as it fades the pictures, and a backdrop at 0.5 over the
 * scene's own sky reflects half of each); the blend is prefiltered with the
 * engine's environment filter (three.js r187's PMREMGenerator) into the
 * scene's environment when a capture finishes (a share that moves, in a
 * cross-fade, is taken up then: the reflections follow a fade every
 * env_every frames). The scene's own environment joins the blend only at a
 * share of 1/256 or more (the last frames of a fade into a world that lights
 * the scene leave it a share no reflection shows, and bringing it in and out
 * cost a filter each). A backdrop lighting the scene alone at its whole share
 * keeps the filter of its capture (its own filtered target, about 4 MB at the
 * filter's 256 cube; the prewarm makes the first): when its world lights the
 * scene alone again with no capture of it finished since (environment turned
 * off for a cross-fade into it and on again at its end), the scene takes that
 * back instead of filtering, and the next capture to finish refreshes it.
 * While no backdrop lights the scene its own environment is back as it was.
 * The capture draws the world's surfaces, sky, lights and shadows; what its
 * pipeline adds as post passes (volumetric fog's beams, decals and particles
 * drawn as passes) is in its picture but not in its reflection. A backdrop
 * with environment off (the default), or at opacity 0, adds nothing: no
 * capture, no pass, the frame as it was. The main scene's
 * environment_intensity scales the whole blend; its environment_rotation is
 * drawn into its own environment's share (the scene's rotation reads 0 while
 * backdrops light it).
 *
 * Sharp reflections of what the backdrop shows on screen come from the
 * screen-space reflections (cw_post_settings.ssr with ssr_backdrop): a ray
 * that finds nothing of the scene on screen goes on toward where its
 * direction lands on screen and, when nothing of the scene stands in the
 * way, takes the backdrops' pictures there (blended at half size). So a near
 * mirror shows a laser or a spot lamp sharp where the environment's cube is
 * soft. As with every screen-space reflection (three.js r187's SSR adds over
 * the lit image) it adds over the environment's reflection: a hit takes the
 * scene, else a ray to the backdrop takes the picture, else only the
 * environment's reflection in the lit image is there.
 *
 * COST of an environment (docs/guides/rendering-3d.md has the 890M's figures):
 * the faces of a capture cost what the world costs to draw at 128 x 128, six
 * times over env_every frames; each finished capture costs a blend (with
 * several sources) and the prefilter (three.js r187's GGX filter, which makes
 * a 256 cube at least), in a frame of its own.
 *
 * WHAT DOES NOT SEE IT. A SubViewport's camera view of the main scene (a
 * monitor, a minimap) draws the world without the backdrop. The main
 * scene's own fog covers the backdrop as it covers its sky (HeightFog
 * sky_distance; 0 keeps the backdrop clear of it); the main sun disk is
 * part of the main background and fades with it.
 *
 * Debug command: `backdrop` (every backdrop: opacity, order, renders, the
 * passes its world ran last frame, CPU ms, its environment's face size, frames
 * a capture takes and captures; the tree's environment: set, updates). Program tables: a backdrop's
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
 *   prewarms (int, read-only)   prewarm renders so far (cw_backdrop_prewarm)
 *   environment (bool, false)   its world lights the scene (REFLECTIONS above)
 *   env_resolution (int, 128)   the capture's face size in texels (16 to 512, a power of two)
 *   env_every (int, 4)          frames a capture's six faces are spread over (1: all six every frame)
 *   env_captures (int, read-only) environment captures finished so far
 *   env_whole (int, read-only)  captures made whole (six faces in one frame)
 *                               while shown; environment turned off keeps
 *                               the capture, so turning it on makes none
 *   env_filters (int, read-only) environment filters run while it lit the scene
 *   env_kept_uses (int, read-only) times the scene took its kept filter back
 *                               instead of filtering (REFLECTIONS above)
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
/* Prewarm: draw the backdrop's world once at the next frame of its tree, whatever its opacity and with its hidden root
 * shown for that render (nothing of it reaches the screen: its picture is blended at opacity 0, and drawn once as a world cut in at
 * opacity 1 draws it before the frame's background covers it), so everything its
 * first shown frame would build is built then: its world's programs and pipelines (shadows, sky, fog, particles,
 * decals), its picture and the targets of its passes, its particle buffers and fog grids, and the tree's backdrop
 * pass. Its first frame shown then costs what any later frame does. A scene app (cw_scene_app) prewarms every
 * Backdrop of its tree by itself at the end of its warm-up, under the loading screen (and at the end of each loading
 * phase, cw_loading_begin); call this for a backdrop added, or a world built, after that (behind a loading screen of
 * the game's own, or a frame before it fades in). What it built is kept while the backdrop is at opacity 0, which
 * costs nothing per frame. With `environment` on, the prewarm also makes a whole capture of its world (the six
 * faces), blends and filters it as the tree's environment would be (into targets nothing reads), filters it alone into
 * the backdrop's kept filter (REFLECTIONS above) and draws the tree's
 * scene once lit by it, every object shown, into small targets of its own: its first shown frame then draws only its
 * share of the next capture, as any later frame does, and lights the scene from the prewarm's capture until that one
 * is done (env_every frames). The scene's environment is not touched. false for a node that is not a Backdrop. */
bool cw_backdrop_prewarm(cw_node backdrop);
/* Prewarm renders made so far (0 for a node that is not a Backdrop). */
int cw_backdrop_prewarms(cw_node backdrop);

#ifdef __cplusplus
}
#endif
#endif

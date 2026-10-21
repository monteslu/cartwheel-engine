/* 3D rendering for the scene tree: each tree owns a three.c scene and
 * renderer; Node3D objects attach under it, and the 3D render node types
 * (Camera3D, MeshInstance3D, lights, WorldEnvironment) configure it.
 *
 * Node types registered here:
 *   Camera3D          fov, near, far, current (the first current camera renders)
 * Every node type's properties (type, default, doc) are in docs/nodes.json and
 * answer to the debug channel: `types`, `props <path>`.
 *
 * Conventions: Y is up, -Z is forward. Cameras look down their -Z. Lights
 * (directional, spot) shine down their -Z, NOT at the origin as three.js's
 * default target does: aim them with rotation or cw_node3d_look_at (which
 * points -Z at the target for every Node3D).
 *
 *   MeshInstance3D    primitive (NONE, BOX, SPHERE, PLANE, CYLINDER) with size:
 *                     BOX x/y/z extents; SPHERE size.x = diameter; CYLINDER
 *                     size.x = diameter along Y, size.y = height; PLANE x by z,
 *                     facing +Y. Also color,
 *                     metalness, roughness, emissive, cast_shadow, receive_shadow,
 *                     mesh (a resource uid, when a mesh resource is set it wins)
 *   DirectionalLight3D, OmniLight3D, SpotLight3D   color, energy, shadow, range...;
 *                     energy is 0 to 100000 (the sun's real illuminance is
 *                     about 100000 lux); a value outside is clamped, and the
 *                     first clamp of a light logs a warning
 *                     a directional shadow covers shadow_extent metres around
 *                     the light (move the light with the player for sharp
 *                     shadows over a big level), shadow_bias / shadow_normal_bias
 *   WorldEnvironment  background, ambient color/energy, fog (linear colours;
 *                     tone curve + exposure live in PostProcess)
 *   PostProcess       the render pipeline's settings (pipeline.h): HDR,
 *                     MSAA, exposure, tone curve,
 *                     bloom, SSAO, TAA, FXAA/SMAA, LUT, lens effects
 *   ProceduralSky, HeightFog   sky / day-night, fog (environment.h)
 *   LightmapGI        baked lightmaps and SH probes (lightmap.h)
 *   MeshInstance3D also carries visibility properties (static, lod_bias,
 *                     visibility_range, occlusion_cull, impostor...): see
 *                     visibility.h (VisibilityManager, Room3D, Portal3D).
 *                     Primitives are unit shapes shared per tree and scaled
 *                     to `size`, so same-shaped meshes instance.
 * Effects node types live in particles.h (GPUParticles3D, GPUParticles2D,
 * ParticleAttractor3D, ParticleCollider3D, Trail3D) and decals.h (Decal3D).
 *
 * Which lights three.c draws and which the engine clusters: the tree's
 * lighting mode (cartwheel/lighting.h, a LightingSettings node). FORWARD,
 * the default, leaves every light to three.c; CLUSTERED shades shadowless
 * omni and spot lights through r186 ClusteredLighting's clusters, hundreds
 * at a time.
 *
 * Drawing goes through the tree's render pipeline (pipeline.h), as three.js
 * r186 draws: linear colours, physical light units, linear HDR.
 */
#ifndef CW_RENDER3D_H
#define CW_RENDER3D_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attach a procedural mesh to a MeshInstance3D for visibility/BVH support.
 * Retains mesh as a child of the node; preserves its local transform, materials
 * and shadow flags. While attached, primitive/resource/look properties are
 * ignored. NULL restores property-driven geometry. No automatic LOD chain is
 * inferred. Returns false for another node type or an already-parented mesh. */
bool cw_mesh3d_set_procedural(cw_node node, t3_mesh *mesh);

/* The tree's three.c scene, created on first use and owned by the tree (freed with it). */
t3_scene *cw_render3d_scene(cw_tree *t);
t3_renderer *cw_render3d_renderer(cw_tree *t); /* NULL before the first draw */
/* A three.c renderer an app drives itself (t3_renderer_render, no scene
 * tree), on whichever GPU API this run uses: t3_renderer_new, plus three.c's
 * WebGPU backend on a WebGPU run (a dual cart must not touch GL once the
 * host selected WebGPU). Draw to the window with cw_render3d_window_target. */
t3_renderer *cw_render3d_window_renderer(int w, int h);
/* Before each t3_renderer_render of a cw_render3d_window_renderer that draws
 * to the window: point it at this frame's window (the frame texture on
 * WebGPU; nothing to do on GL, where it draws to the default framebuffer). */
void cw_render3d_window_target(t3_renderer *r);
/* Render the tree's 3D scene through its current camera into the bound
 * framebuffer (or the renderer's), w x h pixels. Returns false (and creates
 * nothing) when the tree has no current Camera3D. */
bool cw_render3d_draw(cw_tree *t, int w, int h);
/* What cw_render3d_draw does before it renders: create or resize the
 * renderer to w x h and update the current camera's projection. Returns the
 * camera to render with, NULL (creating nothing) when the tree has no
 * current Camera3D. The render pipeline (pipeline.h) calls this, then
 * renders into its own targets. */
t3_camera *cw_render3d_prepare(cw_tree *t, int w, int h);
/* explicit camera if still valid, else the first current Camera3D (O(nodes) walk); null when none */
cw_node cw_render3d_camera(cw_tree *t);
/* overrides the current Camera3D; a null handle reverts to the search */
void cw_render3d_set_camera(cw_tree *t, cw_node camera);
t3_camera *cw_camera3d_t3(cw_node camera); /* owned by the node; NULL when not a Camera3D */

typedef struct cw_render3d_stats {
  int draw_calls, triangles, programs;
  float cpu_ms;
  int skipped; /* draws three.c could not make since the tree began (each reason logged once): should be 0 */
} cw_render3d_stats;
/* renderer counters from the last draw; zeroed before the first render.
 * cpu_ms: CPU milliseconds the last frame spent issuing its 3D (the render
 * pipeline from its start to the overlay phase, or cw_render3d_draw):
 * submission only, the GPU is not waited for. */
void cw_render3d_get_stats(cw_tree *t, cw_render3d_stats *out);

#ifdef __cplusplus
}
#endif
#endif

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
 * omni and spot lights through r187 ClusteredLighting's clusters, hundreds
 * at a time.
 *
 * Drawing goes through the tree's render pipeline (pipeline.h), as three.js
 * r187 draws: linear colours, physical light units, linear HDR.
 */
#ifndef CW_RENDER3D_H
#define CW_RENDER3D_H

#include "render2d.h"
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

/* Reorder an indexed geometry's triangles for the GPU's post-transform vertex cache (meshoptimizer's
 * optimizeVertexCache, Forsyth-style): the same triangles and vertices, an order in which each vertex is
 * shaded fewer times. Meshes exported in modelling order often run the vertex shader two or three times a
 * vertex; for a crowd of thousands of instanced characters that is most of its vertex cost. Opaque meshes draw
 * the same picture (depth decides what shows; only exact depth ties between a mesh's own triangles can
 * differ); a transparent mesh drawn without sorting changes its blend order. Call once after building the
 * geometry, before its first draw. acmr_before / acmr_after (either may be NULL): vertices shaded per triangle
 * for a 16-entry cache, before and after (report only). false (nothing changed) without an index or with an
 * index past the vertices. Opt in: nothing calls it for you. */
bool cw_geometry_optimize_vertex_cache(t3_geometry *g, float *acmr_before, float *acmr_after);

/* The tree's three.c scene, created on first use and owned by the tree (freed with it). */
t3_scene *cw_render3d_scene(cw_tree *t);
t3_renderer *cw_render3d_renderer(cw_tree *t); /* NULL before the first draw */
/* A three.c renderer an app drives itself (t3_renderer_render, no scene
 * tree), on whichever GPU API this run uses: t3_renderer_new, plus three.c's
 * WebGPU backend on a WebGPU run (a dual cart must not touch GL once the
 * host selected WebGPU). Draw to the window with cw_render3d_window_target. */
t3_renderer *cw_render3d_window_renderer(int w, int h);
/* Before a t3_renderer_render into a 2D render target (cw_target_create with
 * depth; null handle = the window, as cw_render3d_window_target): the target
 * then draws as a sprite the right way up, on GL and WebGPU. Size the renderer
 * to the target first (t3_renderer_set_size) and back to the window after. */
void cw_render3d_texture_target(t3_renderer *r, cw_texture target);
/* A 2D render target (cw_target_create) as a three.c texture, for any material's map (t3_material_set_texture with
 * T3_MAP, T3_EMISSIVE_MAP, ...): card faces, signs, a score board drawn with the 2D renderer and worn by any number of
 * meshes, no SubViewport per face.
 *
 *   cw_texture face = cw_target_create(&(cw_target_desc){ .width = 256, .height = 384, .filter = CW_FILTER_LINEAR });
 *   cw_r2d_begin(&(cw_r2d_pass_desc){ .target = face, .clear = true, .clear_color = CW_WHITE });
 *   ... the card's art, text ...
 *   cw_r2d_end();                                    // the 3D texture follows (below)
 *   t3_material_set_texture(card_mat, T3_MAP, cw_target_texture3d(face));
 *
 * The texture is a linear-light copy of the target (the 2D renderer stores display-encoded, premultiplied colour;
 * three.c samples an embedder's texture as it is): alpha kept and divided out of the colour, rows the way three.c
 * reads a texture's v, so the picture lies upright on a plane or a box face whichever renderer drew it. The copy is
 * refreshed at the end of every 2D pass into the target (cw_r2d_end) and by cw_target_texture3d_refresh (after three.c
 * or a pipeline drew into it); a pass in a scene app's render shows in 3D from the next frame, a pass in update or
 * init the same frame. Owned by the target: it follows cw_target_resize (from the next refresh) and goes with
 * cw_texture_destroy, so a material must not outlive it. NULL (logged) for a handle that is not a live target.
 * Costs: one copy at the target's size (RGBA16F where the host renders half floats, else RGBA8) and one fullscreen
 * pass per refresh; no program state of its own (a material wearing it draws as with any map: "+map"). Debug fields render.target_texture3d.refreshes and
 * render.target_texture3d.targets. */
t3_texture *cw_target_texture3d(cw_texture target);
/* Refresh the 3D copy of a target that has one (after three.c or a pipeline drew into it); nothing for another. */
void cw_target_texture3d_refresh(cw_texture target);
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

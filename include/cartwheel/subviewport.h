/* cartwheel SubViewport: a node that renders a picture into a texture, and
 * the ways to show that texture: on a 3D material, on a Sprite2D or any other
 * 2D node, and on a UI control (TextureRect, NinePatchRect).
 *
 * WHAT IT RENDERS. One of two things:
 *
 *   A camera of this world (`camera` = a node path to a Camera3D of the
 *   same tree): a security monitor, a rear-view mirror, a minimap from
 *   above, a portal's far side. The whole render pipeline draws the view
 *   (lights and shadows, sky, fog, particles, decals, the post chain and
 *   tone mapping), without the 2D canvas and the HUD.
 *
 *   A world of its own (`scene` = a .cwscene asset): a character portrait,
 *   a 3D model turning inside a menu panel, a 2D canvas drawn off screen.
 *   The scene is instanced into a tree the SubViewport owns, with its own
 *   three.c scene, cameras, lights, WorldEnvironment, PostProcess and 2D
 *   canvas; nothing in it shows in the main picture and no light crosses.
 *   It ticks with the main tree. cw_subviewport_tree gives the tree and
 *   cw_subviewport_root the instanced scene's root, so game code can turn
 *   the model or swap it; node paths do not cross between the two trees.
 *
 * WHEN. `update_mode`: ALWAYS (each frame), WHEN_VISIBLE (each frame
 * something showed the texture: a 2D node or control drew it, or a mesh that
 * wears it was inside a camera's view), ONCE (one render, then it holds the
 * picture; cw_subviewport_request_update asks for another), DISABLED.
 * `update_every` spreads the cost: 2 renders every other frame. On the Low
 * and Very Low effects presets (cartwheel/quality.h) ALWAYS and WHEN_VISIBLE
 * viewports render every second and every third due frame.
 *
 * TIMING. A world of its own is rendered just before the frame that shows
 * it. A camera of this world is rendered right after the frame's own
 * picture, reusing that frame's shadow maps, light clusters and probe
 * captures as a splitscreen view does, so the texture shows the previous
 * frame's view (16 ms late: fine for a monitor or a minimap; a portal seen
 * while the camera turns fast lags by that frame). The main picture is
 * exactly what it would be without the SubViewport, apart from the surfaces
 * that show the texture.
 *
 * THE TEXTURE. RGBA8, the size of `size`, display colours (tone mapped,
 * sRGB), opaque. Ways to use it:
 *   scene files  any `texture` property of a 2D node or UI control takes
 *                "viewport://Name" (Name is the SubViewport's `texture_name`,
 *                or its node name when that is empty; names are global);
 *                a MeshInstance3D takes it in `viewport_texture`, as its
 *                emissive map by default (a screen that glows by itself) or
 *                as its colour map (`viewport_slot = COLOR`: lit by the
 *                scene's lights like paint)
 *   C, 2D / UI   cw_subviewport_texture: a cw_texture for cw_r2d_sprite,
 *                cw_ui_draw_texture, cw_texture_rect_set_texture
 *   C, 3D        cw_subviewport_t3_texture: a three.c texture for any
 *                material map (t3_material_set_texture)
 * 2D and UI draw the render target itself. A 3D material samples a copy
 * decoded to linear light (half float where the host renders it), made after
 * each render once a material has asked for the texture: one small
 * full-screen pass.
 *
 * LIMITS. A camera view runs every pass of the pipeline again at the
 * viewport's size. Passes that keep one set of targets per tree for a view's
 * size (water reflections and refractions) re-allocate when the sizes
 * differ, each frame both draw; keep water out of a camera view's sight or
 * give it its own world. Volumetric fog keeps a grid for up to 3 camera
 * views besides the main one (fewer in splitscreen); a view past that is
 * drawn without it. TAA never runs in a SubViewport. A mesh that wears a
 * viewport's texture and is seen by that same viewport's camera shows the
 * previous frame's picture (a monitor in a monitor), which is correct.
 *
 * COST. See docs/guides/rendering-3d.md, "SubViewport": a camera view costs
 * about what a splitscreen view of its size costs, less the shadow maps; a
 * world of its own costs its own content. Nothing is allocated per frame.
 *
 * Debug command: `subviewport [<node>]` (size, mode, renders, last CPU ms,
 * whether the texture was shown last frame).
 */
#ifndef CW_SUBVIEWPORT_H
#define CW_SUBVIEWPORT_H

#include "render2d.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SubViewport properties:
 *   size (vec2, 512 x 512)       the texture's size in pixels (1 to 4096)
 *   update_mode (enum, ALWAYS)   DISABLED, ONCE, ALWAYS, WHEN_VISIBLE
 *   update_every (int, 1)        frames between renders (ALWAYS, WHEN_VISIBLE)
 *   camera (string)              node path to a Camera3D of this tree
 *   scene (string)               a .cwscene asset: a world of its own (wins over camera)
 *   texture_name (string)        the name in "viewport://Name"; empty: the node's name
 *   renders (int, read-only)     renders so far
 * Signal: rendered (after each render). */

typedef enum {
  CW_SUBVIEWPORT_DISABLED, CW_SUBVIEWPORT_ONCE, CW_SUBVIEWPORT_ALWAYS, CW_SUBVIEWPORT_WHEN_VISIBLE
} cw_subviewport_update;

/* The picture as a 2D texture (a render target: draw it as a sprite or a UI
 * image, the right way up). Asking counts as showing it (WHEN_VISIBLE).
 * Null handle for a node that is not a SubViewport or before the GPU exists.
 * Owned by the node; a new handle after `size` changes. */
cw_texture cw_subviewport_texture(cw_node viewport);
/* The picture as a three.c texture for a material map (linear light, as
 * three.c's materials expect of a colour or emissive map). Borrowed: the
 * node owns it, and it stays the same object when `size` changes. NULL as
 * above. */
t3_texture *cw_subviewport_t3_texture(cw_node viewport);
/* The viewport's own tree (`scene` set, or made on this call so code can
 * build the world itself: add nodes under cw_tree_root of it). NULL for a
 * node that is not a SubViewport. Owned by the node. */
cw_tree *cw_subviewport_tree(cw_node viewport);
/* The root of the instanced `scene` in that tree (null handle when none). */
cw_node cw_subviewport_root(cw_node viewport);
/* Render once more at the next frame, whatever the mode but DISABLED: the
 * way to refresh a ONCE viewport after its content changed. */
void cw_subviewport_request_update(cw_node viewport);
/* Renders so far (0 for a node that is not a SubViewport). */
int cw_subviewport_renders(cw_node viewport);
/* CPU milliseconds the last render took to issue (the pipeline run for a
 * world of its own, the extra view for a camera; a profiling figure from the
 * host clock: never feed it into game state). */
float cw_subviewport_last_cpu_ms(cw_node viewport);
/* The SubViewport a "viewport://Name" reference (or a bare name) names, or a
 * null handle. O(viewports). */
cw_node cw_subviewport_find(const char *ref);

#ifdef __cplusplus
}
#endif
#endif

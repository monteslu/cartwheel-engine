/* cartwheel 3D picking and projection: which piece is under the mouse or a finger, and where a 3D point lies on
 * the canvas.
 *
 *   const cw_raw_input *in = cw_input_raw();                           (the mouse; a finger with touch_pointer)
 *   cw_vec2 at = cw_r2d_window_to_canvas((cw_vec2){ in->mouse_x, in->mouse_y });
 *   cw_pick3d_hit hit;
 *   if (cw_pick3d_canvas(tree, at, &(cw_pick3d_filter){ .group = "pieces" }, &hit, 1, NULL))
 *     select(hit.node);                                                 (the piece, however many meshes it has)
 *
 *   float depth;
 *   cw_vec2 label = cw_camera3d_world_to_screen(cam, view_rect, piece_top, &depth);   (a label over a piece)
 *
 * Coordinates are canvas pixels as the 2D side has them (render2d.h): origin top left, y down. A view is a camera
 * and the rectangle its picture fills on the canvas: the whole canvas for a single camera, a player's part in split
 * screen (cw_canvas_views3d gives them as the pipeline draws them, the same rectangles as cw_shell_player_rect), or
 * a SubViewport's own pixels (cw_subviewport_view3d) for a picture the game shows itself, where the game maps the
 * pointer into that picture first. The lens follows the camera's properties at the rectangle's aspect, as the
 * renderer draws it: `fov` for PERSPECTIVE, `size` for ORTHOGONAL (Camera3D `projection`), near and far.
 *
 * Every call reads the nodes as they stand now (their positions, rotations, scales, visibility and layers): a piece
 * moved this tick is hit where it now is, with no frame drawn in between, so picking works in fixed_update, in
 * update and in render alike. Reading them writes them into their three.c objects as the next frame's pre_render
 * would; an interpolated node shows its tick position until that frame interpolates it again (render-only).
 *
 * What is tested: every mesh (MeshInstance3D primitives, glTF meshes, a procedural mesh, CSG results) and every
 * instance of a MultiMeshInstance3D or three.c InstancedMesh, triangle by triangle as three.js r187's Raycaster
 * does (a material's side decides whether back faces count). Skinned meshes, lines, points and sprites are not
 * tested: give an animated character a collision shape or an invisible proxy mesh on its own layer. A node hidden
 * by itself or an ancestor is skipped unless the filter asks for hidden ones. A static batch (MeshInstance3D
 * `batch`) is tested through its own mesh, as if it were not batched.
 *
 * Cost: the tree is walked once per call and three.c's raycaster tests only the meshes whose bounding spheres the
 * ray meets. Main thread only. Guide: docs/guides/rendering-3d.md#picking-and-projection. */
#ifndef CW_PICK3D_H
#define CW_PICK3D_H

#include "render2d.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_ray3d {
  t3_vec3 origin;    /* the camera's position (PERSPECTIVE) or the point on the near plane (ORTHOGONAL) */
  t3_vec3 direction; /* unit length, into the picture */
} cw_ray3d;

/* A camera and where its picture lies (canvas pixels, or a SubViewport's pixels). */
typedef struct cw_view3d {
  cw_node camera;
  cw_rect rect;
} cw_view3d;

/* ── projection (any Camera3D, either projection) ─────────────────────── */

/* world -> screen: the point's place in `viewport` (pixels, top left origin, y down). depth (may be NULL): metres
 * in front of the camera along its view direction; 0 or less is behind the camera, where the place is meaningless.
 * A point outside the rectangle is still placed (off the view's edge). Not a Camera3D: (0, 0), depth 0. */
cw_vec2 cw_camera3d_world_to_screen(cw_node camera, cw_rect viewport, t3_vec3 world, float *depth);
/* screen -> world: the point under `screen` that lies `depth` metres in front of the camera (the inverse of
 * cw_camera3d_world_to_screen). */
t3_vec3 cw_camera3d_screen_to_world(cw_node camera, cw_rect viewport, cw_vec2 screen, float depth);
/* The ray through `screen`: every point it passes projects to that place. */
cw_ray3d cw_camera3d_screen_ray(cw_node camera, cw_rect viewport, cw_vec2 screen);

/* ── the canvas's views ──────────────────────────────────────────────── */

/* The views the canvas shows: the pipeline's split-screen views (cw_shell_split, cw_pipeline_set_views) with their
 * rectangles in canvas pixels, else the tree's current camera over the whole canvas. Returns the count (written up
 * to cap; 0 when the tree has no camera). The canvas is the window's last 2D pass (cw_r2d_window_to_canvas's
 * canvas: the virtual canvas when the app sets one), the window's logical size before the first frame. */
int cw_canvas_views3d(cw_tree *t, cw_view3d *out, int cap);
/* The view under a canvas point: its index (and *out, may be NULL), -1 when the point is in none (the fourth
 * quadrant of a three-player split, outside the canvas). */
int cw_canvas_view3d_at(cw_tree *t, cw_vec2 canvas_px, cw_view3d *out);
/* A SubViewport that draws a camera (its `camera` property, or the current camera of a world of its own): that
 * camera, and the rectangle {0, 0, size} in the SubViewport's own pixels. false when it draws no 3D camera. */
bool cw_subviewport_view3d(cw_node subviewport, cw_view3d *out);

/* ── picking ─────────────────────────────────────────────────────────── */

typedef struct cw_pick3d_filter {
  uint32_t layers;      /* Node3D render layers to test (bit n = layer n); 0 = every layer */
  const char *group;    /* only hits under a node of this group (cw_node_add_group); the hit's node is that member, so
                         * a piece built of several meshes is one hit. NULL = any node */
  bool all;             /* every hit along the ray, nearest first (else the nearest one only) */
  float max_distance;   /* along the ray, metres; 0 = no limit */
  bool hidden;          /* also hit hidden nodes */
  cw_node exclude;      /* a subtree never hit (the cursor's own marker, the piece being dragged); null = none */
} cw_pick3d_filter;

typedef struct cw_pick3d_hit {
  cw_node node;         /* the nearest node owning the mesh hit (or the group member, with a group) */
  t3_object *object;    /* the three.c mesh hit (owned by the scene) */
  int instance;         /* the instance of a MultiMesh / InstancedMesh, -1 for a plain mesh */
  int face;             /* the triangle's index in its geometry */
  float distance;       /* along the ray, metres */
  t3_vec3 point;        /* where, world space */
  t3_vec3 normal;       /* the triangle's own normal (not the shading normal), world space, unit length, turned to
                         * face back along the ray */
} cw_pick3d_hit;

/* Hits of a world-space ray in the tree's 3D scene: the count written to hits (at most cap; 1 at most unless
 * filter->all), nearest first. filter NULL = every layer, nearest only. */
int cw_pick3d_ray(cw_tree *t, cw_ray3d ray, const cw_pick3d_filter *filter, cw_pick3d_hit *hits, int cap);
/* Through a camera: the ray under `screen` in `viewport`, tested in the camera's own tree (a SubViewport's world of
 * its own picks its own nodes). */
int cw_pick3d_screen(cw_node camera, cw_rect viewport, cw_vec2 screen, const cw_pick3d_filter *filter,
                     cw_pick3d_hit *hits, int cap);
/* Through whichever view of the canvas lies under canvas_px (cw_canvas_view3d_at): split screen picks in the
 * player's own picture. *view (may be NULL): that view's index, -1 when the point is in no view (no hits then). */
int cw_pick3d_canvas(cw_tree *t, cw_vec2 canvas_px, const cw_pick3d_filter *filter, cw_pick3d_hit *hits, int cap,
                     int *view);

#ifdef __cplusplus
}
#endif
#endif

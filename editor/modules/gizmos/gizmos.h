/* cartwheel editor: the gizmo context and the gizmo toolkit.
 *
 * cwed/editor.h declares the gizmo registry (cwed_gizmo_register) with an
 * opaque struct cwed_gizmo_ctx; this header defines it. It is the one
 * header of a module that other modules include: a module that registers
 * gizmos (lights, shapes, a tilemap brush, an animation path) includes
 * "../gizmos/gizmos.h". The viewports (modules viewport3d / viewport2d)
 * fill a context every frame and call, for every node of the active scene
 * whose type matches a registered gizmo:
 *
 *   draw(node, ctx)          every frame (ctx->selected says whether the
 *                            node is selected): visualizers (light ranges,
 *                            camera frustums, collision shapes) draw for
 *                            every node, edit handles for selected ones;
 *   input(node, ctx, event)  pointer events over the viewport, for SELECTED
 *                            nodes only, before the viewport's own picking
 *                            and camera; return true to take the event. A
 *                            gizmo that takes a press sets ctx->capture to
 *                            itself and gets every move until the release.
 *
 * Drawing. Two kinds of line:
 *   - world lines (3D: cwed_gizmo_line3) are three.c LineSegments on the
 *     editor layer (CWED_LAYER_EDITOR): depth tested against the scene,
 *     never in the game's camera, never saved;
 *   - overlay shapes (cwed_gizmo_overlay_*) are drawn on top of the
 *     viewport image in viewport pixels with the UI's anti-aliased shapes:
 *     handles, the transform gizmo, 2D outlines.
 * Positions in 2D are WORLD (canvas) units; cwed_gizmo_to_view2 maps them.
 *
 * Edits. Gizmos change the scene only through commands (node.set with a
 * merge key, node.set_each for several nodes): a drag is one undo step
 * (the core seals the merge run on the pointer release). Snapping follows
 * ctx->snap and the steps in the context. */
#ifndef CWED_GIZMOS_H
#define CWED_GIZMOS_H

#include "cartwheel/render2d.h"
#include "cwed/editor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* three.c layer bit for editor-only objects (grid, visualizer lines): the
 * editor camera sees layer 0 and this one, game cameras only layer 0. */
#define CWED_LAYER_EDITOR (1u << 20)

typedef enum { CWED_TOOL_SELECT, CWED_TOOL_MOVE, CWED_TOOL_ROTATE, CWED_TOOL_SCALE, CWED_TOOL_COUNT } cwed_tool;

struct cwed_gizmo_ctx {
  cwed_doc *doc;
  bool is_3d;
  bool selected;          /* the node drawn / driven is selected */
  /* the viewport, in pixels (the target the scene renders into) */
  float width, height;
  cw_vec2 pointer;        /* pointer in viewport pixels */
  bool pointer_inside;
  float px;               /* viewport pixels per UI unit (handle sizes scale with it) */
  /* 3D camera (column major, three.js layout) */
  t3_mat4 view, proj, view_proj;
  t3_vec3 cam_pos;
  t3_vec3 ray_origin, ray_dir; /* world ray through the pointer (dir normalized) */
  /* 2D view: world -> viewport pixels */
  cw_xform2d view2;
  float zoom2;
  /* the tool and snapping (viewport settings) */
  cwed_tool tool;
  bool local;             /* transform handles along the node's own axes */
  bool snap;
  float snap_move;        /* 3D: units; 2D: pixels */
  float snap_angle;       /* degrees */
  float snap_scale;       /* scale step (0.1 = 10%) */
  /* 2D: guide lines a snapped move also sticks to (world x / y), within 6 px */
  const float *guides_x, *guides_y;
  int guide_x_count, guide_y_count;
  /* input: the gizmo that owns the pointer until the release (set by the gizmo) */
  const void *capture;
  int capture_handle;
  /* filled by the viewport */
  void (*line3_fn)(cwed_gizmo_ctx *c, t3_vec3 a, t3_vec3 b, cw_color col);
  void (*overlay_line_fn)(cwed_gizmo_ctx *c, cw_vec2 a, cw_vec2 b, cw_color col, float width);
  void (*overlay_poly_fn)(cwed_gizmo_ctx *c, const cw_vec2 *pts, int n, cw_color col);
  void (*overlay_text_fn)(cwed_gizmo_ctx *c, cw_vec2 at, const char *text, cw_color col);
  void *impl;
};

/* ── drawing ─────────────────────────────────────────────────────────── */
void cwed_gizmo_line3(cwed_gizmo_ctx *c, t3_vec3 a, t3_vec3 b, cw_color col);
void cwed_gizmo_circle3(cwed_gizmo_ctx *c, t3_vec3 center, t3_vec3 axis, float radius, cw_color col);
/* a box of half extents `half` under transform xf (NULL = identity) */
void cwed_gizmo_box3(cwed_gizmo_ctx *c, const t3_mat4 *xf, t3_vec3 half, cw_color col);
void cwed_gizmo_sphere3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float radius, cw_color col);
/* capsule / cylinder along the local Y axis: radius, total height */
void cwed_gizmo_capsule3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float radius, float height, cw_color col);
void cwed_gizmo_cylinder3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float radius, float height, cw_color col);
/* a cone from apex along -Z (spot lights, camera frustums): angle = half angle, radians */
void cwed_gizmo_cone3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float length, float angle, cw_color col);
/* a point transformed by xf (NULL = identity) */
t3_vec3 cwed_gizmo_xf(const t3_mat4 *xf, t3_vec3 p);

/* overlay (viewport pixels, on top of everything) */
void cwed_gizmo_overlay_line(cwed_gizmo_ctx *c, cw_vec2 a, cw_vec2 b, cw_color col, float width);
void cwed_gizmo_overlay_circle(cwed_gizmo_ctx *c, cw_vec2 center, float radius, cw_color col, float width);
void cwed_gizmo_overlay_disc(cwed_gizmo_ctx *c, cw_vec2 center, float radius, cw_color col);
void cwed_gizmo_overlay_square(cwed_gizmo_ctx *c, cw_vec2 center, float half, cw_color col);
void cwed_gizmo_overlay_text(cwed_gizmo_ctx *c, cw_vec2 at, const char *text, cw_color col);
/* a 3D segment drawn on top (clipped at the near plane) */
void cwed_gizmo_overlay_line3(cwed_gizmo_ctx *c, t3_vec3 a, t3_vec3 b, cw_color col, float width);

/* ── 3D math against the viewport camera ──────────────────────────────── */
/* world -> viewport pixels; false when the point is behind the camera */
bool cwed_gizmo_project(const cwed_gizmo_ctx *c, t3_vec3 p, cw_vec2 *out);
/* world units covered by one viewport pixel at p (constant-size handles) */
float cwed_gizmo_world_per_px(const cwed_gizmo_ctx *c, t3_vec3 p);
/* the pointer ray hits the plane (point, normal): false when parallel or behind */
bool cwed_gizmo_ray_plane(const cwed_gizmo_ctx *c, t3_vec3 point, t3_vec3 normal, t3_vec3 *hit);
/* parameter t of the point on the line origin + t*axis closest to the pointer ray */
float cwed_gizmo_ray_axis(const cwed_gizmo_ctx *c, t3_vec3 origin, t3_vec3 axis);
/* pixel distance from p (viewport px) to segment ab */
float cwed_gizmo_dist_segment(cw_vec2 p, cw_vec2 a, cw_vec2 b);

/* ── 2D ───────────────────────────────────────────────────────────────── */
cw_vec2 cwed_gizmo_to_view2(const cwed_gizmo_ctx *c, cw_vec2 world);
cw_vec2 cwed_gizmo_from_view2(const cwed_gizmo_ctx *c, cw_vec2 px);
/* a world-space segment drawn on top */
void cwed_gizmo_line2(cwed_gizmo_ctx *c, cw_vec2 a, cw_vec2 b, cw_color col, float width);

/* ── snapping and value text ────────────────────────────────────────── */
float cwed_gizmo_snap(float v, float step); /* step <= 0: v */
/* shortest text that reads back as v (for command lines): "0.45", "1e-05" */
const char *cwed_gizmo_num(double v);
const char *cwed_gizmo_vec2_text(cw_vec2 v);
const char *cwed_gizmo_vec3_text(t3_vec3 v);

/* ── the transform tool (move / rotate / scale), used by both viewports ──
 * draw: the handles for the selection (nothing for the select tool);
 * input: true when it took the event (a press on a handle starts a drag,
 * moves update the selection's transforms through node.set / node.set_each
 * with merge key "gizmo"). hovered handle for highlighting: hover(). */
void cwed_transform_draw(cwed_gizmo_ctx *c);
bool cwed_transform_input(cwed_gizmo_ctx *c, const cw_ui_event *e);
bool cwed_transform_dragging(void);
/* which handle the pointer is over (-1 = none): for cursors and tests */
int cwed_transform_hover(cwed_gizmo_ctx *c);
/* The selection's pivot (world): the first selected node's global position. */
bool cwed_transform_pivot3(cwed_gizmo_ctx *c, t3_vec3 *out);

/* ── run the registered gizmos over the active scene (the viewports) ───── */
void cwed_gizmos_draw_all(cwed_gizmo_ctx *c);
bool cwed_gizmos_input_all(cwed_gizmo_ctx *c, const cw_ui_event *e);

/* Global transform of a Node3D (computed from the local transforms, not
 * the render cache: valid in a scene that never processed). */
void cwed_node3d_world(cw_node n, t3_mat4 *out);
/* Global transform of a Node2D (affine) composed up to the scene root. */
cw_xform2d cwed_node2d_world(cw_node n);

#ifdef __cplusplus
}
#endif
#endif

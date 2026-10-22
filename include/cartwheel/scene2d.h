/* 2D for the scene tree: the canvas node types over the batched 2D renderer
 * (render2d.h), and the tree's 2D pass.
 *
 * ── node types (all properties reflected: .cwscene, inspector, debug channel) ──
 * Every type below except CanvasLayer and CanvasSettings extends Node2D
 * (position, rotation, scale, skew, z_index, z_relative, visible,
 * interpolate, modulate, self_modulate). Pixels, y down, radians clockwise.
 * Point lists ("points", "polygon") are strings of numbers taken in pairs:
 * "0 0, 32 0, 32 16". Textures and fonts are asset paths, loaded (and cached
 * by path) on first draw, so scenes instance without a GL context.
 *
 *   Sprite2D          texture, filter (LINEAR|NEAREST), centered, offset,
 *                     flip_h, flip_v, region_enabled, region (vec4 x y w h),
 *                     hframes, vframes, frame (atlas grid cell)
 *   AnimatedSprite2D  texture, filter, hframes, vframes (the atlas grid),
 *                     regions ("x y w h, x y w h ..." : an explicit frame list
 *                     that replaces the grid when set), animations
 *                     ("idle@4:0-1; run@10:2-5; jump:6!" : name, optional fps,
 *                     frames as indices and ranges, '!' = play once),
 *                     animation (current name; empty = the first), fps
 *                     (default for animations without @fps), speed_scale,
 *                     playing, frame, process (FIXED: advances on fixed ticks
 *                     by the fixed dt, deterministic for replays; FRAME: by
 *                     the frame dt), centered, offset, flip_h, flip_v.
 *                     Signals: frame_changed, animation_finished.
 *   NinePatch2D       texture, filter, region, size, patch_left/top/right/bottom,
 *                     margin_scale, draw_center, centered
 *   Line2D            points, width, color, closed
 *   Polygon2D         polygon, color, texture (optional; uv = point / texture size), offset
 *   Label2D           text, font, font_mode (SDF|BITMAP), bake_size, size, color,
 *                     align (LEFT|CENTER|RIGHT), wrap_width, line_spacing, letter_spacing
 *                     (the text box's top-left sits at the node's origin)
 *   TileMap2D         tileset (texture), filter, tile_size, margin, spacing,
 *                     columns, cell_size, layers (1..8), cells0..cells7,
 *                     animations ("4:2@6; 10:4@8" : tile id, frame count, fps).
 *                     Cells text form: rows separated by '/', cells by spaces;
 *                     '.' = empty, N = tile id (0-based index into the tileset
 *                     grid), suffix x / y / d = flip x / flip y / transpose,
 *                     "N*k" (or ".*k") repeats a cell k times:
 *                     "0*20 / . . 3 3x / 1*4". The map's size is the largest
 *                     row count and row length over all layers.
 *   Camera2D          current, zoom, offset, anchor_top_left, smoothing,
 *                     smoothing_speed, limit_left/top/right/bottom, rotating.
 *                     The first current Camera2D in tree order drives every
 *                     canvas that follows the viewport (the default canvas,
 *                     ParallaxLayers, CanvasLayers with follow_viewport).
 *                     Smoothing runs on fixed ticks (cw_camera2d_follow,
 *                     deterministic) after physics; rendering interpolates.
 *   CanvasLayer       (a Node) layer (draw order; the default canvas is 0),
 *                     offset, rotation, scale, follow_viewport, visible, y_sort.
 *                     Defaults (as in Godot): layer 1 (above the default
 *                     canvas), follow_viewport false (screen space: a HUD;
 *                     set it for a layer that scrolls with the Camera2D),
 *                     offset 0, rotation 0, scale 1, visible, no y_sort.
 *                     Its descendants' transforms start fresh at the layer.
 *   ParallaxLayer     motion_scale (camera factor), motion_offset; its
 *                     descendants draw in their own render layer just above
 *                     the canvas they are in (put them under a CanvasLayer
 *                     with a negative layer for a background).
 *   Light2D           enabled, color, energy, range (radius px), falloff,
 *                     height, shadow. A canvas holding a light is drawn lit.
 *   LightOccluder2D   polygon, closed (casts shadows for lights with shadow)
 *   CanvasSettings    (a Node; the first one in tree order counts)
 *                     virtual_width/virtual_height (0 = window pixels),
 *                     scale_mode (NONE|INTEGER|FIT|STRETCH|EXPAND), clear_color,
 *                     letterbox, ambient (light where no Light2D reaches on
 *                     lit canvases), pixel_snap, y_sort (default canvas).
 *                     Defaults: no virtual canvas, scale_mode NONE, clear
 *                     and letterbox opaque black, ambient white (lit canvases
 *                     fully lit), no pixel_snap, no y_sort. A tree with no
 *                     CanvasSettings draws the same way: window pixels,
 *                     cleared to black, ambient white.
 *
 * ── two coordinate spaces: Node2D vs Control ─────────────────────────────
 * Node2Ds (everything above) live on the VIRTUAL canvas when CanvasSettings
 * sets one: positions, sizes and camera are virtual pixels, scaled to the
 * window by scale_mode. Controls (cartwheel/ui.h: Label, Button, ...) do
 * not: the scene runner draws the UI in its own window pass, so their
 * anchors and offsets are UI units, window pixels divided by cw_ui_scale,
 * even under a CanvasLayer and even with a virtual canvas. With a 320x180
 * virtual canvas in a 640x360 window, a Label at offset_left 8 sits 8
 * window pixels (4 virtual pixels) from the edge. This is deliberate: UI
 * text stays sharp and keeps its size whatever the game's resolution. To
 * lay UI out in virtual pixels instead, set cw_ui_set_scale(window height /
 * virtual height), or draw the UI inside your own virtual pass with
 * cw_ui_draw (pixel-art UI that scales with the game; see ui.h).
 *
 * ── the 2D pass ─────────────────────────────────────────────────────────
 * cw_canvas_draw walks the tree once in tree order: hidden Node2Ds and
 * CanvasLayers hide their subtrees, transforms compose (interpolated between
 * the last two fixed ticks for nodes with `interpolate`), modulate
 * multiplies down, z_index adds down while z_relative. Every canvas item is
 * submitted to render2d in its canvas's render layer at its z, so the
 * renderer's stable sort draws by (canvas layer, z, tree order) and y within
 * y-sorted canvases. Canvases get render layers 0.. in (layer, tree order)
 * order; a frame may hold up to 30.
 *
 * Who draws on top (the UI hook): after the tree's 2D pass ends,
 * cw_canvas_draw runs the tree's registered overlay passes
 * (cw_canvas_add_pass) in `order`. Each runs its OWN render2d pass
 * (cw_r2d_begin with clear = false ... cw_r2d_end) over the frame, so UI
 * (Control nodes) composites over 3D and 2D without sharing layer slots.
 *
 * The scene runner draws: 3D (only when a Camera3D exists; a 2D-only tree
 * never creates a three.c renderer), then cw_canvas_draw. When nothing in
 * the tree is a canvas item and no overlay pass is registered, the 2D pass
 * is skipped entirely. */
#ifndef CW_SCENE2D_H
#define CW_SCENE2D_H

#include "render2d.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── canvas items: types that draw in the 2D pass ──────────────────────
 * A draw function gets the node, its data for the registered type, and the
 * item: the composed global transform (already set with
 * cw_r2d_set_transform, so submit in node-local coordinates), the inherited
 * modulate (multiply it into your colours), the render layer and z (already
 * selected) and the render alpha. Register a type (or an ancestor type: the
 * nearest registered type in the chain wins) once, after the type itself. */
typedef struct cw_canvas_item {
  cw_xform2d xform;
  cw_color modulate;
  int layer, z;
  float alpha;
} cw_canvas_item;
typedef void (*cw_canvas_draw_fn)(cw_node n, void *data, const cw_canvas_item *item);
/* replaces fn on a second call (hot reload); asserts when the type is not registered */
void cw_canvas_register_type(const char *type_name, cw_canvas_draw_fn fn);

/* Draw the tree's 2D into the bound window framebuffer (w x h drawable
 * pixels), then the overlay passes. clear: clear first (the runner passes
 * true when no 3D was drawn). Returns false when it drew nothing. The r2d
 * layer settings (cw_r2d_layer_set) it uses for its canvases are put back
 * when its pass ends, here and in cw_canvas_draw_view, so a pass after it
 * starts from the caller's settings, not the tree's camera. */
bool cw_canvas_draw(cw_tree *t, int w, int h, double alpha, bool clear);

/* Draw the tree's 2D into a render target through a camera of the
 * caller's (an editor viewport: its own pan and zoom, whatever Camera2D the
 * scene has). No virtual canvas: the target's pixels are the canvas. The
 * tree's CanvasSettings ambient still lights lit canvases. Returns false
 * when it drew nothing. */
typedef struct cw_canvas_view {
  cw_texture target;     /* a render target (cw_target_create); its size is the canvas */
  bool use_camera;       /* use camera below instead of the tree's current Camera2D */
  cw_camera2d camera;
  bool clear;            /* clear to clear_color first */
  cw_color clear_color;
  bool skip_overlays;    /* do not run the overlay passes (UI) */
} cw_canvas_view;
/* canvas = the target's size; alpha as cw_canvas_draw */
bool cw_canvas_draw_view(cw_tree *t, const cw_canvas_view *view, double alpha);

/* Overlay passes (UI): run after the tree's 2D pass, lower order first. */
typedef void (*cw_canvas_pass_fn)(cw_tree *t, int w, int h, double alpha, void *user);
/* stable by order; the pass list lives with the tree */
void cw_canvas_add_pass(cw_tree *t, int order, cw_canvas_pass_fn fn, void *user);
/* removes the first pass matching fn and user; no-op if none */
void cw_canvas_remove_pass(cw_tree *t, cw_canvas_pass_fn fn, void *user);

/* The camera that drew the last frame (null when none), and its view of the
 * default canvas: canvas pixels <-> world. */
cw_node cw_canvas_camera(cw_tree *t);
cw_vec2 cw_canvas_screen_to_world(cw_tree *t, cw_vec2 canvas_px); /* identity when no camera drew */
cw_vec2 cw_canvas_world_to_screen(cw_tree *t, cw_vec2 world); /* identity when no camera drew */
/* The camera2d value the last frame used (zoom, limits applied). */
cw_camera2d cw_canvas_camera_value(cw_tree *t);

typedef struct cw_canvas_stats {
  int32_t items, layers, lights, occluders;
} cw_canvas_stats;
cw_canvas_stats cw_canvas_get_stats(cw_tree *t); /* counts from the last cw_canvas_draw */

/* ── per-type helpers ─────────────────────────────────────────────────── */

/* AnimatedSprite2D: switch animation (restarts it when it changes, or when
 * restart is set) and play. */
void cw_animated_sprite2d_play(cw_node n, const char *animation, bool restart);
void cw_animated_sprite2d_stop(cw_node n); /* pauses on the current frame; no-op unless AnimatedSprite2D */

/* TileMap2D cells. tile -1 = empty; flags are CW_TILE_FLIP_* (render2d.h).
 * Setting outside the map grows it. Cost: O(1) (the chunk rebuilds at the
 * next draw); growing reallocates. */
void cw_tilemap2d_set_cell(cw_node n, int layer, int x, int y, int tile, uint32_t flags);
int cw_tilemap2d_get_cell(cw_node n, int layer, int x, int y); /* -1 empty or outside */
void cw_tilemap2d_size(cw_node n, int *w, int *h); /* in cells, over all layers; 0 x 0 when not a TileMap2D */
/* Local node position -> cell (floor), using cell_size. */
void cw_tilemap2d_local_to_cell(cw_node n, cw_vec2 local, int *x, int *y);

/* Parse a point list ("0 0, 32 0, 32 16") into out (frame arena); returns
 * the point count. */
int cw_points_parse(const char *text, cw_vec2 **out);

#ifdef __cplusplus
}
#endif
#endif

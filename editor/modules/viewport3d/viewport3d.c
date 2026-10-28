/* 3D Viewport panel.
 *
 * Renders the active scene's tree through its own render pipeline
 * (cartwheel/pipeline.h) into a render target the panel draws, with an
 * EDITOR camera (a Camera3D outside the scene root, so it is never saved)
 * and editor-only helpers on three.c layer CWED_LAYER_EDITOR: the grid,
 * gizmo world lines (light / camera / collision shape visualizers), the
 * selection boxes. The transform gizmo and handles are overlays drawn on
 * top of the image.
 *
 * Camera: orbit (middle drag or alt+left drag orbits, shift+middle pans,
 * wheel zooms toward the target) or fly (right drag looks around, WASD / Q E
 * move while it is held, shift faster). F frames the selection; 1 / 3 / 7
 * front, right and top views. Tools: Q select, W move, E rotate, R scale,
 * T local/global axes, Y snapping. Left click picks (ctrl adds/removes):
 * meshes by ray, other nodes by their origin handle; a pick inside an
 * instanced scene selects the instance.
 *
 * Commands (view state, not undoable; tool and snap settings persist):
 *   view3d.camera target yaw pitch distance mode    view3d.focus [path]
 *   view3d.tool tool   view3d.local on   view3d.snap on move angle scale
 *   view3d.grid on     view3d.pick x y mode   view3d.state
 *   view3d.play_here   (the "play from here" hook: stores the point under
 *                       the view centre in setting play.from_here, then
 *                       runs play.start; the play backend reads it)
 * Scene edits made by the gizmos are node.set / node.set_each commands. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "../gizmos/gizmos.h"

#define MAX_LINES 32768

typedef struct {
  cwed_doc *doc;
  t3_vec3 target;
  float yaw, pitch, distance; /* radians, radians, units */
  bool fly;
  bool init;
} view_cam;

typedef struct { cw_vec2 a, b; cw_color c; float w; int kind; char text[64]; cw_vec2 pts[24]; int npts; } overlay_cmd;

/* editor-only objects of one document tree (a tree slot: they live and die
 * with the document): the line buffer, the grid, the editor camera */
typedef struct helpers {
  t3_geometry *g;
  t3_attribute *ap, *ac;
  t3_mesh *lines, *grid;
  cw_node camera;
} helpers;

static struct {
  cw_node panel, view, info, toolbar, tool_btn[CWED_TOOL_COUNT], local_btn, snap_btn, grid_btn;
  cw_texture target;
  int tw, th;
  CW_ARRAY(view_cam) cams;
  cwed_gizmo_ctx ctx;
  CW_ARRAY(overlay_cmd) overlay;
  /* world lines: the current tree's helper set */
  float *lpos, *lcol;     /* its attribute arrays */
  int lcount;
  struct helpers *h;
  cw_node camera;         /* the editor camera node (in the doc tree, outside the scene) */
  /* input */
  bool orbiting, panning, looking;
  cw_vec2 last;
  bool rendered_once;
  uint32_t renders;
  int32_t f_renders, f_picks, f_drags;
  uint32_t last_pick_hash;
  t3_raycaster *rc;
  bool syncing;
} V;

static float setf(const char *k) { return (float)cwed_setting_float(k); }

static view_cam *cam_for(cwed_doc *d) {
  for (int i = 0; i < V.cams.count; i++)
    if (V.cams.data[i].doc == d) return &V.cams.data[i];
  view_cam c = { d, t3_v3(0, 0, 0), 0.6f, -0.45f, 14, false, false };
  cw_array_push(&V.cams, c);
  return &V.cams.data[V.cams.count - 1];
}

static void on_doc_closed(const cwed_event *e, void *u) {
  (void)u;
  for (int i = 0; i < V.cams.count; i++)
    if (V.cams.data[i].doc == e->doc) {
      cw_array_remove_swap(&V.cams, i);
      break;
    }
  if (e->doc && V.h && cw_tree_slot(cwed_doc_tree(e->doc), "cwed.viewport3d") == V.h) {
    V.h = NULL;
    V.camera = (cw_node){ 0 };
    V.lpos = V.lcol = NULL;
  }
}

static bool doc_is_3d(cwed_doc *d) {
  cw_node r = cwed_doc_root(d);
  if (!cw_node_valid(r)) return false;
  if (cw_node_is(r, "Node3D")) return true;
  if (cw_node_is(r, "Node2D") || cw_node_is(r, "Control") || cw_node_is(r, "CanvasLayer")) return false;
  for (int i = 0; i < cw_node_child_count(r); i++)
    if (cw_node_is(cw_node_child(r, i), "Node3D")) return true;
  return false;
}

/* ── camera math ─────────────────────────────────────────────────────── */

static t3_quat cam_quat(const view_cam *c) {
  t3_quat qy = t3_quat_from_axis_angle(t3_v3(0, 1, 0), c->yaw);
  t3_quat qx = t3_quat_from_axis_angle(t3_v3(1, 0, 0), c->pitch);
  return t3_quat_multiply(qy, qx);
}
static t3_vec3 cam_forward(const view_cam *c) { return t3_vec3_apply_quat(t3_v3(0, 0, -1), cam_quat(c)); }
static t3_vec3 cam_pos(const view_cam *c) { return t3_vec3_sub(c->target, t3_vec3_scale(cam_forward(c), c->distance)); }

/* ── the line buffer (world lines on the editor layer) ─────────────────── */

static void line3(cwed_gizmo_ctx *c, t3_vec3 a, t3_vec3 b, cw_color col) {
  (void)c;
  if (!V.lpos || V.lcount + 2 > MAX_LINES * 2) return;
  float *p = V.lpos + V.lcount * 3, *k = V.lcol + V.lcount * 3;
  p[0] = a.x, p[1] = a.y, p[2] = a.z, p[3] = b.x, p[4] = b.y, p[5] = b.z;
  /* premultiply by alpha against the dark background (lines are opaque) */
  k[0] = k[3] = col.r * col.a + 0.18f * (1 - col.a);
  k[1] = k[4] = col.g * col.a + 0.18f * (1 - col.a);
  k[2] = k[5] = col.b * col.a + 0.2f * (1 - col.a);
  V.lcount += 2;
}

static void overlay_line(cwed_gizmo_ctx *c, cw_vec2 a, cw_vec2 b, cw_color col, float w) {
  (void)c;
  overlay_cmd o = { .a = a, .b = b, .c = col, .w = w, .kind = 0 };
  cw_array_push(&V.overlay, o);
}
static void overlay_poly(cwed_gizmo_ctx *c, const cw_vec2 *pts, int n, cw_color col) {
  (void)c;
  overlay_cmd o = { .c = col, .kind = 1 };
  o.npts = n > 24 ? 24 : n;
  memcpy(o.pts, pts, sizeof(cw_vec2) * (size_t)o.npts);
  cw_array_push(&V.overlay, o);
}
static void overlay_text(cwed_gizmo_ctx *c, cw_vec2 at, const char *text, cw_color col) {
  (void)c;
  overlay_cmd o = { .a = at, .c = col, .kind = 2 };
  snprintf(o.text, sizeof o.text, "%s", text);
  cw_array_push(&V.overlay, o);
}

static void ensure_helpers(cw_tree *t) {
  helpers *h = cw_tree_slot(t, "cwed.viewport3d");
  if (!h) {
    h = cw_calloc(1, sizeof *h, CW_MEM_EDITOR);
    t3_scene *sc = cw_render3d_scene(t);
    float *zero = cw_calloc(MAX_LINES * 2 * 3, sizeof(float), CW_MEM_EDITOR);
    h->g = t3_geometry_new();
    h->ap = t3_attribute_new(T3_FLOAT32, zero, MAX_LINES * 2, 3);
    h->ac = t3_attribute_new(T3_FLOAT32, zero, MAX_LINES * 2, 3);
    cw_free(zero);
    t3_geometry_set_attribute(h->g, T3_ATTR_POSITION, h->ap);
    t3_geometry_set_attribute(h->g, T3_ATTR_COLOR, h->ac);
    t3_release(h->ap), t3_release(h->ac);
    h->g->draw_count = 0;
    t3_material *m = t3_line_basic_material_new(0xffffff);
    m->vertex_colors = true;
    m->fog = false;
    m->tone_mapped = false;
    h->lines = t3_line_segments_new(h->g, m);
    t3_release(h->g), t3_release(m);
    h->lines->base.frustum_culled = false;
    h->lines->base.render_order = 1000;
    t3_object_set_layers(h->lines, CWED_LAYER_EDITOR);
    t3_object_add(&sc->base, h->lines);
    t3_release(h->lines);
    h->grid = t3_grid_helper_new(200, 200, 0x4a5060, 0x30343e);
    h->grid->base.frustum_culled = false;
    t3_object_set_layers(h->grid, CWED_LAYER_EDITOR);
    for (int i = 0; i < h->grid->material_count; i++) {
      h->grid->materials[i]->fog = false;
      h->grid->materials[i]->tone_mapped = false;
    }
    t3_object_add(&sc->base, h->grid);
    t3_release(h->grid);
    /* the editor camera: under the tree root, beside (not inside) the scene */
    h->camera = cw_node_new("Camera3D", "EditorCamera3D");
    cw_node_add_child(cw_tree_root(t), h->camera);
    cw_node_set(h->camera, "fov", cw_val_float(60));
    cw_node_set(h->camera, "near", cw_val_float(0.05));
    cw_node_set(h->camera, "far", cw_val_float(4000));
    t3_object_set_layers(cw_camera3d_t3(h->camera), 1u | CWED_LAYER_EDITOR);
    cw_render3d_set_camera(t, h->camera);
    cw_tree_set_slot(t, "cwed.viewport3d", h, cw_free);
  }
  V.h = h;
  V.camera = h->camera;
  V.lpos = t3_attribute_f32(h->ap);
  V.lcol = t3_attribute_f32(h->ac);
}

/* ── per-frame context ───────────────────────────────────────────────── */

static void fill_ctx(cwed_doc *d, view_cam *vc) {
  cwed_gizmo_ctx *c = &V.ctx;
  c->doc = d;
  c->is_3d = true;
  c->width = (float)V.tw, c->height = (float)V.th;
  c->px = cw_ui_scale() > 0 ? cw_ui_scale() : 1;
  c->tool = (cwed_tool)(int)cwed_setting_float("viewport3d.tool");
  c->local = cwed_setting_bool("viewport3d.local");
  c->snap = cwed_setting_bool("viewport3d.snap");
  c->snap_move = setf("viewport3d.snap_move");
  c->snap_angle = setf("viewport3d.snap_angle");
  c->snap_scale = setf("viewport3d.snap_scale");
  c->line3_fn = line3;
  c->overlay_line_fn = overlay_line;
  c->overlay_poly_fn = overlay_poly;
  c->overlay_text_fn = overlay_text;
  /* camera matrices (what the render will use) */
  t3_vec3 pos = cam_pos(vc);
  t3_quat q = cam_quat(vc);
  t3_mat4 world, proj;
  t3_mat4_compose(&world, pos, q, t3_v3(1, 1, 1));
  t3_mat4_invert(&c->view, &world);
  float aspect = V.th > 0 ? (float)V.tw / (float)V.th : 1, near = 0.05f, far = 4000, fov = 60 * 0.017453293f;
  float top = near * tanf(fov * 0.5f), right = top * aspect;
  t3_mat4_make_perspective(&proj, -right, right, top, -top, near, far);
  c->proj = proj;
  t3_mat4_multiply(&c->view_proj, &proj, &c->view);
  c->cam_pos = pos;
  /* the pointer ray */
  float nx = c->width > 0 ? c->pointer.x / c->width * 2 - 1 : 0, ny = c->height > 0 ? 1 - c->pointer.y / c->height * 2 : 0;
  t3_mat4 inv;
  t3_mat4_invert(&inv, &c->view_proj);
  t3_vec3 a = t3_vec3_apply_mat4(t3_v3(nx, ny, -1), &inv), b = t3_vec3_apply_mat4(t3_v3(nx, ny, 1), &inv);
  c->ray_origin = a;
  c->ray_dir = t3_vec3_normalize(t3_vec3_sub(b, a));
}

/* ── picking ─────────────────────────────────────────────────────────── */

typedef struct { const t3_object *obj; cw_node node; } obj_map;
typedef struct { CW_ARRAY(obj_map) map; cwed_gizmo_ctx *c; cw_node best; float best_px; } pick_walk;

static bool collect_fn(cw_node n, void *u) {
  pick_walk *w = u;
  if (cw_node_is(n, "Node3D")) {
    t3_object *o = cw_node3d_object(n);
    if (o) cw_array_push(&w->map, ((obj_map){ o, n }));
    if (!cw_node_is(n, "MeshInstance3D")) { /* handle pick: the origin */
      t3_mat4 m;
      cwed_node3d_world(n, &m);
      cw_vec2 s;
      if (cwed_gizmo_project(w->c, t3_mat4_get_position(&m), &s)) {
        float d = cw_v2_length(cw_v2_sub(s, w->c->pointer));
        bool special = cw_node_is(n, "Light3D") || cw_node_is(n, "Camera3D") || cw_node_is(n, "CollisionShape3D");
        if (special && d < 10 * w->c->px && d < w->best_px) w->best_px = d, w->best = n;
      }
    }
  }
  return true;
}

/* the node to select for a hit: the outermost instance root above it (not the scene root) */
static cw_node selectable(cwed_doc *d, cw_node n) {
  cw_node root = cwed_doc_root(d), pick = n;
  for (cw_node p = n; cw_node_valid(p) && p.id != root.id; p = cw_node_parent(p))
    if (cw_scene_instance_source(p) && cw_node_parent(p).id != 0) pick = p;
  return pick;
}

static cw_node pick_at(cwed_doc *d) {
  pick_walk w = { .c = &V.ctx, .best_px = 1e9f };
  cw_node_walk(cwed_doc_root(d), collect_fn, &w);
  cw_node found = w.best;
  if (CW_HANDLE_IS_NULL(found)) {
    if (!V.rc) V.rc = t3_raycaster_new();
    t3_raycaster_set(V.rc, V.ctx.ray_origin, V.ctx.ray_dir);
    const t3_intersection *hits;
    t3_scene *sc = cw_render3d_scene(cwed_doc_tree(d));
    t3_object_update_matrix_world(&sc->base, false);
    int k = t3_raycaster_intersect_object(V.rc, &sc->base, true, &hits);
    for (int i = 0; i < k && CW_HANDLE_IS_NULL(found); i++)
      for (const t3_object *o = hits[i].object; o && CW_HANDLE_IS_NULL(found); o = o->parent)
        for (int m = 0; m < w.map.count; m++)
          if (w.map.data[m].obj == o) {
            found = w.map.data[m].node;
            break;
          }
  }
  cw_array_free(&w.map);
  return CW_HANDLE_IS_NULL(found) ? found : selectable(d, found);
}

static void pick_select(cwed_doc *d, int mode /* 0 set 1 add 2 toggle */) {
  cw_node n = pick_at(d);
  V.f_picks++;
  if (CW_HANDLE_IS_NULL(n)) {
    if (mode == 0) cwed_run("select.clear", NULL);
    return;
  }
  const char *path = cwed_doc_node_path(d, n);
  V.last_pick_hash = cw_hash32(path, strlen(path), CW_HASH32_INIT);
  cwed_runf("select paths=%s mode=%s", cwed_quote(path), mode == 0 ? "set" : mode == 1 ? "add" : "toggle");
}

/* ── drawing the helpers for a frame ─────────────────────────────────── */

static void mesh_half(cw_node n, t3_vec3 *half, t3_vec3 *center) {
  cw_value v;
  int prim = cw_node_get_prop(n, "primitive", &v) ? (int)v.i : 0;
  cw_value s;
  t3_vec3 sz = cw_node_get_prop(n, "size", &s) ? t3_v3(s.v[0], s.v[1], s.v[2]) : t3_v3(1, 1, 1);
  *center = t3_v3(0, 0, 0);
  switch (prim) {
  case 1: *half = t3_vec3_scale(sz, 0.5f); break;
  case 2: *half = t3_v3(sz.x * 0.5f, sz.x * 0.5f, sz.x * 0.5f); break;
  case 3: *half = t3_v3(sz.x * 0.5f, 0.001f, sz.z * 0.5f); break;
  case 4: *half = t3_v3(sz.x * 0.5f, sz.y * 0.5f, sz.x * 0.5f); break;
  default: *half = t3_v3(0.25f, 0.25f, 0.25f); break;
  }
}

static void selection_box(cw_node n) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  cw_color col = cw_rgba(1, 0.62f, 0.2f, 1);
  if (cw_node_is(n, "MeshInstance3D")) {
    t3_vec3 h, c;
    mesh_half(n, &h, &c);
    cwed_gizmo_box3(&V.ctx, &w, t3_vec3_scale(h, 1.02f), col);
    return;
  }
  /* a node without its own mesh: the boxes of mesh descendants, else a cross */
  bool any = false;
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node ch = cw_node_child(n, i);
    if (cw_node_is(ch, "MeshInstance3D")) {
      selection_box(ch);
      any = true;
    } else if (cw_node_is(ch, "Node3D") && cw_node_child_count(ch)) {
      selection_box(ch);
      any = true;
    }
  }
  if (!any) {
    float s = 0.3f;
    t3_vec3 o = cwed_gizmo_xf(&w, t3_v3(0, 0, 0));
    cwed_gizmo_line3(&V.ctx, t3_vec3_add(o, t3_v3(-s, 0, 0)), t3_vec3_add(o, t3_v3(s, 0, 0)), col);
    cwed_gizmo_line3(&V.ctx, t3_vec3_add(o, t3_v3(0, -s, 0)), t3_vec3_add(o, t3_v3(0, s, 0)), col);
    cwed_gizmo_line3(&V.ctx, t3_vec3_add(o, t3_v3(0, 0, -s)), t3_vec3_add(o, t3_v3(0, 0, s)), col);
  }
}

static void build_helpers(cwed_doc *d, view_cam *vc) {
  V.lcount = 0;
  cw_array_clear(&V.overlay);
  bool grid = cwed_setting_bool("viewport3d.grid");
  if (V.h) {
    V.h->grid->base.visible = grid;
    t3_object_set_position(V.h->grid, roundf(vc->target.x), 0, roundf(vc->target.z));
  }
  if (grid) { /* the X and Z axes */
    line3(&V.ctx, t3_v3(-1000, 0.001f, 0), t3_v3(1000, 0.001f, 0), cw_rgba(0.75f, 0.28f, 0.3f, 1));
    line3(&V.ctx, t3_v3(0, 0.001f, -1000), t3_v3(0, 0.001f, 1000), cw_rgba(0.28f, 0.45f, 0.8f, 1));
  }
  cwed_gizmos_draw_all(&V.ctx);
  for (int i = 0; i < cwed_selection_count(); i++)
    if (cw_node_is(cwed_selection_get(i), "Node3D")) selection_box(cwed_selection_get(i));
  cwed_transform_draw(&V.ctx);
  (void)d;
  if (V.h) {
    V.h->ap->version++;
    V.h->ac->version++;
    V.h->g->draw_count = V.lcount;
  }
}

/* ── rendering ───────────────────────────────────────────────────────── */

static void render(cwed_doc *d) {
  cw_tree *t = cwed_doc_tree(d);
  view_cam *vc = cam_for(d);
  ensure_helpers(t);
  t3_vec3 p = cam_pos(vc);
  cw_node3d_set_position(V.camera, p);
  cw_node3d_set_quaternion(V.camera, cam_quat(vc));
  fill_ctx(d, vc);
  build_helpers(d, vc);
  cw_pipeline *pl = cw_pipeline_of(t);
  if (cw_pipeline_has_pass(pl, "canvas")) cw_pipeline_set_pass_enabled(pl, "canvas", false);
  cw_pipeline_set_output(pl, cw_target_rt(V.target)); /* GL and WebGPU */
  cwed_doc_enter(d);
  cw_tree_pre_render(t, 1.0);
  cw_pipeline_render(t, V.tw, V.th, 1.0);
  cwed_doc_leave();
  cw_pipeline_set_output(pl, NULL);
  t3_renderer *r = cw_render3d_renderer(t);
  if (r) t3_renderer_reset_bindings(r);
  V.renders++;
  V.f_renders++;
}

/* ── the view control ────────────────────────────────────────────────── */

typedef struct { int unused; } view_data;
static const cw_node_type *view_t;

static void view_draw(cw_node n, void *data) {
  (void)data;
  cw_rect r = cw_control_rect(n);
  cw_ui_draw_rect((cw_rect){ 0, 0, r.w, r.h }, cw_rgba(0.1f, 0.11f, 0.13f, 1));
  cwed_doc *d = cwed_doc_active();
  if (!d || !doc_is_3d(d) || !cw_texture_valid(V.target)) {
    cw_ui_draw_text_in(cw_ui_font_sans(), 15, d ? "This scene is 2D: see the 2D viewport" : "No scene open", (cw_rect){ 0, 0, r.w, r.h },
                       CW_HALIGN_CENTER, cw_rgba(0.55f, 0.58f, 0.64f, 1), false);
    return;
  }
  float s = V.ctx.px > 0 ? V.ctx.px : 1;
  cw_ui_draw_texture(V.target, (cw_rect){ 0, 0, (float)V.tw / s, (float)V.th / s }, (cw_rect){ 0 }, CW_WHITE);
  cw_ui_push_clip((cw_rect){ 0, 0, r.w, r.h });
  for (int i = 0; i < V.overlay.count; i++) {
    overlay_cmd *o = &V.overlay.data[i];
    if (o->kind == 0) cw_ui_draw_line(cw_v2_scale(o->a, 1 / s), cw_v2_scale(o->b, 1 / s), o->w / s, o->c);
    else if (o->kind == 1) {
      cw_vec2 p[24];
      for (int k = 0; k < o->npts; k++) p[k] = cw_v2_scale(o->pts[k], 1 / s);
      cw_ui_draw_polygon(p, o->npts, o->c);
    } else {
      cw_ui_draw_text(cw_ui_font_sans(), 13, o->text, cw_v2_scale(o->a, 1 / s), o->c);
    }
  }
  cw_ui_pop_clip();
}

static void orbit_by(view_cam *vc, float dx, float dy) {
  vc->yaw -= dx * 0.008f;
  vc->pitch -= dy * 0.008f;
  if (vc->pitch > 1.55f) vc->pitch = 1.55f;
  if (vc->pitch < -1.55f) vc->pitch = -1.55f;
}
static void pan_by(view_cam *vc, float dx, float dy) {
  t3_quat q = cam_quat(vc);
  float k = vc->distance * 0.0016f;
  t3_vec3 right = t3_vec3_apply_quat(t3_v3(1, 0, 0), q), up = t3_vec3_apply_quat(t3_v3(0, 1, 0), q);
  vc->target = t3_vec3_add(vc->target, t3_vec3_add(t3_vec3_scale(right, -dx * k), t3_vec3_scale(up, dy * k)));
}
static void look_by(view_cam *vc, float dx, float dy) {
  /* fly: rotate around the eye */
  t3_vec3 eye = cam_pos(vc);
  orbit_by(vc, dx, dy);
  vc->target = t3_vec3_add(eye, t3_vec3_scale(cam_forward(vc), vc->distance));
}

static bool view_input(cw_node n, void *data, const cw_ui_event *e) {
  (void)data;
  cwed_doc *d = cwed_doc_active();
  if (!d || !doc_is_3d(d)) return false;
  view_cam *vc = cam_for(d);
  float s = cw_ui_scale() > 0 ? cw_ui_scale() : 1;
  V.ctx.pointer = cw_v2_scale(e->pos, s);
  V.ctx.pointer_inside = true;
  fill_ctx(d, vc);
  switch (e->type) {
  case CW_UI_MOUSE_DOWN:
    cw_control_grab_focus(n);
    if (e->button == 0 && !e->alt) {
      if (cwed_transform_input(&V.ctx, e)) {
        V.f_drags++;
        return true;
      }
      if (cwed_gizmos_input_all(&V.ctx, e)) return true;
      pick_select(d, e->ctrl ? 2 : e->shift ? 1 : 0);
      return true;
    }
    if (e->button == 1 && cwed_gizmos_input_all(&V.ctx, e)) return true;
    V.last = e->pos;
    if (e->button == 2 || (e->button == 0 && e->alt)) {
      if (e->shift) V.panning = true;
      else V.orbiting = true;
    } else if (e->button == 1) {
      V.looking = true;
    }
    return true;
  case CW_UI_MOUSE_MOVE: {
    if (cwed_transform_dragging()) return cwed_transform_input(&V.ctx, e);
    if (V.ctx.capture) return cwed_gizmos_input_all(&V.ctx, e);
    cw_vec2 dlt = cw_v2_sub(e->pos, V.last);
    V.last = e->pos;
    if (V.orbiting) orbit_by(vc, dlt.x, dlt.y);
    else if (V.panning) pan_by(vc, dlt.x, dlt.y);
    else if (V.looking) look_by(vc, dlt.x, dlt.y);
    return V.orbiting || V.panning || V.looking;
  }
  case CW_UI_MOUSE_UP:
    if (cwed_transform_dragging()) return cwed_transform_input(&V.ctx, e);
    if (V.ctx.capture) {
      bool r = cwed_gizmos_input_all(&V.ctx, e);
      V.ctx.capture = NULL;
      return r;
    }
    V.orbiting = V.panning = V.looking = false;
    return true;
  case CW_UI_WHEEL:
    vc->distance *= powf(0.88f, e->wheel.y);
    if (vc->distance < 0.2f) vc->distance = 0.2f;
    if (vc->distance > 2000) vc->distance = 2000;
    return true;
  case CW_UI_KEY: {
    if (e->ctrl || e->alt) return false;
    const char *line = NULL;
    switch (e->key) {
    case 20: line = "view3d.tool tool=select"; break; /* Q */
    case 26: line = "view3d.tool tool=move"; break;   /* W */
    case 8: line = "view3d.tool tool=rotate"; break;  /* E */
    case 21: line = "view3d.tool tool=scale"; break;  /* R */
    case 23: line = cwed_setting_bool("viewport3d.local") ? "view3d.local on=false" : "view3d.local on=true"; break; /* T */
    case 28: line = cwed_setting_bool("viewport3d.snap") ? "view3d.snap on=false" : "view3d.snap on=true"; break;   /* Y */
    case 9: line = "view3d.focus"; break; /* F */
    case 30: case 89: line = "view3d.camera yaw=0 pitch=0"; break;    /* 1: front */
    case 32: case 91: line = "view3d.camera yaw=90 pitch=0"; break;   /* 3: right */
    case 36: case 95: line = "view3d.camera yaw=0 pitch=-89"; break;  /* 7: top */
    default: break;
    }
    if (line && !V.looking) {
      cwed_run(line, NULL);
      return true;
    }
    return false;
  }
  default: return false;
  }
}

static void register_view_type(void) {
  static cw_node_type t = { .name = "EditorViewport3D", .parent = "Control", .data_size = sizeof(view_data),
                            .doc = "The editor's 3D viewport (editor module viewport3d)." };
  if (!cw_node_type_find("EditorViewport3D")) {
    cw_node_type_register(&t);
    cw_ui_class_register("EditorViewport3D", &(cw_ui_class){ .draw = view_draw, .input = view_input });
  }
  view_t = cw_node_type_find("EditorViewport3D");
}

/* fly movement with WASD while the right button looks */
static void fly_keys(view_cam *vc, double dt) {
  const cw_raw_input *in = cw_input_raw();
  if (!in || !V.looking) return;
  float sp = setf("viewport3d.fly_speed") * (in->keys[225] || in->keys[229] ? 4.0f : 1.0f) * (float)dt;
  t3_quat q = cam_quat(vc);
  t3_vec3 f = t3_vec3_apply_quat(t3_v3(0, 0, -1), q), r = t3_vec3_apply_quat(t3_v3(1, 0, 0), q), mv = { 0 };
  if (in->keys[26]) mv = t3_vec3_add(mv, f);
  if (in->keys[22]) mv = t3_vec3_sub(mv, f);
  if (in->keys[7]) mv = t3_vec3_add(mv, r);
  if (in->keys[4]) mv = t3_vec3_sub(mv, r);
  if (in->keys[8]) mv = t3_vec3_add(mv, t3_v3(0, 1, 0));
  if (in->keys[20]) mv = t3_vec3_sub(mv, t3_v3(0, 1, 0));
  vc->target = t3_vec3_add(vc->target, t3_vec3_scale(mv, sp));
}

/* ── commands ───────────────────────────────────────────────────────── */

static bool need_3d(cwed_call *c, cwed_doc **d) {
  *d = cwed_doc_active();
  if (!*d) return cwed_fail(c, "no scene open (scene.open path=...)");
  return true;
}

static void reply_cam(cwed_call *c, view_cam *vc) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "target");
  cwed_json_str(j, cwed_gizmo_vec3_text(vc->target));
  cwed_json_printf(j, ",\"yaw\":%s,\"pitch\":%s,\"distance\":%s,\"mode\":\"%s\"", cwed_gizmo_num(vc->yaw * 57.29578f),
                   cwed_gizmo_num(vc->pitch * 57.29578f), cwed_gizmo_num(vc->distance), vc->fly ? "fly" : "orbit");
  t3_vec3 p = cam_pos(vc);
  cwed_json_key(j, "position");
  cwed_json_str(j, cwed_gizmo_vec3_text(p));
}

static bool cmd_camera(cwed_call *c) {
  cwed_doc *d;
  if (!need_3d(c, &d)) return false;
  view_cam *vc = cam_for(d);
  vc->init = true;
  if (cwed_arg_given(c, "target")) {
    cw_value v;
    if (!cw_value_parse(cwed_arg(c, "target"), CW_T_VEC3, &v)) return cwed_fail(c, "target wants vec3(x, y, z)");
    vc->target = t3_v3(v.v[0], v.v[1], v.v[2]);
  }
  if (cwed_arg_given(c, "yaw")) vc->yaw = (float)cwed_arg_float(c, "yaw") * 0.017453293f;
  if (cwed_arg_given(c, "pitch")) {
    float p = (float)cwed_arg_float(c, "pitch");
    if (p < -89.9f || p > 89.9f) return cwed_fail(c, "pitch is degrees between -89.9 and 89.9");
    vc->pitch = p * 0.017453293f;
  }
  if (cwed_arg_given(c, "distance")) {
    float k = (float)cwed_arg_float(c, "distance");
    if (k <= 0) return cwed_fail(c, "distance must be positive");
    vc->distance = k;
  }
  if (cwed_arg_given(c, "mode")) {
    const char *m = cwed_arg(c, "mode");
    if (strcmp(m, "orbit") && strcmp(m, "fly")) return cwed_fail(c, "mode is orbit or fly");
    vc->fly = !strcmp(m, "fly");
  }
  reply_cam(c, vc);
  return true;
}

static bool aabb_of(cw_node n, t3_vec3 *lo, t3_vec3 *hi) {
  bool any = false;
  if (cw_node_is(n, "Node3D")) {
    t3_mat4 w;
    cwed_node3d_world(n, &w);
    t3_vec3 h = { 0.5f, 0.5f, 0.5f }, c;
    if (cw_node_is(n, "MeshInstance3D")) mesh_half(n, &h, &c);
    for (int i = 0; i < 8; i++) {
      t3_vec3 p = cwed_gizmo_xf(&w, t3_v3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z));
      if (!any) *lo = *hi = p, any = true;
      lo->x = fminf(lo->x, p.x), lo->y = fminf(lo->y, p.y), lo->z = fminf(lo->z, p.z);
      hi->x = fmaxf(hi->x, p.x), hi->y = fmaxf(hi->y, p.y), hi->z = fmaxf(hi->z, p.z);
    }
  }
  for (int i = 0; i < cw_node_child_count(n); i++) {
    t3_vec3 a, b;
    if (!aabb_of(cw_node_child(n, i), &a, &b)) continue;
    if (!any) *lo = a, *hi = b, any = true;
    lo->x = fminf(lo->x, a.x), lo->y = fminf(lo->y, a.y), lo->z = fminf(lo->z, a.z);
    hi->x = fmaxf(hi->x, b.x), hi->y = fmaxf(hi->y, b.y), hi->z = fmaxf(hi->z, b.z);
  }
  return any;
}

static bool cmd_focus(cwed_call *c) {
  cwed_doc *d;
  if (!need_3d(c, &d)) return false;
  view_cam *vc = cam_for(d);
  t3_vec3 lo = { 0 }, hi = { 0 };
  bool any = false;
  if (cwed_arg_given(c, "path")) {
    cw_node n = cwed_arg_node(c, "path");
    if (CW_HANDLE_IS_NULL(n)) return false;
    any = aabb_of(n, &lo, &hi);
  } else {
    for (int i = 0; i < cwed_selection_count(); i++) {
      t3_vec3 a, b;
      if (!aabb_of(cwed_selection_get(i), &a, &b)) continue;
      if (!any) lo = a, hi = b, any = true;
      lo.x = fminf(lo.x, a.x), lo.y = fminf(lo.y, a.y), lo.z = fminf(lo.z, a.z);
      hi.x = fmaxf(hi.x, b.x), hi.y = fmaxf(hi.y, b.y), hi.z = fmaxf(hi.z, b.z);
    }
    if (!any) any = aabb_of(cwed_doc_root(d), &lo, &hi);
  }
  if (!any) return cwed_fail(c, "nothing 3D to frame");
  vc->target = t3_vec3_scale(t3_vec3_add(lo, hi), 0.5f);
  float r = t3_vec3_length(t3_vec3_sub(hi, lo)) * 0.5f;
  vc->distance = fmaxf(r / tanf(30 * 0.017453293f), 1.0f);
  reply_cam(c, vc);
  return true;
}

static const char *const TOOL_NAMES[CWED_TOOL_COUNT] = { "select", "move", "rotate", "scale" };

static bool cmd_tool(cwed_call *c) {
  const char *t = cwed_arg(c, "tool");
  for (int i = 0; i < CWED_TOOL_COUNT; i++)
    if (!strcmp(t, TOOL_NAMES[i])) {
      cwed_setting_put("viewport3d.tool", cw_frame_printf("%d", i));
      cwed_json_key(cwed_reply(c), "tool");
      cwed_json_str(cwed_reply(c), t);
      return true;
    }
  return cwed_fail(c, "tool is select, move, rotate or scale");
}
static bool cmd_snap(cwed_call *c) {
  if (cwed_arg_given(c, "on")) {
    bool on = !strcmp(cwed_arg(c, "on"), "toggle") ? !cwed_setting_bool("viewport3d.snap") : cwed_arg_bool(c, "on");
    cwed_setting_put("viewport3d.snap", on ? "true" : "false");
  }
  if (cwed_arg_given(c, "move")) cwed_setting_put("viewport3d.snap_move", cwed_gizmo_num(cwed_arg_float(c, "move")));
  if (cwed_arg_given(c, "angle")) cwed_setting_put("viewport3d.snap_angle", cwed_gizmo_num(cwed_arg_float(c, "angle")));
  if (cwed_arg_given(c, "scale")) cwed_setting_put("viewport3d.snap_scale", cwed_gizmo_num(cwed_arg_float(c, "scale")));
  cwed_json_printf(cwed_reply(c), "\"snap\":%s,\"move\":%s,\"angle\":%s,\"scale\":%s", cwed_setting_bool("viewport3d.snap") ? "true" : "false",
                   cwed_setting("viewport3d.snap_move"), cwed_setting("viewport3d.snap_angle"), cwed_setting("viewport3d.snap_scale"));
  return true;
}

static bool cmd_pick(cwed_call *c) {
  cwed_doc *d;
  if (!need_3d(c, &d)) return false;
  if (!V.tw || !V.th) return cwed_fail(c, "the 3D viewport has not drawn yet (panel.show id=viewport3d, then a frame)");
  V.ctx.pointer = cw_v2((float)cwed_arg_float(c, "x"), (float)cwed_arg_float(c, "y"));
  fill_ctx(d, cam_for(d));
  const char *m = cwed_arg(c, "mode");
  int mode = !strcmp(m, "add") ? 1 : !strcmp(m, "toggle") ? 2 : 0;
  cw_node n = pick_at(d);
  cwed_json_key(cwed_reply(c), "node");
  cwed_json_str(cwed_reply(c), CW_HANDLE_IS_NULL(n) ? "" : cwed_doc_node_path(d, n));
  if (!cwed_arg_bool(c, "dry")) pick_select(d, mode);
  return true;
}

static bool cmd_state(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  cwed_json *j = cwed_reply(c);
  cwed_json_printf(j, "\"size\":[%d,%d],\"renders\":%u,\"tool\":\"%s\",\"local\":%s,\"snap\":%s,\"grid\":%s,\"is_3d\":%s,\"lines\":%d",
                   V.tw, V.th, V.renders, TOOL_NAMES[(int)cwed_setting_float("viewport3d.tool") % CWED_TOOL_COUNT],
                   cwed_setting_bool("viewport3d.local") ? "true" : "false", cwed_setting_bool("viewport3d.snap") ? "true" : "false",
                   cwed_setting_bool("viewport3d.grid") ? "true" : "false", d && doc_is_3d(d) ? "true" : "false", V.lcount / 2);
  if (d) {

    reply_cam(c, cam_for(d));
    t3_vec3 piv;
    if (cwed_transform_pivot3(&V.ctx, &piv)) {
      cw_vec2 s;
      if (cwed_gizmo_project(&V.ctx, piv, &s)) cwed_json_printf(j, ",\"pivot_px\":[%s,%s]", cwed_gizmo_num(s.x), cwed_gizmo_num(s.y));
      /* where the move handles end (for pointer scripts) */
      float size = 90 * V.ctx.px * cwed_gizmo_world_per_px(&V.ctx, piv);
      cwed_json_printf(j, ",\"handle_size\":%s", cwed_gizmo_num(size));
      cwed_json_raw(j, ",\"handles_px\":[");
      for (int i = 0; i < 3; i++) {
        t3_vec3 e = piv;
        (&e.x)[i] += size;
        cw_vec2 p = { 0 };
        cwed_gizmo_project(&V.ctx, e, &p);
        cwed_json_printf(j, "%s[%s,%s]", i ? "," : "", cwed_gizmo_num(p.x), cwed_gizmo_num(p.y));
      }
      cwed_json_raw(j, "]");
    }
    cw_rect g = cw_control_global_rect(V.view);
    cwed_json_printf(j, ",\"view_rect\":[%s,%s,%s,%s]", cwed_gizmo_num(g.x), cwed_gizmo_num(g.y), cwed_gizmo_num(g.w), cwed_gizmo_num(g.h));
  }
  return true;
}

/* world points -> window coordinates (UI units): what a pointer script aims at */
static bool cmd_project(cwed_call *c) {
  cwed_doc *d;
  if (!need_3d(c, &d)) return false;
  if (!V.tw) return cwed_fail(c, "the 3D viewport has not drawn yet");
  fill_ctx(d, cam_for(d));
  cw_rect g = cw_control_global_rect(V.view);
  float s = V.ctx.px > 0 ? V.ctx.px : 1;
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "points");
  cwed_json_raw(j, "[");
  const char *p = cwed_arg(c, "points");
  for (int k = 0; *p; k++) {
    const char *e = strchr(p, '|');
    char item[256];
    snprintf(item, sizeof item, "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
    p += strlen(item) + (e ? 1 : 0);
    cw_value v;
    if (!cw_value_parse(item, CW_T_VEC3, &v)) return cwed_fail(c, "'%s' is not vec3(x, y, z)", item);
    cw_vec2 px;
    bool ok = cwed_gizmo_project(&V.ctx, t3_v3(v.v[0], v.v[1], v.v[2]), &px);
    cwed_json_printf(j, "%s", k ? "," : "");
    if (ok) cwed_json_printf(j, "[%s,%s]", cwed_gizmo_num(g.x + px.x / s), cwed_gizmo_num(g.y + px.y / s));
    else cwed_json_raw(j, "null");
  }
  cwed_json_raw(j, "]");
  return true;
}

typedef struct { cw_node found; } player_walk;
static bool player_fn(cw_node n, void *u) {
  player_walk *w = u;
  if (cw_node_valid(w->found)) return false;
  if (cw_node_in_group(n, "player") && cw_node_is(n, "Node3D")) {
    w->found = n;
    return false;
  }
  return true;
}

static bool cmd_play_here(cwed_call *c) {
  cwed_doc *d;
  if (!need_3d(c, &d)) return false;
  view_cam *vc = cam_for(d);
  /* the point under the view centre on the ground plane, else the orbit target */
  V.ctx.pointer = cw_v2((float)V.tw * 0.5f, (float)V.th * 0.5f);
  fill_ctx(d, vc);
  t3_vec3 at = vc->target, hit;
  if (cwed_gizmo_ray_plane(&V.ctx, t3_v3(0, 0, 0), t3_v3(0, 1, 0), &hit)) at = hit;
  at = t3_v3(roundf(at.x * 100) / 100, roundf(at.y * 100) / 100, roundf(at.z * 100) / 100); /* a value people read */
  cwed_setting_put("play.from_here", cwed_gizmo_vec3_text(at));
  cwed_json_key(cwed_reply(c), "from");
  cwed_json_str(cwed_reply(c), cwed_gizmo_vec3_text(at));
  if (cwed_arg_bool(c, "start")) {
    char *j = NULL;
    bool ok = cwed_run("play.start", &j);
    cwed_json_key(cwed_reply(c), "play");
    cwed_json_raw(cwed_reply(c), j ? j : "null");
    cw_free(j);
    if (!ok) cwed_log(CWED_LOG_WARN, "play from here: play.start refused (see its reply)");
    /* the scene's player (the first node in group "player") starts there */
    player_walk pw = { { 0 } };
    cw_node_walk(cwed_doc_root(d), player_fn, &pw);
    if (ok && cw_node_valid(pw.found)) {
      char gp[1024];
      cw_node_path(pw.found, cwed_doc_root(d), gp, sizeof gp);
      const char *game_path = cw_frame_printf("/root/%s%s%s", cw_node_name(cwed_doc_root(d)), gp[0] ? "/" : "", gp);
      bool moved = cwed_runf("play.set node=%s prop=position value=%s", cwed_quote(game_path), cwed_quote(cwed_gizmo_vec3_text(at))) ;
      cwed_json_key(cwed_reply(c), "player");
      cwed_json_str(cwed_reply(c), game_path);
      cwed_json_printf(cwed_reply(c), ",\"player_moved\":%s", moved ? "true" : "false");
    }
  }
  return true;
}

/* ── panel ───────────────────────────────────────────────────────────── */

static void on_tool_btn(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  if (!V.syncing) cwed_run((const char *)user, NULL);
}

static cw_node mkbtn(const char *text, const char *tip, const char *line, bool toggle) {
  cw_node b = cw_node_new("Button", NULL);
  cw_button_set_text(b, text);
  cw_control_set_tooltip(b, tip);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_node_set(b, "theme_overrides", cw_val_string("font_size = 12"));
  if (toggle) cw_button_set_toggle_mode(b, true);
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_tool_btn, (void *)line, 0);
  return b;
}

static void sync_toolbar(void) {
  V.syncing = true;
  int tool = (int)cwed_setting_float("viewport3d.tool");
  for (int i = 0; i < CWED_TOOL_COUNT; i++)
    if (cw_node_valid(V.tool_btn[i])) cw_button_set_pressed(V.tool_btn[i], i == tool);
  if (cw_node_valid(V.local_btn)) {
    cw_button_set_pressed(V.local_btn, cwed_setting_bool("viewport3d.local"));
    cw_button_set_text(V.local_btn, cwed_setting_bool("viewport3d.local") ? "Local" : "World");
  }
  if (cw_node_valid(V.snap_btn)) cw_button_set_pressed(V.snap_btn, cwed_setting_bool("viewport3d.snap"));
  if (cw_node_valid(V.grid_btn)) cw_button_set_pressed(V.grid_btn, cwed_setting_bool("viewport3d.grid"));
  V.syncing = false;
}

static cw_node create(void *user) {
  (void)user;
  register_view_type();
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node_set(box, "theme_overrides", cw_val_string("separation = 2"));
  V.toolbar = cw_node_new("HBoxContainer", "Toolbar");
  static const char *const lines[CWED_TOOL_COUNT] = { "view3d.tool tool=select", "view3d.tool tool=move", "view3d.tool tool=rotate",
                                                      "view3d.tool tool=scale" };
  static const char *const labels[CWED_TOOL_COUNT] = { "Sel", "Move", "Rot", "Scale" };
  static const char *const tips[CWED_TOOL_COUNT] = { "Select (Q)", "Move (W)", "Rotate (E)", "Scale (R)" };
  for (int i = 0; i < CWED_TOOL_COUNT; i++) {
    V.tool_btn[i] = mkbtn(labels[i], tips[i], lines[i], true);
    cw_node_add_child(V.toolbar, V.tool_btn[i]);
  }
  V.local_btn = mkbtn("World", "Local / global axes (T)", "view3d.local on=toggle", true);
  V.snap_btn = mkbtn("Snap", "Snapping (Y): view3d.snap move=1 angle=15 scale=0.1", "view3d.snap on=toggle", true);
  V.grid_btn = mkbtn("Grid", "Show the grid", "view3d.grid on=toggle", true);
  cw_node_add_child(V.toolbar, V.local_btn);
  cw_node_add_child(V.toolbar, V.snap_btn);
  cw_node_add_child(V.toolbar, V.grid_btn);
  V.info = cw_node_new("Label", "Info");
  cw_node_set(V.info, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  cw_control_set_size_flags(V.info, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_set(V.info, "horizontal_alignment", cw_val_int(CW_HALIGN_RIGHT));
  cw_node_set(V.info, "ellipsis", cw_val_bool(true));
  cw_node_add_child(V.toolbar, V.info);
  cw_node_add_child(box, V.toolbar);
  V.view = cw_node_new("EditorViewport3D", "View");
  cw_control_set_size_flags(V.view, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_set(V.view, "focus_mode", cw_val_int(CW_FOCUS_CLICK));
  cw_node_set(V.view, "clip_contents", cw_val_bool(true));
  cw_node_add_child(box, V.view);
  V.panel = box;
  sync_toolbar();
  return box;
}

/* a toggle argument: on=toggle flips the current value */
static bool toggle_arg(cwed_call *c, const char *key) {
  if (!strcmp(cwed_arg(c, "on"), "toggle")) return !cwed_setting_bool(key);
  return cwed_arg_bool(c, "on");
}
static bool cmd_local_t(cwed_call *c) {
  cwed_setting_put("viewport3d.local", toggle_arg(c, "viewport3d.local") ? "true" : "false");
  return true;
}
static bool cmd_grid_t(cwed_call *c) {
  cwed_setting_put("viewport3d.grid", toggle_arg(c, "viewport3d.grid") ? "true" : "false");
  return true;
}


static void frame(double dt) {
  if (!cw_node_valid(V.view)) return;
  cwed_doc *d = cwed_doc_active();
  bool shown = cw_control_visible_in_tree(V.view) && d && doc_is_3d(d);
  sync_toolbar();
  if (!shown) return;
  cw_rect r = cw_control_rect(V.view);
  float s = cw_ui_scale() > 0 ? cw_ui_scale() : 1;
  int w = (int)(r.w * s), h = (int)(r.h * s);
  if (w < 8 || h < 8) return;
  if (!cw_texture_valid(V.target)) {
    V.target = cw_target_create(&(cw_target_desc){ .width = w, .height = h, .depth = true });
    V.tw = w, V.th = h;
  } else if (w != V.tw || h != V.th) {
    cw_target_resize(V.target, w, h);
    V.tw = w, V.th = h;
  }
  view_cam *vc = cam_for(d);
  if (!vc->init) { /* first look at a scene: frame it */
    vc->init = true;
    t3_vec3 lo, hi;
    if (aabb_of(cwed_doc_root(d), &lo, &hi)) {
      vc->target = t3_vec3_scale(t3_vec3_add(lo, hi), 0.5f);
      float rr = t3_vec3_length(t3_vec3_sub(hi, lo)) * 0.5f;
      vc->distance = fminf(fmaxf(rr * 0.9f, 3.0f), 60.0f);
      vc->target.y = fminf(vc->target.y, 2.0f);
    }
  }
  fly_keys(vc, dt);
  /* the pointer for hover highlights (UI units -> viewport pixels) */
  cw_vec2 p = cw_ui_pointer();
  cw_rect g = cw_control_global_rect(V.view);
  V.ctx.pointer = cw_v2((p.x - g.x) * s, (p.y - g.y) * s);
  V.ctx.pointer_inside = p.x >= g.x && p.y >= g.y && p.x < g.x + g.w && p.y < g.y + g.h;
  render(d);
  const char *tool = TOOL_NAMES[(int)cwed_setting_float("viewport3d.tool") % CWED_TOOL_COUNT];
  cw_label_set_text(V.info, cw_frame_printf("%s  %s%s  yaw %.0f  pitch %.0f  dist %.1f", tool, vc->fly ? "fly" : "orbit",
                                            cwed_setting_bool("viewport3d.snap") ? "  snap" : "", vc->yaw * 57.29578f,
                                            vc->pitch * 57.29578f, vc->distance));
}

static void on_doc_active(const cwed_event *e, void *u) {
  (void)u;
  /* show the viewport that fits the scene */
  if (e->doc && doc_is_3d(e->doc) && cw_node_valid(V.panel)) cwed_run("panel.show id=viewport3d", NULL);
}

static void shutdown(void) {
  if (cw_texture_valid(V.target)) cw_texture_destroy(V.target);
  V.target = (cw_texture){ 0 };
  if (V.rc) t3_release(V.rc);
  V.rc = NULL;
  V.lpos = V.lcol = NULL;
  cw_array_free(&V.cams);
  cw_array_free(&V.overlay);
}

static void init(void) {
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.tool", .type = CWED_ARG_INT, .def = "1", .min = 0, .max = 3,
                                            .doc = "3D tool: 0 select, 1 move, 2 rotate, 3 scale", .hidden = true });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.local", .type = CWED_ARG_BOOL, .def = "false", .doc = "3D gizmo along local axes" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.snap", .type = CWED_ARG_BOOL, .def = "false", .doc = "3D snapping" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.snap_move", .type = CWED_ARG_FLOAT, .def = "0.5", .min = 0.001, .max = 100,
                                            .doc = "3D move snap step (units)" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.snap_angle", .type = CWED_ARG_FLOAT, .def = "15", .min = 0.1, .max = 180,
                                            .doc = "3D rotate snap step (degrees)" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.snap_scale", .type = CWED_ARG_FLOAT, .def = "0.1", .min = 0.001, .max = 10,
                                            .doc = "3D scale snap step" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.grid", .type = CWED_ARG_BOOL, .def = "true", .doc = "Show the 3D grid" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport3d.fly_speed", .type = CWED_ARG_FLOAT, .def = "8", .min = 0.1, .max = 500,
                                            .doc = "Fly camera speed (units per second)" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.from_here", .type = CWED_ARG_STRING, .def = "",
                                            .doc = "Set by view3d.play_here: where the play backend may start the player", .hidden = true });
  cwed_panel_register(&(cwed_panel_desc){ .id = "viewport3d", .title = "3D", .slot = "center", .order = 10, .create = create });
  static const cwed_param cam_p[] = {
    { "target", CWED_ARG_STRING, "", "orbit centre, vec3(x, y, z)" }, { "yaw", CWED_ARG_FLOAT, "", "degrees around +Y (0 looks down -Z)" },
    { "pitch", CWED_ARG_FLOAT, "", "degrees (negative looks down)" }, { "distance", CWED_ARG_FLOAT, "", "from the target" },
    { "mode", CWED_ARG_STRING, "", "orbit or fly" }, { 0 } };
  static const cwed_param focus_p[] = { { "path", CWED_ARG_PATH, "", "node to frame (default: the selection, else the scene)" }, { 0 } };
  static const cwed_param tool_p[] = { { "tool", CWED_ARG_STRING, NULL, "select, move, rotate or scale" }, { 0 } };
  static const cwed_param on_p[] = { { "on", CWED_ARG_STRING, "toggle", "true, false or toggle" }, { 0 } };
  static const cwed_param snap_p[] = {
    { "on", CWED_ARG_STRING, "", "true, false or toggle" }, { "move", CWED_ARG_FLOAT, "", "move step (units)" },
    { "angle", CWED_ARG_FLOAT, "", "rotate step (degrees)" }, { "scale", CWED_ARG_FLOAT, "", "scale step" }, { 0 } };
  static const cwed_param pick_p[] = {
    { "x", CWED_ARG_FLOAT, NULL, "viewport pixel x" }, { "y", CWED_ARG_FLOAT, NULL, "viewport pixel y" },
    { "mode", CWED_ARG_STRING, "set", "set, add or toggle" }, { "dry", CWED_ARG_BOOL, "false", "only report the node" }, { 0 } };
  static const cwed_param here_p[] = { { "start", CWED_ARG_BOOL, "false", "also run play.start" }, { 0 } };
  const uint32_t Q = CWED_CMD_SCENE | CWED_CMD_HIDDEN;
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.camera", .title = "3D View: Camera", .params = cam_p, .flags = Q,
    .doc = "Set the 3D viewport's camera (orbit target, yaw / pitch in degrees, distance, orbit or fly). Replies with the camera.",
    .exec = cmd_camera });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.focus", .title = "3D View: Frame Selection", .params = focus_p,
    .flags = CWED_CMD_SCENE, .shortcut = NULL, .doc = "Frame a node, the selection, or the whole scene.", .exec = cmd_focus });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.tool", .title = "3D View: Tool", .params = tool_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Pick the 3D tool: select, move, rotate, scale.", .exec = cmd_tool });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.local", .title = "3D View: Local Axes", .params = on_p,
    .doc = "Gizmo handles along the node's own axes (true) or the world's (false).", .exec = cmd_local_t });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.snap", .title = "3D View: Snapping", .params = snap_p,
    .doc = "Snapping on/off and its steps (move units, angle degrees, scale step).", .exec = cmd_snap });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.grid", .title = "3D View: Grid", .params = on_p,
    .doc = "Show or hide the 3D grid.", .exec = cmd_grid_t });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.pick", .title = "3D View: Pick", .params = pick_p, .flags = Q,
    .doc = "Select the node under a viewport pixel (what a click does); replies with its path.", .exec = cmd_pick });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.state", .title = "3D View: State", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The 3D viewport: size, camera, tool, snapping, the selection pivot and handle ends in viewport pixels.", .exec = cmd_state });
  static const cwed_param proj_p[] = { { "points", CWED_ARG_STRING, NULL, "world points vec3(x, y, z) separated by |" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.project", .title = "3D View: Project Points", .params = proj_p,
    .flags = CWED_CMD_SCENE | CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Where world points are in the window (UI units, null behind the camera): what a pointer script aims at.", .exec = cmd_project });
  cwed_command_register(&(cwed_command_desc){ .name = "view3d.play_here", .title = "Play From Here", .params = here_p, .flags = CWED_CMD_SCENE,
    .doc = "Play from here: store the point under the 3D view's centre in setting play.from_here and, with start=true, run "
           "play.start and move the scene's player (the first Node3D in group \"player\") there in the running game (play.set).",
    .exec = cmd_play_here });
  cwed_menu_add("View/3D: Frame Selection", "view3d.focus", 200);
  cwed_menu_add("View/3D: Toggle Snapping", "view3d.snap on=toggle", 201);
  cwed_menu_add("View/3D: Toggle Grid", "view3d.grid on=toggle", 202);
  cwed_menu_add("Play/Play From Here", "view3d.play_here start=true", 110);
  cwed_subscribe(CWED_EV_DOC_CLOSED, on_doc_closed, NULL);
  cwed_subscribe(CWED_EV_DOC_ACTIVE | CWED_EV_DOC_OPENED, on_doc_active, NULL);
}

CWED_MODULE(viewport3d) = { .title = "3D Viewport", .init = init, .frame = frame, .shutdown = shutdown };

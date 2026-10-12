/* turntable (editor part): tools for the Turntable node type, written
 * against the editor's extension API (cwed/editor.h) and the gizmo toolkit
 * (gizmos/gizmos.h) only. The native editor builds this file as
 * plugins/turntable/turntable_editor.so and loads it with the project; a cart
 * editor built by the project has it compiled in.
 *
 *   inspector plugin  Turntable.speed: a slider (-20..20 rad/s) with the
 *                     speed in rpm and a Reverse button
 *   gizmo             the reach (radius) as a ring around the axis, with a
 *                     handle that drags the radius (one undo step per drag)
 *   panel             Turntables: every Turntable of the scene, Add / Reverse All
 *   commands          turntable.list, turntable.gizmo (queries)
 *   menu              Scene/Add Turntable
 *
 * Everything changes the scene through commands (node.set, node.set_each,
 * node.add), so undo and the log see it. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cwed/editor.h"
#include "gizmos/gizmos.h"

/* which build of this file is loaded (turntable.list reports it; the hot
 * reload gate edits it in a copy of the project and rebuilds) */
#define TT_EDITOR_REV "1"

static const cw_color RING = { 0.95f, 0.55f, 0.2f, 0.95f };
static const cw_color HANDLE = { 1, 0.85f, 0.3f, 1 };

static float fprop(cw_node n, const char *p, float def) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) && v.type == CW_T_FLOAT ? (float)v.f : def;
}
static t3_vec3 vprop(cw_node n, const char *p, t3_vec3 def) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) && v.type == CW_T_VEC3 ? t3_v3(v.v[0], v.v[1], v.v[2]) : def;
}
static bool is_turntable(cw_node n) { return cw_node_valid(n) && cw_node_is(n, "Turntable"); }

/* ── inspector plugin: speed ─────────────────────────────────────────── */

static bool syncing;

static const char *speed_text(float s) { return cw_frame_printf("%.2f rad/s  %.0f rpm", s, s * 60.0f / 6.2831853f); }

static void on_slider(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  if (syncing) return;
  cwed_prop_commit("speed", cwed_gizmo_num(a[0].f), false); /* the pointer release seals the drag into one step */
}

static void on_reverse(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  if (!cwed_selection_count()) return;
  cwed_prop_commit("speed", cwed_gizmo_num(-fprop(cwed_selection_get(0), "speed", 0)), true);
}

static bool speed_handles(cw_node n, const cw_prop *p) { return is_turntable(n) && !strcmp(p->name, "speed"); }

static void speed_refresh(cw_node e, const cwed_prop_ctx *ctx) {
  syncing = true;
  cw_node slider = cw_node_get(e, "TurntableSlider"), label = cw_node_get(e, "Row/Value");
  float s = (float)ctx->value.f;
  if (cw_node_valid(slider) && cw_range_value(slider) != s) cw_range_set_value(slider, s);
  if (cw_node_valid(label)) cw_label_set_text(label, speed_text(s));
  syncing = false;
}

static cw_node speed_create(const cwed_prop_ctx *ctx) {
  cw_node box = cw_node_new("VBoxContainer", "TurntableSpeed");
  cw_node row = cw_node_new("HBoxContainer", "Row");
  cw_node slider = cw_node_new("HSlider", "TurntableSlider");
  cw_node_set(slider, "min_value", cw_val_float(-20));
  cw_node_set(slider, "max_value", cw_val_float(20));
  cw_node_set(slider, "step", cw_val_float(0.1));
  cw_control_set_size_flags(slider, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_control_set_custom_min_size(slider, (cw_vec2){ 90, 0 });
  cw_node rev = cw_node_new("Button", "TurntableReverse");
  cw_button_set_text(rev, "Reverse");
  cw_node_set(rev, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_node label = cw_node_new("Label", "Value");
  cw_node_set(label, "theme_overrides", cw_val_string("font_size = 12; font_color = #f0a050"));
  cw_control_set_size_flags(label, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  /* slider on its own row (the inspector column is narrow), then Reverse and the value */
  cw_node_add_child(box, slider);
  cw_node_add_child(row, rev);
  cw_node_add_child(row, label);
  cw_node_add_child(box, row);
  speed_refresh(box, ctx);
  cw_connect(slider, "value_changed", (cw_node){ 0 }, NULL, on_slider, NULL, 0);
  cw_connect(rev, "pressed", (cw_node){ 0 }, NULL, on_reverse, NULL, 0);
  return box;
}

static const cwed_inspector_plugin speed_plugin = {
  .name = "turntable.speed", .priority = 10, .handles = speed_handles, .create = speed_create, .refresh = speed_refresh,
};

/* ── gizmo: radius ───────────────────────────────────────────────────── */

static struct {
  bool dragging, handle_valid;
  cw_node node;
  cw_vec2 handle_px, center_px; /* last drawn, viewport pixels */
  float px;
  int draws;
} G;

static void frame_of(cw_node n, t3_vec3 *origin, t3_vec3 *axis, t3_vec3 *u, t3_vec3 *v) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  *origin = cwed_gizmo_xf(&w, t3_v3(0, 0, 0));
  t3_vec3 a = t3_vec3_transform_direction(vprop(n, "axis", t3_v3(0, 1, 0)), &w);
  float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
  a = l > 1e-6f ? t3_v3(a.x / l, a.y / l, a.z / l) : t3_v3(0, 1, 0);
  /* two directions in the turning plane; the handle sits between them */
  t3_vec3 ref = fabsf(a.y) < 0.9f ? t3_v3(0, 1, 0) : t3_v3(1, 0, 0);
  t3_vec3 x = t3_v3(ref.y * a.z - ref.z * a.y, ref.z * a.x - ref.x * a.z, ref.x * a.y - ref.y * a.x);
  float xl = sqrtf(x.x * x.x + x.y * x.y + x.z * x.z);
  x = t3_v3(x.x / xl, x.y / xl, x.z / xl);
  *axis = a;
  *u = x;
  *v = t3_v3(a.y * x.z - a.z * x.y, a.z * x.x - a.x * x.z, a.x * x.y - a.y * x.x);
}

static t3_vec3 handle_point(cw_node n) {
  t3_vec3 o, a, u, v;
  frame_of(n, &o, &a, &u, &v);
  float r = fprop(n, "radius", 1), k = 0.70710678f * r;
  return t3_v3(o.x + (u.x + v.x) * k, o.y + (u.y + v.y) * k, o.z + (u.z + v.z) * k);
}

static void ring_draw(cw_node n, cwed_gizmo_ctx *c) {
  t3_vec3 o, a, u, v;
  frame_of(n, &o, &a, &u, &v);
  float r = fprop(n, "radius", 1);
  cw_color col = RING;
  if (!c->selected) col.a *= 0.5f;
  /* the ring on top of the scene (it lies in the turntable's own plane, which a floor often shares) */
  for (int i = 0; i < 48; i++) {
    float t0 = (float)i * 0.1308997f, t1 = (float)(i + 1) * 0.1308997f;
    t3_vec3 p0 = t3_v3(o.x + (u.x * cosf(t0) + v.x * sinf(t0)) * r, o.y + (u.y * cosf(t0) + v.y * sinf(t0)) * r,
                       o.z + (u.z * cosf(t0) + v.z * sinf(t0)) * r);
    t3_vec3 p1 = t3_v3(o.x + (u.x * cosf(t1) + v.x * sinf(t1)) * r, o.y + (u.y * cosf(t1) + v.y * sinf(t1)) * r,
                       o.z + (u.z * cosf(t1) + v.z * sinf(t1)) * r);
    cwed_gizmo_overlay_line3(c, p0, p1, col, (c->selected ? 2.0f : 1.0f) * c->px);
  }
  cwed_gizmo_line3(c, o, t3_v3(o.x + a.x * 0.6f, o.y + a.y * 0.6f, o.z + a.z * 0.6f), col);
  if (!c->selected) return;
  /* the turning direction: a short arc of arrows ahead of the handle */
  float speed = fprop(n, "speed", 0), dir = speed < 0 ? -1.0f : 1.0f;
  for (int i = 1; i <= 3; i++) {
    float t = 0.7853982f + dir * 0.18f * (float)i;
    t3_vec3 p = t3_v3(o.x + (u.x * cosf(t) + v.x * sinf(t)) * r, o.y + (u.y * cosf(t) + v.y * sinf(t)) * r,
                      o.z + (u.z * cosf(t) + v.z * sinf(t)) * r);
    cw_vec2 s;
    if (cwed_gizmo_project(c, p, &s)) cwed_gizmo_overlay_disc(c, s, (2.0f + (float)i) * c->px, RING);
  }
  cw_vec2 hp, cp;
  if (cwed_gizmo_project(c, handle_point(n), &hp) && cwed_gizmo_project(c, o, &cp)) {
    cwed_gizmo_overlay_line(c, cp, hp, HANDLE, 1.5f * c->px);
    cwed_gizmo_overlay_disc(c, hp, 6 * c->px, G.dragging ? (cw_color){ 1, 1, 1, 1 } : HANDLE);
    cwed_gizmo_overlay_text(c, (cw_vec2){ hp.x + 9 * c->px, hp.y - 6 * c->px }, cw_frame_printf("r %.2f", r), HANDLE);
    G.handle_px = hp;
    G.center_px = cp;
    G.px = c->px;
    G.handle_valid = true;
    G.node = n;
    G.draws++;
  }
}

static bool ring_input(cw_node n, cwed_gizmo_ctx *c, const cw_ui_event *e);
static const cwed_gizmo_desc ring_gizmo = {
  .name = "turntable.radius", .node_type = "Turntable", .is_3d = true, .draw = ring_draw, .input = ring_input,
};

static bool ring_input(cw_node n, cwed_gizmo_ctx *c, const cw_ui_event *e) {
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 0 && !G.dragging) {
    cw_vec2 hp;
    if (!cwed_gizmo_project(c, handle_point(n), &hp)) return false;
    float dx = hp.x - c->pointer.x, dy = hp.y - c->pointer.y;
    if (dx * dx + dy * dy > (10 * c->px) * (10 * c->px)) return false;
    cwed_history_seal();
    G.dragging = true;
    G.node = n;
    c->capture = &ring_gizmo;
    return true;
  }
  if (!G.dragging || G.node.id != n.id) return false;
  if (e->type == CW_UI_MOUSE_MOVE && (e->buttons & 1)) {
    t3_vec3 o, a, u, v, hit;
    frame_of(n, &o, &a, &u, &v);
    if (cwed_gizmo_ray_plane(c, o, a, &hit)) {
      float dx = hit.x - o.x, dy = hit.y - o.y, dz = hit.z - o.z;
      float r = sqrtf(dx * dx + dy * dy + dz * dz);
      if (c->snap) r = cwed_gizmo_snap(r, 0.1f);
      if (r < 0.05f) r = 0.05f;
      cwed_runf("node.set path=%s prop=radius value=%s merge=turntable_radius", cwed_quote(cwed_doc_node_path(c->doc, n)),
                cwed_gizmo_num(r));
    }
    return true;
  }
  if (e->type == CW_UI_MOUSE_UP) {
    G.dragging = false;
    c->capture = NULL;
    cwed_history_seal();
    return true;
  }
  return true;
}

/* ── panel: Turntables ───────────────────────────────────────────────── */

static cw_node list, info;
static bool panel_dirty = true;
static CW_ARRAY(cw_node) rows; /* the list's nodes, in order */

static bool collect(cw_node n, void *user) {
  (void)user;
  if (is_turntable(n)) cw_array_push(&rows, n);
  return true;
}

static void rebuild_rows(void) {
  rows.count = 0;
  cwed_doc *d = cwed_doc_active();
  if (d) cw_node_walk(cwed_doc_root(d), collect, NULL);
}

static void refresh_panel(void) {
  if (!cw_node_valid(list)) return;
  rebuild_rows();
  cwed_doc *d = cwed_doc_active();
  syncing = true;
  cw_item_list_clear(list);
  for (int i = 0; i < rows.count; i++) {
    cw_node n = rows.data[i];
    cw_item_list_add_item(list, cw_frame_printf("%s   speed %.2f   radius %.2f", cwed_doc_node_path(d, n), fprop(n, "speed", 0),
                                                fprop(n, "radius", 0)), (cw_texture){ 0 });
    if (cwed_selection_has(n)) cw_item_list_select(list, i, false);
  }
  syncing = false;
  cw_label_set_text(info, !d ? "No scene open" : rows.count ? cw_frame_printf("%d turntable%s in %s", rows.count, rows.count == 1 ? "" : "s", cwed_doc_title(d))
                                                            : "No turntables in this scene: Add one");
}

static void on_pick(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  if (syncing || a[0].i < 0 || a[0].i >= rows.count) return;
  cwed_runf("select paths=%s", cwed_quote(cwed_doc_node_path(cwed_doc_active(), rows.data[a[0].i])));
}

static void on_add(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  cwed_run("node.add type=Turntable", NULL);
}

static void on_reverse_all(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  rebuild_rows();
  if (!rows.count) return;
  cwed_doc *d = cwed_doc_active();
  char edits[4096] = "";
  for (int i = 0; i < rows.count; i++)
    snprintf(edits + strlen(edits), sizeof edits - strlen(edits), "%s%s:speed=%s", i ? "|" : "", cwed_doc_node_path(d, rows.data[i]),
             cwed_gizmo_num(-fprop(rows.data[i], "speed", 0)));
  cwed_runf("node.set_each edits=%s", cwed_quote(edits));
}

static cw_node panel_create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", "TurntablePanel");
  info = cw_node_new("Label", "TurntableInfo");
  cw_node_set(info, "ellipsis", cw_val_bool(true));
  cw_node row = cw_node_new("HBoxContainer", "Row");
  cw_node add = cw_node_new("Button", "TurntableAdd");
  cw_button_set_text(add, "Add Turntable");
  cw_node_set(add, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(add, "pressed", (cw_node){ 0 }, NULL, on_add, NULL, 0);
  cw_node rev = cw_node_new("Button", "TurntableReverseAll");
  cw_button_set_text(rev, "Reverse All");
  cw_node_set(rev, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(rev, "pressed", (cw_node){ 0 }, NULL, on_reverse_all, NULL, 0);
  cw_node_add_child(row, add);
  cw_node_add_child(row, rev);
  list = cw_node_new("ItemList", "TurntableList");
  cw_control_set_size_flags(list, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_connect(list, "item_selected", (cw_node){ 0 }, NULL, on_pick, NULL, 0);
  cw_node_add_child(box, info);
  cw_node_add_child(box, row);
  cw_node_add_child(box, list);
  panel_dirty = true;
  return box;
}

static void on_event(const cwed_event *e, void *user) {
  (void)e, (void)user;
  panel_dirty = true;
}

/* ── commands ────────────────────────────────────────────────────────── */

static bool cmd_list(cwed_call *c) {
  rebuild_rows();
  cwed_json *j = cwed_reply(c);
  cwed_doc *d = cwed_doc_active();
  cwed_json_key(j, "rev");
  cwed_json_str(j, TT_EDITOR_REV);
  cwed_json_key(j, "turntables");
  cwed_json_raw(j, "[");
  for (int i = 0; i < rows.count; i++) {
    cw_node n = rows.data[i];
    cwed_json_printf(j, "%s{\"path\":", i ? "," : "");
    cwed_json_str(j, cwed_doc_node_path(d, n));
    cwed_json_printf(j, ",\"speed\":%s,\"radius\":%s,\"selected\":%s}", cwed_gizmo_num(fprop(n, "speed", 0)),
                     cwed_gizmo_num(fprop(n, "radius", 0)), cwed_selection_has(n) ? "true" : "false");
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_gizmo(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  bool ok = G.handle_valid && is_turntable(G.node) && cwed_selection_has(G.node);
  cwed_json_key(j, "visible");
  cwed_json_raw(j, ok ? "true" : "false");
  cwed_json_key(j, "dragging");
  cwed_json_raw(j, G.dragging ? "true" : "false");
  cwed_json_key(j, "draws");
  cwed_json_printf(j, "%d", G.draws);
  if (ok) {
    cwed_json_key(j, "node");
    cwed_json_str(j, cwed_doc_node_path(cwed_doc_active(), G.node));
    cwed_json_printf(j, ",\"handle_px\":[%s,%s],\"center_px\":[%s,%s],\"px\":%s", cwed_gizmo_num(G.handle_px.x),
                     cwed_gizmo_num(G.handle_px.y), cwed_gizmo_num(G.center_px.x), cwed_gizmo_num(G.center_px.y), cwed_gizmo_num(G.px));
  }
  return true;
}

/* ── module ──────────────────────────────────────────────────────────── */

static void init(void) {
  /* statics start clean on every load (a reload runs init again) */
  memset(&G, 0, sizeof G);
  panel_dirty = true;
  cwed_inspector_register(&speed_plugin);
  cwed_gizmo_register(&ring_gizmo);
  cwed_panel_register(&(cwed_panel_desc){ .id = "turntables", .title = "Turntables", .slot = "left_bottom", .order = 60,
                                          .create = panel_create });
  cwed_command_register(&(cwed_command_desc){ .name = "turntable.list", .title = "List Turntables", .flags = CWED_CMD_QUERY,
    .doc = "Every Turntable node of the active scene: path, speed, radius, selected (plugin turntable).", .exec = cmd_list });
  cwed_command_register(&(cwed_command_desc){ .name = "turntable.gizmo", .title = "Turntable Gizmo", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The radius gizmo as last drawn: visible (a selected Turntable), the handle and centre in 3D viewport pixels, px "
           "(viewport pixels per UI unit), dragging, draws (plugin turntable).", .exec = cmd_gizmo });
  cwed_menu_add("Scene/Add Turntable", "node.add type=Turntable", 150);
  cwed_subscribe(CWED_EV_DOC_CHANGED | CWED_EV_DOC_ACTIVE | CWED_EV_DOC_CLOSED | CWED_EV_SELECTION, on_event, NULL);
}

static void frame(double dt) {
  (void)dt;
  if (!panel_dirty) return;
  panel_dirty = false;
  refresh_panel();
}

static void shutdown(void) {
  cw_array_free(&rows);
  list = info = (cw_node){ 0 };
}

CWED_PLUGIN(turntable) = { .title = "Turntable tools", .init = init, .frame = frame, .shutdown = shutdown };

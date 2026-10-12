/* ui_editor: an editor-shaped layout to prove the UI carries editor
 * complexity: a menu bar with submenus, a toolbar, nested split panes, a
 * scene Tree (this UI's own node tree, two columns), a FileSystem list, a
 * code view (TextEdit with line numbers and C highlighting), and an
 * inspector built by REFLECTION from the selected node's properties: a grid
 * of labels and SpinBoxes / CheckBoxes / LineEdits / OptionButtons per
 * type, grouped by owning node type, that edit the node live. The layout is
 * a .cwscene with a theme file (themes/editor.theme). Selecting another
 * tree row (mouse or keys) rebuilds the inspector. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

#define MAX_BIND 512

typedef struct { cw_node target; const cw_prop *prop; int comp; } binding;

static struct {
  cw_tree *tree;
  cw_node root, scene_tree, props, selected_label, code, status;
  binding binds[MAX_BIND];
  int nbind;
  int32_t tree_rows, inspector_rows, inspected_props, edits, selections;
  uint32_t selected_hash;
  float layout_ms;
} E;

/* ── the scene tree: this UI's own nodes ────────────────────────────── */

static void add_tree(cw_node n, int parent, int depth) {
  if (depth > 6) return;
  int item = cw_uitree_create_item(E.scene_tree, parent, cw_node_name(n));
  cw_uitree_set_text(E.scene_tree, item, 1, cw_node_get_type(n)->name);
  cw_uitree_set_metadata(E.scene_tree, item, (int64_t)n.id);
  if (depth >= 4) cw_uitree_set_collapsed(E.scene_tree, item, true);
  for (int i = 0; i < cw_node_child_count(n); i++) add_tree(cw_node_child(n, i), item, depth + 1);
}

/* ── the inspector ──────────────────────────────────────────────────── */

static void apply(cw_node e, const cw_value *a, int c, void *user) {
  (void)e, (void)c;
  binding *b = &E.binds[(intptr_t)user];
  cw_value cur;
  if (!cw_node_valid(b->target) || !cw_node_get_prop(b->target, b->prop->name, &cur)) return;
  cw_value v = cur;
  switch (b->prop->type) {
  case CW_T_FLOAT: v.f = a[0].f; break;
  case CW_T_INT: case CW_T_ENUM: v.i = a[0].type == CW_T_FLOAT ? (int64_t)a[0].f : a[0].i; break;
  case CW_T_BOOL: v.b = a[0].b; break;
  case CW_T_STRING: v = cw_val_string(a[0].s); break;
  case CW_T_VEC2: v.v[b->comp] = (float)a[0].f; break;
  default: return;
  }
  if (cw_node_set(b->target, b->prop->name, v)) E.edits++;
}

static int bind(cw_node target, const cw_prop *p, int comp) {
  if (E.nbind >= MAX_BIND) return -1;
  E.binds[E.nbind] = (binding){ target, p, comp };
  return E.nbind++;
}

static cw_node spin(cw_node target, const cw_prop *p, int comp, double value, bool integer) {
  cw_node s = cw_node_new("SpinBox", NULL);
  cw_range_set_limits(s, -100000, 100000, integer ? 1 : 0.01);
  cw_range_set_value(s, value);
  cw_control_set_size_flags(s, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  int k = bind(target, p, comp);
  if (k >= 0) cw_connect(s, "value_changed", (cw_node){ 0 }, NULL, apply, (void *)(intptr_t)k, 0);
  return s;
}

static cw_node editor_for(cw_node target, const cw_prop *p) {
  cw_value v;
  if (!cw_node_get_prop(target, p->name, &v)) return (cw_node){ 0 };
  cw_node w = { 0 };
  int k;
  switch (p->type) {
  case CW_T_FLOAT: return spin(target, p, 0, v.f, false);
  case CW_T_INT: return spin(target, p, 0, (double)v.i, true);
  case CW_T_ENUM:
    if (p->enum_names) {
      w = cw_node_new("OptionButton", NULL);
      for (int i = 0; p->enum_names[i]; i++) cw_option_button_add_item(w, p->enum_names[i]);
      cw_option_button_select(w, (int)v.i);
      if ((k = bind(target, p, 0)) >= 0) cw_connect(w, "item_selected", (cw_node){ 0 }, NULL, apply, (void *)(intptr_t)k, 0);
      return w;
    }
    return spin(target, p, 0, (double)v.i, true);
  case CW_T_BOOL:
    w = cw_node_new("CheckBox", NULL);
    cw_button_set_text(w, v.b ? "On" : "Off");
    cw_button_set_pressed(w, v.b);
    if ((k = bind(target, p, 0)) >= 0) cw_connect(w, "toggled", (cw_node){ 0 }, NULL, apply, (void *)(intptr_t)k, 0);
    return w;
  case CW_T_STRING:
    w = cw_node_new("LineEdit", NULL);
    cw_line_edit_set_text(w, v.s ? v.s : "");
    if ((k = bind(target, p, 0)) >= 0) cw_connect(w, "text_submitted", (cw_node){ 0 }, NULL, apply, (void *)(intptr_t)k, 0);
    return w;
  case CW_T_VEC2: {
    w = cw_node_new("HBoxContainer", NULL);
    bool ro = p->flags & CW_PROP_READONLY;
    for (int c = 0; c < 2; c++) {
      cw_node s = spin(target, p, c, v.v[c], false);
      if (ro) cw_node_set(s, "mouse_filter", cw_val_int(CW_MOUSE_IGNORE)), cw_control_set_modulate(s, cw_rgba(1, 1, 1, 0.6f));
      cw_node_add_child(w, s);
    }
    return w;
  }
  default: {
    char buf[96];
    cw_value_format(&v, buf, sizeof buf);
    w = cw_node_new("Label", NULL);
    cw_label_set_text(w, buf);
    cw_node_set(w, "ellipsis", cw_val_bool(true));
    return w;
  }
  }
}

static void clear_children(cw_node n) {
  while (cw_node_child_count(n)) cw_node_free(cw_node_child(n, cw_node_child_count(n) - 1));
}

static void inspect(cw_node target) {
  clear_children(E.props);
  E.nbind = 0;
  E.inspector_rows = E.inspected_props = 0;
  char title[160];
  snprintf(title, sizeof title, "%s  (%s)", cw_node_name(target), cw_node_get_type(target)->name);
  cw_label_set_text(E.selected_label, title);
  E.selected_hash = cw_hash32(title, strlen(title), CW_HASH32_INIT);
  const cw_prop *props[256];
  const cw_node_type *owners[256];
  int n = cw_node_props(target, props, owners, 256);
  if (n > 256) n = 256;
  const cw_node_type *group = NULL;
  cw_node grid = { 0 };
  /* #region inspector */
  for (int i = 0; i < n; i++) {
    const cw_prop *p = props[i];
    if (!(p->flags & CW_PROP_EDITOR)) continue;
    if (owners[i] != group) { /* a header per owning type */
      group = owners[i];
      cw_node head = cw_node_new("PanelContainer", NULL);
      cw_control_override_style(head, "panel", &(cw_ui_style){ 0 });
      cw_ui_style hs = cw_control_theme_style(E.props, "Inspector", "header");
      cw_control_override_style(head, "panel", &hs);
      cw_node hl = cw_node_new("Label", NULL);
      cw_label_set_text(hl, group->name);
      cw_node_add_child(head, hl);
      cw_node_add_child(E.props, head);
      grid = cw_node_new("GridContainer", NULL);
      cw_node_set(grid, "columns", cw_val_int(2));
      cw_node_add_child(E.props, grid);
    }
    cw_node l = cw_node_new("Label", NULL);
    cw_label_set_text(l, p->name);
    cw_control_set_tooltip(l, p->doc);
    cw_node_set(l, "mouse_filter", cw_val_int(CW_MOUSE_STOP));
    cw_control_set_custom_min_size(l, (cw_vec2){ 110, 0 });
    cw_node_set(l, "ellipsis", cw_val_bool(true));
    cw_node w = editor_for(target, p);
    if (CW_HANDLE_IS_NULL(w)) {
      cw_node_free(l);
      continue;
    }
    cw_control_set_size_flags(w, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
    cw_node_add_child(grid, l);
    cw_node_add_child(grid, w);
    E.inspector_rows++;
  }
  E.inspected_props = n;
  /* #endregion */
}

static void on_select(cw_node t, const cw_value *a, int c, void *u) {
  (void)c, (void)u;
  cw_node target = { (uint64_t)cw_uitree_metadata(t, (int)a[0].i) };
  if (!cw_node_valid(target)) return;
  E.selections++;
  inspect(target);
}

static const char *CODE_HEAD =
    "/* player.c: movement, jumping and the camera follow */\n"
    "#include \"cartwheel/input.h\"\n"
    "#include \"cartwheel/scene.h\"\n\n"
    "typedef struct player {\n"
    "    float speed;      // units per second\n"
    "    float jump;       // initial jump velocity\n"
    "    int coins;\n"
    "    bool on_floor;\n"
    "} player;\n\n";

static void init(void *u) {
  (void)u;
  cw_scene_register_builtin_types();
  E.tree = cw_tree_new();
  cw_set_current_tree(E.tree);
  E.root = cw_scene_instance_path("scenes/editor.cwscene");
  cw_node_add_child(cw_tree_root(E.tree), E.root);
  E.scene_tree = cw_node_get(E.root, "Layout/Body/Main/Docks/ScenePanel/SceneTree");
  E.props = cw_node_get(E.root, "Layout/Body/Main/Right/Inspector/Scroll/Props");
  E.selected_label = cw_node_get(E.root, "Layout/Body/Main/Right/Inspector/Selected");
  E.code = cw_node_get(E.root, "Layout/Body/Main/Right/Center/player.c");
  E.status = cw_node_get(E.root, "Layout/Status/StatusText");
  cw_uitree_set_column_title(E.scene_tree, 0, "Node");
  cw_uitree_set_column_title(E.scene_tree, 1, "Type");
  cw_uitree_set_column_expand(E.scene_tree, 0, true, 60);
  cw_uitree_set_column_expand(E.scene_tree, 1, false, 92);
  int root_item = cw_uitree_create_item(E.scene_tree, -1, "root");
  add_tree(cw_node_get(E.root, "Layout"), root_item, 0);
  E.tree_rows = cw_uitree_visible_rows(E.scene_tree);
  cw_connect(E.scene_tree, "item_selected", (cw_node){ 0 }, NULL, on_select, NULL, 0);
  /* a code file of realistic length: 12 functions */
  size_t cap = 16384, len = 0;
  char *code = cw_frame_alloc(cap);
  len += (size_t)snprintf(code + len, cap - len, "%s", CODE_HEAD);
  for (int i = 0; i < 12; i++)
    len += (size_t)snprintf(code + len, cap - len,
                            "static void step_%02d(player *p, float dt) {\n"
                            "    if (p->on_floor && cw_action_just_pressed(0, %d)) p->on_floor = false;\n"
                            "    p->speed += %d.5f * dt; /* accelerate */\n"
                            "    p->coins += \"abc\"[%d %% 3] == 'b';\n"
                            "}\n\n", i, i, i + 1, i);
  cw_text_edit_set_text(E.code, code);
  cw_text_edit_set_caret(E.code, 6, 4);
  /* inspect the Play button: select its row */
  cw_node play = cw_node_get(E.root, "Layout/Top/TopRow/Play");
  for (int i = 0; i < 400; i++)
    if ((uint64_t)cw_uitree_metadata(E.scene_tree, i) == play.id) {
      cw_uitree_select(E.scene_tree, i);
      break;
    }
  cw_debug_i32("editor.tree_rows", &E.tree_rows);
  cw_debug_i32("editor.inspector_rows", &E.inspector_rows);
  cw_debug_i32("editor.inspected_props", &E.inspected_props);
  cw_debug_i32("editor.edits", &E.edits);
  cw_debug_i32("editor.selections", &E.selections);
  cw_debug_u32("editor.selected_hash", &E.selected_hash);
  cw_debug_f32("editor.layout_ms", &E.layout_ms);
  cw_ui_debug_register("ui");
}

static void update(void *u, double dt) {
  (void)u, (void)dt;
  static int frame;
  if (++frame == 15) { /* select the code view: the inspector rebuilds for a TextEdit */
    cw_node code = E.code;
    for (int i = 0; i < 400; i++)
      if ((uint64_t)cw_uitree_metadata(E.scene_tree, i) == code.id) {
        cw_uitree_select(E.scene_tree, i);
        break;
      }
  }
  E.tree_rows = cw_uitree_visible_rows(E.scene_tree);
  E.layout_ms = cw_ui_get_stats()->layout_ms;
  char st[128];
  snprintf(st, sizeof st, "%u controls  |  %d inspector rows  |  %d edits", cw_ui_get_stats()->controls, E.inspector_rows,
           E.edits);
  cw_label_set_text(E.status, st);
}

static void render(void *u, double a) {
  (void)u, (void)a;
  int w, h;
  cw_drawable_size(&w, &h);
  (void)w, (void)h;
  cw_gpu_clear_window(0, 0, 0, 1); /* both backends */
  cw_ui_render(E.tree);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "ui editor", .width = 1280, .height = 720, .init = init, .update = update,
                        .render = render, .debug = true };
}

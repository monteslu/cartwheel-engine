/* ui_interact: the UI driven by input scripts (tests/ui-*.json). Every
 * widget a player or an editor user touches, at fixed rects, with every
 * signal counted into named debug fields: clicks, toggles, slider drags,
 * typing (LineEdit, TextEdit), list and tree selection, menus with a
 * submenu, an option popup, a modal dialog, wheel and touch scrolling,
 * split dragging and focus moves by keyboard and gamepad. The gate compares
 * these values across hosts. */
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/scene.h"
#include "cartwheel/ui.h"

typedef struct {
  int32_t pressed, toggled, check_on, slider_changes, text_changes, submits, option_sel, list_sel, list_activated;
  int32_t menu_id, menu_count, dialog_ok, dialog_cancel, focus_moves, tree_sel, tree_collapses, spin_changes;
  int32_t meta_clicks;
  uint32_t meta_hash;
  int32_t text_lines, edit_caret, dialog_pressed, scroll_y, split_x, unhandled;
  float slider, spin;
  uint32_t edit_hash, submitted_hash, focus_hash, text_hash; /* romdev reads no byte fields: hashes */
  int32_t edit_len;
  char edit[48], submitted[48], focus[24];
  float probe[8][2]; /* centres of things a script clicks that sit in popups */
} state;
static state S;
static cw_tree *tree;
static cw_node root, menu_file, menu_recent, option, list, tedit, edit, scroll, split, dlg, slider, tr;

static cw_node add(const char *type, const char *name, cw_rect r) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(root, n);
  cw_control_set_rect(n, r);
  return n;
}

static void on(cw_node n, const char *sig, cw_signal_fn fn) { cw_connect(n, sig, (cw_node){ 0 }, NULL, fn, NULL, 0); }
static void s_pressed(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; S.pressed++; }
static void s_toggled(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; S.toggled++; S.check_on = a[0].b; }
static void s_slider(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; S.slider_changes++; S.slider = (float)a[0].f; }
static void s_spin(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; S.spin_changes++; S.spin = (float)a[0].f; }
static void s_text(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; S.text_changes++; }
static void s_submit(cw_node e, const cw_value *a, int c, void *u) {
  (void)e, (void)c, (void)u;
  S.submits++;
  strncpy(S.submitted, a[0].s, sizeof S.submitted - 1);
}
static void s_option(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; S.option_sel = (int32_t)a[0].i; }
static void s_list(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; S.list_sel = (int32_t)a[0].i; }
static void s_list_act(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; S.list_activated++; }
static void s_menu(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; S.menu_id = (int32_t)a[0].i; S.menu_count++; }
static void s_ok(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; S.dialog_ok++; }
static void s_cancel(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; S.dialog_cancel++; }
static void s_focus(cw_node e, const cw_value *a, int c, void *u) {
  (void)a, (void)c, (void)u;
  S.focus_moves++;
  strncpy(S.focus, cw_node_name(e), sizeof S.focus - 1);
}
static void s_tree(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)c, (void)u; S.tree_sel = (int32_t)a[0].i; }
static void s_collapse(cw_node e, const cw_value *a, int c, void *u) { (void)e, (void)a, (void)c, (void)u; S.tree_collapses++; }
static void s_open_dialog(cw_node e, const cw_value *a, int c, void *u) {
  (void)e, (void)a, (void)c, (void)u;
  S.dialog_pressed++;
  cw_dialog_popup_centered(dlg);
}
static void s_meta(cw_node e, const cw_value *a, int c, void *u) {
  (void)e, (void)c, (void)u;
  S.meta_clicks++;
  S.meta_hash = cw_hash32(a[0].s, strlen(a[0].s), CW_HASH32_INIT);
}
static void unhandled(const cw_ui_event *e, void *u) {
  (void)u;
  if (e->type == CW_UI_MOUSE_DOWN || e->type == CW_UI_KEY) S.unhandled++;
}

static void init(void *u) {
  (void)u;
  cw_scene_register_builtin_types();
  tree = cw_tree_new();
  cw_set_current_tree(tree);
  root = cw_node_new("Panel", "Root");
  cw_node_add_child(cw_tree_root(tree), root);
  cw_control_set_anchors_preset(root, CW_PRESET_FULL_RECT, 0);

  cw_node b = add("Button", "Click", (cw_rect){ 20, 20, 120, 34 });
  cw_button_set_text(b, "Click me");
  cw_control_set_tooltip(b, "Counts presses");
  on(b, "pressed", s_pressed);
  cw_node chk = add("CheckBox", "Check", (cw_rect){ 160, 20, 140, 34 });
  cw_button_set_text(chk, "Enabled");
  on(chk, "toggled", s_toggled);
  slider = add("HSlider", "Slider", (cw_rect){ 320, 20, 200, 34 });
  on(slider, "value_changed", s_slider);
  cw_node spin = add("SpinBox", "Spin", (cw_rect){ 540, 20, 140, 34 });
  on(spin, "value_changed", s_spin);

  edit = add("LineEdit", "Edit", (cw_rect){ 20, 70, 240, 34 });
  cw_node_set(edit, "placeholder_text", cw_val_string("type here"));
  on(edit, "text_changed", s_text);
  on(edit, "text_submitted", s_submit);
  option = add("OptionButton", "Option", (cw_rect){ 280, 70, 160, 34 });
  for (int i = 0; i < 5; i++) cw_option_button_add_item(option, (const char *[]){ "Alpha", "Beta", "Gamma", "Delta", "Epsilon" }[i]);
  on(option, "item_selected", s_option);
  cw_node bar = add("MenuBar", "Menu", (cw_rect){ 460, 70, 200, 34 });
  menu_file = cw_node_new("PopupMenu", "File");
  cw_node_add_child(bar, menu_file);
  cw_node_set(menu_file, "items", cw_val_string("New|1\nRecent>Recent\n-\nQuit|9"));
  menu_recent = cw_node_new("PopupMenu", "Recent");
  cw_node_add_child(menu_file, menu_recent);
  cw_node_set(menu_recent, "items", cw_val_string("One|11\nTwo|12\nThree|13"));
  on(menu_file, "id_pressed", s_menu);
  on(menu_recent, "id_pressed", s_menu);
  cw_node dbtn = add("Button", "Dialog", (cw_rect){ 680, 70, 100, 34 });
  cw_button_set_text(dbtn, "Delete...");
  on(dbtn, "pressed", s_open_dialog);
  dlg = cw_node_new("ConfirmationDialog", "Confirm");
  cw_node_add_child(root, dlg);
  cw_node_set(dlg, "title", cw_val_string("Delete scene?"));
  cw_node_set(dlg, "dialog_text", cw_val_string("This cannot be undone."));
  on(dlg, "confirmed", s_ok);
  on(dlg, "canceled", s_cancel);

  list = add("ItemList", "List", (cw_rect){ 20, 120, 200, 150 });
  for (int i = 0; i < 30; i++) cw_item_list_add_item(list, cw_frame_printf("item %02d", i), (cw_texture){ 0 });
  on(list, "item_selected", s_list);
  on(list, "item_activated", s_list_act);
  tedit = add("TextEdit", "Text", (cw_rect){ 240, 120, 300, 150 });
  cw_node_set(tedit, "show_line_numbers", cw_val_bool(true));
  cw_node_set(tedit, "syntax_highlighting", cw_val_int(1));
  cw_text_edit_set_text(tedit, "int x = 1;");
  scroll = add("ScrollContainer", "Scroll", (cw_rect){ 560, 120, 220, 150 });
  cw_node vb = cw_node_new("VBoxContainer", "Rows");
  cw_node_add_child(scroll, vb);
  cw_control_set_size_flags(vb, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  for (int i = 0; i < 20; i++) {
    cw_node r = cw_node_new("Button", cw_frame_printf("Row%d", i));
    cw_button_set_text(r, cw_frame_printf("Row %d", i));
    cw_node_add_child(vb, r);
    on(r, "pressed", s_pressed);
  }
  split = add("HSplitContainer", "Split", (cw_rect){ 20, 290, 300, 80 });
  cw_node pa = cw_node_new("Panel", "A"), pb = cw_node_new("Panel", "B");
  cw_node_add_child(split, pa);
  cw_node_add_child(split, pb);
  cw_control_set_custom_min_size(pa, (cw_vec2){ 60, 0 });
  cw_control_set_custom_min_size(pb, (cw_vec2){ 60, 0 });
  tr = add("Tree", "Tree", (cw_rect){ 340, 290, 200, 170 });
  cw_node_set(tr, "items", cw_val_string("Level\n  Player\n    Camera\n    Sprite\n  Enemies\n    Goblin\n  HUD"));
  on(tr, "item_selected", s_tree);
  on(tr, "item_collapsed", s_collapse);

  cw_node rich = add("RichTextLabel", "Rich", (cw_rect){ 20, 390, 300, 60 });
  cw_node_set(rich, "text", cw_val_string("[b]Docs:[/b] read [url=manual]the manual[/url] or [color=#ff8a5c]ask[/color]."));
  on(rich, "meta_clicked", s_meta);

  /* focus tracking for every focusable control */
  cw_node all[] = { b, chk, slider, spin, edit, option, dbtn, list, tedit, tr };
  for (size_t i = 0; i < CW_ARRAY_LEN(all); i++) on(all[i], "focus_entered", s_focus);
  cw_ui_set_unhandled_input(unhandled, NULL);

#define I32(f) cw_debug_i32(#f, &S.f)
  I32(pressed); I32(toggled); I32(check_on); I32(slider_changes); I32(text_changes); I32(submits); I32(option_sel);
  I32(list_sel); I32(list_activated); I32(menu_id); I32(menu_count); I32(dialog_ok); I32(dialog_cancel); I32(focus_moves);
  I32(tree_sel); I32(tree_collapses); I32(spin_changes); I32(text_lines); I32(edit_caret); I32(dialog_pressed);
  I32(scroll_y); I32(split_x); I32(unhandled);
#undef I32
  cw_debug_i32("meta_clicks", &S.meta_clicks);
  cw_debug_u32("meta_hash", &S.meta_hash);
  cw_debug_f32("slider", &S.slider);
  cw_debug_f32("spin", &S.spin);
  cw_debug_bytes("edit", S.edit, sizeof S.edit);
  cw_debug_u32("edit_hash", &S.edit_hash);
  cw_debug_u32("submitted_hash", &S.submitted_hash);
  cw_debug_u32("focus_hash", &S.focus_hash);
  cw_debug_u32("text_hash", &S.text_hash);
  cw_debug_i32("edit_len", &S.edit_len);
  cw_debug_bytes("submitted", S.submitted, sizeof S.submitted);
  cw_debug_bytes("focus", S.focus, sizeof S.focus);
  cw_debug_f32_array("probe", &S.probe[0][0], 16);
  S.option_sel = S.list_sel = S.menu_id = S.tree_sel = -1;
  cw_ui_debug_register("ui");
}

static void update(void *u, double dt) {
  (void)u, (void)dt;
  memset(S.edit, 0, sizeof S.edit);
  strncpy(S.edit, cw_line_edit_text(edit), sizeof S.edit - 1);
  S.edit_caret = cw_line_edit_caret(edit);
  S.edit_len = (int32_t)strlen(S.edit);
  S.edit_hash = cw_hash32(S.edit, strlen(S.edit), CW_HASH32_INIT);
  S.submitted_hash = cw_hash32(S.submitted, strlen(S.submitted), CW_HASH32_INIT);
  S.focus_hash = cw_hash32(S.focus, strlen(S.focus), CW_HASH32_INIT);
  const char *tx = cw_text_edit_text(tedit);
  S.text_hash = cw_hash32(tx, strlen(tx), CW_HASH32_INIT);
  S.text_lines = cw_text_edit_line_count(tedit);
  S.scroll_y = (int32_t)cw_scroll_get(scroll).y;
  S.split_x = (int32_t)cw_control_rect(cw_node_child(split, 0)).w;
  /* where popup items land (scripts click these; also compared) */
  cw_rect f = cw_control_global_rect(menu_file), r = cw_control_global_rect(menu_recent);
  S.probe[0][0] = f.x + f.w * 0.5f, S.probe[0][1] = f.y + f.h * 0.5f;
  S.probe[1][0] = r.x + r.w * 0.5f, S.probe[1][1] = r.y + r.h * 0.5f;
  cw_rect o = cw_control_global_rect(option);
  S.probe[2][0] = o.x, S.probe[2][1] = o.y + o.h;
  cw_rect d = cw_control_global_rect(dlg);
  S.probe[3][0] = d.x + d.w, S.probe[3][1] = d.y + d.h;
}

static void render(void *u, double a) {
  (void)u, (void)a;
  int w, h;
  cw_drawable_size(&w, &h);
  (void)w, (void)h;
  cw_gpu_clear_window(0, 0, 0, 1); /* both backends */
  cw_ui_render(tree);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "ui interact", .width = 800, .height = 480, .init = init, .update = update,
                        .render = render, .debug = true };
}

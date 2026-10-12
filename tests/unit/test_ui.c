/* UI layout suite: computed rects of anchors, presets, size flags and every
 * container, checked exactly against hand-computed values; dirty tracking
 * (only what changed is re-laid out); content-derived sizes; theme text
 * round trip; controls through scene files. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/ui.h"
#include "test.h"
#include <string.h>

static cw_tree *T;
#define SM(a, b) ((a) + (b))

static cw_node mk(const char *type, const char *name, cw_node parent, float mw, float mh) {
  cw_node n = cw_node_new(type, name);
  if (mw || mh) cw_control_set_custom_min_size(n, (cw_vec2){ mw, mh });
  if (!CW_HANDLE_IS_NULL(parent)) cw_node_add_child(parent, n);
  return n;
}

/* a top-level 400x300 frame at (0,0) */
static cw_node frame(const char *type) {
  cw_node f = mk(type, "Frame", cw_tree_root(T), 0, 0);
  cw_control_set_rect(f, (cw_rect){ 0, 0, 400, 300 });
  return f;
}

static int rect_is(cw_node n, float x, float y, float w, float h) {
  cw_rect r = cw_control_rect(n);
  FOLD(r);
  bool ok = r.x == x && r.y == y && r.w == w && r.h == h;
  if (!ok) cw_error("rect of %s = (%g %g %g %g), expected (%g %g %g %g)", cw_node_name(n), (double)r.x, (double)r.y,
                    (double)r.w, (double)r.h, (double)x, (double)y, (double)w, (double)h);
  return ok;
}
#define RECT(n, x, y, w, h) CHECK(rect_is(n, x, y, w, h))

static void reset(void) {
  cw_scene_register_builtin_types();
  if (T) cw_tree_free(T);
  T = cw_tree_new();
  cw_set_current_tree(T);
}

static void test_anchors(void) {
  reset();
  cw_node f = frame("Control");
  cw_node a = mk("Control", "Centered", f, 0, 0);
  cw_control_set_anchors(a, 0.5f, 0.5f, 0.5f, 0.5f);
  cw_control_set_offsets(a, -50, -20, 50, 20);
  cw_node b = mk("Control", "Wide", f, 0, 0);
  cw_control_set_anchors(b, 0, 1, 1, 1);
  cw_control_set_offsets(b, 10, -30, -10, -10);
  /* min size bigger than the anchored rect: grows per grow direction */
  cw_node g1 = mk("Control", "GrowEnd", f, 80, 0);
  cw_control_set_offsets(g1, 100, 0, 120, 10);
  cw_node g2 = mk("Control", "GrowBegin", f, 80, 0);
  cw_control_set_offsets(g2, 100, 0, 120, 10);
  cw_control_of(g2)->grow_horizontal = CW_GROW_BEGIN;
  cw_node g3 = mk("Control", "GrowBoth", f, 80, 0);
  cw_control_set_offsets(g3, 100, 0, 120, 10);
  cw_control_of(g3)->grow_horizontal = CW_GROW_BOTH;
  cw_ui_layout(T);
  RECT(f, 0, 0, 400, 300);
  RECT(a, 150, 130, 100, 40);
  RECT(b, 10, 270, 380, 20);
  RECT(g1, 100, 0, 80, 10);
  RECT(g2, 40, 0, 80, 10);
  RECT(g3, 70, 0, 80, 10);
  /* presets */
  cw_node p = mk("Control", "Full", f, 0, 0);
  cw_control_set_anchors_preset(p, CW_PRESET_FULL_RECT, 10);
  cw_node q = mk("Control", "BR", f, 30, 20);
  cw_control_set_anchors_preset(q, CW_PRESET_BOTTOM_RIGHT, 5);
  cw_node c = mk("Control", "C", f, 60, 40);
  cw_control_set_anchors_preset(c, CW_PRESET_CENTER, 0);
  cw_ui_layout(T);
  RECT(p, 10, 10, 380, 280);
  RECT(q, 365, 275, 30, 20);
  RECT(c, 170, 130, 60, 40);
  /* the parent resizes: anchored children follow */
  cw_control_set_rect(f, (cw_rect){ 0, 0, 200, 100 });
  cw_ui_layout(T);
  RECT(a, 50, 30, 100, 40);
  RECT(p, 10, 10, 180, 80);
  RECT(q, 165, 75, 30, 20);
  cw_vec2 gp = { cw_control_global_rect(q).x, cw_control_global_rect(q).y };
  CHECK(gp.x == 165 && gp.y == 75);
}

static void test_box(void) {
  reset();
  cw_node f = frame("HBoxContainer");
  cw_control_override_constant(f, "separation", 4);
  cw_node c1 = mk("Control", "C1", f, 50, 20), c2 = mk("Control", "C2", f, 50, 20), c3 = mk("Control", "C3", f, 50, 20);
  cw_control_set_size_flags(c1, CW_SIZE_FILL, CW_SIZE_SHRINK_CENTER);
  cw_control_set_size_flags(c2, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_control_set_size_flags(c3, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_END);
  cw_control_set_stretch_ratio(c3, 2);
  cw_ui_layout(T);
  /* avail 392, mins 150 -> extra 242 + expanders' mins 100 = 342 shared 1:2 */
  RECT(c1, 0, 140, 50, 20);
  RECT(c2, 54, 0, 114, 300);
  RECT(c3, 172, 280, 228, 20);
  CHECK(cw_control_min_size(f).x == 158 && cw_control_min_size(f).y == 20);
  /* refit: an expander whose share is below its min keeps its min */
  cw_control_set_custom_min_size(c3, (cw_vec2){ 300, 20 });
  cw_ui_layout(T);
  /* c3 drops out at 300; c2 gets 392 - 50 - 300 = 42 < its min 50 -> min */
  RECT(c2, 54, 0, 50, 300);
  RECT(c3, 108, 280, 300, 20);
  /* hidden children take no space; no expander: alignment */
  cw_control_set_size_flags(c2, CW_SIZE_FILL, CW_SIZE_FILL);
  cw_control_set_size_flags(c3, CW_SIZE_FILL, CW_SIZE_FILL);
  cw_control_set_visible(c3, false);
  cw_node_set(f, "alignment", cw_val_int(1));
  cw_ui_layout(T);
  RECT(c1, 148, 140, 50, 20);
  RECT(c2, 202, 0, 50, 300);
  cw_node_set(f, "alignment", cw_val_int(2));
  cw_ui_layout(T);
  RECT(c2, 350, 0, 50, 300);
  /* VBox, theme separation */
  reset();
  cw_node v = frame("VBoxContainer");
  float sep = cw_control_theme_constant(v, NULL, "separation");
  CHECK(sep == 6);
  cw_node r1 = mk("Control", "R1", v, 10, 30), r2 = mk("Control", "R2", v, 10, 30);
  cw_control_set_size_flags(r2, CW_SIZE_SHRINK_CENTER, CW_SIZE_EXPAND_FILL);
  cw_ui_layout(T);
  RECT(r1, 0, 0, 400, 30);
  RECT(r2, 195, 36, 10, 264);
}

static void test_grid_margin_center(void) {
  reset();
  cw_node g = frame("GridContainer");
  cw_node_set(g, "columns", cw_val_int(3));
  cw_control_override_constant(g, "h_separation", 10);
  cw_control_override_constant(g, "v_separation", 5);
  cw_node k[5];
  float mins[5][2] = { { 40, 10 }, { 60, 20 }, { 30, 30 }, { 50, 15 }, { 20, 5 } };
  for (int i = 0; i < 5; i++) k[i] = mk("Control", NULL, g, mins[i][0], mins[i][1]);
  cw_ui_layout(T);
  /* columns 50 60 30, rows 30 15 */
  RECT(k[0], 0, 0, 50, 30);
  RECT(k[1], 60, 0, 60, 30);
  RECT(k[2], 130, 0, 30, 30);
  RECT(k[3], 0, 35, 50, 15);
  RECT(k[4], 60, 35, 60, 15);
  CHECK(cw_control_min_size(g).x == 160 && cw_control_min_size(g).y == 50);
  /* column 1 expands: it takes the rest */
  cw_control_set_size_flags(k[4], CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_ui_layout(T);
  RECT(k[4], 60, 35, 300, 15);
  RECT(k[2], 370, 0, 30, 30);

  reset();
  cw_node m = frame("MarginContainer");
  cw_node_set(m, "theme_overrides", cw_val_string("margin_left = 7; margin_top = 3; margin_right = 11; margin_bottom = 13"));
  cw_node in = mk("Control", "In", m, 10, 10);
  cw_ui_layout(T);
  RECT(in, 7, 3, 382, 284);
  CHECK(cw_control_min_size(m).x == 28 && cw_control_min_size(m).y == 26);

  reset();
  cw_node c = frame("CenterContainer");
  cw_node ci = mk("Control", "In", c, 41, 21);
  cw_ui_layout(T);
  RECT(ci, 180, 140, 41, 21);
}

static void test_split_scroll_flow_aspect(void) {
  reset();
  cw_node s = frame("HSplitContainer");
  cw_control_override_constant(s, "separation", 8);
  cw_node a = mk("Control", "A", s, 100, 10), b = mk("Control", "B", s, 50, 10);
  cw_ui_layout(T);
  RECT(a, 0, 0, 100, 300);
  RECT(b, 108, 0, 292, 300);
  cw_split_set_offset(s, 70);
  cw_ui_layout(T);
  RECT(a, 0, 0, 170, 300);
  RECT(b, 178, 0, 222, 300);
  cw_split_set_offset(s, 1000); /* clamped so B keeps its minimum */
  cw_ui_layout(T);
  RECT(a, 0, 0, 342, 300);
  RECT(b, 350, 0, 50, 300);
  cw_control_set_size_flags(a, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_control_set_size_flags(b, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_split_set_offset(s, 0);
  cw_ui_layout(T);
  RECT(a, 0, 0, 196, 300);

  reset();
  cw_node sc = frame("ScrollContainer");
  cw_node content = mk("VBoxContainer", "Content", sc, 0, 0);
  cw_node rows[20];
  for (int i = 0; i < 20; i++) rows[i] = mk("Control", NULL, content, 50, 40);
  cw_ui_layout(T);
  float sep = cw_control_theme_constant(content, NULL, "separation");
  float th = cw_control_theme_constant(sc, "ScrollBar", "thickness");
  float content_h = 20 * 40 + 19 * sep;
  RECT(content, 0, 0, 400 - th, content_h);
  cw_vec2 mx = cw_scroll_max(sc);
  CHECK(mx.x == 0 && mx.y == content_h - 300);
  cw_scroll_ensure_visible(sc, rows[10]);
  CHECK(cw_scroll_get(sc).y == 10 * (40 + sep) + 40 - 300);
  cw_rect gr = cw_control_global_rect(rows[10]);
  CHECK(gr.y + gr.h == 300);
  cw_scroll_set(sc, (cw_vec2){ 0, 1e6f });
  CHECK(cw_scroll_get(sc).y == mx.y);

  reset();
  cw_node fl = frame("HFlowContainer");
  cw_control_override_constant(fl, "h_separation", 10);
  cw_control_override_constant(fl, "v_separation", 4);
  cw_node it[5];
  for (int i = 0; i < 5; i++) it[i] = mk("Control", NULL, fl, 120, 20 + (float)i);
  cw_ui_layout(T);
  /* 3 per line in 400: 120+10+120+10+120 = 380 */
  RECT(it[0], 0, 0, 120, 22);
  RECT(it[2], 260, 0, 120, 22);
  RECT(it[3], 0, 26, 120, 24);
  RECT(it[4], 130, 26, 120, 24);
  CHECK(cw_control_min_size(fl).y == 50);

  reset();
  cw_node as = frame("AspectRatioContainer");
  cw_node_set(as, "ratio", cw_val_float(2));
  cw_node ai = mk("Control", "In", as, 0, 0);
  cw_ui_layout(T);
  RECT(ai, 0, 50, 400, 200); /* fit, centred */
}

static void test_tabs_panel(void) {
  reset();
  cw_node tabs = frame("TabContainer");
  cw_node p1 = mk("Control", "One", tabs, 10, 10), p2 = mk("Control", "Two", tabs, 10, 10);
  cw_ui_layout(T);
  CHECK(cw_control_of(p1)->visible && !cw_control_of(p2)->visible);
  cw_ui_style st = cw_control_theme_style(tabs, NULL, "panel");
  cw_rect r1 = cw_control_rect(p1);
  CHECK(r1.x == st.margin[0] && r1.w == 400 - st.margin[0] - st.margin[2] && r1.y + r1.h == 300 - st.margin[3]);
  cw_tab_container_set_current(tabs, 1);
  cw_ui_layout(T);
  CHECK(!cw_control_of(p1)->visible && cw_control_of(p2)->visible);
  CHECK(cw_control_rect(p2).w == r1.w && cw_control_rect(p2).y == r1.y);

  reset();
  cw_node pc = frame("PanelContainer");
  cw_node pin = mk("Control", "In", pc, 5, 5);
  cw_ui_layout(T);
  cw_ui_style ps = cw_control_theme_style(pc, NULL, "panel");
  RECT(pin, ps.margin[0], ps.margin[1], 400 - SM(ps.margin[0], ps.margin[2]), 300 - SM(ps.margin[1], ps.margin[3]));
}

static void test_content_sizes(void) {
  reset();
  cw_node f = frame("VBoxContainer");
  cw_node l = mk("Label", "L", f, 0, 0);
  cw_label_set_text(l, "Hello, layout");
  cw_node b = mk("Button", "B", f, 0, 0);
  cw_button_set_text(b, "Press me");
  cw_ui_layout(T);
  cw_font font = cw_control_theme_font(l, NULL, "font");
  float fs = cw_control_theme_font_size(l, NULL, "font_size");
  cw_text_metrics m = cw_font_measure(font, "Hello, layout", &(cw_text_style){ .size = fs });
  CHECK(cw_control_min_size(l).x == ceilf(m.width));
  CHECK(cw_control_min_size(l).y == m.height);
  /* per-frame text (a score, a timer) does not pile up interned strings */
  uint32_t base = cw_intern_counted();
  for (int i = 0; i < 3000; i++) {
    cw_label_set_text(l, cw_frame_printf("SCORE %05d", i));
    cw_button_set_text(b, cw_frame_printf("%d left", i));
  }
  CHECK(strcmp(cw_label_text(l), "SCORE 02999") == 0 && strcmp(cw_button_text(b), "2999 left") == 0);
  CHECK((int)cw_intern_counted() <= (int)base + 2);
  cw_label_set_text(l, "Hello, layout");
  cw_button_set_text(b, "Press me");
  cw_ui_style bs = cw_control_theme_style(b, NULL, "normal");
  float bw = cw_ui_text_width(cw_control_theme_font(b, NULL, "font"), cw_control_theme_font_size(b, NULL, "font_size"), "Press me");
  CHECK(cw_control_min_size(b).x == ceilf(bw + bs.margin[0] + bs.margin[2]));
  /* a bigger font override grows the button, and its parent re-lays out */
  float before = cw_control_rect(b).h;
  cw_control_override_constant(b, "font_size", 30);
  cw_ui_layout(T);
  CHECK(cw_control_rect(b).h > before);
  /* autowrap: height follows width */
  cw_node w = mk("Label", "Wrap", f, 0, 0);
  cw_node_set(w, "autowrap", cw_val_bool(true));
  cw_label_set_text(w, "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen");
  cw_ui_layout(T);
  float h400 = cw_control_rect(w).h;
  cw_control_set_rect(f, (cw_rect){ 0, 0, 120, 300 });
  cw_ui_layout(T);
  float h120 = cw_control_rect(w).h;
  CHECK(h120 > h400);
  cw_text_metrics wm = cw_font_measure(font, cw_label_text(w), &(cw_text_style){ .size = fs, .wrap_width = cw_control_rect(w).w });
  CHECK(cw_control_rect(w).w < 400);
  CHECK(h120 == wm.height);
  FOLD(h400);
  FOLD(h120);
}

static void test_dirty_tracking(void) {
  reset();
  cw_node f = frame("VBoxContainer");
  cw_node rows[10], cells[10][10];
  for (int i = 0; i < 10; i++) {
    rows[i] = mk("HBoxContainer", NULL, f, 0, 0);
    for (int j = 0; j < 10; j++) cells[i][j] = mk("Control", NULL, rows[i], 10, 10);
  }
  cw_ui_layout(T);
  const cw_ui_stats *st = cw_ui_get_stats();
  uint32_t passes = st->layout_passes;
  CHECK(st->arranged == 111);
  cw_ui_layout(T); /* clean: nothing happens */
  CHECK(st->layout_passes == passes);
  /* one cell grows wider: only its row's cells + its ancestors re-arrange,
   * and the box gives the other rows the same width (they keep it) */
  cw_control_set_custom_min_size(cells[3][4], (cw_vec2){ 30, 10 });
  cw_ui_layout(T);
  CHECK(st->layout_passes == passes + 1);
  cw_info("arranged after one cell grew: %u, measured %u", st->arranged, st->measured);
  CHECK(st->arranged == 3); /* frame, row 3 and the cell itself: clean siblings only move */
  CHECK(st->measured < 20);
  CHECK(cw_control_rect(cells[3][5]).x == 4 * 16 + 30 + 6);
  /* a new taller cell: every row moves down, nothing else re-lays out */
  cw_control_set_custom_min_size(cells[0][0], (cw_vec2){ 10, 25 });
  cw_ui_layout(T);
  CHECK(cw_control_rect(rows[9]).y == 9 * 16 + 15);
  cw_info("arranged after a row grew: %u", st->arranged);
  CHECK(st->arranged == 12); /* frame, row 0, its 10 cells (all got taller); rows 1-9 only move */
}

static void test_theme_text(void) {
  const char *src =
      "extends = light\n"
      "# a comment\n"
      "default_font_size = 17\n"
      "Button.font_color = #ff8040\n"
      "Button.normal = style(bg=#202428, border=1 2 3 4, border_color=#ffffff80, radius=6, margin=12 6)\n"
      "Button.hover = style(base=Button.normal, bg=#303438)\n"
      "Panel.panel = empty\n"
      "BoxContainer.separation = 9\n";
  cw_theme t = cw_theme_parse(src, "test.theme");
  CHECK(!CW_HANDLE_IS_NULL(t));
  cw_color c;
  CHECK(cw_theme_get_color(t, "Button", "font_color", &c) && c.r == 1 && c.b == 64.0f / 255);
  cw_ui_style s;
  CHECK(cw_theme_get_style(t, "Button", "hover", &s) && s.border[3] == 4 && s.radius[2] == 6 && s.margin[1] == 6);
  CHECK(s.bg.r == 0x30 / 255.0f);
  float k;
  CHECK(cw_theme_get_constant(t, "BoxContainer", "separation", &k) && k == 9);
  CHECK(cw_theme_get_constant(t, "", "default_font_size", &k) && k == 17);
  /* the base theme (light) answers what this one does not */
  CHECK(cw_theme_get_style(t, "LineEdit", "normal", &s) && s.draw);
  char *txt = cw_theme_save(t);
  cw_theme t2 = cw_theme_parse(txt, "resaved");
  char *txt2 = cw_theme_save(t2);
  CHECK(!strcmp(txt, txt2));
  uint32_t h = cw_hash32(txt, strlen(txt), CW_HASH32_INIT);
  FOLD(h);
  cw_free(txt);
  cw_free(txt2);
  /* a bad line is an error with its number */
  CHECK(CW_HANDLE_IS_NULL(cw_theme_parse("Button.normal = style(bg=#zz)\n", "bad.theme")));
  /* a control under a themed parent uses it; overrides beat both */
  reset();
  cw_node f = frame("VBoxContainer");
  cw_control_set_theme(f, t);
  cw_node b = mk("Button", "B", f, 0, 0);
  CHECK(cw_control_theme_color(b, NULL, "font_color").r == 1);
  CHECK(cw_control_theme_font_size(b, NULL, "font_size") == 17);
  cw_node_set(b, "theme_overrides", cw_val_string("font_color = #00ff00; font_size = 11"));
  CHECK(cw_control_theme_color(b, NULL, "font_color").g == 1 && cw_control_theme_font_size(b, NULL, "font_size") == 11);
  cw_theme_destroy(t);
  cw_theme_destroy(t2);
}

static void test_scene_file(void) {
  reset();
  const char *src =
      "[scene format=1]\n\n"
      "[node name=\"Root\" type=\"VBoxContainer\"]\n"
      "anchor_right = 1\n"
      "anchor_bottom = 1\n\n"
      "[node name=\"Title\" parent=\".\" type=\"Label\"]\n"
      "text = \"Hello\"\n"
      "horizontal_alignment = 1\n\n"
      "[node name=\"Go\" parent=\".\" type=\"Button\"]\n"
      "text = \"Go\"\n"
      "toggle_mode = true\n"
      "size_flags_vertical = 3\n\n"
      "[node name=\"List\" parent=\".\" type=\"ItemList\"]\n"
      "items = \"a\\nb\\nc\"\n\n"
      "[node name=\"Tree\" parent=\".\" type=\"Tree\"]\n"
      "items = \"Root\\n  Child|x\\n    Leaf\"\n";
  cw_packed_scene *ps = cw_scene_parse(src, "ui_test.cwscene");
  CHECK(ps != NULL);
  cw_node root = cw_scene_instance(ps);
  cw_node_add_child(cw_tree_root(T), root);
  CHECK(!strcmp(cw_label_text(cw_node_get(root, "Title")), "Hello"));
  CHECK(cw_item_list_count(cw_node_get(root, "List")) == 3);
  cw_node tr = cw_node_get(root, "Tree");
  CHECK(cw_uitree_visible_rows(tr) == 3);
  CHECK(!strcmp(cw_uitree_text(tr, cw_uitree_item_child(tr, 0, 0), 0), "Child"));
  char *saved = cw_scene_save(root);
  CHECK(strstr(saved, "text = \"Hello\"") != NULL);
  CHECK(strstr(saved, "items = \"Root\\n  Child\\n    Leaf\"") != NULL || strstr(saved, "items = \"Root\\n  Child") != NULL);
  cw_packed_scene *ps2 = cw_scene_parse(saved, "ui_test2.cwscene");
  cw_node root2 = cw_scene_instance(ps2);
  char *saved2 = cw_scene_save(root2);
  CHECK(!strcmp(saved, saved2));
  uint32_t h = cw_hash32(saved, strlen(saved), CW_HASH32_INIT);
  FOLD(h);
  cw_free(saved);
  cw_free(saved2);
  cw_node_free(root2);
  /* every UI node type is registered and creatable */
  static const char *types[] = { "Control", "Container", "HBoxContainer", "VBoxContainer", "GridContainer",
                                 "MarginContainer", "CenterContainer", "Panel", "PanelContainer", "ScrollContainer",
                                 "HSplitContainer", "VSplitContainer", "TabContainer", "HFlowContainer",
                                 "VFlowContainer", "AspectRatioContainer", "Label", "RichTextLabel", "Button",
                                 "CheckBox", "CheckButton", "OptionButton", "HSlider", "VSlider", "SpinBox",
                                 "ProgressBar", "LineEdit", "TextEdit", "ItemList", "Tree", "PopupMenu", "MenuBar",
                                 "AcceptDialog", "ConfirmationDialog", "ColorPicker", "TextureRect", "NinePatchRect" };
  int ok = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(types); i++) {
    cw_node n = cw_node_new(types[i], NULL);
    if (cw_is_control(n)) ok++;
    else cw_error("type %s is not a Control", types[i]);
    cw_node_free(n);
  }
  CHECK_EQ_INT(ok, (int)CW_ARRAY_LEN(types));
  /* image controls take a texture filter (pixel-art HUDs): by name, saved
   * only when it differs from LINEAR */
  for (int k = 0; k < 2; k++) {
    cw_node img = cw_node_new(k ? "NinePatchRect" : "TextureRect", "Img");
    cw_value fv;
    CHECK(cw_node_get_prop(img, "filter", &fv) && fv.i == 0);
    char *plain = cw_scene_save(img);
    CHECK(strstr(plain, "filter") == NULL);
    CHECK(cw_node_set(img, "filter", cw_val_enum("NEAREST")));
    CHECK(cw_node_get_prop(img, "filter", &fv) && fv.i == 1);
    char *near = cw_scene_save(img);
    CHECK(strstr(near, "filter = NEAREST") != NULL);
    cw_free(plain);
    cw_free(near);
    cw_node_free(img);
  }
}

static void count_resized(cw_node e, const cw_value *a, int c, void *u) {
  (void)e, (void)a, (void)c;
  (*(int *)u)++;
}

static void test_ranges(void) {
  reset();
  cw_node f = frame("VBoxContainer");
  cw_node s = mk("HSlider", "S", f, 0, 0);
  cw_range_set_limits(s, 0, 10, 0.5);
  cw_range_set_value(s, 3.3);
  CHECK(cw_range_value(s) == 3.5);
  cw_range_set_value(s, 99);
  CHECK(cw_range_value(s) == 10);
  cw_node_set(s, "value", cw_val_float(-4));
  CHECK(cw_range_value(s) == 0);
  /* button groups: exclusive toggles */
  cw_node a = mk("Button", "GA", f, 0, 0), b = mk("Button", "GB", f, 0, 0);
  cw_button_set_toggle_mode(a, true);
  cw_button_set_toggle_mode(b, true);
  cw_button_set_group(a, "g");
  cw_button_set_group(b, "g");
  cw_button_set_pressed(a, true);
  cw_button_set_pressed(b, true);
  CHECK(!cw_button_pressed(a) && cw_button_pressed(b));
  /* resized fires once per size change, after the layout */
  static int resized;
  resized = 0;
  cw_connect(f, "resized", (cw_node){ 0 }, NULL, count_resized, &resized, 0);
  cw_ui_layout(T);
  cw_control_set_rect(f, (cw_rect){ 0, 0, 300, 300 });
  cw_ui_layout(T);
  cw_ui_layout(T);
  CHECK_EQ_INT(resized, 1);
}

/* found while writing the docs: titles past 2 KB in total, NULL, a newline in a title */
static void test_tab_titles(void) {
  reset();
  cw_node tabs = frame("TabContainer");
  char want[40][128];
  for (int i = 0; i < 40; i++) {
    mk("Control", cw_frame_printf("P%d", i), tabs, 1, 1);
    snprintf(want[i], sizeof want[i], "%02d:%0100d", i, 0); /* 103 bytes each: 4 KB in all */
    cw_tab_container_set_title(tabs, i, want[i]);
  }
  int same = 0;
  for (int i = 0; i < 40; i++) {
    const char *t = cw_tab_container_title(tabs, i);
    same += t && !strcmp(t, want[i]);
  }
  CHECK_EQ_INT(same, 40);
  cw_tab_container_set_title(tabs, 3, NULL); /* NULL = back to the page name */
  CHECK(!strcmp(cw_tab_container_title(tabs, 3), "P3"));
  cw_tab_container_set_title(tabs, 1, "two\nlines"); /* one title, later titles unshifted */
  CHECK(!strcmp(cw_tab_container_title(tabs, 1), "two lines"));
  CHECK(!strcmp(cw_tab_container_title(tabs, 2), want[2]));
  cw_tab_container_set_title(tabs, -1, "x"); /* ignored */
  CHECK(!strcmp(cw_tab_container_title(tabs, 0), want[0]));
}

/* cw_ui_render puts back the r2d layer settings of the UI layer (its zoom
 * camera used to stay and scale a later pass on that layer) */
static void test_render_layer_restored(void) {
  reset();
  cw_node f = frame("Panel");
  (void)f;
  int layer = cw_ui_layer();
  cw_r2d_layer_set(layer, NULL);
  cw_ui_render(T);
  CHECK(!cw_r2d_layer_get(layer)->has_camera);
}

/* caret and selection snap to UTF-8 code point starts */
static void test_line_edit_utf8(void) {
  reset();
  cw_node f = frame("VBoxContainer");
  cw_node le = mk("LineEdit", "LE", f, 0, 0);
  cw_line_edit_set_text(le, "a\xc3\xa9\xe2\x82\xac" "b"); /* a e-acute euro b: bytes 0 1 3 6 */
  cw_line_edit_set_caret(le, 2);
  CHECK_EQ_INT(cw_line_edit_caret(le), 1);
  cw_line_edit_set_caret(le, 5);
  CHECK_EQ_INT(cw_line_edit_caret(le), 3);
  int a, b;
  cw_line_edit_select(le, 4, 2);
  cw_line_edit_selection(le, &a, &b);
  CHECK_EQ_INT(a, 1);
  CHECK_EQ_INT(b, 3);
  CHECK_EQ_INT(cw_line_edit_caret(le), 1);
}

/* Escape cancels a drag; the release that follows still reaches the source
 * marked canceled, so a Button that started the drag does not fire */
static int drag_pressed;
static void count_pressed(cw_node n, const cw_value *args, int nargs, void *u) {
  (void)n; (void)args; (void)nargs; (void)u;
  drag_pressed++;
}
static void test_drag_escape(void) {
  reset();
  cw_node f = frame("Control");
  cw_node btn = mk("Button", "B", f, 0, 0);
  cw_control_set_rect(btn, (cw_rect){ 10, 10, 100, 40 });
  cw_ui_layout(T);
  drag_pressed = 0;
  cw_connect(btn, "pressed", (cw_node){ 0 }, NULL, count_pressed, NULL, 0);
  static cw_raw_input raw;
  memset(&raw, 0, sizeof raw);
  raw.mouse_present = true;
  raw.mouse_x = 30, raw.mouse_y = 20;
  /* the UI reads the platform's snapshot (cw_input_raw): write this test's into it */
  cw_raw_input *plat = (cw_raw_input *)cw_input_raw();
  cw_raw_input saved = *plat;
  #define UI_FRAME() do { *plat = raw; cw_ui_process(T, 1.0 / 60); \
    memset(raw.keys_pressed, 0, sizeof raw.keys_pressed); raw.mouse_pressed = 0; } while (0)
  UI_FRAME();
  /* control: a plain click fires */
  raw.mouse_buttons = raw.mouse_pressed = 1;
  UI_FRAME();
  raw.mouse_buttons = 0;
  UI_FRAME();
  CHECK_EQ_INT(drag_pressed, 1);
  /* press, start a drag, Escape, release over the button */
  raw.mouse_buttons = raw.mouse_pressed = 1;
  UI_FRAME();
  CHECK(cw_ui_drag_begin(&(cw_ui_drag){ .source = btn, .kind = "test", .label = "" }));
  raw.keys[41] = raw.keys_pressed[41] = 1; /* Escape */
  UI_FRAME();
  CHECK(cw_ui_drag_current() == NULL);
  raw.keys[41] = 0;
  raw.mouse_buttons = 0;
  UI_FRAME();
  CHECK_EQ_INT(drag_pressed, 1);
  /* the next plain click fires again (the canceled flag does not linger) */
  raw.mouse_buttons = raw.mouse_pressed = 1;
  UI_FRAME();
  raw.mouse_buttons = 0;
  UI_FRAME();
  CHECK_EQ_INT(drag_pressed, 2);
  *plat = saved;
  #undef UI_FRAME
}

CW_TEST_SUITE("ui", { "anchors", test_anchors }, { "box", test_box }, { "grid_margin_center", test_grid_margin_center },
              { "split_scroll_flow_aspect", test_split_scroll_flow_aspect }, { "tabs_panel", test_tabs_panel },
              { "content_sizes", test_content_sizes }, { "dirty_tracking", test_dirty_tracking },
              { "theme_text", test_theme_text }, { "scene_file", test_scene_file }, { "ranges", test_ranges },
              { "tab_titles", test_tab_titles }, { "line_edit_utf8", test_line_edit_utf8 },
              { "drag_escape", test_drag_escape },
              { "render_layer_restored", test_render_layer_restored })

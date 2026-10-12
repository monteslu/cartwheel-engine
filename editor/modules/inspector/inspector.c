/* Inspector panel (M4.1): reflection-driven property editing.
 *
 * Shows the editor-visible properties of the selection grouped by the type
 * that declares them. With several nodes selected it shows the properties
 * they all have (same name and type); a property whose values differ is
 * marked "mixed" and an edit sets it on every node in one undo step
 * (node.set_many). Every row's editor comes from the inspector plugins
 * (cwed_inspector_register); this module registers the built-in ones at
 * priority 0, so another module takes over a type or a single property
 * with a higher priority.
 *
 * Built-in editors: numbers (SpinBox; a slider beside it for ranged
 * properties), bit masks (32 toggles: *_mask), enums (option list), bools,
 * vectors / quaternions (a spin per component), colors (a swatch opening a
 * colour picker), strings (a line; long and multi-line text in a text box),
 * curves ("t v, ..." strings named *_curve: a small graph with draggable
 * keys), resources (res(...) values and asset-path strings: a path field
 * that checks the file exists), node references (a list of the scene's
 * nodes), and read-only values.
 *
 * Changed values: a row whose value differs from its reference shows its
 * name highlighted and a Revert button. The reference is the instanced
 * scene's value for a node of an instance (an OVERRIDE: what the parent
 * scene file stores), else the type's default. Edits go through
 * cwed_prop_commit (node.set / node.set_many with merge key
 * "inspector:<prop>": a drag is one undo step). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/resource.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"

#define MAX_ROWS 256

typedef struct {
  const cw_prop *prop;
  cw_node editor, name_label, revert;
  const cwed_inspector_plugin *plugin;
  bool mixed;
} row;
static row rows[MAX_ROWS];
static int nrows;
static cw_node props_box, header, sub_header, filter_edit, shown, scroll_box;
static const char *focus_pending;
static uint32_t shown_version, shown_sel_hash;
static bool refreshing, rebuild_needed = true, refresh_needed;
static cw_node color_popup, color_picker;
static const cw_prop *color_prop;

/* ── helpers ─────────────────────────────────────────────────────────── */

static bool quiet(void) { return refreshing; }

static int components(cw_type t) {
  switch (t) {
  case CW_T_VEC2: return 2;
  case CW_T_VEC3: return 3;
  case CW_T_VEC4: case CW_T_QUAT: return 4;
  default: return 1;
  }
}

static const char *num(double v) {
  if (v == 0) v = 0; /* no -0 */
  /* fixed notation when it reads back exactly (rulers, values in command lines) */
  for (int p = 0; p <= 7; p++) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", p, v);
    if ((float)strtod(buf, NULL) == (float)v) return cw_frame_printf("%s", buf);
  }
  for (int p = 1; p <= 9; p++) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*g", p, v);
    if ((float)strtod(buf, NULL) == (float)v) return cw_frame_printf("%s", buf);
  }
  return cw_frame_printf("%.9g", v);
}

/* the value as scene files write it (enums by name) */
static const char *value_text(cw_node n, const cw_prop *p) {
  cw_value v;
  if (!cw_node_get_prop(n, p->name, &v)) return "";
  if (p->type == CW_T_ENUM && p->enum_names) {
    int k = 0;
    while (p->enum_names[k] && k < v.i) k++;
    if (p->enum_names[k] && k == v.i) return p->enum_names[k];
  }
  char buf[2048];
  cw_value_format(&v, buf, sizeof buf);
  return cw_frame_printf("%s", buf);
}

static void commit(const cw_prop *p, const char *text, bool final) {
  if (!quiet()) cwed_prop_commit(p->name, text, final);
}

static bool is_mask(const cw_prop *p) {
  size_t l = strlen(p->name);
  return p->type == CW_T_INT && ((l > 5 && !strcmp(p->name + l - 5, "_mask")) || (p->doc && strstr(p->doc, "(bits")));
}
static bool is_curve(const cw_prop *p) {
  size_t l = strlen(p->name);
  return p->type == CW_T_STRING && l > 6 && !strcmp(p->name + l - 6, "_curve");
}
static bool is_path_string(const cw_prop *p) {
  if (p->type != CW_T_STRING) return false;
  static const char *const names[] = { "texture", "tileset", "font", "mesh", "music", "next_scene", "scene", "theme", "sound", "stream", NULL };
  for (int i = 0; names[i]; i++)
    if (!strcmp(p->name, names[i])) return true;
  return false;
}
static bool is_long_text(const cw_prop *p) {
  if (p->type != CW_T_STRING) return false;
  return !strncmp(p->name, "cells", 5) || !strcmp(p->name, "animations") || !strcmp(p->name, "items") || !strcmp(p->name, "theme_overrides");
}

/* ── numbers ─────────────────────────────────────────────────────────── */

static bool ranged(const cw_prop *p) { return p->min < p->max && p->max - p->min <= 100000; }

static void on_number(cw_node e, const cw_value *a, int argc, void *user) {
  (void)argc;
  if (quiet()) return;
  const cw_prop *p = user;
  cw_node box = cw_node_parent(e);
  /* keep the slider and the spin of a ranged row together */
  refreshing = true;
  for (int i = 0; i < cw_node_child_count(box); i++)
    if (cw_node_child(box, i).id != e.id && cw_range_value(cw_node_child(box, i)) != a[0].f) cw_range_set_value(cw_node_child(box, i), a[0].f);
  refreshing = false;
  if (p->type == CW_T_INT || p->type == CW_T_ENUM) commit(p, cw_frame_printf("%lld", (long long)llround(a[0].f)), false);
  else commit(p, num(a[0].f), false);
}

static cw_node spin(const cw_prop *p, double v, bool integer) {
  cw_node s = cw_node_new("SpinBox", NULL);
  double lo = p->min < p->max ? p->min : -1e7, hi = p->min < p->max ? p->max : 1e7;
  cw_range_set_limits(s, lo, hi, integer ? 1 : 0.001);
  cw_range_set_value(s, v);
  cw_control_set_size_flags(s, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  return s;
}

static bool number_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return (p->type == CW_T_FLOAT || p->type == CW_T_INT || (p->type == CW_T_ENUM && !p->enum_names)) && !is_mask(p);
}
static cw_node number_create(const cwed_prop_ctx *c) {
  bool integer = c->prop->type != CW_T_FLOAT;
  double v = c->value.type == CW_T_FLOAT ? c->value.f : (double)c->value.i;
  cw_node box = cw_node_new("HBoxContainer", NULL);
  cw_node_set(box, "theme_overrides", cw_val_string("separation = 4"));
  if (ranged(c->prop)) {
    cw_node sl = cw_node_new("HSlider", NULL);
    cw_range_set_limits(sl, c->prop->min, c->prop->max, integer ? 1 : 0);
    cw_range_set_value(sl, v);
    cw_control_set_size_flags(sl, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
    cw_control_set_custom_min_size(sl, (cw_vec2){ 50, 0 });
    cw_connect(sl, "value_changed", (cw_node){ 0 }, NULL, on_number, (void *)c->prop, 0);
    cw_node_add_child(box, sl);
  }
  cw_node s = spin(c->prop, v, integer);
  cw_connect(s, "value_changed", (cw_node){ 0 }, NULL, on_number, (void *)c->prop, 0);
  cw_node_add_child(box, s);
  return box;
}
static void number_refresh(cw_node e, const cwed_prop_ctx *c) {
  double v = c->value.type == CW_T_FLOAT ? c->value.f : (double)c->value.i;
  for (int i = 0; i < cw_node_child_count(e); i++)
    if (cw_range_value(cw_node_child(e, i)) != v) cw_range_set_value(cw_node_child(e, i), v);
}

/* ── bit masks ───────────────────────────────────────────────────────── */

static void on_bit(cw_node b, const cw_value *a, int argc, void *user) {
  (void)a, (void)argc;
  if (quiet()) return;
  const cw_prop *p = user;
  cw_node grid = cw_node_parent(b);
  uint32_t m = 0;
  for (int i = 0; i < cw_node_child_count(grid) && i < 32; i++)
    if (cw_button_pressed(cw_node_child(grid, i))) m |= 1u << i;
  commit(p, cw_frame_printf("%d", (int32_t)m), true);
}
static bool mask_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return is_mask(p);
}
static cw_node mask_create(const cwed_prop_ctx *c) {
  cw_node grid = cw_node_new("GridContainer", NULL);
  cw_node_set(grid, "columns", cw_val_int(8));
  cw_node_set(grid, "theme_overrides", cw_val_string("h_separation = 1; v_separation = 1"));
  uint32_t m = (uint32_t)c->value.i;
  for (int i = 0; i < 32; i++) {
    cw_node b = cw_node_new("Button", NULL);
    cw_button_set_text(b, cw_frame_printf("%d", i + 1));
    cw_button_set_toggle_mode(b, true);
    cw_button_set_pressed(b, (m >> i) & 1);
    cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
    cw_node_set(b, "theme_overrides", cw_val_string("font_size = 10; normal = style(bg=#2c313c, radius=2, margin=1 0); "
                                                    "hover = style(bg=#3a4150, radius=2, margin=1 0); "
                                                    "pressed = style(bg=#2f8f83, radius=2, margin=1 0)"));
    cw_control_set_tooltip(b, cw_frame_printf("bit %d (layer %d)", i, i));
    cw_control_set_custom_min_size(b, (cw_vec2){ 18, 16 });
    cw_connect(b, "toggled", (cw_node){ 0 }, NULL, on_bit, (void *)c->prop, 0);
    cw_node_add_child(grid, b);
  }
  return grid;
}
static void mask_refresh(cw_node e, const cwed_prop_ctx *c) {
  uint32_t m = (uint32_t)c->value.i;
  for (int i = 0; i < cw_node_child_count(e) && i < 32; i++)
    if (cw_button_pressed(cw_node_child(e, i)) != (bool)((m >> i) & 1)) cw_button_set_pressed(cw_node_child(e, i), (m >> i) & 1);
}

/* ── vectors ─────────────────────────────────────────────────────────── */

static void on_component(cw_node e, const cw_value *a, int argc, void *user) {
  (void)a, (void)argc;
  if (quiet()) return;
  const cw_prop *p = user;
  cw_node box = cw_node_parent(e);
  int k = components(p->type);
  float v[4];
  for (int i = 0; i < k; i++) v[i] = (float)cw_range_value(cw_node_child(box, i));
  const char *pre = p->type == CW_T_QUAT ? "quat" : k == 2 ? "vec2" : k == 3 ? "vec3" : "vec4";
  const char *t = k == 2 ? cw_frame_printf("%s(%s, %s)", pre, num(v[0]), num(v[1]))
                : k == 3 ? cw_frame_printf("%s(%s, %s, %s)", pre, num(v[0]), num(v[1]), num(v[2]))
                         : cw_frame_printf("%s(%s, %s, %s, %s)", pre, num(v[0]), num(v[1]), num(v[2]), num(v[3]));
  commit(p, t, false);
}
static bool vector_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return components(p->type) > 1;
}
static cw_node vector_create(const cwed_prop_ctx *c) {
  cw_node box = cw_node_new("HBoxContainer", NULL);
  cw_node_set(box, "theme_overrides", cw_val_string("separation = 2"));
  static const char *const axis[4] = { "x", "y", "z", "w" };
  for (int i = 0; i < components(c->prop->type); i++) {
    cw_node s = spin(c->prop, c->value.v[i], false);
    cw_control_set_custom_min_size(s, (cw_vec2){ 40, 0 });
    cw_control_set_tooltip(s, axis[i]);
    cw_connect(s, "value_changed", (cw_node){ 0 }, NULL, on_component, (void *)c->prop, 0);
    cw_node_add_child(box, s);
  }
  return box;
}
static void vector_refresh(cw_node e, const cwed_prop_ctx *c) {
  for (int i = 0; i < components(c->prop->type) && i < cw_node_child_count(e); i++)
    if ((float)cw_range_value(cw_node_child(e, i)) != c->value.v[i]) cw_range_set_value(cw_node_child(e, i), c->value.v[i]);
}

/* ── colors ──────────────────────────────────────────────────────────── */

static const char *color_text(cw_color k) {
  return cw_frame_printf("color(%s, %s, %s, %s)", num(k.r), num(k.g), num(k.b), num(k.a));
}
static void swatch_style(cw_node b, cw_color k) {
  cw_ui_style s = cw_ui_style_flat((cw_color){ k.r, k.g, k.b, 1 }, 4, 6, 3);
  s.border[0] = s.border[1] = s.border[2] = s.border[3] = 1;
  s.border_color = cw_rgba(0.35f, 0.38f, 0.45f, 1);
  cw_control_override_style(b, "normal", &s);
  cw_control_override_style(b, "hover", &s);
  cw_control_override_style(b, "pressed", &s);
  float lum = 0.2126f * k.r + 0.7152f * k.g + 0.0722f * k.b;
  cw_control_override_color(b, "font_color", lum > 0.35f ? cw_rgba(0, 0, 0, 1) : cw_rgba(1, 1, 1, 1));
  cw_control_override_color(b, "font_hover_color", lum > 0.35f ? cw_rgba(0, 0, 0, 1) : cw_rgba(1, 1, 1, 1));
  cw_button_set_text(b, cw_frame_printf("%.2f %.2f %.2f %.2f", k.r, k.g, k.b, k.a));
}
static void on_picker(cw_node p, const cw_value *a, int argc, void *user) {
  (void)p, (void)argc, (void)user;
  if (quiet() || !color_prop) return;
  cw_color k = { a[0].v[0], a[0].v[1], a[0].v[2], a[0].v[3] };
  commit(color_prop, color_text(k), false);
}
static void on_swatch(cw_node b, const cw_value *a, int argc, void *user) {
  (void)a, (void)argc;
  color_prop = user;
  cw_value v;
  if (!cw_node_valid(shown) || !cw_node_get_prop(shown, color_prop->name, &v)) return;
  refreshing = true;
  cw_color_picker_set_color(color_picker, (cw_color){ v.v[0], v.v[1], v.v[2], v.v[3] });
  refreshing = false;
  cw_rect g = cw_control_global_rect(b);
  cwed_history_seal();
  cw_popup_show(color_popup, (cw_rect){ g.x, g.y + g.h + 2, 260, 0 }, false);
}
static bool color_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return p->type == CW_T_COLOR;
}
static cw_node color_create(const cwed_prop_ctx *c) {
  cw_node b = cw_node_new("Button", NULL);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_node_set(b, "theme_overrides", cw_val_string("font_size = 12"));
  swatch_style(b, (cw_color){ c->value.v[0], c->value.v[1], c->value.v[2], c->value.v[3] });
  cw_control_set_tooltip(b, "Pick a colour (linear values)");
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_swatch, (void *)c->prop, 0);
  return b;
}
static void color_refresh(cw_node e, const cwed_prop_ctx *c) {
  swatch_style(e, (cw_color){ c->value.v[0], c->value.v[1], c->value.v[2], c->value.v[3] });
  if (color_prop == c->prop && cw_popup_visible(color_popup)) {
    refreshing = true;
    cw_color_picker_set_color(color_picker, (cw_color){ c->value.v[0], c->value.v[1], c->value.v[2], c->value.v[3] });
    refreshing = false;
  }
}

/* ── bools, enums ────────────────────────────────────────────────────── */

static void on_bool(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  commit(user, a[0].b ? "true" : "false", true);
}
static bool bool_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return p->type == CW_T_BOOL;
}
static cw_node bool_create(const cwed_prop_ctx *c) {
  cw_node b = cw_node_new("CheckBox", NULL);
  cw_button_set_text(b, "On");
  cw_button_set_pressed(b, c->value.b);
  cw_connect(b, "toggled", (cw_node){ 0 }, NULL, on_bool, (void *)c->prop, 0);
  return b;
}
static void bool_refresh(cw_node e, const cwed_prop_ctx *c) {
  if (cw_button_pressed(e) != c->value.b) cw_button_set_pressed(e, c->value.b);
}

static void on_enum(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  const cw_prop *p = user;
  if (p->enum_names) commit(p, p->enum_names[a[0].i], true);
}
static bool enum_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return p->type == CW_T_ENUM && p->enum_names;
}
static cw_node enum_create(const cwed_prop_ctx *c) {
  cw_node o = cw_node_new("OptionButton", NULL);
  for (int i = 0; c->prop->enum_names[i]; i++) cw_option_button_add_item(o, c->prop->enum_names[i]);
  cw_option_button_select(o, (int)c->value.i);
  cw_connect(o, "item_selected", (cw_node){ 0 }, NULL, on_enum, (void *)c->prop, 0);
  return o;
}
static void enum_refresh(cw_node e, const cwed_prop_ctx *c) {
  if (cw_option_button_selected(e) != c->value.i) cw_option_button_select(e, (int)c->value.i);
}

/* ── strings ─────────────────────────────────────────────────────────── */

static void on_text(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  commit(user, a[0].s ? cwed_quote(a[0].s) : "\"\"", true);
}
static bool string_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return p->type == CW_T_STRING;
}
static cw_node string_create(const cwed_prop_ctx *c) {
  cw_node l = cw_node_new("LineEdit", NULL);
  cw_line_edit_set_text(l, c->value.s ? c->value.s : "");
  cw_connect(l, "text_submitted", (cw_node){ 0 }, NULL, on_text, (void *)c->prop, 0);
  return l;
}
static void string_refresh(cw_node e, const cwed_prop_ctx *c) {
  const char *s = c->value.s ? c->value.s : "";
  if (strcmp(cw_line_edit_text(e), s) && !cw_control_has_focus(e)) cw_line_edit_set_text(e, s);
}

/* long / multi-line text: a text box with an Apply button */
static void on_apply_text(cw_node b, const cw_value *a, int argc, void *user) {
  (void)a, (void)argc;
  cw_node te = cw_node_child(cw_node_parent(b), 0);
  commit(user, cwed_quote(cw_text_edit_text(te)), true);
}
static bool longtext_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return is_long_text(p);
}
static cw_node longtext_create(const cwed_prop_ctx *c) {
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node te = cw_node_new("TextEdit", NULL);
  cw_text_edit_set_text(te, c->value.s ? c->value.s : "");
  cw_control_set_custom_min_size(te, (cw_vec2){ 0, 70 });
  cw_node_set(te, "theme_overrides", cw_val_string("font_size = 12"));
  cw_node b = cw_node_new("Button", NULL);
  cw_button_set_text(b, "Apply");
  cw_node_set(b, "theme_overrides", cw_val_string("font_size = 12"));
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_apply_text, (void *)c->prop, 0);
  cw_node_add_child(box, te);
  cw_node_add_child(box, b);
  return box;
}
static void longtext_refresh(cw_node e, const cwed_prop_ctx *c) {
  cw_node te = cw_node_child(e, 0);
  const char *s = c->value.s ? c->value.s : "";
  if (!cw_control_has_focus(te) && strcmp(cw_text_edit_text(te), s)) cw_text_edit_set_text(te, s);
}

/* asset paths (strings) and res(...) resources: the path must exist */
static void path_status(cw_node box, const char *path) {
  cw_node st = cw_node_child(box, 1);
  bool ok = !path[0] || cw_vfs_exists(path);
  cw_label_set_text(st, !path[0] ? "" : ok ? "ok" : "missing");
  cw_node_set(st, "theme_overrides", cw_val_string(ok ? "font_size = 11; font_color = #6fbf73" : "font_size = 11; font_color = #e06c6c"));
}
static void on_path(cw_node e, const cw_value *a, int argc, void *user) {
  (void)argc;
  const cw_prop *p = user;
  const char *s = a[0].s ? a[0].s : "";
  path_status(cw_node_parent(e), s);
  if (p->type == CW_T_RES) commit(p, s[0] ? cw_frame_printf("res(0x%llx)", (unsigned long long)cw_res_path_uid(s)) : "res(0x0)", true);
  else commit(p, cwed_quote(s), true);
}
static const char *res_path(uint64_t uid) {
  if (!uid) return "";
  cw_res r = cw_res_find_uid(uid);
  const char *p = CW_HANDLE_IS_NULL(r) ? NULL : cw_res_path(r);
  return p ? p : cw_frame_printf("res(0x%llx)", (unsigned long long)uid);
}
static bool path_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return p->type == CW_T_RES || is_path_string(p);
}
static cw_node path_create(const cwed_prop_ctx *c) {
  cw_node box = cw_node_new("HBoxContainer", NULL);
  cw_node l = cw_node_new("LineEdit", NULL);
  const char *v = c->prop->type == CW_T_RES ? res_path(c->value.res) : c->value.s ? c->value.s : "";
  cw_line_edit_set_text(l, v);
  cw_node_set(l, "placeholder_text", cw_val_string("project path"));
  cw_control_set_size_flags(l, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(l, "text_submitted", (cw_node){ 0 }, NULL, on_path, (void *)c->prop, 0);
  cw_node st = cw_node_new("Label", NULL);
  cw_control_set_custom_min_size(st, (cw_vec2){ 44, 0 });
  cw_node_add_child(box, l);
  cw_node_add_child(box, st);
  if (c->prop->type != CW_T_RES) path_status(box, v);
  return box;
}
static void path_refresh(cw_node e, const cwed_prop_ctx *c) {
  cw_node l = cw_node_child(e, 0);
  const char *v = c->prop->type == CW_T_RES ? res_path(c->value.res) : c->value.s ? c->value.s : "";
  if (!cw_control_has_focus(l) && strcmp(cw_line_edit_text(l), v)) cw_line_edit_set_text(l, v);
  if (c->prop->type != CW_T_RES) path_status(e, v);
}

/* node references: the scene's nodes in a list */
static void on_noderef(cw_node o, const cw_value *a, int argc, void *user) {
  (void)argc;
  cwed_doc *d = cwed_doc_active();
  if (!d) return;
  if (a[0].i == 0) {
    commit(user, "node(\"\")", true);
    return;
  }
  cw_value items;
  if (!cw_node_get_prop(o, "items", &items) || !items.s) return;
  const char *p = items.s;
  for (int i = 0; i < a[0].i && p; i++) p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL;
  if (!p) return;
  const char *e = strchr(p, '\n');
  commit(user, cw_frame_printf("node(\"%.*s\")", e ? (int)(e - p) : (int)strlen(p), p), true);
}
typedef struct { cw_node o; cwed_doc *d; int count; int sel; cw_node want; } noderef_walk;
static bool noderef_fn(cw_node n, void *u) {
  noderef_walk *w = u;
  if (w->count++ > 400) return false;
  cw_option_button_add_item(w->o, cwed_doc_node_path(w->d, n));
  if (n.id == w->want.id) w->sel = w->count;
  return true;
}
static bool noderef_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return p->type == CW_T_NODE;
}
static cw_node noderef_create(const cwed_prop_ctx *c) {
  cw_node o = cw_node_new("OptionButton", NULL);
  cw_option_button_add_item(o, "(none)");
  cwed_doc *d = cwed_doc_active();
  noderef_walk w = { o, d, 0, 0, c->value.node };
  if (d) cw_node_walk(cwed_doc_root(d), noderef_fn, &w);
  cw_option_button_select(o, w.sel);
  cw_connect(o, "item_selected", (cw_node){ 0 }, NULL, on_noderef, (void *)c->prop, 0);
  return o;
}
static void noderef_refresh(cw_node e, const cwed_prop_ctx *c) {
  (void)e, (void)c; /* the list is rebuilt with the inspector */
}

/* ── curves: "t v, t v, ..." ─────────────────────────────────────────── */

typedef struct { const cw_prop *prop; float t[32], v[32]; int n; int drag; float vmin, vmax; } curve_data;
static const cw_node_type *curve_t;

static void curve_parse(curve_data *c, const char *s) {
  c->n = 0;
  while (s && *s && c->n < 32) {
    float a, b;
    int used = 0;
    if (sscanf(s, " %f %f%n", &a, &b, &used) < 2) break;
    c->t[c->n] = a, c->v[c->n] = b, c->n++;
    s += used;
    while (*s == ',' || *s == ' ') s++;
  }
  c->vmin = 0, c->vmax = 1;
  for (int i = 0; i < c->n; i++) c->vmin = fminf(c->vmin, c->v[i]), c->vmax = fmaxf(c->vmax, c->v[i]);
  if (c->vmax - c->vmin < 1e-3f) c->vmax = c->vmin + 1;
}
static const char *curve_text(const curve_data *c) {
  char *s = cw_frame_alloc(32 * 40), *w = s;
  *w = 0;
  for (int i = 0; i < c->n; i++) w += sprintf(w, "%s%s %s", i ? ", " : "", num(c->t[i]), num(c->v[i]));
  return s;
}
static cw_vec2 curve_pt(cw_rect r, const curve_data *c, int i) {
  return cw_v2(r.x + c->t[i] * r.w, r.y + r.h - (c->v[i] - c->vmin) / (c->vmax - c->vmin) * r.h);
}
static void curve_draw(cw_node n, void *data) {
  curve_data *c = data;
  cw_rect full = cw_control_rect(n);
  cw_rect r = { 6, 6, full.w - 12, full.h - 12 };
  cw_ui_draw_round_rect((cw_rect){ 0, 0, full.w, full.h }, 4, cw_rgba(0.12f, 0.13f, 0.16f, 1));
  for (int g = 0; g <= 4; g++) {
    float x = r.x + r.w * (float)g / 4;
    cw_ui_draw_line(cw_v2(x, r.y), cw_v2(x, r.y + r.h), 1, cw_rgba(1, 1, 1, 0.06f));
  }
  if (c->n == 0) {
    cw_ui_draw_line(cw_v2(r.x, r.y + r.h * 0), cw_v2(r.x + r.w, r.y), 1.5f, cw_rgba(0.5f, 0.8f, 1, 0.6f));
    return;
  }
  for (int i = 0; i + 1 < c->n; i++) cw_ui_draw_line(curve_pt(r, c, i), curve_pt(r, c, i + 1), 2, cw_rgba(0.45f, 0.78f, 1, 1));
  for (int i = 0; i < c->n; i++) cw_ui_draw_circle(curve_pt(r, c, i), i == c->drag ? 5 : 4, i == c->drag ? cw_rgba(1, 0.85f, 0.3f, 1) : CW_WHITE);
  char lab[64];
  snprintf(lab, sizeof lab, "%s .. %s", num(c->vmin), num(c->vmax));
  cw_ui_draw_text(cw_ui_font_sans(), 10, lab, cw_v2(r.x + 2, r.y), cw_rgba(1, 1, 1, 0.45f));
}
static bool curve_input(cw_node n, void *data, const cw_ui_event *e) {
  curve_data *c = data;
  cw_rect full = cw_control_rect(n);
  cw_rect r = { 6, 6, full.w - 12, full.h - 12 };
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 0) {
    c->drag = -1;
    for (int i = 0; i < c->n; i++)
      if (cw_v2_length(cw_v2_sub(curve_pt(r, c, i), e->pos)) < 8) c->drag = i;
    if (c->drag < 0 && e->clicks == 2 && c->n < 32) { /* double click adds a key */
      float t = fminf(fmaxf((e->pos.x - r.x) / r.w, 0), 1), v = c->vmin + (1 - (e->pos.y - r.y) / r.h) * (c->vmax - c->vmin);
      int at = 0;
      while (at < c->n && c->t[at] < t) at++;
      memmove(c->t + at + 1, c->t + at, sizeof(float) * (size_t)(c->n - at));
      memmove(c->v + at + 1, c->v + at, sizeof(float) * (size_t)(c->n - at));
      c->t[at] = roundf(t * 100) / 100, c->v[at] = roundf(v * 100) / 100, c->n++;
      cwed_history_seal();
      commit(c->prop, cwed_quote(curve_text(c)), true);
    }
    if (c->drag >= 0) cwed_history_seal();
    return true;
  }
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 1) { /* right click removes a key */
    for (int i = 0; i < c->n; i++)
      if (cw_v2_length(cw_v2_sub(curve_pt(r, c, i), e->pos)) < 8 && c->n > 1) {
        memmove(c->t + i, c->t + i + 1, sizeof(float) * (size_t)(c->n - i - 1));
        memmove(c->v + i, c->v + i + 1, sizeof(float) * (size_t)(c->n - i - 1));
        c->n--;
        commit(c->prop, cwed_quote(curve_text(c)), true);
        return true;
      }
    return true;
  }
  if (e->type == CW_UI_MOUSE_MOVE && (e->buttons & 1) && c->drag >= 0) {
    int i = c->drag;
    float t = fminf(fmaxf((e->pos.x - r.x) / r.w, 0), 1);
    if (i > 0) t = fmaxf(t, c->t[i - 1]);
    if (i + 1 < c->n) t = fminf(t, c->t[i + 1]);
    c->t[i] = roundf(t * 100) / 100;
    c->v[i] = roundf((c->vmin + (1 - (e->pos.y - r.y) / r.h) * (c->vmax - c->vmin)) * 100) / 100;
    commit(c->prop, cwed_quote(curve_text(c)), false);
    return true;
  }
  if (e->type == CW_UI_MOUSE_UP) {
    c->drag = -1;
    return true;
  }
  return false;
}
static bool curve_handles(cw_node n, const cw_prop *p) {
  (void)n;
  return is_curve(p);
}
static cw_node curve_create(const cwed_prop_ctx *c) {
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node g = cw_node_new("InspectorCurve", NULL);
  curve_data *cd = cw_node_data_t(g, curve_t);
  cd->prop = c->prop;
  cd->drag = -1;
  curve_parse(cd, c->value.s);
  cw_control_set_custom_min_size(g, (cw_vec2){ 0, 64 });
  cw_control_set_tooltip(g, "Drag keys; double-click adds one, right-click removes one");
  cw_node l = cw_node_new("LineEdit", NULL);
  cw_line_edit_set_text(l, c->value.s ? c->value.s : "");
  cw_node_set(l, "theme_overrides", cw_val_string("font_size = 12"));
  cw_connect(l, "text_submitted", (cw_node){ 0 }, NULL, on_text, (void *)c->prop, 0);
  cw_node_add_child(box, g);
  cw_node_add_child(box, l);
  return box;
}
static void curve_refresh(cw_node e, const cwed_prop_ctx *c) {
  curve_data *cd = cw_node_data_t(cw_node_child(e, 0), curve_t);
  if (cd && cd->drag < 0) curve_parse(cd, c->value.s);
  cw_node l = cw_node_child(e, 1);
  if (!cw_control_has_focus(l) && strcmp(cw_line_edit_text(l), c->value.s ? c->value.s : "")) cw_line_edit_set_text(l, c->value.s ? c->value.s : "");
}

/* ── read only ───────────────────────────────────────────────────────── */

static bool any_handles(cw_node n, const cw_prop *p) {
  (void)n, (void)p;
  return true;
}
static cw_node any_create(const cwed_prop_ctx *c) {
  char buf[256];
  cw_value_format(&c->value, buf, sizeof buf);
  cw_node l = cw_node_new("Label", NULL);
  cw_label_set_text(l, buf);
  cw_node_set(l, "ellipsis", cw_val_bool(true));
  return l;
}
static void any_refresh(cw_node e, const cwed_prop_ctx *c) {
  char buf[256];
  cw_value_format(&c->value, buf, sizeof buf);
  cw_label_set_text(e, buf);
}

static const cwed_inspector_plugin BUILTIN[] = {
  { "number", 0, number_handles, number_create, number_refresh },
  { "mask", 0, mask_handles, mask_create, mask_refresh },
  { "vector", 0, vector_handles, vector_create, vector_refresh },
  { "color", 0, color_handles, color_create, color_refresh },
  { "bool", 0, bool_handles, bool_create, bool_refresh },
  { "enum", 0, enum_handles, enum_create, enum_refresh },
  { "string", 0, string_handles, string_create, string_refresh },
  { "long_text", 1, longtext_handles, longtext_create, longtext_refresh },
  { "path", 1, path_handles, path_create, path_refresh },
  { "node", 0, noderef_handles, noderef_create, noderef_refresh },
  { "curve", 1, curve_handles, curve_create, curve_refresh },
  { "readonly", -100, any_handles, any_create, any_refresh },
};
#define READONLY_PLUGIN (&BUILTIN[11])

/* ── references (overrides) ──────────────────────────────────────────── */

/* fresh copies to compare against, kept for one inspector build */
#define MAX_REFS 32
static struct { const char *key; cw_node node; } refs[MAX_REFS];
static int nrefs;
static void refs_clear(void) {
  for (int i = 0; i < nrefs; i++)
    if (cw_node_valid(refs[i].node)) cw_node_free(refs[i].node);
  nrefs = 0;
}
static cw_node ref_get(const char *key, bool scene) {
  const char *k = cw_intern(key);
  for (int i = 0; i < nrefs; i++)
    if (refs[i].key == k) return refs[i].node;
  if (nrefs >= MAX_REFS) return (cw_node){ 0 };
  cw_node n = scene ? cw_scene_instance_path(key) : cw_node_new(key, NULL);
  refs[nrefs].key = k, refs[nrefs].node = n;
  nrefs++;
  return n;
}

/* the node n is compared with: the instanced scene's node for nodes of an
 * instance, else a fresh node of its type; *override says which */
static cw_node reference_of(cwed_doc *d, cw_node n, bool *override) {
  cw_node root = cwed_doc_root(d);
  *override = false;
  for (cw_node r = n; cw_node_valid(r) && r.id != root.id; r = cw_node_parent(r)) {
    const char *src = cw_scene_instance_source(r);
    if (!src) continue;
    cw_node fresh = ref_get(src, true);
    if (CW_HANDLE_IS_NULL(fresh)) break;
    char rel[1024];
    cw_node_path(n, r, rel, sizeof rel);
    cw_node m = rel[0] ? cw_node_get(fresh, rel) : fresh;
    if (CW_HANDLE_IS_NULL(m)) break;
    *override = true;
    return m;
  }
  return ref_get(cw_node_get_type(n)->name, false);
}

/* ── the panel ────────────────────────────────────────────────────────── */

static void on_revert(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  row *r = user;
  cwed_doc *d = cwed_doc_active();
  if (!d || !cw_node_valid(shown)) return;
  bool ov;
  cw_node ref = reference_of(d, shown, &ov);
  if (CW_HANDLE_IS_NULL(ref)) return;
  const char *t = value_text(ref, r->prop);
  if (r->prop->type == CW_T_STRING) t = cwed_quote(t);
  cwed_history_seal();
  cwed_prop_commit(r->prop->name, t, true);
}

static uint32_t sel_hash(void) {
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < cwed_selection_count(); i++) {
    uint64_t id = cwed_selection_get(i).id;
    h = cw_hash32(&id, sizeof id, h);
  }
  return h;
}

/* the property exists, with the same type, on every selected node */
static bool shared_prop(const cw_prop *p, bool *mixed) {
  *mixed = false;
  const char *first = NULL;
  for (int i = 0; i < cwed_selection_count(); i++) {
    cw_node n = cwed_selection_get(i);
    const cw_prop *q = cw_node_find_prop(n, p->name, NULL);
    if (!q || q->type != p->type || (q->flags & CW_PROP_READONLY) != (p->flags & CW_PROP_READONLY)) return false;
    const char *t = value_text(n, q);
    if (!first) first = cw_intern(t);
    else if (strcmp(first, t)) *mixed = true;
  }
  return true;
}

static void update_row_marks(row *r, cwed_doc *d) {
  if (!cw_node_valid(r->name_label)) return;
  bool ov;
  cw_node ref = reference_of(d, shown, &ov);
  bool changed = cw_node_valid(ref) && cw_node_find_prop(ref, r->prop->name, NULL) && strcmp(value_text(shown, r->prop), value_text(ref, r->prop));
  bool stored = (r->prop->flags & CW_PROP_STORED) && !(r->prop->flags & CW_PROP_READONLY);
  const char *col = r->mixed ? "#e5c07b" : changed && stored ? (ov ? "#7fd6ff" : "#e8eaf0") : "#9aa2b1";
  cw_node_set(r->name_label, "theme_overrides", cw_val_string(cw_frame_printf("font_color = %s", col)));
  cw_control_set_tooltip(r->name_label, cw_frame_printf("%s%s%s", r->prop->doc ? r->prop->doc : "",
                                                        r->mixed ? "\nMixed: the selected nodes differ" : "",
                                                        changed && stored ? (ov ? "\nOverridden here (differs from the instanced scene)" : "\nChanged from the default") : ""));
  if (cw_node_valid(r->revert)) { /* hidden by look, not visibility: a grid skips hidden cells */
    bool on = changed && stored && cwed_selection_count() == 1;
    cw_button_set_disabled(r->revert, !on);
    cw_control_set_modulate(r->revert, cw_rgba(1, 1, 1, on ? 1 : 0));
  }
}

static void rebuild(void) {
  rebuild_needed = refresh_needed = false;
  refreshing = true;
  cwed_clear_children(props_box);
  refs_clear();
  nrows = 0;
  cwed_doc *d = cwed_doc_active();
  shown = cwed_selection_count() ? cwed_selection_get(0) : (cw_node){ 0 };
  shown_sel_hash = sel_hash();
  if (CW_HANDLE_IS_NULL(shown) || !d) {
    cw_label_set_text(header, d ? "Select a node to edit its properties" : "No scene open");
    cw_label_set_text(sub_header, "");
    refreshing = false;
    return;
  }
  int k = cwed_selection_count();
  cw_label_set_text(header, k > 1 ? cw_frame_printf("%d nodes  (%s, ...)", k, cw_node_get_type(shown)->name)
                                  : cw_frame_printf("%s  (%s)", cw_node_name(shown), cw_node_get_type(shown)->name));
  bool ov;
  (void)reference_of(d, shown, &ov);
  const char *src = cw_scene_instance_source(shown);
  cw_label_set_text(sub_header, k > 1 ? "Edits apply to every selected node (one undo step)"
                                : src && shown.id != cwed_doc_root(d).id ? cw_frame_printf("instance of %s: changed values are overrides", src)
                                : ov ? "inherited from an instanced scene: changed values are overrides"
                                     : cwed_doc_node_path(d, shown));
  const char *filter = cw_line_edit_text(filter_edit);
  const cw_prop *props[256];
  const cw_node_type *owners[256];
  int np = cw_node_props(shown, props, owners, 256);
  if (np > 256) np = 256;
  const cw_node_type *group = NULL;
  cw_node grid = { 0 };
  for (int i = 0; i < np && nrows < MAX_ROWS; i++) {
    const cw_prop *p = props[i];
    if (!(p->flags & CW_PROP_EDITOR)) continue;
    if (filter[0] && !strstr(p->name, filter)) continue;
    bool mixed = false;
    if (k > 1 && !shared_prop(p, &mixed)) continue;
    cwed_prop_ctx ctx = { shown, p, { 0 } };
    if (!cw_node_get_prop(shown, p->name, &ctx.value)) continue;
    const cwed_inspector_plugin *pl = (p->flags & CW_PROP_READONLY) ? READONLY_PLUGIN : cwed_inspector_for(shown, p);
    if (!pl) continue;
    if (owners[i] != group) {
      group = owners[i];
      cw_node head = cw_node_new("PanelContainer", NULL);
      cw_ui_style hs = cw_control_theme_style(props_box, "Inspector", "header");
      cw_control_override_style(head, "panel", &hs);
      cw_node hl = cw_node_new("Label", NULL);
      cw_label_set_text(hl, group->name);
      cw_node_add_child(head, hl);
      cw_node_add_child(props_box, head);
      grid = cw_node_new("GridContainer", NULL);
      cw_node_set(grid, "columns", cw_val_int(3));
      cw_node_add_child(props_box, grid);
    }
    cw_node l = cw_node_new("Label", NULL);
    cw_label_set_text(l, mixed ? cw_frame_printf("%s *", p->name) : p->name);
    cw_node_set(l, "mouse_filter", cw_val_int(CW_MOUSE_STOP));
    cw_control_set_custom_min_size(l, (cw_vec2){ 92, 0 });
    cw_node_set(l, "ellipsis", cw_val_bool(true));
    cw_node e = pl->create(&ctx);
    cw_control_set_size_flags(e, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
    if (p->flags & CW_PROP_READONLY) cw_control_set_modulate(e, cw_rgba(1, 1, 1, 0.6f));
    if (mixed) cw_control_set_modulate(e, cw_rgba(1, 0.92f, 0.75f, 1));
    cw_node rv = cw_node_new("Button", NULL);
    cw_button_set_text(rv, "x");
    cw_node_set(rv, "focus_mode", cw_val_int(CW_FOCUS_NONE));
    cw_node_set(rv, "flat", cw_val_bool(true));
    cw_node_set(rv, "theme_overrides", cw_val_string("font_size = 11; font_color = #9aa2b1"));
    cw_control_set_custom_min_size(rv, (cw_vec2){ 16, 0 });
    cw_node_add_child(grid, l);
    cw_node_add_child(grid, e);
    cw_node_add_child(grid, rv);
    rows[nrows] = (row){ p, e, l, rv, pl, mixed };
    cw_control_set_tooltip(rv, "Revert (to the instanced scene's value, or the default)");
    cw_connect(rv, "pressed", (cw_node){ 0 }, NULL, on_revert, &rows[nrows], 0);
    update_row_marks(&rows[nrows], d);
    nrows++;
  }
  shown_version = cwed_doc_version(d);
  refreshing = false;
}

static void refresh_values(void) {
  refresh_needed = false;
  if (!cw_node_valid(shown)) {
    rebuild();
    return;
  }
  cwed_doc *d = cwed_doc_active();
  refreshing = true;
  for (int i = 0; i < nrows; i++) {
    cwed_prop_ctx ctx = { shown, rows[i].prop, { 0 } };
    bool mixed = false;
    if (cwed_selection_count() > 1) shared_prop(rows[i].prop, &mixed);
    if (mixed != rows[i].mixed) {
      refreshing = false;
      rebuild();
      return;
    }
    if (cw_node_get_prop(shown, rows[i].prop->name, &ctx.value) && rows[i].plugin->refresh) rows[i].plugin->refresh(rows[i].editor, &ctx);
    if (d) update_row_marks(&rows[i], d);
  }
  if (cwed_selection_count() == 1) cw_label_set_text(header, cw_frame_printf("%s  (%s)", cw_node_name(shown), cw_node_get_type(shown)->name));
  refreshing = false;
}

static void on_event(const cwed_event *e, void *user) {
  (void)user;
  if (e->type == CWED_EV_PLUGINS) { /* M5.2: a plugin (un)loads: drop the reference nodes now (they may be of its types) */
    refs_clear();
    rebuild_needed = true;
    return;
  }
  if (e->type == CWED_EV_DOC_CHANGED) refresh_needed = true;
  else rebuild_needed = true;
}

static void frame(double dt) {
  (void)dt;
  if (!cw_node_valid(props_box)) return;
  if (rebuild_needed || sel_hash() != shown_sel_hash || (cwed_selection_count() && !cw_node_valid(shown))) rebuild();
  else if (refresh_needed) refresh_values();
  if (focus_pending) { /* after a layout pass, so the row has its place */
    for (int i = 0; i < nrows; i++)
      if (!strcmp(rows[i].prop->name, focus_pending)) {
        cw_scroll_ensure_visible(scroll_box, rows[i].editor);
        cw_scroll_set(scroll_box, (cw_vec2){ 0, cw_scroll_get(scroll_box).y }); /* keep the names in view */
      }
    focus_pending = NULL;
  }
}

static void on_filter(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)a, (void)argc, (void)user;
  rebuild_needed = true;
}

static void register_curve_type(void) {
  static cw_node_type t = { .name = "InspectorCurve", .parent = "Control", .data_size = sizeof(curve_data),
                            .doc = "The inspector's curve editor (editor module inspector)." };
  if (!cw_node_type_find("InspectorCurve")) {
    cw_node_type_register(&t);
    cw_ui_class_register("InspectorCurve", &(cw_ui_class){ .draw = curve_draw, .input = curve_input });
  }
  curve_t = cw_node_type_find("InspectorCurve");
}

static cw_node create(void *user) {
  (void)user;
  register_curve_type();
  cw_node box = cw_node_new("VBoxContainer", NULL);
  header = cw_node_new("Label", "Header");
  cw_node_set(header, "ellipsis", cw_val_bool(true));
  sub_header = cw_node_new("Label", "SubHeader");
  cw_node_set(sub_header, "ellipsis", cw_val_bool(true));
  cw_node_set(sub_header, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  filter_edit = cw_node_new("LineEdit", "Filter");
  cw_node_set(filter_edit, "placeholder_text", cw_val_string("filter properties"));
  cw_connect(filter_edit, "text_changed", (cw_node){ 0 }, NULL, on_filter, NULL, 0);
  cw_node scroll = scroll_box = cw_node_new("ScrollContainer", "Scroll");
  cw_node_set(scroll, "horizontal_scroll_mode", cw_val_int(1));
  cw_control_set_size_flags(scroll, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  props_box = cw_node_new("VBoxContainer", "Props");
  cw_control_set_size_flags(props_box, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(scroll, props_box);
  cw_node_add_child(box, header);
  cw_node_add_child(box, sub_header);
  cw_node_add_child(box, filter_edit);
  cw_node_add_child(box, scroll);
  /* the colour popup (one, shared by the colour rows) */
  color_popup = cw_node_new("PopupPanel", "ColorPopup");
  color_picker = cw_node_new("ColorPicker", "Picker");
  cw_connect(color_picker, "color_changed", (cw_node){ 0 }, NULL, on_picker, NULL, 0);
  cw_node_add_child(color_popup, color_picker);
  cw_node_add_child(box, color_popup);
  rebuild();
  return box;
}

static void shutdown(void) { refs_clear(); }

/* inspector.focus: scroll a property's row into view (agents, scripts) */
static bool cmd_focus(cwed_call *c) {
  const char *p = cwed_arg(c, "prop");
  if (rebuild_needed || sel_hash() != shown_sel_hash) rebuild();
  for (int i = 0; i < nrows; i++)
    if (!strcmp(rows[i].prop->name, p)) {
      focus_pending = cw_intern(p);
      cwed_json_key(cwed_reply(c), "row");
      cwed_json_printf(cwed_reply(c), "%d", i);
      return true;
    }
  return cwed_fail(c, "the inspector shows no property '%s' (select a node that has it; node.props lists them)", p);
}

static void init(void) {
  for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++) cwed_inspector_register(&BUILTIN[i]);
  static const cwed_param focus_p[] = { { "prop", CWED_ARG_STRING, NULL, "property name" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "inspector.focus", .title = "Inspector: Show Property", .params = focus_p,
    .flags = CWED_CMD_HIDDEN, .doc = "Scroll the inspector to a property of the selection.", .exec = cmd_focus });
  cwed_panel_register(&(cwed_panel_desc){ .id = "inspector", .title = "Inspector", .slot = "right_top", .order = 0, .create = create });
  cwed_subscribe(CWED_EV_DOC_CHANGED | CWED_EV_DOC_ACTIVE | CWED_EV_DOC_CLOSED | CWED_EV_SELECTION | CWED_EV_PLUGINS, on_event, NULL);
}

CWED_MODULE(inspector) = { .title = "Inspector", .milestone = "M4.1", .init = init, .frame = frame, .shutdown = shutdown };

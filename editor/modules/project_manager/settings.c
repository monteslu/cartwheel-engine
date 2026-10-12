/* Project settings (M4.6): project.set / project.get / project.settings and
 * the Project Settings panel (input map, physics layers, render settings).
 *
 * Every change is the undoable editor-wide command project.set, which
 * rewrites one key of project.cwproj through the project layer (natively the
 * project directory, in a cart the in-memory layer) and leaves every other
 * line as it was (cartwheel/project.h cw_project_set). The game reads the
 * same keys at run time (cw_project_load_input, cw_project_apply_app,
 * physics_nodes.h). */
/* paths and messages are cut at their buffer on purpose (each limit is far above real use) */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/project.h"
#include "cartwheel/vfs.h"
#include "pm.h"

static bool dirty = true;

/* ── validation ─────────────────────────────────────────────────────── */

static bool ident(const char *s) {
  if (!*s) return false;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || (*s >= 'A' && *s <= 'Z'))) return false;
  return true;
}

static bool int_in(const char *v, long lo, long hi) {
  char *e;
  long n = strtol(v, &e, 10);
  return *v && !*e && n >= lo && n <= hi;
}

/* NULL when the value suits the key; else why not (frame-arena text) */
static const char *check(const char *key, const char *value) {
  if (!*key || strchr(key, '=') || strchr(key, ' ') || strchr(key, '\n')) return "key: no spaces, '=' or newlines";
  if (strchr(value, '\n')) return "value: one line";
  if (!strncmp(key, "input.", 6)) {
    if (!ident(key + 6)) return "input.<action>: the action name is letters, digits and '_'";
    /* each binding: kind:... (the cw_input_load_bindings line format) */
    const char *s = value;
    while (*s) {
      while (*s == ' ' || *s == ',') s++;
      if (!*s) break;
      const char *e = s;
      while (*e && *e != ',') e++;
      const char *colon = memchr(s, ':', (size_t)(e - s));
      static const char *const kinds[] = { "key", "pad", "axis", "mouse", "touch", "deadzone", "wheel", NULL };
      bool ok = false;
      for (int i = 0; colon && kinds[i]; i++)
        if ((size_t)(colon - s) == strlen(kinds[i]) && !strncmp(s, kinds[i], strlen(kinds[i]))) ok = true;
      if (!ok)
        return cw_frame_printf("'%.*s' is not a binding: use key:space, pad:south, axis:lx-, mouse:left, touch:<id>, deadzone:0.3 "
                               "(cartwheel/input.h)", (int)(e - s), s);
      s = e;
    }
    return NULL;
  }
  if (!strcmp(key, "render.width") || !strcmp(key, "render.height"))
    return int_in(value, 16, 8192) ? NULL : "a size in pixels, 16..8192";
  if (!strcmp(key, "render.msaa"))
    return !strcmp(value, "0") || !strcmp(value, "2") || !strcmp(value, "4") || !strcmp(value, "8") ? NULL : "0, 2, 4 or 8";
  if (!strcmp(key, "render.vsync")) return !strcmp(value, "true") || !strcmp(value, "false") ? NULL : "true or false";
  if (!strcmp(key, "render.fixed_hz")) return int_in(value, 10, 1000) ? NULL : "ticks per second, 10..1000";
  if (!strcmp(key, "substeps")) return int_in(value, 1, 64) ? NULL : "1..64";
  if (!strcmp(key, "gravity_2d") || !strcmp(key, "gravity_3d")) {
    cw_value v;
    bool two = key[8] == '2';
    if (!cw_value_parse(value, CW_T_NONE, &v) || v.type != (two ? CW_T_VEC2 : CW_T_VEC3))
      return two ? "vec2(x, y) in px/s^2, y down" : "vec3(x, y, z) in m/s^2";
    return NULL;
  }
  if (!strcmp(key, "app")) return ident(value) && !(value[0] >= '0' && value[0] <= '9') ? NULL : "a C identifier (the executable and cart name)";
  if (!strcmp(key, "name") && (!*value || strchr(value, '"') || strchr(value, '|'))) return "a name without quotes or '|'";
  return NULL;
}

/* layer name lists are one quoted string (physics_nodes.h) */
static const char *stored_form(const char *key, const char *value) {
  if ((!strcmp(key, "layer_names_2d") || !strcmp(key, "layer_names_3d")) && value[0] != '"')
    return cw_frame_printf("\"%s\"", value);
  return value;
}

/* ── project.set (undoable, editor-wide history) ───────────────────── */

/* undo restores the whole file as it was (exact bytes); new_value NULL = remove */
typedef struct { char *key, *new_value, *old_text; size_t old_len; } set_state;

static void state_free(cwed_call *c, void *p) {
  (void)c;
  set_state *s = p;
  if (!s) return;
  cw_free(s->key);
  cw_free(s->new_value);
  free(s->old_text);
  cw_free(s);
}

static void after_change(const char *key) {
  if (!strcmp(key, "name") || !strcmp(key, "main_scene") || !strcmp(key, "scenes")) cwed_project_reload();
  dirty = true;
}

static bool cmd_set(cwed_call *c) {
  set_state *s = cwed_call_state(c);
  if (!s) {
    const char *key = cwed_arg(c, "key"), *value = cwed_arg(c, "value");
    bool remove = cwed_arg_bool(c, "remove");
    if (!remove && !cwed_arg_given(c, "value")) return cwed_fail(c, "value: the new value (or remove=true to delete the key)");
    const char *why = check(key, remove ? "" : value);
    if (why && !(remove && strncmp(key, "input.", 6))) return cwed_fail(c, "%s = %s: %s", key, value, why);
    if (remove && !cw_project_get(key)) return cwed_fail(c, "%s is not set in project.cwproj (project.settings lists the keys)", key);
    s = cw_alloc(sizeof *s, CW_MEM_GENERAL);
    memset(s, 0, sizeof *s);
    s->key = cw_strdup(key, CW_MEM_GENERAL);
    s->new_value = remove ? NULL : cw_strdup(stored_form(key, value), CW_MEM_GENERAL);
    s->old_text = cw_vfs_read("project.cwproj", &s->old_len);
    cwed_call_set_state(c, s);
  }
  if (!cw_project_set(s->key, s->new_value)) return cwed_fail(c, "cannot write project.cwproj");
  after_change(s->key);
  cwed_json_key(cwed_reply(c), "key");
  cwed_json_str(cwed_reply(c), s->key);
  cwed_json_key(cwed_reply(c), "value");
  if (s->new_value) cwed_json_str(cwed_reply(c), s->new_value);
  else cwed_json_raw(cwed_reply(c), "null");
  return true;
}

static void undo_set(cwed_call *c) {
  set_state *s = cwed_call_state(c);
  if (s->old_text) cw_vfs_write("project.cwproj", s->old_text, s->old_len);
  else cw_vfs_remove("project.cwproj");
  cw_project_reload();
  after_change(s->key);
}

static bool merge_set(cwed_call *prev, cwed_call *next) {
  set_state *p = cwed_call_state(prev), *n = cwed_call_state(next);
  if (!p || !n || strcmp(p->key, n->key)) return false;
  cw_free(p->new_value);
  p->new_value = n->new_value ? cw_strdup(n->new_value, CW_MEM_GENERAL) : NULL;
  return true;
}

static bool cmd_get(cwed_call *c) {
  const char *key = cwed_arg(c, "key"), *v = cw_project_get(key);
  cwed_json_key(cwed_reply(c), "key");
  cwed_json_str(cwed_reply(c), key);
  cwed_json_key(cwed_reply(c), "value");
  if (v) cwed_json_str(cwed_reply(c), v);
  else cwed_json_raw(cwed_reply(c), "null");
  return true;
}

static bool cmd_settings(cwed_call *c) {
  const char *prefix = cwed_arg(c, "prefix");
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "settings");
  cwed_json_raw(j, "{");
  bool first = true;
  for (int i = 0; i < cw_project_count(); i++) {
    const char *k = cw_project_key(i);
    if (strncmp(k, prefix, strlen(prefix))) continue;
    /* the last line of a key wins: skip earlier duplicates */
    bool later = false;
    for (int k2 = i + 1; k2 < cw_project_count(); k2++)
      if (!strcmp(cw_project_key(k2), k)) later = true;
    if (later) continue;
    if (!first) cwed_json_raw(j, ",");
    first = false;
    cwed_json_str(j, k);
    cwed_json_raw(j, ":");
    cwed_json_str(j, cw_project_get(k));
  }
  cwed_json_raw(j, "}");
  return true;
}

/* ── the Project Settings panel ────────────────────────────────────── */

static cw_node body, status;

static void report(bool ok, char *reply) {
  if (!ok && reply) {
    const char *e = strstr(reply, "\"error\":\"");
    cw_label_set_text(status, e ? cw_frame_printf("%.*s", (int)strcspn(e + 9, "\""), e + 9) : reply);
  } else cw_label_set_text(status, "");
  cw_free(reply);
}

static void set_key(const char *key, const char *value) {
  char *reply = NULL;
  bool ok = cwed_run(cw_frame_printf("project.set key=%s value=%s", cwed_quote(key), cwed_quote(value)), &reply);
  report(ok, reply);
}

static void on_text(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  set_key((const char *)user, a[0].s ? a[0].s : "");
}

static void on_check(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  set_key((const char *)user, a[0].b ? "true" : "false");
}

static void on_msaa(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc, (void)user;
  static const char *const m[] = { "0", "2", "4", "8" };
  if (a[0].i >= 0 && a[0].i < 4) set_key("render.msaa", m[a[0].i]);
}

static void on_remove_action(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)a, (void)argc;
  char *reply = NULL;
  bool ok = cwed_run(cw_frame_printf("project.set key=%s remove=true", cwed_quote((const char *)user)), &reply);
  report(ok, reply);
}

static cw_node new_action, new_bindings;
static void on_add_action(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)a, (void)argc, (void)user;
  set_key(cw_frame_printf("input.%s", cw_line_edit_text(new_action)), cw_line_edit_text(new_bindings));
}

static cw_node section(const char *title) {
  cw_node head = cw_node_new("PanelContainer", NULL);
  cw_ui_style hs = cw_control_theme_style(body, "Inspector", "header");
  cw_control_override_style(head, "panel", &hs);
  cw_node l = cw_node_new("Label", NULL);
  cw_label_set_text(l, title);
  cw_node_add_child(head, l);
  cw_node_add_child(body, head);
  cw_node grid = cw_node_new("GridContainer", NULL);
  cw_node_set(grid, "columns", cw_val_int(2));
  cw_control_set_size_flags(grid, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(body, grid);
  return grid;
}

static cw_node label(const char *text) {
  cw_node l = cw_node_new("Label", NULL);
  cw_label_set_text(l, text);
  cw_control_set_custom_min_size(l, (cw_vec2){ 110, 0 });
  cw_node_set(l, "ellipsis", cw_val_bool(true));
  return l;
}

/* a key edited as text (submitted with Enter) */
static void text_row(cw_node grid, const char *title, const char *key, const char *node_name) {
  cw_node_add_child(grid, label(title));
  cw_node e = cw_node_new("LineEdit", node_name);
  cw_line_edit_set_text(e, pm_get(key));
  cw_control_set_size_flags(e, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(e, "text_submitted", (cw_node){ 0 }, NULL, on_text, (void *)cw_intern(key), 0);
  cw_node_add_child(grid, e);
}

static void rebuild(void) {
  dirty = false;
  if (!cw_node_valid(body)) return;
  cwed_clear_children(body);
  cw_node g = section("General");
  text_row(g, "Name", "name", "PsName");
  text_row(g, "Main scene", "main_scene", "PsMainScene");
  text_row(g, "App", "app", "PsApp");

  g = section("Input Map");
  for (int i = 0; i < cw_project_count(); i++) {
    const char *k = cw_project_key(i);
    if (strncmp(k, "input.", 6)) continue;
    cw_node_add_child(g, label(k + 6));
    cw_node row = cw_node_new("HBoxContainer", NULL);
    cw_control_set_size_flags(row, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
    cw_node e = cw_node_new("LineEdit", cw_frame_printf("PsInput_%s", k + 6));
    cw_line_edit_set_text(e, cw_project_get(k));
    cw_control_set_size_flags(e, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
    cw_connect(e, "text_submitted", (cw_node){ 0 }, NULL, on_text, (void *)cw_intern(k), 0);
    cw_node_add_child(row, e);
    cw_node x = cw_node_new("Button", cw_frame_printf("PsInputRemove_%s", k + 6));
    cw_button_set_text(x, "x");
    cw_control_set_tooltip(x, "Remove this action");
    cw_connect(x, "pressed", (cw_node){ 0 }, NULL, on_remove_action, (void *)cw_intern(k), 0);
    cw_node_add_child(row, x);
    cw_node_add_child(g, row);
  }
  new_action = cw_node_new("LineEdit", "PsNewAction");
  cw_node_set(new_action, "placeholder_text", cw_val_string("new_action"));
  cw_node_add_child(g, new_action);
  cw_node row = cw_node_new("HBoxContainer", NULL);
  cw_control_set_size_flags(row, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  new_bindings = cw_node_new("LineEdit", "PsNewBindings");
  cw_node_set(new_bindings, "placeholder_text", cw_val_string("key:space, pad:south"));
  cw_control_set_size_flags(new_bindings, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(row, new_bindings);
  cw_node add = cw_node_new("Button", "PsAddAction");
  cw_button_set_text(add, "Add");
  cw_connect(add, "pressed", (cw_node){ 0 }, NULL, on_add_action, NULL, 0);
  cw_node_add_child(row, add);
  cw_node_add_child(g, row);

  g = section("Physics");
  text_row(g, "2D layers", "layer_names_2d", "PsLayers2d");
  text_row(g, "3D layers", "layer_names_3d", "PsLayers3d");
  text_row(g, "Gravity 2D", "gravity_2d", "PsGravity2d");
  text_row(g, "Gravity 3D", "gravity_3d", "PsGravity3d");

  g = section("Render");
  text_row(g, "Width", "render.width", "PsWidth");
  text_row(g, "Height", "render.height", "PsHeight");
  text_row(g, "Ticks/s", "render.fixed_hz", "PsFixedHz");
  cw_node_add_child(g, label("MSAA"));
  cw_node m = cw_node_new("OptionButton", "PsMsaa");
  cw_option_button_add_item(m, "off");
  cw_option_button_add_item(m, "2x");
  cw_option_button_add_item(m, "4x");
  cw_option_button_add_item(m, "8x");
  int ms = cw_project_get_int("render.msaa", 0);
  cw_option_button_select(m, ms >= 8 ? 3 : ms >= 4 ? 2 : ms >= 2 ? 1 : 0);
  cw_connect(m, "item_selected", (cw_node){ 0 }, NULL, on_msaa, NULL, 0);
  cw_node_add_child(g, m);
  cw_node_add_child(g, label("Vsync"));
  cw_node v = cw_node_new("CheckBox", "PsVsync");
  cw_button_set_text(v, "On");
  cw_button_set_pressed(v, cw_project_get_bool("render.vsync", true));
  cw_connect(v, "toggled", (cw_node){ 0 }, NULL, on_check, (void *)cw_intern("render.vsync"), 0);
  cw_node_add_child(g, v);
}

static void on_event(const cwed_event *e, void *user) {
  (void)e, (void)user;
  dirty = true;
}

static cw_node create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", "PsBox");
  status = cw_node_new("Label", "PsStatus");
  cw_node_set(status, "autowrap", cw_val_bool(true));
  cw_node scroll = cw_node_new("ScrollContainer", "PsScroll");
  cw_node_set(scroll, "horizontal_scroll_mode", cw_val_int(0));
  cw_control_set_size_flags(scroll, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  body = cw_node_new("VBoxContainer", "PsBody");
  cw_control_set_size_flags(body, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(scroll, body);
  cw_node_add_child(box, scroll);
  cw_node_add_child(box, status);
  rebuild();
  return box;
}

void pm_settings_frame(void) {
  if (dirty) rebuild();
}

void pm_settings_init(void) {
  static const cwed_param set_p[] = {
    { "key", CWED_ARG_STRING, NULL, "a project.cwproj key: input.<action>, layer_names_2d, render.width, export.<preset>.<key>, ..." },
    { "value", CWED_ARG_STRING, "", "the new value (input.*: key:space, pad:south ...; render.width: 640; gravity_2d: vec2(0, 900))" },
    { "remove", CWED_ARG_BOOL, "false", "delete the key instead" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.set", .title = "Set Project Setting...", .params = set_p,
    .flags = CWED_CMD_UNDO, .example = "project.set key=render.width value=800",
    .doc = "Set (or remove) one key of project.cwproj, checked for its kind (input bindings, sizes, msaa, gravity, app name); "
           "every other line stays. Undoable (editor history).",
    .exec = cmd_set, .undo = undo_set, .merge = merge_set, .free_state = state_free });
  static const cwed_param get_p[] = { { "key", CWED_ARG_STRING, NULL, "a project.cwproj key" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.get", .title = "Get Project Setting", .params = get_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "One project.cwproj value (null when unset).", .exec = cmd_get });
  static const cwed_param list_p[] = { { "prefix", CWED_ARG_STRING, "", "only keys starting with this (input., render., export.)" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.settings", .title = "Project Settings", .params = list_p,
    .flags = CWED_CMD_QUERY, .doc = "Every project.cwproj key and value (quotes stripped).", .exec = cmd_settings });
  cwed_menu_add("Project/Project Settings...", "panel.show project_settings", 110);
  cwed_panel_register(&(cwed_panel_desc){ .id = "project_settings", .title = "Project Settings", .slot = "right_top", .order = 50,
    .create = create });
  cwed_subscribe(CWED_EV_PROJECT, on_event, NULL);
}

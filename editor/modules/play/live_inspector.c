/* Live panel: one node of the RUNNING game, inspected and edited live.
 *
 * Selecting a row in the Remote panel (or play.inspect) loads the node's
 * reflected properties (`node`, `props`); after that every play.poll_frames
 * frames one `get #uid <every property>` refreshes the values, so values
 * the game moves each tick (physics, animation) stay current. Editing a
 * value runs play.set (the game type-checks it; its error shows here and in
 * the Output panel). "Keep" copies the live value into the edited scene
 * (play.apply: one undoable step) when the node is part of it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "play.h"

#define MAX_ROWS 192

typedef struct {
  char name[64], type[16], value[512];
  bool readonly;
  char **enums;
  int nenums;
  cw_node editor, keep;
} row;

static row rows[MAX_ROWS];
static int nrows, uid_shown = -1, gen_seen = -1, frames;
static char node_path[1024], node_type[64], node_name[128], error_text[512];
static long long values_tick = -1;
static bool syncing, get_inflight, loading;
static int polls;
static cw_node header, box, err_label, filter_edit;
static char *props_json, filter[64];

static void free_rows(void) {
  for (int i = 0; i < nrows; i++) {
    for (int k = 0; k < rows[i].nenums; k++) free(rows[i].enums[k]);
    free(rows[i].enums);
  }
  nrows = 0;
}

static const char *scene_path(void) { return node_path[0] ? pl_doc_path(node_path) : NULL; }

/* ── edits ────────────────────────────────────────────────────────────── */

static void poll_now(void);

static void set_live(row *r, const char *value) {
  char *json = NULL;
  bool ok = cwed_run(cw_frame_printf("play.set node=#%d prop=%s value=%s", uid_shown, r->name, cwed_quote(value)), &json);
  if (!ok) {
    pj *e = pj_parse(json);
    snprintf(error_text, sizeof error_text, "%s", pj_str(e, "error") ? pj_str(e, "error") : "the game refused the value");
    pj_free(e);
  } else error_text[0] = 0;
  cw_free(json);
  poll_now();
}

static void on_text(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  if (!syncing) set_live(&rows[(intptr_t)user], a[0].s ? a[0].s : "");
}
static void on_bool(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  if (!syncing) set_live(&rows[(intptr_t)user], a[0].b ? "true" : "false");
}
static void on_enum(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc;
  row *r = &rows[(intptr_t)user];
  if (!syncing && a[0].i >= 0 && a[0].i < r->nenums) set_live(r, r->enums[a[0].i]);
}
static void on_keep(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)a, (void)argc;
  row *r = &rows[(intptr_t)user];
  const char *sp = scene_path();
  if (!sp) return;
  /* the scene that is playing becomes the active document, then one undoable step */
  cwed_doc *d = pl_doc();
  if (d && d != cwed_doc_active()) cwed_runf("scene.switch doc=%d", cwed_doc_index(d));
  cwed_runf("play.apply path=%s prop=%s value=%s", cwed_quote(sp), r->name, cwed_quote(r->value));
}

/* ── loading a node ───────────────────────────────────────────────────── */

static void build_rows(const pj *props) {
  free_rows();
  cwed_clear_children(box);
  const char *owner = NULL;
  cw_node grid = { 0 };
  bool keepable = scene_path() != NULL;
  for (int i = 0; props && i < props->count && nrows < MAX_ROWS; i++) {
    pj *p = props->items[i];
    const char *name = pj_str(p, "name"), *type = pj_str(p, "type"), *own = pj_str(p, "owner");
    if (!name || !type) continue;
    if (filter[0] && !strstr(name, filter)) continue;
    row *r = &rows[nrows];
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->type, sizeof r->type, "%s", type);
    snprintf(r->value, sizeof r->value, "%s", pj_str(p, "value") ? pj_str(p, "value") : "");
    r->readonly = pj_bool(p, "readonly", false);
    pj *en = pj_get(p, "enum");
    if (en && en->t == PJ_ARR && en->count) {
      r->enums = calloc((size_t)en->count, sizeof *r->enums);
      for (int k = 0; k < en->count; k++) r->enums[k] = strdup(en->items[k]->s ? en->items[k]->s : "?");
      r->nenums = en->count;
    }
    if (!owner || !own || strcmp(owner, own)) {
      owner = own;
      cw_node head = cw_node_new("PanelContainer", NULL);
      cw_ui_style hs = cw_control_theme_style(box, "Inspector", "header");
      cw_control_override_style(head, "panel", &hs);
      cw_node hl = cw_node_new("Label", NULL);
      cw_label_set_text(hl, own ? own : "?");
      cw_node_add_child(head, hl);
      cw_node_add_child(box, head);
      grid = cw_node_new("GridContainer", NULL);
      cw_node_set(grid, "columns", cw_val_int(3));
      cw_control_set_size_flags(grid, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
      cw_node_add_child(box, grid);
    }
    cw_node l = cw_node_new("Label", NULL);
    cw_label_set_text(l, name);
    cw_control_set_tooltip(l, pj_str(p, "doc") ? pj_str(p, "doc") : "");
    cw_node_set(l, "mouse_filter", cw_val_int(CW_MOUSE_STOP));
    cw_control_set_custom_min_size(l, (cw_vec2){ 96, 0 });
    cw_node_set(l, "ellipsis", cw_val_bool(true));
    cw_node e;
    void *idx = (void *)(intptr_t)nrows;
    if (r->readonly) {
      e = cw_node_new("Label", NULL);
      cw_label_set_text(e, r->value);
      cw_node_set(e, "ellipsis", cw_val_bool(true));
      cw_control_set_modulate(e, cw_rgba(1, 1, 1, 0.6f));
    } else if (!strcmp(type, "bool")) {
      e = cw_node_new("CheckBox", NULL);
      cw_button_set_text(e, "On");
      cw_button_set_pressed(e, !strcmp(r->value, "true"));
      cw_connect(e, "toggled", (cw_node){ 0 }, NULL, on_bool, idx, 0);
    } else if (r->nenums) {
      e = cw_node_new("OptionButton", NULL);
      for (int k = 0; k < r->nenums; k++) cw_option_button_add_item(e, r->enums[k]);
      for (int k = 0; k < r->nenums; k++)
        if (!strcmp(r->enums[k], r->value)) cw_option_button_select(e, k);
      cw_connect(e, "item_selected", (cw_node){ 0 }, NULL, on_enum, idx, 0);
    } else {
      e = cw_node_new("LineEdit", NULL);
      cw_node_set(e, "theme_overrides", cw_val_string("minimum_character_width = 4")); /* the Keep column stays in view */
      cw_line_edit_set_text(e, r->value);
      cw_connect(e, "text_submitted", (cw_node){ 0 }, NULL, on_text, idx, 0);
    }
    cw_control_set_size_flags(e, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
    cw_node k = cw_node_new("Button", NULL);
    cw_button_set_text(k, "Keep");
    cw_node_set(k, "focus_mode", cw_val_int(CW_FOCUS_NONE));
    cw_control_set_tooltip(k, keepable ? "Copy this live value into the edited scene (play.apply, undoable)"
                                       : "This node is not in an open scene being played: nothing to keep it in");
    cw_node_set(k, "disabled", cw_val_bool(!keepable || r->readonly));
    cw_connect(k, "pressed", (cw_node){ 0 }, NULL, on_keep, idx, 0);
    cw_node_add_child(grid, l);
    cw_node_add_child(grid, e);
    cw_node_add_child(grid, k);
    r->editor = e;
    r->keep = k;
    nrows++;
  }
}

static void on_props(const cw_remote_reply *r, void *u) {
  (void)u;
  loading = false;
  if (!r->ok) {
    snprintf(error_text, sizeof error_text, "%s", r->error ? r->error : "props failed");
    return;
  }
  free(props_json);
  props_json = strdup(r->json);
  pj *v = pj_parse(r->json);
  syncing = true;
  build_rows(pj_get(v, "props"));
  syncing = false;
  pj_free(v);
}

static void on_node(const cw_remote_reply *r, void *u) {
  (void)u;
  if (!r->ok) {
    loading = false;
    snprintf(error_text, sizeof error_text, "%s", r->error ? r->error : "node failed");
    return;
  }
  pj *v = pj_parse(r->json);
  snprintf(node_path, sizeof node_path, "%s", pj_str(v, "path") ? pj_str(v, "path") : "");
  pl_inspect_path(node_path);
  snprintf(node_type, sizeof node_type, "%s", pj_str(v, "type") ? pj_str(v, "type") : "");
  snprintf(node_name, sizeof node_name, "%s", pj_str(v, "name") ? pj_str(v, "name") : "");
  pj_free(v);
  if (!pl_send(cw_frame_printf("props #%d", uid_shown), on_props, NULL)) loading = false;
}

static void load_node(int uid) {
  uid_shown = uid;
  free_rows();
  cwed_clear_children(box);
  free(props_json);
  props_json = NULL;
  node_path[0] = node_type[0] = node_name[0] = error_text[0] = 0;
  values_tick = -1;
  get_inflight = false;
  if (uid <= 0) return;
  loading = true;
  if (!pl_send(cw_frame_printf("node #%d", uid), on_node, NULL)) loading = false;
}

/* ── live values ──────────────────────────────────────────────────────── */

static void apply_value(row *r, const char *v) {
  if (!strcmp(r->value, v)) return;
  snprintf(r->value, sizeof r->value, "%s", v);
  if (!cw_node_valid(r->editor)) return;
  if (r->readonly) cw_label_set_text(r->editor, v);
  else if (!strcmp(r->type, "bool")) cw_button_set_pressed(r->editor, !strcmp(v, "true"));
  else if (r->nenums) {
    for (int k = 0; k < r->nenums; k++)
      if (!strcmp(r->enums[k], v)) cw_option_button_select(r->editor, k);
  } else if (!cw_control_has_focus(r->editor)) cw_line_edit_set_text(r->editor, v); /* never under the user's typing */
}

static void on_values(const cw_remote_reply *r, void *u) {
  (void)u;
  get_inflight = false;
  if (!r->ok) {
    if (r->error && strstr(r->error, "no node")) snprintf(error_text, sizeof error_text, "the node is gone (freed in the game)");
    return;
  }
  pj *v = pj_parse(r->json);
  pj *vals = pj_get(v, "values");
  values_tick = (long long)pj_num(v, "tick", (double)values_tick);
  syncing = true;
  for (int i = 0; vals && i < vals->count; i++) {
    const char *val = pj_str(vals->items[i], "value");
    for (int k = 0; val && k < nrows; k++)
      if (!strcmp(rows[k].name, vals->keys[i])) apply_value(&rows[k], val);
  }
  syncing = false;
  polls++;
  pj_free(v);
}

static void poll_now(void) {
  if (!pl_running() || uid_shown <= 0 || !nrows || get_inflight) return;
  char req[4000];
  int n = snprintf(req, sizeof req, "get #%d", uid_shown);
  for (int i = 0; i < nrows && n < (int)sizeof req - 80; i++) n += snprintf(req + n, sizeof req - (size_t)n, " %s", rows[i].name);
  if (nrows == 1) snprintf(req + n, sizeof req - (size_t)n, " %s", strcmp(rows[0].name, "uid") ? "uid" : "name"); /* one name gets the flat reply: ask for two */
  get_inflight = true;
  if (!pl_send(req, on_values, NULL)) get_inflight = false;
}

static void on_filter(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc, (void)user;
  snprintf(filter, sizeof filter, "%s", a[0].s ? a[0].s : "");
  if (!props_json) return;
  pj *v = pj_parse(props_json);
  syncing = true;
  build_rows(pj_get(v, "props"));
  syncing = false;
  pj_free(v);
  poll_now();
}

/* ── panel ────────────────────────────────────────────────────────────── */

static cw_node create(void *user) {
  (void)user;
  cw_node outer = cw_node_new("VBoxContainer", NULL);
  header = cw_node_new("Label", "Header");
  cw_node_set(header, "ellipsis", cw_val_bool(true));
  err_label = cw_node_new("Label", "Error");
  cw_node_set(err_label, "autowrap", cw_val_bool(true));
  cw_node_set(err_label, "theme_overrides", cw_val_string("font_color = #ff8a7a; font_size = 13"));
  filter_edit = cw_node_new("LineEdit", "Filter");
  cw_node_set(filter_edit, "placeholder_text", cw_val_string("filter properties"));
  cw_connect(filter_edit, "text_changed", (cw_node){ 0 }, NULL, on_filter, NULL, 0);
  cw_node scroll = cw_node_new("ScrollContainer", "Scroll");
  cw_node_set(scroll, "horizontal_scroll_mode", cw_val_int(1));
  cw_control_set_size_flags(scroll, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  box = cw_node_new("VBoxContainer", "Props");
  cw_control_set_size_flags(box, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(scroll, box);
  cw_node_add_child(outer, header);
  cw_node_add_child(outer, err_label);
  cw_node_add_child(outer, filter_edit);
  cw_node_add_child(outer, scroll);
  return outer;
}

void pl_insp_frame(void) {
  if (!cw_node_valid(header)) return;
  frames++;
  if (gen_seen != pl_generation()) {
    gen_seen = pl_generation();
    load_node(0);
  }
  if (pl_inspected() != uid_shown && pl_running()) load_node(pl_inspected());
  if (pl_running() && frames % pl_poll_frames() == 0) poll_now();
  const char *sp = scene_path();
  if (!pl_active()) cw_label_set_text(header, pl_available() ? "No game running" : "Play is not available in the cart editor");
  else if (uid_shown <= 0) cw_label_set_text(header, "Select a node in the Remote panel");
  else
    cw_label_set_text(header, cw_frame_printf("%s (%s) #%d  ·  tick %lld%s", node_name, node_type, uid_shown, values_tick,
                                              sp ? cw_frame_printf("  ·  scene: %s", sp) : "  ·  not in the edited scene"));
  cw_label_set_text(err_label, error_text);
  cw_control_set_visible(err_label, error_text[0] != 0);
}

void pl_insp_widgets(cwed_json *j) {
  cwed_json_key(j, "live");
  cwed_json_printf(j, "{\"uid\":%d,\"tick\":%lld,\"polls\":%d,\"loading\":%s", uid_shown, values_tick, polls, loading ? "true" : "false");
  cwed_json_key(j, "error");
  cwed_json_str(j, error_text);
  cwed_json_key(j, "path");
  cwed_json_str(j, node_path);
  pl_rect_json(j, "filter", cw_control_global_rect(filter_edit));
  cwed_json_key(j, "rows");
  cwed_json_raw(j, "{");
  for (int i = 0; i < nrows; i++) {
    cw_rect e = cw_control_global_rect(rows[i].editor), k = cw_control_global_rect(rows[i].keep);
    cwed_json_key(j, rows[i].name);
    cwed_json_printf(j, "{\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g,\"keep\":{\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g},\"value\":", e.x, e.y,
                     e.w, e.h, k.x, k.y, k.w, k.h);
    cwed_json_str(j, rows[i].value);
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "}}");
}

void pl_insp_init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "live", .title = "Live", .slot = "right_top", .order = 80, .create = create });
}

/* Triggers panel: "when X then Y" rules on nodes, without code.
 *
 * A rule is a Trigger node (engine: cartwheel/triggers.h): a child of the
 * node whose signal fires it, with an action on a target. Because it is a
 * node it saves in the scene as text, undoes like any edit, shows in the
 * scene tree and edits in the inspector; this panel lists the active scene's
 * rules as sentences and builds new ones for the selected node from its
 * declared signals. Clicking a rule selects its Trigger node (the inspector
 * edits it).
 *
 * Commands:
 *   trigger.add on=<node> signal=<name> action=<SET|CALL|EMIT|SPAWN|FREE|CHANGE_SCENE>
 *               [target=<scene path>] [member=..] [value=..] [filter_group=..] [once=..] [name=..]
 *     undoable; target is a SCENE path ("." the root, "Lamp") and is stored
 *     relative to the new trigger ("../Lamp")
 *   trigger.list   every rule of the active scene, as fields and as a sentence */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/triggers.h"
#include "cwed/editor.h"

static const char *const ACTIONS[] = { "SET", "CALL", "EMIT", "SPAWN", "FREE", "CHANGE_SCENE", NULL };

/* ── paths ───────────────────────────────────────────────────────────── */

/* path from the node at scene path `from` to the node at `to` ("." = root) */
static const char *relative(const char *from, const char *to) {
  const char *fp[64], *tp[64];
  int nf = 0, nt = 0;
  char fb[512], tb[512];
  snprintf(fb, sizeof fb, "%s", strcmp(from, ".") ? from : "");
  snprintf(tb, sizeof tb, "%s", strcmp(to, ".") ? to : "");
  for (char *s = strtok(fb, "/"); s && nf < 64; s = strtok(NULL, "/")) fp[nf++] = s;
  for (char *s = strtok(tb, "/"); s && nt < 64; s = strtok(NULL, "/")) tp[nt++] = s;
  int k = 0;
  while (k < nf && k < nt && !strcmp(fp[k], tp[k])) k++;
  char out[600] = "";
  for (int i = k; i < nf; i++) strncat(out, i > k ? "/.." : "..", sizeof out - strlen(out) - 1);
  for (int i = k; i < nt; i++) {
    if (out[0]) strncat(out, "/", sizeof out - strlen(out) - 1);
    strncat(out, tp[i], sizeof out - strlen(out) - 1);
  }
  return cw_frame_printf("%s", out[0] ? out : ".");
}

static bool declares(cw_node n, const char *sig, char *list, int cap) {
  bool found = false;
  list[0] = 0;
  for (const cw_node_type *t = cw_node_get_type(n); t; t = t->base)
    for (const char *const *s = t->signals; s && *s; s++) {
      if (!strcmp(*s, sig)) found = true;
      snprintf(list + strlen(list), (size_t)cap - strlen(list), "%s%s", list[0] ? ", " : "", *s);
    }
  return found;
}

/* ── trigger.add ─────────────────────────────────────────────────────── */

typedef struct { cw_node node, parent; int index; } slot;

static void attach(cw_node parent, cw_node n, int index) {
  if (index < 0 || index >= cw_node_child_count(parent)) cw_node_add_child(parent, n);
  else cw_node_add_child_at(parent, n, index);
}

static bool add_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  slot *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node)) { /* redo: the very node undo detached */
    attach(s->parent, s->node, s->index);
    cwed_json_key(cwed_reply(c), "path");
    cwed_json_str(cwed_reply(c), cwed_doc_node_path(d, s->node));
    return true;
  }
  cw_node on = cwed_arg_node(c, "on");
  if (CW_HANDLE_IS_NULL(on)) return false;
  const char *sig = cwed_arg(c, "signal"), *action = cwed_arg(c, "action");
  if (!*sig) return cwed_fail(c, "which signal fires the rule? signal=<name> (the node's signals are listed by trigger.signals)");
  int ai = -1;
  for (int i = 0; ACTIONS[i]; i++)
    if (!strcmp(ACTIONS[i], action)) ai = i;
  if (ai < 0) return cwed_fail(c, "action is one of SET, CALL, EMIT, SPAWN, FREE, CHANGE_SCENE (got '%s')", action);
  const char *member = cwed_arg(c, "member"), *value = cwed_arg(c, "value");
  if ((ai == 0 || ai == 1 || ai == 2) && !*member)
    return cwed_fail(c, "%s needs member=<%s>", action, ai == 0 ? "property" : ai == 1 ? "method" : "signal");
  if ((ai == 3 || ai == 5) && !*value) return cwed_fail(c, "%s needs value=<scene path>", action);
  const char *target = cwed_arg(c, "target");
  cw_node tn = cwed_doc_node(d, target);
  if (CW_HANDLE_IS_NULL(tn) && ai != 5) return cwed_fail(c, "no target node '%s' in this scene (a scene path: . or Lamp or Level/Door)", target);
  if (ai == 0) {
    const cw_prop *p = cw_node_find_prop(tn, member, NULL);
    if (!p) return cwed_fail(c, "%s (%s) has no property '%s' (node.props %s lists them)", cw_node_name(tn), cw_node_get_type(tn)->name,
                             member, target);
    cw_value v;
    if (!cwed_arg_value(c, "value", tn, member, &v)) return false; /* the message says what the property wants */
  }
  cw_node n = cw_node_new("Trigger", cwed_arg(c, "name"));
  if (CW_HANDLE_IS_NULL(n)) return cwed_fail(c, "this build has no Trigger node type");
  cw_node_set(n, "signal", cw_val_string(sig));
  cw_node_set(n, "action", cw_val_enum(action));
  cw_node_set(n, "member", cw_val_string(member));
  cw_node_set(n, "value", cw_val_string(value));
  cw_node_set(n, "filter_group", cw_val_string(cwed_arg(c, "filter_group")));
  cw_node_set(n, "once", cw_val_bool(cwed_arg_bool(c, "once")));
  cw_node_add_child(on, n);
  if (ai != 5) cw_node_set(n, "target", cw_val_string(relative(cwed_doc_node_path(d, n), cwed_doc_node_path(d, tn))));
  s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
  s->node = n;
  s->parent = on;
  s->index = cw_node_index(n);
  cwed_call_set_state(c, s);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "path");
  cwed_json_str(j, cwed_doc_node_path(d, n));
  char list[512];
  if (!declares(on, sig, list, sizeof list)) {
    cwed_json_key(j, "warning");
    cwed_json_str(j, cw_frame_printf("%s (%s) declares no signal '%s', so the rule will not fire (its signals: %s)", cw_node_name(on),
                                     cw_node_get_type(on)->name, sig, list[0] ? list : "none"));
  }
  return true;
}

static void add_undo(cwed_call *c) {
  slot *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node) && cw_node_valid(cw_node_parent(s->node))) cw_node_remove_child(cw_node_parent(s->node), s->node);
}

static void add_free(cwed_call *c, void *state) {
  (void)c;
  slot *s = state;
  if (cw_node_valid(s->node) && CW_HANDLE_IS_NULL(cw_node_parent(s->node))) cw_node_free(s->node);
  cw_free(s);
}

/* ── listing ─────────────────────────────────────────────────────────── */

typedef struct { cw_node n[512]; int count; } found;
static bool collect(cw_node n, void *u) {
  found *f = u;
  if (cw_node_is(n, "Trigger") && f->count < 512) f->n[f->count++] = n;
  return true;
}

static const char *sget(cw_node n, const char *p) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) && v.type == CW_T_STRING && v.s ? v.s : "";
}

/* "Goal.body_entered [group player] -> SET ../Lamp.color = color(...) (once)" */
static const char *sentence(cwed_doc *d, cw_node t) {
  cw_value a, once, en, fired;
  cw_node_get_prop(t, "action", &a);
  cw_node_get_prop(t, "once", &once);
  cw_node_get_prop(t, "enabled", &en);
  cw_node_get_prop(t, "fired", &fired);
  int ai = (int)a.i;
  const char *src = sget(t, "source");
  cw_node sn = cw_node_get(t, src[0] ? src : "..");
  const char *srcp = cw_node_valid(sn) ? cwed_doc_node_path(d, sn) : src;
  const char *tg = sget(t, "target");
  cw_node tn = cw_node_get(t, tg[0] ? tg : "..");
  const char *tp = cw_node_valid(tn) ? cwed_doc_node_path(d, tn) : tg;
  const char *grp = sget(t, "filter_group");
  const char *what = "";
  switch (ai) {
  case 0: what = cw_frame_printf("set %s.%s = %s", tp, sget(t, "member"), sget(t, "value")); break;
  case 1: what = cw_frame_printf("call %s.%s(%s)", tp, sget(t, "member"), sget(t, "value")); break;
  case 2: what = cw_frame_printf("%s emits %s", tp, sget(t, "member")); break;
  case 3: what = cw_frame_printf("spawn %s under %s", sget(t, "value"), tp); break;
  case 4: what = cw_frame_printf("free %s", tp); break;
  case 5: what = cw_frame_printf("change scene to %s", sget(t, "value")); break;
  }
  return cw_frame_printf("when %s %s%s%s: %s%s%s", srcp, sget(t, "signal"), grp[0] ? " from group " : "", grp, what,
                         once.b ? " (once)" : "", en.b ? "" : " (disabled)");
}

static bool list_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  found f = { .count = 0 };
  cw_node_walk(cwed_doc_root(d), collect, &f);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "triggers");
  cwed_json_raw(j, "[");
  static const char *const FIELDS[] = { "source", "signal", "filter_group", "target", "member", "value", NULL };
  for (int i = 0; i < f.count; i++) {
    cw_node t = f.n[i];
    if (i) cwed_json_raw(j, ",");
    cwed_json_raw(j, "{");
    cwed_json_key(j, "path");
    cwed_json_str(j, cwed_doc_node_path(d, t));
    for (int k = 0; FIELDS[k]; k++) {
      cwed_json_key(j, FIELDS[k]);
      cwed_json_str(j, sget(t, FIELDS[k]));
    }
    cw_value a;
    cw_node_get_prop(t, "action", &a);
    cwed_json_key(j, "action");
    cwed_json_str(j, a.i >= 0 && a.i < 6 ? ACTIONS[a.i] : "?");
    cwed_json_key(j, "rule");
    cwed_json_str(j, sentence(d, t));
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool signals_exec(cwed_call *c) {
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "signals");
  cwed_json_raw(j, "[");
  int k = 0;
  for (const cw_node_type *t = cw_node_get_type(n); t; t = t->base)
    for (const char *const *s = t->signals; s && *s; s++) {
      if (k++) cwed_json_raw(j, ",");
      cwed_json_str(j, *s);
    }
  cwed_json_raw(j, "]");
  return true;
}

/* ── the panel ───────────────────────────────────────────────────────── */

static cw_node title, rules, sig_opt, act_opt, target_e, member_e, value_e, group_e, once_cb, add_btn, for_label;
static cw_node shown[512];
static int shown_count;
static bool dirty = true, syncing;

static void refresh(void) {
  dirty = false;
  if (!cw_node_valid(rules)) return;
  syncing = true;
  cw_item_list_clear(rules);
  shown_count = 0;
  cwed_doc *d = cwed_doc_active();
  if (!d) {
    cw_label_set_text(title, "No scene open");
    syncing = false;
    return;
  }
  found f = { .count = 0 };
  cw_node_walk(cwed_doc_root(d), collect, &f);
  cw_label_set_text(title, cw_frame_printf("%d rule(s) in %s", f.count, cwed_doc_title(d)));
  for (int i = 0; i < f.count; i++) {
    shown[shown_count++] = f.n[i];
    /* cut: an ItemList is as wide as its longest item and would widen the dock (trigger.list has them whole) */
    const char *sn = sentence(d, f.n[i]);
    int len = (int)strlen(sn);
    int idx = cw_item_list_add_item(rules, cw_frame_printf("%.*s%s", len > 40 ? 38 : len, sn, len > 40 ? "..." : ""), (cw_texture){ 0 });
    if (cwed_selection_has(f.n[i])) cw_item_list_select(rules, idx, true);
  }
  /* the form acts on the selected (non-trigger) node: its signals */
  cw_node sel = cwed_selection_count() ? cwed_selection_get(0) : cwed_doc_root(d);
  if (cw_node_is(sel, "Trigger")) sel = cw_node_parent(sel);
  int prev = cw_option_button_selected(sig_opt);
  cw_option_button_clear(sig_opt);
  int k = 0;
  for (const cw_node_type *t = cw_node_get_type(sel); t; t = t->base)
    for (const char *const *s = t->signals; s && *s; s++) cw_option_button_add_item(sig_opt, *s), k++;
  if (k) cw_option_button_select(sig_opt, prev >= 0 && prev < k ? prev : 0);
  cw_button_set_disabled(add_btn, k == 0);
  cw_label_set_text(for_label, k ? cw_frame_printf("New rule on %s (%s):", cwed_doc_node_path(d, sel), cw_node_get_type(sel)->name)
                                 : cw_frame_printf("%s (%s) emits no signals: select a node that does", cwed_doc_node_path(d, sel),
                                                   cw_node_get_type(sel)->name));
  syncing = false;
}

static void on_event(const cwed_event *e, void *u) {
  (void)e, (void)u;
  dirty = true;
}

static void on_rule(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)argc, (void)u;
  if (syncing) return;
  int i = (int)a[0].i;
  cwed_doc *d = cwed_doc_active();
  if (d && i >= 0 && i < shown_count && cw_node_valid(shown[i])) cwed_runf("select paths=%s", cwed_quote(cwed_doc_node_path(d, shown[i])));
}

static void on_add(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)a, (void)argc, (void)u;
  cwed_doc *d = cwed_doc_active();
  if (!d) return;
  cw_node sel = cwed_selection_count() ? cwed_selection_get(0) : cwed_doc_root(d);
  if (cw_node_is(sel, "Trigger")) sel = cw_node_parent(sel);
  int si = cw_option_button_selected(sig_opt), ai = cw_option_button_selected(act_opt);
  if (si < 0 || ai < 0) return;
  /* the signal's name: walk the same list the option button was filled from */
  const char *sig = NULL;
  int k = 0;
  for (const cw_node_type *t = cw_node_get_type(sel); t && !sig; t = t->base)
    for (const char *const *s = t->signals; s && *s && !sig; s++)
      if (k++ == si) sig = *s;
  if (!sig) return;
  const char *target = cw_line_edit_text(target_e);
  cwed_runf("trigger.add on=%s signal=%s action=%s target=%s member=%s value=%s filter_group=%s once=%s",
            cwed_quote(cwed_doc_node_path(d, sel)), cwed_quote(sig), ACTIONS[ai],
            cwed_quote(target && *target ? target : cwed_doc_node_path(d, sel)), cwed_quote(cw_line_edit_text(member_e)),
            cwed_quote(cw_line_edit_text(value_e)), cwed_quote(cw_line_edit_text(group_e)),
            cw_button_pressed(once_cb) ? "true" : "false");
}

static cw_node field(cw_node grid, const char *label, const char *name, const char *hint) {
  cw_node l = cw_node_new("Label", NULL);
  cw_label_set_text(l, label);
  cw_node_add_child(grid, l);
  cw_node e = cw_node_new("LineEdit", name);
  cw_node_set(e, "placeholder_text", cw_val_string(hint));
  cw_control_set_size_flags(e, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_add_child(grid, e);
  return e;
}

static cw_node create(void *u) {
  (void)u;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  title = cw_node_new("Label", "Title");
  cw_node_add_child(box, title);
  rules = cw_node_new("ItemList", "Rules");
  cw_control_set_size_flags(rules, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_connect(rules, "item_selected", (cw_node){ 0 }, NULL, on_rule, NULL, 0);
  cw_node_add_child(box, rules);
  for_label = cw_node_new("Label", "For");
  cw_node_set(for_label, "ellipsis", cw_val_bool(true));
  cw_node_add_child(box, for_label);
  cw_node grid = cw_node_new("GridContainer", "Form");
  cw_node_set(grid, "columns", cw_val_int(2));
  cw_node l = cw_node_new("Label", NULL);
  cw_label_set_text(l, "When");
  cw_node_add_child(grid, l);
  sig_opt = cw_node_new("OptionButton", "Signal");
  cw_node_add_child(grid, sig_opt);
  l = cw_node_new("Label", NULL);
  cw_label_set_text(l, "Do");
  cw_node_add_child(grid, l);
  act_opt = cw_node_new("OptionButton", "Action");
  for (int i = 0; ACTIONS[i]; i++) cw_option_button_add_item(act_opt, ACTIONS[i]);
  cw_option_button_select(act_opt, 0);
  cw_node_add_child(grid, act_opt);
  target_e = field(grid, "Target", "Target", "scene path (empty = this node)");
  member_e = field(grid, "Member", "Member", "property / method / signal");
  value_e = field(grid, "Value", "Value", "value text or scene path");
  group_e = field(grid, "Only group", "Group", "fire only for nodes in this group");
  cw_node_add_child(box, grid);
  cw_node row = cw_node_new("HBoxContainer", "Buttons");
  once_cb = cw_node_new("CheckBox", "Once");
  cw_button_set_text(once_cb, "Once");
  cw_node_add_child(row, once_cb);
  add_btn = cw_node_new("Button", "Add");
  cw_button_set_text(add_btn, "Add Rule");
  cw_connect(add_btn, "pressed", (cw_node){ 0 }, NULL, on_add, NULL, 0);
  cw_node_add_child(row, add_btn);
  cw_node_add_child(box, row);
  refresh();
  return box;
}

static int32_t dbg_rules;
static uint32_t dbg_hash;

static void frame(double dt) {
  (void)dt;
  if (!dirty) return;
  refresh();
  /* debug fields: the active scene's rules and a hash of them as sentences */
  dbg_rules = 0;
  dbg_hash = CW_HASH32_INIT;
  cwed_doc *d = cwed_doc_active();
  if (!d) return;
  found f = { .count = 0 };
  cw_node_walk(cwed_doc_root(d), collect, &f);
  dbg_rules = f.count;
  for (int i = 0; i < f.count; i++) {
    const char *s = sentence(d, f.n[i]);
    dbg_hash = cw_hash32(s, strlen(s), dbg_hash);
  }
}

static void init(void) {
  static const cwed_param add_p[] = {
    { "on", CWED_ARG_PATH, NULL, "the node whose signal fires the rule (the Trigger becomes its child)" },
    { "signal", CWED_ARG_STRING, NULL, "its signal (trigger.signals lists them)" },
    { "action", CWED_ARG_STRING, "SET", "SET CALL EMIT SPAWN FREE CHANGE_SCENE" },
    { "target", CWED_ARG_STRING, ".", "scene path of the node acted on (stored relative to the trigger)" },
    { "member", CWED_ARG_STRING, "", "SET: property; CALL: method; EMIT: signal; SPAWN: new node name" },
    { "value", CWED_ARG_STRING, "", "SET: value text; CALL: one argument; SPAWN / CHANGE_SCENE: scene path" },
    { "filter_group", CWED_ARG_STRING, "", "fire only when the signal's first argument is a node in this group" },
    { "once", CWED_ARG_BOOL, "false", "fire at most once" },
    { "name", CWED_ARG_STRING, "Trigger", "the Trigger node's name" },
    { 0 } };
  static const cwed_param path_p[] = { { "path", CWED_ARG_PATH, NULL, "a node" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "trigger.add", .title = "Add Trigger Rule", .params = add_p,
    .flags = CWED_CMD_UNDO | CWED_CMD_SCENE,
    .doc = "Add a data-driven rule: when node `on` emits `signal`, do `action` (SET a property, CALL a method, EMIT a signal, "
           "SPAWN a scene, FREE, CHANGE_SCENE) on `target`. A Trigger node, saved in the scene (cartwheel/triggers.h).",
    .example = "trigger.add on={sel} signal=body_entered action=SET target=. member=visible value=false once=true",
    .exec = add_exec, .undo = add_undo, .free_state = add_free });
  cwed_command_register(&(cwed_command_desc){ .name = "trigger.list", .title = "List Trigger Rules",
    .flags = CWED_CMD_SCENE | CWED_CMD_QUERY, .doc = "Every Trigger rule of the active scene: fields and the rule as a sentence.",
    .exec = list_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "trigger.signals", .title = "Node Signals", .params = path_p,
    .flags = CWED_CMD_SCENE | CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "The signals a node's type declares (what a trigger can listen to).",
    .exec = signals_exec });
  cwed_panel_register(&(cwed_panel_desc){ .id = "triggers", .title = "Triggers", .slot = "right_bottom", .order = 20, .create = create });
  cwed_menu_add("Node/Add Trigger Rule...", "trigger.add", 300);
  cw_debug_i32("triggers.rules", &dbg_rules);
  cw_debug_u32("triggers.hash", &dbg_hash);
  cwed_subscribe(CWED_EV_DOC_CHANGED | CWED_EV_DOC_ACTIVE | CWED_EV_DOC_CLOSED | CWED_EV_SELECTION | CWED_EV_DOC_OPENED, on_event, NULL);
}

CWED_MODULE(triggers) = { .title = "Triggers", .init = init, .frame = frame };

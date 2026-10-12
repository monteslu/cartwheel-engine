/* Scene Tree panel.
 *
 * The active scene's hierarchy: name, type and markers per row. Rows of an
 * instanced scene's own nodes (inherited: the parent scene can override
 * their properties but not delete, rename or move them) are dimmed; an
 * instance root says which scene it instances; group membership and signal
 * connections show as markers. Selection is shared with every panel and is
 * multiple (ctrl+click toggles, shift+click extends); dragging rows onto a
 * row reparents them (onto = into it, top / bottom edge = before / after
 * it), and a .cwscene dragged in from a file list (drag kind "file")
 * instances it there. Below the tree, the selected node's groups and
 * signals: add / remove groups, connect / disconnect signals.
 *
 * Commands added here (undoable, with examples):
 *   node.connect from signal to method [flags]    node.disconnect from signal to method
 * The rest are the core's node.* commands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/vfs.h"
#include "cwed/editor.h"

#define MAX_ITEMS 8192

static cw_node tree, hint, tabs, groups_box, group_edit, signals_box, sig_option, sig_target, sig_method;
static bool syncing, dirty = true, side_dirty = true;
static cw_node item_node[MAX_ITEMS];
static int item_count;

/* ── rows ─────────────────────────────────────────────────────────────── */

static bool inherited(cwed_doc *d, cw_node n) {
  cw_node p = cw_node_parent(n);
  return cw_node_valid(p) && p.id != cwed_doc_root(d).id && cw_scene_node_in_instance(p, cwed_doc_root(d));
}

static int conn_count(cw_node n) { return cw_node_connections(n, false, NULL, 0); }

static const char *markers(cwed_doc *d, cw_node n) {
  char buf[512] = "";
  const char *src = cw_scene_instance_source(n);
  if (src && n.id != cwed_doc_root(d).id && !inherited(d, n)) {
    const char *b = strrchr(src, '/');
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "instance %s ", b ? b + 1 : src);
  } else if (inherited(d, n)) {
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "inherited ");
  }
  int k = conn_count(n);
  if (k) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%d signal%s ", k, k > 1 ? "s" : "");
  return cw_frame_printf("%s", buf);
}

/* Group names: parsed from the scene text's groups= attributes (the node
 * API answers membership, not the list). Refreshed with the tree. */
#define MAX_GROUPS 256
static const char *known_groups[MAX_GROUPS];
static int known_group_count;

static void learn_text(const char *t, int depth) {
  for (const char *p = t ? strstr(t, " groups=\"") : NULL; p; p = strstr(p + 1, " groups=\"")) {
    const char *s = p + 9, *e = strchr(s, '"');
    if (!e) break;
    while (s < e) {
      const char *c = memchr(s, ',', (size_t)(e - s));
      const char *end = c ? c : e;
      const char *name = cw_intern(cw_frame_printf("%.*s", (int)(end - s), s));
      bool have = false;
      for (int i = 0; i < known_group_count; i++) have = have || known_groups[i] == name;
      if (!have && known_group_count < MAX_GROUPS) known_groups[known_group_count++] = name;
      s = end + 1;
    }
  }
  /* instanced scenes bring their own groups: read their files too */
  for (const char *p = t && depth < 6 ? strstr(t, "[ext_scene ") : NULL; p; p = strstr(p + 1, "[ext_scene ")) {
    const char *q = strstr(p, "path=\""), *nl = strchr(p, '\n');
    if (!q || (nl && q > nl)) continue;
    q += 6;
    const char *e = strchr(q, '"');
    if (!e) break;
    size_t len;
    char *sub = cw_vfs_read(cw_frame_printf("%.*s", (int)(e - q), q), &len);
    if (sub) {
      learn_text(sub, depth + 1);
      free(sub);
    }
  }
}

static void learn_groups(cwed_doc *d) {
  known_group_count = 0;
  char *t = cwed_doc_text(d);
  learn_text(t, 0);
  cw_free(t);
}

static const char *groups_of(cw_node n) {
  char buf[512] = "";
  for (int i = 0; i < known_group_count; i++)
    if (cw_node_in_group(n, known_groups[i])) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%s", buf[0] ? "," : "", known_groups[i]);
  return cw_frame_printf("%s", buf);
}

static void add_items(cw_node n, int parent, cwed_doc *d) {
  if (item_count >= MAX_ITEMS) return;
  int item = cw_uitree_create_item(tree, parent, cw_node_name(n));
  cw_uitree_set_text(tree, item, 1, cw_node_get_type(n)->name);
  const char *g = groups_of(n);
  cw_uitree_set_text(tree, item, 2, g[0] ? cw_frame_printf("%s[%s]", markers(d, n), g) : markers(d, n));
  cw_uitree_set_metadata(tree, item, (int64_t)n.id);
  if (inherited(d, n)) {
    cw_color dimc = cw_rgba(0.55f, 0.6f, 0.68f, 1);
    for (int c = 0; c < 3; c++) cw_uitree_set_custom_color(tree, item, c, &dimc);
  } else if (cw_scene_instance_source(n) && n.id != cwed_doc_root(d).id) {
    cw_color inst = cw_rgba(0.55f, 0.85f, 1, 1);
    cw_uitree_set_custom_color(tree, item, 0, &inst);
  }
  if (item < MAX_ITEMS) item_node[item] = n, item_count = item + 1 > item_count ? item + 1 : item_count;
  for (int i = 0; i < cw_node_child_count(n); i++) add_items(cw_node_child(n, i), item, d);
  /* an instance starts collapsed (its insides are the instanced scene's) */
  if (cw_scene_instance_source(n) && n.id != cwed_doc_root(d).id && cw_node_child_count(n)) cw_uitree_set_collapsed(tree, item, true);
}

static int item_of(cw_node n) {
  for (int i = 0; i < item_count; i++)
    if (item_node[i].id == n.id) return i;
  return -1;
}

static void sync_selection(void) {
  syncing = true;
  cw_uitree_deselect_all(tree);
  int first = -1;
  for (int i = 0; i < cwed_selection_count(); i++) {
    int it = item_of(cwed_selection_get(i));
    if (it < 0) continue;
    cw_uitree_set_selected(tree, it, true);
    if (first < 0) first = it;
  }
  if (first >= 0) cw_uitree_select(tree, first); /* reveal + focus row */
  syncing = false;
}

static void rebuild(void) {
  syncing = true;
  cw_uitree_clear(tree);
  item_count = 0;
  cwed_doc *d = cwed_doc_active();
  if (d) {
    learn_groups(d);
    add_items(cwed_doc_root(d), -1, d);
  }
  syncing = false;
  sync_selection();
  dirty = false;
  side_dirty = true;
}

/* ── selection from the tree ───────────────────────────────────────────── */

static void push_selection(void) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return;
  size_t cap = 64;
  int k = 0;
  for (int i = 0; i < item_count; i++)
    if (cw_uitree_is_selected(tree, i) && cw_node_valid(item_node[i])) cap += strlen(cwed_doc_node_path(d, item_node[i])) + 1, k++;
  char *paths = cw_frame_alloc(cap), *w = paths;
  *w = 0;
  for (int i = 0; i < item_count; i++)
    if (cw_uitree_is_selected(tree, i) && cw_node_valid(item_node[i])) w += sprintf(w, "%s%s", w == paths ? "" : ",", cwed_doc_node_path(d, item_node[i]));
  if (k) cwed_runf("select paths=%s", cwed_quote(paths));
  else cwed_run("select.clear", NULL);
}

static bool push_pending;
static void on_select(cw_node t, const cw_value *a, int argc, void *user) {
  (void)t, (void)a, (void)argc, (void)user;
  if (!syncing) push_pending = true; /* several signals per click: one select command in frame */
}

static void on_activate(cw_node t, const cw_value *a, int argc, void *user) {
  (void)argc, (void)user;
  cwed_doc *d = cwed_doc_active();
  int it = (int)a[0].i;
  if (!d || it < 0 || it >= item_count) return;
  cw_node n = item_node[it];
  (void)t;
  if (cw_node_valid(n) && !inherited(d, n) && n.id != cwed_doc_root(d).id)
    cwed_runf("palette.open text=%s", cwed_quote(cw_frame_printf("node.rename path=%s name=", cwed_quote(cwed_doc_node_path(d, n)))));
}

/* ── drag and drop ─────────────────────────────────────────────────────── */

static bool is_ancestor_or_self(cw_node a, cw_node n) {
  for (cw_node p = n; cw_node_valid(p); p = cw_node_parent(p))
    if (p.id == a.id) return true;
  return false;
}

/* the dragged nodes: the selection when the dragged row is in it, else that row */
static int dragged(const cw_ui_drag *dg, cw_node *out, int cap) {
  cw_node src = { (uint64_t)dg->payload };
  int k = 0;
  if (cwed_selection_has(src)) {
    for (int i = 0; i < cwed_selection_count() && k < cap; i++) {
      cw_node n = cwed_selection_get(i);
      bool under_other = false;
      for (int j = 0; j < cwed_selection_count(); j++)
        if (j != i && is_ancestor_or_self(cwed_selection_get(j), cw_node_parent(n))) under_other = true;
      if (!under_other) out[k++] = n;
    }
  } else if (cw_node_valid(src)) {
    out[k++] = src;
  }
  return k;
}

/* where a drop lands: parent + index (-1 = last); false when it cannot */
static bool drop_place(cw_node target, const cw_ui_drag *dg, cw_vec2 local, cw_node *parent, int *index, cw_rect *hl) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return false;
  int section;
  int it = cw_uitree_item_at(target, local, &section);
  if (it < 0 || it >= item_count) return false;
  cw_node on = item_node[it];
  if (!cw_node_valid(on)) return false;
  bool root = on.id == cwed_doc_root(d).id;
  if (root || !strcmp(dg->kind, "file")) section = 0;
  if (section == 0) {
    if (inherited(d, on) || (cw_scene_instance_source(on) && !root)) {
      /* children may be added to an instance root, not inside its scene */
      if (inherited(d, on)) return false;
    }
    *parent = on;
    *index = -1;
  } else {
    *parent = cw_node_parent(on);
    if (inherited(d, on)) return false;
    *index = cw_node_index(on) + (section > 0 ? 1 : 0);
  }
  if (hl) {
    cw_rect r = cw_uitree_item_rect(target, it), g = cw_control_global_rect(target);
    r.x -= g.x, r.y -= g.y; /* canvas -> the tree's own coordinates */
    *hl = section == 0 ? r : (cw_rect){ r.x, section < 0 ? r.y - 1 : r.y + r.h - 1, r.w, 3 };
  }
  return true;
}

static bool can_drop(cw_node target, const cw_ui_drag *dg, cw_vec2 local, cw_rect *hl, void *user) {
  (void)user;
  cwed_doc *d = cwed_doc_active();
  if (!d) return false;
  cw_node parent;
  int index;
  if (!strcmp(dg->kind, "file")) {
    size_t l = strlen(dg->text);
    return l > 8 && !strcmp(dg->text + l - 8, ".cwscene") && drop_place(target, dg, local, &parent, &index, hl);
  }
  if (strcmp(dg->kind, "tree_item") || dg->source.id != tree.id) return false;
  if (!drop_place(target, dg, local, &parent, &index, hl)) return false;
  cw_node nodes[64];
  int k = dragged(dg, nodes, 64);
  for (int i = 0; i < k; i++) {
    if (nodes[i].id == cwed_doc_root(d).id || inherited(d, nodes[i]) || is_ancestor_or_self(nodes[i], parent)) return false;
  }
  return k > 0;
}

static void drop(cw_node target, const cw_ui_drag *dg, cw_vec2 local, void *user) {
  (void)user;
  cwed_doc *d = cwed_doc_active();
  cw_node parent;
  int index;
  if (!d || !drop_place(target, dg, local, &parent, &index, NULL)) return;
  const char *pp = cwed_doc_node_path(d, parent);
  if (!strcmp(dg->kind, "file")) {
    cwed_runf("node.instance scene=%s parent=%s", cwed_quote(dg->text), cwed_quote(pp));
    return;
  }
  cw_node nodes[64];
  int k = dragged(dg, nodes, 64);
  for (int i = 0; i < k; i++) {
    cw_node n = nodes[i];
    int idx = index;
    /* moving down inside the same parent: the node's own slot closes first */
    if (idx >= 0 && cw_node_parent(n).id == parent.id && cw_node_index(n) < idx) idx--;
    if (cw_node_parent(n).id == parent.id) {
      if (idx < 0) idx = cw_node_child_count(parent) - 1;
      if (idx != cw_node_index(n)) cwed_runf("node.move path=%s index=%d", cwed_quote(cwed_doc_node_path(d, n)), idx);
    } else {
      cwed_runf("node.reparent path=%s parent=%s index=%d", cwed_quote(cwed_doc_node_path(d, n)), cwed_quote(pp), idx);
    }
    if (index >= 0) index++;
  }
}

/* ── groups and signals of the selected node ─────────────────────────── */

static cw_node shown_node(void) { return cwed_selection_count() == 1 ? cwed_selection_get(0) : (cw_node){ 0 }; }

static void on_group_remove(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  cwed_doc *d = cwed_doc_active();
  cw_node n = shown_node();
  if (d && cw_node_valid(n)) cwed_runf("node.group path=%s group=%s add=false", cwed_quote(cwed_doc_node_path(d, n)), cwed_quote((const char *)user));
}
static void on_group_add(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  cwed_doc *d = cwed_doc_active();
  cw_node n = shown_node();
  const char *g = cw_line_edit_text(group_edit);
  if (d && cw_node_valid(n) && g[0]) {
    cwed_runf("node.group path=%s group=%s", cwed_quote(cwed_doc_node_path(d, n)), cwed_quote(g));
    cw_line_edit_set_text(group_edit, "");
  }
}

typedef struct { const char *signal, *to, *method; } conn_ref;
static conn_ref conn_refs[64];

static void on_disconnect(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  conn_ref *r = user;
  cwed_doc *d = cwed_doc_active();
  cw_node n = shown_node();
  if (d && cw_node_valid(n))
    cwed_runf("node.disconnect from=%s signal=%s to=%s method=%s", cwed_quote(cwed_doc_node_path(d, n)), cwed_quote(r->signal),
              cwed_quote(r->to), cwed_quote(r->method));
}
static void on_connect(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  cwed_doc *d = cwed_doc_active();
  cw_node n = shown_node();
  int k = cw_option_button_selected(sig_option);
  if (!d || !cw_node_valid(n) || k < 0) return;
  /* the option's label is the signal name */
  cw_value items;
  if (!cw_node_get_prop(sig_option, "items", &items) || !items.s) return;
  const char *p = items.s;
  for (int i = 0; i < k && p; i++) p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL;
  if (!p) return;
  const char *e = strchr(p, '\n');
  const char *name = cw_frame_printf("%.*s", e ? (int)(e - p) : (int)strlen(p), p);
  const char *to = cw_line_edit_text(sig_target), *method = cw_line_edit_text(sig_method);
  cwed_runf("node.connect from=%s signal=%s to=%s method=%s", cwed_quote(cwed_doc_node_path(d, n)), cwed_quote(name),
            cwed_quote(to[0] ? to : "."), cwed_quote(method));
}

static cw_node small_label(const char *text, const char *color) {
  cw_node l = cw_node_new("Label", NULL);
  cw_label_set_text(l, text);
  cw_node_set(l, "theme_overrides", cw_val_string(cw_frame_printf("font_size = 12; font_color = %s", color)));
  cw_node_set(l, "ellipsis", cw_val_bool(true));
  cw_control_set_size_flags(l, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  return l;
}
static cw_node small_button(const char *text, const char *tip, void (*fn)(cw_node, const cw_value *, int, void *), void *user) {
  cw_node b = cw_node_new("Button", NULL);
  cw_button_set_text(b, text);
  cw_control_set_tooltip(b, tip);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_node_set(b, "theme_overrides", cw_val_string("font_size = 12"));
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, fn, user, 0);
  return b;
}

static void rebuild_side(void) {
  side_dirty = false;
  cwed_doc *d = cwed_doc_active();
  cw_node n = shown_node();
  cwed_clear_children(groups_box);
  cwed_clear_children(signals_box);
  if (!d || !cw_node_valid(n)) {
    cw_node_add_child(groups_box, small_label(cwed_selection_count() > 1 ? "Select one node to see its groups" : "No node selected", "#8a93a3"));
    cw_node_add_child(signals_box, small_label(cwed_selection_count() > 1 ? "Select one node to see its signals" : "No node selected", "#8a93a3"));
    return;
  }
  /* groups */
  bool any = false;
  for (int i = 0; i < known_group_count; i++) {
    if (!cw_node_in_group(n, known_groups[i])) continue;
    any = true;
    cw_node row = cw_node_new("HBoxContainer", NULL);
    cw_node_add_child(row, small_label(known_groups[i], "#d8dce4"));
    cw_node_add_child(row, small_button("Remove", "node.group add=false", on_group_remove, (void *)known_groups[i]));
    cw_node_add_child(groups_box, row);
  }
  if (!any) cw_node_add_child(groups_box, small_label("In no group", "#8a93a3"));
  cw_node add = cw_node_new("HBoxContainer", NULL);
  group_edit = cw_node_new("LineEdit", "GroupName");
  cw_node_set(group_edit, "placeholder_text", cw_val_string("group name"));
  cw_control_set_size_flags(group_edit, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(group_edit, "text_submitted", (cw_node){ 0 }, NULL, on_group_add, NULL, 0);
  cw_node_add_child(add, group_edit);
  cw_node_add_child(add, small_button("Add", "node.group: add the node to this group", on_group_add, NULL));
  cw_node_add_child(groups_box, add);
  /* signals: the type chain's, then the connections */
  cw_connection_info conns[64];
  int k = cw_node_connections(n, false, conns, 64);
  if (k > 64) k = 64;
  for (int i = 0; i < k; i++) {
    if (!conns[i].method) continue;
    conn_refs[i] = (conn_ref){ conns[i].signal, cw_intern(cwed_doc_node_path(d, conns[i].target)), conns[i].method };
    cw_node row = cw_node_new("HBoxContainer", NULL);
    cw_node_add_child(row, small_label(cw_frame_printf("%s -> %s.%s()", conns[i].signal, conn_refs[i].to, conns[i].method), "#d8dce4"));
    cw_node_add_child(row, small_button("Disconnect", "node.disconnect", on_disconnect, &conn_refs[i]));
    cw_node_add_child(signals_box, row);
  }
  if (!k) cw_node_add_child(signals_box, small_label("No connections", "#8a93a3"));
  cw_node form = cw_node_new("HBoxContainer", NULL);
  sig_option = cw_node_new("OptionButton", "Signal");
  int count = 0;
  for (const cw_node_type *t = cw_node_get_type(n); t; t = t->base)
    for (int i = 0; t->signals && t->signals[i]; i++) cw_option_button_add_item(sig_option, t->signals[i]), count++;
  if (!count) cw_option_button_add_item(sig_option, "(this type emits no signals)");
  cw_option_button_select(sig_option, 0);
  cw_node_set(sig_option, "theme_overrides", cw_val_string("font_size = 12"));
  sig_target = cw_node_new("LineEdit", "Target");
  cw_line_edit_set_text(sig_target, ".");
  cw_control_set_custom_min_size(sig_target, (cw_vec2){ 50, 0 });
  sig_method = cw_node_new("LineEdit", "Method");
  cw_node_set(sig_method, "placeholder_text", cw_val_string("method"));
  cw_control_set_size_flags(sig_method, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(form, sig_option);
  cw_node_add_child(form, sig_target);
  cw_node_add_child(form, sig_method);
  cw_node_add_child(form, small_button("Connect", "node.connect", on_connect, NULL));
  if (count) cw_node_add_child(signals_box, form);
  else cw_node_free(form);
}

/* ── node.connect / node.disconnect ─────────────────────────────────── */

typedef struct {
  cw_node from, to;
  const char *signal, *method;
  uint32_t flags;
  /* the emitter's named connections before, in order (undo rebuilds them so
   * the saved order is restored exactly) */
  int count;
  cw_connection_info before[64];
} conn_state;

static void restore_conns(conn_state *s) {
  cw_connection_info now[64];
  int k = cw_node_connections(s->from, false, now, 64);
  for (int i = 0; i < k && i < 64; i++)
    if (now[i].method) cw_disconnect(now[i].id);
  for (int i = 0; i < s->count; i++)
    cw_connect(s->from, s->before[i].signal, s->before[i].target, s->before[i].method, NULL, NULL, s->before[i].flags);
}

static int find_conn(cw_node from, const char *sig, cw_node to, const char *method, uint64_t *id) {
  cw_connection_info c[64];
  int k = cw_node_connections(from, false, c, 64);
  for (int i = 0; i < k && i < 64; i++)
    if (c[i].method && !strcmp(c[i].signal, sig) && c[i].target.id == to.id && !strcmp(c[i].method, method)) {
      if (id) *id = c[i].id;
      return i;
    }
  return -1;
}

static bool signal_declared(cw_node n, const char *sig) {
  for (const cw_node_type *t = cw_node_get_type(n); t; t = t->base)
    for (int i = 0; t->signals && t->signals[i]; i++)
      if (!strcmp(t->signals[i], sig)) return true;
  return false;
}

static bool conn_common(cwed_call *c, bool connect) {
  cwed_doc *d = cwed_call_doc(c);
  cw_node from = cwed_arg_node(c, "from"), to = cwed_arg_node(c, "to");
  if (CW_HANDLE_IS_NULL(from) || CW_HANDLE_IS_NULL(to)) return false;
  const char *sig = cwed_arg(c, "signal"), *method = cwed_arg(c, "method");
  if (!*sig || strchr(sig, '"')) return cwed_fail(c, "'%s' is not a signal name", sig);
  if (!*method || strchr(method, '"')) return cwed_fail(c, "'%s' is not a method name", method);
  int exists = find_conn(from, sig, to, method, NULL);
  if (connect && exists >= 0) return cwed_fail(c, "%s.%s is already connected to %s.%s()", cwed_arg(c, "from"), sig, cwed_arg(c, "to"), method);
  if (!connect && exists < 0)
    return cwed_fail(c, "no connection %s.%s -> %s.%s() (the scene tree's Signals tab lists them)", cwed_arg(c, "from"), sig, cwed_arg(c, "to"), method);
  conn_state *s = cwed_call_state(c);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  s->from = from, s->to = to;
  s->signal = cw_intern(sig), s->method = cw_intern(method);
  s->flags = (uint32_t)cwed_arg_int(c, "flags");
  cw_connection_info all[64];
  int k = cw_node_connections(from, false, all, 64);
  s->count = 0;
  for (int i = 0; i < k && i < 64; i++)
    if (all[i].method) s->before[s->count++] = all[i];
  if (connect) cw_connect(from, s->signal, to, s->method, NULL, NULL, s->flags);
  else {
    uint64_t id;
    if (find_conn(from, sig, to, method, &id) >= 0) cw_disconnect(id);
  }
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "from");
  cwed_json_str(j, cwed_doc_node_path(d, from));
  cwed_json_key(j, "signal");
  cwed_json_str(j, sig);
  cwed_json_key(j, "to");
  cwed_json_str(j, cwed_doc_node_path(d, to));
  cwed_json_key(j, "method");
  cwed_json_str(j, method);
  cwed_json_printf(j, ",\"declared\":%s", signal_declared(from, sig) ? "true" : "false");
  return true;
}
static bool connect_exec(cwed_call *c) { return conn_common(c, true); }
static bool disconnect_exec(cwed_call *c) { return conn_common(c, false); }
static void conn_undo(cwed_call *c) {
  conn_state *s = cwed_call_state(c);
  if (s && cw_node_valid(s->from)) restore_conns(s);
}

/* the visible rows: path and window rect (UI units), for pointer scripts */
static bool rows_exec(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  if (dirty) rebuild();
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "rows");
  cwed_json_raw(j, "[");
  bool first = true;
  for (int i = 0; i < item_count; i++) {
    cw_rect r = cw_uitree_item_rect(tree, i);
    if (r.w <= 0 || !cw_node_valid(item_node[i]) || !d) continue;
    cwed_json_raw(j, first ? "{\"path\":" : ",{\"path\":");
    first = false;
    cwed_json_str(j, cwed_doc_node_path(d, item_node[i]));
    cwed_json_printf(j, ",\"rect\":[%g,%g,%g,%g],\"selected\":%s}", r.x, r.y, r.w, r.h, cw_uitree_is_selected(tree, i) ? "true" : "false");
  }
  cwed_json_raw(j, "]");
  return true;
}

/* ── panel ─────────────────────────────────────────────────────────────── */

static void on_event(const cwed_event *e, void *user) {
  (void)user;
  if (e->type == CWED_EV_SELECTION) {
    side_dirty = true;
    if (!dirty) sync_selection();
    return;
  }
  dirty = true;
}

static void frame(double dt) {
  (void)dt;
  if (!cw_node_valid(tree)) return;
  if (push_pending) {
    push_pending = false;
    push_selection();
  }
  if (dirty) rebuild();
  if (side_dirty && cw_node_valid(tabs)) rebuild_side();
  cwed_doc *d = cwed_doc_active();
  if (cw_node_valid(hint)) {
    int k = cwed_selection_count();
    cw_label_set_text(hint, !d ? "No scene open" : k == 1 ? cw_frame_printf("%s", cwed_doc_node_path(d, cwed_selection_get(0)))
                                    : k > 1 ? cw_frame_printf("%d nodes selected", k)
                                            : "Drag rows to reparent; ctrl/shift+click selects several");
  }
}

static void on_button(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  const char *what = user;
  cwed_doc *d = cwed_doc_active();
  if (!d) return;
  const char *parent = cwed_selection_count() ? cwed_doc_node_path(d, cwed_selection_get(0)) : ".";
  if (!strcmp(what, "add")) cwed_runf("palette.open text=%s", cwed_quote(cw_frame_printf("node.add parent=%s type=", cwed_quote(parent))));
  else if (!strcmp(what, "instance"))
    cwed_runf("palette.open text=%s", cwed_quote(cw_frame_printf("node.instance parent=%s scene=", cwed_quote(parent))));
  else if (!strcmp(what, "delete")) {
    for (int i = cwed_selection_count() - 1; i >= 0; i--) {
      cw_node n = cwed_selection_get(i);
      if (cw_node_valid(n)) cwed_runf("node.delete %s", cwed_quote(cwed_doc_node_path(d, n)));
    }
  }
}

static cw_node button(const char *text, const char *tip, const char *what) {
  cw_node b = cw_node_new("Button", NULL);
  cw_button_set_text(b, text);
  cw_control_set_tooltip(b, tip);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_button, (void *)what, 0);
  return b;
}

static cw_node scrolled(cw_node content) {
  cw_node s = cw_node_new("ScrollContainer", NULL);
  cw_node_set(s, "horizontal_scroll_mode", cw_val_int(0));
  cw_control_set_size_flags(content, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(s, content);
  return s;
}

static cw_node create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node bar = cw_node_new("HBoxContainer", "Buttons");
  cw_node_add_child(bar, button("+ Node", "Add a node under the selection (node.add)", "add"));
  cw_node_add_child(bar, button("Instance", "Instance another scene under the selection (node.instance)", "instance"));
  cw_node_add_child(bar, button("Delete", "Delete the selected nodes (node.delete, Delete key)", "delete"));
  cw_node_add_child(box, bar);
  tree = cw_node_new("Tree", "Nodes");
  cw_uitree_set_columns(tree, 3);
  cw_uitree_set_column_title(tree, 0, "Node");
  cw_uitree_set_column_title(tree, 1, "Type");
  cw_uitree_set_column_title(tree, 2, "Info");
  cw_uitree_set_column_expand(tree, 0, true, 80);
  cw_uitree_set_column_expand(tree, 1, true, 70);
  cw_uitree_set_column_expand(tree, 2, true, 50);
  cw_node_set(tree, "select_mode", cw_val_int(1));
  cw_node_set(tree, "drag_items", cw_val_bool(true));
  cw_control_set_size_flags(tree, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_control_set_stretch_ratio(tree, 3);
  cw_connect(tree, "item_selected", (cw_node){ 0 }, NULL, on_select, NULL, 0);
  cw_connect(tree, "multi_selected", (cw_node){ 0 }, NULL, on_select, NULL, 0);
  cw_connect(tree, "item_activated", (cw_node){ 0 }, NULL, on_activate, NULL, 0);
  cw_ui_set_drop_target(tree, &(cw_ui_drop_fns){ .can_drop = can_drop, .drop = drop });
  cw_node_add_child(box, tree);
  hint = cw_node_new("Label", "Hint");
  cw_node_set(hint, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  cw_node_set(hint, "ellipsis", cw_val_bool(true));
  cw_node_add_child(box, hint);
  rebuild();
  return box;
}

/* the Node panel: the selected node's groups and signals */
static cw_node create_node_panel(void *user) {
  (void)user;
  tabs = cw_node_new("TabContainer", "NodeTabs");
  cw_control_set_size_flags(tabs, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  groups_box = cw_node_new("VBoxContainer", "Groups");
  signals_box = cw_node_new("VBoxContainer", "Signals");
  cw_node gs = scrolled(groups_box), ss = scrolled(signals_box);
  cw_node_set_name(gs, "Groups");
  cw_node_set_name(ss, "Signals");
  cw_node_add_child(tabs, gs);
  cw_node_add_child(tabs, ss);
  rebuild_side();
  return tabs;
}

static void init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "scene_tree", .title = "Scene", .slot = "left_top", .order = 0, .create = create });
  cwed_panel_register(&(cwed_panel_desc){ .id = "node_signals", .title = "Node", .slot = "right_top", .order = 20, .create = create_node_panel });
  cwed_subscribe(CWED_EV_DOC_CHANGED | CWED_EV_DOC_ACTIVE | CWED_EV_DOC_CLOSED | CWED_EV_SELECTION, on_event, NULL);
  static const cwed_param conn_p[] = {
    { "from", CWED_ARG_PATH, NULL, "the node that emits" },
    { "signal", CWED_ARG_STRING, NULL, "signal name (the Signals tab lists the type's)" },
    { "to", CWED_ARG_PATH, ".", "the node whose method runs" },
    { "method", CWED_ARG_STRING, NULL, "method name (a script / C method the game resolves by name)" },
    { "flags", CWED_ARG_INT, "0", "1 deferred, 2 one-shot" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "scene_tree.rows", .title = "Scene Tree Rows",
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "The Scene panel's visible rows: node path, window rect (UI units), selected.",
    .exec = rows_exec });
  const uint32_t U = CWED_CMD_UNDO | CWED_CMD_SCENE;
  cwed_command_register(&(cwed_command_desc){ .name = "node.connect", .title = "Connect Signal", .params = conn_p, .flags = U,
    .doc = "Connect a node's signal to a method of another node (saved as [connection]). The reply says whether the emitter's type "
           "declares the signal.",
    .example = "node.connect from={sel} signal=body_entered to={sel2} method=on_ball_hit", .exec = connect_exec, .undo = conn_undo });
  cwed_command_register(&(cwed_command_desc){ .name = "node.disconnect", .title = "Disconnect Signal", .params = conn_p, .flags = U,
    .doc = "Remove a signal connection (undo puts it back in the same order).",
    .example = "node.connect from={sel} signal=body_entered to={sel2} method=on_ball_hit\n"
               "node.disconnect from={sel} signal=body_entered to={sel2} method=on_ball_hit",
    .exec = disconnect_exec, .undo = conn_undo });
}

CWED_MODULE(scene_tree) = { .title = "Scene Tree", .init = init, .frame = frame };

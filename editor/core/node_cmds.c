/* node.* commands: every scene edit the editor (and an agent) can make,
 * each with do/undo and serializable text arguments. */
#include "internal.h"

/* A node this history entry may own: freed when the entry goes away while
 * the node is detached (deleted, or an undone add). */
typedef struct { cw_node node, parent; int index; } node_slot;

static void free_slot(cwed_call *c, void *state) {
  (void)c;
  node_slot *s = state;
  if (cw_node_valid(s->node) && CW_HANDLE_IS_NULL(cw_node_parent(s->node))) cw_node_free(s->node);
  cw_free(s);
}

static node_slot *slot_of(cwed_call *c) {
  node_slot *s = cwed_call_state(c);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  return s;
}

/* n belongs to an instanced sub-scene (not its root): the .cwscene format can
 * override its properties but cannot delete, rename or move it */
static bool inside_instance(cwed_doc *d, cw_node n) {
  cw_node p = cw_node_parent(n);
  return cw_node_valid(p) && p.id != d->root.id && cw_scene_node_in_instance(p, d->root);
}

static void reply_node(cwed_call *c, cw_node n) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "path");
  cwed_json_str(j, cwed_doc_node_path(cwed_call_doc(c), n));
  cwed_json_key(j, "name");
  cwed_json_str(j, cw_node_name(n));
  cwed_json_key(j, "type");
  cwed_json_str(j, cw_node_get_type(n)->name);
}

static void attach(cw_node parent, cw_node n, int index) {
  int k = cw_node_child_count(parent);
  if (index < 0 || index >= k) cw_node_add_child(parent, n);
  else cw_node_add_child_at(parent, n, index);
}

/* ── node.add / node.instance ─────────────────────────────────────────── */

static bool add_common(cwed_call *c, bool instance) {
  cwed_doc *d = cwed_call_doc(c);
  node_slot *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node)) { /* redo: the very node undo detached */
    attach(s->parent, s->node, s->index);
    reply_node(c, s->node);
    return true;
  }
  cw_node parent = cwed_arg_node(c, "parent");
  if (CW_HANDLE_IS_NULL(parent)) return false;
  cw_node n;
  if (instance) {
    const char *scene = cwed_arg(c, "scene");
    if (*d->path && !strcmp(scene, d->path)) return cwed_fail(c, "a scene cannot instance itself");
    if (!cw_vfs_exists(scene)) return cwed_fail(c, "no scene '%s' in the project (try: project.info)", scene);
    n = cw_scene_instance_linked(scene);
    if (CW_HANDLE_IS_NULL(n)) return cwed_fail(c, "%s did not load (the log says why)", scene);
    if (cwed_arg_given(c, "name")) cw_node_set_name(n, cwed_arg(c, "name"));
  } else {
    const char *type = cwed_arg(c, "type");
    if (!cw_node_type_find(type)) return cwed_fail(c, "no node type '%s' (registered types: `types` over the debug channel)", type);
    n = cw_node_new(type, cwed_arg_given(c, "name") ? cwed_arg(c, "name") : NULL);
    if (CW_HANDLE_IS_NULL(n)) return cwed_fail(c, "could not create a %s", type);
  }
  int index = (int)cwed_arg_int(c, "index");
  attach(parent, n, index);
  s = slot_of(c);
  s->node = n;
  s->parent = parent;
  s->index = cw_node_index(n);
  reply_node(c, n);
  cwed_json_printf(cwed_reply(c), ",\"uid\":%llu", (unsigned long long)cw_node_uid(n));
  return true;
}
static bool add_exec(cwed_call *c) { return add_common(c, false); }
static bool instance_exec(cwed_call *c) { return add_common(c, true); }
static void add_undo(cwed_call *c) {
  node_slot *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node) && cw_node_valid(cw_node_parent(s->node))) cw_node_remove_child(cw_node_parent(s->node), s->node);
}

/* ── node.delete ──────────────────────────────────────────────────────── */

static bool delete_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  node_slot *s = cwed_call_state(c);
  cw_node n = s && cw_node_valid(s->node) ? s->node : cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  if (n.id == d->root.id) return cwed_fail(c, "the scene root cannot be deleted (scene.new makes another scene)");
  if (inside_instance(d, n))
    return cwed_fail(c, "'%s' belongs to an instanced scene: delete the instance, or edit that scene", cwed_doc_node_path(d, n));
  reply_node(c, n);
  s = slot_of(c);
  s->node = n;
  s->parent = cw_node_parent(n);
  s->index = cw_node_index(n);
  cw_node_remove_child(s->parent, n);
  return true;
}
static void delete_undo(cwed_call *c) {
  node_slot *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node) && cw_node_valid(s->parent)) attach(s->parent, s->node, s->index);
}

/* ── node.rename ──────────────────────────────────────────────────────── */

typedef struct { cw_node node; const char *old_name; } rename_state;

static bool rename_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  const char *name = cwed_arg(c, "name");
  if (!*name || strchr(name, '/') || strchr(name, '.') || strchr(name, ':') || strchr(name, '%') || strchr(name, '"'))
    return cwed_fail(c, "'%s' is not a node name (no / . : %% or quotes, not empty)", name);
  if (inside_instance(d, n)) return cwed_fail(c, "'%s' belongs to an instanced scene and keeps that scene's name", cwed_doc_node_path(d, n));
  rename_state *s = cwed_call_state(c);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  s->node = n;
  s->old_name = cw_intern(cw_node_name(n));
  cw_node_set_name(n, name);
  reply_node(c, n);
  return true;
}
static void rename_undo(cwed_call *c) {
  rename_state *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node)) cw_node_set_name(s->node, s->old_name);
}

/* ── node.set ─────────────────────────────────────────────────────────── */

typedef struct { cw_node node; const char *prop; cw_value old; } set_state;

static bool set_exec(cwed_call *c) {
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  const char *prop = cwed_arg(c, "prop");
  const cw_prop *p = cw_node_find_prop(n, prop, NULL);
  if (p && (p->flags & CW_PROP_READONLY)) return cwed_fail(c, "%s is read only (computed by the engine)", prop);
  cw_value v, old;
  if (!cwed_arg_value(c, "value", n, prop, &v)) return false;
  if (!cw_node_get_prop(n, prop, &old)) return cwed_fail(c, "cannot read %s", prop);
  if (!cw_node_set(n, prop, v)) return cwed_fail(c, "%s refused the value (see the log)", prop);
  set_state *s = cwed_call_state(c);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  s->node = n;
  s->prop = cw_intern(prop);
  s->old = old;
  cw_value now;
  cw_node_get_prop(n, prop, &now);
  char buf[512];
  cw_value_format(&now, buf, sizeof buf);
  cwed_json_key(cwed_reply(c), "value");
  cwed_json_str(cwed_reply(c), buf);
  return true;
}
static void set_undo(cwed_call *c) {
  set_state *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node)) cw_node_set(s->node, s->prop, s->old);
}
static bool set_merge(cwed_call *prev, cwed_call *next) {
  set_state *a = cwed_call_state(prev), *b = cwed_call_state(next);
  return a && b && a->node.id == b->node.id && a->prop == b->prop; /* prev keeps the oldest value */
}

/* ── node.reparent / node.move ────────────────────────────────────────── */

static bool reparent_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  cw_node n = cwed_arg_node(c, "path");
  cw_node np = cwed_arg_node(c, "parent");
  if (CW_HANDLE_IS_NULL(n) || CW_HANDLE_IS_NULL(np)) return false;
  if (n.id == d->root.id) return cwed_fail(c, "the scene root has no parent to change");
  for (cw_node p = np; cw_node_valid(p); p = cw_node_parent(p))
    if (p.id == n.id) return cwed_fail(c, "cannot move a node under itself or its own descendant");
  if (inside_instance(d, n)) return cwed_fail(c, "'%s' belongs to an instanced scene and cannot move", cwed_doc_node_path(d, n));
  node_slot *s = slot_of(c);
  s->node = n;
  s->parent = cw_node_parent(n);
  s->index = cw_node_index(n);
  cw_node_remove_child(s->parent, n);
  attach(np, n, (int)cwed_arg_int(c, "index"));
  reply_node(c, n);
  return true;
}
static void reparent_undo(cwed_call *c) {
  node_slot *s = cwed_call_state(c);
  if (!s || !cw_node_valid(s->node)) return;
  cw_node_remove_child(cw_node_parent(s->node), s->node);
  attach(s->parent, s->node, s->index);
}
static void free_plain(cwed_call *c, void *state) {
  (void)c;
  cw_free(state); /* the node stays in the tree: never freed by this entry */
}

static bool move_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  if (n.id == d->root.id) return cwed_fail(c, "the scene root has no siblings");
  if (inside_instance(d, n)) return cwed_fail(c, "'%s' belongs to an instanced scene and cannot move", cwed_doc_node_path(d, n));
  cw_node parent = cw_node_parent(n);
  int k = cw_node_child_count(parent), to = (int)cwed_arg_int(c, "index");
  if (to < 0 || to >= k) return cwed_fail(c, "index %d is outside 0..%d", to, k - 1);
  node_slot *s = slot_of(c);
  s->node = n;
  s->parent = parent;
  s->index = cw_node_index(n);
  cw_node_move_child(parent, n, to);
  reply_node(c, n);
  cwed_json_printf(cwed_reply(c), ",\"index\":%d", cw_node_index(n));
  return true;
}
static void move_undo(cwed_call *c) {
  node_slot *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node)) cw_node_move_child(s->parent, s->node, s->index);
}

/* ── node.group ───────────────────────────────────────────────────────── */

typedef struct { cw_node node; const char *group; bool was_in; } group_state;

static bool group_exec(cwed_call *c) {
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  const char *g = cwed_arg(c, "group");
  if (!*g || strchr(g, ',') || strchr(g, '"')) return cwed_fail(c, "'%s' is not a group name (no commas or quotes)", g);
  group_state *s = cwed_call_state(c);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  s->node = n;
  s->group = cw_intern(g);
  s->was_in = cw_node_in_group(n, g);
  if (cwed_arg_bool(c, "add")) cw_node_add_group(n, g);
  else cw_node_remove_group(n, g);
  reply_node(c, n);
  cwed_json_printf(cwed_reply(c), ",\"in_group\":%s", cw_node_in_group(n, g) ? "true" : "false");
  return true;
}
static void group_undo(cwed_call *c) {
  group_state *s = cwed_call_state(c);
  if (!s || !cw_node_valid(s->node)) return;
  if (s->was_in) cw_node_add_group(s->node, s->group);
  else cw_node_remove_group(s->node, s->group);
}

/* ── queries ──────────────────────────────────────────────────────────── */

static void prop_value_text(cw_node n, const cw_prop *p, char *buf, int cap) {
  cw_value v;
  if (!cw_node_get_prop(n, p->name, &v)) {
    buf[0] = 0;
    return;
  }
  if (p->type == CW_T_ENUM && p->enum_names) {
    int k = 0;
    while (p->enum_names[k] && k < v.i) k++;
    if (p->enum_names[k] && k == v.i) {
      snprintf(buf, (size_t)cap, "%s", p->enum_names[k]);
      return;
    }
  }
  cw_value_format(&v, buf, cap);
}

static bool props_exec(cwed_call *c) {
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  const cw_prop *props[256];
  const cw_node_type *owners[256];
  int k = cw_node_props(n, props, owners, 256);
  cwed_json *j = cwed_reply(c);
  reply_node(c, n);
  cwed_json_key(j, "props");
  cwed_json_raw(j, "[");
  bool first = true;
  for (int i = 0; i < k && i < 256; i++) {
    if (!(props[i]->flags & CW_PROP_EDITOR)) continue;
    char buf[1024];
    prop_value_text(n, props[i], buf, sizeof buf);
    cwed_json_raw(j, first ? "{\"name\":" : ",{\"name\":");
    first = false;
    cwed_json_str(j, props[i]->name);
    cwed_json_printf(j, ",\"type\":\"%s\",\"owner\":\"%s\",\"value\":", cw_type_name(props[i]->type), owners[i]->name);
    cwed_json_str(j, buf);
    if (props[i]->flags & CW_PROP_READONLY) cwed_json_raw(j, ",\"readonly\":true");
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool get_exec(cwed_call *c) {
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  const cw_prop *p = cw_node_find_prop(n, cwed_arg(c, "prop"), NULL);
  if (!p) return cwed_fail(c, "%s has no property '%s' (try: node.props %s)", cw_node_name(n), cwed_arg(c, "prop"), cwed_arg(c, "path"));
  char buf[1024];
  prop_value_text(n, p, buf, sizeof buf);
  cwed_json_key(cwed_reply(c), "value");
  cwed_json_str(cwed_reply(c), buf);
  cwed_json_key(cwed_reply(c), "type");
  cwed_json_str(cwed_reply(c), cw_type_name(p->type));
  return true;
}

/* ── registration ─────────────────────────────────────────────────────── */

void cwed__node_commands_many(void);
void cwed__node_commands(void) {
  cwed__node_commands_many();
  static const cwed_param add_p[] = {
    { "type", CWED_ARG_STRING, NULL, "node type (MeshInstance3D, Sprite2D, Button, ...)" },
    { "parent", CWED_ARG_PATH, ".", "parent node path" },
    { "name", CWED_ARG_STRING, "", "name (default: the type name; siblings get unique names)" },
    { "index", CWED_ARG_INT, "-1", "position among the parent's children (-1 = last)" },
    { 0 } };
  static const cwed_param inst_p[] = {
    { "scene", CWED_ARG_STRING, NULL, "project path of the .cwscene to instance" },
    { "parent", CWED_ARG_PATH, ".", "parent node path" },
    { "name", CWED_ARG_STRING, "", "name (default: the scene root's name)" },
    { "index", CWED_ARG_INT, "-1", "position among the parent's children (-1 = last)" },
    { 0 } };
  static const cwed_param path_p[] = { { "path", CWED_ARG_PATH, NULL, "node path" }, { 0 } };
  static const cwed_param rename_p[] = {
    { "path", CWED_ARG_PATH, NULL, "node path" }, { "name", CWED_ARG_STRING, NULL, "new name" }, { 0 } };
  static const cwed_param set_p[] = {
    { "path", CWED_ARG_PATH, NULL, "node path" },
    { "prop", CWED_ARG_STRING, NULL, "property name (node.props lists them)" },
    { "value", CWED_ARG_VALUE, NULL, "value as scene files write it: vec3(0, 1, 0), color(1, 0, 0, 1), 2.5, true, SPHERE, text" },
    { 0 } };
  static const cwed_param reparent_p[] = {
    { "path", CWED_ARG_PATH, NULL, "node path" },
    { "parent", CWED_ARG_PATH, NULL, "new parent path" },
    { "index", CWED_ARG_INT, "-1", "position among the new siblings (-1 = last)" },
    { 0 } };
  static const cwed_param move_p[] = {
    { "path", CWED_ARG_PATH, NULL, "node path" }, { "index", CWED_ARG_INT, NULL, "new position among its siblings" }, { 0 } };
  static const cwed_param group_p[] = {
    { "path", CWED_ARG_PATH, NULL, "node path" },
    { "group", CWED_ARG_STRING, NULL, "group name" },
    { "add", CWED_ARG_BOOL, "true", "true adds, false removes" },
    { 0 } };
  static const cwed_param get_p[] = {
    { "path", CWED_ARG_PATH, NULL, "node path" }, { "prop", CWED_ARG_STRING, NULL, "property name" }, { 0 } };
  const uint32_t U = CWED_CMD_UNDO | CWED_CMD_SCENE;
  cwed_command_register(&(cwed_command_desc){ .name = "node.add", .title = "Add Node", .params = add_p, .flags = U,
    .shortcut = "ctrl+shift+n", .doc = "Add a new node of a type under a parent. Replies with its path and uid.",
    .example = "node.add type=MeshInstance3D parent=. name=Added", .exec = add_exec, .undo = add_undo, .free_state = free_slot });
  cwed_command_register(&(cwed_command_desc){ .name = "node.instance", .title = "Instance Scene", .params = inst_p, .flags = U,
    .doc = "Add an instance of another project scene (saved as instance=, with only the overrides).",
    .example = "node.instance scene=props/crate.cwscene parent=. name=Extra", .exec = instance_exec, .undo = add_undo,
    .free_state = free_slot });
  cwed_command_register(&(cwed_command_desc){ .name = "node.delete", .title = "Delete Node", .params = path_p, .flags = U,
    .shortcut = "delete", .doc = "Delete a node and its subtree (undo puts the same nodes back).",
    .example = "node.delete path={sel}", .exec = delete_exec, .undo = delete_undo, .free_state = free_slot });
  cwed_command_register(&(cwed_command_desc){ .name = "node.rename", .title = "Rename Node", .params = rename_p, .flags = U,
    .doc = "Rename a node (siblings keep unique names: the reply has the name it got).",
    .example = "node.rename path={sel} name=Renamed", .exec = rename_exec, .undo = rename_undo });
  cwed_command_register(&(cwed_command_desc){ .name = "node.set", .title = "Set Property", .params = set_p, .flags = U,
    .doc = "Set a property (any reflected one: node.props lists them). With merge=<key>, consecutive sets of the same "
           "property are one undo step (drags).",
    .example = "node.set path={sel} prop=visible value=false", .exec = set_exec, .undo = set_undo, .merge = set_merge });
  cwed_command_register(&(cwed_command_desc){ .name = "node.reparent", .title = "Reparent Node", .params = reparent_p, .flags = U,
    .doc = "Move a node under another parent (local transform kept).",
    .example = "node.reparent path={sel} parent={sel2}", .exec = reparent_exec, .undo = reparent_undo, .free_state = free_plain });
  cwed_command_register(&(cwed_command_desc){ .name = "node.move", .title = "Reorder Node", .params = move_p, .flags = U,
    .doc = "Move a node to another position among its siblings (draw and process order).",
    .example = "node.move path={sel} index=0", .exec = move_exec, .undo = move_undo, .free_state = free_plain });
  cwed_command_register(&(cwed_command_desc){ .name = "node.group", .title = "Node Group", .params = group_p, .flags = U,
    .doc = "Add a node to a group, or remove it (add=false).",
    .example = "node.group path={sel} group=enemies", .exec = group_exec, .undo = group_undo });
  cwed_command_register(&(cwed_command_desc){ .name = "node.props", .title = "Node Properties", .params = path_p,
    .flags = CWED_CMD_SCENE | CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Every editor-visible property of a node: name, type, owner type, value as text.", .exec = props_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "node.get", .title = "Get Property", .params = get_p,
    .flags = CWED_CMD_SCENE | CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "One property's value as text.", .exec = get_exec });
}

/* ── node.set_many (one undo step for a multi-selection edit) ─────────── */

typedef struct { int count; const char *prop; cw_node nodes[64]; cw_value old[64]; } many_state;

static bool set_many_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  const char *prop = cwed_arg(c, "prop"), *p = cwed_arg(c, "paths");
  many_state *s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
  s->prop = cw_intern(prop);
  while (*p) {
    const char *e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    char path[512];
    snprintf(path, sizeof path, "%.*s", (int)l, p);
    p += l + (e ? 1 : 0);
    if (!path[0]) continue;
    cw_node n = cwed_doc_node(d, path);
    cw_value v;
    const cw_prop *pp = CW_HANDLE_IS_NULL(n) ? NULL : cw_node_find_prop(n, prop, NULL);
    if (!pp || (pp->flags & CW_PROP_READONLY) || s->count >= 64 || !cw_node_get_prop(n, prop, &s->old[s->count])) {
      for (int i = s->count - 1; i >= 0; i--) cw_node_set(s->nodes[i], prop, s->old[i]);
      cw_free(s);
      return cwed_fail(c, "'%s' has no writable property '%s'", path, prop);
    }
    /* the value text is parsed per node: the same text means the same thing for each */
    cwed_call *cc = c;
    if (!cwed_arg_value(cc, "value", n, prop, &v) || !cw_node_set(n, prop, v)) {
      for (int i = s->count - 1; i >= 0; i--) cw_node_set(s->nodes[i], prop, s->old[i]);
      cw_free(s);
      return cwed_fail(c, "%s refused the value", path);
    }
    s->nodes[s->count++] = n;
  }
  if (!s->count) {
    cw_free(s);
    return cwed_fail(c, "no paths given");
  }
  many_state *prev = cwed_call_state(c);
  if (prev) cw_free(prev);
  cwed_call_set_state(c, s);
  cwed_json_key(cwed_reply(c), "count");
  cwed_json_printf(cwed_reply(c), "%d", s->count);
  return true;
}
static void set_many_undo(cwed_call *c) {
  many_state *s = cwed_call_state(c);
  for (int i = s ? s->count - 1 : -1; i >= 0; i--)
    if (cw_node_valid(s->nodes[i])) cw_node_set(s->nodes[i], s->prop, s->old[i]);
}
static bool set_many_merge(cwed_call *prev, cwed_call *next) {
  many_state *a = cwed_call_state(prev), *b = cwed_call_state(next);
  if (!a || !b || a->count != b->count || a->prop != b->prop) return false;
  for (int i = 0; i < a->count; i++)
    if (a->nodes[i].id != b->nodes[i].id) return false;
  return true;
}

void cwed__node_commands_many(void);
void cwed__node_commands_many(void) {
  static const cwed_param p[] = {
    { "paths", CWED_ARG_STRING, NULL, "node paths separated by commas" },
    { "prop", CWED_ARG_STRING, NULL, "property name" },
    { "value", CWED_ARG_VALUE, NULL, "value as scene files write it" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "node.set_many", .title = "Set Property on Selection", .params = p,
    .flags = CWED_CMD_UNDO | CWED_CMD_SCENE | CWED_CMD_HIDDEN,
    .doc = "Set one property on several nodes as a single undo step (the inspector with a multi-selection).",
    .example = "node.set_many paths={sel},{sel2} prop=visible value=false", .exec = set_many_exec, .undo = set_many_undo,
    .merge = set_many_merge });
}

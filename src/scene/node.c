/* Nodes, types, hierarchy, groups, the tree and its processing phases. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/plugin.h"
#include "scene_internal.h"

/* ── type registry ──────────────────────────────────────────────────── */

#define MAX_TYPES 256
static cw_node_type *types[MAX_TYPES];
static int type_count;
static cw_map type_by_name; /* interned name ptr -> index+1 */
static bool type_withdrawn[MAX_TYPES]; /* cw_node_type_withdraw: out of type_by_name, still in types[] */
static const char *type_src_file[MAX_TYPES]; /* interned; where the type was (last) registered */
static int type_src_line[MAX_TYPES];

void(cw_node_type_register)(cw_node_type *t) { cw_node_type_register_at(t, NULL, 0); }

const char *cw_node_type_source(const cw_node_type *t, int *line) {
  int i = t && t->index >= 0 && t->index < type_count && types[t->index] == t ? t->index : -1;
  if (i < 0 && t) { /* a newer descriptor of a reloaded type */
    const cw_node_type *cur = cw_node_type_find(t->name);
    i = cur ? cur->index : -1;
  }
  if (line) *line = i >= 0 ? type_src_line[i] : 0;
  return i >= 0 ? type_src_file[i] : NULL;
}

void cw_node_type_register_at(cw_node_type *t, const char *file, int line) {
  if (!type_by_name.slot_cap && !type_by_name.count) cw_map_init(&type_by_name, CW_MEM_SCENE);
  t->name = cw_intern(t->name);
  uint64_t existing;
  if (cw_map_get(&type_by_name, (uint64_t)(uintptr_t)t->name, &existing)) {
    cw_node_type *old = types[existing - 1];
    if (file) {
      type_src_file[existing - 1] = cw_intern(file);
      type_src_line[existing - 1] = line;
    }
    if (old == t) return;
    /* Hot reload: newly loaded code registers its types again. Behaviour
     * (callbacks, properties, docs) is taken from the new descriptor; the data
     * layout must not change, because live nodes hold data in the old one. */
    const char *np = t->parent ? cw_intern(t->parent) : NULL, *op = old->parent ? cw_intern(old->parent) : NULL;
    if (old->data_size != t->data_size || np != op) {
      cw_error("node type '%s' re-registered with a different layout (data %zu -> %zu bytes, parent %s -> %s): "
               "live nodes keep the old code; restart to change a type's data", t->name, old->data_size, t->data_size,
               op ? op : "-", np ? np : "-");
      return;
    }
    old->props = t->props;
    old->signals = t->signals;
    old->doc = t->doc;
    old->init = t->init;
    old->destroy = t->destroy;
    old->enter_tree = t->enter_tree;
    old->ready = t->ready;
    old->exit_tree = t->exit_tree;
    old->fixed_update = t->fixed_update;
    old->update = t->update;
    old->pre_render = t->pre_render;
    old->prop_changed = t->prop_changed;
    old->children_changed = t->children_changed;
    /* the new code reaches its data through its own descriptor
     * (cw_node_data_t(n, &my_type)): resolve it like the registered one */
    t->base = old->base;
    t->depth = old->depth;
    t->index = old->index;
    cw_info("node type '%s' reloaded", t->name);
    return;
  }
  for (int i = 0; i < type_count; i++)
    if (type_withdrawn[i] && types[i]->name == t->name) { /* a withdrawn type comes back (plugin enabled again) */
      type_withdrawn[i] = false;
      cw_map_set(&type_by_name, (uint64_t)(uintptr_t)t->name, (uint64_t)(i + 1));
      if (file) type_src_file[i] = cw_intern(file), type_src_line[i] = line;
      if (types[i] != t) cw_node_type_register_at(t, file, line); /* same name now found: the reload path swaps the code */
      return;
    }
  CW_ASSERT(type_count < MAX_TYPES, "too many node types");
  if (t->parent) {
    const cw_node_type *p = cw_node_type_find(t->parent);
    CW_ASSERT(p, "node type '%s': parent '%s' is not registered yet (register parents first)", t->name, t->parent);
    t->base = p;
    t->depth = p->depth + 1;
    CW_ASSERT(t->depth < CW__MAX_TYPE_DEPTH, "node type '%s' nests too deep", t->name);
  } else {
    t->base = NULL;
    t->depth = 0;
  }
  t->index = type_count;
  type_src_file[type_count] = file ? cw_intern(file) : NULL;
  type_src_line[type_count] = line;
  types[type_count++] = t;
  cw_map_set(&type_by_name, (uint64_t)(uintptr_t)t->name, (uint64_t)type_count);
}

const cw_node_type *cw_node_type_find(const char *name) {
  if (!name || !type_count) return NULL;
  uint64_t i;
  if (!cw_map_get(&type_by_name, (uint64_t)(uintptr_t)cw_intern(name), &i)) return NULL;
  return types[i - 1];
}
int cw_node_type_count(void) { return type_count; }
const cw_node_type *cw_node_type_at(int i) { return i >= 0 && i < type_count ? types[i] : NULL; }

bool cw_node_type_is(const cw_node_type *t, const char *ancestor) {
  const char *a = cw_intern(ancestor);
  for (; t; t = t->base)
    if (t->name == a) return true;
  return false;
}

/* ── node storage ───────────────────────────────────────────────────── */

static cw_pool node_pool;
static uint64_t next_uid = 1;
static bool pool_ready;

static void ensure_pool(void) {
  if (pool_ready) return;
  cw_pool_init(&node_pool, sizeof(node_rec), CW_MEM_SCENE);
  pool_ready = true;
}

node_rec *cw__node_rec(cw_node n) { return pool_ready ? cw_pool_get(&node_pool, n.id) : NULL; }

bool cw_node_valid(cw_node n) { return cw__node_rec(n) != NULL; }

cw_node cw_node_new(const char *type_name, const char *name) {
  ensure_pool();
  const cw_node_type *t = cw_node_type_find(type_name);
  if (!t) {
    const char *hint = type_name ? cw_plugin_type_hint(type_name) : NULL;
    if (hint) {
      cw_error("cw_node_new: unknown node type '%s': %s", type_name, hint);
      return (cw_node){ 0 };
    }
    cw_error("cw_node_new: unknown node type '%s' (register it with cw_node_type_register, or call "
             "cw_scene_register_builtin_types() for engine types)", type_name ? type_name : "(null)");
    return (cw_node){ 0 };
  }
  uint64_t id;
  node_rec *r = cw_pool_alloc(&node_pool, &id);
  cw_node n = { id };
  r->type = t;
  /* one block holds every type's data in the chain, base type first */
  const cw_node_type *chain[CW__MAX_TYPE_DEPTH];
  for (const cw_node_type *c = t; c; c = c->base) chain[c->depth] = (cw_node_type *)c;
  size_t total = 0;
  for (int d = 0; d <= t->depth; d++) {
    r->data_off[d] = (int)total;
    total += (chain[d]->data_size + 15) & ~(size_t)15;
  }
  r->data = total ? cw_calloc(1, total, CW_MEM_SCENE) : NULL;
  r->name = cw_intern_ref(name && *name ? name : t->name); /* counted: per-spawn names must not pile up */
  r->uid = next_uid++;
  r->pause = CW_PAUSE_INHERIT;
  r->proc_fixed = r->proc_frame = true;
  for (int d = 0; d <= t->depth; d++)
    if (chain[d]->init) chain[d]->init(n, r->data ? r->data + r->data_off[d] : NULL);
  return n;
}

const cw_node_type *cw_node_get_type(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r ? r->type : NULL;
}

bool cw_node_is(cw_node n, const char *type) {
  node_rec *r = cw__node_rec(n);
  return r && cw_node_type_is(r->type, type);
}

void *cw_node_data_t(cw_node n, const cw_node_type *t) {
  node_rec *r = cw__node_rec(n);
  if (!r || !t || t->depth > r->type->depth) return NULL;
  const cw_node_type *c = r->type;
  while (c && c->depth > t->depth) c = c->base;
  /* a reloaded type: new code passes its own descriptor (same interned name) */
  if (!c || (c != t && c->name != t->name) || !r->data) return NULL;
  return r->data + r->data_off[t->depth];
}

void *cw_node_data(cw_node n, const char *type) { return cw_node_data_t(n, cw_node_type_find(type)); }

const char *cw_node_name(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r ? r->name : NULL;
}

static bool sibling_has_name(node_rec *parent, const char *name, cw_node except) {
  uint64_t id;
  return name && cw_map_get(&parent->child_names, (uint64_t)(uintptr_t)name, &id) && id != except.id;
}

/* Godot-style: "Enemy" -> "Enemy2", "Enemy3"... when a sibling has the name */
static void make_unique_name(node_rec *parent, cw_node n, node_rec *r) {
  if (!parent || !sibling_has_name(parent, r->name, n)) return;
  char base[256];
  snprintf(base, sizeof base, "%s", r->name);
  size_t len = strlen(base);
  while (len && base[len - 1] >= '0' && base[len - 1] <= '9') base[--len] = 0;
  uint64_t base_key = cw_hash_str(base), k = 2;
  cw_map_get(&parent->name_suffix, base_key, &k);
  if (k < 2) k = 2;
  for (;; k++) {
    char cand[300];
    snprintf(cand, sizeof cand, "%s%llu", base, (unsigned long long)k);
    if (!sibling_has_name(parent, cw_intern_find(cand), n)) {
      const char *old = r->name;
      r->name = cw_intern_ref(cand);
      cw_intern_unref(old);
      cw_map_set(&parent->name_suffix, base_key, k + 1);
      return;
    }
  }
}

static void index_child(node_rec *parent, cw_node child, node_rec *cr) {
  if (!parent->child_names.slot_cap && !parent->child_names.count) {
    cw_map_init(&parent->child_names, CW_MEM_SCENE);
    cw_map_init(&parent->name_suffix, CW_MEM_SCENE);
  }
  cw_map_set(&parent->child_names, (uint64_t)(uintptr_t)cr->name, child.id);
}

void cw_node_set_name(cw_node n, const char *name) {
  node_rec *r = cw__node_rec(n);
  if (!r || !name || !*name) return;
  node_rec *pr = cw__node_rec(r->parent);
  if (pr) cw_map_remove(&pr->child_names, (uint64_t)(uintptr_t)r->name);
  const char *old = r->name;
  r->name = cw_intern_ref(name);
  cw_intern_unref(old);
  if (pr) {
    make_unique_name(pr, n, r);
    index_child(pr, n, r);
  }
  if (r->tree && r->tree->changes) cw__change_note(r->tree, CW__CH_RENAMED, n, -1, NULL);
}

uint64_t cw_node_uid(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r ? r->uid : 0;
}

void cw_node_set_uid(cw_node n, uint64_t uid) {
  node_rec *r = cw__node_rec(n);
  if (!r || !uid) return;
  if (r->tree) cw_map_remove(&r->tree->by_uid, r->uid);
  r->uid = uid;
  if (uid >= next_uid) next_uid = uid + 1;
  if (r->tree) cw_map_set(&r->tree->by_uid, uid, n.id);
}

cw_node cw_node_by_uid(cw_tree *t, uint64_t uid) {
  uint64_t id;
  if (t && cw_map_get(&t->by_uid, uid, &id)) return (cw_node){ id };
  return (cw_node){ 0 };
}

/* ── tree membership ────────────────────────────────────────────────── */

static void enter_tree_rec(cw_node n, cw_tree *t, int index);
static void exit_tree_rec(cw_node n);
static void ready_rec(cw_node n);

static void call_chain(node_rec *r, cw_node n, int which, double dt) {
  const cw_node_type *chain[CW__MAX_TYPE_DEPTH];
  for (const cw_node_type *c = r->type; c; c = c->base) chain[c->depth] = c;
  if (which == 2 || which == 9) { /* exit / destroy: derived first */
    for (int d = r->type->depth; d >= 0; d--) {
      void *data = r->data ? r->data + r->data_off[d] : NULL;
      if (which == 2 && chain[d]->exit_tree) chain[d]->exit_tree(n, data);
      if (which == 9 && chain[d]->destroy) chain[d]->destroy(n, data);
    }
    return;
  }
  for (int d = 0; d <= r->type->depth; d++) {
    void *data = r->data ? r->data + r->data_off[d] : NULL;
    switch (which) {
    case 1: if (chain[d]->enter_tree) chain[d]->enter_tree(n, data); break;
    case 3: if (chain[d]->ready) chain[d]->ready(n, data); break;
    default: (void)dt; break;
    }
  }
}

static void enter_tree_rec(cw_node n, cw_tree *t, int index) {
  node_rec *r = cw__node_rec(n);
  r->tree = t;
  cw_map_set(&t->by_uid, r->uid, n.id);
  if (t->changes) cw__change_note(t, CW__CH_ADDED, n, index, NULL);
  t->node_count++;
  t->order_dirty = true; t->structure_version++;
  for (int i = 0; i < r->groups.count; i++) cw__tree_group_dirty(t, r->groups.data[i]);
  call_chain(r, n, 1, 0);
  for (int i = 0; i < r->children.count; i++) enter_tree_rec(r->children.data[i], t, i);
}

static void ready_rec(cw_node n) {
  node_rec *r = cw__node_rec(n);
  if (!r) return;
  for (int i = 0; i < r->children.count; i++) ready_rec(r->children.data[i]);
  if (!r->ready_done) {
    r->ready_done = true;
    call_chain(r, n, 3, 0);
  }
}

cw_node cw__exit_root;

bool cw__exiting_with_ancestor(cw_node n, const cw_node_type *t) {
  if (n.id == cw__exit_root.id) return false;
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p)) {
    if (cw_node_data_t(p, t)) return true;
    if (p.id == cw__exit_root.id) break;
  }
  return false;
}

static void exit_subtree(cw_node n) {
  cw_node saved = cw__exit_root;
  cw__exit_root = n;
  exit_tree_rec(n);
  cw__exit_root = saved;
}

static void exit_tree_rec(cw_node n) {
  node_rec *r = cw__node_rec(n);
  for (int i = r->children.count - 1; i >= 0; i--) exit_tree_rec(r->children.data[i]);
  call_chain(r, n, 2, 0);
  cw_tree *t = r->tree;
  if (t->changes) cw__change_note(t, CW__CH_REMOVED, n, -1, NULL);
  cw_map_remove(&t->by_uid, r->uid);
  t->node_count--;
  t->order_dirty = true; t->structure_version++;
  for (int i = 0; i < r->groups.count; i++) cw__tree_group_dirty(t, r->groups.data[i]);
  r->tree = NULL;
}

/* ── hierarchy ──────────────────────────────────────────────────────── */

static bool is_ancestor(cw_node a, cw_node n) {
  for (node_rec *r = cw__node_rec(n); r; r = cw__node_rec(r->parent))
    if (r->parent.id == a.id) return true;
  return false;
}

static void children_changed(cw_node parent) {
  node_rec *r = cw__node_rec(parent);
  if (!r || r->freeing) return;
  const cw_node_type *chain[CW__MAX_TYPE_DEPTH];
  for (const cw_node_type *c = r->type; c; c = c->base) chain[c->depth] = c;
  for (int d = 0; d <= r->type->depth; d++)
    if (chain[d]->children_changed) chain[d]->children_changed(parent, r->data ? r->data + r->data_off[d] : NULL);
}

static void do_add_child(cw_node parent, cw_node child, int index) {
  node_rec *pr = cw__node_rec(parent), *cr = cw__node_rec(child);
  if (!pr || !cr) return;
  if (!CW_HANDLE_IS_NULL(cr->parent)) {
    cw_error("add_child: '%s' already has a parent ('%s'); use cw_node_reparent", cr->name, cw_node_name(cr->parent));
    return;
  }
  if (parent.id == child.id || is_ancestor(child, parent)) {
    cw_error("add_child: '%s' cannot be its own ancestor", cr->name);
    return;
  }
  if (index < 0 || index > pr->children.count) index = pr->children.count;
  cw_array_push(&pr->children, child);
  memmove(pr->children.data + index + 1, pr->children.data + index, sizeof(cw_node) * (size_t)(pr->children.count - 1 - index));
  pr->children.data[index] = child;
  cr->parent = parent;
  if (pr->child_names.count || pr->child_names.slot_cap) make_unique_name(pr, child, cr);
  index_child(pr, child, cr);
  if (pr->tree) {
    enter_tree_rec(child, pr->tree, index);
    if (pr->ready_done || pr->tree->root.id == parent.id) ready_rec(child);
  }
  children_changed(parent);
}

static void do_remove_child(cw_node parent, cw_node child) {
  node_rec *pr = cw__node_rec(parent), *cr = cw__node_rec(child);
  if (!pr || !cr || cr->parent.id != parent.id) return;
  if (cr->tree) exit_subtree(child);
  for (int i = pr->children.count - 1; i >= 0; i--)
    if (pr->children.data[i].id == child.id) {
      memmove(pr->children.data + i, pr->children.data + i + 1, sizeof(cw_node) * (size_t)(pr->children.count - i - 1));
      pr->children.count--;
      break;
    }
  cw_map_remove(&pr->child_names, (uint64_t)(uintptr_t)cr->name);
  cr->parent = (cw_node){ 0 };
  children_changed(parent);
}

bool cw__parent_freeing(cw_node n) {
  node_rec *r = cw__node_rec(n), *p = r ? cw__node_rec(r->parent) : NULL;
  return p && p->freeing;
}

/* string fields hold counted strings when a setter stored one (a label's
 * text); pinned ones and literals are untouched by the unref */
static void release_strings(node_rec *r) {
  if (!r->data) return;
  for (const cw_node_type *c = r->type; c; c = c->base)
    for (const cw_prop *p = c->props; p && p->name; p++)
      if (p->type == CW_T_STRING && !p->get && !p->set && p->offset >= 0)
        cw_intern_unref(*(const char **)(r->data + r->data_off[c->depth] + p->offset));
}

static void do_free(cw_node n) {
  node_rec *r = cw__node_rec(n);
  if (!r) return;
  r->freeing = true;
  node_rec *pr = cw__node_rec(r->parent);
  if (pr && pr->freeing) {
    /* the parent goes too: leave its children array alone (it is freed whole);
     * the parent link stays so destroy callbacks can see the parent is going */
    pr->children.count--;
  } else if (!CW_HANDLE_IS_NULL(r->parent)) {
    do_remove_child(r->parent, n);
  } else if (r->tree) {
    exit_subtree(n);
  }
  while (r->children.count) {
    cw_node c = r->children.data[r->children.count - 1];
    do_free(c);
    r = cw__node_rec(n);
  }
  cw_map_free(&r->child_names);
  cw__signals_node_freed(n);
  call_chain(r, n, 9, 0);
  cw_array_free(&r->children);
  cw_array_free(&r->groups);
  cw_map_free(&r->child_names);
  cw_map_free(&r->name_suffix);
  release_strings(r);
  cw_intern_unref(r->name);
  cw_free(r->data);
  cw_pool_release(&node_pool, n.id);
}

/* Structural changes while a tree is being processed are queued. */
static bool defer(cw_node n, cw_node other, int op, int index) {
  node_rec *r = cw__node_rec(n);
  cw_tree *t = r ? r->tree : NULL;
  if (!t) {
    node_rec *o = cw__node_rec(other);
    t = o ? o->tree : NULL;
  }
  if (!t || !t->processing) return false;
  cw__deferred_op op_rec = { op, n, other, index };
  cw_array_push(&t->deferred_ops, op_rec);
  return true;
}

void cw_node_add_child_at(cw_node parent, cw_node child, int index) {
  if (defer(parent, child, CW__OP_ADD, index)) return;
  do_add_child(parent, child, index);
}
void cw_node_add_child(cw_node parent, cw_node child) { cw_node_add_child_at(parent, child, -1); }
void cw_node_remove_child(cw_node parent, cw_node child) {
  if (defer(parent, child, CW__OP_REMOVE, 0)) return;
  do_remove_child(parent, child);
}
void cw_node_free(cw_node n) {
  node_rec *r = cw__node_rec(n);
  if (!r || r->queued_free) return;
  if (defer(n, (cw_node){ 0 }, CW__OP_FREE, 0)) {
    r->queued_free = true;
    return;
  }
  do_free(n);
}
void cw_node_reparent(cw_node n, cw_node new_parent) {
  node_rec *r = cw__node_rec(n);
  if (!r) return;
  if (defer(n, new_parent, CW__OP_REPARENT, 0)) return;
  if (!CW_HANDLE_IS_NULL(r->parent)) do_remove_child(r->parent, n);
  do_add_child(new_parent, n, -1);
}
void cw_node_move_child(cw_node parent, cw_node child, int to) {
  node_rec *pr = cw__node_rec(parent);
  if (!pr) return;
  if (defer(parent, child, CW__OP_MOVE, to)) return;
  int from = -1;
  for (int i = 0; i < pr->children.count; i++)
    if (pr->children.data[i].id == child.id) from = i;
  if (from < 0) return;
  if (to < 0 || to >= pr->children.count) to = pr->children.count - 1;
  cw_node c = pr->children.data[from];
  if (from < to) memmove(pr->children.data + from, pr->children.data + from + 1, sizeof(cw_node) * (size_t)(to - from));
  else memmove(pr->children.data + to + 1, pr->children.data + to, sizeof(cw_node) * (size_t)(from - to));
  pr->children.data[to] = c;
  if (pr->tree) {
    pr->tree->order_dirty = true;
    pr->tree->structure_version++;
    if (pr->tree->changes) cw__change_note(pr->tree, CW__CH_MOVED, c, to, NULL);
  }
  children_changed(parent);
}

void cw__tree_apply_deferred(cw_tree *t) {
  for (int k = 0; k < t->deferred_ops.count; k++) { /* ops may queue more ops: count is live */
    cw__deferred_op op = t->deferred_ops.data[k];
    switch (op.op) {
    case CW__OP_ADD: do_add_child(op.a, op.b, op.index); break;
    case CW__OP_REMOVE: do_remove_child(op.a, op.b); break;
    case CW__OP_FREE: do_free(op.a); break;
    case CW__OP_REPARENT: {
      node_rec *r = cw__node_rec(op.a);
      if (!r) break;
      if (!CW_HANDLE_IS_NULL(r->parent)) do_remove_child(r->parent, op.a);
      do_add_child(op.b, op.a, -1);
      break;
    }
    case CW__OP_MOVE: cw_node_move_child(op.a, op.b, op.index); break;
    }
  }
  cw_array_clear(&t->deferred_ops);
}

cw_node cw_node_parent(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r ? r->parent : (cw_node){ 0 };
}
int cw_node_child_count(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r ? r->children.count : 0;
}
cw_node cw_node_child(cw_node n, int i) {
  node_rec *r = cw__node_rec(n);
  return r && i >= 0 && i < r->children.count ? r->children.data[i] : (cw_node){ 0 };
}
int cw_node_index(cw_node n) {
  node_rec *r = cw__node_rec(n), *p = r ? cw__node_rec(r->parent) : NULL;
  if (!p) return -1;
  for (int i = 0; i < p->children.count; i++)
    if (p->children.data[i].id == n.id) return i;
  return -1;
}
cw_tree *cw_node_tree(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r ? r->tree : NULL;
}
bool cw_node_in_tree(cw_node n) { return cw_node_tree(n) != NULL; }

cw_node cw_node_get(cw_node from, const char *path) {
  node_rec *r = cw__node_rec(from);
  if (!path) return (cw_node){ 0 };
  cw_node cur = from;
  const char *p = path;
  if (*p == '/') {
    cw_tree *t = r ? r->tree : cw_current_tree();
    if (!t) return (cw_node){ 0 };
    cur = t->root;
    p++;
    /* "/root" names the root itself */
    if (!strncmp(p, "root", 4) && (p[4] == '/' || p[4] == 0)) p += 4 + (p[4] == '/');
  }
  if (*p == '%') { /* scene-unique name: search the subtree of the scene root (owner) */
    const char *name = cw_intern_find(p + 1); /* no node has a name nothing interned */
    cw_node found = { 0 };
    if (!name) return found;
    cw_tree *t = r ? r->tree : NULL;
    cw_node root = t ? t->root : from;
    cw__find_by_name(root, name, &found);
    return found;
  }
  while (*p) {
    const char *e = strchr(p, '/');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == 0) { p++; continue; }
    if (len == 1 && p[0] == '.') {
    } else if (len == 2 && p[0] == '.' && p[1] == '.') {
      cur = cw_node_parent(cur);
    } else {
      const char *name = cw_intern_find_n(p, len);
      node_rec *cr = cw__node_rec(cur);
      uint64_t id = 0;
      cur = name && cr && cw_map_get(&cr->child_names, (uint64_t)(uintptr_t)name, &id) ? (cw_node){ id } : (cw_node){ 0 };
    }
    if (CW_HANDLE_IS_NULL(cur)) return cur;
    p += len;
    if (*p == '/') p++;
  }
  return cur;
}

bool cw__find_by_name(cw_node n, const char *interned, cw_node *out) {
  node_rec *r = cw__node_rec(n);
  if (!r) return false;
  if (r->name == interned) { *out = n; return true; }
  for (int i = 0; i < r->children.count; i++)
    if (cw__find_by_name(r->children.data[i], interned, out)) return true;
  return false;
}

int cw_node_path(cw_node n, cw_node from, char *buf, int cap) {
  /* collect ancestors up to `from` (or the root) */
  cw_node chain[256];
  int depth = 0;
  cw_node cur = n;
  while (!CW_HANDLE_IS_NULL(cur) && cur.id != from.id && depth < 256) {
    chain[depth++] = cur;
    cur = cw_node_parent(cur);
  }
  int len = 0;
  buf[0] = 0;
  bool absolute = CW_HANDLE_IS_NULL(from);
  if (absolute) {
    len += snprintf(buf + len, (size_t)(cap > len ? cap - len : 0), "/root");
    depth--; /* the last collected node is the root itself */
  } else if (cur.id != from.id) {
    return -1; /* n is not under from */
  }
  for (int i = depth - 1; i >= 0; i--)
    len += snprintf(buf + len, (size_t)(cap > len ? cap - len : 0), "%s%s", (len || absolute) ? "/" : "", cw_node_name(chain[i]));
  return len;
}

static bool walk_rec(cw_node n, bool (*fn)(cw_node, void *), void *user) {
  node_rec *r = cw__node_rec(n);
  if (!r) return true;
  if (!fn(n, user)) return true;
  for (int i = 0; i < r->children.count; i++) walk_rec(r->children.data[i], fn, user);
  return true;
}
void cw_node_walk(cw_node root, bool (*fn)(cw_node, void *), void *user) { walk_rec(root, fn, user); }

void cw_node_set_process(cw_node n, bool fixed, bool frame) {
  node_rec *r = cw__node_rec(n);
  if (!r) return;
  r->proc_fixed = fixed;
  r->proc_frame = frame;
}
void cw_node_set_pause_mode(cw_node n, cw_pause_mode m) {
  node_rec *r = cw__node_rec(n);
  if (r) r->pause = m;
}
void cw_node_set_user(cw_node n, void *u) {
  node_rec *r = cw__node_rec(n);
  if (r) r->user = u;
}
void *cw_node_user(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r ? r->user : NULL;
}

/* ── groups ─────────────────────────────────────────────────────────── */

void cw_node_add_group(cw_node n, const char *g) {
  node_rec *r = cw__node_rec(n);
  if (!r || cw_node_in_group(n, g)) return;
  cw_array_push(&r->groups, cw_intern(g));
  if (r->tree) cw__tree_group_dirty(r->tree, cw_intern(g));
}
void cw_node_remove_group(cw_node n, const char *g) {
  node_rec *r = cw__node_rec(n);
  if (!r) return;
  const char *in = cw_intern(g);
  for (int i = 0; i < r->groups.count; i++)
    if (r->groups.data[i] == in) {
      memmove(r->groups.data + i, r->groups.data + i + 1, sizeof(char *) * (size_t)(r->groups.count - i - 1));
      r->groups.count--;
      if (r->tree) cw__tree_group_dirty(r->tree, in);
      return;
    }
}
bool cw_node_in_group(cw_node n, const char *g) {
  node_rec *r = cw__node_rec(n);
  if (!r) return false;
  const char *in = cw_intern(g);
  for (int i = 0; i < r->groups.count; i++)
    if (r->groups.data[i] == in) return true;
  return false;
}

/* ── the change log (debug channel: changes <revision>) ─────────────────
 * Off until a client first asks (cw__changes_enable), so a game nobody
 * inspects pays nothing. A ring: a client too far behind resyncs (tree). */

cw__changes *cw__changes_enable(cw_tree *t) {
  if (!t->changes) {
    t->changes = cw_calloc(1, sizeof *t->changes, CW_MEM_SCENE);
    t->changes->rev = 1;
  }
  return t->changes;
}

void cw__change_note(cw_tree *t, int kind, cw_node n, int index, const char *prop) {
  cw__changes *c = t->changes;
  node_rec *r = cw__node_rec(n);
  if (!c || !r) return;
  cw__change *e = &c->ring[c->rev % CW__CHANGE_RING];
  e->rev = c->rev++;
  e->kind = (uint8_t)kind;
  e->uid = r->uid;
  node_rec *pr = cw__node_rec(r->parent);
  e->parent = pr ? pr->uid : 0;
  e->index = index;
  cw_intern_unref(e->name); /* the entry this one overwrites */
  e->name = cw_intern_ref(r->name);
  e->type = r->type->name;
  e->prop = prop ? cw_intern(prop) : NULL;
}

/* ── the tree ───────────────────────────────────────────────────────── */

static cw_tree *current;
cw_tree *cw_current_tree(void) { return current; }
void cw_set_current_tree(cw_tree *t) { current = t; }

static cw_node_type root_type_dummy; /* unused: the root is a plain Node */

cw_tree *cw_tree_new(void) {
  cw_scene_register_builtin_types();
  (void)root_type_dummy;
  cw_tree *t = cw_calloc(1, sizeof *t, CW_MEM_SCENE);
  cw_map_init(&t->by_uid, CW_MEM_SCENE);
  cw_map_init(&t->slots, CW_MEM_SCENE);
  cw_map_init(&t->groups, CW_MEM_SCENE);
  cw_rng_seed(&t->rng, 0x5eed);
  t->root = cw_node_new("Node", "root");
  node_rec *r = cw__node_rec(t->root);
  enter_tree_rec(t->root, t, 0);
  r->ready_done = true;
  if (!current) current = t;
  return t;
}

void cw_tree_free(cw_tree *t) {
  if (!t) return;
  do_free(t->root);
  for (int i = 0; i < t->slots.count; i++) {
    if (!cw_map_live(&t->slots, i)) continue;
    tree_slot *s = (tree_slot *)(uintptr_t)t->slots.vals[i];
    if (s->free_fn) s->free_fn(s->value);
    cw_free(s);
  }
  for (int i = 0; i < t->groups.count; i++) {
    if (!cw_map_live(&t->groups, i)) continue;
    group_cache *g = (group_cache *)(uintptr_t)t->groups.vals[i];
    cw_array_free(&g->nodes);
    cw_free(g);
  }
  cw_map_free(&t->slots);
  cw_map_free(&t->by_uid);
  cw_map_free(&t->groups);
  cw_array_free(&t->order);
  cw_array_free(&t->deferred_ops);
  cw_array_free(&t->deferred_signals);
  for (int p = 0; p < 3; p++) cw_array_free(&t->hooks[p]);
  if (t->changes)
    for (int i = 0; i < CW__CHANGE_RING; i++) cw_intern_unref(t->changes->ring[i].name);
  cw_free(t->changes);
  if (current == t) current = NULL;
  cw_free(t);
}

/* a NULL tree (no current tree yet) answers like an empty one, as
 * cw_tree_slot and cw_tree_free always did */
cw_node cw_tree_root(cw_tree *t) { return t ? t->root : (cw_node){ 0 }; }
int cw_tree_node_count(cw_tree *t) { return t ? t->node_count : 0; }
uint64_t cw_tree_tick(cw_tree *t) { return t ? t->tick : 0; }
cw_rng *cw_tree_rng(cw_tree *t) { return t ? &t->rng : NULL; }
void cw_tree_seed(cw_tree *t, uint64_t seed) {
  if (t) cw_rng_seed(&t->rng, seed);
}
void cw_tree_set_paused(cw_tree *t, bool p) {
  if (t) t->paused = p;
}
bool cw_tree_paused(cw_tree *t) { return t && t->paused; }

void *cw_tree_slot(cw_tree *t, const char *key) {
  uint64_t v;
  if (!t) return NULL; /* a node outside any tree has no per-tree state */
  if (!cw_map_get(&t->slots, (uint64_t)(uintptr_t)cw_intern(key), &v)) return NULL;
  return ((tree_slot *)(uintptr_t)v)->value;
}
void cw_tree_set_slot(cw_tree *t, const char *key, void *value, void (*free_fn)(void *)) {
  if (!t) return;
  uint64_t k = (uint64_t)(uintptr_t)cw_intern(key), v;
  tree_slot *s;
  if (cw_map_get(&t->slots, k, &v)) {
    s = (tree_slot *)(uintptr_t)v;
    if (s->free_fn && s->value != value) s->free_fn(s->value);
  } else {
    s = cw_calloc(1, sizeof *s, CW_MEM_SCENE);
    cw_map_set(&t->slots, k, (uint64_t)(uintptr_t)s);
  }
  s->value = value;
  s->free_fn = free_fn;
}

void cw_tree_add_hook(cw_tree *t, int phase, int order, cw_tree_hook fn, void *user) {
  if (!t) return;
  CW_ASSERT(phase >= 0 && phase < 3, "hook phase is 0 (fixed), 1 (update) or 2 (pre_render)");
  tree_hook h = { order, fn, user };
  cw_array_push(&t->hooks[phase], h);
  /* stable insertion sort by order */
  for (int i = t->hooks[phase].count - 1; i > 0 && t->hooks[phase].data[i - 1].order > h.order; i--) {
    t->hooks[phase].data[i] = t->hooks[phase].data[i - 1];
    t->hooks[phase].data[i - 1] = h;
  }
}

static void build_order_rec(cw_tree *t, cw_node n) {
  node_rec *r = cw__node_rec(n);
  cw_array_push(&t->order, n);
  for (int i = 0; i < r->children.count; i++) build_order_rec(t, r->children.data[i]);
}

void cw__tree_order(cw_tree *t) {
  if (!t->order_dirty) return;
  cw_array_clear(&t->order);
  build_order_rec(t, t->root);
  t->order_dirty = false;
}

static bool effective_pause_allows(cw_tree *t, node_rec *r) {
  cw_pause_mode m = r->pause;
  for (node_rec *p = r; m == CW_PAUSE_INHERIT;) {
    p = cw__node_rec(p->parent);
    if (!p) { m = CW_PAUSE_PAUSABLE; break; }
    m = p->pause;
  }
  switch (m) {
  case CW_PAUSE_DISABLED: return false;
  case CW_PAUSE_ALWAYS: return true;
  case CW_PAUSE_WHEN_PAUSED: return t->paused;
  default: return !t->paused;
  }
}

bool cw_node_can_process(cw_node n) {
  node_rec *r = cw__node_rec(n);
  return r && r->tree && effective_pause_allows(r->tree, r);
}

static void run_hooks(cw_tree *t, int phase, bool before, double dt) {
  for (int i = 0; i < t->hooks[phase].count; i++) {
    tree_hook *h = &t->hooks[phase].data[i];
    if ((h->order < 0) == before) h->fn(t, dt, h->user);
  }
}

static void run_phase(cw_tree *t, int phase, double dt) {
  run_hooks(t, phase, true, dt);
  cw__tree_order(t);
  t->processing = true;
  int n = t->order.count;
  for (int i = 0; i < n; i++) {
    cw_node node = t->order.data[i];
    node_rec *r = cw__node_rec(node);
    if (!r || r->queued_free || r->tree != t) continue;
    if (phase == 0 && !r->proc_fixed) continue;
    if (phase == 1 && !r->proc_frame) continue;
    if (phase != 2 && !effective_pause_allows(t, r)) continue;
    const cw_node_type *chain[CW__MAX_TYPE_DEPTH];
    for (const cw_node_type *c = r->type; c; c = c->base) chain[c->depth] = c;
    for (int d = 0; d <= r->type->depth; d++) {
      void *data = r->data ? r->data + r->data_off[d] : NULL;
      if (phase == 0 && chain[d]->fixed_update) chain[d]->fixed_update(node, data, dt);
      else if (phase == 1 && chain[d]->update) chain[d]->update(node, data, dt);
      else if (phase == 2 && chain[d]->pre_render) chain[d]->pre_render(node, data, dt);
      r = cw__node_rec(node); /* a callback may grow the pool: re-fetch */
      if (!r) break;
    }
  }
  t->processing = false;
  cw__tree_apply_deferred(t);
  cw__signals_flush(t);
  cw__tree_apply_deferred(t);
  run_hooks(t, phase, false, dt);
}

void cw_tree_fixed_update(cw_tree *t, double dt) {
  if (!t) return;
  run_phase(t, 0, dt);
  if (!t->paused) t->tick++;
}
void cw_tree_update(cw_tree *t, double dt) {
  if (t) run_phase(t, 1, dt);
}
void cw_tree_pre_render(cw_tree *t, double alpha) {
  if (t) run_phase(t, 2, alpha);
}

/* ── group caches (tree order) ───────────────────────────────────────── */

void cw__tree_group_dirty(cw_tree *t, const char *g) {
  uint64_t v;
  if (cw_map_get(&t->groups, (uint64_t)(uintptr_t)g, &v)) ((group_cache *)(uintptr_t)v)->dirty = true;
}

static cw_tree *collect_tree;
static const char *collect_group;
static group_cache *collect_cache;
static bool collect_fn(cw_node n, void *u) {
  (void)u;
  if (cw_node_in_group(n, collect_group)) cw_array_push(&collect_cache->nodes, n);
  return true;
}

int cw_tree_group(cw_tree *t, const char *g, cw_node **out) {
  if (!t) {
    if (out) *out = NULL;
    return 0;
  }
  const char *in = cw_intern(g);
  uint64_t v;
  group_cache *gc;
  if (cw_map_get(&t->groups, (uint64_t)(uintptr_t)in, &v)) gc = (group_cache *)(uintptr_t)v;
  else {
    gc = cw_calloc(1, sizeof *gc, CW_MEM_SCENE);
    gc->dirty = true;
    cw_map_set(&t->groups, (uint64_t)(uintptr_t)in, (uint64_t)(uintptr_t)gc);
  }
  if (gc->dirty || gc->version != t->structure_version) {
    gc->version = t->structure_version;
    cw_array_clear(&gc->nodes);
    collect_tree = t;
    collect_group = in;
    collect_cache = gc;
    cw_node_walk(t->root, collect_fn, NULL);
    gc->dirty = false;
  }
  cw_node *copy = cw_frame_alloc(sizeof(cw_node) * (size_t)(gc->nodes.count + 1));
  if (gc->nodes.count) memcpy(copy, gc->nodes.data, sizeof(cw_node) * (size_t)gc->nodes.count);
  *out = copy;
  return gc->nodes.count;
}

/* ── withdrawing types (plugins) ────────────────────────────────────── */

int cw_node_type_live_count(const cw_node_type *t, cw_node *first) {
  if (first) *first = (cw_node){ 0 };
  if (!t || !pool_ready) return 0;
  int n = 0;
  for (int i = 0; i < node_pool.slot_count; i++) {
    node_rec *r = cw_pool_at(&node_pool, i);
    if (!r) continue;
    for (const cw_node_type *x = r->type; x; x = x->base)
      if (x == t) {
        if (!n && first) *first = (cw_node){ cw_pool_id_of(&node_pool, r) };
        n++;
        break;
      }
  }
  return n;
}

bool cw_node_type_withdraw(const char *name) {
  const cw_node_type *t = cw_node_type_find(name);
  if (!t) return false;
  if (cw_node_type_live_count(t, NULL)) return false;
  type_withdrawn[t->index] = true;
  cw_map_remove(&type_by_name, (uint64_t)(uintptr_t)t->name);
  return true;
}

bool cw_node_type_withdrawn(const cw_node_type *t) { return t && t->index >= 0 && t->index < type_count && types[t->index] == t && type_withdrawn[t->index]; }

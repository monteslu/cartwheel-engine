/* Tree snapshots through reflection: what rollback restores and what the
 * desync check hashes. See cartwheel/net.h ("tree snapshots"). */
#include <stdio.h>
#include <stdlib.h>

#include "net_internal.h"
#include "cartwheel/core.h"
#include "scene/scene_internal.h"

typedef struct st_node {
  uint64_t uid, parent_uid;
  int32_t index, parent_rec;
  const cw_node_type *type;
  const char *name;
  int first_val, nvals;
  int blob_off, blob_len;
} st_node;

struct cw_state {
  CW_ARRAY(st_node) nodes;
  CW_ARRAY(cw_value) vals;
  CW_ARRAY(uint8_t) blob;
  cw_map by_uid; /* uid -> record index + 1 */
  uint64_t tick;
  cw_rng rng;
  bool valid;
};

/* ── which properties a type snapshots (cached per type) ─────────────── */
typedef struct plan {
  int n;
  const cw_prop **props;
  const cw_node_type **owners;
  int nhooks;
  const cw_node_type *hook_types[CW__MAX_TYPE_DEPTH];
  cw_state_save_fn hook_save[CW__MAX_TYPE_DEPTH];
  cw_state_load_fn hook_load[CW__MAX_TYPE_DEPTH];
  int hook_nohash[CW__MAX_TYPE_DEPTH];
  uint32_t gen;
} plan;
static cw_map plans;   /* type ptr -> plan* */
static cw_map skips;   /* hash(type name, prop name) -> 1 */
static cw_map hooks;   /* interned type name -> index into hook_tab */
/* nohash: trailing bytes of the private state that are process-local (a
 * library handle) and stay out of the hash */
static struct { cw_state_save_fn save; cw_state_load_fn load; int nohash; } hook_tab[64];
static int hook_count;
static uint32_t config_gen = 1;
static bool maps_ready;
static uint64_t skip_key(const char *type, const char *prop);
static void maps_init(void) {
  if (maps_ready) return;
  maps_ready = true;
  cw_map_init(&plans, CW_MEM_CORE);
  cw_map_init(&skips, CW_MEM_CORE);
  cw_map_init(&hooks, CW_MEM_CORE);
  /* the name is structure (restored with the node), not a value */
  cw_map_set(&skips, skip_key("Node", "name"), 1);
  /* the physics layer's state beside the properties (scene_internal.h) */
  hook_tab[0].save = cw__char2d_state_save;
  hook_tab[0].load = cw__char2d_state_load;
  hook_tab[1].save = cw__char3d_state_save;
  hook_tab[1].load = cw__char3d_state_load;
  hook_tab[0].nohash = hook_tab[1].nohash = CW__CHAR_STATE_NOHASH;
  cw_map_set(&hooks, (uint64_t)(uintptr_t)cw_intern("CharacterBody2D"), 0);
  cw_map_set(&hooks, (uint64_t)(uintptr_t)cw_intern("CharacterBody3D"), 1);
  hook_count = 2;
}
static uint64_t skip_key(const char *type, const char *prop) { return cw_hash_str(type) * 31 + cw_hash_str(prop); }

void cw_state_skip_prop(const char *type, const char *prop) {
  maps_init();
  cw_map_set(&skips, skip_key(type, prop), 1);
  config_gen++;
}
void cw_state_type_hook(const char *type, cw_state_save_fn save, cw_state_load_fn load) {
  maps_init();
  CW_ASSERT(hook_count < (int)CW_ARRAY_LEN(hook_tab), "too many cw_state_type_hook types (64)");
  uint64_t key = (uint64_t)(uintptr_t)cw_intern(type), idx;
  if (!cw_map_get(&hooks, key, &idx)) {
    idx = (uint64_t)hook_count++;
    cw_map_set(&hooks, key, idx);
  }
  hook_tab[idx].save = save;
  hook_tab[idx].load = load;
  config_gen++;
}

static const plan *plan_of_type(const cw_node_type *t) {
  maps_init();
  uint64_t v;
  plan *p = NULL;
  if (cw_map_get(&plans, (uint64_t)(uintptr_t)t, &v)) {
    p = (plan *)(uintptr_t)v;
    if (p->gen == config_gen) return p;
    cw_free(p->props);
    cw_free(p->owners);
  } else {
    p = cw_calloc(1, sizeof *p, CW_MEM_CORE);
    cw_map_set(&plans, (uint64_t)(uintptr_t)t, (uint64_t)(uintptr_t)p);
  }
  const cw_node_type *chain[CW__MAX_TYPE_DEPTH];
  for (const cw_node_type *c = t; c; c = c->base) chain[c->depth] = c;
  int total = 0;
  for (int d = 0; d <= t->depth; d++)
    for (const cw_prop *pr = chain[d]->props; pr && pr->name; pr++) total++;
  const cw_prop **all = cw_alloc(sizeof *all * (size_t)CW_MAX(total, 1), CW_MEM_CORE);
  const cw_node_type **own = cw_alloc(sizeof *own * (size_t)CW_MAX(total, 1), CW_MEM_CORE);
  total = 0;
  for (int d = 0; d <= t->depth; d++)
    for (const cw_prop *pr = chain[d]->props; pr && pr->name; pr++) {
      all[total] = pr;
      own[total++] = chain[d];
    }
  int k = 0;
  for (int i = 0; i < total; i++) {
    const cw_prop *pr = all[i];
    if (pr->flags & CW_PROP_READONLY || pr->type == CW_T_NONE || pr->type >= CW_T_COUNT) continue;
    if (pr->offset < 0 && !(pr->get && pr->set)) continue;
    if (cw_map_get(&skips, skip_key(own[i]->name, pr->name), &v)) continue;
    all[k] = pr;
    own[k] = own[i];
    k++;
  }
  p->n = k;
  p->props = all;
  p->owners = own;
  p->nhooks = 0;
  for (int d = 0; d <= t->depth; d++)
    if (cw_map_get(&hooks, (uint64_t)(uintptr_t)cw_intern(chain[d]->name), &v)) {
      p->hook_types[p->nhooks] = chain[d];
      p->hook_save[p->nhooks] = hook_tab[v].save;
      p->hook_load[p->nhooks] = hook_tab[v].load;
      p->hook_nohash[p->nhooks] = hook_tab[v].nohash;
      p->nhooks++;
    }
  p->gen = config_gen;
  return p;
}
static const plan *plan_of(cw_node n) { return plan_of_type(cw_node_get_type(n)); }

/* ── save ──────────────────────────────────────────────────────────── */

cw_state *cw_state_new(void) {
  cw_state *st = cw_calloc(1, sizeof *st, CW_MEM_CORE);
  cw_map_init(&st->by_uid, CW_MEM_CORE);
  return st;
}
/* records hold a reference on their node's name: a rolled-back node is
 * recreated by name after the live one was freed */
static void drop_names(cw_state *st) {
  for (int i = 0; i < st->nodes.count; i++) cw_intern_unref(st->nodes.data[i].name);
}
void cw_state_free(cw_state *st) {
  if (!st) return;
  drop_names(st);
  cw_array_free(&st->nodes);
  cw_array_free(&st->vals);
  cw_array_free(&st->blob);
  cw_map_free(&st->by_uid);
  cw_free(st);
}
size_t cw_state_bytes(const cw_state *st) {
  return (size_t)st->nodes.cap * sizeof(st_node) + (size_t)st->vals.cap * sizeof(cw_value) + (size_t)st->blob.cap;
}
int cw_state_node_count(const cw_state *st) { return st->nodes.count; }

static void save_node(cw_state *st, cw_node n, int parent_rec, int index) {
  node_rec *r = cw__node_rec(n);
  const plan *p = plan_of(n);
  st_node rec = { r->uid, parent_rec >= 0 ? st->nodes.data[parent_rec].uid : 0, index, parent_rec, r->type, cw_intern_ref(r->name),
                  st->vals.count, p->n, st->blob.count, 0 };
  cw_array_reserve(&st->vals, st->vals.count + p->n);
  for (int i = 0; i < p->n; i++) {
    cw_value v;
    memset(&v, 0, sizeof v);
    cw__prop_read(n, p->owners[i], p->props[i], &v);
    if (v.type == CW_T_NODE) { /* remapped by uid at load (the node may be recreated) */
      uint64_t uid = cw_node_uid(v.node);
      v.node = (cw_node){ 0 };
      v.res = uid;
    }
    st->vals.data[st->vals.count++] = v;
  }
  for (int h = 0; h < p->nhooks; h++) {
    if (!p->hook_save[h]) continue;
    cw_array_reserve(&st->blob, st->blob.count + 1024 + 4);
    uint8_t *at = st->blob.data + st->blob.count;
    int len = p->hook_save[h](n, cw_node_data_t(n, p->hook_types[h]), at + 4, 1024);
    len = CW_CLAMP(len, 0, 1024);
    memcpy(at, &len, 4);
    st->blob.count += 4 + len;
  }
  rec.blob_len = st->blob.count - rec.blob_off;
  int me = st->nodes.count;
  cw_array_push(&st->nodes, rec);
  cw_map_set(&st->by_uid, r->uid, (uint64_t)me + 1);
  for (int i = 0; i < r->children.count; i++) save_node(st, r->children.data[i], me, i);
}

void cw_state_save(cw_state *st, cw_tree *t) {
  CW_ASSERT(!t->processing, "cw_state_save inside a tree phase: save between ticks");
  cw__tree_apply_deferred(t);
  drop_names(st);
  st->nodes.count = st->vals.count = st->blob.count = 0;
  cw_map_free(&st->by_uid);
  cw_map_init(&st->by_uid, CW_MEM_CORE);
  save_node(st, t->root, -1, 0);
  st->tick = t->tick;
  st->rng = t->rng;
  st->valid = true;
}

/* ── load ──────────────────────────────────────────────────────────── */

static bool value_eq(const cw_value *a, const cw_value *b) {
  if (a->type != b->type) return false;
  switch (a->type) {
  case CW_T_BOOL: return a->b == b->b;
  case CW_T_INT: case CW_T_ENUM: return a->i == b->i;
  case CW_T_FLOAT: return !memcmp(&a->f, &b->f, sizeof a->f);
  case CW_T_VEC2: return !memcmp(a->v, b->v, 8);
  case CW_T_VEC3: return !memcmp(a->v, b->v, 12);
  case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR: return !memcmp(a->v, b->v, 16);
  case CW_T_STRING: return a->s == b->s || (a->s && b->s && !strcmp(a->s, b->s));
  case CW_T_NODE: return a->node.id == b->node.id;
  case CW_T_RES: return a->res == b->res;
  default: return true;
  }
}

typedef struct { const cw_state *st; CW_ARRAY(cw_node) doomed; } free_walk;
static bool collect_doomed(cw_node n, void *u) {
  free_walk *fw = u;
  uint64_t v;
  if (cw_map_get(&fw->st->by_uid, cw_node_uid(n), &v)) return true;
  cw_array_push(&fw->doomed, n);
  return false; /* its subtree goes with it */
}

void cw_state_load(const cw_state *st, cw_tree *t) {
  CW_ASSERT(st->valid, "cw_state_load of a state never saved");
  CW_ASSERT(!t->processing, "cw_state_load inside a tree phase: restore between ticks");
  CW_ASSERT(st->nodes.count && st->nodes.data[0].uid == cw_node_uid(t->root),
            "cw_state_load into a different tree than the one saved");
  cw__tree_apply_deferred(t);
  /* 1. nodes the snapshot does not have */
  free_walk fw = { st, { 0 } };
  cw_node_walk(t->root, collect_doomed, &fw);
  for (int i = 0; i < fw.doomed.count; i++) cw_node_free(fw.doomed.data[i]);
  cw_array_free(&fw.doomed);
  /* 2. nodes the tree lost, parents first (pre-order); parents and names */
  cw_node *nodes = cw_frame_alloc(sizeof(cw_node) * (size_t)st->nodes.count);
  for (int i = 0; i < st->nodes.count; i++) {
    const st_node *rec = &st->nodes.data[i];
    cw_node n = cw_node_by_uid(t, rec->uid);
    if (i > 0) {
      cw_node parent = nodes[rec->parent_rec];
      if (!cw_node_valid(n)) {
        n = cw_node_new(rec->type->name, rec->name);
        cw_node_set_uid(n, rec->uid);
        int at = CW_MIN(rec->index, cw_node_child_count(parent));
        cw_node_add_child_at(parent, n, at);
      } else if (cw_node_parent(n).id != parent.id) {
        cw_node_reparent(n, parent);
      }
      if (cw_node_name(n) != rec->name) cw_node_set_name(n, rec->name);
    }
    nodes[i] = n;
  }
  /* 3. sibling order */
  for (int i = 1; i < st->nodes.count; i++) {
    const st_node *rec = &st->nodes.data[i];
    if (cw_node_index(nodes[i]) != rec->index) cw_node_move_child(nodes[rec->parent_rec], nodes[i], rec->index);
  }
  /* 4. properties (written only when they differ), then private state */
  for (int i = 0; i < st->nodes.count; i++) {
    const st_node *rec = &st->nodes.data[i];
    cw_node n = nodes[i];
    const plan *p = plan_of(n);
    CW_ASSERT(p->n == rec->nvals, "node type %s changed its properties between save and load", rec->type->name);
    for (int k = 0; k < p->n; k++) {
      cw_value want = st->vals.data[rec->first_val + k];
      if (want.type == CW_T_NODE) {
        uint64_t uid = want.res;
        want.res = 0;
        want.node = uid ? cw_node_by_uid(t, uid) : (cw_node){ 0 };
      }
      cw_value cur;
      memset(&cur, 0, sizeof cur);
      cw__prop_read(n, p->owners[k], p->props[k], &cur);
      if (!value_eq(&cur, &want)) cw__prop_write(n, p->owners[k], p->props[k], &want);
    }
    const uint8_t *b = st->blob.data + rec->blob_off;
    for (int h = 0; h < p->nhooks; h++) {
      int len;
      memcpy(&len, b, 4);
      if (p->hook_load[h]) p->hook_load[h](n, cw_node_data_t(n, p->hook_types[h]), b + 4, len);
      b += 4 + len;
    }
  }
  cw__phys2d_state_restored(t);
  cw__phys3d_state_restored(t);
  cw__tree_apply_deferred(t);
  t->tick = st->tick;
  t->rng = st->rng;
}

/* ── hash: types, structure, values; never uids, names or pointers ─── */

uint32_t cw_state_hash(const cw_state *st) {
  uint32_t h = CW_HASH32_INIT;
  h = cw_hash32(&st->tick, sizeof st->tick, h);
  h = cw_hash32(st->rng.s, sizeof st->rng.s, h);
  for (int i = 0; i < st->nodes.count; i++) {
    const st_node *rec = &st->nodes.data[i];
    h = cw_hash32(rec->type->name, strlen(rec->type->name), h);
    int32_t shape[3] = { rec->parent_rec, rec->index, rec->nvals };
    h = cw_hash32(shape, sizeof shape, h);
    for (int k = 0; k < rec->nvals; k++) {
      const cw_value *v = &st->vals.data[rec->first_val + k];
      uint8_t ty = (uint8_t)v->type;
      h = cw_hash32(&ty, 1, h);
      switch (v->type) {
      case CW_T_BOOL: { uint8_t b = v->b; h = cw_hash32(&b, 1, h); break; }
      case CW_T_INT: case CW_T_ENUM: h = cw_hash32(&v->i, 8, h); break;
      case CW_T_FLOAT: h = cw_hash32(&v->f, 8, h); break;
      case CW_T_VEC2: h = cw_hash32(v->v, 8, h); break;
      case CW_T_VEC3: h = cw_hash32(v->v, 12, h); break;
      case CW_T_VEC4: case CW_T_QUAT: case CW_T_COLOR: h = cw_hash32(v->v, 16, h); break;
      case CW_T_STRING: if (v->s) h = cw_hash32(v->s, strlen(v->s), h); break;
      case CW_T_NODE: {
        uint64_t idx = 0;
        int32_t ref = v->res && cw_map_get(&st->by_uid, v->res, &idx) ? (int32_t)idx - 1 : -1;
        h = cw_hash32(&ref, 4, h);
        break;
      }
      case CW_T_RES: h = cw_hash32(&v->res, 8, h); break;
      default: break;
      }
    }
    if (rec->blob_len) {
      const plan *p = plan_of_type(rec->type);
      const uint8_t *b = st->blob.data + rec->blob_off;
      for (int k = 0; k < p->nhooks; k++) {
        int len;
        memcpy(&len, b, 4);
        h = cw_hash32(b + 4, (size_t)CW_MAX(0, len - p->hook_nohash[k]), h);
        b += 4 + len;
      }
    }
  }
  return h;
}

/* ── text form, for diffing two peers' states after a desync ──────── */

size_t cw_state_dump(const cw_state *st, char *buf, size_t cap) {
  size_t n = 0;
#define OUT(...) do { int k_ = snprintf(buf ? buf + CW_MIN(n, cap) : NULL, buf && n < cap ? cap - n : 0, __VA_ARGS__); if (k_ > 0) n += (size_t)k_; } while (0)
  OUT("tick %llu rng %08x %08x %08x %08x\n", (unsigned long long)st->tick, st->rng.s[0], st->rng.s[1], st->rng.s[2], st->rng.s[3]);
  for (int i = 0; i < st->nodes.count; i++) {
    const st_node *rec = &st->nodes.data[i];
    OUT("node %d %s \"%s\" parent %d index %d\n", i, rec->type->name, rec->name, rec->parent_rec, rec->index);
    for (int k = 0; k < rec->nvals; k++) {
      cw_value v = st->vals.data[rec->first_val + k];
      char text[160];
      if (v.type == CW_T_NODE) snprintf(text, sizeof text, "uid %llu", (unsigned long long)v.res);
      else cw_value_format(&v, text, sizeof text);
      const plan *p = plan_of_type(rec->type);
      OUT("  %s.%s = %s\n", p->owners[k]->name, p->props[k]->name, text);
    }
    if (rec->blob_len) OUT("  private %d bytes\n", rec->blob_len);
  }
#undef OUT
  if (buf && cap) buf[CW_MIN(n, cap - 1)] = 0;
  return n;
}

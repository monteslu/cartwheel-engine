/* The mixer: binds tracks to node properties and bones, accumulates
 * weighted samples (blends, cross-fades, additive layers), writes the result
 * and extracts root motion. Shared by AnimationPlayer and AnimationTree.
 * Everything iterates arrays in insertion order: deterministic. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"
#include "scene/scene_internal.h"

void cw__amix_init(cw__amix *m, cw_node owner) {
  memset(m, 0, sizeof *m);
  m->owner = owner;
  cw_map_init(&m->index, CW_MEM_ANIM);
  m->hash = CW_HASH32_INIT;
  m->rm_lock = 7;
  m->rm_binding = -1;
}

static void drop_caches(cw__amix *m) {
  for (int i = 0; i < m->caches.count; i++) cw_free(m->caches.data[i].idx);
  cw_array_clear(&m->caches);
}

void cw__amix_free(cw__amix *m) {
  drop_caches(m);
  cw_array_free(&m->caches);
  cw_array_free(&m->b);
  cw_map_free(&m->index);
}

void cw__amix_set_root(cw__amix *m, cw_node root) {
  if (m->root.id == root.id) return;
  m->root = root;
  drop_caches(m);
}

/* ── binding ────────────────────────────────────────────────────────── */

static int comp_of(const char *suffix) {
  static const char *names[] = { "x", "y", "z", "w", "r", "g", "b", "a" };
  for (int i = 0; i < 8; i++)
    if (!strcmp(suffix, names[i])) return i & 3;
  return -1;
}

static uint64_t bkey(int kind, uint64_t a, const void *b, int c) {
  struct { uint64_t kind, a, b, c; } k = { (uint64_t)kind, a, (uint64_t)(uintptr_t)b, (uint64_t)(int64_t)c };
  return cw_hash_bytes(&k, sizeof k, 0x616e696d);
}

static void read_binding(mix_binding *b, cw_value *out) {
  if (b->kind == 1) {
    t3_object *o = b->obj;
    if (b->bprop == 1) { *out = (cw_value){ CW_T_QUAT }; memcpy(out->v, &o->quaternion, 16); }
    else { t3_vec3 p = b->bprop == 0 ? o->position : o->scale; *out = cw_val_vec3(p.x, p.y, p.z); }
    return;
  }
  cw_value v;
  if (!cw_node_get_prop(b->node, b->prop, &v)) { *out = (cw_value){ b->type }; return; }
  if (b->comp >= 0) { float f = v.v[b->comp]; *out = cw_val_float(f); }
  else *out = v;
}

static int bind_track(cw__amix *m, const anim_track *t, const char *anim_name) {
  if (t->kind != CW_TRACK_VALUE || !t->prop) return -1;
  cw_node node = t->node[0] ? cw_node_get(m->root, t->node) : m->root;
  if (!cw_node_valid(node)) {
    cw_warn("animation '%s': track '%s': no node at '%s' from '%s'", anim_name, t->path, t->node, cw_node_name(m->root));
    return -1;
  }
  mix_binding b = { 0 };
  b.node = node;
  b.rot = t->rot;
  uint64_t key;
  if (t->bone) {
    t3_object *root = cw_node3d_object(node);
    t3_object *bone = root ? t3_object_get_by_name(root, t->bone) : NULL;
    if (!bone) {
      cw_warn("animation '%s': track '%s': '%s' has no three.c object named '%s'", anim_name, t->path, t->node, t->bone);
      return -1;
    }
    b.kind = 1;
    b.obj = bone;
    b.bprop = !strcmp(t->prop, "position") ? 0 : !strcmp(t->prop, "quaternion") ? 1 : !strcmp(t->prop, "scale") ? 2 : -1;
    if (b.bprop < 0) {
      cw_warn("animation '%s': bone track '%s': bones animate position, quaternion or scale", anim_name, t->path);
      return -1;
    }
    b.type = b.bprop == 1 ? CW_T_QUAT : CW_T_VEC3;
    b.comp = -1;
    key = bkey(1, 0, bone, b.bprop);
  } else {
    const cw_prop *p = cw_node_find_prop(node, t->prop, NULL);
    b.comp = -1;
    b.prop = t->prop;
    if (!p) {
      const char *dot = strrchr(t->prop, '.');
      if (dot && comp_of(dot + 1) >= 0) {
        b.prop = cw_intern_n(t->prop, (size_t)(dot - t->prop));
        p = cw_node_find_prop(node, b.prop, NULL);
        b.comp = comp_of(dot + 1);
        if (p && b.comp >= cw__value_ncomp(p->type)) p = NULL;
        if (p && cw__value_ncomp(p->type) < 2) p = NULL;
      }
    }
    if (!p) {
      cw_warn("animation '%s': track '%s': '%s' (%s) has no property '%s'", anim_name, t->path, cw_node_name(node),
              cw_node_get_type(node)->name, t->prop);
      return -1;
    }
    if (p->flags & CW_PROP_READONLY) {
      cw_warn("animation '%s': track '%s': '%s' is read-only", anim_name, t->path, t->prop);
      return -1;
    }
    b.type = b.comp >= 0 ? CW_T_FLOAT : p->type;
    key = bkey(0, node.id, b.prop, b.comp);
  }
  uint64_t idx;
  if (cw_map_get(&m->index, key, &idx)) return (int)idx - 1;
  read_binding(&b, &b.base);
  cw_array_push(&m->b, b);
  cw_map_set(&m->index, key, (uint64_t)m->b.count);
  return m->b.count - 1;
}

static mix_cache *cache_for(cw__amix *m, cw_anim anim, anim_rec *a) {
  cw_tree *t = cw_node_tree(m->owner);
  uint64_t sv = t ? t->structure_version : 0;
  if (sv != m->struct_ver) {
    m->struct_ver = sv;
    drop_caches(m);
  }
  for (int i = 0; i < m->caches.count; i++) {
    mix_cache *c = &m->caches.data[i];
    if (c->anim != anim.id) continue;
    if (c->version == a->version) return c;
    cw_free(c->idx);
    c->idx = NULL;
    cw_array_remove_swap(&m->caches, i);
    break;
  }
  mix_cache c = { anim.id, a->version, NULL, a->tracks.count };
  c.idx = cw_alloc(sizeof(int) * (size_t)(c.n ? c.n : 1), CW_MEM_ANIM);
  for (int i = 0; i < c.n; i++) c.idx[i] = bind_track(m, &a->tracks.data[i], a->name);
  cw_array_push(&m->caches, c);
  return &m->caches.data[m->caches.count - 1];
}

/* ── accumulation ───────────────────────────────────────────────────── */

static void quat_mul(float o[4], const float a[4], const float b[4]) {
  float r[4] = {
    a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  };
  memcpy(o, r, sizeof r);
}

static void value_floats(const cw_value *v, float f[4]) {
  if (v->type == CW_T_FLOAT) { f[0] = (float)v->f; f[1] = f[2] = f[3] = 0; }
  else if (v->type == CW_T_INT || v->type == CW_T_ENUM) { f[0] = (float)v->i; f[1] = f[2] = f[3] = 0; }
  else memcpy(f, v->v, 16);
}

static void accumulate_additive(mix_binding *b, const cw_value *s, float w) {
  if (!cw__value_interpolable(b->type)) return;
  if (!b->has_add) {
    memset(b->add, 0, sizeof b->add);
    if (b->type == CW_T_QUAT) b->add[3] = 1;
    b->has_add = true;
  }
  float base[4], smp[4];
  value_floats(&b->base, base);
  value_floats(s, smp);
  if (b->type == CW_T_QUAT) {
    float inv[4] = { -base[0], -base[1], -base[2], base[3] }, d[4], id[4] = { 0, 0, 0, 1 };
    quat_mul(d, inv, smp);
    cw_quat_slerp_det(d, id, d, w);
    quat_mul(b->add, b->add, d);
  } else {
    for (int k = 0; k < 4; k++) b->add[k] += (smp[k] - base[k]) * w;
  }
}

static bool allowed(const cw_map *filter, const anim_track *t) {
  if (!filter) return true;
  uint64_t v;
  return cw_map_get(filter, (uint64_t)(uintptr_t)(t->bone ? t->bone : t->node), &v);
}

static void root_motion_sample(cw__amix *m, const anim_track *t, const anim_cursor *c, float w, cw_type type, cw_value *v) {
  /* the value with the locked axes held at the clip's start ... */
  cw_value start;
  cw__track_sample(t, 0, type, &start);
  for (int k = 0; k < 3; k++)
    if (m->rm_lock & (1 << k)) v->v[k] = start.v[k];
  /* ... and the motion they made this tick goes to the target */
  if (!c) return;
  t3_vec3 d = { 0, 0, 0 };
  for (int s = 0; s < c->seg_count; s++) {
    cw_value a, b;
    cw__track_sample(t, c->seg[s][0], type, &a);
    cw__track_sample(t, c->seg[s][1], type, &b);
    d.x += b.v[0] - a.v[0];
    d.y += b.v[1] - a.v[1];
    d.z += b.v[2] - a.v[2];
  }
  m->rm_acc.x += (m->rm_lock & 1 ? d.x : 0) * w;
  m->rm_acc.y += (m->rm_lock & 2 ? d.y : 0) * w;
  m->rm_acc.z += (m->rm_lock & 4 ? d.z : 0) * w;
  m->rm_weight += w;
}

void cw__amix_sample(cw__amix *m, cw_anim anim, float time, float w, const cw_map *filter, bool additive,
                      const anim_cursor *cursor) {
  anim_rec *a = cw__anim(anim);
  if (!a || w <= 0 || CW_HANDLE_IS_NULL(m->root)) return;
  mix_cache *c = cache_for(m, anim, a);
  for (int i = 0; i < c->n && i < a->tracks.count; i++) {
    int bi = c->idx[i];
    const anim_track *t = &a->tracks.data[i];
    if (bi < 0 || !t->enabled || !allowed(filter, t)) continue;
    mix_binding *b = &m->b.data[bi];
    cw_value v;
    if (!cw__track_sample(t, time, b->type, &v)) continue;
    if (m->rm_track && t->path == m->rm_track && b->type == CW_T_VEC3) {
      b->root_motion = true;
      m->rm_binding = bi;
      if (additive) continue;
      root_motion_sample(m, t, cursor, w, b->type, &v);
    }
    if (additive) { accumulate_additive(b, &v, w); continue; }
    if (!b->touched) {
      b->acc = v;
      b->weight = w;
      b->touched = true;
    } else {
      b->weight += w;
      cw__value_mix(&b->acc, &v, w / b->weight, b->rot);
    }
  }
}

/* ── writing ────────────────────────────────────────────────────────── */

static void hash_value(cw__amix *m, const cw_value *v) {
  switch (v->type) {
  case CW_T_FLOAT: { float f = (float)v->f; m->hash = cw_hash32(&f, 4, m->hash); break; }
  case CW_T_INT: case CW_T_ENUM: m->hash = cw_hash32(&v->i, 8, m->hash); break;
  case CW_T_BOOL: { uint8_t x = v->b; m->hash = cw_hash32(&x, 1, m->hash); break; }
  case CW_T_STRING: m->hash = cw_hash32(v->s ? v->s : "", v->s ? strlen(v->s) : 0, m->hash); break;
  default: m->hash = cw_hash32(v->v, sizeof(float) * (size_t)cw__value_ncomp(v->type), m->hash); break;
  }
}

static void write_binding(mix_binding *b, const cw_value *v) {
  if (b->kind == 1) {
    t3_object *o = b->obj;
    if (b->bprop == 0) o->position = (t3_vec3){ v->v[0], v->v[1], v->v[2] };
    else if (b->bprop == 2) o->scale = (t3_vec3){ v->v[0], v->v[1], v->v[2] };
    else t3_object_set_quaternion(o, (t3_quat){ v->v[0], v->v[1], v->v[2], v->v[3] });
    return;
  }
  if (b->comp >= 0) {
    cw_value full;
    if (!cw_node_get_prop(b->node, b->prop, &full)) return;
    full.v[b->comp] = (float)v->f;
    cw_node_set(b->node, b->prop, full);
  } else {
    cw_node_set(b->node, b->prop, *v);
  }
}

/* world transform of the space a track's values live in */
static void track_parent_world(mix_binding *b, t3_mat4 *out) {
  t3_mat4_identity(out);
  if (b->kind == 1) {
    /* bone: objects between the node's own object and the bone's parent,
     * from their fields (glTF data), under the node's global transform */
    t3_object *top = cw_node3d_object(b->node);
    t3_object *chain[32];
    int n = 0;
    for (t3_object *o = b->obj->parent; o && o != top && n < 32; o = o->parent) chain[n++] = o;
    const t3_mat4 *g = cw_node3d_global_transform(b->node);
    if (g) *out = *g;
    for (int i = n - 1; i >= 0; i--) {
      t3_mat4 l, r;
      t3_mat4_compose(&l, chain[i]->position, chain[i]->quaternion, chain[i]->scale);
      t3_mat4_multiply(&r, out, &l);
      *out = r;
    }
  } else {
    for (cw_node p = cw_node_parent(b->node); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p)) {
      const t3_mat4 *g = cw_node3d_global_transform(p);
      if (g) { *out = *g; break; }
    }
  }
}

static void apply_root_motion(cw__amix *m) {
  m->rm_last = (t3_vec3){ 0, 0, 0 };
  if (!m->rm_track || m->rm_weight <= 0 || m->rm_binding < 0) { m->rm_acc = m->rm_last; m->rm_weight = 0; return; }
  float div = m->rm_weight > 1 ? m->rm_weight : 1;
  t3_vec3 d = { m->rm_acc.x / div, m->rm_acc.y / div, m->rm_acc.z / div };
  m->rm_acc = (t3_vec3){ 0, 0, 0 };
  m->rm_weight = 0;
  cw_node target = m->rm_target;
  if (!cw_node_valid(target) || !cw_node_is(target, "Node3D")) return;
  t3_mat4 src, dst = { 0 }, inv, rel;
  track_parent_world(&m->b.data[m->rm_binding], &src);
  t3_mat4_identity(&dst);
  for (cw_node p = cw_node_parent(target); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p)) {
    const t3_mat4 *g = cw_node3d_global_transform(p);
    if (g) { dst = *g; break; }
  }
  t3_mat4_invert(&inv, &dst);
  t3_mat4_multiply(&rel, &inv, &src);
  const float *e = rel.e;
  t3_vec3 o = { e[0] * d.x + e[4] * d.y + e[8] * d.z, e[1] * d.x + e[5] * d.y + e[9] * d.z, e[2] * d.x + e[6] * d.y + e[10] * d.z };
  m->rm_last = o;
  t3_vec3 p = cw_node3d_position(target);
  cw_node3d_snapshot(target);
  cw_node3d_set_position(target, (t3_vec3){ p.x + o.x, p.y + o.y, p.z + o.z });
  m->hash = cw_hash32(&o, sizeof o, m->hash);
}

void cw__amix_apply(cw__amix *m) {
  for (int i = 0; i < m->b.count; i++) {
    mix_binding *b = &m->b.data[i];
    if (!b->touched && !b->has_add) continue;
    bool live = cw_node_valid(b->node);
    if (live) {
      cw_value v = b->touched ? b->acc : b->base;
      if (b->touched && b->weight < 1) {
        cw_value base = b->base;
        cw_value tmp = base;
        /* acc * weight + base * (1 - weight) */
        cw__value_mix(&tmp, &v, b->weight, b->rot);
        v = tmp;
      }
      if (b->has_add) {
        float f[4];
        value_floats(&v, f);
        if (b->type == CW_T_QUAT) {
          quat_mul(f, f, b->add);
          memcpy(v.v, f, 16);
        } else if (b->type == CW_T_FLOAT) {
          v.f = (float)v.f + b->add[0];
        } else if (b->type == CW_T_INT) {
          v.i += (int64_t)floorf(b->add[0] + 0.5f);
        } else {
          for (int k = 0; k < 4; k++) v.v[k] = f[k] + b->add[k];
        }
      }
      hash_value(m, &v);
      write_binding(b, &v);
    }
    b->touched = false;
    b->has_add = false;
    b->weight = 0;
  }
  apply_root_motion(m);
}

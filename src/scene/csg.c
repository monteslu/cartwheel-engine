/* CSG node types (cartwheel/csg.h): the shapes, the baked mesh a root
 * draws, the preview, and the .cwcsg file format the cwcsg tool writes. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/csg.h"
#include "cartwheel/resource.h"

static const char *const op_names[] = { "UNION", "INTERSECTION", "SUBTRACTION", NULL };

typedef struct csg_node {
  int operation;
  t3_color color;
  /* root only */
  const char *bake_file;
  float roughness, metalness;
  bool cast_shadow, receive_shadow, ghosts;
  /* the bake a root draws */
  const char *built_file;
  cw_res res;
  uint32_t built_version;
  t3_mesh *baked;
  bool stale, warned, failed;
  int triangles;
  uint64_t file_hash, mesh_hash;
  char file_hash_s[17], mesh_hash_s[17];
  /* the preview */
  t3_mesh *prev;
  uint64_t prev_key;
} csg_node;

typedef struct csg_box { t3_vec3 size; } csg_box;
typedef struct csg_cyl { float radius, height; int sides; bool cone; } csg_cyl;
typedef struct csg_sphere { float radius; int segments; } csg_sphere;

static const cw_node_type *shape_t, *box_t, *cyl_t, *sphere_t;

static csg_node *data_of(cw_node n) { return cw_node_data_t(n, shape_t); }

bool cw_csg_is_root(cw_node n) { return data_of(n) && !data_of(cw_node_parent(n)); }

cw_node cw_csg_root_of(cw_node n) {
  if (!data_of(n)) return (cw_node){ 0 };
  while (data_of(cw_node_parent(n))) n = cw_node_parent(n);
  return n;
}

static cw_csg_kind kind_of(cw_node n) {
  if (cw_node_data_t(n, box_t)) return CW_CSG_BOX;
  if (cw_node_data_t(n, cyl_t)) return CW_CSG_CYLINDER;
  if (cw_node_data_t(n, sphere_t)) return CW_CSG_SPHERE;
  return CW_CSG_COMBINER;
}

/* ── hashing and flattening ─────────────────────────────────────────── */

uint64_t cw_csg_fnv1a(const void *bytes, size_t len, uint64_t h) {
  const uint8_t *p = bytes;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}
#define FNV0 0xcbf29ce484222325ull

static uint64_t hash_prop(uint64_t h, cw_node n, const char *name) {
  cw_value v;
  if (!cw_node_get_prop(n, name, &v)) return h;
  h = cw_csg_fnv1a(name, strlen(name), h);
  switch (v.type) {
  case CW_T_BOOL: { uint8_t b = v.b; return cw_csg_fnv1a(&b, 1, h); }
  case CW_T_INT: case CW_T_ENUM: return cw_csg_fnv1a(&v.i, sizeof v.i, h);
  case CW_T_FLOAT: { float f = (float)v.f; return cw_csg_fnv1a(&f, 4, h); }
  case CW_T_VEC3: return cw_csg_fnv1a(v.v, 12, h);
  case CW_T_COLOR: return cw_csg_fnv1a(v.v, 16, h);
  default: return h;
  }
}

static uint64_t hash_branch(cw_node n, uint64_t h, bool root) {
  csg_node *d = data_of(n);
  const char *tn = cw_node_get_type(n)->name;
  h = cw_csg_fnv1a(tn, strlen(tn) + 1, h);
  if (!root) {
    int32_t op = d->operation;
    h = cw_csg_fnv1a(&op, 4, h);
    h = hash_prop(h, n, "position");
    h = hash_prop(h, n, "rotation");
    h = hash_prop(h, n, "scale");
  }
  h = cw_csg_fnv1a(&d->color, sizeof d->color, h);
  static const char *const shape_props[] = { "size", "radius", "height", "sides", "cone", "segments" };
  for (size_t i = 0; i < CW_ARRAY_LEN(shape_props); i++) h = hash_prop(h, n, shape_props[i]);
  int nc = cw_node_child_count(n);
  for (int i = 0; i < nc; i++) {
    cw_node c = cw_node_child(n, i);
    if (data_of(c)) h = hash_branch(c, h, false);
  }
  uint8_t end = 0xff;
  return cw_csg_fnv1a(&end, 1, h);
}

uint64_t cw_csg_input_hash(cw_node root) {
  if (!cw_csg_is_root(root)) return 0;
  return hash_branch(root, FNV0, true);
}

static void local_matrix(cw_node n, t3_mat4 *m) {
  cw_value p, q, s;
  t3_vec3 pos = { 0, 0, 0 }, sc = { 1, 1, 1 };
  t3_quat qu = { 0, 0, 0, 1 };
  if (cw_node_get_prop(n, "position", &p)) pos = (t3_vec3){ p.v[0], p.v[1], p.v[2] };
  if (cw_node_get_prop(n, "quaternion", &q)) qu = (t3_quat){ q.v[0], q.v[1], q.v[2], q.v[3] };
  if (cw_node_get_prop(n, "scale", &s)) sc = (t3_vec3){ s.v[0], s.v[1], s.v[2] };
  t3_mat4_compose(m, pos, qu, sc);
}

static void flatten(cw_node n, int parent, const t3_mat4 *parent_m, bool root, cw_csg_shape *out, int cap, int *count) {
  csg_node *d = data_of(n);
  t3_mat4 m;
  if (root) {
    t3_mat4_identity(&m);
  } else {
    t3_mat4 l;
    local_matrix(n, &l);
    t3_mat4_multiply(&m, parent_m, &l);
  }
  int me = (*count)++;
  if (me < cap) {
    cw_csg_shape *s = &out[me];
    memset(s, 0, sizeof *s);
    s->kind = kind_of(n);
    s->op = root ? CW_CSG_UNION : (cw_csg_op)d->operation;
    s->parent = parent;
    s->to_root = m;
    s->color = d->color;
    csg_box *b = cw_node_data_t(n, box_t);
    csg_cyl *c = cw_node_data_t(n, cyl_t);
    csg_sphere *sp = cw_node_data_t(n, sphere_t);
    if (b) s->size = b->size;
    if (c) { s->radius = c->radius; s->height = c->height; s->segments = c->sides; s->cone = c->cone; }
    if (sp) { s->radius = sp->radius; s->segments = sp->segments; }
  }
  int nc = cw_node_child_count(n);
  for (int i = 0; i < nc; i++) {
    cw_node c = cw_node_child(n, i);
    if (data_of(c)) flatten(c, me, &m, false, out, cap, count);
  }
}

int cw_csg_flatten(cw_node root, cw_csg_shape *out, int cap) {
  if (!cw_csg_is_root(root)) return 0;
  int count = 0;
  flatten(root, -1, NULL, true, out, cap, &count);
  return count;
}

/* ── the file ───────────────────────────────────────────────────────── */

#define CSG_MAGIC "CWCSG001"
#define CSG_HEADER 48

uint8_t *cw_csg_mesh_encode(const cw_csg_mesh *m, size_t *len) {
  size_t v = m->vertex_count, n = CSG_HEADER + v * 11 * 4 + (size_t)m->index_count * 4;
  uint8_t *b = cw_alloc(n, CW_MEM_RESOURCE), *p = b;
  memcpy(p, CSG_MAGIC, 8);
  memcpy(p + 8, &m->vertex_count, 4);
  memcpy(p + 12, &m->index_count, 4);
  memcpy(p + 16, &m->input_hash, 8);
  memcpy(p + 24, m->bounds, 24);
  p += CSG_HEADER;
  memcpy(p, m->positions, v * 12), p += v * 12;
  memcpy(p, m->normals, v * 12), p += v * 12;
  memcpy(p, m->colors, v * 12), p += v * 12;
  memcpy(p, m->uvs, v * 8), p += v * 8;
  memcpy(p, m->indices, (size_t)m->index_count * 4);
  *len = n;
  return b;
}

bool cw_csg_mesh_decode(const uint8_t *b, size_t len, cw_csg_mesh *out) {
  memset(out, 0, sizeof *out);
  if (len < CSG_HEADER || memcmp(b, CSG_MAGIC, 8)) {
    cw_error("csg: not a .cwcsg file (or another version): bake it again with cwcsg");
    return false;
  }
  memcpy(&out->vertex_count, b + 8, 4);
  memcpy(&out->index_count, b + 12, 4);
  memcpy(&out->input_hash, b + 16, 8);
  memcpy(out->bounds, b + 24, 24);
  size_t v = out->vertex_count;
  if (out->index_count % 3 || len != CSG_HEADER + v * 44 + (size_t)out->index_count * 4) {
    cw_error("csg: truncated or corrupt .cwcsg file: bake it again with cwcsg");
    return false;
  }
  const uint8_t *p = b + CSG_HEADER;
  out->positions = (const float *)p, p += v * 12;
  out->normals = (const float *)p, p += v * 12;
  out->colors = (const float *)p, p += v * 12;
  out->uvs = (const float *)p, p += v * 8;
  out->indices = (const uint32_t *)p;
  for (uint32_t i = 0; i < out->index_count; i++)
    if (out->indices[i] >= out->vertex_count) {
      cw_error("csg: a .cwcsg index is out of range: bake it again with cwcsg");
      return false;
    }
  return true;
}

/* ── the baked mesh ─────────────────────────────────────────────────── */

static void drop_baked(cw_node n, csg_node *d) {
  if (d->baked) {
    t3_object_remove(cw_node3d_object(n), d->baked);
    d->baked = NULL;
  }
  d->triangles = 0;
  d->mesh_hash = 0;
  d->mesh_hash_s[0] = 0;
}

static uint64_t hash_attr(uint64_t h, const t3_attribute *a) {
  if (!a) return h;
  size_t es = a->type == T3_UINT16 ? 2 : 4;
  return cw_csg_fnv1a(a->array, (size_t)a->count * (size_t)a->item_size * es, h);
}

static void build_baked(cw_node n, csg_node *d, const uint8_t *bytes, size_t len) {
  cw_csg_mesh m;
  d->file_hash = cw_csg_fnv1a(bytes, len, FNV0);
  snprintf(d->file_hash_s, sizeof d->file_hash_s, "%016llx", (unsigned long long)d->file_hash);
  if (!cw_csg_mesh_decode(bytes, len, &m)) { d->failed = true; return; }
  int v = (int)m.vertex_count;
  t3_geometry *g = t3_geometry_new();
  t3_attribute *a;
  a = t3_attribute_new(T3_FLOAT32, m.positions, v, 3), t3_geometry_set_attribute(g, T3_ATTR_POSITION, a), t3_release(a);
  a = t3_attribute_new(T3_FLOAT32, m.normals, v, 3), t3_geometry_set_attribute(g, T3_ATTR_NORMAL, a), t3_release(a);
  a = t3_attribute_new(T3_FLOAT32, m.colors, v, 3), t3_geometry_set_attribute(g, T3_ATTR_COLOR, a), t3_release(a);
  a = t3_attribute_new(T3_FLOAT32, m.uvs, v, 2), t3_geometry_set_attribute(g, T3_ATTR_UV, a), t3_release(a);
  a = t3_attribute_new(T3_UINT32, m.indices, (int)m.index_count, 1), t3_geometry_set_index(g, a), t3_release(a);
  t3_geometry_compute_bounding_box(g);
  t3_geometry_compute_bounding_sphere(g);
  /* the loaded geometry as the game sees it: the attributes three.c draws */
  uint64_t h = FNV0;
  for (int s = 0; s < T3_ATTR_COUNT; s++) h = hash_attr(h, g->attributes[s]);
  d->mesh_hash = hash_attr(h, g->index);
  snprintf(d->mesh_hash_s, sizeof d->mesh_hash_s, "%016llx", (unsigned long long)d->mesh_hash);
  t3_material *mat = t3_mesh_standard_material_new(0xffffff);
  mat->vertex_colors = true;
  mat->roughness = d->roughness;
  mat->metalness = d->metalness;
  d->baked = t3_mesh_new(g, mat);
  t3_release(g);
  t3_release(mat);
  t3_object_add(cw_node3d_object(n), d->baked);
  t3_release(d->baked);
  d->triangles = (int)m.index_count / 3;
  uint64_t now = cw_csg_input_hash(n);
  d->stale = now != m.input_hash;
  if (d->stale && !d->warned) {
    d->warned = true;
    cw_warn("CSG '%s': %s was baked from other shapes (they changed since): run cwcsg on the scene again",
            cw_node_name(n), d->bake_file);
  }
}

/* Load (or reload) the root's bake file; idempotent within a frame. */
static void ensure_baked(cw_node n, csg_node *d) {
  const char *want = d->bake_file && d->bake_file[0] ? d->bake_file : NULL;
  if (want != d->built_file) {
    drop_baked(n, d);
    cw_res_release(d->res);
    d->res = (cw_res){ 0 };
    d->built_file = want;
    d->built_version = 0;
    d->failed = d->stale = d->warned = false;
    d->file_hash = 0;
    d->file_hash_s[0] = 0;
    if (want) d->res = cw_res_load(want, CW_RES_BINARY);
  }
  if (!want || d->failed) return;
  if (cw_res_get_state(d->res) == CW_RES_STATE_FAILED) {
    if (!d->failed) cw_warn("CSG '%s': no bake file %s: drawing the preview (bake it with cwcsg)", cw_node_name(n), want);
    d->failed = true;
    return;
  }
  const cw_blob *b = cw_res_blob(d->res);
  if (!b || cw_res_version(d->res) == d->built_version) return;
  d->built_version = cw_res_version(d->res);
  drop_baked(n, d);
  build_baked(n, d, b->data, b->len);
}

t3_mesh *cw_csg_baked_mesh(cw_node root) {
  csg_node *d = data_of(root);
  if (!d || !cw_csg_is_root(root)) return NULL;
  ensure_baked(root, d);
  return d->baked;
}

/* ── the preview ────────────────────────────────────────────────────── */

enum { STYLE_SOLID, STYLE_SUB, STYLE_INT };

static t3_geometry *prev_geometry(cw_node n) {
  csg_box *b = cw_node_data_t(n, box_t);
  csg_cyl *c = cw_node_data_t(n, cyl_t);
  csg_sphere *s = cw_node_data_t(n, sphere_t);
  if (b) return t3_box_geometry_new(b->size.x, b->size.y, b->size.z, 1, 1, 1);
  if (c) return t3_cylinder_geometry_new(c->cone ? 0 : c->radius, c->radius, c->height, CW_MAX(c->sides, 3), 1, false);
  if (s) return t3_sphere_geometry_new(s->radius, CW_MAX(s->segments, 3), CW_MAX(s->segments / 2, 2));
  return NULL;
}

static void drop_prev(cw_node n, csg_node *d) {
  if (d->prev) {
    t3_object_remove(cw_node3d_object(n), d->prev);
    d->prev = NULL;
  }
  d->prev_key = 0;
}

static void update_prev(cw_node n, csg_node *d, cw_node root, csg_node *rd) {
  bool show = !rd->baked && kind_of(n) != CW_CSG_COMBINER;
  int style = STYLE_SOLID;
  for (cw_node p = n; show && !((p).id == (root).id); p = cw_node_parent(p)) {
    int op = data_of(p)->operation;
    if (op == CW_CSG_SUBTRACTION) { style = STYLE_SUB; break; }
    if (op == CW_CSG_INTERSECTION) style = STYLE_INT;
  }
  if (show && style != STYLE_SOLID && !rd->ghosts) show = false;
  if (!show) { drop_prev(n, d); return; }
  uint64_t k = FNV0;
  k = cw_csg_fnv1a(&style, sizeof style, k);
  k = cw_csg_fnv1a(&d->color, sizeof d->color, k);
  k = cw_csg_fnv1a(&rd->roughness, 4, k);
  k = cw_csg_fnv1a(&rd->metalness, 4, k);
  k = cw_csg_fnv1a(&rd->cast_shadow, 1, k);
  k = cw_csg_fnv1a(&rd->receive_shadow, 1, k);
  csg_box *b = cw_node_data_t(n, box_t);
  csg_cyl *c = cw_node_data_t(n, cyl_t);
  csg_sphere *s = cw_node_data_t(n, sphere_t);
  if (b) k = cw_csg_fnv1a(b, sizeof *b, k);
  if (c) { k = cw_csg_fnv1a(&c->radius, 8, k); k = cw_csg_fnv1a(&c->sides, 4, k); k = cw_csg_fnv1a(&c->cone, 1, k); }
  if (s) k = cw_csg_fnv1a(s, sizeof *s, k);
  k |= 1; /* never 0 (0 = none built) */
  if (k == d->prev_key) return;
  drop_prev(n, d);
  t3_geometry *g = prev_geometry(n);
  if (!g) return;
  t3_material *mat = t3_mesh_standard_material_new(0xffffff);
  if (style == STYLE_SOLID) {
    mat->color = d->color;
    mat->roughness = rd->roughness;
    mat->metalness = rd->metalness;
  } else {
    mat->color = style == STYLE_SUB ? (t3_color){ 1.0f, 0.15f, 0.1f } : (t3_color){ 0.15f, 0.35f, 1.0f };
    mat->roughness = 1;
    mat->transparent = true;
    mat->opacity = 0.3f;
    mat->depth_write = false;
  }
  d->prev = t3_mesh_new(g, mat);
  t3_release(g);
  t3_release(mat);
  d->prev->base.cast_shadow = style == STYLE_SOLID && rd->cast_shadow;
  d->prev->base.receive_shadow = style == STYLE_SOLID && rd->receive_shadow;
  t3_object_add(cw_node3d_object(n), d->prev);
  t3_release(d->prev);
  d->prev_key = k;
}

/* ── the node types ─────────────────────────────────────────────────── */

static void shape_init(cw_node n, void *data) {
  (void)n;
  csg_node *d = data;
  d->color = (t3_color){ 0.8f, 0.8f, 0.8f };
  d->roughness = 0.9f;
  d->cast_shadow = d->receive_shadow = d->ghosts = true;
}

static void shape_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  csg_node *d = data;
  cw_node root = cw_csg_root_of(n);
  csg_node *rd = data_of(root);
  ensure_baked(root, rd);
  if (((root).id == (n).id) && d->baked) {
    t3_material *m = d->baked->materials[0];
    m->roughness = d->roughness;
    m->metalness = d->metalness;
    d->baked->base.cast_shadow = d->cast_shadow;
    d->baked->base.receive_shadow = d->receive_shadow;
  }
  if (!((root).id == (n).id) && (d->baked || d->built_file)) {
    /* moved under another CSG node: no longer a root */
    drop_baked(n, d);
    cw_res_release(d->res);
    d->res = (cw_res){ 0 };
    d->built_file = NULL;
  }
  update_prev(n, d, root, rd);
}

static void shape_destroy(cw_node n, void *data) {
  (void)n;
  csg_node *d = data;
  cw_res_release(d->res);
  d->res = (cw_res){ 0 };
}

static void ro_baked(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_bool(((csg_node *)data)->baked != NULL);
}
static void ro_stale(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_bool(((csg_node *)data)->stale);
}
static void ro_tris(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_int(((csg_node *)data)->triangles);
}
static void ro_file_hash(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_string(((csg_node *)data)->file_hash_s);
}
static void ro_mesh_hash(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_string(((csg_node *)data)->mesh_hash_s);
}

static const cw_prop shape_props[] = {
  { .name = "operation", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(csg_node, operation),
    .size = sizeof(int), .enum_names = op_names,
    .doc = "How this shape combines with what its CSG parent built so far (ignored on the root)." },
  CW_PROP_FIELD(csg_node, color, CW_T_COLOR, CW_PROP_DEFAULT, "Surface colour (linear); faces this shape cuts take it."),
  CW_PROP_FIELD(csg_node, bake_file, CW_T_STRING, CW_PROP_DEFAULT,
                "Root: the baked mesh asset cwcsg writes (.cwcsg); empty or missing = the preview."),
  CW_PROP_FIELD_RANGE(csg_node, roughness, CW_T_FLOAT, 0, 1, "Root: PBR roughness of the whole result."),
  CW_PROP_FIELD_RANGE(csg_node, metalness, CW_T_FLOAT, 0, 1, "Root: PBR metalness of the whole result."),
  CW_PROP_FIELD(csg_node, cast_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Root: casts shadows."),
  CW_PROP_FIELD(csg_node, receive_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Root: receives shadows."),
  CW_PROP_FIELD(csg_node, ghosts, CW_T_BOOL, CW_PROP_DEFAULT,
                "Root, preview: draw subtracted (red) and intersected (blue) shapes see-through."),
  CW_PROP_ACCESSOR("baked", CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, ro_baked, NULL, "Root: drawing the bake file."),
  CW_PROP_ACCESSOR("stale", CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, ro_stale, NULL,
                   "Root: the shapes changed since the bake file was baked."),
  CW_PROP_ACCESSOR("triangles", CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, ro_tris, NULL, "Root: triangles in the bake."),
  CW_PROP_ACCESSOR("file_hash", CW_T_STRING, CW_PROP_EDITOR | CW_PROP_READONLY, ro_file_hash, NULL,
                   "Root: FNV-1a 64 of the bake file's bytes (hex)."),
  CW_PROP_ACCESSOR("mesh_hash", CW_T_STRING, CW_PROP_EDITOR | CW_PROP_READONLY, ro_mesh_hash, NULL,
                   "Root: FNV-1a 64 of the loaded geometry's attributes and indices (hex)."),
  CW_PROP_END,
};

static cw_node_type shape_type = {
  .name = "CSGShape3D", .parent = "Node3D", .data_size = sizeof(csg_node), .props = shape_props,
  .doc = "Base of the CSG block-out shapes: combines with its CSG parent by `operation`; the topmost one is the root "
         "and draws the baked result (cartwheel/csg.h).",
  .init = shape_init, .destroy = shape_destroy, .pre_render = shape_pre_render,
};

static cw_node_type combiner_type = {
  .name = "CSGCombiner3D", .parent = "CSGShape3D",
  .doc = "A CSG node with no shape of its own: groups CSG children (cartwheel/csg.h).",
};

static void box_init(cw_node n, void *data) {
  (void)n;
  ((csg_box *)data)->size = (t3_vec3){ 1, 1, 1 };
}
static const cw_prop box_props[] = {
  CW_PROP_FIELD(csg_box, size, CW_T_VEC3, CW_PROP_DEFAULT, "Extents along x, y and z, centred on the node."),
  CW_PROP_END,
};
static cw_node_type box_type = {
  .name = "CSGBox3D", .parent = "CSGShape3D", .data_size = sizeof(csg_box), .props = box_props,
  .doc = "A CSG box (cartwheel/csg.h).", .init = box_init,
};

static void cyl_init(cw_node n, void *data) {
  (void)n;
  csg_cyl *c = data;
  c->radius = 0.5f;
  c->height = 2;
  c->sides = 16;
}
static const cw_prop cyl_props[] = {
  CW_PROP_FIELD_RANGE(csg_cyl, radius, CW_T_FLOAT, 0.0001, 1e6, "Radius (at the bottom for a cone)."),
  CW_PROP_FIELD_RANGE(csg_cyl, height, CW_T_FLOAT, 0.0001, 1e6, "Height along Y, centred on the node."),
  CW_PROP_FIELD_RANGE(csg_cyl, sides, CW_T_INT, 3, 256, "Facets around."),
  CW_PROP_FIELD(csg_cyl, cone, CW_T_BOOL, CW_PROP_DEFAULT, "A cone: the top is a point."),
  CW_PROP_END,
};
static cw_node_type cyl_type = {
  .name = "CSGCylinder3D", .parent = "CSGShape3D", .data_size = sizeof(csg_cyl), .props = cyl_props,
  .doc = "A CSG cylinder or cone along Y (cartwheel/csg.h).", .init = cyl_init,
};

static void sphere_init(cw_node n, void *data) {
  (void)n;
  csg_sphere *s = data;
  s->radius = 0.5f;
  s->segments = 16;
}
static const cw_prop sphere_props[] = {
  CW_PROP_FIELD_RANGE(csg_sphere, radius, CW_T_FLOAT, 0.0001, 1e6, "Radius."),
  CW_PROP_FIELD_RANGE(csg_sphere, segments, CW_T_INT, 4, 256, "Facets around the equator (the bake rounds up to a multiple of 4)."),
  CW_PROP_END,
};
static cw_node_type sphere_type = {
  .name = "CSGSphere3D", .parent = "CSGShape3D", .data_size = sizeof(csg_sphere), .props = sphere_props,
  .doc = "A CSG sphere (cartwheel/csg.h).", .init = sphere_init,
};

void cw__register_csg_types(void) {
  cw_node_type_register(&shape_type);
  cw_node_type_register(&combiner_type);
  cw_node_type_register(&box_type);
  cw_node_type_register(&cyl_type);
  cw_node_type_register(&sphere_type);
  shape_t = &shape_type;
  box_t = &box_type;
  cyl_t = &cyl_type;
  sphere_t = &sphere_type;
}

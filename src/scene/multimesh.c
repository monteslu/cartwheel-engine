/* MultiMeshInstance3D (cartwheel/multimesh.h): many instances of the meshes
 * under a source node, from a buffer the game fills, drawn as three.c
 * instanced meshes: one per mesh part per spatial cell, each with its own
 * bounds for three.c to cull (as TerrainScatter3D's cells are). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/multimesh.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "render3d_internal.h"

#define MAX_PARTS 16

typedef struct mm_part {
  t3_geometry *geometry;   /* retained */
  t3_material *material;   /* retained */
  t3_mat4 local;           /* the mesh in the source's parent space (the source's own transform included) */
  bool identity;
  float radius;            /* the geometry's bounding sphere through local, about the instance origin */
} mm_part;

typedef struct mm_chunk {
  int start, count;        /* into order[] */
  t3_sphere sphere;        /* node space */
  t3_instanced_mesh *im[MAX_PARTS];
} mm_chunk;

typedef struct multimesh {
  /* properties */
  const char *source;
  int32_t instance_count, visible_instance_count;
  float chunk_size;
  bool cast_shadow, receive_shadow, use_colors;
  int32_t fill;
  t3_vec3 fill_count, fill_spacing;
  /* the buffer */
  int count, cap;
  float *xf;               /* count x 16 */
  float *color;            /* count x 3 */
  float *custom;           /* count x 4 */
  /* built */
  bool dirty, parts_dirty, warned, range_warned;
  cw_node src_node;
  t3_object *src_obj;
  uint32_t src_sig;        /* what the parts were collected from */
  mm_part parts[MAX_PARTS];
  int nparts;
  CW_ARRAY(mm_chunk) chunks;
  int *order;              /* instance indices, cell by cell */
  int order_cap;
  float built_chunk;
  int built_visible, built_fill_sig;
  bool built_colors;
  cw_multimesh_stats st;
} multimesh;

static const cw_node_type *mm_t;
static multimesh *MM(cw_node n) { return cw_node_data_t(n, mm_t); }

/* ── the buffer ────────────────────────────────────────────────────── */

static void buf_resize(multimesh *d, int count) {
  if (count < 0) count = 0;
  if (count > d->cap) {
    int cap = CW_MAX(count, d->cap * 2);
    d->xf = cw_realloc(d->xf, (size_t)cap * 16 * sizeof(float), CW_MEM_RENDER);
    d->color = cw_realloc(d->color, (size_t)cap * 3 * sizeof(float), CW_MEM_RENDER);
    d->custom = cw_realloc(d->custom, (size_t)cap * 4 * sizeof(float), CW_MEM_RENDER);
    d->cap = cap;
  }
  for (int i = d->count; i < count; i++) {
    float *m = d->xf + (size_t)i * 16;
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
    d->color[i * 3] = d->color[i * 3 + 1] = d->color[i * 3 + 2] = 1;
    memset(d->custom + (size_t)i * 4, 0, 4 * sizeof(float));
  }
  d->count = count;
  d->instance_count = count;
  d->dirty = true;
}

static multimesh *at(cw_node n, int i, const char *what) {
  multimesh *d = MM(n);
  if (!d) return NULL;
  if (i < 0 || i >= d->count) {
    if (!d->range_warned) cw_warn("%s: instance %d is outside the buffer (%d instances: cw_multimesh_set_instance_count first)", what, i, d->count);
    d->range_warned = true;
    return NULL;
  }
  return d;
}

void cw_multimesh_set_instance_count(cw_node n, int count) {
  multimesh *d = MM(n);
  if (d) buf_resize(d, count);
}
int cw_multimesh_instance_count(cw_node n) {
  multimesh *d = MM(n);
  return d ? d->count : 0;
}
void cw_multimesh_set_transform(cw_node n, int i, const t3_mat4 *m) {
  multimesh *d = at(n, i, "cw_multimesh_set_transform");
  if (!d || !m) return;
  memcpy(d->xf + (size_t)i * 16, m->e, 16 * sizeof(float));
  d->dirty = true;
}
bool cw_multimesh_get_transform(cw_node n, int i, t3_mat4 *out) {
  multimesh *d = MM(n);
  if (!d || i < 0 || i >= d->count) return false;
  if (out) memcpy(out->e, d->xf + (size_t)i * 16, 16 * sizeof(float));
  return true;
}
void cw_multimesh_set_color(cw_node n, int i, t3_color c) {
  multimesh *d = at(n, i, "cw_multimesh_set_color");
  if (!d) return;
  d->color[i * 3] = c.r, d->color[i * 3 + 1] = c.g, d->color[i * 3 + 2] = c.b;
  d->dirty = true;
}
void cw_multimesh_set_custom(cw_node n, int i, const float v[4]) {
  multimesh *d = at(n, i, "cw_multimesh_set_custom");
  if (d && v) memcpy(d->custom + (size_t)i * 4, v, 4 * sizeof(float));
}
bool cw_multimesh_get_custom(cw_node n, int i, float out[4]) {
  multimesh *d = MM(n);
  if (!d || i < 0 || i >= d->count) return false;
  if (out) memcpy(out, d->custom + (size_t)i * 4, 4 * sizeof(float));
  return true;
}
float *cw_multimesh_transforms(cw_node n) {
  multimesh *d = MM(n);
  return d && d->count ? d->xf : NULL;
}
void cw_multimesh_touch(cw_node n) {
  multimesh *d = MM(n);
  if (d) d->dirty = true;
}
void cw_multimesh_get_stats(cw_node n, cw_multimesh_stats *out) {
  multimesh *d = MM(n);
  if (d) *out = d->st;
  else memset(out, 0, sizeof *out);
}

/* ── the stamp: meshes under the source ────────────────────────────── */

static void parts_free(multimesh *d) {
  for (int i = 0; i < d->nparts; i++) {
    t3_release(d->parts[i].geometry);
    t3_release(d->parts[i].material);
  }
  d->nparts = 0;
}

static void chunks_free(cw_node n, multimesh *d) {
  t3_object *group = cw_node3d_object(n);
  for (int c = 0; c < d->chunks.count; c++)
    for (int p = 0; p < MAX_PARTS; p++) {
      t3_instanced_mesh *im = d->chunks.data[c].im[p];
      if (im && group && im->mesh.base.parent == group) t3_object_remove(group, (t3_object *)im);
    }
  cw_array_clear(&d->chunks);
}

static cw_node source_of(cw_node n, const multimesh *d) {
  if (d->source && d->source[0]) {
    cw_node s = cw_node_get(n, d->source);
    return cw_node_valid(s) && cw_node_is(s, "Node3D") ? s : (cw_node){ 0 };
  }
  for (int i = 0; i < cw_node_child_count(n); i++)
    if (cw_node_is(cw_node_child(n, i), "MeshInstance3D")) return cw_node_child(n, i);
  return (cw_node){ 0 };
}

static uint32_t sig_walk(const t3_object *o, uint32_t h) {
  if (o->type == T3_MESH) {
    const t3_mesh *m = (const t3_mesh *)o;
    h = cw_hash32(&m->geometry, sizeof m->geometry, h);
    if (m->material_count) h = cw_hash32(&m->materials[0], sizeof m->materials[0], h);
  }
  h = cw_hash32(&o->position, sizeof o->position, h);
  h = cw_hash32(&o->quaternion, sizeof o->quaternion, h);
  h = cw_hash32(&o->scale, sizeof o->scale, h);
  for (int i = 0; i < o->child_count; i++) h = sig_walk(o->children[i], h);
  return h;
}

static void collect(multimesh *d, const t3_object *o, const t3_mat4 *parent) {
  t3_mat4 l, m;
  t3_mat4_compose(&l, o->position, o->quaternion, o->scale);
  t3_mat4_multiply(&m, parent, &l);
  if (o->type == T3_MESH && d->nparts < MAX_PARTS) {
    const t3_mesh *mesh = (const t3_mesh *)o;
    if (mesh->geometry && mesh->material_count) {
      mm_part *p = &d->parts[d->nparts++];
      p->geometry = t3_retain(mesh->geometry);
      p->material = t3_retain(mesh->materials[0]);
      p->local = m;
      static const t3_mat4 I = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
      p->identity = !memcmp(&m, &I, sizeof I);
      if (!mesh->geometry->has_bounding_sphere) t3_geometry_compute_bounding_sphere(mesh->geometry);
      t3_sphere s = mesh->geometry->bounding_sphere;
      t3_vec3 c = t3_vec3_apply_mat4(s.center, &m);
      const float *e = m.e;
      float sx = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), sy = sqrtf(e[4] * e[4] + e[5] * e[5] + e[6] * e[6]),
            sz = sqrtf(e[8] * e[8] + e[9] * e[9] + e[10] * e[10]);
      p->radius = sqrtf(c.x * c.x + c.y * c.y + c.z * c.z) + s.radius * CW_MAX(sx, CW_MAX(sy, sz));
    }
  } else if (o->type == T3_SKINNED_MESH && !d->warned) {
    d->warned = true;
    cw_warn("MultiMeshInstance3D: a skinned mesh under the source is not instanced (static meshes only)");
  }
  for (int i = 0; i < o->child_count; i++) collect(d, o->children[i], &m);
}

/* the parts from the source as it is now; true when they changed */
static bool parts_sync(cw_node n, multimesh *d) {
  cw_node s = source_of(n, d);
  t3_object *so = cw_node_valid(s) ? cw_node3d_object(s) : NULL;
  uint32_t sig = so ? sig_walk(so, CW_HASH32_INIT) : 0;
  if (s.id == d->src_node.id && so == d->src_obj && sig == d->src_sig && !d->parts_dirty) return false;
  d->src_node = s;
  d->src_obj = so;
  d->src_sig = sig;
  d->parts_dirty = false;
  parts_free(d);
  if (so) {
    t3_mat4 I;
    t3_mat4_identity(&I);
    collect(d, so, &I);
    /* the stamp itself is not drawn */
    cw_value v;
    if (cw_node_get_prop(s, "visible", &v) && v.b) cw_node_set(s, "visible", cw_val_bool(false));
  }
  return true;
}

/* ── chunks ────────────────────────────────────────────────────────── */

typedef struct { int64_t key; int idx; } cell_ref;
static int cell_cmp(const void *a, const void *b) {
  const cell_ref *x = a, *y = b;
  return x->key < y->key ? -1 : x->key > y->key ? 1 : x->idx - y->idx;
}

static void fill_grid(multimesh *d) {
  int nx = CW_MAX((int)d->fill_count.x, 1), ny = CW_MAX((int)d->fill_count.y, 1), nz = CW_MAX((int)d->fill_count.z, 1);
  if ((int64_t)nx * ny * nz > 4000000) nx = ny = nz = 1;
  buf_resize(d, nx * ny * nz);
  int i = 0;
  for (int y = 0; y < ny; y++)
    for (int z = 0; z < nz; z++)
      for (int x = 0; x < nx; x++, i++) {
        float *m = d->xf + (size_t)i * 16;
        memset(m, 0, 16 * sizeof(float));
        m[0] = m[5] = m[10] = m[15] = 1;
        m[12] = ((float)x - 0.5f * (float)(nx - 1)) * d->fill_spacing.x;
        m[13] = (float)y * d->fill_spacing.y;
        m[14] = ((float)z - 0.5f * (float)(nz - 1)) * d->fill_spacing.z;
      }
}

static void write_instances(multimesh *d, mm_chunk *c, bool fresh) {
  for (int p = 0; p < d->nparts; p++) {
    t3_instanced_mesh *im = c->im[p];
    const mm_part *part = &d->parts[p];
    float *dst = im->instance_matrix->array;
    for (int k = 0; k < c->count; k++) {
      const float *src = d->xf + (size_t)d->order[c->start + k] * 16;
      if (part->identity) memcpy(dst + (size_t)k * 16, src, 16 * sizeof(float));
      else t3_mat4_multiply((t3_mat4 *)(void *)(dst + (size_t)k * 16), (const t3_mat4 *)(const void *)src, &part->local);
    }
    im->instance_matrix->version++;
    if (d->use_colors) {
      for (int k = 0; k < c->count; k++) {
        const float *col = d->color + (size_t)d->order[c->start + k] * 3;
        t3_instanced_mesh_set_color_at(im, k, (t3_color){ col[0], col[1], col[2] });
      }
      if (im->instance_color) im->instance_color->version++;
    }
    im->count = c->count;
    im->mesh.base.cast_shadow = d->cast_shadow;
    im->mesh.base.receive_shadow = d->receive_shadow;
    t3_instanced_mesh_set_bounding_sphere(im, &c->sphere);
    (void)fresh;
  }
}

static void chunk_bounds(multimesh *d, mm_chunk *c, float part_radius) {
  float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f }, smax = 0;
  for (int k = 0; k < c->count; k++) {
    const float *m = d->xf + (size_t)d->order[c->start + k] * 16;
    for (int a = 0; a < 3; a++) {
      if (m[12 + a] < mn[a]) mn[a] = m[12 + a];
      if (m[12 + a] > mx[a]) mx[a] = m[12 + a];
    }
    float s = CW_MAX(m[0] * m[0] + m[1] * m[1] + m[2] * m[2], CW_MAX(m[4] * m[4] + m[5] * m[5] + m[6] * m[6], m[8] * m[8] + m[9] * m[9] + m[10] * m[10]));
    if (s > smax) smax = s;
  }
  float ex = (mx[0] - mn[0]) * 0.5f, ey = (mx[1] - mn[1]) * 0.5f, ez = (mx[2] - mn[2]) * 0.5f;
  c->sphere = (t3_sphere){ { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f },
                           sqrtf(ex * ex + ey * ey + ez * ez) + sqrtf(smax) * part_radius };
}

static void rebuild(cw_node n, multimesh *d) {
  double t0 = cw__render3d_now_ms();
  t3_object *group = cw_node3d_object(n);
  int total = d->visible_instance_count >= 0 ? CW_MIN(d->visible_instance_count, d->count) : d->count;
  float part_radius = 0;
  for (int p = 0; p < d->nparts; p++) part_radius = CW_MAX(part_radius, d->parts[p].radius);
  if (total > d->order_cap) {
    d->order = cw_realloc(d->order, (size_t)total * sizeof(int), CW_MEM_RENDER);
    d->order_cap = total;
  }
  /* one draw for everything, the same instances as last time: rewrite the buffers in place */
  bool single = !(d->chunk_size > 0);
  if (single && d->chunks.count == 1 && d->chunks.data[0].count == total && total && d->nparts && d->chunks.data[0].im[d->nparts - 1] &&
      d->built_colors == d->use_colors && d->built_chunk == d->chunk_size) {
    mm_chunk *c = &d->chunks.data[0];
    chunk_bounds(d, c, part_radius);
    write_instances(d, c, false);
    d->st.build_ms = (float)(cw__render3d_now_ms() - t0);
    d->st.builds++;
    return;
  }
  chunks_free(n, d);
  d->built_chunk = d->chunk_size;
  d->built_colors = d->use_colors;
  d->st.instances = total;
  if (!total || !d->nparts || !group) {
    d->st.chunks = 0;
    d->st.parts = d->nparts;
    return;
  }
  if (single) {
    for (int i = 0; i < total; i++) d->order[i] = i;
    mm_chunk c = { .start = 0, .count = total };
    cw_array_push(&d->chunks, c);
  } else {
    cell_ref *refs = cw_alloc((size_t)total * sizeof *refs, CW_MEM_RENDER);
    float inv = 1.0f / d->chunk_size;
    for (int i = 0; i < total; i++) {
      const float *m = d->xf + (size_t)i * 16;
      int64_t cx = (int64_t)floorf(m[12] * inv) + (1 << 20), cy = (int64_t)floorf(m[13] * inv) + (1 << 20), cz = (int64_t)floorf(m[14] * inv) + (1 << 20);
      refs[i] = (cell_ref){ (cy << 42) | (cz << 21) | cx, i };
    }
    qsort(refs, (size_t)total, sizeof *refs, cell_cmp);
    for (int i = 0; i < total; i++) {
      d->order[i] = refs[i].idx;
      if (!i || refs[i].key != refs[i - 1].key) {
        mm_chunk c = { .start = i, .count = 0 };
        cw_array_push(&d->chunks, c);
      }
      d->chunks.data[d->chunks.count - 1].count++;
    }
    cw_free(refs);
  }
  for (int ci = 0; ci < d->chunks.count; ci++) {
    mm_chunk *c = &d->chunks.data[ci];
    chunk_bounds(d, c, part_radius);
    for (int p = 0; p < d->nparts; p++) {
      t3_instanced_mesh *im = t3_instanced_mesh_new(d->parts[p].geometry, d->parts[p].material, c->count);
      if (single) im->instance_matrix->usage = T3_DYNAMIC_DRAW;
      c->im[p] = im;
      t3_object_add(group, (t3_object *)im);
      t3_release(im);
    }
    write_instances(d, c, true);
  }
  d->st.chunks = d->chunks.count;
  d->st.parts = d->nparts;
  d->st.build_ms = (float)(cw__render3d_now_ms() - t0);
  d->st.builds++;
}

/* which cells the main camera sees (three.c culls each draw itself; this is the count a game reads) */
static void count_visible(cw_node n, multimesh *d) {
  d->st.visible_chunks = d->st.visible_instances = d->st.draws = 0;
  cw_tree *t = cw_node_tree(n);
  t3_camera *cam = t ? cw_camera3d_t3(cw_render3d_camera(t)) : NULL;
  const t3_mat4 *g = cw_node3d_global_transform(n);
  if (!cam || !g) return;
  t3_mat4 vp;
  t3_frustum f;
  t3_mat4_multiply(&vp, &cam->projection_matrix, &cam->matrix_world_inverse);
  t3_frustum_from_matrix(&f, &vp);
  const float *e = g->e;
  float sc = sqrtf(CW_MAX(e[0] * e[0] + e[1] * e[1] + e[2] * e[2], CW_MAX(e[4] * e[4] + e[5] * e[5] + e[6] * e[6], e[8] * e[8] + e[9] * e[9] + e[10] * e[10])));
  for (int c = 0; c < d->chunks.count; c++) {
    const mm_chunk *ch = &d->chunks.data[c];
    t3_sphere s = { t3_vec3_apply_mat4(ch->sphere.center, g), ch->sphere.radius * sc };
    if (!t3_frustum_intersects_sphere(&f, s)) continue;
    d->st.visible_chunks++;
    d->st.visible_instances += ch->count;
    d->st.draws += d->nparts;
  }
}

/* ── the node type ─────────────────────────────────────────────────── */

static void mm_init(cw_node n, void *data) {
  (void)n;
  multimesh *d = data;
  d->visible_instance_count = -1;
  d->chunk_size = 16;
  d->cast_shadow = false;
  d->receive_shadow = true;
  d->fill_count = (t3_vec3){ 1, 1, 1 };
  d->fill_spacing = (t3_vec3){ 1, 1, 1 };
  d->dirty = true;
}
static void mm_destroy(cw_node n, void *data) {
  multimesh *d = data;
  chunks_free(n, d);
  cw_array_free(&d->chunks);
  parts_free(d);
  cw_free(d->xf);
  cw_free(d->color);
  cw_free(d->custom);
  cw_free(d->order);
}
static void mm_prop_changed(cw_node n, void *data, const cw_prop *prop) {
  (void)n;
  multimesh *d = data;
  if (!strcmp(prop->name, "instance_count")) {
    int want = d->instance_count;
    d->instance_count = d->count;
    buf_resize(d, want);
  }
  if (!strcmp(prop->name, "source")) d->parts_dirty = true;
  d->dirty = true;
}
static void mm_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  multimesh *d = data;
  if (d->fill == 1) {
    int sig = (int)cw_hash32(&d->fill_count, sizeof(t3_vec3) * 2, CW_HASH32_INIT) | 1;
    if (sig != d->built_fill_sig) {
      d->built_fill_sig = sig;
      fill_grid(d);
    }
  }
  if (parts_sync(n, d)) { /* other meshes: the draws are made again */
    chunks_free(n, d);
    d->dirty = true;
  }
  int vis = d->visible_instance_count;
  if (d->dirty || vis != d->built_visible) {
    d->dirty = false;
    d->built_visible = vis;
    rebuild(n, d);
  }
  count_visible(n, d);
}

static void ro_vis(cw_node n, void *data, cw_value *o) { (void)n; *o = cw_val_int(((multimesh *)data)->st.visible_instances); }
static void ro_chunks(cw_node n, void *data, cw_value *o) { (void)n; *o = cw_val_int(((multimesh *)data)->st.chunks); }
static void ro_vchunks(cw_node n, void *data, cw_value *o) { (void)n; *o = cw_val_int(((multimesh *)data)->st.visible_chunks); }

static const char *const fill_names[] = { "NONE", "GRID", NULL };
static const cw_prop mm_props[] = {
  CW_PROP_FIELD(multimesh, source, CW_T_STRING, CW_PROP_DEFAULT, "Node3D (path from this node) whose meshes are drawn per instance; empty: the first MeshInstance3D child. It is hidden."),
  CW_PROP_FIELD_RANGE(multimesh, instance_count, CW_T_INT, 0, 4000000, "Instances in the buffer (new ones are identity, white)."),
  CW_PROP_FIELD_RANGE(multimesh, visible_instance_count, CW_T_INT, -1, 4000000, "Draw only the first N instances (-1: all)."),
  CW_PROP_FIELD_RANGE(multimesh, chunk_size, CW_T_FLOAT, 0, 100000, "Cell size in metres: each cell is culled as one draw (0: one draw for everything)."),
  CW_PROP_FIELD(multimesh, cast_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Instances cast shadows."),
  CW_PROP_FIELD(multimesh, receive_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Instances receive shadows."),
  CW_PROP_FIELD(multimesh, use_colors, CW_T_BOOL, CW_PROP_DEFAULT, "Multiply each instance's colour (cw_multimesh_set_color) into the material's."),
  { .name = "fill", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(multimesh, fill), .size = sizeof(int32_t), .enum_names = fill_names,
    .doc = "GRID fills the buffer with fill_count instances fill_spacing apart, centred in x and z (no code needed); NONE leaves it to the game." },
  CW_PROP_FIELD(multimesh, fill_count, CW_T_VEC3, CW_PROP_DEFAULT, "GRID: instances along x, y and z."),
  CW_PROP_FIELD(multimesh, fill_spacing, CW_T_VEC3, CW_PROP_DEFAULT, "GRID: metres between instances along x, y and z."),
  CW_PROP_ACCESSOR("visible_instances", CW_T_INT, CW_PROP_READONLY, ro_vis, NULL, "Instances in the cells the main camera saw last frame."),
  CW_PROP_ACCESSOR("chunks", CW_T_INT, CW_PROP_READONLY, ro_chunks, NULL, "Cells holding instances."),
  CW_PROP_ACCESSOR("visible_chunks", CW_T_INT, CW_PROP_READONLY, ro_vchunks, NULL, "Cells inside the main camera's view last frame."),
  CW_PROP_END,
};
static cw_node_type mm_type = {
  .name = "MultiMeshInstance3D", .parent = "Node3D", .data_size = sizeof(multimesh), .props = mm_props,
  .doc = "Many instances of the meshes under a source node, from an instance buffer the game fills; culled by cell, lit and shadowed like any mesh.",
  .init = mm_init, .destroy = mm_destroy, .prop_changed = mm_prop_changed, .pre_render = mm_pre_render,
};

static void cmd_multimesh(int argc, const char **argv, cw_reply *r) {
  cw_tree *t = cw_current_tree();
  cw_node n = argc > 1 && t ? cw_node_get(cw_tree_root(t), argv[1]) : (cw_node){ 0 };
  multimesh *d = MM(n);
  if (!d) { cw_reply_error(r, "usage: multimesh <node>: a MultiMeshInstance3D by path (try: tree)"); return; }
  cw_reply_printf(r, "{\"instances\":%d,\"buffer\":%d,\"parts\":%d,\"chunks\":%d,\"visible_chunks\":%d,\"visible_instances\":%d,\"draws\":%d,\"builds\":%d,\"build_ms\":%.3f}",
                  d->st.instances, d->count, d->st.parts, d->st.chunks, d->st.visible_chunks, d->st.visible_instances, d->st.draws, d->st.builds,
                  (double)d->st.build_ms);
}

void cw__register_multimesh_types(void) {
  cw_node_type_register(&mm_type);
  mm_t = &mm_type;
  cw_debug_command("multimesh", "multimesh <node>: a MultiMeshInstance3D's instances, cells, cells and instances in view, draws, last rebuild ms", cmd_multimesh);
}

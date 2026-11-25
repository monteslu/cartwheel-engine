/* Faces that fight for the depth test: the scan, the separation and the debug command (cartwheel/coplanar.h). */
#include "cartwheel/coplanar.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"

static int g_moved_total;
static double g_last_ms;
int cw_coplanar_moved_total(void) { return g_moved_total; }
void cw_coplanar_add_moved(int n) { g_moved_total += n; }
double cw_coplanar_last_ms(void) { return g_last_ms; }
static int g_pairs_last = -1; /* level.coplanar_pairs: the last scan's count (-1 before one) */

/* the plane test: a face's corners within PLANE_TOL of the other's plane (1 mm unless the game sets its own,
 * cw_coplanar_set_plane_tol); overlap: the two overlap by more than OVERLAP_TOL across every edge (neighbours sharing an edge
 * or a corner do not count) */
static float PLANE_TOL = 0.001f;
void cw_coplanar_set_plane_tol(float m) { PLANE_TOL = m; }
/* the depth buffer's own limit (cw_coplanar_set_view): GL's 24-bit depth (WebGPU's 32-bit float is no finer, not reversed)
 * tells two depths apart about d^2 / (near * 2^24) apart at distance d; faces within four of those steps count as one
 * plane, and the separation lays them at least two such tolerances apart */
static struct { bool on; float near, c[3], reach; } VIEW;
void cw_coplanar_set_view(float near, t3_vec3 center, float reach) { VIEW.on = true, VIEW.near = near, VIEW.c[0] = center.x, VIEW.c[1] = center.y, VIEW.c[2] = center.z, VIEW.reach = reach; }
static float tol_at(const float *p) {
  if (!VIEW.on) return PLANE_TOL;
  float d = sqrtf((p[0] - VIEW.c[0]) * (p[0] - VIEW.c[0]) + (p[1] - VIEW.c[1]) * (p[1] - VIEW.c[1]) + (p[2] - VIEW.c[2]) * (p[2] - VIEW.c[2])) + VIEW.reach;
  /* (at most 2.5 cm: further out than about a hundred metres the haze has the picture, and a far shore meeting the sea at
   * a slant would count as one plane with it) */
  return fmaxf(PLANE_TOL, fminf(4.0f * d * d / (VIEW.near * 16777216.0f), 0.025f));
}
#define OVERLAP_TOL 0.0002f
#define SAME_FACING 0.999f

typedef struct ctri {
  float p[3][3];
  float n[3], d, area;
  float lo[3], hi[3];
  int mesh;          /* into the mesh list */
  int t;             /* the triangle in its geometry (first index / 3) */
  int inst;          /* the instance (0 for a plain mesh) */
  bool two;          /* drawn from both sides */
  bool writes;       /* writes depth (two faces that both leave depth alone are ordered by the draw, not the test) */
  float tol;         /* how near another face's plane counts as this one's (tol_at its middle) */
  const t3_material *mat;
  float vn[3];       /* its corners' shading normals, summed and normalised (world) */
  float uv[3][2], col[3][3]; /* its corners' texture coordinates and colours (when it has them) */
  bool has_uv, has_col;
} ctri;

typedef struct cmesh {
  t3_mesh *m;
  int region;
  t3_mat4 world, inv;
} cmesh;

typedef struct cset {
  ctri *t; int n, cap;
  cmesh *m; int nm, capm;
  char (*rname)[96]; int nr, capr;
} cset;

static void *grow(void *p, int *cap, int need, size_t sz) {
  if (need <= *cap) return p;
  int c = *cap ? *cap : 256;
  while (c < need) c *= 2;
  p = realloc(p, sz * (size_t)c);
  if (!p) abort();
  *cap = c;
  return p;
}

static void xf(const t3_mat4 *m, const float *v, float *o) {
  const float *e = m->e;
  o[0] = e[0] * v[0] + e[4] * v[1] + e[8] * v[2] + e[12];
  o[1] = e[1] * v[0] + e[5] * v[1] + e[9] * v[2] + e[13];
  o[2] = e[2] * v[0] + e[6] * v[1] + e[10] * v[2] + e[14];
}
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void xf_dir(const t3_mat4 *m, const float *v, float *o) {
  const float *e = m->e;
  o[0] = e[0] * v[0] + e[4] * v[1] + e[8] * v[2];
  o[1] = e[1] * v[0] + e[5] * v[1] + e[9] * v[2];
  o[2] = e[2] * v[0] + e[6] * v[1] + e[10] * v[2];
}

static bool tri_fill(ctri *q, const float a[3], const float b[3], const float c[3]) {
  float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
  float x[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
  float l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
  if (!(l > 1e-9f)) return false;
  memcpy(q->p[0], a, 12), memcpy(q->p[1], b, 12), memcpy(q->p[2], c, 12);
  for (int i = 0; i < 3; i++) {
    q->n[i] = x[i] / l;
    q->lo[i] = fminf(a[i], fminf(b[i], c[i])), q->hi[i] = fmaxf(a[i], fmaxf(b[i], c[i]));
  }
  q->d = q->n[0] * a[0] + q->n[1] * a[1] + q->n[2] * a[2];
  memcpy(q->vn, q->n, sizeof q->vn), q->mat = NULL, q->has_uv = q->has_col = false;
  q->area = l * 0.5f;
  return true;
}

/* the scene tree's 3D nodes by the object each drives: a region is named for the node a mesh hangs under */
typedef struct objname { const t3_object *o; char name[96]; } objname;
static objname *ON;
static int n_on, cap_on;
static int on_cmp(const void *pa, const void *pb) {
  const objname *x = pa, *y = pb;
  return x->o < y->o ? -1 : x->o > y->o;
}
static bool on_visit(cw_node n, void *u) {
  cw_node root = *(cw_node *)u;
  t3_object *o = cw_node3d_object(n);
  if (!o) return true;
  /* the node's transform and visibility into its object now, as the next frame's pre_render will (a game places its
   * nodes as it builds; three.c sees that only when a frame is drawn) */
  cw_value v;
  o->position = cw_node3d_position(n);
  t3_object_set_quaternion(o, cw_node3d_quaternion(n));
  if (cw_node_get_prop(n, "scale", &v)) o->scale = (t3_vec3){ v.v[0], v.v[1], v.v[2] };
  if (cw_node_get_prop(n, "visible", &v)) o->visible = v.b;
  char path[256];
  if (cw_node_path(n, root, path, sizeof path) <= 0) return true;
  /* the first two names of the path, and the node's own when it is deeper (digits dropped: "Insert12" is "Insert") */
  char out[96] = "";
  int len = 0, depth = 0;
  const char *p = path, *last = path;
  for (const char *q = path; *q; q++)
    if (*q == '/') last = q + 1;
  while (*p && depth < 2) {
    while (*p == '/') p++;
    const char *e = p;
    while (*e && *e != '/') e++;
    if (e > p) len += snprintf(out + len, sizeof out - (size_t)len, "%s%.*s", len ? "/" : "", (int)(e - p), p), depth++;
    p = e;
  }
  if (*p && len < (int)sizeof out - 2) len += snprintf(out + len, sizeof out - (size_t)len, ":%s", last);
  while (len > 0 && out[len - 1] >= '0' && out[len - 1] <= '9') out[--len] = 0;
  ON = grow(ON, &cap_on, n_on + 1, sizeof *ON);
  ON[n_on].o = o;
  memcpy(ON[n_on].name, out, sizeof out);
  n_on++;
  return true;
}
static void names_build(void) {
  n_on = 0;
  cw_tree *t = cw_scene_app_tree();
  if (!t) return;
  cw_node root = cw_tree_root(t);
  cw_node_walk(root, on_visit, &root);
  if (n_on) qsort(ON, (size_t)n_on, sizeof *ON, on_cmp);
}
static const char *name_of(const t3_object *o) {
  if (!n_on) return NULL;
  objname key = { .o = o };
  const objname *f = bsearch(&key, ON, (size_t)n_on, sizeof *ON, on_cmp);
  return f && f->name[0] ? f->name : NULL;
}

static int region_of(cset *s, const t3_object *o, const t3_object *root) {
  char nm[96] = "";
  const char *node = NULL;
  for (const t3_object *a = o; a && a != root && !node; a = a->parent) node = name_of(a);
  if (node) snprintf(nm, sizeof nm, "%s", node);
  else if (o->name && o->name[0]) snprintf(nm, sizeof nm, "%s", o->name);
  else snprintf(nm, sizeof nm, "(unnamed)");
  for (int i = 0; i < s->nr; i++)
    if (!strcmp(s->rname[i], nm)) return i;
  s->rname = grow(s->rname, &s->capr, s->nr + 1, sizeof *s->rname);
  memcpy(s->rname[s->nr], nm, sizeof nm);
  return s->nr++;
}

static uint32_t idx_at(const t3_attribute *ix, int i) {
  return ix->type == T3_UINT16 ? ((const uint16_t *)ix->array)[i] : ix->type == T3_UINT32 ? ((const uint32_t *)ix->array)[i] : (uint32_t)((const float *)ix->array)[i];
}

static bool mat_drawn(const t3_material *m) { return m && m->visible && m->depth_test && m->color_write; }

/* one mesh's triangles into s (instances: one copy per instance); movable: the separation's set (no morph targets) */
static void add_mesh(cset *s, t3_mesh *m, const t3_object *root, bool movable) {
  t3_object *o = &m->base;
  if (o->type == T3_SKINNED_MESH) return;
  t3_geometry *g = m->geometry;
  if (!g || !g->attributes[T3_ATTR_POSITION] || g->attributes[T3_ATTR_POSITION]->type != T3_FLOAT32) return;
  if (movable && g->morph_count) return;
  const t3_attribute *pa = g->attributes[T3_ATTR_POSITION];
  const float *P = pa->array;
  const int ps = pa->item_size;
  const t3_attribute *ix = g->index;
  int total = ix ? ix->count : pa->count;
  int start = g->draw_start > 0 ? g->draw_start : 0, end = total;
  if (g->draw_count >= 0 && start + g->draw_count < end) end = start + g->draw_count;
  int ninst = 1;
  t3_instanced_mesh *im = NULL;
  if (o->type == T3_INSTANCED_MESH) im = (t3_instanced_mesh *)m, ninst = im->count;
  if (ninst <= 0) return;
  s->m = grow(s->m, &s->capm, s->nm + 1, sizeof *s->m);
  cmesh *cm = &s->m[s->nm];
  cm->m = m, cm->region = region_of(s, o, root), cm->world = o->matrix_world;
  t3_mat4_invert(&cm->inv, &o->matrix_world);
  int mi = s->nm++;
  for (int k = 0; k < ninst; k++) {
    t3_mat4 w = o->matrix_world;
    if (im) {
      t3_mat4 inst;
      t3_instanced_mesh_get_matrix_at(im, k, &inst);
      t3_mat4_multiply(&w, &o->matrix_world, &inst);
    }
    for (int i = start; i + 2 < end; i += 3) {
      const t3_material *mat = m->material_count ? m->materials[0] : NULL;
      if (g->group_count && m->material_count > 1) {
        mat = NULL;
        for (int gi = 0; gi < g->group_count; gi++)
          if (i >= g->groups[gi].start && i < g->groups[gi].start + g->groups[gi].count) {
            int mx = g->groups[gi].material_index;
            mat = mx >= 0 && mx < m->material_count ? m->materials[mx] : NULL;
            break;
          }
      }
      if (!mat_drawn(mat)) continue;
      float v[3][3];
      for (int j = 0; j < 3; j++) {
        uint32_t vi = ix ? idx_at(ix, i + j) : (uint32_t)(i + j);
        if ((int)vi >= pa->count) goto skip;
        xf(&w, P + (size_t)vi * (size_t)ps, v[j]);
      }
      s->t = grow(s->t, &s->cap, s->n + 1, sizeof *s->t);
      if (tri_fill(&s->t[s->n], v[0], v[1], v[2])) {
        ctri *q = &s->t[s->n++];
        q->mesh = mi, q->t = i / 3, q->inst = k, q->two = mat->side == T3_DOUBLE_SIDE, q->writes = mat->depth_write;
        float mid[3] = { (v[0][0] + v[1][0] + v[2][0]) / 3, (v[0][1] + v[1][1] + v[2][1]) / 3, (v[0][2] + v[1][2] + v[2][2]) / 3 };
        q->tol = tol_at(mid);
        q->mat = mat, q->vn[0] = q->vn[1] = q->vn[2] = 0;
        const t3_attribute *na = g->attributes[T3_ATTR_NORMAL];
        if (na && na->type == T3_FLOAT32 && na->item_size >= 3) {
          for (int j = 0; j < 3; j++) {
            uint32_t vi = ix ? idx_at(ix, i + j) : (uint32_t)(i + j);
            float wn[3];
            xf_dir(&w, (const float *)na->array + (size_t)vi * (size_t)na->item_size, wn);
            for (int k2 = 0; k2 < 3; k2++) q->vn[k2] += wn[k2];
          }
          float l2 = sqrtf(dot3(q->vn, q->vn));
          for (int k2 = 0; k2 < 3; k2++) q->vn[k2] = l2 > 1e-9f ? q->vn[k2] / l2 : q->n[k2];
        } else memcpy(q->vn, q->n, sizeof q->vn);
        const t3_attribute *ua = g->attributes[T3_ATTR_UV], *ca = g->attributes[T3_ATTR_COLOR];
        q->has_uv = ua && ua->type == T3_FLOAT32 && ua->item_size >= 2, q->has_col = ca && ca->type == T3_FLOAT32 && ca->item_size >= 3;
        for (int j = 0; j < 3; j++) {
          uint32_t vi = ix ? idx_at(ix, i + j) : (uint32_t)(i + j);
          if (q->has_uv) memcpy(q->uv[j], (const float *)ua->array + (size_t)vi * (size_t)ua->item_size, 8);
          if (q->has_col) memcpy(q->col[j], (const float *)ca->array + (size_t)vi * (size_t)ca->item_size, 12);
        }
      }
    skip:;
    }
  }
}

static void walk(cset *s, t3_object *o, const t3_object *root, bool movable) {
  if (!o->visible) return;
  if (o->type == T3_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SKINNED_MESH) add_mesh(s, (t3_mesh *)o, root, movable);
  for (int i = 0; i < o->child_count; i++) walk(s, o->children[i], root, movable);
}

/* ── the pair search ───────────────────────────────────────────────── */
static void basis(const float *n, float *u, float *v) {
  float a[3] = { fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0 };
  u[0] = a[1] * n[2] - a[2] * n[1], u[1] = a[2] * n[0] - a[0] * n[2], u[2] = a[0] * n[1] - a[1] * n[0];
  float l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
  for (int i = 0; i < 3; i++) u[i] /= l;
  v[0] = n[1] * u[2] - n[2] * u[1], v[1] = n[2] * u[0] - n[0] * u[2], v[2] = n[0] * u[1] - n[1] * u[0];
}

/* the two faces lie in one plane, face the same way (or one is drawn from both sides) and overlap by more than a
 * sliver: they fight for the depth test */
static float fights(const ctri *a, const ctri *b) { /* how far they overlap (the narrowest way across), 0: they do not fight */
  float c = dot3(a->n, b->n), w = 1e30f;
  if (!a->writes && !b->writes) return 0;
  const float ptol = fmaxf(a->tol, b->tol);
  if (!(c > SAME_FACING || ((a->two || b->two) && c < -SAME_FACING))) return 0;
  for (int j = 0; j < 3; j++) {
    if (fabsf(dot3(a->n, b->p[j]) - a->d) > ptol) return 0;
    if (fabsf(dot3(b->n, a->p[j]) - b->d) > ptol) return 0;
  }
  /* (far out, a float cannot place a corner closer than a few parts in ten million of its distance: an overlap below
   * that is the rounding of two neighbours, not two surfaces) */
  float far = 0, u[3], v[3], A[3][2], B[3][2];
  for (int j = 0; j < 3; j++)
    for (int k = 0; k < 3; k++) far = fmaxf(far, fmaxf(fabsf(a->p[j][k]), fabsf(b->p[j][k])));
  const float tol = fmaxf(OVERLAP_TOL, far * 5e-7f);
  basis(a->n, u, v);
  for (int j = 0; j < 3; j++) A[j][0] = dot3(a->p[j], u), A[j][1] = dot3(a->p[j], v), B[j][0] = dot3(b->p[j], u), B[j][1] = dot3(b->p[j], v);
  for (int s = 0; s < 2; s++) {
    float(*T)[2] = s ? B : A;
    for (int e = 0; e < 3; e++) {
      float ax = -(T[(e + 1) % 3][1] - T[e][1]), ay = T[(e + 1) % 3][0] - T[e][0], l = sqrtf(ax * ax + ay * ay);
      if (l < 1e-9f) return 0;
      ax /= l, ay /= l;
      float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
      for (int j = 0; j < 3; j++) {
        float pa = A[j][0] * ax + A[j][1] * ay, pb = B[j][0] * ax + B[j][1] * ay;
        a0 = fminf(a0, pa), a1 = fmaxf(a1, pa), b0 = fminf(b0, pb), b1 = fmaxf(b1, pb);
      }
      float o = fminf(a1, b1) - fmaxf(a0, b0);
      if (o < tol) return 0;
      w = fminf(w, o);
    }
  }
  return w;
}

typedef struct cell_e { uint64_t key; int tri; } cell_e;
static int cell_cmp(const void *pa, const void *pb) {
  const cell_e *x = pa, *y = pb;
  return x->key != y->key ? (x->key < y->key ? -1 : 1) : (x->tri < y->tri ? -1 : x->tri > y->tri);
}
static int flt_cmp(const void *pa, const void *pb) { float a = *(const float *)pa, b = *(const float *)pb; return a < b ? -1 : a > b; }
typedef struct cpair { int a, b; float w; } cpair;

/* every fighting pair of s's faces, through a grid over the world (cells about twice the typical face) */
/* the switches (COPLANAR_CONTROL, COPLANAR_SEPARATE, ...): the environment natively, or what the game reads its run
 * options from (cw_coplanar_set_options: a cart reads no environment) */
static const char *env_get(const char *name) { return getenv(name); }
static const char *(*opt_source)(const char *) = env_get;
void cw_coplanar_set_options(const char *(*get)(const char *)) { opt_source = get ? get : env_get; }

static int find_pairs(const cset *s, cpair **out) {
  *out = NULL;
  if (s->n < 2) return 0;
  float lo[3] = { 1e30f, 1e30f, 1e30f };
  float *sz = malloc(sizeof(float) * (size_t)s->n);
  if (!sz) abort();
  for (int i = 0; i < s->n; i++) {
    float m = 0;
    for (int k = 0; k < 3; k++) lo[k] = fminf(lo[k], s->t[i].lo[k] - s->t[i].tol), m = fmaxf(m, s->t[i].hi[k] - s->t[i].lo[k]);
    sz[i] = m;
  }
  qsort(sz, (size_t)s->n, sizeof(float), flt_cmp);
  float cell = fmaxf(sz[s->n / 2] * 2.0f, 0.01f);
  free(sz);
  /* a coarser grid when the faces would fill too many cells: the grid only picks the candidate pairs (every pair that
   * shares a cell is tested), so a coarser one finds the same pairs through more tests. The entries are bounded by
   * memory, not time: a model whose typical face is tiny beside its largest (a snow kit: needles and drifts) would
   * put each large face in millions of 1 cm cells, 16 bytes each, and a cart has one gigabyte in all (such a kit
   * trapped the cart in realloc, the native build having the memory). COPLANAR_CELL_CAP (development) sets the cap:
   * the map check's control runs a snow map with the old 6e7 and must trap. */
  double limit = 1e6;
  if (opt_source("COPLANAR_CELL_CAP") && atof(opt_source("COPLANAR_CELL_CAP")) > 0) limit = atof(opt_source("COPLANAR_CELL_CAP")); /* a cart reads its run options, not the environment (cw_coplanar_set_options) */
  for (;;) {
    double cells = 0;
    for (int i = 0; i < s->n && cells < limit; i++) {
      double c = 1;
      for (int k = 0; k < 3; k++) c *= floor((s->t[i].hi[k] + s->t[i].tol - lo[k]) / cell) - floor((s->t[i].lo[k] - s->t[i].tol - lo[k]) / cell) + 1;
      cells += c;
    }
    if (cells < limit) break;
    cell *= 2;
  }
  int ne = 0, cap = 0;
  cell_e *E = NULL;
#define CI(x, k) ((int64_t)floorf(((x) - lo[k]) / cell))
#define KEY(x, y, z) (((uint64_t)(x) << 42) | ((uint64_t)(y) << 21) | (uint64_t)(z))
  for (int i = 0; i < s->n; i++) {
    const ctri *q = &s->t[i];
    const float e = q->tol;
    int64_t a0 = CI(q->lo[0] - e, 0), a1 = CI(q->hi[0] + e, 0), b0 = CI(q->lo[1] - e, 1), b1 = CI(q->hi[1] + e, 1), c0 = CI(q->lo[2] - e, 2), c1 = CI(q->hi[2] + e, 2);
    for (int64_t x = a0; x <= a1; x++)
      for (int64_t y = b0; y <= b1; y++)
        for (int64_t z = c0; z <= c1; z++) {
          E = grow(E, &cap, ne + 1, sizeof *E);
          E[ne++] = (cell_e){ KEY(x + 1, y + 1, z + 1), i };
        }
  }
  qsort(E, (size_t)ne, sizeof *E, cell_cmp);
  int np = 0, pcap = 0;
  cpair *P = NULL;
  for (int s0 = 0; s0 < ne;) {
    int e = s0 + 1;
    while (e < ne && E[e].key == E[s0].key) e++;
    for (int i = s0; i < e; i++)
      for (int j = i + 1; j < e; j++) {
        const ctri *a = &s->t[E[i].tri], *b = &s->t[E[j].tri];
        bool apart = false;
        float m[3];
        for (int k = 0; k < 3; k++) {
          if (a->hi[k] + a->tol + b->tol < b->lo[k] || b->hi[k] + a->tol + b->tol < a->lo[k]) apart = true;
          m[k] = fmaxf(a->lo[k] - a->tol, b->lo[k] - b->tol);
        }
        if (apart) continue;
        if (KEY(CI(m[0], 0) + 1, CI(m[1], 1) + 1, CI(m[2], 2) + 1) != E[s0].key) continue; /* (the pair once: in its first shared cell) */
        float w = fights(a, b);
        if (!(w > 0)) continue;
        P = grow(P, &pcap, np + 1, sizeof *P);
        P[np++] = (cpair){ E[i].tri, E[j].tri, w };
      }
    s0 = e;
  }
#undef CI
#undef KEY
  free(E);
  *out = P;
  return np;
}

static void cset_free(cset *s) { free(s->t), free(s->m), free(s->rname); memset(s, 0, sizeof *s); }

static int region_cmp(const void *pa, const void *pb) {
  const cw_coplanar_region *x = pa, *y = pb;
  return x->pairs != y->pairs ? (x->pairs > y->pairs ? -1 : 1) : strcmp(x->name, y->name);
}

static void report(const cset *s, const cpair *P, int np, cw_coplanar_report *out) {
  memset(out, 0, sizeof *out);
  out->meshes = s->nm, out->faces = s->n, out->pairs = np;
  /* by region: a pair across two regions is named for both ("a + b") */
  int nr = 0, cap = 0;
  cw_coplanar_region *R = NULL;
  for (int i = 0; i < np; i++) {
    const ctri *a = &s->t[P[i].a], *b = &s->t[P[i].b];
    int ra = s->m[a->mesh].region, rb = s->m[b->mesh].region;
    if (ra > rb) { int t = ra; ra = rb, rb = t; }
    char nm[96];
    if (ra == rb) snprintf(nm, sizeof nm, "%s", s->rname[ra]);
    else snprintf(nm, sizeof nm, "%.46s + %.46s", s->rname[ra], s->rname[rb]);
    int r = 0;
    while (r < nr && strcmp(R[r].name, nm)) r++;
    if (r == nr) {
      R = grow(R, &cap, nr + 1, sizeof *R);
      memset(&R[nr], 0, sizeof *R);
      snprintf(R[nr].name, sizeof R[nr].name, "%s", nm);
      nr++;
    }
    R[r].pairs++;
    if (P[i].w > R[r].width) { /* where the widest overlap is */
      R[r].width = P[i].w;
      for (int k = 0; k < 3; k++) R[r].at[k] = (a->p[0][k] + a->p[1][k] + a->p[2][k] + b->p[0][k] + b->p[1][k] + b->p[2][k]) / 6.0f;
    }
  }
  if (nr) qsort(R, (size_t)nr, sizeof *R, region_cmp);
  out->nregions = nr < 48 ? nr : 48;
  if (nr) memcpy(out->regions, R, sizeof *R * (size_t)out->nregions);
  free(R);
}

/* the control's doubled face: two 1 m squares in one plane, one cut corner to corner the other way */
static void add_control(cset *s) {
  static const float a[4][3] = { { 0, 0.5f, 0 }, { 1, 0.5f, 0 }, { 1, 0.5f, 1 }, { 0, 0.5f, 1 } };
  s->m = grow(s->m, &s->capm, s->nm + 1, sizeof *s->m);
  memset(&s->m[s->nm], 0, sizeof *s->m);
  s->rname = grow(s->rname, &s->capr, s->nr + 1, sizeof *s->rname);
  snprintf(s->rname[s->nr], sizeof s->rname[0], "COPLANAR_CONTROL");
  s->m[s->nm].region = s->nr++;
  const int tris[4][3] = { { 0, 2, 1 }, { 0, 3, 2 }, { 1, 0, 3 }, { 1, 3, 2 } };
  for (int i = 0; i < 4; i++) {
    s->t = grow(s->t, &s->cap, s->n + 1, sizeof *s->t);
    if (tri_fill(&s->t[s->n], a[tris[i][0]], a[tris[i][1]], a[tris[i][2]])) s->t[s->n].mesh = s->nm, s->t[s->n].t = i, s->t[s->n].inst = 0, s->t[s->n].two = false, s->t[s->n].writes = true, s->t[s->n].tol = PLANE_TOL, s->n++;
  }
  s->nm++;
}

static bool env_on(const char *name, bool dflt) {
  const char *v = opt_source(name);
  return v && v[0] ? strcmp(v, "0") != 0 : dflt;
}

static int fighting_pairs(const cset *s, cpair **out);
void cw_coplanar_scan(t3_object *root, cw_coplanar_report *out) {
  cset s = { 0 };
  if (root) {
    t3_object_update_matrix_world(root, false);
    names_build();
    walk(&s, root, root, false);
  }
  if (env_on("COPLANAR_CONTROL", false)) add_control(&s);
  cpair *P;
  int np = fighting_pairs(&s, &P);
  report(&s, P, np, out);
  const char *dump = opt_source("COPLANAR_DUMP"); /* development: the first n pairs, face by face, on stderr */
  for (int i = 0, n = dump ? atoi(dump) : 0; i < np && i < n; i++)
    for (int k = 0; k < 2; k++) {
      const ctri *q = &s.t[k ? P[i].b : P[i].a];
      fprintf(stderr, "coplanar pair %d %c: %s mesh %d inst %d tri %d n (%.3f %.3f %.3f) d %.4f area %.5f two %d writes %d width %.1f mm p0 (%.3f %.3f %.3f) vn (%.3f %.3f %.3f) mat %p\n", i, k ? 'b' : 'a',
              s.rname[s.m[q->mesh].region], q->mesh, q->inst, q->t, q->n[0], q->n[1], q->n[2], q->d, q->area, q->two, q->writes, P[i].w * 1000, q->p[0][0], q->p[0][1], q->p[0][2], q->vn[0], q->vn[1], q->vn[2], (const void *)q->mat);
    }
  free(P);
  cset_free(&s);
  g_pairs_last = out->pairs;
}

/* ── planar patches ────────────────────────────────────────────────────
 * The faces of one mesh copy (mesh and instance) that lie in one plane, look alike (one material) and meet at a corner
 * make one surface: a cap's fan, a deck's two triangles, a wall's top. A patch moves as a whole (moving part of one
 * would crack it), its size is the patch's area, and two faces of one patch overlapping each other (a concave outline
 * fanned from its middle) are one surface drawn twice in one look: not two surfaces fighting. */
typedef struct pkey { int mesh, inst; int64_t x, y, z; int face; } pkey;
static int pkey_cmp(const void *pa, const void *pb) {
  const pkey *a = pa, *b = pb;
  if (a->mesh != b->mesh) return a->mesh < b->mesh ? -1 : 1;
  if (a->inst != b->inst) return a->inst < b->inst ? -1 : 1;
  if (a->x != b->x) return a->x < b->x ? -1 : 1;
  if (a->y != b->y) return a->y < b->y ? -1 : 1;
  if (a->z != b->z) return a->z < b->z ? -1 : 1;
  return a->face < b->face ? -1 : a->face > b->face;
}
static int uf_find(int *p, int i) {
  while (p[i] != i) p[i] = p[p[i]], i = p[i];
  return i;
}
static bool same_plane(const ctri *a, const ctri *b) {
  return a->mat && a->mat == b->mat && dot3(a->n, b->n) > 0.9999f && fabsf(a->d - b->d) < 1e-4f;
}
/* patch[i] for every face (0..*count), and each patch's area */
static int *patches(const cset *s, int *count, float **area) {
  int n = s->n, ne = 0;
  int *par = malloc(sizeof(int) * (size_t)(n ? n : 1));
  pkey *E = malloc(sizeof *E * (size_t)(n ? n * 3 : 1));
  if (!par || !E) abort();
  for (int i = 0; i < n; i++) {
    par[i] = i;
    for (int j = 0; j < 3; j++)
      E[ne++] = (pkey){ s->t[i].mesh, s->t[i].inst, llroundf(s->t[i].p[j][0] * 1e5f), llroundf(s->t[i].p[j][1] * 1e5f), llroundf(s->t[i].p[j][2] * 1e5f), i };
  }
  qsort(E, (size_t)ne, sizeof *E, pkey_cmp);
  for (int a = 0; a < ne;) {
    int b = a + 1;
    while (b < ne && E[b].mesh == E[a].mesh && E[b].inst == E[a].inst && E[b].x == E[a].x && E[b].y == E[a].y && E[b].z == E[a].z) b++;
    for (int i = a; i < b; i++)
      for (int j = i + 1; j < b; j++)
        if (same_plane(&s->t[E[i].face], &s->t[E[j].face])) {
          int ri = uf_find(par, E[i].face), rj = uf_find(par, E[j].face);
          if (ri != rj) par[ri > rj ? ri : rj] = ri < rj ? ri : rj;
        }
    a = b;
  }
  free(E);
  int *id = malloc(sizeof(int) * (size_t)(n ? n : 1)), *map = malloc(sizeof(int) * (size_t)(n ? n : 1)), c = 0;
  if (!id || !map) abort();
  for (int i = 0; i < n; i++) map[i] = -1;
  for (int i = 0; i < n; i++) {
    int r = uf_find(par, i);
    if (map[r] < 0) map[r] = c++;
    id[i] = map[r];
  }
  float *ar = calloc((size_t)(c ? c : 1), sizeof(float));
  if (!ar) abort();
  for (int i = 0; i < n; i++) ar[id[i]] += s->t[i].area;
  free(par), free(map);
  *count = c, *area = ar;
  return id;
}
/* b's corner values (n floats each) at the point p of its plane */
static void at_point(const ctri *b, const float *p, const float *vals, int n, float *out) {
  float u[3], v[3], P[3][2], q[2];
  basis(b->n, u, v);
  for (int j = 0; j < 3; j++) P[j][0] = dot3(b->p[j], u), P[j][1] = dot3(b->p[j], v);
  q[0] = dot3(p, u), q[1] = dot3(p, v);
  float d = (P[1][1] - P[2][1]) * (P[0][0] - P[2][0]) + (P[2][0] - P[1][0]) * (P[0][1] - P[2][1]);
  float w0 = ((P[1][1] - P[2][1]) * (q[0] - P[2][0]) + (P[2][0] - P[1][0]) * (q[1] - P[2][1])) / d;
  float w1 = ((P[2][1] - P[0][1]) * (q[0] - P[2][0]) + (P[0][0] - P[2][0]) * (q[1] - P[2][1])) / d, w2 = 1 - w0 - w1;
  for (int k = 0; k < n; k++) out[k] = vals[k] * w0 + vals[n + k] * w1 + vals[2 * n + k] * w2;
}
/* two faces look the same where they overlap: one material, one shading normal, and over a and b the same texture
 * coordinates and colours (a texture laid by world position, a colour by place), and no instance colour of their own */
static bool same_look(const cset *s, const ctri *a, const ctri *b) {
  const t3_material *m = a->mat;
  if (!m || m != b->mat || dot3(a->vn, b->vn) < 0.9999f) return false;
  const t3_mesh *ma = s->m[a->mesh].m, *mb = s->m[b->mesh].m;
  if ((a->mesh != b->mesh || a->inst != b->inst) && ((ma->base.type == T3_INSTANCED_MESH && ((const t3_instanced_mesh *)ma)->instance_color) ||
                                                     (mb->base.type == T3_INSTANCED_MESH && ((const t3_instanced_mesh *)mb)->instance_color)))
    return false;
  const bool textured = m->map || m->normal_map || m->ao_map || m->emissive_map || m->roughness_map || m->metalness_map || m->alpha_map;
  if (textured && !(a->has_uv && b->has_uv)) return false;
  if (m->vertex_colors && !(a->has_col && b->has_col)) return false;
  for (int j = 0; j < 3; j++) {
    float o[3];
    if (textured) {
      at_point(b, a->p[j], &b->uv[0][0], 2, o);
      if (fabsf(o[0] - a->uv[j][0]) > 1e-3f || fabsf(o[1] - a->uv[j][1]) > 1e-3f) return false;
    }
    if (m->vertex_colors) {
      at_point(b, a->p[j], &b->col[0][0], 3, o);
      if (fabsf(o[0] - a->col[j][0]) > 4e-3f || fabsf(o[1] - a->col[j][1]) > 4e-3f || fabsf(o[2] - a->col[j][2]) > 4e-3f) return false;
    }
  }
  return true;
}

/* drop the pairs that cannot be seen to fight: two faces of one patch (one surface in one look), and two faces that look
 * the same where they overlap (whichever the depth test picks, the pixel is the same colour); the pairs left */
static int drop_patch_pairs(const cset *s, const int *patch, cpair *P, int np) {
  int k = 0;
  for (int i = 0; i < np; i++) {
    const ctri *a = &s->t[P[i].a], *b = &s->t[P[i].b];
    if (patch[P[i].a] == patch[P[i].b]) continue;
    if (same_look(s, a, b)) continue;
    P[k++] = P[i];
  }
  return k;
}
/* the fighting pairs of s, without those inside one patch */
static int fighting_pairs(const cset *s, cpair **out) {
  int np = find_pairs(s, out), npatch;
  if (!np) return 0;
  float *area;
  int *patch = patches(s, &npatch, &area);
  np = drop_patch_pairs(s, patch, *out, np);
  free(patch), free(area);
  return np;
}

/* ── the separation ────────────────────────────────────────────────── */

static size_t attr_bytes(const t3_attribute *a) { return (a->type == T3_UINT16 ? 2 : 4) * (size_t)a->item_size; }

/* give a geometry nnew more vertices (copies of src[i]) and the index ix */
static void regrow(t3_geometry *g, int nv_old, const int *src, int nnew, const uint32_t *ix, int nix) {
  for (int sl = 0; sl < T3_ATTR_COUNT; sl++) {
    t3_attribute *a = g->attributes[sl];
    if (!a || sl == T3_ATTR_INSTANCE_MATRIX || sl == T3_ATTR_INSTANCE_COLOR || a->count != nv_old) continue;
    size_t b = attr_bytes(a);
    unsigned char *d = malloc(b * (size_t)(nv_old + nnew));
    if (!d) abort();
    memcpy(d, a->array, b * (size_t)nv_old);
    for (int i = 0; i < nnew; i++) memcpy(d + b * (size_t)(nv_old + i), (unsigned char *)a->array + b * (size_t)src[i], b);
    t3_attribute *na = t3_attribute_new(a->type, d, nv_old + nnew, a->item_size);
    na->normalized = a->normalized, na->usage = a->usage;
    free(d);
    t3_geometry_set_attribute(g, (t3_attr_slot)sl, na);
    t3_release(na);
  }
  t3_attribute *ni;
  if (nv_old + nnew <= 65535 && g->index->type == T3_UINT16) {
    uint16_t *s16 = malloc(sizeof *s16 * (size_t)nix);
    if (!s16) abort();
    for (int i = 0; i < nix; i++) s16[i] = (uint16_t)ix[i];
    ni = t3_attribute_new(T3_UINT16, s16, nix, 1);
    free(s16);
  } else ni = t3_attribute_new(T3_UINT32, ix, nix, 1);
  t3_geometry_set_index(g, ni);
  t3_release(ni);
}

/* world to the geometry's own space for face i's mesh and instance */
static void occ_inverse(const cset *s, const ctri *q, t3_mat4 *inv) {
  const t3_mesh *m = s->m[q->mesh].m;
  t3_mat4 w = m->base.matrix_world;
  if (m->base.type == T3_INSTANCED_MESH) {
    t3_mat4 inst;
    t3_instanced_mesh_get_matrix_at((t3_instanced_mesh *)m, q->inst, &inst);
    t3_mat4_multiply(&w, &m->base.matrix_world, &inst);
  }
  t3_mat4_invert(inv, &w);
}

static bool same3(const float *a, const float *b) { return fabsf(a[0] - b[0]) < 1e-7f && fabsf(a[1] - b[1]) < 1e-7f && fabsf(a[2] - b[2]) < 1e-7f; }

typedef struct vpos { float p[3]; int v; } vpos;
static int vpos_cmp(const void *pa, const void *pb) {
  const vpos *a = pa, *b = pb;
  for (int k = 0; k < 3; k++)
    if (a->p[k] != b->p[k]) return a->p[k] < b->p[k] ? -1 : 1;
  return a->v - b->v;
}

/* move a geometry's triangles by tdelta (local; tmove[t] set for the moving ones). The corners of one place (one
 * vertex, or several vertices at one position: a cap's rim and its wall's top edge) move together, the walls that
 * meet a moving face stretching to it, unless a face in the moving face's plane that stays (or moves another way) is
 * among them: then the moving face takes a copy of its own. Returns the corners moved. */
static int move_geometry(t3_geometry *g, const char *tmove, const float (*tdelta)[3]) {
  t3_attribute *pa = g->attributes[T3_ATTR_POSITION];
  int nv = pa->count, ps = pa->item_size, nix = g->index ? g->index->count : 0, ntri = (g->index ? nix : nv) / 3, moved = 0;
  uint32_t *ix = malloc(sizeof *ix * (size_t)(ntri > 0 ? ntri * 3 : 1));
  if (!ix) abort();
  for (int i = 0; i < ntri * 3; i++) ix[i] = g->index ? idx_at(g->index, i) : (uint32_t)i;
  float *Pp = pa->array, *P0 = malloc(sizeof(float) * (size_t)nv * (size_t)ps);
  if (!P0) abort();
  memcpy(P0, Pp, sizeof(float) * (size_t)nv * (size_t)ps);
  /* the places: vertices sorted by position, a place a run of one position */
  vpos *VP = malloc(sizeof *VP * (size_t)(nv ? nv : 1));
  int *place = malloc(sizeof(int) * (size_t)(nv ? nv : 1)), *pstart = malloc(sizeof(int) * (size_t)(nv + 1));
  if (!VP || !place || !pstart) abort();
  for (int v = 0; v < nv; v++) memcpy(VP[v].p, P0 + (size_t)v * (size_t)ps, 12), VP[v].v = v;
  qsort(VP, (size_t)nv, sizeof *VP, vpos_cmp);
  int npl = 0;
  for (int i = 0; i < nv; i++) {
    if (!i || vpos_cmp(&(vpos){ { VP[i].p[0], VP[i].p[1], VP[i].p[2] }, 0 }, &(vpos){ { VP[i - 1].p[0], VP[i - 1].p[1], VP[i - 1].p[2] }, 0 })) pstart[npl++] = i;
    place[VP[i].v] = npl - 1;
  }
  pstart[npl] = nv;
  /* the triangles at each place */
  int *tcount = calloc((size_t)npl + 1, sizeof(int)), *tlist;
  if (!tcount) abort();
  for (int t = 0; t < ntri; t++)
    for (int j = 0; j < 3; j++) tcount[place[ix[t * 3 + j]] + 1]++;
  for (int p = 0; p < npl; p++) tcount[p + 1] += tcount[p];
  tlist = malloc(sizeof(int) * (size_t)(ntri > 0 ? ntri * 3 : 1));
  int *fill = malloc(sizeof(int) * (size_t)(npl ? npl : 1));
  if (!tlist || !fill) abort();
  memcpy(fill, tcount, sizeof(int) * (size_t)npl);
  for (int t = 0; t < ntri; t++)
    for (int j = 0; j < 3; j++) tlist[fill[place[ix[t * 3 + j]]]++] = t;
  float (*tn)[3] = malloc(sizeof *tn * (size_t)(ntri > 0 ? ntri : 1)); /* each triangle's normal (own space) */
  if (!tn) abort();
  for (int t = 0; t < ntri; t++) {
    const float *a = P0 + (size_t)ix[t * 3] * (size_t)ps, *b = P0 + (size_t)ix[t * 3 + 1] * (size_t)ps, *c = P0 + (size_t)ix[t * 3 + 2] * (size_t)ps;
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float x[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] }, l = sqrtf(dot3(x, x));
    for (int k = 0; k < 3; k++) tn[t][k] = l > 0 ? x[k] / l : 0;
  }
  char *st = calloc((size_t)nv, 1); /* 2: moved by dv */
  float (*dv)[3] = malloc(sizeof *dv * (size_t)(nv ? nv : 1));
  int *copy = malloc(sizeof(int) * (size_t)(nv ? nv : 1)), *src = NULL, *cnext = NULL, nnew = 0, capn = 0, capd = 0, capx = 0, capp = 0;
  float (*npos)[3] = NULL, (*cdv)[3] = NULL;
  if (!st || !dv || !copy) abort();
  for (int v = 0; v < nv; v++) copy[v] = -1;
  for (int t = 0; t < ntri; t++) {
    if (!tmove[t]) continue;
    const float *ld = tdelta[t];
    float ln[3] = { ld[0], ld[1], ld[2] }, l = sqrtf(dot3(ln, ln));
    for (int k = 0; k < 3; k++) ln[k] /= l;
    for (int j = 0; j < 3; j++) {
      int slot = t * 3 + j;
      uint32_t vi = ix[slot];
      if ((int)vi >= nv) continue; /* (a copy made already) */
      if (st[vi] == 2 && same3(dv[vi], ld)) continue;
      /* may the whole place move? not past a face in this plane that stays or moves another way, nor a corner
       * already moved another way */
      int pl = place[vi];
      bool free_place = st[vi] != 2;
      for (int q = tcount[pl]; q < tcount[pl + 1] && free_place; q++) {
        int u = tlist[q];
        if (u == t || (tmove[u] && same3(tdelta[u], ld))) continue;
        if (fabsf(dot3(tn[u], ln)) > 0.99f) free_place = false;
      }
      for (int q = pstart[pl]; q < pstart[pl + 1] && free_place; q++) {
        int vk = VP[q].v;
        if (st[vk] == 2 && !same3(dv[vk], ld)) free_place = false;
      }
      if (free_place || !g->index) {
        for (int q = pstart[pl]; q < pstart[pl + 1]; q++) {
          int vk = VP[q].v;
          if (!g->index && vk != (int)vi) continue;
          for (int k = 0; k < 3; k++) Pp[(size_t)vk * (size_t)ps + (size_t)k] = P0[(size_t)vk * (size_t)ps + (size_t)k] + ld[k], dv[vk][k] = ld[k];
          st[vk] = 2;
        }
      } else { /* a copy of its own (one per way) */
        int c = copy[vi];
        while (c >= 0 && !same3(cdv[c], ld)) c = cnext[c];
        if (c < 0) {
          src = grow(src, &capn, nnew + 1, sizeof *src);
          npos = grow(npos, &capp, nnew + 1, sizeof *npos);
          cdv = grow(cdv, &capd, nnew + 1, sizeof *cdv);
          cnext = grow(cnext, &capx, nnew + 1, sizeof *cnext);
          src[nnew] = (int)vi;
          for (int k = 0; k < 3; k++) npos[nnew][k] = P0[(size_t)vi * (size_t)ps + (size_t)k] + ld[k], cdv[nnew][k] = ld[k];
          cnext[nnew] = copy[vi], copy[vi] = nnew;
          c = nnew++;
        }
        ix[slot] = (uint32_t)(nv + c);
      }
      moved++;
    }
  }
  if (nnew) {
    regrow(g, nv, src, nnew, ix, nix);
    float *NP = g->attributes[T3_ATTR_POSITION]->array;
    for (int i = 0; i < nnew; i++)
      for (int k = 0; k < 3; k++) NP[(size_t)(nv + i) * (size_t)ps + (size_t)k] = npos[i][k];
  }
  g->attributes[T3_ATTR_POSITION]->version++;
  g->version++;
  t3_geometry_compute_bounding_box(g);
  t3_geometry_compute_bounding_sphere(g);
  free(ix), free(P0), free(VP), free(place), free(pstart), free(tcount), free(tlist), free(fill), free(tn), free(st), free(dv), free(copy), free(src), free(npos),
    free(cdv), free(cnext);
  return moved;
}

/* one pass: layer the overlapping patches among root's meshes; the corners moved */
static bool shared_geo(const cset *s, int mesh) {
  const t3_mesh *m = s->m[mesh].m;
  if (m->base.type == T3_INSTANCED_MESH && ((const t3_instanced_mesh *)m)->count > 1) return true;
  for (int j = 0; j < s->nm; j++)
    if (j != mesh && s->m[j].m->geometry == m->geometry) return true;
  return false;
}
static int separate_pass(t3_object *root, float eps) {
  cset s = { 0 };
  walk(&s, root, root, true);
  cpair *P;
  int np = find_pairs(&s, &P);
  int n = s.n, moved = 0;
  if (np) {
    int npatch;
    float *parea;
    int *patch = patches(&s, &npatch, &parea);
    np = drop_patch_pairs(&s, patch, P, np);
    struct nudge_s { t3_mesh *m; int inst; } *nudged = NULL;
    int nnudge = 0, capnudge = 0;
    /* (two copies of one geometry placed over each other, two instances of a model, move together whatever is done to
     * the geometry: such a pair is left to the instances' placement) */
    int kept = 0;
    for (int i = 0; i < np; i++) {
      const ctri *a = &s.t[P[i].a], *b = &s.t[P[i].b];
      if (s.m[a->mesh].m->geometry == s.m[b->mesh].m->geometry && (a->mesh != b->mesh || a->inst != b->inst)) {
        /* two instances of one mesh whose faces lie on each other's (two trees' cards in one plane): the later
         * instance steps aside along the faces' normal, when that is sideways (a step up would lift it off the
         * ground: those are left, and the scan after names them) */
        const ctri *q = a->inst > b->inst ? a : b;
        t3_mesh *mm = s.m[q->mesh].m;
        if (a->mesh == b->mesh && mm->base.type == T3_INSTANCED_MESH && fabsf(q->n[1]) < 0.7f) {
          bool done = false;
          for (int k = 0; k < nnudge && !done; k++) done = nudged[k].m == mm && nudged[k].inst == q->inst;
          if (!done) {
            t3_mat4 im, inv;
            t3_instanced_mesh_get_matrix_at((t3_instanced_mesh *)mm, q->inst, &im);
            t3_mat4_invert(&inv, &mm->base.matrix_world); /* (the step is in the world: into the mesh's own space) */
            float step = fmaxf(eps, 2.0f * q->tol), wd[3] = { q->n[0] * step, q->n[1] * step, q->n[2] * step }, ld[3];
            xf_dir(&inv, wd, ld);
            im.e[12] += ld[0], im.e[13] += ld[1], im.e[14] += ld[2];
            t3_instanced_mesh_set_matrix_at((t3_instanced_mesh *)mm, q->inst, &im);
            ((t3_instanced_mesh *)mm)->instance_matrix->version++;
            nudged = grow(nudged, &capnudge, nnudge + 1, sizeof *nudged);
            nudged[nnudge++] = (struct nudge_s){ mm, q->inst };
            moved++;
          }
        }
        continue;
      }
      P[kept++] = P[i];
    }
    np = kept;
    free(nudged);
    /* which of the two patches of a pair comes out in front. Two in one mesh copy: the one drawn later, the one the depth
     * test mostly let through already (with equal depths the later draw wins), so the picture keeps the look it had
     * and only the speckle where rounding went the other way goes. Two of different meshes (drawn in an order that
     * changes with the view): the smaller, the detail on what it is laid on. */
    int *first = malloc(sizeof(int) * (size_t)(npatch ? npatch : 1)), (*mv)[2] = malloc(sizeof *mv * (size_t)(np ? np : 1)), nmv = 0;
    float *off = calloc((size_t)(npatch ? npatch : 1), sizeof(float)); /* along its own normal */
    signed char *kind = calloc((size_t)(npatch ? npatch : 1), 1);        /* 1: it moves */
    if (!first || !mv || !off || !kind) abort();
    for (int c = 0; c < npatch; c++) first[c] = -1;
    for (int i = 0; i < n; i++)
      if (first[patch[i]] < 0) first[patch[i]] = i;
    for (int i = 0; i < np; i++) {
      const ctri *a = &s.t[P[i].a], *b = &s.t[P[i].b];
      int pa = patch[P[i].a], pb = patch[P[i].b], m;
      if (a->mesh == b->mesh && a->inst == b->inst) m = a->t > b->t ? 0 : 1;
      else m = parea[pa] < parea[pb] || (parea[pa] == parea[pb] && pa > pb) ? 0 : 1;
      mv[nmv][0] = m ? pb : pa, mv[nmv][1] = m ? pa : pb, nmv++;
    }
    /* each mover out in front of its base, a step past where the base is (relaxed until it holds) */
    for (int it = 0, changed = 1; it < 16 && changed; it++) {
      changed = 0;
      for (int i = 0; i < nmv; i++) {
        int cs = mv[i][0], cb = mv[i][1];
        const ctri *ts = &s.t[first[cs]], *tb = &s.t[first[cb]];
        float step = fmaxf(eps, 2.0f * fmaxf(ts->tol, tb->tol)), ob = off[cb] * dot3(tb->n, ts->n);
        const float front = dot3(tb->n, ts->n) > 0 ? 1.0f : -1.0f; /* (the base's side, along the mover's normal) */
        float want = ob + front * step;
        if (fabsf(want) > 6.0f * step) continue; /* (two patches each in front of the other somewhere: no endless climb) */
        if (!kind[cs] || (front > 0 ? want > off[cs] + 1e-7f : want < off[cs] - 1e-7f)) off[cs] = want, kind[cs] = 1, changed = 1;
      }
    }
    free(mv);
    /* each geometry once (instances and meshes that share it move together): a triangle takes the biggest move any of
     * its copies has */
    for (int mi = 0; mi < s.nm; mi++) {
      t3_geometry *g = s.m[mi].m->geometry;
      bool seen = false;
      for (int mj = 0; mj < mi && !seen; mj++) seen = s.m[mj].m->geometry == g;
      if (seen) continue;
      int ntri = (g->index ? g->index->count : g->attributes[T3_ATTR_POSITION]->count) / 3;
      char *tmove = calloc((size_t)(ntri > 0 ? ntri : 1), 1);
      float (*tdelta)[3] = calloc((size_t)(ntri > 0 ? ntri : 1), sizeof *tdelta), *tmag = calloc((size_t)(ntri > 0 ? ntri : 1), sizeof(float));
      if (!tmove || !tdelta || !tmag) abort();
      bool any = false;
      for (int i = 0; i < n; i++) {
        const ctri *q = &s.t[i];
        float o = off[patch[i]];
        if (s.m[q->mesh].m->geometry != g || q->t >= ntri || !kind[patch[i]] || fabsf(o) <= tmag[q->t]) continue;
        t3_mat4 inv;
        occ_inverse(&s, q, &inv);
        float wd[3] = { q->n[0] * o, q->n[1] * o, q->n[2] * o };
        xf_dir(&inv, wd, tdelta[q->t]);
        tmag[q->t] = fabsf(o), tmove[q->t] = 1, any = true;
      }
      if (any) moved += move_geometry(g, tmove, (const float (*)[3])tdelta);
      free(tmove), free(tdelta), free(tmag);
    }
    free(first), free(off), free(kind), free(patch), free(parea);
  }
  if (opt_source("COPLANAR_DEBUG")) fprintf(stderr, "coplanar pass: %d faces, %d pairs, %d corners moved\n", n, np, moved); /* (development) */
  free(P);
  cset_free(&s);
  return moved;
}

/* CW_COPLANAR_UNSHARE: each plain mesh under root that draws a geometry another plain mesh there draws too (the unit
 * primitives MeshInstance3D nodes share) gets a copy of its own, so pieces laid on pieces can move apart */
static t3_attribute *attr_copy(const t3_attribute *a) {
  t3_attribute *c = t3_attribute_new(a->type, a->array, a->count, a->item_size);
  c->normalized = a->normalized;
  return c;
}
static void plain_meshes(t3_object *o, t3_mesh ***list, int *n, int *cap) {
  if (!o->visible) return;
  if (o->type == T3_MESH) {
    t3_mesh *m = (t3_mesh *)o;
    if (m->geometry && !m->geometry->morph_count && !m->geometry->named_count && m->geometry->attributes[T3_ATTR_POSITION]) {
      *list = grow(*list, cap, *n + 1, sizeof **list);
      (*list)[(*n)++] = m;
    }
  }
  for (int i = 0; i < o->child_count; i++) plain_meshes(o->children[i], list, n, cap);
}
static int unshare(t3_object *root) {
  t3_mesh **L = NULL;
  int n = 0, cap = 0, copies = 0;
  plain_meshes(root, &L, &n, &cap);
  for (int i = 0; i < n; i++) {
    const t3_geometry *g = L[i]->geometry;
    bool earlier = false;
    for (int j = 0; j < i && !earlier; j++) earlier = L[j]->geometry == g;
    if (!earlier) continue; /* (the first mesh drawing it keeps it) */
    t3_geometry *c = t3_geometry_new();
    for (int sl = 0; sl < T3_ATTR_COUNT; sl++)
      if (g->attributes[sl]) t3_geometry_set_attribute(c, (t3_attr_slot)sl, attr_copy(g->attributes[sl]));
    if (g->index) t3_geometry_set_index(c, attr_copy(g->index));
    for (int k = 0; k < g->group_count; k++) t3_geometry_add_group(c, g->groups[k].start, g->groups[k].count, g->groups[k].material_index);
    c->draw_start = g->draw_start, c->draw_count = g->draw_count;
    t3_release(L[i]->geometry);
    L[i]->geometry = c;
    copies++;
  }
  free(L);
  return copies;
}

int cw_coplanar_separate(t3_object *root, float eps, int *left) { return cw_coplanar_separate_ex(root, eps, left, 0); }

int cw_coplanar_separate_ex(t3_object *root, float eps, int *left, unsigned flags) {
  if (left) *left = 0;
  if (!root || !env_on("COPLANAR_SEPARATE", true)) return 0;
  const double t0 = cw_time();
  names_build();
  t3_object_update_matrix_world(root, true);
  if (flags & CW_COPLANAR_UNSHARE) unshare(root);
  int moved = 0;
  /* (a face moved onto a layer can meet another there, and a shared geometry's faces meet each other's copies: a few
   * passes, until none is left) */
  { /* every geometry drawn more than once (instances, meshes sharing it) first in its own space, with its own materials:
     * its copies are of different sizes, and a step through one copy's scale is another's step through its own, so
     * what can be done in the model's own space is (the world passes after see only what the placing made) */
    cset s = { 0 };
    walk(&s, root, root, true);
    for (int mi = 0; mi < s.nm; mi++) {
      bool seen = false;
      for (int mj = 0; mj < mi && !seen; mj++) seen = s.m[mj].m->geometry == s.m[mi].m->geometry;
      if (seen || !shared_geo(&s, mi)) continue;
      t3_mesh *src = s.m[mi].m;
      t3_object *grp = t3_group_new();
      t3_mesh *me = t3_mesh_new_multi(src->geometry, src->materials, src->material_count);
      t3_object_add(grp, me);
      t3_object_update_matrix_world(grp, true);
      for (int pass = 0; pass < 8; pass++) {
        int m = separate_pass(grp, eps * (float)(pass + 1)); /* (a longer step each pass: a face that landed on another plane leaves it rather than going back) */
        moved += m;
        if (!m) break;
      }
      t3_release(me), t3_release(grp);
    }
    cset_free(&s);
  }
  for (int pass = 0; pass < 8; pass++) {
    int m = separate_pass(root, eps * (float)(pass + 1));
    moved += m;
    if (!m) break;
  }
  g_moved_total += moved;
  g_last_ms = (cw_time() - t0) * 1000.0;
  if (left) {
    cset s = { 0 };
    walk(&s, root, root, true);
    cpair *P;
    *left = fighting_pairs(&s, &P);
    free(P);
    cset_free(&s);
  }
  return moved;
}

int cw_coplanar_separate_model(t3_geometry *const *geos, int n, float eps) {
  t3_object *grp = t3_group_new();
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  for (int i = 0; i < n; i++)
    if (geos[i]) {
      t3_mesh *me = t3_mesh_new(geos[i], m);
      t3_object_add(grp, me);
      t3_release(me);
    }
  int moved = cw_coplanar_separate(grp, eps, NULL);
  t3_release(m), t3_release(grp);
  return moved;
}

/* ── the debug command ─────────────────────────────────────────────── */
static void reply_report(cw_reply *r, const cw_coplanar_report *rep) {
  cw_reply_printf(r, "{\"meshes\":%d,\"faces\":%d,\"pairs\":%d,\"moved\":%d,\"regions\":[", rep->meshes, rep->faces, rep->pairs, g_moved_total);
  for (int i = 0; i < rep->nregions; i++) {
    cw_reply_printf(r, "%s{\"name\":", i ? "," : "");
    cw_reply_str(r, rep->regions[i].name);
    cw_reply_printf(r, ",\"pairs\":%d,\"width_mm\":%.1f,\"at\":[%.3f,%.3f,%.3f]}", rep->regions[i].pairs, rep->regions[i].width * 1000.0f, rep->regions[i].at[0], rep->regions[i].at[1], rep->regions[i].at[2]);
  }
  cw_reply_raw(r, "]}");
}

/* the detector on made-up faces: what must count and what must not */
static int selftest(char *why, size_t cap) {
  const float tol = PLANE_TOL;
  const bool view = VIEW.on;
  PLANE_TOL = 0.001f, VIEW.on = false; /* (the made-up faces are measured against the default) */
  static const float sq[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } };
  struct { const char *name; float off[3]; int cut; bool flip, two; float scale; int want; } C[] = {
    { "a doubled face, cut the other way", { 0, 0, 0 }, 1, false, false, 1, 1 },
    { "a doubled face, the same cut", { 0, 0, 0 }, 0, false, false, 1, 1 },
    { "a small face laid on a big one", { 0.25f, 0, 0.25f }, 0, false, false, 0.25f, 1 },
    { "a face 0.5 mm over another", { 0, 0.0005f, 0 }, 1, false, false, 1, 1 },
    { "a face 5 mm over another", { 0, 0.005f, 0 }, 1, false, false, 1, 0 },
    { "a neighbour sharing an edge", { 1, 0, 0 }, 0, false, false, 1, 0 },
    { "a face turned away (single sided)", { 0, 0, 0 }, 0, true, false, 1, 0 },
    { "a face turned away, drawn from both sides", { 0, 0, 0 }, 0, true, true, 1, 1 },
  };
  int bad = 0;
  why[0] = 0;
  for (size_t c = 0; c < sizeof C / sizeof *C; c++) {
    cset s = { 0 };
    s.m = grow(s.m, &s.capm, 1, sizeof *s.m);
    memset(s.m, 0, sizeof *s.m);
    s.nm = 1;
    s.rname = grow(s.rname, &s.capr, 1, sizeof *s.rname);
    snprintf(s.rname[0], sizeof s.rname[0], "selftest"), s.nr = 1;
    const int cuts[2][2][3] = { { { 0, 2, 1 }, { 0, 3, 2 } }, { { 1, 0, 3 }, { 1, 3, 2 } } };
    for (int f = 0; f < 2; f++) /* the base square, then the other face */
      for (int t = 0; t < 2; t++) {
        float v[3][3];
        for (int j = 0; j < 3; j++) {
          int k = cuts[f ? C[c].cut : 0][t][f && C[c].flip ? 2 - j : j];
          for (int a = 0; a < 3; a++) v[j][a] = f ? sq[k][a] * C[c].scale + C[c].off[a] : sq[k][a];
        }
        s.t = grow(s.t, &s.cap, s.n + 1, sizeof *s.t);
        if (tri_fill(&s.t[s.n], v[0], v[1], v[2])) s.t[s.n].mesh = 0, s.t[s.n].t = f, s.t[s.n].inst = 0, s.t[s.n].two = f && C[c].two, s.t[s.n].writes = true, s.t[s.n].tol = PLANE_TOL, s.n++;
      }
    cpair *P;
    int np = find_pairs(&s, &P);
    /* pairs between the two squares (each square's own two halves only touch along their cut) */
    int across = 0;
    for (int i = 0; i < np; i++) across += s.t[P[i].a].t != s.t[P[i].b].t;
    bool ok = C[c].want ? across > 0 : np == 0;
    if (!ok) bad++, snprintf(why + strlen(why), cap - strlen(why), "%s%s: %d pairs", why[0] ? "; " : "", C[c].name, np);
    free(P);
    cset_free(&s);
  }
  PLANE_TOL = tol, VIEW.on = view;
  return bad;
}

static void cmd_coplanar(int argc, const char **argv, cw_reply *r) {
  if (argc > 1 && !strcmp(argv[1], "selftest")) {
    char why[1024];
    int bad = selftest(why, sizeof why);
    cw_reply_printf(r, "{\"selftest\":%s,\"why\":", bad ? "false" : "true");
    cw_reply_str(r, why);
    cw_reply_raw(r, "}");
    return;
  }
  cw_tree *t = cw_scene_app_tree();
  t3_scene *sc = t ? cw_render3d_scene(t) : NULL;
  cw_coplanar_report rep;
  cw_coplanar_scan(sc ? &sc->base : NULL, &rep);
  reply_report(r, &rep);
}

/* ── the motion sweep's camera ─────────────────────────────────────────
 * COPLANAR_SWEEP="from,x,y,z,tx,ty,tz,dx,dy,dz": from frame `from` on the game is held still (the debugger's pause:
 * no tick, no update) and the current camera looks from (x, y, z) at (tx, ty, tz), both gliding (dx, dy, dz) metres a
 * second: only the camera moves, so a pixel of the still world that changes from one frame to the next and back is the
 * depth test changing its mind (a game's coplanar sweep gate). */
static struct { bool on; int from, frame; float p[3], t[3], d[3]; } SW;
/* the sweep's control (COPLANAR_CONTROL=1): a doubled face drawn half way to where the camera looks, a square a fifth of the
 * view across facing the camera (red) and the same square turned an eighth of the way round (green), leaning through
 * the red one by a few depth steps: two faces nearer than the depth tells apart, which must flicker. They take the look of a plain opaque mesh of
 * the game's own (its material copied, coloured), so the game's program table already holds what draws them. */
static t3_mesh *plain_mesh(t3_object *o) {
  if (!o->visible) return NULL;
  if (o->type == T3_MESH) {
    t3_mesh *m = (t3_mesh *)o;
    if (m->material_count == 1 && m->materials[0] && !m->materials[0]->transparent && m->materials[0]->depth_write && !m->morph_influence_count) return m;
  }
  for (int i = 0; i < o->child_count; i++) {
    t3_mesh *m = plain_mesh(o->children[i]);
    if (m) return m;
  }
  return NULL;
}
static void control_face(cw_tree *t) {
  t3_scene *sc = cw_render3d_scene(t);
  t3_mesh *like = sc ? plain_mesh(&sc->base) : NULL;
  if (!like) { cw_error("coplanar: no plain mesh in the scene to draw the control's faces like"); return; }
  /* (half way to what the camera looks at, so nothing of the world stands in front of it) */
  float n[3] = { SW.p[0] - SW.t[0], SW.p[1] - SW.t[1], SW.p[2] - SW.t[2] }, l = sqrtf(dot3(n, n)), u[3], v[3], c[3];
  for (int i = 0; i < 3; i++) n[i] /= l;
  basis(n, u, v);
  cw_node cam = cw_render3d_camera(t), at = cw_node_valid(cam) ? cw_node_parent(cam) : cw_tree_root(t); /* (beside the camera: in the 3D world) */
  /* the green one leans through the red one by a few steps of the depth buffer across its width (d^2 / (near * 2^24) a
   * step, d^2 (far - near) / (near far 2^24)), so the two are nearer than the depth can tell apart across a broad band,
   * as faces laid on faces are */
  cw_value nv;
  const float near = cw_node_valid(cam) && cw_node_get_prop(cam, "near", &nv) && nv.f > 0 ? (float)nv.f : 0.1f,
              far = cw_node_valid(cam) && cw_node_get_prop(cam, "far", &nv) && nv.f > near ? (float)nv.f : 1000.0f;
  const float d = fmaxf(l * 0.5f, near * 1.05f), h = d * 0.1f; /* (past the near plane: a map seen from far keeps it far out) */
  for (int i = 0; i < 3; i++) c[i] = SW.p[i] - n[i] * d;
  const float lean = 2.0f * d * d * (far - near) / (near * far * 16777216.0f) / h;
  if (!cw_node_valid(at)) at = cw_tree_root(t);
  const t3_vec3 off = cw_node3d_global_position(at);
  for (int q = 0; q < 2; q++) {
    float pos[4][3], nor[4][3], uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int k = 0; k < 4; k++) {
      float a = k == 0 || k == 3 ? -h : h, b = k < 2 ? -h : h;
      if (q) { /* (turned 45 degrees: a diamond over the square) */
        float r = 0.70710678f, a2 = (a - b) * r, b2 = (a + b) * r;
        a = a2, b = b2;
      }
      for (int i = 0; i < 3; i++) pos[k][i] = c[i] + u[i] * a + v[i] * b + (q ? n[i] * a * lean : 0), nor[k][i] = n[i];
    }
    const uint16_t ix[6] = { 0, 1, 2, 0, 2, 3 };
    t3_geometry *g = t3_geometry_new();
    t3_geometry_set_attribute(g, T3_ATTR_POSITION, t3_attribute_new(T3_FLOAT32, pos, 4, 3));
    t3_geometry_set_attribute(g, T3_ATTR_NORMAL, t3_attribute_new(T3_FLOAT32, nor, 4, 3));
    t3_geometry_set_attribute(g, T3_ATTR_UV, t3_attribute_new(T3_FLOAT32, uv, 4, 2));
    for (int sl = 0; sl < T3_ATTR_COUNT; sl++) /* (every other attribute the borrowed look reads, at its first value) */
      if (sl != T3_ATTR_POSITION && sl != T3_ATTR_NORMAL && sl != T3_ATTR_UV && sl != T3_ATTR_INSTANCE_MATRIX && sl != T3_ATTR_INSTANCE_COLOR && like->geometry->attributes[sl]) {
        const t3_attribute *a = like->geometry->attributes[sl];
        size_t b = attr_bytes(a);
        unsigned char d[4 * 64];
        for (int k = 0; k < 4 && b <= 64; k++) memcpy(d + b * (size_t)k, a->array, b);
        if (b <= 64) t3_geometry_set_attribute(g, (t3_attr_slot)sl, t3_attribute_new(a->type, d, 4, a->item_size));
      }
    t3_geometry_set_index(g, t3_attribute_new(T3_UINT16, ix, 6, 1));
    t3_material *m = t3_material_clone(like->materials[0]);
    m->color = q ? (t3_color){ 0, 1, 0 } : (t3_color){ 1, 0, 0 };
    m->emissive = q ? (t3_color){ 0, 0.6f, 0 } : (t3_color){ 0.6f, 0, 0 }; /* (lit or not, red against green) */
    t3_mesh *me = t3_mesh_new(g, m);
    me->base.frustum_culled = false, me->base.cast_shadow = like->base.cast_shadow, me->base.receive_shadow = like->base.receive_shadow;
    cw_node node = cw_node_new("Node3D", q ? "CoplanarControlB" : "CoplanarControlA");
    cw_node_add_child(at, node);
    cw_node3d_set_position(node, (t3_vec3){ -off.x, -off.y, -off.z }); /* (the squares are in world coordinates; the parents here carry no turn) */
    cw_node3d_set_object(node, &me->base);
    t3_release(me), t3_release(g), t3_release(m);
  }
}
static void sweep_hook(cw_tree *t, double dt, void *u) {
  (void)dt, (void)u;
  if (++SW.frame < SW.from) return;
  if (SW.frame == SW.from) {
    cw_debug_pause(true);
    if (env_on("COPLANAR_CONTROL", false)) control_face(t);
  }
  cw_node cam = cw_render3d_camera(t);
  if (!cw_node_valid(cam)) return;
  float k = (float)(SW.frame - SW.from) / 60.0f;
  t3_vec3 p = { SW.p[0] + SW.d[0] * k, SW.p[1] + SW.d[1] * k, SW.p[2] + SW.d[2] * k };
  t3_vec3 at = { SW.t[0] + SW.d[0] * k, SW.t[1] + SW.d[1] * k, SW.t[2] + SW.d[2] * k };
  cw_node3d_set_position(cam, p);
  cw_node3d_look_at(cam, at, (t3_vec3){ 0, 1, 0 });
  t3_object *o = cw_node3d_object(cam); /* (the nodes wrote their objects before this hook: this frame draws from these) */
  o->position = cw_node3d_position(cam);
  t3_object_set_quaternion(o, cw_node3d_quaternion(cam));
  t3_object_update_world_matrix(o, true, true);
}

void cw_coplanar_init(void) {
  cw_debug_command("coplanar", "coplanar [selftest]: faces in one plane that fight for the depth test, by region", cmd_coplanar);
  cw_debug_i32("level.coplanar_pairs", &g_pairs_last);
  cw_debug_i32("level.coplanar_moved", &g_moved_total);
  cw_coplanar_sweep_arm();
}

void cw_coplanar_sweep_arm(void) {
  const char *sw = opt_source("COPLANAR_SWEEP");
  cw_tree *t = cw_scene_app_tree();
  if (sw && t && sscanf(sw, "%d,%f,%f,%f,%f,%f,%f,%f,%f,%f", &SW.from, &SW.p[0], &SW.p[1], &SW.p[2], &SW.t[0], &SW.t[1], &SW.t[2], &SW.d[0], &SW.d[1], &SW.d[2]) == 10) {
    SW.frame = 0; /* (frames count from here) */
    if (!SW.on) cw_tree_add_hook(t, 2, 1 << 20, sweep_hook, NULL); /* (pre_render, after every node and the game's own camera) */
    SW.on = true;
  }
}

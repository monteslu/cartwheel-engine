/* 3D picking and projection (cartwheel/pick3d.h): canvas points to rays through a Camera3D, rays to the nodes they
 * hit (three.c's Raycaster for meshes, a per-instance test here for instanced meshes), 3D points to the canvas. */
#include "cartwheel/pick3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/math.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/subviewport.h"

bool cw__camera3d_lens(cw_node n, float aspect, bool *ortho, float *fov, float *size, float *near, float *far); /* scene/render3d.c */
void cw__node3d_sync_objects(cw_node n, void (*visit)(cw_node n, t3_object *o, void *u), void *u);          /* scene/nodes.c */
int cw__multimesh_instance_of(cw_node n, const t3_object *im, int k);                                    /* scene/multimesh.c */
cw_vec2 cw__r2d_window_canvas_size(void);                                                                 /* render2d/batch.c */

/* ── the lens ────────────────────────────────────────────────────────── */

typedef struct lens {
  bool ortho;
  float sx, sy;   /* half the view's width and height at depth 1 (perspective) or in metres (orthographic) */
  float near;
  t3_mat4 world, view; /* the camera's world matrix and its inverse */
} lens;

static bool lens_of(cw_node camera, cw_rect vp, lens *l) {
  float fov, size, near, far;
  float aspect = vp.h > 0 && vp.w > 0 ? vp.w / vp.h : 1;
  if (!cw__camera3d_lens(camera, aspect, &l->ortho, &fov, &size, &near, &far)) return false;
  const t3_mat4 *m = cw_node3d_global_transform(camera);
  if (!m) return false;
  l->world = *m;
  t3_mat4_invert(&l->view, &l->world);
  l->near = near;
  if (l->ortho) {
    l->sy = size * 0.5f;
  } else {
    float s, c;
    cw_sincos(fov * 0.5f * (CW_PI / 180.0f), &s, &c);
    l->sy = c != 0 ? s / c : 1e6f;
  }
  const t3_camera *tc = cw_camera3d_t3(camera); /* (a zoom set on the three.c camera narrows the view as it draws) */
  if (tc && tc->zoom > 0) l->sy /= tc->zoom;
  l->sx = l->sy * aspect;
  return true;
}

/* screen -> normalised device x, y (-1..1, y up) */
static void to_ndc(cw_rect vp, cw_vec2 p, float *nx, float *ny) {
  *nx = vp.w > 0 ? (p.x - vp.x) / vp.w * 2 - 1 : 0;
  *ny = vp.h > 0 ? 1 - (p.y - vp.y) / vp.h * 2 : 0;
}

cw_vec2 cw_camera3d_world_to_screen(cw_node camera, cw_rect vp, t3_vec3 world, float *depth) {
  lens l;
  if (depth) *depth = 0;
  if (!lens_of(camera, vp, &l)) return (cw_vec2){ 0, 0 };
  t3_vec3 v = t3_vec3_apply_mat4(world, &l.view);
  float d = -v.z;
  if (depth) *depth = d;
  float nx, ny;
  if (l.ortho) {
    nx = v.x / l.sx, ny = v.y / l.sy;
  } else {
    float dd = fabsf(d) > 1e-12f ? d : (d < 0 ? -1e-12f : 1e-12f);
    nx = v.x / (dd * l.sx), ny = v.y / (dd * l.sy);
  }
  return (cw_vec2){ vp.x + (nx * 0.5f + 0.5f) * vp.w, vp.y + (0.5f - ny * 0.5f) * vp.h };
}

static t3_vec3 view_point(const lens *l, float nx, float ny, float depth) {
  if (l->ortho) return t3_v3(nx * l->sx, ny * l->sy, -depth);
  return t3_v3(nx * l->sx * depth, ny * l->sy * depth, -depth);
}

t3_vec3 cw_camera3d_screen_to_world(cw_node camera, cw_rect vp, cw_vec2 screen, float depth) {
  lens l;
  if (!lens_of(camera, vp, &l)) return t3_v3(0, 0, 0);
  float nx, ny;
  to_ndc(vp, screen, &nx, &ny);
  return t3_vec3_apply_mat4(view_point(&l, nx, ny, depth), &l.world);
}

cw_ray3d cw_camera3d_screen_ray(cw_node camera, cw_rect vp, cw_vec2 screen) {
  lens l;
  cw_ray3d r = { t3_v3(0, 0, 0), t3_v3(0, 0, -1) };
  if (!lens_of(camera, vp, &l)) return r;
  float nx, ny;
  to_ndc(vp, screen, &nx, &ny);
  if (l.ortho) {
    r.origin = t3_vec3_apply_mat4(view_point(&l, nx, ny, l.near), &l.world);
    r.direction = t3_vec3_normalize(t3_vec3_transform_direction(t3_v3(0, 0, -1), &l.world));
  } else {
    r.origin = t3_vec3_apply_mat4(t3_v3(0, 0, 0), &l.world);
    t3_vec3 p = t3_vec3_apply_mat4(view_point(&l, nx, ny, 1), &l.world);
    r.direction = t3_vec3_normalize(t3_vec3_sub(p, r.origin));
  }
  return r;
}

/* ── views ───────────────────────────────────────────────────────────── */

static cw_vec2 canvas_size(void) {
  cw_vec2 s = cw__r2d_window_canvas_size();
  if (s.x > 0 && s.y > 0) return s;
  int w = 0, h = 0;
  cw_window_size(&w, &h);
  return (cw_vec2){ (float)w, (float)h };
}

int cw_canvas_views3d(cw_tree *t, cw_view3d *out, int cap) {
  if (!t) return 0;
  cw_vec2 cs = canvas_size();
  const cw_view *v = NULL;
  int n = cw_pipeline_views(cw_pipeline_of(t), &v);
  if (n >= 2) {
    for (int i = 0; i < n && i < cap; i++)
      out[i] = (cw_view3d){ v[i].camera, { v[i].x * cs.x, v[i].y * cs.y, v[i].w * cs.x, v[i].h * cs.y } };
    return n;
  }
  cw_node cam = cw_render3d_camera(t);
  if (!cw_node_valid(cam)) return 0;
  if (cap > 0) out[0] = (cw_view3d){ cam, { 0, 0, cs.x, cs.y } };
  return 1;
}

int cw_canvas_view3d_at(cw_tree *t, cw_vec2 p, cw_view3d *out) {
  cw_view3d v[CW_MAX_VIEWS];
  int n = cw_canvas_views3d(t, v, CW_MAX_VIEWS);
  for (int i = 0; i < n && i < CW_MAX_VIEWS; i++) {
    cw_rect r = v[i].rect;
    if (p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h) {
      if (out) *out = v[i];
      return i;
    }
  }
  return -1;
}

bool cw_subviewport_view3d(cw_node sv, cw_view3d *out) {
  if (!cw_node_is(sv, "SubViewport")) return false;
  cw_value v;
  float w = 512, h = 512;
  if (cw_node_get_prop(sv, "size", &v)) w = v.v[0], h = v.v[1];
  w = CW_CLAMP(roundf(w), 1, 4096), h = CW_CLAMP(roundf(h), 1, 4096);
  cw_node cam = { 0 };
  cw_tree *own = cw_subviewport_tree(sv);
  if (own) cam = cw_render3d_camera(own);
  else if (cw_node_get_prop(sv, "camera", &v) && v.s && v.s[0]) cam = cw_node_get(sv, v.s);
  if (!cw_node_is(cam, "Camera3D")) return false;
  if (out) *out = (cw_view3d){ cam, { 0, 0, w, h } };
  return true;
}

/* ── picking ─────────────────────────────────────────────────────────── */

typedef struct objnode { const t3_object *o; cw_node n; } objnode;
typedef struct ihit { t3_intersection x; int instance; t3_mat4 m; } ihit; /* a hit and the matrix it was found under */

static struct {
  objnode *map;
  int nmap, capmap;
  ihit *hits;
  int nhits, caphits;
  t3_object **inst;
  int ninst, capinst;
  t3_raycaster *rc;
} P;

static void *grow(void *p, int *cap, int need, size_t sz) {
  if (need <= *cap) return p;
  int c = *cap ? *cap : 64;
  while (c < need) c *= 2;
  p = cw_realloc(p, sz * (size_t)c, CW_MEM_SCENE);
  *cap = c;
  return p;
}

static void map_visit(cw_node n, t3_object *o, void *u) {
  (void)u;
  P.map = grow(P.map, &P.capmap, P.nmap + 1, sizeof *P.map);
  P.map[P.nmap++] = (objnode){ o, n };
}
static int map_cmp(const void *a, const void *b) {
  const t3_object *x = ((const objnode *)a)->o, *y = ((const objnode *)b)->o;
  return x < y ? -1 : x > y;
}
static cw_node node_of_object(const t3_object *o) {
  if (!P.nmap) return (cw_node){ 0 };
  objnode key = { o, { 0 } };
  const objnode *f = bsearch(&key, P.map, (size_t)P.nmap, sizeof *P.map, map_cmp);
  return f ? f->n : (cw_node){ 0 };
}
/* the nearest node owning o (o's own, else its parents') and the object that node drives */
static cw_node owner_of(const t3_object *o, const t3_object **owner_obj) {
  for (const t3_object *a = o; a; a = a->parent) {
    cw_node n = node_of_object(a);
    if (cw_node_valid(n)) {
      if (owner_obj) *owner_obj = a;
      return n;
    }
  }
  if (owner_obj) *owner_obj = NULL;
  return (cw_node){ 0 };
}

static void push_hit(const t3_intersection *x, int instance, const t3_mat4 *m) {
  P.hits = grow(P.hits, &P.caphits, P.nhits + 1, sizeof *P.hits);
  P.hits[P.nhits++] = (ihit){ *x, instance, *m };
}

static void other_cb(t3_object *o, void *u) {
  (void)u;
  if (o->type != T3_INSTANCED_MESH) return; /* (skinned meshes, lines: not tested) */
  P.inst = grow(P.inst, &P.capinst, P.ninst + 1, sizeof *P.inst);
  P.inst[P.ninst++] = o;
}

static uint32_t index_at(const t3_attribute *ix, int i) {
  return ix->type == T3_UINT16 ? ((const uint16_t *)ix->array)[i] : ((const uint32_t *)ix->array)[i];
}
static t3_vec3 pos_at(const t3_attribute *pa, uint32_t i) {
  const float *p = (const float *)pa->array + (size_t)i * (size_t)pa->item_size;
  return t3_v3(p[0], p[1], pa->item_size > 2 ? p[2] : 0);
}

/* Ray.intersectTriangle: the distance along r, or -1 (three.c's raycaster.c test, as three.js r187) */
static float ray_tri(t3_vec3 o, t3_vec3 d, t3_vec3 a, t3_vec3 b, t3_vec3 c, bool cull) {
  t3_vec3 e1 = t3_vec3_sub(b, a), e2 = t3_vec3_sub(c, a), n = t3_vec3_cross(e1, e2);
  float ddn = t3_vec3_dot(d, n), sign;
  if (ddn > 0) {
    if (cull) return -1;
    sign = 1;
  } else if (ddn < 0) {
    sign = -1;
    ddn = -ddn;
  } else return -1;
  t3_vec3 diff = t3_vec3_sub(o, a);
  float d1 = sign * t3_vec3_dot(d, t3_vec3_cross(diff, e2));
  if (d1 < 0) return -1;
  float d2 = sign * t3_vec3_dot(d, t3_vec3_cross(e1, diff));
  if (d2 < 0 || d1 + d2 > ddn) return -1;
  float qn = -sign * t3_vec3_dot(diff, n);
  if (qn < 0) return -1;
  return qn / ddn;
}

/* every instance of an instanced mesh against the ray: its bounding sphere first, then its triangles */
static void instanced_test(t3_instanced_mesh *im, t3_ray ray, float near, float far) {
  t3_mesh *m = &im->mesh;
  t3_geometry *g = m->geometry;
  if (!g || !m->material_count || !m->materials[0]) return;
  const t3_attribute *pa = g->attributes[T3_ATTR_POSITION];
  if (!pa || pa->type != T3_FLOAT32 || pa->item_size < 2) return;
  const t3_material *mat = m->materials[0];
  /* the geometry's bounds: a box's centre and the farthest vertex from it */
  t3_vec3 lo = t3_v3(INFINITY, INFINITY, INFINITY), hi = t3_v3(-INFINITY, -INFINITY, -INFINITY);
  for (int i = 0; i < pa->count; i++) {
    t3_vec3 p = pos_at(pa, (uint32_t)i);
    lo = t3_v3(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z)), hi = t3_v3(fmaxf(hi.x, p.x), fmaxf(hi.y, p.y), fmaxf(hi.z, p.z));
  }
  t3_vec3 ctr = t3_vec3_scale(t3_vec3_add(lo, hi), 0.5f);
  float rad = 0;
  for (int i = 0; i < pa->count; i++) rad = fmaxf(rad, t3_vec3_distance(ctr, pos_at(pa, (uint32_t)i)));
  const t3_attribute *ix = g->index;
  int total = ix ? ix->count : pa->count;
  int start = g->draw_start > 0 ? g->draw_start : 0, end = g->draw_count < 0 ? total : start + g->draw_count;
  if (end > total) end = total;
  for (int k = 0; k < im->count; k++) {
    t3_mat4 inst, w, inv;
    t3_instanced_mesh_get_matrix_at(im, k, &inst);
    t3_mat4_multiply(&w, &m->base.matrix_world, &inst);
    /* the sphere, scaled by the largest axis */
    const float *e = w.e;
    float s0 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2], s1 = e[4] * e[4] + e[5] * e[5] + e[6] * e[6], s2 = e[8] * e[8] + e[9] * e[9] + e[10] * e[10];
    float r = rad * sqrtf(fmaxf(s0, fmaxf(s1, s2)));
    t3_vec3 c = t3_vec3_apply_mat4(ctr, &w), oc = t3_vec3_sub(c, ray.origin);
    float along = t3_vec3_dot(oc, ray.direction), d2 = t3_vec3_dot(oc, oc) - along * along;
    if (d2 > r * r || along + r < 0) continue;
    t3_mat4_invert(&inv, &w);
    t3_vec3 lo2 = t3_vec3_apply_mat4(ray.origin, &inv);
    t3_vec3 ld = t3_vec3_normalize(t3_vec3_sub(t3_vec3_apply_mat4(t3_vec3_add(ray.origin, ray.direction), &inv), lo2));
    for (int i = start; i + 2 < end; i += 3) {
      uint32_t ia = ix ? index_at(ix, i) : (uint32_t)i, ib = ix ? index_at(ix, i + 1) : (uint32_t)i + 1, ic = ix ? index_at(ix, i + 2) : (uint32_t)i + 2;
      if ((int)ia >= pa->count || (int)ib >= pa->count || (int)ic >= pa->count) continue;
      t3_vec3 a = pos_at(pa, ia), b = pos_at(pa, ib), cc = pos_at(pa, ic);
      float t = mat->side == T3_BACK_SIDE ? ray_tri(lo2, ld, cc, b, a, true) : ray_tri(lo2, ld, a, b, cc, mat->side != T3_DOUBLE_SIDE);
      if (t < 0) continue;
      t3_vec3 world = t3_vec3_apply_mat4(t3_vec3_add(lo2, t3_vec3_scale(ld, t)), &w);
      float dist = t3_vec3_distance(ray.origin, world);
      if (dist < near || dist > far) continue;
      t3_intersection x = { dist, world, &m->base, i / 3, ia, ib, ic, 0 };
      push_hit(&x, k, &w);
    }
  }
}

static int hit_cmp(const void *a, const void *b) {
  const ihit *x = a, *y = b;
  if (x->x.distance != y->x.distance) return x->x.distance < y->x.distance ? -1 : 1;
  return x->instance < y->instance ? -1 : x->instance > y->instance; /* (stable for equal distances) */
}

static bool under(cw_node n, cw_node top) {
  for (; cw_node_valid(n); n = cw_node_parent(n))
    if (n.id == top.id) return true;
  return false;
}

/* hidden: the owning node or an ancestor is hidden, or a three.c object between the mesh and its node is (a hidden
 * LOD level); a static batch hides the mesh it draws, which still counts */
static bool shown(const t3_object *o, cw_node owner, const t3_object *owner_obj) {
  if (!cw_node_visible_in_tree(owner)) return false;
  for (const t3_object *a = o; a && a != owner_obj; a = a->parent) {
    if (a->visible) continue;
    cw_value b;
    if (a == o && cw_node_get_prop(owner, "batch", &b) && b.b) continue;
    return false;
  }
  return true;
}

static t3_vec3 face_normal(const ihit *h, t3_vec3 dir) {
  const t3_mesh *m = (const t3_mesh *)h->x.object;
  const t3_attribute *pa = m->geometry ? m->geometry->attributes[T3_ATTR_POSITION] : NULL;
  if (!pa) return t3_vec3_scale(dir, -1);
  t3_vec3 a = t3_vec3_apply_mat4(pos_at(pa, h->x.a), &h->m), b = t3_vec3_apply_mat4(pos_at(pa, h->x.b), &h->m),
          c = t3_vec3_apply_mat4(pos_at(pa, h->x.c), &h->m);
  t3_vec3 n = t3_vec3_cross(t3_vec3_sub(b, a), t3_vec3_sub(c, a));
  float l = t3_vec3_length(n);
  if (!(l > 0)) return t3_vec3_scale(dir, -1);
  n = t3_vec3_scale(n, 1 / l);
  return t3_vec3_dot(n, dir) > 0 ? t3_vec3_scale(n, -1) : n;
}

int cw_pick3d_ray(cw_tree *t, cw_ray3d ray, const cw_pick3d_filter *f, cw_pick3d_hit *hits, int cap) {
  static const cw_pick3d_filter none = { 0 };
  if (!f) f = &none;
  if (!t || cap <= 0 || !hits) return 0;
  t3_scene *sc = cw_render3d_scene(t);
  if (!sc) return 0;
  float dl = t3_vec3_length(ray.direction);
  if (!(dl > 0)) return 0;
  ray.direction = t3_vec3_scale(ray.direction, 1 / dl);
  /* the world as the nodes have it now, and which node drives which object */
  P.nmap = P.nhits = P.ninst = 0;
  cw__node3d_sync_objects(cw_tree_root(t), map_visit, NULL);
  if (P.nmap) qsort(P.map, (size_t)P.nmap, sizeof *P.map, map_cmp);
  t3_object_update_world_matrix(&sc->base, false, true);
  if (!P.rc) P.rc = t3_raycaster_new();
  t3_raycaster *rc = P.rc;
  t3_raycaster_set(rc, ray.origin, ray.direction);
  rc->near = 0;
  rc->far = f->max_distance > 0 ? f->max_distance : INFINITY;
  rc->layers = f->layers ? f->layers : 0xffffffffu;
  const t3_intersection *x = NULL;
  t3_object *root = &sc->base;
  int n = t3_raycaster_intersect_objects_ex(rc, &root, 1, true, &x, other_cb, NULL);
  for (int i = 0; i < n; i++) push_hit(&x[i], -1, &x[i].object->matrix_world);
  t3_ray r = { ray.origin, ray.direction };
  for (int i = 0; i < P.ninst; i++) instanced_test((t3_instanced_mesh *)P.inst[i], r, rc->near, rc->far);
  if (P.nhits > 1) qsort(P.hits, (size_t)P.nhits, sizeof *P.hits, hit_cmp);
  int out = 0;
  for (int i = 0; i < P.nhits && out < cap; i++) {
    const ihit *h = &P.hits[i];
    const t3_object *owner_obj = NULL;
    cw_node node = owner_of(h->x.object, &owner_obj);
    if (!f->hidden && cw_node_valid(node) && !shown(h->x.object, node, owner_obj)) continue;
    if (cw_node_valid(f->exclude) && cw_node_valid(node) && under(node, f->exclude)) continue;
    int inst = h->instance;
    if (inst >= 0 && cw_node_is(node, "MultiMeshInstance3D")) inst = cw__multimesh_instance_of(node, h->x.object, inst);
    if (f->group) {
      cw_node g = node;
      while (cw_node_valid(g) && !cw_node_in_group(g, f->group)) g = cw_node_parent(g);
      if (!cw_node_valid(g)) continue;
      node = g;
    }
    hits[out++] = (cw_pick3d_hit){ .node = node, .object = h->x.object, .instance = inst, .face = h->x.face_index,
                                   .distance = h->x.distance, .point = h->x.point, .normal = face_normal(h, ray.direction) };
    if (!f->all) break;
  }
  return out;
}

int cw_pick3d_screen(cw_node camera, cw_rect vp, cw_vec2 screen, const cw_pick3d_filter *f, cw_pick3d_hit *hits, int cap) {
  cw_tree *t = cw_node_tree(camera);
  if (!t || !cw_node_is(camera, "Camera3D")) return 0;
  return cw_pick3d_ray(t, cw_camera3d_screen_ray(camera, vp, screen), f, hits, cap);
}

int cw_pick3d_canvas(cw_tree *t, cw_vec2 p, const cw_pick3d_filter *f, cw_pick3d_hit *hits, int cap, int *view) {
  cw_view3d v;
  int i = cw_canvas_view3d_at(t, p, &v);
  if (view) *view = i;
  if (i < 0) return 0;
  return cw_pick3d_screen(v.camera, v.rect, p, f, hits, cap);
}

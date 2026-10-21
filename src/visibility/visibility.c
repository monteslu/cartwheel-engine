/* Visibility: VisibilityManager, Room3D, Portal3D; the per-tree culling
 * state, the "visibility" / "occlusion_queries" / "impostors" /
 * "visibility_restore" passes. See cartwheel/visibility.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/jobs.h"
#include "cartwheel/profile.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "render/pipeline_internal.h"
#include "render/quality_internal.h"
#include "scene/scene_internal.h"
#include "vis_internal.h"
#if CW_WGPU
#include "fx/fx_wgpu.h"
#endif

/* ── nodes ──────────────────────────────────────────────────────────── */

typedef struct vis_manager {
  bool frustum_cull, occlusion_cull, occlusion_wait, portals, lod, impostors;
  float occlusion_margin;
  int occlusion_interval, occlusion_requery_occluded;
  bool occlusion_guard;
  float lod_error_px, shadow_distance;
  /* stats */
  int meshes, static_meshes, visible, frustum_culled, occlusion_culled, portal_culled, distance_culled;
  int queries, query_latency;
  float lod_counts[4];
  int impostors_drawn, impostor_fading, bvh_nodes, rooms_visible;
  float cull_ms;
} vis_manager;

typedef struct room3d { t3_vec3 extents; } room3d;
typedef struct portal3d { cw_vec2 size; bool enabled; } portal3d;

static const cw_node_type *manager_t, *room_t, *portal_t;

/* ── per-tree state ─────────────────────────────────────────────────── */

typedef struct fade_slot {
  t3_mesh *mesh;
  int count;
  t3_material *orig[4], *clone[4];
  uint32_t layers; /* the mesh's own layers while it is drawn on FADE_LAYER */
} fade_slot;

/* the layer a fading mesh is drawn on for the frame: the scene pass skips
 * it, the impostor pass draws it after the impostors (fade_draw) */
#define FADE_LAYER (1u << 31)
typedef struct fade_cache {
  t3_object *obj; /* the object the slots were made for */
  CW_ARRAY(fade_slot) slots;
} fade_cache;

typedef struct vrec {
  cw_node node;
  cw__mesh_vis *vis;
  t3_object *obj;
  int kind, lod_levels;
  bool cast_shadow, is_static;
  uint64_t look_key;
  float lmin[3], lmax[3]; /* bounds in obj space */
  bool have_local, have_world;
  uint32_t wver;
  float bmin[3], bmax[3]; /* world */
  float center[3], radius, scale;
  uint32_t rooms;         /* bit 0: reaches outside; bit i: overlaps room i - 1 (all bits past 31 rooms) */
  /* this frame */
  uint8_t cand, was_cand, drawn, reason;
  int lod;
  float imp_alpha;
  bool want_query;
  /* the crossfade: transparent clones of the mesh's materials, swapped in
   * while it fades out under its impostor */
  struct fade_cache *fade;
  /* occlusion */
  GLuint q;
  bool q_pending, occluded;
  uint64_t q_frame;
} vrec;

enum { R_VISIBLE, R_FRUSTUM, R_DISTANCE, R_PORTAL, R_OCCLUSION };

typedef struct vroom {
  cw_node node;
  t3_mat4 inv;
  float ext[3];
} vroom;
typedef struct vportal {
  cw_node node;
  float c[4][3], center[3], normal[3];
  float ax[3], ay[3], hx, hy; /* the opening's axes (unit) and half sizes */
  int a, b; /* room index + 1 on each side, 0 = outside */
  bool enabled;
} vportal;

typedef struct room_frusta { CW_ARRAY(vis_frustum) f; } room_frusta;

typedef struct vis_state {
  cw_tree *tree;
  uint64_t structure_version;
  bool have_records;
  CW_ARRAY(vrec) recs;
  CW_ARRAY(cw_node) dir_lights, local_lights; /* DirectionalLight3D; Omni / SpotLight3D */
  vis_bvh bvh;
  bool bvh_dirty;
  CW_ARRAY(vroom) rooms;
  CW_ARRAY(vportal) portals;
  CW_ARRAY(room_frusta) rf;
  CW_ARRAY(t3_object *) hidden;
  CW_ARRAY(fade_cache *) faded; /* swapped in this frame: swapped back by the restore pass */
  CW_ARRAY(t3_object *) fade_lights; /* fade_draw's scratch: lights given FADE_LAYER for its render */
  vis_impostors *imp;
  cw_visibility_stats stats;
  uint64_t frame;
  float prev_cam[3], prev_fwd[3];
  bool have_prev_cam;
  float margin_ang;  /* extra query growth per metre of distance: the camera's turn and parallax over the latency */
  float moved, turn, near_occluder, near_clip; /* this frame's camera step; the nearest drawn static box last frame */
  bool occl_off;     /* this frame trusts no occlusion result (the camera brushes an occluder) */
  float cam[3], margin, far_plane[4];
  uint32_t cam_layers; /* the camera's layers: what fade_draw restores */
  t3_mat4 vp;
  bool passes_added, ran;
  vis_manager *mgr;
  /* GL for the query boxes */
  GLuint box_prog, box_vbo, box_vao;
  struct vis_wq *wq; /* WebGPU occlusion queries */
} vis_state;

#if CW_WGPU
static void wq_free(struct vis_wq *q);
static void wq_reset(struct vis_wq *q);
#endif

static void fade_cache_free(struct fade_cache *fc);

static void state_free(void *p) {
  vis_state *s = p;
  for (int i = 0; i < s->recs.count; i++) {
    if (s->recs.data[i].q) glDeleteQueries(1, &s->recs.data[i].q);
    fade_cache_free(s->recs.data[i].fade);
  }
  cw_array_free(&s->recs);
  cw_array_free(&s->faded);
  cw_array_free(&s->fade_lights);
  cw_array_free(&s->dir_lights);
  cw_array_free(&s->local_lights);
  cw__vis_bvh_free(&s->bvh);
  cw_array_free(&s->rooms);
  cw_array_free(&s->portals);
  for (int i = 0; i < s->rf.count; i++) cw_array_free(&s->rf.data[i].f);
  cw_array_free(&s->rf);
  cw_array_free(&s->hidden);
  cw__vis_impostors_free(s->imp);
  if (s->box_prog) glDeleteProgram(s->box_prog);
  if (s->box_vbo) glDeleteBuffers(1, &s->box_vbo);
  if (s->box_vao) glDeleteVertexArrays(1, &s->box_vao);
#if CW_WGPU
  wq_free(s->wq);
#endif
  cw_free(s);
}

static vis_state *vstate(cw_tree *t) {
  vis_state *s = cw_tree_slot(t, "visibility");
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
    s->tree = t;
    s->bvh.root = -1;
    s->imp = cw__vis_impostors_new();
    cw_tree_set_slot(t, "visibility", s, state_free);
  }
  return s;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* the first VisibilityManager in tree order */
static bool find_manager(cw_node n, void *u) {
  cw_node *out = u;
  if (!CW_HANDLE_IS_NULL(*out)) return false;
  if (cw_node_data_t(n, manager_t)) *out = n;
  return true;
}

/* ── records ────────────────────────────────────────────────────────── */

typedef struct collect { vis_state *s; } collect;
static bool collect_fn(cw_node n, void *u) {
  vis_state *s = ((collect *)u)->s;
  if (cw_node_get_type(n) && cw_node_type_is(cw_node_get_type(n), "MeshInstance3D")) {
    vrec r = { 0 };
    r.node = n;
    cw_array_push(&s->recs, r);
  } else if (cw_node_data_t(n, room_t)) {
    cw_array_push(&s->rooms, (vroom){ .node = n });
  } else if (cw_node_data_t(n, portal_t)) {
    cw_array_push(&s->portals, (vportal){ .node = n });
  } else if (cw_node_is(n, "DirectionalLight3D")) {
    cw_array_push(&s->dir_lights, n);
  } else if (cw_node_is(n, "OmniLight3D") || cw_node_is(n, "SpotLight3D")) {
    cw_array_push(&s->local_lights, n);
  }
  return true;
}

static void rebuild_records(vis_state *s) {
#if CW_WGPU
  wq_reset(s->wq); /* pending results name record indices */
#endif
  for (int i = 0; i < s->recs.count; i++) {
    if (s->recs.data[i].q) glDeleteQueries(1, &s->recs.data[i].q);
    fade_cache_free(s->recs.data[i].fade);
  }
  cw_array_clear(&s->recs);
  cw_array_clear(&s->rooms);
  cw_array_clear(&s->portals);
  cw_array_clear(&s->dir_lights);
  cw_array_clear(&s->local_lights);
  collect c = { s };
  cw_node_walk(cw_tree_root(s->tree), collect_fn, &c);
  s->bvh_dirty = true;
  s->have_records = true;
  s->structure_version = s->tree->structure_version;
}

/* refresh a record's mesh info (cheap; the object can change when a
 * resource finishes loading or the primitive changes) */
/* returns true when the drawn object changed (the BVH must be rebuilt) */
static bool refresh_info(vrec *r) {
  cw__mesh_info info;
  if (!cw__mesh3d_info(r->node, &info)) {
    bool had = r->obj != NULL;
    r->obj = NULL;
    return had;
  }
  bool changed = info.obj != r->obj;
  if (changed) {
    r->have_local = false;
    r->have_world = false;
    r->wver = 0;
  }
  r->vis = info.vis;
  r->obj = info.obj;
  r->kind = info.kind;
  r->lod_levels = info.lod_levels;
  r->cast_shadow = info.cast_shadow;
  r->look_key = info.look_key;
  return changed;
}

typedef struct box_acc { float mn[3], mx[3]; t3_mat4 inv_root; bool any; } box_acc;
static void acc_mesh(t3_object *o, void *u) {
  box_acc *a = u;
  if (o->type == T3_INSTANCED_MESH) {
    /* Project forest cells provide a conservative, immutable sphere covering
     * all placements and both LODs; never cache the current LOD subset. */
    t3_instanced_mesh *im = (t3_instanced_mesh *)o;
    if (!im->_isph_fixed) return;
    t3_mat4 rel;
    t3_mat4_multiply(&rel, &a->inv_root, &o->matrix_world);
    for (int c = 0; c < 8; c++) {
      t3_vec3 p = { im->_isph[0] + (c & 1 ? 1 : -1) * im->_isph[3],
                    im->_isph[1] + (c & 2 ? 1 : -1) * im->_isph[3],
                    im->_isph[2] + (c & 4 ? 1 : -1) * im->_isph[3] };
      p = t3_vec3_apply_mat4(p, &rel);
      float v[3] = { p.x, p.y, p.z };
      for (int k = 0; k < 3; k++) { a->mn[k] = fminf(a->mn[k],v[k]); a->mx[k] = fmaxf(a->mx[k],v[k]); }
    }
    a->any = true;
    return;
  }
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_geometry *g = ((t3_mesh *)o)->geometry;
  if (!g || !g->attributes[T3_ATTR_POSITION]) return;
  if (!g->has_bounding_box) t3_geometry_compute_bounding_box(g);
  t3_mat4 rel;
  t3_mat4_multiply(&rel, &a->inv_root, &o->matrix_world);
  for (int c = 0; c < 8; c++) {
    t3_vec3 p = { c & 1 ? g->bounding_box.max.x : g->bounding_box.min.x, c & 2 ? g->bounding_box.max.y : g->bounding_box.min.y,
                  c & 4 ? g->bounding_box.max.z : g->bounding_box.min.z };
    p = t3_vec3_apply_mat4(p, &rel);
    float v[3] = { p.x, p.y, p.z };
    for (int k = 0; k < 3; k++) {
      a->mn[k] = fminf(a->mn[k], v[k]);
      a->mx[k] = fmaxf(a->mx[k], v[k]);
    }
    a->any = true;
  }
}

/* the object's bounds in its own space (once: LOD switches keep the box of
 * the full mesh) */
static void local_bounds(vrec *r) {
  box_acc a = { { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f } };
  t3_mat4_invert(&a.inv_root, &r->obj->matrix_world);
  t3_object_traverse(r->obj, acc_mesh, &a);
  if (!a.any) {
    for (int k = 0; k < 3; k++) a.mn[k] = a.mx[k] = 0;
  }
  memcpy(r->lmin, a.mn, sizeof a.mn);
  memcpy(r->lmax, a.mx, sizeof a.mx);
  r->have_local = true;
}

static void world_bounds(vrec *r) {
  const float *m = r->obj->matrix_world.e;
  float c[3], e[3];
  for (int k = 0; k < 3; k++) {
    c[k] = (r->lmin[k] + r->lmax[k]) * 0.5f;
    e[k] = (r->lmax[k] - r->lmin[k]) * 0.5f;
  }
  for (int k = 0; k < 3; k++) {
    float wc = m[12 + k] + m[k] * c[0] + m[4 + k] * c[1] + m[8 + k] * c[2];
    float we = fabsf(m[k]) * e[0] + fabsf(m[4 + k]) * e[1] + fabsf(m[8 + k]) * e[2];
    r->bmin[k] = wc - we;
    r->bmax[k] = wc + we;
    r->center[k] = wc;
  }
  float ex = r->bmax[0] - r->center[0], ey = r->bmax[1] - r->center[1], ez = r->bmax[2] - r->center[2];
  r->radius = sqrtf(ex * ex + ey * ey + ez * ez);
  r->scale = t3_mat4_max_scale_on_axis(&r->obj->matrix_world);
  r->wver = r->obj->_world_version;
  r->have_world = true;
}

/* the rooms a box overlaps, and the outside unless one room contains it */
static uint32_t rooms_mask(const vis_state *s, const float mn[3], const float mx[3]) {
  if (s->rooms.count > 31) return 0xffffffffu;
  uint32_t m = 0;
  bool contained = false;
  for (int i = 0; i < s->rooms.count; i++) {
    const vroom *rm = &s->rooms.data[i];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int c = 0; c < 8; c++) {
      t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ c & 1 ? mx[0] : mn[0], c & 2 ? mx[1] : mn[1], c & 4 ? mx[2] : mn[2] }, &rm->inv);
      float v[3] = { p.x, p.y, p.z };
      for (int k = 0; k < 3; k++) {
        lo[k] = fminf(lo[k], v[k]);
        hi[k] = fmaxf(hi[k], v[k]);
      }
    }
    bool overlap = true, inside = true;
    for (int k = 0; k < 3; k++) {
      if (lo[k] > rm->ext[k] || hi[k] < -rm->ext[k]) overlap = false;
      if (lo[k] < -rm->ext[k] || hi[k] > rm->ext[k]) inside = false;
    }
    if (overlap) m |= 1u << (i + 1);
    contained |= inside;
  }
  return contained ? m : m | 1u;
}

static int room_of_pad(const vis_state *s, const float p[3], float pad) {
  for (int i = 0; i < s->rooms.count; i++) {
    const vroom *rm = &s->rooms.data[i];
    t3_vec3 l = t3_vec3_apply_mat4((t3_vec3){ p[0], p[1], p[2] }, &rm->inv);
    if (fabsf(l.x) <= rm->ext[0] + pad && fabsf(l.y) <= rm->ext[1] + pad && fabsf(l.z) <= rm->ext[2] + pad) return i + 1;
  }
  return 0;
}
static int room_of(const vis_state *s, const float p[3]) { return room_of_pad(s, p, 0); }

/* ── portals ────────────────────────────────────────────────────────── */

static void update_rooms(vis_state *s) {
  for (int i = 0; i < s->rooms.count; i++) {
    vroom *rm = &s->rooms.data[i];
    room3d *d = cw_node_data_t(rm->node, room_t);
    t3_mat4_invert(&rm->inv, cw_node3d_global_transform(rm->node));
    rm->ext[0] = d->extents.x;
    rm->ext[1] = d->extents.y;
    rm->ext[2] = d->extents.z;
  }
  for (int i = 0; i < s->portals.count; i++) {
    vportal *p = &s->portals.data[i];
    portal3d *d = cw_node_data_t(p->node, portal_t);
    const t3_mat4 *W = cw_node3d_global_transform(p->node);
    float hx = d->size.x * 0.5f, hy = d->size.y * 0.5f;
    static const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
    for (int k = 0; k < 4; k++) {
      t3_vec3 w = t3_vec3_apply_mat4((t3_vec3){ sx[k] * hx, sy[k] * hy, 0 }, W);
      p->c[k][0] = w.x;
      p->c[k][1] = w.y;
      p->c[k][2] = w.z;
    }
    t3_vec3 c = t3_mat4_get_position(W), n = t3_vec3_normalize(t3_vec3_transform_direction((t3_vec3){ 0, 0, 1 }, W));
    p->center[0] = c.x;
    p->center[1] = c.y;
    p->center[2] = c.z;
    p->normal[0] = n.x;
    p->normal[1] = n.y;
    p->normal[2] = n.z;
    t3_vec3 ux = t3_vec3_normalize(t3_vec3_transform_direction((t3_vec3){ 1, 0, 0 }, W));
    t3_vec3 uy = t3_vec3_normalize(t3_vec3_transform_direction((t3_vec3){ 0, 1, 0 }, W));
    float scx = sqrtf(W->e[0] * W->e[0] + W->e[1] * W->e[1] + W->e[2] * W->e[2]);
    float scy = sqrtf(W->e[4] * W->e[4] + W->e[5] * W->e[5] + W->e[6] * W->e[6]);
    p->ax[0] = ux.x;
    p->ax[1] = ux.y;
    p->ax[2] = ux.z;
    p->ay[0] = uy.x;
    p->ay[1] = uy.y;
    p->ay[2] = uy.z;
    p->hx = hx * scx;
    p->hy = hy * scy;
    /* the room on each side: probe a little way out (walls leave a gap
     * between a room's box and the opening); none = the outside */
    static const float probe[4] = { 0.05f, 0.25f, 0.5f, 1.0f };
    p->a = p->b = 0;
    for (int k = 0; k < 4 && !p->a; k++) {
      float q[3] = { c.x + n.x * probe[k], c.y + n.y * probe[k], c.z + n.z * probe[k] };
      p->a = room_of(s, q);
    }
    for (int k = 0; k < 4 && !p->b; k++) {
      float q[3] = { c.x - n.x * probe[k], c.y - n.y * probe[k], c.z - n.z * probe[k] };
      p->b = room_of(s, q);
    }
    p->enabled = d->enabled;
  }
}

/* clip a convex polygon by the frustum's planes (Sutherland-Hodgman) */
static int clip_poly(const vis_frustum *f, float (*in)[3], int n, float (*out)[3]) {
  float buf[2][16][3];
  float (*src)[3] = in;
  int ns = n;
  for (int p = 0; p < f->n && ns > 0; p++) {
    float (*dst)[3] = buf[p & 1];
    int nd = 0;
    for (int i = 0; i < ns; i++) {
      const float *a = src[i], *b = src[(i + 1) % ns];
      float da = f->nx[p] * a[0] + f->ny[p] * a[1] + f->nz[p] * a[2] + f->d[p];
      float db = f->nx[p] * b[0] + f->ny[p] * b[1] + f->nz[p] * b[2] + f->d[p];
      if (da >= 0 && nd < 16) memcpy(dst[nd++], a, sizeof(float) * 3);
      if ((da >= 0) != (db >= 0) && nd < 16) {
        float t = da / (da - db);
        for (int k = 0; k < 3; k++) dst[nd][k] = a[k] + (b[k] - a[k]) * t;
        nd++;
      }
    }
    src = dst;
    ns = nd;
  }
  for (int i = 0; i < ns; i++) memcpy(out[i], src[i], sizeof(float) * 3);
  return ns;
}

static void visit_room(vis_state *s, int room, const vis_frustum *f, int depth) {
  cw_array_push(&s->rf.data[room].f, *f);
  if (depth >= 8) return;
  for (int i = 0; i < s->portals.count; i++) {
    const vportal *p = &s->portals.data[i];
    if (!p->enabled || (p->a != room && p->b != room) || p->a == p->b) continue;
    int other = p->a == room ? p->b : p->a;
    /* the camera in the portal's opening: the other side sees the same view */
    float dc = (s->cam[0] - p->center[0]) * p->normal[0] + (s->cam[1] - p->center[1]) * p->normal[1] +
               (s->cam[2] - p->center[2]) * p->normal[2];
    float lx = (s->cam[0] - p->center[0]) * p->ax[0] + (s->cam[1] - p->center[1]) * p->ax[1] + (s->cam[2] - p->center[2]) * p->ax[2];
    float ly = (s->cam[0] - p->center[0]) * p->ay[0] + (s->cam[1] - p->center[1]) * p->ay[1] + (s->cam[2] - p->center[2]) * p->ay[2];
    if (fabsf(dc) < 0.35f && fabsf(lx) < p->hx + 0.3f && fabsf(ly) < p->hy + 0.3f) {
      visit_room(s, other, f, depth + 1);
      continue;
    }
    float poly[16][3];
    int n = clip_poly(f, (float (*)[3])p->c, 4, poly);
    if (n < 3) continue;
    vis_frustum nf = { 0 };
    float cx = 0, cy = 0, cz = 0;
    for (int k = 0; k < n; k++) {
      cx += poly[k][0] / n;
      cy += poly[k][1] / n;
      cz += poly[k][2] / n;
    }
    for (int k = 0; k < n && nf.n < VIS_MAX_PLANES - 1; k++) {
      const float *a = poly[k], *b = poly[(k + 1) % n];
      t3_vec3 e1 = { a[0] - s->cam[0], a[1] - s->cam[1], a[2] - s->cam[2] }, e2 = { b[0] - s->cam[0], b[1] - s->cam[1], b[2] - s->cam[2] };
      t3_vec3 nn = t3_vec3_cross(e1, e2);
      float l = t3_vec3_length(nn);
      if (l < 1e-9f) continue;
      nn = t3_vec3_scale(nn, 1 / l);
      float d = -(nn.x * s->cam[0] + nn.y * s->cam[1] + nn.z * s->cam[2]);
      if (nn.x * cx + nn.y * cy + nn.z * cz + d < 0) {
        nn = t3_vec3_scale(nn, -1);
        d = -d;
      }
      nf.nx[nf.n] = nn.x;
      nf.ny[nf.n] = nn.y;
      nf.nz[nf.n] = nn.z;
      nf.d[nf.n] = d;
      nf.n++;
    }
    /* the camera's far plane bounds every view */
    nf.nx[nf.n] = s->far_plane[0];
    nf.ny[nf.n] = s->far_plane[1];
    nf.nz[nf.n] = s->far_plane[2];
    nf.d[nf.n] = s->far_plane[3];
    nf.n++;
    visit_room(s, other, &nf, depth + 1);
  }
}

/* ── the cull ───────────────────────────────────────────────────────── */

typedef struct cull_ctx {
  vis_state *s;
  const vis_frustum *f;
  const float *sweep;
  uint8_t *in;
  float viewport_h, proj_scale, lod_budget;
  bool lod, use_rooms, shadows, keep_casters;
} cull_ctx;

static void bounds_range(int begin, int end, int worker, void *u) {
  (void)worker;
  vis_state *s = u;
  for (int i = begin; i < end; i++) {
    vrec *r = &s->recs.data[i];
    if (!r->obj || !r->have_local) continue;
    if (!r->have_world || r->wver != r->obj->_world_version) world_bounds(r);
  }
}

static void dyn_range(int begin, int end, int worker, void *u) {
  (void)worker;
  cull_ctx *c = u;
  for (int i = begin; i < end; i++) {
    vrec *r = &c->s->recs.data[i];
    if (!r->obj || r->is_static) continue;
    c->in[i] = (uint8_t)cw__vis_box_test(c->f, r->bmin, r->bmax, r->cast_shadow ? c->sweep : NULL);
  }
}

static void classify_range(int begin, int end, int worker, void *u) {
  (void)worker;
  cull_ctx *c = u;
  vis_state *s = c->s;
  for (int i = begin; i < end; i++) {
    vrec *r = &s->recs.data[i];
    r->cand = 0;
    r->lod = -1;
    r->imp_alpha = 0;
    if (!r->obj) continue;
    if (c->f && !c->in[i] && !(c->keep_casters && r->cast_shadow)) {
      r->reason = R_FRUSTUM;
      continue;
    }
    /* a swept caster that is not itself in view: keep it for its shadow */
    float dx = r->center[0] - s->cam[0], dy = r->center[1] - s->cam[1], dz = r->center[2] - s->cam[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (r->vis->visibility_range > 0 && dist - r->radius > r->vis->visibility_range) {
      r->reason = R_DISTANCE;
      continue;
    }
    if (c->use_rooms && !(c->shadows && r->cast_shadow)) {
      bool ok = false;
      for (int b = 0; b <= s->rooms.count && b < 32 && !ok; b++) {
        if (!(r->rooms & (1u << b))) continue;
        const room_frusta *rf = &s->rf.data[b];
        for (int k = 0; k < rf->f.count && !ok; k++) ok = cw__vis_box_test(&rf->f.data[k], r->bmin, r->bmax, NULL) != 0;
      }
      if (!ok) {
        r->reason = R_PORTAL;
        continue;
      }
    }
    r->cand = 1;
    r->reason = R_VISIBLE;
    /* LOD: the coarsest level whose error stays within the budget */
    int lod = 0;
    if (c->lod && r->lod_levels > 1 && r->kind != CW__MESH_GLTF) {
      float d = fmaxf(dist - r->radius, 1e-3f);
      float budget = c->lod_budget * r->vis->lod_bias;
      for (int l = 1; l < r->lod_levels; l++) {
        float px = cw__mesh3d_lod_error(r->node, l) * r->scale * c->proj_scale / d;
        if (px <= budget) lod = l;
        else break;
      }
    }
    r->lod = lod;
    if (r->vis->impostor) {
      float D = r->vis->impostor_distance, F = r->vis->impostor_fade;
      if (dist >= D) r->imp_alpha = 1;
      else if (F > 0 && dist > D - F) r->imp_alpha = (dist - (D - F)) / F;
    }
  }
}

/* ── the mesh side of an impostor crossfade ──────────────────────────── */

static void fade_cache_free(fade_cache *fc) {
  if (!fc) return;
  for (int i = 0; i < fc->slots.count; i++)
    for (int k = 0; k < fc->slots.data[i].count; k++) t3_release(fc->slots.data[i].clone[k]);
  cw_array_free(&fc->slots);
  cw_free(fc);
}

typedef struct fade_build { fade_cache *fc; } fade_build;
static void fade_collect(t3_object *o, void *u) {
  fade_cache *fc = ((fade_build *)u)->fc;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  fade_slot s = { m, CW_MIN(m->material_count, 4) };
  for (int k = 0; k < s.count; k++) {
    s.orig[k] = m->materials[k];
    s.clone[k] = t3_material_clone(m->materials[k]);
    s.clone[k]->transparent = true;
    s.clone[k]->depth_write = false; /* a faded mesh never hides what is behind it (occlusion queries, later passes) */
    s.clone[k]->version++;
  }
  cw_array_push(&fc->slots, s);
}

static bool fade_after_impostors(void) {
#if CW_WGPU
  return !cw__wgpu_on();
#else
  return true;
#endif
}

/* draw the record's meshes with opacity `op` this frame (clones of their
 * materials, transparent; the restore pass swaps the originals back), on
 * FADE_LAYER: not in the scene pass but after the impostors (fade_draw), so
 * an impostor behind the mesh is under it, as a mesh behind it would be.
 * Drawn in the scene pass, the opaque impostors drawn after it covered the
 * fading mesh wherever its silhouette reaches past its own impostor's: the
 * moment a mesh began to fade, the impostors behind it popped through. */
static void fade_mesh(vis_state *s, vrec *r, float op) {
  if (r->fade && r->fade->obj != r->obj) {
    fade_cache_free(r->fade);
    r->fade = NULL;
  }
  if (!r->fade) {
    r->fade = cw_calloc(1, sizeof *r->fade, CW_MEM_RENDER);
    r->fade->obj = r->obj;
    fade_build fb = { r->fade };
    t3_object_traverse(r->obj, fade_collect, &fb);
  }
  for (int i = 0; i < r->fade->slots.count; i++) {
    fade_slot *sl = &r->fade->slots.data[i];
    for (int k = 0; k < sl->count; k++) {
      /* the original may have changed since (a material edit): follow it */
      if (sl->mesh->materials[k] != sl->orig[k] && sl->mesh->materials[k] != sl->clone[k]) {
        t3_release(sl->clone[k]);
        sl->orig[k] = sl->mesh->materials[k];
        sl->clone[k] = t3_material_clone(sl->orig[k]);
        sl->clone[k]->transparent = true;
        sl->clone[k]->depth_write = false;
        sl->clone[k]->version++;
      }
      t3_material *c = sl->clone[k];
      c->color = sl->orig[k]->color;
      c->opacity = sl->orig[k]->opacity * op;
      sl->mesh->materials[k] = c;
    }
    /* the field, not t3_object_set_layers: the restore pass puts it back
     * before anything raycasts, so the raycast candidate cache stays valid.
     * (WebGPU: fade_draw is GL's; a fading mesh stays in the scene pass) */
    if (fade_after_impostors() && sl->mesh->base.layers != FADE_LAYER) {
      sl->layers = sl->mesh->base.layers;
      if (sl->layers & s->cam_layers) sl->mesh->base.layers = FADE_LAYER;
    }
  }
  cw_array_push(&s->faded, r->fade);
}

static void fade_restore(vis_state *s) {
  for (int i = 0; i < s->faded.count; i++) {
    fade_cache *fc = s->faded.data[i];
    for (int j = 0; j < fc->slots.count; j++) {
      fade_slot *sl = &fc->slots.data[j];
      for (int k = 0; k < sl->count; k++)
        if (sl->mesh->materials[k] == sl->clone[k]) sl->mesh->materials[k] = sl->orig[k];
      if (sl->mesh->base.layers == FADE_LAYER) sl->mesh->base.layers = sl->layers;
    }
  }
  cw_array_clear(&s->faded);
}

static void fade_light(t3_object *o, void *u) {
  vis_state *s = u;
  bool light = o->type == T3_AMBIENT_LIGHT || o->type == T3_HEMISPHERE_LIGHT || o->type == T3_DIRECTIONAL_LIGHT ||
               o->type == T3_POINT_LIGHT || o->type == T3_SPOT_LIGHT;
  if (light && (o->layers & s->cam_layers) && !(o->layers & FADE_LAYER)) {
    o->layers |= FADE_LAYER;
    cw_array_push(&s->fade_lights, o);
  }
}

/* the fading meshes, after the impostors (GL; WebGPU bakes no impostors yet,
 * so nothing fades there): a second render of the scene that sees only
 * FADE_LAYER, into the scene target as it stands (no clear, no background),
 * lit by the lights the camera sees */
static void fade_draw(vis_state *s, cw_pass_ctx *ctx) {
  if (!s->faded.count || !ctx->renderer || !ctx->scene3d || !ctx->camera) return;
  t3_renderer *r = ctx->renderer;
  t3_scene *sc = ctx->scene3d;
  t3_camera *cam = ctx->camera;
  t3_object_traverse(&sc->base, fade_light, s);
  const t3_render_info *ri = t3_renderer_info(r);
  unsigned calls = ri->calls, tris = ri->triangles;
  bool has_bg = sc->has_background, auto_clear = t3_renderer_get_auto_clear(r);
  struct t3_texture *bg_tex = sc->background_texture;
  sc->has_background = false;
  sc->background_texture = NULL;
  cam->base.layers = FADE_LAYER;
  t3_renderer_set_auto_clear(r, false);
  t3_renderer_reset_state(r); /* the impostors drew with their own GL */
  t3_renderer_set_framebuffer(r, ctx->pipeline->bound_fbo);
  t3_renderer_render(r, sc, cam);
  t3_renderer_set_auto_clear(r, auto_clear);
  cam->base.layers = s->cam_layers;
  sc->has_background = has_bg;
  sc->background_texture = bg_tex;
  for (int i = 0; i < s->fade_lights.count; i++) s->fade_lights.data[i]->layers &= ~FADE_LAYER;
  cw_array_clear(&s->fade_lights);
  /* the frame's counts are both renders' */
  t3_render_info *w = (t3_render_info *)ri;
  w->calls += calls;
  w->triangles += tris;
}

#if CW_WGPU
static void read_queries_wgpu(vis_state *s, bool wait);
#endif

/* read last frame's query results */
static void read_queries(vis_state *s, bool wait) {
#if CW_WGPU
  if (cw__wgpu_on()) {
    read_queries_wgpu(s, wait);
    return;
  }
#endif
  for (int i = 0; i < s->recs.count; i++) {
    vrec *r = &s->recs.data[i];
    if (!r->q_pending) continue;
    GLuint avail = 0, samples = 1;
    glGetQueryObjectuiv(r->q, GL_QUERY_RESULT_AVAILABLE, &avail);
#if !defined(__wasm__)
    if (!avail && wait) avail = 1; /* GL_QUERY_RESULT below waits for the GPU */
#else
    (void)wait;
#endif
    if (avail) {
      glGetQueryObjectuiv(r->q, GL_QUERY_RESULT, &samples);
      r->q_pending = false;
      r->occluded = samples == 0;
      s->stats.query_latency = (int)(s->frame - r->q_frame);
    } else {
      s->stats.queries_late++;
      /* late: in wait mode (a host whose results only arrive between
       * frames) at once, otherwise after two frames, it counts as visible
       * until the answer comes */
      if (wait || s->frame - r->q_frame > 2) r->occluded = false;
    }
  }
}

static float query_margin(const vis_state *s, const vrec *r) {
  float dx = r->center[0] - s->cam[0], dy = r->center[1] - s->cam[1], dz = r->center[2] - s->cam[2];
  return s->margin + sqrtf(dx * dx + dy * dy + dz * dz) * s->margin_ang;
}

static float box_dist(const float c[3], const float mn[3], const float mx[3]) {
  float d2 = 0;
  for (int k = 0; k < 3; k++) {
    float v = c[k] < mn[k] ? mn[k] - c[k] : c[k] > mx[k] ? c[k] - mx[k] : 0;
    d2 += v * v;
  }
  return sqrtf(d2);
}

/* the camera inside the grown box, or so near it that the box crosses the near
 * plane: a query would lose the clipped faces and could report an object
 * the camera stands next to as hidden, so it is not asked */
static bool cam_in_box(const vis_state *s, const vrec *r, float m) {
  float mn[3], mx[3];
  for (int k = 0; k < 3; k++) {
    mn[k] = r->bmin[k] - m;
    mx[k] = r->bmax[k] + m;
  }
  return box_dist(s->cam, mn, mx) < s->near_clip * 3 + 0.05f;
}

/* a grown box the view's edge (or near plane) cuts: while the camera moves,
 * what lies past the edge can come into view before a query sees it */
static bool box_on_edge(const vis_frustum *f, const vrec *r, float m) {
  float mn[3], mx[3];
  for (int k = 0; k < 3; k++) {
    mn[k] = r->bmin[k] - m;
    mx[k] = r->bmax[k] + m;
  }
  return cw__vis_box_test(f, mn, mx, NULL) != 2;
}

static void vis_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  CW_PROFILE_SCOPE("visibility.cull");
  vis_state *s = vstate(ctx->tree);
  cw_node mn = { 0 };
  cw_node_walk(cw_tree_root(ctx->tree), find_manager, &mn);
  vis_manager *mg = cw_node_data_t(mn, manager_t);
  s->mgr = mg;
  s->ran = false;
  if (!mg || !ctx->camera) return;
  double t0 = now_ms();
  s->ran = true;
  s->frame++;
  memset(&s->stats, 0, sizeof s->stats);
  if (!s->have_records || s->structure_version != ctx->tree->structure_version) rebuild_records(s);
  /* world matrices: the camera and every moving mesh now (static meshes keep
   * the ones three.c's last render computed: a static mesh that moves is
   * seen, and refitted, a frame late) */
  t3_object_update_world_matrix(ctx->camera, true, false);
  for (int i = 0; i < s->recs.count; i++) {
    vrec *r = &s->recs.data[i];
    if (refresh_info(r)) s->bvh_dirty = true;
    if (!r->obj) continue;
    bool st = r->vis->is_static;
    if (!st || !r->have_local) t3_object_update_world_matrix(r->obj, true, !r->have_local);
    if (!r->have_local) local_bounds(r);
    if (st != r->is_static) s->bvh_dirty = true;
    r->is_static = st;
    r->was_cand = r->cand;
  }
  update_rooms(s); /* before any mesh takes its rooms */
  /* world boxes of what moved; statics that moved refit the BVH */
  uint32_t *before = cw_frame_alloc(sizeof(uint32_t) * (size_t)(s->recs.count + 1));
  for (int i = 0; i < s->recs.count; i++) before[i] = s->recs.data[i].wver;
  cw_parallel_for(s->recs.count, 256, bounds_range, s);
  bool refit = false;
  for (int i = 0; i < s->recs.count; i++) {
    vrec *r = &s->recs.data[i];
    if (r->wver != before[i]) {
      if (r->is_static) {
        refit = true;
        if (s->rooms.count) r->rooms = rooms_mask(s, r->bmin, r->bmax);
      }
    }
    if (!r->is_static && s->rooms.count) r->rooms = rooms_mask(s, r->bmin, r->bmax);
  }
  if (s->bvh_dirty) {
    int ns = 0;
    for (int i = 0; i < s->recs.count; i++) ns += s->recs.data[i].obj && s->recs.data[i].is_static;
    float *mins = cw_frame_alloc(sizeof(float) * 3 * (size_t)(ns + 1)), *maxs = cw_frame_alloc(sizeof(float) * 3 * (size_t)(ns + 1));
    int32_t *ids = cw_frame_alloc(sizeof(int32_t) * (size_t)(ns + 1));
    int k = 0;
    for (int i = 0; i < s->recs.count; i++) {
      vrec *r = &s->recs.data[i];
      if (!r->obj || !r->is_static) continue;
      memcpy(mins + 3 * k, r->bmin, 12);
      memcpy(maxs + 3 * k, r->bmax, 12);
      ids[k++] = i;
      r->rooms = s->rooms.count ? rooms_mask(s, r->bmin, r->bmax) : 1;
    }
    cw__vis_bvh_build(&s->bvh, ns, mins, maxs, ids);
    s->bvh_dirty = false;
  } else if (refit && s->recs.count) {
    cw__vis_bvh_refit(&s->bvh, &s->recs.data[0].bmin[0], &s->recs.data[0].bmax[0], (int)(sizeof(vrec) / sizeof(float)));
  }

  /* the camera */
  t3_mat4 view, vp;
  t3_mat4_invert(&view, &ctx->camera->base.matrix_world);
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &view);
  s->vp = vp;
  t3_vec3 cp = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  s->cam[0] = cp.x;
  s->cam[1] = cp.y;
  s->cam[2] = cp.z;
  s->cam_layers = ctx->camera->base.layers;
  float moved = 0;
  if (s->have_prev_cam)
    moved = sqrtf((cp.x - s->prev_cam[0]) * (cp.x - s->prev_cam[0]) + (cp.y - s->prev_cam[1]) * (cp.y - s->prev_cam[1]) +
                  (cp.z - s->prev_cam[2]) * (cp.z - s->prev_cam[2]));
  const float *we = ctx->camera->base.matrix_world.e;
  float fwd[3] = { -we[8], -we[9], -we[10] };
  float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  for (int k = 0; k < 3; k++) fwd[k] /= fl > 0 ? fl : 1;
  float turn = 0;
  if (s->have_prev_cam) {
    float c = fwd[0] * s->prev_fwd[0] + fwd[1] * s->prev_fwd[1] + fwd[2] * s->prev_fwd[2];
    turn = acosf(CW_CLAMP(c, -1, 1));
  }
  memcpy(s->prev_cam, s->cam, sizeof s->cam);
  memcpy(s->prev_fwd, fwd, sizeof fwd);
  s->have_prev_cam = true;
  /* query boxes grow by the camera's motion over three frames of latency:
   * translation directly, turning in proportion to distance */
  s->margin = mg->occlusion_margin + moved * 3;
  /* parallax: an occluder at distance n sweeps moved / n radians a frame across
   * what lies behind it; n is the nearest static box drawn last frame */
  float n_occ = fmaxf(s->near_occluder, 0.5f);
  /* (six frames of motion: the nearest box under-estimates how close an
   * occluder's edge can sweep across a narrow opening) */
  s->margin_ang = mg->occlusion_guard ? tanf(fminf(turn * 6 + moved * 6 / n_occ, 1.2f)) : 0;
  if (!mg->occlusion_guard) s->margin = mg->occlusion_margin;
  s->moved = moved;
  s->turn = turn;
  s->occl_off = mg->occlusion_guard && s->near_occluder < mg->occlusion_margin + moved * 3;
  s->near_clip = ctx->camera->near;
  vis_frustum f;
  cw__vis_frustum_from(&f, &vp);
  /* three.js plane order: right, left, bottom, top, far, near */
  s->far_plane[0] = f.nx[4];
  s->far_plane[1] = f.ny[4];
  s->far_plane[2] = f.nz[4];
  s->far_plane[3] = f.d[4];

  /* shadows: a DirectionalLight3D with shadow on sweeps casters along it */
  float sweep[3] = { 0, 0, 0 };
  bool shadows = false;
  for (int i = 0; i < s->dir_lights.count; i++) {
    cw_value v;
    if (!cw_node_valid(s->dir_lights.data[i]) || !cw_node_get_prop(s->dir_lights.data[i], "shadow", &v) || !v.b) continue;
    t3_vec3 d = t3_vec3_transform_direction((t3_vec3){ 0, 0, -1 }, cw_node3d_global_transform(s->dir_lights.data[i]));
    d = t3_vec3_normalize(d);
    sweep[0] = d.x * mg->shadow_distance;
    sweep[1] = d.y * mg->shadow_distance;
    sweep[2] = d.z * mg->shadow_distance;
    shadows = true;
    break;
  }
  /* an omni or spot light with shadow on: its shadow maps may need any caster
   * (they cover all directions), so casters are never culled */
  bool keep_casters = false;
  for (int i = 0; i < s->local_lights.count && !keep_casters; i++) {
    cw_value v;
    keep_casters = cw_node_valid(s->local_lights.data[i]) && cw_node_get_prop(s->local_lights.data[i], "shadow", &v) && v.b;
  }
  if (keep_casters) shadows = true;

  /* occlusion results from the frame before */
  if (mg->occlusion_cull) read_queries(s, mg->occlusion_wait);

  /* portals */
  bool use_rooms = mg->portals && s->rooms.count > 0;
  if (use_rooms) {
    while (s->rf.count < s->rooms.count + 1) cw_array_push(&s->rf, (room_frusta){ 0 });
    for (int i = 0; i < s->rf.count; i++) cw_array_clear(&s->rf.data[i].f);
    int cr = room_of(s, s->cam);
    /* in a doorway (inside the wall, between two rooms' boxes): both sides */
    int door = -1;
    for (int i = 0; i < s->portals.count && !cr && door < 0; i++) {
      const vportal *p = &s->portals.data[i];
      float d[3] = { s->cam[0] - p->center[0], s->cam[1] - p->center[1], s->cam[2] - p->center[2] };
      float dn = d[0] * p->normal[0] + d[1] * p->normal[1] + d[2] * p->normal[2];
      float dx = d[0] * p->ax[0] + d[1] * p->ax[1] + d[2] * p->ax[2], dy = d[0] * p->ay[0] + d[1] * p->ay[1] + d[2] * p->ay[2];
      if (p->enabled && fabsf(dn) < 0.35f && fabsf(dx) < p->hx && fabsf(dy) < p->hy) door = i;
    }
    if (door >= 0) {
      visit_room(s, s->portals.data[door].a, &f, 0);
      visit_room(s, s->portals.data[door].b, &f, 0);
    } else visit_room(s, cr, &f, 0);
    for (int i = 0; i < s->rooms.count + 1; i++) s->stats.rooms_visible += s->rf.data[i].f.count > 0 && i > 0;
  }

  /* frustum: the BVH for statics, a flat pass for the rest */
  uint8_t *in = cw_frame_calloc((size_t)s->recs.count + 1, 1);
  cull_ctx cc = { s, mg->frustum_cull ? &f : NULL, shadows ? sweep : NULL, in };
  if (mg->frustum_cull) {
    s->stats.bvh_nodes = cw__vis_bvh_cull(&s->bvh, &f, shadows ? sweep : NULL, in);
    cw_parallel_for(s->recs.count, 256, dyn_range, &cc);
    /* swept boxes found casters: everything else is tested exactly */
    if (shadows)
      for (int i = 0; i < s->recs.count; i++) {
        vrec *r = &s->recs.data[i];
        if (in[i] && r->obj && !r->cast_shadow) in[i] = (uint8_t)cw__vis_box_test(&f, r->bmin, r->bmax, NULL);
      }
  }
  float fov = ctx->camera->fov > 0 ? ctx->camera->fov : 60;
  float vh = (float)ctx->height;
  cc.viewport_h = vh;
  cc.proj_scale = ctx->camera->base.type == T3_PERSPECTIVE_CAMERA ? vh / (2 * tanf(fov * (CW_PI / 360))) : vh / 2;
  cc.lod_budget = mg->lod_error_px * cw__quality_lod_bias(); /* cartwheel/quality.h: effects detail */
  cc.lod = mg->lod;
  cc.use_rooms = use_rooms;
  cc.shadows = shadows;
  cc.keep_casters = keep_casters;
  cw_parallel_for(s->recs.count, 128, classify_range, &cc);

  /* serial: glTF LODs, occlusion, impostors, hiding */
  cw__vis_impostors_begin_frame(s->imp);
  cw_array_clear(&s->hidden);
  for (int i = 0; i < s->recs.count; i++) {
    vrec *r = &s->recs.data[i];
    r->want_query = false;
    r->drawn = 0;
    if (!r->obj) continue;
    s->stats.meshes++;
    s->stats.static_meshes += r->is_static;
    if (r->cand) {
      bool exempt = !r->vis->occlusion_cull || (shadows && r->cast_shadow);
      if (!r->was_cand) r->occluded = false; /* new to the view: visible until a query says otherwise */
      if (mg->occlusion_cull && !exempt) {
        float qm = query_margin(s, r);
        if (s->occl_off || cam_in_box(s, r, qm) ||
            (mg->occlusion_guard && (s->moved > 1e-5f || s->turn > 1e-6f) && box_on_edge(&f, r, qm)))
          r->occluded = false;
        else if (!r->q_pending && (s->frame + (uint64_t)i) % (uint64_t)CW_MAX(r->occluded ? mg->occlusion_requery_occluded : mg->occlusion_interval, 1) == 0)
          r->want_query = true;
        if (r->occluded) {
          r->cand = 0;
          r->reason = R_OCCLUSION;
        }
      } else r->occluded = false;
    }
    if (!r->cand) {
      switch (r->reason) {
      case R_FRUSTUM: s->stats.frustum_culled++; break;
      case R_DISTANCE: s->stats.distance_culled++; break;
      case R_PORTAL: s->stats.portal_culled++; break;
      case R_OCCLUSION: s->stats.occlusion_culled++; break;
      default: break;
      }
      r->vis->lod = -1;
      r->obj->visible = false;
      cw_array_push(&s->hidden, r->obj);
      continue;
    }
    /* drawn */
    r->drawn = 1;
    s->stats.visible++;
    if (mg->impostors && r->imp_alpha > 0) {
      cw__mesh_info info;
      cw__mesh3d_info(r->node, &info);
      float rad, cl[3];
      int atlas = cw__vis_impostor_atlas(s->imp, ctx, r->node, &info, &rad, cl);
      if (atlas >= 0) {
        t3_object *g = cw_node3d_object(r->node);
        t3_vec3 gp, gs;
        t3_quat gq;
        t3_mat4_decompose(&g->matrix_world, &gp, &gq, &gs);
        t3_vec3 cw = t3_vec3_add(gp, t3_vec3_apply_quat((t3_vec3){ cl[0], cl[1], cl[2] }, gq));
        vis_impostor_inst inst = { { cw.x, cw.y, cw.z }, rad, { gq.x, gq.y, gq.z, gq.w }, CW_MIN(2 * r->imp_alpha, 1) };
        cw__vis_impostor_add(s->imp, atlas, &inst);
        if (r->imp_alpha >= 1) {
          s->stats.impostors++;
          r->vis->lod = 100;
          r->obj->visible = false;
          cw_array_push(&s->hidden, r->obj);
          continue;
        }
        s->stats.impostor_fading++;
        /* the mesh fades out over the second half of the band, under an
         * impostor that is opaque by then: the switch never pops */
        float op = CW_CLAMP(2 * (1 - r->imp_alpha), 0, 1);
        if (op < 1) fade_mesh(s, r, op);
      }
    }
    if (mg->lod && r->lod_levels > 1) {
      if (r->kind == CW__MESH_GLTF) {
        cw_gltf_instance_auto_lod(r->obj, ctx->camera, (float)ctx->height, mg->lod_error_px * cw__quality_lod_bias() * r->vis->lod_bias);
        r->lod = cw_gltf_instance_lod(r->obj);
      } else cw__mesh3d_set_lod(r->node, r->lod);
    }
    r->vis->lod = r->lod;
    s->stats.lod_counts[CW_CLAMP(r->lod, 0, 3)]++;
  }
  /* the nearest static box drawn: next frame's parallax bound */
  float near = 1e9f;
  for (int i = 0; i < s->recs.count; i++) {
    const vrec *r = &s->recs.data[i];
    if (r->drawn && r->is_static) near = fminf(near, box_dist(s->cam, r->bmin, r->bmax));
  }
  s->near_occluder = near;
  s->stats.impostor_atlases = cw__vis_impostor_atlas_count(s->imp);
  s->stats.rooms = s->rooms.count;
  s->stats.cull_ms = (float)(now_ms() - t0);
}

/* ── occlusion queries ──────────────────────────────────────────────── */

static const char *const box_vs =
    "#version 300 es\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "uniform mat4 u_vp;\n"
    "uniform vec3 u_min, u_max;\n"
    "void main() { gl_Position = u_vp * vec4(mix(u_min, u_max, a_pos), 1.0); }\n";
static const char *const box_fs = "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n";

static bool box_gl(vis_state *s) {
  if (s->box_prog) return true;
  GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(v, 1, &box_vs, NULL);
  glShaderSource(f, 1, &box_fs, NULL);
  glCompileShader(v);
  glCompileShader(f);
  s->box_prog = glCreateProgram();
  glAttachShader(s->box_prog, v);
  glAttachShader(s->box_prog, f);
  glLinkProgram(s->box_prog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(s->box_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    cw_error("visibility: the occlusion box program failed to build");
    glDeleteProgram(s->box_prog);
    s->box_prog = 0;
    return false;
  }
  /* 0..1 cube, 12 triangles */
  static const float c[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
                                 { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } };
  static const int idx[36] = { 0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                               3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2 };
  float v36[36 * 3];
  for (int i = 0; i < 36; i++) memcpy(v36 + 3 * i, c[idx[i]], 12);
  glGenBuffers(1, &s->box_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, s->box_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof v36, v36, GL_STATIC_DRAW);
  glGenVertexArrays(1, &s->box_vao);
  glBindVertexArray(s->box_vao);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, 0);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
}

static bool queries_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  vis_state *s = cw_tree_slot(ctx->tree, "visibility");
  return s && s->ran && s->mgr && s->mgr->occlusion_cull;
}

#if CW_WGPU
static void queries_exec_wgpu(cw_pass_ctx *ctx, vis_state *s);
#endif

static void queries_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  vis_state *s = vstate(ctx->tree);
#if CW_WGPU
  if (cw__wgpu_on()) {
    queries_exec_wgpu(ctx, s);
    return;
  }
#endif
  if (!box_gl(s)) return;
  glUseProgram(s->box_prog);
  GLint lvp = glGetUniformLocation(s->box_prog, "u_vp"), lmn = glGetUniformLocation(s->box_prog, "u_min"),
        lmx = glGetUniformLocation(s->box_prog, "u_max");
  /* the jittered projection the scene drew with (TAA): ctx->camera's */
  t3_mat4 view, vp;
  t3_mat4_invert(&view, &ctx->camera->base.matrix_world);
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &view);
  glUniformMatrix4fv(lvp, 1, GL_FALSE, vp.e);
  glBindVertexArray(s->box_vao);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDisable(GL_CULL_FACE);
  int n = 0;
  for (int i = 0; i < s->recs.count; i++) {
    vrec *r = &s->recs.data[i];
    if (!r->want_query) continue;
    if (!r->q) glGenQueries(1, &r->q);
    float m = query_margin(s, r);
    glUniform3f(lmn, r->bmin[0] - m, r->bmin[1] - m, r->bmin[2] - m);
    glUniform3f(lmx, r->bmax[0] + m, r->bmax[1] + m, r->bmax[2] + m);
    glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, r->q);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
    r->q_pending = true;
    r->q_frame = s->frame;
    n++;
  }
  s->stats.queries = n;
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glBindVertexArray(0);
}

#if CW_WGPU
/* ── occlusion queries on WebGPU ──────────────────────────────────────
 * One occlusion query set; each frame the boxes that want a query are drawn
 * depth-tested (no depth writes, no colour) against the scene depth in one
 * render pass, the set is resolved into a buffer and read back
 * asynchronously. A result names the frame it was made in; read_queries
 * applies it to the records queried that frame (results arrive a frame or
 * more late, as GL's do: late = visible after two frames, at once in wait
 * mode; a result that never arrives also leaves its records visible). */
#define WQ_PEND 8
typedef struct vis_wq {
  WGPUQuerySet set;
  WGPUBuffer resolve, ubo, vbo;
  int cap;
  cw__wg_readback *rb;
  WGPUBindGroupLayout bgl;
  WGPURenderPipeline pipe;
  uint32_t pipe_key;
  struct { uint64_t frame; int n; int *recs; } pend[WQ_PEND]; /* a slot per readback slot in flight */
  int pend_next;
} vis_wq;

static void wq_reset(vis_wq *q) {
  if (!q) return;
  for (int i = 0; i < WQ_PEND; i++) q->pend[i].n = 0;
}

static void wq_free(vis_wq *q) {
  if (!q) return;
  if (q->set) wgpuQuerySetRelease(q->set);
  if (q->resolve) wgpuBufferRelease(q->resolve);
  if (q->ubo) wgpuBufferRelease(q->ubo);
  if (q->vbo) wgpuBufferRelease(q->vbo);
  if (q->pipe) wgpuRenderPipelineRelease(q->pipe);
  if (q->bgl) wgpuBindGroupLayoutRelease(q->bgl);
  cw__wg_readback_free(q->rb);
  for (int i = 0; i < WQ_PEND; i++) cw_free(q->pend[i].recs);
  cw_free(q);
}

static const char WGSL_QBOX[] =
    "struct U { vp: mat4x4f, mn: vec4f, mx: vec4f }\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@vertex fn vs(@location(0) a: vec3f) -> @builtin(position) vec4f {\n"
    "  return cw_clip(u.vp * vec4f(mix(u.mn.xyz, u.mx.xyz, a), 1.0), u.mn.w); }\n";

static void read_queries_wgpu(vis_state *s, bool wait) {
  vis_wq *q = s->wq;
  uint64_t *res = q && q->rb ? cw_frame_alloc((size_t)q->cap * 8) : NULL;
  uint64_t tag;
  while (res && cw__wg_readback_take(q->rb, res, &tag)) {
    {
      for (int p = 0; p < WQ_PEND; p++) {
        if (q->pend[p].frame != tag || !q->pend[p].n) continue;
        for (int k = 0; k < q->pend[p].n; k++) {
          int ri = q->pend[p].recs[k];
          if (ri >= s->recs.count) continue;
          vrec *r = &s->recs.data[ri];
          if (!r->q_pending || r->q_frame != tag) continue;
          r->q_pending = false;
          r->occluded = res[k] == 0;
          s->stats.query_latency = (int)(s->frame - r->q_frame);
        }
        q->pend[p].n = 0;
      }
    }
  }
  for (int i = 0; i < s->recs.count; i++) {
    vrec *r = &s->recs.data[i];
    if (!r->q_pending) continue;
    s->stats.queries_late++;
    if (wait || s->frame - r->q_frame > 2) r->occluded = false;
  }
}

static void queries_exec_wgpu(cw_pass_ctx *ctx, vis_state *s) {
  const cw__wgpu_pass_targets *tg = cw__fx_pass_targets(ctx);
  int n = 0;
  for (int i = 0; i < s->recs.count; i++) n += s->recs.data[i].want_query;
  s->stats.queries = 0;
  if (!n) return;
  if (!tg || !tg->enc || !tg->depth) {
    static bool warned;
    if (!warned) cw_error("visibility: the pipeline gave the occlusion queries no WebGPU depth; nothing is occlusion culled");
    warned = true;
    return;
  }
  if (!s->wq) s->wq = cw_calloc(1, sizeof(vis_wq), CW_MEM_RENDER);
  vis_wq *q = s->wq;
  WGPUDevice dev = cw__wgpu_device();
  if (q->cap < n) {
    int cap = 64;
    while (cap < n) cap <<= 1;
    if (q->set) wgpuQuerySetRelease(q->set);
    if (q->resolve) wgpuBufferRelease(q->resolve);
    if (q->ubo) wgpuBufferRelease(q->ubo);
    cw__wg_readback_free(q->rb);
    WGPUQuerySetDescriptor qd = { .label = CW_WGPU_SV("visibility occlusion"), .type = WGPUQueryType_Occlusion,
                                  .count = (uint32_t)cap };
    q->set = wgpuDeviceCreateQuerySet(dev, &qd);
    q->resolve = cw__wg_buffer((size_t)cap * 8, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc, "occlusion results");
    q->ubo = cw__wg_buffer((size_t)cap * 256, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "occlusion boxes");
    q->rb = cw__wg_readback_new_fifo((size_t)cap * 8, 4, "occlusion readback");
    q->cap = cap;
    wq_reset(q);
    for (int i = 0; i < s->recs.count; i++) s->recs.data[i].q_pending = false; /* their results went with the old ring */
  }
  uint32_t key = (uint32_t)tg->depth_format | (uint32_t)(tg->samples > 1 ? tg->samples : 1) << 24;
  if (!q->pipe || q->pipe_key != key) {
    if (q->pipe) wgpuRenderPipelineRelease(q->pipe);
    const char *const parts[] = { WGSL_QBOX, NULL };
    WGPUShaderModule sm = cw__wg_module("visibility.query_box", parts);
    if (!q->bgl) {
      cw__wg_slot sl = { CW_WB_UNIFORM_DYN, WGPUShaderStage_Vertex, 0, 96 };
      q->bgl = cw__wg_layout(&sl, 1);
    }
    WGPUVertexAttribute at = { .format = WGPUVertexFormat_Float32x3, .offset = 0, .shaderLocation = 0 };
    WGPUVertexBufferLayout vb = { .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 12, .attributeCount = 1, .attributes = &at };
    WGPUDepthStencilState ds = { .format = tg->depth_format, .depthWriteEnabled = WGPUOptionalBool_False,
                                 .depthCompare = WGPUCompareFunction_LessEqual,
                                 .stencilFront = { .compare = WGPUCompareFunction_Always },
                                 .stencilBack = { .compare = WGPUCompareFunction_Always } };
    WGPURenderPipelineDescriptor d = {
      .label = CW_WGPU_SV("visibility.query_box"), .layout = cw__wg_pipeline_layout(&q->bgl, 1),
      .vertex = { .module = sm, .entryPoint = CW_WGPU_SV("vs"), .bufferCount = 1, .buffers = &vb },
      .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
      .depthStencil = &ds, .multisample = { .count = (uint32_t)(tg->samples > 1 ? tg->samples : 1), .mask = 0xffffffffu },
    };
    q->pipe = wgpuDeviceCreateRenderPipeline(dev, &d);
    wgpuPipelineLayoutRelease(d.layout);
    wgpuShaderModuleRelease(sm);
    q->pipe_key = key;
  }
  if (!q->vbo) {
    static const float c[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
                                   { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } };
    static const int idx[36] = { 0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                                 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2 };
    float v36[36 * 3];
    for (int i = 0; i < 36; i++) memcpy(v36 + 3 * i, c[idx[i]], 12);
    q->vbo = cw__wg_buffer(sizeof v36, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "occlusion box");
    cw__wg_write(q->vbo, 0, v36, sizeof v36);
  }
  /* the jittered projection the scene drew with (TAA): ctx->camera's */
  t3_mat4 view, vp;
  t3_mat4_invert(&view, &ctx->camera->base.matrix_world);
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &view);
  uint8_t *ub = cw_frame_calloc((size_t)n * 256, 1);
  int *recs = cw_alloc(sizeof(int) * (size_t)n, CW_MEM_RENDER);
  int k = 0;
  for (int i = 0; i < s->recs.count; i++) {
    vrec *r = &s->recs.data[i];
    if (!r->want_query) continue;
    float m = query_margin(s, r);
    float *o = (float *)(ub + 256 * k);
    memcpy(o, vp.e, 64);
    for (int c = 0; c < 3; c++) {
      o[16 + c] = r->bmin[c] - m;
      o[20 + c] = r->bmax[c] + m;
    }
    o[19] = tg->flip_y ? -1.0f : 1.0f;
    recs[k++] = i;
  }
  cw__wg_write(q->ubo, 0, ub, (size_t)n * 256);
  WGPURenderPassDepthStencilAttachment da = { .view = tg->depth, .depthLoadOp = WGPULoadOp_Load, .depthStoreOp = WGPUStoreOp_Store };
  if (tg->depth_format == WGPUTextureFormat_Depth24PlusStencil8 || tg->depth_format == WGPUTextureFormat_Depth32FloatStencil8) {
    da.stencilLoadOp = WGPULoadOp_Load;
    da.stencilStoreOp = WGPUStoreOp_Store;
  }
  WGPURenderPassDescriptor rp = { .label = CW_WGPU_SV("occlusion_queries"), .depthStencilAttachment = &da,
                                  .occlusionQuerySet = q->set };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(tg->enc, &rp);
  wgpuRenderPassEncoderSetViewport(p, 0, 0, (float)tg->w, (float)tg->h, 0, 1);
  wgpuRenderPassEncoderSetPipeline(p, q->pipe);
  wgpuRenderPassEncoderSetVertexBuffer(p, 0, q->vbo, 0, WGPU_WHOLE_SIZE);
  cw__wg_entry e = { .buffer = q->ubo, .size = 96 };
  WGPUBindGroup bg = cw__wg_group(q->bgl, &e, 1);
  for (int i = 0; i < n; i++) {
    uint32_t off = (uint32_t)(256 * i);
    wgpuRenderPassEncoderSetBindGroup(p, 0, bg, 1, &off);
    wgpuRenderPassEncoderBeginOcclusionQuery(p, (uint32_t)i);
    wgpuRenderPassEncoderDraw(p, 36, 1, 0, 0);
    wgpuRenderPassEncoderEndOcclusionQuery(p);
  }
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
  wgpuBindGroupRelease(bg);
  wgpuCommandEncoderResolveQuerySet(tg->enc, q->set, 0, (uint32_t)n, q->resolve, 0);
  if (!cw__wg_readback_request(q->rb, tg->enc, q->resolve, 0, s->frame)) {
    /* every readback slot is still in flight: no query this frame */
    cw_free(recs);
    return;
  }
  int slot = q->pend_next;
  q->pend_next = (q->pend_next + 1) % WQ_PEND;
  cw_free(q->pend[slot].recs);
  q->pend[slot].recs = recs;
  q->pend[slot].n = n;
  q->pend[slot].frame = s->frame;
  for (int i = 0; i < n; i++) {
    vrec *r = &s->recs.data[recs[i]];
    r->q_pending = true;
    r->q_frame = s->frame;
  }
  s->stats.queries = n;
}
#endif

/* tests: the visibility passes on a caller-built context (phase: 0 the
 * culling pass, 1 the occlusion queries, 2 the restore) */
static bool restore_active(const cw_pass_ctx *ctx, void *u);
static void restore_exec(cw_pass_ctx *ctx, void *u);
bool cw__vis_test_restore_active(cw_pass_ctx *ctx);
bool cw__vis_test_restore_active(cw_pass_ctx *ctx) { return restore_active(ctx, NULL); }
void cw__vis_test_exec(cw_pass_ctx *ctx, int phase);
void cw__vis_test_exec(cw_pass_ctx *ctx, int phase) {
  vis_state *s = cw_tree_slot(ctx->tree, "visibility");
  if (phase == 0) vis_exec(ctx, NULL);
  else if (phase == 1 && queries_active(ctx, NULL)) queries_exec(ctx, NULL);
  else if (phase == 2 && s && restore_active(ctx, NULL)) restore_exec(ctx, NULL);
}

/* ── impostors, restore ─────────────────────────────────────────────── */

static bool imp_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  vis_state *s = cw_tree_slot(ctx->tree, "visibility");
  return s && s->ran && s->mgr && s->mgr->impostors && (s->stats.impostors || s->stats.impostor_fading);
}

static void imp_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  vis_state *s = vstate(ctx->tree);
  cw__vis_impostors_draw(s->imp, ctx);
  if (fade_after_impostors()) fade_draw(s, ctx);
}

static bool restore_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  vis_state *s = cw_tree_slot(ctx->tree, "visibility");
  /* The pipeline snapshots active predicates before culling executes.
   * Schedule restoration even on the first frame, before hidden is filled. */
  return s != NULL;
}

static void restore_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  vis_state *s = vstate(ctx->tree);
  for (int i = 0; i < s->hidden.count; i++) s->hidden.data[i]->visible = true;
  fade_restore(s);
  cw_array_clear(&s->hidden);
  /* publish the stats on the manager */
  vis_manager *m = s->mgr;
  if (!m) return;
  const cw_visibility_stats *st = &s->stats;
  m->meshes = st->meshes;
  m->static_meshes = st->static_meshes;
  m->visible = st->visible;
  m->frustum_culled = st->frustum_culled;
  m->occlusion_culled = st->occlusion_culled;
  m->portal_culled = st->portal_culled;
  m->distance_culled = st->distance_culled;
  m->queries = st->queries;
  m->query_latency = st->query_latency;
  for (int k = 0; k < 4; k++) m->lod_counts[k] = (float)st->lod_counts[k];
  m->impostors_drawn = st->impostors;
  m->impostor_fading = st->impostor_fading;
  m->bvh_nodes = st->bvh_nodes;
  m->rooms_visible = st->rooms_visible;
  m->cull_ms = st->cull_ms;
}

/* A split-screen view omits PRESENT; restore before the next view's GI,
 * shadow and visibility passes inspect the shared scene. */
void cw__visibility_end_view(cw_tree *t);
void cw__visibility_end_view(cw_tree *t) {
  cw_pass_ctx ctx={0};ctx.tree=t;
  if(restore_active(&ctx,NULL))restore_exec(&ctx,NULL);
}

void cw_visibility_get_stats(cw_tree *t, cw_visibility_stats *out) {
  vis_state *s = t ? cw_tree_slot(t, "visibility") : NULL;
  if (!s) memset(out, 0, sizeof *out);
  else *out = s->stats;
}

bool cw_visibility_drawn(cw_node mesh) {
  cw_tree *t = cw_node_tree(mesh);
  vis_state *s = t ? cw_tree_slot(t, "visibility") : NULL;
  if (!s || !s->ran) return true;
  for (int i = 0; i < s->recs.count; i++)
    if (s->recs.data[i].node.id == mesh.id) return s->recs.data[i].drawn != 0;
  return true;
}

static void add_passes(cw_tree *t) {
  vis_state *s = vstate(t);
  if (s->passes_added) return;
  s->passes_added = true;
  cw_pipeline *p = cw_pipeline_of(t);
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "visibility", .phase = CW_PHASE_SHADOW, .order = 2000,
                                           .execute = vis_exec });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "impostors", .phase = CW_PHASE_OPAQUE, .order = 100,
                                           .writes = CW_RES_COLOR | CW_RES_DEPTH, .active = imp_active,
                                           .execute = imp_exec });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "occlusion_queries", .phase = CW_PHASE_OPAQUE, .order = 900,
                                           .reads = 0, .active = queries_active, .execute = queries_exec });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "visibility_restore", .phase = CW_PHASE_PRESENT, .order = 1000,
                                           .active = restore_active, .execute = restore_exec });
}

/* ── node types ─────────────────────────────────────────────────────── */

static void mgr_init(cw_node n, void *data) {
  (void)n;
  vis_manager *m = data;
  m->frustum_cull = true;
  m->occlusion_cull = false;
  m->portals = true;
  m->lod = true;
  m->impostors = true;
  m->occlusion_margin = 0.5f;
  m->occlusion_interval = 4;
  m->occlusion_requery_occluded = 1;
  m->occlusion_guard = true;
  m->lod_error_px = 1;
  m->shadow_distance = 50;
}

static void mgr_enter(cw_node n, void *data) {
  (void)data;
  add_passes(cw_node_tree(n));
}

#define MF(field, ty, doc) CW_PROP_FIELD(vis_manager, field, ty, CW_PROP_DEFAULT, doc)
#define MS(field, ty, doc) CW_PROP_FIELD(vis_manager, field, ty, CW_PROP_EDITOR | CW_PROP_READONLY, doc)
static const cw_prop mgr_props[] = {
  MF(frustum_cull, CW_T_BOOL, "Frustum culling: a SIMD BVH4 over static meshes, a flat pass over the rest."),
  MF(occlusion_cull, CW_T_BOOL, "GPU occlusion queries on bounding boxes (one frame of latency, never pops)."),
  MF(occlusion_wait, CW_T_BOOL, "Read every query result at the next frame, waiting if needed (same counts on every host)."),
  CW_PROP_FIELD_RANGE(vis_manager, occlusion_margin, CW_T_FLOAT, 0, 1e6, "Query boxes grow by this (plus 3 frames of camera motion)."),
  MF(occlusion_interval, CW_T_INT, "Visible meshes are re-queried every this many frames (staggered)."),
  MF(occlusion_requery_occluded, CW_T_INT, "Occluded meshes are re-queried every this many frames (1: every frame; more saves queries but shows disocclusion late)."),
  MF(occlusion_guard, CW_T_BOOL, "Grow query boxes by the camera's motion and parallax over the latency, and trust no result while the camera brushes an occluder (off: plain one-frame-late queries, which pop)."),
  MF(portals, CW_T_BOOL, "Room3D / Portal3D culling."),
  MF(lod, CW_T_BOOL, "Pick LOD levels by screen-space error."),
  CW_PROP_FIELD_RANGE(vis_manager, lod_error_px, CW_T_FLOAT, 0, 1000, "LOD error budget in pixels."),
  MF(impostors, CW_T_BOOL, "Draw far meshes with `impostor` set as octahedral impostors."),
  CW_PROP_FIELD_RANGE(vis_manager, shadow_distance, CW_T_FLOAT, 0, 1e6, "Shadow casters' boxes are swept this far along the light."),
  MS(meshes, CW_T_INT, "MeshInstance3D nodes considered last frame."),
  MS(static_meshes, CW_T_INT, "Of those, static (in the BVH)."),
  MS(visible, CW_T_INT, "Drawn (as a mesh or an impostor)."),
  MS(frustum_culled, CW_T_INT, "Outside the view."),
  MS(occlusion_culled, CW_T_INT, "Hidden behind what the scene drew."),
  MS(portal_culled, CW_T_INT, "In rooms the camera cannot see into."),
  MS(distance_culled, CW_T_INT, "Past their visibility_range."),
  MS(queries, CW_T_INT, "Occlusion queries issued."),
  MS(query_latency, CW_T_INT, "Frames the last query result took."),
  MS(lod_counts, CW_T_VEC4, "Meshes drawn at LOD 0, 1, 2, 3+."),
  MS(impostors_drawn, CW_T_INT, "Meshes drawn as impostors only."),
  MS(impostor_fading, CW_T_INT, "Impostors fading in over their mesh."),
  MS(bvh_nodes, CW_T_INT, "BVH nodes visited."),
  MS(rooms_visible, CW_T_INT, "Rooms the camera sees into."),
  MS(cull_ms, CW_T_FLOAT, "CPU milliseconds of the visibility pass."),
  CW_PROP_END,
};

static cw_node_type mgr_type = {
  .name = "VisibilityManager", .parent = "Node", .data_size = sizeof(vis_manager), .props = mgr_props,
  .doc = "Culling (BVH frustum, occlusion queries, portals), LOD and impostors for the tree's meshes (visibility.h).",
  .init = mgr_init, .enter_tree = mgr_enter,
};

static void room_init(cw_node n, void *data) {
  (void)n;
  ((room3d *)data)->extents = (t3_vec3){ 2, 2, 2 };
}
static const cw_prop room_props[] = {
  CW_PROP_FIELD(room3d, extents, CW_T_VEC3, CW_PROP_DEFAULT, "Half sizes of the room's box."),
  CW_PROP_END,
};
static cw_node_type room_type = {
  .name = "Room3D", .parent = "Node3D", .data_size = sizeof(room3d), .props = room_props,
  .doc = "Interior space for portal culling: static meshes centred inside belong to it.", .init = room_init,
};

static void portal_init(cw_node n, void *data) {
  (void)n;
  portal3d *p = data;
  p->size = cw_v2(1, 2);
  p->enabled = true;
}
static const cw_prop portal_props[] = {
  CW_PROP_FIELD(portal3d, size, CW_T_VEC2, CW_PROP_DEFAULT, "Opening width and height (the node's XY plane)."),
  CW_PROP_FIELD(portal3d, enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Open (false: a shut door blocks the view)."),
  CW_PROP_END,
};
static cw_node_type portal_type = {
  .name = "Portal3D", .parent = "Node3D", .data_size = sizeof(portal3d), .props = portal_props,
  .doc = "An opening between two rooms (or a room and the outside).", .init = portal_init,
};

void cw__register_visibility_types(void) {
  cw_node_type_register(&mgr_type);
  manager_t = &mgr_type;
  cw_node_type_register(&room_type);
  room_t = &room_type;
  cw_node_type_register(&portal_type);
  portal_t = &portal_type;
}

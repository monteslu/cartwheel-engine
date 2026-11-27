/* The Terrain3D and TerrainScatter3D node types (cartwheel/terrain.h).
 *
 * Terrain3D builds its cw_terrain when it becomes ready (so its physics is
 * there before the first tick) and again whenever a property changes (at
 * the next fixed update or frame). The terrain is centred on the node
 * (translation only): the node owns the terrain's origin, so a game moves
 * the node, never the terrain (cw_terrain_set_origin on it is undone at the
 * next tick, with a warning). The terrain is drawn while the node and every
 * Node3D above it are visible (its meshes sit in one group the pass shows or
 * hides); a hidden terrain still collides. Each frame the pipeline's "terrain" SHADOW-phase pass
 * (order 1500, before the visibility pass) updates it for the rendering
 * camera: uploads, patch selection, scatter posing.
 *
 * TerrainScatter3D children each add one scatter layer: a built-in mesh
 * (GRASS, ROCK, TREE: generated with vertex colours, two LODs each) or a
 * mesh resource (a glTF sub-resource; its LOD chain gives the LODs). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/physics_nodes.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "cartwheel/vfs.h"
#include "terrain_internal.h"

/* ── TerrainScatter3D ──────────────────────────────────────────────── */

static cw_terrain *terrain_of_data(void *terrain3d_data); /* (Terrain3D's, below) */

typedef struct scatter3d {
  int shape; /* GRASS, ROCK, TREE */
  const char *mesh;
  float color[4];
  float density;
  const char *density_map;
  int layers;
  float min_slope;           /* degrees: the steepest ground it grows on (90 = any) */
  float scale_min, scale_max;
  bool align;
  float sink;
  float visible_range, fade;
  float lod_distance;
  bool cast_shadow;
  int knob;                  /* GRASS (cw_quality_scale("grass")) or SCATTER */
  int seed;
  float wind_strength, wind_frequency, wind_direction[2], wind_gust, wind_gust_scale;
  bool smooth_alpha, two_sided_lighting;
  int fade_mode;             /* SHRINK, DITHER */
  float lod_blend;
  float tint_jitter;         /* -1: the shape's own (GRASS 0.25, the rest 0) */
  bool root_from_terrain;
  float root_height;
  float bend;
  bool shadow_at_root;
  float shadow_soften;
  /* runtime */
  int layer;                 /* in the parent's terrain, -1 none */
  bool dirty;
  bool map_wait;             /* density_map names a file not there yet: the layer holds its index with no instances and
                              * is applied again once the file system changes (cw_vfs_generation past map_gen) */
  uint32_t map_gen;
  cw_res res[CW_SCATTER_PARTS];
  const char *res_mesh;      /* the mesh value res[] was loaded for (interned): another one releases them first */
} scatter3d;

/* the test seam for tests/unit/test_terrain.c's control: true brings back the old lazy sync (cw_terrain_of applies a
 * changed scatter child only when the Terrain3D itself is dirty, and a missing density map is read once) */
bool cw__terrain_lazy_scatter_test;
/* the test seam for the mesh swap's control: true brings back the old cache (the parts loaded for the first mesh are
 * kept, so a mesh set on a live layer changes nothing) */
bool cw__terrain_keep_meshes_test;

static const char *const shape_names[] = { "GRASS", "ROCK", "TREE", NULL };
static const char *const knob_names[] = { "GRASS", "SCATTER", NULL };
static const char *const fade_names[] = { "SHRINK", "DITHER", NULL };
static const cw_node_type *scatter_t, *terrain_t;

/* a mesh of rings: (radius, y, colour) per ring, `seg` around; closed at both ends by the first and last ring */
static t3_geometry *rings(const float (*ring)[5], int nr, int seg, float sx, float sz) {
  int nv = nr * (seg + 1);
  float *pos = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER), *nrm = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  float *col = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  for (int r = 0; r < nr; r++)
    for (int s = 0; s <= seg; s++) {
      float a = (float)s / (float)seg * 6.2831853f, c = cosf(a), sn = sinf(a);
      int v = r * (seg + 1) + s;
      pos[3 * v] = ring[r][0] * c * sx, pos[3 * v + 1] = ring[r][1], pos[3 * v + 2] = ring[r][0] * sn * sz;
      /* the slope between neighbouring rings */
      int ra = r > 0 ? r - 1 : r, rb = r + 1 < nr ? r + 1 : r;
      float dr = ring[rb][0] - ring[ra][0], dy = ring[rb][1] - ring[ra][1];
      float nx = dy, ny = -dr, l = sqrtf(nx * nx + ny * ny);
      if (l < 1e-6f) nx = 0, ny = 1, l = 1;
      nrm[3 * v] = nx / l * c, nrm[3 * v + 1] = ny / l, nrm[3 * v + 2] = nx / l * sn;
      col[3 * v] = ring[r][2], col[3 * v + 1] = ring[r][3], col[3 * v + 2] = ring[r][4];
    }
  int ni = (nr - 1) * seg * 6;
  uint16_t *idx = cw_alloc(sizeof(uint16_t) * (size_t)ni, CW_MEM_RENDER);
  int k = 0;
  for (int r = 0; r + 1 < nr; r++)
    for (int s = 0; s < seg; s++) {
      uint16_t a = (uint16_t)(r * (seg + 1) + s), b = (uint16_t)(a + 1), c = (uint16_t)(a + seg + 1), d = (uint16_t)(c + 1);
      idx[k++] = a, idx[k++] = c, idx[k++] = b;
      idx[k++] = b, idx[k++] = c, idx[k++] = d;
    }
  t3_geometry *g = t3_geometry_new();
  { t3_attribute *a_ = t3_attribute_new(T3_FLOAT32, pos, nv, 3); t3_geometry_set_attribute(g, T3_ATTR_POSITION, a_); t3_release(a_); }
  { t3_attribute *a_ = t3_attribute_new(T3_FLOAT32, nrm, nv, 3); t3_geometry_set_attribute(g, T3_ATTR_NORMAL, a_); t3_release(a_); }
  { t3_attribute *a_ = t3_attribute_new(T3_FLOAT32, col, nv, 3); t3_geometry_set_attribute(g, T3_ATTR_COLOR, a_); t3_release(a_); }
  { t3_attribute *a_ = t3_attribute_new(T3_UINT16, idx, ni, 1); t3_geometry_set_index(g, a_); t3_release(a_); }
  cw_free(pos), cw_free(nrm), cw_free(col), cw_free(idx);
  return g;
}

/* a grass tuft: `blades` curved blades fanned round the centre, each `seg` segments up its length, `width` metres
 * wide at the root and tapering to a point, leaning out and curving over by a share of its height. Every triangle
 * is in the index twice, once each way round, with one normal bent up toward the sky: a blade is lit the same from
 * both sides (a front-side material, no flipped back face). Vertex colours run from a dark root to a lighter tip,
 * a few blades drier than the rest. Deterministic (a hash of the blade's number). */
static float tuft_u(int b, int k) {
  uint32_t h = (uint32_t)b * 0x9E3779B1u ^ (uint32_t)k * 0x85EBCA77u;
  h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
  return (float)(h >> 8) * (1.0f / 16777216.0f);
}
static t3_geometry *tuft(int blades, int seg, float width) {
  int per = 2 * seg + 1, nv = blades * per, tris = 2 * seg - 1;
  float *pos = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER), *nrm = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  float *col = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  int ni = blades * tris * 6;
  uint16_t *idx = cw_alloc(sizeof(uint16_t) * (size_t)ni, CW_MEM_RENDER);
  int k = 0;
  for (int b = 0; b < blades; b++) {
    /* where it grows (spread over a disc of 9 cm), which way it leans (mostly outward), how tall and how far over */
    float r0 = 0.09f * sqrtf(tuft_u(b, 1)), a0 = 6.2831853f * ((float)b + tuft_u(b, 2) * 0.6f) / (float)blades;
    float ox = r0 * cosf(a0), oz = r0 * sinf(a0);
    float la = a0 + (tuft_u(b, 3) - 0.5f) * 1.6f, ldx = cosf(la), ldz = sinf(la);
    float sx = -ldz, sz = ldx; /* across the blade */
    float hgt = 0.30f + 0.30f * tuft_u(b, 4), over = hgt * (0.18f + 0.42f * tuft_u(b, 5));
    float w = width * (0.75f + 0.5f * tuft_u(b, 6));
    bool dry = tuft_u(b, 7) < 0.18f;
    float root[3] = { 0.040f, 0.075f, 0.018f };
    float tip[3] = { dry ? 0.28f : 0.14f, dry ? 0.27f : 0.27f, dry ? 0.08f : 0.05f };
    float v = 0.85f + 0.3f * tuft_u(b, 8); /* each blade a little lighter or darker */
    int base = b * per;
    for (int i = 0; i <= seg; i++) {
      float t = (float)i / (float)seg;
      /* the spine: up, curving out (the lean grows with the square of the height), and its tangent */
      float px = ox + ldx * over * t * t, pz = oz + ldz * over * t * t, py = hgt * (t - 0.22f * (over / hgt) * (over / hgt) * t * t);
      float tx = ldx * over * 2 * t, tz = ldz * over * 2 * t, ty = hgt * (1 - 0.44f * (over / hgt) * (over / hgt) * t);
      /* the face normal (tangent x across, the side facing up), bent most of the way up */
      float fnx = ty * sz, fny = tz * sx - tx * sz, fnz = -ty * sx;
      if (fny < 0) fnx = -fnx, fny = -fny, fnz = -fnz;
      float fl = sqrtf(fnx * fnx + fny * fny + fnz * fnz);
      if (fl < 1e-6f) fnx = 0, fny = 1, fnz = 0, fl = 1;
      float nx = fnx / fl * 0.45f, ny = fny / fl * 0.45f + 0.8f, nz = fnz / fl * 0.45f;
      float nl = sqrtf(nx * nx + ny * ny + nz * nz);
      nx /= nl, ny /= nl, nz /= nl;
      float g = powf(t, 0.8f), c[3];
      for (int q = 0; q < 3; q++) c[q] = (root[q] + (tip[q] - root[q]) * g) * v;
      float hw = i == seg ? 0 : w * 0.5f * powf(1 - t, 0.7f);
      for (int sidei = 0; sidei < (i == seg ? 1 : 2); sidei++) {
        int vi = i == seg ? base + 2 * seg : base + 2 * i + sidei;
        float sg = sidei ? 1.0f : -1.0f;
        pos[3 * vi] = px + sx * hw * sg, pos[3 * vi + 1] = py, pos[3 * vi + 2] = pz + sz * hw * sg;
        nrm[3 * vi] = nx, nrm[3 * vi + 1] = ny, nrm[3 * vi + 2] = nz;
        memcpy(&col[3 * vi], c, sizeof c);
      }
    }
    /* each triangle both ways round */
    for (int i = 0; i < seg; i++) {
      uint16_t l0 = (uint16_t)(base + 2 * i), r0i = (uint16_t)(l0 + 1);
      if (i + 1 < seg) {
        uint16_t l1 = (uint16_t)(l0 + 2), r1 = (uint16_t)(l0 + 3);
        uint16_t q[6] = { l0, r0i, r1, l0, r1, l1 };
        for (int e = 0; e < 6; e++) idx[k++] = q[e];
        for (int e = 0; e < 6; e += 3) idx[k++] = q[e], idx[k++] = q[e + 2], idx[k++] = q[e + 1];
      } else {
        uint16_t tp = (uint16_t)(base + 2 * seg);
        idx[k++] = l0, idx[k++] = r0i, idx[k++] = tp;
        idx[k++] = l0, idx[k++] = tp, idx[k++] = r0i;
      }
    }
  }
  t3_geometry *g = t3_geometry_new();
  { t3_attribute *a_ = t3_attribute_new(T3_FLOAT32, pos, nv, 3); t3_geometry_set_attribute(g, T3_ATTR_POSITION, a_); t3_release(a_); }
  { t3_attribute *a_ = t3_attribute_new(T3_FLOAT32, nrm, nv, 3); t3_geometry_set_attribute(g, T3_ATTR_NORMAL, a_); t3_release(a_); }
  { t3_attribute *a_ = t3_attribute_new(T3_FLOAT32, col, nv, 3); t3_geometry_set_attribute(g, T3_ATTR_COLOR, a_); t3_release(a_); }
  { t3_attribute *a_ = t3_attribute_new(T3_UINT16, idx, k, 1); t3_geometry_set_index(g, a_); t3_release(a_); }
  cw_free(pos), cw_free(nrm), cw_free(col), cw_free(idx);
  return g;
}

static void builtin_mesh(int shape, int lod, const float color[4], t3_geometry **g, t3_material **m) {
  t3_material *mat = t3_mesh_standard_material_new(0xffffff);
  mat->vertex_colors = true;
  mat->color = (t3_color){ color[0], color[1], color[2] };
  mat->roughness = 0.9f;
  mat->metalness = 0;
  if (shape == 0) {
    /* 12 curved blades of three segments near, 6 straight wider ones far (the same cover at a distance) */
    *g = lod ? tuft(6, 1, 0.05f) : tuft(12, 3, 0.03f);
    mat->roughness = 1.0f;
  } else if (shape == 1) {
    static const float rock[][5] = { { 0.0f, -0.1f, 0.30f, 0.29f, 0.27f }, { 0.55f, 0.05f, 0.38f, 0.36f, 0.33f },
                                     { 0.50f, 0.35f, 0.45f, 0.43f, 0.40f }, { 0.25f, 0.55f, 0.50f, 0.48f, 0.45f },
                                     { 0.0f, 0.60f, 0.52f, 0.50f, 0.47f } };
    *g = rings(rock, 5, lod ? 5 : 9, 1.0f, 0.75f);
  } else {
    static const float tree[][5] = { { 0.00f, 0.0f, 0.25f, 0.16f, 0.08f }, { 0.16f, 0.0f, 0.25f, 0.16f, 0.08f },
                                     { 0.14f, 1.1f, 0.25f, 0.16f, 0.08f }, { 1.30f, 1.0f, 0.05f, 0.18f, 0.06f },
                                     { 0.95f, 2.4f, 0.06f, 0.22f, 0.07f }, { 0.55f, 3.6f, 0.08f, 0.26f, 0.08f },
                                     { 0.00f, 4.8f, 0.10f, 0.30f, 0.10f } };
    *g = rings(tree, 7, lod ? 5 : 10, 1, 1);
  }
  *m = mat;
}

static void scatter_init(cw_node n, void *data) {
  (void)n;
  scatter3d *s = data;
  s->color[0] = s->color[1] = s->color[2] = s->color[3] = 1;
  s->mesh = cw_intern("");
  s->density_map = cw_intern("");
  s->density = 0.5f;
  s->min_slope = 35;
  s->scale_min = 0.8f, s->scale_max = 1.2f;
  s->align = true;
  s->visible_range = 60;
  s->fade = 10;
  s->lod_distance = 25;
  cw_wind w = cw_wind_default();
  s->wind_frequency = w.frequency, s->wind_direction[0] = w.direction[0], s->wind_direction[1] = w.direction[1];
  s->wind_gust = w.gust, s->wind_gust_scale = w.gust_scale;
  s->lod_blend = 6;
  s->tint_jitter = -1;
  s->layer = -1;
  s->dirty = true;
}

static void scatter_changed(cw_node n, void *data, const cw_prop *p) {
  (void)n;
  (void)p;
  ((scatter3d *)data)->dirty = true;
}

static void scatter_remove(cw_terrain *t, scatter3d *s) {
  if (t && s->layer >= 0) cw_terrain_scatter_remove(t, s->layer);
  s->layer = -1;
}

static void scatter_release_meshes(scatter3d *s) {
  for (int p = 0; p < CW_SCATTER_PARTS; p++) {
    if (s->res[p].id) cw_res_release(s->res[p]);
    s->res[p] = (cw_res){ 0 };
  }
  s->res_mesh = NULL;
}

static void scatter_destroy(cw_node n, void *data) {
  (void)n;
  scatter_release_meshes(data);
}

/* (re)add the layer to the parent's terrain */
static void scatter_apply(cw_node n, scatter3d *s, cw_terrain *t) {
  scatter_remove(t, s);
  s->dirty = false;
  if (!t) return;
  cw_scatter_def d = cw_scatter_default_def();
  t3_geometry *gs[CW_SCATTER_LODS][CW_SCATTER_PARTS] = { { 0 } };
  t3_material *ms[CW_SCATTER_LODS][CW_SCATTER_PARTS] = { { 0 } };
  bool own = true;
  /* a mesh set on a live layer (swapped in at run time): the parts loaded for the old one go, so the new one loads */
  if (s->res_mesh != s->mesh && !cw__terrain_keep_meshes_test) scatter_release_meshes(s);
  if (s->mesh && *s->mesh) {
    s->res_mesh = s->mesh;
    /* "file.glb#mesh/m/p": that primitive; "file.glb": mesh 0's primitives, each a part */
    bool one = strchr(s->mesh, '#') != NULL;
    int parts = 1;
    if (!one) {
      /* a one-mesh glTF: its primitives (cw_gltf.mesh_count counts them) */
      cw_res file = cw_res_load(s->mesh, CW_RES_GLTF);
      const cw_gltf *gl = cw_res_gltf(file);
      parts = gl ? CW_CLAMP(gl->mesh_count, 1, CW_SCATTER_PARTS) : 1;
      cw_res_release(file);
    }
    for (int p = 0; p < parts; p++) {
      if (!s->res[p].id) {
        char path[512];
        if (one) snprintf(path, sizeof path, "%s", s->mesh);
        else snprintf(path, sizeof path, "%s#mesh/0/%d", s->mesh, p);
        s->res[p] = cw_res_load(path, CW_RES_MESH);
      }
      const cw_mesh *m = cw_res_mesh(s->res[p]);
      if (!m) {
        if (!p) {
          cw_error("TerrainScatter3D %s: mesh %s did not load", cw_node_name(n), s->mesh);
          return;
        }
        break;
      }
      for (int l = 0; l < CW_SCATTER_LODS && l < m->lod_count; l++) gs[l][p] = m->lod[l], ms[l][p] = m->material;
    }
    own = false;
  } else {
    for (int l = 0; l < 2; l++) builtin_mesh(s->shape, l, s->color, &gs[l][0], &ms[l][0]);
  }
  for (int l = 0; l < CW_SCATTER_LODS; l++)
    for (int p = 0; p < CW_SCATTER_PARTS; p++) d.geometry[l][p] = gs[l][p], d.material[l][p] = ms[l][p];
  for (int l = 0; l < CW_SCATTER_LODS; l++) d.lod_distance[l] = s->lod_distance * (float)(l + 1);
  d.density = s->density;
  d.layers = (uint32_t)s->layers;
  d.min_slope_cos = cosf(cw_clampf(s->min_slope, 0, 90) * 3.14159265f / 180.0f);
  d.scale_min = s->scale_min;
  d.scale_max = s->scale_max;
  d.align_to_normal = s->align;
  d.sink = s->sink;
  d.visible_range = s->visible_range;
  d.fade = s->fade;
  d.cast_shadow = s->cast_shadow;
  d.density_knob = s->knob ? "scatter" : "grass";
  d.wind = (cw_wind){ .strength = s->wind_strength, .frequency = s->wind_frequency, .direction = { s->wind_direction[0], s->wind_direction[1] },
                      .gust = s->wind_gust, .gust_scale = s->wind_gust_scale };
  d.smooth_alpha = s->smooth_alpha;
  d.two_sided_lighting = s->two_sided_lighting;
  d.fade_mode = s->fade_mode ? CW_SCATTER_FADE_DITHER : CW_SCATTER_FADE_SHRINK;
  d.lod_blend = s->lod_blend;
  d.tint_jitter = s->tint_jitter >= 0 ? s->tint_jitter : own && s->shape == 0 ? 0.25f : 0;
  d.root_from_terrain = s->root_from_terrain;
  d.root_height = s->root_height;
  d.bend = s->bend;
  d.shadow_at_root = s->shadow_at_root;
  d.shadow_soften = s->shadow_soften;
  d.seed = (uint32_t)s->seed ^ (uint32_t)cw_hash_str(cw_node_name(n));
  uint8_t *grey = NULL;
  static const uint8_t none = 0;
  s->map_wait = false;
  if (s->density_map && *s->density_map && !cw__terrain_lazy_scatter_test && !cw_vfs_exists(s->density_map)) {
    /* not written yet (a map the game makes in its init, after the scene is ready): the layer takes its index with
     * no instances (a 1 x 1 map of 0), and is applied again when the file system changes */
    s->map_wait = true;
    s->map_gen = cw_vfs_generation();
    d.density_map = &none, d.dm_w = d.dm_h = 1;
  } else if (s->density_map && *s->density_map) {
    extern unsigned char *stbi_load_from_memory(const unsigned char *, int, int *, int *, int *, int);
    extern void stbi_image_free(void *);
    size_t len = 0;
    uint8_t *bytes = cw_vfs_read(s->density_map, &len);
    int w = 0, h = 0, c;
    grey = bytes ? stbi_load_from_memory(bytes, (int)len, &w, &h, &c, 1) : NULL;
    free(bytes);
    if (!grey) cw_error("TerrainScatter3D %s: cannot read density map %s", cw_node_name(n), s->density_map);
    d.density_map = grey, d.dm_w = w, d.dm_h = h;
  }
  s->layer = cw_terrain_scatter_add(t, &d);
  if (grey) {
    extern void stbi_image_free(void *);
    stbi_image_free(grey);
  }
  if (own)
    for (int l = 0; l < CW_SCATTER_LODS; l++)
      for (int p = 0; p < CW_SCATTER_PARTS; p++) {
        if (gs[l][p]) t3_release(gs[l][p]);
        if (ms[l][p]) t3_release(ms[l][p]);
      }
}

/* leaving the tree (or freed): the layer leaves the parent's terrain */
static void scatter_exit(cw_node n, void *data) {
  scatter3d *s = data;
  cw_node parent = cw_node_parent(n);
  cw_terrain *t = terrain_t ? terrain_of_data(cw_node_data_t(parent, terrain_t)) : NULL;
  scatter_remove(t, s);
}

static const cw_prop scatter_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(scatter3d, shape), .size = sizeof(int),
    .enum_names = shape_names, .doc = "Built-in mesh when `mesh` is empty: GRASS (tufts), ROCK, TREE (two LODs each, vertex coloured)." },
  CW_PROP_FIELD(scatter3d, mesh, CW_T_STRING, CW_PROP_DEFAULT, "A glTF mesh: \"file.glb#mesh/m/p\" one primitive, or \"file.glb\" (one mesh) its primitives as parts (a trunk and a crown); its LOD chains give the LODs. Set on a live layer it swaps the mesh at once (the same places)."),
  CW_PROP_FIELD(scatter3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Tint of the built-in meshes."),
  CW_PROP_FIELD_RANGE(scatter3d, density, CW_T_FLOAT, 0, 16, "Instances per square metre at full density."),
  CW_PROP_FIELD(scatter3d, density_map, CW_T_STRING, CW_PROP_DEFAULT, "Grey image over the whole terrain scaling the density (empty = 1). A path not written yet (a map the game makes in its init) leaves the layer empty until the file is written; set from code, cw_terrain_of applies it at once."),
  CW_PROP_FIELD(scatter3d, layers, CW_T_INT, CW_PROP_DEFAULT, "Control layers it grows on (bit i = layer i; 0 = all)."),
  CW_PROP_FIELD_RANGE(scatter3d, min_slope, CW_T_FLOAT, 0, 90, "Steepest ground it grows on, degrees."),
  CW_PROP_FIELD_RANGE(scatter3d, scale_min, CW_T_FLOAT, 0.01, 100, "Smallest uniform scale."),
  CW_PROP_FIELD_RANGE(scatter3d, scale_max, CW_T_FLOAT, 0.01, 100, "Largest uniform scale."),
  CW_PROP_FIELD(scatter3d, align, CW_T_BOOL, CW_PROP_DEFAULT, "Tilt with the ground (grass, rocks) instead of standing up (trees)."),
  CW_PROP_FIELD_RANGE(scatter3d, sink, CW_T_FLOAT, 0, 10, "Metres pushed into the ground (x scale)."),
  CW_PROP_FIELD_RANGE(scatter3d, visible_range, CW_T_FLOAT, 1, 10000, "Metres it is drawn to."),
  CW_PROP_FIELD_RANGE(scatter3d, fade, CW_T_FLOAT, 0, 10000, "Metres at the end of visible_range over which instances shrink away."),
  CW_PROP_FIELD_RANGE(scatter3d, lod_distance, CW_T_FLOAT, 0, 10000, "Metres where each coarser mesh LOD takes over (LOD i + 1 at (i + 1) x this)."),
  CW_PROP_FIELD(scatter3d, cast_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "The finest LOD casts shadows."),
  { .name = "knob", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(scatter3d, knob), .size = sizeof(int),
    .enum_names = knob_names, .doc = "Which quality density thins it: GRASS or SCATTER (cartwheel/quality.h)." },
  CW_PROP_FIELD(scatter3d, seed, CW_T_INT, CW_PROP_DEFAULT, "Placement seed (mixed with the node's name)."),
  CW_PROP_FIELD_RANGE(scatter3d, wind_strength, CW_T_FLOAT, 0, 10, "Wind: metres the top of the mesh leans at most (0: no wind). Sways with a phase from each instance's place, so neighbours never move together (foliage.h)."),
  CW_PROP_FIELD_RANGE(scatter3d, wind_frequency, CW_T_FLOAT, 0, 20, "Wind: sways a second."),
  { .name = "wind_direction", .type = CW_T_VEC2, .flags = CW_PROP_DEFAULT, .offset = offsetof(scatter3d, wind_direction), .size = sizeof(float) * 2,
    .doc = "Wind: world x, z it blows toward." },
  CW_PROP_FIELD_RANGE(scatter3d, wind_gust, CW_T_FLOAT, 0, 1, "Wind: share of the strength that comes in gusts travelling over the field."),
  CW_PROP_FIELD_RANGE(scatter3d, wind_gust_scale, CW_T_FLOAT, 0.5, 1000, "Wind: metres between gust fronts."),
  CW_PROP_FIELD(scatter3d, smooth_alpha, CW_T_BOOL, CW_PROP_DEFAULT, "Alpha-tested meshes (leaf and grass cards): alpha to coverage under MSAA, hashed alpha TAA resolves without it: no stair-stepped or flickering edges."),
  CW_PROP_FIELD(scatter3d, two_sided_lighting, CW_T_BOOL, CW_PROP_DEFAULT, "Double-sided meshes: back faces lit with the front face's normal (no dark backs on leaves with up-bent normals)."),
  { .name = "fade_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(scatter3d, fade_mode), .size = sizeof(int),
    .enum_names = fade_names, .doc = "SHRINK: instances shrink over the fade and LODs switch at lod_distance. DITHER: each instance at its own distance: it shrinks away over its own share of the fade, and over the lod_blend metres it lowers to a fifth of its height, swaps to the next LOD and grows back (one LOD of an instance at a time, whole instances: no grain under TAA in motion, no depth fighting between LODs, the pop softened and spread over the band)." },
  CW_PROP_FIELD_RANGE(scatter3d, lod_blend, CW_T_FLOAT, 0, 1000, "DITHER: metres over which each LOD crossfades with the next, centred on its switch distance."),
  CW_PROP_FIELD_RANGE(scatter3d, tint_jitter, CW_T_FLOAT, -1, 1, "Per-instance colour variation 0..1 (brightness and a dry yellow); -1: the shape's own (GRASS 0.25, the rest none)."),
  CW_PROP_FIELD(scatter3d, root_from_terrain, CW_T_BOOL, CW_PROP_DEFAULT, "Each instance's lower part takes the terrain's colour under it (cw_terrain_albedo_at), blending into its own colour above."),
  CW_PROP_FIELD_RANGE(scatter3d, root_height, CW_T_FLOAT, 0, 100, "Metres the root colour reaches (0: 0.6 of the mesh's height)."),
  CW_PROP_FIELD_RANGE(scatter3d, bend, CW_T_FLOAT, 0, 1.5, "Radians the tops turn over at a mover's full push (cw_foliage_mover_add: characters pushing through grass; 0: movers ignored, 1.1 lays grass well over)."),
  CW_PROP_FIELD(scatter3d, shadow_at_root, CW_T_BOOL, CW_PROP_DEFAULT, "One shadow sample per instance, at its root: a tuft is wholly lit or wholly shadowed (no speckled shadow edge through grass under TAA)."),
  CW_PROP_FIELD_RANGE(scatter3d, shadow_soften, CW_T_FLOAT, 0, 1, "With shadow_at_root: each fragment's shadow sample moves from the root toward its own point by this times its height share (0: one sample per instance)."),
  CW_PROP_END,
};

static cw_node_type scatter_type = {
  .name = "TerrainScatter3D", .parent = "Node", .data_size = sizeof(scatter3d), .props = scatter_props,
  .doc = "A scatter layer (grass, rocks, trees) of its parent Terrain3D (terrain.h).", .init = scatter_init,
  .destroy = scatter_destroy, .exit_tree = scatter_exit, .prop_changed = scatter_changed,
};

/* ── Terrain3D ─────────────────────────────────────────────────────── */

typedef struct terrain3d {
  int size_x, size_z, region_size, grid_quads, lod_count;
  float vertex_spacing, min_height, max_height, lod0_range, far_distance, visible_range, side_detile;
  float blend_sharpness, macro_tint, macro_tint_scale, macro_layer, macro_layer_scale, macro_layer_near, macro_layer_far;
  float parallax_distance;
  bool uv_blend;
  const char *heightmap, *control, *splat;
  int generate_seed;
  float generate_feature, generate_amplitude;
  int generate_octaves;
  const char *albedo[CW_TERRAIN_LAYERS], *normal[CW_TERRAIN_LAYERS];
  float uv_scale[CW_TERRAIN_LAYERS], sharpness[CW_TERRAIN_LAYERS], parallax[CW_TERRAIN_LAYERS];
  float color[4];
  float roughness;
  bool physics, cast_shadow;
  int quality;
  /* runtime */
  cw_terrain *t;
  bool dirty;
  t3_renderer *bound;
  cw_tree *tree;
  t3_vec3 origin;      /* the origin last given to the terrain (node_origin) */
  bool origin_warned;  /* the terrain was moved by other code: said once */
  bool shown;          /* it and every Node3D above it visible (terrain_sync) */
} terrain3d;

#define LAYER_FIELD(i, fld, nm, ty, docstr)                                                                    \
  { .name = "layer" #i "_" nm, .type = ty, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(terrain3d, fld[i]), \
    .size = (int)sizeof(((terrain3d *)0)->fld[i]), .doc = docstr }
#define LAYER_PROPS(i)                                                                                     \
  LAYER_FIELD(i, albedo, "albedo", CW_T_STRING, "Layer " #i ": albedo RGB + height A image."),             \
    LAYER_FIELD(i, normal, "normal", CW_T_STRING, "Layer " #i ": normal RGB (+Y) + roughness A image."),   \
    LAYER_FIELD(i, uv_scale, "uv_scale", CW_T_FLOAT, "Layer " #i ": texture repeats per metre."),             \
    LAYER_FIELD(i, sharpness, "blend_sharpness", CW_T_FLOAT, "Layer " #i ": its own height-blend sharpness (0 = blend_sharpness)."), \
    LAYER_FIELD(i, parallax, "parallax", CW_T_FLOAT, "Layer " #i ": parallax depth, metres (0 = flat).")

static void terrain_init(cw_node n, void *data) {
  (void)n;
  terrain3d *d = data;
  cw_terrain_def def = cw_terrain_default_def();
  d->size_x = def.size_x, d->size_z = def.size_z, d->region_size = def.region_size, d->grid_quads = def.grid_quads;
  d->vertex_spacing = def.vertex_spacing, d->min_height = def.min_height, d->max_height = def.max_height;
  d->far_distance = def.far_distance;
  d->blend_sharpness = def.blend_sharpness, d->macro_tint_scale = def.macro_tint_scale;
  d->macro_layer_scale = def.macro_layer_scale, d->macro_layer_far = def.macro_layer_far;
  d->parallax_distance = def.parallax_distance;
  d->heightmap = d->control = d->splat = cw_intern("");
  d->generate_feature = 200, d->generate_amplitude = 1, d->generate_octaves = 6;
  for (int i = 0; i < CW_TERRAIN_LAYERS; i++) d->albedo[i] = d->normal[i] = cw_intern(""), d->uv_scale[i] = 0.25f;
  d->color[0] = d->color[1] = d->color[2] = d->color[3] = 1;
  d->roughness = 1;
  d->physics = true;
  d->cast_shadow = true;
  d->quality = -1;
  d->dirty = true;
}

static t3_vec3 node_origin(cw_node n, const terrain3d *d) {
  t3_vec3 p = cw_node3d_global_position(n);
  float sp = d->vertex_spacing > 0 ? d->vertex_spacing : 1;
  return (t3_vec3){ p.x - (float)d->size_x * sp * 0.5f, p.y, p.z - (float)d->size_z * sp * 0.5f };
}

static bool add_scatter_child(cw_node c, void *u) {
  cw_node parent = *(cw_node *)u;
  scatter3d *s = cw_node_data_t(c, scatter_t);
  if (s) {
    terrain3d *d = cw_node_data_t(parent, terrain_t);
    s->layer = -1; /* the old terrain took its layers with it */
    scatter_apply(c, s, d->t);
  }
  return false;
}

/* the node's shading properties into a def (cw_terrain_set_shading takes them live) */
static void shading_def(const terrain3d *d, cw_terrain_def *def) {
  def->side_detile = d->side_detile, def->blend_sharpness = d->blend_sharpness;
  def->macro_tint = d->macro_tint, def->macro_tint_scale = d->macro_tint_scale;
  def->macro_layer = d->macro_layer, def->macro_layer_scale = d->macro_layer_scale;
  def->macro_layer_near = d->macro_layer_near, def->macro_layer_far = d->macro_layer_far;
  def->uv_blend = d->uv_blend, def->parallax_distance = d->parallax_distance;
}

static void build(cw_node n, terrain3d *d) {
  d->dirty = false;
  if (d->t) cw_terrain_destroy(d->t);
  d->t = NULL;
  d->bound = NULL;
  cw_terrain_def def = cw_terrain_default_def();
  def.size_x = d->size_x, def.size_z = d->size_z, def.region_size = d->region_size, def.grid_quads = d->grid_quads;
  def.lod_count = d->lod_count, def.vertex_spacing = d->vertex_spacing, def.min_height = d->min_height;
  def.max_height = d->max_height, def.lod0_range = d->lod0_range, def.far_distance = d->far_distance;
  def.visible_range = d->visible_range;
  shading_def(d, &def);
  def.no_shadow_cast = !d->cast_shadow;
  def.origin = node_origin(n, d);
  cw_terrain *t = cw_terrain_create(&def);
  if (!t) return;
  d->t = t;
  d->origin = t->def.origin;
  if (d->heightmap && *d->heightmap) cw_terrain_import_heightmap(t, d->heightmap);
  else if (d->generate_seed) cw_terrain_generate(t, (uint32_t)d->generate_seed, d->generate_feature, d->generate_octaves, d->generate_amplitude);
  if (d->control && *d->control) cw_terrain_import_control(t, d->control);
  else if (d->splat && *d->splat) cw_terrain_import_splat(t, d->splat, 0);
  for (int i = 0; i < CW_TERRAIN_LAYERS; i++)
    if ((d->albedo[i] && *d->albedo[i]) || (d->normal[i] && *d->normal[i])) cw_terrain_set_layer(t, i, d->albedo[i], d->normal[i], d->uv_scale[i]);
    else t->layer[i].uv_scale = d->uv_scale[i];
  for (int i = 0; i < CW_TERRAIN_LAYERS; i++)
    cw_terrain_set_layer_blend_sharpness(t, i, d->sharpness[i]), cw_terrain_set_layer_parallax(t, i, d->parallax[i]);
  cw_terrain_set_quality(t, d->quality);
  cw_tree *tree = cw_node_tree(n);
  if (tree) {
    if (d->physics) cw_terrain_attach_physics(t, cw_physics3d_world(tree), NULL);
    cw_terrain_add_to_scene(t, cw_render3d_scene(tree));
    t3_material *m = cw_terrain_material(t);
    m->color = (t3_color){ d->color[0], d->color[1], d->color[2] };
    m->roughness = d->roughness;
  }
  for (int i = 0; i < cw_node_child_count(n); i++) add_scatter_child(cw_node_child(n, i), &n);
}

/* a scatter child to apply: changed, not yet added, or waiting for a density map the file system may now hold */
static bool scatter_pending(const scatter3d *s) {
  return s && (s->dirty || s->layer < 0 || (s->map_wait && cw_vfs_generation() != s->map_gen));
}

static bool find_dirty_scatter(cw_node c, void *u) {
  terrain3d *d = u;
  scatter3d *s = cw_node_data_t(c, scatter_t);
  if (s && d->t && scatter_pending(s)) {
    if (s->map_wait && !s->dirty && s->layer >= 0 && !cw_vfs_exists(s->density_map)) s->map_gen = cw_vfs_generation();
    else scatter_apply(c, s, d->t);
  }
  return false;
}

static bool same_point(t3_vec3 a, t3_vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static void terrain_sync(cw_node n, terrain3d *d) {
  if (d->dirty) build(n, d);
  if (!d->t) return;
  /* the node owns the origin: a cw_terrain_set_origin from elsewhere is undone here, and said once */
  if (!same_point(d->t->def.origin, d->origin) && !d->origin_warned) {
    cw_warn("Terrain3D %s: its terrain was moved with cw_terrain_set_origin; it follows its node, so move the node instead", cw_node_name(n));
    d->origin_warned = true;
  }
  d->origin = node_origin(n, d);
  cw_terrain_set_origin(d->t, d->origin);
  d->shown = cw_node_visible_in_tree(n);
  cw__terrain_set_shown(d->t, d->shown);
  for (int i = 0; i < cw_node_child_count(n); i++) find_dirty_scatter(cw_node_child(n, i), d);
}

static void terrain_ready(cw_node n, void *data) { terrain_sync(n, data); }
static void terrain_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  terrain_sync(n, data);
}

static void terrain_changed(cw_node n, void *data, const cw_prop *p) {
  (void)n;
  terrain3d *d = data;
  if (d->t && !strcmp(p->name, "quality")) {
    cw_terrain_set_quality(d->t, d->quality);
    return;
  }
  static const char *const shading_props[] = { "side_detile", "blend_sharpness", "macro_tint", "macro_tint_scale",
                                               "macro_layer", "macro_layer_scale", "macro_layer_near",
                                               "macro_layer_far", "parallax_distance", "uv_blend" };
  for (int i = 0; d->t && i < (int)CW_ARRAY_LEN(shading_props); i++)
    if (!strcmp(p->name, shading_props[i])) {
      cw_terrain_def def = *cw_terrain_get_def(d->t);
      shading_def(d, &def);
      cw_terrain_set_shading(d->t, &def);
      return;
    }
  if (d->t && !strncmp(p->name, "layer", 5) && p->name[5] >= '0' && p->name[5] < '0' + CW_TERRAIN_LAYERS && p->name[6] == '_' &&
      (!strcmp(p->name + 7, "blend_sharpness") || !strcmp(p->name + 7, "parallax"))) {
    int i = p->name[5] - '0';
    cw_terrain_set_layer_blend_sharpness(d->t, i, d->sharpness[i]);
    cw_terrain_set_layer_parallax(d->t, i, d->parallax[i]);
    return;
  }
  if (d->t && (!strcmp(p->name, "color") || !strcmp(p->name, "roughness"))) {
    t3_material *m = cw_terrain_material(d->t);
    m->color = (t3_color){ d->color[0], d->color[1], d->color[2] };
    m->roughness = d->roughness;
    return;
  }
  d->dirty = true;
}

static void terrain_destroy(cw_node n, void *data) {
  (void)n;
  terrain3d *d = data;
  if (d->t) cw_terrain_destroy(d->t);
  d->t = NULL;
}

/* the terrains of a tree, for the pass */
typedef struct terrain_tree {
  CW_ARRAY(cw_node) nodes;
} terrain_tree;

static void ttree_free(void *p) {
  terrain_tree *tt = p;
  cw_array_free(&tt->nodes);
  cw_free(tt);
}

static void terrain_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  terrain_tree *tt = cw_tree_slot(ctx->tree, "terrain");
  if (!tt || !ctx->camera) return;
  t3_object *cam = &ctx->camera->base;
  t3_vec3 cp = { cam->matrix_world.e[12], cam->matrix_world.e[13], cam->matrix_world.e[14] };
  /* one-pass split screen: the pass runs once per view, each selecting its own patches and scatter (its own meshes,
   * on its view's layer); the first view's run does the frame's work (uploads, physics) as a lone view's does */
  int view = ctx->view_count > 0 ? ctx->view_index : 0, views = ctx->view_count > 0 ? ctx->view_count : 0;
  for (int i = 0; i < tt->nodes.count; i++) {
    terrain3d *d = cw_node_data_t(tt->nodes.data[i], terrain_t);
    if (!d) continue;
    if (view > 0) {
      if (d->t && d->shown) cw__terrain_update_view(d->t, view, views, cp, &ctx->view_proj);
      continue;
    }
    terrain_sync(tt->nodes.data[i], d);
    if (!d->t) continue;
    cw_terrain_add_to_scene(d->t, ctx->scene3d);
    if (!d->shown) continue; /* hidden: nothing of it is drawn or cast, and nothing is selected or posed for this view */
    if (ctx->renderer && d->bound != ctx->renderer) {
      cw_terrain_bind_renderer(d->t, ctx->renderer);
      d->bound = ctx->renderer;
    }
    cw__terrain_update_view(d->t, 0, views, cp, &ctx->view_proj);
  }
}

static void terrain_enter(cw_node n, void *data) {
  terrain3d *d = data;
  cw_tree *t = cw_node_tree(n);
  terrain_tree *tt = cw_tree_slot(t, "terrain");
  if (!tt) {
    tt = cw_calloc(1, sizeof *tt, CW_MEM_RENDER);
    cw_tree_set_slot(t, "terrain", tt, ttree_free);
  }
  cw_array_push(&tt->nodes, n);
  cw_pipeline *p = cw_pipeline_of(t);
  if (!cw_pipeline_has_pass(p, "terrain"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "terrain", .phase = CW_PHASE_SHADOW, .order = 1500,
                                             .execute = terrain_exec, .views = CW_PASS_VIEWS_EACH });
  d->tree = t;
}

static void terrain_exit(cw_node n, void *data) {
  terrain3d *d = data;
  cw_tree *t = cw_node_tree(n);
  terrain_tree *tt = t ? cw_tree_slot(t, "terrain") : NULL;
  if (tt)
    for (int i = 0; i < tt->nodes.count; i++)
      if (tt->nodes.data[i].id == n.id) {
        tt->nodes.data[i] = tt->nodes.data[--tt->nodes.count];
        break;
      }
  /* the terrain leaves its scene and world with the node */
  if (d->t) cw_terrain_destroy(d->t);
  d->t = NULL;
  d->dirty = true;
}

static const cw_prop terrain_props[] = {
  CW_PROP_FIELD_RANGE(terrain3d, size_x, CW_T_INT, 8, 8192, "Quads along x (a multiple of grid_quads)."),
  CW_PROP_FIELD_RANGE(terrain3d, size_z, CW_T_INT, 8, 8192, "Quads along z."),
  CW_PROP_FIELD_RANGE(terrain3d, region_size, CW_T_INT, 8, 8192, "Quads per physics region (a power of two)."),
  CW_PROP_FIELD_RANGE(terrain3d, vertex_spacing, CW_T_FLOAT, 0.01, 100, "Metres between samples."),
  CW_PROP_FIELD(terrain3d, min_height, CW_T_FLOAT, CW_PROP_DEFAULT, "Height of step 0 (metres, above the node)."),
  CW_PROP_FIELD(terrain3d, max_height, CW_T_FLOAT, CW_PROP_DEFAULT, "Height of step 65535."),
  CW_PROP_FIELD_RANGE(terrain3d, grid_quads, CW_T_INT, 8, 64, "Quads along a patch's side (power of two)."),
  CW_PROP_FIELD_RANGE(terrain3d, lod_count, CW_T_INT, 0, CW_TERRAIN_MAX_LODS, "Levels (0 = as many as the size allows)."),
  CW_PROP_FIELD_RANGE(terrain3d, lod0_range, CW_T_FLOAT, 0, 100000, "Metres the finest level reaches (0 = the least that stays crack-free)."),
  CW_PROP_FIELD_RANGE(terrain3d, side_detile, CW_T_FLOAT, 0, 1, "Cliffs hide the layer's repeat: each small cell reads the layer at its own offset, with colour and strata by world position (0: off)."),
  CW_PROP_FIELD_RANGE(terrain3d, blend_sharpness, CW_T_FLOAT, 0, 1, "Height blend: two layers share where their weighted heights are within this (smaller: crisper edges along the higher layer's shapes; 0 = 0.15)."),
  CW_PROP_FIELD_RANGE(terrain3d, macro_tint, CW_T_FLOAT, 0, 1, "Near ground's brightness and hue varied by world-space noise, this much either way, so one layer stops reading as wallpaper (0: off)."),
  CW_PROP_FIELD_RANGE(terrain3d, macro_tint_scale, CW_T_FLOAT, 0, 100000, "Metres across a feature of the macro tint (0 = 40)."),
  CW_PROP_FIELD_RANGE(terrain3d, macro_layer, CW_T_FLOAT, 0, 1, "Share of a second read of each layer with a larger tile, blended in by distance (0: off)."),
  CW_PROP_FIELD_RANGE(terrain3d, macro_layer_scale, CW_T_FLOAT, 0, 1000, "The second read's tile, times the layer's own (0 = 4)."),
  CW_PROP_FIELD_RANGE(terrain3d, macro_layer_near, CW_T_FLOAT, 0, 100000, "Metres where the second read starts fading in."),
  CW_PROP_FIELD_RANGE(terrain3d, macro_layer_far, CW_T_FLOAT, 0, 100000, "Metres where the second read has its full share (0 = 60)."),
  CW_PROP_FIELD(terrain3d, uv_blend, CW_T_BOOL, CW_PROP_DEFAULT, "Control texels of different uv rotation or scale blended across the texel by height, so a change of rotation draws no seam (off: each point takes its nearest texel's, a cheaper program)."),
  CW_PROP_FIELD_RANGE(terrain3d, parallax_distance, CW_T_FLOAT, 0, 100000, "Metres within which layers with a parallax depth are drawn with parallax occlusion (0 = 30)."),
  CW_PROP_FIELD_RANGE(terrain3d, far_distance, CW_T_FLOAT, 1, 100000, "Metres past which only the macro colour is drawn (x quality)."),
  CW_PROP_FIELD_RANGE(terrain3d, visible_range, CW_T_FLOAT, 0, 1e7, "Patches farther are not drawn (0 = unlimited)."),
  CW_PROP_FIELD(terrain3d, heightmap, CW_T_STRING, CW_PROP_DEFAULT, "16-bit grey PNG or .r16 heightmap (resized to the samples)."),
  CW_PROP_FIELD(terrain3d, control, CW_T_STRING, CW_PROP_DEFAULT, "Terrain3D control map as a PNG (RGBA = the word, big end in R)."),
  CW_PROP_FIELD(terrain3d, splat, CW_T_STRING, CW_PROP_DEFAULT, "RGBA weights for layers 0..3 (used when control is empty)."),
  CW_PROP_FIELD(terrain3d, generate_seed, CW_T_INT, CW_PROP_DEFAULT, "With no heightmap: fBm heights from this seed (0 = flat)."),
  CW_PROP_FIELD_RANGE(terrain3d, generate_feature, CW_T_FLOAT, 1, 100000, "Generated hills' size, metres."),
  CW_PROP_FIELD_RANGE(terrain3d, generate_amplitude, CW_T_FLOAT, 0, 1, "Generated heights' share of the range."),
  CW_PROP_FIELD_RANGE(terrain3d, generate_octaves, CW_T_INT, 1, 12, "Generated octaves."),
  LAYER_PROPS(0), LAYER_PROPS(1), LAYER_PROPS(2), LAYER_PROPS(3),
  LAYER_PROPS(4), LAYER_PROPS(5), LAYER_PROPS(6), LAYER_PROPS(7),
  CW_PROP_FIELD(terrain3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Tint over the splat."),
  CW_PROP_FIELD_RANGE(terrain3d, roughness, CW_T_FLOAT, 0, 1, "Scales the layers' roughness."),
  CW_PROP_FIELD(terrain3d, physics, CW_T_BOOL, CW_PROP_DEFAULT, "Height-field bodies in the tree's 3D world."),
  CW_PROP_FIELD(terrain3d, cast_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "The near patches cast shadows (morphed as drawn)."),
  CW_PROP_FIELD_RANGE(terrain3d, quality, CW_T_INT, -1, 3, "Shading level 0..3 (Low..Ultra); -1 follows the graphics settings."),
  CW_PROP_END,
};

static cw_node_type terrain_type = {
  .name = "Terrain3D", .parent = "Node3D", .data_size = sizeof(terrain3d), .props = terrain_props,
  .doc = "Heightmap terrain: CDLOD, Box3D height-field collision, Terrain3D-style splat (terrain.h).",
  .init = terrain_init, .destroy = terrain_destroy, .enter_tree = terrain_enter, .ready = terrain_ready,
  .exit_tree = terrain_exit, .fixed_update = terrain_fixed, .prop_changed = terrain_changed,
};

static cw_terrain *terrain_of_data(void *data) { return data ? ((terrain3d *)data)->t : NULL; }

cw_terrain *cw_terrain_of(cw_node n) {
  terrain3d *d = terrain_t ? cw_node_data_t(n, terrain_t) : NULL;
  if (!d) return NULL;
  if (!cw_node_tree(n)) return d->t;
  /* a changed TerrainScatter3D child (a density_map set from code, a map just written) is applied at once, so the
   * game reads its new instances the same tick (cw_terrain_scatter_cell) */
  bool pending = d->dirty;
  for (int i = 0; !pending && !cw__terrain_lazy_scatter_test && i < cw_node_child_count(n); i++)
    pending = scatter_pending(cw_node_data_t(cw_node_child(n, i), scatter_t));
  if (pending) terrain_sync(n, d);
  return d->t;
}

void cw__register_terrain_types(void) {
  cw_node_type_register(&terrain_type);
  terrain_t = &terrain_type;
  cw_node_type_register(&scatter_type);
  scatter_t = &scatter_type;
}

/* ── the built-in scatter meshes, for code that makes its own layers ── */
void cw_scatter_def_builtin(cw_scatter_def *d, cw_scatter_shape shape, float r, float g, float b) {
  if (!d) return;
  float c[4] = { r, g, b, 1 };
  int s = shape == CW_SCATTER_ROCK ? 1 : shape == CW_SCATTER_TREE ? 2 : 0;
  for (int l = 0; l < 2; l++) builtin_mesh(s, l, c, &d->geometry[l][0], &d->material[l][0]);
  if (s == 0 && !(d->tint_jitter > 0)) d->tint_jitter = 0.25f; /* grass varies from tuft to tuft */
}

void cw_scatter_def_release(cw_scatter_def *d) {
  if (!d) return;
  for (int l = 0; l < CW_SCATTER_LODS; l++)
    for (int p = 0; p < CW_SCATTER_PARTS; p++) {
      if (d->geometry[l][p]) t3_release(d->geometry[l][p]);
      if (d->material[l][p]) t3_release(d->material[l][p]);
      d->geometry[l][p] = NULL;
      d->material[l][p] = NULL;
    }
}

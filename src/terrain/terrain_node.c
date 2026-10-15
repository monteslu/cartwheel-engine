/* The Terrain3D and TerrainScatter3D node types (cartwheel/terrain.h).
 *
 * Terrain3D builds its cw_terrain when it becomes ready (so its physics is
 * there before the first tick) and again whenever a property changes (at
 * the next fixed update or frame). The terrain is centred on the node
 * (translation only). Each frame the pipeline's "terrain" SHADOW-phase pass
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
  /* runtime */
  int layer;                 /* in the parent's terrain, -1 none */
  bool dirty;
  cw_res res[CW_SCATTER_PARTS];
} scatter3d;

static const char *const shape_names[] = { "GRASS", "ROCK", "TREE", NULL };
static const char *const knob_names[] = { "GRASS", "SCATTER", NULL };
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
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, t3_attribute_new(T3_FLOAT32, pos, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, t3_attribute_new(T3_FLOAT32, nrm, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_COLOR, t3_attribute_new(T3_FLOAT32, col, nv, 3));
  t3_geometry_set_index(g, t3_attribute_new(T3_UINT16, idx, ni, 1));
  cw_free(pos), cw_free(nrm), cw_free(col), cw_free(idx);
  return g;
}

/* a grass tuft: `blades` thin triangles fanned around the centre, dark at the root, light at the tip */
static t3_geometry *tuft(int blades) {
  int nv = blades * 3;
  float *pos = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER), *nrm = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  float *col = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  for (int b = 0; b < blades; b++) {
    float a = (float)b / (float)blades * 3.14159265f, c = cosf(a), s = sinf(a);
    float lean = 0.12f * (float)((b * 7) % 5 - 2), h = 0.45f + 0.08f * (float)((b * 3) % 4);
    float ox = 0.06f * cosf(a * 3), oz = 0.06f * sinf(a * 5);
    float p[3][3] = { { ox - c * 0.05f, 0, oz - s * 0.05f }, { ox + c * 0.05f, 0, oz + s * 0.05f },
                      { ox - s * lean, h, oz + c * lean } };
    for (int v = 0; v < 3; v++) {
      int i = b * 3 + v;
      memcpy(&pos[3 * i], p[v], sizeof p[v]);
      /* normals mostly up: lit like the ground under them */
      nrm[3 * i] = -s * 0.3f, nrm[3 * i + 1] = 0.95f, nrm[3 * i + 2] = c * 0.3f;
      float tip = v == 2 ? 1.0f : 0.0f;
      col[3 * i] = 0.10f + 0.25f * tip, col[3 * i + 1] = 0.22f + 0.33f * tip, col[3 * i + 2] = 0.05f + 0.08f * tip;
    }
  }
  t3_geometry *g = t3_geometry_new();
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, t3_attribute_new(T3_FLOAT32, pos, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, t3_attribute_new(T3_FLOAT32, nrm, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_COLOR, t3_attribute_new(T3_FLOAT32, col, nv, 3));
  cw_free(pos), cw_free(nrm), cw_free(col);
  return g;
}

static void builtin_mesh(int shape, int lod, const float color[4], t3_geometry **g, t3_material **m) {
  t3_material *mat = t3_mesh_standard_material_new(0xffffff);
  mat->vertex_colors = true;
  mat->color = (t3_color){ color[0], color[1], color[2] };
  mat->roughness = 0.9f;
  mat->metalness = 0;
  if (shape == 0) {
    *g = tuft(lod ? 2 : 5);
    mat->side = T3_DOUBLE_SIDE;
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

static void scatter_destroy(cw_node n, void *data) {
  (void)n;
  scatter3d *s = data;
  for (int p = 0; p < CW_SCATTER_PARTS; p++) {
    if (s->res[p].id) cw_res_release(s->res[p]);
    s->res[p] = (cw_res){ 0 };
  }
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
  if (s->mesh && *s->mesh) {
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
  d.seed = (uint32_t)s->seed ^ (uint32_t)cw_hash_str(cw_node_name(n));
  uint8_t *grey = NULL;
  if (s->density_map && *s->density_map) {
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

static const cw_prop scatter_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(scatter3d, shape), .size = sizeof(int),
    .enum_names = shape_names, .doc = "Built-in mesh when `mesh` is empty: GRASS (tufts), ROCK, TREE (two LODs each, vertex coloured)." },
  CW_PROP_FIELD(scatter3d, mesh, CW_T_STRING, CW_PROP_DEFAULT, "A glTF mesh: \"file.glb#mesh/m/p\" one primitive, or \"file.glb\" (one mesh) its primitives as parts (a trunk and a crown); its LOD chains give the LODs."),
  CW_PROP_FIELD(scatter3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Tint of the built-in meshes."),
  CW_PROP_FIELD_RANGE(scatter3d, density, CW_T_FLOAT, 0, 16, "Instances per square metre at full density."),
  CW_PROP_FIELD(scatter3d, density_map, CW_T_STRING, CW_PROP_DEFAULT, "Grey image over the whole terrain scaling the density (empty = 1)."),
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
  CW_PROP_END,
};

static cw_node_type scatter_type = {
  .name = "TerrainScatter3D", .parent = "Node", .data_size = sizeof(scatter3d), .props = scatter_props,
  .doc = "A scatter layer (grass, rocks, trees) of its parent Terrain3D (terrain.h).", .init = scatter_init,
  .destroy = scatter_destroy, .prop_changed = scatter_changed,
};

/* ── Terrain3D ─────────────────────────────────────────────────────── */

typedef struct terrain3d {
  int size_x, size_z, region_size, grid_quads, lod_count;
  float vertex_spacing, min_height, max_height, lod0_range, far_distance, visible_range;
  const char *heightmap, *control, *splat;
  int generate_seed;
  float generate_feature, generate_amplitude;
  int generate_octaves;
  const char *albedo[CW_TERRAIN_LAYERS], *normal[CW_TERRAIN_LAYERS];
  float uv_scale[CW_TERRAIN_LAYERS];
  float color[4];
  float roughness;
  bool physics, cast_shadow;
  int quality;
  /* runtime */
  cw_terrain *t;
  bool dirty;
  t3_renderer *bound;
  cw_tree *tree;
} terrain3d;

#define LAYER_FIELD(i, fld, nm, ty, docstr)                                                                    \
  { .name = "layer" #i "_" nm, .type = ty, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(terrain3d, fld[i]), \
    .size = (int)sizeof(((terrain3d *)0)->fld[i]), .doc = docstr }
#define LAYER_PROPS(i)                                                                                     \
  LAYER_FIELD(i, albedo, "albedo", CW_T_STRING, "Layer " #i ": albedo RGB + height A image."),             \
    LAYER_FIELD(i, normal, "normal", CW_T_STRING, "Layer " #i ": normal RGB (+Y) + roughness A image."),   \
    LAYER_FIELD(i, uv_scale, "uv_scale", CW_T_FLOAT, "Layer " #i ": texture repeats per metre.")

static void terrain_init(cw_node n, void *data) {
  (void)n;
  terrain3d *d = data;
  cw_terrain_def def = cw_terrain_default_def();
  d->size_x = def.size_x, d->size_z = def.size_z, d->region_size = def.region_size, d->grid_quads = def.grid_quads;
  d->vertex_spacing = def.vertex_spacing, d->min_height = def.min_height, d->max_height = def.max_height;
  d->far_distance = def.far_distance;
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
  def.no_shadow_cast = !d->cast_shadow;
  def.origin = node_origin(n, d);
  cw_terrain *t = cw_terrain_create(&def);
  if (!t) return;
  d->t = t;
  if (d->heightmap && *d->heightmap) cw_terrain_import_heightmap(t, d->heightmap);
  else if (d->generate_seed) cw_terrain_generate(t, (uint32_t)d->generate_seed, d->generate_feature, d->generate_octaves, d->generate_amplitude);
  if (d->control && *d->control) cw_terrain_import_control(t, d->control);
  else if (d->splat && *d->splat) cw_terrain_import_splat(t, d->splat, 0);
  for (int i = 0; i < CW_TERRAIN_LAYERS; i++)
    if ((d->albedo[i] && *d->albedo[i]) || (d->normal[i] && *d->normal[i])) cw_terrain_set_layer(t, i, d->albedo[i], d->normal[i], d->uv_scale[i]);
    else t->layer[i].uv_scale = d->uv_scale[i];
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

static bool find_dirty_scatter(cw_node c, void *u) {
  terrain3d *d = u;
  scatter3d *s = cw_node_data_t(c, scatter_t);
  if (s && (s->dirty || s->layer < 0) && d->t) scatter_apply(c, s, d->t);
  return false;
}

static void terrain_sync(cw_node n, terrain3d *d) {
  if (d->dirty) build(n, d);
  if (!d->t) return;
  cw_terrain_set_origin(d->t, node_origin(n, d));
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
  for (int i = 0; i < tt->nodes.count; i++) {
    terrain3d *d = cw_node_data_t(tt->nodes.data[i], terrain_t);
    if (!d) continue;
    terrain_sync(tt->nodes.data[i], d);
    if (!d->t) continue;
    cw_terrain_add_to_scene(d->t, ctx->scene3d);
    if (ctx->renderer && d->bound != ctx->renderer) {
      cw_terrain_bind_renderer(d->t, ctx->renderer);
      d->bound = ctx->renderer;
    }
    cw_terrain_update(d->t, cp, &ctx->view_proj);
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
                                             .execute = terrain_exec });
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

cw_terrain *cw_terrain_of(cw_node n) {
  terrain3d *d = terrain_t ? cw_node_data_t(n, terrain_t) : NULL;
  if (!d) return NULL;
  if (d->dirty && cw_node_tree(n)) terrain_sync(n, d);
  return d->t;
}

void cw__register_terrain_types(void) {
  cw_node_type_register(&terrain_type);
  terrain_t = &terrain_type;
  cw_node_type_register(&scatter_type);
  scatter_t = &scatter_type;
}

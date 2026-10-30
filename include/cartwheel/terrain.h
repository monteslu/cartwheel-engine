/* cartwheel terrain: heightmap terrain drawn with CDLOD, collided through
 * Box3D's height field, painted with Terrain3D's control-map layout, and
 * covered with deterministic scatter (grass, rocks, trees).
 *
 * ── data ───────────────────────────────────────────────────────────────
 * A terrain is a grid of (size_x + 1) x (size_z + 1) height samples,
 * vertex_spacing metres apart, its sample (0, 0) at `origin`. Heights are
 * uint16 steps between min_height and max_height:
 *   y = origin.y + min_height + q * (max_height - min_height) / 65535
 * which is exactly how Box3D's height field stores them, so physics,
 * gameplay (cw_terrain_height_at) and the drawn surface agree. Each sample
 * also carries a control word in Terrain3D's layout (bits 31..27 base layer,
 * 26..22 overlay layer, 21..14 blend, 13..10 uv rotation in 1/16 turns,
 * 9..7 uv scale step, bit 2 hole; cw_terrain_control below), so painted maps
 * from Terrain3D's tools mean the same thing here.
 *
 * Triangles: every quad splits along the diagonal from its (x, z + 1)
 * corner to its (x + 1, z) corner, as Box3D's height field does.
 *
 * ── drawing (CDLOD) ────────────────────────────────────────────────────
 * The terrain is a quadtree of patches. Each frame a camera selects the
 * patches to draw (cw_terrain_update): a patch of level L covers
 * grid_quads << L quads and is drawn with the same grid_quads x grid_quads
 * grid, so its vertices are 2^L samples apart. Level L is used within
 * lod0_range * lod_ratio^L metres of the camera (3D distance, so mountains
 * get the detail they need) and its odd vertices morph vertically into the
 * next level's triangles before that range ends, so neighbouring levels meet
 * without cracks or skirts. Selection runs on the job pool (one job per
 * root patch, results in root order), culls patches against the view
 * frustum four children at a time with SIMD, and sorts what it keeps front
 * to back. Every selected patch is one instance of one InstancedMesh: the
 * whole near terrain is one draw, the far terrain another.
 *
 * Shading is three.js r186's MeshStandardMaterial (lights, shadows, fog and
 * environment are three.js's own) with a TSL extension of cartwheel's
 * (src/terrain/extensions): the height fetch and morph in the vertex stage;
 * in the fragment stage the splat of 8 layers by the control map (the two
 * strongest at a point, weighted from the four control texels round it),
 * triplanar on steep slopes, and past far_distance only a baked macro colour
 * and the normal map (two texture reads). Patches wholly past far_distance
 * draw with a third program that reads only those two.
 *
 * Quality (cartwheel/quality.h, effects detail):
 *   Low     nearest control texel (base + overlay: 2 layers), no triplanar,
 *           far distance x0.5        (the Vega 8 class default)
 *   Medium  nearest control texel, triplanar on steep slopes, far x0.75
 *   High    Medium's shading (two layers weighted from 4 control texels,
 *           height blending, triplanar), far x1
 *   Ultra   High, far x1.5 and patch ranges x1.25
 * and the scatter's density follows cw_quality_scale("grass") / ("scatter").
 *
 * Shadows: the terrain receives shadows and (cast_shadow) casts them: the
 * shadow pass draws the near patches with the same extension's caster
 * program (depth+instbig+xterrain), morphed for the viewing camera, so the
 * shadow follows the drawn surface. Patches past far_distance never cast.
 *
 * ── physics ────────────────────────────────────────────────────────────
 * cw_terrain_attach_physics makes one static body with one Box3D height
 * field per region (region_size quads square, regions share their edge
 * samples and one global height range, so they line up exactly). Holes
 * (control bit 2) are holes for physics too. cw_shape3d_heightfield and
 * cw_heightfield3d (physics3d.h) are the same height field on its own.
 *
 * ── scatter ────────────────────────────────────────────────────────────
 * Scatter layers place copies of a mesh over the terrain in square cells:
 * 32 m, doubled while that stays within a quarter of the layer's visible
 * range (trees at 600 m use 128 m cells: a few dozen draws, not hundreds).
 * Placement is a pure function of (cell, layer seed) by integer hashing:
 * the same instances on every host, at any worker count, whatever the
 * camera did. A candidate is kept by the layer's density (per square
 * metre), its density map, the control layers it may stand on and the
 * slope. Each cell is one InstancedMesh per mesh LOD with its own bounding
 * sphere; cells are generated on the job pool when the camera comes within
 * range, frustum culled, and posed each frame on the job pool: instances
 * shrink to nothing over the last `fade` metres of visible_range (no pop),
 * and switch mesh LOD by distance.
 *
 * ── threads ────────────────────────────────────────────────────────────
 * Main thread, outside physics steps, like the rest of the scene. The
 * functions spread their own work over cw_jobs.
 *
 * ── node types (properties: docs/nodes.json) ───────────────────────────
 *   Terrain3D         (Node3D) a terrain: heightmap (16-bit PNG or .r16),
 *                     control (a Terrain3D control map as a PNG: RGBA = the
 *                     word's bytes, big end in R) or splat (an RGBA weight
 *                     image for layers 0..3), layer0_albedo .. layer7_*,
 *                     sizes, heights, LOD settings, physics. Placing the
 *                     node moves the terrain (translation only).
 *   TerrainScatter3D  (Node) a scatter layer of its parent Terrain3D: mesh
 *                     (a primitive or a glTF resource), density, density_map,
 *                     layers (bit mask of the control layers it grows on),
 *                     slope, scale range, visible_range, fade, LOD meshes,
 *                     cast_shadow, seed. */
#ifndef CW_TERRAIN_H
#define CW_TERRAIN_H

#include "scene.h"
#include "physics3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_terrain cw_terrain;

#define CW_TERRAIN_LAYERS 8
#define CW_TERRAIN_MAX_LODS 12

typedef struct cw_terrain_def {
  int size_x, size_z;       /* quads; multiples of grid_quads: 512 x 512 */
  int region_size;          /* quads per physics region (power of two): 256 */
  float vertex_spacing;     /* metres between samples: 1 */
  float min_height, max_height; /* metres: 0, 100 */
  t3_vec3 origin;           /* world position of sample (0, 0) at height 0 */
  int grid_quads;           /* quads along a patch's side (power of two, 8..64): 32 */
  int lod_count;            /* levels (0 = as many as the sizes allow, at most CW_TERRAIN_MAX_LODS) */
  float lod0_range;         /* metres level 0 reaches; 0 = 2.5 patch widths (raised to that if smaller) */
  float lod_ratio;          /* each level reaches this much farther: 2 */
  float morph_start;        /* where morphing starts inside a level's band, 0..1: 0.66 */
  float far_distance;       /* metres: past it only the macro colour is drawn (0 = 400) */
  float far_fade;           /* metres over which splat fades into macro: 0 = far_distance / 4 */
  float visible_range;      /* patches farther than this are not drawn: 0 = unlimited */
  bool no_shadow_cast;      /* the near patches cast shadows unless this is set: false */
} cw_terrain_def;
/* The defaults noted above. */
cw_terrain_def cw_terrain_default_def(void);

/* A flat terrain at min_height. NULL (logged) for sizes that are not
 * multiples of grid_quads or a region_size that is not a power of two. */
cw_terrain *cw_terrain_create(const cw_terrain_def *def);
/* Frees the terrain, its scatter, its physics and its scene objects. */
void cw_terrain_destroy(cw_terrain *t);
/* The definition it was made with, the levels and ranges it settled on filled in. */
const cw_terrain_def *cw_terrain_get_def(const cw_terrain *t);
/* Moves the terrain (its sample (0, 0) to origin); physics bodies follow. */
void cw_terrain_set_origin(cw_terrain *t, t3_vec3 origin);

/* ── heights and control ──────────────────────────────────────────────── */

/* Sets a w x h rectangle of samples at (x0, z0) (clipped to the terrain)
 * from row-major uint16 steps. Marks the normals, the GPU copies and the
 * physics regions it touches dirty (rebuilt on the next update). */
void cw_terrain_set_heights(cw_terrain *t, int x0, int z0, int w, int h, const uint16_t *q);
/* The same from metres (quantized with Box3D's rounding). */
void cw_terrain_set_heights_m(cw_terrain *t, int x0, int z0, int w, int h, const float *metres);
/* The sample array (size_x + 1) x (size_z + 1), row-major (z rows); read only. */
const uint16_t *cw_terrain_heights(const cw_terrain *t);
/* Control words for a rectangle, as cw_terrain_set_heights. */
void cw_terrain_set_control(cw_terrain *t, int x0, int z0, int w, int h, const uint32_t *ctl);
/* The control words, laid out as cw_terrain_heights; read only. */
const uint32_t *cw_terrain_controls(const cw_terrain *t);
/* A control word from its fields (Terrain3D's layout). blend 0..255 is the
 * overlay's share, uv_angle 0..15 sixteenths of a turn, uv_scale 0..7 (0 =
 * 1x, 1..4 = +20 % steps, 5..7 = -20 % steps). */
uint32_t cw_terrain_control(int base, int overlay, int blend, int uv_angle, int uv_scale, bool hole);
/* fBm heights over the whole terrain (render-independent, deterministic):
 * octaves of value noise from integer hashing; amplitude 0..1 of the range. */
void cw_terrain_generate(cw_terrain *t, uint32_t seed, float feature_m, int octaves, float amplitude);

/* ── import ───────────────────────────────────────────────────────────── */

/* A heightmap image into the samples (resized to fit by bilinear filtering
 * when its size differs): a 16-bit grey PNG (8-bit works, coarsely), or a
 * raw .r16 (little-endian uint16, square: width = sqrt(bytes / 2)). Values
 * map 0..65535 onto min_height..max_height. Paths go through the VFS. */
bool cw_terrain_import_heightmap(cw_terrain *t, const char *path);
/* A Terrain3D control map exported as a PNG (each pixel's RGBA bytes are
 * the word, big end in R), nearest-resized to the samples. */
bool cw_terrain_import_control(cw_terrain *t, const char *path);
/* A splat (weight) image: R, G, B, A weigh layers first_layer .. +3. Each
 * sample gets the two heaviest as base and overlay, blended by weight. */
bool cw_terrain_import_splat(cw_terrain *t, const char *path, int first_layer);

/* ── layers ───────────────────────────────────────────────────────────── */

/* Layer i's textures, Terrain3D's packing: albedo RGB + height A, and
 * normal RGB (OpenGL convention, +Y) + roughness A. Either path may be NULL
 * (flat grey, flat normal, roughness 0.9). The images are resized to the
 * layer tile size (512) into two atlases. uv_scale: repeats per metre. */
bool cw_terrain_set_layer(cw_terrain *t, int i, const char *albedo_height, const char *normal_rough, float uv_scale);
/* The same from RGBA8 pixels (w x h), e.g. generated textures. */
void cw_terrain_set_layer_pixels(cw_terrain *t, int i, const uint8_t *albedo_height, const uint8_t *normal_rough,
                                 int w, int h, float uv_scale);

/* Use src's layers (its textures, uv scales and mean colours) instead of
 * this terrain's own: what each cell's terrain of a streamed world does
 * (cartwheel/stream.h), so the 2 x 8 layer atlases (about 11 MB of GPU
 * memory with mips) exist once, not once per cell. The textures are
 * reference counted: src may be destroyed first. Set src's layers before;
 * this terrain's own cw_terrain_set_layer is refused afterwards. */
void cw_terrain_share_layers(cw_terrain *t, cw_terrain *src);

/* ── queries (deterministic: + - * / and Box3D) ───────────────────────── */

/* Height at world (x, z) on the triangles physics collides with: the same
 * value a downward ray cast against the terrain's physics finds. Outside the
 * terrain, the nearest edge's height. */
float cw_terrain_height_at(const cw_terrain *t, float x, float z);
/* The triangle's normal there (unit, +Y up). */
t3_vec3 cw_terrain_normal_at(const cw_terrain *t, float x, float z);
typedef struct cw_terrain_hit {
  t3_vec3 point, normal;
  float distance;
  uint32_t control;         /* the control word of the nearest sample */
} cw_terrain_hit;
/* A ray from o along unit d, up to maxd metres, against the triangles
 * (holes let it through). */
bool cw_terrain_raycast(const cw_terrain *t, t3_vec3 o, t3_vec3 d, float maxd, cw_terrain_hit *out);
/* True when the sample nearest (x, z) is a hole. */
bool cw_terrain_is_hole(const cw_terrain *t, float x, float z);

/* ── physics ──────────────────────────────────────────────────────────── */

/* Static height-field bodies in w (one per region) with def's layer, mask
 * and material (NULL = defaults). Replaces earlier ones; edits rebuild the
 * regions they touch on the next cw_terrain_update (or
 * cw_terrain_sync_physics). */
void cw_terrain_attach_physics(cw_terrain *t, cw_world3d w, const cw_shape3d_def *def);
/* Destroys the region bodies (the height fields stay: queries use them). */
void cw_terrain_detach_physics(cw_terrain *t);
/* Rebuilds the dirty regions now (one job per region). */
void cw_terrain_sync_physics(cw_terrain *t);
/* Region (rx, rz)'s shape; null when there is none. */
cw_shape3d cw_terrain_region_shape(const cw_terrain *t, int rx, int rz);

/* ── drawing ──────────────────────────────────────────────────────────── */

/* Adds the terrain's meshes to a three.c scene (one terrain draws in one
 * scene). The tree's Terrain3D node does this itself. */
void cw_terrain_add_to_scene(cw_terrain *t, t3_scene *scene);
/* Uploads what changed, picks patches for the camera (vp: its view-
 * projection, for culling; NULL = no culling), poses the scatter. Call once
 * per frame before rendering; the Terrain3D node does it in the pipeline's
 * SHADOW phase. */
void cw_terrain_update(cw_terrain *t, t3_vec3 camera, const t3_mat4 *vp);
/* Registers the terrain's shading hooks with a renderer (the node does
 * this). Needed once per renderer before the terrain draws. */
void cw_terrain_bind_renderer(cw_terrain *t, t3_renderer *r);
/* The near material (MeshStandardMaterial): its colour, roughness and
 * metalness scale the terrain's. */
t3_material *cw_terrain_material(cw_terrain *t);
/* Quality level for shading (CW_QUALITY_LOW .. ULTRA); -1 follows
 * cw_quality_get() (the default). */
void cw_terrain_set_quality(cw_terrain *t, int quality);

typedef struct cw_terrain_stats {
  int patches, patches_far;     /* instances drawn: near / far programs */
  int patches_culled;           /* patches the frustum rejected */
  int nodes_visited;
  int levels[CW_TERRAIN_MAX_LODS]; /* patches per level */
  int scatter_cells, scatter_cells_visible, scatter_instances, scatter_draws;
  float select_ms, scatter_ms, upload_ms;
  uint64_t selection_hash;      /* FNV-1a over the selected patches (tests) */
} cw_terrain_stats;
/* The last update's numbers (zero for NULL). */
void cw_terrain_get_stats(const cw_terrain *t, cw_terrain_stats *out);

/* ── scatter ──────────────────────────────────────────────────────────── */

#define CW_SCATTER_LODS 3
#define CW_SCATTER_PARTS 4
typedef struct cw_scatter_def {
  /* mesh LODs, finest first (a LOD with no part 0 ends the list), each of up to CW_SCATTER_PARTS parts
   * drawn with the same instances (a trunk and a crown: a glTF mesh's primitives); retained */
  t3_geometry *geometry[CW_SCATTER_LODS][CW_SCATTER_PARTS];
  t3_material *material[CW_SCATTER_LODS][CW_SCATTER_PARTS]; /* per part (NULL = the previous LOD's part) */
  float lod_distance[CW_SCATTER_LODS];    /* metres where LOD i + 1 takes over */
  float density;            /* instances per square metre at full density: 0.5 */
  uint32_t layers;          /* control layers it grows on (bit i = layer i; on the base layer, or the
                               overlay where the blend is over half): 0 = all */
  const uint8_t *density_map; /* optional: dm_w x dm_h grey (0..255) over the whole terrain; copied */
  int dm_w, dm_h;
  float min_slope_cos;      /* only where the normal's y is at least this: 0 */
  float scale_min, scale_max; /* uniform scale range: 0.8, 1.2 */
  bool align_to_normal;     /* tilt with the ground (grass, rocks) instead of standing up (trees) */
  float sink;               /* metres pushed into the ground: 0 */
  float visible_range;      /* metres: 60 */
  float fade;               /* metres at the end of visible_range over which instances shrink away: 10 */
  bool cast_shadow;         /* LOD 0 casts (coarser LODs never do): false */
  const char *density_knob; /* cw_quality_scale knob: "grass" (default) or "scatter" */
  uint32_t seed;
} cw_scatter_def;
/* The defaults noted above, no meshes. */
cw_scatter_def cw_scatter_default_def(void);
/* The built-in scatter shapes (TerrainScatter3D's `shape`). */
typedef enum { CW_SCATTER_GRASS, CW_SCATTER_ROCK, CW_SCATTER_TREE } cw_scatter_shape;
/* The built-in meshes (what TerrainScatter3D's `shape` draws: vertex coloured, two
 * LODs each, tinted by r, g, b) into d's geometry and material: NEW references,
 * released with cw_scatter_def_release once the layer is added (the layer
 * holds its own). */
void cw_scatter_def_builtin(cw_scatter_def *d, cw_scatter_shape shape, float r, float g, float b);
/* Releases every geometry and material d holds and clears them. */
void cw_scatter_def_release(cw_scatter_def *d);
/* Adds a scatter layer; returns its index (-1 when 16 exist). */
int cw_terrain_scatter_add(cw_terrain *t, const cw_scatter_def *def);
/* Removes a scatter layer and its instances (its index is free again). */
void cw_terrain_scatter_remove(cw_terrain *t, int layer);
/* Regenerates every cell (after a density map or the heights changed). */
void cw_terrain_scatter_invalidate(cw_terrain *t);
/* Placement only (no GPU): the instances of cell (cx, cz) of a layer, as
 * x, y, z, scale, yaw quintuples into out (up to cap). Returns the count.
 * What a test hashes. */
int cw_terrain_scatter_cell(cw_terrain *t, int layer, int cx, int cz, float *out, int cap);

/* ── terrain sets: terrain that streams with a world's cells ────────────
 * A big world (cartwheel/stream.h) is too big for one height grid: a 16 km
 * square at 2 m would be 67 M samples. A terrain set holds one small
 * terrain per loaded cell instead, all with the same definition, sharing
 * one template terrain's layer textures (cw_terrain_share_layers) and
 * getting the same scatter layers (each cell its own seed). Neighbouring
 * cells must repeat their shared edge samples; with equal definitions their
 * LOD bands line up, so cells meet without cracks. The set draws every
 * terrain from one SHADOW-phase pipeline pass ("terrainset") of its tree.
 * Physics: each cell's height fields are built when it is added, and its
 * static bodies are created only while it is active. Main thread. */
typedef struct cw_terrain_set cw_terrain_set;
#define CW_TERRAIN_SET_SCATTER 4
typedef struct cw_terrain_set_desc {
  cw_terrain_def def;       /* every cell's terrain (origin is per cell) */
  cw_terrain *layers;       /* the template whose layers every cell shares (yours; set its layers first) */
  cw_scatter_def scatter[CW_TERRAIN_SET_SCATTER]; /* each cell's scatter layers (seed ^ cell id * 0x9E3779B1); the set keeps its own references */
  int scatter_count;
  cw_world3d world;         /* physics while active (null: none) */
  cw_shape3d_def shape;     /* the height-field shapes' settings */
} cw_terrain_set_desc;
/* One per tree. NULL (logged) without a tree or template. */
cw_terrain_set *cw_terrain_set_create(cw_tree *tree, const cw_terrain_set_desc *d);
/* Destroys every cell terrain (not the template). */
void cw_terrain_set_destroy(cw_terrain_set *s);
/* A cell's terrain from its samples ((size_x + 1) x (size_z + 1) height
 * steps and control words, either may be NULL), its sample (0, 0) at
 * origin. Builds its height fields (job pool). What on_load does. Cost: a
 * 256 x 256 cell is about a millisecond plus the first frame's upload. */
cw_terrain *cw_terrain_set_add(cw_terrain_set *s, int id, t3_vec3 origin, const uint16_t *q, const uint32_t *ctl);
/* Destroys cell id's terrain (on_unload); no-op when absent. */
void cw_terrain_set_remove(cw_terrain_set *s, int id);
/* Whether cell id's background build (height fields, texels, quadtree: started by
 * cw_terrain_set_add) is done; with wait, finish it now. What a stream's
 * cell_ready returns. True for a cell not in the set. */
bool cw_terrain_set_ready(cw_terrain_set *s, int id, bool wait);
/* Physics on (on_activate) or off (on_deactivate). */
void cw_terrain_set_activate(cw_terrain_set *s, int id, bool on);
/* A new origin for a cell's terrain (on_cell_rebase); its bodies follow. */
void cw_terrain_set_move(cw_terrain_set *s, int id, t3_vec3 origin);
cw_terrain *cw_terrain_set_get(const cw_terrain_set *s, int id); /* NULL when absent */
/* The cell terrain under (x, z) in local coordinates, NULL off every cell. O(cells). */
cw_terrain *cw_terrain_set_at(const cw_terrain_set *s, float x, float z);
typedef struct cw_terrain_set_stats {
  int terrains, terrains_drawn, patches, scatter_instances, scatter_draws;
  float update_ms;          /* the pass's CPU time last frame (selection, scatter posing, uploads) */
  float upload_ms;
} cw_terrain_set_stats;
/* The pass's numbers from the last frame (zero for NULL). */
void cw_terrain_set_get_stats(const cw_terrain_set *s, cw_terrain_set_stats *out);

/* The Terrain3D node's terrain (NULL before it entered a tree or for
 * another node). */
cw_terrain *cw_terrain_of(cw_node terrain3d);

#ifdef __cplusplus
}
#endif
#endif

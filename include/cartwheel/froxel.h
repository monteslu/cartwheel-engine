/* Froxel grid: a camera's view frustum cut into a 3D grid of cells (screen
 * tiles x depth slices), and the volumes (light spheres, spot cones) that
 * touch each cell, binned on the CPU on the job pool. Clustered lighting
 * (lighting.h) uses one per tree; anything else that wants per-cell lists
 * (fog volumes, decals, probes) can make its own grid, or read the lighting
 * one with cw_lighting_froxels.
 *
 * Grid (three.js r186 ClusteredLighting's): tiles of tile_px x tile_px
 * pixels, nx = ceil(w / tile_px), ny = ceil(h / tile_px), and z_slices
 * slices spaced exponentially between near and far:
 *   slice k covers view depth [near * (far / near)^(k / nz), near * (far / near)^((k + 1) / nz)].
 * A view depth d (positive, -view z) is in slice floor(log(d) * zscale + zbias)
 * (clamped to [0, nz - 1]). Tile x counts from the left edge of the image, y
 * from its BOTTOM (NDC y = -1): tile (x, y) of a point at NDC (u, v) is
 * floor((u * 0.5 + 0.5) * nx), floor((v * 0.5 + 0.5) * ny). Cell (x, y, z)
 * has the linear index x + nx * (y + ny * z).
 *
 * Binning (Filament's Froxelizer, ported to C: Apache-2.0, see NOTICE): each
 * volume's screen rectangle and depth range bound the cells it can touch;
 * every candidate cell's view-space box is tested against the sphere (and a
 * cone's angle test, Filament's coneVsSphere on the cell's bounding sphere).
 * Rows of cells (one y and z) are binned in parallel; each row lists its
 * volumes in the order they were given, so the result does not depend on the
 * worker count. A cell keeps at most max_per_froxel volumes (the first in
 * input order); the rest are dropped and counted in the stats.
 *
 * Results: one record per cell, (first item << 8) | count (count <= 255),
 * into one item list of uint16 volume indices, cells in linear order. The
 * GPU copies (cw_froxel_grid_textures) are R32UI DataTextures read with
 * textureLoad: the records one texel a cell (cell f at (f % width, f /
 * width)), the items two to a texel (item k in texel k >> 1, the low 16 bits
 * when k is even). src/render/extensions/cluster_part.mjs (froxelRecord) is
 * the TSL that finds a fragment's record.
 *
 * Cost: O(cells a volume's bounds cover) box tests per volume, spread over
 * the pool once a frame has more than a few thousand tests; memory: 4 bytes
 * a cell, 2 bytes an item, plus max_per_froxel x 2 bytes a cell of scratch.
 * Render-only: nothing here may feed the simulation. */
#ifndef CW_FROXEL_H
#define CW_FROXEL_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_froxel_grid cw_froxel_grid;

typedef struct cw_froxel_config {
  int tile_px;         /* 32 (r186) */
  int z_slices;        /* 24 (r186); at most 256 */
  int max_per_froxel;  /* 64 (r186); at most 255 */
} cw_froxel_config;
#define CW_FROXEL_CONFIG_DEFAULT ((cw_froxel_config){ 32, 24, 64 })

/* A volume in the camera's view space (right-handed, the camera looks down
 * -Z): a sphere, or a spot light's cone (apex at the centre, along dir,
 * cos_angle / sin_angle of the half angle, as long as radius). */
typedef struct cw_froxel_volume {
  float x, y, z, radius;
  float dir[3];        /* unit; ignored for a sphere */
  float cos_angle;     /* <= -1: a sphere */
  float sin_angle;
} cw_froxel_volume;

typedef struct cw_froxel_stats {
  int nx, ny, nz;
  int volumes;         /* binned last time */
  int references;      /* items in all cells */
  int max_in_froxel;
  int overflowed;      /* cells that hit max_per_froxel */
  int dropped;         /* references they dropped */
  float bin_ms;        /* CPU time of the last bin (report only) */
} cw_froxel_stats;

cw_froxel_grid *cw_froxel_grid_new(void);
void cw_froxel_grid_free(cw_froxel_grid *g);
/* Fix the grid for a view of w x h pixels: proj is the camera's projection
 * (16 floats column-major, three.js layout; perspective or orthographic),
 * near < far the depth range the slices cover (a camera's near and far).
 * Rebuilds the cells' boxes when anything changed (O(cells)). */
void cw_froxel_grid_setup(cw_froxel_grid *g, const cw_froxel_config *c, int w, int h, const float proj[16], float near,
                          float far);
/* Bin n volumes (n <= 65535; more are dropped). Replaces the last result. */
void cw_froxel_grid_bin(cw_froxel_grid *g, const cw_froxel_volume *v, int n);

/* the grid: cells per axis and the slice mapping (slice = floor(log(depth) * zscale + zbias)) */
void cw_froxel_grid_dims(const cw_froxel_grid *g, int *nx, int *ny, int *nz, float *zscale, float *zbias);
/* view depth where slice k starts (k = nz: where the last ends) */
float cw_froxel_slice_depth(const cw_froxel_grid *g, int k);
/* the cell holding a view-space point, -1 outside the grid */
int cw_froxel_cell_of(const cw_froxel_grid *g, float x, float y, float z);
/* a cell's view-space bounding box */
void cw_froxel_cell_box(const cw_froxel_grid *g, int cell, float mn[3], float mx[3]);
/* the last bin: per-cell records ((first << 8) | count, nx * ny * nz of them) and the item list */
const uint32_t *cw_froxel_records(const cw_froxel_grid *g, int *cells);
const uint16_t *cw_froxel_items(const cw_froxel_grid *g, int *count);
void cw_froxel_grid_stats(const cw_froxel_grid *g, cw_froxel_stats *out);
/* FNV-1a over the records and items (tests: the same everywhere at any worker count) */
uint64_t cw_froxel_grid_hash(const cw_froxel_grid *g);

/* The last bin as R32UI DataTextures (t3_texture *; NearestFilter, made and
 * re-uploaded on demand: a new bin makes them upload on their next use),
 * owned by the grid. Their widths, which the shader needs, come back too. */
struct t3_texture;
void cw_froxel_grid_textures(cw_froxel_grid *g, struct t3_texture **records, int *records_width, struct t3_texture **items,
                             int *items_width);

#ifdef __cplusplus
}
#endif
#endif

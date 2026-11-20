/* Private structs shared by the 2D node layer (nodes2d.c, tilemap2d.c,
 * canvas.c). */
#ifndef CW_SCENE2D_INTERNAL_H
#define CW_SCENE2D_INTERNAL_H

#include "scene_internal.h"
#include "cartwheel/scene2d.h"

typedef struct cw__canvas_layer {
  int layer;
  cw_vec2 offset;
  float rotation;
  cw_vec2 scale;
  bool follow_viewport;
  bool visible;
  bool y_sort;
} cw__canvas_layer;

typedef struct cw__parallax_layer {
  cw_vec2 motion_scale;
  cw_vec2 motion_offset;
} cw__parallax_layer;

typedef struct cw__camera2d {
  bool current;
  float zoom;
  cw_vec2 offset;
  bool anchor_top_left;
  bool smoothing;
  float smoothing_speed;
  int limit_left, limit_top, limit_right, limit_bottom;
  bool rotating;
  /* runtime (fixed ticks) */
  bool started;
  cw_vec2 pos, prev_pos;
  float rot, prev_rot;
} cw__camera2d;

typedef struct cw__canvas_settings {
  int virtual_width, virtual_height;
  int scale_mode;
  float clear_color[4];
  float letterbox[4];
  float ambient[4];
  bool pixel_snap;
  bool y_sort;
  /* lit canvases: how deep light reaches into occluders */
  float occluder_depth;
  /* the post chain (cw_r2d_post) over the canvases that follow the camera; CanvasLayers in screen space draw after it */
  bool post;
  float exposure, bloom_threshold, bloom_intensity, contrast, saturation, vignette;
  bool bloom;
  int tone;
  /* global illumination (cw_r2d_gi) of the lit canvases */
  bool gi;
  float gi_intensity, gi_bounce;
  /* wind (cw_r2d_wind) for Sprite2Ds with sway */
  float wind_strength, wind_speed, wind_wavelength, wind_gusts;
} cw__canvas_settings;

typedef struct cw__light2d {
  bool enabled;
  float color[4];
  float energy, range, falloff, height;
  bool shadow;
  float softness, cone, cone_soft, volumetric;
  bool directional;
  const char *texture; /* a light texture (cw_light2d.texture), asset path */
} cw__light2d;

extern const cw_node_type *cw__node2d_t, *cw__parallax_t, *cw__camera2d_t, *cw__settings2d_t, *cw__light2d_t;

/* the camera hook: registers Camera2D nodes for the after-physics update */
void cw__canvas_camera_enter(cw_node n);
void cw__canvas_camera_exit(cw_node n);

/* shared texture / font caches (by interned path) */
cw_texture cw__tex_get(const char *path, int filter);
cw_font cw__font_get(const char *path, int mode, float bake_size);

static inline cw_color cw__col4(const float c[4]) { return (cw_color){ c[0], c[1], c[2], c[3] }; }
static inline cw_color cw__col_mul(cw_color a, cw_color b) { return (cw_color){ a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a }; }

/* points cached against the (interned) source string */
typedef struct cw__points {
  const char *src;
  CW_ARRAY(cw_vec2) pts;
} cw__points;
const cw_vec2 *cw__points_get(cw__points *p, const char *text, int *n);

/* ── TileMap2D for whoever builds from its cells (TileCollision2D, the navigation bake) ──────────────────
 * Shapes are integers, CW__TILE_SUB units to a tile pixel, so shared edges of neighbouring cells compare exactly. */
#define CW__TILE_SUB 16
#define CW__TILE_SHAPE_PTS 12
typedef struct cw__tile_shape {
  uint8_t kind;  /* 0 none, 1 a polygon wound with a negative shoelace sum (y down), 2 a one-way segment a -> b (a left of b) */
  uint8_t n;
  int32_t pt[CW__TILE_SHAPE_PTS][2]; /* inside the cell: 0..cell extent, flips applied */
} cw__tile_shape;
typedef struct cw__tile_edit {
  int32_t x, y, layer;
} cw__tile_edit;
typedef struct cw__tilemap_info {
  int w, h, layers;
  cw_vec2 tile_size, cell_size; /* cell_size already resolved (never 0) */
  int32_t ext_x, ext_y;         /* a cell's extent in shape units: tile size * CW__TILE_SUB */
  uint32_t serial, epoch;
  const char *tile_shapes;      /* interned: a change means every shape may have changed */
  int layout;                   /* cw_tile_layout */
} cw__tilemap_info;
/* Where cell (x, y)'s box starts, in shape units (what cw_tile_layout_cell_origin gives in pixels). */
static inline void cw__tile_cell_offset(const cw__tilemap_info *in, int x, int y, int32_t *ox, int32_t *oy) {
  switch (in->layout) {
  case CW_TILE_LAYOUT_ISOMETRIC: *ox = (x - y - 1) * (in->ext_x / 2), *oy = (x + y) * (in->ext_y / 2); break;
  case CW_TILE_LAYOUT_HEX_ROWS: *ox = x * in->ext_x + ((y & 1) ? in->ext_x / 2 : 0), *oy = y * (in->ext_y / 4 * 3); break;
  case CW_TILE_LAYOUT_HEX_COLUMNS: *ox = x * (in->ext_x / 4 * 3), *oy = y * in->ext_y + ((x & 1) ? in->ext_y / 2 : 0); break;
  default: *ox = x * in->ext_x, *oy = y * in->ext_y;
  }
}
bool cw__tilemap2d_info(cw_node n, cw__tilemap_info *out); /* false when n is not a TileMap2D */
/* The shape of the tile in a cell with the cell's flips applied; kind 0 when empty, outside or shapeless. */
void cw__tilemap2d_cell_shape(cw_node n, int layer, int x, int y, cw__tile_shape *out);
/* Cell edits with a serial in [since, serial): returns how many it wrote, or -1 when some have left the log
 * (the caller then treats every cell as changed). */
int cw__tilemap2d_edits_since(cw_node n, uint32_t since, cw__tile_edit *out, int cap);

/* terrains (tile_terrain2d.c): the parsed `terrains` text of a TileMap2D */
typedef struct cw__terrains cw__terrains;
cw__terrains *cw__terrains_parse(const char *text);
void cw__terrains_free(cw__terrains *t);
cw__terrains *cw__tilemap2d_terrains(cw_node n); /* NULL when n is not a TileMap2D or has none */

#endif

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
} cw__canvas_settings;

typedef struct cw__light2d {
  bool enabled;
  float color[4];
  float energy, range, falloff, height;
  bool shadow;
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

#endif

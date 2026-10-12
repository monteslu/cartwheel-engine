/* A three.c drawer for cw_phys_lines (physics debug draw): one
 * LineSegments object with per-vertex colors whose buffers are refilled
 * each frame. Header-only sample helper; capacity grows when needed. */
#ifndef CW_SAMPLE_PHYS_LINES_T3_H
#define CW_SAMPLE_PHYS_LINES_T3_H

#include <stdlib.h>
#include <string.h>

#include "cartwheel/physics.h"
#include "three.h"

typedef struct phys_lines_t3 {
  t3_mesh *obj;
  t3_geometry *geo;
  int cap; /* segments */
} phys_lines_t3;

static inline void phys_lines_t3_init(phys_lines_t3 *d, t3_object *parent) {
  memset(d, 0, sizeof *d);
  d->geo = t3_geometry_new();
  t3_material *m = t3_line_basic_material_new(0xffffff);
  m->vertex_colors = true;
  m->depth_test = false; /* drawn over the scene, like a debug overlay */
  m->transparent = true;
  d->obj = t3_line_segments_new(d->geo, m);
  T3_OBJ(d->obj)->frustum_culled = false;
  T3_OBJ(d->obj)->render_order = 1000;
  t3_object_add(parent, d->obj);
  t3_release(m);
}

static inline void phys_lines_t3_update(phys_lines_t3 *d, const cw_phys_lines *l) {
  int n = cw_phys_lines_count(l);
  if (n > d->cap || !d->geo->attributes[T3_ATTR_POSITION]) {
    d->cap = n > 64 ? n + n / 2 : 64;
    float *zero = calloc((size_t)d->cap * 6, sizeof(float));
    t3_attribute *pos = t3_attribute_new(T3_FLOAT32, zero, d->cap * 2, 3);
    t3_attribute *col = t3_attribute_new(T3_FLOAT32, zero, d->cap * 2, 3);
    pos->usage = col->usage = T3_DYNAMIC_DRAW;
    t3_geometry_set_attribute(d->geo, T3_ATTR_POSITION, pos);
    t3_geometry_set_attribute(d->geo, T3_ATTR_COLOR, col);
    t3_release(pos);
    t3_release(col);
    free(zero);
  }
  t3_attribute *pos = d->geo->attributes[T3_ATTR_POSITION], *col = d->geo->attributes[T3_ATTR_COLOR];
  memcpy(pos->array, l->pos.data, (size_t)n * 6 * sizeof(float));
  float *c = col->array;
  for (int i = 0; i < n; i++) {
    uint32_t h = l->color.data[i];
    float r = (float)((h >> 16) & 255) / 255.0f, g = (float)((h >> 8) & 255) / 255.0f, b = (float)(h & 255) / 255.0f;
    c[i * 6 + 0] = c[i * 6 + 3] = r;
    c[i * 6 + 1] = c[i * 6 + 4] = g;
    c[i * 6 + 2] = c[i * 6 + 5] = b;
  }
  pos->version++;
  col->version++;
  d->geo->draw_start = 0;
  d->geo->draw_count = n * 2;
  d->geo->version++;
}

#endif

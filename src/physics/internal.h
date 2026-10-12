/* Shared internals of the physics wrappers (not public). */
#ifndef CW_PHYSICS_INTERNAL_H
#define CW_PHYSICS_INTERNAL_H

#include <float.h>

#include "cartwheel/physics.h"
#include "cartwheel/physics_tasks.h"

/* Filter bits above the 32 layers (physics.h): every non-sensor shape
 * accepts QUERY, sensors accept SENSOR_QUERY. Queries carry QUERY, plus
 * SENSOR_QUERY when they want sensors. */
#define CW__QUERY_BIT (1ull << 63)
#define CW__SENSOR_QUERY_BIT (1ull << 62)

static inline uint64_t cw__shape_category(int layer) { return 1ull << (layer & 31); }
static inline uint64_t cw__shape_mask(uint32_t mask, bool sensor) {
  return (uint64_t)mask | (sensor ? CW__SENSOR_QUERY_BIT : CW__QUERY_BIT);
}
static inline uint64_t cw__query_category(const cw_phys_filter *f) {
  return CW__QUERY_BIT | (f && f->sensors ? CW__SENSOR_QUERY_BIT : 0);
}
static inline uint64_t cw__query_mask(const cw_phys_filter *f) { return f ? (uint64_t)f->layers : CW_PHYS_ALL_LAYERS; }

/* layer names (owned copies) */
typedef struct cw__layers {
  char *names[CW_PHYS_LAYERS];
} cw__layers;
void cw__layers_init(cw__layers *l, const char *const names[CW_PHYS_LAYERS]);
void cw__layers_free(cw__layers *l);
int cw__layers_find(const cw__layers *l, const char *name, size_t len);
uint32_t cw__layers_bits(const cw__layers *l, const char *names);

/* the running state hash */
static inline uint32_t cw__fold(uint32_t h, const void *p, size_t n) { return cw_hash32(p, n, h); }

/* sorted insertion of query hits: keeps the `cap` smallest (key, id) */
static inline bool cw__hit_less(float fa, uint64_t ia, float fb, uint64_t ib) {
  return fa < fb || (fa == fb && ia < ib);
}

/* circle/line helpers for 3D wireframes */
void cw__lines_circle(cw_phys_lines *l, const float c[3], const float ax[3], const float ay[3], float r, uint32_t color,
                      int segments);

#endif

#include <string.h>

#include "internal.h"

cw_phys_material cw_phys_material_default(void) {
  return (cw_phys_material){ .friction = 0.6f };
}

void cw_phys_lines_add(cw_phys_lines *l, const float a[3], const float b[3], uint32_t color) {
  cw_array_reserve(&l->pos, l->pos.count + 6);
  memcpy(l->pos.data + l->pos.count, a, 3 * sizeof(float));
  memcpy(l->pos.data + l->pos.count + 3, b, 3 * sizeof(float));
  l->pos.count += 6;
  cw_array_push(&l->color, color & 0xffffffu);
}

void cw_phys_lines_free(cw_phys_lines *l) {
  cw_array_free(&l->pos);
  cw_array_free(&l->color);
}

void cw__lines_circle(cw_phys_lines *l, const float c[3], const float ax[3], const float ay[3], float r, uint32_t color,
                      int segments) {
  /* rendering only: libm is allowed, but the table below needs none */
  float prev[3] = { c[0] + ax[0] * r, c[1] + ax[1] * r, c[2] + ax[2] * r };
  for (int i = 1; i <= segments; i++) {
    float s, co;
    cw_sincos(CW_TAU * (float)i / (float)segments, &s, &co);
    float p[3] = { c[0] + (ax[0] * co + ay[0] * s) * r, c[1] + (ax[1] * co + ay[1] * s) * r,
                   c[2] + (ax[2] * co + ay[2] * s) * r };
    cw_phys_lines_add(l, prev, p, color);
    memcpy(prev, p, sizeof p);
  }
}

void cw__layers_init(cw__layers *l, const char *const names[CW_PHYS_LAYERS]) {
  for (int i = 0; i < CW_PHYS_LAYERS; i++) l->names[i] = names[i] ? cw_strdup(names[i], CW_MEM_PHYSICS) : NULL;
}

void cw__layers_free(cw__layers *l) {
  for (int i = 0; i < CW_PHYS_LAYERS; i++) cw_free(l->names[i]);
  memset(l, 0, sizeof *l);
}

int cw__layers_find(const cw__layers *l, const char *name, size_t len) {
  for (int i = 0; i < CW_PHYS_LAYERS; i++)
    if (l->names[i] && strlen(l->names[i]) == len && !memcmp(l->names[i], name, len)) return i;
  return -1;
}

uint32_t cw__layers_bits(const cw__layers *l, const char *names) {
  uint32_t bits = 0;
  const char *p = names;
  while (p && *p) {
    while (*p == ' ' || *p == '|' || *p == ',') p++;
    const char *e = p;
    while (*e && *e != '|' && *e != ',' && *e != ' ') e++;
    if (e > p) {
      int i = cw__layers_find(l, p, (size_t)(e - p));
      if (i < 0) cw_error("physics: no layer named '%.*s' (name it in the world def's layers[])", (int)(e - p), p);
      else bits |= 1u << i;
    }
    p = e;
  }
  return bits;
}

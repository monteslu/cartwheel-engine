/* Navmesh debug draw (nav.h): polygon edges, walls, tile bounds and
 * obstacles as line segments in physics.h's list. */
#include <math.h>

#include "nav_internal.h"

typedef struct {
  cw_phys_lines *out;
  uint32_t flags;
} draw_ctx;

static void edge(const float a[3], const float b[3], bool boundary, void *u) {
  draw_ctx *d = u;
  /* raised a little so the lines sit over the floor they describe */
  const float A[3] = { a[0], a[1] + 0.03f, a[2] }, B[3] = { b[0], b[1] + 0.03f, b[2] };
  if (boundary && (d->flags & CW_NAV_DRAW_BOUNDARY)) cw_phys_lines_add(d->out, A, B, 0x00e0ff);
  else if (!boundary && (d->flags & CW_NAV_DRAW_POLYS)) cw_phys_lines_add(d->out, A, B, 0x2a6f8a);
}

static void tile(const float mn[3], const float mx[3], void *u) {
  draw_ctx *d = u;
  float y = mn[1];
  const float c[4][3] = { { mn[0], y, mn[2] }, { mx[0], y, mn[2] }, { mx[0], y, mx[2] }, { mn[0], y, mx[2] } };
  for (int i = 0; i < 4; i++) cw_phys_lines_add(d->out, c[i], c[(i + 1) & 3], 0x555555);
}

static void obstacle(int kind, const float a[3], const float b[3], float r, void *u) {
  draw_ctx *d = u;
  if (kind == 0) { /* cylinder: base and top rings, four uprights */
    static const float ring[12][2] = { { 1, 0 }, { 0.8660254f, 0.5f }, { 0.5f, 0.8660254f }, { 0, 1 },
                                       { -0.5f, 0.8660254f }, { -0.8660254f, 0.5f }, { -1, 0 },
                                       { -0.8660254f, -0.5f }, { -0.5f, -0.8660254f }, { 0, -1 },
                                       { 0.5f, -0.8660254f }, { 0.8660254f, -0.5f } };
    for (int k = 0; k < 12; k++) {
      const float *p0 = ring[k], *p1 = ring[(k + 1) % 12];
      for (int h = 0; h < 2; h++) {
        float y = h ? b[1] : a[1];
        const float A[3] = { a[0] + p0[0] * r, y, a[2] + p0[1] * r }, B[3] = { a[0] + p1[0] * r, y, a[2] + p1[1] * r };
        cw_phys_lines_add(d->out, A, B, 0xff5533);
      }
      if (k % 3 == 0) {
        const float A[3] = { a[0] + p0[0] * r, a[1], a[2] + p0[1] * r }, B[3] = { a[0] + p0[0] * r, b[1], a[2] + p0[1] * r };
        cw_phys_lines_add(d->out, A, B, 0xff5533);
      }
    }
    return;
  }
  /* box bounds: 12 edges */
  for (int i = 0; i < 8; i++)
    for (int axis = 0; axis < 3; axis++) {
      if (i & (1 << axis)) continue;
      int j = i | (1 << axis);
      const float A[3] = { (i & 1) ? b[0] : a[0], (i & 2) ? b[1] : a[1], (i & 4) ? b[2] : a[2] };
      const float B[3] = { (j & 1) ? b[0] : a[0], (j & 2) ? b[1] : a[1], (j & 4) ? b[2] : a[2] };
      cw_phys_lines_add(d->out, A, B, 0xff5533);
    }
}

/* A link: its arc in 8 segments (green when paths can use it, red when an end
 * missed the navmesh), a short upright tick at each end, and for a one-way
 * link two barbs at the end. */
static void link(const cw_nav_link *l, bool connected, void *u) {
  draw_ctx *d = u;
  uint32_t col = connected ? 0x44ff66 : 0xff3344;
  const float a[3] = { l->start.x, l->start.y, l->start.z }, b[3] = { l->end.x, l->end.y, l->end.z };
  float dx = b[0] - a[0], dz = b[2] - a[2];
  float len = sqrtf(dx * dx + dz * dz);
  float arc = l->arc > 0 ? l->arc : 0.25f * len + 0.2f;
  float prev[3] = { a[0], a[1], a[2] };
  for (int k = 1; k <= 8; k++) {
    float t = (float)k / 8;
    const float p[3] = { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t + 4 * arc * t * (1 - t), a[2] + (b[2] - a[2]) * t };
    cw_phys_lines_add(d->out, prev, p, col);
    prev[0] = p[0], prev[1] = p[1], prev[2] = p[2];
  }
  const float a2[3] = { a[0], a[1] + 0.4f, a[2] }, b2[3] = { b[0], b[1] + 0.4f, b[2] };
  cw_phys_lines_add(d->out, a, a2, col);
  cw_phys_lines_add(d->out, b, b2, col);
  if (!l->bidirectional && len > 0.01f) {
    float ux = dx / len * 0.3f, uz = dz / len * 0.3f;
    const float w1[3] = { b[0] - ux - uz, b[1] + 0.15f, b[2] - uz + ux }, w2[3] = { b[0] - ux + uz, b[1] + 0.15f, b[2] - uz - ux };
    cw_phys_lines_add(d->out, b, w1, col);
    cw_phys_lines_add(d->out, b, w2, col);
  }
}

void cw_nav_debug_draw(cw_navmesh nm, cw_phys_lines *out, uint32_t flags) {
  if (!out || !cw_navmesh_valid(nm)) return;
  draw_ctx d = { out, flags };
  if (flags & (CW_NAV_DRAW_POLYS | CW_NAV_DRAW_BOUNDARY)) cw__nav_each_edge(nm, edge, &d);
  if (flags & CW_NAV_DRAW_TILES) cw__nav_each_tile(nm, tile, &d);
  if (flags & CW_NAV_DRAW_OBSTACLES) cw__nav_each_obstacle(nm, obstacle, &d);
  if (flags & CW_NAV_DRAW_LINKS) cw__nav_each_link(nm, link, &d);
}

void cw_nav_debug_draw_path(cw_phys_lines *out, const t3_vec3 *pts, int count, uint32_t color) {
  if (!out || !pts) return;
  for (int i = 0; i + 1 < count; i++) {
    const float a[3] = { pts[i].x, pts[i].y, pts[i].z }, b[3] = { pts[i + 1].x, pts[i + 1].y, pts[i + 1].z };
    cw_phys_lines_add(out, a, b, color);
  }
}

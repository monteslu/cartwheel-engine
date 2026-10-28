/* Navigation internals shared by the navmesh shim (navmesh.cpp, C++ over
 * Recast/Detour) and the C parts (crowd, geometry, nodes, debug draw). */
#ifndef CW_NAV_INTERNAL_H
#define CW_NAV_INTERNAL_H

#include "cartwheel/nav.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t cw__poly; /* a Detour polygon ref (32-bit refs) */

/* Each call uses the calling pool thread's query object; safe from range
 * jobs while the navmesh does not change. Positions are float[3]. */
bool cw__nav_nearest_ref(cw_navmesh nm, const float p[3], const cw_nav_filter *f, cw__poly *ref, float out[3]);
/* An A* search of at most max_iters iterations (0: the node pool's limit);
 * returns the corridor length (0: failed). *partial: ends short of eref. */
int cw__nav_path_polys(cw_navmesh nm, cw__poly sref, const float spos[3], cw__poly eref, const float epos[3],
                       const cw_nav_filter *f, int max_iters, cw__poly *polys, int max, bool *partial);
/* The straight path from spos to epos along a corridor: up to max corners
 * (xyz each) with Detour's straight-path flags and the polygon each one
 * enters (flags, refs may be NULL); returns the count. */
int cw__nav_corners(cw_navmesh nm, const float spos[3], const float epos[3], const cw__poly *path, int npath,
                    float *corners, uint8_t *flags, cw__poly *refs, int max);
/* Crossing link polygon ref, entered from prev: its start and end (in the
 * direction of travel); returns the link id (0: not a link). */
uint32_t cw__nav_link_cross(cw_navmesh nm, cw__poly prev, cw__poly ref, float start[3], float end[3]);
/* The link table entry (defaults filled in); NULL for an unknown id. */
const cw_nav_link *cw__nav_link(cw_navmesh nm, uint32_t id);
void cw__nav_each_link(cw_navmesh nm, void (*fn)(const cw_nav_link *l, bool connected, void *ctx), void *ctx);
/* moveAlongSurface from (sref, s) toward e: the reached point, and the
 * polygons crossed (visited[0] = sref ... the end polygon); returns their count. */
int cw__nav_move_along(cw_navmesh nm, cw__poly sref, const float s[3], const float e[3], const cw_nav_filter *f,
                       float out[3], cw__poly *visited, int max);
/* A raycast along the surface from (sref, s) toward e: *t (FLT_MAX: reached
 * e) and the polygons it crossed; returns their count (0: failed). */
int cw__nav_raycast_polys(cw_navmesh nm, cw__poly sref, const float s[3], const float e[3], const cw_nav_filter *f,
                          float *t, cw__poly *polys, int max);
bool cw__nav_poly_height(cw_navmesh nm, cw__poly ref, const float p[3], float *h);
bool cw__nav_ref_ok(cw_navmesh nm, cw__poly ref, const cw_nav_filter *f);
/* The point on the polygon closest to p. */
void cw__nav_closest_on_poly(cw_navmesh nm, cw__poly ref, const float p[3], float out[3]);
/* Raises a version number whenever tiles change (obstacles, rebuilds): crowds re-check their corridors. */
uint32_t cw__nav_version(cw_navmesh nm);
bool cw__nav_bounds(cw_navmesh nm, float bmin[3], float bmax[3]);
/* Crowds attach to their navmesh so freeing it frees them. */
void cw__nav_attach_crowd(cw_navmesh nm, cw_crowd c);
void cw__nav_detach_crowd(cw_navmesh nm, cw_crowd c);
void cw__crowd_free_for_navmesh(cw_crowd c);

/* Debug-draw geometry: every polygon edge (a, b, and whether it is a
 * boundary), tile bounds, obstacles (shape kind, center, size). */
typedef void (*cw__nav_edge_fn)(const float a[3], const float b[3], bool boundary, void *ctx);
void cw__nav_each_edge(cw_navmesh nm, cw__nav_edge_fn fn, void *ctx);
void cw__nav_each_tile(cw_navmesh nm, void (*fn)(const float bmin[3], const float bmax[3], void *ctx), void *ctx);
void cw__nav_each_obstacle(cw_navmesh nm, void (*fn)(int kind, const float a[3], const float b[3], float r, void *ctx),
                           void *ctx);

/* A three.c geometry's triangles (positions, index), transformed. */
void cw__nav_geom_add_geometry(cw_nav_geom *g, const t3_geometry *geo, const t3_mat4 *xf, uint8_t area);
/* The nav node types (nodes.c; cw_scene_register_builtin_types calls it). */
void cw__register_nav_types(void);

double cw__nav_now_ms(void); /* wall time for stats only */

#ifdef __cplusplus
}
#endif
#endif

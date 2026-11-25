/* cartwheel hex grids: coordinates, neighbours, distance, rings, spirals, lines, rotation and reflection, rounding,
 * hex to 2D and 3D positions and back, and a map that keeps a value per hex.
 *
 * For boards made of hexagons (a tile-laying game, a hive of pieces, a strategy map). It is plain math over integer
 * coordinates, with no nodes and nothing drawn; nav2d.h keeps its own hex only for paths on 2D tile maps.
 *
 *   cw_hex_layout lay = { .orientation = CW_HEX_POINTY, .size = { 0.5f, 0.5f } };   // 0.5 m centre to corner
 *   cw_axial ring[6 * 2];
 *   int n = cw_hex_ring(cw_hex_make(0, 0), 2, ring, 12);                           // the 12 hexes two steps out
 *   t3_vec3 at = cw_hex_to_world(&lay, ring[0], 0.02f);                             // where a piece stands
 *   cw_axial under = cw_hex_from_world(&lay, hit_point);                               // which hex a pick hit
 *
 * ── Coordinates ───────────────────────────────────────────────────────
 * Axial (q, r); the cube form's third coordinate is s = -q - r (cw_hex_s), so q + r + s = 0 always. Distance is
 * (|dq| + |dr| + |ds|) / 2. Offset coordinates (a rectangular board's column and row, four conventions) convert with
 * cw_hex_to_offset / cw_hex_from_offset; every other call takes axial.
 *
 * Directions 0..5 start at +q and go counterclockwise as drawn with y down (the 2D canvas): 0 (+1, 0), 1 (+1, -1),
 * 2 (0, -1), 3 (-1, 0), 4 (-1, +1), 5 (0, +1). On screen (y down), with a pointy layout they are east, north-east,
 * north-west, west, south-west, south-east; with a flat layout south-east, north-east, north, north-west, south-west,
 * south. Any int is taken modulo 6, so dir + 1 is always the next one counterclockwise.
 *
 * ── Layout ────────────────────────────────────────────────────────────
 * A layout places hexes in a plane: `size` is the distance from a hex's centre to a corner (x and y apart, so a
 * squashed board works; equal for regular hexagons), `origin` where hex (0, 0)'s centre is. POINTY hexes have a
 * corner at the top (rows are straight, columns zigzag); FLAT hexes have an edge at the top. In 2D the plane is the
 * canvas (y down). In 3D the same layout lies on the ground: layout x is world +X and layout y is world +Z, at the
 * height the call gives, so a camera looking down -Z sees the board as the 2D layout draws it.
 *
 * Corner i is at angle 60 i degrees (pointy: 60 i + 30) from +x toward +y.
 *
 * ── Determinism ───────────────────────────────────────────────────────
 * Everything on integer coordinates is exact. The point conversions use + - * / and rounding only (no sin, cos or
 * sqrt at run time), so a position and the hex found for it are the same on every host. cw_hex_map_keys gives keys in
 * one order (by r, then q) whatever order they went in, and cw_hex_map_hash folds keys and values in that order, so a
 * board's hash agrees across hosts and replays. */
#ifndef CW_HEX_H
#define CW_HEX_H

#include "base.h"
#include "math.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A hex in axial coordinates; the cube form's third coordinate is s = -q - r (the type is not called cw_hex: that is
 * render2d.h's colour helper). */
typedef struct cw_axial {
  int q, r;
} cw_axial;
/* Fractional axial coordinates: a point between centres (cw_hex_from_point_frac, cw_hex_lerp), cw_hex_round's input. */
typedef struct cw_axialf {
  float q, r;
} cw_axialf;

typedef enum {
  CW_HEX_POINTY, /* a corner at the top: rows straight, columns zigzag */
  CW_HEX_FLAT,   /* an edge at the top: columns straight, rows zigzag */
} cw_hex_orientation;

typedef struct cw_hex_layout {
  cw_hex_orientation orientation;
  cw_vec2 size;   /* centre to corner, along x and along y (equal for regular hexagons); 0 is taken as 1 */
  cw_vec2 origin; /* hex (0, 0)'s centre */
} cw_hex_layout;

/* Offset coordinates for rectangular boards: which rows (pointy) or columns (flat) are pushed half a hex. */
typedef enum {
  CW_HEX_ODD_R,  /* pointy: odd rows pushed right */
  CW_HEX_EVEN_R, /* pointy: even rows pushed right */
  CW_HEX_ODD_Q,  /* flat: odd columns pushed down */
  CW_HEX_EVEN_Q, /* flat: even columns pushed down */
} cw_hex_offset_kind;

/* The three lines through a hex that a reflection mirrors across: the line where q stays the same, r, or s. */
typedef enum { CW_HEX_AXIS_Q, CW_HEX_AXIS_R, CW_HEX_AXIS_S } cw_hex_axis;

/* ── coordinates ────────────────────────────────────────────────────── */

static inline cw_axial cw_hex_make(int q, int r) { return (cw_axial){ q, r }; }
/* The cube form's third coordinate, -q - r. */
static inline int cw_hex_s(cw_axial h) { return -h.q - h.r; }
/* The same hex. */
static inline bool cw_hex_eq(cw_axial a, cw_axial b) { return a.q == b.q && a.r == b.r; }
/* a + b (a step added to a hex, or two steps added). */
static inline cw_axial cw_hex_add(cw_axial a, cw_axial b) { return (cw_axial){ a.q + b.q, a.r + b.r }; }
/* a - b (the step from b to a). */
static inline cw_axial cw_hex_sub(cw_axial a, cw_axial b) { return (cw_axial){ a.q - b.q, a.r - b.r }; }
/* h times k (k steps of a direction). */
static inline cw_axial cw_hex_scale(cw_axial h, int k) { return (cw_axial){ h.q * k, h.r * k }; }

/* The step to the neighbour in direction dir (any int, taken modulo 6; see the directions above). */
cw_axial cw_hex_direction(int dir);
cw_axial cw_hex_neighbor(cw_axial h, int dir); /* h + cw_hex_direction(dir) */
/* The six diagonal steps (two hexes away, between two directions): d 0 is between directions 0 and 1, and so on
 * (any int, modulo 6). Each is at distance 2. */
cw_axial cw_hex_diagonal(int d);
cw_axial cw_hex_diagonal_neighbor(cw_axial h, int d); /* h + cw_hex_diagonal(d) */

int cw_hex_length(cw_axial h);              /* steps from (0, 0) */
int cw_hex_distance(cw_axial a, cw_axial b);  /* steps from a to b */
/* The direction from `from` that points closest to `to` (0..5; on an exact tie between two, the lower); -1 when
 * they are the same hex. */
int cw_hex_direction_to(cw_axial from, cw_axial to);

/* Turn h about center by 60 degrees a step: + counterclockwise (direction d becomes d + 1), - clockwise; any int
 * (taken modulo 6). */
cw_axial cw_hex_rotate(cw_axial h, cw_axial center, int steps);
/* Mirror h across the line through center where the axis coordinate stays the same (CW_HEX_AXIS_Q keeps q and swaps
 * r and s about center). Mirroring twice gives h back. */
cw_axial cw_hex_reflect(cw_axial h, cw_axial center, cw_hex_axis axis);

/* The hex a fractional position is in (cube rounding: the nearest centre; a point exactly on an edge goes to one
 * side the same way on every host). */
cw_axial cw_hex_round(cw_axialf f);
cw_axialf cw_hex_lerp(cw_axial a, cw_axial b, float t); /* a + (b - a) t */

/* ── shapes (each returns how many hexes the whole shape has; at most cap are written, out may be NULL to count) ─ */

/* The hexes on the straight line from a to b, both ends included, each next to the one before: distance + 1. */
int cw_hex_line(cw_axial a, cw_axial b, cw_axial *out, int cap);
/* The ring `radius` steps from center: 6 radius hexes (1, the centre, for radius 0; 0 for a negative radius),
 * starting at center + radius * direction 4 and going counterclockwise. */
int cw_hex_ring(cw_axial center, int radius, cw_axial *out, int cap);
/* Every hex within radius of center, the centre first, then ring 1, ring 2 ... (each as cw_hex_ring): 1 + 3r(r+1). */
int cw_hex_spiral(cw_axial center, int radius, cw_axial *out, int cap);
/* How many hexes are within radius of a centre (cw_hex_spiral's count): 1 + 3r(r+1), 0 for a negative radius. */
static inline int cw_hex_area_count(int radius) { return radius < 0 ? 0 : 1 + 3 * radius * (radius + 1); }
/* The hexes reachable from start in at most `steps` moves to a neighbour, entering only hexes passable() allows
 * (start is always in, passable or not), nearest first (breadth first; within one distance, in the order found going
 * round directions 0..5). passable NULL: every hex. A piece's moves, a flood fill of a region. */
int cw_hex_reachable(cw_axial start, int steps, bool (*passable)(cw_axial h, void *user), void *user, cw_axial *out, int cap);

/* ── offset coordinates ──────────────────────────────────────────────── */

/* A hex's column and row in an offset convention (col or row may be NULL). */
void cw_hex_to_offset(cw_axial h, cw_hex_offset_kind kind, int *col, int *row);
/* The hex at a column and row of an offset convention (cw_hex_to_offset's inverse). */
cw_axial cw_hex_from_offset(int col, int row, cw_hex_offset_kind kind);

/* ── positions ──────────────────────────────────────────────────────── */

cw_vec2 cw_hex_to_point(const cw_hex_layout *l, cw_axial h);         /* the hex's centre in the layout's plane */
cw_axialf cw_hex_from_point_frac(const cw_hex_layout *l, cw_vec2 p); /* where p is, in fractional hexes */
cw_axial cw_hex_from_point(const cw_hex_layout *l, cw_vec2 p);       /* the hex p is in */
cw_vec2 cw_hex_corner(const cw_hex_layout *l, cw_axial h, int corner); /* corner 0..5 (any int, modulo 6) */
void cw_hex_corners(const cw_hex_layout *l, cw_axial h, cw_vec2 out[6]); /* all six corners, 0..5 (an outline for cw_r2d_polygon) */
/* 3D: the layout on the ground plane (layout x = world X, layout y = world Z) at height y. */
t3_vec3 cw_hex_to_world(const cw_hex_layout *l, cw_axial h, float y);
cw_axial cw_hex_from_world(const cw_hex_layout *l, t3_vec3 p); /* the hex under p (p.y is ignored) */
cw_axialf cw_hex_from_world_frac(const cw_hex_layout *l, t3_vec3 p); /* where p is on the board, in fractional hexes */
t3_vec3 cw_hex_world_corner(const cw_hex_layout *l, cw_axial h, int corner, float y); /* corner 0..5 on the ground at height y */

/* ── a map of hexes ─────────────────────────────────────────────────────
 * A value of value_size bytes for each hex in it, any shape (a hexagonal board, a growing tile map, scattered
 * pieces). Values are copied in and live in the map: a pointer from get or set is good until the next set of a hex
 * not yet in the map, remove, clear or free. Lookups are a hash probe. Main thread or one owner at a time. */
typedef struct cw_hex_map cw_hex_map;

cw_hex_map *cw_hex_map_new(size_t value_size); /* value_size 0 makes a set of hexes */
void cw_hex_map_free(cw_hex_map *m);           /* NULL is a no-op */
/* Put a value at h (inserting h, or replacing its value): value NULL stores zeros. Returns the stored value. */
void *cw_hex_map_set(cw_hex_map *m, cw_axial h, const void *value);
void *cw_hex_map_get(const cw_hex_map *m, cw_axial h); /* the value at h; NULL when h is not in the map */
bool cw_hex_map_has(const cw_hex_map *m, cw_axial h); /* h is in the map */
bool cw_hex_map_remove(cw_hex_map *m, cw_axial h); /* false when h was not in it */
int cw_hex_map_count(const cw_hex_map *m);          /* hexes in the map (0 for NULL) */
void cw_hex_map_clear(cw_hex_map *m);               /* empty it (keeps its memory for refilling) */
size_t cw_hex_map_value_size(const cw_hex_map *m);  /* the value_size it was made with */
/* Every hex in the map sorted by r, then q (the same order on every host, whatever order they went in): returns the
 * count, writes at most cap (out may be NULL to count). */
int cw_hex_map_keys(const cw_hex_map *m, cw_axial *out, int cap);
/* A hash of every key and value in cw_hex_map_keys order, chained from seed (CW_HASH32_INIT to start): a board's
 * state for a test or a replay check. */
uint32_t cw_hex_map_hash(const cw_hex_map *m, uint32_t seed);
#define CW_HEX_MAP_GET(m, h, type) ((type *)cw_hex_map_get((m), (h)))

#ifdef __cplusplus
}
#endif
#endif

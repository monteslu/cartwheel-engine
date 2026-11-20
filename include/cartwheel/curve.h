/* Curves and paths: a Bezier curve you can walk by distance, and the nodes that put one in a scene.
 *
 * ── cw_curve ─────────────────────────────────────────────────────────
 * A curve is a list of points, each with an `in` and an `out` handle (offsets from the point; zero handles make
 * straight segments) and a tilt (a roll about the direction of travel, radians). Between two points it is a
 * cubic Bezier: p0, p0 + out0, p1 + in1, p1. A closed curve adds the segment from the last point back to the
 * first. One type serves 2D and 3D: a 2D curve keeps z at 0 (the *2d calls).
 *
 * Walking it by distance needs the curve BAKED: cut into short straight pieces with the distance to each. That
 * happens on the first query after a change (cw_curve_length, cw_curve_sample_baked, cw_curve_closest_offset,
 * ...) and costs about one Bezier evaluation per bake_interval of length; queries after it are a binary search.
 * Each baked point also carries a frame (forward, up) carried along the curve without twisting about it (the
 * double reflection method), so a loop or a corkscrew has a sensible "up" everywhere; a closed curve's frames
 * meet at the seam. Tilt rolls that frame.
 *
 * Determinism: everything here is float + - * / and sqrt plus cartwheel/math.h (cw_sincos, cw_atan2), in a fixed
 * order, so baking and sampling give the same bits natively and in a cart. Simulation may depend on it.
 *
 * Threads: a curve is not thread safe (a query may bake); use it from one thread at a time.
 *
 * ── text form ────────────────────────────────────────────────────────
 * Points separated by commas, numbers by spaces. 3D: "x y z", "x y z  ix iy iz  ox oy oz" or the same plus a
 * tilt in radians. 2D: "x y" or "x y  ix iy  ox oy". The Path nodes' `points` property is this text.
 *
 * ── nodes ────────────────────────────────────────────────────────────
 *   Path3D (Node3D), Path2D (Node2D)
 *     points         the curve as text (above), in the node's local space
 *     closed         join the last point to the first
 *     bake_interval  the length of a baked piece: 0.2 (3D, meters), 5 (2D, pixels)
 *     up             Path3D: the reference up the first frame is built from (0, 1, 0)
 *     length         read-only
 *     signal curve_changed()
 *   PathFollow3D (Node3D), PathFollow2D (Node2D): a child of a Path that sits ON the curve. Setting progress
 *   (or anything below) moves the node at once: its position, and its rotation by the rotation mode, in the
 *   Path's space.
 *     progress       distance along the curve
 *     progress_ratio the same as 0..1 of the length (not saved: progress is)
 *     loop           true: progress wraps around the length; false: it stops at the ends
 *     speed          units a second added to progress every fixed tick (0 = the game moves it). The node is
 *                    drawn smoothly between ticks.
 *     h_offset, v_offset   3D: sideways (the frame's right) and up; 2D: along the curve and across it
 *     rotation_mode  PathFollow3D: NONE (keep the node's rotation), Y (turn to face along the curve, level),
 *                    XY (also pitch up and down it), XYZ (also roll by the curve's tilt), ORIENTED (the
 *                    curve's own carried frame with its tilt: right through loops and corkscrews)
 *     tilt_enabled   PathFollow3D: apply the curve's tilt (XYZ and ORIENTED)
 *     use_model_front  PathFollow3D: the node's +Z faces along the curve instead of its -Z
 *     rotates        PathFollow2D: turn the node along the curve
 *     signal end_reached()   a follower with loop off and a speed arrived at an end (once per arrival)
 * A follower under a node that is not a Path does nothing. */
#ifndef CW_CURVE_H
#define CW_CURVE_H

#include "math.h"
#include "scene.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_curve cw_curve;

typedef struct cw_curve_point {
  t3_vec3 position;
  t3_vec3 in, out; /* handles, relative to position */
  float tilt;      /* radians about the direction of travel */
} cw_curve_point;

/* A place on a baked curve: forward is the direction of travel, up and right complete the frame (unit length,
 * perpendicular; right = forward x up). tilt is the interpolated tilt, already applied to up and right when the
 * sample asked for it. */
typedef struct cw_curve_frame {
  t3_vec3 position, forward, up, right;
  float tilt;
} cw_curve_frame;

cw_curve *cw_curve_new(void); /* empty, open, bake_interval 0.2, up (0, 1, 0); free with cw_curve_free */
void cw_curve_free(cw_curve *c); /* NULL is a no-op */
void cw_curve_clear(cw_curve *c); /* drops every point; settings stay */
int cw_curve_point_count(const cw_curve *c); /* points in the curve */
/* Insert a point before index (index < 0 or >= count appends). */
void cw_curve_add_point(cw_curve *c, t3_vec3 position, t3_vec3 in, t3_vec3 out, int index);
void cw_curve_remove_point(cw_curve *c, int index); /* out of range is ignored */
cw_curve_point cw_curve_get_point(const cw_curve *c, int index); /* zeros when out of range */
void cw_curve_set_point(cw_curve *c, int index, cw_curve_point p); /* out of range is ignored */
void cw_curve_set_closed(cw_curve *c, bool closed); /* join the last point back to the first */
bool cw_curve_closed(const cw_curve *c); /* as set (false by default) */
/* The length of a baked piece (clamped to at least 0.0001). Smaller follows tight bends closer and costs more
 * memory: 16 bytes times length / interval, about. */
void cw_curve_set_bake_interval(cw_curve *c, float interval);
float cw_curve_bake_interval(const cw_curve *c); /* as set (0.2 by default) */
/* The reference up the first frame is made from (made perpendicular to the first direction). */
void cw_curve_set_up(cw_curve *c, t3_vec3 up);
/* Counts every change to the curve (points, closed, interval, up): compare to know a curve moved. */
uint32_t cw_curve_version(const cw_curve *c);

/* The Bezier itself, not baked: segment in [0, point_count - 1) (one more when closed), t in [0, 1]. Zero when
 * the curve has no point; the point when it has one. */
t3_vec3 cw_curve_sample(const cw_curve *c, int segment, float t);

/* Baked queries (the first one after a change bakes). offset is a distance from the start, clamped to
 * [0, length]. */
float cw_curve_length(cw_curve *c);
/* The point at that distance along the curve; zero for an empty curve. */
t3_vec3 cw_curve_sample_baked(cw_curve *c, float offset);
/* Position and frame at offset; apply_tilt rolls up and right by the tilt there. */
cw_curve_frame cw_curve_sample_frame(cw_curve *c, float offset, bool apply_tilt);
/* The offset of the point on the curve nearest to p, and that point. O(baked pieces). */
float cw_curve_closest_offset(cw_curve *c, t3_vec3 p);
/* see cw_curve_closest_offset */
t3_vec3 cw_curve_closest_point(cw_curve *c, t3_vec3 p);
/* The baked polyline: positions and the distance to each (for drawing a path or building a mesh along it).
 * Returns the count; the arrays live until the next change to the curve. Either pointer may be NULL. */
int cw_curve_baked(cw_curve *c, const t3_vec3 **positions, const float **offsets);
/* A hash of the baked curve (positions, distances, frames): equal on every host for the same points. */
uint32_t cw_curve_hash(cw_curve *c);

/* Text form (see the top). dims is 2 or 3. parse replaces the points; false (points cleared, logged) on text
 * that is not numbers or has a wrong count per point. format follows the snprintf contract: the full length is
 * returned, at most cap - 1 bytes + NUL written. */
bool cw_curve_parse(cw_curve *c, const char *text, int dims);
/* see cw_curve_parse; whole numbers are written as digits, others as the shortest text that reads back the same */
int cw_curve_format(const cw_curve *c, char *buf, int cap, int dims);

/* 2D forms of the same calls (z = 0). */
void cw_curve_add_point2d(cw_curve *c, cw_vec2 position, cw_vec2 in, cw_vec2 out, int index);
/* cw_curve_sample_baked without z */
cw_vec2 cw_curve_sample_baked2d(cw_curve *c, float offset);
/* Position and unit direction of travel at offset (direction (1, 0) for a curve with no length). */
void cw_curve_sample_dir2d(cw_curve *c, float offset, cw_vec2 *position, cw_vec2 *direction);
/* cw_curve_closest_offset for a 2D point */
float cw_curve_closest_offset2d(cw_curve *c, cw_vec2 p);

/* ── the nodes ──────────────────────────────────────────────────────── */

/* The curve a Path2D or Path3D owns (NULL for other nodes). After changing it through this pointer call
 * cw_path_curve_changed, which rewrites the node's `points` text, moves its followers and emits curve_changed. */
cw_curve *cw_path_curve(cw_node path);
/* see cw_path_curve; no-op for a node that is not a Path */
void cw_path_curve_changed(cw_node path);
/* Move a PathFollow2D / PathFollow3D to its progress now (it does so by itself whenever one of its properties
 * or its Path's curve changes; call this after moving it under another Path by hand). */
void cw_path_follow_update(cw_node follow);

#ifdef __cplusplus
}
#endif
#endif

/* Water: a wavy surface that reflects, refracts and flows, and things that
 * float on it. A port of three.js r186's Water / Water2 / WaterMesh add-ons
 * (planar reflection and refraction rendered from mirrored and clipped
 * cameras, scrolling normal maps, flow maps) with Gerstner waves (GPU Gems
 * 1, chapter 1) moving the surface itself.
 *
 * ── node types ─────────────────────────────────────────────────────────
 *   Water3D      (Node3D) a rectangle of water of `size` (x by z metres)
 *                in the node's XZ plane, its rest level at the node's y,
 *                facing +Y. Properties:
 *     size, subdivisions   the grid the waves move (quads along the longer
 *                          side; quality scales it: Low 1/4, Medium 1/2)
 *     wave_count, wave0..wave3   up to four Gerstner waves, each a vec4:
 *                          direction (degrees, 0 = +X, 90 = +Z), wavelength
 *                          (m), amplitude (m), steepness (0..1: 0 round
 *                          swells, 1 sharp crests; the waves' steepness is
 *                          shared so crests never loop over)
 *     wave_speed, time_offset   multiplies deep-water speed (sqrt(g / k));
 *                          seconds added to the waves' clock
 *     color                tint of what is seen through the water (linear)
 *     deep_color           the water body's colour where no refraction is
 *                          drawn (Low, Medium)
 *     murk                 share of the refraction lost into deep_color
 *                          (0.7; 0 = clear as glass)
 *     detail, roughness    the normal map ripples' strength; the surface
 *                          roughness the lights' glints see
 *     reflectivity         Fresnel F0 (0.02, water)
 *     distortion           how far the normal maps bend reflection and
 *                          refraction
 *     normal_map           a tangent-space normal map image (empty: a
 *                          seamless gen://noise normal map), scrolled twice
 *                          along the flow (Water2's two-phase flow)
 *     normal_scale         normal map repeats per metre
 *     flow_direction       vec2, the flow without a flow map
 *     flow_speed           how fast the normal maps scroll (Water2's 0.03)
 *     flow_map             optional image: RG = the flow direction at each
 *                          point (Water2's flowMap, 0.5 = still), over the
 *                          whole rectangle
 *     reflection, refraction   draw them (quality may turn them down or off)
 *     resolution_scale     of the reflection and refraction targets (0.5)
 *     debug_view           NONE, or HEIGHT: the surface's height as colour
 *                          (tests read it back: R, G = height's 16 bits
 *                          over debug_range, B = 1)
 *     debug_range          vec2 (min, max) heights for debug_view HEIGHT
 *
 *     floor_map_size       cells along the longer side of the baked floor depth
 *                          map (128; 0 = no map, no shore shading). The map is
 *                          made once, from the tree's static physics shapes: a
 *                          static shape holding the point at the rest level is
 *                          land (depth 0); else the highest static hit of a ray
 *                          down from the rest level is the floor, a miss is deep
 *     floor_range          metres under the rest level the rays probe (40)
 *     shallow_color        colour of the water over a shallow floor (linear);
 *                          it blends to deep_color as the depth grows
 *     clarity              metres of water over which the shallow colour has
 *                          become deep_color (5)
 *     shore_foam           width in metres of the foam where the water meets
 *                          the floor (0.7; 0 = none)
 *     crest_foam           0..1: foam on the wave crests where the surface
 *                          bunches up (0.35; 0 = none)
 *
 *   Buoyancy3D   (Node3D) child of a RigidBody3D: pushes it up where its
 *                probes are under the water and drags it where they are
 *                wet. Properties:
 *     water                the Water3D (empty = the tree's first)
 *     probes               "x y z, x y z, ..." in the body's space (empty:
 *                          the eight corners of its first BOX shape, or a
 *                          sphere's / capsule's bounding box, at 90%)
 *     volume               m^3 the body displaces when fully under (0: the
 *                          box's volume); shared out equally over probes
 *     probe_depth          metres over which a probe goes from dry to fully
 *                          under (0: a quarter of the box's height)
 *     fluid_density        1000 (kg/m^3)
 *     linear_drag          N per m/s per fully wet probe, against the probe
 *                          point's velocity relative to the water's
 *     angular_drag         N*m per rad/s, scaled by the wet share
 *     submerged            read-only: the wet share of the probes, 0..1
 *
 * ── time and determinism ────────────────────────────────────────────────
 * The waves run on the tree's fixed clock: their time is cw_tree_tick x the
 * fixed dt (plus time_offset), never a wall clock. Every wave phase is
 * reduced into [0, 2 pi) in double precision (fmod is exact) and the sums
 * use cw_sin / cw_cos, so cw_water_height_at and Buoyancy3D are the same
 * bits on every host. The GPU evaluates the same sums from the same reduced
 * phases (native sin / cos, so to float rounding) at the grid's vertices;
 * between vertices the drawn surface is flat, so it is within the grid's
 * sag of cw_water_height_at (a few millimetres at the default grid).
 *
 * Drawing renders the scene twice more before the main pass, as r186's
 * Reflector and Refractor do: mirrored about the rest level (an oblique
 * near plane clips what is below it) and clipped to below it, each at
 * resolution_scale, with the water hidden (Low draws neither: the
 * environment's specular stands in for the reflection, deep_color for the
 * refraction; Medium draws the reflection only). Costs: the water guide;
 * on the 890M the water_boat frame is 1.0 ms (Low) to 2.1 ms (Ultra) of GPU
 * time on GL at 1080p, a height query 0.25 us on the CPU.
 */
#ifndef CW_WATER_H
#define CW_WATER_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_WATER_MAX_WAVES 4

/* One Gerstner wave (what Water3D's wave0..wave3 hold). */
typedef struct cw_water_wave {
  float direction_deg; /* 0 = +X, 90 = +Z */
  float wavelength;    /* metres, > 0 */
  float amplitude;     /* metres */
  float steepness;     /* 0..1 */
} cw_water_wave;

/* A set of waves, the pure-function form of a Water3D's (local space: x, z
 * on the rest plane, y up from the rest level). */
typedef struct cw_waves {
  int count;
  cw_water_wave wave[CW_WATER_MAX_WAVES];
  float speed; /* multiplies the deep-water speed; 1 */
} cw_waves;

/* Where the surface point that rests at (x0, z0) is at time t: out[0..2] =
 * its displaced local position (x, height, z). out_normal (may be NULL):
 * the surface's unit normal there. Deterministic (cw_sin / cw_cos, phases
 * reduced in double). */
void cw_waves_displace(const cw_waves *w, float x0, float z0, double t, float out[3], float out_normal[3]);
/* The surface's height above the rest level over the local point (x, z) at
 * time t. Gerstner waves move points sideways too, so this finds the rest
 * point that lands on (x, z) first (four fixed-point steps, converged to
 * well under a millimetre for steepness below 1). Deterministic. */
float cw_waves_height(const cw_waves *w, float x, float z, double t);

/* What the water is doing at a world point. */
typedef struct cw_water_sample {
  float height;      /* world y of the surface over (x, z) */
  t3_vec3 normal;    /* world, unit */
  t3_vec3 velocity;  /* the water's own velocity there (m/s, world): the wave orbit */
} cw_water_sample;

/* The tree's first Water3D (null when it has none). O(1) after the first call per tree. */
cw_node cw_water_find(cw_tree *t);
/* The time the waves of a Water3D are at now, in seconds: its tree's fixed
 * ticks x the fixed dt + time_offset. 0 for a null node. */
double cw_water_time(cw_node water);
/* The surface's world height over world (x, z) at time t (cw_water_time
 * for now). Outside the rectangle the waves continue (open sea). The rest
 * level when water is not a Water3D. Deterministic. */
float cw_water_height_at(cw_node water, float x, float z, double t);
/* Height, normal and water velocity at world (x, z), time t. False (out
 * zeroed but for a flat normal) when water is not a Water3D. Deterministic. */
bool cw_water_sample_at(cw_node water, float x, float z, double t, cw_water_sample *out);
/* The Water3D's waves, as cw_waves (for cw_waves_* in its local space). */
bool cw_water_get_waves(cw_node water, cw_waves *out);

/* The water's baked floor: metres of water over the floor at world (x, z)
 * at rest (0 where the floor is land or above the rest level; a large value
 * where the map has no floor or the point is outside it). Deterministic: it
 * comes from physics ray casts, never from the GPU. 1e6 for a null or
 * unbaked water. */
float cw_water_floor_depth_at(cw_node water, float x, float z);
/* Bake the floor map again (the static shapes changed). Done on the next
 * frame. */
void cw_water_rebake_floor(cw_node water);

/* Last frame's numbers for a tree's water. */
typedef struct cw_water_stats {
  int surfaces;           /* Water3D nodes drawn */
  int reflection_w, reflection_h; /* reflection target (0 when off) */
  int refraction_w, refraction_h; /* refraction target (0 when off) */
  int triangles;          /* of the surfaces' grids */
  int buoyant;            /* Buoyancy3D nodes that pushed last tick */
} cw_water_stats;
/* Zeroed when t is NULL or has no water. */
void cw_water_get_stats(cw_tree *t, cw_water_stats *out);

#ifdef __cplusplus
}
#endif
#endif

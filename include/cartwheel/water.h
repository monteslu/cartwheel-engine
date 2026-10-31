/* Water: a wavy surface that reflects, refracts and flows, and things that
 * float on it. A port of three.js r187's Water / Water2 / WaterMesh add-ons
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
 *     grid                 RECT (the default) or VIEW: rings around the camera out
 *                          to horizon_distance, subdivisions quads around each
 *                          ring, grid_min_cell metres at the least between them;
 *                          for water much larger than its detail. Each wave fades
 *                          out past five to eight wavelengths from the camera,
 *                          where the rings can no longer draw it
 *     wave_shelter         vec4, how much cw_water_set_wave_mask holds each wave down
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
 *     swell_reflect        how far the swell's own tilt moves the reflection (0)
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
 *     foam_scale           how much finer the foam's pattern is (1)
 *     foam_lace            how much of that pattern cuts holes in the foam (1.4)
 *     foam_spread          m/s a wake's foam spreads as it fades (0)
 *
 *     scatter, glint       the turquoise light the sun sends through crests
 *                          toward the eye (needs a ProceduralSky); the fine
 *                          sparkle of the sun on small ripples
 *     ripple_cells, ripple_size, ripple_center, ripple_speed, ripple_damping,
 *     ripple_strength, foam_fade, rain, rain_size, rain_strength, splash_fx,
 *     splash_fx_min        the interactive ripple layer (see cw_water_splash)
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
 *     splash, wake         how much of a splash entering the water and of a
 *                          wake moving through it the body makes (1; 0: none)
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
 * Drawing renders the scene twice more before the main pass, as r187's
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

/* A map of how free the waves are (0..1, nx x nz floats, row z, col x, over
 * the node's rectangle, clamped at its edge): each wave's height there is
 * multiplied by 1 + (mask - 1) * wave_shelter[i]. So the same swell can be
 * full height on the open sea and gone inside a harbour, with the local chop
 * untouched. The drawn surface and cw_water_height_at / cw_water_sample_at /
 * Buoyancy3D read the same bilinear map: deterministic, and they agree. Keep
 * it smooth: the normal ignores its slope. The map is copied. NULL removes it. */
void cw_water_set_wave_mask(cw_node water, const float *mask, int nx, int nz);

/* The water's baked floor: metres of water over the floor at world (x, z)
 * at rest (0 where the floor is land or above the rest level; a large value
 * where the map has no floor or the point is outside it). Deterministic: it
 * comes from physics ray casts, never from the GPU. 1e6 for a null or
 * unbaked water. */
float cw_water_floor_depth_at(cw_node water, float x, float z);
/* Bake the floor map again (the static shapes changed). Done on the next
 * frame. */
void cw_water_rebake_floor(cw_node water);

/* ── interactive ripples ──────────────────────────────────────────────────
 * Water3D keeps a height field of small ripples (ripple_cells, ripple_size,
 * ripple_center) that is drawn as a bent normal and foam. A splash, a wake or
 * rain writes into it, it spreads and fades on the fixed clock. All + - * and
 * integer hashes in a fixed order: the same bits on every host. It is a look,
 * not a gameplay height: bodies float on the Gerstner waves alone. */
/* A round dent of "depth" metres at the centre and "radius" metres wide at
 * world (x, z), with "foam" (0..1) left on it; it spreads out as a ring. A
 * deep enough one (splash_fx_min) also restarts the next particle emitter of
 * the splash_fx pool at the surface. No-op without a ripple grid. */
void cw_water_splash(cw_node water, float x, float z, float radius, float depth, float foam);
/* A body moving through the water at (vx, vz): call once per fixed tick; it
 * leaves a trough and foam along its path. */
void cw_water_wake(cw_node water, float x, float z, float vx, float vz, float radius, float depth, float foam);
/* Keep the ripple grid around world (x, z): the grid and what is on it move by
 * whole cells so the ripples stay where they are in the world, and cells
 * coming in are flat. Call it from fixed_update with simulation values, so the
 * field stays the same bits on every host. This is how a small, sharp grid
 * covers a course much larger than itself. */
void cw_water_ripple_follow(cw_node water, float x, float z);
/* The ripple height in metres over world (x, z) (0 outside the grid). */
float cw_water_ripple_at(cw_node water, float x, float z);
/* A hash of the ripple field (heights and foam): the cross-host test. 0 without a grid. */
uint32_t cw_water_ripple_hash(cw_node water);

/* Last frame's numbers for a tree's water. */
typedef struct cw_water_stats {
  int surfaces;           /* Water3D nodes drawn */
  int reflection_w, reflection_h; /* reflection target (0 when off) */
  int refraction_w, refraction_h; /* refraction target (0 when off) */
  int triangles;          /* of the surfaces' grids */
  int buoyant;            /* Buoyancy3D nodes that pushed last tick */
  int underwater;         /* 1 when the last frame was drawn from under a Water3D's surface (its fog, caustics, waterline) */
} cw_water_stats;
/* Zeroed when t is NULL or has no water. */
void cw_water_get_stats(cw_tree *t, cw_water_stats *out);

#ifdef __cplusplus
}
#endif
#endif

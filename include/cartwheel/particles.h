/* Particles and trails: GPU particles in 3D (transform feedback), CPU
 * particles in 2D (on the render2d batcher), ribbon trails.
 *
 * ── node types (properties: docs/nodes.json, the debug channel's `props`) ──
 *
 *   GPUParticles3D       (Node3D) `amount` particles simulated on the GPU by
 *                        transform feedback: per particle position, age,
 *                        velocity and its spawn cycle, ping-ponged between two
 *                        buffer pairs every fixed tick. Emission is a pure
 *                        function of the particle index and sim time (Godot's
 *                        model): particle i spawns at
 *                          start + i / amount * lifetime * (1 - explosiveness)
 *                        and again every `lifetime` seconds while emitting, with
 *                        its random values hashed from (seed, i, cycle) by an
 *                        integer hash. No clock is read: the sim advances by the
 *                        tree's fixed step only, so the same seed and the same
 *                        number of ticks give the same particles on every host.
 *     emission           emission_shape POINT / SPHERE / SPHERE_SURFACE / BOX /
 *                        RING / MESH, emission_extents (box half sizes),
 *                        emission_radius (+ emission_inner_radius for RING),
 *                        emission_mesh (a MeshInstance3D node: 4096 points spread
 *                        over its surface by area, with normals, in that node's
 *                        space at spawn time), emission_normal_speed (extra speed
 *                        along the surface normal, SPHERE_SURFACE and MESH)
 *     motion             direction (local), spread (cone half angle, degrees),
 *                        speed_min / speed_max, gravity (world), damping,
 *                        turbulence (strength), turbulence_scale (noise
 *                        frequency), turbulence_speed (noise scroll): a curl
 *                        noise field from a 32^3 tiling 3D texture
 *                        ParticleAttractor3D nodes anywhere in the tree pull
 *                        (strength > 0) or push every system (up to 8)
 *     collision          collide_depth: bounce off the scene's depth buffer
 *                        (screen space: only what the camera sees, surfaces
 *                        thinner than depth_thickness are solid, normals rebuilt
 *                        from depth); ParticleCollider3D nodes (PLANE, SPHERE,
 *                        BOX; up to 8) are analytic; bounce, friction,
 *                        collision_radius
 *     look               draw_mode BILLBOARD / STRETCHED (along velocity,
 *                        stretch x speed) / MESH (mesh_primitive BOX, SPHERE or
 *                        a mesh_node's mesh, drawn instanced); size, size_random,
 *                        color (tint), energy (x colour: HDR glow),
 *                        color_ramp and size_curve (over life, strings below),
 *                        texture (asset path; default a soft dot), hframes /
 *                        vframes (flipbook played over life), spin (rad/s,
 *                        random sign), blend ADD / ALPHA / OPAQUE, soft_distance
 *                        (fade where particles meet geometry), sort (ALPHA:
 *                        back to front with a bitonic sort in fragment passes)
 *     trails             trail_length (history samples, 0 = off, max 64): each
 *                        particle draws a camera-facing ribbon through its last
 *                        positions (one sample per fixed tick, kept in a float
 *                        texture ring on the GPU), trail_width (tapering to 0)
 *     lifetime           emitting, amount, lifetime, lifetime_random,
 *                        explosiveness, one_shot (emits one cycle, then
 *                        emitting turns off), preprocess (seconds simulated when
 *                        emission starts), speed_scale, seed, local_coords
 *                        (particles follow the emitter)
 *     stats              alive (read-only: particles alive now, computed on the
 *                        CPU from the same emission function), gpu_stats (read
 *                        the GPU's own count and position sum back every frame:
 *                        a stall, for tests), gpu_alive, gpu_sum (read-only)
 *   ParticleAttractor3D  (Node3D) strength (acceleration at the centre; < 0
 *                        repels), radius, attenuation (falloff exponent)
 *   ParticleCollider3D   (Node3D) shape PLANE (the node's XZ plane, normal +Y,
 *                        extents.xz bounds it; 0 = infinite), SPHERE (radius),
 *                        BOX (extents = half sizes, particles stay outside)
 *   GPUParticles2D       (Node2D) the same emission model on the CPU (threaded
 *                        on the job pool), drawn as sprites through render2d:
 *                        amount, lifetime, lifetime_random, explosiveness,
 *                        one_shot, emitting, seed, emission_shape (POINT,
 *                        CIRCLE, RECT), emission_radius, emission_extents,
 *                        direction (degrees, 0 = +x), spread, speed_min/max,
 *                        gravity, damping, size, size_random, color,
 *                        color_ramp, size_curve, texture, blend (ALPHA, ADD),
 *                        spin, local_coords, alive (read-only)
 *   Trail3D              (Node3D) a ribbon that follows the node: the last
 *                        `length` positions (one per fixed tick) as a camera
 *                        facing strip of `width`, color_ramp along its length
 *                        (0 = head), blend
 *
 * Curves are strings of "t value" stops (t in 0..1, linear between stops):
 *   size_curve  "0 0.2, 0.15 1, 1 0"
 *   color_ramp  "0 1 0.8 0.3 0, 0.1 1 0.6 0.2 1, 1 0.4 0.1 0.1 0" (t r g b a)
 * An empty string is constant 1 (white, full size).
 *
 * Rendering. Every GPUParticles3D (and Trail3D) of a tree draws in the
 * pipeline's "particles" EFFECTS pass (order 200): after the opaque scene and
 * the sky, depth tested against the scene, unlit (the colour is the light:
 * the scene is HDR, so energy > 1 blooms). The pass reads CW_RES_DEPTH (soft
 * particles and depth collision sample ctx->depth_tex), so a tree with
 * particles renders offscreen. The simulation runs in the same pass, one
 * transform feedback draw per pending fixed tick (no draw happens while the
 * tree is paused). Costs: one TF draw per system per tick (amount vertices),
 * one instanced draw per system per frame (plus one for trails); sorting adds
 * log2(n)(log2(n)+1)/2 full-target passes over a 2^k texel key texture;
 * gpu_stats adds a scatter, a reduction and a 1-pixel readback (a stall).
 * On WebGPU (cartwheel/gpu.h) the simulation, the sort and the gpu_stats
 * reduction are compute passes and the draws read the particle buffers
 * directly; gpu_alive / gpu_sum arrive asynchronously, never as a stall: a
 * frame or more after the state they describe (between frames in carts).
 * Same seed, same alive counts on both backends; position sums agree to
 * float rounding.
 * GPU capability: float colour targets (RGBA32F) for sort, trails and
 * gpu_stats (every host cartwheel supports has them; their absence is logged
 * and those features are refused, never silently swapped).
 *
 * GPUParticles2D simulates in fixed_update (CPU, deterministic: cw_rng-free
 * integer hashes, the deterministic math of cartwheel/math.h) and draws in
 * the 2D pass as one cw_r2d_sprites batch. */
#ifndef CW_PARTICLES_H
#define CW_PARTICLES_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Restart emission of a GPUParticles3D or GPUParticles2D now: every particle
 * dies and the emission window starts at the current sim time (emitting is
 * set). */
void cw_particles_restart(cw_node n);
/* Particles alive now (GPUParticles3D / GPUParticles2D); the same number as
 * the `alive` property. */
int cw_particles_alive(cw_node n);
/* A GPUParticles3D with bursts > 0: fire its next burst slot (the oldest
 * reused) once at world position at, its particles' direction dir (zero:
 * the system's direction). The slot's amount / bursts particles spawn as a
 * one-shot of that system would (the same lifetime, explosiveness, shape,
 * speeds, forces and look), each firing with a new seed. A pool of one-shot
 * effects becomes one system and one draw, where a system each cost a
 * simulation pass and a draw whenever any was alive. No-op for another node
 * or bursts 0. Render only. */
void cw_particles_burst(cw_node n, t3_vec3 at, t3_vec3 dir);

/* Tree totals, for profilers and debug fields. */
typedef struct cw_particles_stats {
  int systems;        /* GPUParticles3D nodes in the tree */
  int alive;          /* sum of their `alive` */
  int capacity;       /* sum of their `amount` */
  int sim_steps;      /* transform feedback steps run last frame */
  int draw_calls;     /* particle + trail draws last frame */
  int sort_passes;    /* bitonic passes last frame */
} cw_particles_stats;
/* last frame's counts; zeroed when the tree has no particles or t is NULL */
void cw_particles_get_stats(cw_tree *t, cw_particles_stats *out);

/* The deterministic per-particle hash the GPU uses (a 32-bit integer
 * finalizer): exposed so tests and tools reproduce emission on the CPU. */
static inline uint32_t cw_particle_hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

#ifdef __cplusplus
}
#endif
#endif

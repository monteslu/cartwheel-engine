/* cartwheel noise: coherent random patterns for clouds, terrain, water,
 * fog density and textures. FastNoiseLite 1.1.1 (MIT, Jordan Peck) wrapped
 * as cw_noise_*, on the CPU (C) and on the GPU (GLSL and WGSL ports).
 *
 *   cw_noise n = cw_noise_default();      // OpenSimplex2, frequency 0.01
 *   n.fractal = CW_NOISE_FRACTAL_FBM;
 *   n.octaves = 5;
 *   float h = cw_noise_2d(&n, x, z);       // about -1 .. 1
 *
 * Patterns (cw_noise_type): OpenSimplex2 (smooth, the general default),
 * OpenSimplex2S (smoother, a little slower), Cellular (Voronoi cells,
 * cracks, scales, stones), Perlin, ValueCubic and Value (blocky, cheap).
 * Fractals stack octaves: FBM (clouds, rolling terrain), Ridged (mountain
 * ridges, lightning, veins), PingPong (terraces, wood rings). Domain warp
 * bends the input coordinates by another noise first (swirls, marble,
 * eroded-looking terrain): cw_noise_warp_2d / _3d.
 *
 * Determinism. The C code uses only + - * /, integer hashing and float /
 * int conversions (no libm at all, its square root included: FastNoiseLite
 * carries its own), so a given cw_noise and input return the same bits on
 * every host, native and wasm, under the engine's -ffp-contract=off.
 * Simulation code may use it (terrain heights, spawn maps, buoyancy). The
 * GPU ports follow the C code line for line; GPUs round differently (fused
 * multiply-add, division precision), so they agree with the CPU to about
 * 1e-4, not bit for bit: use the CPU result for anything the game state
 * depends on and the GPU one for pixels.
 *
 * Costs (one core of the 890M machine, Release): one octave of 2D noise
 * is 25 ns (Perlin) to 110 ns (OpenSimplex2) a sample, 3D 90 to 160 ns,
 * cellular 190 ns (2D) and 600 ns (3D); fractals multiply by the octave
 * count. cw_noise_fill_2d spreads rows over the job pool.
 *
 * Threading: every function is pure and thread safe. */
#ifndef CW_NOISE_H
#define CW_NOISE_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CW_NOISE_OPENSIMPLEX2,
  CW_NOISE_OPENSIMPLEX2S,
  CW_NOISE_CELLULAR,
  CW_NOISE_PERLIN,
  CW_NOISE_VALUE_CUBIC,
  CW_NOISE_VALUE,
  CW_NOISE_TYPE_COUNT
} cw_noise_type;

/* 3D only: rotates the lattice so 2D slices (XY or XZ planes) look better.
 * XZ suits terrain sampled at (x, height, z). */
typedef enum { CW_NOISE_ROTATION_NONE, CW_NOISE_ROTATION_XY, CW_NOISE_ROTATION_XZ } cw_noise_rotation;

typedef enum {
  CW_NOISE_FRACTAL_NONE,
  CW_NOISE_FRACTAL_FBM,
  CW_NOISE_FRACTAL_RIDGED,
  CW_NOISE_FRACTAL_PINGPONG,
  CW_NOISE_FRACTAL_WARP_PROGRESSIVE, /* domain warp only: each octave warps the result of the last */
  CW_NOISE_FRACTAL_WARP_INDEPENDENT, /* domain warp only: every octave warps the input */
  CW_NOISE_FRACTAL_COUNT
} cw_noise_fractal;

typedef enum {
  CW_NOISE_DIST_EUCLIDEAN,
  CW_NOISE_DIST_EUCLIDEAN_SQ,
  CW_NOISE_DIST_MANHATTAN,
  CW_NOISE_DIST_HYBRID,
  CW_NOISE_DIST_COUNT
} cw_noise_cellular_distance;

/* what a cellular sample returns: the nearest cell's random value, the
 * distance to the nearest point (distance), to the second nearest
 * (distance2), or a mix of the two (cracks between cells: DISTANCE2_SUB) */
typedef enum {
  CW_NOISE_CELL_VALUE,
  CW_NOISE_CELL_DISTANCE,
  CW_NOISE_CELL_DISTANCE2,
  CW_NOISE_CELL_DISTANCE2_ADD,
  CW_NOISE_CELL_DISTANCE2_SUB,
  CW_NOISE_CELL_DISTANCE2_MUL,
  CW_NOISE_CELL_DISTANCE2_DIV,
  CW_NOISE_CELL_RETURN_COUNT
} cw_noise_cellular_return;

typedef enum {
  CW_NOISE_WARP_NONE,
  CW_NOISE_WARP_OPENSIMPLEX2,
  CW_NOISE_WARP_OPENSIMPLEX2_REDUCED,
  CW_NOISE_WARP_BASIC_GRID,
  CW_NOISE_WARP_COUNT
} cw_noise_warp;

/* Every setting, FastNoiseLite's, with its default (cw_noise_default). A
 * plain value: copy it, keep it in a component, hash it. */
typedef struct cw_noise {
  int32_t seed;                       /* 1337 */
  float frequency;                    /* 0.01: features per input unit (1 / feature size) */
  cw_noise_type type;                 /* OPENSIMPLEX2 */
  cw_noise_rotation rotation;         /* NONE (3D only) */
  cw_noise_fractal fractal;           /* NONE */
  int32_t octaves;                    /* 3 (fractals) */
  float lacunarity;                   /* 2: frequency multiplier per octave */
  float gain;                         /* 0.5: amplitude multiplier per octave */
  float weighted_strength;            /* 0: 0..1, octaves weighted by the one below (FBM, Ridged) */
  float ping_pong_strength;           /* 2 */
  cw_noise_cellular_distance cellular_distance; /* EUCLIDEAN_SQ */
  cw_noise_cellular_return cellular_return;     /* DISTANCE */
  float cellular_jitter;              /* 1: 0 = a regular grid of points */
  cw_noise_warp warp;                 /* OPENSIMPLEX2: what cw_noise_warp_* use (NONE: no warp) */
  float warp_amp;                     /* 30: warp distance in input units */
} cw_noise;

/* FastNoiseLite's defaults (above). */
cw_noise cw_noise_default(void);

/* One 2D sample, about -1 .. 1 (cellular distances can leave that range; the
 * texture generator below remaps). */
float cw_noise_2d(const cw_noise *n, float x, float y);
/* One 3D sample, about -1 .. 1 (as cw_noise_2d). */
float cw_noise_3d(const cw_noise *n, float x, float y, float z);

/* Domain warp: move (x, y) by n.warp scaled by n.warp_amp, in place, then
 * sample another (or the same) cw_noise there. A warp fractal
 * (WARP_PROGRESSIVE / WARP_INDEPENDENT) runs `octaves` warps. Does nothing
 * when n.warp is NONE. */
void cw_noise_warp_2d(const cw_noise *n, float *x, float *y);
/* The 3D domain warp: (x, y, z) in place, as cw_noise_warp_2d. */
void cw_noise_warp_3d(const cw_noise *n, float *x, float *y, float *z);

/* A grid of samples: out[row * w + col] = noise at (x0 + col * step,
 * y0 + row * step), after warp->warp when warp is non-NULL (a warp of its
 * own settings, as FastNoiseLite's demo pairs two states). Rows are spread
 * over the job pool (cw_parallel_for); every value is the same as the
 * single-sample call, so the result does not depend on the worker count. */
void cw_noise_fill_2d(const cw_noise *n, const cw_noise *warp, float *out, int w, int h, float x0, float y0,
                      float step);
/* The 3D form: a slice at depth z (out is w x h), x/y as above. */
void cw_noise_fill_3d_slice(const cw_noise *n, const cw_noise *warp, float *out, int w, int h, float x0, float y0,
                            float z, float step);

/* A FNV-1a 64 hash of a grid of samples' bits (for tests and caches). */
uint64_t cw_noise_hash(const float *v, size_t count);

/* ── names (resource paths, scene files, the debug channel) ─────────── */
/* "opensimplex2", "opensimplex2s", "cellular", "perlin", "value_cubic", "value" */
const char *cw_noise_type_name(cw_noise_type t);
/* -1 when unknown */
int cw_noise_type_from_name(const char *s);

/* ── generated textures: the gen://noise resource ─────────────────────
 * cw_res_load("gen://noise?type=perlin&fractal=fbm&octaves=5&size=256",
 * CW_RES_TEXTURE) makes a texture of noise (cartwheel/resource.h); the path
 * is its cache key, so the same path loads once and is shared. It is
 * generated on a worker like an image decode (cw_res_load_async works), is
 * linear data (not sRGB), RGBA8, repeat-wrapped and mipmapped. Keys (any
 * order, all optional):
 *
 *   size=256 | 512x256     texels (1..4096)
 *   type=opensimplex2      the cw_noise_type name
 *   seed=1337  freq=0.01   frequency in features per TEXEL (0.01: about 100 texels a feature)
 *   fractal=none|fbm|ridged|pingpong   octaves=3 lacunarity=2 gain=0.5
 *   weighted=0 pingpong=2  weighted strength, ping-pong strength
 *   rotation=none|xy|xz    (with z=)
 *   cell_dist=euclidean|euclidean_sq|manhattan|hybrid
 *   cell_return=value|distance|distance2|distance2_add|distance2_sub|distance2_mul|distance2_div
 *   jitter=1
 *   warp=none|opensimplex2|opensimplex2_reduced|basic_grid  warp_amp=30 (texels)
 *   warp_freq=0.005 warp_seed=(seed + 100) warp_fractal=none|progressive|independent warp_octaves=3
 *   x=0 y=0                offset in texels;  z=   a slice of 3D noise at this depth
 *   seamless=1             the texture tiles: four shifted copies blended by distance
 *                          to the edges (the middle loses a little contrast)
 *   out=gray               r = g = b = the noise, a = 255
 *       rgba               four layers, seed, seed + 1, seed + 2, seed + 3, one per channel
 *       normal             a tangent-space normal map of the noise as a height field
 *                          (strength=2: height units per texel), b up, a = the height
 *   min=-1 max=1           the noise range mapped to 0 .. 255 (clamped)
 *
 * Example: clouds "gen://noise?size=512&fractal=fbm&octaves=6&freq=0.004&seamless=1",
 * water ripples "gen://noise?size=256&type=perlin&fractal=fbm&octaves=4&freq=0.03&out=normal&seamless=1",
 * stones "gen://noise?type=cellular&cell_return=distance2_sub&freq=0.03".
 *
 * Generate the same pixels into memory (RGBA8, w x h x 4) without the GPU.
 * Returns false (logged, naming the key) on a bad query; *w / *h get the
 * size. The bytes are cw_alloc'd (CW_MEM_RESOURCE): cw_free them. */
bool cw_noise_image(const char *query, uint8_t **rgba, int *w, int *h);

/* ── the GPU ports ────────────────────────────────────────────────────
 * Shader source to paste in front of your own (cw_pass_program,
 * cw_pass_program_wgsl). It defines, in GLSL ES 3.00 and in WGSL:
 *
 *   float cw_noise2(vec2 p, CwNoise n)       fn cw_noise2(p: vec2f, n: CwNoise) -> f32
 *   float cw_noise3(vec3 p, CwNoise n)       fn cw_noise3(p: vec3f, n: CwNoise) -> f32
 *   CwNoise cw_noise_make(int seed, float frequency, int type, int fractal, int octaves)
 *                                            fn cw_noise_make(...) -> CwNoise
 *   CwNoise cw_noise_unpack(vec4 a, vec4 b, vec4 c, vec4 d)
 *                                            fn cw_noise_unpack(...) -> CwNoise
 *
 * CwNoise is a struct of the cw_noise fields above with enums as int / i32,
 * in this order: seed, frequency, type (named ntype in WGSL, where type is
 * reserved), rotation, fractal, octaves, lacunarity, gain,
 * weighted_strength, ping_pong_strength, cellular_distance,
 * cellular_return, cellular_jitter. cw_noise_make fills the rest with the
 * defaults; cw_noise_unpack takes four vec4 uniforms filled from a
 * cw_noise by cw_noise_gpu_params. Every pattern and fractal of
 * cw_noise_2d / _3d is there; domain warp is CPU only. Names starting with
 * cwn_ / CWN_ are the port's own. The tables (gradients, cell offsets) make
 * each source about 55 KB; a driver compiles it once per program, and
 * unused functions cost nothing at run time. GPU cost on the 890M: 0.18 ms
 * a million pixels for one octave of OpenSimplex2, 0.8 ms for five (the
 * noise guide has the table). */
const char *cw_noise_glsl(void);
/* The WGSL port (as cw_noise_glsl). */
const char *cw_noise_wgsl(void);
/* n as the 16 floats cw_noise_unpack takes (four vec4: out[0..3], [4..7],
 * [8..11], [12..15]), the CwNoise fields in order, ints as exact floats,
 * out[13..15] zero. */
void cw_noise_gpu_params(const cw_noise *n, float out[16]);

#ifdef __cplusplus
}
#endif
#endif

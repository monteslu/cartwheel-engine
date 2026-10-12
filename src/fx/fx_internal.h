/* Effects module internals: particles (3D GPU, 2D CPU), trails, decals.
 * Shared GL helpers live in fx_gl.c. */
#ifndef CW_FX_INTERNAL_H
#define CW_FX_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/gl.h"
#include "cartwheel/particles.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/scene.h"

/* ── GL helpers (fx_gl.c) ─────────────────────────────────────────── */

/* Build a program from GLSL ES 3.00 bodies (the version line and highp
 * precision for float, int and every sampler type are prepended). varyings:
 * transform feedback outputs captured to separate buffers (NULL = none).
 * Returns 0 and logs the compiler output on failure. */
unsigned cw__fx_program(const char *name, const char *vs, const char *fs, const char *const *varyings, int nvaryings);
/* Uniform location, cached per (program, name). name must be a string
 * literal or otherwise live for the program's life. */
int cw__fx_loc(unsigned prog, const char *name);
void cw__fx_u1f(unsigned prog, const char *name, float v);
void cw__fx_u1i(unsigned prog, const char *name, int v);
void cw__fx_u1ui(unsigned prog, const char *name, unsigned v);
void cw__fx_u2f(unsigned prog, const char *name, float x, float y);
void cw__fx_u3f(unsigned prog, const char *name, float x, float y, float z);
void cw__fx_u4f(unsigned prog, const char *name, float x, float y, float z, float w);
void cw__fx_u4fv(unsigned prog, const char *name, const float *v, int count);
void cw__fx_mat4(unsigned prog, const char *name, const float *m16);
/* textures */
unsigned cw__fx_tex2d(int w, int h, unsigned ifmt, unsigned fmt, unsigned type, bool linear, const void *data);
void cw__fx_bind_tex(int unit, unsigned target, unsigned tex);
/* an FBO over 1 or 2 colour textures (and an optional depth texture) */
unsigned cw__fx_fbo(unsigned tex0, unsigned tex1, unsigned depth);
/* whether RGBA32F is colour-renderable here (probed once, needs GL) */
bool cw__fx_float32_ok(void);
/* A 32^3 RGBA8 tiling curl-noise volume (rgb = vector * 0.5 + 0.5),
 * generated once from a fixed seed. */
unsigned cw__fx_noise3d(void);
/* its texels (32^3 RGBA8, x fastest), shared by the WebGPU path */
const uint8_t *cw__fx_noise3d_rgba(void);
/* A 64x64 soft round dot (RGBA8, premultiplied white). */
unsigned cw__fx_dot_texture(void);
const uint8_t *cw__fx_dot_rgba(void); /* its 64x64 texels */
/* An RGBA8 texture from an image asset (straight alpha), cached by path.
 * 0 when missing (logged once). */
unsigned cw__fx_texture_asset(const char *path, int *w, int *h);

/* Curves: "t v [v v v], ..." with `comps` values per stop, baked into n
 * samples (linear between stops, clamped outside). Empty / NULL = constant
 * `def` for every component. Returns false (and fills with def) on a parse
 * error, logging `what`. */
bool cw__fx_curve_bake(const char *text, int comps, float def, int n, float *out, const char *what);

/* this run draws with WebGPU (false in builds without it) */
bool cw__fx_wgpu_on(void);

/* ── per-tree registry ────────────────────────────────────────────── */

typedef struct cw__psys cw__psys;
typedef struct cw__trail cw__trail;
typedef struct cw__decal_sys cw__decal_sys;

typedef struct cw__fx_tree {
  cw_tree *tree;
  CW_ARRAY(cw_node) systems;    /* GPUParticles3D, in enter order */
  CW_ARRAY(cw_node) attractors; /* ParticleAttractor3D */
  CW_ARRAY(cw_node) colliders;  /* ParticleCollider3D */
  CW_ARRAY(cw_node) trails;     /* Trail3D */
  bool pass_added;
  cw_particles_stats stats, last_stats;
} cw__fx_tree;

cw__fx_tree *cw__fx_tree_of(cw_tree *t);
void cw__fx_remove_node(void *arr, cw_node n); /* from one of the CW_ARRAY(cw_node) above */
void cw__fx_ensure_pass(cw_tree *t);

/* particles3d.c / trail3d.c, called from the pass (fx_pass.c) */
void cw__psys_frame(cw_node n, cw_pass_ctx *ctx, cw__fx_tree *ft);
void cw__trail3d_draw(cw_node n, cw_pass_ctx *ctx, cw__fx_tree *ft);

/* the shared quad strip (4 corners, -1..1) and the ribbon programs */
unsigned cw__fx_quad_vbo(void);
/* a VAO with no attributes (draws that read only gl_VertexID / gl_InstanceID) */
unsigned cw__fx_empty_vao(void);

/* tests: decals + particles + trails on a caller-built context */
void cw__fx_test_exec(cw_pass_ctx *ctx);

void cw__register_particles3d_types(void);
void cw__register_particles2d_types(void);
void cw__register_trail3d_types(void);
void cw__register_decal_types(void);

#endif

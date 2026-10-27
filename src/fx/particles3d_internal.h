/* GPUParticles3D internals shared by the GL path (particles3d.c) and the
 * WebGPU path (particles3d_wgpu.c). */
#ifndef CW_PARTICLES3D_INTERNAL_H
#define CW_PARTICLES3D_INTERNAL_H

#include "cartwheel/render3d.h"
#include "fx_internal.h"
#if CW_WGPU
#include "fx_wgpu.h"
#endif

enum { SHAPE_POINT, SHAPE_SPHERE, SHAPE_SPHERE_SURFACE, SHAPE_BOX, SHAPE_RING, SHAPE_MESH, SHAPE_PARENT };
enum { DRAW_BILLBOARD, DRAW_STRETCHED, DRAW_MESH };
enum { MP_BOX, MP_SPHERE, MP_TETRA, MP_NODE };
enum { BLEND_ADD, BLEND_ALPHA, BLEND_OPAQUE, BLEND_DISTORT, BLEND_REFRACT };

#define CURVE_N 256
#define EMIT_W 64
#define EMIT_N (EMIT_W * EMIT_W)
#define MAX_STEPS 256
#define MAX_ATTR 8
#define MAX_COLL 8
#define TRAIL_MAX 64

typedef struct step_rec {
  float t0, t1;   /* sim time relative to the emission start */
  t3_mat4 m0, m1; /* the spawn frame (emitter or emission mesh) at t0 and t1 */
} step_rec;

struct cw__psys {
  void *wg; /* the WebGPU runtime (particles3d_wgpu.c), NULL on GL */
  int cap; /* allocated particles */
  GLuint buf[2][2];
  GLuint sim_vao[2], draw_vao;
  int cur;
  bool need_reset;
  bool gl_ready;
  /* emission window, sim seconds */
  double sim_time, emit_start, emit_end; /* emit_end < 0 = open */
  bool was_emitting, has_emitted; /* prewarm can make a valid emit_start negative */
  t3_mat4 last_m;
  bool have_last_m;
  step_rec *steps;
  int nsteps;
  /* the emission window is over and every particle of it is dead (p3d_fixed):
   * nothing to simulate or draw until the next window */
  bool idle;
  /* curves */
  GLuint curve_tex;
  const char *baked_ramp, *baked_size;
  /* emission mesh points */
  GLuint emit_tex;
  float *emit_px; /* the CPU points (cw__psys_emit_points) */
  uint64_t emit_built_node;
  const void *emit_built_geo;
  /* mesh drawn per particle */
  GLuint mesh_vbo, mesh_ibo;
  int mesh_count;
  int mesh_built_kind;
  const void *mesh_built_geo;
  /* state textures (sort, trails, gpu stats) */
  int tw, trows;
  GLuint state_tex[2], state_fbo;
  /* sort */
  int sort_n, sw, sh;
  GLuint key_tex[3], key_fbo[3]; /* the keys, ping-pong; [2] the ranks (a small sort) */
  int sorted; /* which key texture holds the result */
  /* trails */
  int hist_len;
  GLuint hist_tex, hist_fbo;
  int hist_head, hist_count;
  /* gpu stats reduction targets */
  GLuint red_tex[4], red_fbo[4];
  int red_w[4], red_h[4], red_levels;
};

typedef struct p3d {
  bool emitting;
  int amount;
  float lifetime, lifetime_random, explosiveness;
  bool one_shot;
  float preprocess, speed_scale;
  int seed;
  bool local_coords;
  int emission_shape;
  t3_vec3 emission_extents;
  float emission_radius, emission_inner_radius;
  const char *emission_mesh; /* node path, relative to the emitter */
  float emission_normal_speed;
  const char *parent;        /* PARENT: node path of the GPUParticles3D whose particles this one spawns from */
  float inherit_velocity;    /* PARENT: share of the parent particle's velocity the child starts with */
  t3_vec3 direction;
  float spread, speed_min, speed_max;
  t3_vec3 gravity;
  float damping, turbulence, turbulence_scale, turbulence_speed;
  bool collide_depth;
  float depth_thickness, bounce, friction, collision_radius;
  int draw_mode, mesh_primitive;
  const char *mesh_node;     /* node path */
  float size, size_random, stretch;
  float color[4];
  float energy;
  const char *color_ramp, *size_curve, *texture;
  int hframes, vframes;
  float spin;
  int blend;
  float soft_distance;
  float distort_strength; /* DISTORT / REFRACT: how far the picture is shifted (world units at the particle) */
  const char *motion_texture; /* flipbook: motion vector sheet (red, green = motion to the next frame), frames are blended along it */
  float motion_strength;        /* its scale (1 = as the baker wrote it) */
  bool sort;
  int trail_length;
  float trail_width;
  /* stats */
  int alive;
  bool gpu_stats;
  int gpu_alive;
  t3_vec3 gpu_sum;
  cw__psys *rt;
} p3d;

/* the runtime of a GPUParticles3D node (NULL for any other node) */
cw__psys *cw__psys_of(cw_node n);
/* the GPUParticles3D data of a node (NULL for any other node) */
p3d *cw__psys_data(cw_node n);
/* particles3d.c helpers the WebGPU path uses */
/* the tree's attractors (pos xyz, radius, strength, attenuation, 0, 0 each)
 * and colliders (inverse, world matrix, shape params), as the shaders read them */
int cw__psys_attractors(const cw__fx_tree *ft, float attr[MAX_ATTR * 8]);
int cw__psys_colliders(const cw__fx_tree *ft, float cinv[MAX_COLL * 16], float cm[MAX_COLL * 16], float cp[MAX_COLL * 4]);
void cw__psys_spawn_frame(cw_node n, const p3d *d, t3_mat4 *out);
t3_geometry *cw__psys_node_geometry(cw_node n, t3_mat4 *local);
cw_node cw__psys_ref_node(cw_node self, const char *path);
/* the emission points of emission_shape MESH (EMIT_N positions then EMIT_N
 * normals, xyzw), rebuilt when the node or its geometry changed; NULL when
 * there is no usable mesh. *changed is set when they were (re)built. */
const float *cw__psys_emit_points(cw_node self, p3d *d, cw__psys *r, bool *changed);
/* the per-particle mesh of draw_mode MESH, (re)built when it changed: GL
 * buffers, or cw__psys_mesh_upload_wgpu on WebGPU; r->mesh_count = indices
 * (0 = none) */
void cw__psys_ensure_mesh(cw_node self, p3d *d, cw__psys *r);

#if CW_WGPU
/* a system's frame on WebGPU: the compute work and copies on *cp (the
 * pass's shared compute pass, opened and ended as needed; true = it has
 * draws), then its draws into the pass's render pass */
bool cw__psys_frame_wgpu(cw_node n, p3d *d, cw_pass_ctx *ctx, cw__fx_tree *ft, WGPUComputePassEncoder *cp);
void cw__psys_draw_wgpu(WGPURenderPassEncoder rp, cw_node n, p3d *d, cw_pass_ctx *ctx, cw__fx_tree *ft, const cw__wgpu_pass_targets *tg);
void cw__psys_free_wgpu(cw__psys *r);
void cw__psys_reset_wgpu(cw__psys *r);
/* pos xyz + normal xyz per vertex, u32 indices */
/* tests */
WGPUBuffer cw__psys_wgpu_keys(cw_node n, int *count, int *amount);
void cw__fx_wgpu_test_fault(int fault);
void cw__psys_mesh_upload_wgpu(cw__psys *r, const float *v, int nv, const uint32_t *idx, int ni);
#endif

#endif

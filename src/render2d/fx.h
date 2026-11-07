/* render2d effects layer: offscreen targets of any format and full-screen
 * passes, the same calls on GL and WebGPU. The HDR canvas, the post chain
 * (bloom, tone mapping, grading), the distance field, the light buffers and
 * global illumination are built from these.
 *
 * Orientation: every fx target keeps GL's row order on both backends (row 0
 * = the bottom), and a pass's v_uv is (0, 0) at the bottom left of its
 * destination, so a GLSL pass and its WGSL twin read and write the same
 * texels. */
#ifndef CW_R2D_FX_H
#define CW_R2D_FX_H

#include "internal.h"

typedef enum { FX_RGBA8, FX_RGBA16F, FX_FMT_COUNT } fx_fmt;

typedef struct fx_rt {
  int w, h;
  fx_fmt fmt;
  GLuint tex, fbo; /* GL */
  void *wtex;      /* WebGPU: WGPUTexture */
} fx_rt;

/* where a pass writes: an fx target, or the canvas the pass ends in */
typedef struct fx_dst {
  int w, h;
  GLuint fbo;        /* GL */
  void *wview;       /* WebGPU: a render attachment view */
  int wfmt;          /* WebGPU: its WGPUTextureFormat */
  bool offscreen;    /* GL row order (fx targets, render targets, the virtual canvas) */
  int vp[4];         /* x, y (GL rows), w, h; w 0 = the whole target */
} fx_dst;

/* a full-screen program: GLSL ES 3.00 fragment body and WGSL fragment body.
 * GLSL gets: uniform vec4 u_v[8]; uniform vec2 u_size (destination pixels);
 * uniform sampler2D t0..t3; in vec2 v_uv; out vec4 o_col.
 * WGSL gets: fx.size (vec2f), fx.v (array<vec4f, 8>), t0..t3 (texture_2d<f32>),
 * s_lin / s_near (samplers), and defines @fragment fn fs(i: VO) with i.uv. */
typedef struct fx_prog {
  const char *name, *glsl, *wgsl;
  GLuint gl;
  GLint u_v, u_size;
  void *wmod; /* WGPUShaderModule */
  bool failed;
} fx_prog;

enum { FX_BLEND_NONE, FX_BLEND_PREMUL, FX_BLEND_ADD };
/* uniform blocks a cw_r2d_end may spend on fx passes (WebGPU staging) */
#define R2D_FX_BLOCKS 192

/* the format HDR targets get: RGBA16F where the host renders it, else RGBA8 */
fx_fmt fx_hdr_format(void);
/* (re)create rt at this size and format (no-op when it already is) */
void fx_rt_ensure(fx_rt *rt, int w, int h, fx_fmt f);
void fx_rt_free(fx_rt *rt);
fx_dst fx_dst_of(const fx_rt *rt);
/* one full-screen triangle of p into dst; src[k] bound as t<k> (NULL: a black
 * texel); v: 8 vec4 of uniforms (may be NULL) */
void fx_pass(fx_prog *p, const fx_dst *dst, bool clear, const float clear_rgba[4], const fx_rt *const src[4],
             const float v[8][4], int blend);
void fx_shutdown(void);

/* backends (fx.c GL, wgpu.c WebGPU) */
void *r2d_wg_fx_tex(int w, int h, fx_fmt f);
int r2d_wg_fx_format(fx_fmt f); /* its WGPUTextureFormat */
void *r2d_wg_fx_view(void *tex); /* a render attachment view (owned by the caller of the pass: transient) */
void r2d_wg_fx_pass(fx_prog *p, const fx_dst *dst, bool clear, const float clear_rgba[4], const fx_rt *const src[4],
                    const float v[8][4], int blend);
void r2d_wg_fx_prog_free(fx_prog *p);
/* WebGPU: the render pass now open (r2d_wg_end's) is ended before an fx pass
 * and reopened after by the caller */
#endif

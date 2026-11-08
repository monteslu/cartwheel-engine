/* render2d on WebGPU (webgpu.h, WGSL). The submission stream, the stable
 * sort and the batching are the GL path's (batch.c); this file turns the
 * same batches into render passes:
 *
 *   canvas pass   the pass target (window frame, virtual canvas, or a
 *                 cw_target), cleared or loaded
 *   lit runs      scene (+ normal) pass, light pass (depth-stencil for
 *                 shadow volumes), then the canvas pass again with the
 *                 composite
 *   blit          a virtual canvas onto the window (letterboxed)
 *
 * Every cw_r2d_end records one command encoder and submits it, after
 * writing the pass's vertices, indices, uniforms and scratch geometry with
 * wgpuQueueWriteBuffer (queue order makes the writes land before the draws).
 *
 * Orientation: GL's offscreen targets store rows bottom-up and the CPU side
 * (UVs of render targets, the light shader's window coordinates) relies on
 * it. WebGPU's framebuffer y points down, so passes into OFFSCREEN targets
 * flip y in the vertex stage (flip = -1): the texture's memory then matches
 * GL's row for row, gl_FragCoord.y == @builtin(position).y, and every UV the
 * shared code computes is right. Window passes do not flip (flip = +1), and
 * the composite, which reads an offscreen target at the fragment position,
 * mirrors the row when it writes the window.
 *
 * Compatibility mode (the wasmcart tier's floor): array textures declare
 * their binding view dimension (2D array, even with one layer), render
 * attachments use 2D single-layer views, flat varyings use `either`,
 * identical blend on both attachments of the lit pass, depthClearValue set. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "fx.h"
#include "cartwheel/platform.h"

#if !CW_WGPU
bool r2d_wg(void) { return false; }
bool r2d_wg_init(void) { return false; }
void r2d_wg_shutdown(void) {}
int r2d_wg_max_layers(void) { return 1; }
void *r2d_wg_tex_new(int w, int h, int layers) { (void)w, (void)h, (void)layers; return NULL; }
void *r2d_wg_tex_new_hdr(int w, int h, int layers) { (void)w, (void)h, (void)layers; return NULL; }
void *r2d_wg_tex_new_levels(int w, int h, int layers, int levels) { (void)w, (void)h, (void)layers, (void)levels; return NULL; }
void r2d_wg_tex_write_level(void *tex, int level, int x, int y, int layer, int w, int h, const uint8_t *px) {
  (void)tex, (void)level, (void)x, (void)y, (void)layer, (void)w, (void)h, (void)px;
}
void *r2d_wg_depth_new(int w, int h) { (void)w, (void)h; return NULL; }
void r2d_wg_tex_fill(void *tex, int from, int to, const uint8_t rgba[4]) { (void)tex, (void)from, (void)to, (void)rgba; }
void r2d_wg_tex_copy(void *src, void *dst, int layers) { (void)src, (void)dst, (void)layers; }
void r2d_wg_tex_write(void *tex, int x, int y, int layer, int w, int h, const uint8_t *px) {
  (void)tex, (void)x, (void)y, (void)layer, (void)w, (void)h, (void)px;
}
void r2d_wg_tex_release(void *tex) { (void)tex; }
void r2d_wg_array_changed(r2d_array *a) { (void)a; }
void r2d_wg_end(const r2d_batch *b, int n, const uint32_t *idx) { (void)b, (void)n, (void)idx; }
void *r2d_wg_buffer(void *old, const void *data, size_t bytes, bool index) { (void)old, (void)data, (void)bytes, (void)index; return NULL; }
void r2d_wg_buffer_release(void *buf) { (void)buf; }
void r2d_wg_tilemap_begin(const float u[24], int array, bool lit) { (void)u, (void)array, (void)lit; }
void r2d_wg_instances(const r2d_inst_draw *d, bool lit, void *stream_buf) { (void)d, (void)lit, (void)stream_buf; }
void *r2d_wg_instance_buffer(void *old, size_t bytes) { (void)old, (void)bytes; return NULL; }
void r2d_wg_buffer_write(void *buf, size_t offset, const void *data, size_t bytes) { (void)buf, (void)offset, (void)data, (void)bytes; }
void r2d_wg_tilemap_chunk(void *vbuf, int quads) { (void)vbuf, (void)quads; }
void r2d_wg_particles(const r2d_part_draw *d, bool lit) { (void)d, (void)lit; }
void *r2d_wg_yuv_new(const int pw[3], const int ph[3]) { (void)pw, (void)ph; return NULL; }
void r2d_wg_yuv_convert(void *y, const uint8_t *const planes[3], void *target_tex, int w, int h) {
  (void)y, (void)planes, (void)target_tex, (void)w, (void)h;
}
void r2d_wg_yuv_free(void *y) { (void)y; }
unsigned r2d_wg_target_rt(void *tex, int w, int h) { (void)tex, (void)w, (void)h; return 0; }
void *r2d_wg_layer_view(void *tex) { (void)tex; return NULL; }
void r2d_wg_view_release(void *view) { (void)view; }
void r2d_wg_target_rt_free(unsigned id) { (void)id; }
void *r2d_wg_fx_tex(int w, int h, fx_fmt f) { (void)w, (void)h, (void)f; return NULL; }
int r2d_wg_fx_format(fx_fmt f) { (void)f; return 0; }
bool r2d_wg_small_hdr_ok(void) { return false; }
void *r2d_wg_fx_view(void *tex) { (void)tex; return NULL; }
void r2d_wg_fx_pass(fx_prog *p, const fx_dst *dst, bool clear, const float cc[4], const fx_rt *const src[4], const float v[8][4],
                    int blend) {
  (void)p, (void)dst, (void)clear, (void)cc, (void)src, (void)v, (void)blend;
}
void r2d_wg_fx_prog_free(fx_prog *p) { (void)p; }
bool r2d_wg_timers_init(void) { return false; }
bool r2d_wg_timer_mark(int part, bool end) { (void)part, (void)end; return false; }
void r2d_wg_timers_resolve(void) {}
void r2d_wg_timers_map(void) {}
void r2d_wg_mask_draw(void *mask, int w, int h, const float *tris, int nverts, float cw, float ch) {
  (void)mask, (void)w, (void)h, (void)tris, (void)nverts, (void)cw, (void)ch;
}
void r2d_wg_mask_batches(void *mask, const r2d_batch *b, int n) { (void)mask, (void)b, (void)n; }
void r2d_wg_lights_draw(int cookie_array, void *light, void *add, int fmt, const float amb[4], const float *data, int rows, const float up[4],
                        const float uq[4], const float ut[4], void *normal, void *seed, void *mask, void *mat, int w, int h) {
  (void)cookie_array, (void)light, (void)add, (void)fmt, (void)amb, (void)data, (void)rows, (void)up, (void)uq, (void)ut, (void)normal, (void)seed, (void)mask,
      (void)mat, (void)w, (void)h;
}
#else

#include "platform/wgpu.h"
#include "wgpu_count.h"

#define SV CW_WGPU_SV
#define UALIGN 256u

/* ── WGSL ──────────────────────────────────────────────────────────────── */

static const char *WGSL_COMMON =
    "struct U { layers: array<vec4f, 64>, target_size: vec2f, flip: f32, zbias: f32, wind: vec4f, fog: array<vec4f, 32> };\n"
    "struct Q { target_size: vec2f, flip: f32, mode: f32, a: vec4f, b: vec4f, c: vec4f, d: vec4f, e: vec4f, f: vec4f };\n"
    "@group(1) @binding(0) var t_tex: texture_2d_array<f32>;\n"
    "@group(1) @binding(1) var t_smp: sampler;\n"
    "@group(1) @binding(2) var t_nrm: texture_2d_array<f32>;\n"
    "@group(1) @binding(3) var t_mat: texture_2d_array<f32>;\n"
    "fn ndc(p: vec2f, t: vec2f, flip: f32) -> vec4f {\n"
    "  return vec4f(p.x / t.x * 2.0 - 1.0, (1.0 - p.y / t.y * 2.0) * flip, 0.0, 1.0);\n"
    "}\n"
    "fn gmod(x: f32, y: f32) -> f32 { return x - y * floor(x / y); }\n";

/* the stream: sprites, shapes, text, geometry (GL: gl.c SPRITE_VS / FS) */
static const char *WGSL_SPRITE =
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "struct VO { @builtin(position) @invariant pos: vec4f, @location(0) uv: vec2f, @location(1) color: vec4f,\n"
    "  @location(2) @interpolate(flat, either) page: u32, @location(3) @interpolate(flat, either) mode: u32,\n"
    "  @location(4) @interpolate(flat, either) sdfw: f32, @location(5) @interpolate(flat, either) sdf: vec2f,\n"
    "  @location(6) @interpolate(flat, either) emis: f32, @location(7) @interpolate(flat, either) dist: f32,\n"
    "  @location(8) @interpolate(flat, either) fog: vec4f, @location(9) fp: vec2f };\n" /* fp: gl.c v_fp */
    "fn sprite_vs(a_pos: vec2f, a_uv: vec2f, a_color: vec4f, a_info: vec4u, a_sdf: vec4f, a_depth: f32) -> VO {\n"
    "  let L = a_info.z * 2u;\n"
    "  let r0 = u.layers[L]; let r1 = u.layers[L + 1u];\n"
    "  var p = vec2f(r0.x * a_pos.x + r0.y * a_pos.y + r0.z, r1.x * a_pos.x + r1.y * a_pos.y + r1.z);\n"
    "  let fp = vec2f(p.x, -p.y);\n"
    "  if ((a_info.y & 2u) == 0u && a_info.w > 0u && u.wind.x != 0.0) {\n"
    "    let ph = u.wind.y + a_pos.x * u.wind.z + a_pos.y * u.wind.z * 0.37;\n"
    "    let s = sin(ph) + 0.45 * sin(ph * 2.3 + 1.7) * u.wind.w;\n"
    "    p.x += f32(a_info.w) / 255.0 * u.wind.x * s * sqrt(abs(r0.x * r1.y - r0.y * r1.x));\n"
    "  }\n"
    "  if (r0.w > 0.5) { p = floor(p + 0.5); }\n"
    "  var o: VO;\n"
    "  o.pos = ndc(p, u.target_size, u.flip);\n"
    "  o.uv = a_uv;\n"
    "  o.color = vec4f(a_color.rgb * a_color.a * (1.0 + a_sdf.w * 15.0), a_color.a);\n"
    "  o.page = a_info.x;\n"
    "  o.mode = a_info.y;\n"
    "  o.sdfw = 0.6 * f32(a_info.w) / 255.0 / max(sqrt(abs(r0.x * r1.y - r0.y * r1.x)), 1e-4);\n"
    "  o.sdf = a_sdf.xy;\n"
    "  o.emis = a_sdf.z;\n"
    "  o.dist = r1.w;\n"
    "  o.fog = u.fog[a_info.z];\n"
    "  o.pos.z = max(a_depth - u.zbias, 0.0);\n"
    "  o.fp = fp;\n"
    "  return o;\n"
    "}\n"
    "@vertex fn vs(@location(0) a_pos: vec2f, @location(1) a_uv: vec2f, @location(2) a_color: vec4f,\n"
    "              @location(3) a_info: vec4u, @location(4) a_sdf: vec4f) -> VO {\n"
    "  return sprite_vs(a_pos, a_uv, a_color, a_info, a_sdf, 0.0);\n"
    "}\n"
    /* depth culling (cull.c): each vertex's depth from a second buffer */
    "@vertex fn vs_z(@location(0) a_pos: vec2f, @location(1) a_uv: vec2f, @location(2) a_color: vec4f,\n"
    "              @location(3) a_info: vec4u, @location(4) a_sdf: vec4f, @location(9) a_depth: f32) -> VO {\n"
    "  return sprite_vs(a_pos, a_uv, a_color, a_info, a_sdf, a_depth);\n"
    "}\n"
    /* GPU particles (GL: gl.c PART_VS): pa as u_pa there */
    "struct PA { a: array<vec4f, 9> };\n"
    "@group(2) @binding(0) var<uniform> pa: PA;\n"
    "fn hsh(x0: u32) -> u32 { var x = x0; x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u; return x; }\n"
    "fn rnd(s: ptr<function, u32>) -> f32 { *s = hsh(*s); return f32(*s >> 8u) / 16777216.0; }\n"
    "@vertex fn vs_particles(@builtin(vertex_index) k: u32, @builtin(instance_index) inst: u32) -> VO {\n"
    "  let q = vec2f(select(0.0, 1.0, k == 1u || k == 2u || k == 4u), select(0.0, 1.0, k == 2u || k == 4u || k == 5u));\n"
    "  var s = hsh((inst * 0x9E3779B9u) ^ u32(pa.a[7].w));\n"
    "  let ph = rnd(&s); let life = max(mix(pa.a[3].x, pa.a[3].y, rnd(&s)), 1e-3);\n"
    "  let tt = pa.a[2].w + ph * life; let cyc = floor(tt / life); let age = tt - cyc * life; let f = age / life;\n"
    "  var r = hsh(s ^ (u32(cyc) * 0x85ebca6bu));\n"
    "  let ra = rnd(&r); let rb = rnd(&r);\n"
    "  var p = pa.a[0].xy + (vec2f(ra, rb) * 2.0 - 1.0) * pa.a[0].zw;\n"
    "  let ang = pa.a[1].x + (rnd(&r) * 2.0 - 1.0) * pa.a[1].y;\n"
    "  let v = vec2f(cos(ang), sin(ang)) * mix(pa.a[1].z, pa.a[1].w, rnd(&r));\n"
    "  let dr = pa.a[2].z;\n"
    "  if (dr > 1e-4) { let e = (1.0 - exp(-dr * age)) / dr; p += v * e + pa.a[2].xy * (age - e) / dr; }\n"
    "  else { p += v * age + 0.5 * pa.a[2].xy * age * age; }\n"
    "  let wob = rnd(&r) * 6.2831853;\n"
    "  p += vec2f(sin(age * 2.7 + wob), cos(age * 2.1 + wob * 1.3)) * pa.a[7].z * f;\n"
    "  let sz = mix(pa.a[3].z, pa.a[3].w, f); let rot = pa.a[7].y * (rnd(&r) * 2.0 - 1.0) * age;\n"
    "  let l = (q - 0.5) * sz;\n"
    "  let cs = cos(rot); let sn = sin(rot);\n"
    "  let w = p + vec2f(cs * l.x - sn * l.y, sn * l.x + cs * l.y);\n"
    "  let L = u32(pa.a[8].x) * 2u;\n"
    "  let r0 = u.layers[L]; let r1 = u.layers[L + 1u];\n"
    "  let c = vec2f(r0.x * w.x + r0.y * w.y + r0.z, r1.x * w.x + r1.y * w.y + r1.z);\n"
    "  var o: VO;\n"
    "  o.fp = vec2f(c.x, -c.y);\n"
    "  o.pos = ndc(c, u.target_size, u.flip);\n"
    "  o.uv = mix(pa.a[6].xy, pa.a[6].zw, q);\n"
    "  let col = mix(pa.a[4], pa.a[5], f);\n"
    "  o.color = vec4f(col.rgb * col.a * pa.a[8].z, col.a);\n"
    "  o.page = u32(pa.a[7].x);\n"
    "  o.mode = u32(pa.a[8].y);\n"
    "  o.sdfw = 0.0; o.sdf = vec2f(0.0);\n"
    "  o.emis = pa.a[8].w; o.dist = 0.0;\n"
    "  o.fog = u.fog[u32(pa.a[8].x)];\n"
    "  return o;\n"
    "}\n"
    /* sprite instances (GL: gl.c INST_VS): inb as u_in there */
    "struct INB { a: array<vec4f, 67> };\n"
    "@group(2) @binding(1) var<uniform> inb: INB;\n"
    "@vertex fn vs_instances(@builtin(vertex_index) k: u32, @location(0) i_a: vec4f, @location(1) i_col: vec4f, @location(2) i_fr: vec2u) -> VO {\n"
    "  let q = vec2f(select(0.0, 1.0, k == 1u || k == 2u || k == 4u), select(0.0, 1.0, k == 2u || k == 4u || k == 5u));\n"
    "  let f = i_fr.x; let fl = i_fr.y;\n"
    "  let uv = inb.a[3u + 2u * f]; let fs = inb.a[4u + 2u * f];\n"
    "  let sc = select(i_a.w, 1.0, i_a.w == 0.0);\n"
    "  let sz = select(fs.xy, inb.a[0].zw, inb.a[0].z > 0.0) * sc;\n"
    "  let l = (q - inb.a[0].xy) * sz;\n"
    "  let cs = cos(i_a.z); let sn = sin(i_a.z);\n"
    "  let w = i_a.xy + vec2f(cs * l.x - sn * l.y, sn * l.x + cs * l.y);\n"
    "  let L = u32(inb.a[1].x) * 2u;\n"
    "  let r0 = u.layers[L]; let r1 = u.layers[L + 1u];\n"
    "  var p = vec2f(r0.x * w.x + r0.y * w.y + r0.z, r1.x * w.x + r1.y * w.y + r1.z);\n"
    "  let fp = vec2f(p.x, -p.y);\n"
    "  if (q.y < 0.5 && inb.a[2].z > 0.0 && u.wind.x != 0.0) {\n"
    "    let ph = u.wind.y + w.x * u.wind.z + w.y * u.wind.z * 0.37;\n"
    "    let s = sin(ph) + 0.45 * sin(ph * 2.3 + 1.7) * u.wind.w;\n"
    "    p.x += inb.a[2].z * u.wind.x * s * sqrt(abs(r0.x * r1.y - r0.y * r1.x));\n"
    "  }\n"
    "  if (r0.w > 0.5) { p = floor(p + 0.5); }\n"
    "  var o: VO;\n"
    "  o.pos = ndc(p, u.target_size, u.flip);\n"
    "  let tq = vec2f(select(q.x, 1.0 - q.x, (fl & 1u) != 0u), select(q.y, 1.0 - q.y, (fl & 2u) != 0u));\n"
    "  o.uv = mix(uv.xy, uv.zw, tq);\n"
    "  let col = select(i_col, vec4f(1.0), all(i_col == vec4f(0.0)));\n"
    "  o.color = vec4f(col.rgb * col.a * inb.a[1].z, col.a);\n"
    "  o.page = u32(inb.a[2].x);\n"
    "  o.mode = u32(inb.a[1].y);\n"
    "  o.sdfw = 0.0; o.sdf = vec2f(inb.a[2].y, 0.0);\n"
    "  o.emis = inb.a[1].w; o.dist = r1.w;\n"
    "  o.fog = u.fog[u32(inb.a[1].x)];\n"
    "  o.fp = fp;\n"
    "  return o;\n"
    "}\n"
    "fn shade(i: VO, t: vec4f) -> vec4f {\n"
    "  var c: vec4f;\n"
    "  if ((i.mode & 1u) != 0u) { c = i.color; }\n"
    "  else if ((i.mode & 2u) != 0u) {\n"
    "    let w = clamp(i.sdfw, 0.004, 0.5) + i.sdf.y; let e = 0.5 - i.sdf.x;\n"
    "    var lo = e - w;\n"
    "    if (i.sdf.x + i.sdf.y > 0.0) { lo = max(lo, 0.004); }\n"
    "    c = i.color * smoothstep(lo, e + w, t.a);\n"
    "  } else { c = t * i.color; }\n"
    "  if ((i.mode & 2u) == 0u && i.sdf.x > 0.0) { c = vec4f(mix(c.rgb, i.color.rgb / max(i.color.a, 1e-4) * c.a, i.sdf.x), c.a); }\n"
    "  return c;\n"
    "}\n"
    "fn fogged(i: VO, c: vec4f) -> vec4f {\n"
    "  if (i.fog.a > 0.0) { return vec4f(mix(c.rgb, i.fog.rgb * c.a, i.fog.a), c.a); }\n"
    "  return c;\n"
    "}\n"
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  var c = fogged(i, shade(i, textureSample(t_tex, t_smp, i.uv, i.page)));\n"
    "  if ((i.mode & 4u) != 0u) { c.a = 0.0; }\n"
    "  return c;\n"
    "}\n"
    /* depth culling's pre-pass (GL: SPRITE_FS with PREPASS): only texels that replace what is under them */
    "@fragment fn fs_pre(i: VO) {\n"
    "  let c = shade(i, textureSample(t_tex, t_smp, i.uv, i.page));\n"
    "  if ((i.mode & 4u) != 0u || c.a < 1.0) { discard; }\n"
    "}\n"
    /* occluders from art (GL: SPRITE_FS with MASK) */
    "@fragment fn fs_mask(i: VO) -> @location(0) vec4f {\n"
    "  let c = shade(i, textureSample(t_tex, t_smp, i.uv, i.page));\n"
    "  if (c.a < 0.5) { discard; }\n"
    "  return vec4f(1.0);\n"
    "}\n"
    /* distortion layers (GL: SPRITE_FS with DISTORT) */
    "@fragment fn fs_distort(i: VO) -> @location(0) vec4f {\n"
    "  var c = shade(i, textureSample(t_tex, t_smp, i.uv, i.page));\n"
    "  if ((i.mode & 4u) != 0u) { c.a = 0.0; }\n"
    "  var off = vec2f(0.0);\n"
    "  if (c.a > 1e-4) { off = (c.rg / c.a * 2.0 - 1.0) * c.a * i.dist; }\n"
    "  return vec4f(off, 0.0, 0.0);\n"
    "}\n"
    /* a turned or flipped sprite's normals turned with it (gl.c TURN_NORMAL_GLSL) */
    "fn turn_normal(enc: vec3f, uvx: vec2f, uvy: vec2f, fpx: vec2f, fpy: vec2f) -> vec3f {\n"
    "  let det = uvx.x * uvy.y - uvy.x * uvx.y;\n"
    "  if (det == 0.0) { return enc; }\n"
    "  let du = (fpx * uvy.y - fpy * uvx.y) * sign(det); let dv = (fpy * uvx.x - fpx * uvy.x) * sign(det);\n"
    "  let lu = length(du); let lv = length(dv);\n"
    "  if (!(lu > 0.0) || !(lv > 0.0)) { return enc; }\n"
    "  let T = du / lu; let U = -dv / lv;\n"
    "  if (T.x > 0.0 && U.y > 0.0 && abs(T.y) < 1e-3 && abs(U.x) < 1e-3) { return enc; }\n"
    "  let e = enc * 2.0 - 1.0;\n"
    "  return vec3f(T * e.x + U * e.y, e.z) * 0.5 + 0.5;\n"
    "}\n"
    "struct LitOut { @location(0) col: vec4f, @location(1) nrm: vec4f, @location(2) mat: vec4f };\n"
    "fn lit_body(i: VO) -> LitOut {\n"
    "  let uvx = dpdx(i.uv); let uvy = dpdy(i.uv); let fpx = dpdx(i.fp); let fpy = dpdy(i.fp);\n" /* before any branch (uniform control flow) */
    "  var c = shade(i, textureSample(t_tex, t_smp, i.uv, i.page));\n"
    "  let n = textureSample(t_nrm, t_smp, i.uv, i.page);\n"
    "  var m = textureSample(t_mat, t_smp, i.uv, i.page).rgb;\n"
    "  var enc = turn_normal(n.rgb, uvx, uvy, fpx, fpy);\n"
    "  if ((i.mode & 3u) != 0u) { enc = vec3f(0.5, 0.5, 0.0); m = vec3f(0.0); }\n"
    "  var o: LitOut;\n"
    "  o.nrm = vec4f(enc, 1.0) * c.a;\n"
    "  o.mat = vec4f(max(i.emis, m.r), m.g, m.b, 1.0) * c.a;\n"
    "  if ((i.mode & 4u) != 0u) { o.nrm = vec4f(0.0); o.mat = vec4f(0.0); c.a = 0.0; }\n"
    "  o.col = c;\n"
    "  return o;\n"
    "}\n"
    /* normals without a material buffer (no material map or emission drawn) */
    "@fragment fn fs_lit(i: VO) -> LitOut { return lit_body(i); }\n"
    "struct LitOutN { @location(0) col: vec4f, @location(1) nrm: vec4f };\n"
    "@fragment fn fs_lit_n(i: VO) -> LitOutN {\n"
    "  let l = lit_body(i);\n"
    "  var o: LitOutN;\n"
    "  o.col = l.col; o.nrm = l.nrm;\n"
    "  return o;\n"
    "}\n";

/* tilemap chunks (GL: TILE_VS / FS): q.a = m0, b = m1, c = set0, d = set1,
 * e = (inv_page, time, 0), f = tint */
static const char *WGSL_TILE =
    "@group(0) @binding(0) var<uniform> q: Q;\n"
    "struct VO { @builtin(position) @invariant pos: vec4f, @location(0) uv: vec2f, @location(1) @interpolate(flat, either) page: u32 };\n"
    "@vertex fn vs(@location(0) a_pos: vec2f, @location(1) a_tile: vec4u) -> VO {\n"
    "  var p = vec2f(q.a.x * a_pos.x + q.a.y * a_pos.y + q.a.z, q.b.x * a_pos.x + q.b.y * a_pos.y + q.b.z);\n"
    "  if (q.a.w > 0.5) { p = floor(p + 0.5); }\n"
    "  var o: VO;\n"
    "  o.pos = ndc(p, q.target_size, q.flip);\n"
    "  o.pos.z = q.b.w;\n" /* the draw's depth (depth culling, cull.c); 0 = nearest */
    "  var id = f32(a_tile.x);\n"
    "  let frames = f32(a_tile.z);\n"
    "  if (frames > 1.0) { id += gmod(floor(q.e.z * f32(a_tile.w) / 16.0), frames); }\n"
    "  let col = gmod(id, q.d.z); let row = floor(id / q.d.z);\n"
    "  let corner = vec2f(gmod(f32(a_tile.y), 2.0), floor(f32(a_tile.y) / 2.0));\n"
    "  let inset = mix(vec2f(0.02), q.c.zw - 0.02, corner);\n"
    "  o.uv = (q.c.xy + vec2f(col, row) * q.d.xy + inset) * q.e.xy;\n"
    "  o.page = u32(q.d.w);\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  return textureSample(t_tex, t_smp, i.uv, i.page) * vec4f(q.f.rgb * q.f.a, q.f.a);\n"
    "}\n"
    "@fragment fn fs_pre(i: VO) {\n"
    "  let c = textureSample(t_tex, t_smp, i.uv, i.page) * vec4f(q.f.rgb * q.f.a, q.f.a);\n"
    "  if (c.a < 1.0) { discard; }\n"
    "}\n"
    "@fragment fn fs_mask(i: VO) -> @location(0) vec4f {\n"
    "  let c = textureSample(t_tex, t_smp, i.uv, i.page) * vec4f(q.f.rgb * q.f.a, q.f.a);\n"
    "  if (c.a < 0.5) { discard; }\n"
    "  return vec4f(1.0);\n"
    "}\n"
    "struct LitOut { @location(0) col: vec4f, @location(1) nrm: vec4f, @location(2) mat: vec4f };\n"
    "fn lit_body(i: VO) -> LitOut {\n"
    "  let c = textureSample(t_tex, t_smp, i.uv, i.page) * vec4f(q.f.rgb * q.f.a, q.f.a);\n"
    "  let n = textureSample(t_nrm, t_smp, i.uv, i.page);\n"
    "  let m = textureSample(t_mat, t_smp, i.uv, i.page);\n"
    "  var o: LitOut;\n"
    "  o.col = c;\n"
    "  o.nrm = vec4f(n.rgb, 1.0) * c.a;\n"
    "  o.mat = vec4f(m.rgb, 1.0) * c.a;\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs_lit(i: VO) -> LitOut { return lit_body(i); }\n"
    "struct LitOutN { @location(0) col: vec4f, @location(1) nrm: vec4f };\n"
    "@fragment fn fs_lit_n(i: VO) -> LitOutN {\n"
    "  let l = lit_body(i);\n"
    "  var o: LitOutN;\n"
    "  o.col = l.col; o.nrm = l.nrm;\n"
    "  return o;\n"
    "}\n";

/* scratch quads in target pixels: lights, shadows, composite, blit
 * (GL: QUAD_VS and the four fragment shaders) */
static const char *WGSL_QUAD =
    "@group(0) @binding(0) var<uniform> q: Q;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };\n"
    "@vertex fn vs(@location(0) a_pos: vec2f, @location(1) a_uv: vec2f) -> VO {\n"
    "  var o: VO;\n"
    "  o.pos = ndc(a_pos, q.target_size, q.flip);\n"
    "  o.uv = a_uv;\n"
    "  return o;\n"
    "}\n"
    /* one point light (stores light / 2): a = centre (GL window coords),
     * b = colour * energy / 2 + radius, c = falloff, height, use normals */
    "@fragment fn fs_light(i: VO) -> @location(0) vec4f {\n"
    "  let d = i.pos.xy - q.a.xy;\n"
    "  let att = pow(clamp(1.0 - length(d) / q.b.w, 0.0, 1.0), q.c.x);\n"
    "  var ndl = 1.0;\n"
    "  if (q.c.z > 0.5) {\n"
    "    let n = textureLoad(t_nrm, vec2i(i.pos.xy), 0, 0);\n"
    "    if (n.a > 0.004) {\n"
    "      let e = n.rgb / n.a;\n"
    "      let mapped = smoothstep(0.0, 0.5, e.z);\n"
    "      let N = normalize(e * 2.0 - 1.0);\n"
    "      let Ld = normalize(vec3f(-d, q.c.y));\n"
    "      ndl = mix(1.0, max(dot(N, Ld), 0.0), mapped);\n"
    "    }\n"
    "  }\n"
    "  return vec4f(q.b.rgb * att * ndl, 0.0);\n"
    "}\n"
    "@fragment fn fs_shadow(i: VO) -> @location(0) vec4f { return vec4f(0.0); }\n"
    /* colour * light into the canvas; mode 1: the target is the window (rows
     * top-down) while the sources are offscreen (rows bottom-up) */
    "@fragment fn fs_composite(i: VO) -> @location(0) vec4f {\n"
    "  var p = vec2i(i.pos.xy);\n"
    "  if (q.mode > 0.5) { p.y = i32(q.target_size.y) - 1 - p.y; }\n"
    "  let s = textureLoad(t_tex, p, 0, 0);\n"
    "  let l = textureLoad(t_nrm, p, 0, 0).rgb * 2.0;\n"
    "  return vec4f(s.rgb * l, s.a);\n"
    "}\n"
    /* q.a.y: sharp filtering (cw_scale_filter SHARP), q.a.zw canvas size, q.b.x window pixels per canvas pixel */
    "@fragment fn fs_blit(i: VO) -> @location(0) vec4f {\n"
    "  var uv = i.uv;\n"
    "  if (q.a.y > 0.5) { let t = uv * q.a.zw; let c = floor(t) + 0.5; uv = (c + clamp((t - c) * q.b.x, vec2f(-0.5), vec2f(0.5))) / q.a.zw; }\n"
    "  return textureSample(t_tex, t_smp, uv, u32(q.a.x));\n"
    "}\n";

/* ── state ─────────────────────────────────────────────────────────────── */

enum { P_SPRITE, P_SPRITE_LIT, P_TILE, P_TILE_LIT, P_LIGHT, P_LIGHT_ST, P_SHADOW, P_COMPOSITE, P_BLIT, P_SPRITE_DISTORT,
       P_SPRITE_LIT_N, P_TILE_LIT_N, P_SPRITE_MASK, P_TILE_MASK, P_PART, P_PART_LIT, P_PART_LIT_N, P_INST, P_INST_LIT, P_INST_LIT_N, P_SPRITE_PRE, P_TILE_PRE, P_COUNT }; /* _N: lit with normals, no material buffer */
enum { B_OPAQUE, B_ALPHA, B_MULTIPLY, B_ADD };
#define U_STREAM_SIZE 1568u
#define U_SMALL_SIZE 112u

typedef struct pipe_rec {
  uint32_t key;
  WGPURenderPipeline p;
} pipe_rec;

static struct {
  bool inited;
  WGPUDevice dev;
  WGPUQueue q;
  WGPUShaderModule sm_sprite, sm_tile, sm_quad;
  WGPUBindGroupLayout bgl_stream, bgl_small, bgl_tex, bgl_part, bgl_inst;
  WGPUPipelineLayout pl_stream, pl_small, pl_shadow, pl_part, pl_inst;
  WGPUBindGroup bg_inst; /* over ubuf, the instance block (group 2, binding 1) */
  WGPUBindGroup bg_part; /* over ubuf, the particle block (group 2) */
  WGPUSampler samplers[8]; /* nearest | repeat << 1 | mipmapped << 2 */
  WGPUTexture dummy;       /* 1x1x1 "no normal" array (also the solid draws' colour texture) */
  WGPUTextureView dummy_view;
  WGPUTexture dummy_mat;   /* 1x1x1 "no material" array (zeros) */
  WGPUTextureView dummy_mat_view;
  WGPUBuffer vbuf, ibuf, ubuf, sbuf, tile_ibuf;
  size_t vcap, icap, ucap, scap;
  CW_ARRAY(pipe_rec) pipes;
  /* recording */
  WGPUCommandEncoder enc;
  WGPURenderPassEncoder pass;
  WGPUTextureFormat pass_fmt;
  bool pass_ds;             /* the open pass has the culling depth buffer: draws test it (cull.c) */
  bool ds_clear;            /* begin_pass clears the depth buffer (the pre-pass) */
  bool cull_on;             /* the group now drawing culls: canvas and lit passes take the depth buffer */
  WGPUTexture cull_tex;     /* the depth buffer (Depth32Float, canvas size) */
  WGPUTextureView cull_view;
  int cull_w, cull_h;
  WGPUBuffer dbuf;          /* each vertex's depth */
  size_t dcap;
  float pass_flip;
  int pass_w, pass_h;
  CW_ARRAY(uint8_t) ustage; /* uniforms for this end, UALIGN-aligned blocks */
  CW_ARRAY(float) sstage;   /* scratch quads (x, y, u, v) */
  WGPUBindGroup bg_stream, bg_small; /* over ubuf (remade when it grows) */
  CW_ARRAY(WGPUBindGroup) transient; /* released after the submit */
  uint32_t stream_off[3];            /* stream uniform blocks: [0] flip -1, [1] flip +1, [2] flip -1 without the culling bias */
} W;

bool r2d_wg(void) { return cw__wgpu_on(); }
int r2d_wg_max_layers(void) { return (int)cw__wgpu_limits()->maxTextureArrayLayers; }

/* ── textures ──────────────────────────────────────────────────────────── */

void *r2d_wg_tex_new(int w, int h, int layers) {
  WGPUTextureBindingViewDimension vd = { .chain = { .sType = WGPUSType_TextureBindingViewDimension },
                                         .textureBindingViewDimension = WGPUTextureViewDimension_2DArray };
  WGPUTextureDescriptor d = { .nextInChain = &vd.chain, .label = SV("r2d array"),
                              .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst  | WGPUTextureUsage_RenderAttachment| WGPUTextureUsage_CopySrc |
                                       WGPUTextureUsage_RenderAttachment,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, (uint32_t)layers },
                              .format = WGPUTextureFormat_RGBA8Unorm, .mipLevelCount = 1, .sampleCount = 1 };
  return wgpuDeviceCreateTexture(W.dev, &d);
}

void *r2d_wg_tex_new_hdr(int w, int h, int layers) {
  WGPUTextureBindingViewDimension vd = { .chain = { .sType = WGPUSType_TextureBindingViewDimension },
                                         .textureBindingViewDimension = WGPUTextureViewDimension_2DArray };
  WGPUTextureDescriptor d = { .nextInChain = &vd.chain, .label = SV("r2d array (HDR)"),
                              .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc |
                                       WGPUTextureUsage_RenderAttachment,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, (uint32_t)layers },
                              .format = WGPUTextureFormat_RGBA16Float, .mipLevelCount = 1, .sampleCount = 1 };
  return wgpuDeviceCreateTexture(W.dev, &d);
}

void *r2d_wg_tex_new_levels(int w, int h, int layers, int levels) {
  WGPUTextureBindingViewDimension vd = { .chain = { .sType = WGPUSType_TextureBindingViewDimension },
                                         .textureBindingViewDimension = WGPUTextureViewDimension_2DArray };
  WGPUTextureDescriptor d = { .nextInChain = &vd.chain, .label = SV("r2d array (mipmapped)"),
                              .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc | WGPUTextureUsage_RenderAttachment,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, (uint32_t)layers },
                              .format = WGPUTextureFormat_RGBA8Unorm, .mipLevelCount = (uint32_t)levels, .sampleCount = 1 };
  return wgpuDeviceCreateTexture(W.dev, &d);
}

void r2d_wg_tex_write_level(void *tex, int level, int x, int y, int layer, int w, int h, const uint8_t *px) {
  WGPUTexelCopyTextureInfo d = { .texture = tex, .mipLevel = (uint32_t)level, .origin = { (uint32_t)x, (uint32_t)y, (uint32_t)layer },
                                 .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = (uint32_t)w * 4, .rowsPerImage = (uint32_t)h };
  WGPUExtent3D e = { (uint32_t)w, (uint32_t)h, 1 };
  wgpuQueueWriteTexture(W.q, &d, px, (size_t)w * (size_t)h * 4, &l, &e);
}

void *r2d_wg_depth_new(int w, int h) {
  WGPUTextureDescriptor d = { .label = SV("r2d depth-stencil"), .usage = WGPUTextureUsage_RenderAttachment,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, 1 },
                              .format = WGPUTextureFormat_Depth24PlusStencil8, .mipLevelCount = 1, .sampleCount = 1 };
  return wgpuDeviceCreateTexture(W.dev, &d);
}

void r2d_wg_tex_release(void *tex) {
  if (tex) wgpuTextureRelease((WGPUTexture)tex);
}

/* a render-attachment view of one layer */
static WGPUTextureView layer_view(WGPUTexture t, int layer) {
  WGPUTextureViewDescriptor d = { .format = wgpuTextureGetFormat(t), .dimension = WGPUTextureViewDimension_2D,
                                  .mipLevelCount = 1, .baseArrayLayer = (uint32_t)layer, .arrayLayerCount = 1,
                                  .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_RenderAttachment };
  return wgpuTextureCreateView(t, &d);
}
static WGPUTextureView array_view(WGPUTexture t) {
  WGPUTextureViewDescriptor d = { .format = wgpuTextureGetFormat(t), .dimension = WGPUTextureViewDimension_2DArray,
                                  .mipLevelCount = wgpuTextureGetMipLevelCount(t), .arrayLayerCount = wgpuTextureGetDepthOrArrayLayers(t),
                                  .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_TextureBinding };
  return wgpuTextureCreateView(t, &d);
}

static void submit(WGPUCommandEncoder enc) {
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(W.q, 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
}

void r2d_wg_tex_fill(void *tex, int from, int to, const uint8_t rgba[4]) {
  if (!tex || from >= to) return;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(W.dev, NULL);
  for (int l = from; l < to; l++) {
    WGPUTextureView v = layer_view((WGPUTexture)tex, l);
    WGPURenderPassColorAttachment ca = { .view = v, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                         .storeOp = WGPUStoreOp_Store,
                                         .clearValue = { rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0, rgba[3] / 255.0 } };
    WGPURenderPassDescriptor rp = { .colorAttachmentCount = 1, .colorAttachments = &ca };
    WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderEnd(p);
    wgpuRenderPassEncoderRelease(p);
    wgpuTextureViewRelease(v);
  }
  submit(enc);
}

void r2d_wg_tex_copy(void *src, void *dst, int layers) {
  if (!src || !dst || layers <= 0) return;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(W.dev, NULL);
  WGPUTexelCopyTextureInfo s = { .texture = src, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyTextureInfo d = { .texture = dst, .aspect = WGPUTextureAspect_All };
  WGPUExtent3D e = { wgpuTextureGetWidth(src), wgpuTextureGetHeight(src), (uint32_t)layers };
  wgpuCommandEncoderCopyTextureToTexture(enc, &s, &d, &e);
  submit(enc);
}

void r2d_wg_tex_write(void *tex, int x, int y, int layer, int w, int h, const uint8_t *px) {
  WGPUTexelCopyTextureInfo d = { .texture = tex, .origin = { (uint32_t)x, (uint32_t)y, (uint32_t)layer },
                                 .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = (uint32_t)w * 4, .rowsPerImage = (uint32_t)h };
  WGPUExtent3D e = { (uint32_t)w, (uint32_t)h, 1 };
  wgpuQueueWriteTexture(W.q, &d, px, (size_t)w * (size_t)h * 4, &l, &e);
}

void r2d_wg_array_changed(r2d_array *a) {
  if (a->wbg) wgpuBindGroupRelease((WGPUBindGroup)a->wbg);
  a->wbg = NULL;
}

static WGPUBindGroup tex_group4(WGPUTextureView a, WGPUSampler s, WGPUTextureView b, WGPUTextureView c) {
  WGPUBindGroupEntry e[4] = { { .binding = 0, .textureView = a }, { .binding = 1, .sampler = s },
                              { .binding = 2, .textureView = b }, { .binding = 3, .textureView = c } };
  WGPUBindGroupDescriptor d = { .layout = W.bgl_tex, .entryCount = 4, .entries = e };
  return wgpuDeviceCreateBindGroup(W.dev, &d);
}

static WGPUSampler sampler_for(bool nearest, bool repeat) { return W.samplers[(nearest ? 1 : 0) | (repeat ? 2 : 0)]; }
static WGPUSampler sampler_mip(void) { return W.samplers[4]; } /* linear, clamped, between mip levels */

/* (colour, sampler, normal-or-dummy) of an array, cached on it */
static WGPUBindGroup array_group(int ai) {
  if (ai < 0 || ai >= R.arrays.count || ai == (int)R2D_NO_ARRAY) return NULL;
  r2d_array *a = &R.arrays.data[ai];
  if (!a->wbg) {
    WGPUTextureView cv = array_view(a->wtex);
    WGPUTextureView nv = a->wntex ? array_view(a->wntex) : W.dummy_view;
    WGPUTextureView mv = a->wmtex ? array_view(a->wmtex) : W.dummy_mat_view;
    a->wbg = tex_group4(cv, a->levels > 1 ? sampler_mip() : sampler_for(a->nearest, a->repeat), nv, mv);
    wgpuTextureViewRelease(cv);
    if (a->wntex) wgpuTextureViewRelease(nv);
    if (a->wmtex) wgpuTextureViewRelease(mv);
  }
  return (WGPUBindGroup)a->wbg;
}
static WGPUBindGroup dummy_group;

/* ── buffers ───────────────────────────────────────────────────────────── */

static WGPUBuffer make_buffer(size_t bytes, WGPUBufferUsage usage, const char *label) {
  WGPUBufferDescriptor d = { .label = SV(label), .usage = usage | WGPUBufferUsage_CopyDst, .size = (bytes + 3) & ~(size_t)3 };
  return wgpuDeviceCreateBuffer(W.dev, &d);
}

static void ensure(WGPUBuffer *b, size_t *cap, size_t need, WGPUBufferUsage usage, const char *label) {
  if (*b && *cap >= need) return;
  size_t n = *cap ? *cap : 4096;
  while (n < need) n *= 2;
  uint64_t max = cw__wgpu_limits()->maxBufferSize; /* doubling past the device's largest buffer: stop at it */
  if (max && n > max && need <= max) n = (size_t)max & ~(size_t)3;
  if (*b) wgpuBufferRelease(*b);
  *b = make_buffer(n, usage, label);
  *cap = n;
}

/* A big stream goes up in pieces of at most WRITE_PIECE bytes (one wgpuQueueWriteBuffer of a ~6 MB index stream was
 * seen to abort inside Dawn on a native run); queue order still lands every piece before the draws. */
#define WRITE_PIECE ((size_t)4 << 20)
static void write_padded(WGPUBuffer b, const void *data, size_t bytes) {
  if (!bytes) return;
  size_t whole = bytes & ~(size_t)3;
  for (size_t at = 0; at < whole; at += WRITE_PIECE)
    wgpuQueueWriteBuffer(W.q, b, at, (const uint8_t *)data + at, CW_MIN(WRITE_PIECE, whole - at));
  if (bytes & 3) { /* writeBuffer sizes are multiples of 4: the tail padded with zeros */
    uint8_t tail[4] = { 0 };
    memcpy(tail, (const uint8_t *)data + whole, bytes - whole);
    wgpuQueueWriteBuffer(W.q, b, whole, tail, 4);
  }
}

/* A stream larger than the device's largest buffer cannot be drawn in one pass: say so (the first time) and draw
 * nothing, where creating the buffer would lose the device. */
static bool stream_fits(size_t vb, size_t ib) {
  uint64_t max = cw__wgpu_limits()->maxBufferSize;
  static long long test_max = -1; /* CW_TEST_R2D_STREAM_MAX=<bytes>: a smaller limit (tests/r2d-big-stream.mjs) */
  if (test_max < 0) test_max = getenv("CW_TEST_R2D_STREAM_MAX") ? atoll(getenv("CW_TEST_R2D_STREAM_MAX")) : 0;
  if (test_max > 0) max = (uint64_t)test_max;
  if (!max || (vb <= max && ib <= max)) return true;
  static bool said;
  if (!said) {
    said = true;
    cw_error("render2d: a 2D pass of %d vertices (%.1f MB) and %d indices (%.1f MB) is larger than this device's largest buffer "
             "(%.1f MB): the pass was not drawn; split it into several passes or draw fewer things",
             R.verts.count, (double)vb / 1048576.0, R.idx.count, (double)ib / 1048576.0, (double)max / 1048576.0);
  }
  return false;
}

void *r2d_wg_buffer(void *old, const void *data, size_t bytes, bool index) {
  WGPUBuffer b = old;
  size_t have = b ? (size_t)wgpuBufferGetSize(b) : 0;
  if (!b || have < bytes) {
    if (b) wgpuBufferRelease(b);
    b = make_buffer(bytes ? bytes : 16, index ? WGPUBufferUsage_Index : WGPUBufferUsage_Vertex, "r2d chunk");
  }
  write_padded(b, data, bytes);
  return b;
}

void r2d_wg_buffer_release(void *buf) {
  if (buf) wgpuBufferRelease((WGPUBuffer)buf);
}

/* ── pipelines ─────────────────────────────────────────────────────────── */

static WGPUBlendState blend_of(int b) {
  WGPUBlendComponent alpha = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
  switch (b) {
  case B_MULTIPLY:
    return (WGPUBlendState){ { WGPUBlendOperation_Add, WGPUBlendFactor_Dst, WGPUBlendFactor_OneMinusSrcAlpha },
                             { WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One } };
  case B_ADD:
    return (WGPUBlendState){ { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One },
                             { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One } };
  default: return (WGPUBlendState){ alpha, alpha };
  }
}

static WGPURenderPipeline pipeline(int prog, int blend, WGPUTextureFormat fmt) {
  /* the pass's depth (culling, cull.c): none, or the group's depth tested (W.pass_ds) */
  bool pre = prog == P_SPRITE_PRE || prog == P_TILE_PRE;
  bool zt = W.pass_ds && !pre;
  uint32_t key = (uint32_t)prog | ((uint32_t)blend << 5) | ((uint32_t)fmt << 8) | (zt ? 1u << 24 : 0);
  for (int i = 0; i < W.pipes.count; i++)
    if (W.pipes.data[i].key == key) return W.pipes.data[i].p;
  static const WGPUVertexAttribute stream_attrs[5] = {
    { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 },
    { .format = WGPUVertexFormat_Float32x2, .offset = 8, .shaderLocation = 1 },
    { .format = WGPUVertexFormat_Unorm8x4, .offset = 16, .shaderLocation = 2 },
    { .format = WGPUVertexFormat_Uint8x4, .offset = 20, .shaderLocation = 3 },
    { .format = WGPUVertexFormat_Unorm8x4, .offset = 24, .shaderLocation = 4 },
  };
  static const WGPUVertexAttribute tile_attrs[2] = {
    { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 },
    { .format = WGPUVertexFormat_Uint16x4, .offset = 8, .shaderLocation = 1 },
  };
  static const WGPUVertexAttribute quad_attrs[2] = {
    { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 },
    { .format = WGPUVertexFormat_Float32x2, .offset = 8, .shaderLocation = 1 },
  };
  WGPUVertexBufferLayout vbl;
  WGPUShaderModule sm;
  WGPUPipelineLayout pl;
  const char *fs = "fs";
  bool lit = prog == P_SPRITE_LIT || prog == P_TILE_LIT || prog == P_SPRITE_LIT_N || prog == P_TILE_LIT_N || prog == P_PART_LIT ||
             prog == P_PART_LIT_N || prog == P_INST_LIT || prog == P_INST_LIT_N;
  bool lit_n = prog == P_SPRITE_LIT_N || prog == P_TILE_LIT_N || prog == P_PART_LIT_N || prog == P_INST_LIT_N;
  bool part = prog == P_PART || prog == P_PART_LIT || prog == P_PART_LIT_N;
  bool inst = prog == P_INST || prog == P_INST_LIT || prog == P_INST_LIT_N;
  static const WGPUVertexAttribute inst_attrs[3] = {
    { .format = WGPUVertexFormat_Float32x4, .offset = 0, .shaderLocation = 0 },
    { .format = WGPUVertexFormat_Unorm8x4, .offset = 16, .shaderLocation = 1 },
    { .format = WGPUVertexFormat_Uint16x2, .offset = 20, .shaderLocation = 2 },
  };
  if (inst) {
    vbl = (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Instance, .arrayStride = 24, .attributeCount = 3, .attributes = inst_attrs };
    sm = W.sm_sprite;
    pl = W.pl_inst;
  } else if (prog == P_SPRITE || prog == P_SPRITE_LIT || prog == P_SPRITE_LIT_N || prog == P_SPRITE_DISTORT || prog == P_SPRITE_MASK || prog == P_SPRITE_PRE || part) {
    vbl = (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = sizeof(r2d_vertex), .attributeCount = 5, .attributes = stream_attrs };
    sm = W.sm_sprite;
    pl = part ? W.pl_part : W.pl_stream;
  } else if (prog == P_TILE || prog == P_TILE_LIT || prog == P_TILE_LIT_N || prog == P_TILE_MASK || prog == P_TILE_PRE) {
    vbl = (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 16, .attributeCount = 2, .attributes = tile_attrs };
    sm = W.sm_tile;
    pl = W.pl_small;
  } else {
    vbl = (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 16, .attributeCount = 2, .attributes = quad_attrs };
    sm = W.sm_quad;
    pl = prog == P_SHADOW ? W.pl_shadow : W.pl_small;
    fs = prog == P_LIGHT || prog == P_LIGHT_ST ? "fs_light" : prog == P_SHADOW ? "fs_shadow" : prog == P_COMPOSITE ? "fs_composite" : "fs_blit";
  }
  if (lit) fs = lit_n ? "fs_lit_n" : "fs_lit";
  if (prog == P_SPRITE_DISTORT) fs = "fs_distort";
  if (prog == P_SPRITE_MASK || prog == P_TILE_MASK) fs = "fs_mask";
  if (pre) fs = "fs_pre";
  bool stream = prog == P_SPRITE || prog == P_SPRITE_LIT || prog == P_SPRITE_LIT_N || prog == P_SPRITE_PRE;
  static const WGPUVertexAttribute depth_attr = { .format = WGPUVertexFormat_Float32, .offset = 0, .shaderLocation = 9 };
  WGPUVertexBufferLayout vb2[2] = { vbl, { .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 4, .attributeCount = 1, .attributes = &depth_attr } };
  bool zvs = stream && (zt || pre); /* the stream with its depth buffer */
  WGPUBlendState bs = blend_of(blend);
  WGPUColorTargetState ct[3] = {
    { .format = fmt, .blend = blend == B_OPAQUE ? NULL : &bs, .writeMask = prog == P_SHADOW ? WGPUColorWriteMask_None : WGPUColorWriteMask_All },
    { .format = WGPUTextureFormat_RGBA8Unorm, .blend = blend == B_OPAQUE ? NULL : &bs, .writeMask = WGPUColorWriteMask_All },
    { .format = WGPUTextureFormat_RGBA8Unorm, .blend = blend == B_OPAQUE ? NULL : &bs, .writeMask = WGPUColorWriteMask_All },
  };
  WGPUFragmentState fst = { .module = sm, .entryPoint = SV(fs), .targetCount = pre ? 0 : lit ? (lit_n ? 2 : 3) : 1, .targets = ct };
  bool ds = zt || pre;
  WGPUStencilFaceState sf = { .compare = WGPUCompareFunction_Always, .failOp = WGPUStencilOperation_Keep,
                              .depthFailOp = WGPUStencilOperation_Keep, .passOp = WGPUStencilOperation_Keep };
  WGPUDepthStencilState dss = { .format = WGPUTextureFormat_Depth32Float, .depthWriteEnabled = pre ? WGPUOptionalBool_True : WGPUOptionalBool_False,
                                .depthCompare = WGPUCompareFunction_Less, .stencilFront = sf, .stencilBack = sf,
                                .stencilReadMask = 0, .stencilWriteMask = 0 };
  WGPURenderPipelineDescriptor d = {
    .label = SV("r2d"),
    .layout = pl,
    .vertex = { .module = sm, .entryPoint = SV(part ? "vs_particles" : inst ? "vs_instances" : zvs ? "vs_z" : "vs"), .bufferCount = part ? 0 : zvs ? 2 : 1,
                .buffers = zvs ? vb2 : &vbl },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
    .depthStencil = ds ? &dss : NULL,
    .multisample = { .count = 1, .mask = 0xffffffffu },
    .fragment = &fst,
  };
  WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(W.dev, &d);
  cw_array_push(&W.pipes, ((pipe_rec){ key, p }));
  return p;
}

/* ── init / shutdown ───────────────────────────────────────────────────── */

static WGPUBindGroupLayout uniform_layout(uint64_t size) {
  WGPUBindGroupLayoutEntry e = { .binding = 0, .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                                 .buffer = { .type = WGPUBufferBindingType_Uniform, .hasDynamicOffset = true, .minBindingSize = size } };
  WGPUBindGroupLayoutDescriptor d = { .entryCount = 1, .entries = &e };
  return wgpuDeviceCreateBindGroupLayout(W.dev, &d);
}

static WGPUShaderModule module(const char *label, const char *body) {
  size_t n = strlen(WGSL_COMMON) + strlen(body) + 1;
  char *s = cw_alloc(n, CW_MEM_RENDER2D);
  snprintf(s, n, "%s%s", WGSL_COMMON, body);
  WGPUShaderModule m = cw__wgpu_shader(label, s);
  cw_free(s);
  return m;
}

bool r2d_wg_init(void) {
  if (W.inited) return true;
  W.dev = cw__wgpu_device();
  W.q = cw__wgpu_queue();
  if (!W.dev) {
    cw_error("render2d: WebGPU selected but no device");
    return false;
  }
  W.sm_sprite = module("r2d sprite", WGSL_SPRITE);
  W.sm_tile = module("r2d tile", WGSL_TILE);
  W.sm_quad = module("r2d quad", WGSL_QUAD);
  W.bgl_stream = uniform_layout(U_STREAM_SIZE);
  W.bgl_small = uniform_layout(U_SMALL_SIZE);
  WGPUBindGroupLayoutEntry te[4] = {
    { .binding = 0, .visibility = WGPUShaderStage_Fragment,
      .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2DArray } },
    { .binding = 1, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } },
    { .binding = 2, .visibility = WGPUShaderStage_Fragment,
      .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2DArray } },
    { .binding = 3, .visibility = WGPUShaderStage_Fragment,
      .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2DArray } },
  };
  W.bgl_tex = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 4, .entries = te });
  WGPUBindGroupLayout s2[2] = { W.bgl_stream, W.bgl_tex }, q2[2] = { W.bgl_small, W.bgl_tex };
  W.pl_stream = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 2, .bindGroupLayouts = s2 });
  W.bgl_part = uniform_layout(144);
  WGPUBindGroupLayout s3[3] = { W.bgl_stream, W.bgl_tex, W.bgl_part };
  W.pl_part = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 3, .bindGroupLayouts = s3 });
  {
    WGPUBindGroupLayoutEntry ie = { .binding = 1, .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                                    .buffer = { .type = WGPUBufferBindingType_Uniform, .hasDynamicOffset = true, .minBindingSize = R2D_INST_VEC4 * 16 } };
    W.bgl_inst = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 1, .entries = &ie });
    WGPUBindGroupLayout s4[3] = { W.bgl_stream, W.bgl_tex, W.bgl_inst };
    W.pl_inst = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 3, .bindGroupLayouts = s4 });
  }
  W.pl_small = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 2, .bindGroupLayouts = q2 });
  W.pl_shadow = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 1, .bindGroupLayouts = q2 });
  for (int i = 0; i < 8; i++) {
    WGPUFilterMode f = (i & 1) ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    WGPUAddressMode a = (i & 2) ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
    WGPUSamplerDescriptor sd = { .addressModeU = a, .addressModeV = a, .addressModeW = WGPUAddressMode_ClampToEdge,
                                 .magFilter = f, .minFilter = f,
                                 .mipmapFilter = (i & 4) ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest,
                                 .lodMinClamp = 0, .lodMaxClamp = 32, .maxAnisotropy = 1 };
    W.samplers[i] = wgpuDeviceCreateSampler(W.dev, &sd);
  }
  W.dummy = r2d_wg_tex_new(1, 1, 1);
  static const uint8_t none[4] = { 128, 128, 0, 255 };
  r2d_wg_tex_write(W.dummy, 0, 0, 0, 1, 1, none);
  W.dummy_view = array_view(W.dummy);
  W.dummy_mat = r2d_wg_tex_new(1, 1, 1);
  static const uint8_t nomat[4] = { 0, 0, 0, 0 };
  r2d_wg_tex_write(W.dummy_mat, 0, 0, 0, 1, 1, nomat);
  W.dummy_mat_view = array_view(W.dummy_mat);
  dummy_group = tex_group4(W.dummy_view, W.samplers[1], W.dummy_view, W.dummy_mat_view);
  /* the tile chunks' shared index buffer (GL: R.tile_ibo) */
  int quads = CW_TILE_CHUNK * CW_TILE_CHUNK;
  uint16_t *ix = cw_alloc(sizeof(uint16_t) * 6 * (size_t)quads, CW_MEM_RENDER2D);
  for (int qd = 0; qd < quads; qd++) {
    uint16_t b = (uint16_t)(qd * 4);
    uint16_t s[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3) };
    memcpy(ix + 6 * qd, s, sizeof s);
  }
  W.tile_ibuf = r2d_wg_buffer(NULL, ix, sizeof(uint16_t) * 6 * (size_t)quads, true);
  cw_free(ix);
  W.inited = true;
  return true;
}

static void yuv_shutdown(void);
static void fx_shutdown_wg(void);
void r2d_wg_shutdown(void) {
  fx_shutdown_wg();
  yuv_shutdown();
  if (!W.inited) return;
  for (int i = 0; i < W.pipes.count; i++) wgpuRenderPipelineRelease(W.pipes.data[i].p);
  cw_array_free(&W.pipes);
  if (W.cull_view) wgpuTextureViewRelease(W.cull_view);
  if (W.cull_tex) wgpuTextureRelease(W.cull_tex);
  WGPUBuffer bufs[] = { W.vbuf, W.ibuf, W.ubuf, W.sbuf, W.tile_ibuf, W.dbuf };
  for (size_t i = 0; i < CW_ARRAY_LEN(bufs); i++)
    if (bufs[i]) wgpuBufferRelease(bufs[i]);
  if (W.bg_stream) wgpuBindGroupRelease(W.bg_stream);
  if (W.bg_small) wgpuBindGroupRelease(W.bg_small);
  if (dummy_group) wgpuBindGroupRelease(dummy_group);
  dummy_group = NULL;
  wgpuTextureViewRelease(W.dummy_view);
  wgpuTextureRelease(W.dummy);
  wgpuTextureViewRelease(W.dummy_mat_view);
  wgpuTextureRelease(W.dummy_mat);
  for (int i = 0; i < 8; i++) wgpuSamplerRelease(W.samplers[i]);
  wgpuPipelineLayoutRelease(W.pl_stream);
  wgpuPipelineLayoutRelease(W.pl_part);
  wgpuPipelineLayoutRelease(W.pl_inst);
  wgpuBindGroupLayoutRelease(W.bgl_inst);
  if (W.bg_inst) wgpuBindGroupRelease(W.bg_inst);
  wgpuBindGroupLayoutRelease(W.bgl_part);
  if (W.bg_part) wgpuBindGroupRelease(W.bg_part);
  wgpuPipelineLayoutRelease(W.pl_small);
  wgpuPipelineLayoutRelease(W.pl_shadow);
  wgpuBindGroupLayoutRelease(W.bgl_stream);
  wgpuBindGroupLayoutRelease(W.bgl_small);
  wgpuBindGroupLayoutRelease(W.bgl_tex);
  wgpuShaderModuleRelease(W.sm_sprite);
  wgpuShaderModuleRelease(W.sm_tile);
  wgpuShaderModuleRelease(W.sm_quad);
  cw_array_free(&W.ustage);
  cw_array_free(&W.sstage);
  cw_array_free(&W.transient);
  memset(&W, 0, sizeof W);
}

/* ── recording ─────────────────────────────────────────────────────────── */

static uint32_t ualloc(const void *data, size_t size) {
  uint32_t off = (uint32_t)W.ustage.count;
  size_t blk = (size + UALIGN - 1) & ~(size_t)(UALIGN - 1);
  cw_array_reserve(&W.ustage, W.ustage.count + (int)blk);
  memset(W.ustage.data + off, 0, blk);
  memcpy(W.ustage.data + off, data, size);
  W.ustage.count += (int)blk;
  return off;
}

/* the small uniform block of a quad / tile draw for the current pass */
typedef struct qblock {
  float target[2], flip, mode;
  float v[6][4];
} qblock;
CW_STATIC_ASSERT(sizeof(qblock) == U_SMALL_SIZE, "small uniform block layout");

static qblock qb(void) {
  qblock b;
  memset(&b, 0, sizeof b);
  b.target[0] = (float)W.pass_w;
  b.target[1] = (float)W.pass_h;
  b.flip = W.pass_flip;
  return b;
}

/* 6 vertices (two triangles) of a quad into the scratch stage: first vertex */
static uint32_t squad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
  uint32_t first = (uint32_t)(W.sstage.count / 4);
  float q[24] = { x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y0, u1, v0, x1, y1, u1, v1, x0, y1, u0, v1 };
  for (int i = 0; i < 24; i++) cw_array_push(&W.sstage, q[i]);
  return first;
}

/* ── GPU timers (CW_R2D_TIMERS, timers.c): each render pass of a part writes
 * timestamps at its beginning and end; a part's time is its passes' sum ── */

#define TQ 128
static struct {
  WGPUQuerySet qs;
  WGPUBuffer resolve, readback;
  int used, part[TQ / 2], open; /* open: the part now marked (-1 none) */
  int rpairs, rpart[TQ / 2];
  bool mapping, requested;
} TW = { .open = -1 };

bool r2d_wg_timers_init(void) {
  if (!cw__wgpu_timestamps()) {
    cw_warn("render2d: CW_R2D_TIMERS on a WebGPU device without timestamps: no GPU times");
    return false;
  }
  TW.qs = wgpuDeviceCreateQuerySet(W.dev, &(WGPUQuerySetDescriptor){ .label = SV("r2d timers"), .type = WGPUQueryType_Timestamp, .count = TQ });
  TW.resolve = wgpuDeviceCreateBuffer(W.dev, &(WGPUBufferDescriptor){ .usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc, .size = TQ * 8 });
  TW.readback = wgpuDeviceCreateBuffer(W.dev, &(WGPUBufferDescriptor){ .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, .size = TQ * 8 });
  return true;
}

bool r2d_wg_timer_mark(int part, bool end) {
  if (!TW.qs || TW.mapping) return false;
  TW.open = end ? -1 : part;
  return true;
}

/* timestamp writes for a render pass about to begin (NULL when untimed) */
static const WGPUPassTimestampWrites *pass_ts(WGPUPassTimestampWrites *tw) {
  if (!TW.qs || TW.mapping || TW.open < 0 || TW.used + 2 > TQ) return NULL;
  TW.part[TW.used / 2] = TW.open;
  *tw = (WGPUPassTimestampWrites){ .querySet = TW.qs, .beginningOfPassWriteIndex = (uint32_t)TW.used, .endOfPassWriteIndex = (uint32_t)TW.used + 1 };
  TW.used += 2;
  return tw;
}

void r2d_wg_timers_resolve(void) {
  if (!TW.qs || TW.mapping || !TW.used) return;
  wgpuCommandEncoderResolveQuerySet(W.enc, TW.qs, 0, (uint32_t)TW.used, TW.resolve, 0);
  wgpuCommandEncoderCopyBufferToBuffer(W.enc, TW.resolve, 0, TW.readback, 0, (uint64_t)TW.used * 8);
  TW.rpairs = TW.used / 2;
  memcpy(TW.rpart, TW.part, sizeof TW.part);
  TW.used = 0;
  TW.mapping = true;
  TW.requested = false;
}

static void begin_pass(WGPUTextureView color, WGPUTextureFormat fmt, int w, int h, bool offscreen, bool clear, const double cc[4],
                       WGPUTextureView normal, WGPUTextureView ds, WGPUTextureView mat) {
  WGPURenderPassColorAttachment ca[3] = {
    { .view = color, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load,
      .storeOp = WGPUStoreOp_Store, .clearValue = clear ? (WGPUColor){ cc[0], cc[1], cc[2], cc[3] } : (WGPUColor){ 0, 0, 0, 0 } },
    { .view = normal, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
      .clearValue = { 0, 0, 0, 0 } },
    { .view = mat, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
      .clearValue = { 0, 0, 0, 0 } },
  };
  /* ds: depth culling's depth buffer (cull.c), kept (W.ds_clear: cleared, the pre-pass) */
  WGPURenderPassDepthStencilAttachment da = { .view = ds, .depthLoadOp = W.ds_clear ? WGPULoadOp_Clear : WGPULoadOp_Load,
                                              .depthStoreOp = WGPUStoreOp_Store, .depthClearValue = 1.0f };
  WGPUPassTimestampWrites tw;
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = !color ? 0 : mat ? 3 : normal ? 2 : 1, .colorAttachments = ca, .depthStencilAttachment = ds ? &da : NULL,
                                  .timestampWrites = pass_ts(&tw) };
  W.pass = wgpuCommandEncoderBeginRenderPass(W.enc, &rp);
  W.pass_fmt = fmt;
  W.pass_ds = ds != NULL;
  W.pass_w = w;
  W.pass_h = h;
  W.pass_flip = offscreen ? -1.0f : 1.0f;
}

static void end_pass(void) {
  if (!W.pass) return;
  wgpuRenderPassEncoderEnd(W.pass);
  wgpuRenderPassEncoderRelease(W.pass);
  W.pass = NULL;
}

/* scissor from a canvas-space rect (clip 0 = the whole target) */
static void set_clip(uint32_t clip) {
  int x0 = 0, y0 = 0, x1 = W.pass_w, y1 = W.pass_h;
  if (clip) {
    cw_rect c = r2d_clip_px(R.clips.data[clip]);
    int cx0 = (int)(c.x + 0.5f), cy0 = (int)(c.y + 0.5f), cx1 = (int)(c.x + c.w + 0.5f), cy1 = (int)(c.y + c.h + 0.5f);
    if (W.pass_flip < 0) { /* GL rows: y from the bottom */
      int t = W.pass_h - cy1;
      cy1 = W.pass_h - cy0;
      cy0 = t;
    }
    x0 = CW_CLAMP(cx0, 0, W.pass_w);
    x1 = CW_CLAMP(cx1, x0, W.pass_w);
    y0 = CW_CLAMP(cy0, 0, W.pass_h);
    y1 = CW_CLAMP(cy1, y0, W.pass_h);
  }
  wgpuRenderPassEncoderSetScissorRect(W.pass, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
}

static void draw_scratch(uint32_t first, uint32_t count) {
  wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.sbuf, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(W.pass, count, 1, first, 0);
  R.stats.draw_calls++;
}

static WGPUTextureView keep_view(WGPUTextureView v); /* released after the submit */
static CW_ARRAY(WGPUTextureView) transient_views;
static WGPUTextureView keep_view(WGPUTextureView v) {
  cw_array_push(&transient_views, v);
  return v;
}
static WGPUBindGroup keep_group(WGPUBindGroup g) {
  cw_array_push(&W.transient, g);
  return g;
}

static WGPUTexture tex_of(cw_texture t) { return (WGPUTexture)R.arrays.data[r2d_tex_get(t)->array].wtex; }

/* tilemaps: tilemap.c calls these from r2d_tilemap_render inside the pass,
 * with the blend of the batch being drawn */
static int cur_blend_wg;
void r2d_wg_tilemap_begin(const float u[24], int array, bool lit) {
  qblock b = qb();
  memcpy(b.v, u, sizeof b.v);
  uint32_t off = ualloc(&b, sizeof b);
  /* lit: the scene pass has its normal attachment (normals in use) */
  int prog = R.mask_mode ? P_TILE_MASK : R.depth_mode == 1 ? P_TILE_PRE : lit ? (R.material_used ? P_TILE_LIT : P_TILE_LIT_N) : P_TILE;
  wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(prog, R.mask_mode || R.depth_mode ? B_OPAQUE : cur_blend_wg, W.pass_fmt));
  wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_small, 1, &off);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 1, array_group(array), 0, NULL);
  wgpuRenderPassEncoderSetIndexBuffer(W.pass, W.tile_ibuf, WGPUIndexFormat_Uint16, 0, WGPU_WHOLE_SIZE);
}
void r2d_wg_particles(const r2d_part_draw *d, bool lit) {
  int prog = lit ? (R.material_used ? P_PART_LIT : P_PART_LIT_N) : P_PART;
  wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(prog, cur_blend_wg, W.pass_fmt));
  uint32_t off = W.stream_off[W.pass_flip < 0 ? 0 : 1];
  wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_stream, 1, &off);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 1, d->array == R2D_NO_ARRAY ? dummy_group : array_group((int)d->array), 0, NULL);
  uint32_t po = ualloc(d->u, sizeof d->u);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 2, W.bg_part, 1, &po);
  wgpuRenderPassEncoderDraw(W.pass, 6, (uint32_t)d->count, 0, 0);
  R.stats.draw_calls++;
}
void r2d_wg_instances(const r2d_inst_draw *d, bool lit, void *stream_buf) {
  int prog = lit ? (R.material_used ? P_INST_LIT : P_INST_LIT_N) : P_INST;
  WGPUBuffer vb = d->wbuf ? (WGPUBuffer)d->wbuf : (WGPUBuffer)stream_buf;
  if (!vb) return;
  wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(prog, cur_blend_wg, W.pass_fmt));
  uint32_t off = W.stream_off[W.pass_flip < 0 ? 0 : 1];
  wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_stream, 1, &off);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 1, d->array == R2D_NO_ARRAY ? dummy_group : array_group((int)d->array), 0, NULL);
  uint32_t io = ualloc(d->u, sizeof d->u);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 2, W.bg_inst, 1, &io);
  wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, vb, d->offset, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(W.pass, 6, (uint32_t)d->count, 0, 0);
  R.stats.draw_calls++;
}
void *r2d_wg_instance_buffer(void *old, size_t bytes) {
  if (old) wgpuBufferRelease((WGPUBuffer)old);
  return make_buffer(bytes ? bytes : 16, WGPUBufferUsage_Vertex, "r2d instances");
}
void r2d_wg_buffer_write(void *buf, size_t offset, const void *data, size_t bytes) {
  if (!buf || !bytes) return;
  wgpuQueueWriteBuffer(W.q, (WGPUBuffer)buf, offset, data, (bytes + 3) & ~(size_t)3);
}
void r2d_wg_tilemap_chunk(void *vbuf, int quads) {
  wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, (WGPUBuffer)vbuf, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDrawIndexed(W.pass, (uint32_t)quads * 6, 1, 0, 0, 0);
}


/* the target the canvas pass draws into */
typedef struct canvas_target {
  WGPUTextureView view;
  WGPUTextureFormat fmt;
  bool offscreen;
} canvas_target;

static void begin_canvas(const canvas_target *ct, bool clear, const double cc[4]) {
  begin_pass(ct->view, ct->fmt, R.canvas_w, R.canvas_h, ct->offscreen, clear, cc, NULL, W.cull_on && ct->offscreen ? W.cull_view : NULL, 0);
}

/* depth culling (cull.c): the group's opaque texels, nearest first, into the depth buffer (no colour) */
static void cull_prepass_wg(const r2d_cull_plan *cp, const r2d_batch *b, int from, int n, int g) {
  if (!W.cull_tex || W.cull_w != R.canvas_w || W.cull_h != R.canvas_h) {
    if (W.cull_view) wgpuTextureViewRelease(W.cull_view);
    if (W.cull_tex) wgpuTextureRelease(W.cull_tex);
    WGPUTextureDescriptor td = { .label = SV("r2d cull depth"), .usage = WGPUTextureUsage_RenderAttachment, .dimension = WGPUTextureDimension_2D,
                                 .size = { (uint32_t)R.canvas_w, (uint32_t)R.canvas_h, 1 }, .format = WGPUTextureFormat_Depth32Float,
                                 .mipLevelCount = 1, .sampleCount = 1 };
    W.cull_tex = wgpuDeviceCreateTexture(W.dev, &td);
    W.cull_view = wgpuTextureCreateView(W.cull_tex, NULL);
    W.cull_w = R.canvas_w, W.cull_h = R.canvas_h;
  }
  end_pass();
  W.ds_clear = true;
  begin_pass(NULL, WGPUTextureFormat_Undefined, R.canvas_w, R.canvas_h, true, false, (double[4]){ 0, 0, 0, 0 }, NULL, W.cull_view, NULL);
  W.ds_clear = false;
  int last = from;
  while (last < n && cp->grp[last] == g) last++;
  int cur = -1;
  uint32_t cur_array = 0xffffffffu, cur_clip = 0xffffffffu;
  for (int i = last - 1; i >= from; i--) {
    if (!cp->occ[i]) continue;
    uint32_t clip = R2D_ST_CLIP(b[i].state);
    if (clip != cur_clip) set_clip(cur_clip = clip);
    if (R2D_ST_KIND(b[i].state) == R2D_KIND_TILEMAP) {
      R.depth_mode = 1;
      R.tile_depth = r2d_cull_depth_t(2.0f * (float)(b[i].first / 3));
      r2d_tilemap_render(&R.tdraws.data[b[i].payload], false, R.canvas_h);
      R.depth_mode = 0;
      cur = -1;
      cur_array = 0xffffffffu;
      continue;
    }
    if (cur != P_SPRITE_PRE) {
      wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(P_SPRITE_PRE, B_OPAQUE, WGPUTextureFormat_Undefined));
      uint32_t off = W.stream_off[2]; /* offscreen, no bias: each draw's own depth */
      wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_stream, 1, &off);
      wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.vbuf, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderSetVertexBuffer(W.pass, 1, W.dbuf, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderSetIndexBuffer(W.pass, W.ibuf, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
      cur = P_SPRITE_PRE;
    }
    uint32_t arr = R2D_ST_ARRAY(b[i].state);
    if (arr != cur_array) {
      cur_array = arr;
      wgpuRenderPassEncoderSetBindGroup(W.pass, 1, arr == R2D_NO_ARRAY ? dummy_group : array_group((int)arr), 0, NULL);
    }
    wgpuRenderPassEncoderDrawIndexed(W.pass, b[i].count, 1, b[i].first, 0, 0);
    R.stats.draw_calls++;
  }
  end_pass();
}

/* ── fx: targets of any format and full-screen passes (fx.h) ─────────── */

static const char *WGSL_FX_HEAD =
    "struct FX { size: vec2f, flip: f32, pad: f32, v: array<vec4f, 8> };\n"
    "@group(0) @binding(0) var<uniform> fx: FX;\n"
    "@group(1) @binding(0) var t0: texture_2d<f32>;\n"
    "@group(1) @binding(1) var t1: texture_2d<f32>;\n"
    "@group(1) @binding(2) var t2: texture_2d<f32>;\n"
    "@group(1) @binding(3) var t3: texture_2d<f32>;\n"
    "@group(1) @binding(4) var s_lin: sampler;\n"
    "@group(1) @binding(5) var s_near: sampler;\n"
    "@group(1) @binding(6) var t4: texture_2d<f32>;\n"
    "@group(1) @binding(7) var t5: texture_2d<f32>;\n";
/* the full-screen vertex stage of fx programs */
static const char *WGSL_FX_VS =
    "struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  var o: VO;\n"
    "  o.pos = vec4f(p.x * 2.0 - 1.0, (p.y * 2.0 - 1.0) * fx.flip, 0.0, 1.0);\n"
    "  o.uv = p;\n"
    "  return o;\n"
    "}\n";
#define U_FX_SIZE 144u

typedef struct fx_pipe { const fx_prog *p; int fmt, blend; WGPURenderPipeline pipe; } fx_pipe;
static struct {
  WGPUBindGroupLayout bgl_u, bgl_t;
  WGPUPipelineLayout pl;
  WGPUBindGroup bg_u;   /* over W.ubuf (remade when it grows) */
  WGPUBuffer bg_u_buf;
  WGPUTexture black;
  WGPUTextureView black_view;
  CW_ARRAY(fx_pipe) pipes;
} FX;

int r2d_wg_fx_format(fx_fmt f) {
  return f == FX_RGBA16F ? WGPUTextureFormat_RGBA16Float : f == FX_RG11B10F ? WGPUTextureFormat_RG11B10Ufloat : WGPUTextureFormat_RGBA8Unorm;
}
bool r2d_wg_small_hdr_ok(void) { return wgpuDeviceHasFeature(W.dev, WGPUFeatureName_RG11B10UfloatRenderable); }

void *r2d_wg_fx_tex(int w, int h, fx_fmt f) {
  WGPUTextureDescriptor d = { .label = SV("r2d fx"),
                              .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc |
                                       WGPUTextureUsage_CopyDst,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, 1 },
                              .format = (WGPUTextureFormat)r2d_wg_fx_format(f), .mipLevelCount = 1, .sampleCount = 1 };
  return wgpuDeviceCreateTexture(W.dev, &d);
}

static WGPUTextureView fx_tex_view(WGPUTexture t) {
  WGPUTextureViewDescriptor d = { .format = wgpuTextureGetFormat(t), .dimension = WGPUTextureViewDimension_2D, .mipLevelCount = 1,
                                  .arrayLayerCount = 1, .aspect = WGPUTextureAspect_All };
  return wgpuTextureCreateView(t, &d);
}

void *r2d_wg_fx_view(void *tex) { return tex ? keep_view(fx_tex_view((WGPUTexture)tex)) : NULL; }

static void fx_init(void) {
  if (FX.bgl_u) return;
  WGPUBindGroupLayoutEntry ue = { .binding = 0, .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                                  .buffer = { .type = WGPUBufferBindingType_Uniform, .hasDynamicOffset = true, .minBindingSize = U_FX_SIZE } };
  FX.bgl_u = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 1, .entries = &ue });
  WGPUBindGroupLayoutEntry te[8];
  memset(te, 0, sizeof te);
  for (int k = 0; k < 8; k++)
    te[k] = (WGPUBindGroupLayoutEntry){ .binding = (uint32_t)k, .visibility = WGPUShaderStage_Fragment,
                                        .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2D } };
  te[4] = (WGPUBindGroupLayoutEntry){ .binding = 4, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } };
  te[5] = (WGPUBindGroupLayoutEntry){ .binding = 5, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } };
  FX.bgl_t = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 8, .entries = te });
  WGPUBindGroupLayout two[2] = { FX.bgl_u, FX.bgl_t };
  FX.pl = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 2, .bindGroupLayouts = two });
  FX.black = r2d_wg_fx_tex(1, 1, FX_RGBA8);
  static const uint8_t z[4] = { 0, 0, 0, 0 };
  WGPUTexelCopyTextureInfo d = { .texture = FX.black, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = 4, .rowsPerImage = 1 };
  wgpuQueueWriteTexture(W.q, &d, z, 4, &l, &(WGPUExtent3D){ 1, 1, 1 });
  FX.black_view = fx_tex_view(FX.black);
}

static void lw_shutdown(void);
static void timers_shutdown(void);
static void fx_shutdown_wg(void) {
  lw_shutdown();
  timers_shutdown();
  for (int i = 0; i < FX.pipes.count; i++) wgpuRenderPipelineRelease(FX.pipes.data[i].pipe);
  cw_array_free(&FX.pipes);
  if (FX.bg_u) wgpuBindGroupRelease(FX.bg_u);
  if (FX.black_view) wgpuTextureViewRelease(FX.black_view);
  if (FX.black) wgpuTextureRelease(FX.black);
  if (FX.pl) wgpuPipelineLayoutRelease(FX.pl);
  if (FX.bgl_u) wgpuBindGroupLayoutRelease(FX.bgl_u);
  if (FX.bgl_t) wgpuBindGroupLayoutRelease(FX.bgl_t);
  memset(&FX, 0, sizeof FX);
}

void r2d_wg_fx_prog_free(fx_prog *p) {
  if (p->wmod) wgpuShaderModuleRelease((WGPUShaderModule)p->wmod);
  p->wmod = NULL;
}

static WGPURenderPipeline fx_pipeline(fx_prog *p, int fmt, int blend) {
  for (int i = 0; i < FX.pipes.count; i++)
    if (FX.pipes.data[i].p == p && FX.pipes.data[i].fmt == fmt && FX.pipes.data[i].blend == blend) return FX.pipes.data[i].pipe;
  if (!p->wmod) {
    size_t n = strlen(WGSL_FX_HEAD) + strlen(WGSL_FX_VS) + strlen(p->wgsl) + 1;
    char *s = cw_alloc(n, CW_MEM_RENDER2D);
    snprintf(s, n, "%s%s%s", WGSL_FX_HEAD, WGSL_FX_VS, p->wgsl);
    p->wmod = cw__wgpu_shader(p->name, s);
    cw_free(s);
    if (!p->wmod) {
      p->failed = true;
      return NULL;
    }
  }
  WGPUBlendState bs = blend == FX_BLEND_ADD ? blend_of(B_ADD) : blend_of(B_ALPHA);
  WGPUColorTargetState ct = { .format = (WGPUTextureFormat)fmt, .blend = blend == FX_BLEND_NONE ? NULL : &bs,
                              .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = p->wmod, .entryPoint = SV("fs"), .targetCount = 1, .targets = &ct };
  WGPURenderPipelineDescriptor d = { .label = SV(p->name), .layout = FX.pl,
                                     .vertex = { .module = p->wmod, .entryPoint = SV("vs") },
                                     .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
                                     .multisample = { .count = 1, .mask = 0xffffffffu }, .fragment = &fs };
  WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(W.dev, &d);
  cw_array_push(&FX.pipes, ((fx_pipe){ p, fmt, blend, pipe }));
  return pipe;
}

void r2d_wg_fx_pass(fx_prog *p, const fx_dst *dst, bool clear, const float cc[4], const fx_rt *const src[FX_SRC], const float v[8][4],
                    int blend) {
  if (!W.enc) {
    cw_warn("render2d: an fx pass outside cw_r2d_end on WebGPU is not drawn (%s)", p->name);
    return;
  }
  fx_init();
  WGPURenderPipeline pipe = fx_pipeline(p, dst->wfmt, blend);
  if (!pipe) return;
  if (FX.bg_u_buf != W.ubuf) { /* the uniform buffer grew: a bind group over the new one */
    if (FX.bg_u) wgpuBindGroupRelease(FX.bg_u);
    WGPUBindGroupEntry e = { .binding = 0, .buffer = W.ubuf, .size = U_FX_SIZE };
    FX.bg_u = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = FX.bgl_u, .entryCount = 1, .entries = &e });
    FX.bg_u_buf = W.ubuf;
  }
  end_pass();
  double c4[4] = { cc ? cc[0] : 0, cc ? cc[1] : 0, cc ? cc[2] : 0, cc ? cc[3] : 0 };
  begin_pass((WGPUTextureView)dst->wview, (WGPUTextureFormat)dst->wfmt, dst->w, dst->h, dst->offscreen, clear, c4, NULL, NULL, 0);
  int vx = 0, vy = 0, vw = dst->w, vh = dst->h;
  if (dst->vp[2] > 0) vx = dst->vp[0], vy = dst->vp[1], vw = dst->vp[2], vh = dst->vp[3];
  /* vp is in GL rows (from the bottom); an offscreen target's memory is GL's, the window's is not */
  int wy = dst->offscreen ? vy : dst->h - vy - vh;
  wgpuRenderPassEncoderSetViewport(W.pass, (float)vx, (float)wy, (float)vw, (float)vh, 0, 1);
  struct { float size[2], flip, pad; float v[8][4]; } u;
  memset(&u, 0, sizeof u);
  u.size[0] = (float)vw;
  u.size[1] = (float)vh;
  u.flip = dst->offscreen ? -1.0f : 1.0f;
  if (v) memcpy(u.v, v, sizeof u.v);
  uint32_t off = ualloc(&u, sizeof u);
  WGPUBindGroupEntry e[8];
  memset(e, 0, sizeof e);
  for (int k = 0; k < FX_SRC; k++) {
    WGPUTextureView tv = src && src[k] && src[k]->wtex ? keep_view(fx_tex_view((WGPUTexture)src[k]->wtex)) : FX.black_view;
    e[k < 4 ? k : k + 2] = (WGPUBindGroupEntry){ .binding = (uint32_t)(k < 4 ? k : k + 2), .textureView = tv };
  }
  e[4] = (WGPUBindGroupEntry){ .binding = 4, .sampler = W.samplers[0] };
  e[5] = (WGPUBindGroupEntry){ .binding = 5, .sampler = W.samplers[1] };
  WGPUBindGroup g = keep_group(wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = FX.bgl_t, .entryCount = 8, .entries = e }));
  wgpuRenderPassEncoderSetPipeline(W.pass, pipe);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 0, FX.bg_u, 1, &off);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 1, g, 0, NULL);
  wgpuRenderPassEncoderDraw(W.pass, 3, 1, 0, 0);
  R.stats.draw_calls++;
  end_pass();
}


/* ── lights and the occluder mask (light.c) ────────────────────────────── */

/* the tiled lights (GL: light.c GL_LIGHT_VS / FS): fx.v[0] = normals, field
 * scale, field size; [1] = march steps, occluder depth, field reach; [2] =
 * tiles across, tile pixels, the tile table's first texel, the lists' first
 * texel; [3] = ambient. Its own bindings: t4 is the RGBA32F table */
static const char *WGSL_LIGHT =
    "struct FX { size: vec2f, flip: f32, pad: f32, v: array<vec4f, 8> };\n"
    "@group(0) @binding(0) var<uniform> fx: FX;\n"
    "@group(1) @binding(0) var t0: texture_2d<f32>;\n"
    "@group(1) @binding(1) var t1: texture_2d<f32>;\n"
    "@group(1) @binding(2) var t2: texture_2d<f32>;\n"
    "@group(1) @binding(3) var t3: texture_2d<f32>;\n"
    "@group(1) @binding(4) var t4: texture_2d<f32>;\n"
    "@group(1) @binding(5) var t_cookie: texture_2d_array<f32>;\n"
    "@group(1) @binding(6) var s_cookie: sampler;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  return vec4f(p.x * 2.0 - 1.0, (p.y * 2.0 - 1.0) * fx.flip, 0.0, 1.0);\n"
    "}\n"
    "fn dat(i: i32) -> vec4f { return textureLoad(t4, vec2i(i & 1023, i >> 10u), 0); }\n"
    "fn field(p: vec2f) -> f32 {\n"
    "  let q = p * fx.v[0].y;\n"
    "  let iq = clamp(vec2i(q), vec2i(0), vec2i(fx.v[0].zw) - 1);\n"
    "  let s = textureLoad(t1, iq, 0);\n"
    "  let sp = vec2f(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0) + 0.5;\n"
    "  return min(length(sp - q) / fx.v[0].y, fx.v[1].z);\n"
    "}\n"
    "fn shadow(p0: vec2f, rd: vec2f, maxt0: f32, k: f32, inside: bool) -> f32 {\n"
    "  let px = 0.75 / fx.v[0].y; let skip = 1.5 / fx.v[0].y;\n"
    "  var depth = 1.0; var p = p0; var maxt = maxt0;\n"
    "  if (inside) {\n"
    "    if (fx.v[1].y <= 0.0) { return 1.0; }\n"
    "    var s = 0.0; let D = fx.v[1].y;\n"
    "    for (var j = 0; j < 8; j++) {\n"
    "      s += D / 8.0;\n"
    "      if (textureLoad(t2, vec2i(p + rd * s), 0).r < 0.5) { break; }\n"
    "    }\n"
    "    if (s >= D) { return 0.0; }\n"
    "    depth = 1.0 - s / D;\n"
    "    depth *= depth;\n"
    "    p = p + rd * s; maxt = maxt - s;\n"
    "  }\n"
    "  var res = 1.0; var t = px;\n"
    "  let n = i32(fx.v[1].x);\n"
    "  for (var i = 0; i < 64; i++) {\n"
    "    if (i >= n || t >= maxt) { break; }\n"
    "    let h = field(p + rd * t);\n"
    "    if (h < px) { if (t > skip) { return 0.0; } t += px; continue; }\n"
    "    res = min(res, k * h / t);\n"
    "    t += max(h, px);\n"
    "  }\n"
    "  res = clamp(res, 0.0, 1.0);\n"
    "  return res * res * (3.0 - 2.0 * res) * depth;\n"
    "}\n"
    "struct LC { col: vec4f, add: vec4f };\n"
    "fn lights(pos: vec4f) -> LC {\n"
    "  let dv = fx.v[3].w;\n" /* > 1: a smaller light buffer, each texel shades the canvas pixel at its centre (light.c) */
    "  let fc = select(pos.xy, floor(pos.xy) * dv + floor(dv * 0.5) + 0.5, dv > 1.0);\n"
    "  let ip = vec2i(fc);\n"
    "  var N = vec3f(0.0, 0.0, 1.0); var mapped = 0.0;\n"
    "  if (fx.v[0].x > 0.5) {\n"
    "    let n = textureLoad(t0, ip, 0);\n"
    "    if (n.a > 0.004) { let e = n.rgb / n.a; mapped = smoothstep(0.0, 0.5, e.z); N = normalize(e * 2.0 - 1.0); }\n"
    "  }\n"
    "  let Nn = normalize(mix(vec3f(0.0, 0.0, 1.0), N, mapped));\n"
    "  var spec = 0.0; var gloss = 0.0; var cov = 0.0;\n"
    "  if (fx.v[0].x > 0.5) { let mt = textureLoad(t3, ip, 0); if (mt.a > 0.004) { spec = mt.g / mt.a; gloss = mt.b / mt.a; cov = mt.a; } }\n"
    "  let inside = fx.v[0].y > 0.0 && textureLoad(t2, ip, 0).r > 0.5;\n"
    "  var acc = fx.v[3].rgb; var accA = vec3f(0.0);\n"
    "  let tl = ip / i32(fx.v[2].y);\n"
    "  let hd = dat(i32(fx.v[2].z) + tl.y * i32(fx.v[2].x) + tl.x);\n"
    "  let first = i32(hd.x); let cnt = i32(hd.y);\n"
    "  for (var k = 0; k < cnt; k++) {\n"
    "    let e = first + k;\n"
    "    let li = i32(dat(i32(fx.v[2].w) + (e >> 2u))[e & 3]);\n"
    "    let c = dat(li * 6); let col = dat(li * 6 + 1); let s = dat(li * 6 + 2); let f = dat(li * 6 + 3);\n"
    "    let dirl = f.z > 0.5;\n"
    "    let d = fc - c.xy;\n"
    "    let dl = length(d);\n"
    "    var att = 1.0;\n"
    "    if (!dirl) { att = pow(clamp(1.0 - dl / c.z, 0.0, 1.0), col.w); }\n"
    "    if (!dirl && s.z <= 1.0) { att *= smoothstep(s.z, s.w, dot(d / max(dl, 1e-4), s.xy)); }\n"
    "    if (att <= 0.0) { continue; }\n"
    "    var lc = col.rgb;\n"
    "    if (f.w > 0.5) {\n" /* a light texture (GL: light.c) */
    "      let tr = dat(li * 6 + 4); let tp = dat(li * 6 + 5);\n"
    "      let lq = vec2f(dot(d, s.xy), dot(d, vec2f(-s.y, s.x))) / c.z;\n"
    "      let tq = clamp(vec2f(0.5 + 0.5 * lq.x, 0.5 - 0.5 * lq.y), vec2f(0.0), vec2f(1.0));\n"
    "      lc *= textureSampleLevel(t_cookie, s_cookie, mix(tr.xy, tr.zw, tq), u32(tp.x), 0.0).rgb;\n"
    "      if (max(lc.r, max(lc.g, lc.b)) <= 0.0) { continue; }\n"
    "    }\n"
    "    var L = normalize(vec3f(-d, c.w));\n"
    "    if (dirl) { L = normalize(vec3f(-s.xy, c.w)); }\n"
    "    let ndl = mix(1.0, max(dot(N, L), 0.0), mapped);\n"
    "    var sh = 1.0;\n"
    "    if (f.x > 0.0) {\n"
    "      if (dirl) { sh = shadow(fc, -s.xy, length(fx.v[0].zw / fx.v[0].y), f.x, inside); }\n"
    "      else if (dl > 1e-3) { sh = shadow(fc, -d / dl, dl, f.x, inside); }\n"
    "    }\n"
    "    acc += lc * att * ndl * sh;\n"
    "    var spv = 0.0;\n"
    "    if (spec > 0.0) {\n"
    "      let pw = exp2(2.0 + 8.0 * gloss);\n"
    "      let H = normalize(L + vec3f(0.0, 0.0, 1.0));\n"
    "      spv = spec * pow(max(dot(Nn, H), 0.0), pw) * (pw + 8.0) / 64.0 * max(dot(Nn, L), 0.0) * cov;\n"
    "    }\n"
    "    accA += lc * att * sh * (f.y + spv);\n"
    "  }\n"
    "  var o: LC;\n"
    "  o.col = vec4f(acc, 1.0);\n"
    "  o.add = vec4f(accA, 0.0);\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f { return lights(pos).col; }\n"
    "struct LT2 { @location(0) col: vec4f, @location(1) add: vec4f };\n"
    "@fragment fn fs_add(@builtin(position) pos: vec4f) -> LT2 { let l = lights(pos); var o: LT2; o.col = l.col; o.add = l.add; return o; }\n";

/* the occluder mask: solid triangles in canvas pixels (y down); fx.v[0].xy = canvas size */
static const char *WGSL_MASK =
    "@vertex fn vs(@location(0) a: vec2f) -> @builtin(position) vec4f {\n"
    "  return vec4f(a.x / fx.v[0].x * 2.0 - 1.0, (1.0 - a.y / fx.v[0].y * 2.0) * fx.flip, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4f { return vec4f(1.0); }\n";

static struct {
  WGPUShaderModule sm_light, sm_mask;
  WGPUPipelineLayout pl_u, pl_light;
  WGPUBindGroupLayout bgl_light; /* t0..t3 filterable, t4 the RGBA32F table */
  WGPUTexture data;
  int data_rows;
  WGPURenderPipeline mask;
  WGPURenderPipeline light[2][2]; /* [add][format: 0 RGBA8, 1 RGBA16F] */
} LW;

static WGPUShaderModule fx_module(const char *label, const char *body) {
  size_t n = strlen(WGSL_FX_HEAD) + strlen(body) + 1;
  char *s = cw_alloc(n, CW_MEM_RENDER2D);
  snprintf(s, n, "%s%s", WGSL_FX_HEAD, body);
  WGPUShaderModule m = cw__wgpu_shader(label, s);
  cw_free(s);
  return m;
}

static void fx_ubind(void) {
  if (FX.bg_u_buf == W.ubuf && FX.bg_u) return;
  if (FX.bg_u) wgpuBindGroupRelease(FX.bg_u);
  WGPUBindGroupEntry e = { .binding = 0, .buffer = W.ubuf, .size = U_FX_SIZE };
  FX.bg_u = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = FX.bgl_u, .entryCount = 1, .entries = &e });
  FX.bg_u_buf = W.ubuf;
}

static uint32_t fx_uniforms(int w, int h, bool offscreen, const float v[8][4]) {
  struct { float size[2], flip, pad; float v[8][4]; } u;
  memset(&u, 0, sizeof u);
  u.size[0] = (float)w;
  u.size[1] = (float)h;
  u.flip = offscreen ? -1.0f : 1.0f;
  if (v) memcpy(u.v, v, sizeof u.v);
  return ualloc(&u, sizeof u);
}

static void lw_init(void) {
  fx_init();
  if (LW.sm_light) return;
  LW.sm_light = cw__wgpu_shader("r2d light", WGSL_LIGHT);
  WGPUBindGroupLayoutEntry te[7];
  memset(te, 0, sizeof te);
  for (int k = 0; k < 5; k++)
    te[k] = (WGPUBindGroupLayoutEntry){ .binding = (uint32_t)k, .visibility = WGPUShaderStage_Fragment,
                                        .texture = { .sampleType = k == 4 ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float,
                                                     .viewDimension = WGPUTextureViewDimension_2D } };
  te[5] = (WGPUBindGroupLayoutEntry){ .binding = 5, .visibility = WGPUShaderStage_Fragment,
                                      .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2DArray } };
  te[6] = (WGPUBindGroupLayoutEntry){ .binding = 6, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } };
  LW.bgl_light = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 7, .entries = te });
  WGPUBindGroupLayout two[2] = { FX.bgl_u, LW.bgl_light };
  LW.pl_light = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 2, .bindGroupLayouts = two });
  LW.sm_mask = fx_module("r2d occluder mask", WGSL_MASK);
  LW.pl_u = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 1, .bindGroupLayouts = &FX.bgl_u });
}

static void lw_shutdown(void) {
  for (int a = 0; a < 2; a++)
    for (int f = 0; f < 2; f++)
      if (LW.light[a][f]) wgpuRenderPipelineRelease(LW.light[a][f]);
  if (LW.mask) wgpuRenderPipelineRelease(LW.mask);
  if (LW.pl_u) wgpuPipelineLayoutRelease(LW.pl_u);
  if (LW.pl_light) wgpuPipelineLayoutRelease(LW.pl_light);
  if (LW.bgl_light) wgpuBindGroupLayoutRelease(LW.bgl_light);
  if (LW.data) wgpuTextureRelease(LW.data);
  if (LW.sm_light) wgpuShaderModuleRelease(LW.sm_light);
  if (LW.sm_mask) wgpuShaderModuleRelease(LW.sm_mask);
  memset(&LW, 0, sizeof LW);
}

static WGPURenderPipeline light_pipeline(bool add, WGPUTextureFormat fmt) {
  int fi = fmt == WGPUTextureFormat_RGBA16Float ? 1 : 0;
  if (LW.light[add][fi]) return LW.light[add][fi];
  WGPUColorTargetState ct[2] = { { .format = fmt, .writeMask = WGPUColorWriteMask_All },
                                 { .format = fmt, .writeMask = WGPUColorWriteMask_All } };
  WGPUFragmentState fs = { .module = LW.sm_light, .entryPoint = SV(add ? "fs_add" : "fs"), .targetCount = add ? 2 : 1, .targets = ct };
  WGPURenderPipelineDescriptor d = { .label = SV("r2d light"), .layout = LW.pl_light,
                                     .vertex = { .module = LW.sm_light, .entryPoint = SV("vs") },
                                     .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
                                     .multisample = { .count = 1, .mask = 0xffffffffu }, .fragment = &fs };
  return LW.light[add][fi] = wgpuDeviceCreateRenderPipeline(W.dev, &d);
}

static uint32_t scratch_push(const float *f, int n) {
  uint32_t first = (uint32_t)W.sstage.count;
  cw_array_reserve(&W.sstage, W.sstage.count + n);
  memcpy(W.sstage.data + W.sstage.count, f, sizeof(float) * (size_t)n);
  W.sstage.count += n;
  return first; /* in floats */
}

void r2d_wg_mask_draw(void *mask, int w, int h, const float *tris, int nverts, float cw, float ch) {
  lw_init();
  fx_ubind();
  end_pass();
  WGPUTextureView v = keep_view(fx_tex_view((WGPUTexture)mask));
  double zero[4] = { 0, 0, 0, 0 };
  begin_pass(v, WGPUTextureFormat_RGBA8Unorm, w, h, true, true, zero, NULL, NULL, 0);
  if (nverts) {
    if (!LW.mask) {
      static const WGPUVertexAttribute at = { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 };
      WGPUVertexBufferLayout vbl = { .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 8, .attributeCount = 1, .attributes = &at };
      WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA8Unorm, .writeMask = WGPUColorWriteMask_All };
      WGPUFragmentState fs = { .module = LW.sm_mask, .entryPoint = SV("fs"), .targetCount = 1, .targets = &ct };
      WGPURenderPipelineDescriptor d = { .label = SV("r2d occluder mask"), .layout = LW.pl_u,
                                         .vertex = { .module = LW.sm_mask, .entryPoint = SV("vs"), .bufferCount = 1, .buffers = &vbl },
                                         .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
                                         .multisample = { .count = 1, .mask = 0xffffffffu }, .fragment = &fs };
      LW.mask = wgpuDeviceCreateRenderPipeline(W.dev, &d);
    }
    float uv[8][4] = { { cw, ch, 0, 0 } };
    uint32_t off = fx_uniforms(w, h, true, uv);
    uint32_t first = scratch_push(tris, nverts * 2);
    wgpuRenderPassEncoderSetPipeline(W.pass, LW.mask);
    wgpuRenderPassEncoderSetBindGroup(W.pass, 0, FX.bg_u, 1, &off);
    wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.sbuf, (uint64_t)first * 4, (uint64_t)nverts * 8);
    wgpuRenderPassEncoderDraw(W.pass, (uint32_t)nverts, 1, 0, 0);
    R.stats.draw_calls++;
  }
  end_pass();
}

void r2d_wg_mask_batches(void *mask, const r2d_batch *b, int n) {
  end_pass();
  double zero[4] = { 0, 0, 0, 0 };
  begin_pass(keep_view(fx_tex_view((WGPUTexture)mask)), WGPUTextureFormat_RGBA8Unorm, R.canvas_w, R.canvas_h, true, false, zero, NULL, NULL, NULL);
  int cur = -1;
  for (int i = 0; i < n; i++) {
    if (R2D_ST_KIND(b[i].state) == R2D_KIND_TILEMAP) {
      R.mask_mode = true;
      r2d_tilemap_render(&R.tdraws.data[b[i].payload], false, R.canvas_h);
      R.mask_mode = false;
      cur = -1;
      continue;
    }
    if (cur != P_SPRITE_MASK) {
      wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(P_SPRITE_MASK, B_OPAQUE, WGPUTextureFormat_RGBA8Unorm));
      uint32_t off = W.stream_off[0];
      wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_stream, 1, &off);
      wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.vbuf, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderSetIndexBuffer(W.pass, W.ibuf, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
      cur = P_SPRITE_MASK;
    }
    uint32_t arr = R2D_ST_ARRAY(b[i].state);
    wgpuRenderPassEncoderSetBindGroup(W.pass, 1, arr == R2D_NO_ARRAY ? dummy_group : array_group((int)arr), 0, NULL);
    wgpuRenderPassEncoderDrawIndexed(W.pass, b[i].count, 1, b[i].first, 0, 0);
    R.stats.draw_calls++;
  }
  end_pass();
}

void r2d_wg_lights_draw(int cookie_array, void *light, void *add, int fmt, const float amb[4], const float *data, int rows, const float up[4],
                        const float uq[4], const float ut[4], void *normal, void *seed, void *mask, void *mat, int w, int h) {
  lw_init();
  fx_ubind();
  end_pass();
  /* the table, grown in rows as needed */
  if (!LW.data || LW.data_rows < rows) {
    if (LW.data) wgpuTextureRelease(LW.data);
    LW.data_rows = CW_MAX(rows, LW.data_rows * 2);
    WGPUTextureDescriptor td = { .label = SV("r2d light table"), .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                 .dimension = WGPUTextureDimension_2D, .size = { 1024, (uint32_t)LW.data_rows, 1 },
                                 .format = WGPUTextureFormat_RGBA32Float, .mipLevelCount = 1, .sampleCount = 1 };
    LW.data = wgpuDeviceCreateTexture(W.dev, &td);
  }
  WGPUTexelCopyTextureInfo dst = { .texture = LW.data, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout lay = { .bytesPerRow = 1024 * 16, .rowsPerImage = (uint32_t)rows };
  wgpuQueueWriteTexture(W.q, &dst, data, (size_t)rows * 1024 * 16, &lay, &(WGPUExtent3D){ 1024, (uint32_t)rows, 1 });
  WGPURenderPassColorAttachment ca[2] = {
    { .view = keep_view(fx_tex_view((WGPUTexture)light)), .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
      .storeOp = WGPUStoreOp_Store, .clearValue = { amb[0], amb[1], amb[2], amb[3] } },
    { .view = add ? keep_view(fx_tex_view((WGPUTexture)add)) : NULL, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
      .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store, .clearValue = { 0, 0, 0, 0 } },
  };
  WGPUPassTimestampWrites tsw;
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = add ? 2 : 1, .colorAttachments = ca, .timestampWrites = pass_ts(&tsw) };
  W.pass = wgpuCommandEncoderBeginRenderPass(W.enc, &rp);
  W.pass_fmt = (WGPUTextureFormat)fmt;
  W.pass_ds = false;
  W.pass_w = w;
  W.pass_h = h;
  W.pass_flip = -1.0f;
  float uv[8][4];
  memset(uv, 0, sizeof uv);
  memcpy(uv[0], up, sizeof(float) * 4);
  memcpy(uv[1], uq, sizeof(float) * 4);
  memcpy(uv[2], ut, sizeof(float) * 4);
  memcpy(uv[3], amb, sizeof(float) * 4);
  uint32_t off = fx_uniforms(w, h, true, uv);
  WGPUTexture tx[5] = { (WGPUTexture)normal, (WGPUTexture)seed, (WGPUTexture)mask, (WGPUTexture)mat, LW.data };
  WGPUBindGroupEntry e[7];
  memset(e, 0, sizeof e);
  for (int k = 0; k < 5; k++) e[k] = (WGPUBindGroupEntry){ .binding = (uint32_t)k, .textureView = tx[k] ? keep_view(fx_tex_view(tx[k])) : FX.black_view };
  /* the light textures' array (cw_light2d.texture), or the 1x1 dummy */
  e[5] = (WGPUBindGroupEntry){ .binding = 5, .textureView = cookie_array >= 0 ? keep_view(array_view((WGPUTexture)R.arrays.data[cookie_array].wtex)) : W.dummy_view };
  e[6] = (WGPUBindGroupEntry){ .binding = 6, .sampler = W.samplers[0] };
  WGPUBindGroup g = keep_group(wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = LW.bgl_light, .entryCount = 7, .entries = e }));
  wgpuRenderPassEncoderSetPipeline(W.pass, light_pipeline(add != NULL, (WGPUTextureFormat)fmt));
  wgpuRenderPassEncoderSetBindGroup(W.pass, 0, FX.bg_u, 1, &off);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 1, g, 0, NULL);
  wgpuRenderPassEncoderDraw(W.pass, 3, 1, 0, 0);
  R.stats.draw_calls++;
  end_pass();
}

static void on_r2d_ts(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) {
  (void)msg, (void)u1, (void)u2;
  if (st == WGPUMapAsyncStatus_Success) {
    const uint64_t *v = wgpuBufferGetConstMappedRange(TW.readback, 0, (size_t)TW.rpairs * 16);
    float ms[R2D_T_COUNT] = { 0 };
    bool seen[R2D_T_COUNT] = { 0 };
    for (int i = 0; v && i < TW.rpairs; i++) {
      int p = TW.rpart[i];
      if (v[2 * i + 1] >= v[2 * i]) ms[p] += (float)(v[2 * i + 1] - v[2 * i]) / 1e6f;
      seen[p] = true;
    }
    r2d_timers_wg_result(ms, seen);
    wgpuBufferUnmap(TW.readback);
  }
  TW.mapping = false;
}

void r2d_wg_timers_map(void) {
  if (!TW.mapping || TW.requested) return;
  TW.requested = true;
  wgpuBufferMapAsync(TW.readback, WGPUMapMode_Read, 0, (size_t)TW.rpairs * 16,
                     (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_r2d_ts });
}

static void timers_shutdown(void) {
  if (TW.qs) wgpuQuerySetRelease(TW.qs);
  if (TW.resolve) wgpuBufferRelease(TW.resolve);
  if (TW.readback) wgpuBufferRelease(TW.readback);
  memset(&TW, 0, sizeof TW);
  TW.open = -1;
}

/* an fx destination for a canvas target */
static fx_dst ct_dst(const canvas_target *ct) {
  return (fx_dst){ .w = R.canvas_w, .h = R.canvas_h, .wview = ct->view, .wfmt = (int)ct->fmt, .offscreen = ct->offscreen };
}
/* a lit run's lights and composite onto ct, then the canvas pass again (loaded) */
static void lit_finish(const canvas_target *ct, int lo, int hi) {
  end_pass();
  r2d_timer_end();
  fx_dst d = ct_dst(ct);
  r2d_lit_end(lo, hi, &d);
  r2d_timer_begin(R2D_T_DRAW);
  double none[4] = { 0, 0, 0, 0 };
  begin_canvas(ct, false, none);
}
/* the post chain onto the pass's own canvas, which then takes the layers above it */
static void post_finish(const canvas_target *out) {
  end_pass();
  r2d_timer_end();
  r2d_timer_begin(R2D_T_POST);
  fx_dst d = ct_dst(out);
  r2d_post_run(&d);
  r2d_timer_end();
  r2d_timer_begin(R2D_T_DRAW);
  double none[4] = { 0, 0, 0, 0 };
  begin_canvas(out, false, none);
}

void r2d_wg_end(const r2d_batch *batches, int nb, const uint32_t *idx) {
  if (!W.inited) return;
  /* where the canvas lands */
  bool has_target = !CW_HANDLE_IS_NULL(R.pass.target) && r2d_tex_get(R.pass.target);
  bool virt = !has_target && R.pass.virtual_width > 0 && R.pass.virtual_height > 0;
  canvas_target ct;
  if (virt) {
    r2d_ensure_rt(&R.canvas_rt, R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, false);
    ct = (canvas_target){ keep_view(layer_view(tex_of(R.canvas_rt), 0)), WGPUTextureFormat_RGBA8Unorm, true };
  } else if (has_target) {
    /* a target whose rows run top-down (three.c drew it: cw_render3d_texture_target) is
     * drawn unflipped, as the window is */
    ct = (canvas_target){ keep_view(layer_view(tex_of(R.pass.target), 0)), WGPUTextureFormat_RGBA8Unorm,
                          r2d_tex_get(R.pass.target)->y_up };
  } else {
    WGPUTextureView fv = cw__wgpu_frame_view();
    if (!fv) return;
    ct = (canvas_target){ fv, cw__wgpu_frame_format(), false };
  }
  /* capacities for this end (the bind groups over ubuf / sbuf must exist
   * before recording): one uniform block per stream flip, tilemap draw,
   * light, shadow, composite and blit; scratch for every quad and shadow */
  int lit_runs = 0, edges = 0;
  for (int i = 0; i < nb; i++)
    if (batches[i].lit && (i == 0 || !batches[i - 1].lit)) lit_runs++;
  for (int i = 0; i < R.occluders.count; i++) edges += R.occluders.data[i].count;
  size_t ublocks = 8 + R2D_FX_BLOCKS + (size_t)R.tdraws.count + (size_t)R.pdraws.count + (size_t)R.idraws.count * 5 + (size_t)lit_runs * (2 + 2 * (size_t)R.lights.count);
  size_t uneed = 3 * 2048 + ublocks * UALIGN;
  size_t sneed = 4096 + ((size_t)lit_runs * ((size_t)R.lights.count * 16 + (size_t)edges * 18 + 64)) * 4;
  bool regroup = !W.ubuf || W.ucap < uneed;
  ensure(&W.ubuf, &W.ucap, uneed, WGPUBufferUsage_Uniform, "r2d uniforms");
  ensure(&W.sbuf, &W.scap, sneed, WGPUBufferUsage_Vertex, "r2d scratch");
  if (regroup) {
    FX.bg_u_buf = NULL; /* fx: its bind group names the old buffer */
    if (W.bg_stream) wgpuBindGroupRelease(W.bg_stream);
    if (W.bg_small) wgpuBindGroupRelease(W.bg_small);
    WGPUBindGroupEntry e = { .binding = 0, .buffer = W.ubuf, .size = U_STREAM_SIZE };
    W.bg_stream = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = W.bgl_stream, .entryCount = 1, .entries = &e });
    e.size = U_SMALL_SIZE;
    W.bg_small = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = W.bgl_small, .entryCount = 1, .entries = &e });
    if (W.bg_part) wgpuBindGroupRelease(W.bg_part);
    e.size = 144;
    W.bg_part = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = W.bgl_part, .entryCount = 1, .entries = &e });
    if (W.bg_inst) wgpuBindGroupRelease(W.bg_inst);
    WGPUBindGroupEntry ie = { .binding = 1, .buffer = W.ubuf, .size = R2D_INST_VEC4 * 16 };
    W.bg_inst = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = W.bgl_inst, .entryCount = 1, .entries = &ie });
  }
  if (R.verts.count) {
    size_t vb = sizeof(r2d_vertex) * (size_t)R.verts.count, ib = sizeof(uint32_t) * (size_t)R.idx.count;
    if (!stream_fits(vb, ib)) return;
    ensure(&W.vbuf, &W.vcap, vb, WGPUBufferUsage_Vertex, "r2d stream vertices");
    ensure(&W.ibuf, &W.icap, ib, WGPUBufferUsage_Index, "r2d stream indices");
    write_padded(W.vbuf, R.verts.data, vb);
    write_padded(W.ibuf, idx, ib);
  }
  void *inst_buf = r2d_instances_upload_wg();
  /* depth culling (cull.c): the plan, and each vertex's depth when some group culls */
  bool hdr_on = r2d_post_active();
  const r2d_cull_plan *cp = r2d_cull_build(batches, nb, idx, R.idx.count, hdr_on, hdr_on ? r2d_post_hud_layer() : CW_R2D_MAX_LAYERS);
  if (cp->any) {
    ensure(&W.dbuf, &W.dcap, sizeof(float) * (size_t)R.verts.count, WGPUBufferUsage_Vertex, "r2d depths");
    write_padded(W.dbuf, cp->vdepth, sizeof(float) * (size_t)R.verts.count);
  }
  W.cull_on = false;
  cw_array_clear(&W.ustage);
  cw_array_clear(&W.sstage);
  /* the stream's layer table, once per flip */
  {
    float u[U_STREAM_SIZE / 4];
    memset(u, 0, sizeof u);
    for (int l = 0; l < CW_R2D_MAX_LAYERS; l++) {
      const cw_xform2d *m = &R.layer_view[l];
      float *r = u + l * 8;
      r[0] = m->a, r[1] = m->c, r[2] = m->tx, r[3] = R.layers[l].pixel_snap ? 1.0f : 0.0f;
      r[4] = m->b, r[5] = m->d, r[6] = m->ty, r[7] = R.layers[l].distortion * r2d_px();
    }
    u[256] = (float)R.canvas_w;
    u[257] = (float)R.canvas_h;
    r2d_wind_uniform(u + 260);
    r2d_fog_uniform(u + 264);
    u[259] = cp->bias; /* depth culling's test (cull.c) */
    u[258] = -1.0f;
    W.stream_off[0] = ualloc(u, sizeof u);
    u[258] = 1.0f;
    W.stream_off[1] = ualloc(u, sizeof u);
    u[258] = -1.0f;
    u[259] = 0;
    W.stream_off[2] = ualloc(u, sizeof u);
  }

  W.enc = wgpuDeviceCreateCommandEncoder(W.dev, NULL);
  r2d_timer_begin(R2D_T_DRAW);
  cw_color cc0 = R.pass.clear_color;
  double cc[4] = { cc0.r * cc0.a, cc0.g * cc0.a, cc0.b * cc0.a, cc0.a };
  begin_canvas(&ct, R.pass.clear, cc);
  /* HDR: layers under the chain's hud layer draw into the HDR canvas (post.c) */
  bool in_hdr = r2d_post_active();
  int hud = in_hdr ? r2d_post_hud_layer() : CW_R2D_MAX_LAYERS;
  canvas_target out_ct = ct;
  if (in_hdr) {
    end_pass();
    r2d_post_ensure();
    ct = (canvas_target){ keep_view(fx_tex_view((WGPUTexture)r2d_post_wtex())), (WGPUTextureFormat)r2d_post_wfmt(), true };
    begin_canvas(&ct, true, R.pass.clear ? cc : (double[4]){ 0, 0, 0, 0 });
  }
  bool in_lit = false;
  int lit_lo = 0, lit_hi = 0;
  uint32_t cur_array = 0xffffffffu, cur_blend = 0xffffffffu, cur_clip = 0xffffffffu;
  int cur_prog = -1;
  int prev_layer = -1;
  bool in_dist = false;
  int cur_g = -1;
  for (int i = 0; i < nb; i++) {
    const r2d_batch *b = &batches[i];
    int g = cp->any ? cp->grp[i] : -1;
    if (in_hdr && b->layer >= hud) {
      if (in_lit) lit_finish(&ct, lit_lo, lit_hi);
      in_lit = false;
      if (in_dist) { end_pass(); in_dist = false; }
      post_finish(&out_ct);
      ct = out_ct;
      in_hdr = false;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
      cur_prog = -1;
    }
    if (b->layer != prev_layer) { /* a new layer: leave a distortion layer, read the screen, enter one */
      const cw_r2d_layer_desc *ld = &R.layers[b->layer];
      bool dist = in_hdr && ld->distortion > 0 && r2d_dist_ok();
      bool reset = false;
      if (in_dist && !dist) {
        end_pass();
        /* the canvas below the distortion layers, bent now (post.c), so the layers above stay straight */
        if (r2d_post_dist_resolve()) ct = (canvas_target){ keep_view(fx_tex_view((WGPUTexture)r2d_post_wtex())), (WGPUTextureFormat)r2d_post_wfmt(), true };
        begin_canvas(&ct, false, (double[4]){ 0, 0, 0, 0 });
        in_dist = false;
        reset = true;
      }
      if (in_hdr && ld->screen_read) {
        if (in_lit) lit_finish(&ct, lit_lo, lit_hi);
        in_lit = false;
        end_pass();
        WGPUTexelCopyTextureInfo s = { .texture = (WGPUTexture)r2d_post_wtex(), .aspect = WGPUTextureAspect_All };
        WGPUTexelCopyTextureInfo d = { .texture = (WGPUTexture)r2d_screen_wtex(), .aspect = WGPUTextureAspect_All };
        wgpuCommandEncoderCopyTextureToTexture(W.enc, &s, &d, &(WGPUExtent3D){ (uint32_t)R.canvas_w, (uint32_t)R.canvas_h, 1 });
        begin_canvas(&ct, false, (double[4]){ 0, 0, 0, 0 });
        reset = true;
      }
      if (dist && !in_dist) {
        if (in_lit) lit_finish(&ct, lit_lo, lit_hi);
        in_lit = false;
        end_pass();
        bool first;
        void *dt = r2d_post_dist_wtex(&first);
        begin_pass(keep_view(fx_tex_view((WGPUTexture)dt)), WGPUTextureFormat_RGBA16Float, R.canvas_w, R.canvas_h, true, first,
                   (double[4]){ 0, 0, 0, 0 }, NULL, NULL, NULL);
        in_dist = true;
        reset = true;
      }
      if (reset) {
        cur_array = cur_blend = cur_clip = 0xffffffffu;
        cur_prog = -1;
      }
      prev_layer = b->layer;
    }
    if (!in_dist && in_hdr && R.layers[b->layer].distortion > 0) continue; /* no float targets: nothing bends */
    if (in_dist) {
      if (R2D_ST_KIND(b->state) != R2D_KIND_STREAM) continue;
      uint32_t clip = R2D_ST_CLIP(b->state);
      if (clip != cur_clip) set_clip(cur_clip = clip);
      if (cur_prog != P_SPRITE_DISTORT) {
        wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(P_SPRITE_DISTORT, B_ADD, WGPUTextureFormat_RGBA16Float));
        uint32_t off = W.stream_off[0];
        wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_stream, 1, &off);
        wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.vbuf, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(W.pass, W.ibuf, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        cur_prog = P_SPRITE_DISTORT;
        cur_array = 0xffffffffu;
      }
      uint32_t arr = R2D_ST_ARRAY(b->state);
      if (arr != cur_array) {
        cur_array = arr;
        wgpuRenderPassEncoderSetBindGroup(W.pass, 1, arr == R2D_NO_ARRAY ? dummy_group : array_group((int)arr), 0, NULL);
      }
      wgpuRenderPassEncoderDrawIndexed(W.pass, b->count, 1, b->first, 0, 0);
      R.stats.draw_calls++;
      R.stats.batches++;
      continue;
    }
    if (cp->any && g != cur_g) { /* a new culling group (cull.c) */
      W.cull_on = false;
      if (in_lit) lit_finish(&ct, lit_lo, lit_hi); /* a lit run ends its group */
      else if (W.pass_ds) { /* the canvas without the old group's depth */
        end_pass();
        begin_canvas(&ct, false, (double[4]){ 0, 0, 0, 0 });
      }
      in_lit = false;
      cur_g = g;
      if (g >= 0 && cp->gcull[g]) {
        r2d_timer_end();
        r2d_timer_begin(R2D_T_CULL);
        cull_prepass_wg(cp, batches, i, nb, g);
        r2d_timer_end();
        r2d_timer_begin(R2D_T_DRAW);
        W.cull_on = true;
        begin_canvas(&ct, false, (double[4]){ 0, 0, 0, 0 });
      }
      cur_array = cur_blend = cur_clip = 0xffffffffu;
      cur_prog = -1;
    }
    if (b->lit && !in_lit) {
      end_pass();
      void *stex, *ntex, *mtex;
      int sfmt;
      r2d_lit_targets(&stex, &ntex, &mtex, &sfmt);
      WGPUTextureView sv = keep_view(fx_tex_view((WGPUTexture)stex));
      WGPUTextureView nv = ntex ? keep_view(fx_tex_view((WGPUTexture)ntex)) : NULL;
      WGPUTextureView mv = mtex ? keep_view(fx_tex_view((WGPUTexture)mtex)) : NULL;
      double zero[4] = { 0, 0, 0, 0 };
      cw_array_clear(&R.occ_batches);
      begin_pass(sv, (WGPUTextureFormat)sfmt, R.canvas_w, R.canvas_h, true, true, zero, nv, W.cull_on ? W.cull_view : NULL, mv);
      in_lit = true;
      lit_lo = b->layer;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
      cur_prog = -1;
    } else if (!b->lit && in_lit) {
      lit_finish(&ct, lit_lo, lit_hi);
      in_lit = false;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
      cur_prog = -1;
    }
    if (in_lit) lit_hi = b->layer_last;
    if (in_lit && (b->state & R2D_ST_OCCL)) cw_array_push(&R.occ_batches, *b);
    uint32_t blend = R2D_ST_BLEND(b->state), clip = R2D_ST_CLIP(b->state);
    int bl = blend == CW_BLEND_OPAQUE ? B_OPAQUE : blend == CW_BLEND_MULTIPLY ? B_MULTIPLY : B_ALPHA;
    if (clip != cur_clip) set_clip(cur_clip = clip);
    cur_blend_wg = bl;
    if (R2D_ST_KIND(b->state) == R2D_KIND_TILEMAP) {
      R.tile_depth = W.pass_ds ? r2d_cull_depth_t(2.0f * (float)(b->first / 3)) - cp->bias : 0;
      r2d_tilemap_render(&R.tdraws.data[b->payload], in_lit && R.normals_used, R.canvas_h);
      cur_prog = -1;
      cur_array = 0xffffffffu;
      continue;
    }
    if (R2D_ST_KIND(b->state) == R2D_KIND_PARTICLES) {
      r2d_wg_particles(&R.pdraws.data[b->payload], in_lit && R.normals_used);
      cur_prog = -1;
      cur_array = 0xffffffffu;
      continue;
    }
    if (R2D_ST_KIND(b->state) == R2D_KIND_INSTANCES) {
      r2d_wg_instances(&R.idraws.data[b->payload], in_lit && R.normals_used, inst_buf);
      cur_prog = -1;
      cur_array = 0xffffffffu;
      continue;
    }
    int prog = in_lit && R.normals_used ? (R.material_used ? P_SPRITE_LIT : P_SPRITE_LIT_N) : P_SPRITE;
    if (prog != cur_prog || blend != cur_blend) {
      wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(prog, bl, W.pass_fmt));
      if (prog != cur_prog) {
        uint32_t off = W.stream_off[W.pass_flip < 0 ? 0 : 1];
        wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_stream, 1, &off);
        wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.vbuf, 0, WGPU_WHOLE_SIZE);
        if (W.pass_ds) wgpuRenderPassEncoderSetVertexBuffer(W.pass, 1, W.dbuf, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(W.pass, W.ibuf, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        cur_array = 0xffffffffu;
      }
      cur_prog = prog;
      cur_blend = blend;
    }
    uint32_t arr = R2D_ST_ARRAY(b->state);
    if (arr != cur_array) {
      cur_array = arr;
      WGPUBindGroup g = arr == R2D_NO_ARRAY ? dummy_group : array_group((int)arr);
      wgpuRenderPassEncoderSetBindGroup(W.pass, 1, g, 0, NULL);
    }
    wgpuRenderPassEncoderDrawIndexed(W.pass, b->count, 1, b->first, 0, 0);
    R.stats.draw_calls++;
    R.stats.batches++;
  }
  W.cull_on = false;
  if (in_lit) lit_finish(&ct, lit_lo, lit_hi);
  if (in_dist) end_pass();
  if (in_hdr) post_finish(&out_ct);
  end_pass();
  r2d_timer_end();

  /* virtual canvas -> window (letterboxed) */
  if (virt) {
    WGPUTextureView fv = cw__wgpu_frame_view();
    if (fv) {
      int tw = R.target_w, th = R.target_h;
      cw_rect r = R.canvas_rect;
      bool covers = r.x <= 0 && r.y <= 0 && r.x + r.w >= (float)tw && r.y + r.h >= (float)th;
      cw_color lb = R.pass.letterbox;
      double lc[4] = { lb.r, lb.g, lb.b, 1 };
      begin_pass(fv, cw__wgpu_frame_format(), tw, th, false, !covers, lc, NULL, NULL, 0);
      WGPUTextureView cv = keep_view(array_view(tex_of(R.canvas_rt)));
      cw_scale_filter sf = R.pass.scale_filter;
      WGPUBindGroup g = keep_group(tex_group4(cv, W.samplers[sf == CW_SCALE_FILTER_NEAREST ? 1 : 0], W.dummy_view, W.dummy_mat_view));
      qblock b = qb();
      b.v[0][1] = sf == CW_SCALE_FILTER_SHARP ? 1.0f : 0.0f;
      b.v[0][2] = (float)R.canvas_w, b.v[0][3] = (float)R.canvas_h;
      b.v[1][0] = r.w / (float)CW_MAX(1, R.canvas_w);
      uint32_t off = ualloc(&b, sizeof b);
      wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(P_BLIT, B_OPAQUE, W.pass_fmt));
      wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_small, 1, &off);
      wgpuRenderPassEncoderSetBindGroup(W.pass, 1, g, 0, NULL);
      draw_scratch(squad(r.x, r.y, r.x + r.w, r.y + r.h, 0, 1, 1, 0), 6); /* the canvas is y-up */
      end_pass();
    }
  }

  if ((size_t)W.ustage.count > W.ucap || (size_t)W.sstage.count * 4 > W.scap)
    cw__wgpu_fail("render2d: uniform / scratch estimate too small (%d > %zu, %zu > %zu)", W.ustage.count, W.ucap,
                  (size_t)W.sstage.count * 4, W.scap);
  else {
    write_padded(W.ubuf, W.ustage.data, (size_t)W.ustage.count);
    write_padded(W.sbuf, W.sstage.data, (size_t)W.sstage.count * 4);
  }
  r2d_wg_timers_resolve();
  submit(W.enc);
  W.enc = NULL;
  r2d_wg_timers_map();
  for (int i = 0; i < W.transient.count; i++) wgpuBindGroupRelease(W.transient.data[i]);
  cw_array_clear(&W.transient);
  for (int i = 0; i < transient_views.count; i++) wgpuTextureViewRelease(transient_views.data[i]);
  cw_array_clear(&transient_views);
}
/* ── video frames: YUV planes -> RGB target (yuv.c) ────────────────────── */

static const char *WGSL_YUV =
    "@group(0) @binding(0) var ty: texture_2d<f32>;\n"
    "@group(0) @binding(1) var tcb: texture_2d<f32>;\n"
    "@group(0) @binding(2) var tcr: texture_2d<f32>;\n"
    "@group(0) @binding(3) var<uniform> u: vec4f;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  return vec4f(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    /* the target's rows are bottom-up (as GL's): framebuffer row r holds video row h - 1 - r */
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "  let x = i32(pos.x);\n"
    "  let y = i32(u.y) - 1 - i32(pos.y);\n"
    "  let Y = textureLoad(ty, vec2i(x, y), 0).r;\n"
    "  let cb = textureLoad(tcb, vec2i(x / 2, y / 2), 0).r - 0.5;\n"
    "  let cr = textureLoad(tcr, vec2i(x / 2, y / 2), 0).r - 0.5;\n"
    "  let l = (Y - 0.0627451) * 1.164383;\n"
    "  return vec4f(clamp(vec3f(l + 1.596027 * cr, l - 0.391762 * cb - 0.812968 * cr, l + 2.017232 * cb), vec3f(0.0), vec3f(1.0)), 1.0);\n"
    "}\n";

typedef struct r2d_yuv_wg {
  WGPUTexture plane[3];
  WGPUTextureView view[3];
  WGPUBuffer ubuf;
  WGPUBindGroup bg;
  int pw[3], ph[3];
} r2d_yuv_wg;

static struct {
  WGPUBindGroupLayout bgl;
  WGPURenderPipeline pipe;
} Y;

static bool yuv_pipeline(void) {
  if (Y.pipe) return true;
  WGPUShaderModule sm = cw__wgpu_shader("video yuv", WGSL_YUV);
  if (!sm) return false;
  WGPUBindGroupLayoutEntry e[4];
  memset(e, 0, sizeof e);
  for (int i = 0; i < 3; i++)
    e[i] = (WGPUBindGroupLayoutEntry){ .binding = (uint32_t)i, .visibility = WGPUShaderStage_Fragment,
                                       .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2D } };
  e[3] = (WGPUBindGroupLayoutEntry){ .binding = 3, .visibility = WGPUShaderStage_Fragment,
                                     .buffer = { .type = WGPUBufferBindingType_Uniform, .minBindingSize = 16 } };
  Y.bgl = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 4, .entries = e });
  WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 1, .bindGroupLayouts = &Y.bgl });
  WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA8Unorm, .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = sm, .entryPoint = SV("fs"), .targetCount = 1, .targets = &ct };
  WGPURenderPipelineDescriptor d = { .label = SV("video yuv"), .layout = pl,
                                     .vertex = { .module = sm, .entryPoint = SV("vs") },
                                     .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
                                     .multisample = { .count = 1, .mask = 0xffffffffu },
                                     .fragment = &fs };
  Y.pipe = wgpuDeviceCreateRenderPipeline(W.dev, &d);
  wgpuPipelineLayoutRelease(pl);
  wgpuShaderModuleRelease(sm);
  return Y.pipe != NULL;
}

void *r2d_wg_yuv_new(const int pw[3], const int ph[3]) {
  if (!yuv_pipeline()) return NULL;
  r2d_yuv_wg *y = cw_calloc(1, sizeof *y, CW_MEM_RENDER2D);
  WGPUBindGroupEntry be[4];
  memset(be, 0, sizeof be);
  for (int i = 0; i < 3; i++) {
    y->pw[i] = pw[i];
    y->ph[i] = ph[i];
    WGPUTextureDescriptor td = { .label = SV("video plane"), .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                 .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)pw[i], (uint32_t)ph[i], 1 },
                                 .format = WGPUTextureFormat_R8Unorm, .mipLevelCount = 1, .sampleCount = 1 };
    y->plane[i] = wgpuDeviceCreateTexture(W.dev, &td);
    y->view[i] = wgpuTextureCreateView(y->plane[i], NULL);
    be[i] = (WGPUBindGroupEntry){ .binding = (uint32_t)i, .textureView = y->view[i] };
  }
  y->ubuf = make_buffer(16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "video yuv uniforms");
  be[3] = (WGPUBindGroupEntry){ .binding = 3, .buffer = y->ubuf, .size = 16 };
  y->bg = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = Y.bgl, .entryCount = 4, .entries = be });
  return y;
}

void r2d_wg_yuv_convert(void *yp, const uint8_t *const planes[3], void *target_tex, int w, int h) {
  r2d_yuv_wg *y = yp;
  if (!y || !target_tex) return;
  for (int i = 0; i < 3; i++) {
    WGPUTexelCopyTextureInfo d = { .texture = y->plane[i], .aspect = WGPUTextureAspect_All };
    WGPUTexelCopyBufferLayout l = { .bytesPerRow = (uint32_t)y->pw[i], .rowsPerImage = (uint32_t)y->ph[i] };
    WGPUExtent3D e = { (uint32_t)y->pw[i], (uint32_t)y->ph[i], 1 };
    wgpuQueueWriteTexture(W.q, &d, planes[i], (size_t)y->pw[i] * (size_t)y->ph[i], &l, &e);
  }
  float u[4] = { (float)w, (float)h, 0, 0 };
  wgpuQueueWriteBuffer(W.q, y->ubuf, 0, u, sizeof u);
  WGPUTextureView v = layer_view((WGPUTexture)target_tex, 0);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(W.dev, NULL);
  WGPURenderPassColorAttachment ca = { .view = v, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                       .storeOp = WGPUStoreOp_Store, .clearValue = { 0, 0, 0, 1 } };
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = 1, .colorAttachments = &ca };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetViewport(p, 0, 0, (float)w, (float)h, 0, 1);
  wgpuRenderPassEncoderSetPipeline(p, Y.pipe);
  wgpuRenderPassEncoderSetBindGroup(p, 0, y->bg, 0, NULL);
  wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
  submit(enc);
  wgpuTextureViewRelease(v);
}

void r2d_wg_yuv_free(void *yp) {
  r2d_yuv_wg *y = yp;
  if (!y) return;
  wgpuBindGroupRelease(y->bg);
  wgpuBufferRelease(y->ubuf);
  for (int i = 0; i < 3; i++) {
    wgpuTextureViewRelease(y->view[i]);
    wgpuTextureRelease(y->plane[i]);
  }
  cw_free(y);
}

static void yuv_shutdown(void) {
  if (Y.pipe) wgpuRenderPipelineRelease(Y.pipe);
  if (Y.bgl) wgpuBindGroupLayoutRelease(Y.bgl);
  Y.pipe = NULL;
  Y.bgl = NULL;
}
/* a 3D pipeline output into a target (cw_target_rt): the pipeline's registry wraps layer 0 */
unsigned cw__pw_target_wrap(WGPUTexture color, int w, int h);
void cw__pw_target_unwrap(unsigned target);
unsigned r2d_wg_target_rt(void *tex, int w, int h) { return cw__pw_target_wrap((WGPUTexture)tex, w, h); }
void *r2d_wg_layer_view(void *tex) { return layer_view((WGPUTexture)tex, 0); }
void r2d_wg_view_release(void *view) { if (view) wgpuTextureViewRelease((WGPUTextureView)view); }
void r2d_wg_target_rt_free(unsigned id) { cw__pw_target_unwrap(id); }
#endif

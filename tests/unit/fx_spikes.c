/* fx_spikes: what the heavy water and explosion techniques cost on WebGPU.
 *
 * Phase 0 measurements: not renderers. Each spike runs the compute passes
 * of one technique at the size the research plan names, every frame, and
 * the pipeline's GPU timers (timestamp queries) say what it costs:
 *
 *   fft     3 cascades of a 256 x 256 ocean: spectrum evolve (height and the
 *           two horizontal displacements), 3 inverse FFTs each (rows then
 *           columns, one workgroup per row, 8 butterfly stages in shared
 *           memory), assemble height + displacement + a fold (foam) term.
 *   ripple  a 512 x 512 height-field wave equation, 2 steps a frame.
 *   fire    one step of a 64 x 96 x 64 smoke and fire grid: advect the
 *           velocity, buoyancy, vorticity confinement, divergence, 6 Jacobi
 *           pressure iterations, projection, advect and burn the scalars,
 *           and the copy back (13 dispatches).
 *   march   the ray march of that grid at half of 1080p (960 x 540), 64 steps
 *           a ray, the grid covering about a third of the frame.
 *   mpm     one step of a 20 000 particle 3D MLS-MPM liquid on a 64^3 grid:
 *           clear, particle to grid (fixed-point integer atomics), grid
 *           update, grid to particle.
 *
 * WebGPU only (compatibility limits: 4 storage buffers a stage, 128 threads
 * a workgroup). On GL the app reports nothing. Debug state: spike.ms (the
 * GPU ms average per spike, in the order above), spike.dispatches (per
 * frame), spike.frames. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "fx/fx_internal.h"
#include "fx/fx_wgpu.h"
#if CW_WGPU
#include "platform/wgpu.h"
#include "render/pipeline_wgpu.h"
#endif

enum { S_FFT, S_RIPPLE, S_FIRE, S_MARCH, S_MPM, S_COUNT };
static const char *const NAMES[S_COUNT] = { "spike.fft", "spike.ripple", "spike.fire_step", "spike.fire_march", "spike.mpm" };
static float ms[S_COUNT];
static int32_t dispatches[S_COUNT], frames;
static const char *const DISP_KEYS[S_COUNT] = { "spike.dispatches.fft", "spike.dispatches.ripple", "spike.dispatches.fire_step", "spike.dispatches.fire_march", "spike.dispatches.mpm" };

#if CW_WGPU
#define VIS WGPUShaderStage_Compute
#define SV(s) ((WGPUStringView){ s, sizeof(s) - 1 })

typedef struct pipe { WGPUComputePipeline pl; WGPUBindGroupLayout bgl; WGPUPipelineLayout lay; } pipe;

static WGPUBuffer buf(size_t bytes, const char *label) {
  return cw__wg_buffer(bytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, label);
}
static WGPUBuffer ubuf(size_t bytes, const char *label) {
  return cw__wg_buffer(bytes, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, label);
}

/* a compute pipeline whose layout is nstorage storage bindings (read-write) and, when uniform, a uniform at binding nstorage */
static pipe mk(const char *label, const char *wgsl, const char *entry, int nstorage, bool uniform) {
  pipe p = { 0 };
  cw__wg_slot slots[8];
  int n = 0;
  for (int i = 0; i < nstorage; i++) slots[n++] = (cw__wg_slot){ CW_WB_STORAGE, VIS, 0, 0 };
  if (uniform) slots[n++] = (cw__wg_slot){ CW_WB_UNIFORM, VIS, 0, 0 };
  p.bgl = cw__wg_layout(slots, n);
  p.lay = cw__wg_pipeline_layout(&p.bgl, 1);
  const char *parts[] = { wgsl, NULL };
  WGPUShaderModule m = cw__wg_module(label, parts);
  if (m) p.pl = cw__wg_compute(label, m, entry, p.lay);
  if (m) wgpuShaderModuleRelease(m);
  return p;
}

static WGPUBindGroup bind(pipe *p, WGPUBuffer *b, int nb, WGPUBuffer u) {
  cw__wg_entry e[8];
  memset(e, 0, sizeof e);
  for (int i = 0; i < nb; i++) e[i].buffer = b[i];
  int n = nb;
  if (u) e[n++].buffer = u;
  return cw__wg_group(p->bgl, e, n);
}

/* one dispatch in a compute pass of its own (the pass boundary is the barrier) */
static int ndisp;
static void run(WGPUCommandEncoder enc, pipe *p, WGPUBindGroup g, uint32_t gx, uint32_t gy) {
  WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(enc, NULL);
  wgpuComputePassEncoderSetPipeline(cp, p->pl);
  wgpuComputePassEncoderSetBindGroup(cp, 0, g, 0, NULL);
  wgpuComputePassEncoderDispatchWorkgroups(cp, gx, gy, 1);
  wgpuComputePassEncoderEnd(cp);
  wgpuComputePassEncoderRelease(cp);
  ndisp++;
}
static uint32_t groups(uint32_t threads) { return (threads + 63) / 64; }

static uint32_t rng_state = 12345;
static float rnd(void) {
  rng_state = rng_state * 1664525u + 1013904223u;
  return (float)(rng_state >> 8) * (1.0f / 16777216.0f);
}

/* ── fft ──────────────────────────────────────────────────────────────── */

#define FN 256
#define FN2 (FN * FN)
static const char FFT_WGSL[] =
    "struct U { t: f32, a: f32, b: f32, c: f32 };\n"
    "@group(0) @binding(0) var<storage, read_write> a0: array<vec4f>;\n"  /* h0 (evolve) / spec (fft) */
    "@group(0) @binding(1) var<storage, read_write> a1: array<vec2f>;\n"
    "@group(0) @binding(2) var<uniform> u: U;\n"
    "const N: u32 = 256u;\n"
    "const N2: u32 = 65536u;\n"
    "fn cmul(a: vec2f, b: vec2f) -> vec2f { return vec2f(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }\n"
    /* spectrum evolve: spec[c * 3 + field] */
    "@compute @workgroup_size(64) fn evolve(@builtin(global_invocation_id) g: vec3u) {\n"
    "  let id = g.x; let c = id / N2; let i = id % N2;\n"
    "  if (c >= 3u) { return; }\n"
    "  let L = 120.0 / pow(3.1, f32(c));\n"
    "  let kx = (f32(i % N) - 128.0) * 6.2831853 / L; let ky = (f32(i / N) - 128.0) * 6.2831853 / L;\n"
    "  let kl = max(sqrt(kx * kx + ky * ky), 1e-4);\n"
    "  let w = sqrt(9.81 * kl); let ph = w * u.t;\n"
    "  let e = vec2f(cos(ph), sin(ph));\n"
    "  let h0 = a0[c * N2 + i];\n"
    "  let h = cmul(h0.xy, e) + cmul(vec2f(h0.z, -h0.w), vec2f(e.x, -e.y));\n"
    "  a1[(c * 3u + 0u) * N2 + i] = h;\n"
    "  a1[(c * 3u + 1u) * N2 + i] = cmul(h, vec2f(0.0, -kx / kl));\n"
    "  a1[(c * 3u + 2u) * N2 + i] = cmul(h, vec2f(0.0, -ky / kl));\n"
    "}\n"
    "var<workgroup> sh: array<vec2f, 256>;\n"
    "fn rev8(x: u32) -> u32 { return reverseBits(x) >> 24u; }\n"
    /* in-place inverse FFT of one line of 256: 128 threads, one butterfly each per stage */
    "fn fft_line(base: u32, stride: u32, t: u32) {\n"
    "  sh[t] = a1[base + rev8(t) * stride];\n"
    "  sh[t + 128u] = a1[base + rev8(t + 128u) * stride];\n"
    "  workgroupBarrier();\n"
    "  for (var s = 0u; s < 8u; s++) {\n"
    "    let half = 1u << s;\n"
    "    let j = t & (half - 1u);\n"
    "    let i0 = ((t >> s) << (s + 1u)) + j;\n"
    "    let i1 = i0 + half;\n"
    "    let ang = 3.14159265 * f32(j) / f32(half);\n"
    "    let tw = vec2f(cos(ang), sin(ang));\n"
    "    let x = sh[i0]; let y = cmul(sh[i1], tw);\n"
    "    sh[i0] = x + y; sh[i1] = x - y;\n"
    "    workgroupBarrier();\n"
    "  }\n"
    "  a1[base + t * stride] = sh[t];\n"
    "  a1[base + (t + 128u) * stride] = sh[t + 128u];\n"
    "}\n"
    "@compute @workgroup_size(128) fn rows(@builtin(workgroup_id) w: vec3u, @builtin(local_invocation_index) t: u32) {\n"
    "  fft_line(w.y * N2 + w.x * N, 1u, t);\n"
    "}\n"
    "@compute @workgroup_size(128) fn cols(@builtin(workgroup_id) w: vec3u, @builtin(local_invocation_index) t: u32) {\n"
    "  fft_line(w.y * N2 + w.x, N, t);\n"
    "}\n"
    /* assemble height + displacement and a fold term from the neighbours' displacement (a0 reused as the output) */
    "@compute @workgroup_size(64) fn assemble(@builtin(global_invocation_id) g: vec3u) {\n"
    "  let id = g.x; let c = id / N2; let i = id % N2;\n"
    "  if (c >= 3u) { return; }\n"
    "  let x = i % N; let y = i / N;\n"
    "  let sgn = select(-1.0, 1.0, ((x + y) & 1u) == 0u);\n"
    "  let hb = (c * 3u) * N2; let xb = (c * 3u + 1u) * N2; let zb = (c * 3u + 2u) * N2;\n"
    "  let h = a1[hb + i].x * sgn; let dx = a1[xb + i].x * sgn; let dz = a1[zb + i].x * sgn;\n"
    "  let ip = y * N + ((x + 1u) & 255u); let jp = ((y + 1u) & 255u) * N + x;\n"
    "  let dxx = (a1[xb + ip].x * -sgn) - dx; let dzz = (a1[zb + jp].x * -sgn) - dz;\n"
    "  let jac = (1.0 + dxx) * (1.0 + dzz);\n"
    "  a0[c * N2 + i] = vec4f(h, dx, dz, clamp(1.0 - jac, 0.0, 1.0));\n"
    "}\n";

static struct { pipe ev, rows, cols, asm_; WGPUBuffer h0, spec, out, u; WGPUBindGroup bg_ev, bg_rows, bg_asm; bool ok; } F;

static void fft_init(void) {
  F.ev = mk("fft.evolve", FFT_WGSL, "evolve", 2, true);
  F.rows = mk("fft.rows", FFT_WGSL, "rows", 2, true);
  F.cols = mk("fft.cols", FFT_WGSL, "cols", 2, true);
  F.asm_ = mk("fft.assemble", FFT_WGSL, "assemble", 2, true);
  F.h0 = buf(sizeof(float) * 4 * FN2 * 3, "fft h0");
  F.out = buf(sizeof(float) * 4 * FN2 * 3, "fft out");
  F.spec = buf(sizeof(float) * 2 * FN2 * 9, "fft spec");
  F.u = ubuf(16, "fft u");
  float *h0 = cw_alloc(sizeof(float) * 4 * FN2 * 3, CW_MEM_RENDER);
  for (int c = 0; c < 3; c++)
    for (int i = 0; i < FN2; i++) {
      float kx = (float)(i % FN - 128), ky = (float)(i / FN - 128), k2 = kx * kx + ky * ky + 1e-3f;
      float amp = sqrtf(expf(-1.0f / (k2 * 0.002f + 1e-3f)) / (k2 * k2 + 1.0f)) * 0.3f;
      float *p = h0 + ((size_t)c * FN2 + i) * 4;
      p[0] = amp * (rnd() - 0.5f); p[1] = amp * (rnd() - 0.5f); p[2] = amp * (rnd() - 0.5f); p[3] = amp * (rnd() - 0.5f);
    }
  cw__wg_write(F.h0, 0, h0, sizeof(float) * 4 * FN2 * 3);
  cw_free(h0);
  /* the evolve reads h0 (binding 0) and writes spec (1); the FFTs work on spec (binding 1); assemble reads spec and writes out */
  WGPUBuffer ev[2] = { F.h0, F.spec }, ff[2] = { F.out, F.spec };
  F.bg_ev = bind(&F.ev, ev, 2, F.u);
  F.bg_rows = bind(&F.rows, ev, 2, F.u);
  F.bg_asm = bind(&F.asm_, ff, 2, F.u);
  F.ok = F.ev.pl && F.rows.pl && F.cols.pl && F.asm_.pl;
}

static void fft_frame(WGPUCommandEncoder enc, double t) {
  float u[4] = { (float)t, 0, 0, 0 };
  cw__wg_write(F.u, 0, u, sizeof u);
  run(enc, &F.ev, F.bg_ev, groups(3 * FN2), 1);
  run(enc, &F.rows, F.bg_rows, FN, 9);
  run(enc, &F.cols, F.bg_rows, FN, 9);
  run(enc, &F.asm_, F.bg_asm, groups(3 * FN2), 1);
}

/* ── ripple ───────────────────────────────────────────────────────────── */

#define RN 512
static const char RIPPLE_WGSL[] =
    "struct U { t: f32, a: f32, b: f32, c: f32 };\n"
    "@group(0) @binding(0) var<storage, read_write> src: array<vec2f>;\n"
    "@group(0) @binding(1) var<storage, read_write> dst: array<vec2f>;\n"
    "@group(0) @binding(2) var<uniform> u: U;\n"
    "const N: u32 = 512u;\n"
    "@compute @workgroup_size(64) fn step(@builtin(global_invocation_id) g: vec3u) {\n"
    "  let i = g.x; if (i >= N * N) { return; }\n"
    "  let x = i % N; let y = i / N;\n"
    "  let l = src[y * N + max(x, 1u) - 1u].x; let r = src[y * N + min(x + 1u, N - 1u)].x;\n"
    "  let d = src[max(y, 1u) * N - N + x].x; let up = src[min(y + 1u, N - 1u) * N + x].x;\n"
    "  let s = src[i];\n"
    "  let avg = (l + r + d + up) * 0.25;\n"
    "  var v = (s.y + (avg - s.x) * 1.9) * 0.995;\n"
    /* a drop every few frames */
    "  let dx = f32(x) - (256.0 + 100.0 * sin(u.t)); let dy = f32(y) - (256.0 + 100.0 * cos(u.t * 0.7));\n"
    "  if (dx * dx + dy * dy < 9.0) { v += 0.05; }\n"
    "  dst[i] = vec2f(s.x + v, v);\n"
    "}\n";

static struct { pipe st; WGPUBuffer a, b, u; WGPUBindGroup ab, ba; bool ok; } R;

static void ripple_init(void) {
  R.st = mk("ripple.step", RIPPLE_WGSL, "step", 2, true);
  R.a = buf(sizeof(float) * 2 * RN * RN, "ripple a");
  R.b = buf(sizeof(float) * 2 * RN * RN, "ripple b");
  R.u = ubuf(16, "ripple u");
  WGPUBuffer ab[2] = { R.a, R.b }, ba[2] = { R.b, R.a };
  R.ab = bind(&R.st, ab, 2, R.u);
  R.ba = bind(&R.st, ba, 2, R.u);
  R.ok = R.st.pl != NULL;
}
static void ripple_frame(WGPUCommandEncoder enc, double t) {
  float u[4] = { (float)t, 0, 0, 0 };
  cw__wg_write(R.u, 0, u, sizeof u);
  run(enc, &R.st, R.ab, groups(RN * RN), 1);
  run(enc, &R.st, R.ba, groups(RN * RN), 1);
}

/* ── fire ─────────────────────────────────────────────────────────────── */

#define GX 64
#define GY 96
#define GZ 64
#define GN (GX * GY * GZ)
static const char FIRE_COMMON[] =
    "const GX: i32 = 64; const GY: i32 = 96; const GZ: i32 = 64;\n"
    "fn cid(p: vec3i) -> u32 { let q = clamp(p, vec3i(0), vec3i(GX - 1, GY - 1, GZ - 1)); return u32(q.x + GX * (q.y + GY * q.z)); }\n"
    "fn unpack(i: u32) -> vec3i { let x = i32(i) % GX; let y = (i32(i) / GX) % GY; let z = i32(i) / (GX * GY); return vec3i(x, y, z); }\n";
static const char FIRE_WGSL[] =
    /* bindings: 0 A (vel.xyz, temp), 1 B, 2 D1 (density, fuel, 0, 0), 3 D2; pressure bufs P0 P1 and DV reuse the same layout shape */
    "@group(0) @binding(0) var<storage, read_write> a: array<vec4f>;\n"
    "@group(0) @binding(1) var<storage, read_write> b: array<vec4f>;\n"
    "@group(0) @binding(2) var<storage, read_write> c: array<vec4f>;\n"
    "@group(0) @binding(3) var<storage, read_write> d: array<vec4f>;\n"
    "fn tri(p: vec3f) -> vec4f {\n"
    "  let f = floor(p); let w = p - f; let i = vec3i(f);\n"
    "  let c00 = mix(a[cid(i)], a[cid(i + vec3i(1, 0, 0))], w.x);\n"
    "  let c10 = mix(a[cid(i + vec3i(0, 1, 0))], a[cid(i + vec3i(1, 1, 0))], w.x);\n"
    "  let c01 = mix(a[cid(i + vec3i(0, 0, 1))], a[cid(i + vec3i(1, 0, 1))], w.x);\n"
    "  let c11 = mix(a[cid(i + vec3i(0, 1, 1))], a[cid(i + vec3i(1, 1, 1))], w.x);\n"
    "  return mix(mix(c00, c10, w.y), mix(c01, c11, w.y), w.z);\n"
    "}\n"
    "fn hash3(p: vec3u) -> f32 { return f32(ph((p.x * 73856093u) ^ (p.y * 19349663u) ^ (p.z * 83492791u))) / 4294967296.0; }\n"
    /* inject: a burning source at the bottom */
    "@compute @workgroup_size(64) fn inject(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  let p = unpack(g.x); let q = vec3f(p) - vec3f(32.0, 8.0, 32.0);\n"
    "  if (dot(q, q) < 36.0) { var v = a[g.x]; v.w = 1.0; a[g.x] = v; var s = c[g.x]; s.x = max(s.x, 0.5); s.y = 1.0; c[g.x] = s; }\n"
    "}\n"
    /* advect velocity: a (read) -> b */
    "@compute @workgroup_size(64) fn advect(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  let p = vec3f(unpack(g.x)); let v = a[g.x];\n"
    "  let back = p - v.xyz * 0.5;\n"
    "  b[g.x] = tri(back) * 0.998;\n"
    "}\n"
    /* buoyancy + vorticity confinement + turbulence: b rw, c read (density) */
    "@compute @workgroup_size(64) fn confine(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  let p = unpack(g.x);\n"
    "  var v = b[g.x];\n"
    "  let vx1 = b[cid(p + vec3i(1, 0, 0))].xyz; let vx0 = b[cid(p - vec3i(1, 0, 0))].xyz;\n"
    "  let vy1 = b[cid(p + vec3i(0, 1, 0))].xyz; let vy0 = b[cid(p - vec3i(0, 1, 0))].xyz;\n"
    "  let vz1 = b[cid(p + vec3i(0, 0, 1))].xyz; let vz0 = b[cid(p - vec3i(0, 0, 1))].xyz;\n"
    "  let w = 0.5 * vec3f(vy1.z - vy0.z - (vz1.y - vz0.y), vz1.x - vz0.x - (vx1.z - vx0.z), vx1.y - vx0.y - (vy1.x - vy0.x));\n"
    "  let wl = length(w);\n"
    "  let n = vec3f(length(b[cid(p + vec3i(1,0,0))].xyz - b[cid(p - vec3i(1,0,0))].xyz), length(vy1 - vy0), length(vz1 - vz0));\n"
    "  let nn = n / (length(n) + 1e-4);\n"
    "  var f = cross(nn, w) * 0.2;\n"
    "  let s = c[g.x];\n"
    "  f.y += (v.w * 0.6 + s.x * 0.2) * 0.3;\n"
    "  f += (vec3f(hash3(vec3u(p)), hash3(vec3u(p) + 7u), hash3(vec3u(p) + 13u)) - 0.5) * 0.05 * s.x;\n"
    "  v = vec4f(v.xyz + f, v.w);\n"
    "  b[g.x] = v;\n"
    "}\n"
    /* divergence of b into d.x */
    "@compute @workgroup_size(64) fn divergence(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  let p = unpack(g.x);\n"
    "  let dv = 0.5 * (b[cid(p + vec3i(1,0,0))].x - b[cid(p - vec3i(1,0,0))].x + b[cid(p + vec3i(0,1,0))].y - b[cid(p - vec3i(0,1,0))].y + b[cid(p + vec3i(0,0,1))].z - b[cid(p - vec3i(0,0,1))].z);\n"
    "  var o = d[g.x]; o.y = dv; d[g.x] = o;\n"
    "}\n"
    /* one Jacobi iteration: pressure in c.z (read) -> d.x ... uses channel x of c (src) and y of d (div) to d.x */
    "@compute @workgroup_size(64) fn jacobi(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  let p = unpack(g.x);\n"
    "  let s = a[cid(p + vec3i(1,0,0))].x + a[cid(p - vec3i(1,0,0))].x + a[cid(p + vec3i(0,1,0))].x + a[cid(p - vec3i(0,1,0))].x + a[cid(p + vec3i(0,0,1))].x + a[cid(p - vec3i(0,0,1))].x;\n"
    "  var o = b[g.x]; o.x = (s - c[g.x].y) / 6.0; b[g.x] = o;\n"
    "}\n"
    /* project: subtract the pressure gradient (pressure in a.x), velocity in b */
    "@compute @workgroup_size(64) fn project(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  let p = unpack(g.x);\n"
    "  let gr = 0.5 * vec3f(a[cid(p + vec3i(1,0,0))].x - a[cid(p - vec3i(1,0,0))].x, a[cid(p + vec3i(0,1,0))].x - a[cid(p - vec3i(0,1,0))].x, a[cid(p + vec3i(0,0,1))].x - a[cid(p - vec3i(0,0,1))].x);\n"
    "  var v = b[g.x]; v = vec4f(v.xyz - gr, v.w); b[g.x] = v;\n"
    "}\n"
    /* advect density + fuel (a = scalars read, trilinear), burn, dissipate: velocity in b, out to d */
    "@compute @workgroup_size(64) fn scalars(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  let p = vec3f(unpack(g.x)); let v = b[g.x];\n"
    "  var s = tri(p - v.xyz * 0.5);\n"
    "  let burn = min(s.y, 0.02) * select(0.0, 1.0, v.w > 0.2);\n"
    "  s.y -= burn; s.x = (s.x + burn * 2.0) * 0.99;\n"
    "  var t = v; t.w = (v.w + burn * 20.0) * 0.97;\n"
    "  d[g.x] = s;\n"
    "  b[g.x] = t;\n"
    "}\n"
    "@compute @workgroup_size(64) fn copy(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 393216u) { return; }\n"
    "  a[g.x] = b[g.x]; c[g.x] = vec4f(d[g.x].xy, 0.0, 0.0);\n"
    "}\n";

static struct { pipe inj, adv, conf, div, jac, proj, sca, cpy; WGPUBuffer A, B, D1, D2; WGPUBindGroup g_inj, g_adv, g_conf, g_div, g_jac, g_jac2, g_proj, g_sca, g_cpy; bool ok; } W;

static void fire_init(void) {
  char src[16384];
  snprintf(src, sizeof src, "%s%s", FIRE_COMMON, FIRE_WGSL);
  W.inj = mk("fire.inject", src, "inject", 4, false);
  W.adv = mk("fire.advect", src, "advect", 4, false);
  W.conf = mk("fire.confine", src, "confine", 4, false);
  W.div = mk("fire.div", src, "divergence", 4, false);
  W.jac = mk("fire.jacobi", src, "jacobi", 4, false);
  W.proj = mk("fire.project", src, "project", 4, false);
  W.sca = mk("fire.scalars", src, "scalars", 4, false);
  W.cpy = mk("fire.copy", src, "copy", 4, false);
  size_t sz = sizeof(float) * 4 * GN;
  W.A = buf(sz, "fire A"); W.B = buf(sz, "fire B"); W.D1 = buf(sz, "fire D1"); W.D2 = buf(sz, "fire D2");
  float *dens = cw_alloc(sz, CW_MEM_RENDER);
  memset(dens, 0, sz);
  for (int z = 0; z < GZ; z++)
    for (int y = 0; y < GY; y++)
      for (int x = 0; x < GX; x++) {
        float dx = (float)x - 32, dy = ((float)y - 40) * 0.6f, dz = (float)z - 32, r = sqrtf(dx * dx + dy * dy + dz * dz);
        float *p = dens + ((size_t)x + (size_t)GX * ((size_t)y + (size_t)GY * (size_t)z)) * 4;
        p[0] = r < 22 ? (1.0f - r / 22) * (0.6f + 0.4f * rnd()) : 0; /* density */
        p[1] = r < 10 ? 0.5f : 0;                                      /* fuel */
      }
  cw__wg_write(W.D1, 0, dens, sz);
  cw_free(dens);
  /* layout of the four bindings per pass (a, b, c, d) */
  WGPUBuffer ab[4] = { W.A, W.B, W.D1, W.D2 };
  /* jacobi: a = pressure source, b = pressure out + velocity, c = divergence in .y: here A and B swap roles for the iterations */
  WGPUBuffer ba[4] = { W.B, W.A, W.D1, W.D2 };
  W.g_inj = bind(&W.inj, ab, 4, 0);
  W.g_adv = bind(&W.adv, ab, 4, 0);
  W.g_conf = bind(&W.conf, ab, 4, 0);
  W.g_div = bind(&W.div, ab, 4, 0);
  W.g_jac = bind(&W.jac, ab, 4, 0);
  W.g_jac2 = bind(&W.jac, ba, 4, 0);
  W.g_proj = bind(&W.proj, ab, 4, 0);
  W.g_sca = bind(&W.sca, ab, 4, 0);
  W.g_cpy = bind(&W.cpy, ab, 4, 0);
  W.ok = W.inj.pl && W.adv.pl && W.conf.pl && W.div.pl && W.jac.pl && W.proj.pl && W.sca.pl && W.cpy.pl;
}

static void fire_frame(WGPUCommandEncoder enc) {
  uint32_t g = groups(GN);
  run(enc, &W.inj, W.g_inj, g, 1);
  run(enc, &W.adv, W.g_adv, g, 1);
  run(enc, &W.conf, W.g_conf, g, 1);
  run(enc, &W.div, W.g_div, g, 1);
  for (int i = 0; i < 6; i++) run(enc, &W.jac, (i & 1) ? W.g_jac2 : W.g_jac, g, 1);
  run(enc, &W.proj, W.g_proj, g, 1);
  run(enc, &W.sca, W.g_sca, g, 1);
  run(enc, &W.cpy, W.g_cpy, g, 1);
}

/* ── march ────────────────────────────────────────────────────────────── */

#define MW 960
#define MH 540
static const char MARCH_WGSL[] =
    "struct U { cam: vec4f, lo: vec4f, hi: vec4f, p: vec4f };\n"
    "@group(0) @binding(0) var<storage, read_write> dens: array<vec4f>;\n"
    "@group(0) @binding(1) var<storage, read_write> img: array<vec4f>;\n"
    "@group(0) @binding(2) var<uniform> u: U;\n"
    "const GX: i32 = 64; const GY: i32 = 96; const GZ: i32 = 64;\n"
    "fn cid(p: vec3i) -> u32 { let q = clamp(p, vec3i(0), vec3i(GX - 1, GY - 1, GZ - 1)); return u32(q.x + GX * (q.y + GY * q.z)); }\n"
    "fn tri(p: vec3f) -> vec4f {\n"
    "  let f = floor(p); let w = p - f; let i = vec3i(f);\n"
    "  let c00 = mix(dens[cid(i)], dens[cid(i + vec3i(1, 0, 0))], w.x);\n"
    "  let c10 = mix(dens[cid(i + vec3i(0, 1, 0))], dens[cid(i + vec3i(1, 1, 0))], w.x);\n"
    "  let c01 = mix(dens[cid(i + vec3i(0, 0, 1))], dens[cid(i + vec3i(1, 0, 1))], w.x);\n"
    "  let c11 = mix(dens[cid(i + vec3i(0, 1, 1))], dens[cid(i + vec3i(1, 1, 1))], w.x);\n"
    "  return mix(mix(c00, c10, w.y), mix(c01, c11, w.y), w.z);\n"
    "}\n"
    "@compute @workgroup_size(8, 8) fn march(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 960u || g.y >= 540u) { return; }\n"
    "  let uv = (vec2f(g.xy) + 0.5) / vec2f(960.0, 540.0) * 2.0 - 1.0;\n"
    "  let ro = u.cam.xyz; let rd = normalize(vec3f(uv.x * 1.2, -uv.y * 0.68, -1.0));\n"
    "  let inv = 1.0 / rd; let t0 = (u.lo.xyz - ro) * inv; let t1 = (u.hi.xyz - ro) * inv;\n"
    "  let tn = min(t0, t1); let tf = max(t0, t1);\n"
    "  let ta = max(max(tn.x, tn.y), max(tn.z, 0.0)); let tb = min(min(tf.x, tf.y), tf.z);\n"
    "  var col = vec3f(0.0); var a = 0.0;\n"
    "  if (tb > ta) {\n"
    "    let steps = 64; let dt = (tb - ta) / f32(steps);\n"
    "    for (var i = 0; i < steps; i++) {\n"
    "      let p = ro + rd * (ta + (f32(i) + 0.5) * dt);\n"
    "      let q = (p - u.lo.xyz) / (u.hi.xyz - u.lo.xyz) * vec3f(64.0, 96.0, 64.0);\n"
    "      let s = tri(q);\n"
    "      let d = s.x * 0.9;\n"
    "      let fire = clamp(s.y * 4.0, 0.0, 1.0);\n"
    "      let emit = vec3f(1.0, 0.45, 0.1) * fire * 3.0 + vec3f(0.18, 0.17, 0.16) * (1.0 - fire);\n"
    "      let al = 1.0 - exp(-d * dt * 3.0);\n"
    "      col += (1.0 - a) * al * emit; a += (1.0 - a) * al;\n"
    "      if (a > 0.99) { break; }\n"
    "    }\n"
    "  }\n"
    "  img[g.y * 960u + g.x] = vec4f(col, a);\n"
    "}\n";

static struct { pipe m; WGPUBuffer img, u; WGPUBindGroup g; bool ok; } M;

static void march_init(void) {
  M.m = mk("fire.march", MARCH_WGSL, "march", 2, true);
  M.img = buf(sizeof(float) * 4 * MW * MH, "march image");
  M.u = ubuf(64, "march u");
  /* the box covers about a third of the frame: a camera 9 units away looking at a 10 x 9 x 10 box */
  float u[16] = { 0, 4.5f, 9, 0, -5, 0, -5, 0, 5, 9, 5, 0, 0, 0, 0, 0 };
  cw__wg_write(M.u, 0, u, sizeof u);
  WGPUBuffer b[2] = { W.D1, M.img };
  M.g = bind(&M.m, b, 2, M.u);
  M.ok = M.m.pl != NULL;
}
static void march_frame(WGPUCommandEncoder enc) { run(enc, &M.m, M.g, MW / 8, MH / 8); }

/* ── mpm ──────────────────────────────────────────────────────────────── */

#define MP 20000
#define MG 64
static const char MPM_WGSL[] =
    "@group(0) @binding(0) var<storage, read_write> pa: array<vec4f>;\n"           /* 5 per particle: x, v, C0, C1, C2 */
    "@group(0) @binding(1) var<storage, read_write> gr: array<atomic<i32>>;\n"     /* 4 per cell: mx, my, mz, mass (fixed point 1e4) */
    "@group(0) @binding(2) var<storage, read_write> gv: array<vec4f>;\n"
    "const G: i32 = 64; const FP: f32 = 10000.0;\n"
    "fn cell(p: vec3i) -> i32 { return p.x + G * (p.y + G * p.z); }\n"
    "@compute @workgroup_size(64) fn clear_grid(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 262144u * 4u) { return; }\n"
    "  atomicStore(&gr[g.x], 0);\n"
    "}\n"
    "@compute @workgroup_size(64) fn p2g(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 20000u) { return; }\n"
    "  let x = pa[g.x * 5u].xyz; let v = pa[g.x * 5u + 1u].xyz;\n"
    "  let C = mat3x3f(pa[g.x * 5u + 2u].xyz, pa[g.x * 5u + 3u].xyz, pa[g.x * 5u + 4u].xyz);\n"
    "  let bi = vec3i(floor(x - 0.5)); let f = x - vec3f(bi) - 0.5;\n"
    "  let w0 = 0.5 * (1.5 - f) * (1.5 - f); let w1 = 0.75 - (f - 1.0) * (f - 1.0); let w2 = 0.5 * (f - 0.5) * (f - 0.5);\n"
    "  for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { for (var k = 0; k < 3; k++) {\n"
    "    let wx = select(select(w0.x, w1.x, i == 1), w2.x, i == 2);\n"
    "    let wy = select(select(w0.y, w1.y, j == 1), w2.y, j == 2);\n"
    "    let wz = select(select(w0.z, w1.z, k == 1), w2.z, k == 2);\n"
    "    let w = wx * wy * wz;\n"
    "    let d = vec3f(vec3i(i, j, k)) - f;\n"
    "    let mom = w * (v + C * d);\n"
    "    let ci = cell(bi + vec3i(i, j, k)) * 4;\n"
    "    if (ci >= 0 && ci < 262144 * 4 - 3) {\n"
    "      atomicAdd(&gr[ci], i32(mom.x * FP)); atomicAdd(&gr[ci + 1], i32(mom.y * FP)); atomicAdd(&gr[ci + 2], i32(mom.z * FP));\n"
    "      atomicAdd(&gr[ci + 3], i32(w * FP));\n"
    "    }\n"
    "  } } }\n"
    "}\n"
    "@compute @workgroup_size(64) fn update(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 262144u) { return; }\n"
    "  let m = f32(atomicLoad(&gr[g.x * 4u + 3u])) / FP;\n"
    "  var v = vec3f(0.0);\n"
    "  if (m > 1e-4) {\n"
    "    v = vec3f(f32(atomicLoad(&gr[g.x * 4u])), f32(atomicLoad(&gr[g.x * 4u + 1u])), f32(atomicLoad(&gr[g.x * 4u + 2u]))) / FP / m;\n"
    "    v.y -= 0.05;\n"
    "    let p = vec3i(i32(g.x) % G, (i32(g.x) / G) % G, i32(g.x) / (G * G));\n"
    "    if (p.x < 3 || p.x > G - 4) { v.x = 0.0; }\n"
    "    if (p.y < 3 || p.y > G - 4) { v.y = max(v.y, 0.0) * select(1.0, 0.0, p.y > G - 4); }\n"
    "    if (p.z < 3 || p.z > G - 4) { v.z = 0.0; }\n"
    "  }\n"
    "  gv[g.x] = vec4f(v, m);\n"
    "}\n"
    "@compute @workgroup_size(64) fn g2p(@builtin(global_invocation_id) g: vec3u) {\n"
    "  if (g.x >= 20000u) { return; }\n"
    "  let x = pa[g.x * 5u].xyz;\n"
    "  let bi = vec3i(floor(x - 0.5)); let f = x - vec3f(bi) - 0.5;\n"
    "  let w0 = 0.5 * (1.5 - f) * (1.5 - f); let w1 = 0.75 - (f - 1.0) * (f - 1.0); let w2 = 0.5 * (f - 0.5) * (f - 0.5);\n"
    "  var nv = vec3f(0.0); var C = mat3x3f(vec3f(0.0), vec3f(0.0), vec3f(0.0));\n"
    "  for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { for (var k = 0; k < 3; k++) {\n"
    "    let wx = select(select(w0.x, w1.x, i == 1), w2.x, i == 2);\n"
    "    let wy = select(select(w0.y, w1.y, j == 1), w2.y, j == 2);\n"
    "    let wz = select(select(w0.z, w1.z, k == 1), w2.z, k == 2);\n"
    "    let w = wx * wy * wz;\n"
    "    let d = vec3f(vec3i(i, j, k)) - f;\n"
    "    let gi = cell(clamp(bi + vec3i(i, j, k), vec3i(0), vec3i(G - 1)));\n"
    "    let gvv = gv[gi].xyz;\n"
    "    nv += w * gvv;\n"
    "    C += mat3x3f(gvv * (w * d.x * 4.0), gvv * (w * d.y * 4.0), gvv * (w * d.z * 4.0));\n"
    "  } } }\n"
    "  let nx = clamp(x + nv, vec3f(3.0), vec3f(60.0));\n"
    "  pa[g.x * 5u] = vec4f(nx, 1.0); pa[g.x * 5u + 1u] = vec4f(nv, 0.0);\n"
    "  pa[g.x * 5u + 2u] = vec4f(C[0], 0.0); pa[g.x * 5u + 3u] = vec4f(C[1], 0.0); pa[g.x * 5u + 4u] = vec4f(C[2], 0.0);\n"
    "}\n";

static struct { pipe clr, p2g, upd, g2p; WGPUBuffer P, G, GV; WGPUBindGroup g_clr, g_p2g, g_upd, g_g2p; bool ok; } P;

static void mpm_init(void) {
  /* the pipelines bind the buffers they use: clear and p2g (P, G), update (P, G, GV), g2p (P, GV): one 3 binding layout for all */
  P.clr = mk("mpm.clear", MPM_WGSL, "clear_grid", 3, false);
  P.p2g = mk("mpm.p2g", MPM_WGSL, "p2g", 3, false);
  P.upd = mk("mpm.update", MPM_WGSL, "update", 3, false);
  P.g2p = mk("mpm.g2p", MPM_WGSL, "g2p", 3, false);
  P.P = buf(sizeof(float) * 4 * 5 * MP, "mpm particles");
  P.G = buf(sizeof(int32_t) * 4 * MG * MG * MG, "mpm grid");
  P.GV = buf(sizeof(float) * 4 * MG * MG * MG, "mpm grid v");
  float *pa = cw_alloc(sizeof(float) * 4 * 5 * MP, CW_MEM_RENDER);
  memset(pa, 0, sizeof(float) * 4 * 5 * MP);
  for (int i = 0; i < MP; i++) {
    float *p = pa + (size_t)i * 20;
    p[0] = 12.0f + (float)(i % 25) * 0.9f + rnd() * 0.3f;
    p[1] = 6.0f + (float)((i / 25) % 30) * 0.9f + rnd() * 0.3f;
    p[2] = 12.0f + (float)(i / 750) * 0.9f + rnd() * 0.3f;
    p[3] = 1;
  }
  cw__wg_write(P.P, 0, pa, sizeof(float) * 4 * 5 * MP);
  cw_free(pa);
  WGPUBuffer b[3] = { P.P, P.G, P.GV };
  P.g_clr = bind(&P.clr, b, 3, 0);
  P.g_p2g = bind(&P.p2g, b, 3, 0);
  P.g_upd = bind(&P.upd, b, 3, 0);
  P.g_g2p = bind(&P.g2p, b, 3, 0);
  P.ok = P.clr.pl && P.p2g.pl && P.upd.pl && P.g2p.pl;
}
static void mpm_frame(WGPUCommandEncoder enc) {
  run(enc, &P.clr, P.g_clr, groups(MG * MG * MG * 4), 1);
  run(enc, &P.p2g, P.g_p2g, groups(MP), 1);
  run(enc, &P.upd, P.g_upd, groups(MG * MG * MG), 1);
  run(enc, &P.g2p, P.g_g2p, groups(MP), 1);
}

/* ── the passes ───────────────────────────────────────────────────────── */

static void spike_exec(cw_pass_ctx *ctx, void *user) {
  int which = (int)(intptr_t)user;
  static bool inited;
  if (!inited) {
    inited = true;
    fft_init();
    ripple_init();
    fire_init();
    march_init();
    mpm_init();
  }
  WGPUCommandEncoder enc = cw__pw_encoder();
  ndisp = 0;
  cw__pw_timing_span((void *)enc, false);
  switch (which) {
  case S_FFT: if (F.ok) fft_frame(enc, ctx->time); break;
  case S_RIPPLE: if (R.ok) ripple_frame(enc, ctx->time); break;
  case S_FIRE: if (W.ok) fire_frame(enc); break;
  case S_MARCH: if (M.ok && W.ok) march_frame(enc); break;
  case S_MPM: if (P.ok) mpm_frame(enc); break;
  }
  cw__pw_timing_span((void *)enc, true);
  dispatches[which] = ndisp;
}
#endif

static void init(void *u) {
  (void)u;
  cw_debug_f32_array("spike.ms", ms, S_COUNT);
  for (int i = 0; i < S_COUNT; i++) cw_debug_i32(DISP_KEYS[i], &dispatches[i]);
  cw_debug_i32("spike.frames", &frames);
#if CW_WGPU
  if (cw_gpu_backend() != CW_GPU_WEBGPU) return;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  for (int i = 0; i < S_COUNT; i++)
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = NAMES[i], .phase = CW_PHASE_EFFECTS, .order = 900 + i,
                                             .execute = spike_exec, .user = (void *)(intptr_t)i });
#endif
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  frames++;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  const cw_pass_stat *ps;
  int n = cw_pipeline_pass_stats(p, &ps);
  for (int k = 0; k < S_COUNT; k++)
    for (int i = 0; i < n; i++)
      if (!strcmp(ps[i].name, NAMES[k]) && ps[i].gpu_ms_avg > 0) ms[k] = ps[i].gpu_ms_avg;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_spikes", .width = 1920, .height = 1080, .render_scale = 1, .debug = true, .init = init,
                              .render = render };
  return cw_scene_app("main.cwscene", &base);
}

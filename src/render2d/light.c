/* render2d lights: the light buffer of a run of lit layers.
 *
 *   scene     the run's layers into a colour buffer (+ normals), batch.c
 *   field     occluder outlines filled into a mask, jump-flooded into the
 *             nearest occluder pixel per pixel (the distance field), once per
 *             run when a light casts shadows
 *   lights    the ambient clear, then every light of the run in one
 *             instanced draw: falloff, spot cone, normal maps, a march
 *             through the field for shadows, and the light the air scatters
 *             (volumetric) into a second buffer
 *   composite colour * light + scattered light over the canvas
 *
 * The field, the lights and the composite are the same on GL and WebGPU
 * (fx.h passes; the instanced draws through r2d_wg_* in wgpu.c). */
#include <math.h>
#include <string.h>

#include "fx.h"
#include "cartwheel/quality.h"

void cw_r2d_light(const cw_light2d *l) {
  CW_ASSERT(R.in_pass, "submit lights between cw_r2d_begin and cw_r2d_end");
  if (!R.layers[R.cur_layer].lit) {
    cw_warn("cw_r2d_light: layer %d is not lit (cw_r2d_layer_desc.lit); the light is ignored", R.cur_layer);
    return;
  }
  cw_array_push(&R.lights, ((r2d_light){ *l, R.has_xform ? R.xform : cw_xform2d_identity(), R.cur_layer }));
}

void cw_r2d_occluder(const cw_vec2 *pts, int n, bool closed) {
  CW_ASSERT(R.in_pass, "submit occluders between cw_r2d_begin and cw_r2d_end");
  if (n < 2) return;
  int first = R.occ_pts.count;
  for (int i = 0; i < n; i++) {
    cw_vec2 p = pts[i];
    if (R.has_xform) p = cw_xform2d_apply(&R.xform, p);
    cw_array_push(&R.occ_pts, p);
  }
  cw_array_push(&R.occluders, ((r2d_occluder){ first, n, closed, R.cur_layer }));
}

/* ── state ─────────────────────────────────────────────────────────────── */

static struct {
  fx_rt scene, normal, mat, light, add, mask, seed[2];
  int seed_out;          /* which seed buffer holds the field */
  float field_scale;     /* field pixels per canvas pixel */
  int steps;             /* shadow march steps */
  /* GL */
  GLuint mrt_fbo, light_fbo;
  GLuint prog[2], vao;
  GLint u_p[2], u_q[2], u_t[2], u_amb[2];
  GLuint data_tex;       /* the tiled lights' table (RGBA32F, 1024 wide) */
  int data_rows;
  GLuint mask_prog, mask_vao, mask_vbo;
  GLint mask_u_target;
} LT;

#define INST_FLOATS 16
#define LIGHT_TILE 32 /* pixels: the tiled light pass's tile */
#define FIELD_REACH 128 /* field pixels the shadow flood reaches (its largest step is half) */

/* ── shaders ───────────────────────────────────────────────────────────── */

/* the lights, tiled: one full-screen pass writes ambient plus every light
 * of the pixel's 32-pixel tile, once (no blending of overlapping quads).
 * t_data (RGBA32F, 1024 wide) holds, from texel 0: each light's 4 texels
 * (c = centre in GL rows, radius, height; col = rgb in light buffer units,
 * falloff; s = spot direction in GL rows, cos of the cone's outer and inner
 * edge (outer > 1: none); f = shadow hardness (0: none), volumetric,
 * directional, 0), then a texel per tile (first list entry, count), then the
 * tiles' light lists, 4 indices a texel. u_p = normals, field scale, field
 * size; u_q = march steps, occluder depth, field reach; u_t = tiles across,
 * tile pixels, the tile table's first texel, the lists' first texel;
 * u_amb = ambient in light buffer units */
static const char *GL_LIGHT_VS =
    "#version 300 es\nprecision highp float;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

#define GL_LIGHT_FS_BODY                                                                                         \
  "uniform sampler2D t_nrm, t_seed, t_mask, t_mat, t_data;\n"                                                    \
  "uniform vec4 u_p, u_q, u_t, u_amb;\n"                                                                        \
  "layout(location=0) out vec4 o_col;\n"                                                                         \
  "#ifdef ADD\nlayout(location=1) out vec4 o_add;\n#endif\n"                                                     \
  "vec4 dat(int i) { return texelFetch(t_data, ivec2(i & 1023, i >> 10), 0); }\n"                               \
  "float field(vec2 p) {\n"                                                                                      \
  "  vec2 q = p * u_p.y;\n"                                                                                      \
  "  ivec2 iq = clamp(ivec2(q), ivec2(0), ivec2(u_p.zw) - 1);\n"                                                 \
  "  vec4 s = texelFetch(t_seed, iq, 0);\n"                                                                      \
  "  vec2 sp = vec2(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0) + 0.5;\n"                         \
  "  return min(length(sp - q) / u_p.y, u_q.z);\n"                                                               \
  "}\n"                                                                                                          \
  "float shadow(vec2 p, vec2 rd, float maxt, float k, bool inside) {\n"                                          \
  "  float px = 0.75 / u_p.y, skip = 1.5 / u_p.y;\n"                                                             \
  "  float depth = 1.0;\n"                                                                                       \
  "  if (inside) {\n"                                                                                            \
  "    if (u_q.y <= 0.0) return 1.0;\n"                                                                          \
  "    float s = 0.0, D = u_q.y;\n"                                                                              \
  "    for (int j = 0; j < 8; j++) {\n"                                                                          \
  "      s += D / 8.0;\n"                                                                                        \
  "      if (texelFetch(t_mask, ivec2(p + rd * s), 0).r < 0.5) break;\n"                                        \
  "    }\n"                                                                                                      \
  "    if (s >= D) return 0.0;\n"                                                                                \
  "    depth = 1.0 - s / D;\n"                                                                                   \
  "    depth *= depth;\n"                                                                                        \
  "    p += rd * s; maxt -= s;\n"                                                                                \
  "  }\n"                                                                                                        \
  "  float res = 1.0, t = px;\n"                                                                                 \
  "  int n = int(u_q.x);\n"                                                                                      \
  "  for (int i = 0; i < 64; i++) {\n"                                                                           \
  "    if (i >= n || t >= maxt) break;\n"                                                                        \
  "    float h = field(p + rd * t);\n"                                                                           \
  "    if (h < px) { if (t > skip) return 0.0; t += px; continue; }\n"                                         \
  "    res = min(res, k * h / t);\n"                                                                             \
  "    t += max(h, px);\n"                                                                                       \
  "  }\n"                                                                                                        \
  "  res = clamp(res, 0.0, 1.0);\n"                                                                              \
  "  return res * res * (3.0 - 2.0 * res) * depth;\n"                                                            \
  "}\n"                                                                                                          \
  "void main() {\n"                                                                                              \
  "  vec2 fc = gl_FragCoord.xy;\n"                                                                               \
  "  ivec2 ip = ivec2(fc);\n"                                                                                    \
  /* the surface, read once: its normal and material */                                                       \
  "  vec3 N = vec3(0.0, 0.0, 1.0); float mapped = 0.0;\n"                                                       \
  "  if (u_p.x > 0.5) {\n"                                                                                       \
  "    vec4 n = texelFetch(t_nrm, ip, 0);\n"                                                                     \
  "    if (n.a > 0.004) { vec3 e = n.rgb / n.a; mapped = smoothstep(0.0, 0.5, e.z); N = normalize(e * 2.0 - 1.0); }\n" \
  "  }\n"                                                                                                        \
  "  vec3 Nn = normalize(mix(vec3(0.0, 0.0, 1.0), N, mapped));\n"                                               \
  "  float spec = 0.0, gloss = 0.0, cov = 0.0;\n"                                                               \
  "#ifdef ADD\n"                                                                                                 \
  "  if (u_p.x > 0.5) { vec4 mt = texelFetch(t_mat, ip, 0); if (mt.a > 0.004) { spec = mt.g / mt.a; gloss = mt.b / mt.a; cov = mt.a; } }\n" \
  "#endif\n"                                                                                                     \
  "  bool inside = u_p.y > 0.0 && texelFetch(t_mask, ip, 0).r > 0.5;\n"                                         \
  "  vec3 acc = u_amb.rgb, accA = vec3(0.0);\n"                                                                  \
  "  ivec2 tl = ip / int(u_t.y);\n"                                                                              \
  "  vec4 hd = dat(int(u_t.z) + tl.y * int(u_t.x) + tl.x);\n"                                                   \
  "  int first = int(hd.x), cnt = int(hd.y);\n"                                                                 \
  "  for (int k = 0; k < cnt; k++) {\n"                                                                          \
  "    int e = first + k;\n"                                                                                     \
  "    int li = int(dat(int(u_t.w) + (e >> 2))[e & 3]);\n"                                                       \
  "    vec4 c = dat(li * 4), col = dat(li * 4 + 1), s = dat(li * 4 + 2), f = dat(li * 4 + 3);\n"                \
  "    bool dirl = f.z > 0.5;\n"                                                                                 \
  "    vec2 d = fc - c.xy;\n"                                                                                    \
  "    float dl = length(d);\n"                                                                                  \
  "    float att = dirl ? 1.0 : pow(clamp(1.0 - dl / c.z, 0.0, 1.0), col.w);\n"                                \
  "    if (!dirl && s.z <= 1.0) att *= smoothstep(s.z, s.w, dot(d / max(dl, 1e-4), s.xy));\n"                   \
  "    if (att <= 0.0) continue;\n"                                                                              \
  "    vec3 L = dirl ? normalize(vec3(-s.xy, c.w)) : normalize(vec3(-d, c.w));\n"                               \
  "    float ndl = mix(1.0, max(dot(N, L), 0.0), mapped);\n"                                                    \
  "    float sh = 1.0;\n"                                                                                        \
  "    if (f.x > 0.0) {\n"                                                                                       \
  "      if (dirl) sh = shadow(fc, -s.xy, length(u_p.zw / u_p.y), f.x, inside);\n"                              \
  "      else if (dl > 1e-3) sh = shadow(fc, -d / dl, dl, f.x, inside);\n"                                      \
  "    }\n"                                                                                                      \
  "    acc += col.rgb * att * ndl * sh;\n"                                                                       \
  "#ifdef ADD\n"                                                                                                 \
  "    float spv = 0.0;\n"                                                                                       \
  "    if (spec > 0.0) {\n"                                                                                      \
  "      float pw = exp2(2.0 + 8.0 * gloss);\n"                                                                \
  "      vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));\n"                                                        \
  "      spv = spec * pow(max(dot(Nn, H), 0.0), pw) * (pw + 8.0) / 64.0 * max(dot(Nn, L), 0.0) * cov;\n"       \
  "    }\n"                                                                                                      \
  "    accA += col.rgb * att * sh * (f.y + spv);\n"                                                              \
  "#endif\n"                                                                                                     \
  "  }\n"                                                                                                        \
  "  o_col = vec4(acc, 1.0);\n"                                                                                  \
  "#ifdef ADD\n  o_add = vec4(accA, 0.0);\n#endif\n"                                                           \
  "}\n"

static const char *GL_LIGHT_FS = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n" GL_LIGHT_FS_BODY;
static const char *GL_LIGHT_FS_ADD =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n#define ADD 1\n" GL_LIGHT_FS_BODY;

/* the occluder mask: solid triangles in canvas pixels, scaled to the mask */
static const char *GL_MASK_VS =
    "#version 300 es\nprecision highp float;\nlayout(location=0) in vec2 a_pos;\nuniform vec2 u_target;\n"
    "void main() { gl_Position = vec4(a_pos.x / u_target.x * 2.0 - 1.0, 1.0 - a_pos.y / u_target.y * 2.0, 0.0, 1.0); }\n";
static const char *GL_MASK_FS = "#version 300 es\nprecision highp float;\nout vec4 o_col;\nvoid main() { o_col = vec4(1.0); }\n";

/* jump flood: seeds are field pixel coordinates in RGBA8 (x lo, x hi, y lo,
 * y hi), 65535 = none. Init from the mask; each step keeps the nearest of 9 */
static const char *GL_SEED =
    "vec4 enc(vec2 p) { vec2 hi = floor(p / 256.0); vec2 lo = p - hi * 256.0; return vec4(lo.x, hi.x, lo.y, hi.y) / 255.0; }\n"
    "void main() {\n"
    "  vec2 q = floor(v_uv * u_size);\n"
    "  int n = int(u_v[0].x);\n"
    "  float m = 0.0;\n"
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
    "    if (i >= n || j >= n) continue;\n"
    "    m = max(m, texture(t0, (q + (vec2(i, j) + 0.5) / float(n)) / u_size).r);\n"
    "  }\n"
    "  o_col = m > 0.01 ? enc(q) : vec4(1.0);\n"
    "}\n";
static const char *WG_SEED =
    "fn enc(p: vec2f) -> vec4f { let hi = floor(p / 256.0); let lo = p - hi * 256.0; return vec4f(lo.x, hi.x, lo.y, hi.y) / 255.0; }\n"
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  let q = floor(i0.uv * fx.size);\n"
    "  let n = i32(fx.v[0].x);\n"
    "  var m = 0.0;\n"
    "  for (var j = 0; j < 4; j++) { for (var i = 0; i < 4; i++) {\n"
    "    if (i >= n || j >= n) { continue; }\n"
    "    m = max(m, textureSampleLevel(t0, s_lin, (q + (vec2f(f32(i), f32(j)) + 0.5) / f32(n)) / fx.size, 0.0).r);\n"
    "  } }\n"
    "  if (m > 0.01) { return enc(q); }\n"
    "  return vec4f(1.0);\n"
    "}\n";
static const char *GL_JFA =
    "void main() {\n"
    "  ivec2 q = ivec2(v_uv * u_size), sz = ivec2(u_size);\n"
    "  int st = int(u_v[0].x);\n"
    "  vec2 best = vec2(65535.0); float bd = 1e20;\n"
    "  for (int j = -1; j <= 1; j++)\n"
    "    for (int i = -1; i <= 1; i++) {\n"
    "      ivec2 n = clamp(q + ivec2(i, j) * st, ivec2(0), sz - 1);\n"
    "      vec4 s = texelFetch(t0, n, 0);\n"
    "      vec2 sp = vec2(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0);\n"
    "      if (sp.x > 65534.0) continue;\n"
    "      vec2 dd = sp - vec2(q);\n"
    "      float d = dot(dd, dd);\n"
    "      if (d < bd) { bd = d; best = sp; }\n"
    "    }\n"
    "  if (best.x > 65534.0) { o_col = vec4(1.0); return; }\n"
    "  vec2 hi = floor(best / 256.0); vec2 lo = best - hi * 256.0;\n"
    "  o_col = vec4(lo.x, hi.x, lo.y, hi.y) / 255.0;\n"
    "}\n";
static const char *WG_JFA =
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  let q = vec2i(i0.uv * fx.size); let sz = vec2i(fx.size);\n"
    "  let st = i32(fx.v[0].x);\n"
    "  var best = vec2f(65535.0); var bd = 1e20;\n"
    "  for (var j = -1; j <= 1; j++) {\n"
    "    for (var i = -1; i <= 1; i++) {\n"
    "      let n = clamp(q + vec2i(i, j) * st, vec2i(0), sz - 1);\n"
    "      let s = textureLoad(t0, n, 0);\n"
    "      let sp = vec2f(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0);\n"
    "      if (sp.x > 65534.0) { continue; }\n"
    "      let dd = sp - vec2f(q);\n"
    "      let d = dot(dd, dd);\n"
    "      if (d < bd) { bd = d; best = sp; }\n"
    "    }\n"
    "  }\n"
    "  if (best.x > 65534.0) { return vec4f(1.0); }\n"
    "  let hi = floor(best / 256.0); let lo = best - hi * 256.0;\n"
    "  return vec4f(lo.x, hi.x, lo.y, hi.y) / 255.0;\n"
    "}\n";

/* colour * light (+ scattered light) over the canvas: t0 scene, t1 light,
 * t2 scattered; u_v[0].x = light buffer scale back (2: RGBA8 holds light/2) */
static const char *GL_COMPOSITE =
    "void main() {\n"
    "  ivec2 q = ivec2(v_uv * u_size);\n"
    "  vec4 s = texelFetch(t0, q, 0);\n"
    "  vec3 l = texelFetch(t1, q, 0).rgb * u_v[0].x;\n"
    "  vec3 a = u_v[0].y > 0.5 ? texelFetch(t2, q, 0).rgb : vec3(0.0);\n"
    "  vec4 m = texelFetch(t3, q, 0);\n"
    "  float e = m.a > 0.004 ? clamp(m.r / m.a, 0.0, 1.0) : 0.0;\n"
    "  o_col = vec4(s.rgb * mix(l, vec3(1.0), e) + a, s.a);\n"
    "}\n";
static const char *WG_COMPOSITE =
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  let q = vec2i(i.uv * fx.size);\n"
    "  let s = textureLoad(t0, q, 0);\n"
    "  let l = textureLoad(t1, q, 0).rgb * fx.v[0].x;\n"
    "  var a = vec3f(0.0);\n"
    "  if (fx.v[0].y > 0.5) { a = textureLoad(t2, q, 0).rgb; }\n"
    "  let m = textureLoad(t3, q, 0);\n"
    "  var e = 0.0;\n"
    "  if (m.a > 0.004) { e = clamp(m.r / m.a, 0.0, 1.0); }\n"
    "  return vec4f(s.rgb * mix(l, vec3f(1.0), e) + a, s.a);\n"
    "}\n";

static fx_prog prog_seed = { "r2d field seed", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog prog_jfa = { "r2d field flood", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog prog_comp = { "r2d lit composite", NULL, NULL, 0, 0, 0, NULL, false };

static void progs_init(void) {
  if (prog_seed.glsl) return;
  prog_seed.glsl = GL_SEED, prog_seed.wgsl = WG_SEED;
  prog_jfa.glsl = GL_JFA, prog_jfa.wgsl = WG_JFA;
  prog_comp.glsl = GL_COMPOSITE, prog_comp.wgsl = WG_COMPOSITE;
}

/* ── GL programs ───────────────────────────────────────────────────────── */

static GLuint gl_program(const char *name, const char *vs, const char *fs) {
  GLuint s[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
  const char *src[2] = { vs, fs };
  for (int i = 0; i < 2; i++) {
    glShaderSource(s[i], 1, &src[i], NULL);
    glCompileShader(s[i]);
    GLint ok = 0;
    glGetShaderiv(s[i], GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048];
      GLsizei n = 0;
      glGetShaderInfoLog(s[i], sizeof log, &n, log);
      cw_error("render2d: %s shader failed: %.*s", name, (int)n, log);
      return 0;
    }
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, s[0]);
  glAttachShader(p, s[1]);
  glLinkProgram(p);
  glDeleteShader(s[0]);
  glDeleteShader(s[1]);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    cw_error("render2d: %s program failed to link", name);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static bool gl_init(void) {
  if (LT.prog[0]) return true;
  const char *fs[2] = { GL_LIGHT_FS, GL_LIGHT_FS_ADD };
  for (int i = 0; i < 2; i++) {
    LT.prog[i] = gl_program("light", GL_LIGHT_VS, fs[i]);
    if (!LT.prog[i]) return false;
    glUseProgram(LT.prog[i]);
    LT.u_p[i] = glGetUniformLocation(LT.prog[i], "u_p");
    LT.u_q[i] = glGetUniformLocation(LT.prog[i], "u_q");
    LT.u_t[i] = glGetUniformLocation(LT.prog[i], "u_t");
    LT.u_amb[i] = glGetUniformLocation(LT.prog[i], "u_amb");
    glUniform1i(glGetUniformLocation(LT.prog[i], "t_data"), 7);
    glUniform1i(glGetUniformLocation(LT.prog[i], "t_nrm"), 3);
    glUniform1i(glGetUniformLocation(LT.prog[i], "t_seed"), 4);
    glUniform1i(glGetUniformLocation(LT.prog[i], "t_mask"), 5);
    glUniform1i(glGetUniformLocation(LT.prog[i], "t_mat"), 6);
  }
  glGenVertexArrays(1, &LT.vao); /* the full-screen triangle needs no buffers */
  LT.mask_prog = gl_program("occluder mask", GL_MASK_VS, GL_MASK_FS);
  if (!LT.mask_prog) return false;
  LT.mask_u_target = glGetUniformLocation(LT.mask_prog, "u_target");
  glGenVertexArrays(1, &LT.mask_vao);
  glGenBuffers(1, &LT.mask_vbo);
  glBindVertexArray(LT.mask_vao);
  glBindBuffer(GL_ARRAY_BUFFER, LT.mask_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
  glBindVertexArray(0);
  return true;
}

/* ── the scene buffers ─────────────────────────────────────────────────── */

static fx_fmt scene_format(void) { return R.has_post ? fx_hdr_format() : FX_RGBA8; }

void r2d_lit_targets(void **scene_tex, void **normal_tex, void **mat_tex, int *scene_wfmt) {
  fx_rt_ensure(&LT.scene, R.canvas_w, R.canvas_h, scene_format());
  if (R.normals_used) fx_rt_ensure(&LT.normal, R.canvas_w, R.canvas_h, FX_RGBA8);
  if (R.material_used) fx_rt_ensure(&LT.mat, R.canvas_w, R.canvas_h, FX_RGBA8);
  *scene_tex = LT.scene.wtex;
  *normal_tex = R.normals_used ? LT.normal.wtex : NULL;
  *mat_tex = R.material_used ? LT.mat.wtex : NULL;
  *scene_wfmt = r2d_wg_fx_format(LT.scene.fmt);
}

void r2d_lit_begin_gl(void) {
  fx_rt_ensure(&LT.scene, R.canvas_w, R.canvas_h, scene_format());
  if (R.normals_used) fx_rt_ensure(&LT.normal, R.canvas_w, R.canvas_h, FX_RGBA8);
  if (R.material_used) fx_rt_ensure(&LT.mat, R.canvas_w, R.canvas_h, FX_RGBA8);
  if (!LT.mrt_fbo) glGenFramebuffers(1, &LT.mrt_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, LT.mrt_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, LT.scene.tex, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, R.normals_used ? LT.normal.tex : 0, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, R.material_used ? LT.mat.tex : 0, 0);
  GLenum bufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
  glDrawBuffers(R.normals_used ? (R.material_used ? 3 : 2) : 1, bufs);
  glViewport(0, 0, R.canvas_w, R.canvas_h);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
}

/* ── the distance field ────────────────────────────────────────────────── */

static float xform_scale(const cw_xform2d *m) {
  float det = m->a * m->d - m->b * m->c;
  return sqrtf(det < 0 ? -det : det);
}

/* the occluders of layers [lo, hi] as triangles in canvas pixels (x, y pairs):
 * closed outlines filled, open ones as strokes `w` canvas pixels wide */
static int occluder_tris(int lo, int hi, float w, float **out) {
  int cap = 0;
  for (int i = 0; i < R.occluders.count; i++) {
    const r2d_occluder *o = &R.occluders.data[i];
    if (o->layer >= lo && o->layer <= hi) cap += 3 * (o->closed ? o->count : 0) + 6 * o->count;
  }
  if (!cap) return 0;
  float *v = cw_frame_alloc(sizeof(float) * 2 * (size_t)cap);
  uint32_t *ix = cw_frame_alloc(sizeof(uint32_t) * 3 * (size_t)cap);
  int nv = 0;
  for (int i = 0; i < R.occluders.count; i++) {
    const r2d_occluder *o = &R.occluders.data[i];
    if (o->layer < lo || o->layer > hi) continue;
    const cw_xform2d *m = &R.layer_view[o->layer];
    cw_vec2 *p = cw_frame_alloc(sizeof(cw_vec2) * (size_t)o->count);
    for (int k = 0; k < o->count; k++) p[k] = cw_xform2d_apply(m, R.occ_pts.data[o->first + k]);
    int ne = o->closed ? o->count : o->count - 1;
    if (o->closed && o->count >= 3) {
      int ni = cw_triangulate(p, o->count, ix);
      for (int k = 0; k < ni; k++) {
        v[2 * nv] = p[ix[k]].x;
        v[2 * nv + 1] = p[ix[k]].y;
        nv++;
      }
    }
    /* open outlines (a wall line, a ledge) as strokes */
    for (int e = 0; e < ne && (!o->closed || o->count < 3); e++) {
      cw_vec2 a = p[e], b = p[(e + 1) % o->count];
      cw_vec2 d = cw_v2_sub(b, a);
      float l = cw_v2_length(d);
      if (l < 1e-4f) continue;
      cw_vec2 n = { -d.y / l * w * 0.5f, d.x / l * w * 0.5f };
      cw_vec2 q[6] = { cw_v2_add(a, n), cw_v2_add(b, n), cw_v2_sub(b, n), cw_v2_add(a, n), cw_v2_sub(b, n), cw_v2_sub(a, n) };
      for (int k = 0; k < 6; k++) {
        v[2 * nv] = q[k].x;
        v[2 * nv + 1] = q[k].y;
        nv++;
      }
    }
  }
  *out = v;
  return nv;
}

static void quality_of_field(int *steps, float *scale) {
  int e = cw_gfx_effective(CW_GFX_EFFECTS);
  static const int st[5] = { 12, 16, 24, 32, 48 };
  static const float sc[5] = { 0.25f, 0.25f, 0.5f, 0.5f, 1.0f };
  e = CW_CLAMP(e, 0, 4);
  *steps = st[e];
  float s = sc[e];
  /* never under 320 field pixels across (pixel-art canvases keep their pixels) */
  float minw = 320.0f / (float)CW_MAX(1, R.canvas_w);
  *scale = CW_MIN(1.0f, CW_MAX(s, minw));
}

static void build_mask(int lo, int hi) {
  float *tris = NULL;
  int nv = occluder_tris(lo, hi, 1.0f, &tris);
  fx_rt_ensure(&LT.mask, R.canvas_w, R.canvas_h, FX_RGBA8);
  if (r2d_wg()) {
    r2d_wg_mask_draw(LT.mask.wtex, R.canvas_w, R.canvas_h, tris, nv, (float)R.canvas_w, (float)R.canvas_h);
    if (R.occ_batches.count) r2d_wg_mask_batches(LT.mask.wtex, R.occ_batches.data, R.occ_batches.count);
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, LT.mask.fbo);
    glViewport(0, 0, R.canvas_w, R.canvas_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (nv) {
      glUseProgram(LT.mask_prog);
      glUniform2f(LT.mask_u_target, (float)R.canvas_w, (float)R.canvas_h);
      glBindVertexArray(LT.mask_vao);
      glBindBuffer(GL_ARRAY_BUFFER, LT.mask_vbo);
      glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * 8, tris, GL_STREAM_DRAW);
      glDrawArrays(GL_TRIANGLES, 0, nv);
      R.stats.draw_calls++;
      glBindVertexArray(0);
    }
    /* art that casts shadows (cw_r2d_set_occluder): its alpha over half */
    for (int i = 0; i < R.occ_batches.count; i++) {
      const r2d_batch *b = &R.occ_batches.data[i];
      if (R2D_ST_KIND(b->state) == R2D_KIND_TILEMAP) {
        R.mask_mode = true;
        r2d_tilemap_render(&R.tdraws.data[b->payload], false, R.canvas_h);
        R.mask_mode = false;
        continue;
      }
      glUseProgram(R.sprite_mask.prog);
      r2d_upload_layer_uniforms(&R.sprite_mask);
      glBindVertexArray(R.vao[R.ring]);
      uint32_t arr = R2D_ST_ARRAY(b->state);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_normal : R.arrays.data[arr].tex);
      glDrawElements(GL_TRIANGLES, (GLsizei)b->count, GL_UNSIGNED_INT, (void *)((size_t)b->first * 4));
      R.stats.draw_calls++;
    }
    glBindVertexArray(0);
  }
}

static bool build_field(void) {
  quality_of_field(&LT.steps, &LT.field_scale);
  int fw = CW_MAX(1, (int)lroundf((float)R.canvas_w * LT.field_scale));
  int fh = CW_MAX(1, (int)lroundf((float)R.canvas_h * LT.field_scale));
  LT.field_scale = (float)fw / (float)R.canvas_w;
  fx_rt_ensure(&LT.seed[0], fw, fh, FX_RGBA8);
  fx_rt_ensure(&LT.seed[1], fw, fh, FX_RGBA8);
  /* seeds, then the flood: steps from half the larger side down to 1, and one more 1 */
  fx_dst d0 = fx_dst_of(&LT.seed[0]);
  const fx_rt *in[4] = { &LT.mask, NULL, NULL, NULL };
  float sv[8][4] = { { (float)CW_MAX(1, (int)lroundf(0.5f / LT.field_scale)), 0, 0, 0 } };
  fx_pass(&prog_seed, &d0, false, NULL, in, sv, FX_BLEND_NONE);
  int cur = 0;
  int st = 1;
  /* the flood reaches FIELD_REACH field pixels; past that the field reads the
   * reach (a march step never jumps further than the flood looked) */
  while (st * 2 < CW_MIN(CW_MAX(fw, fh), FIELD_REACH)) st *= 2;
  for (;; st /= 2) {
    int s = st < 1 ? 1 : st;
    float v[8][4] = { { (float)s, 0, 0, 0 } };
    fx_dst d = fx_dst_of(&LT.seed[cur ^ 1]);
    const fx_rt *src[4] = { &LT.seed[cur], NULL, NULL, NULL };
    fx_pass(&prog_jfa, &d, false, NULL, src, v, FX_BLEND_NONE);
    cur ^= 1;
    if (st < 1) break; /* the extra pass of step 1 (JFA+1: fixes the flood's rare misses) */
  }
  LT.seed_out = cur;
  return true;
}

/* ── lights ────────────────────────────────────────────────────────────── */

static void composite_to(const fx_dst *dst, float scale_back, bool add) {
  float v[8][4] = { { scale_back, add ? 1.0f : 0.0f, 0, 0 } };
  const fx_rt *in[4] = { &LT.scene, &LT.light, add ? &LT.add : NULL, R.material_used ? &LT.mat : NULL };
  fx_pass(&prog_comp, dst, false, NULL, in, v, FX_BLEND_PREMUL);
}

void r2d_lit_end(int lo, int hi, const fx_dst *dst) {
  progs_init();
  if (!r2d_wg() && !gl_init()) return;
  if (!r2d_wg()) { /* the scene framebuffer back to one draw buffer */
    glBindFramebuffer(GL_FRAMEBUFFER, LT.mrt_fbo);
    GLenum one[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, one);
  }
  bool hdr = R.has_post && fx_hdr_format() == FX_RGBA16F;
  float k = hdr ? 1.0f : 0.5f; /* the RGBA8 light buffer holds light / 2 */
  /* instances, culled to the canvas */
  float *inst = cw_frame_alloc(sizeof(float) * INST_FLOATS * (size_t)CW_MAX(1, R.lights.count));
  int n = 0;
  bool shadows = false, add = false;
  for (int i = 0; i < R.lights.count; i++) {
    const r2d_light *rl = &R.lights.data[i];
    if (rl->layer < lo || rl->layer > hi) continue;
    const cw_light2d *l = &rl->l;
    cw_xform2d m = cw_xform2d_mul(&R.layer_view[rl->layer], &rl->local);
    cw_vec2 c = cw_xform2d_apply(&m, l->pos);
    float s = xform_scale(&m), r = l->radius * s;
    if (!l->directional) {
      if (r <= 0) continue;
      if (c.x + r < 0 || c.y + r < 0 || c.x - r > (float)R.canvas_w || c.y - r > (float)R.canvas_h) continue;
    }
    R.stats.lights++;
    cw_color col = r2d_default_white(l->color);
    float e = (l->energy != 0 ? l->energy : 1) * col.a * k;
    float hgt = l->directional ? (l->height > 0 ? l->height : 0.5f) : (l->height > 0 ? l->height * s : r * 0.25f);
    /* direction in canvas space (the layer's rotation applies), then in GL rows (y up) */
    float ca = cosf(l->angle), sa = sinf(l->angle);
    cw_vec2 dir = { m.a * ca + m.c * sa, m.b * ca + m.d * sa };
    float dl = cw_v2_length(dir);
    dir = dl > 1e-6f ? cw_v2_scale(dir, 1.0f / dl) : (cw_vec2){ 1, 0 };
    float outer = 2.0f, inner = 2.0f;
    if (l->cone > 0 && !l->directional) {
      float soft = l->cone_soft > 0 ? CW_CLAMP(l->cone_soft, 0.0f, 1.0f) : 0.2f;
      outer = cosf(l->cone);
      inner = cosf(l->cone * (1 - soft));
    }
    float hard = 0;
    if (l->shadows) {
      shadows = true;
      hard = 64.0f * powf(1.0f / 64.0f, CW_CLAMP(l->softness, 0.0f, 1.0f)) + 1.0f; /* 65 (hard) .. 2 (soft) */
    }
    if (l->volumetric > 0 || R.material_used) add = true;
    float *w = inst + INST_FLOATS * n++;
    float v[INST_FLOATS] = { c.x, (float)R.canvas_h - c.y, r, hgt,
                             col.r * e, col.g * e, col.b * e, l->falloff > 0 ? l->falloff : 2.0f,
                             dir.x, -dir.y, outer, inner,
                             hard, l->volumetric > 0 ? l->volumetric : 0, l->directional ? 1.0f : 0.0f, 0 };
    memcpy(w, v, sizeof v);
  }
  bool gi = r2d_gi_enabled();
  r2d_timer_begin(R2D_T_FIELD);
  if (shadows || gi) build_mask(lo, hi);
  if (shadows) build_field();
  r2d_timer_end();
  r2d_timer_begin(R2D_T_LIGHTS);
  fx_fmt lf = hdr ? FX_RGBA16F : FX_RGBA8;
  fx_rt_ensure(&LT.light, R.canvas_w, R.canvas_h, lf);
  if (add) fx_rt_ensure(&LT.add, R.canvas_w, R.canvas_h, hdr ? FX_RGBA16F : FX_RGBA8);
  cw_color a = R.pass.ambient;
  float amb[4] = { a.r * k, a.g * k, a.b * k, 1 };
  const fx_rt *seed = shadows ? &LT.seed[LT.seed_out] : NULL;
  float up[4] = { R.normals_used ? 1.0f : 0.0f, shadows ? LT.field_scale : 1.0f, seed ? (float)seed->w : 1.0f,
                  seed ? (float)seed->h : 1.0f };
  /* how deep light reaches into occluders (canvas pixels; 0 = all through, as an occluder's face) */
  const cw_r2d_layer_desc *ld = &R.layers[lo];
  float od = ld->occluder_depth > 0 ? ld->occluder_depth * xform_scale(&R.layer_view[lo]) : 0;
  float uq[4] = { (float)LT.steps, od, (float)FIELD_REACH * 0.5f / (shadows ? LT.field_scale : 1.0f), 0 };
  /* the table: the lights, then a texel per tile (first entry, count), then the
   * tiles' lists (4 light indices a texel); tiles count in GL rows, as the pass's
   * fragment coordinates do */
  int tx = (R.canvas_w + LIGHT_TILE - 1) / LIGHT_TILE, ty = (R.canvas_h + LIGHT_TILE - 1) / LIGHT_TILE, nt = tx * ty;
  int *cnt = cw_frame_alloc(sizeof(int) * (size_t)nt);
  int (*span)[4] = cw_frame_alloc(sizeof(int[4]) * (size_t)CW_MAX(1, n));
  memset(cnt, 0, sizeof(int) * (size_t)nt);
  int entries = 0;
  for (int i = 0; i < n; i++) {
    const float *L = inst + INST_FLOATS * i;
    int x0 = 0, y0 = 0, x1 = tx - 1, y1 = ty - 1;
    if (L[14] < 0.5f) { /* a point or spot: the tiles its radius covers */
      x0 = CW_MAX(0, (int)floorf((L[0] - L[2]) / LIGHT_TILE)), x1 = CW_MIN(tx - 1, (int)floorf((L[0] + L[2]) / LIGHT_TILE));
      y0 = CW_MAX(0, (int)floorf((L[1] - L[2]) / LIGHT_TILE)), y1 = CW_MIN(ty - 1, (int)floorf((L[1] + L[2]) / LIGHT_TILE));
    }
    span[i][0] = x0, span[i][1] = y0, span[i][2] = x1, span[i][3] = y1;
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) cnt[y * tx + x]++;
    if (x1 >= x0 && y1 >= y0) entries += (x1 - x0 + 1) * (y1 - y0 + 1);
  }
  int head0 = 4 * n, list0 = head0 + nt, texels = list0 + (entries + 3) / 4;
  int rows = CW_MAX(1, (texels + 1023) / 1024);
  float *data = cw_frame_alloc(sizeof(float) * 4 * 1024 * (size_t)rows);
  memset(data + 4 * (size_t)head0, 0, sizeof(float) * 4 * ((size_t)rows * 1024 - (size_t)head0));
  memcpy(data, inst, sizeof(float) * INST_FLOATS * (size_t)n);
  int *fill = cw_frame_alloc(sizeof(int) * (size_t)nt);
  for (int t = 0, e = 0; t < nt; t++) {
    data[4 * (head0 + t)] = (float)e;
    data[4 * (head0 + t) + 1] = (float)cnt[t];
    fill[t] = e;
    e += cnt[t];
  }
  for (int i = 0; i < n; i++)
    for (int y = span[i][1]; y <= span[i][3]; y++)
      for (int x = span[i][0]; x <= span[i][2]; x++) data[4 * list0 + fill[y * tx + x]++] = (float)i;
  float ut[4] = { (float)tx, (float)LIGHT_TILE, (float)head0, (float)list0 };
  if (r2d_wg()) {
    r2d_wg_lights_draw(LT.light.wtex, add ? LT.add.wtex : NULL, r2d_wg_fx_format(lf), amb, data, rows, up, uq, ut,
                       R.normals_used ? LT.normal.wtex : NULL, seed ? seed->wtex : NULL, seed ? LT.mask.wtex : NULL,
                       R.material_used ? LT.mat.wtex : NULL, R.canvas_w, R.canvas_h);
  } else {
    if (!LT.light_fbo) glGenFramebuffers(1, &LT.light_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, LT.light_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, LT.light.tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, add ? LT.add.tex : 0, 0);
    GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(add ? 2 : 1, bufs);
    glViewport(0, 0, R.canvas_w, R.canvas_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    {
      glDisable(GL_BLEND);
      /* the table */
      glActiveTexture(GL_TEXTURE0 + 7);
      if (!LT.data_tex || LT.data_rows < rows) {
        if (!LT.data_tex) glGenTextures(1, &LT.data_tex);
        glBindTexture(GL_TEXTURE_2D, LT.data_tex);
        LT.data_rows = CW_MAX(rows, LT.data_rows * 2);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x8814 /* GL_RGBA32F */, 1024, LT.data_rows, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      }
      glBindTexture(GL_TEXTURE_2D, LT.data_tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, rows, GL_RGBA, GL_FLOAT, data);
      int pi = add ? 1 : 0;
      glUseProgram(LT.prog[pi]);
      glUniform4fv(LT.u_p[pi], 1, up);
      glUniform4fv(LT.u_q[pi], 1, uq);
      glUniform4fv(LT.u_t[pi], 1, ut);
      glUniform4fv(LT.u_amb[pi], 1, amb);
      glActiveTexture(GL_TEXTURE0 + 3);
      glBindTexture(GL_TEXTURE_2D, R.normals_used ? LT.normal.tex : 0);
      glActiveTexture(GL_TEXTURE0 + 4);
      glBindTexture(GL_TEXTURE_2D, seed ? seed->tex : 0);
      glActiveTexture(GL_TEXTURE0 + 5);
      glBindTexture(GL_TEXTURE_2D, seed ? LT.mask.tex : 0);
      glActiveTexture(GL_TEXTURE0 + 6);
      glBindTexture(GL_TEXTURE_2D, R.material_used ? LT.mat.tex : 0);
      glActiveTexture(GL_TEXTURE0);
      glBindVertexArray(LT.vao);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      R.stats.draw_calls++;
      glBindVertexArray(0);
    }
    GLenum one[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, one);
  }
  r2d_timer_end();
  r2d_timer_begin(R2D_T_GI);
  if (gi) r2d_gi_run(&LT.mask, &LT.scene, R.material_used ? &LT.mat : NULL, &LT.light, k);
  r2d_timer_end();
  r2d_timer_begin(R2D_T_COMPOSITE);
  composite_to(dst, 1.0f / k, add);
  r2d_timer_end();
}

void r2d_lights_shutdown(void) {
  fx_rt *rts[] = { &LT.scene, &LT.normal, &LT.mat, &LT.light, &LT.add, &LT.mask, &LT.seed[0], &LT.seed[1] };
  for (size_t i = 0; i < CW_ARRAY_LEN(rts); i++) fx_rt_free(rts[i]);
  if (!r2d_wg()) {
    for (int i = 0; i < 2; i++)
      if (LT.prog[i]) glDeleteProgram(LT.prog[i]);
    if (LT.mask_prog) glDeleteProgram(LT.mask_prog);
    GLuint vaos[2] = { LT.vao, LT.mask_vao }, fbos[2] = { LT.mrt_fbo, LT.light_fbo };
    glDeleteVertexArrays(2, vaos);
    glDeleteBuffers(1, &LT.mask_vbo);
    if (LT.data_tex) glDeleteTextures(1, &LT.data_tex);
    glDeleteFramebuffers(2, fbos);
  }
  memset(&LT, 0, sizeof LT);
}

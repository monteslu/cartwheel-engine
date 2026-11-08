/* render2d shaders and programs (GLSL ES 3.00, identical on every host). */
#include <stdio.h>
#include <string.h>

#include "internal.h"

#define HEADER "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2DArray;\n"

/* the stream: sprites, shapes, text, geometry */
static const char *SPRITE_VS =
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_color;\n"
    "layout(location=3) in vec4 a_info;\n"
    "layout(location=4) in vec4 a_sdf;\n"
    "layout(location=5) in float a_depth;\n" /* depth culling (cull.c): 0 = nearest */
    "uniform float u_zbias;\n"
    "uniform vec4 u_layers[64];\n"
    "uniform vec2 u_target;\n"
    "uniform vec4 u_wind;\n"
    "uniform vec4 u_fog[32];\n" /* per canvas layer: fog colour, amount */ /* sway px, phase (time x speed), spatial frequency, gustiness */
    "out vec2 v_uv; out vec4 v_color; flat out float v_page; flat out int v_mode; flat out float v_sdfw; flat out vec2 v_sdf;\n"
    "flat out float v_emis, v_dist;\n"
    "out vec2 v_fp;\n" /* the canvas position, y up, before wind and snapping (lit layers turn normal maps by it) */
    "flat out vec4 v_fog;\n"
    "void main() {\n"
    "  int L = int(a_info.z) * 2;\n"
    "  vec4 r0 = u_layers[L], r1 = u_layers[L + 1];\n"
    "  vec2 p = vec2(r0.x * a_pos.x + r0.y * a_pos.y + r0.z, r1.x * a_pos.x + r1.y * a_pos.y + r1.z);\n"
    "  v_fp = vec2(p.x, -p.y);\n"
    "  if ((int(a_info.y) & 2) == 0 && a_info.w > 0.0 && u_wind.x != 0.0) {\n"
    /* wind: the sprite's top edge sways (a_info.w = its share x 255), phased by where it stands in the world */
    "    float ph = u_wind.y + a_pos.x * u_wind.z + a_pos.y * u_wind.z * 0.37;\n"
    "    float s = sin(ph) + 0.45 * sin(ph * 2.3 + 1.7) * u_wind.w;\n"
    "    p.x += a_info.w / 255.0 * u_wind.x * s * sqrt(abs(r0.x * r1.y - r0.y * r1.x));\n"
    "  }\n"
    "  if (r0.w > 0.5) p = floor(p + 0.5);\n"
    "  gl_Position = vec4(p.x / u_target.x * 2.0 - 1.0, 1.0 - p.y / u_target.y * 2.0, max(a_depth - u_zbias, 0.0) * 2.0 - 1.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_color = vec4(a_color.rgb * a_color.a * (1.0 + a_sdf.w * 15.0), a_color.a);\n"
    "  v_page = a_info.x;\n"
    "  v_mode = int(a_info.y);\n"
    /* SDF: a_info.w = distance change per screen pixel at layer scale 1 (x255);
     * the layer's zoom shrinks it. No derivatives: hosts disagree on fwidth. */
    "  v_sdfw = 0.6 * a_info.w / 255.0 / max(sqrt(abs(r0.x * r1.y - r0.y * r1.x)), 1e-4);\n"
    "  v_sdf = a_sdf.xy;\n"
    "  v_emis = a_sdf.z;\n"
    "  v_dist = r1.w;\n"
    "  v_fog = u_fog[int(a_info.z)];\n"
    "}\n";

/* A normal map's x and y lie along its texture's +u and up (-v); the light pass reads them as the canvas's x and up.
 * A turned, flipped or sheared sprite (rotation, flip_x / flip_y, an instance's rotation and flags, a particle's spin,
 * a turned camera) carries its texture axes with it: where +u and -v point on the canvas comes from the screen
 * derivatives of the uv and of the canvas position (v_fp, y up: the framebuffer's row order and the wind's sway do not
 * count), and the normal is turned into them. An unturned, unflipped sprite (+u along +x, -v along +y) keeps its
 * value bit for bit. WebGPU: wgpu.c WGSL_SPRITE turn_normal, the same arithmetic. */
#define TURN_NORMAL_GLSL \
  "vec3 turn_normal(vec3 enc, vec2 uvx, vec2 uvy, vec2 fpx, vec2 fpy) {\n" \
  "  float det = uvx.x * uvy.y - uvy.x * uvx.y;\n" \
  "  if (det == 0.0) return enc;\n" \
  "  vec2 du = (fpx * uvy.y - fpy * uvx.y) * sign(det), dv = (fpy * uvx.x - fpx * uvy.x) * sign(det);\n" \
  "  float lu = length(du), lv = length(dv);\n" \
  "  if (!(lu > 0.0) || !(lv > 0.0)) return enc;\n" \
  "  vec2 T = du / lu, U = -dv / lv;\n" \
  "  if (T.x > 0.0 && U.y > 0.0 && abs(T.y) < 1e-3 && abs(U.x) < 1e-3) return enc;\n" \
  "  vec3 e = enc * 2.0 - 1.0;\n" \
  "  return vec3(T * e.x + U * e.y, e.z) * 0.5 + 0.5;\n" \
  "}\n"

static const char *SPRITE_FS =
    "uniform sampler2DArray u_tex;\n"
    "#ifdef LIT\nuniform sampler2DArray u_nrm, u_mat;\nlayout(location=1) out vec4 o_nrm;\nlayout(location=2) out vec4 o_mat;\nin vec2 v_fp;\n"
    TURN_NORMAL_GLSL
    "#endif\n"
    "in vec2 v_uv; in vec4 v_color; flat in float v_page; flat in int v_mode; flat in float v_sdfw; flat in vec2 v_sdf;\n"
    "flat in float v_emis, v_dist;\n"
    "flat in vec4 v_fog;\n"
    "layout(location=0) out vec4 o_col;\n"
    "void main() {\n"
    "#ifdef LIT\n" /* derivatives before any branch (undefined in non-uniform control flow) */
    "  vec2 uvx = dFdx(v_uv), uvy = dFdy(v_uv), fpx = dFdx(v_fp), fpy = dFdy(v_fp);\n"
    "#endif\n"
    "  vec4 t = texture(u_tex, vec3(v_uv, v_page));\n"
    "  vec4 c;\n"
    "  if ((v_mode & 1) != 0) c = v_color;\n"
    "  else if ((v_mode & 2) != 0) {\n"
    /* outlines and shadows (text styles) move the edge out by v_sdf.x and
     * soften it by v_sdf.y; nothing past the glyph's distance range (0) */
    "    float w = clamp(v_sdfw, 0.004, 0.5) + v_sdf.y, e = 0.5 - v_sdf.x, lo = e - w;\n"
    "    if (v_sdf.x + v_sdf.y > 0.0) lo = max(lo, 0.004);\n"
    "    c = v_color * smoothstep(lo, e + w, t.a);\n"
    "  } else c = t * v_color;\n"
    /* flash (sprites, shapes: sdf.x): toward the vertex colour, keeping the shape's alpha */
    "  if ((v_mode & 2) == 0 && v_sdf.x > 0.0) c.rgb = mix(c.rgb, v_color.rgb / max(v_color.a, 1e-4) * c.a, v_sdf.x);\n"
    "#ifdef PREPASS\n" /* depth culling: only texels that replace what is under them */
    "  if ((v_mode & 4) != 0 || c.a < 1.0) discard;\n"
    "  o_col = vec4(0.0);\n"
    "  return;\n"
    "#endif\n"
    "#ifndef LIT\n"
    /* the layer's fog: toward its colour by its amount (atmospheric depth) */
    "  if (v_fog.a > 0.0) c.rgb = mix(c.rgb, v_fog.rgb * c.a, v_fog.a);\n"
    "#endif\n"
    "#ifdef LIT\n"
    "  vec4 n = texture(u_nrm, vec3(v_uv, v_page));\n"
    "  vec3 enc = (v_mode & 3) != 0 ? vec3(0.5, 0.5, 0.0) : turn_normal(n.rgb, uvx, uvy, fpx, fpy);\n"
    "  o_nrm = (v_mode & 4) != 0 ? vec4(0.0) : vec4(enc, 1.0) * c.a;\n"
    /* material: emission (the larger of the draw state's and the map's), specular, gloss */
    "  vec3 m = (v_mode & 3) != 0 ? vec3(0.0) : texture(u_mat, vec3(v_uv, v_page)).rgb;\n"
    "  o_mat = (v_mode & 4) != 0 ? vec4(0.0) : vec4(max(v_emis, m.r), m.g, m.b, 1.0) * c.a;\n"
    "#endif\n"
    "  if ((v_mode & 4) != 0) c.a = 0.0;\n"
    "#ifdef MASK\n"
    "  if (c.a < 0.5) discard;\n"
    "  o_col = vec4(1.0);\n"
    "  return;\n"
    "#endif\n"
    "#ifdef DISTORT\n"
    /* distortion layers: red / green (0.5 = none) x alpha x the layer's reach, added up */
    "  vec2 off = c.a > 1e-4 ? (c.rg / c.a * 2.0 - 1.0) * c.a * v_dist : vec2(0.0);\n"
    "  o_col = vec4(off, 0.0, 0.0);\n"
    "#else\n"
    "  o_col = c;\n"
    "#endif\n"
    "}\n";

/* GPU particles (cw_r2d_particles): no state anywhere. Particle i of an
 * emitter is a pure function of the time: its own phase and life, then per
 * life cycle a fresh spawn point, direction, speed and spin from an integer
 * hash, and its path in closed form (launch velocity under gravity and linear
 * drag). The vertex stage builds each quad from gl_InstanceID; the fragment
 * stage is the sprites' (lit layers light it). u_pa: [0] pos, extent; [1]
 * angle, spread, speed min, max; [2] gravity, drag, time; [3] life min, max,
 * size start, end; [4] colour start; [5] colour end; [6] uv rect; [7] page,
 * spin, turbulence, seed; [8] canvas layer, mode, intensity, emission */
static const char *PART_VS =
    "uniform vec4 u_layers[64];\n"
    "uniform vec2 u_target;\n"
    "uniform vec4 u_fog[32];\n"
    "uniform vec4 u_pa[9];\n"
    "out vec2 v_uv; out vec4 v_color; flat out float v_page; flat out int v_mode; flat out float v_sdfw; flat out vec2 v_sdf;\n"
    "flat out float v_emis, v_dist;\n"
    "out vec2 v_fp;\n" /* the canvas position, y up, before wind and snapping (lit layers turn normal maps by it) */
    "flat out vec4 v_fog;\n"
    "uint hsh(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }\n"
    "float rnd(inout uint s) { s = hsh(s); return float(s >> 8) / 16777216.0; }\n"
    "void main() {\n"
    "  int k = gl_VertexID;\n"
    "  vec2 q = vec2((k == 1 || k == 2 || k == 4) ? 1.0 : 0.0, (k == 2 || k == 4 || k == 5) ? 1.0 : 0.0);\n"
    "  uint s = hsh(uint(gl_InstanceID) * 0x9E3779B9u ^ uint(u_pa[7].w));\n"
    "  float ph = rnd(s), life = max(mix(u_pa[3].x, u_pa[3].y, rnd(s)), 1e-3);\n"
    "  float tt = u_pa[2].w + ph * life, cyc = floor(tt / life), age = tt - cyc * life, f = age / life;\n"
    "  uint r = hsh(s ^ (uint(cyc) * 0x85ebca6bu));\n"
    "  vec2 p = u_pa[0].xy + (vec2(rnd(r), rnd(r)) * 2.0 - 1.0) * u_pa[0].zw;\n"
    "  float ang = u_pa[1].x + (rnd(r) * 2.0 - 1.0) * u_pa[1].y;\n"
    "  vec2 v = vec2(cos(ang), sin(ang)) * mix(u_pa[1].z, u_pa[1].w, rnd(r));\n"
    "  float dr = u_pa[2].z;\n"
    "  if (dr > 1e-4) { float e = (1.0 - exp(-dr * age)) / dr; p += v * e + u_pa[2].xy * (age - e) / dr; }\n"
    "  else p += v * age + 0.5 * u_pa[2].xy * age * age;\n"
    "  float wob = rnd(r) * 6.2831853;\n"
    "  p += vec2(sin(age * 2.7 + wob), cos(age * 2.1 + wob * 1.3)) * u_pa[7].z * f;\n"
    "  float sz = mix(u_pa[3].z, u_pa[3].w, f), rot = u_pa[7].y * (rnd(r) * 2.0 - 1.0) * age;\n"
    "  vec2 l = (q - 0.5) * sz;\n"
    "  float cs = cos(rot), sn = sin(rot);\n"
    "  vec2 w = p + vec2(cs * l.x - sn * l.y, sn * l.x + cs * l.y);\n"
    "  int L = int(u_pa[8].x) * 2;\n"
    "  vec4 r0 = u_layers[L], r1 = u_layers[L + 1];\n"
    "  vec2 c = vec2(r0.x * w.x + r0.y * w.y + r0.z, r1.x * w.x + r1.y * w.y + r1.z);\n"
    "  v_fp = vec2(c.x, -c.y);\n"
    "  gl_Position = vec4(c.x / u_target.x * 2.0 - 1.0, 1.0 - c.y / u_target.y * 2.0, -1.0, 1.0);\n"
    "  v_uv = mix(u_pa[6].xy, u_pa[6].zw, q);\n"
    "  vec4 col = mix(u_pa[4], u_pa[5], f);\n"
    "  v_color = vec4(col.rgb * col.a * u_pa[8].z, col.a);\n"
    "  v_page = u_pa[7].x;\n"
    "  v_mode = int(u_pa[8].y);\n"
    "  v_sdfw = 0.0; v_sdf = vec2(0.0);\n"
    "  v_emis = u_pa[8].w; v_dist = 0.0;\n"
    "  v_fog = u_fog[int(u_pa[8].x)];\n"
    "}\n";

/* sprite instances (cw_r2d_instances): the quad from gl_VertexID, the rest
 * from the 24-byte instance (x, y, rotation, scale; colour; frame, flags)
 * and u_in: [0] pivot, size override; [1] canvas layer, mode, intensity,
 * emission; [2] page, flash, sway, 0; then per frame its uv rect and pixel
 * size. The fragment stage is the sprites'. */
CW_STATIC_ASSERT(R2D_INST_VEC4 == 67, "INST_VS declares u_in[67]");
static const char *INST_VS =
    "layout(location=0) in vec4 i_a;\n"
    "layout(location=1) in vec4 i_col;\n"
    "layout(location=2) in vec2 i_fr;\n"
    "uniform vec4 u_layers[64];\n"
    "uniform vec2 u_target;\n"
    "uniform vec4 u_wind;\n"
    "uniform vec4 u_fog[32];\n"
    "uniform vec4 u_in[67];\n"
    "out vec2 v_uv; out vec4 v_color; flat out float v_page; flat out int v_mode; flat out float v_sdfw; flat out vec2 v_sdf;\n"
    "flat out float v_emis, v_dist;\n"
    "out vec2 v_fp;\n" /* the canvas position, y up, before wind and snapping (lit layers turn normal maps by it) */
    "flat out vec4 v_fog;\n"
    "void main() {\n"
    "  int k = gl_VertexID;\n"
    "  vec2 q = vec2((k == 1 || k == 2 || k == 4) ? 1.0 : 0.0, (k == 2 || k == 4 || k == 5) ? 1.0 : 0.0);\n"
    "  int f = int(i_fr.x), fl = int(i_fr.y);\n"
    "  vec4 uv = u_in[3 + 2 * f], fs = u_in[4 + 2 * f];\n"
    "  float sc = i_a.w == 0.0 ? 1.0 : i_a.w;\n"
    "  vec2 sz = (u_in[0].z > 0.0 ? u_in[0].zw : fs.xy) * sc;\n"
    "  vec2 l = (q - u_in[0].xy) * sz;\n"
    "  float cs = cos(i_a.z), sn = sin(i_a.z);\n"
    "  vec2 w = i_a.xy + vec2(cs * l.x - sn * l.y, sn * l.x + cs * l.y);\n"
    "  int L = int(u_in[1].x) * 2;\n"
    "  vec4 r0 = u_layers[L], r1 = u_layers[L + 1];\n"
    "  vec2 p = vec2(r0.x * w.x + r0.y * w.y + r0.z, r1.x * w.x + r1.y * w.y + r1.z);\n"
    "  v_fp = vec2(p.x, -p.y);\n"
    "  if (q.y < 0.5 && u_in[2].z > 0.0 && u_wind.x != 0.0) {\n"
    "    float ph = u_wind.y + w.x * u_wind.z + w.y * u_wind.z * 0.37;\n"
    "    float s = sin(ph) + 0.45 * sin(ph * 2.3 + 1.7) * u_wind.w;\n"
    "    p.x += u_in[2].z * u_wind.x * s * sqrt(abs(r0.x * r1.y - r0.y * r1.x));\n"
    "  }\n"
    "  if (r0.w > 0.5) p = floor(p + 0.5);\n"
    "  gl_Position = vec4(p.x / u_target.x * 2.0 - 1.0, 1.0 - p.y / u_target.y * 2.0, -1.0, 1.0);\n"
    "  vec2 tq = vec2((fl & 1) != 0 ? 1.0 - q.x : q.x, (fl & 2) != 0 ? 1.0 - q.y : q.y);\n"
    "  v_uv = mix(uv.xy, uv.zw, tq);\n"
    "  vec4 col = i_col == vec4(0.0) ? vec4(1.0) : i_col;\n"
    "  v_color = vec4(col.rgb * col.a * u_in[1].z, col.a);\n"
    "  v_page = u_in[2].x;\n"
    "  v_mode = int(u_in[1].y);\n"
    "  v_sdfw = 0.0; v_sdf = vec2(u_in[2].y, 0.0);\n"
    "  v_emis = u_in[1].w; v_dist = r1.w;\n"
    "  v_fog = u_fog[int(u_in[1].x)];\n"
    "}\n";

/* tilemap chunks: static geometry, animated in the vertex shader */
static const char *TILE_VS =
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_tile;\n" /* id, corner (u | v<<1), frames, fps*16 */
    "uniform vec4 u_m0, u_m1;\n"
    "uniform vec2 u_target;\n"
    "uniform vec4 u_set0;\n" /* first cell origin px (x, y), tile size px (w, h) */
    "uniform vec4 u_set1;\n" /* stride px (x, y), columns, page */
    "uniform vec2 u_inv_page;\n"
    "uniform float u_time;\n"
    "out vec2 v_uv; flat out float v_page;\n"
    "void main() {\n"
    "  vec2 p = vec2(u_m0.x * a_pos.x + u_m0.y * a_pos.y + u_m0.z, u_m1.x * a_pos.x + u_m1.y * a_pos.y + u_m1.z);\n"
    "  if (u_m0.w > 0.5) p = floor(p + 0.5);\n"
    /* u_m1.w: the draw's depth (depth culling, cull.c); 0 = nearest */
    "  gl_Position = vec4(p.x / u_target.x * 2.0 - 1.0, 1.0 - p.y / u_target.y * 2.0, u_m1.w > 0.0 ? u_m1.w * 2.0 - 1.0 : -1.0, 1.0);\n"
    "  float id = a_tile.x;\n"
    "  if (a_tile.z > 1.0) id += mod(floor(u_time * a_tile.w / 16.0), a_tile.z);\n"
    "  float col = mod(id, u_set1.z), row = floor(id / u_set1.z);\n"
    "  vec2 corner = vec2(mod(a_tile.y, 2.0), floor(a_tile.y / 2.0));\n"
    "  vec2 inset = mix(vec2(0.02), u_set0.zw - 0.02, corner);\n"
    "  v_uv = (u_set0.xy + vec2(col, row) * u_set1.xy + inset) * u_inv_page;\n"
    "  v_page = u_set1.w;\n"
    "}\n";

static const char *TILE_FS =
    "uniform sampler2DArray u_tex;\n"
    "uniform vec4 u_tint;\n"
    "#ifdef LIT\nuniform sampler2DArray u_nrm, u_mat;\nlayout(location=1) out vec4 o_nrm;\nlayout(location=2) out vec4 o_mat;\n#endif\n"
    "in vec2 v_uv; flat in float v_page;\n"
    "layout(location=0) out vec4 o_col;\n"
    "void main() {\n"
    "  vec4 c = texture(u_tex, vec3(v_uv, v_page)) * vec4(u_tint.rgb * u_tint.a, u_tint.a);\n"
    "#ifdef LIT\n"
    "  o_nrm = vec4(texture(u_nrm, vec3(v_uv, v_page)).rgb, 1.0) * c.a;\n"
    "  o_mat = vec4(texture(u_mat, vec3(v_uv, v_page)).rgb, 1.0) * c.a;\n"
    "#endif\n"
    "#ifdef MASK\n"
    "  if (c.a < 0.5) discard;\n"
    "  c = vec4(1.0);\n"
    "#endif\n"
    "#ifdef PREPASS\n"
    "  if (c.a < 1.0) discard;\n"
    "#endif\n"
    "  o_col = c;\n"
    "}\n";

/* scratch quads in target pixels (lights, composite, blit) */
static const char *QUAD_VS =
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "uniform vec2 u_target;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.x / u_target.x * 2.0 - 1.0, 1.0 - a_pos.y / u_target.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

/* u_sharp: sharp filtering (cw_scale_filter SHARP): x on, yz canvas size, w window pixels per canvas pixel */
static const char *BLIT_FS =
    "uniform sampler2DArray u_tex;\n"
    "uniform float u_page;\n"
    "uniform vec4 u_sharp;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec2 uv = v_uv;\n"
    "  if (u_sharp.x > 0.5) { vec2 t = uv * u_sharp.yz, c = floor(t) + 0.5; uv = (c + clamp((t - c) * u_sharp.w, -0.5, 0.5)) / u_sharp.yz; }\n"
    "  o_col = texture(u_tex, vec3(uv, u_page));\n"
    "}\n";

static GLuint compile_stage(const char *name, GLenum type, const char *defines, const char *src) {
  GLuint s = glCreateShader(type);
  const char *parts[3] = { HEADER, defines, src };
  GLint lens[3] = { (GLint)strlen(parts[0]), (GLint)strlen(parts[1]), (GLint)strlen(parts[2]) };
  glShaderSource(s, 3, parts, lens);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei n = 0;
    glGetShaderInfoLog(s, sizeof log, &n, log);
    cw_error("render2d: %s %s shader failed to compile: %.*s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment",
             (int)n, log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static bool build(r2d_program *p, const char *name, const char *defines, const char *vs, const char *fs) {
  memset(p, 0, sizeof *p);
  GLuint v = compile_stage(name, GL_VERTEX_SHADER, defines, vs);
  GLuint f = compile_stage(name, GL_FRAGMENT_SHADER, defines, fs);
  if (!v || !f) return false;
  GLuint prog = glCreateProgram();
  glAttachShader(prog, v);
  glAttachShader(prog, f);
  glLinkProgram(prog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei n = 0;
    glGetProgramInfoLog(prog, sizeof log, &n, log);
    cw_error("render2d: %s program failed to link: %.*s", name, (int)n, log);
    glDeleteProgram(prog);
    return false;
  }
  p->prog = prog;
  p->u_layers = glGetUniformLocation(prog, "u_layers");
  p->u_target = glGetUniformLocation(prog, "u_target");
  p->u_tex = glGetUniformLocation(prog, "u_tex");
  p->u_nrm = glGetUniformLocation(prog, "u_nrm");
  p->u_time = glGetUniformLocation(prog, "u_time");
  glUseProgram(prog);
  /* fixed units: 0 = colour, 1 = normals, 2 = light buffer */
  if (p->u_tex >= 0) glUniform1i(p->u_tex, 0);
  if (p->u_nrm >= 0) glUniform1i(p->u_nrm, 1);
  GLint um = glGetUniformLocation(prog, "u_mat");
  if (um >= 0) glUniform1i(um, 2);
  GLint ul = glGetUniformLocation(prog, "u_light");
  if (ul >= 0 && strstr(fs, "sampler2DArray u_light")) glUniform1i(ul, 2);
  return true;
}


bool r2d_programs_init(void) {
  bool ok = build(&R.sprite, "sprite", "", SPRITE_VS, SPRITE_FS) &&
            build(&R.sprite_distort, "sprite (distortion)", "#define DISTORT 1\n", SPRITE_VS, SPRITE_FS) &&
            build(&R.sprite_lit, "sprite (lit)", "#define LIT 1\n", SPRITE_VS, SPRITE_FS) &&
            build(&R.tile, "tilemap", "", TILE_VS, TILE_FS) &&
            build(&R.tile_lit, "tilemap (lit)", "#define LIT 1\n", TILE_VS, TILE_FS) &&
            build(&R.sprite_mask, "sprite (occluder mask)", "#define MASK 1\n", SPRITE_VS, SPRITE_FS) &&
            build(&R.part, "particles", "", PART_VS, SPRITE_FS) &&
            build(&R.inst, "instances", "", INST_VS, SPRITE_FS) && build(&R.inst_lit, "instances (lit)", "#define LIT 1\n", INST_VS, SPRITE_FS) && build(&R.part_lit, "particles (lit)", "#define LIT 1\n", PART_VS, SPRITE_FS) &&
            build(&R.tile_mask, "tilemap (occluder mask)", "#define MASK 1\n", TILE_VS, TILE_FS) &&
            build(&R.sprite_pre, "sprite (depth pre-pass)", "#define PREPASS 1\n", SPRITE_VS, SPRITE_FS) &&
            build(&R.tile_pre, "tilemap (depth pre-pass)", "#define PREPASS 1\n", TILE_VS, TILE_FS) &&
            build(&R.blit, "blit", "", QUAD_VS, BLIT_FS);
  if (!ok) return false;
  r2d_program *tiles[4] = { &R.tile, &R.tile_lit, &R.tile_mask, &R.tile_pre };
  for (int i = 0; i < 4; i++) {
    GLuint p = tiles[i]->prog;
    tiles[i]->u[U_M0] = glGetUniformLocation(p, "u_m0");
    tiles[i]->u[U_M1] = glGetUniformLocation(p, "u_m1");
    tiles[i]->u[U_SET0] = glGetUniformLocation(p, "u_set0");
    tiles[i]->u[U_SET1] = glGetUniformLocation(p, "u_set1");
    tiles[i]->u[U_INV_PAGE] = glGetUniformLocation(p, "u_inv_page");
    tiles[i]->u[U_TINT] = glGetUniformLocation(p, "u_tint");
  }
  R.blit.u[U_PAGE] = glGetUniformLocation(R.blit.prog, "u_page");
  R.blit.u[U_PAGE + 1] = glGetUniformLocation(R.blit.prog, "u_sharp");
  R.part.u[0] = glGetUniformLocation(R.part.prog, "u_pa");
  R.part_lit.u[0] = glGetUniformLocation(R.part_lit.prog, "u_pa");
  R.inst.u[0] = glGetUniformLocation(R.inst.prog, "u_in");
  R.inst_lit.u[0] = glGetUniformLocation(R.inst_lit.prog, "u_in");
  return true;
}

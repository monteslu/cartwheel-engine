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
    "uniform vec4 u_layers[64];\n"
    "uniform vec2 u_target;\n"
    "out vec2 v_uv; out vec4 v_color; flat out float v_page; flat out int v_mode; flat out float v_sdfw; flat out vec2 v_sdf;\n"
    "void main() {\n"
    "  int L = int(a_info.z) * 2;\n"
    "  vec4 r0 = u_layers[L], r1 = u_layers[L + 1];\n"
    "  vec2 p = vec2(r0.x * a_pos.x + r0.y * a_pos.y + r0.z, r1.x * a_pos.x + r1.y * a_pos.y + r1.z);\n"
    "  if (r0.w > 0.5) p = floor(p + 0.5);\n"
    "  gl_Position = vec4(p.x / u_target.x * 2.0 - 1.0, 1.0 - p.y / u_target.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_color = vec4(a_color.rgb * a_color.a, a_color.a);\n"
    "  v_page = a_info.x;\n"
    "  v_mode = int(a_info.y);\n"
    /* SDF: a_info.w = distance change per screen pixel at layer scale 1 (x255);
     * the layer's zoom shrinks it. No derivatives: hosts disagree on fwidth. */
    "  v_sdfw = 0.6 * a_info.w / 255.0 / max(sqrt(abs(r0.x * r1.y - r0.y * r1.x)), 1e-4);\n"
    "  v_sdf = a_sdf.xy;\n"
    "}\n";

static const char *SPRITE_FS =
    "uniform sampler2DArray u_tex;\n"
    "#ifdef LIT\nuniform sampler2DArray u_nrm;\nlayout(location=1) out vec4 o_nrm;\n#endif\n"
    "in vec2 v_uv; in vec4 v_color; flat in float v_page; flat in int v_mode; flat in float v_sdfw; flat in vec2 v_sdf;\n"
    "layout(location=0) out vec4 o_col;\n"
    "void main() {\n"
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
    "#ifdef LIT\n"
    "  vec4 n = texture(u_nrm, vec3(v_uv, v_page));\n"
    "  vec3 enc = (v_mode & 3) != 0 ? vec3(0.5, 0.5, 0.0) : n.rgb;\n"
    "  o_nrm = (v_mode & 4) != 0 ? vec4(0.0) : vec4(enc, 1.0) * c.a;\n"
    "#endif\n"
    "  if ((v_mode & 4) != 0) c.a = 0.0;\n"
    "  o_col = c;\n"
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
    "  gl_Position = vec4(p.x / u_target.x * 2.0 - 1.0, 1.0 - p.y / u_target.y * 2.0, 0.0, 1.0);\n"
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
    "#ifdef LIT\nuniform sampler2DArray u_nrm;\nlayout(location=1) out vec4 o_nrm;\n#endif\n"
    "in vec2 v_uv; flat in float v_page;\n"
    "layout(location=0) out vec4 o_col;\n"
    "void main() {\n"
    "  vec4 c = texture(u_tex, vec3(v_uv, v_page)) * vec4(u_tint.rgb * u_tint.a, u_tint.a);\n"
    "#ifdef LIT\n"
    "  o_nrm = vec4(texture(u_nrm, vec3(v_uv, v_page)).rgb, 1.0) * c.a;\n"
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

/* one point light into the light buffer (stores light / 2) */
static const char *LIGHT_FS =
    "uniform sampler2DArray u_nrm;\n"
    "uniform vec2 u_center;\n" /* GL window coordinates (y up) */
    "uniform vec4 u_light;\n"  /* rgb = colour * energy / 2, a = radius px */
    "uniform vec3 u_params;\n" /* falloff, height px, use normals */
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec2 d = gl_FragCoord.xy - u_center;\n"
    "  float att = pow(clamp(1.0 - length(d) / u_light.a, 0.0, 1.0), u_params.x);\n"
    "  float ndl = 1.0;\n"
    "  if (u_params.z > 0.5) {\n"
    "    vec4 n = texelFetch(u_nrm, ivec3(ivec2(gl_FragCoord.xy), 0), 0);\n"
    "    if (n.a > 0.004) {\n"
    "      vec3 e = n.rgb / n.a;\n"
    "      float mapped = smoothstep(0.0, 0.5, e.z);\n"
    "      vec3 N = normalize(e * 2.0 - 1.0);\n"
    "      vec3 L = normalize(vec3(-d, u_params.y));\n"
    "      ndl = mix(1.0, max(dot(N, L), 0.0), mapped);\n"
    "    }\n"
    "  }\n"
    "  o_col = vec4(u_light.rgb * att * ndl, 0.0);\n"
    "}\n";

static const char *SHADOW_FS = "out vec4 o_col;\nvoid main() { o_col = vec4(0.0); }\n";

/* colour * light into the target, premultiplied over */
static const char *COMPOSITE_FS =
    "uniform sampler2DArray u_tex;\n"
    "uniform sampler2DArray u_light;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  ivec3 p = ivec3(ivec2(gl_FragCoord.xy), 0);\n"
    "  vec4 s = texelFetch(u_tex, p, 0);\n"
    "  vec3 l = texelFetch(u_light, p, 0).rgb * 2.0;\n"
    "  o_col = vec4(s.rgb * l, s.a);\n"
    "}\n";

static const char *BLIT_FS =
    "uniform sampler2DArray u_tex;\n"
    "uniform float u_page;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() { o_col = texture(u_tex, vec3(v_uv, u_page)); }\n";

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
  GLint ul = glGetUniformLocation(prog, "u_light");
  if (ul >= 0 && strstr(fs, "sampler2DArray u_light")) glUniform1i(ul, 2);
  return true;
}


bool r2d_programs_init(void) {
  bool ok = build(&R.sprite, "sprite", "", SPRITE_VS, SPRITE_FS) &&
            build(&R.sprite_lit, "sprite (lit)", "#define LIT 1\n", SPRITE_VS, SPRITE_FS) &&
            build(&R.tile, "tilemap", "", TILE_VS, TILE_FS) &&
            build(&R.tile_lit, "tilemap (lit)", "#define LIT 1\n", TILE_VS, TILE_FS) &&
            build(&R.light, "light", "", QUAD_VS, LIGHT_FS) && build(&R.shadow, "shadow", "", QUAD_VS, SHADOW_FS) &&
            build(&R.composite, "composite", "", QUAD_VS, COMPOSITE_FS) && build(&R.blit, "blit", "", QUAD_VS, BLIT_FS);
  if (!ok) return false;
  r2d_program *tiles[2] = { &R.tile, &R.tile_lit };
  for (int i = 0; i < 2; i++) {
    GLuint p = tiles[i]->prog;
    tiles[i]->u[U_M0] = glGetUniformLocation(p, "u_m0");
    tiles[i]->u[U_M1] = glGetUniformLocation(p, "u_m1");
    tiles[i]->u[U_SET0] = glGetUniformLocation(p, "u_set0");
    tiles[i]->u[U_SET1] = glGetUniformLocation(p, "u_set1");
    tiles[i]->u[U_INV_PAGE] = glGetUniformLocation(p, "u_inv_page");
    tiles[i]->u[U_TINT] = glGetUniformLocation(p, "u_tint");
  }
  R.light.u[U_CENTER] = glGetUniformLocation(R.light.prog, "u_center");
  R.light.u[U_LIGHT] = glGetUniformLocation(R.light.prog, "u_light");
  R.light.u[U_PARAMS] = glGetUniformLocation(R.light.prog, "u_params");
  R.blit.u[U_PAGE] = glGetUniformLocation(R.blit.prog, "u_page");
  return true;
}

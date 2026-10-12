/* SMAA 1x (Jimenez, Echevarria, Sousa, Gutierrez 2012): luma edge detection,
 * blending weights from orthogonal pattern searches with corner rounding,
 * neighbourhood blending. Diagonal pattern detection is off (the reference's
 * MEDIUM / HIGH-without-diagonals configuration: threshold 0.1, 16 search
 * steps, corner rounding 25).
 *
 * The algorithm is the reference's, written for a top-left origin: every
 * fetch of the image, edges and weights goes through FL() (a vertical flip),
 * so the passes run the reference logic verbatim on the mirrored image and
 * the result mirrors back. The two lookup tables are made here instead of
 * shipped: the orthogonal area texture is computed at first use by a port of
 * the reference generator's orthogonal half (AreaTex.py: area(),
 * areaortho(), smootharea(), quadratic distance compression, subsample 0),
 * and the search texture (SearchTex.py) is replaced by the closed form it
 * tabulates: the bilinear fetch is decoded back to its four edges and the
 * deltaLeft / deltaRight rules applied in the shader. */
#include <math.h>
#include <string.h>

#include "pipeline_internal.h"

#define S(...) #__VA_ARGS__ "\n"
#include "pipeline_post_wgsl.h"

/* ── the orthogonal area table ──────────────────────────────────────── */

enum { SIZE_ORTHO = 16, SMOOTH_MAX_DISTANCE = 32, AREA_W = SIZE_ORTHO * 5 };

/* area under the line p1 -> p2 for the pixel x .. x + 1: (below, above) */
static void area(double p1x, double p1y, double p2x, double p2y, double x, double out[2]) {
  double dx = p2x - p1x, dy = p2y - p1y;
  double x1 = x, x2 = x + 1.0;
  double y1 = p1y + dy * (x1 - p1x) / dx, y2 = p1y + dy * (x2 - p1x) / dx;
  bool inside = (x1 >= p1x && x1 < p2x) || (x2 > p1x && x2 <= p2x);
  out[0] = out[1] = 0;
  if (!inside) return;
  bool trapezoid = copysign(1.0, y1) == copysign(1.0, y2) || fabs(y1) < 1e-4 || fabs(y2) < 1e-4;
  if (trapezoid) {
    double a = (y1 + y2) / 2.0;
    if (a < 0) out[0] = fabs(a);
    else out[1] = fabs(a);
    return;
  }
  /* the line crosses the edge inside the pixel: two triangles */
  double xc = -p1y * dx / dy + p1x;
  double ip, fr = modf(xc, &ip);
  double a1 = xc > p1x ? y1 * fr / 2.0 : 0.0;
  double a2 = xc < p2x ? y2 * (1.0 - fr) / 2.0 : 0.0;
  double a = fabs(a1) > fabs(a2) ? a1 : -a2;
  if (a < 0) out[0] = fabs(a1), out[1] = fabs(a2);
  else out[0] = fabs(a2), out[1] = fabs(a1);
}

static void smootharea(double d, const double a1[2], const double a2[2], double out[2]) {
  double p = d / SMOOTH_MAX_DISTANCE;
  p = p < 0 ? 0 : p > 1 ? 1 : p;
  for (int i = 0; i < 2; i++) {
    double b1 = sqrt(a1[i] * 2.0) * 0.5, b2 = sqrt(a2[i] * 2.0) * 0.5;
    out[i] = (b1 + (a1[i] - b1) * p) + (b2 + (a2[i] - b2) * p);
  }
}

static void areaortho(int pattern, double left, double right, double offset, double out[2]) {
  double d = left + right + 1, o1 = 0.5 + offset, o2 = 0.5 + offset - 1.0;
  double a1[2], a2[2], t[2];
  out[0] = out[1] = 0;
  switch (pattern) {
  case 1: if (left <= right) area(0, o2, d / 2, 0, left, out); break;
  case 2: if (left >= right) area(d / 2, 0, d, o2, left, out); break;
  case 3:
    area(0, o2, d / 2, 0, left, a1);
    area(d / 2, 0, d, o2, left, a2);
    smootharea(d, a1, a2, out);
    break;
  case 4: if (left <= right) area(0, o1, d / 2, 0, left, out); break;
  case 6:
    if (fabs(offset) > 0) {
      area(0, o1, d, o2, left, a1);
      area(0, o1, d / 2, 0, left, a2);
      area(d / 2, 0, d, o2, left, t);
      out[0] = (a1[0] + a2[0] + t[0]) / 2, out[1] = (a1[1] + a2[1] + t[1]) / 2;
    } else area(0, o1, d, o2, left, out);
    break;
  case 7: area(0, o1, d, o2, left, out); break;
  case 8: if (left >= right) area(d / 2, 0, d, o1, left, out); break;
  case 9:
    if (fabs(offset) > 0) {
      area(0, o2, d, o1, left, a1);
      area(0, o2, d / 2, 0, left, a2);
      area(d / 2, 0, d, o1, left, t);
      out[0] = (a1[0] + a2[0] + t[0]) / 2, out[1] = (a1[1] + a2[1] + t[1]) / 2;
    } else area(0, o2, d, o1, left, out);
    break;
  case 11: area(0, o2, d, o1, left, out); break;
  case 12:
    area(0, o1, d / 2, 0, left, a1);
    area(d / 2, 0, d, o1, left, a2);
    smootharea(d, a1, a2, out);
    break;
  case 13: area(0, o2, d, o1, left, out); break;
  case 14: area(0, o1, d, o2, left, out); break;
  default: break; /* 0, 5, 10, 15: no blending */
  }
}

/* pattern -> (e1, e2) slots: round(4 * e) of the crossing edges' bilinear fetch */
static const int edgesortho[16][2] = { { 0, 0 }, { 3, 0 }, { 0, 3 }, { 3, 3 }, { 1, 0 }, { 4, 0 }, { 1, 3 }, { 4, 3 },
                                       { 0, 1 }, { 3, 1 }, { 0, 4 }, { 3, 4 }, { 1, 1 }, { 4, 1 }, { 1, 4 }, { 4, 4 } };

static unsigned area_tex;
static void make_area_tex(void) {
  static uint8_t rg[AREA_W * AREA_W * 2];
  memset(rg, 0, sizeof rg);
  for (int p = 0; p < 16; p++)
    for (int l = 0; l < SIZE_ORTHO; l++)
      for (int r = 0; r < SIZE_ORTHO; r++) {
        double a[2];
        areaortho(p, (double)(l * l), (double)(r * r), 0.0, a);
        int x = edgesortho[p][0] * SIZE_ORTHO + l, y = edgesortho[p][1] * SIZE_ORTHO + r;
        for (int c = 0; c < 2; c++) {
          double v = a[c] * 255.0 + 0.5;
          rg[(y * AREA_W + x) * 2 + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
      }
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* rows as glTexImage2D gets them: both index memory rows by v */
    area_tex = cw__pw_rg8(AREA_W, AREA_W, rg);
    return;
  }
  glGenTextures(1, &area_tex);
  glBindTexture(GL_TEXTURE_2D, area_tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, AREA_W, AREA_W, 0, GL_RG, GL_UNSIGNED_BYTE, rg);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
}

/* ── shaders ────────────────────────────────────────────────────────── */

static const char *SMAA_COMMON =
    "#define SMAA_THRESHOLD 0.1\n"
    "#define SMAA_MAX_SEARCH_STEPS 16\n"
    "#define SMAA_CORNER_ROUNDING_NORM 0.25\n"
    "#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0\n"
    "#define SMAA_AREATEX_MAX_DISTANCE 16.0\n"
    S(uniform vec4 SMAA_RT_METRICS;
      vec2 FL(vec2 c) { return vec2(c.x, 1.0 - c.y); }
      vec4 SMP(sampler2D t, vec2 c) { return texture(t, FL(c)); }
      vec4 SMPO(sampler2D t, vec2 c, vec2 o) { return texture(t, FL(c + o * SMAA_RT_METRICS.xy)); });

static const char *SMAA_EDGE_FS = S(
  uniform sampler2D u_color;
  out vec4 o;
  void main() {
    vec2 texcoord = FL(v_uv);
    vec4 offset0 = SMAA_RT_METRICS.xyxy * vec4(-1.0, 0.0, 0.0, -1.0) + texcoord.xyxy;
    vec4 offset1 = SMAA_RT_METRICS.xyxy * vec4(1.0, 0.0, 0.0, 1.0) + texcoord.xyxy;
    vec4 offset2 = SMAA_RT_METRICS.xyxy * vec4(-2.0, 0.0, 0.0, -2.0) + texcoord.xyxy;
    vec2 threshold = vec2(SMAA_THRESHOLD);
    vec3 weights = vec3(0.2126, 0.7152, 0.0722);
    float L = dot(SMP(u_color, texcoord).rgb, weights);
    float Lleft = dot(SMP(u_color, offset0.xy).rgb, weights);
    float Ltop = dot(SMP(u_color, offset0.zw).rgb, weights);
    vec4 delta;
    delta.xy = abs(L - vec2(Lleft, Ltop));
    vec2 edges = step(threshold, delta.xy);
    if (dot(edges, vec2(1.0)) == 0.0) discard;
    float Lright = dot(SMP(u_color, offset1.xy).rgb, weights);
    float Lbottom = dot(SMP(u_color, offset1.zw).rgb, weights);
    delta.zw = abs(L - vec2(Lright, Lbottom));
    vec2 maxDelta = max(delta.xy, delta.zw);
    float Lleftleft = dot(SMP(u_color, offset2.xy).rgb, weights);
    float Ltoptop = dot(SMP(u_color, offset2.zw).rgb, weights);
    delta.zw = abs(vec2(Lleft, Ltop) - vec2(Lleftleft, Ltoptop));
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    o = vec4(edges, 0.0, 1.0);
  });

static const char *SMAA_WEIGHT_FS = S(
  uniform sampler2D u_edges;
  uniform sampler2D u_area;
  out vec4 o;
  /* SearchTex in closed form: the bilinear fetch at (-0.25, -0.125) mixes
   * four edges as 0.125 * (0.25 e0 + 0.75 e1) + 0.875 * (0.25 e2 + 0.75 e3);
   * decode them and apply SearchTex.py's deltaLeft / deltaRight. Returns
   * the search length correction 3.25 - delta (what the reference computes
   * as -(255/127) * SMAASearchLength + 3.25). */
  vec4 decode4(float v) {
    float best = 1e9;
    vec4 bits = vec4(0.0);
    for (int i = 0; i < 16; i++) {
      vec4 b = vec4(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1), float((i >> 3) & 1));
      float f = 0.125 * (0.25 * b.x + 0.75 * b.y) + 0.875 * (0.25 * b.z + 0.75 * b.w);
      float d = abs(f - v);
      if (d < best) { best = d; bits = b; }
    }
    return bits;
  }
  float searchCorrection(vec2 e, bool right) {
    vec4 left = decode4(e.x), top = decode4(e.y);
    float d = 0.0;
    if (!right) {
      if (top.w > 0.5) d += 1.0;
      if (d == 1.0 && top.z > 0.5 && left.y < 0.5 && left.w < 0.5) d += 1.0;
    } else {
      if (top.w > 0.5 && left.y < 0.5 && left.w < 0.5) d += 1.0;
      if (d == 1.0 && top.z > 0.5 && left.x < 0.5 && left.z < 0.5) d += 1.0;
    }
    return 3.25 - d;
  }
  float SMAASearchXLeft(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    for (int i = 0; i < 64; i++) {
      if (!(texcoord.x > end && e.g > 0.8281 && e.r == 0.0)) break;
      e = SMP(u_edges, texcoord).rg;
      texcoord = -vec2(2.0, 0.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    return SMAA_RT_METRICS.x * searchCorrection(e, false) + texcoord.x;
  }
  float SMAASearchXRight(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    for (int i = 0; i < 64; i++) {
      if (!(texcoord.x < end && e.g > 0.8281 && e.r == 0.0)) break;
      e = SMP(u_edges, texcoord).rg;
      texcoord = vec2(2.0, 0.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    return -SMAA_RT_METRICS.x * searchCorrection(e, true) + texcoord.x;
  }
  float SMAASearchYUp(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    for (int i = 0; i < 64; i++) {
      if (!(texcoord.y > end && e.r > 0.8281 && e.g == 0.0)) break;
      e = SMP(u_edges, texcoord).rg;
      texcoord = -vec2(0.0, 2.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    return SMAA_RT_METRICS.y * searchCorrection(e.gr, false) + texcoord.y;
  }
  float SMAASearchYDown(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    for (int i = 0; i < 64; i++) {
      if (!(texcoord.y < end && e.r > 0.8281 && e.g == 0.0)) break;
      e = SMP(u_edges, texcoord).rg;
      texcoord = vec2(0.0, 2.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    return -SMAA_RT_METRICS.y * searchCorrection(e.gr, true) + texcoord.y;
  }
  vec2 SMAAArea(vec2 dist, float e1, float e2) {
    vec2 tc = vec2(SMAA_AREATEX_MAX_DISTANCE) * floor(4.0 * vec2(e1, e2) + 0.5) + dist;
    tc = (tc + 0.5) / (5.0 * SMAA_AREATEX_MAX_DISTANCE);
    return texture(u_area, tc).rg;
  }
  void SMAADetectHorizontalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0);
    factor.x -= rounding.x * SMPO(u_edges, texcoord.xy, vec2(0.0, 1.0)).r;
    factor.x -= rounding.y * SMPO(u_edges, texcoord.zw, vec2(1.0, 1.0)).r;
    factor.y -= rounding.x * SMPO(u_edges, texcoord.xy, vec2(0.0, -2.0)).r;
    factor.y -= rounding.y * SMPO(u_edges, texcoord.zw, vec2(1.0, -2.0)).r;
    weights *= clamp(factor, 0.0, 1.0);
  }
  void SMAADetectVerticalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0);
    factor.x -= rounding.x * SMPO(u_edges, texcoord.xy, vec2(1.0, 0.0)).g;
    factor.x -= rounding.y * SMPO(u_edges, texcoord.zw, vec2(1.0, 1.0)).g;
    factor.y -= rounding.x * SMPO(u_edges, texcoord.xy, vec2(-2.0, 0.0)).g;
    factor.y -= rounding.y * SMPO(u_edges, texcoord.zw, vec2(-2.0, 1.0)).g;
    weights *= clamp(factor, 0.0, 1.0);
  }
  void main() {
    vec2 texcoord = FL(v_uv);
    vec2 pixcoord = texcoord * SMAA_RT_METRICS.zw;
    vec4 offset0 = SMAA_RT_METRICS.xyxy * vec4(-0.25, -0.125, 1.25, -0.125) + texcoord.xyxy;
    vec4 offset1 = SMAA_RT_METRICS.xyxy * vec4(-0.125, -0.25, -0.125, 1.25) + texcoord.xyxy;
    vec4 offset2 = SMAA_RT_METRICS.xxyy * (vec4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS)) +
                   vec4(offset0.xz, offset1.yw);
    vec4 weights = vec4(0.0);
    vec2 e = SMP(u_edges, texcoord).rg;
    if (e.g > 0.0) {
      vec2 d;
      vec3 coords;
      coords.x = SMAASearchXLeft(offset0.xy, offset2.x);
      coords.y = offset1.y;
      d.x = coords.x;
      float e1 = SMP(u_edges, coords.xy).r;
      coords.z = SMAASearchXRight(offset0.zw, offset2.y);
      d.y = coords.z;
      d = floor(abs(SMAA_RT_METRICS.zz * d - pixcoord.xx) + 0.5);
      vec2 sqrt_d = sqrt(d);
      float e2 = SMPO(u_edges, coords.zy, vec2(1.0, 0.0)).r;
      weights.rg = SMAAArea(sqrt_d, e1, e2);
      coords.y = texcoord.y;
      SMAADetectHorizontalCornerPattern(weights.rg, coords.xyzy, d);
    }
    if (e.r > 0.0) {
      vec2 d;
      vec3 coords;
      coords.y = SMAASearchYUp(offset1.xy, offset2.z);
      coords.x = offset0.x;
      d.x = coords.y;
      float e1 = SMP(u_edges, coords.xy).g;
      coords.z = SMAASearchYDown(offset1.zw, offset2.w);
      d.y = coords.z;
      d = floor(abs(SMAA_RT_METRICS.ww * d - pixcoord.yy) + 0.5);
      vec2 sqrt_d = sqrt(d);
      float e2 = SMPO(u_edges, coords.xz, vec2(0.0, 1.0)).g;
      weights.ba = SMAAArea(sqrt_d, e1, e2);
      coords.x = texcoord.x;
      SMAADetectVerticalCornerPattern(weights.ba, coords.xyxz, d);
    }
    o = weights;
  });

static const char *SMAA_BLEND_FS = S(
  uniform sampler2D u_color;
  uniform sampler2D u_blend;
  out vec4 o;
  void main() {
    vec2 texcoord = FL(v_uv);
    vec4 offset = SMAA_RT_METRICS.xyxy * vec4(1.0, 0.0, 0.0, 1.0) + texcoord.xyxy;
    vec4 a;
    a.x = SMP(u_blend, offset.xy).a;
    a.y = SMP(u_blend, offset.zw).g;
    a.wz = SMP(u_blend, texcoord).xz;
    if (dot(a, vec4(1.0)) < 1e-5) {
      o = SMP(u_color, texcoord);
    } else {
      bool h = max(a.x, a.z) > max(a.y, a.w);
      vec4 blendingOffset = h ? vec4(a.x, 0.0, a.z, 0.0) : vec4(0.0, a.y, 0.0, a.w);
      vec2 blendingWeight = h ? a.xz : a.yw;
      blendingWeight /= dot(blendingWeight, vec2(1.0));
      vec4 blendingCoord = blendingOffset * vec4(SMAA_RT_METRICS.xy, -SMAA_RT_METRICS.xy) + texcoord.xyxy;
      vec4 color = blendingWeight.x * SMP(u_color, blendingCoord.xy);
      color += blendingWeight.y * SMP(u_color, blendingCoord.zw);
      o = color;
    }
  });

/* ── the pass ───────────────────────────────────────────────────────── */

static unsigned edge_prog, weight_prog, blend_prog;

static unsigned program(const char *name, const char *body, const char *wgsl) {
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  const char *common = wg ? WGSL_SMAA_COMMON : SMAA_COMMON;
  if (wg) body = wgsl;
  size_t a = strlen(common), b = strlen(body);
  char *src = cw_alloc(a + b + 1, CW_MEM_RENDER);
  memcpy(src, common, a);
  memcpy(src + a, body, b + 1);
  unsigned p = wg ? cw_pass_program_wgsl(name, src) : cw_pass_program(name, src);
  cw_free(src);
  return p;
}

void cw__smaa_exec(cw_pass_ctx *ctx);
void cw__smaa_exec(cw_pass_ctx *ctx) {
  if (!edge_prog) {
    edge_prog = program("smaa.edges", SMAA_EDGE_FS, WGSL_SMAA_EDGE);
    weight_prog = program("smaa.weights", SMAA_WEIGHT_FS, WGSL_SMAA_WEIGHT);
    blend_prog = program("smaa.blend", SMAA_BLEND_FS, WGSL_SMAA_BLEND);
    make_area_tex();
  }
  if (!edge_prog || !weight_prog || !blend_prog) return;
  float m[4] = { 1.0f / (float)ctx->width, 1.0f / (float)ctx->height, (float)ctx->width, (float)ctx->height };
  const cw_rt *edges = cw_pass_rt(ctx, ctx->width, ctx->height, CW_RT_RGBA8, false);
  const cw_rt *weights = cw_pass_rt(ctx, ctx->width, ctx->height, CW_RT_RGBA8, false);
  if (!edges || !weights) return;
  /* edges (where no edge is found the pass discards: start from zero) */
  cw_pass_bind_target(ctx, edges);
  cw_pass_clear(0, 0, 0, 0, false);
  cw_pass_use(edge_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_uniform1i(edge_prog, "u_color", 0);
  cw_pass_uniform4f(edge_prog, "SMAA_RT_METRICS", m[0], m[1], m[2], m[3]);
  cw_pass_fullscreen();
  /* blending weights */
  cw_pass_bind_target(ctx, weights);
  cw_pass_use(weight_prog);
  cw_pass_bind_texture(0, edges->tex);
  cw_pass_bind_texture(1, area_tex);
  cw_pass_uniform1i(weight_prog, "u_edges", 0);
  cw_pass_uniform1i(weight_prog, "u_area", 1);
  cw_pass_uniform4f(weight_prog, "SMAA_RT_METRICS", m[0], m[1], m[2], m[3]);
  cw_pass_fullscreen();
  /* neighbourhood blending into the chain's target */
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(blend_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, weights->tex);
  cw_pass_uniform1i(blend_prog, "u_color", 0);
  cw_pass_uniform1i(blend_prog, "u_blend", 1);
  cw_pass_uniform4f(blend_prog, "SMAA_RT_METRICS", m[0], m[1], m[2], m[3]);
  cw_pass_fullscreen();
  cw_pass_rt_release(ctx, edges);
  cw_pass_rt_release(ctx, weights);
}

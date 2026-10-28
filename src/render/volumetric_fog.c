/* Volumetric fog (cartwheel/environment.h): HeightFog's volumetric mode and
 * FogVolume3D, lit per cell of a froxel grid (a 3D grid aligned with the
 * view frustum, its slices crowding toward the camera).
 *
 * The algorithm is Wronski's ("Volumetric fog", SIGGRAPH 2014) and
 * Hillaire's ("Physically based and unified volumetric rendering in
 * Frostbite", SIGGRAPH 2015), as Godot's volumetric_fog*.glsl (MIT) lays it
 * out (NOTICE):
 *   1. shadows: the sun and up to three spot lights get depth maps of their
 *      own (tiles of one atlas), fitted to the grid's reach, drawn with
 *      three.c's caster programs (t3_renderer_render_depth_to);
 *   2. inject: per cell, at a jittered point, the medium (the HeightFog's
 *      exponential density, every FogVolume3D: box, ellipsoid or world, with
 *      edge fade, height falloff and value noise) and the light it scatters
 *      toward the camera (ambient, each light through a Henyey-Greenstein
 *      phase, shadowed), blended with the previous frames reprojected
 *      (temporal: it hides the jitter's noise);
 *   3. integrate front to back along each column (Hillaire's
 *      energy-conserving step): in-scattered light and transmittance up to
 *      each slice;
 *   4. apply: an EFFECTS pass looks the integrated grid up at each pixel's
 *      depth (the analytic height fog continues past the grid).
 * GL (GLES 3.0): inject and integrate are fragment passes into the layers of
 * RGBA16F 3D textures (several layers a draw through multiple render
 * targets; integration carries its running sum in a 2D target between
 * draws). WebGPU: compute shaders into storage textures
 * (volumetric_fog_wgpu.c). */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/environment.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "environment_internal.h"
#include "pipeline_internal.h"
#include "volumetric_fog_internal.h"
#define S(...) #__VA_ARGS__ "\n"

_Static_assert(sizeof(vf_light_u) == 144, "vf_light_u is std140 / WGSL sized");
_Static_assert(sizeof(vf_vol_u) == 128, "vf_vol_u is std140 / WGSL sized");
_Static_assert(sizeof(vf_uniforms) == 256 + 144 * VF_MAX_LIGHTS + 128 * VF_MAX_VOLUMES, "vf_uniforms layout");

/* ── FogVolume3D ────────────────────────────────────────────────────── */

typedef struct fog_volume {
  int shape; /* 0 box, 1 ellipsoid, 2 world */
  t3_vec3 size;
  float density;
  t3_color albedo, emission;
  float emission_energy, height_falloff, edge_fade, noise, noise_scale;
  t3_vec3 noise_wind;
} fog_volume;

/* ── the tree's volumetric state ────────────────────────────────────── */

typedef struct vf_light_in { const t3_light *l; int kind; bool shadow; float energy; } vf_light_in;
typedef struct vf_vol_in { const t3_object *o; fog_volume p; } vf_vol_in;

/* the GL resources (one grid; remade when the size changes) */
typedef struct vf_gl {
  int w, h, d, atlas;
  unsigned scatter[2], integ, carry[2], shadow_tex, shadow_fbo, ubo;
  unsigned inj_fbo[2][32], int_fbo[64];
  int n_inj, n_int;
} vf_gl;

/* one view's grid: splitscreen gives each view its own (its own camera's
 * history, its own uniforms), sized to the view's share of the screen */
typedef struct vf_slot {
  vf_uniforms u;
  bool built;            /* the grid was built this frame (the apply pass reads it) */
  uint64_t built_frame;  /* ctx->frame of the last build */
  bool have_history;
  int cur;               /* the scatter volume written this frame */
  float apply_sun[6];    /* the analytic tail's sun: direction, colour (albedo applied) */
  vf_gl gl;
  vf_wg *wg;
} vf_slot;

typedef struct vfog_rt {
  uint64_t stamp_l, stamp_v; /* cw_frame_index() the entries belong to */
  vf_light_in lights[VF_MAX_LIGHTS];
  int nl;
  vf_vol_in vols[VF_MAX_VOLUMES];
  int nv;
  bool warned_lights, warned_vols, warned_ortho;
  cw_volumetric_fog_state st;
  t3_camera *cam_dir, *cam_spot[3];
  vf_slot slot[CW_MAX_VIEWS]; /* [0] a single view's; [i] splitscreen view i's */
} vfog_rt;

/* test hook (gate fog-split's control): CW_TEST_VFOG_ONE_GRID=1 every splitscreen view shares one full-size grid,
 * its history and its uniforms, as before views had their own */
static bool one_grid(void) {
#if CW_CART
  return false;
#else
  static int on = -1;
  if (on < 0) on = getenv("CW_TEST_VFOG_ONE_GRID") && atoi(getenv("CW_TEST_VFOG_ONE_GRID")) != 0;
  return on != 0;
#endif
}

/* the grid of the view being drawn */
static vf_slot *slot_of(vfog_rt *v, const cw_pass_ctx *ctx) {
  int i = ctx->pipeline && !one_grid() ? ctx->pipeline->view_i : -1;
  return &v->slot[i >= 0 && i < CW_MAX_VIEWS ? i : 0];
}
/* the view's share of the output's width (1 without splitscreen): its grid and shadow tile shrink with it, so a
 * cell covers as many pixels in a quarter-screen view as in a full one */
static float view_share(const cw_pass_ctx *ctx) {
  const cw_pipeline *p = ctx->pipeline;
  if (!p || one_grid() || p->view_i < 0 || p->view_i >= p->nviews) return 1;
  float w = p->views[p->view_i].w;
  return w > 0 && w < 1 ? w : 1;
}

static bool wgpu(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }

bool cw__vfog_supported(void) {
  if (wgpu()) return true;
  return cw_pipeline_caps_get()->float_render;
}

static void gl_free(vf_gl *g);

static void vfog_free(void *p) {
  vfog_rt *v = p;
  if (v->cam_dir) t3_release(v->cam_dir);
  for (int i = 0; i < 3; i++)
    if (v->cam_spot[i]) t3_release(v->cam_spot[i]);
  for (int i = 0; i < CW_MAX_VIEWS; i++) {
    if (!wgpu()) gl_free(&v->slot[i].gl);
    else cw__vf_wg_free(v->slot[i].wg);
  }
  cw_free(v);
}

static bool vf_build_active(const cw_pass_ctx *ctx, void *user);
static void vf_build_exec(cw_pass_ctx *ctx, void *user);
static bool vf_apply_active(const cw_pass_ctx *ctx, void *user);
static void vf_apply_exec(cw_pass_ctx *ctx, void *user);

static vfog_rt *vfog_of(cw_tree *t, bool make) {
  vfog_rt *v = cw_tree_slot(t, "vfog");
  if (v || !make) return v;
  v = cw_calloc(1, sizeof *v, CW_MEM_RENDER);
  cw_tree_set_slot(t, "vfog", v, vfog_free);
  cw_pipeline *p = cw_pipeline_of(t);
  /* the grid first (shadow maps, inject, integrate: targets of its own), then
   * the apply over the scene, where the analytic height fog would draw */
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "volumetric_fog.grid", .phase = CW_PHASE_EFFECTS, .order = 290,
                                           .active = vf_build_active, .execute = vf_build_exec, .user = v, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "volumetric_fog", .phase = CW_PHASE_EFFECTS, .order = 300,
                                           .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR, .active = vf_apply_active,
                                           .execute = vf_apply_exec, .user = v, .webgpu = true });
  return v;
}

void cw__vfog_touch(cw_tree *t) { vfog_of(t, true); }

void cw__vfog_push_light(cw_tree *t, const t3_light *l, int kind, bool shadow, float fog_energy) {
  vfog_rt *v = vfog_of(t, false);
  if (!v) return;
  uint64_t f = cw_frame_index();
  if (v->stamp_l != f) v->stamp_l = f, v->nl = 0;
  for (int i = 0; i < v->nl; i++)
    if (v->lights[i].l == l) {
      v->lights[i] = (vf_light_in){ l, kind, shadow, fog_energy };
      return;
    }
  if (v->nl >= VF_MAX_LIGHTS) {
    if (!v->warned_lights) cw_warn("volumetric fog: more than %d lights with fog_energy > 0; the rest light no fog", VF_MAX_LIGHTS);
    v->warned_lights = true;
    return;
  }
  v->lights[v->nl++] = (vf_light_in){ l, kind, shadow, fog_energy };
}

static void push_volume(cw_tree *t, const t3_object *o, const fog_volume *p) {
  vfog_rt *v = vfog_of(t, true);
  uint64_t f = cw_frame_index();
  if (v->stamp_v != f) v->stamp_v = f, v->nv = 0;
  for (int i = 0; i < v->nv; i++)
    if (v->vols[i].o == o) {
      v->vols[i].p = *p;
      return;
    }
  if (v->nv >= VF_MAX_VOLUMES) {
    if (!v->warned_vols) cw_warn("volumetric fog: more than %d FogVolume3D nodes in view; the rest are not drawn", VF_MAX_VOLUMES);
    v->warned_vols = true;
    return;
  }
  v->vols[v->nv++] = (vf_vol_in){ o, *p };
}

void cw_volumetric_fog_get_state(cw_tree *t, cw_volumetric_fog_state *out) {
  memset(out, 0, sizeof *out);
  vfog_rt *v = t ? vfog_of(t, false) : NULL;
  if (v) *out = v->st;
}

/* ── quality ────────────────────────────────────────────────────────── */

/* effects detail (cartwheel/quality.h) -> the grid: cells across (down
 * follows the aspect), slices, the shadow tile and whether spot lights get
 * shadows */
typedef struct vf_preset { int gw, d, tile; bool spot_shadows; } vf_preset;
static const vf_preset PRESETS[CW_QUALITY_COUNT] = {
  { 64, 32, 256, false },  /* Low */
  { 128, 64, 512, true },   /* Medium */
  { 160, 64, 1024, true }, /* High */
  { 240, 96, 1024, true }, /* Ultra */
};

/* ── shaders ────────────────────────────────────────────────────────── */

static const char VF_GLSL_COMMON[] =
    "#define MAX_L 8\n#define MAX_V 16\n"
    S(struct FogLight { vec4 pos; vec4 dir; vec4 color; vec4 cone; vec4 tile; mat4 shadow; };
      struct FogVol { mat4 inv; vec4 a; vec4 albedo; vec4 emission; vec4 wind; };
      layout(std140) uniform FogU {
        mat4 inv_view; mat4 prev_vp; vec4 grid; vec4 proj; vec4 slices; vec4 jitter; vec4 medium; vec4 ambient;
        vec4 albedo; vec4 counts; FogLight lights[MAX_L]; FogVol vols[MAX_V];
      } u;
      float vf_slice_dist(float s) { return u.slices.x * pow(max(s * u.grid.w, 1e-10), u.slices.y); }
      vec3 vf_ray(vec2 cell) {
        vec2 ndc = cell / u.grid.xy * 2.0 - 1.0;
        return vec3((ndc.x + u.proj.z) / u.proj.x, (ndc.y + u.proj.w) / u.proj.y, -1.0);
      }
      uint vf_hash(uvec3 v) {
        v = v * 1664525u + 1013904223u;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        v ^= v >> 16u;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        return v.x;
      }
      float vf_h(ivec3 p) { return float(vf_hash(uvec3(p)) >> 8u) / 16777216.0; }
      float vf_vnoise(vec3 x) {
        vec3 i = floor(x);
        vec3 f = x - i;
        f = f * f * (3.0 - 2.0 * f);
        ivec3 b = ivec3(i);
        float a0 = mix(vf_h(b), vf_h(b + ivec3(1, 0, 0)), f.x);
        float a1 = mix(vf_h(b + ivec3(0, 1, 0)), vf_h(b + ivec3(1, 1, 0)), f.x);
        float a2 = mix(vf_h(b + ivec3(0, 0, 1)), vf_h(b + ivec3(1, 0, 1)), f.x);
        float a3 = mix(vf_h(b + ivec3(0, 1, 1)), vf_h(b + ivec3(1, 1, 1)), f.x);
        return mix(mix(a0, a1, f.y), mix(a2, a3, f.y), f.z);
      }
      float vf_noise(vec3 x) { return vf_vnoise(x) * 0.6667 + vf_vnoise(x * 2.03 + vec3(17.1, 5.3, 11.7)) * 0.3333; }
      float vf_hg(float mu, float g) {
        float g2 = g * g;
        return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
      }
      /* Hillaire's step: the light scattered over ds of a medium with
       * extinction e (s.a) and in-scattering s.rgb, through what is left of acc */
      /* what a half-float cell may hold: a light too strong for RGBA16F (65504 at most) would store an
       * infinity there, and one bad frame (a NaN from any input) a NaN; the temporal history (and its
       * linear fetch) would carry and spread either for good. A NaN becomes 0 (mix with a bvec4 is a
       * select, no arithmetic), the rest is clamped (light to 60000; extinction to 64 per unit, opaque
       * within a few centimetres, so a broken cell's extinction fades out with the blend in about a
       * second instead of the minutes 60000 would take). u.counts.z > 0.5 turns the bound off and u.counts.w
       * > 0.5 puts an infinity into the left quarter of the grid (tests/fog-flare.mjs) */
      vec4 vf_bound(vec4 c) { return u.counts.z > 0.5 ? c : clamp(mix(c, vec4(0.0), isnan(c)), vec4(0.0), vec4(60000.0, 60000.0, 60000.0, 64.0)); }
      /* the history a cell blends in: bounded, and at most 8 times what the cell holds now (plus a little), so
       * a cell that went bad, or a light switched off, fades in a frame or two rather than over seconds */
      vec4 vf_hist(vec4 cur, vec4 h) { return u.counts.z > 0.5 ? h : min(vf_bound(h), cur * 8.0 + vec4(0.02)); }
      vec4 vf_acc(vec4 acc, vec4 s, float ds) {
        float e = s.a;
        float tr = exp(-e * ds);
        float w = e * ds < 1e-3 ? ds * (1.0 - 0.5 * e * ds) : (1.0 - tr) / e;
        return vec4(acc.rgb + acc.a * s.rgb * w, acc.a * tr);
      });

static const char VF_GLSL_INJECT[] = S(
  uniform highp sampler3D u_hist;
  uniform highp sampler2DShadow u_shadow;
  uniform int u_z0;
  float vf_shadow(int i, vec3 p) {
    vec4 c = u.lights[i].shadow * vec4(p, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 n = c.xyz / c.w;
    if (abs(n.x) > 1.0 || abs(n.y) > 1.0 || n.z > 1.0) return 1.0;
    vec2 uv = (n.xy * 0.5 + 0.5) * u.lights[i].tile.zw + u.lights[i].tile.xy;
    return texture(u_shadow, vec3(uv, n.z * 0.5 + 0.5 - u.lights[i].cone.w));
  }
  vec4 vf_inject(ivec3 c) {
    vec3 cell = vec3(c) + 0.5 + u.jitter.xyz;
    vec3 rv = vf_ray(cell.xy) * vf_slice_dist(cell.z);
    vec3 p = (u.inv_view * vec4(rv, 1.0)).xyz;
    vec3 v = normalize(p - u.inv_view[3].xyz);
    float ext = 0.0;
    vec3 sca = vec3(0.0), emi = vec3(0.0);
    if (u.medium.x > 0.0) {
      float d = u.medium.x * exp(min(-u.medium.y * (p.y - u.medium.z), 10.0));
      ext += d;
      sca += d * u.albedo.rgb;
    }
    int nv = int(u.counts.y);
    for (int i = 0; i < MAX_V; i++) {
      if (i >= nv) break;
      vec3 q = (u.vols[i].inv * vec4(p, 1.0)).xyz;
      vec4 a = u.vols[i].a;
      float r = a.y < 0.5 ? max(abs(q.x), max(abs(q.y), abs(q.z))) : a.y < 1.5 ? length(q) : 0.0;
      if (r >= 1.0) continue;
      float d = a.x * (a.z > 0.0 ? clamp((1.0 - r) / a.z, 0.0, 1.0) : 1.0);
      if (a.w > 0.0) d *= exp(min(-a.w * (p.y - u.vols[i].wind.w), 10.0));
      if (u.vols[i].albedo.w > 0.0) {
        float n = vf_noise((p - u.vols[i].wind.xyz * u.jitter.w) * u.vols[i].emission.w);
        d *= mix(1.0, clamp(n * 2.0 - 0.5, 0.0, 1.0), u.vols[i].albedo.w);
      }
      ext += d;
      sca += d * u.vols[i].albedo.rgb;
      emi += d * u.vols[i].emission.rgb;
    }
    vec3 li = u.ambient.rgb;
    if (ext > 0.0) {
      int nl = int(u.counts.x);
      for (int i = 0; i < MAX_L; i++) {
        if (i >= nl) break;
        vec4 lp = u.lights[i].pos;
        vec3 l = lp.xyz;
        float att = 1.0;
        if (lp.w > 0.5) {
          vec3 dv = lp.xyz - p;
          float dist = length(dv);
          l = dv / max(dist, 1e-4);
          att = 1.0 / max(pow(max(dist, 1e-4), u.lights[i].color.w), 0.01);
          float rg = u.lights[i].dir.w;
          if (rg > 0.0) {
            float k = dist / rg;
            float w = clamp(1.0 - k * k * k * k, 0.0, 1.0);
            att *= w * w;
          }
          if (lp.w > 1.5) att *= smoothstep(u.lights[i].cone.x, u.lights[i].cone.y, dot(-l, u.lights[i].dir.xyz));
        }
        if (att <= 0.0) continue;
        if (u.lights[i].cone.z > 0.5) att *= vf_shadow(i, p);
        li += u.lights[i].color.rgb * att * vf_hg(dot(v, l), u.medium.w);
      }
    }
    vec4 res = vec4(sca * li + emi, ext);
    if (u.counts.w > 0.5 && float(c.x) < u.grid.x * 0.25) res = vec4(u.counts.w * 3e38 * 10.0);
    res = vf_bound(res);
    if (u.slices.w > 0.0) {
      vec4 pc = u.prev_vp * vec4(p, 1.0);
      if (pc.w > 1e-4) {
        vec3 h = vec3(pc.xy / pc.w * 0.5 + 0.5, pow(clamp(pc.w / u.slices.x, 1e-10, 2.0), u.slices.z));
        if (all(greaterThanEqual(h, vec3(0.0))) && all(lessThanEqual(h, vec3(1.0)))) res = mix(res, vf_hist(res, texture(u_hist, h)), u.slices.w);
      }
    }
    return res;
  });

static const char VF_GLSL_INTEGRATE[] = S(
  uniform highp sampler3D u_scatter;
  uniform highp sampler2D u_carry;
  uniform int u_z0;
  uniform int u_first;
  vec4 vf_step(ivec2 xy, int z, float rl, vec4 acc) {
    if (z >= int(u.grid.z)) return acc;
    vec4 s = texelFetch(u_scatter, ivec3(xy, z), 0);
    return vf_acc(acc, s, (vf_slice_dist(float(z + 1)) - vf_slice_dist(float(z))) * rl);
  });

/* apply: the grid at each pixel's depth, then the analytic height fog past
 * it; premultiplied (in-scattered light, 1 - transmittance) */
static const char VF_GLSL_APPLY[] = S(
  uniform sampler2D u_depth;
  uniform highp sampler3D u_vol;
  uniform mat4 u_inv_view_proj;
  uniform vec3 u_cam;
  uniform vec3 u_fwd;
  uniform vec4 u_pz;
  uniform vec4 u_grid;
  uniform vec4 u_fog;
  uniform vec4 u_dist;
  uniform vec3 u_color;
  uniform vec3 u_sun_dir;
  uniform vec3 u_sun_color;
  uniform float u_g;
  out vec4 o;
  float vf_od(vec3 v, float l) {
    float a = u_fog.x * exp(-u_fog.y * (u_cam.y - u_fog.z));
    float k = u_fog.y * v.y * l;
    return a * l * (abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k);
  }
  float vf_hg(float mu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
  }

  vec4 vf_smooth_volume(vec3 uvw) {
    vec2 size = vec2(textureSize(u_vol, 0).xy);
    vec2 p = uvw.xy * size - 0.5;
    vec2 cell = floor(p), f = fract(p), f2 = f*f, f3 = f2*f;
    vec2 w0 = (1.0-f)*(1.0-f)*(1.0-f)/6.0;
    vec2 w1 = (3.0*f3-6.0*f2+4.0)/6.0;
    vec2 w2 = (-3.0*f3+3.0*f2+3.0*f+1.0)/6.0;
    vec2 w3 = f3/6.0, g0 = w0+w1, g1 = w2+w3;
    vec2 h0 = (cell-1.0+w1/g0+0.5)/size;
    vec2 h1 = (cell+1.0+w3/g1+0.5)/size;
    return mix(mix(texture(u_vol,vec3(h0,uvw.z)), texture(u_vol,vec3(h1.x,h0.y,uvw.z)),g1.x),
               mix(texture(u_vol,vec3(h0.x,h1.y,uvw.z)), texture(u_vol,vec3(h1,uvw.z)),g1.x),g1.y);
  }
  void main() {
    float d = texture(u_depth, v_uv).r;
    vec4 a = u_inv_view_proj * vec4(v_uv * 2.0 - 1.0, -1.0, 1.0);
    vec3 v = normalize(a.xyz / a.w - u_cam);
    float vz = max(dot(v, u_fwd), 1e-4);
    bool sky = d >= 1.0;
    float z = d * 2.0 - 1.0;
    float dist = sky ? 1e30 : -((u_pz.y - z * u_pz.w) / (z * u_pz.z - u_pz.x));
    /* a slice nearer than the depth: the cell that holds a surface also holds air behind it (lit outside a wall) */
    float c = max(u_grid.x * pow(clamp(dist / u_grid.y, 1e-10, 1.0), u_grid.z) - 1.0, 0.0);
    vec4 vol = vf_smooth_volume(vec3(v_uv, (c - 0.5) / u_grid.x));
    float k = clamp(c, 0.0, 1.0);
    vec3 rgb = vol.rgb * k;
    float tr = mix(1.0, vol.a, k);
    if (u_grid.w > 0.5) {
      float l0 = u_grid.y / vz;
      float l1 = sky ? u_dist.y : dist / vz;
      if (l1 > l0) {
        float f = min(1.0 - exp(-(vf_od(v, l1) - vf_od(v, l0))), u_fog.w);
        rgb += tr * (u_color + u_sun_color * vf_hg(dot(v, u_sun_dir), u_g)) * f;
        tr *= 1.0 - f;
      }
    }
    o = vec4(rgb, 1.0 - tr);
  });

/* WGSL: the same block and functions (volumetric_fog_wgpu.c builds the
 * compute modules on it) */
const char CW__VF_WGSL_COMMON[] =
    S(struct FogLight { pos: vec4f, dir: vec4f, color: vec4f, cone: vec4f, tile: vec4f, shadow: mat4x4f };
      struct FogVol { inv: mat4x4f, a: vec4f, albedo: vec4f, emission: vec4f, wind: vec4f };
      struct FogU {
        inv_view: mat4x4f, prev_vp: mat4x4f, grid: vec4f, proj: vec4f, slices: vec4f, jitter: vec4f, medium: vec4f,
        ambient: vec4f, albedo: vec4f, counts: vec4f, lights: array<FogLight, 8>, vols: array<FogVol, 16>
      };
      @group(0) @binding(0) var<uniform> u: FogU;
      fn vf_slice_dist(s: f32) -> f32 { return u.slices.x * pow(max(s * u.grid.w, 1e-10), u.slices.y); }
      fn vf_ray(cell: vec2f) -> vec3f {
        let ndc = cell / u.grid.xy * 2.0 - 1.0;
        return vec3f((ndc.x + u.proj.z) / u.proj.x, (ndc.y + u.proj.w) / u.proj.y, -1.0);
      }
      fn vf_hash(v0: vec3u) -> u32 {
        var v = v0 * 1664525u + vec3u(1013904223u);
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        v ^= v >> vec3u(16u);
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        return v.x;
      }
      fn vf_h(p: vec3i) -> f32 { return f32(vf_hash(vec3u(p)) >> 8u) / 16777216.0; }
      fn vf_vnoise(x: vec3f) -> f32 {
        let i = floor(x);
        var f = x - i;
        f = f * f * (3.0 - 2.0 * f);
        let b = vec3i(i);
        let a0 = mix(vf_h(b), vf_h(b + vec3i(1, 0, 0)), f.x);
        let a1 = mix(vf_h(b + vec3i(0, 1, 0)), vf_h(b + vec3i(1, 1, 0)), f.x);
        let a2 = mix(vf_h(b + vec3i(0, 0, 1)), vf_h(b + vec3i(1, 0, 1)), f.x);
        let a3 = mix(vf_h(b + vec3i(0, 1, 1)), vf_h(b + vec3i(1, 1, 1)), f.x);
        return mix(mix(a0, a1, f.y), mix(a2, a3, f.y), f.z);
      }
      fn vf_noise(x: vec3f) -> f32 { return vf_vnoise(x) * 0.6667 + vf_vnoise(x * 2.03 + vec3f(17.1, 5.3, 11.7)) * 0.3333; }
      fn vf_hg(mu: f32, g: f32) -> f32 {
        let g2 = g * g;
        return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
      }
      fn vf_bound(c: vec4f) -> vec4f { return select(clamp(select(c, vec4f(0.0), c != c), vec4f(0.0), vec4f(60000.0, 60000.0, 60000.0, 64.0)), c, u.counts.z > 0.5); }
      fn vf_hist(cur: vec4f, h: vec4f) -> vec4f { return select(min(vf_bound(h), cur * 8.0 + vec4f(0.02)), h, u.counts.z > 0.5); }
      fn vf_acc(acc: vec4f, s: vec4f, ds: f32) -> vec4f {
        let e = s.a;
        let tr = exp(-e * ds);
        let w = select((1.0 - tr) / e, ds * (1.0 - 0.5 * e * ds), e * ds < 1e-3);
        return vec4f(acc.rgb + acc.a * s.rgb * w, acc.a * tr);
      });

static const char VF_WGSL_APPLY[] =
    S(struct U {
        u_inv_view_proj: mat4x4f, u_cam: vec3f, u_fwd: vec3f, u_pz: vec4f, u_grid: vec4f, u_fog: vec4f, u_dist: vec4f,
        u_color: vec3f, u_sun_dir: vec3f, u_sun_color: vec3f, u_g: f32
      };
      @group(0) @binding(0) var<uniform> u: U;
      @group(0) @binding(1) var u_depth: texture_2d<f32>;
      @group(0) @binding(2) var u_vol: texture_3d<f32>;
      fn vf_od(v: vec3f, l: f32) -> f32 {
        let a = u.u_fog.x * exp(-u.u_fog.y * (u.u_cam.y - u.u_fog.z));
        let k = u.u_fog.y * v.y * l;
        return a * l * select(1.0 - 0.5 * k, (1.0 - exp(-k)) / k, abs(k) > 1e-4);
      }
      fn vf_hg(mu: f32, g: f32) -> f32 {
        let g2 = g * g;
        return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
      }

      fn vf_smooth_volume(uvw: vec3f) -> vec4f {
        let size = vec2f(textureDimensions(u_vol).xy);
        let p = uvw.xy * size - 0.5;
        let cell = floor(p); let f = fract(p); let f2 = f*f; let f3 = f2*f;
        let w0 = (1.0-f)*(1.0-f)*(1.0-f)/6.0;
        let w1 = (3.0*f3-6.0*f2+4.0)/6.0;
        let w2 = (-3.0*f3+3.0*f2+3.0*f+1.0)/6.0;
        let w3 = f3/6.0; let g0 = w0+w1; let g1 = w2+w3;
        let h0 = (cell-1.0+w1/g0+0.5)/size;
        let h1 = (cell+1.0+w3/g1+0.5)/size;
        return mix(mix(textureSampleLevel(u_vol,cw_linear,vec3f(h0,uvw.z),0.0),textureSampleLevel(u_vol,cw_linear,vec3f(h1.x,h0.y,uvw.z),0.0),g1.x),
                   mix(textureSampleLevel(u_vol,cw_linear,vec3f(h0.x,h1.y,uvw.z),0.0),textureSampleLevel(u_vol,cw_linear,vec3f(h1,uvw.z),0.0),g1.x),g1.y);
      }
      @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
        let zl = cw_load(u_depth, uv).r;
        let a = u.u_inv_view_proj * vec4f(uv * 2.0 - 1.0, -1.0, 1.0);
        let v = normalize(a.xyz / a.w - u.u_cam);
        let vz = max(dot(v, u.u_fwd), 1e-4);
        let sky = zl > 1e30;
        let dist = select(zl, 1e30, sky);
        let c = max(u.u_grid.x * pow(clamp(dist / u.u_grid.y, 1e-10, 1.0), u.u_grid.z) - 1.0, 0.0);
        let vol = vf_smooth_volume(vec3f(uv, (c - 0.5) / u.u_grid.x));
        let k = clamp(c, 0.0, 1.0);
        var rgb = vol.rgb * k;
        var tr = mix(1.0, vol.a, k);
        if (u.u_grid.w > 0.5) {
          let l0 = u.u_grid.y / vz;
          let l1 = select(dist / vz, u.u_dist.y, sky);
          if (l1 > l0) {
            let f = min(1.0 - exp(-(vf_od(v, l1) - vf_od(v, l0))), u.u_fog.w);
            rgb += tr * (u.u_color + u.u_sun_color * vf_hg(dot(v, u.u_sun_dir), u.u_g)) * f;
            tr *= 1.0 - f;
          }
        }
        return vec4f(rgb, 1.0 - tr);
      });

/* ── GL resources and passes ────────────────────────────────────────── */

#define VF_UBO_BINDING 7
static int gl_nout;          /* inject: layers a draw (the host's draw buffers, at most 8) */
static unsigned gl_inj_prog, gl_int_prog, gl_failed;

static int gl_draw_buffers(void) {
  if (gl_nout) return gl_nout;
  GLint db = 4, ca = 4;
  glGetIntegerv(GL_MAX_DRAW_BUFFERS, &db);
  glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &ca);
  int n = CW_MIN(db, ca);
  gl_nout = n >= 8 ? 8 : 4;
  return gl_nout;
}

static unsigned gl_program(const char *name, const char *body, int nout, bool inject) {
  /* the outputs are an array indexed by constants (GLSL ES 3.00) */
  size_t cap = strlen(VF_GLSL_COMMON) + strlen(body) + 4096;
  char *src = cw_alloc(cap, CW_MEM_RENDER);
  int k = snprintf(src, cap, "%s%slayout(location = 0) out vec4 o[%d];\nvoid main() {\n  ivec2 xy = ivec2(gl_FragCoord.xy);\n",
                   VF_GLSL_COMMON, body, nout);
  if (inject) {
    for (int i = 0; i < nout; i++) k += snprintf(src + k, cap - (size_t)k, "  o[%d] = vf_inject(ivec3(xy, u_z0 + %d));\n", i, i);
  } else {
    k += snprintf(src + k, cap - (size_t)k,
                  "  vec4 acc = u_first != 0 ? vec4(0.0, 0.0, 0.0, 1.0) : texelFetch(u_carry, xy, 0);\n"
                  "  float rl = length(vf_ray(vec2(xy) + 0.5));\n");
    for (int i = 0; i < nout - 1; i++)
      k += snprintf(src + k, cap - (size_t)k, "  acc = vf_bound(vf_step(xy, u_z0 + %d, rl, acc));\n  o[%d] = acc;\n", i, i);
    k += snprintf(src + k, cap - (size_t)k, "  o[%d] = acc;\n", nout - 1);
  }
  snprintf(src + k, cap - (size_t)k, "}\n");
  unsigned p = cw_pass_program(name, src);
  cw_free(src);
  if (!p) return 0;
  GLuint bi = glGetUniformBlockIndex(p, "FogU");
  if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, VF_UBO_BINDING);
  glUseProgram(p);
  if (inject) {
    glUniform1i(glGetUniformLocation(p, "u_hist"), 0);
    glUniform1i(glGetUniformLocation(p, "u_shadow"), 1);
  } else {
    glUniform1i(glGetUniformLocation(p, "u_scatter"), 0);
    glUniform1i(glGetUniformLocation(p, "u_carry"), 1);
  }
  return p;
}

static unsigned tex3d(int w, int h, int d) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_3D, t);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, w, h, d, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_3D, 0);
  return t;
}

static unsigned tex2d(int w, int h) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return t;
}

static void gl_free(vf_gl *g) {
  if (g->n_inj) glDeleteFramebuffers(g->n_inj, g->inj_fbo[0]), glDeleteFramebuffers(g->n_inj, g->inj_fbo[1]);
  if (g->n_int) glDeleteFramebuffers(g->n_int, g->int_fbo);
  GLuint t[6] = { g->scatter[0], g->scatter[1], g->integ, g->carry[0], g->carry[1], g->shadow_tex };
  for (int i = 0; i < 6; i++)
    if (t[i]) glDeleteTextures(1, &t[i]);
  if (g->shadow_fbo) glDeleteFramebuffers(1, &g->shadow_fbo);
  if (g->ubo) glDeleteBuffers(1, &g->ubo);
  memset(g, 0, sizeof *g);
}

static bool fbo_ok(const char *what) {
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st == GL_FRAMEBUFFER_COMPLETE) return true;
  cw_error("volumetric fog: the %s framebuffer is incomplete (0x%x); volumetric fog is off", what, (unsigned)st);
  return false;
}

/* (re)make the grid's GL objects; true when they are new (no history) */
static bool gl_resources(vf_gl *g, int w, int h, int d, int atlas, bool *ok) {
  *ok = true;
  if (g->w == w && g->h == h && g->d == d && g->atlas == atlas && g->ubo) return false;
  int nout = gl_draw_buffers(), nl = nout - 1;
  if (!g->ubo || g->w != w || g->h != h || g->d != d) {
    unsigned ubo = g->ubo, st = g->shadow_tex, sf = g->shadow_fbo;
    int a = g->atlas;
    g->ubo = g->shadow_tex = g->shadow_fbo = 0;
    gl_free(g);
    g->ubo = ubo, g->shadow_tex = st, g->shadow_fbo = sf, g->atlas = a;
    if (!g->ubo) {
      glGenBuffers(1, &g->ubo);
      glBindBuffer(GL_UNIFORM_BUFFER, g->ubo);
      glBufferData(GL_UNIFORM_BUFFER, sizeof(vf_uniforms), NULL, GL_DYNAMIC_DRAW);
      glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
    g->w = w, g->h = h, g->d = d;
    g->scatter[0] = tex3d(w, h, d);
    g->scatter[1] = tex3d(w, h, d);
    g->integ = tex3d(w, h, d);
    g->carry[0] = tex2d(w, h);
    g->carry[1] = tex2d(w, h);
    GLenum bufs[8];
    g->n_inj = d / nout;
    for (int c = 0; c < 2; c++) {
      glGenFramebuffers(g->n_inj, g->inj_fbo[c]);
      for (int b = 0; b < g->n_inj; b++) {
        glBindFramebuffer(GL_FRAMEBUFFER, g->inj_fbo[c][b]);
        for (int i = 0; i < nout; i++) {
          glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, g->scatter[c], 0, b * nout + i);
          bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
        }
        glDrawBuffers(nout, bufs);
        if (!fbo_ok("inject")) *ok = false;
      }
    }
    g->n_int = (d + nl - 1) / nl;
    glGenFramebuffers(g->n_int, g->int_fbo);
    for (int b = 0; b < g->n_int; b++) {
      glBindFramebuffer(GL_FRAMEBUFFER, g->int_fbo[b]);
      for (int i = 0; i < nl; i++) {
        int z = b * nl + i;
        if (z < d) {
          glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, g->integ, 0, z);
          bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
        } else bufs[i] = GL_NONE;
      }
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)nl, GL_TEXTURE_2D, g->carry[b & 1], 0);
      bufs[nl] = GL_COLOR_ATTACHMENT0 + (GLenum)nl;
      glDrawBuffers(nout, bufs);
      if (!fbo_ok("integrate")) *ok = false;
    }
  }
  if (g->atlas != atlas || !g->shadow_tex) {
    if (g->shadow_tex) glDeleteTextures(1, &g->shadow_tex);
    if (g->shadow_fbo) glDeleteFramebuffers(1, &g->shadow_fbo);
    g->atlas = atlas;
    glGenTextures(1, &g->shadow_tex);
    glBindTexture(GL_TEXTURE_2D, g->shadow_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, atlas, atlas, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &g->shadow_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g->shadow_tex, 0);
    GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    if (!fbo_ok("shadow")) *ok = false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

static void gl_run(vf_slot *S) {
  vf_gl *g = &S->gl;
  int nout = gl_draw_buffers(), nl = nout - 1;
  /* uniform locations once: a glGet* waits for the driver thread (Mesa's glthread) to drain */
  static GLint zl = -1, il = -1, fl = -1;
  if (!gl_inj_prog && !gl_failed) {
    gl_inj_prog = gl_program("volumetric_fog.inject", VF_GLSL_INJECT, nout, true);
    gl_int_prog = gl_program("volumetric_fog.integrate", VF_GLSL_INTEGRATE, nout, false);
    if (!gl_inj_prog || !gl_int_prog) gl_failed = 1;
    else {
      zl = glGetUniformLocation(gl_inj_prog, "u_z0");
      il = glGetUniformLocation(gl_int_prog, "u_z0");
      fl = glGetUniformLocation(gl_int_prog, "u_first");
    }
  }
  if (gl_failed) return;
  cw__pass_state_reset();
  glBindBuffer(GL_UNIFORM_BUFFER, g->ubo);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof S->u, &S->u);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
  glBindBufferBase(GL_UNIFORM_BUFFER, VF_UBO_BINDING, g->ubo);
  glViewport(0, 0, g->w, g->h);
  /* inject into scatter[cur], the history from the other */
  glUseProgram(gl_inj_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_3D, g->scatter[S->cur ^ 1]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, g->shadow_tex);
  for (int b = 0; b < g->n_inj; b++) {
    glBindFramebuffer(GL_FRAMEBUFFER, g->inj_fbo[S->cur][b]);
    glUniform1i(zl, b * nout);
    cw_pass_fullscreen();
  }
  /* integrate front to back, nl layers a draw, the running sum carried */
  glUseProgram(gl_int_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_3D, g->scatter[S->cur]);
  for (int b = 0; b < g->n_int; b++) {
    glBindFramebuffer(GL_FRAMEBUFFER, g->int_fbo[b]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g->carry[(b + 1) & 1]);
    glUniform1i(il, b * nl);
    glUniform1i(fl, b == 0);
    cw_pass_fullscreen();
  }
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_3D, 0);
  glBindBufferBase(GL_UNIFORM_BUFFER, VF_UBO_BINDING, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ── building the grid ──────────────────────────────────────────────── */

static float halton(int i, int b) {
  float f = 1, r = 0;
  while (i > 0) {
    f /= (float)b;
    r += f * (float)(i % b);
    i /= b;
  }
  return r;
}

static t3_vec3 v3norm(t3_vec3 a) {
  float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
  return l > 1e-12f ? (t3_vec3){ a.x / l, a.y / l, a.z / l } : (t3_vec3){ 0, 0, 1 };
}
static float v3dot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static t3_vec3 v3cross(t3_vec3 a, t3_vec3 b) {
  return (t3_vec3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

/* FogVolume3D nodes this frame */
static bool content(const vfog_rt *v) { return v->nv > 0 && v->stamp_v == cw_frame_index(); }

static const cw__height_fog *medium_of(cw_tree *t) {
  const cw__height_fog *f = cw__height_fog_of(t);
  return f && f->volumetric && f->density > 0 ? f : NULL;
}

static bool vf_build_active(const cw_pass_ctx *ctx, void *user) {
  vfog_rt *v = user;
  slot_of(v, ctx)->built = false;
  if (!cw__vfog_supported() || !ctx->camera || !ctx->renderer) return false;
  bool any = medium_of(ctx->tree) != NULL || content(v);
  if (any && ctx->camera->base.type != T3_PERSPECTIVE_CAMERA) {
    if (!v->warned_ortho) cw_warn("volumetric fog needs a perspective camera; it is off for this one");
    v->warned_ortho = true;
    return false;
  }
  return any;
}

/* a depth tile of the atlas from cam (three.c's caster programs) */
static void render_tile(cw_pass_ctx *ctx, vf_slot *S, t3_camera *cam, int tile, int ts, bool first) {
  int x = (tile & 1) * ts, y = (tile >> 1) * ts;
  t3_renderer *r = ctx->renderer;
  t3_object_update_world_matrix(cam, false, false);
  if (wgpu()) {
    int fmt = 0;
    void *view = cw__vf_wg_atlas_view(S->wg, &fmt);
    t3_renderer_set_external_encoder(r, (uintptr_t)cw__vf_wg_encoder());
    /* a WebGPU pass clears the whole attachment: only the frame's first tile clears */
    t3_external_target et = { .depth = (uintptr_t)view, .depth_format = fmt, .load_depth = !first, .clear_depth = 1,
                              .x = x, .y = y, .w = ts, .h = ts, .flip_y = true };
    t3_renderer_render_depth_to(r, ctx->scene3d, cam, &et);
    return;
  }
  t3_renderer_reset_bindings(r);
  t3_external_target et = { .color = S->gl.shadow_fbo, .load_depth = false, .clear_depth = 1, .x = x, .y = y, .w = ts, .h = ts };
  t3_renderer_render_depth_to(r, ctx->scene3d, cam, &et);
  t3_renderer_reset_bindings(r);
}

static void shadow_matrix(t3_camera *cam, float out[16]) {
  t3_object_update_world_matrix(cam, false, false);
  t3_mat4 view, m;
  t3_mat4_invert(&view, &cam->base.matrix_world);
  t3_mat4_multiply(&m, &cam->projection_matrix, &view);
  memcpy(out, m.e, sizeof m.e);
}

static void vf_build_exec(cw_pass_ctx *ctx, void *user) {
  vfog_rt *v = user;
  vf_slot *S = slot_of(v, ctx);
  const cw__height_fog *hf = cw__height_fog_of(ctx->tree), *med = medium_of(ctx->tree);
  int q = CW_CLAMP(cw_gfx_get(CW_GFX_EFFECTS), 0, CW_QUALITY_COUNT - 1);
  const vf_preset *pr = &PRESETS[q];
  float share = view_share(ctx);
  int gw = CW_MAX(32, (int)lroundf((float)pr->gw * share));
  int gh = CW_MAX(1, (int)lroundf((float)gw * (float)ctx->height / (float)CW_MAX(ctx->width, 1)));
  int gd = pr->d, ts = CW_MAX(256, (int)lroundf((float)pr->tile * share)), atlas = ts * 2;
  float dist = hf ? CW_MAX(hf->volumetric_distance, 1.0f) : 64.0f;
  float spread = hf ? CW_CLAMP(hf->volumetric_detail_spread, 1.0f, 4.0f) : 2.0f;
  float temporal = hf ? CW_CLAMP(hf->volumetric_temporal, 0.0f, 0.98f) : 0.9f;
  bool lights_now = v->stamp_l == cw_frame_index(), vols_now = v->stamp_v == cw_frame_index();
  int nl = lights_now ? v->nl : 0, nv = vols_now ? v->nv : 0;

  /* GPU resources */
  bool fresh;
  if (wgpu()) fresh = cw__vf_wg_resources(&S->wg, gw, gh, gd, atlas);
  else {
    bool ok;
    fresh = gl_resources(&S->gl, gw, gh, gd, atlas, &ok);
    if (!ok) {
      gl_free(&S->gl);
      return;
    }
  }
  if (fresh || S->built_frame + 1 != ctx->frame) S->have_history = false;
  S->cur = S->have_history ? S->cur ^ 1 : 0;

  vf_uniforms *u = &S->u;
  memset(u, 0, sizeof *u);
  t3_mat4 inv_view;
  t3_mat4_invert(&inv_view, &ctx->view);
  memcpy(u->inv_view, inv_view.e, sizeof u->inv_view);
  memcpy(u->prev_vp, ctx->prev_view_proj.e, sizeof u->prev_vp);
  u->grid[0] = (float)gw, u->grid[1] = (float)gh, u->grid[2] = (float)gd, u->grid[3] = 1.0f / (float)gd;
  const float *P = ctx->proj.e;
  u->proj[0] = P[0], u->proj[1] = P[5], u->proj[2] = P[8], u->proj[3] = P[9];
  float hist = S->have_history ? temporal : 0;
  u->slices[0] = dist, u->slices[1] = spread, u->slices[2] = 1.0f / spread, u->slices[3] = hist;
  if (temporal > 0) {
    int k = (int)(ctx->frame % 16) + 1;
    u->jitter[0] = halton(k, 2) - 0.5f, u->jitter[1] = halton(k, 3) - 0.5f, u->jitter[2] = halton(k, 5) - 0.5f;
  }
  u->jitter[3] = (float)ctx->time;
  t3_color amb = { 0, 0, 0 }, horizon, sky_sun_color;
  t3_vec3 sky_sun;
  bool sky = cw__sky_env(ctx->tree, NULL, &sky_sun, &sky_sun_color, &horizon);
  if (hf) {
    amb = (hf->use_sky_color && sky) ? horizon : hf->color;
    float a = CW_MAX(hf->volumetric_ambient, 0.0f);
    amb.r *= a, amb.g *= a, amb.b *= a;
  }
  t3_color alb = hf ? hf->volumetric_albedo : (t3_color){ 1, 1, 1 };
  if (med) u->medium[0] = med->density, u->medium[1] = CW_MAX(med->height_falloff, 0.0f), u->medium[2] = med->base_height;
  u->medium[3] = hf ? CW_CLAMP(hf->sun_g, -0.95f, 0.95f) : 0.2f;
  u->ambient[0] = amb.r, u->ambient[1] = amb.g, u->ambient[2] = amb.b;
  u->albedo[0] = alb.r, u->albedo[1] = alb.g, u->albedo[2] = alb.b;

  /* the camera, for the shadow fits */
  t3_vec3 cam = { inv_view.e[12], inv_view.e[13], inv_view.e[14] };
  t3_vec3 fwd = v3norm((t3_vec3){ -inv_view.e[8], -inv_view.e[9], -inv_view.e[10] });

  /* lights */
  int shadows = 0, nsh = 0, spot_tiles = 0;
  bool sun_tile = false;
  memset(S->apply_sun, 0, sizeof S->apply_sun);
  bool apply_sun_set = false;
  int n = 0;
  for (int i = 0; i < nl && n < VF_MAX_LIGHTS; i++) {
    const vf_light_in *in = &v->lights[i];
    const t3_light *l = in->l;
    const float *m = l->base.matrix_world.e;
    vf_light_u *o = &u->lights[n];
    float e = l->intensity * in->energy;
    o->color[0] = l->color.r * e, o->color[1] = l->color.g * e, o->color[2] = l->color.b * e, o->color[3] = l->decay;
    if (in->kind == 0) {
      t3_vec3 d = v3norm((t3_vec3){ m[8], m[9], m[10] });
      o->pos[0] = d.x, o->pos[1] = d.y, o->pos[2] = d.z, o->pos[3] = 0;
      if (!apply_sun_set) {
        apply_sun_set = true;
        S->apply_sun[0] = d.x, S->apply_sun[1] = d.y, S->apply_sun[2] = d.z;
        S->apply_sun[3] = o->color[0] * alb.r, S->apply_sun[4] = o->color[1] * alb.g, S->apply_sun[5] = o->color[2] * alb.b;
      }
      if (in->shadow && !sun_tile) {
        /* an orthographic map round the sphere that holds the grid, its
         * centre snapped to whole texels (no shimmer as the camera moves) */
        sun_tile = true;
        float tx = 1.0f / P[0], ty = 1.0f / P[5], k2 = tx * tx + ty * ty;
        float cd = dist * (1 + k2) * 0.5f, rad = cd;
        if (cd > dist) cd = dist, rad = sqrtf(k2) * dist;
        rad = ceilf(rad);
        t3_vec3 c = { cam.x + fwd.x * cd, cam.y + fwd.y * cd, cam.z + fwd.z * cd };
        t3_vec3 up = fabsf(d.y) > 0.999f ? (t3_vec3){ 0, 0, 1 } : (t3_vec3){ 0, 1, 0 };
        t3_vec3 xa = v3norm(v3cross(up, d)), ya = v3cross(d, xa);
        float texel = 2 * rad / (float)ts, cx = floorf(v3dot(c, xa) / texel) * texel, cy = floorf(v3dot(c, ya) / texel) * texel;
        float cz = v3dot(c, d);
        c = (t3_vec3){ xa.x * cx + ya.x * cy + d.x * cz, xa.y * cx + ya.y * cy + d.y * cz, xa.z * cx + ya.z * cy + d.z * cz };
        float back = CW_MAX(rad, 100.0f);
        if (!v->cam_dir) v->cam_dir = t3_orthographic_camera_new(-1, 1, 1, -1, 0.1f, 10);
        t3_camera *sc = v->cam_dir;
        sc->base.up = up;
        sc->left = sc->bottom = -rad, sc->right = sc->top = rad, sc->near = 0.05f, sc->far = 2 * rad + back;
        t3_camera_update_projection_matrix(sc);
        sc->base.position = (t3_vec3){ c.x + d.x * (rad + back), c.y + d.y * (rad + back), c.z + d.z * (rad + back) };
        t3_object_look_at(sc, c.x, c.y, c.z);
        render_tile(ctx, S, sc, 0, ts, shadows == 0);
        shadow_matrix(sc, o->shadow);
        o->cone[2] = 1, o->cone[3] = 0.0005f;
        o->tile[0] = 0, o->tile[1] = 0, o->tile[2] = 0.5f, o->tile[3] = 0.5f;
        shadows++;
      }
    } else {
      o->pos[0] = m[12], o->pos[1] = m[13], o->pos[2] = m[14], o->pos[3] = (float)in->kind;
      t3_vec3 ax = v3norm((t3_vec3){ -m[8], -m[9], -m[10] });
      o->dir[0] = ax.x, o->dir[1] = ax.y, o->dir[2] = ax.z, o->dir[3] = l->distance;
      if (in->kind == 2) {
        float co = cosf(l->angle), ci = cosf(l->angle * (1 - l->penumbra));
        o->cone[0] = co, o->cone[1] = CW_MAX(ci, co + 1e-4f);
        if (in->shadow && pr->spot_shadows && spot_tiles < 3) {
          int t = ++spot_tiles;
          if (!v->cam_spot[t - 1]) v->cam_spot[t - 1] = t3_perspective_camera_new(50, 1, 0.1f, 50);
          t3_camera *sc = v->cam_spot[t - 1];
          float far = l->distance > 0 ? l->distance : dist * 2;
          sc->fov = CW_MIN(l->angle * 2 * 1.05f * 180.0f / CW_PI, 170.0f), sc->aspect = 1, sc->near = 0.1f, sc->far = far;
          t3_camera_update_projection_matrix(sc);
          sc->base.position = (t3_vec3){ m[12], m[13], m[14] };
          sc->base.up = fabsf(ax.y) > 0.999f ? (t3_vec3){ 0, 0, 1 } : (t3_vec3){ 0, 1, 0 };
          t3_object_look_at(sc, m[12] + ax.x, m[13] + ax.y, m[14] + ax.z);
          render_tile(ctx, S, sc, t, ts, shadows == 0);
          shadow_matrix(sc, o->shadow);
          o->cone[2] = 1, o->cone[3] = 0.0002f;
          o->tile[0] = (float)(t & 1) * 0.5f, o->tile[1] = (float)(t >> 1) * 0.5f, o->tile[2] = 0.5f, o->tile[3] = 0.5f;
          shadows++;
        }
      }
    }
    n++;
  }
  nsh = shadows;
  u->counts[0] = (float)n;

  /* volumes */
  int nvol = 0;
  for (int i = 0; i < nv; i++) {
    const fog_volume *p = &v->vols[i].p;
    if (p->density <= 0) continue;
    vf_vol_u *o = &u->vols[nvol++];
    t3_mat4 w = v->vols[i].o->matrix_world;
    float hs[3] = { CW_MAX(p->size.x, 1e-4f) * 0.5f, CW_MAX(p->size.y, 1e-4f) * 0.5f, CW_MAX(p->size.z, 1e-4f) * 0.5f };
    for (int c = 0; c < 3; c++)
      for (int r = 0; r < 3; r++) w.e[c * 4 + r] *= hs[c];
    t3_mat4 inv;
    t3_mat4_invert(&inv, &w);
    memcpy(o->inv, inv.e, sizeof o->inv);
    o->a[0] = p->density, o->a[1] = (float)p->shape, o->a[2] = CW_CLAMP(p->edge_fade, 0.0f, 1.0f), o->a[3] = CW_MAX(p->height_falloff, 0.0f);
    o->albedo[0] = p->albedo.r, o->albedo[1] = p->albedo.g, o->albedo[2] = p->albedo.b, o->albedo[3] = CW_CLAMP(p->noise, 0.0f, 1.0f);
    float ee = p->emission_energy;
    o->emission[0] = p->emission.r * ee, o->emission[1] = p->emission.g * ee, o->emission[2] = p->emission.b * ee;
    o->emission[3] = CW_MAX(p->noise_scale, 0.0f);
    o->wind[0] = p->noise_wind.x, o->wind[1] = p->noise_wind.y, o->wind[2] = p->noise_wind.z;
    o->wind[3] = p->shape == 2 ? w.e[13] : w.e[13] - (fabsf(w.e[1]) + fabsf(w.e[5]) + fabsf(w.e[9]));
  }
  u->counts[1] = (float)nvol;
  { /* test hooks (tests/fog-flare.mjs): CW_TEST_VFOG_UNBOUNDED=1 the grid without its bound;
     * CW_TEST_VFOG_POISON=1 an infinity in the left quarter of the cells on frames 5 to 7 */
    static int unbounded = -1, poison = -1;
    if (unbounded < 0) unbounded = getenv("CW_TEST_VFOG_UNBOUNDED") && atoi(getenv("CW_TEST_VFOG_UNBOUNDED")) != 0;
    if (poison < 0) poison = getenv("CW_TEST_VFOG_POISON") && atoi(getenv("CW_TEST_VFOG_POISON")) != 0;
    u->counts[2] = unbounded ? 1.0f : 0.0f;
    u->counts[3] = poison && ctx->frame >= 5 && ctx->frame <= 7 ? 1.0f : 0.0f;
  }
  if (!(med || nvol)) return; /* nothing to draw after all */

  if (wgpu()) cw__vf_wg_run(S->wg, u, S->cur, NULL);
  else gl_run(S);

  S->built = true;
  S->built_frame = ctx->frame;
  S->have_history = temporal > 0;
  v->st = (cw_volumetric_fog_state){ .active = true, .grid = { gw, gh, gd }, .lights = n, .shadowed_lights = nsh,
                                     .volumes = nvol, .distance = dist, .shadow_map = nsh ? ts : 0 };
}

/* ── apply ──────────────────────────────────────────────────────────── */

static bool vf_apply_active(const cw_pass_ctx *ctx, void *user) {
  (void)ctx;
  vfog_rt *v = user;
  /* the grid pass runs first in the same phase and sets built; at
   * scheduling time it has not run yet, so the apply follows its activity */
  return cw__vfog_supported() && ctx->camera && ctx->renderer && ctx->camera->base.type == T3_PERSPECTIVE_CAMERA &&
         (medium_of(ctx->tree) != NULL || content(v));
}

static unsigned apply_program(void) {
  static unsigned prog;
  if (!prog) {
    if (wgpu()) {
      static char w[8192];
      snprintf(w, sizeof w, "%s", VF_WGSL_APPLY);
      prog = cw_pass_program_wgsl("volumetric_fog", w);
    } else prog = cw_pass_program("volumetric_fog", VF_GLSL_APPLY);
  }
  return prog;
}

static void vf_apply_exec(cw_pass_ctx *ctx, void *user) {
  vfog_rt *v = user;
  vf_slot *S = slot_of(v, ctx);
  if (!S->built || !ctx->depth_tex) {
    v->st.active = false;
    return;
  }
  unsigned pr = apply_program();
  if (!pr) return;
  const cw__height_fog *hf = cw__height_fog_of(ctx->tree), *med = medium_of(ctx->tree);
  cw_pass_use(pr);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(pr, "u_depth", 0);
  cw_pass_bind_texture_3d(1, wgpu() ? cw__vf_wg_integrated(S->wg) : S->gl.integ);
  cw_pass_uniform1i(pr, "u_vol", 1);
  cw_pass_uniform_mat4(pr, "u_inv_view_proj", &ctx->inv_view_proj);
  const float *iv = S->u.inv_view;
  cw_pass_uniform3f(pr, "u_cam", iv[12], iv[13], iv[14]);
  t3_vec3 fwd = v3norm((t3_vec3){ -iv[8], -iv[9], -iv[10] });
  cw_pass_uniform3f(pr, "u_fwd", fwd.x, fwd.y, fwd.z);
  const float *e = ctx->camera->projection_matrix.e;
  cw_pass_uniform4f(pr, "u_pz", e[10], e[14], e[11], e[15]);
  cw_pass_uniform4f(pr, "u_grid", S->u.grid[2], S->u.slices[0], S->u.slices[2], med ? 1.0f : 0.0f);
  if (med) {
    cw_pass_uniform4f(pr, "u_fog", med->density, CW_MAX(med->height_falloff, 0.0f), med->base_height,
                      CW_CLAMP(med->max_opacity, 0.0f, 1.0f));
    cw_pass_uniform4f(pr, "u_dist", 0, med->sky_distance, 0, 0);
  } else {
    cw_pass_uniform4f(pr, "u_fog", 0, 0, 0, 0);
    cw_pass_uniform4f(pr, "u_dist", 0, 0, 0, 0);
  }
  /* the tail's in-scattered light: the grid's ambient and its first
   * directional light, unshadowed, so the two meet without a seam */
  t3_color alb = hf ? hf->volumetric_albedo : (t3_color){ 1, 1, 1 };
  cw_pass_uniform3f(pr, "u_color", S->u.ambient[0] * alb.r, S->u.ambient[1] * alb.g, S->u.ambient[2] * alb.b);
  cw_pass_uniform3f(pr, "u_sun_dir", S->apply_sun[0], S->apply_sun[1], S->apply_sun[2]);
  cw_pass_uniform3f(pr, "u_sun_color", S->apply_sun[3], S->apply_sun[4], S->apply_sun[5]);
  cw_pass_uniform1f(pr, "u_g", S->u.medium[3]);
  cw_pass_set_blend(CW_PASS_BLEND_PREMUL);
  cw_pass_fullscreen();
  cw_pass_set_blend(CW_PASS_BLEND_NONE);
}

/* ── the FogVolume3D node ───────────────────────────────────────────── */

static void vol_init(cw_node n, void *data) {
  (void)n;
  fog_volume *f = data;
  f->shape = 0;
  f->size = (t3_vec3){ 2, 2, 2 };
  f->density = 0.2f;
  f->albedo = (t3_color){ 1, 1, 1 };
  f->emission_energy = 1;
  f->edge_fade = 0.1f;
  f->noise_scale = 0.5f;
}

static void vol_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  cw_tree *t = cw_node_tree(n);
  t3_object *o = cw_node3d_object(n);
  if (!t || !o || !o->visible) return;
  push_volume(t, o, data);
}

static const char *const vol_shapes[] = { "BOX", "ELLIPSOID", "WORLD", NULL };

#define VF(field, lo, hi, docs) CW_PROP_FIELD_RANGE(fog_volume, field, CW_T_FLOAT, lo, hi, docs)
static const cw_prop vol_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(fog_volume, shape), .size = (int)sizeof(int),
    .enum_names = vol_shapes, .doc = "BOX or ELLIPSOID of `size` (the node's transform moves, turns and scales it), or WORLD: everywhere." },
  CW_PROP_FIELD(fog_volume, size, CW_T_VEC3, CW_PROP_DEFAULT, "Full size of the box or ellipsoid (world units before the node's scale)."),
  VF(density, 0, 100, "Extinction per world unit inside (how thick the fog is)."),
  CW_PROP_FIELD(fog_volume, albedo, CW_T_COLOR, CW_PROP_DEFAULT, "Share of the light the fog scatters (its colour under white light)."),
  CW_PROP_FIELD(fog_volume, emission, CW_T_COLOR, CW_PROP_DEFAULT, "Light the fog gives off itself (glowing mist; lights nothing else)."),
  VF(emission_energy, 0, 1000, "Scale of emission."),
  VF(height_falloff, 0, 100, "How fast the density falls with height above the volume's lowest point (0 = even)."),
  VF(edge_fade, 0, 1, "Share of the volume, from its surface inward, over which the density fades in (soft edges)."),
  VF(noise, 0, 1, "How much value noise breaks the density into wisps (0 = even)."),
  VF(noise_scale, 0, 100, "Noise features per world unit."),
  CW_PROP_FIELD(fog_volume, noise_wind, CW_T_VEC3, CW_PROP_DEFAULT, "The noise drifts this many world units a second."),
  CW_PROP_END,
};

static cw_node_type vol_type = {
  .name = "FogVolume3D", .parent = "Node3D", .data_size = sizeof(fog_volume), .props = vol_props,
  .doc = "A region of volumetric fog (box, ellipsoid or everywhere), lit by the scene's lights with their shadows.",
  .init = vol_init, .pre_render = vol_pre_render,
};

void cw__register_fog_volume_types(void) { cw_node_type_register(&vol_type); }

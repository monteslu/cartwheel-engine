/* ProceduralSky (cartwheel/environment.h): single-scattering atmosphere,
 * the sun it drives, the sky cube (background + environment), the sun disk. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/environment.h"
#include "cartwheel/render3d.h"
#include "environment_internal.h"
#include "bake_internal.h"
#if CW_WGPU
#include "fx/fx_wgpu.h"
#endif

/* ── the atmosphere (C and GLSL agree) ──────────────────────────────── */

#define RE 6360e3f
#define RA 6420e3f
#define HR 7994.0f
#define HM 1200.0f
static const float BETA_R[3] = { 5.802e-6f, 13.558e-6f, 33.1e-6f };
#define BETA_M 3.996e-6f
#define VIEW_H 2.0f
#define PRIMARY_STEPS 16
#define LIGHT_STEPS 8

/* where a ray from o (height above the centre) along d leaves the sphere of
 * radius r (-1 when it misses) */
static float ray_exit(t3_vec3 o, t3_vec3 d, float r) {
  float b = t3_vec3_dot(o, d), c = t3_vec3_dot(o, o) - r * r;
  float disc = b * b - c;
  if (disc < 0) return -1;
  return -b + sqrtf(disc);
}
static float ray_ground(t3_vec3 o, t3_vec3 d) {
  float b = t3_vec3_dot(o, d), c = t3_vec3_dot(o, o) - RE * RE;
  float disc = b * b - c;
  if (disc < 0 || b > 0) return -1;
  float t = -b - sqrtf(disc);
  return t > 0 ? t : -1;
}

t3_color cw_sky_transmittance(t3_vec3 sun, float rayleigh, float mie) {
  t3_vec3 o = { 0, RE + VIEW_H, 0 };
  if (ray_ground(o, sun) > 0) return (t3_color){ 0, 0, 0 };
  float t = ray_exit(o, sun, RA), ds = t / LIGHT_STEPS, odr = 0, odm = 0;
  for (int i = 0; i < LIGHT_STEPS; i++) {
    t3_vec3 p = t3_vec3_add(o, t3_vec3_scale(sun, ds * ((float)i + 0.5f)));
    float h = sqrtf(t3_vec3_dot(p, p)) - RE;
    odr += cw_exp(-h / HR) * ds;
    odm += cw_exp(-h / HM) * ds;
  }
  t3_color c;
  float *cc = &c.r;
  for (int k = 0; k < 3; k++) cc[k] = cw_exp(-(BETA_R[k] * rayleigh * odr + BETA_M * 1.11f * mie * odm));
  return c;
}

t3_color cw_sky_radiance(t3_vec3 dir, t3_vec3 sun, float rayleigh, float mie, float g) {
  t3_vec3 o = { 0, RE + VIEW_H, 0 };
  float tmax = ray_exit(o, dir, RA), tg = ray_ground(o, dir);
  if (tg > 0) tmax = tg;
  float ds = tmax / PRIMARY_STEPS, odr = 0, odm = 0;
  float sr[3] = { 0, 0, 0 }, sm[3] = { 0, 0, 0 };
  for (int i = 0; i < PRIMARY_STEPS; i++) {
    t3_vec3 p = t3_vec3_add(o, t3_vec3_scale(dir, ds * ((float)i + 0.5f)));
    float h = sqrtf(t3_vec3_dot(p, p)) - RE;
    float hr = cw_exp(-h / HR) * ds, hm = cw_exp(-h / HM) * ds;
    odr += hr;
    odm += hm;
    if (ray_ground(p, sun) > 0) continue; /* the planet's shadow */
    float tl = ray_exit(p, sun, RA), dl = tl / LIGHT_STEPS, odrl = 0, odml = 0;
    for (int j = 0; j < LIGHT_STEPS; j++) {
      t3_vec3 q = t3_vec3_add(p, t3_vec3_scale(sun, dl * ((float)j + 0.5f)));
      float hq = sqrtf(t3_vec3_dot(q, q)) - RE;
      odrl += cw_exp(-hq / HR) * dl;
      odml += cw_exp(-hq / HM) * dl;
    }
    for (int k = 0; k < 3; k++) {
      float tau = BETA_R[k] * rayleigh * (odr + odrl) + BETA_M * 1.11f * mie * (odm + odml);
      float a = cw_exp(-tau);
      sr[k] += a * hr;
      sm[k] += a * hm;
    }
  }
  float mu = t3_vec3_dot(dir, sun);
  float pr = 3.0f / (16.0f * CW_PI) * (1 + mu * mu);
  float g2 = g * g, den = 1 + g2 - 2 * g * mu;
  float pm = 3.0f / (8.0f * CW_PI) * ((1 - g2) * (1 + mu * mu)) / ((2 + g2) * den * sqrtf(den));
  t3_color c;
  float *cc = &c.r;
  for (int k = 0; k < 3; k++) cc[k] = sr[k] * BETA_R[k] * rayleigh * pr + sm[k] * BETA_M * mie * pm;
  return c;
}

static const char SKY_FS[] =
    "uniform int u_face;\n"
    "uniform vec3 u_sun;\n"
    "uniform vec4 u_params; /* rayleigh, mie, g, irradiance */\n"
    "uniform vec3 u_ground; /* albedo x sunlit irradiance / PI */\n"
    "uniform vec3 u_night;\n"
    "out vec4 o;\n"
    "const float RE = 6360e3, RA = 6420e3, HR = 7994.0, HM = 1200.0;\n"
    "const vec3 BR = vec3( 5.802e-6, 13.558e-6, 33.1e-6 );\n"
    "const float BM = 3.996e-6;\n"
    "float rayExit( vec3 o, vec3 d, float r ) {\n"
    "\tfloat b = dot( o, d ), c = dot( o, o ) - r * r, disc = b * b - c;\n"
    "\treturn disc < 0.0 ? -1.0 : -b + sqrt( disc );\n"
    "}\n"
    "float rayGround( vec3 o, vec3 d ) {\n"
    "\tfloat b = dot( o, d ), c = dot( o, o ) - RE * RE, disc = b * b - c;\n"
    "\tif ( disc < 0.0 || b > 0.0 ) return -1.0;\n"
    "\tfloat t = -b - sqrt( disc );\n"
    "\treturn t > 0.0 ? t : -1.0;\n"
    "}\n"
    "vec3 sky( vec3 dir ) {\n"
    "\tvec3 org = vec3( 0.0, RE + 2.0, 0.0 );\n"
    "\tfloat tmax = rayExit( org, dir, RA ), tg = rayGround( org, dir );\n"
    "\tif ( tg > 0.0 ) tmax = tg;\n"
    "\tfloat ds = tmax / 16.0, odr = 0.0, odm = 0.0;\n"
    "\tvec3 sr = vec3( 0.0 ), sm = vec3( 0.0 );\n"
    "\tfor ( int i = 0; i < 16; i ++ ) {\n"
    "\t\tvec3 p = org + dir * ( ds * ( float( i ) + 0.5 ) );\n"
    "\t\tfloat h = length( p ) - RE;\n"
    "\t\tfloat hr = exp( -h / HR ) * ds, hm = exp( -h / HM ) * ds;\n"
    "\t\todr += hr; odm += hm;\n"
    "\t\tif ( rayGround( p, u_sun ) > 0.0 ) continue;\n"
    "\t\tfloat tl = rayExit( p, u_sun, RA ), dl = tl / 8.0, odrl = 0.0, odml = 0.0;\n"
    "\t\tfor ( int j = 0; j < 8; j ++ ) {\n"
    "\t\t\tvec3 q = p + u_sun * ( dl * ( float( j ) + 0.5 ) );\n"
    "\t\t\tfloat hq = length( q ) - RE;\n"
    "\t\t\todrl += exp( -hq / HR ) * dl; odml += exp( -hq / HM ) * dl;\n"
    "\t\t}\n"
    "\t\tvec3 a = exp( -( BR * u_params.x * ( odr + odrl ) + BM * 1.11 * u_params.y * ( odm + odml ) ) );\n"
    "\t\tsr += a * hr; sm += a * hm;\n"
    "\t}\n"
    "\tfloat mu = dot( dir, u_sun ), g = u_params.z, g2 = g * g, den = 1.0 + g2 - 2.0 * g * mu;\n"
    "\tfloat pr = 3.0 / ( 16.0 * 3.14159265 ) * ( 1.0 + mu * mu );\n"
    "\tfloat pm = 3.0 / ( 8.0 * 3.14159265 ) * ( ( 1.0 - g2 ) * ( 1.0 + mu * mu ) ) / ( ( 2.0 + g2 ) * den * sqrt( den ) );\n"
    "\tvec3 l = ( sr * BR * u_params.x * pr + sm * BM * u_params.y * pm ) * u_params.w;\n"
    "\tif ( tg > 0.0 ) l += u_ground * exp( -( BR * u_params.x * odr + BM * 1.11 * u_params.y * odm ) );\n"
    "\treturn l + u_night;\n"
    "}\n"
    "void main() {\n"
    "\tfloat sc = v_uv.x * 2.0 - 1.0, tc = v_uv.y * 2.0 - 1.0;\n"
    "\tvec3 g = u_face == 0 ? vec3( 1.0, -tc, -sc ) : u_face == 1 ? vec3( -1.0, -tc, sc ) : u_face == 2 ? vec3( sc, 1.0, tc )\n"
    "\t\t: u_face == 3 ? vec3( sc, -1.0, -tc ) : u_face == 4 ? vec3( sc, -tc, 1.0 ) : vec3( -sc, -tc, -1.0 );\n"
    /* stored in CubeTexture's convention (x mirrored): three.c's background
     * and PMREMGenerator both read cube textures that way */
    "\tvec3 dir = normalize( vec3( -g.x, g.y, g.z ) );\n"
    "\to = vec4( sky( dir ), 1.0 );\n"
    "}\n";

/* the analytic sun disk, after the opaque scene, where nothing was drawn */
static const char SUN_FS[] =
    "uniform mat4 u_inv_view_proj;\n"
    "uniform vec3 u_sun;\n"
    "uniform vec4 u_disk; /* cos(radius), cos(inner), radiance scale, 0 */\n"
    "uniform vec3 u_color;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "\tvec4 a = u_inv_view_proj * vec4( v_uv * 2.0 - 1.0, -1.0, 1.0 );\n"
    "\tvec4 b = u_inv_view_proj * vec4( v_uv * 2.0 - 1.0, 1.0, 1.0 );\n"
    "\tvec3 dir = normalize( b.xyz / b.w - a.xyz / a.w );\n"
    "\tfloat c = dot( dir, u_sun );\n"
    "\tif ( c < u_disk.x ) discard;\n"
    "\tfloat edge = smoothstep( u_disk.x, u_disk.y, c );\n"
    /* at the far plane: the depth test passes only where nothing was drawn */
    "\tgl_FragDepth = 1.0;\n"
    "\to = vec4( u_color * u_disk.z * edge, 0.0 );\n"
    "}\n";

/* ── the node ───────────────────────────────────────────────────────── */

typedef struct sky_node {
  float time_of_day, day_speed, latitude, declination, north;
  const char *sun;          /* path to the DirectionalLight3D it drives */
  float sun_energy, sky_energy, rayleigh, mie, mie_g;
  float sun_size, sun_disk_intensity;
  t3_color ground_color, night_color;
  bool background, environment, sun_disk;
  float env_update_angle;
  int resolution;
  float elevation;          /* read only */
} sky_node;

static const cw_node_type *sky_t;

/* per tree */
typedef struct sky_rt {
  cw_tree *tree;
  sky_node params;          /* this frame's, from the node */
  bool present;             /* a ProceduralSky rendered this frame */
  t3_vec3 sun;              /* toward the sun */
  t3_color sun_color;       /* transmittance */
  float sun_light;          /* energy given to the driven light */
  t3_color horizon;
  /* GPU */
  GLuint cube, fbo;
  int size;
  t3_texture *cube_tex;     /* the cube as a three.c texture (background) */
  t3_pmrem_generator *pmrem;
  t3_render_target *env;
  t3_vec3 built_sun;
  uint64_t built_key;
  bool built;
  int updates;
#if CW_WGPU
  /* WebGPU: the cube (RGBA16F, cube binding), wrapped as cube_tex
   * (t3_texture_from_wgpu: cube_tex is released before it) */
  WGPUTexture wcube;
  WGPUTextureView wcube_v;
  WGPUBuffer wubo;
#endif
} sky_rt;

static void sky_rt_free(void *p) {
  sky_rt *s = p;
  if (s->env) t3_release(s->env);
  if (s->cube_tex) t3_release(s->cube_tex);
  if (s->pmrem) t3_pmrem_generator_destroy(s->pmrem);
  if (s->fbo) glDeleteFramebuffers(1, &s->fbo);
  if (s->cube) glDeleteTextures(1, &s->cube);
#if CW_WGPU
  if (s->wcube_v) wgpuTextureViewRelease(s->wcube_v);
  if (s->wcube) wgpuTextureRelease(s->wcube);
  if (s->wubo) wgpuBufferRelease(s->wubo);
#endif
  cw_free(s);
}

static void pass_sky_env(cw_pass_ctx *ctx, void *user);
static void pass_sky_sun(cw_pass_ctx *ctx, void *user);
static bool sky_sun_active(const cw_pass_ctx *ctx, void *user) {
  (void)ctx;
  sky_rt *s = user;
  return s->present && s->params.sun_disk && s->params.background && s->sun.y > -0.02f;
}
static bool sky_env_active(const cw_pass_ctx *ctx, void *user) {
  (void)ctx;
  return ((sky_rt *)user)->present;
}

static sky_rt *sky_of(cw_tree *t) {
  sky_rt *s = cw_tree_slot(t, "sky");
  if (s) return s;
  s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
  s->tree = t;
  cw_tree_set_slot(t, "sky", s, sky_rt_free);
  cw_pipeline *p = cw_pipeline_of(t);
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "sky_env", .phase = CW_PHASE_SHADOW, .order = 100,
                                           .active = sky_env_active, .execute = pass_sky_env, .user = s });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "sky_sun", .phase = CW_PHASE_SKY, .order = 0, .writes = CW_RES_COLOR,
                                           .active = sky_sun_active, .execute = pass_sky_sun, .user = s });
  return s;
}

void cw_sky_get_state(cw_tree *t, cw_sky_state *out) {
  memset(out, 0, sizeof *out);
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  if (!s || !s->present) return;
  out->active = true;
  out->time_of_day = s->params.time_of_day;
  out->sun_direction = s->sun;
  out->sun_elevation = asinf(CW_CLAMP(s->sun.y, -1.0f, 1.0f)) * 57.2957795f;
  out->sun_color = s->sun_color;
  out->sun_energy = s->sun_light;
  out->horizon = s->horizon;
  out->cube_updates = s->updates;
}

bool cw__sky_env(cw_tree *t, t3_texture **env, t3_vec3 *sun, t3_color *sun_color, t3_color *horizon) {
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  if (!s || !s->present) return false;
  if (env) *env = s->env && s->params.environment ? s->env->texture : NULL;
  if (sun) *sun = s->sun;
  if (sun_color) *sun_color = (t3_color){ s->sun_color.r * s->sun_light, s->sun_color.g * s->sun_light, s->sun_color.b * s->sun_light };
  if (horizon) *horizon = s->horizon;
  return true;
}

bool cw__sky_params(cw_tree *t, t3_vec3 *sun, float *rayleigh, float *mie, float *g, float *e0, t3_color *night) {
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  if (!s || !s->present) return false;
  *sun = s->sun;
  *rayleigh = s->params.rayleigh;
  *mie = s->params.mie;
  *g = s->params.mie_g;
  *e0 = s->params.sun_energy * s->params.sky_energy;
  *night = s->params.night_color;
  return true;
}

static void sky_init(cw_node n, void *data) {
  (void)n;
  sky_node *k = data;
  k->time_of_day = 10;
  k->latitude = 35;
  k->declination = 10;
  k->sun = cw_intern("");
  k->sun_energy = 4;
  k->sky_energy = 1;
  k->rayleigh = 1;
  k->mie = 1;
  k->mie_g = 0.76f;
  k->sun_size = 0.53f;
  k->sun_disk_intensity = 0.02f;
  k->ground_color = (t3_color){ 0.3f, 0.27f, 0.23f };
  k->night_color = (t3_color){ 0.0004f, 0.0006f, 0.0012f };
  k->background = k->environment = k->sun_disk = true;
  k->env_update_angle = 1;
  k->resolution = 128;
}

static void sky_fixed(cw_node n, void *data, double dt) {
  (void)n;
  sky_node *k = data;
  if (k->day_speed == 0) return;
  k->time_of_day += (float)(k->day_speed * dt);
  while (k->time_of_day >= 24) k->time_of_day -= 24;
  while (k->time_of_day < 0) k->time_of_day += 24;
}

/* toward the sun, world space: +X east, -Z north, +Y up */
static t3_vec3 sun_direction(const sky_node *k) {
  const float d2r = CW_PI / 180.0f;
  float h = (k->time_of_day - 12) * 15 * d2r, lat = k->latitude * d2r, dec = k->declination * d2r;
  float sh, ch, sl, cl, sd, cd;
  cw_sincos(h, &sh, &ch);
  cw_sincos(lat, &sl, &cl);
  cw_sincos(dec, &sd, &cd);
  float east = -cd * sh, north = sd * cl - cd * ch * sl, up = sd * sl + cd * ch * cl;
  t3_vec3 v = { east, up, -north };
  float so, co;
  cw_sincos(k->north * d2r, &so, &co);
  t3_vec3 r = { v.x * co + v.z * so, v.y, -v.x * so + v.z * co };
  return t3_vec3_normalize(r);
}

static void sky_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  sky_node *k = data;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  sky_rt *s = sky_of(t);
  s->params = *k;
  s->present = true;
  s->sun = sun_direction(k);
  k->elevation = asinf(CW_CLAMP(s->sun.y, -1.0f, 1.0f)) * 57.2957795f;
  s->sun_color = cw_sky_transmittance(s->sun, k->rayleigh, k->mie);
  /* the disk sinks below the horizon over its own diameter */
  float fade = cw_smoothstep(-0.6f, 0.6f, k->elevation);
  s->sun_light = k->sun_energy * fade;
  /* the horizon toward the sun (the fog's colour) */
  t3_vec3 hz = t3_vec3_normalize((t3_vec3){ s->sun.x, 0.02f, s->sun.z });
  if (t3_vec3_length_sq((t3_vec3){ s->sun.x, 0, s->sun.z }) < 1e-6f) hz = (t3_vec3){ 1, 0.02f, 0 };
  t3_color h = cw_sky_radiance(hz, s->sun, k->rayleigh, k->mie, k->mie_g);
  float e0 = k->sun_energy * k->sky_energy;
  s->horizon = (t3_color){ h.r * e0 + k->night_color.r, h.g * e0 + k->night_color.g, h.b * e0 + k->night_color.b };
  /* drive the sun light */
  if (k->sun && k->sun[0]) {
    cw_node l = cw_node_get(n, k->sun);
    if (cw_node_is(l, "DirectionalLight3D")) {
      t3_vec3 p = cw_node3d_position(l);
      cw_node3d_look_at(l, t3_vec3_sub(p, s->sun), fabsf(s->sun.y) > 0.999f ? (t3_vec3){ 0, 0, 1 } : (t3_vec3){ 0, 1, 0 });
      cw_node_set(l, "color", cw_val_color(s->sun_color.r, s->sun_color.g, s->sun_color.b, 1));
      cw_node_set(l, "energy", cw_val_float(s->sun_light));
    } else if (!CW_HANDLE_IS_NULL(l) || k->sun[0]) {
      static bool warned;
      if (!warned) {
        warned = true;
        cw_warn("ProceduralSky: `sun` (%s) is not a DirectionalLight3D", k->sun);
      }
    }
  }
}

static void sky_exit(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  if (!s) return;
  s->present = false;
  t3_scene *sc = cw_render3d_scene(t);
  if (sc->background_texture == s->cube_tex) t3_scene_set_background_texture(sc, NULL);
  if (s->env && sc->environment == s->env->texture) t3_scene_set_environment(sc, NULL);
}

static void elev_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_float(((sky_node *)data)->elevation);
}

#define SF(field, lo, hi, docs) \
  { .name = #field, .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(sky_node, field), \
    .size = (int)sizeof(float), .min = lo, .max = hi, .doc = docs }
#define SB(field, docs) \
  { .name = #field, .type = CW_T_BOOL, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(sky_node, field), \
    .size = (int)sizeof(bool), .doc = docs }

static const cw_prop sky_props[] = {
  SF(time_of_day, 0, 24, "Local solar time, hours (12 = noon)."),
  SF(day_speed, -24, 24, "Hours of the day per second of game time (the day / night cycle; 0 = still)."),
  SF(latitude, -90, 90, "Degrees north."),
  SF(declination, -23.44, 23.44, "The sun's declination in degrees (the season: +23.44 June solstice, -23.44 December)."),
  SF(north, -360, 360, "Where north is: degrees clockwise (seen from above) from -Z."),
  CW_PROP_FIELD(sky_node, sun, CW_T_STRING, CW_PROP_DEFAULT, "Path to the DirectionalLight3D the sky drives (direction, colour, energy)."),
  SF(sun_energy, 0, 1e6, "The sun's illuminance before the atmosphere (physical units; the light gets this x transmittance)."),
  SF(sky_energy, 0, 1e3, "Sky radiance multiplier (1 = physically scattered sunlight)."),
  SF(rayleigh, 0, 10, "Air density multiplier (blue sky, red sunsets)."),
  SF(mie, 0, 100, "Haze density multiplier (glow around the sun, washed-out horizon)."),
  SF(mie_g, 0, 0.99, "Haze forward scattering (Henyey-Greenstein g)."),
  SF(sun_size, 0, 10, "Apparent diameter of the sun disk, degrees."),
  SF(sun_disk_intensity, 0, 100, "Sun disk brightness as a share of the physical radiance (it is extreme: 1 = physical)."),
  CW_PROP_FIELD(sky_node, ground_color, CW_T_COLOR, CW_PROP_DEFAULT, "Ground albedo below the horizon."),
  CW_PROP_FIELD(sky_node, night_color, CW_T_COLOR, CW_PROP_DEFAULT, "Radiance added everywhere (the night sky's floor)."),
  SB(background, "Draw the sky as the scene's background."),
  SB(environment, "Prefilter the sky into the scene's environment (image-based light)."),
  SB(sun_disk, "Draw the sun disk."),
  SF(env_update_angle, 0, 90, "Re-render the sky cube and environment when the sun moved this many degrees."),
  { .name = "resolution", .type = CW_T_INT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(sky_node, resolution),
    .size = (int)sizeof(int), .min = 16, .max = 1024, .doc = "Sky cube face size in texels." },
  CW_PROP_ACCESSOR("sun_elevation", CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, elev_get, NULL, "Degrees above the horizon (read only)."),
  CW_PROP_END,
};

static cw_node_type sky_type = {
  .name = "ProceduralSky", .parent = "Node", .data_size = sizeof(sky_node), .props = sky_props,
  .doc = "A physically based sky (Rayleigh + Mie single scattering) with a day / night cycle: it drives a "
         "DirectionalLight3D, is the background and (prefiltered) the scene's environment.",
  .init = sky_init, .fixed_update = sky_fixed, .pre_render = sky_pre_render, .exit_tree = sky_exit,
};

/* ── passes ─────────────────────────────────────────────────────────── */

static uint64_t params_key(const sky_node *k) {
  uint64_t h = 1469598103934665603ull;
  const float f[] = { k->sun_energy, k->sky_energy, k->rayleigh, k->mie, k->mie_g, k->ground_color.r, k->ground_color.g,
                      k->ground_color.b, k->night_color.r, k->night_color.g, k->night_color.b, (float)k->resolution,
                      k->environment ? 1.0f : 0.0f };
  return cw_hash_bytes(f, sizeof f, h);
}

/* the cube's uniforms: sun, (rayleigh, mie, g, irradiance), ground, night */
static void cube_params(const sky_rt *s, float sun[3], float par[4], float ground[3], float night[3]) {
  const sky_node *k = &s->params;
  sun[0] = s->sun.x;
  sun[1] = s->sun.y;
  sun[2] = s->sun.z;
  par[0] = k->rayleigh;
  par[1] = k->mie;
  par[2] = k->mie_g;
  par[3] = k->sun_energy * k->sky_energy;
  /* the ground below the horizon: albedo x (sunlight through the air) / PI */
  float lit = CW_MAX(s->sun.y, 0) * k->sun_energy / CW_PI;
  ground[0] = k->ground_color.r * lit * s->sun_color.r;
  ground[1] = k->ground_color.g * lit * s->sun_color.g;
  ground[2] = k->ground_color.b * lit * s->sun_color.b;
  night[0] = k->night_color.r;
  night[1] = k->night_color.g;
  night[2] = k->night_color.b;
}

/* GL: the cube (re)made at this size and rendered, 6 faces; sc (may be
 * NULL) loses the old cube as its background when it is replaced */
static bool gl_render_cube(sky_rt *s, int size, t3_scene *sc) {
  static unsigned prog;
  if (!prog) prog = cw_pass_program("sky.cube", SKY_FS);
  if (!prog) return false;
  if (!s->cube || s->size != size) {
    if (s->cube_tex) {
      if (sc && sc->background_texture == s->cube_tex) t3_scene_set_background_texture(sc, NULL);
      t3_release(s->cube_tex);
      s->cube_tex = NULL;
    }
    if (s->cube) glDeleteTextures(1, &s->cube);
    glGenTextures(1, &s->cube);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s->cube);
    for (int f = 0; f < 6; f++)
      glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F, size, size, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    if (!s->fbo) glGenFramebuffers(1, &s->fbo);
    s->size = size;
    s->cube_tex = t3_texture_from_gl(s->cube, size, size, 1, true, false);
    s->cube_tex->type = T3_HALF_FLOAT_TYPE; /* HDR: PMREM keeps it in RGBE */
  }
  float sun[3], par[4], ground[3], night[3];
  cube_params(s, sun, par, ground, night);
  glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
  glViewport(0, 0, size, size);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(prog);
  cw_pass_uniform4f(prog, "u_params", par[0], par[1], par[2], par[3]);
  glUniform3f(glGetUniformLocation(prog, "u_sun"), sun[0], sun[1], sun[2]);
  glUniform3f(glGetUniformLocation(prog, "u_ground"), ground[0], ground[1], ground[2]);
  glUniform3f(glGetUniformLocation(prog, "u_night"), night[0], night[1], night[2]);
  for (int f = 0; f < 6; f++) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, s->cube, 0);
    cw_pass_uniform1i(prog, "u_face", f);
    cw_pass_fullscreen();
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

static bool sky_dirty(const sky_rt *s, uint64_t key) {
  const sky_node *k = &s->params;
  float moved = s->built ? acosf(CW_CLAMP(t3_vec3_dot(s->sun, s->built_sun), -1.0f, 1.0f)) * 57.2957795f : 1e9f;
  return !s->built || key != s->built_key || moved > k->env_update_angle;
}

static void built(sky_rt *s, uint64_t key) {
  s->built = true;
  s->built_key = key;
  s->built_sun = s->sun;
  s->updates++;
}

#if CW_WGPU
static bool wgpu_build_cube(sky_rt *s, int size);
#endif

static void pass_sky_env(cw_pass_ctx *ctx, void *user) {
  sky_rt *s = user;
  const sky_node *k = &s->params;
  if (!ctx->renderer) return;
  t3_scene *sc = ctx->scene3d;
  uint64_t key = params_key(k);
  int size = CW_CLAMP(k->resolution, 16, 1024);
  if (sky_dirty(s, key)) {
    bool gl = true;
#if CW_WGPU
    if (cw__wgpu_on()) {
      if (!wgpu_build_cube(s, size)) return;
      /* three.c's PMREM submits its own command buffer, like the cube: queue
       * order puts both before the frame's work */
      t3_renderer_set_external_encoder(ctx->renderer, 0);
      gl = false;
    }
#endif
    if (gl) {
      if (!gl_render_cube(s, size, sc)) return;
      t3_renderer_reset_bindings(ctx->renderer);
    }
    if (k->environment) {
      if (!s->pmrem) s->pmrem = t3_pmrem_generator_new(ctx->renderer);
      t3_render_target *env = t3_pmrem_from_cubemap(s->pmrem, s->cube_tex);
      if (gl) t3_renderer_reset_bindings(ctx->renderer);
      if (env) {
        if (s->env) t3_release(s->env);
        s->env = env;
      }
    }
    built(s, key);
  }
  /* the scene's background and environment follow the node's switches */
  t3_texture *bg = k->background ? s->cube_tex : NULL;
  if (sc->background_texture != bg) t3_scene_set_background_texture(sc, bg);
  t3_texture *env = k->environment && s->env ? s->env->texture : NULL;
  if (sc->environment != env && (env || (s->env && sc->environment == s->env->texture))) t3_scene_set_environment(sc, env);
}

#if CW_WGPU
static void pass_sky_sun_wgpu(cw_pass_ctx *ctx, sky_rt *s);
#endif

static void pass_sky_sun(cw_pass_ctx *ctx, void *user) {
  sky_rt *s = user;
#if CW_WGPU
  if (cw__wgpu_on()) {
    pass_sky_sun_wgpu(ctx, s);
    return;
  }
#endif
  static unsigned prog;
  if (!prog) prog = cw_pass_program("sky.sun", SUN_FS);
  if (!prog) return;
  const sky_node *k = &s->params;
  float r = CW_MAX(k->sun_size, 0.01f) * 0.5f * CW_PI / 180.0f;
  /* physical disk radiance: the irradiance it gives over its solid angle */
  float solid = CW_PI * r * r;
  float scale = k->sun_disk_intensity * s->sun_light / solid;
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glUseProgram(prog);
  cw_pass_uniform_mat4(prog, "u_inv_view_proj", &ctx->inv_view_proj);
  glUniform3f(glGetUniformLocation(prog, "u_sun"), s->sun.x, s->sun.y, s->sun.z);
  cw_pass_uniform4f(prog, "u_disk", cosf(r), cosf(r * 0.7f), scale, 0);
  glUniform3f(glGetUniformLocation(prog, "u_color"), s->sun_color.r, s->sun_color.g, s->sun_color.b);
  cw_pass_fullscreen();
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  (void)ctx;
}

#if CW_WGPU
/* SKY_FS in WGSL. v_uv is GL's (0 at row 0 of the face), so the faces hold
 * the same texels, row for row, as the GL cube. */
static const char WGSL_SKY[] =
    "struct U { face: vec4i, sun: vec4f, params: vec4f, ground: vec4f, night: vec4f, size: vec4f }\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "const RE = 6360e3; const RA = 6420e3; const HR = 7994.0; const HM = 1200.0;\n"
    "const BR = vec3f(5.802e-6, 13.558e-6, 33.1e-6); const BM = 3.996e-6;\n"
    "fn rayExit(o: vec3f, d: vec3f, r: f32) -> f32 {\n"
    "  let b = dot(o, d); let c = dot(o, o) - r * r; let disc = b * b - c;\n"
    "  if (disc < 0.0) { return -1.0; } return -b + sqrt(disc); }\n"
    "fn rayGround(o: vec3f, d: vec3f) -> f32 {\n"
    "  let b = dot(o, d); let c = dot(o, o) - RE * RE; let disc = b * b - c;\n"
    "  if (disc < 0.0 || b > 0.0) { return -1.0; }\n"
    "  let t = -b - sqrt(disc); return select(-1.0, t, t > 0.0); }\n"
    "fn sky(dir: vec3f) -> vec3f {\n"
    "  let sun = u.sun.xyz;\n"
    "  let org = vec3f(0.0, RE + 2.0, 0.0);\n"
    "  var tmax = rayExit(org, dir, RA); let tg = rayGround(org, dir);\n"
    "  if (tg > 0.0) { tmax = tg; }\n"
    "  let ds = tmax / 16.0; var odr = 0.0; var odm = 0.0;\n"
    "  var sr = vec3f(0.0); var sm = vec3f(0.0);\n"
    "  for (var i = 0; i < 16; i++) {\n"
    "    let p = org + dir * (ds * (f32(i) + 0.5));\n"
    "    let h = length(p) - RE;\n"
    "    let hr = exp(-h / HR) * ds; let hm = exp(-h / HM) * ds;\n"
    "    odr += hr; odm += hm;\n"
    "    if (rayGround(p, sun) > 0.0) { continue; }\n"
    "    let tl = rayExit(p, sun, RA); let dl = tl / 8.0; var odrl = 0.0; var odml = 0.0;\n"
    "    for (var j = 0; j < 8; j++) {\n"
    "      let q = p + sun * (dl * (f32(j) + 0.5));\n"
    "      let hq = length(q) - RE;\n"
    "      odrl += exp(-hq / HR) * dl; odml += exp(-hq / HM) * dl;\n"
    "    }\n"
    "    let a = exp(-(BR * u.params.x * (odr + odrl) + BM * 1.11 * u.params.y * (odm + odml)));\n"
    "    sr += a * hr; sm += a * hm;\n"
    "  }\n"
    "  let mu = dot(dir, sun); let g = u.params.z; let g2 = g * g; let den = 1.0 + g2 - 2.0 * g * mu;\n"
    "  let pr = 3.0 / (16.0 * 3.14159265) * (1.0 + mu * mu);\n"
    "  let pm = 3.0 / (8.0 * 3.14159265) * ((1.0 - g2) * (1.0 + mu * mu)) / ((2.0 + g2) * den * sqrt(den));\n"
    "  var l = (sr * BR * u.params.x * pr + sm * BM * u.params.y * pm) * u.params.w;\n"
    "  if (tg > 0.0) { l += u.ground.xyz * exp(-(BR * u.params.x * odr + BM * 1.11 * u.params.y * odm)); }\n"
    "  return l + u.night.xyz;\n"
    "}\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)); return vec4f(p * 2.0 - 1.0, 0.0, 1.0); }\n"
    "@fragment fn fs(@builtin(position) fp: vec4f) -> @location(0) vec4f {\n"
    "  let uv = fp.xy / u.size.x;\n"
    "  let sc = uv.x * 2.0 - 1.0; let tc = uv.y * 2.0 - 1.0;\n"
    "  let f = u.face.x;\n"
    "  var g: vec3f;\n"
    "  if (f == 0) { g = vec3f(1.0, -tc, -sc); } else if (f == 1) { g = vec3f(-1.0, -tc, sc); }\n"
    "  else if (f == 2) { g = vec3f(sc, 1.0, tc); } else if (f == 3) { g = vec3f(sc, -1.0, -tc); }\n"
    "  else if (f == 4) { g = vec3f(sc, -tc, 1.0); } else { g = vec3f(-sc, -tc, -1.0); }\n"
    "  let dir = normalize(vec3f(-g.x, g.y, g.z));\n"
    "  return vec4f(sky(dir), 1.0);\n"
    "}\n";

static WGPURenderPipeline sky_pipe;
static WGPUBindGroupLayout sky_bgl;

/* the cube (re)made at this size, 6 full-screen passes into 2D views of its
 * layers (compatibility mode binds it with a cube view dimension) */
static bool wgpu_render_cube(sky_rt *s, int size, WGPUCommandEncoder enc) {
  if (!sky_pipe) {
    const char *const parts[] = { WGSL_SKY, NULL };
    WGPUShaderModule sm = cw__wg_module("sky.cube", parts);
    cw__wg_slot sl = { CW_WB_UNIFORM_DYN, WGPUShaderStage_Fragment, 0, 96 };
    sky_bgl = cw__wg_layout(&sl, 1);
    WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA16Float, .writeMask = WGPUColorWriteMask_All };
    WGPUFragmentState fs = { .module = sm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
    WGPURenderPipelineDescriptor d = {
      .label = CW_WGPU_SV("sky.cube"), .layout = cw__wg_pipeline_layout(&sky_bgl, 1),
      .vertex = { .module = sm, .entryPoint = CW_WGPU_SV("vs") },
      .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
      .multisample = { .count = 1, .mask = 0xffffffffu }, .fragment = &fs,
    };
    sky_pipe = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
    wgpuPipelineLayoutRelease(d.layout);
    wgpuShaderModuleRelease(sm);
  }
  if (!s->wcube || s->size != size) {
    if (s->wcube_v) wgpuTextureViewRelease(s->wcube_v);
    if (s->wcube) wgpuTextureRelease(s->wcube);
    s->wcube = cw__wg_texture(WGPUTextureDimension_2D, size, size, 6, 1, WGPUTextureFormat_RGBA16Float,
                              WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
                              WGPUTextureViewDimension_Cube, "sky cube");
    s->wcube_v = cw__wg_view(s->wcube, WGPUTextureViewDimension_Cube, 0, 1, 0, 6);
    s->size = size;
  }
  if (!s->wubo) s->wubo = cw__wg_buffer(256 * 6, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "sky cube u");
  float sun[3], par[4], ground[3], night[3];
  cube_params(s, sun, par, ground, night);
  uint8_t ub[256 * 6];
  memset(ub, 0, sizeof ub);
  for (int f = 0; f < 6; f++) {
    float *o = (float *)(ub + 256 * f);
    int32_t face = f;
    memcpy(o, &face, 4);
    memcpy(o + 4, sun, 12);
    memcpy(o + 8, par, 16);
    memcpy(o + 12, ground, 12);
    memcpy(o + 16, night, 12);
    o[20] = (float)size;
  }
  cw__wg_write(s->wubo, 0, ub, sizeof ub);
  cw__wg_entry e = { .buffer = s->wubo, .size = 96 };
  WGPUBindGroup bg = cw__wg_group(sky_bgl, &e, 1);
  for (int f = 0; f < 6; f++) {
    WGPUTextureView v = cw__wg_view(s->wcube, WGPUTextureViewDimension_2D, 0, 1, f, 1);
    WGPURenderPassColorAttachment ca = { .view = v, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                         .storeOp = WGPUStoreOp_Store };
    WGPURenderPassDescriptor rp = { .label = CW_WGPU_SV("sky.cube"), .colorAttachmentCount = 1, .colorAttachments = &ca };
    WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    uint32_t off = (uint32_t)(256 * f);
    wgpuRenderPassEncoderSetPipeline(p, sky_pipe);
    wgpuRenderPassEncoderSetBindGroup(p, 0, bg, 1, &off);
    wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(p);
    wgpuRenderPassEncoderRelease(p);
    wgpuTextureViewRelease(v);
  }
  wgpuBindGroupRelease(bg);
  return true;
}

/* the sky cube on WebGPU: rendered on its own encoder and submitted at once
 * (it depends on nothing the frame draws; queue order puts it before the
 * frame's work), wrapped for three.c as the background and PMREM source */
static bool wgpu_build_cube(sky_rt *s, int size) {
  if (s->wcube && s->size != size && s->cube_tex) {
    /* the wrapper goes before the texture it wraps */
    t3_release(s->cube_tex);
    s->cube_tex = NULL;
  }
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(cw__wgpu_device(), NULL);
  bool ok = wgpu_render_cube(s, size, enc);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(cw__wgpu_queue(), 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
  if (!ok) return false;
  if (!s->cube_tex) s->cube_tex = t3_texture_from_wgpu(s->wcube, s->wcube_v, size, size, 1, true, false);
  return s->cube_tex != NULL;
}

/* the sun disk: SUN_FS in WGSL, at the far plane, additive, where nothing
 * was drawn (depth test against the scene depth, no depth writes) */
static const char WGSL_SUN[] =
    "struct U { ivp: mat4x4f, sun: vec4f, disk: vec4f, color: vec4f, p: vec4f }\n" /* p: 1/w, 1/h, flip */
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)); return vec4f(p * 2.0 - 1.0, 0.0, 1.0); }\n"
    "struct FO { @location(0) c: vec4f, @builtin(frag_depth) d: f32 }\n"
    "@fragment fn fs(@builtin(position) fp: vec4f) -> FO {\n"
    "  var uv = fp.xy * u.p.xy;\n"
    "  if (u.p.z < 0.5) { uv.y = 1.0 - uv.y; }\n"
    "  let a = u.ivp * vec4f(uv * 2.0 - 1.0, -1.0, 1.0);\n"
    "  let b = u.ivp * vec4f(uv * 2.0 - 1.0, 1.0, 1.0);\n"
    "  let dir = normalize(b.xyz / b.w - a.xyz / a.w);\n"
    "  let c = dot(dir, u.sun.xyz);\n"
    "  if (c < u.disk.x) { discard; }\n"
    "  let edge = smoothstep(u.disk.x, u.disk.y, c);\n"
    "  var o: FO; o.c = vec4f(u.color.rgb * u.disk.z * edge, 0.0); o.d = 1.0; return o;\n"
    "}\n";

static void pass_sky_sun_wgpu(cw_pass_ctx *ctx, sky_rt *s) {
  const cw__wgpu_pass_targets *tg = cw__fx_pass_targets(ctx);
  if (!tg || !tg->enc || !tg->depth) return;
  static WGPURenderPipeline pipe;
  static uint32_t pipe_key;
  static WGPUBindGroupLayout bgl;
  static WGPUBuffer ubo;
  uint32_t key = (uint32_t)tg->color_format | (uint32_t)tg->depth_format << 12 | (uint32_t)(tg->samples > 1 ? tg->samples : 1) << 24;
  if (!pipe || key != pipe_key) {
    const char *const parts[] = { WGSL_SUN, NULL };
    WGPUShaderModule sm = cw__wg_module("sky.sun", parts);
    if (!bgl) {
      cw__wg_slot sl = { CW_WB_UNIFORM, WGPUShaderStage_Fragment, 0, 128 };
      bgl = cw__wg_layout(&sl, 1);
      ubo = cw__wg_buffer(128, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "sky sun u");
    }
    WGPUColorTargetState ct = { .format = tg->color_format, .blend = cw__wg_blend(CW_WG_BLEND_ADD), .writeMask = WGPUColorWriteMask_All };
    WGPUFragmentState fs = { .module = sm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
    WGPUDepthStencilState ds = { .format = tg->depth_format, .depthWriteEnabled = WGPUOptionalBool_False,
                                 .depthCompare = WGPUCompareFunction_LessEqual,
                                 .stencilFront = { .compare = WGPUCompareFunction_Always },
                                 .stencilBack = { .compare = WGPUCompareFunction_Always } };
    WGPURenderPipelineDescriptor d = {
      .label = CW_WGPU_SV("sky.sun"), .layout = cw__wg_pipeline_layout(&bgl, 1),
      .vertex = { .module = sm, .entryPoint = CW_WGPU_SV("vs") },
      .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
      .depthStencil = &ds, .multisample = { .count = (uint32_t)(tg->samples > 1 ? tg->samples : 1), .mask = 0xffffffffu },
      .fragment = &fs,
    };
    if (pipe) wgpuRenderPipelineRelease(pipe);
    pipe = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
    pipe_key = key;
    wgpuPipelineLayoutRelease(d.layout);
    wgpuShaderModuleRelease(sm);
  }
  const sky_node *k = &s->params;
  float r = CW_MAX(k->sun_size, 0.01f) * 0.5f * CW_PI / 180.0f;
  float solid = CW_PI * r * r;
  float u[32] = { 0 };
  memcpy(u, ctx->inv_view_proj.e, 64);
  u[16] = s->sun.x;
  u[17] = s->sun.y;
  u[18] = s->sun.z;
  u[20] = cosf(r);
  u[21] = cosf(r * 0.7f);
  u[22] = k->sun_disk_intensity * s->sun_light / solid;
  u[24] = s->sun_color.r;
  u[25] = s->sun_color.g;
  u[26] = s->sun_color.b;
  u[28] = 1.0f / (float)tg->w;
  u[29] = 1.0f / (float)tg->h;
  u[30] = tg->flip_y ? 1.0f : 0.0f;
  cw__wg_write(ubo, 0, u, sizeof u);
  cw__wg_entry e = { .buffer = ubo };
  WGPUBindGroup bg = cw__wg_group(bgl, &e, 1);
  WGPURenderPassEncoder p = cw__wg_scene_pass(tg, true, "sky_sun");
  wgpuRenderPassEncoderSetPipeline(p, pipe);
  wgpuRenderPassEncoderSetBindGroup(p, 0, bg, 0, NULL);
  wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
  wgpuBindGroupRelease(bg);
}

WGPUTexture cw__sky_cube_wgpu(cw_tree *t, int *size);
WGPUTexture cw__sky_cube_wgpu(cw_tree *t, int *size) {
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  if (!s || !s->wcube) return NULL;
  *size = s->size;
  return s->wcube;
}
#endif

/* tests: build the sky cube now, without the pipeline or a three.c renderer
 * (GL: no PMREM, which is three.c's there); false when the tree has no sky */
bool cw__sky_test_build(cw_tree *t);
bool cw__sky_test_build(cw_tree *t) {
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  if (!s || !s->present) return false;
#if CW_WGPU
  if (cw__wgpu_on()) {
    if (!wgpu_build_cube(s, CW_CLAMP(s->params.resolution, 16, 1024))) return false;
    built(s, params_key(&s->params));
    return true;
  }
#endif
  if (!gl_render_cube(s, CW_CLAMP(s->params.resolution, 16, 1024), NULL)) return false;
  built(s, params_key(&s->params));
  return true;
}
/* tests: the SKY-phase sun disk on a caller-built context */
void cw__sky_test_sun(cw_pass_ctx *ctx);
void cw__sky_test_sun(cw_pass_ctx *ctx) {
  sky_rt *s = ctx->tree ? cw_tree_slot(ctx->tree, "sky") : NULL;
  if (s && sky_sun_active(ctx, s)) pass_sky_sun(ctx, s);
}
/* the GL cube as a three.c texture (tests feed it to three.c's PMREM) */
t3_texture *cw__sky_cube_tex(cw_tree *t);
t3_texture *cw__sky_cube_tex(cw_tree *t) {
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  return s ? s->cube_tex : NULL;
}
/* the GL cube and its face size (0 when none) */
unsigned cw__sky_cube_gl(cw_tree *t, int *size);
unsigned cw__sky_cube_gl(cw_tree *t, int *size) {
  sky_rt *s = t ? cw_tree_slot(t, "sky") : NULL;
  if (!s) return 0;
  *size = s->size;
  return s->cube;
}

void cw__register_sky_types(void) {
  void cw__environment_debug_init(void); /* environment_debug.c */
  cw__environment_debug_init();
  cw_node_type_register(&sky_type);
  sky_t = &sky_type;
}

/* night_street: hundreds of lights through clustered shading
 * (cartwheel/lighting.h). A city street at night, 240 m long: 58 street
 * lamps, 96 neon signs, 48 shop-window spot lights and 12 muzzle flashes
 * down the road (214 clustered lights, flashing on and off by tick), four
 * shadowed floodlights over the road (three.c's shadow maps) and a dim moon.
 * The camera drives down the street at 15 m/s.
 *
 * LightingSettings.mode is CLUSTERED (the scene file); NS_FORWARD=1 (native,
 * the gate's control) switches it to FORWARD, where three.c lights a surface
 * with 8 omni and 8 spot lights at most and the rest light nothing.
 * NS_PERF=1 holds the camera still (night_street_perf renders 1920x1080).
 * NS_PARITY=1 keeps 7 omni and 7 spot lights, all shadowless (see light()).
 *
 * Debug state: ns.tick, ns.clustered, ns.spots, ns.forward, ns.refs,
 * ns.max_in_cluster, ns.overflow, ns.materials, ns.grid_hash (the binned
 * cells and lists, folded to 32 bits), ns.cam_z. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/lighting.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

#define LAMPS_PER_SIDE 29
#define BUILDINGS_PER_SIDE 24
#define FLASHES 12
#define FLOODS 4
#define STREET_Z0 20.0f
#define STREET_LEN 240.0f

static int32_t tick, clustered, spots, forward, refs, max_in, overflow, materials, grid_hash;
static float cam_z;
static bool built;
static cw_node flashes[FLASHES];

/* night_street_perf: segments of PERF_SEG frames at each preset, then High in FORWARD mode */
#ifndef NS_PERF_BUILD
#define NS_PERF_BUILD 0
#endif
#define PERF_SEG 150
#define PERF_SETTLE 30
#define PERF_SEGS 5
static const cw_quality perf_preset[PERF_SEGS] = { CW_QUALITY_LOW, CW_QUALITY_MEDIUM, CW_QUALITY_HIGH, CW_QUALITY_ULTRA,
                                                   CW_QUALITY_HIGH };
static const char *const perf_label[PERF_SEGS] = { "low", "medium", "high", "ultra", "high-forward" };
static double perf_sum[PERF_SEGS], perf_cpu_sum[PERF_SEGS], perf_gpu_sum[PERF_SEGS], perf_bin_sum[PERF_SEGS];
static float perf_max[PERF_SEGS];
static int perf_n[PERF_SEGS], perf_lights[PERF_SEGS];

static double wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

static t3_instanced_mesh *instanced(t3_object *parent, t3_geometry *g, t3_material *m, int count, bool cast) {
  t3_instanced_mesh *im = t3_instanced_mesh_new(g, m, count);
  im->mesh.base.frustum_culled = false;
  im->mesh.base.cast_shadow = cast;
  im->mesh.base.receive_shadow = true;
  t3_object_add(parent, im);
  t3_release(im);
  return im;
}

static void place(t3_instanced_mesh *im, int i, t3_vec3 p, t3_vec3 s) {
  t3_mat4 m;
  t3_mat4_compose(&m, p, (t3_quat){ 0, 0, 0, 1 }, s);
  t3_instanced_mesh_set_matrix_at(im, i, &m);
}

static t3_material *standard(float r, float g, float b, float rough, float metal) {
  t3_material *m = t3_mesh_standard_material_new(0xffffff);
  m->color = (t3_color){ r, g, b };
  m->roughness = rough;
  m->metalness = metal;
  return m;
}

/* NS_PARITY=1: at most 7 omni and 7 spot lights, all shadowless, so FORWARD
 * (three.c lights them all) and CLUSTERED must draw the same image;
 * NS_PARITY=2 leaves one omni light out (the parity check's control) */
static int parity, n_omni, n_spot;

static cw_node light(cw_node parent, const char *type, const char *name, t3_vec3 p, t3_color c, float energy, float range) {
  if (parity) {
    bool omni = !strcmp(type, "OmniLight3D");
    int *k = omni ? &n_omni : &n_spot;
    if (*k >= (omni && parity == 2 ? 6 : 7)) return (cw_node){ 0 };
    ++*k;
  }
  cw_node n = cw_node_new(type, name);
  cw_node_set(n, "color", cw_val_color(c.r, c.g, c.b, 1));
  cw_node_set(n, "energy", cw_val_float(energy));
  cw_node_set(n, "range", cw_val_float(range));
  cw_node_add_child(parent, n);
  cw_node3d_set_position(n, p);
  return n;
}

static void build(cw_tree *t) {
  cw_node root = cw_node_get(cw_tree_root(t), "Street");
  t3_object *parent = cw_node3d_object(root);
  cw_rng rng;
  cw_rng_seed(&rng, 2026);
  float zc = STREET_Z0 - STREET_LEN * 0.5f;

  /* road, sidewalks, cars */
  t3_geometry *box = t3_box_geometry_new(1, 1, 1, 1, 1, 1);
  t3_material *asphalt = standard(0.035f, 0.035f, 0.04f, 0.55f, 0);
  t3_instanced_mesh *road = instanced(parent, box, asphalt, 1, false);
  place(road, 0, (t3_vec3){ 0, -0.05f, zc }, (t3_vec3){ 60, 0.1f, STREET_LEN + 40 });
  t3_material *stone = standard(0.18f, 0.17f, 0.16f, 0.8f, 0);
  t3_instanced_mesh *walks = instanced(parent, box, stone, 2, false);
  for (int s = 0; s < 2; s++) place(walks, s, (t3_vec3){ s ? 6.5f : -6.5f, 0.08f, zc }, (t3_vec3){ 3, 0.16f, STREET_LEN });
  t3_material *paint = standard(1, 1, 1, 0.35f, 0.4f);
  t3_instanced_mesh *cars = instanced(parent, box, paint, 10, true);
  for (int i = 0; i < 10; i++) {
    float x = (i & 1) ? 2.5f : -2.5f, z = STREET_Z0 - 15 - (float)i * 21 - cw_rng_range(&rng, 0, 8);
    place(cars, i, (t3_vec3){ x, 0.75f, z }, (t3_vec3){ 1.9f, 1.4f, 4.4f });
    t3_instanced_mesh_set_color_at(cars, i, (t3_color){ cw_rng_range(&rng, 0.05f, 0.6f), cw_rng_range(&rng, 0.05f, 0.4f),
                                                          cw_rng_range(&rng, 0.05f, 0.5f) });
  }

  /* buildings: facades at |x| = 8 */
  t3_material *wall = standard(1, 1, 1, 0.85f, 0);
  t3_instanced_mesh *blocks = instanced(parent, box, wall, 2 * BUILDINGS_PER_SIDE, true);
  float bw = STREET_LEN / BUILDINGS_PER_SIDE;
  for (int s = 0; s < 2; s++)
    for (int i = 0; i < BUILDINGS_PER_SIDE; i++) {
      float h = cw_rng_range(&rng, 8, 30), d = cw_rng_range(&rng, 10, 16);
      float z = STREET_Z0 - bw * ((float)i + 0.5f);
      place(blocks, s * BUILDINGS_PER_SIDE + i, (t3_vec3){ (s ? 1 : -1) * (8 + d * 0.5f), h * 0.5f, z },
            (t3_vec3){ d, h, bw - 0.6f });
      float g = cw_rng_range(&rng, 0.12f, 0.3f);
      t3_instanced_mesh_set_color_at(blocks, s * BUILDINGS_PER_SIDE + i, (t3_color){ g, g * 0.95f, g * 0.9f });
    }

  /* the shadowed floodlights first (three.c wants a kind's shadow casters
   * before its plain lights), then everything clustered */
  cw_node lights = cw_node_new("Node3D", "Lights");
  cw_node_add_child(root, lights);
  for (int i = 0; i < (parity ? 0 : FLOODS); i++) {
    char name[24];
    snprintf(name, sizeof name, "Flood%d", i);
    float z = STREET_Z0 - 30 - (float)i * 52;
    cw_node f = light(lights, "SpotLight3D", name, (t3_vec3){ (i & 1) ? 6 : -6, 9, z }, (t3_color){ 0.85f, 0.9f, 1 }, 220, 30);
    cw_node_set(f, "angle", cw_val_float(0.55f));
    cw_node_set(f, "penumbra", cw_val_float(0.4f));
    cw_node_set(f, "shadow", cw_val_bool(true));
    cw_node_set(f, "shadow_size", cw_val_int(1024));
    cw_node_set(f, "shadow_bias", cw_val_float(-0.0005f));
    cw_node3d_look_at(f, (t3_vec3){ 0, 0, z - 6 }, (t3_vec3){ 0, 1, 0 });
  }

  /* street lamps: iron posts, glowing bulbs, warm omni lights */
  t3_geometry *pole = t3_cylinder_geometry_new(0.07f, 0.1f, 5.2f, 8, 1, false);
  t3_material *iron = standard(0.04f, 0.04f, 0.045f, 0.5f, 0.6f);
  t3_instanced_mesh *posts = instanced(parent, pole, iron, 2 * LAMPS_PER_SIDE, true);
  t3_geometry *bulb = t3_sphere_geometry_new(0.2f, 10, 6);
  t3_material *glow = t3_mesh_basic_material_new(0xffffff);
  glow->color = (t3_color){ 6, 4.2f, 2.4f };
  t3_instanced_mesh *bulbs = instanced(parent, bulb, glow, 2 * LAMPS_PER_SIDE, false);
  for (int s = 0; s < 2; s++)
    for (int i = 0; i < LAMPS_PER_SIDE; i++) {
      int k = s * LAMPS_PER_SIDE + i;
      float x = s ? 6.6f : -6.6f, z = STREET_Z0 - 4 - (float)i * 8 - (s ? 4 : 0);
      place(posts, k, (t3_vec3){ x, 2.6f, z }, (t3_vec3){ 1, 1, 1 });
      place(bulbs, k, (t3_vec3){ x, 5.3f, z }, (t3_vec3){ 1, 1, 1 });
      char name[24];
      snprintf(name, sizeof name, "Lamp%d", k);
      light(lights, "OmniLight3D", name, (t3_vec3){ x, 5.1f, z }, (t3_color){ 1, 0.68f, 0.38f }, 14, 11);
    }

  /* neon signs: two a building, glowing bars on the facade with a coloured light each */
  static const t3_color neon[6] = { { 1, 0.1f, 0.5f }, { 0.1f, 0.8f, 1 }, { 0.6f, 1, 0.1f },
                                    { 1, 0.4f, 0.05f }, { 0.6f, 0.2f, 1 }, { 1, 0.95f, 0.2f } };
  t3_material *neon_m = t3_mesh_basic_material_new(0xffffff);
  t3_instanced_mesh *bars = instanced(parent, box, neon_m, 4 * BUILDINGS_PER_SIDE, false);
  int nb = 0;
  for (int s = 0; s < 2; s++)
    for (int i = 0; i < BUILDINGS_PER_SIDE; i++)
      for (int j = 0; j < 2; j++) {
        float z = STREET_Z0 - bw * ((float)i + 0.3f + 0.4f * (float)j), y = cw_rng_range(&rng, 3.2f, 7.5f);
        float x = s ? 7.85f : -7.85f;
        t3_color c = neon[(int)cw_rng_range(&rng, 0, 5.999f)];
        place(bars, nb, (t3_vec3){ x, y, z }, (t3_vec3){ 0.12f, 0.35f, cw_rng_range(&rng, 1.2f, 3.5f) });
        t3_instanced_mesh_set_color_at(bars, nb, (t3_color){ c.r * 8, c.g * 8, c.b * 8 });
        nb++;
        char name[24];
        snprintf(name, sizeof name, "Neon%d", nb);
        light(lights, "OmniLight3D", name, (t3_vec3){ x + (s ? -0.6f : 0.6f), y, z }, c, 7, 6);
      }

  /* shop windows: a spot light over each door, aimed at the sidewalk */
  for (int s = 0; s < 2; s++)
    for (int i = 0; i < BUILDINGS_PER_SIDE; i++) {
      char name[24];
      snprintf(name, sizeof name, "Shop%d", s * BUILDINGS_PER_SIDE + i);
      float x = s ? 7.7f : -7.7f, z = STREET_Z0 - bw * ((float)i + 0.75f);
      cw_node sp = light(lights, "SpotLight3D", name, (t3_vec3){ x, 3.4f, z }, (t3_color){ 1, 0.85f, 0.6f }, 16, 8);
      if (!cw_node_valid(sp)) continue;
      cw_node_set(sp, "angle", cw_val_float(0.75f));
      cw_node_set(sp, "penumbra", cw_val_float(0.5f));
      cw_node3d_look_at(sp, (t3_vec3){ x + (s ? -2.5f : 2.5f), 0, z }, (t3_vec3){ 0, 1, 0 });
    }

  /* muzzle flashes down the road */
  for (int i = 0; i < (parity ? 0 : FLASHES); i++) {
    char name[24];
    snprintf(name, sizeof name, "Flash%d", i);
    flashes[i] = light(lights, "OmniLight3D", name,
                       (t3_vec3){ cw_rng_range(&rng, -3.5f, 3.5f), 1.2f, STREET_Z0 - 40 - (float)i * 14 },
                       (t3_color){ 1, 0.6f, 0.2f }, 0, 8);
  }

  t3_release(box); t3_release(asphalt); t3_release(stone); t3_release(paint); t3_release(wall);
  t3_release(pole); t3_release(iron); t3_release(bulb); t3_release(glow); t3_release(neon_m);
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("ns.tick", &tick);
  cw_debug_i32("ns.clustered", &clustered);
  cw_debug_i32("ns.spots", &spots);
  cw_debug_i32("ns.forward", &forward);
  cw_debug_i32("ns.refs", &refs);
  cw_debug_i32("ns.max_in_cluster", &max_in);
  cw_debug_i32("ns.overflow", &overflow);
  cw_debug_i32("ns.materials", &materials);
  cw_debug_i32("ns.grid_hash", &grid_hash);
  cw_debug_f32("ns.cam_z", &cam_z);
}

static bool perf_mode(void) {
  const char *p = getenv("NS_PERF");
  return p && *p == '1';
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  if (!built) {
    built = true;
    const char *pa = getenv("NS_PARITY");
    parity = pa ? atoi(pa) : 0;
    build(t);
    const char *f = getenv("NS_FORWARD");
    if (f && *f == '1') cw_node_set(cw_node_get(cw_tree_root(t), "Street/Lighting"), "mode", cw_val_enum("FORWARD"));
  }
  tick++;
  if (NS_PERF_BUILD) {
    int seg = (tick - 1) / PERF_SEG;
    if (seg < PERF_SEGS && (tick - 1) % PERF_SEG == 0) {
      cw_quality_set(perf_preset[seg], false);
      cw_node_set(cw_node_get(cw_tree_root(t), "Street/Lighting"), "mode",
                  cw_val_enum(seg == PERF_SEGS - 1 ? "FORWARD" : "CLUSTERED"));
    }
  }
  for (int i = 0; i < FLASHES && cw_node_valid(flashes[i]); i++) {
    bool on = (tick + i * 5) % 17 < 3;
    cw_node_set(flashes[i], "energy", cw_val_float(on ? 60.0f : 0.0f));
  }
  cam_z = perf_mode() || parity ? -20.0f : 15.0f - (float)tick * 0.25f;
  cw_node cam = cw_node_get(cw_tree_root(t), "Street/Camera");
  cw_node3d_set_position(cam, (t3_vec3){ 0.6f * cw_sin((float)tick * 0.02f), 3, cam_z });
  cw_node3d_set_rotation(cam, (t3_vec3){ -0.06f, 0.08f * cw_sin((float)tick * 0.013f), 0 });
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_tree *t = cw_scene_app_tree();
  cw_lighting_stats s;
  cw_lighting_get_stats(t, &s);
  clustered = s.clustered;
  spots = s.clustered_spots;
  forward = s.forward;
  refs = s.grid.references;
  max_in = s.grid.max_in_froxel;
  overflow = s.grid.dropped;
  materials = s.materials;
  cw_froxel_grid *g = cw_lighting_froxels(t);
  uint64_t h = g ? cw_froxel_grid_hash(g) : 0;
  grid_hash = (int32_t)(uint32_t)(h ^ (h >> 32));
  if (!NS_PERF_BUILD) return;
  /* frame time: render end to render end, the GPU drained each frame on GL
   * (a 1-pixel read); WebGPU reports the passes' GPU time instead */
  static double last;
  double submitted = wall_ms();
  if (cw_gpu_backend() == CW_GPU_GL) {
    uint8_t px[4];
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  }
  double now = wall_ms();
  int seg = (tick - 1) / PERF_SEG, at = (tick - 1) % PERF_SEG;
  if (last > 0 && seg >= 0 && seg < PERF_SEGS && at >= PERF_SETTLE) {
    float d = (float)(now - last);
    perf_sum[seg] += d;
    perf_cpu_sum[seg] += submitted - last;
    perf_gpu_sum[seg] += cw_pipeline_gpu_ms(cw_pipeline_of(t));
    perf_bin_sum[seg] += s.grid.bin_ms;
    if (d > perf_max[seg]) perf_max[seg] = d;
    perf_lights[seg] = s.clustered;
    if (++perf_n[seg] == PERF_SEG - PERF_SETTLE)
      fprintf(stderr, "night-street-perf seg=%d preset=%s api=%s frame_ms=%.2f max_ms=%.2f cpu_ms=%.2f gpu_ms=%.2f bin_ms=%.3f clustered=%d\n",
              seg, perf_label[seg], cw_gpu_backend_name(), perf_sum[seg] / perf_n[seg], perf_max[seg],
              perf_cpu_sum[seg] / perf_n[seg], perf_gpu_sum[seg] / perf_n[seg], perf_bin_sum[seg] / perf_n[seg],
              perf_lights[seg]);
  }
  last = now;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "night_street", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
#ifdef NS_PERF_BUILD
  base.width = 1920;
  base.height = 1080;
#endif
  return cw_scene_app("main.cwscene", &base);
}

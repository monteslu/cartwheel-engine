/* fx_particles: GPUParticles3D in every mode on uneven ground.
 *
 *   Fountain   SPHERE emitter, depth-buffer collision on steps, a ramp and a
 *              boulder, trails (10 samples per particle)
 *   Sparks     an explosive burst, STRETCHED along velocity, bouncing off an
 *              analytic sphere collider and the floor plane collider
 *   Smoke      ALPHA blended, GPU bitonic sort back to front, soft particles,
 *              curl-noise turbulence, size over life
 *   Debris     instanced MESH boxes (OPAQUE), spinning, bouncing on the plane
 *   Swirl      a RING emitter pulled by a ParticleAttractor3D
 *   Skin       emission from a sphere mesh's surface (MESH shape, normals)
 *   Ribbon     a Trail3D on a node orbiting the scene
 *
 * Debug state: fx.<name>.alive (CPU emission function, exact), .gpu_alive
 * and .gpu_sum (read back from the GPU: compared with a tolerance),
 * fx.systems / fx.alive / fx.draws / fx.sort_passes / fx.steps, fx.tick. */
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static const char *const names[] = { "Fountain", "Sparks", "Smoke", "Debris", "Swirl", "Skin" };
#define NSYS 6
static int32_t alive[NSYS], gpu_alive[NSYS];
static float gpu_sum[NSYS][3];
static int32_t tot_systems, tot_alive, tot_draws, tot_sort, tot_steps;
static uint32_t tick;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
  static char keys[NSYS][3][40];
  for (int i = 0; i < NSYS; i++) {
    snprintf(keys[i][0], 40, "fx.%s.alive", names[i]);
    snprintf(keys[i][1], 40, "fx.%s.gpu_alive", names[i]);
    snprintf(keys[i][2], 40, "fx.%s.gpu_sum", names[i]);
    cw_debug_i32(keys[i][0], &alive[i]);
    cw_debug_i32(keys[i][1], &gpu_alive[i]);
    cw_debug_f32_array(keys[i][2], gpu_sum[i], 3);
  }
  cw_debug_i32("fx.systems", &tot_systems);
  cw_debug_i32("fx.alive", &tot_alive);
  cw_debug_i32("fx.draws", &tot_draws);
  cw_debug_i32("fx.sort_passes", &tot_sort);
  cw_debug_i32("fx.steps", &tot_steps);
  cw_debug_u32("fx.tick", &tick);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  float s, c, s2, c2;
  cw_sincos((float)tick * 0.045f, &s, &c);
  cw_sincos((float)tick * 0.09f, &s2, &c2);
  cw_node3d_set_position(node("World/Orbiter"), (t3_vec3){ 3.2f * c, 1.4f + 0.5f * s2, 1.2f + 2.0f * s });
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  for (int i = 0; i < NSYS; i++) {
    char path[64];
    snprintf(path, sizeof path, "World/%s", names[i]);
    cw_node n = node(path);
    cw_value v;
    alive[i] = cw_particles_alive(n);
    if (cw_node_get_prop(n, "gpu_alive", &v)) gpu_alive[i] = (int32_t)v.i;
    if (cw_node_get_prop(n, "gpu_sum", &v))
      for (int k = 0; k < 3; k++) gpu_sum[i][k] = v.v[k];
  }
  cw_particles_stats st;
  cw_particles_get_stats(cw_scene_app_tree(), &st);
  tot_systems = st.systems;
  tot_alive = st.alive;
  tot_draws = st.draw_calls;
  tot_sort = st.sort_passes;
  tot_steps = st.sim_steps;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_particles", .width = 640, .height = 360, .debug = true,
                              .init = init, .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}

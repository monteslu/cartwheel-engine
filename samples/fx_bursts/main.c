/* fx_bursts: a pool of one-shot effects as one GPUParticles3D of burst slots
 * (cartwheel/particles.h: bursts, cw_particles_burst). Eight slots of 40
 * sparks each; every 9 ticks the next slot fires at the next point of a ring,
 * thrown outward from it, so up to eight bursts live at once from one
 * simulation and one draw.
 *
 * Debug state: fx.tick, fx.bursts.alive (the CPU emission function, exact on
 * every host), fx.bursts.fired, fx.systems / fx.steps / fx.draws (one system
 * for the pool). FX_BURSTS_OFF=1 (the gate's control) leaves the system at
 * bursts 0, so cw_particles_burst does nothing and no spark is drawn. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t tick, alive, fired, systems, steps, draws;
static cw_node pool;

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("fx.tick", &tick);
  cw_debug_i32("fx.bursts.alive", &alive);
  cw_debug_i32("fx.bursts.fired", &fired);
  cw_debug_i32("fx.systems", &systems);
  cw_debug_i32("fx.steps", &steps);
  cw_debug_i32("fx.draws", &draws);
  cw_node3d_look_at(node("Root/Camera"), (t3_vec3){ 0, 0.5f, 0 }, (t3_vec3){ 0, 1, 0 });
  cw_node3d_look_at(node("Root/Sun"), (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 1, 0 });
  pool = cw_node_new("GPUParticles3D", "Sparks");
  cw_node_add_child(node("Root"), pool);
  cw_node_set(pool, "amount", cw_val_int(8 * 40));
  cw_node_set(pool, "lifetime", cw_val_float(0.9f));
  cw_node_set(pool, "lifetime_random", cw_val_float(0.3f));
  cw_node_set(pool, "explosiveness", cw_val_float(1));
  cw_node_set(pool, "emission_shape", cw_val_enum("SPHERE"));
  cw_node_set(pool, "emission_radius", cw_val_float(0.08f));
  cw_node_set(pool, "spread", cw_val_float(35));
  cw_node_set(pool, "speed_min", cw_val_float(2));
  cw_node_set(pool, "speed_max", cw_val_float(5));
  cw_node_set(pool, "gravity", cw_val_vec3(0, -6, 0));
  cw_node_set(pool, "draw_mode", cw_val_enum("STRETCHED"));
  cw_node_set(pool, "stretch", cw_val_float(0.05f));
  cw_node_set(pool, "size", cw_val_float(0.06f));
  cw_node_set(pool, "energy", cw_val_float(6));
  cw_node_set(pool, "color", cw_val_color(1, 0.7f, 0.3f, 1));
  cw_node_set(pool, "seed", cw_val_int(7));
  if (!getenv("FX_BURSTS_OFF")) cw_node_set(pool, "bursts", cw_val_int(8));
}

static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  if (tick % 9 == 0) { /* the next point of a ring, thrown outward and up */
    float a = (float)(tick / 9) * 0.785398f, s, c;
    cw_sincos(a, &s, &c);
    cw_particles_burst(pool, (t3_vec3){ 3 * c, 0.3f, 3 * s }, (t3_vec3){ c, 1.2f, s });
    fired++;
  }
  tick++;
  alive = cw_particles_alive(pool);
}

static void render(void *u, double a) {
  (void)u, (void)a;
  cw_particles_stats st;
  cw_particles_get_stats(cw_scene_app_tree(), &st);
  systems = st.systems, steps = st.sim_steps, draws = st.draw_calls;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc app = { .title = "fx_bursts", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed_update,
                             .render = render };
  return cw_scene_app("main.cwscene", &app);
}

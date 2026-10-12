/* joints_nodes: every Joint3D and Joint2D variant from one .cwscene. 3D: a
 * motor-driven HINGE door, a chain of PIN joints, a SLIDER piston with
 * limits and a motor, a SPRING, a WELD pair, a CONE arm and a DISTANCE rope
 * that breaks under a heavy ball (its `broken` signal is connected in the
 * scene to "count_break"). 2D, drawn over the 3D view in screen space: a
 * PIN bridge with a load, a car on two WHEEL joints with a motor, a SPRING
 * and a SLIDER with a motor between limits.
 *
 * Debug fields: door.angle, ball3.pos, piston.x, bob.y, weld.dist, arm.pos,
 * rope.broken, bridge.mid_y, car.x, bouncer.y, shuttle.x, phys3d.hash,
 * phys2d.hash, joints3d, joints2d (live joints). Input-free. */
#include "cartwheel/debug.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static struct {
  float door, ball3[3], piston, bob, weld, arm[3], mid_y, car, bouncer, shuttle;
  int32_t broken, joints3d, joints2d;
  uint32_t h3, h2;
} G;

static void count_break(cw_node e, const cw_value *a, int c, void *u) {
  (void)e; (void)a; (void)c; (void)u;
  G.broken++;
}
static cw_signal_fn resolve(cw_node target, const char *method, void **user) {
  (void)target;
  *user = NULL;
  return !strcmp(method, "count_break") ? count_break : NULL;
}

static void init(void *u) {
  (void)u;
  cw_debug_f32("door.angle", &G.door);
  cw_debug_f32_array("ball3.pos", G.ball3, 3);
  cw_debug_f32("piston.x", &G.piston);
  cw_debug_f32("bob.y", &G.bob);
  cw_debug_f32("weld.dist", &G.weld);
  cw_debug_f32_array("arm.pos", G.arm, 3);
  cw_debug_i32("rope.broken", &G.broken);
  cw_debug_f32("bridge.mid_y", &G.mid_y);
  cw_debug_f32("car.x", &G.car);
  cw_debug_f32("bouncer.y", &G.bouncer);
  cw_debug_f32("shuttle.x", &G.shuttle);
  cw_debug_u32("phys3d.hash", &G.h3);
  cw_debug_u32("phys2d.hash", &G.h2);
  cw_debug_i32("joints3d", &G.joints3d);
  cw_debug_i32("joints2d", &G.joints2d);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  cw_node L = cw_node_get(cw_tree_root(t), "Lab");
  cw_value v;
  cw_node_get_prop(cw_node_get(L, "Door"), "rotation", &v);
  G.door = v.v[1];
  t3_vec3 b3 = cw_node3d_position(cw_node_get(L, "Ball3"));
  G.ball3[0] = b3.x, G.ball3[1] = b3.y, G.ball3[2] = b3.z;
  G.piston = cw_node3d_position(cw_node_get(L, "Piston")).x;
  G.bob = cw_node3d_position(cw_node_get(L, "Bob")).y;
  t3_vec3 wa = cw_node3d_position(cw_node_get(L, "WeldA")), wb = cw_node3d_position(cw_node_get(L, "WeldB"));
  G.weld = t3_vec3_length(t3_vec3_sub(wa, wb));
  t3_vec3 am = cw_node3d_position(cw_node_get(L, "Arm"));
  G.arm[0] = am.x, G.arm[1] = am.y, G.arm[2] = am.z;
  G.mid_y = cw_node2d_position(cw_node_get(L, "Board/Plank2")).y;
  G.car = cw_node2d_position(cw_node_get(L, "Board/Chassis")).x;
  G.bouncer = cw_node2d_position(cw_node_get(L, "Board/Bouncer")).y;
  G.shuttle = cw_node2d_position(cw_node_get(L, "Board/Shuttle")).x;
  cw_phys_stats s3 = cw_world3d_stats(cw_physics3d_world(t)), s2 = cw_world2d_stats(cw_physics2d_world(t));
  G.h3 = s3.hash;
  G.h2 = s2.hash;
  G.joints3d = s3.joints;
  G.joints2d = s2.joints;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_set_method_resolver(resolve);
  static cw_app_desc base = { .title = "joints_nodes", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}

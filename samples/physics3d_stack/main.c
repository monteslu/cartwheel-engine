/* physics3d_stack: the 3D physics module end to end.
 *
 * A 6-level pyramid of boxes (91 boxes), a capsule and a convex hull on
 * top, a static triangle-mesh ramp, all bound to three.c meshes and drawn
 * with render-time interpolation (cw_world3d_sync(alpha)). At tick 60 a
 * heavy ball is fired into the pyramid; contact and hit events are counted
 * into debug fields, the world's running state hash (every moved transform
 * and every event, every step) is published, and a batched ray cast (64 rays
 * on the job pool) measures the pile's height profile. The physics debug
 * draw is overlaid as lines.
 *
 * Debug fields: phys.hash, phys.steps, phys.bodies, phys.awake, phys.contacts,
 * ev.begin, ev.end, ev.hit, ev.hit_speed_max, ball.x/y/z, ray.hits,
 * ray.height_sum, lines.count */
#include <math.h>

#include "../common/phys_lines_t3.h"
#include "cartwheel/debug.h"
#include "cartwheel/physics.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "three.h"

#define LEVELS 6
#define RAYS 64

typedef struct app {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  phys_lines_t3 lines_t3;
  cw_phys_lines lines;
  cw_world3d world;
  cw_body3d ball;
  cw_mesh3d ramp_mesh;
  t3_material *mat_box, *mat_ball, *mat_static;
  /* debug fields */
  uint32_t hash;
  int32_t steps, bodies, awake, contacts, ev_begin, ev_end, ev_hit, ray_hits, line_count;
  float hit_speed_max, ball_pos[3], ray_height_sum;
} app;

static app A;

static t3_mesh *add_mesh(t3_geometry *g, t3_material *m) {
  t3_mesh *mesh = t3_mesh_new(g, m);
  t3_object_add((t3_object *)A.scene, mesh);
  t3_release(mesh);
  t3_release(g);
  return mesh;
}

static void build_world(void) {
  /* #region world_def */
  cw_world3d_def wd = cw_world3d_default_def();
  wd.layers[0] = "static";
  wd.layers[1] = "props";
  wd.hit_speed = 2;
  A.world = cw_world3d_create(&wd);
  /* #endregion */

  cw_body3d_def bd = cw_body3d_default_def();
  cw_shape3d_def sd = cw_shape3d_default_def();

  /* ground */
  bd.type = CW_BODY_STATIC;
  cw_body3d ground = cw_body3d_create(A.world, &bd);
  sd.layer = 0;
  sd.material.user_material = 1;
  cw_shape3d_box(ground, &sd, (t3_vec3){ 20, 0.5f, 20 }, cw_xform3_at(0, -0.5f, 0));
  t3_mesh *gm = add_mesh(t3_box_geometry_new(40, 1, 40, 1, 1, 1), A.mat_static);
  t3_object_set_position(gm, 0, -0.5f, 0);

  /* a static triangle-mesh ramp behind the pyramid */
  static const t3_vec3 rv[6] = { { -4, 0, -6 }, { 4, 0, -6 }, { 4, 2, -9 }, { -4, 2, -9 }, { -4, 0, -9 }, { 4, 0, -9 } };
  static const int32_t ri[12] = { 0, 1, 2, 0, 2, 3, 0, 3, 4, 1, 5, 2 };
  A.ramp_mesh = cw_mesh3d_create(rv, 6, ri, 4);
  cw_shape3d_mesh(ground, &sd, A.ramp_mesh, (t3_vec3){ 1, 1, 1 });

  /* the pyramid: LEVELS levels of (n x n) boxes */
  bd.type = CW_BODY_DYNAMIC;
  sd.layer = 1;
  sd.material.user_material = 2;
  sd.hit_events = true;
  const float h = 0.5f;
  for (int level = 0; level < LEVELS; level++) {
    int n = LEVELS - level;
    for (int i = 0; i < n; i++)
      for (int k = 0; k < n; k++) {
        bd.position = (t3_vec3){ (i - (n - 1) * 0.5f) * 2.05f * h, h + level * 2 * h, (k - (n - 1) * 0.5f) * 2.05f * h };
        cw_body3d b = cw_body3d_create(A.world, &bd);
        cw_shape3d_box(b, &sd, (t3_vec3){ h, h, h }, cw_xform3_identity());
        cw_body3d_bind_object(b, (t3_object *)add_mesh(t3_box_geometry_new(2 * h, 2 * h, 2 * h, 1, 1, 1), A.mat_box));
      }
  }
  /* a capsule and a hull on top */
  bd.position = (t3_vec3){ 0, LEVELS * 2 * h + 0.6f, 0 };
  cw_body3d cap = cw_body3d_create(A.world, &bd);
  cw_shape3d_capsule(cap, &sd, (t3_vec3){ 0, -0.4f, 0 }, (t3_vec3){ 0, 0.4f, 0 }, 0.3f);
  /* a cylinder stands in for the capsule; the debug lines show the real shape */
  cw_body3d_bind_object(cap, (t3_object *)add_mesh(t3_cylinder_geometry_new(0.3f, 0.3f, 1.4f, 12, 1, false), A.mat_ball));
  static const t3_vec3 tetra[5] = { { 0, 0.6f, 0 }, { -0.5f, -0.3f, -0.4f }, { 0.5f, -0.3f, -0.4f }, { 0, -0.3f, 0.6f }, { 0.1f, 0.1f, 0.1f } };
  bd.position = (t3_vec3){ 1.5f, 0.5f, 3 };
  cw_body3d hull = cw_body3d_create(A.world, &bd);
  cw_shape3d_hull(hull, &sd, tetra, 5);
  cw_body3d_bind_object(hull, (t3_object *)add_mesh(t3_tetrahedron_geometry_new(0.6f, 0), A.mat_ball));

  /* the ball, parked off to the side until it is fired */
  bd.position = (t3_vec3){ -12, 1, 0 };
  bd.bullet = true;
  A.ball = cw_body3d_create(A.world, &bd);
  cw_shape3d_def bs = sd;
  bs.density = 20;
  cw_shape3d_sphere(A.ball, &bs, (t3_vec3){ 0, 0, 0 }, 0.6f);
  cw_body3d_bind_object(A.ball, (t3_object *)add_mesh(t3_sphere_geometry_new(0.6f, 20, 12), A.mat_ball));
}

static void init(void *user) {
  (void)user;
  int w, h;
  cw_drawable_size(&w, &h);
  A.renderer = cw_render3d_window_renderer(w, h);
  t3_renderer_set_clear_color(A.renderer, 0x1d2430, 1);
  A.scene = t3_scene_new();
  A.camera = t3_perspective_camera_new(55, (float)w / (float)h, 0.1f, 200);
  t3_object_set_position(A.camera, 9, 7, 11);
  t3_object_look_at(A.camera, 0, 2, 0);
  t3_light *sun = t3_directional_light_new(0xffffff, 2.82743f);
  t3_object_set_position(sun, 4, 10, 6);
  t3_object_add((t3_object *)A.scene, sun);
  t3_object_add((t3_object *)A.scene, t3_ambient_light_new(0x505868, 3.14159f));
  A.mat_box = t3_mesh_standard_material_new(0xd08a3a);
  A.mat_ball = t3_mesh_standard_material_new(0x4aa3df);
  A.mat_static = t3_mesh_standard_material_new(0x56606e);
  phys_lines_t3_init(&A.lines_t3, (t3_object *)A.scene);
  build_world();

  cw_debug_u32("phys.hash", &A.hash);
  cw_debug_i32("phys.steps", &A.steps);
  cw_debug_i32("phys.bodies", &A.bodies);
  cw_debug_i32("phys.awake", &A.awake);
  cw_debug_i32("phys.contacts", &A.contacts);
  cw_debug_i32("ev.begin", &A.ev_begin);
  cw_debug_i32("ev.end", &A.ev_end);
  cw_debug_i32("ev.hit", &A.ev_hit);
  cw_debug_f32("ev.hit_speed_max", &A.hit_speed_max);
  cw_debug_f32_array("ball.pos", A.ball_pos, 3);
  cw_debug_i32("ray.hits", &A.ray_hits);
  cw_debug_f32("ray.height_sum", &A.ray_height_sum);
  cw_debug_i32("lines.count", &A.line_count);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  if (A.steps == 60) { /* fire */
    cw_body3d_set_transform(A.ball, cw_xform3_at(-10, 1.2f, 0.3f));
    cw_body3d_set_velocity(A.ball, (t3_vec3){ 22, 1.5f, 0 });
  }
  /* #region step */
  cw_world3d_step(A.world, (float)dt);
  int n;
  const cw_phys_event3d *ev = cw_world3d_events(A.world, &n);
  for (int i = 0; i < n; i++) {
    switch (ev[i].type) {
    case CW_PHYS_CONTACT_BEGIN: A.ev_begin++; break;
    case CW_PHYS_CONTACT_END: A.ev_end++; break;
    case CW_PHYS_HIT:
      A.ev_hit++;
      if (ev[i].speed > A.hit_speed_max) A.hit_speed_max = ev[i].speed;
      break;
    default: break;
    }
  }
  /* #endregion */
  cw_phys_stats s = cw_world3d_stats(A.world);
  A.hash = s.hash;
  A.steps = s.steps;
  A.bodies = s.bodies;
  A.awake = s.awake_bodies;
  A.contacts = s.contacts;
  cw_xform3 b = cw_body3d_transform(A.ball);
  A.ball_pos[0] = b.p.x;
  A.ball_pos[1] = b.p.y;
  A.ball_pos[2] = b.p.z;

  /* height profile of the pile: 8 x 8 rays straight down, on the job pool */
  /* #region rays */
  cw_ray3d rays[RAYS];
  cw_ray_hit3d hits[RAYS];
  for (int i = 0; i < RAYS; i++)
    rays[i] = (cw_ray3d){ { -3.5f + (float)(i % 8), 20, -3.5f + (float)(i / 8) }, { 0, -30, 0 } };
  cw_phys_filter f = cw_phys_filter_layers(cw_world3d_layer_bits(A.world, "props"));
  A.ray_hits = cw_world3d_raycast_batch(A.world, rays, RAYS, &f, hits);
  float sum = 0;
  for (int i = 0; i < RAYS; i++)
    if (hits[i].shape.id) sum += hits[i].point.y;
  A.ray_height_sum = sum;
  /* #endregion */
}

/* #region render */
static void render(void *user, double alpha) {
  (void)user;
  cw_world3d_sync(A.world, (float)alpha);
  cw_phys_lines_clear(&A.lines);
  cw_world3d_debug_draw(A.world, &A.lines, CW_PHYS_DRAW_SHAPES | CW_PHYS_DRAW_JOINTS);
  A.line_count = cw_phys_lines_count(&A.lines);
  phys_lines_t3_update(&A.lines_t3, &A.lines);
  cw_render3d_window_target(A.renderer);
  t3_renderer_render(A.renderer, A.scene, A.camera);
}
/* #endregion */

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "physics3d stack", .width = 640, .height = 360, .debug = true, .init = init,
                        .fixed_update = fixed_update, .render = render };
}

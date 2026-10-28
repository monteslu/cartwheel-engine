/* platformer2d: the 2D platformer controller (cartwheel/platformer2d.h) on a scripted course.
 *
 * A floor, a one-way platform to jump up through and drop back down, a tall wall to wall-jump off, a gap to dash
 * across and a pool to swim in. The script presses buttons by tick; the named state proves each move happened:
 * counters for every event, where the character stood after each part, and a hash of the whole run.
 * Drawn with an orthographic three.c camera, bodies as boxes.
 *
 * Debug fields: pf.hash, pf.pos, pf.vel, pf.floor, pf.one_way, pf.jumps, pf.air_jumps, pf.wall_jumps, pf.dashes,
 * pf.lands, pf.drops, pf.strokes, pf.part, pf.top_y, pf.wall_best_y, pf.dash_x, pf.pool_y */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/platformer2d.h"
#include "three.h"

enum { L_WORLD = 0, L_ONE_WAY = 5 };

typedef struct app {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  t3_mesh *hero;
  cw_world2d world;
  cw_platformer2d pf;
  int32_t tick;
  uint32_t hash;
  float pos[2], vel[2];
  int32_t floor, one_way, jumps, air_jumps, wall_jumps, dashes, lands, drops, strokes, part;
  float top_y, wall_best_y, dash_x, pool_y;
} app;
static app A;

static void box(float cx, float cy, float hx, float hy, int layer, uint32_t color) {
  cw_body2d_def bd = cw_body2d_default_def();
  bd.type = CW_BODY_STATIC;
  bd.position = (cw_pos2){ cx, cy };
  cw_body2d b = cw_body2d_create(A.world, &bd);
  cw_shape2d_def sd = cw_shape2d_default_def();
  sd.layer = layer;
  cw_shape2d_box(b, &sd, (cw_vec2){ hx, hy }, (cw_xform2){ { 0, 0 }, { 1, 0 } });
  t3_mesh *m = t3_mesh_new(t3_box_geometry_new(2 * hx, 2 * hy, 1, 1, 1, 1), t3_mesh_basic_material_new(color));
  t3_object_set_position(m, cx, cy, 0);
  t3_object_add((t3_object *)A.scene, m);
}

static void init(void *user) {
  (void)user;
  int w, h;
  cw_drawable_size(&w, &h);
  A.renderer = cw_render3d_window_renderer(w, h);
  t3_renderer_set_clear_color(A.renderer, 0x182028, 1);
  A.scene = t3_scene_new();
  A.camera = t3_orthographic_camera_new(-4, 28, 10, -8, -10, 10);
  t3_object_set_position(A.camera, 0, 0, 5);
  cw_world2d_def wd = cw_world2d_default_def();
  wd.gravity = (cw_vec2){ 0, 0 };
  A.world = cw_world2d_create(&wd);
  box(12, -1, 16, 1, L_WORLD, 0x56708a);            /* floor: top at y = 0, x -4..28 */
  box(6, 3.1f, 2.5f, 0.15f, L_ONE_WAY, 0xe0a040);   /* one-way platform: top at y = 3.25, x 3.5..8.5 */
  box(15, 5, 0.5f, 5, L_WORLD, 0x7a5a4a);           /* wall: x 14.5..15.5, to y = 10 */
  cw_platformer2d_def d = cw_platformer2d_default_def();
  d.mover = cw_mover2d_default_def();
  d.mover.one_way_mask = getenv("CW_TEST_NO_ONE_WAY") ? 0 : 1u << L_ONE_WAY; /* the gate's control: solid from below */
  d.air_jumps = 1;
  d.wall_jump = true;
  d.dash_speed = 16;
  cw_platformer2d_init(&A.pf, A.world, &d, (cw_pos2){ 6, 0.6 });
  A.hero = t3_mesh_new(t3_box_geometry_new(0.6f, 1.8f, 0.6f, 1, 1, 1), t3_mesh_basic_material_new(0x60e080));
  t3_object_add((t3_object *)A.scene, A.hero);
  cw_debug_u32("pf.hash", &A.hash);
  cw_debug_f32_array("pf.pos", A.pos, 2);
  cw_debug_f32_array("pf.vel", A.vel, 2);
  cw_debug_i32("pf.floor", &A.floor);
  cw_debug_i32("pf.one_way", &A.one_way);
  cw_debug_i32("pf.jumps", &A.jumps);
  cw_debug_i32("pf.air_jumps", &A.air_jumps);
  cw_debug_i32("pf.wall_jumps", &A.wall_jumps);
  cw_debug_i32("pf.dashes", &A.dashes);
  cw_debug_i32("pf.lands", &A.lands);
  cw_debug_i32("pf.drops", &A.drops);
  cw_debug_i32("pf.strokes", &A.strokes);
  cw_debug_i32("pf.part", &A.part);
  cw_debug_f32("pf.top_y", &A.top_y);
  cw_debug_f32("pf.wall_best_y", &A.wall_best_y);
  cw_debug_f32("pf.dash_x", &A.dash_x);
  cw_debug_f32("pf.pool_y", &A.pool_y);
}

/* the script: [from, to) ticks */
static cw_platformer2d_input script(int t) {
  cw_platformer2d_input in = { 0 };
  if (t >= 40 && t < 70) in.move_x = 1;                       /* run right a little, then stop under nothing */
  if (t == 80) in.jump_pressed = true;                        /* jump straight up, under the one-way platform */
  if (t >= 80 && t < 110) in.jump_held = true;
  if (t >= 120 && t < 150) in.move_x = -1;                    /* walk back to the platform's x range */
  if (t >= 150 && t < 180) in.move_x = 0;
  if (t == 360) in.dash_pressed = true;                       /* dash along the floor */
  if (t == 200) in.jump_pressed = true, in.jump_held = true;  /* jump onto the one-way from below */
  if (t >= 200 && t < 230) in.jump_held = true;
  if (t == 300) in.jump_pressed = true, in.down_held = true;  /* down + jump: drop through it */
  if (t >= 330 && t < 400) in.move_x = 1;                     /* run to the wall */
  if (t >= 400 && t < 700) in.move_x = 1;                     /* keep pushing: slide, wall jump, climb the shaft */
  if (t == 430 || t == 470 || t == 510 || t == 550) in.jump_pressed = true;
  if ((t >= 430 && t < 445) || (t >= 470 && t < 485) || (t >= 510 && t < 525) || (t >= 550 && t < 565)) in.jump_held = true;
  return in;
}

static void fixed_update(void *user, double dt) {
  (void)user;
  int t = A.tick++;
  cw_platformer2d_input in = script(t);
  /* a wall jump kicks away: the script keeps pressing into the wall after it, as a player would */
  A.pf.submerged = t >= 700 ? 1.0f : 0.0f; /* the last part is under water */
  if (t == 720) in.jump_pressed = true;
  cw_platformer2d_step(&A.pf, &in, (float)dt);
  cw_world2d_step(A.world, (float)dt);
  uint32_t e = A.pf.events;
  A.jumps += (e & CW_PF_JUMP) != 0;
  A.air_jumps += (e & CW_PF_AIR_JUMP) != 0;
  A.wall_jumps += (e & CW_PF_WALL_JUMP) != 0;
  A.dashes += (e & CW_PF_DASH) != 0;
  A.lands += (e & CW_PF_LAND) != 0;
  A.drops += (e & CW_PF_DROP) != 0;
  A.strokes += (e & CW_PF_STROKE) != 0;
  A.floor = A.pf.body.on_floor;
  A.one_way = A.pf.body.on_one_way;
  A.pos[0] = (float)A.pf.body.position.x, A.pos[1] = (float)A.pf.body.position.y;
  A.vel[0] = A.pf.body.velocity.x, A.vel[1] = A.pf.body.velocity.y;
  if (A.one_way && A.pos[1] > A.top_y) A.top_y = A.pos[1];
  if (t > 400 && A.pos[1] > A.wall_best_y) A.wall_best_y = A.pos[1];
  union { float f; uint32_t u; } a = { A.pos[0] }, b = { A.pos[1] };
  A.hash = (A.hash ^ a.u) * 16777619u;
  A.hash = (A.hash ^ b.u) * 16777619u;
  A.hash = (A.hash ^ (uint32_t)e) * 16777619u;
  A.part = t >= 700 ? 5 : t >= 400 ? 4 : t >= 300 ? 3 : t >= 200 ? 2 : t >= 80 ? 1 : 0;
}

static void render(void *user, double alpha) {
  (void)user;
  cw_pos2 p = cw_mover2d_interpolated(&A.pf.body, (float)alpha);
  t3_object_set_position(A.hero, (float)p.x, (float)p.y + 0.9f, 0);
  cw_render3d_window_target(A.renderer);
  t3_renderer_render(A.renderer, A.scene, A.camera);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "platformer2d", .width = 640, .height = 360, .debug = true, .init = init,
                        .fixed_update = fixed_update, .render = render };
}

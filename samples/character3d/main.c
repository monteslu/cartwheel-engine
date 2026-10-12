/* character3d: a CharacterBody3D-style course on the Box3D mover
 * (cw_mover3d, cartwheel/physics3d.h), driven by pad 0.
 *
 * The course, along +x: stairs (5 x 0.2 m) up to a plateau, a drop, a low
 * bar to bump the head on, a 25 degree ramp, a kinematic platform sliding
 * along z (ride it), a crate to push, and a 60 degree slope the mover must
 * NOT climb. Coins (sensor spheres) are picked up with an overlap query
 * that asks for sensors; picked coins are destroyed.
 *
 * Input: left stick moves (x right, y toward the camera), South jumps.
 * Axis values in input scripts should be -1, 0 or 1: those survive every
 * host's 16-bit stick encoding exactly. Built twice: `character3d` reads the
 * pad (gate: tests/character3d-input.json), `character3d_auto` plays the
 * same script from the tick count (input-free, so wasmcart-native can run
 * it too).
 *
 * Debug fields: mover.pos, mover.vel, mover.floor, mover.wall, mover.ceiling,
 * count.steps, count.snaps, count.ceiling, count.jumps, count.coins,
 * ride.dz, crate.pos, mover.max_y, mover.hash (every tick's position and
 * flags folded), phys.hash */
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics.h"
#include "cartwheel/platform.h"
#include "three.h"

#define COINS 5

typedef struct app {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  t3_material *mat_static, *mat_dyn, *mat_player, *mat_coin;
  t3_mesh *player_mesh, *coin_mesh[COINS];
  cw_world3d world;
  cw_body3d platform, crate, coin[COINS];
  cw_mover3d mover;
  uint32_t prev_buttons; /* the autopilot's buttons last tick: its jump edge */
  int32_t tick;
  float ride_z0;
  /* debug fields */
  float pos[3], vel[3], max_y, ride_dz, crate_pos[3];
  int32_t on_floor, on_wall, on_ceiling, steps, snaps, ceiling, jumps, coins;
  uint32_t mover_hash, phys_hash;
} app;

static app A;

#ifdef CHARACTER_AUTOPILOT
/* the same script as tests/character3d-input.json: {frame, buttons, lx, ly} */
static const struct { int frame; uint32_t buttons; float lx, ly; } SCRIPT[] = {
#include "script.inc"
};
#endif

static t3_mesh *add(t3_geometry *g, t3_material *m) {
  t3_mesh *mesh = t3_mesh_new(g, m);
  t3_release(g);
  t3_object_add((t3_object *)A.scene, mesh);
  t3_release(mesh);
  return mesh;
}

/* a static box with a matching mesh */
static void block(cw_body3d ground, float cx, float cy, float cz, float hx, float hy, float hz, t3_quat q) {
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_box(ground, &sd, (t3_vec3){ hx, hy, hz }, (cw_xform3){ { cx, cy, cz }, q });
  t3_mesh *m = add(t3_box_geometry_new(2 * hx, 2 * hy, 2 * hz, 1, 1, 1), A.mat_static);
  t3_object_set_position(m, cx, cy, cz);
  t3_object_set_quaternion(m, q);
}

static t3_quat about_z(float radians) {
  float s, c;
  cw_sincos(radians * 0.5f, &s, &c);
  return (t3_quat){ 0, 0, s, c };
}

static void build(void) {
  cw_world3d_def wd = cw_world3d_default_def();
  wd.layers[0] = "world";
  wd.layers[1] = "coins";
  A.world = cw_world3d_create(&wd);
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body3d ground = cw_body3d_create(A.world, &bd);
  const t3_quat id = { 0, 0, 0, 1 };

  block(ground, 15, -0.5f, 0, 30, 0.5f, 10, id);                     /* floor, top y = 0 */
  for (int i = 0; i < 5; i++) {                                       /* stairs x 3..5.5, then a plateau to x = 9 */
    float x0 = 3 + 0.5f * (float)i, top = 0.2f * (float)(i + 1);
    block(ground, (x0 + 9) * 0.5f, top * 0.5f, 0, (9 - x0) * 0.5f, top * 0.5f, 2, id);
  }
  block(ground, 12.5f, 2.3f, 0, 0.3f, 0.1f, 3, id);                   /* the low bar over x = 12.5 */
  block(ground, 17, -0.6f, 0, 2.6f, 1.2f, 2, about_z(25 * CW_PI / 180)); /* 25 degree ramp, top near x = 19 */
  block(ground, 34, 0, 0, 3, 1.5f, 3, about_z(60 * CW_PI / 180));     /* 60 degree slope from x = 32.3 */

  /* kinematic platform sliding along z at x = 24 */
  bd.type = CW_BODY_KINEMATIC;
  bd.position = (t3_vec3){ 24, 0.125f, 0 };
  A.platform = cw_body3d_create(A.world, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_box(A.platform, &sd, (t3_vec3){ 1.5f, 0.125f, 1.5f }, cw_xform3_identity());
  cw_body3d_bind_object(A.platform, (t3_object *)add(t3_box_geometry_new(3, 0.25f, 3, 1, 1, 1), A.mat_dyn));

  /* a crate to push */
  bd.type = CW_BODY_DYNAMIC;
  bd.position = (t3_vec3){ 29, 0.5f, 0 };
  A.crate = cw_body3d_create(A.world, &bd);
  cw_shape3d_box(A.crate, &sd, (t3_vec3){ 0.5f, 0.5f, 0.5f }, cw_xform3_identity());
  cw_body3d_bind_object(A.crate, (t3_object *)add(t3_box_geometry_new(1, 1, 1, 1, 1, 1), A.mat_dyn));

  /* coins: sensors on their own layer */
  static const t3_vec3 at[COINS] = { { 2, 0.9f, 0 }, { 7, 1.9f, 0 }, { 12, 0.9f, 0 }, { 18, 2.6f, 0 }, { 26.5f, 0.9f, 0 } };
  bd.type = CW_BODY_STATIC;
  cw_shape3d_def cs = cw_shape3d_default_def();
  cs.sensor = true;
  cs.layer = 1;
  for (int i = 0; i < COINS; i++) {
    bd.position = at[i];
    A.coin[i] = cw_body3d_create(A.world, &bd);
    cw_shape3d_sphere(A.coin[i], &cs, (t3_vec3){ 0, 0, 0 }, 0.3f);
    A.coin_mesh[i] = add(t3_torus_geometry_new(0.25f, 0.07f, 8, 16, CW_TAU), A.mat_coin);
    t3_object_set_position(A.coin_mesh[i], at[i].x, at[i].y, at[i].z);
  }

  cw_mover3d_def md = cw_mover3d_default_def();
  md.push_mass = 80;
  cw_mover3d_init(&A.mover, A.world, &md, (t3_vec3){ 0, 0.5f, 0 });
}

static cw_action jump_action; /* pad South: just_pressed keeps a tap shorter than a tick */

static void init(void *user) {
  (void)user;
  int w, h;
  cw_drawable_size(&w, &h);
  jump_action = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump_action, "pad:south");
  A.renderer = t3_renderer_new(w, h);
  t3_renderer_set_clear_color(A.renderer, 0x87a8c8, 1);
  A.scene = t3_scene_new();
  A.camera = t3_perspective_camera_new(60, (float)w / (float)h, 0.1f, 200);
  t3_light *sun = t3_directional_light_new(0xffffff, 2.82743f);
  t3_object_set_position(sun, 5, 10, 7);
  t3_object_add((t3_object *)A.scene, sun);
  t3_object_add((t3_object *)A.scene, t3_ambient_light_new(0x606060, 3.14159f));
  A.mat_static = t3_mesh_standard_material_new(0x8c8f99);
  A.mat_dyn = t3_mesh_standard_material_new(0xc98a4b);
  A.mat_player = t3_mesh_standard_material_new(0x3a7bd5);
  A.mat_coin = t3_mesh_standard_material_new(0xf2c94c);
  A.player_mesh = add(t3_cylinder_geometry_new(0.3f, 0.3f, 1.8f, 16, 1, false), A.mat_player);
  build();
  A.mover_hash = CW_HASH32_INIT;
  cw_debug_f32_array("mover.pos", A.pos, 3);
  cw_debug_f32_array("mover.vel", A.vel, 3);
  cw_debug_i32("mover.floor", &A.on_floor);
  cw_debug_i32("mover.wall", &A.on_wall);
  cw_debug_i32("mover.ceiling", &A.on_ceiling);
  cw_debug_f32("mover.max_y", &A.max_y);
  cw_debug_i32("count.steps", &A.steps);
  cw_debug_i32("count.snaps", &A.snaps);
  cw_debug_i32("count.ceiling", &A.ceiling);
  cw_debug_i32("count.jumps", &A.jumps);
  cw_debug_i32("count.coins", &A.coins);
  cw_debug_f32("ride.dz", &A.ride_dz);
  cw_debug_f32_array("crate.pos", A.crate_pos, 3);
  cw_debug_u32("mover.hash", &A.mover_hash);
  cw_debug_u32("phys.hash", &A.phys_hash);
}

/* the stick, and whether jump went down this tick */
static bool read_input(float *lx, float *ly) {
#ifdef CHARACTER_AUTOPILOT
  int frame = A.tick + 1; /* the script's 1-based engine frame */
  uint32_t buttons = 0;
  *lx = *ly = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(SCRIPT) && SCRIPT[i].frame <= frame; i++) {
    *lx = SCRIPT[i].lx;
    *ly = SCRIPT[i].ly;
    buttons = SCRIPT[i].buttons;
  }
  bool jump = (buttons & (1u << CW_PAD_SOUTH)) && !(A.prev_buttons & (1u << CW_PAD_SOUTH));
  A.prev_buttons = buttons;
  return jump;
#else
  const cw_pad_state *p = &cw_input_raw()->pads[0];
  *lx = p->axes[CW_AXIS_LX];
  *ly = p->axes[CW_AXIS_LY];
  return cw_action_just_pressed(0, jump_action);
#endif
}

static void fixed_update(void *user, double dt) {
  (void)user;
  float fdt = (float)dt;
  cw_mover3d *m = &A.mover;
  float lx, ly;
  bool jump = read_input(&lx, &ly);

  /* the platform slides along z; drive it before the mover moves */
  float pz = A.tick < 420 ? 0 : 1.5f * cw_sin((float)(A.tick - 420) * 0.02f);
  cw_body3d_set_target(A.platform, cw_xform3_at(24, 0.125f, pz), fdt);

  const float speed = 4, accel = 30, gravity = 18;
  t3_vec3 want = { lx * speed, 0, ly * speed };
  float k = accel * fdt;
  m->velocity.x += CW_CLAMP(want.x - m->velocity.x, -k, k);
  m->velocity.z += CW_CLAMP(want.z - m->velocity.z, -k, k);
  if (jump && m->on_floor) {
    m->velocity.y = 6.5f;
    A.jumps++;
  }
  m->velocity.y -= gravity * fdt;
  bool riding = m->on_floor && m->floor_body.id == A.platform.id;
  float z0 = m->position.z;
  cw_mover3d_move(m, fdt);
  if (riding) A.ride_dz += CW_MAX(m->position.z - z0, z0 - m->position.z); /* travel while riding */
  A.steps += m->stepped;
  A.snaps += m->snapped;
  A.ceiling += m->on_ceiling;
  cw_world3d_step(A.world, fdt);

  /* coins: an overlap query against the capsule that asks for sensors */
  cw_qshape3d q = cw_qshape3d_capsule((t3_vec3){ m->position.x, m->position.y + 0.3f, m->position.z },
                                      (t3_vec3){ m->position.x, m->position.y + 1.5f, m->position.z }, 0.3f);
  cw_phys_filter f = cw_phys_filter_layers(cw_world3d_layer_bits(A.world, "coins"));
  f.sensors = true;
  cw_shape3d hit[4];
  int n = cw_world3d_overlap(A.world, &q, &f, hit, 4);
  for (int i = 0; i < n; i++)
    for (int c = 0; c < COINS; c++)
      if (cw_body3d_valid(A.coin[c]) && cw_shape3d_body(hit[i]).id == A.coin[c].id) {
        cw_body3d_destroy(A.coin[c]);
        T3_OBJ(A.coin_mesh[c])->visible = false;
        A.coins++;
      }

  A.pos[0] = m->position.x;
  A.pos[1] = m->position.y;
  A.pos[2] = m->position.z;
  A.vel[0] = m->velocity.x;
  A.vel[1] = m->velocity.y;
  A.vel[2] = m->velocity.z;
  A.on_floor = m->on_floor;
  A.on_wall = m->on_wall;
  A.on_ceiling = m->on_ceiling;
  if (m->position.y > A.max_y) A.max_y = m->position.y;
  uint32_t flags = (uint32_t)m->on_floor | (uint32_t)m->on_wall << 1 | (uint32_t)m->on_ceiling << 2;
  A.mover_hash = cw_hash32(A.pos, sizeof A.pos, A.mover_hash);
  A.mover_hash = cw_hash32(&flags, sizeof flags, A.mover_hash);
  cw_xform3 c = cw_body3d_transform(A.crate);
  A.crate_pos[0] = c.p.x;
  A.crate_pos[1] = c.p.y;
  A.crate_pos[2] = c.p.z;
  A.phys_hash = cw_world3d_stats(A.world).hash;
  A.tick++;
}

static void render(void *user, double alpha) {
  (void)user;
  cw_world3d_sync(A.world, (float)alpha);
  t3_vec3 p = cw_mover3d_interpolated(&A.mover, (float)alpha);
  t3_object_set_position(A.player_mesh, p.x, p.y + 0.9f, p.z);
  for (int c = 0; c < COINS; c++) t3_object_set_rotation(A.coin_mesh[c], 0, (float)A.tick * 0.05f, 0);
  t3_object_set_position(A.camera, p.x - 4, p.y + 3.5f, p.z + 7);
  t3_object_look_at(A.camera, p.x + 1, p.y + 0.8f, p.z);
  t3_renderer_render(A.renderer, A.scene, A.camera);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "character3d", .width = 640, .height = 360, .debug = true, .init = init,
                        .fixed_update = fixed_update, .render = render };
}

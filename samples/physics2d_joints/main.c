/* physics2d_joints: the 2D physics module end to end (Box2D v3).
 *
 * One scene with every joint kind and the event kinds that matter:
 *   - a rope bridge: 12 planks on revolute joints between two posts;
 *   - a car: chassis + two wheels on wheel joints (spring suspension, spin
 *     motor) driving over a chain-shape bump and through a sensor zone;
 *   - a pendulum on a distance joint, a piston on a prismatic joint whose
 *     motor reverses every 90 ticks, two boxes welded into an L, a filter
 *     joint, a motor joint holding a box in place;
 *   - a weight hanging from a BREAKABLE revolute joint, kicked at tick 120
 *     so it drops onto the bridge;
 *   - a mouse joint dragging a crate along a scripted path (ticks 30-150);
 *   - a kinematic platform moving left/right with a mover2d character
 *     standing on it (it must ride the platform, not slide off).
 * Rendered with an orthographic three.c camera: bodies bound to meshes
 * (interpolated), joints and shapes from the debug draw as lines.
 *
 * Debug fields: phys.hash, phys.steps, phys.bodies, phys.joints, ev.begin,
 * ev.end, ev.hit, ev.sensor_enter, ev.sensor_exit, ev.joint_break,
 * car.x, pendulum.len, piston.t, revolute.angle, mover.pos, mover.floor,
 * crate.pos, lines.count */
#include "../common/phys_lines_t3.h"
#include "cartwheel/debug.h"
#include "cartwheel/physics.h"
#include "cartwheel/platform.h"
#include "three.h"

typedef struct app {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  phys_lines_t3 lines_t3;
  cw_phys_lines lines;
  t3_material *mat_dyn, *mat_static, *mat_car, *mat_sensor;
  cw_world2d world;
  cw_body2d chassis, crate, weight, platform, pend_ball;
  cw_joint2d piston, pendulum, mouse, hinge, bridge_first;
  cw_shape2d sensor;
  cw_mover2d mover;
  t3_mesh *mover_mesh;
  int32_t tick;
  /* debug fields */
  uint32_t hash;
  int32_t steps, bodies, joints, ev_begin, ev_end, ev_hit, ev_enter, ev_exit, ev_break, mover_floor, line_count;
  float car_x, pend_len, piston_t, rev_angle, mover_pos[2], crate_pos[2];
} app;

static app A;

static t3_mesh *mesh_box(float hx, float hy, t3_material *m) {
  t3_mesh *mesh = t3_mesh_new(t3_box_geometry_new(2 * hx, 2 * hy, 0.2f, 1, 1, 1), m);
  t3_release(mesh->geometry);
  t3_object_add((t3_object *)A.scene, mesh);
  t3_release(mesh);
  return mesh;
}
static t3_mesh *mesh_disc(float r, t3_material *m) {
  t3_mesh *mesh = t3_mesh_new(t3_circle_geometry_new(r, 24), m);
  t3_release(mesh->geometry);
  t3_object_add((t3_object *)A.scene, mesh);
  t3_release(mesh);
  return mesh;
}

static cw_body2d box_body(cw_body_type type, float x, float y, float hx, float hy, const cw_shape2d_def *sd,
                          t3_material *m) {
  cw_body2d_def bd = cw_body2d_default_def();
  bd.type = type;
  bd.position = (cw_vec2){ x, y };
  cw_body2d b = cw_body2d_create(A.world, &bd);
  cw_shape2d_box(b, sd, (cw_vec2){ hx, hy }, cw_xform2_identity());
  if (m) cw_body2d_bind_object(b, (t3_object *)mesh_box(hx, hy, m));
  return b;
}

static void build(void) {
  cw_world2d_def wd = cw_world2d_default_def();
  wd.layers[0] = "world";
  wd.layers[1] = "props";
  wd.layers[2] = "zones";
  A.world = cw_world2d_create(&wd);
  cw_shape2d_def sd = cw_shape2d_default_def();
  cw_shape2d_def props = sd;
  props.layer = 1;
  props.hit_events = true;

  /* ground, and a chain bump the car drives over */
  cw_body2d ground = box_body(CW_BODY_STATIC, 0, -0.5f, 16, 0.5f, &sd, A.mat_static);
  static const cw_vec2 bump[6] = { { 9, 0 }, { 8, 0 }, { 6.5f, 0.35f }, { 5.5f, 0.35f }, { 4, 0 }, { 3, 0 } };
  cw_body2d_def gd = cw_body2d_default_def();
  gd.type = CW_BODY_STATIC;
  cw_chain2d_create(cw_body2d_create(A.world, &gd), &sd, bump, 6, false);

  /* rope bridge between two posts */
  box_body(CW_BODY_STATIC, -14.5f, 2, 0.5f, 2, &sd, A.mat_static);
  cw_body2d right_post = box_body(CW_BODY_STATIC, -3.5f, 2, 0.5f, 2, &sd, A.mat_static);
  (void)right_post;
  cw_body2d prev = (cw_body2d){ 0 };
  const int planks = 12;
  const float x0 = -14, x1 = -4, plank = (x1 - x0) / planks;
  for (int i = 0; i <= planks; i++) {
    cw_body2d cur = { 0 };
    if (i < planks) cur = box_body(CW_BODY_DYNAMIC, x0 + plank * (i + 0.5f), 4, plank * 0.5f, 0.08f, &props, A.mat_dyn);
    cw_joint2d_def jd = cw_joint2d_default_def(CW_JOINT_REVOLUTE);
    jd.a = prev; /* null = the world at the left post */
    jd.b = i < planks ? cur : prev;
    if (i == planks) jd.a = (cw_body2d){ 0 };
    cw_joint2d_def_anchor(&jd, (cw_vec2){ x0 + plank * i, 4 }, (cw_vec2){ 1, 0 });
    cw_joint2d j = cw_joint2d_create(A.world, &jd);
    if (i == 0) A.bridge_first = j;
    prev = cur;
  }
  /* something to sag the bridge */
  box_body(CW_BODY_DYNAMIC, -9, 6, 0.4f, 0.4f, &props, A.mat_dyn);

  /* the car */
  A.chassis = box_body(CW_BODY_DYNAMIC, -1, 1.2f, 1.1f, 0.3f, &props, A.mat_car);
  for (int s = -1; s <= 1; s += 2) {
    cw_body2d_def bd = cw_body2d_default_def();
    bd.position = (cw_vec2){ -1 + 0.75f * s, 0.6f };
    cw_body2d wheel = cw_body2d_create(A.world, &bd);
    cw_shape2d_def ws = props;
    ws.material.friction = 0.9f;
    ws.material.rolling_resistance = 0.02f;
    cw_shape2d_circle(wheel, &ws, (cw_vec2){ 0, 0 }, 0.4f);
    cw_body2d_bind_object(wheel, (t3_object *)mesh_disc(0.4f, A.mat_car));
    cw_joint2d_def jd = cw_joint2d_default_def(CW_JOINT_WHEEL);
    jd.a = A.chassis;
    jd.b = wheel;
    cw_joint2d_def_anchor(&jd, bd.position, (cw_vec2){ 0, 1 }); /* suspension travels along y */
    jd.spring = true;
    jd.hertz = 4;
    jd.damping_ratio = 0.7f;
    jd.limit = true;
    jd.lower = -0.25f;
    jd.upper = 0.25f;
    jd.motor = true;
    jd.motor_speed = -6; /* clockwise: drives right */
    jd.max_motor = 2.5f;
    cw_joint2d_create(A.world, &jd);
  }

  /* sensor zone on the car's path */
  cw_body2d_def zd = cw_body2d_default_def();
  zd.type = CW_BODY_STATIC;
  zd.position = (cw_vec2){ 11, 1 };
  cw_body2d zone = cw_body2d_create(A.world, &zd);
  cw_shape2d_def ss = sd;
  ss.sensor = true;
  ss.layer = 2;
  A.sensor = cw_shape2d_box(zone, &ss, (cw_vec2){ 1, 1 }, cw_xform2_identity());
  t3_object_set_position(mesh_box(1, 1, A.mat_sensor), 11, 1, -0.3f);

  /* pendulum: a distance joint from the world */
  cw_body2d_def pd = cw_body2d_default_def();
  pd.position = (cw_vec2){ 6.5f, 13 }; /* pulled aside: it swings */
  A.pend_ball = cw_body2d_create(A.world, &pd);
  cw_shape2d_circle(A.pend_ball, &props, (cw_vec2){ 0, 0 }, 0.35f);
  cw_body2d_bind_object(A.pend_ball, (t3_object *)mesh_disc(0.35f, A.mat_dyn));
  cw_joint2d_def dj = cw_joint2d_default_def(CW_JOINT_DISTANCE);
  dj.b = A.pend_ball;
  dj.frame_a = cw_xform2_at(4, 15); /* the world anchor (a = null: the world) */
  dj.frame_b = cw_xform2_identity();
  A.pendulum = cw_joint2d_create(A.world, &dj);

  /* piston: prismatic + motor, pushing a box */
  cw_body2d piston = box_body(CW_BODY_DYNAMIC, 13, 0.4f, 0.4f, 0.3f, &props, A.mat_car);
  cw_joint2d_def pj = cw_joint2d_default_def(CW_JOINT_PRISMATIC);
  pj.b = piston;
  cw_joint2d_def_anchor(&pj, (cw_vec2){ 13, 0.4f }, (cw_vec2){ -1, 0 });
  pj.limit = true;
  pj.lower = 0;
  pj.upper = 2.5f;
  pj.motor = true;
  pj.motor_speed = 2;
  pj.max_motor = 200;
  A.piston = cw_joint2d_create(A.world, &pj);
  box_body(CW_BODY_DYNAMIC, 11.8f, 0.3f, 0.3f, 0.3f, &props, A.mat_dyn);

  /* welded L, dropped */
  cw_body2d l1 = box_body(CW_BODY_DYNAMIC, 7, 8, 0.8f, 0.15f, &props, A.mat_dyn);
  cw_body2d l2 = box_body(CW_BODY_DYNAMIC, 7.65f, 8.65f, 0.15f, 0.5f, &props, A.mat_dyn);
  cw_joint2d_def wj = cw_joint2d_default_def(CW_JOINT_WELD);
  wj.a = l1;
  wj.b = l2;
  cw_joint2d_def_anchor(&wj, (cw_vec2){ 7.65f, 8 }, (cw_vec2){ 1, 0 });
  cw_joint2d_create(A.world, &wj);
  /* filter joint: the L passes through this box */
  cw_body2d ghost = box_body(CW_BODY_DYNAMIC, 7.2f, 5, 0.5f, 0.5f, &props, A.mat_sensor);
  cw_joint2d_def fj = cw_joint2d_default_def(CW_JOINT_FILTER);
  fj.a = l1;
  fj.b = ghost;
  cw_joint2d_create(A.world, &fj);

  /* motor joint: a box held in mid air relative to the world */
  cw_body2d held = box_body(CW_BODY_DYNAMIC, -1, 9, 0.5f, 0.5f, &props, A.mat_car);
  cw_joint2d_def mj = cw_joint2d_default_def(CW_JOINT_MOTOR);
  mj.b = held;
  mj.frame_a = cw_xform2_at(-1, 9);
  mj.max_force = 500;
  cw_joint2d_create(A.world, &mj);

  /* breakable hinge: a weight hanging from the world */
  A.weight = box_body(CW_BODY_DYNAMIC, -11, 8, 0.4f, 0.4f, &props, A.mat_dyn);
  cw_joint2d_def hj = cw_joint2d_default_def(CW_JOINT_REVOLUTE);
  hj.b = A.weight;
  cw_joint2d_def_anchor(&hj, (cw_vec2){ -11, 8.6f }, (cw_vec2){ 1, 0 });
  hj.break_force = 3 * cw_body2d_mass(A.weight) * 10;
  hj.user = &A.weight;
  A.hinge = cw_joint2d_create(A.world, &hj);

  /* the crate the mouse joint drags */
  A.crate = box_body(CW_BODY_DYNAMIC, -6, 0.5f, 0.5f, 0.5f, &props, A.mat_car);

  /* kinematic platform with a character standing on it */
  A.platform = box_body(CW_BODY_KINEMATIC, -6, 11, 1.5f, 0.2f, &sd, A.mat_static);
  cw_mover2d_def md = cw_mover2d_default_def();
  md.radius = 0.25f;
  md.height = 1;
  cw_mover2d_init(&A.mover, A.world, &md, (cw_vec2){ -6, 11.25f });
  A.mover_mesh = mesh_box(0.25f, 0.5f, A.mat_car);
}

static void init(void *user) {
  (void)user;
  int w, h;
  cw_drawable_size(&w, &h);
  A.renderer = t3_renderer_new(w, h);
  t3_renderer_set_clear_color(A.renderer, 0x202a33, 1);
  A.scene = t3_scene_new();
  A.camera = t3_orthographic_camera_new(-16, 16, 16.5f, -1.5f, -10, 10);
  t3_object_set_position(A.camera, 0, 0, 5);
  A.mat_dyn = t3_mesh_basic_material_new(0xe0a040);
  A.mat_static = t3_mesh_basic_material_new(0x5a6676);
  A.mat_car = t3_mesh_basic_material_new(0x4aa3df);
  A.mat_sensor = t3_mesh_basic_material_new(0x2f5d3a);
  phys_lines_t3_init(&A.lines_t3, (t3_object *)A.scene);
  build();
  cw_debug_u32("phys.hash", &A.hash);
  cw_debug_i32("phys.steps", &A.steps);
  cw_debug_i32("phys.bodies", &A.bodies);
  cw_debug_i32("phys.joints", &A.joints);
  cw_debug_i32("ev.begin", &A.ev_begin);
  cw_debug_i32("ev.end", &A.ev_end);
  cw_debug_i32("ev.hit", &A.ev_hit);
  cw_debug_i32("ev.sensor_enter", &A.ev_enter);
  cw_debug_i32("ev.sensor_exit", &A.ev_exit);
  cw_debug_i32("ev.joint_break", &A.ev_break);
  cw_debug_f32("car.x", &A.car_x);
  cw_debug_f32("pendulum.len", &A.pend_len);
  cw_debug_f32("piston.t", &A.piston_t);
  cw_debug_f32("revolute.angle", &A.rev_angle);
  cw_debug_f32_array("mover.pos", A.mover_pos, 2);
  cw_debug_i32("mover.floor", &A.mover_floor);
  cw_debug_f32_array("crate.pos", A.crate_pos, 2);
  cw_debug_i32("lines.count", &A.line_count);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  float fdt = (float)dt;
  int t = A.tick++;
  /* scripted drivers (all from the tick count: deterministic) */
  if (t % 90 == 0) cw_joint2d_set_motor(A.piston, true, (t / 90) % 2 ? -2.0f : 2.0f, 200);
  if (t == 30) {
    cw_joint2d_def md = cw_joint2d_default_def(CW_JOINT_MOUSE);
    md.b = A.crate;
    md.target = cw_body2d_transform(A.crate).p;
    A.mouse = cw_joint2d_create(A.world, &md);
  }
  if (t > 30 && t < 150) {
    float s = (float)(t - 30) / 120.0f;
    cw_joint2d_set_target(A.mouse, (cw_vec2){ -6 + 4 * s, 0.5f + 3 * cw_sin(s * CW_PI) });
  }
  if (t == 150) cw_joint2d_destroy(A.mouse);
  if (t == 120) cw_body2d_apply_impulse(A.weight, (cw_vec2){ 0, -40 }, NULL);
  /* the platform swings left and right; the mover rides it */
  float px = -6 + 3 * cw_sin((float)t * 0.03f);
  cw_body2d_set_target(A.platform, (cw_xform2){ { px, 11 }, { 1, 0 } }, fdt);
  A.mover.velocity.y -= 10 * fdt;
  cw_mover2d_move(&A.mover, fdt);

  cw_world2d_step(A.world, fdt);
  int n;
  const cw_phys_event2d *ev = cw_world2d_events(A.world, &n);
  for (int i = 0; i < n; i++) {
    switch (ev[i].type) {
    case CW_PHYS_CONTACT_BEGIN: A.ev_begin++; break;
    case CW_PHYS_CONTACT_END: A.ev_end++; break;
    case CW_PHYS_HIT: A.ev_hit++; break;
    case CW_PHYS_SENSOR_ENTER: A.ev_enter += ev[i].shape_a.id == A.sensor.id; break;
    case CW_PHYS_SENSOR_EXIT: A.ev_exit += ev[i].shape_a.id == A.sensor.id; break;
    case CW_PHYS_JOINT_BREAK: A.ev_break += ev[i].user == &A.weight; break;
    default: break;
    }
  }
  cw_phys_stats s = cw_world2d_stats(A.world);
  A.hash = s.hash;
  A.steps = s.steps;
  A.bodies = s.bodies;
  A.joints = s.joints;
  A.car_x = cw_body2d_transform(A.chassis).p.x;
  A.pend_len = cw_joint2d_value(A.pendulum);
  A.piston_t = cw_joint2d_value(A.piston);
  A.rev_angle = cw_joint2d_value(A.bridge_first);
  A.mover_pos[0] = A.mover.position.x;
  A.mover_pos[1] = A.mover.position.y;
  A.mover_floor = A.mover.on_floor;
  cw_vec2 c = cw_body2d_transform(A.crate).p;
  A.crate_pos[0] = c.x;
  A.crate_pos[1] = c.y;
}

static void render(void *user, double alpha) {
  (void)user;
  cw_world2d_sync(A.world, (float)alpha);
  cw_vec2 mp = cw_mover2d_interpolated(&A.mover, (float)alpha);
  t3_object_set_position(A.mover_mesh, mp.x, mp.y + 0.5f, 0);
  cw_phys_lines_clear(&A.lines);
  cw_world2d_debug_draw(A.world, &A.lines, CW_PHYS_DRAW_SHAPES | CW_PHYS_DRAW_JOINTS);
  A.line_count = cw_phys_lines_count(&A.lines);
  phys_lines_t3_update(&A.lines_t3, &A.lines);
  t3_renderer_render(A.renderer, A.scene, A.camera);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "physics2d joints", .width = 640, .height = 360, .debug = true, .init = init,
                        .fixed_update = fixed_update, .render = render };
}

/* hello: one SDL2 + GLES3 program, built natively and as a cart
 * from the same source: a three.c cube that spins at a fixed rate, moves with
 * the left stick / arrow keys, and a 440 Hz tone while South / Space is held.
 * The cube's state is a pure function of the tick count and input, so native
 * and cart runs of the same input script render the same frames. */
#include <math.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "three.h"

typedef struct {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  t3_mesh *cube;
  float x, angle;
  float prev_x, prev_angle;
  volatile int tone;
  double phase;
  int32_t ticks;
  cw_action left, right, tone_on;
} hello;

static hello app;

static void fill(void *user, float *out, int frames) {
  hello *h = user;
  for (int i = 0; i < frames; i++) {
    float s = h->tone ? 0.15f * (float)sin(h->phase) : 0.0f;
    h->phase += 2.0 * M_PI * 440.0 / cw_audio_rate();
    if (h->phase > 2.0 * M_PI) h->phase -= 2.0 * M_PI;
    out[2 * i] = out[2 * i + 1] = s;
  }
}

static void init(void *user) {
  hello *h = user;
  int w, hgt;
  cw_drawable_size(&w, &hgt);
  /* #region three_scene */
  h->renderer = cw_render3d_window_renderer(w, hgt); /* t3_renderer_new on this run's GPU API */
  t3_renderer_set_clear_color(h->renderer, 0x202830, 1.0f);
  h->scene = t3_scene_new();
  h->camera = t3_perspective_camera_new(60.0f, (float)w / (float)hgt, 0.1f, 100.0f);
  t3_object_set_position(h->camera, 0, 1.5f, 4);
  t3_object_look_at(h->camera, 0, 0, 0);
  t3_material *mat = t3_mesh_standard_material_new(0x3399ff);
  h->cube = t3_mesh_new(t3_box_geometry_new(1, 1, 1, 1, 1, 1), mat);
  t3_object_add((t3_object *)h->scene, h->cube);
  t3_light *sun = t3_directional_light_new(0xffffff, 3.14159f);
  t3_object_set_position(sun, 3, 5, 2);
  t3_object_add((t3_object *)h->scene, sun);
  t3_object_add((t3_object *)h->scene, t3_ambient_light_new(0x404040, 3.14159f));
  /* #endregion */
  cw_audio_open(48000, fill, h);
  /* actions, not raw keys: fixed_update reads the tick's snapshot, which
   * keeps a tap shorter than a tick (it reads as pressed for one tick) */
  h->left = cw_action_add("left", 0.2f);
  h->right = cw_action_add("right", 0.2f);
  h->tone_on = cw_action_add("tone", 0.5f);
  cw_action_bind_text(h->left, "key:left, axis:lx-");
  cw_action_bind_text(h->right, "key:right, axis:lx+");
  cw_action_bind_text(h->tone_on, "key:space, pad:south");
  cw_debug_f32("x", &h->x);
  cw_debug_f32("angle", &h->angle);
  cw_debug_i32("ticks", &h->ticks);
}

/* #region fixed_update */
static void fixed_update(void *user, double dt) {
  hello *h = user;
  float move = cw_input_axis(0, h->left, h->right);
  h->prev_x = h->x;
  h->prev_angle = h->angle;
  h->x += move * 2.0f * (float)dt;
  h->angle += 1.5f * (float)dt;
  h->tone = cw_action_pressed(0, h->tone_on);
  h->ticks++;
}
/* #endregion */

/* #region render */
static void render(void *user, double alpha) {
  hello *h = user;
  float a = (float)alpha;
  float ang = h->prev_angle + (h->angle - h->prev_angle) * a;
  t3_object_set_position(h->cube, h->prev_x + (h->x - h->prev_x) * a, 0, 0);
  t3_object_set_rotation(h->cube, ang * 0.5f, ang, 0);
  cw_render3d_window_target(h->renderer); /* draw into this frame's window */
  t3_renderer_render(h->renderer, h->scene, h->camera);
}
/* #endregion */

/* #region main */
cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){
    .title = "cartwheel hello",
    .width = 640,
    .height = 360,
    .init = init,
    .fixed_update = fixed_update,
    .render = render,
    .debug = true,
    .user = &app,
  };
}
/* #endregion */

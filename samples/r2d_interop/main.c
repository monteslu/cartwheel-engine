/* r2d_interop: three.c and the 2D renderer drawing one frame, on GL or WebGPU.
 * Each frame: three.c renders a torus knot INTO a 2D render target (its
 * framebuffer, with depth), then a cube scene to the window; the 2D pass
 * draws on top of that 3D frame without clearing it (a HUD panel, text,
 * sprites) and draws the render target as a rotating, translucent sprite.
 * cw_render3d_texture_target / cw_render3d_window_target point three.c at the
 * target or the window on either API. On GL, after the 2D pass,
 * t3_renderer_reset_bindings() resyncs three.c's state cache, as render2d.h
 * documents (on WebGPU it costs nothing). */
#include <math.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "three.h"

typedef struct {
  t3_renderer *r;
  t3_scene *scene, *rt_scene;
  t3_camera *cam, *rt_cam;
  t3_mesh *cube, *knot;
  cw_texture rt, sheet;
  cw_font sans;
  int32_t ticks;
  uint32_t t3_calls, t3_triangles;
  int win_w, win_h;
} app_t;
static app_t app;

static void init(void *user) {
  app_t *a = user;
  cw_drawable_size(&a->win_w, &a->win_h);
  a->r = cw_render3d_window_renderer(a->win_w, a->win_h);
  t3_renderer_set_clear_color(a->r, 0x182030, 1.0f);
  /* window scene: a ring of cubes */
  a->scene = t3_scene_new();
  a->cam = t3_perspective_camera_new(55, (float)a->win_w / (float)a->win_h, 0.1f, 100);
  t3_object_set_position(a->cam, 0, 2.2f, 6);
  t3_object_look_at(a->cam, 0, 0, 0);
  a->cube = t3_mesh_new(t3_box_geometry_new(1.4f, 1.4f, 1.4f, 1, 1, 1), t3_mesh_standard_material_new(0x3399ff));
  t3_object_add((t3_object *)a->scene, a->cube);
  for (int i = 0; i < 6; i++) {
    t3_mesh *m = t3_mesh_new(t3_box_geometry_new(0.5f, 0.5f, 0.5f, 1, 1, 1), t3_mesh_standard_material_new(0xff8040));
    t3_object_set_position(m, 3 * cosf((float)i * 1.047f), 0, 3 * sinf((float)i * 1.047f));
    t3_object_add((t3_object *)a->scene, m);
  }
  t3_light *sun = t3_directional_light_new(0xffffff, 3.14159f);
  t3_object_set_position(sun, 3, 5, 2);
  t3_object_add((t3_object *)a->scene, sun);
  t3_object_add((t3_object *)a->scene, t3_ambient_light_new(0x404040, 3.14159f));
  /* render-target scene: a torus knot */
  a->rt_scene = t3_scene_new();
  a->rt_scene->has_background = true;
  a->rt_scene->background = t3_color_hex(0x402050);
  a->rt_cam = t3_perspective_camera_new(50, 1, 0.1f, 50);
  t3_object_set_position(a->rt_cam, 0, 0, 5);
  a->knot = t3_mesh_new(t3_torus_knot_geometry_new(1, 0.35f, 96, 12, 2, 3), t3_mesh_standard_material_new(0x40ff90));
  t3_object_add((t3_object *)a->rt_scene, a->knot);
  t3_light *l2 = t3_directional_light_new(0xffffff, 3.14159f);
  t3_object_set_position(l2, 2, 3, 4);
  t3_object_add((t3_object *)a->rt_scene, l2);
  t3_object_add((t3_object *)a->rt_scene, t3_ambient_light_new(0x303030, 3.14159f));

  a->rt = cw_target_create(&(cw_target_desc){ .width = 192, .height = 192, .depth = true });
  a->sheet = cw_texture_load("images/sheet.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->sans = cw_font_load("fonts/FiraSans-Regular.ttf", NULL);
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_u32("t3.calls", &a->t3_calls);
  cw_debug_u32("t3.triangles", &a->t3_triangles);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  float t = (float)a->ticks / 60.0f;
  t3_object_set_rotation(a->cube, t * 0.5f, t, 0);
  t3_object_set_rotation(a->knot, t * 0.8f, t * 0.3f, 0);

  /* 1. three.c into the 2D render target */
  t3_renderer_set_size(a->r, 192, 192);
  cw_render3d_texture_target(a->r, a->rt);
  t3_renderer_render(a->r, a->rt_scene, a->rt_cam);
  /* 2. three.c to the window */
  t3_renderer_set_size(a->r, a->win_w, a->win_h);
  cw_render3d_window_target(a->r);
  t3_renderer_render(a->r, a->scene, a->cam);
  const t3_render_info *info = t3_renderer_info(a->r);
  a->t3_calls = info->calls;
  a->t3_triangles = info->triangles;

  /* 3. 2D over the 3D frame (no clear) */
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = false });
  cw_r2d_sprite(&(cw_sprite){ .texture = a->rt, .pos = { 520, 120 }, .pivot = { 0.5f, 0.5f }, .rotation = sinf(t) * 0.3f,
                              .color = cw_rgba(1, 1, 1, 0.92f) });
  cw_r2d_rect_lines((cw_rect){ 10, 10, 250, 70 }, 2, cw_hex(0xffffffff));
  cw_r2d_rect((cw_rect){ 12, 12, 246, 66 }, cw_hex(0x000000a0));
  cw_r2d_text(a->sans, "three.c + render2d", (cw_vec2){ 22, 18 }, &(cw_text_style){ .size = 24 });
  cw_r2d_text(a->sans, "3D into a 2D target, 2D over 3D", (cw_vec2){ 22, 50 },
              &(cw_text_style){ .size = 16, .color = cw_hex(0xa0d0ffff) });
  for (int i = 0; i < 4; i++)
    cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = { (float)((i + a->ticks / 8) % 4) * 32, 0, 32, 32 },
                                .pos = { 60.0f + (float)i * 50 + 20 * sinf(t + (float)i), 320 }, .pivot = { 0.5f, 1 },
                                .scale = { 1.5f, 1.5f } });
  cw_r2d_end();
  /* 4. three.c's GL cache is stale after the 2D pass's GL work */
  t3_renderer_reset_bindings(a->r);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d interop", .width = 640, .height = 360, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}

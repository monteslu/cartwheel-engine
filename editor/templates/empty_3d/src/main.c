/* Empty 3D (a cartwheel project template): the main scene (main.cwscene)
 * runs; this file is where game logic goes. The cube spins so a running game
 * is visibly running; ui_accept (project.cwproj `input.*`) counts presses.
 *
 * Debug fields: game.ticks, game.presses, cube.rotation, tree.nodes. */
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/project.h"
#include "cartwheel/prompt.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene_file.h"

static struct {
  int32_t ticks, presses, nodes;
  float rotation;
} G;
static cw_action accept;

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  accept = cw_action_find("ui_accept");
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_i32("game.presses", &G.presses);
  cw_debug_f32("cube.rotation", &G.rotation);
  cw_debug_i32("tree.nodes", &G.nodes);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  cw_node cube = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main/Cube");
  G.rotation += (float)dt;
  cw_node_set(cube, "rotation", cw_val_vec3(0, G.rotation, 0));
  if (cw_action_just_pressed(0, accept)) G.presses++;
  G.nodes = cw_tree_node_count(cw_scene_app_tree());
  G.ticks++;
}

/* #region prompt */
/* The controls along the bottom, drawn for the device the player holds (cartwheel/prompt.h): button glyphs on a
 * pad, keycaps on a keyboard, and a rebinding shows at once. Name the action ("{jump}"), never a button ("A"). */
static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  cw_r2d_begin(NULL); /* the window, over the scene */
  cw_vec2 size = cw_r2d_canvas_size();
  float hs = size.y * 0.03f; /* body text: 3% of the canvas height, over the 2.4% floor (cartwheel/render2d.h) */
  cw_vec2 hz = cw_prompt_text_size((cw_font){ 0 }, "{ui_accept} counts a press", 0, hs);
  cw_prompt_text((cw_font){ 0 }, "{ui_accept} counts a press", 0, cw_v2(size.x * 0.5f, size.y - hz.y - hs), hs, (cw_color){ 0 }, CW_ALIGN_CENTER);
  cw_r2d_end();
}
/* #endregion */

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc app = { .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed_update, .render = render };
  cw_project_apply_app(&app); /* render.* and the name from project.cwproj */
  const char *main_scene = cw_project_get("main_scene");
  return cw_scene_app(main_scene ? main_scene : "main.cwscene", &app);
}

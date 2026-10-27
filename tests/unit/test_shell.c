/* cartwheel/shell.h: the shared_screen option. A split-screen game gets one view per player; a shared-screen
 * game (a scrolling brawler, a co-op platformer) gets one view, and every player's rect is the whole canvas.
 * Each shared check is paired with the same call without the option, which must give the other answer: a
 * shell that ignored shared_screen cannot pass both halves. */
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "cartwheel/shell.h"
#include "test.h"

static bool same_rect(cw_rect a, cw_rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static cw_tree *make_tree(cw_node *cams, int n) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  for (int i = 0; i < n; i++) {
    cams[i] = cw_node_new("Camera3D", "Cam");
    cw_node_add_child(cw_tree_root(t), cams[i]);
  }
  return t;
}

/* returns the views the shell asked the pipeline for (3 players), and fills the rects */
static int run(bool shared, cw_rect *rects, cw_rect *shared_rect, cw_rect *canvas) {
  /* CW_TEST_NO_SHARED: the gate control, the option is left off so the shared checks must fail */
  if (getenv("CW_TEST_NO_SHARED")) shared = false;
  cw_shell_init(&(cw_shell_desc){ .title = "t", .max_players = 4, .shared_screen = shared });
  cw_shell_start(3);
  cw_node cams[4];
  cw_tree *t = make_tree(cams, 4);
  cw_pipeline *p = cw_pipeline_of(t);
  int set = cw_shell_split(p, cams);
  const cw_view *v;
  int views = cw_pipeline_views(p, &v);
  CHECK_EQ_INT(set, views); /* what the shell reports is what the pipeline holds */
  cw_r2d_begin(NULL); /* the canvas size is known once a pass has run */
  cw_vec2 sz = cw_r2d_canvas_size();
  cw_r2d_end();
  for (int i = 0; i < 3; i++) rects[i] = cw_shell_player_rect(i);
  *shared_rect = cw_shell_shared_rect();
  *canvas = cw_rect_make(0, 0, sz.x, sz.y);
  cw_tree_free(t);
  return views;
}

static void test_shared_and_split(void) {
  cw_rect r[3], sh, canvas, r2[3], sh2, canvas2;
  int shared_views = run(true, r, &sh, &canvas);
  CHECK(canvas.w > 0 && canvas.h > 0);
  CHECK_EQ_INT(shared_views, 0); /* one view: the tree's own camera fills the output */
  for (int i = 0; i < 3; i++) CHECK(same_rect(r[i], canvas)); /* each player gets the whole canvas */
  CHECK(sh.w == 0 && sh.h == 0); /* no fourth quadrant to hand to a shared HUD */
  /* control: the same three players without the option split the screen */
  int split_views = run(false, r2, &sh2, &canvas2);
  CHECK_EQ_INT(split_views, 3);
  for (int i = 0; i < 3; i++) CHECK(!same_rect(r2[i], canvas2) && r2[i].w < canvas2.w + 0.5f && r2[i].w * 2 <= canvas2.w + 0.5f);
  CHECK(sh2.w > 0 && sh2.h > 0);
  uint32_t v = (uint32_t)shared_views * 10u + (uint32_t)split_views;
  FOLD(v);
}

static void test_one_player(void) {
  cw_shell_init(&(cw_shell_desc){ .title = "t", .max_players = 4, .shared_screen = true });
  cw_shell_start(1);
  cw_r2d_begin(NULL);
  cw_vec2 sz = cw_r2d_canvas_size();
  cw_r2d_end();
  CHECK(same_rect(cw_shell_player_rect(0), cw_rect_make(0, 0, sz.x, sz.y)));
}

CW_TEST_SUITE("shell", { "shared and split", test_shared_and_split }, { "one player", test_one_player });

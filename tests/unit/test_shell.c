/* cartwheel/shell.h: the shared_screen option. A split-screen game gets one view per player; a shared-screen
 * game (a scrolling brawler, a co-op platformer) gets one view, and every player's rect is the whole canvas.
 * Each shared check is paired with the same call without the option, which must give the other answer: a
 * shell that ignored shared_screen cannot pass both halves. */
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "cartwheel/shell.h"
#include "cartwheel/ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
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
  if (cw_test_env("CW_TEST_NO_SHARED")) shared = false; /* from frame 2 on every host: the case runs late (CW_TEST_LATE) */
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

/* No screen shake: cw_shell_screen_shake is 0 at every stored level, with reduced motion on or off, so a game that
 * multiplies its shake by it shakes nothing; the stored level still survives a save commit and a fresh init (an older
 * save keeps its layout and a migration can read it). CW_TEST_SHAKE_IGNORED (the gate's second control) checks Full as
 * the old shell answered it (1): that check must fail. */
static void test_screen_shake(void) {
  cw_shell_init(&(cw_shell_desc){ .title = "t", .max_players = 4 });
  float k[6];
  int n = 0;
  for (int rm = 0; rm < 2; rm++) {
    cw_ui_set_reduced_motion(rm != 0);
    for (int l = CW_SHELL_SHAKE_FULL; l <= CW_SHELL_SHAKE_OFF; l++) {
      cw_shell_set_shake_level((cw_shell_shake)l);
      k[n++] = cw_shell_screen_shake();
    }
  }
  cw_ui_set_reduced_motion(false);
  for (int i = 0; i < n; i++) CHECK(k[i] == 0.0f);
  if (getenv("CW_TEST_SHAKE_IGNORED")) CHECK(k[0] == 1.0f);
  /* the stored level is kept: a new init over the same save region reads Reduced back */
  cw_shell_set_shake_level(CW_SHELL_SHAKE_REDUCED);
  cw_shell_init(&(cw_shell_desc){ .title = "t", .max_players = 4 });
  CHECK_EQ_INT((int)cw_shell_shake_level(), (int)CW_SHELL_SHAKE_REDUCED);
  CHECK(cw_shell_screen_shake() == 0.0f);
  cw_shell_set_shake_level(CW_SHELL_SHAKE_FULL);
  uint32_t v = (uint32_t)(k[0] * 100) + (uint32_t)(k[5] * 100);
  FOLD(v);
}

/* Text size and Text background (cartwheel/render2d.h): the steps go round 100, 125, 150, 200%, the shell's save
 * keeps both (the save region holds the byte after a commit, and init applies it again), a byte of 0 (a save from
 * before the settings) leaves the defaults, and the UI's text scale is the same number. */
static void test_text_settings(void) {
  cw_shell_init(&(cw_shell_desc){ .title = "t" });
  cw_text_scale_restore(0);
  CHECK(cw_text_scale() == 1.0f);
  cw_text_scale_cycle();
  CHECK(cw_text_scale() == 1.25f);
  cw_text_scale_cycle();
  cw_text_scale_cycle();
  CHECK(cw_text_scale() == 2.0f);
  CHECK(!strcmp(cw_text_scale_label(), "200%"));
  cw_text_scale_cycle();
  CHECK(cw_text_scale() == 1.0f); /* round to 100% */
  cw_text_scale_cycle_back();
  CHECK(cw_text_scale() == 2.0f);
  cw_text_scale_set(1.5f);
  cw_text_backdrop_set(CW_TEXT_BACKDROP_PANEL);
  CHECK(cw_ui_text_scale() == 1.5f); /* one scale: the UI's is the engine's */
  cw_shell_save_commit();
  uint8_t byte = cw_text_scale_saved();
  CHECK(byte != 0);
  /* another run: the defaults, then the shell's init reads the save back */
  cw_text_scale_set(1.0f);
  cw_text_backdrop_set(CW_TEXT_BACKDROP_OFF);
  cw_shell_init(&(cw_shell_desc){ .title = "t" });
  CHECK(cw_text_scale() == 1.5f);
  CHECK((int)cw_text_backdrop_get() == (int)CW_TEXT_BACKDROP_PANEL);
  CHECK(!strcmp(cw_text_backdrop_label(), "Panel"));
  /* a save from before the settings: nothing changes */
  cw_text_scale_set(1.25f);
  cw_text_scale_restore(0);
  CHECK(cw_text_scale() == 1.25f);
  cw_text_scale_set(1.0f);
  cw_text_backdrop_set(CW_TEXT_BACKDROP_OFF);
  cw_shell_save_commit();
  uint32_t fold = byte;
  FOLD(fold);
}

/* The join screen's seats (cw_shell_join_seat_rect, the boxes cw_shell_draw draws): for 2, 3 and 4 seats the row is
 * centred about the canvas middle, where the "Join" heading and the hints are centred, each box a quarter of the canvas
 * less the gap, one quarter apart; four seats are exactly where the shell always put them (w * s + u * 0.4). An unheld
 * seat number is an empty rect. CW_TEST_OLD_SEATS (the gate control) puts the boxes where the old shell drew them
 * (from the left edge whatever the seat count) before the centring checks: 2 and 3 seats must fail, 4 must not. */
static void test_join_seats(void) {
  bool old = cw_test_env("CW_TEST_OLD_SEATS") != NULL;
  for (int n = 2; n <= 4; n++) {
    cw_shell_init(&(cw_shell_desc){ .title = "t", .max_players = n });
    cw_r2d_begin(NULL);
    cw_vec2 sz = cw_r2d_canvas_size();
    cw_r2d_end();
    float u = sz.y / 20.0f, w = sz.x / 4.0f;
    cw_rect r[4];
    for (int i = 0; i < n; i++) {
      r[i] = cw_shell_join_seat_rect(i);
      cw_rect was = cw_rect_make(w * (float)i + u * 0.4f, sz.y * 0.3f, w - u * 0.8f, sz.y * 0.4f); /* the old layout */
      if (n == 4) CHECK(same_rect(r[i], was)); /* four seats: unchanged */
      if (old) r[i] = was;
      CHECK(fabsf(r[i].w - (w - u * 0.8f)) < 0.01f && r[i].y == sz.y * 0.3f && r[i].h == sz.y * 0.4f);
      if (i) CHECK(fabsf(r[i].x - r[i - 1].x - w) < 0.01f);
      FOLD(r[i]);
    }
    float mid = (r[0].x + r[n - 1].x + r[n - 1].w) * 0.5f;
    CHECK(fabsf(mid - sz.x * 0.5f) < 0.5f); /* the row's middle is the heading's */
    CHECK(cw_shell_join_seat_rect(n).w == 0 && cw_shell_join_seat_rect(-1).w == 0);
  }
}

/* reads the gate's switches (CW_TEST_NO_SHARED, CW_TEST_OLD_SEATS): runs at frame 2, when every host has them */
void cw_test_late(void) {
  test_shared_and_split();
  test_join_seats();
}
CW_TEST_SUITE("shell", { "one player", test_one_player },
              { "screen shake setting", test_screen_shake }, { "text settings", test_text_settings });

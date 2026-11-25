/* board_pick: a board game's table, for 3D picking (cartwheel/pick3d.h), cursors between targets
 * (cartwheel/cursor.h) and coplanar faces (cartwheel/coplanar.h).
 *
 * A 6 x 6 board of flat squares laid on a walnut board, a stack of three cards on one square (all of them in the
 * board's top plane), and six pieces (a body and a head each, in group "pieces"). The mouse or a finger picks a
 * piece (cw_pick3d_canvas: through whichever view is under it) and lifts it; two players' cursors hop between the
 * 36 squares on their pads at the same time (cw_cursors_step), each by their own picture's directions, and the
 * pointer moves player 1's cursor to the square under it (cw_cursors_point). The squares and the cards are laid
 * apart once the board is built (cw_coplanar_separate), so the depth test never has to choose between faces in one
 * plane.
 *
 * Test switches (cw_test_env, applied at frame 2): BP_MODE=persp (default) | ortho (the camera ORTHOGONAL, size 9)
 * | split (two views, the second player's camera across the table); CW_TEST_PICK_OFFSET=<px> (the gate control: the
 * pointer's ray moved that far right, so it misses); CW_TEST_CURSOR_FLIP=1 (the control: sticks read mirrored);
 * COPLANAR_SEPARATE=0 (the control: the board left as built, its planes fighting).
 *
 * Debug state: bp.tick, bp.mode, pick.n (clicks), pick.last (the piece the last click lifted, -1 none), pick.seq (the
 * pieces of the first four clicks, a base-8 number with 7 for none), cur.p0 / cur.p1 (the cursors' squares),
 * cur.hops0 / cur.hops1, cur.p1_pads (player 1's square at frame 66, before the clicks), cur.point (the square the
 * pointer put player 1 on), cop.pairs (faces still fighting after
 * the separation), cop.moved, fight.px (pixels of the top card's middle drawn in another surface's colour). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/coplanar.h"
#include "cartwheel/cursor.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/input.h"
#include "cartwheel/pick3d.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

#define N 6
#define SQ 1.2f
#define PIECES 6

static struct {
  cw_node world, cam, cam2, board, sq[N * N], card[3], piece[PIECES], marker[2];
  cw_cursors *cur;
  cw_action left, right, up, down;
  int32_t tick, mode, pick_n, pick_last, pick_seq, p0, p1, hops0, hops1, point, p1_pads, pairs, moved, fight;
  bool ready;
  uint32_t ticket;
  int rx, ry;
} S;

static const int PIECE_SQ[PIECES] = { 7, 9, 11, 26, 28, 33 };

static cw_node mesh(cw_node parent, const char *name, const char *prim, t3_vec3 at, t3_vec3 size, float r, float g, float b) {
  cw_node m = cw_node_new("MeshInstance3D", name);
  cw_node_add_child(parent, m);
  cw_node_set(m, "primitive", cw_val_enum(prim));
  cw_node_set(m, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(m, "color", cw_val_color(r, g, b, 1));
  cw_node_set(m, "roughness", cw_val_float(0.7f));
  cw_node3d_set_position(m, at);
  return m;
}
static t3_vec3 square_at(int i) { return (t3_vec3){ ((float)(i % N) - 2.5f) * SQ, 0, ((float)(i / N) - 2.5f) * SQ }; }

static void init(void *u) {
  (void)u;
  cw_tree *t = cw_scene_app_tree();
  S.world = cw_node_get(cw_tree_root(t), "World");
  S.cam = cw_node_get(S.world, "Camera");
  S.cam2 = cw_node_get(S.world, "Camera2");
  cw_node3d_look_at(S.cam, (t3_vec3){ 0, 0, 0.4f }, (t3_vec3){ 0, 1, 0 });
  cw_node3d_look_at(S.cam2, (t3_vec3){ 0, 0, -0.4f }, (t3_vec3){ 0, 1, 0 });
  S.board = mesh(S.world, "Board", "BOX", (t3_vec3){ 0, -0.1f, 0 }, (t3_vec3){ 8, 0.2f, 8 }, 0.25f, 0.13f, 0.06f);
  /* the squares: flat planes in the board's top plane */
  for (int i = 0; i < N * N; i++) {
    char nm[16];
    snprintf(nm, sizeof nm, "Sq%d", i);
    bool light = ((i % N) + (i / N)) & 1;
    S.sq[i] = mesh(S.world, nm, "PLANE", square_at(i), (t3_vec3){ SQ * 0.96f, 1, SQ * 0.96f }, light ? 0.75f : 0.12f,
                   light ? 0.68f : 0.3f, light ? 0.5f : 0.16f);
  }
  /* three cards on square 21, each smaller and turned further, all in the same plane */
  t3_vec3 c = square_at(21);
  static const float cs[3] = { 1.0f, 0.75f, 0.5f }, col[3][3] = { { 0.8f, 0.1f, 0.08f }, { 0.1f, 0.65f, 0.12f }, { 0.08f, 0.16f, 0.85f } };
  for (int k = 0; k < 3; k++) {
    char nm[16];
    snprintf(nm, sizeof nm, "Card%d", k);
    S.card[k] = mesh(S.world, nm, "PLANE", c, (t3_vec3){ cs[k], 1, cs[k] }, col[k][0], col[k][1], col[k][2]);
    cw_node3d_set_rotation(S.card[k], (t3_vec3){ 0, 0.35f * (float)k, 0 });
  }
  for (int i = 0; i < PIECES; i++) {
    char nm[16];
    snprintf(nm, sizeof nm, "Piece%d", i);
    S.piece[i] = cw_node_new("Node3D", nm);
    cw_node_add_child(S.world, S.piece[i]);
    cw_node_add_group(S.piece[i], "pieces");
    cw_node3d_set_position(S.piece[i], square_at(PIECE_SQ[i]));
    bool p1 = i >= 3;
    mesh(S.piece[i], "Body", "CYLINDER", (t3_vec3){ 0, 0.2f, 0 }, (t3_vec3){ 0.7f, 0.4f, 0 }, p1 ? 0.55f : 0.85f, p1 ? 0.08f : 0.8f, p1 ? 0.08f : 0.7f);
    mesh(S.piece[i], "Head", "SPHERE", (t3_vec3){ 0, 0.55f, 0 }, (t3_vec3){ 0.36f, 0.36f, 0.36f }, p1 ? 0.55f : 0.85f, p1 ? 0.08f : 0.8f, p1 ? 0.08f : 0.7f);
  }
  /* the cursors' markers: a floating bar over the square, on layer 2 (picks test layer 1 only) */
  for (int p = 0; p < 2; p++) {
    S.marker[p] = mesh(S.world, p ? "Marker1" : "Marker0", "BOX", (t3_vec3){ 0, 0, 0 }, (t3_vec3){ SQ * 0.8f, 0.06f, 0.12f },
                       p ? 0.95f : 0.2f, p ? 0.35f : 0.6f, p ? 0.1f : 0.95f);
    cw_node_set(S.marker[p], "emissive", cw_val_color(p ? 0.6f : 0.1f, p ? 0.2f : 0.3f, p ? 0.05f : 0.6f, 1));
    cw_node_set(S.marker[p], "layers", cw_val_int(2));
  }
  cw_node_set(S.cam, "cull_layers", cw_val_int(3));
  cw_node_set(S.cam2, "cull_layers", cw_val_int(3));
  S.cur = cw_cursors_new(&(cw_cursor_desc){ .pick_layers = 1 }); /* (the markers' layer 2 is never under the pointer) */
  cw_cursors_set_nodes(S.cur, S.sq, N * N);
  cw_cursors_place(S.cur, 0, 14);
  cw_cursors_place(S.cur, 1, 21);
  S.left = cw_action_add("left", 0.3f), S.right = cw_action_add("right", 0.3f), S.up = cw_action_add("up", 0.3f), S.down = cw_action_add("down", 0.3f);
  cw_action_bind_text(S.left, "pad:left, axis:lx-, key:left");
  cw_action_bind_text(S.right, "pad:right, axis:lx+, key:right");
  cw_action_bind_text(S.up, "pad:up, axis:ly-, key:up");
  cw_action_bind_text(S.down, "pad:down, axis:ly+, key:down");
  S.pick_last = S.point = -1;
  S.pick_seq = 0;
  cw_debug_i32("bp.tick", &S.tick);
  cw_debug_i32("bp.mode", &S.mode);
  cw_debug_i32("pick.n", &S.pick_n);
  cw_debug_i32("pick.last", &S.pick_last);
  cw_debug_i32("pick.seq", &S.pick_seq);
  cw_debug_i32("cur.p0", &S.p0);
  cw_debug_i32("cur.p1", &S.p1);
  cw_debug_i32("cur.hops0", &S.hops0);
  cw_debug_i32("cur.hops1", &S.hops1);
  cw_debug_i32("cur.point", &S.point);
  cw_debug_i32("cur.p1_pads", &S.p1_pads);
  cw_debug_i32("cop.pairs", &S.pairs);
  cw_debug_i32("cop.moved", &S.moved);
  cw_debug_i32("fight.px", &S.fight);
}

static const char *test_opt(const char *name) { return cw_test_env(name); }

/* the mode and the separation, once the switches can be read (frame 2) and the meshes are built (frame 1) */
static void setup(void) {
  cw_tree *t = cw_scene_app_tree();
  const char *m = cw_test_env("BP_MODE");
  S.mode = m && !strcmp(m, "ortho") ? 1 : m && !strcmp(m, "split") ? 2 : 0;
  if (S.mode == 1) {
    cw_node_set(S.cam, "projection", cw_val_enum("ORTHOGONAL"));
    cw_node_set(S.cam, "size", cw_val_float(9));
  }
  cw_view3d v0 = { S.cam, { 0, 0, 1280, 720 } }, v1 = v0;
  if (S.mode == 2) {
    cw_node cams[2] = { S.cam, S.cam2 };
    cw_pipeline_split(cw_pipeline_of(t), cams, 2);
    cw_view3d vs[2];
    cw_canvas_views3d(t, vs, 2);
    v0 = vs[0], v1 = vs[1];
  }
  /* #region views */
  cw_cursors_set_view(S.cur, 0, &v0);
  cw_cursors_set_view(S.cur, 1, &v1);
  /* #endregion */
  /* where the pieces' heads are on each view (what an input script clicks) */
  for (int i = 0; i < PIECES; i++) {
    t3_vec3 hp = square_at(PIECE_SQ[i]);
    hp.y = 0.55f;
    cw_vec2 a = cw_camera3d_world_to_screen(v0.camera, v0.rect, hp, NULL), b = cw_camera3d_world_to_screen(v1.camera, v1.rect, hp, NULL);
    cw_info("board_pick: piece %d head at %.0f,%.0f (view 0) %.0f,%.0f (view 1)", i, a.x, a.y, b.x, b.y);
  }
  for (int i = 0; i < N * N; i += 7) {
    cw_vec2 a = cw_camera3d_world_to_screen(v0.camera, v0.rect, square_at(i), NULL), b = cw_camera3d_world_to_screen(v1.camera, v1.rect, square_at(i), NULL);
    cw_info("board_pick: square %d at %.0f,%.0f (view 0) %.0f,%.0f (view 1)", i, a.x, a.y, b.x, b.y);
  }
  /* #region coplanar */
  /* the board's planes laid apart: squares on the board, cards on the square and on each other */
  cw_coplanar_set_options(test_opt);
  cw_coplanar_set_plane_tol(0.0005f);
  int left = 0;
  S.moved = cw_coplanar_separate_ex(cw_node3d_object(S.world), 0.002f, &left, CW_COPLANAR_UNSHARE);
  cw_coplanar_report r;
  cw_coplanar_scan(cw_node3d_object(S.world), &r);
  S.pairs = r.pairs;
  /* #endregion */
  cw_info("board_pick: mode %d, %d vertices moved off shared planes in %.1f ms, %d pairs left", S.mode, S.moved, cw_coplanar_last_ms(), S.pairs);
  S.ready = true;
}

static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  if (!S.ready && cw_frame_index() >= 1) setup();
  if (!S.ready) return;
  S.tick++;
  bool flip = cw_test_env("CW_TEST_CURSOR_FLIP") != NULL;
  /* #region cursors */
  for (int p = 0; p < 2; p++) {
    cw_vec2 v = cw_input_vector(p, S.left, S.right, S.up, S.down, CW_DEADZONE_RADIAL, 0.2f);
    if (flip) v.x = -v.x, v.y = -v.y;
    if (cw_cursors_step(S.cur, p, v) >= 0) *(p ? &S.hops1 : &S.hops0) += 1;
    t3_vec3 at = square_at(cw_cursors_target(S.cur, p));
    cw_node3d_set_position(S.marker[p], (t3_vec3){ at.x, 0.9f + 0.1f * (float)p, at.z + (p ? 0.25f : -0.25f) });
  }
  /* #endregion */
  S.p0 = cw_cursors_target(S.cur, 0), S.p1 = cw_cursors_target(S.cur, 1);
  if (cw_frame_index() == 65) S.p1_pads = S.p1; /* (after the pads' moves, before a click moves it) */
}

/* the pointer, once per rendered frame: a press picks a piece (and lifts it) and moves player 1's cursor */
static void update(void *u, double dt) {
  (void)u, (void)dt;
  if (!S.ready) return;
  /* #region pick_point */
  const cw_raw_input *in = cw_input_raw();
  if (!in || !(in->mouse_pressed & 1)) return;
  cw_vec2 at = cw_r2d_window_to_canvas((cw_vec2){ in->mouse_x, in->mouse_y });
  /* #endregion */
  const char *off = cw_test_env("CW_TEST_PICK_OFFSET");
  if (off) at.x += (float)atof(off);
  /* #region pick_hit */
  cw_tree *t = cw_scene_app_tree();
  cw_pick3d_hit h;
  int got = cw_pick3d_canvas(t, at, &(cw_pick3d_filter){ .group = "pieces", .layers = 1 }, &h, 1, NULL);
  int which = -1;
  for (int i = 0; got && i < PIECES; i++)
    if (S.piece[i].id == h.node.id) which = i;
  /* #endregion */
  if (S.pick_last >= 0) cw_node3d_set_position(S.piece[S.pick_last], square_at(PIECE_SQ[S.pick_last]));
  S.pick_last = which;
  if (which >= 0) cw_node3d_set_position(S.piece[which], (t3_vec3){ square_at(PIECE_SQ[which]).x, 0.35f, square_at(PIECE_SQ[which]).z });
  if (S.pick_n < 4) {
    int32_t mul = 1;
    for (int k = 0; k < S.pick_n; k++) mul *= 8;
    S.pick_seq += (which >= 0 ? which : 7) * mul;
  }
  S.pick_n++;
  if (!got) S.point = cw_cursors_point(S.cur, 1, t, at); /* (a click on the board, not a piece: player 1's cursor goes there) */
  cw_info("board_pick: click at %.0f,%.0f: piece %d, point %d", at.x, at.y, which, S.point);
}

/* the top card's middle read back: every pixel there must be the top card's blue */
static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  if (!S.ready || S.mode == 2) return;
  if (S.ticket) {
    uint8_t px[12 * 12 * 4];
    int got = cw_gpu_read_result(S.ticket, px);
    if (got != 0) S.ticket = 0;
    if (got == 1) {
      int bad = 0;
      for (int i = 0; i < 12 * 12; i++) {
        const uint8_t *q = px + i * 4;
        if (!(q[2] > q[0] + 16 && q[2] > q[1] + 16)) bad++;
      }
      S.fight = bad;
    }
  }
  if (!S.ticket && S.tick > 0 && S.tick % 20 == 0) {
    /* (the canvas is 1280 x 720; the window's pixels may be more: the read is of window pixels) */
    cw_vec2 s = cw_camera3d_world_to_screen(S.cam, (cw_rect){ 0, 0, 1280, 720 }, square_at(21), NULL);
    int dw = 0, dh = 0;
    cw_drawable_size(&dw, &dh);
    float k = dw > 0 ? (float)dw / 1280.0f : 1;
    S.ticket = cw_gpu_read_window((int)lroundf(s.x * k) - 6, (int)lroundf(s.y * k) - 6, 12, 12);
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "board_pick", .width = 1280, .height = 720, .debug = true, .init = init,
                              .fixed_update = fixed_update, .update = update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}

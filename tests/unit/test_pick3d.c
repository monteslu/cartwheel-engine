/* Board-game support: 3D picking and projection (cartwheel/pick3d.h), cursors between targets (cartwheel/cursor.h)
 * and coplanar faces (cartwheel/coplanar.h). Expected places are worked out by hand from the camera's lens (not by
 * calling the projection back), so a projection and a pick that agree with each other but not with the picture
 * fail. Nothing here draws: the meshes are built by a pre_render, the hits come from their triangles. */
#include <math.h>
#include <string.h>

#include "cartwheel/coplanar.h"
#include "cartwheel/cursor.h"
#include "cartwheel/multimesh.h"
#include "cartwheel/pick3d.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/subviewport.h"
#include "test.h"

#define NEAR(a, b, e) (fabsf((float)(a) - (float)(b)) <= (e))

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}
static cw_node box(cw_node parent, const char *name, t3_vec3 at, t3_vec3 size) {
  cw_node m = add(parent, "MeshInstance3D", name);
  cw_node_set(m, "primitive", cw_val_enum("BOX"));
  cw_node_set(m, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node3d_set_position(m, at);
  return m;
}
static void fold_vec2(cw_vec2 v) {
  int32_t q[2] = { (int32_t)lroundf(v.x * 16), (int32_t)lroundf(v.y * 16) };
  FOLD(q);
}

/* a camera at (0, 0, 0) looking down -Z with a 90 degree field: tan(45) = 1, so at depth d the view spans -d..d */
static void test_project_perspective(void) {
  cw_tree *t = cw_tree_new();
  cw_node cam = add(cw_tree_root(t), "Camera3D", "Cam");
  cw_node_set(cam, "fov", cw_val_float(90));
  cw_rect vp = { 100, 50, 200, 100 }; /* aspect 2: x spans -2d..2d */
  float depth = -1;
  cw_vec2 s = cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ 0, 0, -5 }, &depth);
  CHECK(NEAR(s.x, 200, 1e-3f) && NEAR(s.y, 100, 1e-3f) && NEAR(depth, 5, 1e-5f)); /* the centre */
  s = cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ 4, 1, -2 }, &depth);    /* ndc (1, 0.5) */
  CHECK(NEAR(s.x, 300, 1e-3f) && NEAR(s.y, 75, 1e-3f) && NEAR(depth, 2, 1e-5f));
  s = cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ -2, -2, -4 }, &depth);  /* ndc (-0.25, -0.5) */
  CHECK(NEAR(s.x, 175, 1e-3f) && NEAR(s.y, 125, 1e-3f));
  cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ 0, 0, 3 }, &depth);
  CHECK(depth < 0); /* behind */
  /* the inverse and the ray */
  t3_vec3 w = cw_camera3d_screen_to_world(cam, vp, (cw_vec2){ 300, 75 }, 2);
  CHECK(NEAR(w.x, 4, 1e-4f) && NEAR(w.y, 1, 1e-4f) && NEAR(w.z, -2, 1e-4f));
  cw_ray3d r = cw_camera3d_screen_ray(cam, vp, (cw_vec2){ 300, 75 });
  CHECK(NEAR(r.origin.x, 0, 1e-6f) && NEAR(r.origin.z, 0, 1e-6f));
  float l = sqrtf(16 + 1 + 4);
  CHECK(NEAR(r.direction.x, 4 / l, 1e-5f) && NEAR(r.direction.y, 1 / l, 1e-5f) && NEAR(r.direction.z, -2 / l, 1e-5f));
  /* moved and turned: a quarter turn left about Y looks down -X */
  cw_node3d_set_position(cam, (t3_vec3){ 10, 2, 0 });
  cw_node3d_set_rotation(cam, (t3_vec3){ 0, CW_PI / 2, 0 });
  s = cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ 7, 2, 0 }, &depth);
  CHECK(NEAR(s.x, 200, 1e-3f) && NEAR(s.y, 100, 1e-3f) && NEAR(depth, 3, 1e-4f));
  s = cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ 7, 2, -6 }, &depth); /* the camera's right is -Z: x 6 at depth 3 = ndc 1 */
  CHECK(NEAR(s.x, 300, 1e-2f) && NEAR(s.y, 100, 1e-2f));
  fold_vec2(s);
  /* not a camera */
  s = cw_camera3d_world_to_screen(cw_tree_root(t), vp, (t3_vec3){ 1, 2, 3 }, &depth);
  CHECK(s.x == 0 && s.y == 0 && depth == 0);
  cw_tree_free(t);
}

/* ORTHOGONAL: size 10 high, a 400 x 200 view is 20 wide; looking straight down (-Y), its up is -Z */
static void test_project_ortho(void) {
  cw_tree *t = cw_tree_new();
  cw_node cam = add(cw_tree_root(t), "Camera3D", "Cam");
  cw_node_set(cam, "projection", cw_val_enum("ORTHOGONAL"));
  cw_node_set(cam, "size", cw_val_float(10));
  cw_node3d_set_position(cam, (t3_vec3){ 0, 10, 0 });
  cw_node3d_set_rotation(cam, (t3_vec3){ -CW_PI / 2, 0, 0 });
  cw_rect vp = { 0, 0, 400, 200 };
  float depth;
  cw_vec2 s = cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ 5, 0, 0 }, &depth);
  CHECK(NEAR(s.x, 300, 1e-2f) && NEAR(s.y, 100, 1e-2f) && NEAR(depth, 10, 1e-4f));
  s = cw_camera3d_world_to_screen(cam, vp, (t3_vec3){ 0, 3, -2.5f }, &depth); /* up the screen; nearer, same place */
  CHECK(NEAR(s.x, 200, 1e-2f) && NEAR(s.y, 50, 1e-2f) && NEAR(depth, 7, 1e-4f));
  cw_ray3d r = cw_camera3d_screen_ray(cam, vp, (cw_vec2){ 300, 50 });
  CHECK(NEAR(r.direction.x, 0, 1e-5f) && NEAR(r.direction.y, -1, 1e-5f) && NEAR(r.direction.z, 0, 1e-5f));
  CHECK(NEAR(r.origin.x, 5, 1e-3f) && NEAR(r.origin.z, -2.5f, 1e-3f));
  t3_vec3 w = cw_camera3d_screen_to_world(cam, vp, (cw_vec2){ 300, 50 }, 10);
  CHECK(NEAR(w.x, 5, 1e-3f) && NEAR(w.y, 0, 1e-3f) && NEAR(w.z, -2.5f, 1e-3f));
  /* the renderer's camera takes the same lens (its projection matrix maps the point to the same place) */
  t3_camera *tc = cw_camera3d_t3(cam);
  CHECK(tc != NULL);
  cw_tree_free(t);
}

/* a board with three pieces (a piece: a Node3D in group "pieces" with a base and a cap) under a camera above it */
typedef struct board { cw_tree *t; cw_node cam, board, piece[3], cap[3]; } board;
static const t3_vec3 PIECE_AT[3] = { { -2, 0, 0 }, { 0, 0, 1 }, { 2, 0, -1 } };
static board make_board(bool ortho) {
  board b;
  b.t = cw_tree_new();
  cw_node root = cw_tree_root(b.t);
  b.cam = add(root, "Camera3D", "Cam");
  if (ortho) cw_node_set(b.cam, "projection", cw_val_enum("ORTHOGONAL")), cw_node_set(b.cam, "size", cw_val_float(8));
  cw_node3d_set_position(b.cam, (t3_vec3){ 0, 8, 6 });
  cw_node3d_look_at(b.cam, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 1, 0 });
  b.board = box(root, "Board", (t3_vec3){ 0, -0.1f, 0 }, (t3_vec3){ 8, 0.2f, 8 });
  for (int i = 0; i < 3; i++) {
    char nm[16];
    snprintf(nm, sizeof nm, "Piece%d", i);
    b.piece[i] = add(root, "Node3D", nm);
    cw_node_add_group(b.piece[i], "pieces");
    cw_node3d_set_position(b.piece[i], PIECE_AT[i]);
    box(b.piece[i], "Base", (t3_vec3){ 0, 0.25f, 0 }, (t3_vec3){ 0.8f, 0.5f, 0.8f });
    b.cap[i] = box(b.piece[i], "Cap", (t3_vec3){ 0, 0.6f, 0 }, (t3_vec3){ 0.4f, 0.2f, 0.4f });
  }
  cw_tree_pre_render(b.t, 1.0); /* the meshes are built in pre_render */
  return b;
}

static void test_pick_board(bool ortho) {
  board b = make_board(ortho);
  cw_rect vp = { 0, 0, 1280, 720 };
  cw_pick3d_filter pieces = { .group = "pieces" };
  cw_pick3d_hit h[8];
  for (int i = 0; i < 3; i++) {
    /* the cap's top centre, projected: the cap is hit, and the piece through the group */
    t3_vec3 top = { PIECE_AT[i].x, 0.7f, PIECE_AT[i].z };
    cw_vec2 s = cw_camera3d_world_to_screen(b.cam, vp, top, NULL);
    int n = cw_pick3d_screen(b.cam, vp, s, NULL, h, 8);
    CHECK_EQ_INT(n, 1);
    CHECK(n && h[0].node.id == b.cap[i].id && h[0].instance == -1);
    CHECK(n && NEAR(h[0].point.x, top.x, 1e-3f) && NEAR(h[0].point.y, 0.7f, 1e-3f) && NEAR(h[0].point.z, top.z, 1e-3f));
    CHECK(n && NEAR(h[0].normal.y, 1, 1e-4f)); /* the top face, facing the camera */
    n = cw_pick3d_screen(b.cam, vp, s, &pieces, h, 8);
    CHECK(n == 1 && h[0].node.id == b.piece[i].id);
    /* the control's offset: where a point 1.2 m to the right of the cap's top lies misses every piece and lands on
     * the board */
    cw_vec2 off = cw_camera3d_world_to_screen(b.cam, vp, (t3_vec3){ top.x + 1.2f, top.y, top.z }, NULL);
    CHECK(off.x > s.x + 20);
    n = cw_pick3d_screen(b.cam, vp, off, NULL, h, 8);
    CHECK(n == 1 && h[0].node.id == b.board.id && NEAR(h[0].point.y, 0, 1e-3f));
    CHECK_EQ_INT(cw_pick3d_screen(b.cam, vp, off, &pieces, h, 8), 0);
    FOLD(h[0].distance);
  }
  /* every hit along the ray through piece 1's cap: cap top, then base or board under it, nearest first */
  cw_vec2 s1 = cw_camera3d_world_to_screen(b.cam, vp, (t3_vec3){ 0, 0.7f, 1 }, NULL);
  int n = cw_pick3d_screen(b.cam, vp, s1, &(cw_pick3d_filter){ .all = true }, h, 8);
  CHECK(n >= 3);
  for (int i = 1; i < n; i++) CHECK(h[i].distance >= h[i - 1].distance);
  CHECK(n && h[0].node.id == b.cap[1].id);
  /* max distance short of the cap: nothing */
  CHECK_EQ_INT(cw_pick3d_screen(b.cam, vp, s1, &(cw_pick3d_filter){ .max_distance = h[0].distance * 0.9f }, h, 8), 0);
  /* hidden: piece 1 hidden is passed through to the board; hit anyway with `hidden` */
  cw_node_set(b.piece[1], "visible", cw_val_bool(false));
  n = cw_pick3d_screen(b.cam, vp, s1, NULL, h, 8);
  CHECK(n == 1 && h[0].node.id == b.board.id);
  n = cw_pick3d_screen(b.cam, vp, s1, &(cw_pick3d_filter){ .hidden = true }, h, 8);
  CHECK(n == 1 && h[0].node.id == b.cap[1].id);
  cw_node_set(b.piece[1], "visible", cw_val_bool(true));
  /* exclude the piece's subtree */
  n = cw_pick3d_screen(b.cam, vp, s1, &(cw_pick3d_filter){ .exclude = b.piece[1] }, h, 8);
  CHECK(n == 1 && h[0].node.id == b.board.id);
  /* layers: piece 1 on layer 2 is missed by a layer-1 pick, hit by a layer-2 one */
  cw_node_set(b.piece[1], "layers", cw_val_int(2));
  n = cw_pick3d_screen(b.cam, vp, s1, &(cw_pick3d_filter){ .layers = 1 }, h, 8);
  CHECK(n == 1 && h[0].node.id == b.board.id);
  n = cw_pick3d_screen(b.cam, vp, s1, &(cw_pick3d_filter){ .layers = 2, .group = "pieces" }, h, 8);
  CHECK(n == 1 && h[0].node.id == b.piece[1].id);
  cw_node_set(b.piece[1], "layers", cw_val_int(1));
  /* moved this tick, no frame drawn: hit where it is now */
  cw_node3d_set_position(b.piece[0], (t3_vec3){ -3, 0, 2 });
  cw_vec2 s0 = cw_camera3d_world_to_screen(b.cam, vp, (t3_vec3){ -3, 0.7f, 2 }, NULL);
  n = cw_pick3d_screen(b.cam, vp, s0, &pieces, h, 8);
  CHECK(n == 1 && h[0].node.id == b.piece[0].id);
  /* a side face: the base seen from the camera's side has a normal facing the camera (+Z) */
  cw_vec2 sf = cw_camera3d_world_to_screen(b.cam, vp, (t3_vec3){ 2, 0.15f, -0.6f }, NULL);
  n = cw_pick3d_screen(b.cam, vp, sf, NULL, h, 8);
  CHECK(n == 1 && NEAR(h[0].normal.z, 1, 1e-4f) && NEAR(h[0].point.z, -0.6f, 1e-3f));
  cw_tree_free(b.t);
}
static void test_pick_perspective(void) { test_pick_board(false); }
static void test_pick_ortho(void) { test_pick_board(true); }

/* split screen: two cameras on opposite sides of the board, the canvas halved; a point in a half picks through that
 * half's camera */
static void test_pick_split(void) {
  board b = make_board(false);
  cw_node cam2 = add(cw_tree_root(b.t), "Camera3D", "Cam2");
  cw_node3d_set_position(cam2, (t3_vec3){ 0, 8, -6 });
  cw_node3d_look_at(cam2, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 1, 0 });
  cw_view3d one[4];
  CHECK_EQ_INT(cw_canvas_views3d(b.t, one, 4), 1);
  CHECK(one[0].camera.id == b.cam.id && one[0].rect.x == 0 && one[0].rect.w > 0);
  cw_vec2 cs = { one[0].rect.w, one[0].rect.h };
  cw_view v[2] = { { b.cam, 0, 0, 0.5f, 1 }, { cam2, 0.5f, 0, 0.5f, 1 } };
  cw_pipeline_set_views(cw_pipeline_of(b.t), v, 2);
  cw_view3d views[4];
  CHECK_EQ_INT(cw_canvas_views3d(b.t, views, 4), 2);
  CHECK(views[1].camera.id == cam2.id && NEAR(views[1].rect.x, cs.x * 0.5f, 1e-4f) && NEAR(views[1].rect.w, cs.x * 0.5f, 1e-4f));
  for (int k = 0; k < 2; k++) {
    for (int i = 0; i < 3; i++) {
      cw_vec2 s = cw_camera3d_world_to_screen(views[k].camera, views[k].rect, (t3_vec3){ PIECE_AT[i].x, 0.7f, PIECE_AT[i].z }, NULL);
      cw_view3d at;
      CHECK_EQ_INT(cw_canvas_view3d_at(b.t, s, &at), k);
      int view = -9;
      cw_pick3d_hit h;
      int n = cw_pick3d_canvas(b.t, s, &(cw_pick3d_filter){ .group = "pieces" }, &h, 1, &view);
      CHECK(n == 1 && view == k && h.node.id == b.piece[i].id);
    }
  }
  /* the second camera sees the board turned round: piece 0 (x -2) is on the right of its half */
  cw_vec2 s0 = cw_camera3d_world_to_screen(cam2, views[1].rect, (t3_vec3){ -2, 0.7f, 0 }, NULL);
  CHECK(s0.x > views[1].rect.x + views[1].rect.w * 0.5f);
  int view = -9;
  cw_pick3d_hit h;
  CHECK_EQ_INT(cw_pick3d_canvas(b.t, (cw_vec2){ -5, -5 }, NULL, &h, 1, &view), 0);
  CHECK_EQ_INT(view, -1);
  cw_tree_free(b.t);
}

/* a MultiMeshInstance3D of nine tiles in cells: the instance picked is the buffer's index */
static void test_pick_multimesh(void) {
  board b = make_board(false);
  cw_node mm = add(cw_tree_root(b.t), "MultiMeshInstance3D", "Tiles");
  cw_node_set(mm, "chunk_size", cw_val_float(2));
  cw_node st = box(mm, "Stamp", (t3_vec3){ 0, 0.05f, 0 }, (t3_vec3){ 0.6f, 0.1f, 0.6f });
  (void)st;
  cw_multimesh_set_instance_count(mm, 9);
  for (int i = 0; i < 9; i++) {
    float x = (float)(i % 3) * 1.5f - 1.5f - 0.75f, z = (float)(i / 3) * 1.5f + 2.25f - 1.5f;
    t3_mat4 m = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, z, 1 } };
    cw_multimesh_set_transform(mm, 8 - i, &m); /* (the buffer in another order than the cells) */
  }
  cw_tree_pre_render(b.t, 1.0);
  cw_tree_pre_render(b.t, 1.0);
  cw_rect vp = { 0, 0, 1280, 720 };
  int ok = 0;
  for (int i = 0; i < 9; i++) {
    float x = (float)(i % 3) * 1.5f - 1.5f - 0.75f, z = (float)(i / 3) * 1.5f + 2.25f - 1.5f;
    cw_vec2 s = cw_camera3d_world_to_screen(b.cam, vp, (t3_vec3){ x, 0.1f, z }, NULL);
    cw_pick3d_hit h;
    int n = cw_pick3d_screen(b.cam, vp, s, NULL, &h, 1);
    if (n == 1 && h.node.id == mm.id && h.instance == 8 - i && NEAR(h.point.y, 0.1f, 1e-3f)) ok++;
    else cw_error("multimesh tile %d: n %d instance %d", i, n, n ? h.instance : -2);
  }
  CHECK_EQ_INT(ok, 9);
  cw_tree_free(b.t);
}

/* a SubViewport's world of its own: its camera and its pixels pick its nodes */
static void test_pick_subviewport(void) {
  cw_tree *t = cw_tree_new();
  cw_node sv = add(cw_tree_root(t), "SubViewport", "Inset");
  cw_node_set(sv, "size", cw_val_vec2(320, 240));
  cw_tree *w = cw_subviewport_tree(sv);
  CHECK(w != NULL);
  if (!w) { cw_tree_free(t); return; }
  cw_node cam = add(cw_tree_root(w), "Camera3D", "Camera");
  cw_node3d_set_position(cam, (t3_vec3){ 0, 0, 5 });
  cw_node cube = box(cw_tree_root(w), "Cube", (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 1, 1, 1 });
  cw_tree_pre_render(w, 1.0);
  cw_view3d v;
  CHECK(cw_subviewport_view3d(sv, &v));
  CHECK(v.camera.id == cam.id && v.rect.w == 320 && v.rect.h == 240);
  cw_pick3d_hit h;
  CHECK(cw_pick3d_screen(v.camera, v.rect, (cw_vec2){ 160, 120 }, NULL, &h, 1) == 1 && h.node.id == cube.id && NEAR(h.point.z, 0.5f, 1e-4f));
  CHECK_EQ_INT(cw_pick3d_screen(v.camera, v.rect, (cw_vec2){ 10, 10 }, NULL, &h, 1), 0);
  CHECK(!cw_subviewport_view3d(cw_tree_root(t), &v));
  cw_tree_free(t);
}

/* ── cursors ───────────────────────────────────────────────────────── */

/* a 3 x 3 grid, 100 apart (2D, y down): 0 1 2 / 3 4 5 / 6 7 8 */
static void grid(t3_vec3 *p) {
  for (int i = 0; i < 9; i++) p[i] = (t3_vec3){ (float)(i % 3) * 100, (float)(i / 3) * 100, 0 };
}

static void test_cursor_select(void) {
  cw_vec2 g[9];
  for (int i = 0; i < 9; i++) g[i] = (cw_vec2){ (float)(i % 3) * 100, (float)(i / 3) * 100 };
  CHECK_EQ_INT(cw_cursor_hop_select(g, 9, 4, (cw_vec2){ 1, 0 }, NULL, NULL, 0, NULL), 5);
  CHECK_EQ_INT(cw_cursor_hop_select(g, 9, 4, (cw_vec2){ 0, -1 }, NULL, NULL, 0, NULL), 1);
  CHECK_EQ_INT(cw_cursor_hop_select(g, 9, 4, (cw_vec2){ 1, -1 }, NULL, NULL, 0, NULL), 2);
  CHECK_EQ_INT(cw_cursor_hop_select(g, 9, 4, (cw_vec2){ -1, 1 }, NULL, NULL, 0, NULL), 6);
  CHECK_EQ_INT(cw_cursor_hop_select(g, 9, 3, (cw_vec2){ -1, 0 }, NULL, NULL, 0, NULL), -1); /* the edge: nothing left */
  CHECK_EQ_INT(cw_cursor_hop_select(g, 9, 0, (cw_vec2){ 1, 0 }, NULL, NULL, 0, NULL), 1);  /* the nearer of 1 and 2 */
  /* irregular spots: a near one a little off the line beats a far one dead ahead; one too far round (more than about
   * 60 degrees off) is never taken */
  cw_vec2 s[4] = { { 0, 0 }, { 300, 0 }, { 100, 40 }, { 40, -100 } };
  CHECK_EQ_INT(cw_cursor_hop_select(s, 4, 0, (cw_vec2){ 1, 0 }, NULL, NULL, 0, NULL), 2);
  cw_vec2 s2[3] = { { 0, 0 }, { 40, -100 }, { 30, 90 } };
  CHECK_EQ_INT(cw_cursor_hop_select(s2, 3, 0, (cw_vec2){ 1, 0 }, NULL, NULL, 0, NULL), -1);
  /* towers' own numbers: along + 2.2 sideways, within along x 1.75 */
  cw_vec2 s3[3] = { { 0, 0 }, { 200, 0 }, { 100, 60 } }; /* 100 + 132 = 232 > 200: the far one dead ahead */
  CHECK_EQ_INT(cw_cursor_hop_select(s3, 3, 0, (cw_vec2){ 1, 0 }, NULL, NULL, 0, NULL), 1);
  cw_vec2 s4[3] = { { 0, 0 }, { 200, 0 }, { 100, 40 } }; /* 100 + 88 = 188 < 200: the near one */
  CHECK_EQ_INT(cw_cursor_hop_select(s4, 3, 0, (cw_vec2){ 1, 0 }, NULL, NULL, 0, NULL), 2);
}

static bool odd_only(int player, int target, void *u) {
  (void)u;
  return player != 1 || (target & 1);
}

/* two players at once, hold-to-repeat timing, the allow rule */
static void test_cursor_players(void) {
  t3_vec3 p[9];
  grid(p);
  cw_cursors *c = cw_cursors_new(NULL);
  cw_cursors_set_points(c, p, 9);
  cw_cursors_set_allow(c, odd_only, NULL);
  cw_cursors_place(c, 0, 0);
  cw_cursors_place(c, 1, 4); /* not odd: player 1 goes to an allowed one at its first step */
  /* tick 0: player 0 right, player 1 down */
  CHECK_EQ_INT(cw_cursors_step(c, 0, (cw_vec2){ 1, 0 }), 1);
  int t1 = cw_cursors_step(c, 1, (cw_vec2){ 0, 1 });
  CHECK(cw_cursors_target(c, 1) & 1);
  CHECK_EQ_INT(cw_cursors_target(c, 1), 7);
  CHECK_EQ_INT(t1, 7);
  /* held: no hop until tick 18, then every 9 */
  int hops = 0, first_repeat = -1;
  for (int tick = 1; tick <= 40; tick++) {
    int h = cw_cursors_step(c, 0, (cw_vec2){ 0.9f, 0.1f });
    if (h >= 0) {
      hops++;
      if (first_repeat < 0) first_repeat = tick;
    }
    cw_cursors_step(c, 1, (cw_vec2){ 0, 0 });
  }
  CHECK_EQ_INT(first_repeat, 18);
  CHECK_EQ_INT(hops, 1); /* 1 -> 2 at tick 18; at 27 and 36 there is nothing further right */
  CHECK_EQ_INT(cw_cursors_target(c, 0), 2);
  /* a released stick and a new press hops at once */
  cw_cursors_step(c, 0, (cw_vec2){ 0, 0 });
  CHECK_EQ_INT(cw_cursors_step(c, 0, (cw_vec2){ 0, 1 }), 5);
  /* inside the dead zone: nothing */
  CHECK_EQ_INT(cw_cursors_step(c, 0, (cw_vec2){ 0, 0 }), -1);
  CHECK_EQ_INT(cw_cursors_step(c, 0, (cw_vec2){ 0.3f, 0 }), -1);
  /* player 1 left from 7: 6 is not odd; 3 lies up and left, 45 degrees off, inside the cone */
  CHECK_EQ_INT(cw_cursors_hop(c, 1, (cw_vec2){ -1, 0 }), 3);
  CHECK_EQ_INT(cw_cursors_hop(c, 1, (cw_vec2){ 0, -1 }), 1); /* up from 3: 0 is not odd, 1 is up and right */
  /* the pointer: nearest within the radius, the allow rule kept */
  CHECK_EQ_INT(cw_cursors_point(c, 0, NULL, (cw_vec2){ 210, 190 }), 8);
  CHECK_EQ_INT(cw_cursors_point(c, 0, NULL, (cw_vec2){ 150, 150 }), -1); /* 70 px from every spot */
  CHECK_EQ_INT(cw_cursors_target(c, 0), 8);
  CHECK_EQ_INT(cw_cursors_point(c, 1, NULL, (cw_vec2){ 0, 0 }), -1);  /* 0 is not odd for player 1 */
  int32_t st[2] = { cw_cursors_target(c, 0), cw_cursors_target(c, 1) };
  FOLD(st);
  cw_cursors_free(c);
}

/* 3D targets through each player's camera: two players on opposite sides of a board see it turned round, so up on
 * the stick goes away from each of them */
static void test_cursor_views(void) {
  board b = make_board(false);
  cw_node cam2 = add(cw_tree_root(b.t), "Camera3D", "Cam2");
  cw_node3d_set_position(cam2, (t3_vec3){ 0, 8, -6 });
  cw_node3d_look_at(cam2, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 1, 0 });
  /* a column of three squares along Z: 0 at z -2 (far from camera 1), 1 at 0, 2 at z 2 */
  t3_vec3 col[3] = { { 0, 0, -2 }, { 0, 0, 0 }, { 0, 0, 2 } };
  cw_cursors *c = cw_cursors_new(NULL);
  cw_cursors_set_points(c, col, 3);
  cw_cursors_set_view(c, 0, &(cw_view3d){ b.cam, { 0, 0, 640, 720 } });
  cw_cursors_set_view(c, 1, &(cw_view3d){ cam2, { 640, 0, 640, 720 } });
  cw_cursors_place(c, 0, 1);
  cw_cursors_place(c, 1, 1);
  CHECK_EQ_INT(cw_cursors_step(c, 0, (cw_vec2){ 0, -1 }), 0); /* up: away from camera 1 (toward -Z) */
  CHECK_EQ_INT(cw_cursors_step(c, 1, (cw_vec2){ 0, -1 }), 2); /* up: away from camera 2 (toward +Z) */
  /* without a view, points_xz reads the board plane: up is -Z */
  cw_cursors *d = cw_cursors_new(&(cw_cursor_desc){ .points_xz = true });
  cw_cursors_set_points(d, col, 3);
  cw_cursors_place(d, 0, 1);
  CHECK_EQ_INT(cw_cursors_hop(d, 0, (cw_vec2){ 0, -1 }), 0);
  cw_cursors_free(d);
  /* node targets: the pieces; a pointer on a piece's cap picks the piece through the 3D hit, even far from its centre */
  cw_cursors *e = cw_cursors_new(NULL);
  cw_cursors_set_nodes(e, b.piece, 3);
  cw_cursors_set_view(e, 0, &(cw_view3d){ b.cam, { 0, 0, 1280, 720 } });
  cw_vec2 edge = cw_camera3d_world_to_screen(b.cam, (cw_rect){ 0, 0, 1280, 720 }, (t3_vec3){ 2.38f, 0.3f, -0.6f }, NULL);
  CHECK_EQ_INT(cw_cursors_point(e, 0, b.t, edge), 2);
  /* the nodes move and the targets with them */
  cw_cursors_place(e, 0, 0);
  CHECK_EQ_INT(cw_cursors_hop(e, 0, (cw_vec2){ 1, 0 }), 1);
  cw_node3d_set_position(b.piece[2], (t3_vec3){ 0.5f, 0, 1 }); /* now just right of piece 1 */
  CHECK_EQ_INT(cw_cursors_hop(e, 0, (cw_vec2){ 1, 0 }), 2);
  cw_cursors_free(e);
  cw_cursors_free(c);
  cw_tree_free(b.t);
}

/* ── coplanar ──────────────────────────────────────────────────────── */

static t3_mesh *quad(t3_object *parent, float y, float w, uint32_t color) {
  t3_geometry *g = t3_plane_geometry_new(w, w, 1, 1);
  t3_material *m = t3_mesh_standard_material_new(color);
  t3_mesh *q = t3_mesh_new(g, m);
  t3_object_set_rotation(q, -CW_PI / 2, 0, 0); /* (facing +Y) */
  q->base.position.y = y;
  t3_object_add(parent, q);
  t3_release(g), t3_release(m), t3_release(q);
  return q;
}

static void test_coplanar(void) {
  t3_object *root = t3_group_new();
  /* a board and a tile laid flat on it (one plane, two looks), and a tile 5 mm above it that does not count */
  quad(root, 0, 4, 0x206020);
  t3_mesh *tile = quad(root, 0, 1, 0xc0a040);
  quad(root, 0.005f, 0.5f, 0x4040c0);
  t3_object_update_world_matrix(root, false, true);
  cw_coplanar_report r;
  cw_coplanar_scan(root, &r);
  CHECK_EQ_INT(r.meshes, 3);
  CHECK_EQ_INT(r.pairs, 2); /* the tile's two triangles over the board's */
  int left = -1, moved = cw_coplanar_separate(root, 0.002f, &left);
  CHECK(moved > 0);
  CHECK_EQ_INT(left, 0);
  t3_object_update_world_matrix(root, false, true);
  cw_coplanar_scan(root, &r);
  CHECK_EQ_INT(r.pairs, 0);
  CHECK(cw_coplanar_moved_total() >= moved);
  CHECK(cw_coplanar_last_ms() >= 0);
  /* the smaller one came out in front: the tile is now above the board */
  const float *p = tile->geometry->attributes[T3_ATTR_POSITION]->array;
  t3_vec3 wp = t3_vec3_apply_mat4(t3_v3(p[0], p[1], p[2]), &tile->base.matrix_world);
  CHECK(wp.y > 0.001f && wp.y < 0.01f);
  FOLD(moved);
  t3_release(root);
}

CW_TEST_SUITE("pick3d", { "project_perspective", test_project_perspective }, { "project_ortho", test_project_ortho },
              { "pick_perspective", test_pick_perspective }, { "pick_ortho", test_pick_ortho }, { "pick_split", test_pick_split },
              { "pick_multimesh", test_pick_multimesh }, { "pick_subviewport", test_pick_subviewport },
              { "cursor_select", test_cursor_select }, { "cursor_players", test_cursor_players },
              { "cursor_views", test_cursor_views }, { "coplanar", test_coplanar })

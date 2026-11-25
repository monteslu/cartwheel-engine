/* board_kit: the board-game kit pieces of v0.84.0 together, in a scene app (the case each one is for).
 *
 * A green table seen square on (an orthographic camera looking straight down), two cards and four pieces:
 *   - The cards' faces are 2D render targets drawn with the 2D renderer and worn by three.c materials
 *     (cw_target_texture3d): no SubViewport per face. A face is red in its top half and blue in its bottom half, so a
 *     face that came out upside down or not at all reads wrong. A click or a tap on a card turns it over: its face is
 *     drawn again (green over yellow), and the 3D copy follows from the 2D pass.
 *   - Two white boxes and two white cylinders, one of each solid and one a ghost (MeshInstance3D opacity 0.35): the
 *     boxes are primitives (the opacity is part of their look), the cylinders procedural meshes (cw_mesh3d_set_procedural:
 *     their materials are cloned for the node).
 *   - The pointer is read only through the recorded channel (cw_input_pointer_channel, cw_input_pointer in
 *     fixed_update), never the raw mouse: a match records from its start and the debug request `replay` puts the game
 *     back as the match began and replays it, the clicks included.
 *   - The shell's title has two items of the game's (Credits, How to play), each a page (draw_page).
 *   - A test start in the shell's menu_update: in a scene app the tree is paused outside play and the app's
 *     fixed_update does not run at the title (cartwheel/shell.h, the pause rule).
 *
 * Test switches (cw_test_env):
 *   BOARD_KIT_START=1           start a one-player match from the title (menu_update)
 *   BOARD_KIT_START_IN_FIXED=1  the control: try that start in the app's fixed_update instead, which never runs at the
 *                               title of a scene app, so the match never starts
 *   BOARD_KIT_NO_TITLE_ITEMS=1  the control: no title items (the second row is Settings again)
 *   BOARD_KIT_CONTROL=card      the control: the cards wear a plain white material, not their faces
 *   BOARD_KIT_CONTROL=ghost     the control: the ghosts keep opacity 1
 *   CW_TEST_POINTER_UNRECORDED=1 (the engine's control): the pointer is left out of the recording
 *
 * Debug state: kit.started, kit.start_screen (the shell's screen when the start ran: 1 the title), kit.ticks,
 * kit.title_calls, kit.title_index, kit.page_seen (menu_update saw a title page), kit.page_drawn (draw_page ran),
 * kit.card_top / kit.card_bottom (card 0's face read back on screen: 1 red, 2 blue, 3 green, 4 yellow, 0 another
 * colour, -1 unread), kit.card1_top, kit.ghost_box / kit.ghost_cyl (1: the ghost's pixel lies between the table's and
 * its solid twin's on every channel by at least 10 levels, 0 not, -1 unread), kit.read (1 once all were read),
 * kit.flips (cards turned), kit.flipped (a bit per card turned face down), kit.replay_done, kit.replay_match,
 * kit.replay_hash_match. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/input.h"
#include "cartwheel/pick3d.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/shell.h"
#include "cartwheel/ui.h"

#define CARDS 2
#define CANVAS_W 960
#define CANVAS_H 540
enum { R_C0_TOP, R_C0_BOTTOM, R_C1_TOP, R_TABLE, R_BOX, R_BOX_GHOST, R_CYL, R_CYL_GHOST, R_COUNT };

/* what the rules change: hashed for the replay check */
typedef struct state {
  int32_t ticks, flips, flipped, picks;
} state;

static struct {
  cw_node world, cam, card[CARDS], box[2], cyl[2];
  cw_texture face[CARDS];
  t3_material *face_mat[CARDS];
  bool face_dirty[CARDS];
  state s, start_state;
  bool started_by_env, fresh, control_card, control_ghost, configured;
  int32_t started, start_screen, title_calls, title_index, page_seen, page_drawn;
  int32_t card1_px; /* card 1 top pixel as 0xRRGGBB, for a failure message */
  int32_t card_top, card_bottom, card1_top, ghost_box, ghost_cyl, read;
  int32_t ticks, flips, flipped;
  int32_t replaying, replay_done, replay_match, replay_hash_match;
  uint32_t want_state, want_input;
  uint8_t *recording;
  size_t recording_len;
  uint32_t ticket[R_COUNT];
  uint8_t px[R_COUNT][3];
  int got;
} G;

static const t3_vec3 CARD_AT[CARDS] = { { -3.6f, 0.02f, 0 }, { -1.8f, 0.02f, 0 } };
static const char *const TITLE_ITEMS[2] = { "Credits", "How to play" };

/* ── the cards' faces: 2D render targets, worn in 3D ─────────────────── */

static void draw_face(int i) {
  bool down = G.s.flipped & (1 << i);
  cw_color top = down ? cw_rgba(0.1f, 0.85f, 0.15f, 1) : cw_rgba(0.9f, 0.1f, 0.1f, 1);
  cw_color bottom = down ? cw_rgba(0.95f, 0.85f, 0.1f, 1) : cw_rgba(0.1f, 0.2f, 0.9f, 1);
  cw_r2d_begin(&(cw_r2d_pass_desc){ .target = G.face[i], .clear = true, .clear_color = CW_WHITE });
  cw_r2d_rect(cw_rect_make(8, 8, 112, 88), top);
  cw_r2d_rect(cw_rect_make(8, 96, 112, 88), bottom);
  char n[4];
  snprintf(n, sizeof n, "%d", i + 1);
  cw_r2d_text(cw_ui_font_sans(), n, cw_v2(14, 10), &(cw_text_style){ .size = 22, .color = CW_WHITE });
  cw_r2d_end(); /* the 3D copy follows (cw_target_texture3d) */
  G.face_dirty[i] = false;
}

static cw_node add_mesh(const char *name, const char *prim, t3_vec3 at, t3_vec3 size, cw_color c) {
  cw_node m = cw_node_new("MeshInstance3D", name);
  cw_node_add_child(G.world, m);
  cw_node_set(m, "primitive", cw_val_enum(prim));
  cw_node_set(m, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(m, "color", cw_val_color(c.r, c.g, c.b, 1));
  cw_node_set(m, "roughness", cw_val_float(0.8f));
  cw_node3d_set_position(m, at);
  return m;
}

static cw_node add_procedural(const char *name, t3_mesh *mesh, t3_vec3 at) {
  cw_node n = cw_node_new("MeshInstance3D", name);
  cw_node_add_child(G.world, n);
  cw_mesh3d_set_procedural(n, mesh);
  t3_release(mesh);
  cw_node3d_set_position(n, at);
  return n;
}

/* the switches a gate sets reach a cart from its second frame (cw_test_env): read once, when first needed */
static void configure(void) {
  if (G.configured || cw_frame_index() < 1) return;
  G.configured = true;
  const char *c = cw_test_env("BOARD_KIT_CONTROL");
  G.control_card = c && !strcmp(c, "card");
  G.control_ghost = c && !strcmp(c, "ghost");
  if (cw_test_env("BOARD_KIT_NO_TITLE_ITEMS")) cw_shell_set_title_items(NULL);
  for (int i = 0; i < CARDS; i++) {
    t3_material *m = t3_mesh_basic_material_new(0xffffff);
    if (!G.control_card) t3_material_set_texture(m, T3_MAP, cw_target_texture3d(G.face[i]));
    t3_mesh *q = t3_mesh_new(t3_plane_geometry_new(1.4f, 2.0f, 1, 1), m);
    t3_object_set_rotation(q, -CW_PI / 2, 0, 0); /* flat on the table, its top edge away from the player (-Z) */
    G.face_mat[i] = m;
    char nm[8];
    snprintf(nm, sizeof nm, "Card%d", i);
    G.card[i] = add_procedural(nm, q, CARD_AT[i]);
    cw_node_add_group(G.card[i], "cards");
  }
  float op = G.control_ghost ? 1.0f : 0.35f;
  cw_node_set(G.box[1], "opacity", cw_val_float(op));
  cw_node_set(G.cyl[1], "opacity", cw_val_float(op));
  cw_node_set(G.box[1], "cast_shadow", cw_val_bool(false));
  cw_node_set(G.cyl[1], "cast_shadow", cw_val_bool(false));
}

/* ── shell hooks ──────────────────────────────────────────────────── */

static void start(int players, void *user) {
  (void)players, (void)user;
  memset(&G.s, 0, sizeof G.s);
  G.start_state = G.s;
  for (int i = 0; i < CARDS; i++) G.face_dirty[i] = true;
  G.started = 1;
  G.fresh = true;
  G.replaying = G.replay_done = 0;
  cw_input_record_begin(); /* the whole match, every click */
}

/* #region menu_update */
/* the menus' tick: runs at the title of a scene app, where the paused tree calls no fixed_update */
static void menu_update(cw_shell_screen screen, void *user) {
  (void)user;
  configure();
  if (screen == CW_SHELL_PAGE) G.page_seen = 1;
  if (G.started_by_env || screen != CW_SHELL_TITLE || cw_test_env("BOARD_KIT_START_IN_FIXED")) return;
  if (cw_test_env("BOARD_KIT_START")) {
    G.started_by_env = true;
    G.start_screen = (int32_t)screen;
    cw_shell_start(1);
  }
}
/* #endregion */

static void title_item(int index, void *user) {
  (void)user;
  G.title_calls++;
  G.title_index = index;
}

static void draw_page(int index, cw_vec2 canvas, void *user) {
  (void)user;
  G.page_drawn = 1;
  const char *lines[2][3] = { { "Credits", "Made with cartwheel", "Cards drawn in 2D, worn in 3D" },
                              { "How to play", "Click or tap a card to turn it over", "The ghosts show where a piece would go" } };
  for (int l = 0; l < 3; l++)
    cw_r2d_text(cw_ui_font_sans(), lines[index & 1][l], cw_v2(canvas.x * 0.5f, canvas.y * (0.2f + 0.12f * (float)l)),
                &(cw_text_style){ .size = canvas.y * (l ? 0.045f : 0.08f), .color = CW_WHITE, .align = CW_ALIGN_CENTER });
}

/* ── the rules ────────────────────────────────────────────────────── */

static uint32_t state_hash(void) { return cw_hash32(&G.s, sizeof G.s, CW_HASH32_INIT); }

/* `replay`: the state now and the input hash so far are what the replay must reach */
static void cmd_replay(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  if (cw_shell_screen_now() != CW_SHELL_PLAY || !cw_input_recording_active()) {
    cw_reply_error(r, "replay needs a match in play (it replays the match from its start)");
    return;
  }
  G.want_state = state_hash();
  cw_input_record_end();
  G.want_input = cw_input_record_hash();
  size_t len = 0;
  const uint8_t *d = cw_input_recording(&len);
  cw_free(G.recording);
  G.recording = cw_alloc(len ? len : 1, CW_MEM_GAME);
  memcpy(G.recording, d, len);
  G.recording_len = len;
  G.s = G.start_state; /* back to the match's start */
  for (int i = 0; i < CARDS; i++) G.face_dirty[i] = true;
  if (!cw_input_replay_begin(G.recording, G.recording_len)) {
    cw_reply_error(r, "the recording did not load");
    return;
  }
  G.replaying = 1;
  G.replay_done = G.replay_match = G.replay_hash_match = 0;
  cw_reply_printf(r, "{\"bytes\":%zu,\"state\":%u,\"input\":%u}", len, G.want_state, G.want_input);
}

static void init(void *user) {
  (void)user;
  cw_tree *t = cw_scene_app_tree();
  G.world = cw_node_get(cw_tree_root(t), "World");
  G.cam = cw_node_get(G.world, "Camera");
  cw_shell_init(&(cw_shell_desc){ .title = "Board Kit", .subtitle = "cards, ghosts and clicks", .start = start,
                                  .title_items = { TITLE_ITEMS[0], TITLE_ITEMS[1] }, .title_item = title_item,
                                  .draw_page = draw_page, .menu_update = menu_update });
  cw_input_pointer_channel(true); /* clicks and taps go into the recording, the replay and the input hash */
  add_mesh("Table", "BOX", (t3_vec3){ 0, -0.1f, 0 }, (t3_vec3){ 12, 0.2f, 8 }, cw_rgba(0.05f, 0.25f, 0.12f, 1));
  cw_color white = cw_rgba(0.9f, 0.9f, 0.9f, 1);
  G.box[0] = add_mesh("Box", "BOX", (t3_vec3){ 0.6f, 0.45f, -1.2f }, (t3_vec3){ 0.9f, 0.9f, 0.9f }, white);
  G.box[1] = add_mesh("BoxGhost", "BOX", (t3_vec3){ 0.6f, 0.45f, 1.2f }, (t3_vec3){ 0.9f, 0.9f, 0.9f }, white);
  for (int k = 0; k < 2; k++) {
    t3_material *m = t3_mesh_standard_material_new(0xe6e6e6);
    m->roughness = 0.8f;
    t3_mesh *cyl = t3_mesh_new(t3_cylinder_geometry_new(0.45f, 0.45f, 0.9f, 24, 1, false), m);
    t3_release(m);
    G.cyl[k] = add_procedural(k ? "CylGhost" : "Cyl", cyl, (t3_vec3){ 2.4f, 0.45f, k ? 1.2f : -1.2f });
  }
  for (int i = 0; i < CARDS; i++) {
    G.face[i] = cw_target_create(&(cw_target_desc){ .width = 128, .height = 192, .filter = CW_FILTER_LINEAR });
    G.face_dirty[i] = true;
  }
  G.title_index = -1;
  G.card_top = G.card_bottom = G.card1_top = G.ghost_box = G.ghost_cyl = -1;
  cw_debug_i32("kit.started", &G.started);
  cw_debug_i32("kit.start_screen", &G.start_screen);
  cw_debug_i32("kit.ticks", &G.ticks);
  cw_debug_i32("kit.title_calls", &G.title_calls);
  cw_debug_i32("kit.title_index", &G.title_index);
  cw_debug_i32("kit.page_seen", &G.page_seen);
  cw_debug_i32("kit.page_drawn", &G.page_drawn);
  cw_debug_i32("kit.card_top", &G.card_top);
  cw_debug_i32("kit.card_bottom", &G.card_bottom);
  cw_debug_i32("kit.card1_top", &G.card1_top);
  cw_debug_i32("kit.card1_px", &G.card1_px);
  cw_debug_i32("kit.ghost_box", &G.ghost_box);
  cw_debug_i32("kit.ghost_cyl", &G.ghost_cyl);
  cw_debug_i32("kit.read", &G.read);
  cw_debug_i32("kit.flips", &G.flips);
  cw_debug_i32("kit.flipped", &G.flipped);
  cw_debug_i32("kit.replaying", &G.replaying);
  cw_debug_i32("kit.replay_done", &G.replay_done);
  cw_debug_i32("kit.replay_match", &G.replay_match);
  cw_debug_i32("kit.replay_hash_match", &G.replay_hash_match);
  cw_debug_command("replay", "replay: replay the match from its start out of its own recording, then compare (kit.replay_match)", cmd_replay);
}

static void fixed_update(void *user, double dt) {
  (void)user, (void)dt;
  configure();
  /* the control: a test start here never runs at a scene app's title (the tree is paused outside play) */
  if (!G.started_by_env && cw_shell_screen_now() == CW_SHELL_TITLE && cw_test_env("BOARD_KIT_START_IN_FIXED") && cw_test_env("BOARD_KIT_START")) {
    G.started_by_env = true;
    G.start_screen = (int32_t)cw_shell_screen_now();
    cw_shell_start(1);
  }
  if (!cw_shell_update()) return;
  if (G.fresh) { /* the tick the match began in is not recorded: play starts at the next */
    G.fresh = false;
    return;
  }
  if (G.replaying && !cw_input_replay_active()) { /* the recording has run out: the replay reached the request */
    G.replay_match = state_hash() == G.want_state;
    G.replay_hash_match = cw_input_replay_hash() == G.want_input;
    G.replaying = 0;
    G.replay_done = 1;
    cw_info("board_kit replay: state %s, input hash %s", G.replay_match ? "same" : "DIFFERENT", G.replay_hash_match ? "same" : "DIFFERENT");
  }
  G.s.ticks++;
  /* #region pointer */
  cw_pointer pt = cw_input_pointer(); /* this tick's, recorded and replayed */
  if (pt.pressed) {
    cw_pick3d_hit hit;
    cw_vec2 at = cw_r2d_window_to_canvas(pt.pos);
    if (cw_pick3d_canvas(cw_scene_app_tree(), at, &(cw_pick3d_filter){ .group = "cards" }, &hit, 1, NULL)) {
      for (int i = 0; i < CARDS; i++)
        if (hit.node.id == G.card[i].id) {
          G.s.flipped ^= 1 << i;
          G.s.flips++;
          G.face_dirty[i] = true;
        }
    }
    G.s.picks++;
  }
  /* #endregion */
  G.ticks = G.s.ticks, G.flips = G.s.flips, G.flipped = G.s.flipped;
}

/* faces are drawn before the frame's 3D, so a turned card shows the same frame */
static void update(void *user, double dt) {
  (void)user, (void)dt;
  if (!G.configured) return;
  for (int i = 0; i < CARDS; i++)
    if (G.face_dirty[i] || !G.face_mat[i]) draw_face(i);
}

/* ── reading the picture back ─────────────────────────────────────── */

static int classify(const uint8_t *p) {
  int r = p[0], g = p[1], b = p[2];
  if (r > b + 60 && g > b + 60 && abs(r - g) < 60) return 4; /* yellow */
  if (r > g + 40 && r > b + 40) return 1;
  if (b > r + 40 && b > g + 40) return 2;
  if (g > r + 40 && g > b + 40) return 3;
  return 0;
}
static int between(const uint8_t *ghost, const uint8_t *table, const uint8_t *solid) {
  for (int c = 0; c < 3; c++) {
    int lo = CW_MIN(table[c], solid[c]), hi = CW_MAX(table[c], solid[c]);
    if (hi - lo < 20 || ghost[c] < lo + 10 || ghost[c] > hi - 10) return 0;
  }
  return 1;
}

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  int dw = 0, dh = 0;
  cw_drawable_size(&dw, &dh);
  float k = dw > 0 ? (float)dw / CANVAS_W : 1;
  int pending = 0;
  for (int i = 0; i < R_COUNT; i++) {
    if (!G.ticket[i]) continue;
    uint8_t px[4 * 4 * 4];
    int got = cw_gpu_read_result(G.ticket[i], px);
    if (got != 0) G.ticket[i] = 0;
    if (got == 1) memcpy(G.px[i], px + (2 * 4 + 2) * 4, 3), G.got |= 1 << i;
    pending += G.ticket[i] != 0;
  }
  if (G.got == (1 << R_COUNT) - 1) {
    G.card_top = classify(G.px[R_C0_TOP]);
    G.card_bottom = classify(G.px[R_C0_BOTTOM]);
    G.card1_top = classify(G.px[R_C1_TOP]);
    G.card1_px = G.px[R_C1_TOP][0] << 16 | G.px[R_C1_TOP][1] << 8 | G.px[R_C1_TOP][2];
    G.ghost_box = between(G.px[R_BOX_GHOST], G.px[R_TABLE], G.px[R_BOX]);
    G.ghost_cyl = between(G.px[R_CYL_GHOST], G.px[R_TABLE], G.px[R_CYL]);
    G.read = 1;
  }
  /* read again every 30 frames of play (a turned card reads its new face) */
  if (!pending && G.started && cw_shell_screen_now() == CW_SHELL_PLAY && G.s.ticks >= 20 && cw_frame_index() % 30 == 0) {
    const t3_vec3 at[R_COUNT] = { { CARD_AT[0].x, 0, -0.5f }, { CARD_AT[0].x, 0, 0.5f }, { CARD_AT[1].x, 0, -0.5f }, { 1.5f, 0, 0 },
                                  { 0.6f, 0.9f, -1.2f }, { 0.6f, 0.9f, 1.2f }, { 2.4f, 0.9f, -1.2f }, { 2.4f, 0.9f, 1.2f } };
    G.got = 0;
    for (int i = 0; i < R_COUNT; i++) {
      cw_vec2 s = cw_camera3d_world_to_screen(G.cam, (cw_rect){ 0, 0, CANVAS_W, CANVAS_H }, at[i], NULL);
      G.ticket[i] = cw_gpu_read_window((int)lroundf(s.x * k) - 2, (int)lroundf(s.y * k) - 2, 4, 4);
    }
  }
  if (cw_shell_hud_shown()) {
    cw_r2d_begin(NULL);
    cw_r2d_text(cw_ui_font_sans(), "Click a card to turn it over", cw_v2(16, 12), &(cw_text_style){ .size = 22, .color = CW_WHITE });
    cw_r2d_end();
  }
  cw_shell_draw();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "cartwheel board kit", .width = CANVAS_W, .height = CANVAS_H, .debug = true, .init = init,
                              .fixed_update = fixed_update, .update = update, .render = render };
  base.save = cw_shell_save_region();
  base.save_size = cw_shell_save_region_size();
  return cw_scene_app("main.cwscene", &base);
}

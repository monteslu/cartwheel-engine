/* hex_board: a small board game on a hex board for up to eight seats: people on pads, people passing a pad, CPUs.
 *
 * It shows three engine pieces together:
 *   - hex grids (cartwheel/hex.h): the board is the hexes within 3 steps of the centre (cw_hex_spiral), drawn from a
 *     pointy layout (cw_hex_to_point, cw_hex_corners), kept in a cw_hex_map of owners; a player's score is the size
 *     of their largest connected group (cw_hex_reachable through their own hexes); the hex under the mouse is outlined
 *     (cw_hex_from_point, drawing only: the rules read actions alone, so a replay needs nothing else).
 *   - CPU and hot seats in the shell (cw_shell_desc.cpu_seats, hot_seats): up to eight seats, each a person on a pad,
 *     a hot seat (people passing the pads round: cw_shell_set_turn hands the free pads to the seat whose turn it is) or
 *     a CPU at Easy, Normal or Hard; the game reads cw_shell_seat_cpu per seat.
 *   - actions set by the game (cw_input_set_actions): a CPU moves its cursor and places a stone by pressing the same
 *     actions a pad does, one press at a time, in fixed_update. The rules then read every seat through cw_action_*,
 *     never knowing who is a CPU. So the input recording, a replay and the input hash carry the CPUs' moves.
 *
 * Play: players take turns. Move the cursor (stick, d-pad, arrows; up and down keep to the column, as offset rows
 * cw_hex_to_offset) and place a stone (South, Space). When the board is full the largest group wins and a new round
 * starts.
 *
 * Test switches:
 *   HEX_BOARD_SEATS=a,b,... (cw_test_env): skip the menus, start with up to eight seats (-1 a person on the next pad,
 *     -2 a hot seat, 0..2 a CPU at that level), e.g. "-1,0,1,2" or "-2,-2,-2,-2,-2,-2,-2,-2".
 *   The debug request `replay`: stop the recording the match started, put the game back as the match began, and replay
 *     it; when it ends, game.replay_match says the state came out the same and game.replay_hash_match that the input
 *     hash did. CW_TEST_SET_UNRECORDED=1 is its control (the CPUs' actions left out of the recording). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/hex.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/prompt.h"
#include "cartwheel/render2d.h"
#include "cartwheel/shell.h"
#include "cartwheel/ui.h"

enum { RADIUS = 3, CELLS = 37, NP = 8 };
enum { A_LEFT, A_RIGHT, A_UP, A_DOWN, A_PLACE, A_COUNT };

typedef struct cpu_mind {
  cw_axial target; /* where it means to place */
  int32_t wait;    /* ticks until its next press (its reaction) */
  int32_t has_target;
} cpu_mind;

/* everything the rules change: hashed for the replay check */
typedef struct game_state {
  int32_t seats, turn, placed, round, winner, over_ticks, cpu_moves;
  int32_t cpu[NP];      /* -1 a person, else the CPU's level */
  int32_t score[NP], wins[NP], placed_by[NP];
  cw_axial cursor[NP];
  cpu_mind mind[NP];
  cw_rng rng;           /* the CPUs' mistakes */
} game_state;

static struct {
  game_state s;
  cw_hex_map *board;    /* owner (int8) per placed hex */
  cw_axial cells[CELLS];
  cw_action act[A_COUNT];
  cw_hex_layout lay;
  bool fresh;           /* the start hook ran this tick: gameplay begins at the next (the first recorded) */
  bool started_by_env;
  int32_t stray;        /* presses a hot seat made when it was not its turn (the routing gate holds it at 0) */
  int32_t ticks;
  uint32_t board_hash, state_hash;
  /* the replay check */
  int32_t replaying, replay_done, replay_match, replay_hash_match;
  game_state start_state;
  uint32_t want_state, want_input;
  uint8_t *recording;
  size_t recording_len;
} G;

static const uint32_t COL[NP] = { 0xe0604cff, 0x4f9fe6ff, 0x5ccf72ff, 0xe8c547ff, 0xa77be8ff, 0xf0913aff, 0x3fc4b8ff, 0xe87bb6ff };
static const char *const SEAT_NAME[NP] = { "Red", "Blue", "Green", "Gold", "Violet", "Orange", "Teal", "Pink" };

static int8_t owner(cw_axial h) {
  int8_t *o = CW_HEX_MAP_GET(G.board, h, int8_t);
  return o ? *o : -1;
}
static bool on_board(cw_axial h) { return cw_hex_length(h) <= RADIUS; }

/* #region score */
static int score_of_owner = 0;
static bool owned_by(cw_axial h, void *user) {
  (void)user;
  return on_board(h) && owner(h) == score_of_owner;
}
/* the largest connected group of p's stones */
static int score(int p) {
  score_of_owner = p;
  int best = 0;
  cw_axial reach[CELLS];
  for (int i = 0; i < CELLS; i++)
    if (owner(G.cells[i]) == p) {
      int n = cw_hex_reachable(G.cells[i], CELLS, owned_by, NULL, reach, CELLS);
      if (n > best) best = n;
    }
  return best;
}

/* #endregion */
static void new_round(void) {
  cw_hex_map_clear(G.board);
  G.s.placed = 0;
  G.s.winner = -1;
  G.s.over_ticks = 0;
  G.s.turn = G.s.round % G.s.seats;
  for (int p = 0; p < NP; p++) {
    G.s.cursor[p] = cw_hex_make(0, 0);
    G.s.score[p] = 0;
    memset(&G.s.mind[p], 0, sizeof G.s.mind[p]);
  }
}

/* ── the CPU: picks a hex, then walks the cursor there pressing the pad's actions ── */

static int gain_if(int p, cw_axial h) {
  int8_t v = (int8_t)p;
  cw_hex_map_set(G.board, h, &v);
  int s = score(p);
  cw_hex_map_remove(G.board, h);
  return s;
}

static cw_axial cpu_choose(int p, int level) {
  cw_axial empty[CELLS];
  int n = 0;
  for (int i = 0; i < CELLS; i++)
    if (owner(G.cells[i]) < 0) empty[n++] = G.cells[i];
  /* mistakes by level: Easy picks at random half the time, Normal one time in five, Hard never */
  int slip = level == CW_SHELL_CPU_EASY ? 2 : level == CW_SHELL_CPU_NORMAL ? 5 : 0;
  if (slip && cw_rng_int(&G.s.rng, 0, slip - 1) == 0) return empty[cw_rng_int(&G.s.rng, 0, n - 1)];
  int best = -1000000, at = 0;
  for (int i = 0; i < n; i++) {
    int v = gain_if(p, empty[i]) * 4;
    if (level == CW_SHELL_CPU_HARD) /* Hard also blocks: what the next player would gain there */
      v += gain_if((p + 1) % G.s.seats, empty[i]) * 2;
    v -= cw_hex_length(empty[i]); /* near the middle on a tie */
    if (v > best) best = v, at = i;
  }
  return empty[at];
}

/* #region cpu_presses */
/* the presses for this tick: one action down for one tick, then up, so every press is a fresh just_pressed */
static void cpu_tick(int p, int level) {
  cpu_mind *m = &G.s.mind[p];
  cw_action_value v[1];
  int n = 0;
  static const int REACT[CW_SHELL_CPU_LEVELS] = { 9, 6, 4 }; /* ticks between presses */
  if (G.s.winner < 0 && G.s.turn == p) {
    if (!m->has_target) {
      m->target = cpu_choose(p, level);
      m->has_target = 1;
      m->wait = REACT[level] * 2; /* a moment to think */
    }
    if (m->wait > 0) m->wait--;
    else {
      int cc, cr, tc, tr;
      cw_hex_to_offset(G.s.cursor[p], CW_HEX_ODD_R, &cc, &cr);
      cw_hex_to_offset(m->target, CW_HEX_ODD_R, &tc, &tr);
      int a = tr < cr ? A_UP : tr > cr ? A_DOWN : tc < cc ? A_LEFT : tc > cc ? A_RIGHT : A_PLACE;
      v[n++] = (cw_action_value){ G.act[a], 1.0f };
      m->wait = REACT[level];
      if (a == A_PLACE) m->has_target = 0;
    }
  }
  cw_input_set_actions(p, v, n); /* the whole seat this tick: what is not listed is up */
}

/* #endregion */
/* ── the rules: every seat read the same way ───────────────────────── */

static void place(int p, cw_axial h) {
  int8_t v = (int8_t)p;
  cw_hex_map_set(G.board, h, &v);
  G.s.placed++;
  G.s.placed_by[p]++;
  if (G.s.cpu[p] >= 0) G.s.cpu_moves++;
  for (int i = 0; i < G.s.seats; i++) G.s.score[i] = score(i);
  if (G.s.placed == CELLS) {
    int w = 0;
    for (int i = 1; i < G.s.seats; i++)
      if (G.s.score[i] > G.s.score[w]) w = i;
    G.s.winner = w;
    G.s.wins[w]++;
  } else {
    G.s.turn = (G.s.turn + 1) % G.s.seats;
    cw_axial *c = &G.s.cursor[G.s.turn];
    for (int i = 0; i < CELLS && owner(*c) >= 0; i++) /* the next player's cursor starts on a free hex */
      if (owner(G.cells[i]) < 0) *c = G.cells[i];
  }
}

static void move_cursor(int p, int a) {
  int c, r;
  cw_hex_to_offset(G.s.cursor[p], CW_HEX_ODD_R, &c, &r);
  if (a == A_LEFT) c--;
  if (a == A_RIGHT) c++;
  if (a == A_UP) r--;
  if (a == A_DOWN) r++;
  cw_axial h = cw_hex_from_offset(c, r, CW_HEX_ODD_R);
  if (on_board(h)) G.s.cursor[p] = h;
}

static void rules_tick(void) {
  if (G.s.winner >= 0) {
    if (++G.s.over_ticks >= 120) {
      G.s.round++;
      new_round();
    }
    return;
  }
  int p = G.s.turn;
  for (int q = 0; q < G.s.seats; q++) /* a hot seat whose turn it is not must hear nothing (cw_shell_set_turn) */
    for (int a = 0; a < A_COUNT && q != p; a++)
      if (cw_shell_seat_hot(q) && cw_action_just_pressed(q, G.act[a])) G.stray++;
  for (int a = A_LEFT; a <= A_DOWN; a++)
    if (cw_action_just_pressed(p, G.act[a])) move_cursor(p, a);
  if (cw_action_just_pressed(p, G.act[A_PLACE]) && owner(G.s.cursor[p]) < 0) place(p, G.s.cursor[p]);
}

static void hash_state(void) {
  G.board_hash = cw_hex_map_hash(G.board, CW_HASH32_INIT);
  G.state_hash = cw_hash32(&G.s, sizeof G.s, G.board_hash);
}

/* ── shell hooks ──────────────────────────────────────────────────── */

/* #region start */
static void start(int players, void *user) {
  (void)user;
  memset(&G.s, 0, sizeof G.s);
  G.s.seats = players;
  for (int p = 0; p < NP; p++) {
    cw_shell_cpu_level lv;
    G.s.cpu[p] = p < players && cw_shell_seat_cpu(p, &lv) ? (int32_t)lv : -1;
  }
  cw_rng_seed(&G.s.rng, 2026);
  new_round();
  cw_shell_set_turn(G.s.turn); /* the free pads drive the hot seat whose turn it is */
  G.start_state = G.s;
  G.fresh = true;
  G.replaying = G.replay_done = 0;
  cw_input_record_begin(); /* from the next tick: the whole match, the CPUs' presses included */
}
/* #endregion */
static void players_changed(int players, void *user) {
  (void)user;
  G.s.seats = players;
  for (int p = 0; p < NP; p++) {
    cw_shell_cpu_level lv;
    G.s.cpu[p] = p < players && cw_shell_seat_cpu(p, &lv) ? (int32_t)lv : -1;
  }
  if (G.s.turn >= players) G.s.turn = 0;
}

/* `replay`: the state now and the input hash so far are what the replay must reach */
static void cmd_replay(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  if (cw_shell_screen_now() != CW_SHELL_PLAY || !cw_input_recording_active()) {
    cw_reply_error(r, "replay needs a match in play (it replays the match from its start)");
    return;
  }
  hash_state();
  G.want_state = G.state_hash;
  cw_input_record_end();
  G.want_input = cw_input_record_hash();
  size_t len = 0;
  const uint8_t *d = cw_input_recording(&len);
  cw_free(G.recording);
  G.recording = cw_alloc(len ? len : 1, CW_MEM_GAME);
  memcpy(G.recording, d, len);
  G.recording_len = len;
  uint32_t ticks = cw_input_record_ticks();
  /* back to the match's start: the board, the cursors, the CPUs' minds and their random stream */
  G.s = G.start_state;
  cw_hex_map_clear(G.board);
  if (!cw_input_replay_begin(G.recording, G.recording_len)) {
    cw_reply_error(r, "the recording did not load");
    return;
  }
  G.replaying = 1;
  G.replay_done = G.replay_match = G.replay_hash_match = 0;
  cw_reply_printf(r, "{\"ticks\":%u,\"bytes\":%zu,\"state\":%u,\"input\":%u}", ticks, len, G.want_state, G.want_input);
}

static void init(void *user) {
  (void)user;
  /* #region shell */
  cw_shell_desc d = { .title = "Hex Board", .subtitle = "people and CPUs, one board", .max_players = NP, .cpu_seats = true,
                      .hot_seats = true, .start = start, .players_changed = players_changed };
  for (int p = 0; p < NP; p++) d.seat_names[p] = SEAT_NAME[p], d.seat_colors[p] = cw_hex(COL[p]);
  cw_shell_init(&d);
  /* #endregion */
  static const char *const NAMES[A_COUNT] = { "left", "right", "up", "down", "place" };
  static const char *const BINDS[A_COUNT] = {
    "key:left, key:a, pad:left, axis:lx-@0.5", "key:right, key:d, pad:right, axis:lx+@0.5",
    "key:up, key:w, pad:up, axis:ly-@0.5", "key:down, key:s, pad:down, axis:ly+@0.5", "key:space, key:return, pad:south",
  };
  for (int a = 0; a < A_COUNT; a++) {
    G.act[a] = cw_action_add(NAMES[a], 0.5f);
    cw_action_bind_text(G.act[a], BINDS[a]);
  }
  G.board = cw_hex_map_new(sizeof(int8_t));
  cw_hex_spiral(cw_hex_make(0, 0), RADIUS, G.cells, CELLS);
  G.lay = (cw_hex_layout){ CW_HEX_POINTY, { 40, 40 }, { 480, 290 } };
  cw_debug_i32("game.seats", &G.s.seats);
  cw_debug_field("game.cpu", G.s.cpu, CW_DBG_I32, NP);
  cw_debug_i32("game.turn", &G.s.turn);
  cw_debug_i32("game.placed", &G.s.placed);
  cw_debug_i32("game.round", &G.s.round);
  cw_debug_i32("game.winner", &G.s.winner);
  cw_debug_i32("game.cpu_moves", &G.s.cpu_moves);
  cw_debug_field("game.score", G.s.score, CW_DBG_I32, NP);
  cw_debug_field("game.wins", G.s.wins, CW_DBG_I32, NP);
  cw_debug_field("game.placed_by", G.s.placed_by, CW_DBG_I32, NP);
  cw_debug_i32("game.stray", &G.stray);
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_u32("game.board_hash", &G.board_hash);
  cw_debug_u32("game.state_hash", &G.state_hash);
  cw_debug_i32("game.replaying", &G.replaying);
  cw_debug_i32("game.replay_done", &G.replay_done);
  cw_debug_i32("game.replay_match", &G.replay_match);
  cw_debug_i32("game.replay_hash_match", &G.replay_hash_match);
  cw_debug_command("replay", "replay: replay the match from its start out of its own recording, then compare (game.replay_match)", cmd_replay);
}

static void fixed_update(void *user, double dt) {
  (void)user, (void)dt;
  const char *seats = !G.started_by_env && cw_shell_screen_now() == CW_SHELL_TITLE ? cw_test_env("HEX_BOARD_SEATS") : NULL;
  if (seats && *seats) {
    int cpu[NP], n = 0;
    for (const char *c = seats; *c && n < NP; n++) {
      cpu[n] = atoi(c);
      c = strchr(c, ',');
      if (!c) { n++; break; }
      c++;
    }
    G.started_by_env = true;
    cw_shell_start_seats(n, cpu);
  }
  if (!cw_shell_update()) return;
  if (G.fresh) { /* the tick the match began in is not recorded: play starts at the next */
    G.fresh = false;
    return;
  }
  if (G.replaying && !cw_input_replay_active()) { /* the recording has run out: the replay reached the moment of the request */
    hash_state();
    G.replay_match = G.state_hash == G.want_state;
    G.replay_hash_match = cw_input_replay_hash() == G.want_input;
    G.replaying = 0;
    G.replay_done = 1;
    cw_info("hex_board replay: state %s, input hash %s", G.replay_match ? "same" : "DIFFERENT", G.replay_hash_match ? "same" : "DIFFERENT");
  }
/* #region tick */
  /* the CPUs press first (this tick's actions), then the rules read every seat alike */
  for (int p = 0; p < G.s.seats; p++)
    if (G.s.cpu[p] >= 0) cpu_tick(p, G.s.cpu[p]);
  rules_tick();
  cw_shell_set_turn(G.s.winner < 0 ? G.s.turn : -1); /* the turn passed: the pads follow it */
/* #endregion */
  G.ticks++;
  hash_state();
}

/* ── drawing ──────────────────────────────────────────────────────── */

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x16202aff) });
  cw_vec2 sz = cw_r2d_canvas_size();
  G.lay.origin = cw_v2(sz.x * 0.5f, sz.y * 0.54f);
  G.lay.size.x = G.lay.size.y = sz.y * 0.074f;
  bool play = cw_shell_screen_now() == CW_SHELL_PLAY || cw_shell_screen_now() == CW_SHELL_PAUSE;
  for (int i = 0; i < CELLS && play; i++) {
    cw_vec2 k[6];
    cw_hex_corners(&G.lay, G.cells[i], k);
    cw_vec2 c = cw_hex_to_point(&G.lay, G.cells[i]);
    for (int j = 0; j < 6; j++) k[j] = cw_v2(c.x + (k[j].x - c.x) * 0.93f, c.y + (k[j].y - c.y) * 0.93f); /* a gap between hexes */
    int o = owner(G.cells[i]);
    cw_r2d_polygon(k, 6, o >= 0 ? cw_hex(COL[o]) : cw_hex(0x2c3a48ff));
    if (o < 0) cw_r2d_polygon_lines(k, 6, 2, cw_hex(0x45586bff));
  }
  const cw_raw_input *raw = cw_input_raw();
  if (play && raw) { /* the hex under the mouse (drawing only) */
    cw_axial h = cw_hex_from_point(&G.lay, cw_r2d_window_to_canvas(cw_v2(raw->mouse_x, raw->mouse_y)));
    if (on_board(h)) {
      cw_vec2 k[6];
      cw_hex_corners(&G.lay, h, k);
      cw_r2d_polygon_lines(k, 6, 2, cw_rgba(1, 1, 1, 0.35f));
    }
  }
  if (play && G.s.winner < 0 && G.s.seats > 0) { /* the cursor of the player whose turn it is */
    cw_vec2 k[6];
    cw_hex_corners(&G.lay, G.s.cursor[G.s.turn], k);
    cw_r2d_polygon_lines(k, 6, sz.y * 0.008f, cw_hex(COL[G.s.turn]));
    cw_r2d_polygon_lines(k, 6, sz.y * 0.003f, CW_WHITE);
  }
  if (cw_shell_hud_shown()) {
    float ts = sz.y * 0.032f;
    for (int p = 0; p < G.s.seats; p++) {
      char line[48];
      cw_shell_cpu_level lv;
      bool cpu = cw_shell_seat_cpu(p, &lv);
      snprintf(line, sizeof line, "%s%s   group %d   wins %d", SEAT_NAME[p], cpu ? (lv == CW_SHELL_CPU_EASY ? " (CPU Easy)" : lv == CW_SHELL_CPU_NORMAL ? " (CPU Normal)" : " (CPU Hard)") : "",
               G.s.score[p], G.s.wins[p]);
      cw_vec2 at = cw_v2(sz.x * 0.03f, sz.y * 0.06f + (float)p * ts * 1.5f);
      if (p == G.s.turn && G.s.winner < 0) cw_r2d_rect(cw_rect_make(at.x - ts * 0.4f, at.y + ts * 0.15f, ts * 0.25f, ts * 0.8f), cw_hex(COL[p]));
      cw_r2d_text(cw_ui_font_sans(), line, at, &(cw_text_style){ .size = ts, .color = cw_hex(COL[p]) });
    }
    if (G.s.winner >= 0) {
      char line[48];
      snprintf(line, sizeof line, "Player %d wins the round", G.s.winner + 1);
      cw_r2d_text(cw_ui_font_sans(), line, cw_v2(sz.x * 0.5f - sz.y * 0.3f, sz.y * 0.06f), &(cw_text_style){ .size = sz.y * 0.04f, .color = CW_WHITE });
    } else if (G.s.cpu[G.s.turn] < 0)
      cw_prompt_text(cw_ui_font_sans(), "{left|right} {up|down} move    {place} place", G.s.turn,
                     cw_v2(sz.x * 0.5f, sz.y * 0.93f), sz.y * 0.03f, cw_rgba(1, 1, 1, 0.85f), CW_ALIGN_CENTER);
  }
  cw_r2d_end();
  cw_shell_draw();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){
    .title = "cartwheel hex board",
    .width = 960,
    .height = 540,
    .init = init,
    .fixed_update = fixed_update,
    .render = render,
    .save = cw_shell_save_region(),
    .save_size = cw_shell_save_region_size(),
    .debug = true,
  };
}

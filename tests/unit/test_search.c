/* Game-tree search (cartwheel/search.h), each result against an independent answer:
 *   - tic-tac-toe: from every reachable position, minimax's move keeps the position's value as a plain memoised
 *     negamax computes it (so it never loses from any position), with and without the transposition table and
 *     with undo; minimax against itself draws;
 *   - connect four: positions with one winning move and one forced block, checked by a brute-force search, solved;
 *   - one player moving twice in a row: the right move needs max at both of its nodes (never turns by parity);
 *   - chance: the root values at a dice roll equal the weighted average computed by hand (expectimax);
 *   - MCTS against uniform random play over many seeded games: tic-tac-toe, a dice game for two and for three;
 *   - determinism: the same seed gives the same move, nodes and ranking; a search spread over ticks in slices gives
 *     the move and node count of an all-at-once run with the same node budget, minimax and MCTS; the background
 *     job gives the same move; a millisecond budget is kept; seeded mistakes at their rate.
 * Control (CW_TEST_SEARCH_WEAK=1): the late case runs the strength checks through a weakened player (minimax two
 * plies deep on tic-tac-toe, the twice-in-a-row game seen through a turn() that alternates, the MCTS seat replaced by random
 * play, the baseline the margin is measured against); each must fail. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/search.h"
#include "test.h"

static double clock_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* ── tic-tac-toe ───────────────────────────────────────────────────── */
typedef struct ttt {
  int8_t c[9]; /* 0 empty, 1 X (player 0), 2 O (player 1) */
  int8_t turn, moves, winner; /* winner: 0 none, 1 X, 2 O, 3 draw */
  int8_t pad;
} ttt;

static const int LINES[8][3] = { { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 }, { 0, 3, 6 },
                                 { 1, 4, 7 }, { 2, 5, 8 }, { 0, 4, 8 }, { 2, 4, 6 } };

static int ttt_judge(const ttt *t) {
  for (int l = 0; l < 8; l++) {
    int a = t->c[LINES[l][0]];
    if (a && a == t->c[LINES[l][1]] && a == t->c[LINES[l][2]]) return a;
  }
  return t->moves == 9 ? 3 : 0;
}
static int ttt_moves(const void *st, cw_search_move *out, int cap, void *u) {
  (void)u;
  const ttt *t = st;
  int n = 0;
  for (int i = 0; i < 9 && n < cap; i++)
    if (!t->c[i]) out[n++] = (cw_search_move)i;
  return n;
}
static void ttt_apply(void *st, cw_search_move m, void *u) {
  (void)u;
  ttt *t = st;
  t->c[m] = (int8_t)(t->turn + 1);
  t->moves++;
  t->turn ^= 1;
  t->winner = (int8_t)ttt_judge(t);
}
static void ttt_undo(void *st, cw_search_move m, void *u) {
  (void)u;
  ttt *t = st;
  t->c[m] = 0;
  t->moves--;
  t->turn ^= 1;
  t->winner = 0;
}
static int ttt_turn(const void *st, void *u) { (void)u; return ((const ttt *)st)->turn; }
static bool ttt_over(const void *st, void *u) { (void)u; return ((const ttt *)st)->winner != 0; }
static void ttt_eval(const void *st, float *s, void *u) {
  (void)u;
  const ttt *t = st;
  s[0] = s[1] = 0;
  if (t->winner == 1 || t->winner == 2) {
    int w = t->winner - 1;
    s[w] = CW_SEARCH_WIN - t->moves;
    s[1 - w] = -(CW_SEARCH_WIN - t->moves);
  }
}
static const cw_search_game TTT = { .players = 2, .state_size = sizeof(ttt), .moves = ttt_moves, .apply = ttt_apply,
                                    .turn = ttt_turn, .terminal = ttt_over, .evaluate = ttt_eval };

static ttt ttt_empty(void) {
  ttt t;
  memset(&t, 0, sizeof t);
  return t;
}

/* the independent answer: memoised negamax, the value for the player to move (+1 win, 0 draw, -1 loss) */
static int8_t ttt_memo[19683];
static bool ttt_memo_set[19683];
static int ttt_code(const ttt *t) {
  int c = 0;
  for (int i = 0; i < 9; i++) c = c * 3 + t->c[i];
  return c;
}
static int ttt_solve(ttt *t) {
  int code = ttt_code(t);
  if (ttt_memo_set[code]) return ttt_memo[code];
  int v;
  if (t->winner == 3) v = 0;
  else if (t->winner) v = -1; /* the player who just moved won */
  else {
    v = -2;
    for (int i = 0; i < 9; i++)
      if (!t->c[i]) {
        ttt_apply(t, (cw_search_move)i, NULL);
        int cv = -ttt_solve(t);
        ttt_undo(t, (cw_search_move)i, NULL);
        if (cv > v) v = cv;
      }
  }
  ttt_memo_set[code] = true;
  ttt_memo[code] = (int8_t)v;
  return v;
}

/* every reachable position that is not over */
static int ttt_collect(ttt *t, ttt *out, int n, bool *seen) {
  int code = ttt_code(t);
  if (seen[code]) return n;
  seen[code] = true;
  if (t->winner) return n;
  out[n++] = *t;
  for (int i = 0; i < 9; i++)
    if (!t->c[i]) {
      ttt_apply(t, (cw_search_move)i, NULL);
      n = ttt_collect(t, out, n, seen);
      ttt_undo(t, (cw_search_move)i, NULL);
    }
  return n;
}

static ttt *g_positions;
static int g_npositions;
static void ttt_positions(void) {
  if (g_positions) return;
  bool *seen = calloc(19683, 1);
  g_positions = malloc(sizeof(ttt) * 6000);
  ttt e = ttt_empty();
  g_npositions = ttt_collect(&e, g_positions, 0, seen);
  free(seen);
}

/* moves from every reachable position that lose value against the perfect answer */
static int ttt_blunders(const cw_search_game *g, const cw_search_options *o, int64_t *nodes) {
  ttt_positions();
  cw_search *s = cw_search_new(g, o);
  int bad = 0;
  for (int i = 0; i < g_npositions; i++) {
    ttt t = g_positions[i];
    int want = ttt_solve(&t);
    cw_search_move m = CW_SEARCH_MOVE_NONE;
    if (!cw_search_run(s, &t, &m) || m >= 9 || t.c[m]) { bad++; continue; }
    cw_search_stats st;
    cw_search_get_stats(s, &st);
    if (nodes) *nodes += st.nodes;
    ttt_apply(&t, m, NULL);
    if (-ttt_solve(&t) != want) bad++;
  }
  cw_search_free(s);
  return bad;
}

static void test_tictactoe(void) {
  ttt_positions();
  CHECK_EQ_INT(g_npositions, 4520); /* 5478 legal positions, 958 of them over */
  ttt e = ttt_empty();
  CHECK_EQ_INT(ttt_solve(&e), 0);
  cw_search_options o = { .kind = CW_SEARCH_MINIMAX };
  int64_t nodes = 0;
  CHECK_EQ_INT(ttt_blunders(&TTT, &o, &nodes), 0);
  FOLD(nodes);
  /* no table */
  cw_search_options nt = { .kind = CW_SEARCH_MINIMAX, .tt_entries = -1 };
  CHECK_EQ_INT(ttt_blunders(&TTT, &nt, NULL), 0);
  /* undo instead of copies: the same moves and nodes */
  cw_search_game gu = TTT;
  gu.undo = ttt_undo;
  int64_t nodes_u = 0;
  CHECK_EQ_INT(ttt_blunders(&gu, &o, &nodes_u), 0);
  CHECK_EQ_INT(nodes_u, nodes);
  /* minimax against itself draws, and finds the quickest win when one is there */
  cw_search *s = cw_search_new(&TTT, &o);
  ttt t = ttt_empty();
  while (!t.winner) {
    cw_search_move m;
    CHECK(cw_search_run(s, &t, &m));
    ttt_apply(&t, m, NULL);
  }
  CHECK_EQ_INT(t.winner, 3);
  /* X: 0 1 _ / O: 3 4 _ : X wins at once at 2 (O would win at 5) */
  t = ttt_empty();
  int seq[4] = { 0, 3, 1, 4 };
  for (int i = 0; i < 4; i++) ttt_apply(&t, (cw_search_move)seq[i], NULL);
  cw_search_move m;
  CHECK(cw_search_run(s, &t, &m) && m == 2);
  cw_search_stats st;
  cw_search_get_stats(s, &st);
  CHECK(st.best_value > CW_SEARCH_WIN);
  /* a finished game and a position with one move */
  ttt_apply(&t, 2, NULL);
  CHECK(!cw_search_start(s, &t));
  CHECK(!cw_search_result(s, &m));
  cw_search_free(s);
  /* refused games */
  cw_search_game bad = TTT;
  bad.evaluate = NULL;
  CHECK(cw_search_new(&bad, &o) == NULL);
  bad = TTT;
  bad.players = 5;
  CHECK(cw_search_new(&bad, &o) == NULL);
  cw_search_free(NULL);
}

/* ── connect four ──────────────────────────────────────────────────── */
#define C4W 7
#define C4H 6
typedef struct c4 {
  int8_t b[C4H][C4W]; /* row 0 at the bottom; 0 empty, 1 player 0, 2 player 1 */
  int8_t h[C4W];
  int8_t turn, moves, winner, pad;
} c4;

static bool c4_line(const c4 *c, int col, int row) {
  int p = c->b[row][col];
  static const int D[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };
  for (int d = 0; d < 4; d++) {
    int n = 1;
    for (int k = 1; k < 4; k++) {
      int x = col + D[d][0] * k, y = row + D[d][1] * k;
      if (x < 0 || x >= C4W || y < 0 || y >= C4H || c->b[y][x] != p) break;
      n++;
    }
    for (int k = 1; k < 4; k++) {
      int x = col - D[d][0] * k, y = row - D[d][1] * k;
      if (x < 0 || x >= C4W || y < 0 || y >= C4H || c->b[y][x] != p) break;
      n++;
    }
    if (n >= 4) return true;
  }
  return false;
}
static int c4_moves(const void *st, cw_search_move *out, int cap, void *u) {
  (void)u;
  const c4 *c = st;
  static const int ORDER[C4W] = { 3, 2, 4, 1, 5, 0, 6 };
  int n = 0;
  for (int i = 0; i < C4W && n < cap; i++)
    if (c->h[ORDER[i]] < C4H) out[n++] = (cw_search_move)ORDER[i];
  return n;
}
static void c4_apply(void *st, cw_search_move m, void *u) {
  (void)u;
  c4 *c = st;
  int col = (int)m, row = c->h[col]++;
  c->b[row][col] = (int8_t)(c->turn + 1);
  c->moves++;
  if (c4_line(c, col, row)) c->winner = (int8_t)(c->turn + 1);
  else if (c->moves == C4W * C4H) c->winner = 3;
  c->turn ^= 1;
}
static int c4_turn(const void *st, void *u) { (void)u; return ((const c4 *)st)->turn; }
static bool c4_over(const void *st, void *u) { (void)u; return ((const c4 *)st)->winner != 0; }
static void c4_eval(const void *st, float *s, void *u) {
  (void)u;
  const c4 *c = st;
  s[0] = s[1] = 0;
  if (c->winner == 1 || c->winner == 2) {
    int w = c->winner - 1;
    s[w] = CW_SEARCH_WIN - c->moves;
    s[1 - w] = -(CW_SEARCH_WIN - c->moves);
    return;
  }
  for (int y = 0; y < C4H; y++) /* the centre column is worth a little */
    if (c->b[y][3]) s[c->b[y][3] - 1] += 1;
}
static const cw_search_game C4 = { .players = 2, .state_size = sizeof(c4), .moves = c4_moves, .apply = c4_apply,
                                   .turn = c4_turn, .terminal = c4_over, .evaluate = c4_eval };

static c4 c4_from(const char *cols) { /* a move list as column digits */
  c4 c;
  memset(&c, 0, sizeof c);
  for (const char *p = cols; *p; p++) c4_apply(&c, (cw_search_move)(*p - '0'), NULL);
  return c;
}

/* brute force (no pruning, no table): +1 when the player to move wins within depth plies, -1 loses, 0 neither */
static int c4_brute(c4 *c, int depth) {
  if (c->winner) return c->winner == 3 ? 0 : -1;
  if (depth == 0) return 0;
  int best = -2;
  for (int col = 0; col < C4W; col++) {
    if (c->h[col] >= C4H) continue;
    c4 k = *c;
    c4_apply(&k, (cw_search_move)col, NULL);
    int v = -c4_brute(&k, depth - 1);
    if (v > best) best = v;
  }
  return best;
}

static void test_connect4(void) {
  cw_search_options o = { .kind = CW_SEARCH_MINIMAX, .max_depth = 8 };
  cw_search *s = cw_search_new(&C4, &o);
  /* player 0 has the bottom row at columns 1 and 2 with both sides open: only column 3 makes two threats (a forced
   * win in three plies); column 0 and 4 make one, which player 1 blocks */
  c4 c = c4_from("1122");
  int winners = 0, win_col = -1;
  for (int col = 0; col < C4W; col++) {
    c4 k = c;
    c4_apply(&k, (cw_search_move)col, NULL);
    if (-c4_brute(&k, 6) == 1) winners++, win_col = col;
  }
  CHECK_EQ_INT(winners, 1);
  CHECK_EQ_INT(win_col, 3);
  cw_search_move m;
  CHECK(cw_search_run(s, &c, &m) && (int)m == 3);
  cw_search_stats st;
  cw_search_get_stats(s, &st);
  CHECK(st.best_value > CW_SEARCH_WIN);
  FOLD(st.nodes);
  /* forced wins four of its own moves away (seven plies) that no five-ply look sees, one winning move each, found
   * and checked by the brute force: one for each player */
  static const struct { const char *seq; int col; } DEEP[2] = { { "451332125650", 4 }, { "123113341", 2 } };
  for (int d = 0; d < 2; d++) {
    c = c4_from(DEEP[d].seq);
    CHECK_EQ_INT(c4_brute(&c, 5), 0);
    CHECK_EQ_INT(c4_brute(&c, 7), 1);
    int unique = 0, want = -1;
    for (int col = 0; col < C4W; col++) {
      c4 k = c;
      if (k.h[col] >= C4H) continue;
      c4_apply(&k, (cw_search_move)col, NULL);
      if (-c4_brute(&k, 6) == 1) unique++, want = col;
    }
    CHECK_EQ_INT(unique, 1);
    CHECK_EQ_INT(want, DEEP[d].col);
    CHECK(cw_search_run(s, &c, &m));
    CHECK_EQ_INT((int)m, DEEP[d].col);
    cw_search_get_stats(s, &st);
    CHECK(st.best_value > CW_SEARCH_WIN);
    FOLD(st.nodes);
  }
  /* player 1 must block: player 0 has three in column 0 */
  c = c4_from("0605");
  c4_apply(&c, 0, NULL); /* now 0 has three stacked in column 0, player 1 to move */
  CHECK(cw_search_run(s, &c, &m) && (int)m == 0);
  cw_search_free(s);
}

/* ── one player moving twice in a row ──────────────────────────────── */
typedef struct twice {
  int8_t stage, turn; /* stage 0: player 0 picks A (moves again) or B (ends, 0); stage 1: player 0 picks +5 or -10 */
  int8_t over, pad;
  int32_t val;
} twice;
static int tw_moves(const void *st, cw_search_move *out, int cap, void *u) {
  (void)u; (void)st;
  if (cap < 2) return 0;
  out[0] = 0;
  out[1] = 1;
  return 2;
}
static void tw_apply(void *st, cw_search_move m, void *u) {
  (void)u;
  twice *t = st;
  if (t->stage == 0) {
    if (m == 0) t->stage = 1; /* A: the same player decides again */
    else t->val = 0, t->over = 1, t->turn = 1;
  } else {
    t->val = m == 0 ? 5 : -10;
    t->over = 1;
    t->turn = 1;
  }
}
static int tw_turn(const void *st, void *u) { (void)u; return ((const twice *)st)->turn; }
static int tw_turn_parity(const void *st, void *u) { /* the control: turns assumed to alternate */
  (void)u;
  return ((const twice *)st)->stage & 1;
}
static bool tw_over(const void *st, void *u) { (void)u; return ((const twice *)st)->over; }
static void tw_eval(const void *st, float *s, void *u) {
  (void)u;
  const twice *t = st;
  s[0] = (float)t->val;
  s[1] = (float)-t->val;
}
static const cw_search_game TWICE = { .players = 2, .state_size = sizeof(twice), .moves = tw_moves, .apply = tw_apply,
                                      .turn = tw_turn, .terminal = tw_over, .evaluate = tw_eval };

/* the move minimax picks at the start: A (0) is right, worth +5 to player 0 */
static int twice_pick(bool parity, cw_search_kind kind) {
  cw_search_game g = TWICE;
  if (parity) g.turn = tw_turn_parity;
  cw_search_options o = { .kind = kind, .max_depth = 4 };
  cw_search *s = cw_search_new(&g, &o);
  twice t;
  memset(&t, 0, sizeof t);
  cw_search_move m = CW_SEARCH_MOVE_NONE;
  cw_search_run(s, &t, &m);
  cw_search_free(s);
  return (int)m;
}

static void test_twice(void) {
  CHECK_EQ_INT(twice_pick(false, CW_SEARCH_MINIMAX), 0);
  cw_search_options o = { .kind = CW_SEARCH_MINIMAX };
  cw_search *s = cw_search_new(&TWICE, &o);
  twice t;
  memset(&t, 0, sizeof t);
  cw_search_move m;
  CHECK(cw_search_run(s, &t, &m) && m == 0);
  cw_search_stats st;
  cw_search_get_stats(s, &st);
  CHECK(st.best_value == 10.0f); /* 5 - (-5): player 0's lead */
  cw_search_free(s);
  /* the same in a 3-player max-n game and paranoid: player 0 moves twice */
  cw_search_game g3 = TWICE;
  g3.players = 3;
  CHECK_EQ_INT(twice_pick(false, CW_SEARCH_MINIMAX), 0);
  cw_search *s3 = cw_search_new(&g3, &o);
  CHECK(cw_search_run(s3, &t, &m) && m == 0);
  cw_search_free(s3);
  cw_search_options op = { .kind = CW_SEARCH_MINIMAX, .paranoid = true };
  s3 = cw_search_new(&g3, &op);
  CHECK(cw_search_run(s3, &t, &m) && m == 0);
  cw_search_free(s3);
}

/* ── a dice game (pig): roll to add to the turn's total, a 1 loses it; hold to bank it. First to PIG_GOAL ───── */
#define PIG_GOAL 20
typedef struct pig {
  int16_t score[4];
  int16_t total;
  int8_t turn, players, pending, over;
} pig;
enum { PIG_ROLL, PIG_HOLD };
static int pig_moves(const void *st, cw_search_move *out, int cap, void *u) {
  (void)u; (void)st;
  if (cap < 2) return 0;
  out[0] = PIG_ROLL;
  out[1] = PIG_HOLD;
  return 2;
}
static void pig_next(pig *p) {
  p->total = 0;
  p->turn = (int8_t)((p->turn + 1) % p->players);
}
static void pig_apply(void *st, cw_search_move m, void *u) {
  (void)u;
  pig *p = st;
  if (p->pending) { /* a die face 1..6 */
    p->pending = 0;
    if (m == 1) pig_next(p);
    else p->total = (int16_t)(p->total + (int)m);
    if (p->score[p->turn] + p->total >= PIG_GOAL) { /* banks a winning total at once */
      p->score[p->turn] = (int16_t)(p->score[p->turn] + p->total);
      p->over = 1;
    }
    return;
  }
  if (m == PIG_ROLL) p->pending = 1;
  else {
    p->score[p->turn] = (int16_t)(p->score[p->turn] + p->total);
    pig_next(p);
  }
}
static int pig_turn(const void *st, void *u) {
  (void)u;
  const pig *p = st;
  return p->pending ? CW_SEARCH_CHANCE : p->turn;
}
static bool pig_over(const void *st, void *u) { (void)u; return ((const pig *)st)->over; }
static void pig_eval(const void *st, float *s, void *u) {
  (void)u;
  const pig *p = st;
  for (int i = 0; i < p->players; i++) {
    if (p->over) s[i] = p->score[i] >= PIG_GOAL ? CW_SEARCH_WIN : CW_SEARCH_LOSS;
    else s[i] = p->score[i] + (i == p->turn ? 0.5f * p->total : 0);
  }
}
static int pig_outcomes(const void *st, cw_search_move *out, uint32_t *w, int cap, void *u) {
  (void)u; (void)st;
  if (cap < 6) return 0;
  for (int i = 0; i < 6; i++) out[i] = (cw_search_move)(i + 1), w[i] = 1;
  return 6;
}
static const cw_search_game PIG = { .players = 2, .state_size = sizeof(pig), .moves = pig_moves, .apply = pig_apply,
                                    .turn = pig_turn, .terminal = pig_over, .evaluate = pig_eval,
                                    .outcomes = pig_outcomes };

static pig pig_new(int players) {
  pig p;
  memset(&p, 0, sizeof p);
  p.players = (int8_t)players;
  return p;
}

static void test_chance(void) {
  /* greedy (depth 1): each root move's value is the weighted average over the die (expectimax), by hand */
  pig p = pig_new(2);
  p.score[0] = 4, p.score[1] = 10, p.total = 6;
  cw_search_options o = { .kind = CW_SEARCH_GREEDY };
  cw_search *s = cw_search_new(&PIG, &o);
  cw_search_move m;
  CHECK(cw_search_run(s, &p, &m));
  cw_search_root_move rm[4];
  int n = cw_search_root_moves(s, rm, 4);
  CHECK_EQ_INT(n, 2);
  float roll = 0;
  for (int f = 1; f <= 6; f++) { /* player 0's lead after the roll */
    float me = f == 1 ? 4 : 4 + 0.5f * (6 + f), other = 10;
    roll += (me - other) / 6;
  }
  float hold = 10 - 10;                             /* banks 6: 10 against 10 */
  for (int i = 0; i < n; i++) {
    float want = rm[i].move == PIG_ROLL ? roll : hold;
    CHECK(fabsf(rm[i].value - want) < 1e-4f);
  }
  CHECK(m == PIG_HOLD); /* hold 0 beats roll -0.25 */
  /* a root at a pending roll is refused: a chance event is never a decision */
  p.pending = 1;
  CHECK(!cw_search_start(s, &p));
  cw_search_free(s);
  /* chance() instead of outcomes: minimax samples one face per chance node from the seed, the same every run */
  cw_search_game gs = PIG;
  gs.outcomes = NULL;
  p.pending = 0;
  cw_search_options om = { .kind = CW_SEARCH_MINIMAX, .max_depth = 3, .seed = 5 };
  s = cw_search_new(&gs, &om);
  CHECK(cw_search_run(s, &p, &m)); /* no chance hook at all: the chance node is scored as it stands */
  cw_search_free(s);
}

/* a sampler for gs above (chance() form) */
static void pig_chance(void *st, cw_rng *rng, void *u) { pig_apply(st, (cw_search_move)cw_rng_int(rng, 1, 6), u); }

/* ── matches against uniform random play ───────────────────────────── */
typedef struct agent {
  cw_search *s; /* NULL: uniform random */
} agent;

/* plays one game; returns the winner (0..players-1), or -1 for a draw */
static int play(const cw_search_game *g, void *state, agent *agents, cw_rng *rng, int64_t *nodes) {
  cw_search_move mv[CW_SEARCH_MAX_MOVES];
  uint32_t w[CW_SEARCH_MAX_MOVES];
  for (int ply = 0; ply < 2000 && !g->terminal(state, g->user); ply++) {
    int t = g->turn(state, g->user);
    if (t == CW_SEARCH_CHANCE) {
      int n = g->outcomes(state, mv, w, CW_SEARCH_MAX_MOVES, g->user);
      g->apply(state, mv[cw_rng_int(rng, 0, n - 1)], g->user); /* every outcome here weighs the same */
      continue;
    }
    cw_search_move m;
    if (agents[t].s) {
      if (!cw_search_run(agents[t].s, state, &m)) return -2;
      cw_search_stats st;
      cw_search_get_stats(agents[t].s, &st);
      if (nodes) *nodes += st.nodes;
    } else {
      int n = g->moves(state, mv, CW_SEARCH_MAX_MOVES, g->user);
      m = mv[cw_rng_int(rng, 0, n - 1)];
    }
    g->apply(state, m, g->user);
  }
  float s[CW_SEARCH_MAX_PLAYERS] = { 0 };
  g->evaluate(state, s, g->user);
  int best = 0, ties = 0;
  for (int p = 1; p < g->players; p++) {
    if (s[p] > s[best]) best = p, ties = 0;
    else if (s[p] == s[best]) ties++;
  }
  return ties ? -1 : best;
}

typedef struct tally {
  int games, wins, losses, draws;
} tally;

/* the searching player takes every seat in turn against random players */
static tally versus_random(const cw_search_game *g, const cw_search_options *o, int games, uint64_t seed,
                           void (*reset)(void *state, int players), int64_t *nodes, bool baseline) {
  tally t = { 0 };
  cw_search *s = cw_search_new(g, o);
  cw_rng rng;
  cw_rng_seed(&rng, seed);
  uint8_t state[256];
  for (int i = 0; i < games; i++) {
    agent a[CW_SEARCH_MAX_PLAYERS] = { { 0 } };
    int seat = i % g->players;
    a[seat].s = baseline ? NULL : s; /* baseline: the seat plays random too (the control) */
    reset(state, g->players);
    int w = play(g, state, a, &rng, nodes);
    t.games++;
    if (w == seat) t.wins++;
    else if (w < 0) t.draws++;
    else t.losses++;
  }
  cw_search_free(s);
  return t;
}

static void reset_ttt(void *st, int players) {
  (void)players;
  *(ttt *)st = ttt_empty();
}
static void reset_pig(void *st, int players) { *(pig *)st = pig_new(players); }

static int g_mcts_ttt_wins, g_mcts_ttt_losses, g_mcts_pig_wins, g_mcts_pig3_wins;

static void test_mcts_vs_random(void) {
  int64_t nodes = 0;
  cw_search_options o = { .kind = CW_SEARCH_MCTS, .max_nodes = 6000, .seed = 11 };
  tally t = versus_random(&TTT, &o, 60, 1, reset_ttt, &nodes, false);
  cw_info("search: MCTS against random tic-tac-toe: %d wins, %d draws, %d losses of %d", t.wins, t.draws, t.losses,
          t.games);
  g_mcts_ttt_wins = t.wins, g_mcts_ttt_losses = t.losses;
  CHECK(t.wins >= 45); /* random against random wins about 44% (X 58%, O 29%) */
  CHECK(t.losses <= 1);
  FOLD(t);
  cw_search_options op = { .kind = CW_SEARCH_MCTS, .max_nodes = 4000, .seed = 12 };
  tally tp = versus_random(&PIG, &op, 100, 2, reset_pig, &nodes, false);
  cw_info("search: MCTS against random at the dice game: %d wins of %d", tp.wins, tp.games);
  g_mcts_pig_wins = tp.wins;
  CHECK(tp.wins >= 75); /* evens would be 50 */
  FOLD(tp);
  cw_search_game g3 = PIG;
  g3.players = 3;
  tally t3 = versus_random(&g3, &op, 90, 3, reset_pig, &nodes, false);
  cw_info("search: MCTS against two random players at the dice game: %d wins of %d", t3.wins, t3.games);
  g_mcts_pig3_wins = t3.wins;
  CHECK(t3.wins >= 50); /* a fair share would be 30 */
  FOLD(t3);
  /* minimax with expectimax plays the dice game too, two and three players, max-n and paranoid */
  cw_search_options om = { .kind = CW_SEARCH_MINIMAX, .max_depth = 4 };
  tally tm = versus_random(&PIG, &om, 60, 4, reset_pig, &nodes, false);
  cw_info("search: minimax (expectimax) against random at the dice game: %d wins of %d", tm.wins, tm.games);
  CHECK(tm.wins >= 42);
  FOLD(tm);
  tally tn = versus_random(&g3, &om, 60, 5, reset_pig, &nodes, false);
  om.paranoid = true;
  tally tq = versus_random(&g3, &om, 60, 5, reset_pig, &nodes, false);
  cw_info("search: three players: max-n %d, paranoid %d wins of 60", tn.wins, tq.wins);
  CHECK(tn.wins >= 30 && tq.wins >= 30);
  FOLD(tn);
  FOLD(tq);
  FOLD(nodes);
}

/* ── determinism, ticks, background, time, mistakes ────────────────── */
static bool same_run(const cw_search_game *g, const cw_search_options *o, const void *state, int64_t slice) {
  cw_search *a = cw_search_new(g, o), *b = cw_search_new(g, o);
  cw_search_move ma = CW_SEARCH_MOVE_NONE, mb = CW_SEARCH_MOVE_NONE;
  cw_search_run(a, state, &ma);
  cw_search_start(b, state);
  int steps = 0;
  while (!cw_search_step(b, 0, slice) && steps < 1000000) steps++;
  cw_search_result(b, &mb);
  cw_search_stats sa, sb;
  cw_search_get_stats(a, &sa);
  cw_search_get_stats(b, &sb);
  cw_search_root_move ra[16], rb[16];
  int na = cw_search_root_moves(a, ra, 16), nb = cw_search_root_moves(b, rb, 16);
  bool same = ma == mb && sa.nodes == sb.nodes && sa.best == sb.best && na == nb && sb.steps > 3;
  for (int i = 0; i < na && same; i++)
    same = ra[i].move == rb[i].move && ra[i].value == rb[i].value && ra[i].visits == rb[i].visits;
  if (!same)
    cw_error("search: sliced run differs: moves %llu/%llu nodes %lld/%lld steps %d", (unsigned long long)ma,
             (unsigned long long)mb, (long long)sa.nodes, (long long)sb.nodes, sb.steps);
  cw_search_free(a);
  cw_search_free(b);
  return same;
}

static void test_determinism(void) {
  c4 c = c4_from("3324");
  /* minimax with a node budget that ends mid-iteration, sliced in odd sizes */
  cw_search_options om = { .kind = CW_SEARCH_MINIMAX, .max_nodes = 30011 };
  CHECK(same_run(&C4, &om, &c, 97));
  CHECK(same_run(&C4, &om, &c, 1));
  /* MCTS */
  cw_search_options oc = { .kind = CW_SEARCH_MCTS, .max_nodes = 30011, .seed = 77 };
  CHECK(same_run(&C4, &oc, &c, 501));
  /* with chance and mistakes */
  pig p = pig_new(3);
  p.score[1] = 7;
  cw_search_game g3 = PIG;
  g3.players = 3;
  cw_search_options op = { .kind = CW_SEARCH_MCTS, .max_nodes = 20000, .seed = 9, .mistake = 0.5f };
  CHECK(same_run(&g3, &op, &p, 333));
  cw_search_options opm = { .kind = CW_SEARCH_MINIMAX, .max_nodes = 20000, .seed = 9, .mistake = 0.5f };
  CHECK(same_run(&g3, &opm, &p, 333));
  /* chance() sampling in minimax is seeded */
  cw_search_game gs = PIG;
  gs.outcomes = NULL;
  gs.chance = pig_chance;
  cw_search_options os = { .kind = CW_SEARCH_MINIMAX, .max_depth = 6, .seed = 3 };
  CHECK(same_run(&gs, &os, &p, 50));
  /* the same seed: the same answer; another seed: another MCTS ranking */
  cw_search *a = cw_search_new(&C4, &oc);
  cw_search_move m1, m2;
  cw_search_run(a, &c, &m1);
  cw_search_root_move r1[7], r2[7];
  cw_search_root_moves(a, r1, 7);
  cw_search_run(a, &c, &m2);
  cw_search_root_moves(a, r2, 7);
  CHECK(m1 == m2 && memcmp(r1, r2, sizeof r1) == 0);
  FOLD(r1);
  cw_search_free(a);
  cw_search_options oc2 = oc;
  oc2.seed = 78;
  a = cw_search_new(&C4, &oc2);
  cw_search_run(a, &c, &m2);
  cw_search_root_moves(a, r2, 7);
  CHECK(memcmp(r1, r2, sizeof r1) != 0);
  cw_search_free(a);
  /* the background job: the same move as a run on this thread */
  a = cw_search_new(&C4, &oc);
  CHECK(cw_search_start_background(a, &c));
  double t0 = clock_ms();
  while (!cw_search_done(a) && clock_ms() - t0 < 60000) {}
  cw_search_move mbg = CW_SEARCH_MOVE_NONE;
  CHECK(cw_search_result(a, &mbg) && mbg == m1);
  cw_search_free(a);
}

static void test_time(void) {
  c4 c = c4_from("");
  /* a whole-search budget of 15 ms with no node limit to speak of */
  cw_search_options o = { .kind = CW_SEARCH_MCTS, .max_nodes = INT64_MAX / 2, .time_ms = 15, .seed = 1 };
  cw_search *s = cw_search_new(&C4, &o);
  double t0 = clock_ms();
  cw_search_move m;
  CHECK(cw_search_run(s, &c, &m));
  double took = clock_ms() - t0;
  cw_search_stats st;
  cw_search_get_stats(s, &st);
  cw_info("search: a 15 ms budget took %.2f ms (%lld nodes)", took, (long long)st.nodes);
  CHECK(took >= 15 && took < 15 + 25);
  CHECK(st.nodes > 100);
  cw_search_free(s);
  /* per-slice milliseconds: each step returns near its 3 ms */
  cw_search_options om = { .kind = CW_SEARCH_MINIMAX, .max_nodes = INT64_MAX / 2, .max_depth = 40 };
  s = cw_search_new(&C4, &om);
  cw_search_start(s, &c);
  double worst = 0;
  for (int i = 0; i < 5; i++) {
    t0 = clock_ms();
    bool done = cw_search_step(s, 3, 0);
    double dt = clock_ms() - t0;
    if (dt > worst) worst = dt;
    CHECK(!done);
  }
  cw_info("search: 3 ms slices took at most %.2f ms", worst);
  CHECK(worst < 3 + 10);
  cw_search_free(s);
}

static void test_mistakes(void) {
  ttt t = ttt_empty();
  cw_search_options o = { .kind = CW_SEARCH_MINIMAX, .max_depth = 9 };
  /* mistake 0: the best; mistake 1: never the best; 0.35: about that often, seeded */
  int made = 0;
  for (int i = 0; i < 200; i++) {
    o.seed = (uint64_t)i;
    o.mistake = 0.35f;
    cw_search *s = cw_search_new(&TTT, &o);
    cw_search_move m;
    cw_search_run(s, &t, &m);
    cw_search_stats st;
    cw_search_get_stats(s, &st);
    made += st.mistake_made;
    CHECK((m != st.best) == st.mistake_made);
    cw_search_free(s);
  }
  cw_info("search: %d mistakes in 200 at 0.35", made);
  CHECK(made >= 50 && made <= 90);
  FOLD(made);
  o.mistake = 1;
  o.mistake_spread = 1;
  cw_search *s = cw_search_new(&TTT, &o);
  cw_search_move m;
  cw_search_run(s, &t, &m);
  cw_search_stats st;
  cw_search_get_stats(s, &st);
  CHECK(st.mistake_made && m != st.best);
  /* a mistake comes from the next best: with spread 1 it is the second of the ranking */
  cw_search_root_move rm[9];
  int n = cw_search_root_moves(s, rm, 9);
  CHECK(n == 9 && rm[0].move == st.best && rm[1].move == m);
  cw_search_free(s);
  /* the levels */
  cw_search_options lv;
  cw_search_level_options(0, CW_SEARCH_MCTS, &lv);
  CHECK(lv.kind == CW_SEARCH_MCTS && lv.mistake > 0.3f && lv.max_nodes > 0);
  cw_search_level_options(2, CW_SEARCH_MINIMAX, &lv);
  CHECK(lv.mistake == 0 && lv.max_depth == 0);
  /* Hard plays tic-tac-toe perfectly from the start; Easy does not always */
  cw_search_level_options(0, CW_SEARCH_MINIMAX, &lv);
  int easy_bad = 0;
  for (int i = 0; i < 20; i++) {
    lv.seed = (uint64_t)i;
    cw_search *e = cw_search_new(&TTT, &lv);
    ttt x = g_positions[100 + i * 37];
    int want = ttt_solve(&x);
    cw_search_run(e, &x, &m);
    ttt_apply(&x, m, NULL);
    easy_bad += -ttt_solve(&x) != want;
    cw_search_free(e);
  }
  cw_info("search: Easy lost value in %d of 20 positions", easy_bad);
  CHECK(easy_bad > 0);
}

/* late (cw_test_env is read from frame 2): the strength checks once more, through the real player or, as the control,
 * through a weakened one, which must fail */
void cw_test_late(void) {
  bool weak = cw_test_env("CW_TEST_SEARCH_WEAK") != NULL;
  cw_search_options o = { .kind = CW_SEARCH_MINIMAX, .max_depth = weak ? 2 : 0 };
  int blunders = ttt_blunders(&TTT, &o, NULL);
  int twice = twice_pick(weak, CW_SEARCH_MINIMAX);
  cw_search_options oc = { .kind = CW_SEARCH_MCTS, .max_nodes = 4000, .seed = 12 };
  tally tp = versus_random(&PIG, &oc, 100, 2, reset_pig, NULL, weak);
  if (weak)
    cw_info("search control: %d tic-tac-toe blunders, twice-in-a-row picked %d, %d dice wins of 100", blunders, twice,
            tp.wins);
  CHECK_EQ_INT(blunders, 0);
  CHECK_EQ_INT(twice, 0);
  CHECK(tp.wins >= 75);
}

CW_TEST_SUITE("search", { "tictactoe", test_tictactoe }, { "connect4", test_connect4 }, { "twice", test_twice },
              { "chance", test_chance }, { "mcts_vs_random", test_mcts_vs_random },
              { "determinism", test_determinism }, { "time", test_time }, { "mistakes", test_mistakes })

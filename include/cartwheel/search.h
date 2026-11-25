/* cartwheel game-tree search: a computer player for turn-based games (board games, card games, puzzles).
 *
 * Two searches over the same game callbacks:
 *   - CW_SEARCH_MINIMAX: alpha-beta with iterative deepening, a transposition table keyed by the game's state hash
 *     and move ordering (the table's best move first, then the game's priority, the root by the last iteration's
 *     values). Two players: alpha-beta on score[me] - score[other]. Three or four players: max-n (every player
 *     picks what is best for itself) by default, or paranoid (me against everyone, on score[me] - the best other
 *     score) when cw_search_options.paranoid is set. Chance nodes (dice, draws) average their outcomes by weight
 *     (expectimax) when the game lists them (outcomes), else take one outcome sampled from the seed.
 *   - CW_SEARCH_MCTS: Monte Carlo tree search (UCT), for games with chance or more players. Open loop: the tree is
 *     keyed by moves, a chance event is sampled again on every iteration, so a hidden draw is never decided early.
 *     Playouts pick uniform moves (or the game's playout policy) for up to playout_depth moves, then score with
 *     evaluate. The reward (cw_search_options.reward) is the margin by default: a logistic of the player's lead
 *     over the best other player, 1 / (1 + e^(-lead / reward_scale)), so a win (CW_SEARCH_WIN) is 1 and a player
 *     behind still gains by closing the gap. CW_SEARCH_REWARD_PLACE (the place between the lowest and highest
 *     score, the only reward before v0.84.0) gives the last of three or four, and the one behind of two, 0 whatever
 *     it does: a CPU that falls behind then plays at random. See "The MCTS reward" below.
 *   - CW_SEARCH_GREEDY: every legal move applied once and evaluated (minimax at depth 1).
 *
 *   static const cw_search_game ttt = { .players = 2, .state_size = sizeof(board), .moves = ttt_moves,
 *                                       .apply = ttt_apply, .turn = ttt_turn, .terminal = ttt_over,
 *                                       .evaluate = ttt_eval };
 *   cw_search_options o;
 *   cw_search_level_options(level, CW_SEARCH_MINIMAX, &o);          // the shell's CPU Easy / Normal / Hard
 *   o.seed = match_seed ^ turn_number;
 *   cw_search *s = cw_search_new(&ttt, &o);
 *   cw_search_start(s, &board);                                     // when the CPU's turn begins
 *   // each fixed_update: a slice of work; true when the answer is final
 *   if (cw_search_step(s, 2.0f, 0)) { cw_search_move m; cw_search_result(s, &m); ... press it through actions ... }
 *
 * ── The game ──────────────────────────────────────────────────────────
 * A state is a block of state_size bytes the search copies (memcpy, or the game's copy). Keep it flat: no
 * pointers, padding zeroed (memset before filling), the game's random generator inside it. A move is a 64-bit code
 * the game packs (piece, from, to, a set of gems). A turn made of several decisions is one decision per move, and the
 * same player may move several times in a row: the search asks turn() at every node and never assumes turns
 * alternate. List legal moves in a fixed order (ties go to the earlier move).
 *
 * evaluate fills score[0..players-1], each from that player's view, larger better. At a terminal state it is the
 * result: put wins and losses in bands no heuristic reaches (CW_SEARCH_WIN, CW_SEARCH_LOSS) and subtract the number
 * of moves played from a win, so a faster win scores higher; the search does not adjust values by depth.
 *
 * Chance: when the next thing to happen is random (a roll, a card from a hidden deck, a market refill), turn()
 * answers CW_SEARCH_CHANCE. outcomes() lists the possible results as move codes with integer weights, and the search
 * plays one with the ordinary apply() (undo() takes it back) while turn() == CW_SEARCH_CHANCE. chance() instead
 * resolves the event itself from the search's random generator. Give either or both: minimax uses outcomes (all of
 * them, averaged) or else samples chance; MCTS samples chance, or else picks from outcomes by weight. A chance node
 * is never a player's decision: it is left out of mistakes, move ordering and the root ranking, and a root that is a
 * chance node is refused (resolve it in the match first).
 *
 * ── Budgets and determinism ───────────────────────────────────────────
 * Work is counted in nodes: one apply or one evaluate, in every kind. Given the same state, options and seed, the
 * answer depends only on max_nodes, max_depth and the game: never on how the work is split over ticks or which host
 * runs it. cw_search_step(s, ms, nodes) runs one slice: it stops at the slice's node count or its milliseconds, only
 * between whole units (a node in minimax, a whole iteration in MCTS), so a search spread over twenty ticks gives the
 * move an all-at-once run gives. A millisecond limit (the slice's ms, or options.time_ms over the whole search) ends
 * work by the wall clock, so with one the answer can differ per host and per run: keep gates, tests and replays on
 * node budgets (record the CPU's move through the input path, cw_input_set_actions, and a replay never searches).
 *
 * ── Difficulty ────────────────────────────────────────────────────────
 * Seeded mistakes over a real search: once the root moves are ranked, with probability `mistake` the move played is
 * another one, picked from the next `mistake_spread` best (weighted toward the better ones), drawn from the seed.
 * With mistakes on, minimax searches every root move with a full window so their values are exact. Shallower depth
 * and fewer nodes make a weaker player too; cw_search_level_options maps the shell's CPU levels to all three.
 *
 * ── The MCTS reward ───────────────────────────────────────────────────
 * reward_scale is in evaluate's units: the lead worth 0.73, a lead of twice that 0.88, the same behind 0.27. Set it
 * to a fifth or so of a typical winning margin in the game's points (the default, 4, is that for a race to 20).
 * Too small saturates: every lead past a few scales is 1 or 0, and a player far behind is blind again; too large
 * flattens the rewards toward 0.5 and UCT needs more nodes to tell moves apart. Measured on the dice game of
 * tests/unit/test_search.c (a race to 20, 4000 nodes a move), one margin seat against place seats and the other
 * way round, 480 / 720 / 960 games: at scale 4 the margin and the place were even with two players (242 and 239
 * wins of a fair 240), and the margin was stronger with three (300 against 202 of 240) and four (287 against 192);
 * against random seats the two won the same (365 and 363 of 400 with two players, 517 and 517 of 600 with three,
 * 643 and 651 of 800 with four). Scale 8 was stronger head to head with two (283 against 194) but lost 2-player
 * games to random seats the place wins (329 against 363 of 400); 16 and 32 lost 4-player games. A seat 30 points
 * behind with no way to win chose the move that closes the gap most in 17 of 20 searches (four players) and 18
 * (two) on the margin, 0 of 20 on the place.
 *
 * ── Threads ───────────────────────────────────────────────────────────
 * cw_search_step runs on the calling thread. cw_search_start_background runs the whole search on the job pool's
 * background queue (cartwheel/jobs.h) while the game keeps drawing; the callbacks then run on a worker, so they must
 * touch only the state they are given. Poll cw_search_done; with no workers the job runs at once on the caller. */
#ifndef CW_SEARCH_H
#define CW_SEARCH_H

#include "base.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_SEARCH_MAX_PLAYERS 4
#define CW_SEARCH_MAX_MOVES 512   /* the most legal moves (or chance outcomes) one node may list */
#define CW_SEARCH_MAX_PLY 128     /* the deepest line minimax follows (chance nodes count) */
#define CW_SEARCH_CHANCE (-1)     /* turn(): a random event is due */
#define CW_SEARCH_WIN 1.0e6f      /* evaluate bands for terminal states */
#define CW_SEARCH_LOSS (-1.0e6f)

typedef uint64_t cw_search_move; /* a game-packed decision or chance outcome */
#define CW_SEARCH_MOVE_NONE UINT64_MAX

typedef struct cw_search_game {
  int players;       /* 1..4 */
  size_t state_size; /* bytes the search keeps per copy */
  /* copies a state into a block of state_size bytes; NULL: memcpy */
  void (*copy)(void *dst, const void *src, void *user);
  /* the legal moves of the player to move, in a fixed order; returns the count (<= cap). Never 0 at a state that
   * is not terminal: a player with nothing to do gets a pass move */
  int (*moves)(const void *state, cw_search_move *out, int cap, void *user);
  /* plays a move (or, at a chance node, an outcome) in place, on the search's own copy */
  void (*apply)(void *state, cw_search_move move, void *user);
  /* optional: takes apply back exactly; minimax then works on one state instead of a copy per ply */
  void (*undo)(void *state, cw_search_move move, void *user);
  /* who decides next: 0..players-1, or CW_SEARCH_CHANCE. Not asked at terminal states */
  int (*turn)(const void *state, void *user);
  bool (*terminal)(const void *state, void *user);
  /* every player's value of the state, from that player's view (see the top of this file) */
  void (*evaluate)(const void *state, float score[CW_SEARCH_MAX_PLAYERS], void *user);
  /* optional: the transposition table key (whose turn it is included). NULL with copy NULL: cw_hash_bytes over the
   * state's bytes, seed 0; NULL with a copy: no table */
  uint64_t (*hash)(const void *state, void *user);
  /* optional: resolves a pending chance event in place from rng (a die rolled, a card drawn) */
  void (*chance)(void *state, cw_rng *rng, void *user);
  /* optional: move ordering for minimax, higher first (stable: equal priorities keep the list order) */
  float (*priority)(const void *state, cw_search_move move, void *user);
  /* optional: the MCTS playout policy, one of moves[0..n-1]; NULL: uniform */
  cw_search_move (*playout)(const void *state, const cw_search_move *moves, int n, cw_rng *rng, void *user);
  void *user; /* passed to every callback */
  /* optional: the outcomes of a pending chance event as move codes with integer weights (a die: six outcomes of
   * weight 1; a bag of 3 red and 2 blue: two outcomes of weight 3 and 2); returns the count (<= cap). The search
   * plays one with apply() while turn() == CW_SEARCH_CHANCE */
  int (*outcomes)(const void *state, cw_search_move *out, uint32_t *weight, int cap, void *user);
} cw_search_game;

typedef enum { CW_SEARCH_MINIMAX, CW_SEARCH_MCTS, CW_SEARCH_GREEDY } cw_search_kind;

/* What an MCTS playout is worth to each player (see the top of this file). */
typedef enum {
  CW_SEARCH_REWARD_DEFAULT, /* 0: CW_SEARCH_REWARD_MARGIN */
  CW_SEARCH_REWARD_MARGIN,  /* a logistic of the player's lead over the best other player: 1 / (1 + e^(-lead / scale)) */
  CW_SEARCH_REWARD_PLACE,   /* the player's place between the lowest and highest score (0..1); before v0.84.0 the only one */
} cw_search_reward;

/* Zero fields take the defaults in brackets. */
typedef struct cw_search_options {
  cw_search_kind kind;
  uint64_t seed;        /* mistakes, MCTS, sampled chance; derive it from the match (seed, turn number) for replays */
  int max_depth;        /* minimax: the deepest iteration, in decisions (chance nodes do not count) [64] */
  int64_t max_nodes;    /* the deterministic budget: applies + evaluates [200000] */
  float time_ms;        /* a wall-clock budget over the whole search; 0: none (not deterministic when set) */
  bool paranoid;        /* minimax with 3-4 players: me against everyone instead of max-n (ignored at 2) */
  float mistake;        /* 0..1: the seeded chance of playing a worse move [0] */
  int mistake_spread;   /* a mistake picks among the next this-many best moves [3] */
  float uct_c;          /* MCTS exploration constant [1.4] */
  int playout_depth;    /* MCTS: moves per playout before evaluate; < 0: to the end (at most 1000) [24] */
  int tree_nodes;       /* MCTS: tree size; when full, iterations go on without growing it [65536] */
  cw_search_reward reward; /* MCTS: what a playout is worth [MARGIN] */
  float reward_scale;   /* MCTS margin reward: the lead, in evaluate's units, worth 0.73 (e / (1 + e)) [4] */
  int tt_entries;       /* minimax: transposition table entries, rounded up to a power of two; < 0: none [65536] */
} cw_search_options;

/* The shell's CPU levels (cw_shell_cpu_level: 0 Easy, 1 Normal, 2 Hard) as options for kind; seed and kind are the
 * only other fields set. Easy: mistake 0.35, depth 2, 20000 nodes; Normal: mistake 0.1, depth 4, 100000 nodes; Hard: no
 * mistakes, the defaults (depth 64, 200000 nodes). A game tunes from there. */
void cw_search_level_options(int cpu_level, cw_search_kind kind, cw_search_options *out);

typedef struct cw_search cw_search;

/* game and options are copied. NULL when the game lacks moves, apply, turn, terminal or evaluate, or players or
 * state_size is out of range */
cw_search *cw_search_new(const cw_search_game *game, const cw_search_options *options);
void cw_search_free(cw_search *s); /* waits for a background search first; NULL is a no-op */
/* Starts a search for the player to move in state (copied; the caller's state is never touched). false when the
 * state is terminal or a chance node; a root with one legal move is done at once. Restarts any search. */
bool cw_search_start(cw_search *s, const void *state);
/* One slice of work: until `nodes` more nodes (0: no node limit for the slice) or `ms` milliseconds (0: none) have
 * gone, or the search is done. true when the answer is final. */
bool cw_search_step(cw_search *s, float ms, int64_t nodes);
/* Starts and runs to the end on the calling thread; the chosen move in *move (may be NULL). false as start. */
bool cw_search_run(cw_search *s, const void *state, cw_search_move *move);
/* Starts the search (as cw_search_start) on the job pool's background queue; poll cw_search_done. */
bool cw_search_start_background(cw_search *s, const void *state);
bool cw_search_done(cw_search *s); /* finished (a background job too); false before any start */
/* The chosen move (mistakes applied) once done; false before. */
bool cw_search_result(const cw_search *s, cw_search_move *move);

typedef struct cw_search_stats {
  int64_t nodes;          /* applies + evaluates */
  int64_t iterations;     /* MCTS iterations; minimax completed depths */
  int depth;              /* minimax: the deepest completed iteration */
  int steps;              /* cw_search_step calls that did work */
  int root_moves;
  int tree_nodes;         /* MCTS: nodes in the tree */
  int64_t tt_hits;        /* minimax: table answers that cut a node */
  cw_search_move best;    /* the best move before any mistake */
  float best_value;       /* its value: the minimax value, or MCTS mean reward 0..1 */
  cw_search_move chosen;  /* the move played */
  bool mistake_made;
  double elapsed_ms;      /* wall time spent */
} cw_search_stats;
/* What the last search did (zeros before any start); safe while a background search runs only once it is done. */
void cw_search_get_stats(const cw_search *s, cw_search_stats *out);

/* A root move as ranked by cw_search_root_moves. */
typedef struct cw_search_root_move {
  cw_search_move move;
  float value; /* minimax: its value (a bound for a pruned move when mistakes are off); MCTS: mean reward 0..1 */
  int visits;  /* MCTS playouts through it; minimax 0 */
} cw_search_root_move;
/* The root moves, best first, as ranked by the last completed work; returns the count written (at most cap). */
int cw_search_root_moves(const cw_search *s, cw_search_root_move *out, int cap);

#ifdef __cplusplus
}
#endif
#endif

/* Game-tree search (cartwheel/search.h): minimax (alpha-beta, iterative deepening, a transposition table, max-n and
 * paranoid for more players, expectimax at chance nodes) and open-loop Monte Carlo tree search, both as resumable
 * machines so a search spreads over ticks and gives the same answer however it is split. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/core.h"
#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/search.h"

#define INF_V 3.0e38f
static void mcts_rewards(const struct cw_search *s, const float v[CW_SEARCH_MAX_PLAYERS], float r[CW_SEARCH_MAX_PLAYERS]);
#define NP CW_SEARCH_MAX_PLAYERS

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* ── minimax frames ─────────────────────────────────────────────────── */
typedef struct frame {
  uint8_t *own;           /* this frame's state copy (allocated on first use) */
  uint8_t *st;            /* the state of this node: own, or the parent's buffer when undo is used */
  cw_search_move *mv;     /* moves or chance outcomes */
  uint32_t *w;            /* chance weights */
  int n, i;
  int mover;              /* a player, or CW_SEARCH_CHANCE */
  int depth;              /* decisions left */
  bool shared;            /* st is the parent's buffer: undo on return */
  bool sampled;           /* a chance node resolved by chance(): one child, copied */
  bool has_key;
  bool cut;               /* a leaf below was cut by depth (not searched to the end) */
  uint64_t key;
  float alpha, beta, a0, b0;
  float best;             /* scalar modes */
  float vec[NP];          /* max-n best, or chance accumulator */
  double wsum;
  cw_search_move best_move;
} frame;

enum { MODE_SCALAR, MODE_MAXN };
enum { PH_ENTER, PH_NEXT, PH_RETURN };
enum { TT_EXACT, TT_LOWER, TT_UPPER };

typedef struct tt_entry {
  uint64_t key;
  float v[NP];
  cw_search_move move;
  int16_t depth;
  uint8_t flag, used, cut;
} tt_entry;

/* ── MCTS nodes ─────────────────────────────────────────────────────── */
typedef struct mnode {
  cw_search_move move;
  int parent, child, sibling;
  int mover;
  int visits, avail;
  double reward; /* summed reward of the player who made the move */
} mnode;

typedef struct sort_item {
  float p;
  int i;
  cw_search_move m;
} sort_item;

struct cw_search {
  cw_search_game g;
  cw_search_options o;
  bool tt_on;
  int mode, root_player;
  bool started, done;
  uint8_t *root;
  cw_rng rng, mistake_rng;
  /* root moves, in the order they are searched, and their values from the last completed work */
  cw_search_move root_mv[CW_SEARCH_MAX_MOVES];
  float root_val[CW_SEARCH_MAX_MOVES], it_val[CW_SEARCH_MAX_MOVES];
  bool it_has[CW_SEARCH_MAX_MOVES];
  int root_visits[CW_SEARCH_MAX_MOVES];
  int root_n;
  bool have_ranking;
  /* minimax */
  frame F[CW_SEARCH_MAX_PLY];
  int sp, phase, iter_depth;
  bool depth_cut;
  bool ret_cut;
  float ret_s, ret_v[NP];
  tt_entry *tt;
  uint32_t tt_mask;
  sort_item sort_tmp[CW_SEARCH_MAX_MOVES];
  /* mcts */
  mnode *tree;
  int tree_n, tree_cap;
  uint8_t *work;
  cw_search_move mbuf[CW_SEARCH_MAX_MOVES];
  uint32_t wbuf[CW_SEARCH_MAX_MOVES];
  int *path;
  /* results */
  cw_search_stats stats;
  cw_search_move best, chosen;
  float best_value;
  /* background */
  cw_job *job;
};

static const cw_search_options DEFAULTS = { .max_depth = 64, .max_nodes = 200000, .mistake_spread = 3,
                                            .uct_c = 1.4f, .playout_depth = 24, .tree_nodes = 65536,
                                            .reward = CW_SEARCH_REWARD_MARGIN, .reward_scale = 4.0f,
                                            .tt_entries = 65536 };

void cw_search_level_options(int cpu_level, cw_search_kind kind, cw_search_options *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->kind = kind;
  if (cpu_level <= 0) {
    out->mistake = 0.35f;
    out->max_depth = 2;
    out->max_nodes = 20000;
  } else if (cpu_level == 1) {
    out->mistake = 0.1f;
    out->max_depth = 4;
    out->max_nodes = 100000;
  }
}

static void copy_state(cw_search *s, void *dst, const void *src) {
  if (s->g.copy) s->g.copy(dst, src, s->g.user);
  else memcpy(dst, src, s->g.state_size);
}

static uint64_t hash_state(cw_search *s, const void *st) {
  if (s->g.hash) return s->g.hash(st, s->g.user);
  return cw_hash_bytes(st, s->g.state_size, 0);
}

cw_search *cw_search_new(const cw_search_game *game, const cw_search_options *options) {
  if (!game || !game->moves || !game->apply || !game->turn || !game->terminal || !game->evaluate) return NULL;
  if (game->players < 1 || game->players > NP || game->state_size == 0) return NULL;
  cw_search *s = cw_calloc(1, sizeof *s, CW_MEM_GAME);
  s->g = *game;
  s->o = options ? *options : DEFAULTS;
  cw_search_options *o = &s->o;
  if (o->max_depth <= 0) o->max_depth = DEFAULTS.max_depth;
  if (o->max_depth > CW_SEARCH_MAX_PLY - 1) o->max_depth = CW_SEARCH_MAX_PLY - 1;
  if (o->kind == CW_SEARCH_GREEDY) o->max_depth = 1;
  if (o->max_nodes <= 0) o->max_nodes = DEFAULTS.max_nodes;
  if (o->mistake_spread <= 0) o->mistake_spread = DEFAULTS.mistake_spread;
  if (o->uct_c <= 0) o->uct_c = DEFAULTS.uct_c;
  if (o->playout_depth == 0) o->playout_depth = DEFAULTS.playout_depth;
  if (o->tree_nodes <= 0) o->tree_nodes = DEFAULTS.tree_nodes;
  if (o->reward != CW_SEARCH_REWARD_MARGIN && o->reward != CW_SEARCH_REWARD_PLACE) o->reward = DEFAULTS.reward;
  if (!(o->reward_scale > 0)) o->reward_scale = DEFAULTS.reward_scale;
  if (o->tt_entries == 0) o->tt_entries = DEFAULTS.tt_entries;
  if (o->mistake < 0) o->mistake = 0;
  if (o->mistake > 1) o->mistake = 1;
  s->mode = (game->players >= 3 && !o->paranoid) ? MODE_MAXN : MODE_SCALAR;
  s->tt_on = o->kind != CW_SEARCH_MCTS && o->tt_entries > 0 && (game->hash || !game->copy);
  if (s->tt_on) {
    uint32_t n = 1;
    while (n < (uint32_t)o->tt_entries && n < (1u << 24)) n <<= 1;
    s->tt = cw_calloc(n, sizeof(tt_entry), CW_MEM_GAME);
    s->tt_mask = n - 1;
  }
  s->root = cw_alloc(game->state_size, CW_MEM_GAME);
  if (o->kind == CW_SEARCH_MCTS) {
    s->tree_cap = o->tree_nodes;
    s->tree = cw_alloc((size_t)s->tree_cap * sizeof(mnode), CW_MEM_GAME);
    s->work = cw_alloc(game->state_size, CW_MEM_GAME);
    s->path = cw_alloc((size_t)(s->tree_cap + 1) * sizeof(int), CW_MEM_GAME);
  }
  return s;
}

static void wait_job(cw_search *s) {
  if (s->job) {
    cw_job_wait(s->job);
    s->job = NULL;
  }
}

void cw_search_free(cw_search *s) {
  if (!s) return;
  wait_job(s);
  for (int i = 0; i < CW_SEARCH_MAX_PLY; i++) {
    cw_free(s->F[i].own);
    cw_free(s->F[i].mv);
    cw_free(s->F[i].w);
  }
  cw_free(s->tt);
  cw_free(s->root);
  cw_free(s->tree);
  cw_free(s->work);
  cw_free(s->path);
  cw_free(s);
}

static frame *frame_at(cw_search *s, int i) {
  frame *f = &s->F[i];
  if (!f->own) {
    f->own = cw_alloc(s->g.state_size, CW_MEM_GAME);
    f->mv = cw_alloc(sizeof(cw_search_move) * CW_SEARCH_MAX_MOVES, CW_MEM_GAME);
    f->w = cw_alloc(sizeof(uint32_t) * CW_SEARCH_MAX_MOVES, CW_MEM_GAME);
  }
  return f;
}

/* the scalar value of a score vector for the root player */
static float scalar_of(const cw_search *s, const float *v) {
  int n = s->g.players, r = s->root_player;
  if (n == 1) return v[0];
  float other = -INF_V;
  for (int p = 0; p < n; p++)
    if (p != r && v[p] > other) other = v[p];
  return v[r] - other;
}

/* the root player's view of a returned value */
static float root_view(const cw_search *s, float sc, const float *v) {
  return s->mode == MODE_MAXN ? v[s->root_player] : sc;
}

static int cmp_items(const void *a, const void *b) {
  const sort_item *x = a, *y = b;
  if (x->p != y->p) return x->p > y->p ? -1 : 1;
  return x->i - y->i;
}

/* moves in search order: the table's move first, then the game's priority (stable) */
static void order_moves(cw_search *s, frame *f, cw_search_move tt_move) {
  int n = f->n;
  if (n < 2) return;
  if (!s->g.priority && tt_move == CW_SEARCH_MOVE_NONE) return;
  for (int i = 0; i < n; i++) {
    sort_item *it = &s->sort_tmp[i];
    it->m = f->mv[i];
    it->i = i;
    it->p = s->g.priority ? s->g.priority(f->st, f->mv[i], s->g.user) : 0.0f;
    if (it->m == tt_move) it->p = INF_V;
  }
  qsort(s->sort_tmp, (size_t)n, sizeof(sort_item), cmp_items);
  for (int i = 0; i < n; i++) f->mv[i] = s->sort_tmp[i].m;
}

static void leaf_value(cw_search *s, const void *st) {
  float v[NP] = { 0 };
  s->g.evaluate(st, v, s->g.user);
  s->stats.nodes++;
  memcpy(s->ret_v, v, sizeof v);
  s->ret_s = scalar_of(s, v);
}

static int sample_outcome(cw_search *s, const uint32_t *w, int n, cw_rng *rng) {
  uint64_t total = 0;
  for (int i = 0; i < n; i++) total += w[i];
  if (total == 0) return cw_rng_int(rng, 0, n - 1);
  uint64_t pick = ((uint64_t)cw_rng_u32(rng) * total) >> 32, acc = 0;
  for (int i = 0; i < n; i++) {
    acc += w[i];
    if (pick < acc) return i;
  }
  (void)s;
  return n - 1;
}

/* entering frame sp, whose state is set: a leaf answers at once, else its children are listed */
static void mm_enter(cw_search *s) {
  frame *f = &s->F[s->sp];
  const void *st = f->st;
  void *u = s->g.user;
  f->sampled = false;
  f->has_key = false;
  f->cut = false;
  s->ret_cut = false;
  if (s->g.terminal(st, u)) {
    leaf_value(s, st);
    s->phase = PH_RETURN;
    return;
  }
  int t = s->g.turn(st, u);
  if (t != CW_SEARCH_CHANCE && (t < 0 || t >= s->g.players)) t = 0;
  f->mover = t;
  if ((t != CW_SEARCH_CHANCE && f->depth <= 0) || s->sp >= CW_SEARCH_MAX_PLY - 1) {
    s->ret_cut = true;
    leaf_value(s, st);
    s->phase = PH_RETURN;
    return;
  }
  f->i = 0;
  f->best_move = CW_SEARCH_MOVE_NONE;
  if (t == CW_SEARCH_CHANCE) {
    f->n = 0;
    if (s->g.outcomes) f->n = s->g.outcomes(st, f->mv, f->w, CW_SEARCH_MAX_MOVES, u);
    if (f->n <= 0) {
      if (!s->g.chance) { /* a chance node the game cannot resolve: score it as it stands */
        leaf_value(s, st);
        s->phase = PH_RETURN;
        return;
      }
      f->sampled = true;
      f->n = 1;
      f->mv[0] = CW_SEARCH_MOVE_NONE;
      f->w[0] = 1;
    }
    if (f->n > CW_SEARCH_MAX_MOVES) f->n = CW_SEARCH_MAX_MOVES;
    f->wsum = 0;
    f->best = 0;
    memset(f->vec, 0, sizeof f->vec);
    f->alpha = -INF_V, f->beta = INF_V;
    s->phase = PH_NEXT;
    return;
  }
  cw_search_move tt_move = CW_SEARCH_MOVE_NONE;
  f->a0 = f->alpha, f->b0 = f->beta;
  if (s->tt_on) {
    f->key = hash_state(s, st);
    f->has_key = true;
    if (s->sp > 0) {
      tt_entry *e = &s->tt[f->key & s->tt_mask];
      if (e->used && e->key == f->key) {
        tt_move = e->move;
        if (e->depth >= f->depth || !e->cut) {
          bool hit = false;
          if (s->mode == MODE_MAXN || e->flag == TT_EXACT) hit = true;
          else if (e->flag == TT_LOWER && e->v[0] >= f->beta) hit = true;
          else if (e->flag == TT_UPPER && e->v[0] <= f->alpha) hit = true;
          if (hit) {
            s->stats.tt_hits++;
            memcpy(s->ret_v, e->v, sizeof e->v);
            s->ret_s = e->v[0];
            s->ret_cut = e->cut;
            s->phase = PH_RETURN;
            return;
          }
        }
      }
    }
  }
  if (s->sp == 0) {
    f->n = s->root_n;
    memcpy(f->mv, s->root_mv, sizeof(cw_search_move) * (size_t)s->root_n);
  } else {
    f->n = s->g.moves(st, f->mv, CW_SEARCH_MAX_MOVES, u);
    if (f->n > CW_SEARCH_MAX_MOVES) f->n = CW_SEARCH_MAX_MOVES;
    if (f->n <= 0) {
      leaf_value(s, st);
      s->phase = PH_RETURN;
      return;
    }
    order_moves(s, f, tt_move);
  }
  bool maxing = s->mode == MODE_MAXN || s->g.players == 1 || t == s->root_player;
  f->best = maxing ? -INF_V : INF_V;
  for (int p = 0; p < NP; p++) f->vec[p] = -INF_V;
  s->phase = PH_NEXT;
}

/* frame sp's next child, or its value when its children are done or cut */
static void mm_next(cw_search *s) {
  frame *f = &s->F[s->sp];
  void *u = s->g.user;
  bool cut = f->mover != CW_SEARCH_CHANCE && s->mode == MODE_SCALAR && f->alpha >= f->beta;
  if (f->i < f->n && !cut) {
    frame *c = frame_at(s, s->sp + 1);
    cw_search_move m = f->mv[f->i];
    if (f->sampled) {
      copy_state(s, c->own, f->st);
      s->g.chance(c->own, &s->rng, u);
      c->st = c->own;
      c->shared = false;
    } else if (s->g.undo) {
      c->st = f->st;
      s->g.apply(c->st, m, u);
      c->shared = true;
    } else {
      copy_state(s, c->own, f->st);
      s->g.apply(c->own, m, u);
      c->st = c->own;
      c->shared = false;
    }
    s->stats.nodes++;
    c->depth = f->depth - (f->mover == CW_SEARCH_CHANCE ? 0 : 1);
    if (f->mover == CW_SEARCH_CHANCE || (s->sp == 0 && s->o.mistake > 0)) c->alpha = -INF_V, c->beta = INF_V;
    else c->alpha = f->alpha, c->beta = f->beta;
    s->sp++;
    s->phase = PH_ENTER;
    return;
  }
  /* the node's value */
  s->ret_cut = f->cut;
  if (f->mover == CW_SEARCH_CHANCE) {
    double ws = f->wsum > 0 ? f->wsum : 1;
    s->ret_s = (float)(f->best / ws);
    for (int p = 0; p < NP; p++) s->ret_v[p] = (float)(f->vec[p] / ws);
  } else {
    s->ret_s = f->best;
    memcpy(s->ret_v, f->vec, sizeof f->vec);
    if (s->mode == MODE_SCALAR) s->ret_v[0] = f->best;
    if (f->has_key && f->depth > 0) {
      tt_entry *e = &s->tt[f->key & s->tt_mask];
      e->key = f->key;
      memcpy(e->v, s->ret_v, sizeof e->v);
      e->move = f->best_move;
      e->depth = (int16_t)f->depth;
      e->used = 1;
      e->cut = f->cut;
      if (s->mode == MODE_MAXN) e->flag = TT_EXACT;
      else e->flag = f->best <= f->a0 ? TT_UPPER : f->best >= f->b0 ? TT_LOWER : TT_EXACT;
    }
  }
  s->phase = PH_RETURN;
}

static void finish(cw_search *s);

static void mm_iteration_start(cw_search *s) {
  frame *f = frame_at(s, 0);
  copy_state(s, f->own, s->root);
  f->st = f->own;
  f->shared = false;
  f->depth = s->iter_depth;
  f->alpha = -INF_V, f->beta = INF_V;
  s->sp = 0;
  s->depth_cut = false;
  for (int i = 0; i < s->root_n; i++) s->it_has[i] = false;
  s->phase = PH_ENTER;
}

/* rank the root moves by value (stable): the next iteration searches them in this order */
static void sort_root(cw_search *s, float *vals, int *visits) {
  for (int i = 1; i < s->root_n; i++) { /* insertion sort: stable, n is small at the root */
    cw_search_move m = s->root_mv[i];
    float v = vals[i];
    int vi = visits ? visits[i] : 0;
    int j = i - 1;
    while (j >= 0 && (visits ? (visits[j] < vi || (visits[j] == vi && vals[j] < v)) : vals[j] < v)) {
      s->root_mv[j + 1] = s->root_mv[j];
      vals[j + 1] = vals[j];
      if (visits) visits[j + 1] = visits[j];
      j--;
    }
    s->root_mv[j + 1] = m;
    vals[j + 1] = v;
    if (visits) visits[j + 1] = vi;
  }
}

static void mm_return(cw_search *s) {
  if (s->sp == 0) { /* an iteration is complete */
    frame *f = &s->F[0];
    s->depth_cut = s->ret_cut;
    memcpy(s->root_val, s->it_val, sizeof(float) * (size_t)s->root_n);
    s->best = f->best_move;
    s->best_value = root_view(s, f->best, f->vec);
    s->have_ranking = true;
    s->stats.depth = s->iter_depth;
    s->stats.iterations++;
    sort_root(s, s->root_val, NULL);
    if (s->iter_depth >= s->o.max_depth || !s->depth_cut) {
      finish(s);
      return;
    }
    s->iter_depth++;
    mm_iteration_start(s);
    return;
  }
  frame *c = &s->F[s->sp];
  frame *f = &s->F[s->sp - 1];
  cw_search_move m = f->mv[f->i];
  if (c->shared) s->g.undo(f->st, m, s->g.user);
  s->sp--;
  if (s->ret_cut) f->cut = true;
  float sc = s->ret_s;
  const float *v = s->ret_v;
  if (f->mover == CW_SEARCH_CHANCE) {
    double w = f->w[f->i];
    f->wsum += w;
    f->best = (float)(f->best + w * sc);
    for (int p = 0; p < NP; p++) f->vec[p] = (float)(f->vec[p] + w * v[p]);
  } else if (s->mode == MODE_MAXN) {
    if (v[f->mover] > f->vec[f->mover] || f->best_move == CW_SEARCH_MOVE_NONE) {
      memcpy(f->vec, v, sizeof f->vec);
      f->best = v[f->mover];
      f->best_move = m;
    }
  } else {
    bool maxing = s->g.players == 1 || f->mover == s->root_player;
    if (maxing ? (sc > f->best) : (sc < f->best)) {
      f->best = sc;
      f->best_move = m;
    }
    if (f->best_move == CW_SEARCH_MOVE_NONE) f->best_move = m;
    if (maxing && sc > f->alpha) f->alpha = sc;
    if (!maxing && sc < f->beta) f->beta = sc;
  }
  if (s->sp == 0) {
    s->it_val[f->i] = root_view(s, sc, v);
    s->it_has[f->i] = true;
  }
  f->i++;
  s->phase = PH_NEXT;
}

/* ── MCTS ───────────────────────────────────────────────────────────── */
static int tree_add(cw_search *s, int parent, cw_search_move m, int mover) {
  if (s->tree_n >= s->tree_cap) return -1;
  int i = s->tree_n++;
  mnode *n = &s->tree[i];
  n->move = m;
  n->parent = parent;
  n->child = -1;
  n->sibling = -1;
  n->mover = mover;
  n->visits = 0;
  n->avail = 1;
  n->reward = 0;
  if (parent >= 0) {
    /* append, so children stay in creation order */
    int *link = &s->tree[parent].child;
    while (*link >= 0) link = &s->tree[*link].sibling;
    *link = i;
  }
  return i;
}

static int find_child(cw_search *s, int node, cw_search_move m) {
  for (int c = s->tree[node].child; c >= 0; c = s->tree[c].sibling)
    if (s->tree[c].move == m) return c;
  return -1;
}

/* resolves a chance event in the MCTS working state */
static bool mcts_chance(cw_search *s, void *st) {
  void *u = s->g.user;
  if (s->g.chance) {
    s->g.chance(st, &s->rng, u);
  } else if (s->g.outcomes) {
    int n = s->g.outcomes(st, s->mbuf, s->wbuf, CW_SEARCH_MAX_MOVES, u);
    if (n <= 0) return false;
    if (n > CW_SEARCH_MAX_MOVES) n = CW_SEARCH_MAX_MOVES;
    s->g.apply(st, s->mbuf[sample_outcome(s, s->wbuf, n, &s->rng)], u);
  } else {
    return false;
  }
  s->stats.nodes++;
  return true;
}

static void mcts_iteration(cw_search *s) {
  void *u = s->g.user;
  void *st = s->work;
  copy_state(s, st, s->root);
  int node = 0, plen = 0;
  s->path[plen++] = 0;
  bool stuck = false;
  /* selection and expansion */
  for (;;) {
    if (s->g.terminal(st, u)) break;
    int t = s->g.turn(st, u);
    if (t == CW_SEARCH_CHANCE) {
      if (!mcts_chance(s, st)) { stuck = true; break; }
      continue;
    }
    if (t < 0 || t >= s->g.players) t = 0;
    int n = s->g.moves(st, s->mbuf, CW_SEARCH_MAX_MOVES, u);
    if (n <= 0) break;
    if (n > CW_SEARCH_MAX_MOVES) n = CW_SEARCH_MAX_MOVES;
    int untried = 0;
    for (int i = 0; i < n; i++) {
      int c = find_child(s, node, s->mbuf[i]);
      if (c < 0) untried++;
      else s->tree[c].avail++;
    }
    if (untried > 0 && s->tree_n < s->tree_cap) {
      int k = cw_rng_int(&s->rng, 0, untried - 1);
      cw_search_move m = CW_SEARCH_MOVE_NONE;
      for (int i = 0; i < n; i++)
        if (find_child(s, node, s->mbuf[i]) < 0 && k-- == 0) {
          m = s->mbuf[i];
          break;
        }
      int c = tree_add(s, node, m, t);
      s->g.apply(st, m, u);
      s->stats.nodes++;
      s->path[plen++] = c;
      break;
    }
    if (untried > 0) break; /* the tree is full: play out from here */
    int best = -1;
    float best_score = -INF_V;
    for (int i = 0; i < n; i++) {
      int c = find_child(s, node, s->mbuf[i]);
      const mnode *cn = &s->tree[c];
      float mean = (float)(cn->reward / (cn->visits > 0 ? cn->visits : 1));
      float sc = cn->visits > 0 ? mean + s->o.uct_c * sqrtf(cw_ln((float)cn->avail) / (float)cn->visits) : INF_V;
      if (sc > best_score) best_score = sc, best = c;
    }
    s->g.apply(st, s->tree[best].move, u);
    s->stats.nodes++;
    node = best;
    s->path[plen++] = node;
  }
  /* playout */
  int limit = s->o.playout_depth < 0 ? 1000 : s->o.playout_depth;
  for (int d = 0; !stuck && d < limit && !s->g.terminal(st, u);) {
    int t = s->g.turn(st, u);
    if (t == CW_SEARCH_CHANCE) {
      if (!mcts_chance(s, st)) break;
      continue;
    }
    int n = s->g.moves(st, s->mbuf, CW_SEARCH_MAX_MOVES, u);
    if (n <= 0) break;
    if (n > CW_SEARCH_MAX_MOVES) n = CW_SEARCH_MAX_MOVES;
    cw_search_move m = s->g.playout ? s->g.playout(st, s->mbuf, n, &s->rng, u) : s->mbuf[cw_rng_int(&s->rng, 0, n - 1)];
    s->g.apply(st, m, u);
    s->stats.nodes++;
    d++;
  }
  float v[NP] = { 0 }, r[NP] = { 0 };
  s->g.evaluate(st, v, u);
  s->stats.nodes++;
  mcts_rewards(s, v, r);
  if (s->g.players == 1) r[0] = v[0]; /* one player: the raw value (the game keeps it in 0..1 for UCT's scale) */
  for (int i = 0; i < plen; i++) {
    mnode *n = &s->tree[s->path[i]];
    n->visits++;
    if (i > 0) n->reward += r[n->mover];
  }
  s->stats.iterations++;
}

/* a playout's worth to each player, 0..1 (cw_search_reward) */
static void mcts_rewards(const cw_search *s, const float v[NP], float r[NP]) {
  int np = s->g.players;
  if (s->o.reward == CW_SEARCH_REWARD_PLACE) { /* each player's place between the lowest and highest score */
    float lo = INF_V, hi = -INF_V;
    for (int p = 0; p < np; p++) {
      if (v[p] < lo) lo = v[p];
      if (v[p] > hi) hi = v[p];
    }
    for (int p = 0; p < np; p++) r[p] = hi > lo ? (v[p] - lo) / (hi - lo) : 0.5f;
    return;
  }
  /* the margin: a logistic of the lead over the best other player, so a player behind still gains by closing the
   * gap and a player ahead by widening it (cw_exp is the same on every host; its ends saturate to 0 and 1) */
  for (int p = 0; p < np; p++) {
    float best = -INF_V;
    for (int q = 0; q < np; q++)
      if (q != p && v[q] > best) best = v[q];
    float lead = (v[p] - best) / s->o.reward_scale;
    if (lead > 80) lead = 80;
    if (lead < -80) lead = -80;
    r[p] = 1.0f / (1.0f + cw_exp(-lead));
  }
}

static void mcts_rank(cw_search *s) {
  for (int i = 0; i < s->root_n; i++) {
    int c = find_child(s, 0, s->root_mv[i]);
    s->root_visits[i] = c >= 0 ? s->tree[c].visits : 0;
    s->root_val[i] = c >= 0 && s->tree[c].visits > 0 ? (float)(s->tree[c].reward / s->tree[c].visits) : 0;
  }
  sort_root(s, s->root_val, s->root_visits);
  s->best = s->root_mv[0];
  s->best_value = s->root_val[0];
  s->have_ranking = true;
}

/* ── shared ─────────────────────────────────────────────────────────── */
static void finish(cw_search *s) {
  if (s->o.kind == CW_SEARCH_MCTS) {
    mcts_rank(s);
  } else if (!s->have_ranking) {
    /* no iteration completed: the partial one's best, else the first move */
    frame *f = &s->F[0];
    s->best = f->n > 0 && f->best_move != CW_SEARCH_MOVE_NONE ? f->best_move : s->root_mv[0];
    s->best_value = 0;
    for (int i = 0; i < s->root_n; i++) s->root_val[i] = s->it_has[i] ? s->it_val[i] : -INF_V;
  }
  s->chosen = s->best;
  s->stats.mistake_made = false;
  if (s->o.mistake > 0 && s->root_n > 1 && cw_rng_float(&s->mistake_rng) < s->o.mistake) {
    /* the next mistake_spread best moves (the root list is ranked, the best first), weighted toward the better */
    int bi = 0;
    while (bi < s->root_n && s->root_mv[bi] != s->best) bi++;
    int cand[CW_SEARCH_MAX_MOVES], nc = 0;
    for (int i = 0; i < s->root_n && nc < s->o.mistake_spread; i++)
      if (i != bi && (s->o.kind != CW_SEARCH_MCTS || s->root_visits[i] > 0)) cand[nc++] = i;
    if (nc > 0) {
      int total = nc * (nc + 1) / 2, pick = cw_rng_int(&s->mistake_rng, 0, total - 1), k = 0;
      for (int w = nc; k < nc; k++, w--) {
        if (pick < w) break;
        pick -= w;
      }
      if (k >= nc) k = nc - 1;
      s->chosen = s->root_mv[cand[k]];
      s->stats.mistake_made = true;
    }
  }
  s->stats.best = s->best;
  s->stats.best_value = s->best_value;
  s->stats.chosen = s->chosen;
  s->done = true;
}

bool cw_search_start(cw_search *s, const void *state) {
  if (!s || !state) return false;
  wait_job(s);
  void *u = s->g.user;
  s->started = false;
  s->done = false;
  s->have_ranking = false;
  memset(&s->stats, 0, sizeof s->stats);
  s->best = s->chosen = CW_SEARCH_MOVE_NONE;
  s->best_value = 0;
  if (s->g.terminal(state, u)) return false;
  int t = s->g.turn(state, u);
  if (t < 0 || t >= s->g.players) return false;
  copy_state(s, s->root, state);
  s->root_player = t;
  s->root_n = s->g.moves(state, s->root_mv, CW_SEARCH_MAX_MOVES, u);
  if (s->root_n <= 0) return false;
  if (s->root_n > CW_SEARCH_MAX_MOVES) s->root_n = CW_SEARCH_MAX_MOVES;
  s->stats.root_moves = s->root_n;
  cw_rng_seed(&s->rng, s->o.seed);
  cw_rng_seed(&s->mistake_rng, s->o.seed ^ 0x9e3779b97f4a7c15ull);
  for (int i = 0; i < s->root_n; i++) s->root_val[i] = 0, s->root_visits[i] = 0;
  s->started = true;
  if (s->tt) memset(s->tt, 0, (size_t)(s->tt_mask + 1) * sizeof(tt_entry));
  if (s->o.kind == CW_SEARCH_MCTS) {
    s->tree_n = 0;
    tree_add(s, -1, CW_SEARCH_MOVE_NONE, -1);
  } else {
    /* the game's priority orders the root's first iteration */
    frame *f = frame_at(s, 0);
    f->st = s->root;
    f->n = s->root_n;
    memcpy(f->mv, s->root_mv, sizeof(cw_search_move) * (size_t)s->root_n);
    order_moves(s, f, CW_SEARCH_MOVE_NONE);
    memcpy(s->root_mv, f->mv, sizeof(cw_search_move) * (size_t)s->root_n);
    s->iter_depth = 1;
    mm_iteration_start(s);
  }
  if (s->root_n == 1) {
    s->best = s->chosen = s->root_mv[0];
    s->stats.best = s->stats.chosen = s->best;
    s->have_ranking = true;
    s->done = true;
  }
  return true;
}

bool cw_search_step(cw_search *s, float ms, int64_t nodes) {
  if (!s || !s->started) return false;
  if (s->done) return true;
  double t0 = now_ms();
  int64_t n0 = s->stats.nodes;
  int checks = 0;
  s->stats.steps++;
  for (;;) {
    if (s->stats.nodes >= s->o.max_nodes) { finish(s); break; }
    if (nodes > 0 && s->stats.nodes - n0 >= nodes) break;
    if ((ms > 0 || s->o.time_ms > 0) && (s->o.kind == CW_SEARCH_MCTS || (++checks & 15) == 0)) {
      double el = now_ms() - t0;
      if (s->o.time_ms > 0 && s->stats.elapsed_ms + el >= s->o.time_ms) { finish(s); break; }
      if (ms > 0 && el >= ms) break;
    }
    if (s->o.kind == CW_SEARCH_MCTS) {
      mcts_iteration(s);
      continue;
    }
    switch (s->phase) {
    case PH_ENTER: mm_enter(s); break;
    case PH_NEXT: mm_next(s); break;
    default: mm_return(s); break;
    }
    if (s->done) break;
  }
  s->stats.elapsed_ms += now_ms() - t0;
  s->stats.tree_nodes = s->tree_n;
  return s->done;
}

bool cw_search_run(cw_search *s, const void *state, cw_search_move *move) {
  if (!cw_search_start(s, state)) return false;
  while (!cw_search_step(s, 0, 0)) {}
  if (move) *move = s->chosen;
  return true;
}

static void background_fn(void *ctx) {
  cw_search *s = ctx;
  while (!cw_search_step(s, 0, 0)) {}
}

bool cw_search_start_background(cw_search *s, const void *state) {
  if (!cw_search_start(s, state)) return false;
  if (!s->done) s->job = cw_job_background(background_fn, s);
  return true;
}

bool cw_search_done(cw_search *s) {
  if (!s || !s->started) return false;
  if (s->job) {
    if (!cw_job_done(s->job)) return false;
    wait_job(s);
  }
  return s->done;
}

bool cw_search_result(const cw_search *s, cw_search_move *move) {
  if (!s || !s->done || s->job) return false;
  if (move) *move = s->chosen;
  return true;
}

void cw_search_get_stats(const cw_search *s, cw_search_stats *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!s) return;
  *out = s->stats;
  out->tree_nodes = s->tree_n;
}

int cw_search_root_moves(const cw_search *s, cw_search_root_move *out, int cap) {
  if (!s || !out || !s->started) return 0;
  int n = s->root_n < cap ? s->root_n : cap;
  for (int i = 0; i < n; i++) {
    out[i].move = s->root_mv[i];
    out[i].value = s->root_val[i];
    out[i].visits = s->o.kind == CW_SEARCH_MCTS ? s->root_visits[i] : 0;
  }
  return n;
}

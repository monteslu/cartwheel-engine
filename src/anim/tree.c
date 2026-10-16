/* AnimationTree: a blend graph (clips, 1D/2D blend spaces, lerp, additive,
 * state machines) evaluated each tick into a mixer. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"
#include "cartwheel/profile.h"

enum { BN_CLIP, BN_BLEND1D, BN_BLEND2D, BN_BLEND2, BN_ADD, BN_SM };
enum { OP_TRUTHY, OP_FALSY, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };

typedef struct { const char *name; int node; } sm_state;
typedef struct {
  int from, to;
  cw_transition t;
  const char *cparam; /* NULL: always */
  int op;
  float value;
} sm_trans;

typedef struct bnode {
  int kind, parent;
  /* clip */
  const char *anim_name;
  float speed;
  anim_cursor cur;
  float weight; /* last evaluation */
  bool ended;
  /* blends */
  CW_ARRAY(int) kids;
  CW_ARRAY(float) px;
  CW_ARRAY(float) py;
  const char *param, *param_y;
  cw_map *filter;
  /* state machine */
  CW_ARRAY(sm_state) states;
  CW_ARRAY(sm_trans) trans;
  int current, next;
  float fade, fade_t;
  CW_ARRAY(int) travel;
} bnode;

typedef struct { const char *name; float value; } tparam;
typedef struct { cw_anim anim; int count; } fired_span;

typedef struct atree {
  const char *player;
  bool active;
  int32_t process;
  const char *root_motion_track;
  const char *root_motion_target;
  int32_t root_motion_lock;
  CW_ARRAY(bnode) nodes;
  int root;
  CW_ARRAY(tparam) params;
  cw__amix *mix;
  cw_node player_node;
  anim_fired_list fired;
  CW_ARRAY(fired_span) spans;
  CW_ARRAY(const char *) entered;
} atree;

static const cw_node_type *tree_t;

static atree *T(cw_node n, const char *fn) {
  atree *t = cw_node_data_t(n, tree_t);
  if (!t && fn) cw_warn("%s: '%s' is not an AnimationTree", fn, cw_node_valid(n) ? cw_node_name(n) : "(invalid node)");
  return t;
}

static bnode *B(atree *t, cw_blend_id id, const char *fn) {
  if (id < 0 || id >= t->nodes.count) { cw_warn("%s: no blend node %d (the tree has %d)", fn, id, t->nodes.count); return NULL; }
  return &t->nodes.data[id];
}

static cw_blend_id new_node(atree *t, int kind) {
  bnode b = { 0 };
  b.kind = kind;
  b.parent = -1;
  b.speed = 1;
  b.next = -1;
  b.cur.dir = 1;
  b.cur.fire_start = true;
  cw_array_push(&t->nodes, b);
  if (t->root < 0) t->root = t->nodes.count - 1;
  return t->nodes.count - 1;
}

static bool adopt(atree *t, cw_blend_id parent, cw_blend_id child, const char *fn) {
  bnode *c = B(t, child, fn);
  if (!c) return false;
  if (c->parent >= 0) { cw_warn("%s: blend node %d already has a parent (%d); graph nodes are not shared", fn, child, c->parent); return false; }
  if (child == parent) return false;
  c->parent = parent;
  if (t->root == child) t->root = parent;
  return true;
}

/* ── params ─────────────────────────────────────────────────────────── */

static float param(atree *t, const char *name) {
  for (int i = 0; i < t->params.count; i++)
    if (t->params.data[i].name == name) return t->params.data[i].value;
  return 0;
}

void cw_animtree_set_param(cw_node n, const char *name, float value) {
  atree *t = T(n, "cw_animtree_set_param");
  if (!t) return;
  const char *in = cw_intern(name);
  for (int i = 0; i < t->params.count; i++)
    if (t->params.data[i].name == in) { t->params.data[i].value = value; return; }
  cw_array_push(&t->params, ((tparam){ in, value }));
}
float cw_animtree_get_param(cw_node n, const char *name) {
  atree *t = T(n, "cw_animtree_get_param");
  return t ? param(t, cw_intern(name)) : 0;
}

/* ── building ───────────────────────────────────────────────────────── */

cw_blend_id cw_animtree_clip(cw_node n, const char *anim_name) {
  atree *t = T(n, "cw_animtree_clip");
  if (!t) return -1;
  cw_blend_id id = new_node(t, BN_CLIP);
  t->nodes.data[id].anim_name = cw_intern(anim_name);
  return id;
}

void cw_animtree_clip_set_speed(cw_node n, cw_blend_id clip, float speed) {
  atree *t = T(n, "cw_animtree_clip_set_speed");
  bnode *b = t ? B(t, clip, "cw_animtree_clip_set_speed") : NULL;
  if (b) b->speed = speed;
}

cw_blend_id cw_animtree_blend1d(cw_node n, const char *p) {
  atree *t = T(n, "cw_animtree_blend1d");
  if (!t) return -1;
  cw_blend_id id = new_node(t, BN_BLEND1D);
  t->nodes.data[id].param = cw_intern(p);
  return id;
}

void cw_animtree_blend1d_add(cw_node n, cw_blend_id space, cw_blend_id child, float pos) {
  atree *t = T(n, "cw_animtree_blend1d_add");
  bnode *b = t ? B(t, space, "cw_animtree_blend1d_add") : NULL;
  if (!b || b->kind != BN_BLEND1D || !adopt(t, space, child, "cw_animtree_blend1d_add")) return;
  b = &t->nodes.data[space];
  cw_array_push(&b->kids, child);
  cw_array_push(&b->px, pos);
}

cw_blend_id cw_animtree_blend2d(cw_node n, const char *px, const char *py) {
  atree *t = T(n, "cw_animtree_blend2d");
  if (!t) return -1;
  cw_blend_id id = new_node(t, BN_BLEND2D);
  t->nodes.data[id].param = cw_intern(px);
  t->nodes.data[id].param_y = cw_intern(py);
  return id;
}

void cw_animtree_blend2d_add(cw_node n, cw_blend_id space, cw_blend_id child, float x, float y) {
  atree *t = T(n, "cw_animtree_blend2d_add");
  bnode *b = t ? B(t, space, "cw_animtree_blend2d_add") : NULL;
  if (!b || b->kind != BN_BLEND2D || !adopt(t, space, child, "cw_animtree_blend2d_add")) return;
  b = &t->nodes.data[space];
  cw_array_push(&b->kids, child);
  cw_array_push(&b->px, x);
  cw_array_push(&b->py, y);
}

static cw_blend_id two(cw_node n, int kind, cw_blend_id a, cw_blend_id c, const char *p, const char *fn) {
  atree *t = T(n, fn);
  if (!t) return -1;
  cw_blend_id id = new_node(t, kind);
  if (!adopt(t, id, a, fn) || !adopt(t, id, c, fn)) return id;
  bnode *b = &t->nodes.data[id];
  cw_array_push(&b->kids, a);
  cw_array_push(&b->kids, c);
  b->param = cw_intern(p);
  return id;
}
cw_blend_id cw_animtree_blend2(cw_node n, cw_blend_id a, cw_blend_id b, const char *p) { return two(n, BN_BLEND2, a, b, p, "cw_animtree_blend2"); }
cw_blend_id cw_animtree_add(cw_node n, cw_blend_id base, cw_blend_id add, const char *p) { return two(n, BN_ADD, base, add, p, "cw_animtree_add"); }

void cw_animtree_set_filter(cw_node n, cw_blend_id id, const char *const *names, int count) {
  atree *t = T(n, "cw_animtree_set_filter");
  bnode *b = t ? B(t, id, "cw_animtree_set_filter") : NULL;
  if (!b) return;
  if (!b->filter) {
    b->filter = cw_alloc(sizeof(cw_map), CW_MEM_ANIM);
    cw_map_init(b->filter, CW_MEM_ANIM);
  }
  for (int i = 0; i < count; i++) cw_map_set(b->filter, (uint64_t)(uintptr_t)cw_intern(names[i]), 1);
}

void cw_animtree_set_root(cw_node n, cw_blend_id id) {
  atree *t = T(n, "cw_animtree_set_root");
  if (t && B(t, id, "cw_animtree_set_root")) t->root = id;
}

cw_blend_id cw_animtree_state_machine(cw_node n) {
  atree *t = T(n, "cw_animtree_state_machine");
  return t ? new_node(t, BN_SM) : -1;
}

static int state_index(bnode *sm, const char *name) {
  const char *in = cw_intern(name);
  for (int i = 0; i < sm->states.count; i++)
    if (sm->states.data[i].name == in) return i;
  return -1;
}

static bnode *SM(cw_node n, cw_blend_id id, const char *fn, atree **tout) {
  atree *t = T(n, fn);
  bnode *b = t ? B(t, id, fn) : NULL;
  if (b && b->kind != BN_SM) { cw_warn("%s: blend node %d is not a state machine", fn, id); b = NULL; }
  if (tout) *tout = t;
  return b;
}

void cw_animtree_sm_add_state(cw_node n, cw_blend_id sm, const char *state, cw_blend_id node) {
  atree *t;
  bnode *b = SM(n, sm, "cw_animtree_sm_add_state", &t);
  if (!b) return;
  if (state_index(b, state) >= 0) { cw_warn("cw_animtree_sm_add_state: state '%s' exists", state); return; }
  if (!adopt(t, sm, node, "cw_animtree_sm_add_state")) return;
  b = &t->nodes.data[sm];
  cw_array_push(&b->states, ((sm_state){ cw_intern(state), node }));
}

static bool parse_condition(const char *c, sm_trans *tr) {
  tr->cparam = NULL;
  if (!c || !*c) return true;
  while (*c == ' ') c++;
  bool neg = *c == '!';
  if (neg) c++;
  const char *s = c;
  while (*c && *c != ' ' && *c != '=' && *c != '!' && *c != '<' && *c != '>') c++;
  tr->cparam = cw_intern_n(s, (size_t)(c - s));
  while (*c == ' ') c++;
  if (!*c) { tr->op = neg ? OP_FALSY : OP_TRUTHY; return true; }
  if (neg) return false;
  static const struct { const char *s; int op; } ops[] = { { "==", OP_EQ }, { "!=", OP_NE }, { "<=", OP_LE }, { ">=", OP_GE },
                                                           { "<", OP_LT }, { ">", OP_GT } };
  for (size_t i = 0; i < CW_ARRAY_LEN(ops); i++)
    if (!strncmp(c, ops[i].s, strlen(ops[i].s))) {
      tr->op = ops[i].op;
      c += strlen(ops[i].s);
      char *end;
      tr->value = strtof(c, &end);
      if (end == c) {
        while (*end == ' ') end++;
        if (!strncmp(end, "true", 4)) tr->value = 1;
        else if (!strncmp(end, "false", 5)) tr->value = 0;
        else return false;
      }
      return true;
    }
  return false;
}

void cw_animtree_sm_add_transition(cw_node n, cw_blend_id sm, const char *from, const char *to, const cw_transition *td) {
  atree *t;
  bnode *b = SM(n, sm, "cw_animtree_sm_add_transition", &t);
  if (!b) return;
  sm_trans tr = { state_index(b, from), state_index(b, to), td ? *td : (cw_transition){ 0 } };
  if (tr.from < 0 || tr.to < 0) {
    cw_warn("cw_animtree_sm_add_transition: unknown state '%s' (add states before transitions)", tr.from < 0 ? from : to);
    return;
  }
  if (!parse_condition(tr.t.condition, &tr)) {
    cw_warn("cw_animtree_sm_add_transition %s -> %s: cannot parse condition \"%s\" (write \"speed > 0.5\", \"grounded\", "
            "\"!grounded\")", from, to, tr.t.condition);
    return;
  }
  tr.t.condition = NULL;
  cw_array_push(&b->trans, tr);
}

/* ── state machine logic ────────────────────────────────────────────── */

static void reset_node(atree *t, int id) {
  bnode *b = &t->nodes.data[id];
  memset(&b->cur, 0, sizeof b->cur);
  b->cur.dir = 1;
  b->cur.fire_start = true;
  b->ended = false;
  if (b->kind == BN_SM) {
    b->current = 0;
    b->next = -1;
    b->fade_t = 0;
    cw_array_clear(&b->travel);
    if (b->states.count) reset_node(t, t->nodes.data[id].states.data[0].node);
    return;
  }
  for (int i = 0; i < b->kids.count; i++) reset_node(t, t->nodes.data[id].kids.data[i]);
}

static void enter(atree *t, int sm_id, int state, float xfade) {
  bnode *b = &t->nodes.data[sm_id];
  if (state < 0 || state >= b->states.count) return;
  int node = b->states.data[state].node;
  reset_node(t, node);
  b = &t->nodes.data[sm_id];
  if (xfade > 0 && state != b->current) {
    b->next = state;
    b->fade = xfade;
    b->fade_t = 0;
  } else {
    b->current = state;
    b->next = -1;
  }
  cw_array_push(&t->entered, b->states.data[state].name);
}

static bool cond_ok(atree *t, const sm_trans *tr) {
  if (!tr->cparam) return true;
  float v = param(t, tr->cparam);
  switch (tr->op) {
  case OP_TRUTHY: return v != 0;
  case OP_FALSY: return v == 0;
  case OP_EQ: return v == tr->value;
  case OP_NE: return v != tr->value;
  case OP_LT: return v < tr->value;
  case OP_LE: return v <= tr->value;
  case OP_GT: return v > tr->value;
  default: return v >= tr->value;
  }
}

static const sm_trans *find_trans(bnode *b, int from, int to) {
  for (int i = 0; i < b->trans.count; i++)
    if (b->trans.data[i].from == from && b->trans.data[i].to == to && b->trans.data[i].t.advance != CW_ADVANCE_DISABLED)
      return &b->trans.data[i];
  return NULL;
}

static void sm_step(atree *t, int sm_id, double dt) {
  bnode *b = &t->nodes.data[sm_id];
  if (!b->states.count) return;
  if (b->next >= 0) {
    b->fade_t += (float)dt;
    if (b->fade_t < b->fade) return;
    b->current = b->next;
    b->next = -1;
  }
  bool ended = t->nodes.data[b->states.data[b->current].node].ended;
  if (b->travel.count) {
    int hop = b->travel.data[0];
    const sm_trans *tr = find_trans(b, b->current, hop);
    if (!tr) { cw_array_clear(&b->travel); return; }
    if (tr->t.switch_mode == CW_SWITCH_AT_END && !ended) return;
    memmove(b->travel.data, b->travel.data + 1, sizeof(int) * (size_t)(b->travel.count - 1));
    b->travel.count--;
    enter(t, sm_id, hop, tr->t.xfade);
    return;
  }
  for (int i = 0; i < b->trans.count; i++) {
    const sm_trans *tr = &b->trans.data[i];
    if (tr->from != b->current || tr->t.advance != CW_ADVANCE_AUTO || !cond_ok(t, tr)) continue;
    if (tr->t.switch_mode == CW_SWITCH_AT_END && !ended) continue;
    enter(t, sm_id, tr->to, tr->t.xfade);
    return;
  }
}

void cw_animtree_sm_start(cw_node n, cw_blend_id sm, const char *state) {
  atree *t;
  bnode *b = SM(n, sm, "cw_animtree_sm_start", &t);
  if (!b) return;
  int s = state_index(b, state);
  if (s < 0) { cw_warn("cw_animtree_sm_start: no state '%s'", state); return; }
  cw_array_clear(&b->travel);
  enter(t, sm, s, 0);
}

void cw_animtree_sm_travel(cw_node n, cw_blend_id sm, const char *state) {
  atree *t;
  bnode *b = SM(n, sm, "cw_animtree_sm_travel", &t);
  if (!b) return;
  int goal = state_index(b, state);
  if (goal < 0) { cw_warn("cw_animtree_sm_travel: no state '%s'", state); return; }
  int from = b->next >= 0 ? b->next : b->current;
  cw_array_clear(&b->travel);
  if (from == goal) return;
  int ns = b->states.count;
  int *prev = cw_frame_alloc(sizeof(int) * (size_t)ns), *queue = cw_frame_alloc(sizeof(int) * (size_t)ns);
  for (int i = 0; i < ns; i++) prev[i] = -2;
  int qh = 0, qt = 0;
  queue[qt++] = from;
  prev[from] = -1;
  while (qh < qt && prev[goal] == -2) {
    int s = queue[qh++];
    for (int i = 0; i < b->trans.count; i++) {
      sm_trans *tr = &b->trans.data[i];
      if (tr->from != s || tr->t.advance == CW_ADVANCE_DISABLED || prev[tr->to] != -2) continue;
      prev[tr->to] = s;
      queue[qt++] = tr->to;
    }
  }
  if (prev[goal] == -2) { /* unreachable: go there directly */
    enter(t, sm, goal, 0);
    return;
  }
  int len = 0;
  for (int s = goal; s != from; s = prev[s]) len++;
  cw_array_reserve(&b->travel, len);
  b->travel.count = len;
  for (int s = goal, i = len - 1; s != from; s = prev[s], i--) b->travel.data[i] = s;
}

const char *cw_animtree_sm_current(cw_node n, cw_blend_id sm) {
  bnode *b = SM(n, sm, "cw_animtree_sm_current", NULL);
  return b && b->states.count ? b->states.data[b->current].name : NULL;
}
const char *cw_animtree_sm_next(cw_node n, cw_blend_id sm) {
  bnode *b = SM(n, sm, "cw_animtree_sm_next", NULL);
  return b && b->next >= 0 ? b->states.data[b->next].name : NULL;
}

/* ── evaluation ─────────────────────────────────────────────────────── */

static void eval(atree *t, int id, float w, double dt, const cw_map *filter, bool additive);

static void blend1d_weights(atree *t, bnode *b, float *wk) {
  int n = b->kids.count;
  float p = param(t, b->param);
  int lo = -1, hi = -1;
  for (int i = 0; i < n; i++) {
    wk[i] = 0;
    float x = b->px.data[i];
    if (x <= p && (lo < 0 || x > b->px.data[lo])) lo = i;
    if (x >= p && (hi < 0 || x < b->px.data[hi])) hi = i;
  }
  if (lo < 0 && hi < 0) return;
  if (lo < 0) { wk[hi] = 1; return; }
  if (hi < 0 || lo == hi || b->px.data[hi] == b->px.data[lo]) { wk[lo] = 1; return; }
  float a = (p - b->px.data[lo]) / (b->px.data[hi] - b->px.data[lo]);
  wk[lo] = 1 - a;
  wk[hi] = a;
}

static void blend2d_weights(atree *t, bnode *b, float *wk) {
  int n = b->kids.count;
  float x = param(t, b->param), y = param(t, b->param_y), sum = 0;
  for (int i = 0; i < n; i++) {
    float wi = 1, xi = b->px.data[i], yi = b->py.data[i];
    for (int j = 0; j < n; j++) {
      if (j == i) continue;
      float dx = b->px.data[j] - xi, dy = b->py.data[j] - yi, l2 = dx * dx + dy * dy;
      if (l2 == 0) continue;
      float h = 1 - ((x - xi) * dx + (y - yi) * dy) / l2;
      h = h < 0 ? 0 : h > 1 ? 1 : h;
      if (h < wi) wi = h;
    }
    wk[i] = wi;
    sum += wi;
  }
  if (sum > 0) {
    for (int i = 0; i < n; i++) wk[i] /= sum;
    return;
  }
  int best = 0;
  float bd = 0;
  for (int i = 0; i < n; i++) {
    float dx = x - b->px.data[i], dy = y - b->py.data[i], d = dx * dx + dy * dy;
    if (i == 0 || d < bd) { bd = d; best = i; }
  }
  wk[best] = 1;
}

static void eval_kids(atree *t, int id, const float *wk, float w, double dt, const cw_map *filter, bool additive) {
  int n = t->nodes.data[id].kids.count;
  float best = -1;
  bool ended = false;
  for (int i = 0; i < n; i++) {
    int k = t->nodes.data[id].kids.data[i];
    eval(t, k, w * wk[i], dt, filter, additive);
    if (wk[i] > best) { best = wk[i]; ended = t->nodes.data[k].ended; }
  }
  t->nodes.data[id].ended = ended;
}

static void eval(atree *t, int id, float w, double dt, const cw_map *filter, bool additive) {
  bnode *b = &t->nodes.data[id];
  if (b->filter) filter = b->filter;
  b->weight = w;
  switch (b->kind) {
  case BN_CLIP: {
    cw_anim a = cw__player_lookup(t->player_node, b->anim_name);
    anim_rec *r = cw__anim(a);
    if (!r) { b->ended = false; return; }
    bool fire = w > 0 && !additive;
    int before = t->fired.count;
    cw__cursor_advance(r, &b->cur, dt * b->speed, fire ? &t->fired : NULL);
    b = &t->nodes.data[id];
    b->ended = b->cur.ended || b->cur.finished;
    if (t->fired.count > before) {
      /* keep only key events (signals belong to players) */
      int k = before;
      for (int i = before; i < t->fired.count; i++)
        if (t->fired.data[i].kind == 0) t->fired.data[k++] = t->fired.data[i];
      t->fired.count = k;
      if (k > before) cw_array_push(&t->spans, ((fired_span){ a, k - before }));
    }
    if (w > 0) cw__amix_sample(t->mix, a, (float)b->cur.time, w, filter, additive, &b->cur);
    return;
  }
  case BN_BLEND1D: case BN_BLEND2D: {
    float *wk = cw_frame_alloc(sizeof(float) * (size_t)(b->kids.count + 1));
    if (b->kind == BN_BLEND1D) blend1d_weights(t, b, wk);
    else blend2d_weights(t, b, wk);
    eval_kids(t, id, wk, w, dt, filter, additive);
    return;
  }
  case BN_BLEND2: {
    float a = cw_clampf(param(t, b->param), 0, 1), wk[2] = { 1 - a, a };
    if (b->kids.count == 2) eval_kids(t, id, wk, w, dt, filter, additive);
    return;
  }
  case BN_ADD: {
    if (b->kids.count != 2) return;
    float amount = param(t, b->param);
    int base = b->kids.data[0], add = b->kids.data[1];
    eval(t, base, w, dt, filter, additive);
    eval(t, add, w * amount, dt, filter, true);
    t->nodes.data[id].ended = t->nodes.data[base].ended;
    return;
  }
  case BN_SM: {
    sm_step(t, id, dt);
    b = &t->nodes.data[id];
    if (!b->states.count) return;
    int cur = b->states.data[b->current].node;
    if (b->next >= 0) {
      float f = b->fade > 0 ? cw_clampf(b->fade_t / b->fade, 0, 1) : 1;
      int nx = b->states.data[b->next].node;
      eval(t, cur, w * (1 - f), dt, filter, additive);
      eval(t, nx, w * f, dt, filter, additive);
    } else {
      eval(t, cur, w, dt, filter, additive);
    }
    b = &t->nodes.data[id];
    t->nodes.data[id].ended = t->nodes.data[b->states.data[b->current].node].ended;
    return;
  }
  }
}

static void advance(cw_node n, atree *t, double dt) {
  CW_PROFILE_SCOPE("anim.tree");
  if (!t->active || t->root < 0 || !t->nodes.count) return;
  t->player_node = t->player && *t->player ? cw_node_get(n, t->player) : (cw_node){ 0 };
  if (!cw_node_valid(t->player_node)) {
    static bool warned;
    if (!warned) cw_warn("AnimationTree '%s': player '%s' not found (set its `player` path)", cw_node_name(n), t->player ? t->player : "");
    warned = true;
    return;
  }
  cw__amix_set_root(t->mix, cw__player_root(t->player_node));
  t->mix->rm_track = t->root_motion_track && *t->root_motion_track ? t->root_motion_track : NULL;
  t->mix->rm_lock = t->root_motion_lock;
  t->mix->rm_target = t->root_motion_target && *t->root_motion_target ? cw_node_get(n, t->root_motion_target) : (cw_node){ 0 };
  cw_array_clear(&t->fired);
  cw_array_clear(&t->spans);
  eval(t, t->root, 1, dt, NULL, false);
  cw__amix_apply(t->mix);
  /* keys, then state changes */
  cw_node root = cw__player_root(t->player_node);
  int at = 0;
  int nspans = t->spans.count;
  for (int s = 0; s < nspans; s++) {
    fired_span sp = t->spans.data[s];
    anim_rec *r = cw__anim(sp.anim);
    for (int i = 0; i < sp.count && r; i++, at++) {
      anim_fired f = t->fired.data[at];
      anim_track *tr = &r->tracks.data[f.track];
      anim_mkey *k = &tr->mkeys.data[f.key];
      if (tr->kind == CW_TRACK_METHOD) cw__fire_method_key(root, tr, k);
      else {
        cw_value args[2] = { cw_val_string(k->name), cw_val_string(r->name) };
        cw_emit(n, "animation_event", args, 2);
      }
      t = T(n, NULL);
      if (!t) return;
      r = cw__anim(sp.anim);
    }
  }
  /* states entered by this tick, or by sm_start/travel since the last one */
  for (int i = 0; i < t->entered.count; i++) {
    cw_value v = cw_val_string(t->entered.data[i]);
    cw_emit(n, "state_changed", &v, 1);
    t = T(n, NULL);
    if (!t) return;
  }
  cw_array_clear(&t->entered);
}

void cw_animtree_advance(cw_node n, double dt) {
  atree *t = T(n, "cw_animtree_advance");
  if (t) advance(n, t, dt);
}
uint32_t cw_animtree_hash(cw_node n) { atree *t = T(n, NULL); return t ? t->mix->hash : 0; }
t3_vec3 cw_animtree_root_motion(cw_node n) { atree *t = T(n, NULL); return t ? t->mix->rm_last : (t3_vec3){ 0, 0, 0 }; }
float cw_animtree_clip_weight(cw_node n, cw_blend_id clip) {
  atree *t = T(n, "cw_animtree_clip_weight");
  bnode *b = t ? B(t, clip, "cw_animtree_clip_weight") : NULL;
  return b ? b->weight : 0;
}

/* ── the node type ──────────────────────────────────────────────────── */

static void t_init(cw_node n, void *d) {
  atree *t = d;
  t->active = true;
  t->root = -1;
  t->root_motion_lock = 7;
  t->mix = cw_alloc(sizeof *t->mix, CW_MEM_ANIM);
  cw__amix_init(t->mix, n);
}

static void t_destroy(cw_node n, void *d) {
  (void)n;
  atree *t = d;
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *b = &t->nodes.data[i];
    cw_array_free(&b->kids);
    cw_array_free(&b->px);
    cw_array_free(&b->py);
    cw_array_free(&b->states);
    cw_array_free(&b->trans);
    cw_array_free(&b->travel);
    if (b->filter) { cw_map_free(b->filter); cw_free(b->filter); }
  }
  cw_array_free(&t->nodes);
  cw_array_free(&t->params);
  cw_array_free(&t->fired);
  cw_array_free(&t->spans);
  cw_array_free(&t->entered);
  cw__amix_free(t->mix);
  cw_free(t->mix);
}

static void t_fixed(cw_node n, void *d, double dt) {
  atree *t = d;
  if (t->process == CW_ANIM_PROCESS_FIXED) advance(n, t, dt);
}
static void t_update(cw_node n, void *d, double dt) {
  atree *t = d;
  if (t->process == CW_ANIM_PROCESS_UPDATE) advance(n, t, dt);
}

extern const char *const cw__anim_process_names[];

static const cw_prop tree_props[] = {
  CW_PROP_FIELD(atree, player, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, "AnimationPlayer (path from this node) whose library and root_node it uses."),
  CW_PROP_FIELD(atree, active, CW_T_BOOL, CW_PROP_DEFAULT, "Evaluates the graph each tick."),
  { .name = "process", .type = CW_T_ENUM, .flags = CW_PROP_STORED | CW_PROP_EDITOR, .offset = offsetof(atree, process),
    .size = sizeof(int32_t), .enum_names = cw__anim_process_names, .doc = "Which phase advances it." },
  CW_PROP_FIELD(atree, root_motion_track, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, "Position track whose motion moves root_motion_target."),
  CW_PROP_FIELD(atree, root_motion_target, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, "Node3D (path from the tree) moved by root motion."),
  CW_PROP_FIELD(atree, root_motion_lock, CW_T_INT, CW_PROP_STORED | CW_PROP_EDITOR, "Axes given to the target: bit 0 x, 1 y, 2 z."),
  CW_PROP_END,
};

static const char *const tree_signals[] = { "state_changed", "animation_event", NULL };

static cw_node_type tree_type = {
  .name = "AnimationTree",
  .parent = "Node",
  .data_size = sizeof(atree),
  .props = tree_props,
  .signals = tree_signals,
  .doc = "Blends a player's animations through a graph: state machines, blend spaces, additive layers.",
  .init = t_init,
  .destroy = t_destroy,
  .fixed_update = t_fixed,
  .update = t_update,
};

void cw__register_tree_type(void) {
  cw_node_type_register(&tree_type);
  tree_t = &tree_type;
}

/* ── debug channel ──────────────────────────────────────────────────── */

#include "cartwheel/debug.h"

static void cmd_tree(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: animtree <tree path>"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationTree");
  if (!cw_node_valid(n)) return;
  atree *t = T(n, NULL);
  cw_reply_printf(r, "{\"active\":%s,\"hash\":%u,\"params\":{", t->active ? "true" : "false", t->mix->hash);
  for (int i = 0; i < t->params.count; i++) {
    if (i) cw_reply_raw(r, ",");
    cw_reply_str(r, t->params.data[i].name);
    cw_reply_printf(r, ":%.9g", t->params.data[i].value);
  }
  cw_reply_raw(r, "},\"machines\":[");
  bool first = true;
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *b = &t->nodes.data[i];
    if (b->kind != BN_SM || !b->states.count) continue;
    if (!first) cw_reply_raw(r, ",");
    first = false;
    cw_reply_printf(r, "{\"id\":%d,\"current\":", i);
    cw_reply_str(r, b->states.data[b->current].name);
    cw_reply_raw(r, ",\"next\":");
    if (b->next >= 0) cw_reply_str(r, b->states.data[b->next].name);
    else cw_reply_raw(r, "null");
    cw_reply_printf(r, ",\"travel\":%d}", b->travel.count);
  }
  cw_reply_raw(r, "],\"clips\":[");
  first = true;
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *b = &t->nodes.data[i];
    if (b->kind != BN_CLIP) continue;
    if (!first) cw_reply_raw(r, ",");
    first = false;
    cw_reply_printf(r, "{\"id\":%d,\"anim\":", i);
    cw_reply_str(r, b->anim_name);
    cw_reply_printf(r, ",\"weight\":%.6g,\"time\":%.9g}", b->weight, b->cur.time);
  }
  cw_reply_raw(r, "]}");
}

static void cmd_param(int argc, const char **argv, cw_reply *r) {
  if (argc < 4) { cw_reply_error(r, "usage: animtree_param <tree path> <name> <value>"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationTree");
  if (!cw_node_valid(n)) return;
  cw_animtree_set_param(n, argv[2], !strcmp(argv[3], "true") ? 1 : !strcmp(argv[3], "false") ? 0 : strtof(argv[3], NULL));
  cw_reply_raw(r, "{\"ok\":true}");
}

static void cmd_travel(int argc, const char **argv, cw_reply *r) {
  if (argc < 4) { cw_reply_error(r, "usage: animtree_travel <tree path> <machine id> <state> (ids: animtree <path>)"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationTree");
  if (!cw_node_valid(n)) return;
  atree *t = T(n, NULL);
  int id = atoi(argv[2]);
  if (id < 0 || id >= t->nodes.count || t->nodes.data[id].kind != BN_SM) { cw_reply_error(r, "blend node %d is not a state machine", id); return; }
  if (state_index(&t->nodes.data[id], argv[3]) < 0) { cw_reply_error(r, "no state '%s'", argv[3]); return; }
  cw_animtree_sm_travel(n, id, argv[3]);
  cw_reply_raw(r, "{\"ok\":true}");
}

void cw__register_tree_debug_commands(void) {
  cw_debug_command("animtree", "animtree <tree path>: parameters, state machines, clip weights, pose hash", cmd_tree);
  cw_debug_command("animtree_param", "animtree_param <tree path> <name> <value>: set a parameter", cmd_param);
  cw_debug_command("animtree_travel", "animtree_travel <tree path> <machine id> <state>: travel through the graph", cmd_travel);
}

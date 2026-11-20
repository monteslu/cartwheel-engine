/* AnimationTree: a blend graph (clips, 1D/2D blend spaces, lerp, additive,
 * layers with bone masks, one-shots, time scale, state machines) evaluated
 * each tick into a mixer; the graph as text and its [animtree] scene sections. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"
#include "cartwheel/profile.h"
#include "cartwheel/scene_file.h"
#include "scene/scene_internal.h"

static float cw_minf(float a, float b) { return a < b ? a : b; }

enum { BN_CLIP, BN_BLEND1D, BN_BLEND2D, BN_BLEND2, BN_ADD, BN_SM, BN_LAYER, BN_ONESHOT, BN_TIMESCALE, BN_KINDS };
enum { OP_TRUTHY, OP_FALSY, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };

typedef struct { const char *name; int node; } sm_state;
typedef struct {
  int from, to;
  cw_transition t;
  const char *cparam; /* NULL: always */
  const char *ctext;  /* the condition as written (interned; NULL: none) */
  int op;
  float value;
} sm_trans;

typedef struct bnode {
  int kind, parent;
  const char *name; /* its name in graph text (interned; NULL: "n<id>") */
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
  bool sync;          /* blend spaces: clip children share one cycle phase */
  cw_map *filter;     /* interned names allowed (value 1: listed, 2: a descendant of a "name+" entry) */
  CW_ARRAY(const char *) filter_names; /* as given, in order ("Spine+" keeps its +) */
  uint64_t filter_ver; /* what the "name+" entries were last expanded against */
  /* one-shot */
  float fadein, fadeout;
  bool os_active;
  float os_t, os_out; /* seconds since it fired; seconds into an asked-for fade-out (< 0: none) */
  /* state machine */
  CW_ARRAY(sm_state) states;
  CW_ARRAY(sm_trans) trans;
  int current, next;
  float fade, fade_t;
  CW_ARRAY(int) travel;
} bnode;

typedef struct { const char *name; float value; bool declared; float def; } tparam;
/* the graph an enclosing scene replaced (its [animtree] section names a tree inside an instance) */
typedef struct { uint64_t root; const char *before; } toverride;
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
  anim_event_hook hook;
  CW_ARRAY(fired_span) spans;
  CW_ARRAY(const char *) entered;
  /* scene files */
  bool graph_stored;       /* written to scene files (a graph loaded from text) */
  const char *loaded_text; /* the graph as the last scene section left it (interned) */
  CW_ARRAY(toverride) overrides;
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
  cw_array_push(&t->params, ((tparam){ in, value, false, 0 }));
}
void cw_animtree_declare_param(cw_node n, const char *name, float value) {
  atree *t = T(n, "cw_animtree_declare_param");
  if (!t) return;
  cw_animtree_set_param(n, name, value);
  const char *in = cw_intern(name);
  for (int i = 0; i < t->params.count; i++)
    if (t->params.data[i].name == in) { t->params.data[i].declared = true; t->params.data[i].def = value; }
}
static void set_param_raw(atree *t, const char *in, float value) {
  for (int i = 0; i < t->params.count; i++)
    if (t->params.data[i].name == in) { t->params.data[i].value = value; return; }
  cw_array_push(&t->params, ((tparam){ in, value, false, 0 }));
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

cw_blend_id cw_animtree_layer(cw_node n, cw_blend_id base, cw_blend_id over, const char *p) {
  cw_blend_id id = two(n, BN_LAYER, base, over, p && *p ? p : NULL, "cw_animtree_layer");
  return id;
}
cw_blend_id cw_animtree_oneshot(cw_node n, cw_blend_id base, cw_blend_id shot, const char *p, float fadein, float fadeout) {
  cw_blend_id id = two(n, BN_ONESHOT, base, shot, p, "cw_animtree_oneshot");
  atree *t = T(n, NULL);
  if (t && id >= 0) {
    t->nodes.data[id].fadein = fadein > 0 ? fadein : 0;
    t->nodes.data[id].fadeout = fadeout > 0 ? fadeout : 0;
    t->nodes.data[id].os_out = -1;
  }
  return id;
}
bool cw_animtree_oneshot_active(cw_node n, cw_blend_id id) {
  atree *t = T(n, "cw_animtree_oneshot_active");
  bnode *b = t ? B(t, id, "cw_animtree_oneshot_active") : NULL;
  return b && b->kind == BN_ONESHOT && b->os_active;
}
cw_blend_id cw_animtree_time_scale(cw_node n, cw_blend_id child, const char *p) {
  atree *t = T(n, "cw_animtree_time_scale");
  if (!t) return -1;
  cw_blend_id id = new_node(t, BN_TIMESCALE);
  if (!adopt(t, id, child, "cw_animtree_time_scale")) return id;
  bnode *b = &t->nodes.data[id];
  cw_array_push(&b->kids, child);
  b->param = cw_intern(p);
  return id;
}
void cw_animtree_set_sync(cw_node n, cw_blend_id space, bool sync) {
  atree *t = T(n, "cw_animtree_set_sync");
  bnode *b = t ? B(t, space, "cw_animtree_set_sync") : NULL;
  if (!b) return;
  if (b->kind != BN_BLEND1D && b->kind != BN_BLEND2D) { cw_warn("cw_animtree_set_sync: blend node %d is not a blend space", space); return; }
  b->sync = sync;
}

void cw_animtree_set_filter(cw_node n, cw_blend_id id, const char *const *names, int count) {
  atree *t = T(n, "cw_animtree_set_filter");
  bnode *b = t ? B(t, id, "cw_animtree_set_filter") : NULL;
  if (!b) return;
  if (!b->filter) {
    b->filter = cw_alloc(sizeof(cw_map), CW_MEM_ANIM);
    cw_map_init(b->filter, CW_MEM_ANIM);
  }
  for (int i = 0; i < count; i++) {
    const char *in = cw_intern(names[i]);
    bool had = false;
    for (int k = 0; k < b->filter_names.count; k++) had |= b->filter_names.data[k] == in;
    if (had) continue;
    cw_array_push(&b->filter_names, in);
    size_t len = strlen(in);
    if (len > 1 && in[len - 1] == '+') cw_map_set(b->filter, (uint64_t)(uintptr_t)cw_intern_n(in, len - 1), 1);
    else cw_map_set(b->filter, (uint64_t)(uintptr_t)in, 1);
  }
  b->filter_ver = 0; /* expand "name+" entries again */
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
  tr.ctext = tr.t.condition && *tr.t.condition ? cw_intern(tr.t.condition) : NULL;
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
  b->os_active = false;
  b->os_t = 0;
  b->os_out = -1;
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

/* A node's place in its cycle, 0..1: a clip's time over its length; a blend
 * space's first clip child (a synced space keeps them all alike). false when
 * the node has no single cycle (a state machine, a layer). */
static bool node_phase(atree *t, int id, float *phase) {
  bnode *b = &t->nodes.data[id];
  if (b->kind == BN_CLIP) {
    anim_rec *r = cw__anim(cw__player_lookup(t->player_node, b->anim_name));
    if (!r || r->length <= 0) return false;
    *phase = (float)(b->cur.time / (double)r->length);
    return true;
  }
  if (b->kind == BN_BLEND1D || b->kind == BN_BLEND2D || b->kind == BN_TIMESCALE) {
    for (int i = 0; i < b->kids.count; i++)
      if (node_phase(t, b->kids.data[i], phase)) return true;
  }
  return false;
}
static void node_set_phase(atree *t, int id, float phase) {
  bnode *b = &t->nodes.data[id];
  if (b->kind == BN_CLIP) {
    anim_rec *r = cw__anim(cw__player_lookup(t->player_node, b->anim_name));
    if (!r || r->length <= 0) return;
    b->cur.time = (double)phase * (double)r->length;
    b->cur.fire_start = false; /* it joins mid-cycle: keys before this point do not fire */
    return;
  }
  if (b->kind == BN_BLEND1D || b->kind == BN_BLEND2D || b->kind == BN_TIMESCALE)
    for (int i = 0; i < b->kids.count; i++) node_set_phase(t, t->nodes.data[id].kids.data[i], phase);
}

static void enter_ex(atree *t, int sm_id, int state, float xfade, bool sync) {
  bnode *b = &t->nodes.data[sm_id];
  if (state < 0 || state >= b->states.count) return;
  int node = b->states.data[state].node;
  float phase = 0;
  bool has_phase = sync && b->states.count && state != b->current && node_phase(t, b->states.data[b->current].node, &phase);
  reset_node(t, node);
  if (has_phase) node_set_phase(t, node, phase);
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
static void enter(atree *t, int sm_id, int state, float xfade) { enter_ex(t, sm_id, state, xfade, false); }

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
    enter_ex(t, sm_id, hop, tr->t.xfade, tr->t.sync);
    return;
  }
  for (int i = 0; i < b->trans.count; i++) {
    const sm_trans *tr = &b->trans.data[i];
    if (tr->from != b->current || tr->t.advance != CW_ADVANCE_AUTO || !cond_ok(t, tr)) continue;
    if (tr->t.switch_mode == CW_SWITCH_AT_END && !ended) continue;
    enter_ex(t, sm_id, tr->to, tr->t.xfade, tr->t.sync);
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

/* a clip node's cycle length in seconds of tree time (its clip's length over its speed); 0: not a clip */
static float clip_cycle(atree *t, int id) {
  bnode *b = &t->nodes.data[id];
  if (b->kind != BN_CLIP) return 0;
  anim_rec *r = cw__anim(cw__player_lookup(t->player_node, b->anim_name));
  float sp = b->speed < 0 ? -b->speed : b->speed;
  return r && r->length > 0 && sp > 0 ? r->length / sp : 0;
}

static void eval_kids(atree *t, int id, const float *wk, float w, double dt, const cw_map *filter, bool additive) {
  int n = t->nodes.data[id].kids.count;
  float best = -1;
  bool ended = false;
  /* sync: every clip child runs through its cycle in the weighted mean of the
   * cycles, so a walk and a run of different lengths keep their feet in step */
  float cycle = 0;
  if (t->nodes.data[id].sync) {
    float sum = 0, ws = 0;
    for (int i = 0; i < n; i++) {
      float c = clip_cycle(t, t->nodes.data[id].kids.data[i]);
      if (c > 0 && wk[i] > 0) { sum += c * wk[i]; ws += wk[i]; }
    }
    cycle = ws > 0 ? sum / ws : 0;
  }
  for (int i = 0; i < n; i++) {
    int k = t->nodes.data[id].kids.data[i];
    float c = cycle > 0 ? clip_cycle(t, k) : 0;
    eval(t, k, w * wk[i], c > 0 ? dt * (double)(c / cycle) : dt, filter, additive);
    if (wk[i] > best) { best = wk[i]; ended = t->nodes.data[k].ended; }
  }
  t->nodes.data[id].ended = ended;
}

/* "name+" filter entries: the bone and every bone below it. Expanded against
 * the bone tracks of the player's library (a bone no clip animates needs no
 * entry), again when the tree's structure or the library changes. */
static void expand_filter(atree *t, bnode *b) {
  cw_tree *tr = cw_node_tree(t->player_node);
  uint64_t ver = (tr ? tr->structure_version : 0) * 1000003u + (uint64_t)cw_anim_player_count(t->player_node) + 1;
  if (b->filter_ver == ver) return;
  b->filter_ver = ver;
  bool any = false;
  for (int i = 0; i < b->filter_names.count; i++) {
    const char *nm = b->filter_names.data[i];
    size_t len = strlen(nm);
    any |= len > 1 && nm[len - 1] == '+';
  }
  if (!any) return;
  cw_node root = cw__player_root(t->player_node);
  int na = cw_anim_player_count(t->player_node);
  for (int a = 0; a < na; a++) {
    anim_rec *r = cw__anim(cw__player_lookup(t->player_node, cw_anim_player_name_at(t->player_node, a)));
    for (int k = 0; r && k < r->tracks.count; k++) {
      const anim_track *tk = &r->tracks.data[k];
      uint64_t v;
      if (!tk->bone || cw_map_get(b->filter, (uint64_t)(uintptr_t)tk->bone, &v)) continue;
      cw_node node = tk->node[0] ? cw_node_get(root, tk->node) : root;
      t3_object *top = cw_node_valid(node) ? cw_node3d_object(node) : NULL;
      t3_object *o = top ? t3_object_get_by_name(top, tk->bone) : NULL;
      for (o = o ? o->parent : NULL; o && o != top; o = o->parent) {
        if (!o->name || !*o->name) continue;
        const char *in = cw_intern(o->name);
        if (cw_map_get(b->filter, (uint64_t)(uintptr_t)in, &v) && v == 1) {
          /* only an ancestor listed with + carries its descendants */
          bool plus = false;
          for (int i = 0; i < b->filter_names.count && !plus; i++) {
            const char *nm = b->filter_names.data[i];
            size_t len = strlen(nm);
            plus = len > 1 && nm[len - 1] == '+' && strlen(in) == len - 1 && !strncmp(nm, in, len - 1);
          }
          if (plus) { cw_map_set(b->filter, (uint64_t)(uintptr_t)tk->bone, 2); break; }
        }
      }
    }
  }
}

static void clear_weights(atree *t, int id) {
  bnode *b = &t->nodes.data[id];
  b->weight = 0;
  for (int i = 0; i < b->kids.count; i++) clear_weights(t, b->kids.data[i]);
  for (int i = 0; i < b->states.count; i++) clear_weights(t, b->states.data[i].node);
}

/* base at the node's weight, then the overlay input replaces `amount` of it on the bones the overlay writes */
static void eval_overlay(atree *t, int id, float amount, float w, double dt, const cw_map *filter, bool additive, bool add_mode) {
  bnode *b = &t->nodes.data[id];
  int base = b->kids.data[0], over = b->kids.data[1];
  const cw_map *of = filter;
  if (b->filter) { expand_filter(t, b); of = t->nodes.data[id].filter; }
  eval(t, base, w, dt, filter, additive);
  if (additive || add_mode) eval(t, over, w * amount, dt, of, true);
  else if (amount > 0 && w > 0 && cw__amix_layer_begin(t->mix)) {
    eval(t, over, 1, dt, of, false);
    cw__amix_layer_end(t->mix, amount, w);
  } else eval(t, over, 0, dt, of, false);
  t->nodes.data[id].ended = t->nodes.data[base].ended;
}

static void eval(atree *t, int id, float w, double dt, const cw_map *filter, bool additive) {
  bnode *b = &t->nodes.data[id];
  if (b->filter && b->kind != BN_LAYER && b->kind != BN_ONESHOT) {
    expand_filter(t, b);
    b = &t->nodes.data[id];
    filter = b->filter;
  }
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
  case BN_LAYER: {
    if (b->kids.count != 2) return;
    eval_overlay(t, id, b->param ? cw_clampf(param(t, b->param), 0, 1) : 1, w, dt, filter, additive, false);
    return;
  }
  case BN_ONESHOT: {
    if (b->kids.count != 2) return;
    int shot = b->kids.data[1];
    float req = param(t, b->param);
    if (req > 0) { /* fire (again): the shot starts over */
      reset_node(t, shot);
      b = &t->nodes.data[id];
      b->os_active = true;
      b->os_t = 0;
      b->os_out = -1;
      set_param_raw(t, b->param, 0);
    } else if (req < 0) { /* abort: fade out from here */
      if (b->os_active && b->os_out < 0) b->os_out = 0;
      set_param_raw(t, b->param, 0);
    }
    float amount = 0;
    if (b->os_active) {
      amount = b->fadein > 0 ? cw_clampf(b->os_t / b->fadein, 0, 1) : 1;
      /* a clip that ends: fade out over its last `fadeout` seconds */
      bnode *sb = &t->nodes.data[shot];
      anim_rec *r = sb->kind == BN_CLIP ? cw__anim(cw__player_lookup(t->player_node, sb->anim_name)) : NULL;
      if (r && r->loop == CW_LOOP_NONE && sb->speed > 0) {
        float left = (float)(((double)r->length - sb->cur.time) / (double)sb->speed);
        if (b->fadeout > 0 && left < b->fadeout) amount = cw_minf(amount, cw_clampf(left / b->fadeout, 0, 1));
        if (left <= 0 && b->os_t > 0) b->os_active = false;
      } else if (sb->ended && b->os_t > 0 && b->os_out < 0) {
        b->os_out = 0; /* anything else: when it reports its end */
      }
      if (b->os_out >= 0) {
        amount = cw_minf(amount, b->fadeout > 0 ? cw_clampf(1 - b->os_out / b->fadeout, 0, 1) : 0);
        if (b->os_out >= b->fadeout) b->os_active = false;
        b->os_out += (float)dt;
      }
      if (!b->os_active) amount = 0;
      b->os_t += (float)dt;
    }
    if (b->os_active) eval_overlay(t, id, amount, w, dt, filter, additive, false);
    else {
      clear_weights(t, shot);
      b = &t->nodes.data[id];
      eval(t, b->kids.data[0], w, dt, filter, additive);
      t->nodes.data[id].ended = t->nodes.data[t->nodes.data[id].kids.data[0]].ended;
    }
    return;
  }
  case BN_TIMESCALE: {
    if (b->kids.count != 1) return;
    int k = b->kids.data[0];
    eval(t, k, w, dt * (double)param(t, b->param), filter, additive);
    t->nodes.data[id].ended = t->nodes.data[k].ended;
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
        if (t->hook.fn) t->hook.fn(n, k->name, r->name, tr->times.data[f.key], t->hook.user);
        if (!T(n, NULL)) return;
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

void cw_animtree_on_event(cw_node n, cw_anim_event_fn fn, void *user) {
  atree *t = T(n, "cw_animtree_on_event");
  if (t) t->hook = (anim_event_hook){ fn, user };
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

static void free_graph(atree *t) {
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *b = &t->nodes.data[i];
    cw_array_free(&b->kids);
    cw_array_free(&b->px);
    cw_array_free(&b->py);
    cw_array_free(&b->states);
    cw_array_free(&b->trans);
    cw_array_free(&b->travel);
    cw_array_free(&b->filter_names);
    if (b->filter) { cw_map_free(b->filter); cw_free(b->filter); }
  }
  cw_array_clear(&t->nodes);
  t->root = -1;
  for (int i = 0; i < t->params.count;) { /* declared parameters belong to the graph */
    if (t->params.data[i].declared) {
      memmove(t->params.data + i, t->params.data + i + 1, sizeof *t->params.data * (size_t)(t->params.count - i - 1));
      t->params.count--;
    } else i++;
  }
  cw_array_clear(&t->fired);
  cw_array_clear(&t->spans);
  cw_array_clear(&t->entered);
}

static void t_destroy(cw_node n, void *d) {
  (void)n;
  atree *t = d;
  free_graph(t);
  cw_array_free(&t->overrides);
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
    cw_reply_printf(r, "{\"id\":%d,\"name\":", i);
    cw_reply_str(r, b->name ? b->name : cw_frame_printf("n%d", i));
    cw_reply_raw(r, ",\"current\":");
    cw_reply_str(r, b->states.data[b->current].name);
    cw_reply_raw(r, ",\"next\":");
    if (b->next >= 0) cw_reply_str(r, b->states.data[b->next].name);
    else cw_reply_raw(r, "null");
    cw_reply_printf(r, ",\"travel\":%d}", b->travel.count);
  }
  cw_reply_raw(r, "],\"oneshots\":[");
  first = true;
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *b = &t->nodes.data[i];
    if (b->kind != BN_ONESHOT) continue;
    if (!first) cw_reply_raw(r, ",");
    first = false;
    cw_reply_printf(r, "{\"id\":%d,\"name\":", i);
    cw_reply_str(r, b->name ? b->name : cw_frame_printf("n%d", i));
    cw_reply_raw(r, ",\"param\":");
    cw_reply_str(r, b->param ? b->param : "");
    cw_reply_printf(r, ",\"active\":%s}", b->os_active ? "true" : "false");
  }
  cw_reply_raw(r, "],\"clips\":[");
  first = true;
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *b = &t->nodes.data[i];
    if (b->kind != BN_CLIP) continue;
    if (!first) cw_reply_raw(r, ",");
    first = false;
    cw_reply_printf(r, "{\"id\":%d,\"name\":", i);
    cw_reply_str(r, b->name ? b->name : cw_frame_printf("n%d", i));
    cw_reply_raw(r, ",\"anim\":");
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
  if (argc < 4) { cw_reply_error(r, "usage: animtree_travel <tree path> <machine name or id> <state> (names and ids: animtree <path>)"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationTree");
  if (!cw_node_valid(n)) return;
  atree *t = T(n, NULL);
  int id = cw_animtree_find(n, argv[2]);
  if (id < 0 && argv[2][0] >= '0' && argv[2][0] <= '9') id = atoi(argv[2]);
  if (id < 0 || id >= t->nodes.count || t->nodes.data[id].kind != BN_SM) { cw_reply_error(r, "'%s' is not a state machine of this tree (see: animtree %s)", argv[2], argv[1]); return; }
  if (state_index(&t->nodes.data[id], argv[3]) < 0) { cw_reply_error(r, "no state '%s'", argv[3]); return; }
  cw_animtree_sm_travel(n, id, argv[3]);
  cw_reply_raw(r, "{\"ok\":true}");
}

static void cmd_tree_text(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: animtree_text <tree path>: the blend graph as [animtree] section text"); return; }
  cw_node n = cw__anim_debug_node(argv[1], r, "AnimationTree");
  if (!cw_node_valid(n)) return;
  char *t = cw_animtree_to_text(n);
  cw_reply_str(r, t);
  cw_free(t);
}

void cw__register_tree_debug_commands(void) {
  cw_debug_command("animtree_text", "animtree_text <tree path>: the blend graph as .cwscene [animtree] text", cmd_tree_text);
  cw_debug_command("animtree", "animtree <tree path>: parameters, state machines, clip weights, pose hash", cmd_tree);
  cw_debug_command("animtree_param", "animtree_param <tree path> <name> <value>: set a parameter", cmd_param);
  cw_debug_command("animtree_travel", "animtree_travel <tree path> <machine name or id> <state>: travel through the graph", cmd_travel);
}

/* ── the graph as text ([animtree] sections) ───────────────────────────
 * One statement per line: a keyword, then key=value attributes (values
 * quoted when they hold spaces). A node statement names a graph node; the
 * lines that fill it (point, state, transition) follow it. Nodes may be
 * named before they are defined: the loader makes every node first, then
 * wires them. Written back in node order, defaults omitted, so a loaded
 * graph saves byte for byte.
 *
 *   param name="speed" value=0
 *   clip name="walk" anim="walk" speed=1.2
 *   blend1d name="move" param="speed" sync=true
 *   point of="move" node="idle" x=0
 *   blend2d name="look" param="look_x" param_y="look_y"
 *   point of="look" node="look_l" x=-1 y=0
 *   blend2 name="mix" a="x" b="y" param="amount"
 *   add name="lean" base="move" add="lean_clip" param="lean"
 *   layer name="aim" base="body" over="aim_pose" param="aiming" bones="Spine+|Head"
 *   oneshot name="hit" base="aim" shot="swing" param="attack" fadein=0.08 fadeout=0.15 bones="Spine+"
 *   timescale name="slow" node="hit" param="rate"
 *   machine name="body"
 *   state of="body" name="ground" node="move"
 *   transition of="body" from="ground" to="jump" xfade=0.06 if="jumping" advance=ENABLED switch=AT_END sync=true
 *   filter node="lean_clip" bones="Spine|Chest"
 *   root node="slow" */

static const char *const kind_names[BN_KINDS] = { "clip", "blend1d", "blend2d", "blend2", "add", "machine", "layer", "oneshot", "timescale" };
static const char *const advance_names[] = { "AUTO", "ENABLED", "DISABLED" };
static const char *const switch_names[] = { "IMMEDIATE", "AT_END" };

void cw_animtree_clear(cw_node n) {
  atree *t = T(n, "cw_animtree_clear");
  if (t) free_graph(t);
}
cw_blend_id cw_animtree_root(cw_node n) { atree *t = T(n, "cw_animtree_root"); return t ? t->root : -1; }
int cw_animtree_node_count(cw_node n) { atree *t = T(n, "cw_animtree_node_count"); return t ? t->nodes.count : 0; }

cw_blend_id cw_animtree_find(cw_node n, const char *name) {
  atree *t = T(n, "cw_animtree_find");
  if (!t || !name) return -1;
  const char *in = cw_intern(name);
  for (int i = 0; i < t->nodes.count; i++)
    if (t->nodes.data[i].name == in) return i;
  /* unnamed nodes answer to n<id>, as the text writes them */
  if (name[0] == 'n' && name[1] >= '0' && name[1] <= '9') {
    char *end;
    long id = strtol(name + 1, &end, 10);
    if (!*end && id >= 0 && id < t->nodes.count && !t->nodes.data[id].name) return (cw_blend_id)id;
  }
  return -1;
}
void cw_animtree_set_name(cw_node n, cw_blend_id id, const char *name) {
  atree *t = T(n, "cw_animtree_set_name");
  bnode *b = t ? B(t, id, "cw_animtree_set_name") : NULL;
  if (b) b->name = name && *name ? cw_intern(name) : NULL;
}
static const char *gname(atree *t, int id) {
  return t->nodes.data[id].name ? t->nodes.data[id].name : cw_intern(cw_frame_printf("n%d", id));
}
const char *cw_animtree_name(cw_node n, cw_blend_id id) {
  atree *t = T(n, "cw_animtree_name");
  return t && id >= 0 && id < t->nodes.count ? gname(t, id) : NULL;
}
void cw_animtree_set_stored(cw_node n, bool stored) {
  atree *t = T(n, "cw_animtree_set_stored");
  if (t) t->graph_stored = stored;
}

typedef struct { char *buf; size_t len, cap; } gbuf;
static void gput(gbuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2 + 256;
    b->buf = cw_realloc(b->buf, b->cap, CW_MEM_ANIM);
  }
  memcpy(b->buf + b->len, s, n + 1);
  b->len += n;
}
static void gfloat(gbuf *b, float f) {
  char tmp[64];
  cw_value v = cw_val_float(f);
  cw_value_format(&v, tmp, sizeof tmp);
  gput(b, tmp);
}
static void gattr_s(gbuf *b, const char *key, const char *s) {
  char tmp[2100];
  cw_value v = { CW_T_STRING };
  v.s = s ? s : "";
  cw_value_format(&v, tmp, sizeof tmp);
  gput(b, " ");
  gput(b, key);
  gput(b, "=");
  gput(b, tmp);
}
static void gattr_f(gbuf *b, const char *key, float f) {
  gput(b, " ");
  gput(b, key);
  gput(b, "=");
  gfloat(b, f);
}
static void gbones(gbuf *b, bnode *nd) {
  if (!nd->filter_names.count) return;
  gbuf names = { 0 };
  for (int k = 0; k < nd->filter_names.count; k++) {
    if (k) gput(&names, "|");
    gput(&names, nd->filter_names.data[k]);
  }
  gattr_s(b, "bones", names.buf);
  cw_free(names.buf);
}

char *cw_animtree_to_text(cw_node n) {
  atree *t = T(n, "cw_animtree_to_text");
  gbuf b = { 0 };
  gput(&b, "");
  if (!t) return b.buf;
  for (int i = 0; i < t->params.count; i++) {
    if (!t->params.data[i].declared) continue;
    gput(&b, "param");
    gattr_s(&b, "name", t->params.data[i].name);
    gattr_f(&b, "value", t->params.data[i].def);
    gput(&b, "\n");
  }
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *nd = &t->nodes.data[i];
    gput(&b, kind_names[nd->kind]);
    gattr_s(&b, "name", gname(t, i));
    bool two_in = nd->kids.count == 2;
    switch (nd->kind) {
    case BN_CLIP:
      gattr_s(&b, "anim", nd->anim_name);
      if (nd->speed != 1) gattr_f(&b, "speed", nd->speed);
      break;
    case BN_BLEND1D:
      gattr_s(&b, "param", nd->param);
      if (nd->sync) gput(&b, " sync=true");
      break;
    case BN_BLEND2D:
      gattr_s(&b, "param", nd->param);
      gattr_s(&b, "param_y", nd->param_y);
      if (nd->sync) gput(&b, " sync=true");
      break;
    case BN_BLEND2:
      if (two_in) { gattr_s(&b, "a", gname(t, nd->kids.data[0])); gattr_s(&b, "b", gname(t, nd->kids.data[1])); }
      gattr_s(&b, "param", nd->param);
      break;
    case BN_ADD:
      if (two_in) { gattr_s(&b, "base", gname(t, nd->kids.data[0])); gattr_s(&b, "add", gname(t, nd->kids.data[1])); }
      gattr_s(&b, "param", nd->param);
      break;
    case BN_LAYER:
      if (two_in) { gattr_s(&b, "base", gname(t, nd->kids.data[0])); gattr_s(&b, "over", gname(t, nd->kids.data[1])); }
      if (nd->param) gattr_s(&b, "param", nd->param);
      gbones(&b, nd);
      break;
    case BN_ONESHOT:
      if (two_in) { gattr_s(&b, "base", gname(t, nd->kids.data[0])); gattr_s(&b, "shot", gname(t, nd->kids.data[1])); }
      gattr_s(&b, "param", nd->param);
      if (nd->fadein != 0) gattr_f(&b, "fadein", nd->fadein);
      if (nd->fadeout != 0) gattr_f(&b, "fadeout", nd->fadeout);
      gbones(&b, nd);
      break;
    case BN_TIMESCALE:
      if (nd->kids.count == 1) gattr_s(&b, "node", gname(t, nd->kids.data[0]));
      gattr_s(&b, "param", nd->param);
      break;
    default: break;
    }
    gput(&b, "\n");
    if (nd->kind == BN_BLEND1D || nd->kind == BN_BLEND2D)
      for (int k = 0; k < nd->kids.count; k++) {
        gput(&b, "point");
        gattr_s(&b, "of", gname(t, i));
        gattr_s(&b, "node", gname(t, nd->kids.data[k]));
        gattr_f(&b, "x", nd->px.data[k]);
        if (nd->kind == BN_BLEND2D) gattr_f(&b, "y", nd->py.data[k]);
        gput(&b, "\n");
      }
    for (int k = 0; k < nd->states.count; k++) {
      gput(&b, "state");
      gattr_s(&b, "of", gname(t, i));
      gattr_s(&b, "name", nd->states.data[k].name);
      gattr_s(&b, "node", gname(t, nd->states.data[k].node));
      gput(&b, "\n");
    }
    for (int k = 0; k < nd->trans.count; k++) {
      sm_trans *tr = &nd->trans.data[k];
      gput(&b, "transition");
      gattr_s(&b, "of", gname(t, i));
      gattr_s(&b, "from", nd->states.data[tr->from].name);
      gattr_s(&b, "to", nd->states.data[tr->to].name);
      if (tr->t.xfade != 0) gattr_f(&b, "xfade", tr->t.xfade);
      if (tr->ctext) gattr_s(&b, "if", tr->ctext);
      if (tr->t.advance != CW_ADVANCE_AUTO) { gput(&b, " advance="); gput(&b, advance_names[tr->t.advance]); }
      if (tr->t.switch_mode != CW_SWITCH_IMMEDIATE) { gput(&b, " switch="); gput(&b, switch_names[tr->t.switch_mode]); }
      if (tr->t.sync) gput(&b, " sync=true");
      gput(&b, "\n");
    }
    if (nd->kind != BN_LAYER && nd->kind != BN_ONESHOT && nd->filter_names.count) {
      gput(&b, "filter");
      gattr_s(&b, "node", gname(t, i));
      gbones(&b, nd);
      gput(&b, "\n");
    }
  }
  if (t->root >= 0 && t->nodes.count) {
    gput(&b, "root");
    gattr_s(&b, "node", gname(t, t->root));
    gput(&b, "\n");
  }
  return b.buf;
}

/* a statement split into its keyword and attributes */
#define GL_MAX_ATTRS 12
typedef struct {
  char kw[24];
  int n;
  const char *key[GL_MAX_ATTRS], *val[GL_MAX_ATTRS]; /* interned */
  bool used[GL_MAX_ATTRS];
} gline;

static bool gline_parse(gline *g, const char *s, char *err, int cap) {
  memset(g, 0, sizeof *g);
  int k = 0;
  while (*s && *s != ' ' && *s != '\t' && k < 23) g->kw[k++] = *s++;
  g->kw[k] = 0;
  for (;;) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return true;
    const char *ks = s;
    while (*s && *s != '=' && *s != ' ') s++;
    if (*s != '=' || s == ks) { snprintf(err, (size_t)cap, "expected key=value after '%s', got '%s'", g->kw, ks); return false; }
    if (g->n == GL_MAX_ATTRS) { snprintf(err, (size_t)cap, "too many attributes"); return false; }
    g->key[g->n] = cw_intern_n(ks, (size_t)(s - ks));
    s++;
    char buf[2048];
    int n = 0;
    if (*s == '"') {
      for (s++; *s && *s != '"' && n < 2047; s++) {
        if (*s == '\\' && s[1]) s++;
        buf[n++] = *s;
      }
      if (*s != '"') { snprintf(err, (size_t)cap, "%s: unterminated string", g->key[g->n]); return false; }
      s++;
    } else {
      while (*s && *s != ' ' && *s != '\t' && n < 2047) buf[n++] = *s++;
    }
    buf[n] = 0;
    g->val[g->n++] = cw_intern(buf);
  }
}
static const char *gattr(gline *g, const char *key) {
  for (int i = 0; i < g->n; i++)
    if (!strcmp(g->key[i], key)) { g->used[i] = true; return g->val[i]; }
  return NULL;
}
/* every attribute a statement did not read is a mistake worth naming (a typo would otherwise do nothing) */
static bool gline_done(gline *g, char *err, int cap) {
  for (int i = 0; i < g->n; i++)
    if (!g->used[i]) { snprintf(err, (size_t)cap, "%s: unknown attribute '%s'", g->kw, g->key[i]); return false; }
  return true;
}
static bool gfloatv(gline *g, const char *key, float *out, char *err, int cap) {
  const char *v = gattr(g, key);
  if (!v) return true;
  char *end;
  float f = strtof(v, &end);
  if (end == v || *end) { snprintf(err, (size_t)cap, "%s=%s is not a number", key, v); return false; }
  *out = f;
  return true;
}
static bool gboolv(gline *g, const char *key, bool *out, char *err, int cap) {
  const char *v = gattr(g, key);
  if (!v) return true;
  if (!strcmp(v, "true")) *out = true;
  else if (!strcmp(v, "false")) *out = false;
  else { snprintf(err, (size_t)cap, "%s=%s: use true or false", key, v); return false; }
  return true;
}
static int name_of(const char *const *names, int count, const char *s) {
  for (int i = 0; s && i < count; i++)
    if (!strcmp(names[i], s)) return i;
  return -1;
}
static void set_bones(cw_node n, int id, const char *bones) {
  const char *list[128];
  int count = 0;
  for (const char *p = bones; *p && count < 128;) {
    const char *e = strchr(p, '|');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len) list[count++] = cw_intern_n(p, len);
    p += len + (e ? 1 : 0);
  }
  cw_animtree_set_filter(n, id, list, count);
}

#define GERR(...) do { snprintf(err, (size_t)cap, __VA_ARGS__); return false; } while (0)

/* pass 0 makes the nodes; pass 1 wires them and reads everything else */
static bool graph_line(cw_node n, atree *t, const char *line, int pass, char *err, int cap) {
  gline g;
  if (!gline_parse(&g, line, err, cap)) return false;
  int kind = name_of(kind_names, BN_KINDS, g.kw);
  if (kind >= 0) {
    const char *name = gattr(&g, "name");
    if (!name || !*name) GERR("%s needs name=", g.kw);
    if (pass == 0) {
      if (cw_animtree_find(n, name) >= 0) GERR("%s '%s' is defined twice", g.kw, name);
      int root_before = t->root;
      cw_blend_id id = new_node(t, kind);
      t->nodes.data[id].name = name;
      t->nodes.data[id].os_out = -1;
      t->root = root_before >= 0 ? root_before : id;
      return true;
    }
    int id = cw_animtree_find(n, name);
    bnode *b = &t->nodes.data[id];
    const char *in[2] = { NULL, NULL }, *in_key[2] = { NULL, NULL };
    switch (kind) {
    case BN_CLIP:
      if (!(b->anim_name = gattr(&g, "anim"))) GERR("clip '%s' needs anim=", name);
      if (!gfloatv(&g, "speed", &b->speed, err, cap)) return false;
      break;
    case BN_BLEND1D: case BN_BLEND2D:
      if (!(b->param = gattr(&g, "param"))) GERR("%s '%s' needs param=", g.kw, name);
      if (kind == BN_BLEND2D && !(b->param_y = gattr(&g, "param_y"))) GERR("blend2d '%s' needs param_y=", name);
      if (!gboolv(&g, "sync", &b->sync, err, cap)) return false;
      break;
    case BN_BLEND2: in_key[0] = "a"; in_key[1] = "b"; break;
    case BN_ADD: in_key[0] = "base"; in_key[1] = "add"; break;
    case BN_LAYER: in_key[0] = "base"; in_key[1] = "over"; break;
    case BN_ONESHOT: in_key[0] = "base"; in_key[1] = "shot"; break;
    case BN_TIMESCALE: in_key[0] = "node"; break;
    default: break;
    }
    for (int i = 0; i < 2 && in_key[i]; i++) {
      in[i] = gattr(&g, in_key[i]);
      if (!in[i]) GERR("%s '%s' needs %s=", g.kw, name, in_key[i]);
      int c = cw_animtree_find(n, in[i]);
      if (c < 0) GERR("%s '%s': %s=\"%s\" names no graph node", g.kw, name, in_key[i], in[i]);
      if (c == id || t->nodes.data[c].parent >= 0) GERR("%s '%s': '%s' already has a parent (graph nodes are not shared)", g.kw, name, in[i]);
      t->nodes.data[c].parent = id;
      if (t->root == c) t->root = id;
      cw_array_push(&t->nodes.data[id].kids, c);
    }
    b = &t->nodes.data[id];
    if (in_key[0]) {
      b->param = gattr(&g, "param");
      if (!b->param && kind != BN_LAYER) GERR("%s '%s' needs param=", g.kw, name);
    }
    if (kind == BN_ONESHOT && (!gfloatv(&g, "fadein", &b->fadein, err, cap) || !gfloatv(&g, "fadeout", &b->fadeout, err, cap))) return false;
    if (kind == BN_LAYER || kind == BN_ONESHOT) {
      const char *bones = gattr(&g, "bones");
      if (bones) set_bones(n, id, bones);
    }
    return gline_done(&g, err, cap);
  }
  if (pass == 0) {
    static const char *const others[] = { "param", "point", "state", "transition", "filter", "root" };
    if (name_of(others, 6, g.kw) < 0)
      GERR("unknown statement '%s' (param, clip, blend1d, blend2d, blend2, add, layer, oneshot, timescale, machine, point, state, transition, filter, root)", g.kw);
    return true;
  }
#define FIND(var, key) \
  const char *var##_name = gattr(&g, key); \
  int var = var##_name ? cw_animtree_find(n, var##_name) : -1; \
  if (var < 0) GERR("%s: %s=\"%s\" names no graph node", g.kw, key, var##_name ? var##_name : "")
  if (!strcmp(g.kw, "param")) {
    const char *name = gattr(&g, "name");
    float v = 0;
    if (!name || !*name) GERR("param needs name=");
    if (!gfloatv(&g, "value", &v, err, cap)) return false;
    cw_animtree_declare_param(n, name, v);
    return gline_done(&g, err, cap);
  }
  if (!strcmp(g.kw, "point")) {
    FIND(p, "of");
    FIND(c, "node");
    bnode *b = &t->nodes.data[p];
    if (b->kind != BN_BLEND1D && b->kind != BN_BLEND2D) GERR("point: '%s' is a %s, not a blend space", p_name, kind_names[b->kind]);
    if (t->nodes.data[c].parent >= 0 || c == p) GERR("point: '%s' already has a parent (graph nodes are not shared)", c_name);
    float x = 0, y = 0;
    if (!gfloatv(&g, "x", &x, err, cap) || !gfloatv(&g, "y", &y, err, cap)) return false;
    if (b->kind == BN_BLEND1D && gattr(&g, "y")) GERR("point: '%s' is a blend1d: it takes x= only", p_name);
    t->nodes.data[c].parent = p;
    if (t->root == c) t->root = p;
    cw_array_push(&b->kids, c);
    cw_array_push(&b->px, x);
    if (b->kind == BN_BLEND2D) cw_array_push(&b->py, y);
    return gline_done(&g, err, cap);
  }
  if (!strcmp(g.kw, "state")) {
    FIND(sm, "of");
    FIND(c, "node");
    const char *name = gattr(&g, "name");
    bnode *b = &t->nodes.data[sm];
    if (b->kind != BN_SM) GERR("state: '%s' is not a machine", sm_name);
    if (!name || !*name || state_index(b, name) >= 0) GERR("state needs a name= that is new in '%s'", sm_name);
    if (t->nodes.data[c].parent >= 0 || c == sm) GERR("state '%s': '%s' already has a parent (graph nodes are not shared)", name, c_name);
    t->nodes.data[c].parent = sm;
    if (t->root == c) t->root = sm;
    cw_array_push(&b->states, ((sm_state){ name, c }));
    return gline_done(&g, err, cap);
  }
  if (!strcmp(g.kw, "transition")) {
    FIND(sm, "of");
    bnode *b = &t->nodes.data[sm];
    if (b->kind != BN_SM) GERR("transition: '%s' is not a machine", sm_name);
    const char *from = gattr(&g, "from"), *to = gattr(&g, "to"), *adv = gattr(&g, "advance"), *sw = gattr(&g, "switch");
    int fi = from ? state_index(b, from) : -1, ti = to ? state_index(b, to) : -1;
    if (fi < 0 || ti < 0) GERR("transition: '%s' has no state '%s' (states come before the transitions that use them)", sm_name, fi < 0 ? (from ? from : "") : to ? to : "");
    int ai = adv ? name_of(advance_names, 3, adv) : 0, si = sw ? name_of(switch_names, 2, sw) : 0;
    if (ai < 0) GERR("transition: advance=%s: use AUTO, ENABLED or DISABLED", adv);
    if (si < 0) GERR("transition: switch=%s: use IMMEDIATE or AT_END", sw);
    sm_trans tr = { fi, ti, { 0 } };
    tr.t.advance = (cw_advance_mode)ai;
    tr.t.switch_mode = (cw_switch_mode)si;
    tr.t.condition = gattr(&g, "if");
    if (!gfloatv(&g, "xfade", &tr.t.xfade, err, cap) || !gboolv(&g, "sync", &tr.t.sync, err, cap)) return false;
    if (!parse_condition(tr.t.condition, &tr))
      GERR("transition %s -> %s: cannot read if=\"%s\" (write \"speed > 0.5\", \"grounded\", \"!grounded\")", from, to, tr.t.condition);
    tr.ctext = tr.t.condition && *tr.t.condition ? tr.t.condition : NULL;
    tr.t.condition = NULL;
    cw_array_push(&b->trans, tr);
    return gline_done(&g, err, cap);
  }
  if (!strcmp(g.kw, "filter")) {
    FIND(id, "node");
    const char *bones = gattr(&g, "bones");
    if (!bones) GERR("filter needs bones=\"a|b\"");
    set_bones(n, id, bones);
    return gline_done(&g, err, cap);
  }
  if (!strcmp(g.kw, "root")) {
    FIND(id, "node");
    t->root = id;
    return gline_done(&g, err, cap);
  }
#undef FIND
  GERR("unknown statement '%s'", g.kw);
}

static bool load_text(cw_node n, atree *t, const char *text, const char *src, char *err, int cap) {
  free_graph(t);
  t->graph_stored = true;
  for (int pass = 0; pass < 2; pass++) {
    int lineno = 0;
    for (const char *p = text ? text : ""; *p;) {
      const char *e = strchr(p, '\n');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      lineno++;
      char line[4096];
      if (len >= sizeof line) {
        snprintf(err, (size_t)cap, "%s:%d: a line over %d bytes", src, lineno, (int)sizeof line);
        free_graph(t);
        return false;
      }
      memcpy(line, p, len);
      line[len] = 0;
      p += e ? len + 1 : len;
      char *s = line;
      while (*s == ' ' || *s == '\t') s++;
      size_t l = strlen(s);
      while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) s[--l] = 0;
      if (!*s || *s == ';' || *s == '#') continue;
      char why[512];
      if (!graph_line(n, t, s, pass, why, sizeof why)) {
        snprintf(err, (size_t)cap, "%s:%d: %s", src, lineno, why);
        free_graph(t);
        return false;
      }
    }
  }
  /* every input wired: a two-input node that lost one would evaluate nothing */
  for (int i = 0; i < t->nodes.count; i++) {
    bnode *b = &t->nodes.data[i];
    if (b->kind == BN_SM && !b->states.count) {
      snprintf(err, (size_t)cap, "%s: machine '%s' has no states", src, gname(t, i));
      free_graph(t);
      return false;
    }
    if ((b->kind == BN_BLEND1D || b->kind == BN_BLEND2D) && !b->kids.count) {
      snprintf(err, (size_t)cap, "%s: %s '%s' has no points", src, kind_names[b->kind], gname(t, i));
      free_graph(t);
      return false;
    }
  }
  return true;
}

bool cw_animtree_load_text(cw_node n, const char *text) {
  atree *t = T(n, "cw_animtree_load_text");
  if (!t) return false;
  char err[640];
  if (load_text(n, t, text, "cw_animtree_load_text", err, sizeof err)) return true;
  cw_error("AnimationTree '%s': %s", cw_node_name(n), err);
  return false;
}

/* ── [animtree tree="path"] scene sections ───────────────────────────── */

typedef struct { const char *tree; gbuf text; int first_line; } gsec;

static void *gsec_begin(const char *h, const char *path, int lineno) {
  const char *tree = cw_scene_attr(h, "tree");
  if (!tree) { cw_error("%s:%d: [animtree] needs tree=\"path of the AnimationTree from the scene root\"", path, lineno); return NULL; }
  gsec *g = cw_calloc(1, sizeof *g, CW_MEM_ANIM);
  g->tree = tree;
  g->first_line = lineno;
  gput(&g->text, "");
  return g;
}

static bool gsec_line(void *st, const char *line, const char *path, int lineno) {
  gsec *g = st;
  /* the statement's shape is checked now, with the file's line number; names are resolved at instance time */
  gline gl;
  char why[512];
  static const char *const others[] = { "param", "point", "state", "transition", "filter", "root" };
  if (!gline_parse(&gl, line, why, sizeof why)) { cw_error("%s:%d: [animtree] %s", path, lineno, why); return false; }
  if (name_of(kind_names, BN_KINDS, gl.kw) < 0 && name_of(others, 6, gl.kw) < 0) {
    cw_error("%s:%d: [animtree] unknown statement '%s' (param, clip, blend1d, blend2d, blend2, add, layer, oneshot, timescale, machine, point, state, transition, filter, root)",
             path, lineno, gl.kw);
    return false;
  }
  /* blank and comment lines are gone already, so line k of the text is not file line first_line + k: keep the number */
  (void)lineno;
  gput(&g->text, line);
  gput(&g->text, "\n");
  return true;
}

static void gsec_instance(void *st, cw_node root, const char *path) {
  gsec *g = st;
  cw_node n = strcmp(g->tree, ".") ? cw_node_get(root, g->tree) : root;
  atree *t = cw_node_is(n, "AnimationTree") ? T(n, NULL) : NULL;
  if (!t) { cw_error("%s:%d: [animtree] tree '%s' is not an AnimationTree in this scene", path, g->first_line, g->tree); return; }
  /* a tree inside an instanced scene: this section replaces that scene's graph (an override) */
  if (cw_scene_node_in_instance(n, root))
    cw_array_push(&t->overrides, ((toverride){ root.id, t->loaded_text ? t->loaded_text : cw_intern("") }));
  char err[640];
  if (!load_text(n, t, g->text.buf, path, err, sizeof err)) { cw_error("%s (in the [animtree] section at line %d)", err, g->first_line); return; }
  char *now = cw_animtree_to_text(n);
  t->loaded_text = cw_intern(now);
  cw_free(now);
}

static void gsec_free(void *st) {
  gsec *g = st;
  cw_free(g->text.buf);
  cw_free(g);
}

typedef struct { cw_node root; void (*put)(void *, const char *); void *ctx; } gsave;
static bool gsave_fn(cw_node n, void *u) {
  gsave *w = u;
  if (!cw_node_is(n, "AnimationTree")) return true;
  atree *t = T(n, NULL);
  if (!t || !t->graph_stored) return true;
  char *body = cw_animtree_to_text(n);
  bool write = t->nodes.count > 0;
  if (cw_scene_node_in_instance(n, w->root)) {
    /* the instanced scene's own graph is in its file: written here only when it differs from that */
    const char *base = t->loaded_text ? t->loaded_text : "";
    for (int i = 0; i < t->overrides.count; i++)
      if (t->overrides.data[i].root == w->root.id) { base = t->overrides.data[i].before; break; }
    write = strcmp(body, base) != 0;
  }
  if (write) {
    char path[1024];
    cw_node_path(n, w->root, path, sizeof path);
    gbuf b = { 0 };
    gput(&b, "\n[animtree");
    gattr_s(&b, "tree", path[0] ? path : ".");
    gput(&b, "]\n");
    gput(&b, body);
    w->put(w->ctx, b.buf);
    cw_free(b.buf);
  }
  cw_free(body);
  return true;
}
static void gsec_save(cw_node root, void (*put)(void *, const char *), void *ctx) {
  gsave w = { root, put, ctx };
  cw_node_walk(root, gsave_fn, &w);
}

static const cw_scene_section graph_section = {
  .name = "animtree",
  .begin = gsec_begin,
  .line = gsec_line,
  .instance = gsec_instance,
  .free = gsec_free,
  .save = gsec_save,
};
void cw__register_animtree_scene_section(void) { cw_scene_register_section(&graph_section); }

/* Tweens: handle-based property sequences run by per-tree hooks. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"

enum { ST_PROP, ST_INTERVAL, ST_CALLBACK };

typedef struct tstep {
  int kind, group;
  cw_node node;
  const char *prop;
  cw_value from, to, target;
  bool has_from, relative, started;
  float duration, delay;
  cw_ease ease;
  cw_tween_fn fn;
  void *user;
} tstep;

typedef struct tween_rec {
  cw_tree *tree;
  CW_ARRAY(tstep) steps;
  int ngroups, group;
  double elapsed; /* in the current group */
  int loops, loops_done;
  float speed;
  int process;
  cw_node bound;
  bool paused, parallel_next;
  cw_tween_fn fin;
  void *fin_user;
} tween_rec;

static cw_pool pool;
static bool pool_ready;

static tween_rec *TW(cw_tween tw) { return pool_ready ? cw_pool_get(&pool, tw.id) : NULL; }

static void kill(cw_tween tw) {
  tween_rec *r = TW(tw);
  if (!r) return;
  cw_array_free(&r->steps);
  cw_pool_release(&pool, tw.id);
}

static void process(cw_tween tw, double dt);

static void hook(cw_tree *t, double dt, void *user) {
  int mode = (int)(intptr_t)user;
  if (!pool_ready) return;
  int n = pool.slot_count; /* tweens created now start next phase */
  for (int i = 0; i < n; i++) {
    tween_rec *r = cw_pool_at(&pool, i);
    if (!r || r->tree != t || r->process != mode) continue;
    /* a paused tree stops its tweens, except those bound to a node that
     * still processes while paused (pause_mode ALWAYS / WHEN_PAUSED: menus) */
    if (cw_tree_paused(t) && !(cw_node_valid(r->bound) && cw_node_can_process(r->bound))) continue;
    process((cw_tween){ cw_pool_id_of(&pool, r) }, dt);
  }
}

static void tree_gone(void *v) {
  cw_tree *t = v;
  if (!pool_ready) return;
  for (int i = 0; i < pool.slot_count; i++) {
    tween_rec *r = cw_pool_at(&pool, i);
    if (r && r->tree == t) kill((cw_tween){ cw_pool_id_of(&pool, r) });
  }
}

cw_tween cw_tween_new(cw_tree *tree) {
  if (!tree) tree = cw_current_tree();
  if (!tree) { cw_warn("cw_tween_new: no tree (pass one, or run a scene)"); return (cw_tween){ 0 }; }
  if (!pool_ready) { cw_pool_init(&pool, sizeof(tween_rec), CW_MEM_ANIM); pool_ready = true; }
  if (!cw_tree_slot(tree, "anim.tweens")) {
    cw_tree_set_slot(tree, "anim.tweens", tree, tree_gone);
    cw_tree_add_hook(tree, 0, 100, hook, (void *)(intptr_t)CW_TWEEN_FIXED);
    cw_tree_add_hook(tree, 1, 100, hook, (void *)(intptr_t)CW_TWEEN_UPDATE);
  }
  uint64_t id;
  tween_rec *r = cw_pool_alloc(&pool, &id);
  r->tree = tree;
  r->loops = 1;
  r->speed = 1;
  return (cw_tween){ id };
}

static tween_rec *R(cw_tween tw, const char *fn) {
  tween_rec *r = TW(tw);
  if (!r && fn) cw_warn("%s: stale tween handle (finished tweens free themselves)", fn);
  return r;
}

static tstep *push_step(tween_rec *r, int kind) {
  tstep s = { 0 };
  s.kind = kind;
  if (r->parallel_next && r->ngroups) s.group = r->ngroups - 1;
  else s.group = r->ngroups++;
  r->parallel_next = false;
  cw_array_push(&r->steps, s);
  return &r->steps.data[r->steps.count - 1];
}

void cw_tween_add_property(cw_tween tw, cw_node n, const char *prop, cw_value to, float duration, cw_ease ease) {
  tween_rec *r = R(tw, "cw_tween_add_property");
  if (!r) return;
  if (!cw_node_find_prop(n, prop, NULL)) {
    cw_warn("cw_tween_add_property: '%s' has no property '%s'", cw_node_valid(n) ? cw_node_name(n) : "(invalid node)", prop);
    return;
  }
  tstep *s = push_step(r, ST_PROP);
  s->node = n;
  s->prop = cw_intern(prop);
  s->to = to;
  s->duration = duration > 0 ? duration : 0;
  s->ease = ease;
}

void cw_tween_add_interval(cw_tween tw, float seconds) {
  tween_rec *r = R(tw, "cw_tween_add_interval");
  if (r) push_step(r, ST_INTERVAL)->duration = seconds > 0 ? seconds : 0;
}

void cw_tween_add_callback(cw_tween tw, cw_tween_fn fn, void *user) {
  tween_rec *r = R(tw, "cw_tween_add_callback");
  if (!r) return;
  tstep *s = push_step(r, ST_CALLBACK);
  s->fn = fn;
  s->user = user;
}

static tstep *last(cw_tween tw, const char *fn) {
  tween_rec *r = R(tw, fn);
  if (!r) return NULL;
  if (!r->steps.count) { cw_warn("%s: the tween has no step yet", fn); return NULL; }
  return &r->steps.data[r->steps.count - 1];
}
void cw_tween_from(cw_tween tw, cw_value from) {
  tstep *s = last(tw, "cw_tween_from");
  if (s) { s->from = from; s->has_from = true; }
}
void cw_tween_relative(cw_tween tw) { tstep *s = last(tw, "cw_tween_relative"); if (s) s->relative = true; }
void cw_tween_delay(cw_tween tw, float sec) { tstep *s = last(tw, "cw_tween_delay"); if (s) s->delay = sec > 0 ? sec : 0; }
void cw_tween_parallel(cw_tween tw) { tween_rec *r = R(tw, "cw_tween_parallel"); if (r) r->parallel_next = true; }
void cw_tween_set_loops(cw_tween tw, int loops) { tween_rec *r = R(tw, "cw_tween_set_loops"); if (r) r->loops = loops < 0 ? 0 : loops; }
void cw_tween_set_speed(cw_tween tw, float sp) { tween_rec *r = R(tw, "cw_tween_set_speed"); if (r) r->speed = sp; }
void cw_tween_set_process(cw_tween tw, cw_tween_process m) { tween_rec *r = R(tw, "cw_tween_set_process"); if (r) r->process = m; }
void cw_tween_bind(cw_tween tw, cw_node n) { tween_rec *r = R(tw, "cw_tween_bind"); if (r) r->bound = n; }
void cw_tween_on_finished(cw_tween tw, cw_tween_fn fn, void *user) {
  tween_rec *r = R(tw, "cw_tween_on_finished");
  if (r) { r->fin = fn; r->fin_user = user; }
}
void cw_tween_pause(cw_tween tw) { tween_rec *r = R(tw, NULL); if (r) r->paused = true; }
void cw_tween_play(cw_tween tw) { tween_rec *r = R(tw, NULL); if (r) r->paused = false; }
void cw_tween_kill(cw_tween tw) { kill(tw); }
bool cw_tween_running(cw_tween tw) { tween_rec *r = TW(tw); return r && !r->paused; }
int cw_tween_loops_done(cw_tween tw) { tween_rec *r = TW(tw); return r ? r->loops_done : 0; }
int cw_tween_count(cw_tree *t) {
  if (!pool_ready) return 0;
  if (!t) t = cw_current_tree();
  int k = 0;
  for (int i = 0; i < pool.slot_count; i++) {
    tween_rec *r = cw_pool_at(&pool, i);
    if (r && r->tree == t) k++;
  }
  return k;
}

cw_tween cw_tween_property(cw_node n, const char *prop, cw_value to, float duration, cw_ease ease) {
  cw_tree *t = cw_node_tree(n);
  cw_tween tw = cw_tween_new(t);
  if (CW_HANDLE_IS_NULL(tw)) return tw;
  cw_tween_bind(tw, n);
  cw_tween_add_property(tw, n, prop, to, duration, ease);
  return tw;
}

/* ── running ────────────────────────────────────────────────────────── */

static void interp(const cw_value *a, const cw_value *b, float e, cw_value *out) {
  *out = *a;
  cw_value bb = *b;
  if (!cw__value_coerce(&bb, a->type) || !cw__value_interpolable(a->type)) {
    if (e >= 1) *out = *b;
    return;
  }
  switch (a->type) {
  case CW_T_FLOAT: out->f = (float)a->f + ((float)bb.f - (float)a->f) * e; break;
  case CW_T_INT: out->i = a->i + (int64_t)floorf((float)(bb.i - a->i) * e + 0.5f); break;
  case CW_T_QUAT: cw_quat_slerp_det(out->v, a->v, bb.v, e); break;
  default:
    for (int k = 0; k < 4; k++) out->v[k] = a->v[k] + (bb.v[k] - a->v[k]) * e;
    break;
  }
}

static void relative_target(tstep *s) {
  cw_value to = s->to;
  s->target = s->from;
  if (!cw__value_coerce(&to, s->from.type)) return;
  switch (s->from.type) {
  case CW_T_FLOAT: s->target.f = s->from.f + to.f; break;
  case CW_T_INT: s->target.i = s->from.i + to.i; break;
  case CW_T_QUAT: {
    const float *a = s->from.v, *b = to.v;
    float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                   a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    memcpy(s->target.v, r, sizeof r);
    break;
  }
  default:
    for (int k = 0; k < 4; k++) s->target.v[k] = s->from.v[k] + to.v[k];
    break;
  }
}

/* run one step at its local time; false when the tween died in a callback */
static bool run_step(cw_tween tw, int i, double el) {
  tween_rec *r = TW(tw);
  tstep *s = &r->steps.data[i];
  double local = el - s->delay;
  if (local < 0) return true;
  bool first = !s->started;
  s->started = true;
  if (s->kind == ST_CALLBACK) {
    if (first && s->fn) {
      s->fn(tw, s->user);
      return TW(tw) != NULL;
    }
    return true;
  }
  if (s->kind != ST_PROP || !cw_node_valid(s->node)) return true;
  if (first) {
    if (!s->has_from) cw_node_get_prop(s->node, s->prop, &s->from);
    else {
      cw_value cur;
      if (cw_node_get_prop(s->node, s->prop, &cur)) cw__value_coerce(&s->from, cur.type);
    }
    if (s->relative) relative_target(s);
    else {
      s->target = s->to;
      cw__value_coerce(&s->target, s->from.type);
    }
  }
  float t = s->duration > 0 ? (float)(local / s->duration) : 1;
  if (t > 1) t = 1;
  float e = t >= 1 ? 1 : cw_ease_apply(s->ease, t);
  cw_value v;
  interp(&s->from, &s->target, e, &v);
  cw_node_set(s->node, s->prop, v);
  return true;
}

static double group_len(tween_rec *r, int g) {
  double len = 0;
  for (int i = 0; i < r->steps.count; i++) {
    tstep *s = &r->steps.data[i];
    if (s->group == g && s->delay + s->duration > len) len = s->delay + s->duration;
  }
  return len;
}

static void finish(cw_tween tw) {
  tween_rec *r = TW(tw);
  cw_tween_fn fn = r->fin;
  void *u = r->fin_user;
  kill(tw);
  if (fn) fn(tw, u);
}

static void process(cw_tween tw, double dt) {
  tween_rec *r = TW(tw);
  if (!r) return;
  if (!CW_HANDLE_IS_NULL(r->bound) && !cw_node_valid(r->bound)) { kill(tw); return; }
  if (r->paused) return;
  if (!r->ngroups) return;
  double rem = dt * r->speed;
  int passes = 0;
  (void)passes;
  for (;;) {
    r = TW(tw);
    if (!r) return;
    if (r->group >= r->ngroups) {
      r->loops_done++;
      if (r->loops && r->loops_done >= r->loops) { finish(tw); return; }
      r->group = 0;
      r->elapsed = 0;
      for (int i = 0; i < r->steps.count; i++) r->steps.data[i].started = false;
      if (!r->loops) { /* a forever-loop that takes no time runs once per tick */
        double total = 0;
        for (int g = 0; g < r->ngroups; g++) total += group_len(r, g);
        if (total == 0 && ++passes >= 1) return;
      }
    }
    int g = r->group;
    double glen = group_len(r, g), nel = r->elapsed + rem;
    for (int i = 0; i < r->steps.count; i++) {
      r = TW(tw);
      if (!r) return;
      if (r->steps.data[i].group != g) continue;
      if (!run_step(tw, i, nel < glen ? nel : glen)) return;
    }
    r = TW(tw);
    if (!r) return;
    if (nel >= glen) {
      rem = nel - glen;
      r->elapsed = 0;
      r->group++;
      continue;
    }
    r->elapsed = nel;
    return;
  }
}

void cw_tween_step(cw_tween tw, double dt) { process(tw, dt); }

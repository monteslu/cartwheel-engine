/* Signals: named events with connections to C callbacks (or, for scene
 * files, method names a resolver turns into callbacks). */
#include "scene_internal.h"

typedef struct conn {
  cw_node emitter, target;
  const char *signal, *method; /* interned */
  cw_signal_fn fn;
  void *user;
  uint32_t flags;
} conn;

static cw_pool conns;
static bool ready;
static cw_method_resolver resolver;

void cw_set_method_resolver(cw_method_resolver r) { resolver = r; }

static void track(cw_node n, uint64_t id) {
  node_rec *r = cw__node_rec(n);
  if (r) cw_array_push(&r->conns, id);
}

uint64_t cw_connect(cw_node emitter, const char *signal, cw_node target, const char *method, cw_signal_fn fn,
                    void *user, uint32_t flags) {
  if (!ready) { cw_pool_init(&conns, sizeof(conn), CW_MEM_SCENE); ready = true; }
  if (!cw_node_valid(emitter)) return 0;
  if (!fn && method && resolver && cw_node_valid(target)) fn = resolver(target, method, &user);
  uint64_t id;
  conn *c = cw_pool_alloc(&conns, &id);
  c->emitter = emitter;
  c->target = target;
  c->signal = cw_intern(signal);
  c->method = method ? cw_intern(method) : NULL;
  c->fn = fn;
  c->user = user;
  c->flags = flags;
  track(emitter, id);
  if (!CW_HANDLE_IS_NULL(target) && target.id != emitter.id) track(target, id);
  return id;
}

void cw_disconnect(uint64_t id) {
  if (ready) cw_pool_release(&conns, id); /* node lists keep the stale id; it reads as NULL */
}

static void call(conn *c, uint64_t id, cw_node emitter, const cw_value *args, int argc) {
  if (!c->fn) {
    cw_warn("signal '%s': connection to method '%s' has no callback (no resolver found it)", c->signal,
            c->method ? c->method : "?");
    return;
  }
  cw_signal_fn fn = c->fn;
  void *user = c->user;
  if (c->flags & CW_CONNECT_ONESHOT) cw_disconnect(id);
  fn(emitter, args, argc, user);
}

void cw_emit(cw_node emitter, const char *signal, const cw_value *args, int argc) {
  node_rec *r = cw__node_rec(emitter);
  if (!r || !ready || !r->conns.count) return;
  const char *sig = cw_intern(signal);
  /* snapshot: callbacks may connect/disconnect */
  int n = r->conns.count;
  uint64_t *ids = cw_frame_alloc(sizeof(uint64_t) * (size_t)n);
  memcpy(ids, r->conns.data, sizeof(uint64_t) * (size_t)n);
  for (int i = 0; i < n; i++) {
    conn *c = cw_pool_get(&conns, ids[i]);
    if (!c || c->emitter.id != emitter.id || c->signal != sig) continue;
    if (c->flags & CW_CONNECT_DEFERRED) {
      cw_tree *t = r->tree ? r->tree : cw_current_tree();
      if (!t) continue;
      cw__queued_signal q = { ids[i], emitter, argc > 8 ? 8 : argc };
      for (int k = 0; k < q.argc; k++) q.args[k] = args[k];
      cw_array_push(&t->deferred_signals, q);
      continue;
    }
    call(c, ids[i], emitter, args, argc);
  }
}

void cw__signals_flush(cw_tree *t) {
  for (int i = 0; i < t->deferred_signals.count; i++) { /* live count: deferred emits may queue more */
    cw__queued_signal q = t->deferred_signals.data[i];
    conn *c = ready ? cw_pool_get(&conns, q.conn) : NULL;
    if (c && cw_node_valid(q.emitter)) call(c, q.conn, q.emitter, q.args, q.argc);
  }
  cw_array_clear(&t->deferred_signals);
}

void cw__signals_node_freed(cw_node n) {
  node_rec *r = cw__node_rec(n);
  if (!r || !ready) return;
  for (int i = 0; i < r->conns.count; i++) cw_pool_release(&conns, r->conns.data[i]);
  cw_array_free(&r->conns);
}

/* For the scene writer: every connection whose emitter is under `root`. */
int cw__signals_of(cw_node emitter, uint64_t *out, int cap) {
  node_rec *r = cw__node_rec(emitter);
  int k = 0;
  if (!r || !ready) return 0;
  for (int i = 0; i < r->conns.count && k < cap; i++) {
    conn *c = cw_pool_get(&conns, r->conns.data[i]);
    if (c && c->emitter.id == emitter.id) out[k++] = r->conns.data[i];
  }
  return k;
}
bool cw__signal_info(uint64_t id, cw_node *emitter, const char **signal, cw_node *target, const char **method,
                     uint32_t *flags) {
  conn *c = ready ? cw_pool_get(&conns, id) : NULL;
  if (!c) return false;
  *emitter = c->emitter;
  *signal = c->signal;
  *target = c->target;
  *method = c->method;
  *flags = c->flags;
  return true;
}

/* ── methods (cw_node_call) ──────────────────────────────────────────── */

static CW_ARRAY(cw_method) methods;

void cw_node_type_add_method(const char *type, const char *name, const char *args, cw_method_fn fn, void *user,
                             const char *doc) {
  const char *t = cw_intern(type), *nm = cw_intern(name);
  for (int i = 0; i < methods.count; i++)
    if (methods.data[i].type == t && methods.data[i].name == nm) { /* re-registration replaces (hot reload) */
      methods.data[i].args = args;
      methods.data[i].fn = fn;
      methods.data[i].user = user;
      methods.data[i].doc = doc;
      return;
    }
  cw_method m = { t, nm, args, fn, user, doc };
  cw_array_push(&methods, m);
}

int cw_node_methods(cw_node n, const cw_method **out, int cap) {
  const cw_node_type *ty = cw_node_get_type(n);
  int k = 0;
  for (const cw_node_type *c = ty; c; c = c->base) {
    const char *tn = cw_intern(c->name);
    for (int i = 0; i < methods.count; i++)
      if (methods.data[i].type == tn) {
        if (k < cap && out) out[k] = &methods.data[i];
        k++;
      }
  }
  return k;
}

const cw_method *cw_node_find_method(cw_node n, const char *name) {
  const char *nm = cw_intern(name);
  for (const cw_node_type *c = cw_node_get_type(n); c; c = c->base) {
    const char *tn = cw_intern(c->name);
    for (int i = 0; i < methods.count; i++)
      if (methods.data[i].type == tn && methods.data[i].name == nm) return &methods.data[i];
  }
  return NULL;
}

bool cw_node_call(cw_node n, const char *name, const cw_value *args, int argc, cw_value *ret) {
  if (ret) *ret = (cw_value){ CW_T_NONE };
  if (!cw_node_valid(n)) return false;
  const cw_method *m = cw_node_find_method(n, name);
  if (m) return m->fn(n, args, argc, ret, m->user);
  if (resolver) {
    void *user = NULL;
    cw_signal_fn fn = resolver(n, name, &user);
    if (fn) {
      fn(n, args, argc, user);
      return true;
    }
  }
  return false;
}

/* ── connection queries (editors: the signal connections view) ─────────── */

int cw_node_connections(cw_node n, bool incoming, cw_connection_info *out, int cap) {
  node_rec *r = cw__node_rec(n);
  int k = 0;
  if (!r || !ready) return 0;
  for (int i = 0; i < r->conns.count; i++) {
    conn *c = cw_pool_get(&conns, r->conns.data[i]);
    if (!c || (incoming ? c->target.id != n.id : c->emitter.id != n.id)) continue;
    if (k < cap && out)
      out[k] = (cw_connection_info){ r->conns.data[i], c->emitter, c->target, c->signal, c->method, c->flags };
    k++;
  }
  return k;
}

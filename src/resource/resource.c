/* The resource registry: handles, paths, UIDs, reference counts, the
 * deferred-free queue, the type registry and the async pipeline (read on the
 * main thread, decode on the job pool, finish in request order under a
 * per-frame budget). See cartwheel/resource.h. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/vfs.h"
#include "resource_internal.h"

#define MAX_TYPES 64

typedef struct { cw_res_done_fn fn; void *user; } res_cb;
typedef struct { uint64_t id; cw_res_done_fn fn; void *user; } res_notify;

typedef struct res_slot {
  cw_res_type type;
  cw_res_state state;
  int32_t refs;
  const char *path; /* interned, canonical */
  uint64_t uid;
  void *payload;
  uint32_t version;
  int64_t bytes;
  /* a load in flight */
  uint8_t *file;
  size_t file_len;
  int extra_count;
  char **extra_paths;
  uint8_t **extra_bytes;
  size_t *extra_lens;
  cw_res *deps;
  int dep_count;
  cw_job *job;
  atomic_int decode_done;
  bool take_bytes;
  uint64_t file_hash; /* of the bytes last loaded (hot reload compares) */
  void *decoded;
  size_t cost;
  bool queued, cancelled;
  /* load timing (wall clock: reported, never fed to game state) */
  double t_request, read_ms, decode_ms, finish_ms, total_ms;
  uint64_t frame_request;
  int frames_to_ready;
  size_t file_bytes;
  /* sub-resources */
  bool owned_sub;   /* registered by the parent's finish: lives as long as the parent */
  bool sub_wait;    /* an async request for a sub-resource whose parent is still loading */
  bool sub_seen;    /* reload bookkeeping */
  cw_res parent;
  uint64_t *subs;   /* owned sub-resource ids (parent side) */
  int sub_count, sub_cap;
  /* deferred free */
  bool pending_free;
  uint64_t free_at;
  res_cb *cbs;
  int cb_count, cb_cap;
} res_slot;

static struct {
  bool init, shutting_down;
  cw_pool slots;
  cw_map by_path; /* cw_hash_str(path) -> slot id */
  cw_map by_uid;  /* uid -> slot id */
  cw_map index_uid;  /* res.index: uid -> interned path */
  cw_map index_path; /* res.index: cw_hash_str(path) -> uid */
  cw_res_type_desc types[MAX_TYPES];
  int type_count;
  CW_ARRAY(uint64_t) queue;
  int queue_head;
  CW_ARRAY(res_notify) notify;
  CW_ARRAY(uint64_t) frees;
  uint64_t frame;
  uint64_t head_id, head_since; /* starvation guard: the undecoded head and since when */
  size_t budget_bytes;
  double budget_ms;
  uint64_t loads, cache_hits, decoded, freed, uploaded_last;
  double read_ms, decode_ms, finish_ms;
  int64_t file_bytes;
  int finished_last;
} R;

static void ensure_init(void);
static void finish_slot(uint64_t id, res_slot *s);

static res_slot *slot_get(cw_res h) { return R.init ? cw_pool_get(&R.slots, h.id) : NULL; }
static cw_res mkres(uint64_t id) { return (cw_res){ id }; }
static const cw_res_type_desc *type_desc(cw_res_type t) {
  return t >= 1 && (int)t <= R.type_count ? &R.types[t - 1] : NULL;
}
uint64_t cw__res_frame_index(void) { return R.frame; }

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/* ── type registry ───────────────────────────────────────────────── */
cw_res_type cw_res_register_type(const cw_res_type_desc *d) {
  ensure_init();
  for (int i = 0; i < R.type_count; i++)
    if (!strcmp(R.types[i].name, d->name)) return (cw_res_type)(i + 1);
  CW_ASSERT(R.type_count < MAX_TYPES, "too many resource types (%d)", MAX_TYPES);
  CW_ASSERT(d->name && d->finish && d->unload, "a resource type needs a name, finish and unload");
  R.types[R.type_count] = *d;
  R.types[R.type_count].name = cw_intern(d->name);
  if (d->extensions) R.types[R.type_count].extensions = cw_intern(d->extensions);
  return (cw_res_type)++R.type_count;
}
cw_res_type cw_res_type_find(const char *name) {
  ensure_init();
  for (int i = 0; i < R.type_count; i++)
    if (!strcmp(R.types[i].name, name)) return (cw_res_type)(i + 1);
  return 0;
}
const char *cw_res_type_name(cw_res_type t) {
  ensure_init();
  const cw_res_type_desc *d = type_desc(t);
  return d ? d->name : "none";
}

static cw_res_type type_for_path(const char *path) {
  if (!strncmp(path, "gen:/noise", 10)) return CW_RES_TEXTURE; /* gen://noise?... (cartwheel/noise.h) */
  const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
  if (!dot || (slash && dot < slash)) return CW_RES_BINARY;
  char ext[16];
  size_t n = strlen(dot);
  if (n >= sizeof ext) return CW_RES_BINARY;
  for (size_t i = 0; i <= n; i++) ext[i] = (char)((dot[i] >= 'A' && dot[i] <= 'Z') ? dot[i] + 32 : dot[i]);
  for (int i = 0; i < R.type_count; i++) {
    const char *e = R.types[i].extensions;
    if (!e) continue;
    for (const char *p = e; (p = strstr(p, ext)); p += n)
      if ((p == e || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return (cw_res_type)(i + 1);
  }
  return CW_RES_BINARY;
}

/* ── init, res.index ─────────────────────────────────────────────── */
static void load_index(void) {
  size_t len;
  char *t = cw_asset_load("res.index", &len);
  if (!t) return;
  int n = 0;
  for (char *line = t; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    unsigned long long uid;
    char tname[64], path[CW_RES_PATH_MAX];
    if (line[0] != '#' && sscanf(line, "%llx %63s %1023[^\n]", &uid, tname, path) == 3) {
      const char *p = cw_intern(path);
      cw_map_set(&R.index_uid, uid, (uint64_t)(uintptr_t)p);
      cw_map_set(&R.index_path, cw_hash_str(p), uid);
      n++;
    }
    line = nl ? nl + 1 : NULL;
  }
  free(t);
  cw_log(CW_LOG_DEBUG, "resources: res.index lists %d files", n);
}

extern void (*cw__res_frame_hook)(void);
extern void (*cw__res_shutdown_hook)(void);
static void res_command(int argc, const char **argv, cw_reply *r);
static void reload_command(int argc, const char **argv, cw_reply *r);

static void ensure_init(void) {
  if (R.init) return;
  R.init = true;
  cw__res_frame_hook = cw_res_frame;
  cw__res_shutdown_hook = cw_res_shutdown;
  cw_pool_init(&R.slots, sizeof(res_slot), CW_MEM_RESOURCE);
  cw_map_init(&R.by_path, CW_MEM_RESOURCE);
  cw_map_init(&R.by_uid, CW_MEM_RESOURCE);
  cw_map_init(&R.index_uid, CW_MEM_RESOURCE);
  cw_map_init(&R.index_path, CW_MEM_RESOURCE);
  R.budget_bytes = 32u << 20;
  R.budget_ms = 4;
  const cw_res_type_desc *builtin[] = { &cw__res_texture_type, &cw__res_mesh_type, &cw__res_material_type,
                                        &cw__res_gltf_type, &cw__res_audio_type, &cw__res_font_type,
                                        &cw__res_json_type, &cw__res_text_type, &cw__res_binary_type };
  for (size_t i = 0; i < CW_ARRAY_LEN(builtin); i++) {
    cw_res_type t = cw_res_register_type(builtin[i]);
    if (t != i + 1) CW_FATAL("built-in resource type order changed");
  }
  load_index();
  cw_debug_command("res", "res [path-substring]: loaded resources (path, type, state, refs, uid, version, bytes) and totals", res_command);
  cw_debug_command("reload", "reload <resource path> [...]: re-read loaded resources in place (the project layer's copy "
                             "first, cartwheel/vfs.h, then the assets); handles stay valid, version goes up", reload_command);
}

/* ── slots ───────────────────────────────────────────────────────── */
static uint64_t find_path(const char *interned) {
  uint64_t id;
  if (!R.init || !cw_map_get(&R.by_path, cw_hash_str(interned), &id)) return 0;
  res_slot *s = cw_pool_get(&R.slots, id);
  if (!s) return 0;
  if (s->path != interned) CW_FATAL("resource path hash collision: %s / %s (rename one)", s->path, interned);
  return id;
}

static uint64_t uid_for(const char *path) {
  uint64_t uid;
  if (cw_map_get(&R.index_path, cw_hash_str(path), &uid)) return uid;
  return cw_res_path_uid(path);
}

static uint64_t new_slot(const char *interned, cw_res_type type, res_slot **out) {
  uint64_t id;
  res_slot *s = cw_pool_alloc(&R.slots, &id);
  s->type = type;
  s->path = interned;
  s->state = CW_RES_STATE_LOADING;
  s->uid = uid_for(interned);
  cw_map_set(&R.by_path, cw_hash_str(interned), id);
  uint64_t other;
  if (!cw_map_get(&R.by_uid, s->uid, &other) || !cw_pool_get(&R.slots, other)) cw_map_set(&R.by_uid, s->uid, id);
  *out = s;
  return id;
}

static void add_cb(res_slot *s, cw_res_done_fn fn, void *user) {
  if (!fn) return;
  if (s->cb_count == s->cb_cap) {
    s->cb_cap = s->cb_cap ? s->cb_cap * 2 : 2;
    s->cbs = cw_realloc(s->cbs, (size_t)s->cb_cap * sizeof *s->cbs, CW_MEM_RESOURCE);
  }
  s->cbs[s->cb_count++] = (res_cb){ fn, user };
}

static void fire_cbs(uint64_t id, res_slot *s) {
  /* callbacks may load or release: copy them out first */
  int n = s->cb_count;
  if (!n) return;
  res_cb *cbs = cw_alloc((size_t)n * sizeof *cbs, CW_MEM_RESOURCE);
  memcpy(cbs, s->cbs, (size_t)n * sizeof *cbs);
  s->cb_count = 0;
  cw_res_state st = s->state;
  for (int i = 0; i < n; i++) cbs[i].fn(mkres(id), st, cbs[i].user);
  cw_free(cbs);
}

static void drop_load_data(res_slot *s) {
  if (!s->take_bytes) cw_free(s->file);
  s->file = NULL;
  s->take_bytes = false;
  for (int i = 0; i < s->extra_count; i++) { cw_free(s->extra_paths[i]); cw_free(s->extra_bytes[i]); }
  cw_free(s->extra_paths);
  cw_free(s->extra_bytes);
  cw_free(s->extra_lens);
  s->extra_paths = NULL;
  s->extra_bytes = NULL;
  s->extra_lens = NULL;
  s->extra_count = 0;
}

static void slot_fail(uint64_t id, res_slot *s, bool notify_later) {
  s->state = CW_RES_STATE_FAILED;
  drop_load_data(s);
  if (notify_later) {
    for (int i = 0; i < s->cb_count; i++) cw_array_push(&R.notify, ((res_notify){ id, s->cbs[i].fn, s->cbs[i].user }));
    s->cb_count = 0;
  } else {
    fire_cbs(id, s);
  }
}

static void release_deps(cw_res *deps, int n) {
  for (int i = 0; i < n; i++) cw_res_release(deps[i]);
  cw_free(deps);
}

static void free_slot(uint64_t id, res_slot *s) {
  const cw_res_type_desc *d = type_desc(s->type);
  if (s->job) { cw_job_wait(s->job); s->job = NULL; }
  if (s->decoded && d) {
    if (d->discard) d->discard(s->decoded, d->type_user);
    s->decoded = NULL;
  }
  /* owned sub-resources go with their parent (none can be referenced: a
   * referenced sub holds the parent) */
  for (int i = 0; i < s->sub_count; i++) {
    res_slot *c = cw_pool_get(&R.slots, s->subs[i]);
    if (!c) continue;
    CW_ASSERT(c->refs == 0, "sub-resource %s still referenced while its parent is freed", c->path);
    uint64_t v;
    if (cw_map_get(&R.by_path, cw_hash_str(c->path), &v) && v == s->subs[i]) cw_map_remove(&R.by_path, cw_hash_str(c->path));
    if (cw_map_get(&R.by_uid, c->uid, &v) && v == s->subs[i]) cw_map_remove(&R.by_uid, c->uid);
    cw_free(c->cbs);
    cw_pool_release(&R.slots, s->subs[i]);
  }
  cw_free(s->subs);
  if (s->state == CW_RES_STATE_READY && s->payload && d && !s->owned_sub) d->unload(s->payload, d->type_user);
  drop_load_data(s);
  cw_res *deps = s->deps;
  int dn = s->dep_count;
  uint64_t v;
  if (cw_map_get(&R.by_path, cw_hash_str(s->path), &v) && v == id) cw_map_remove(&R.by_path, cw_hash_str(s->path));
  if (cw_map_get(&R.by_uid, s->uid, &v) && v == id) cw_map_remove(&R.by_uid, s->uid);
  cw_free(s->cbs);
  cw_pool_release(&R.slots, id);
  R.freed++;
  release_deps(deps, dn);
}

/* ── loading ─────────────────────────────────────────────────────── */
static uint8_t *read_file(const char *path, size_t *len) {
  /* generated resources ("gen://noise?..."; canonical "gen:/noise?..."):
   * the "file" is the generator's name and query, which the type's decode
   * step turns into data (texture.c: gen:/noise) */
  if (!strncmp(path, "gen:/", 5)) {
    size_t n = strlen(path + 5);
    uint8_t *b = cw_alloc(n + 1, CW_MEM_RESOURCE);
    memcpy(b, path + 5, n + 1);
    *len = n;
    return b;
  }
  /* the project layer first (an editor's project directory, files written
   * or imported into memory, removal marks), then the assets */
  cw_vfs_layer layer = cw_vfs_root() || cw_vfs_changed_count() ? cw_vfs_where(path) : CW_VFS_ASSET; /* a plain game: assets only */
  if (layer == CW_VFS_NONE) return NULL;
  if (layer != CW_VFS_ASSET) {
    size_t n;
    void *d = cw_vfs_read(path, &n);
    if (!d) return NULL;
    uint8_t *b = cw_alloc(n + 1, CW_MEM_RESOURCE);
    memcpy(b, d, n);
    b[n] = 0;
    free(d);
    *len = n;
    return b;
  }
  long n = cw_asset_size(path);
  if (n < 0) return NULL;
  uint8_t *b = cw_alloc((size_t)n + 1, CW_MEM_RESOURCE);
  if (cw_asset_read(path, b, (size_t)n) != n) { cw_free(b); return NULL; }
  b[n] = 0;
  *len = (size_t)n;
  return b;
}

struct cw_res_prepare {
  uint64_t id;
  res_slot *s;
  bool async;
};

cw_res cw_res_prepare_dep(cw_res_prepare *p, const char *path, cw_res_type type) {
  cw_res d = p->async ? cw_res_load_async(path, type, NULL, NULL) : cw_res_load(path, type);
  res_slot *s = cw_pool_get(&R.slots, p->id); /* the pool may have grown: re-get */
  p->s = s;
  s->deps = cw_realloc(s->deps, (size_t)(s->dep_count + 1) * sizeof *s->deps, CW_MEM_RESOURCE);
  s->deps[s->dep_count++] = d;
  return d;
}

bool cw_res_prepare_read(cw_res_prepare *p, const char *path) {
  char c[CW_RES_PATH_MAX];
  size_t len;
  uint8_t *b = cw__res_canon(path, c, sizeof c) ? read_file(c, &len) : NULL;
  if (!b) return false;
  res_slot *s = p->s;
  int n = s->extra_count + 1;
  s->extra_paths = cw_realloc(s->extra_paths, (size_t)n * sizeof *s->extra_paths, CW_MEM_RESOURCE);
  s->extra_bytes = cw_realloc(s->extra_bytes, (size_t)n * sizeof *s->extra_bytes, CW_MEM_RESOURCE);
  s->extra_lens = cw_realloc(s->extra_lens, (size_t)n * sizeof *s->extra_lens, CW_MEM_RESOURCE);
  s->extra_paths[n - 1] = cw_strdup(c, CW_MEM_RESOURCE);
  s->extra_bytes[n - 1] = b;
  s->extra_lens[n - 1] = len;
  s->extra_count = n;
  return true;
}

const uint8_t *cw_res_prepare_bytes(cw_res_prepare *p, size_t *len) {
  *len = p->s->file_len;
  return p->s->file;
}
const char *cw_res_prepare_path(cw_res_prepare *p) { return p->s->path; }

static void decode_now(res_slot *s) {
  const cw_res_type_desc *d = type_desc(s->type);
  double t0 = now_ms();
  s->file_hash = cw_hash_bytes(s->file, s->file_len, 0);
  bool take = false;
  cw_res_decode_args a = { s->path, s->file, s->file_len, s->extra_count, (const char *const *)s->extra_paths,
                           (const uint8_t *const *)s->extra_bytes, s->extra_lens, d->type_user, &take };
  size_t cost = 0;
  s->decoded = d->decode ? d->decode(&a, &cost) : (void *)s; /* no decode step: finish reads the file */
  s->cost = cost ? cost : s->file_len;
  s->take_bytes = take;
  s->decode_ms = now_ms() - t0;
}

static void decode_job(void *ctx) {
  res_slot *s = ctx;
  decode_now(s);
  atomic_store_explicit(&s->decode_done, 1, memory_order_release);
}

/* Read + prepare (main thread), then decode inline or on the pool. */
static void start_load(uint64_t id, bool async) {
  res_slot *s = cw_pool_get(&R.slots, id);
  s->state = CW_RES_STATE_LOADING;
  s->cancelled = false;
  s->t_request = now_ms();
  s->frame_request = R.frame;
  s->read_ms = s->decode_ms = s->finish_ms = 0;
  if (!s->file) {
    s->file = read_file(s->path, &s->file_len);
    s->read_ms = now_ms() - s->t_request;
    if (!s->file) {
      const char *pack = cw_asset_pack_path();
      cw_error("res://%s: not found (%s%s)", s->path, pack ? "pack " : "cart bundle / asset root",
               pack ? pack : "");
      slot_fail(id, s, async);
      return;
    }
  }
  const cw_res_type_desc *d = type_desc(s->type);
  if (d->prepare) {
    cw_res_prepare p = { id, s, async };
    bool ok = d->prepare(&p, d->type_user);
    s = cw_pool_get(&R.slots, id);
    if (!ok) { slot_fail(id, s, async); return; }
  }
  R.decoded++;
  if (async) {
    atomic_store(&s->decode_done, 0);
    s->queued = true;
    cw_array_push(&R.queue, id);
    s->job = d->decode ? cw_job_single(decode_job, s) : NULL;
    if (!s->job) atomic_store(&s->decode_done, 1);
  } else {
    decode_now(s);
    finish_slot(id, s);
  }
}

static void finish_slot(uint64_t id, res_slot *s) {
  const cw_res_type_desc *d = type_desc(s->type);
  if (s->job) { cw_job_wait(s->job); s->job = NULL; }
  s->queued = false;
  void *dec = s->decoded;
  s->decoded = NULL;
  if (!dec) { cw_error("res://%s: could not decode it as %s", s->path, d->name); slot_fail(id, s, false); return; }
  if (dec == (void *)s) dec = NULL; /* type without a decode step */
  double t0 = now_ms();
  s->file_bytes = s->file_len;
  void *payload = d->finish(dec ? dec : (void *)s, mkres(id), s->deps, s->dep_count, d->type_user);
  s = cw_pool_get(&R.slots, id); /* finish may register sub-resources (pool growth) */
  s->finish_ms = now_ms() - t0;
  s->total_ms = now_ms() - s->t_request;
  s->frames_to_ready = (int)(R.frame - s->frame_request);
  R.read_ms += s->read_ms;
  R.decode_ms += s->decode_ms;
  R.finish_ms += s->finish_ms;
  R.file_bytes += (int64_t)s->file_bytes;
  drop_load_data(s);
  if (!payload) { cw_error("res://%s: could not finish loading it as %s", s->path, d->name); slot_fail(id, s, false); return; }
  s->payload = payload;
  s->state = CW_RES_STATE_READY;
  s->version++;
  s->bytes = d->size ? (int64_t)d->size(payload, d->type_user) : 0;
  fire_cbs(id, s);
}

/* the decode-less types read the file in finish through this */
const uint8_t *cw__res_slot_file(void *slot_as_decoded, size_t *len, bool take) {
  res_slot *s = slot_as_decoded;
  *len = s->file_len;
  if (take) s->take_bytes = true;
  return s->file;
}

static void ref_slot(uint64_t id, res_slot *s) {
  s->refs++;
  if (s->pending_free) s->pending_free = false;
  if (s->refs == 1 && !CW_HANDLE_IS_NULL(s->parent)) cw_res_add_ref(s->parent);
}

static cw_res load_sub(const char *path, cw_res_type type, bool async, cw_res_done_fn done, void *user);

static cw_res load(const char *in, cw_res_type type, bool async, cw_res_done_fn done, void *user) {
  ensure_init();
  R.loads++;
  char c[CW_RES_PATH_MAX];
  if (!cw__res_canon(in, c, sizeof c)) {
    cw_error("resource path \"%s\" is invalid: use a relative path under the asset root, no \"..\"", in ? in : "(null)");
    c[0] = 0;
    snprintf(c, sizeof c, "invalid:%s", in ? in : "");
    res_slot *s;
    uint64_t id = find_path(cw_intern(c));
    if (id) { s = cw_pool_get(&R.slots, id); ref_slot(id, s); }
    else { id = new_slot(cw_intern(c), CW_RES_BINARY, &s); s->refs = 1; slot_fail(id, s, async); }
    if (done) { if (async) cw_array_push(&R.notify, ((res_notify){ id, done, user })); else done(mkres(id), CW_RES_STATE_FAILED, user); }
    return mkres(id);
  }
  if (strchr(c, '#')) return load_sub(c, type, async, done, user);
  const char *p = cw_intern(c);
  uint64_t id = find_path(p);
  if (id) {
    res_slot *s = cw_pool_get(&R.slots, id);
    R.cache_hits++;
    if (type && s->type != type)
      cw_warn("res://%s is loaded as %s, requested as %s: returning the loaded one", p, cw_res_type_name(s->type), cw_res_type_name(type));
    ref_slot(id, s);
    if (s->cancelled) {
      add_cb(s, done, user);
      start_load(id, async);
    } else if (s->state == CW_RES_STATE_LOADING) {
      add_cb(s, done, user);
      if (!async) cw_res_wait(mkres(id));
    } else if (done) {
      if (async) cw_array_push(&R.notify, ((res_notify){ id, done, user }));
      else done(mkres(id), s->state, user);
    }
    return mkres(id);
  }
  res_slot *s;
  id = new_slot(p, type ? type : type_for_path(p), &s);
  s->refs = 1;
  add_cb(s, done, user);
  start_load(id, async);
  return mkres(id);
}

static cw_res load_sub(const char *c, cw_res_type type, bool async, cw_res_done_fn done, void *user) {
  const char *p = cw_intern(c);
  uint64_t id = find_path(p);
  if (id) {
    res_slot *s = cw_pool_get(&R.slots, id);
    R.cache_hits++;
    ref_slot(id, s);
    if (s->state == CW_RES_STATE_LOADING) {
      add_cb(s, done, user);
      if (!async) cw_res_wait(mkres(id));
    } else if (done) {
      if (async) cw_array_push(&R.notify, ((res_notify){ id, done, user }));
      else done(mkres(id), s->state, user);
    }
    return mkres(id);
  }
  const char *hash = strchr(c, '#');
  char parent_path[CW_RES_PATH_MAX];
  snprintf(parent_path, sizeof parent_path, "%.*s", (int)(hash - c), c);
  cw_res parent = async ? cw_res_load_async(parent_path, 0, NULL, NULL) : cw_res_load(parent_path, 0);
  /* the parent reference just taken becomes the sub-resource's */
  id = find_path(p);
  res_slot *s;
  if (id) { /* registered by the parent's (sync) finish */
    s = cw_pool_get(&R.slots, id);
    s->refs++;
    if (s->refs > 1) cw_res_release(parent); /* someone else's reference already holds it */
    if (done) { if (async) cw_array_push(&R.notify, ((res_notify){ id, done, user })); else done(mkres(id), s->state, user); }
    return mkres(id);
  }
  id = new_slot(p, type ? type : CW_RES_BINARY, &s);
  s->refs = 1;
  s->parent = parent;
  add_cb(s, done, user);
  cw_res_state ps = cw_res_get_state(parent);
  if (ps == CW_RES_STATE_LOADING) {
    s->sub_wait = true;
    s->queued = true;
    atomic_store(&s->decode_done, 1);
    cw_array_push(&R.queue, id);
    return mkres(id);
  }
  cw_error("res://%s: %s", p, ps == CW_RES_STATE_FAILED ? "its file failed to load" : "no such sub-resource in that file");
  slot_fail(id, s, async);
  return mkres(id);
}

cw_res cw_res_load(const char *path, cw_res_type type) { return load(path, type, false, NULL, NULL); }
cw_res cw_res_load_async(const char *path, cw_res_type type, cw_res_done_fn done, void *user) {
  return load(path, type, true, done, user);
}

void cw_res_index_set(uint64_t uid, const char *path) {
  ensure_init();
  char c[CW_RES_PATH_MAX];
  if (!uid || !path || !cw__res_canon(path, c, sizeof c)) return;
  const char *p = cw_intern(c);
  uint64_t old;
  if (cw_map_get(&R.index_path, cw_hash_str(p), &old) && old != uid) cw_map_remove(&R.index_uid, old);
  cw_map_set(&R.index_uid, uid, (uint64_t)(uintptr_t)p);
  cw_map_set(&R.index_path, cw_hash_str(p), uid);
}

const char *cw_res_index_path(uint64_t uid) {
  ensure_init();
  uint64_t v;
  return cw_map_get(&R.index_uid, uid, &v) ? (const char *)(uintptr_t)v : NULL;
}

cw_res cw_res_load_uid(uint64_t uid, cw_res_type type) {
  ensure_init();
  uint64_t v;
  if (cw_map_get(&R.index_uid, uid, &v)) return cw_res_load((const char *)(uintptr_t)v, type);
  if (cw_map_get(&R.by_uid, uid, &v) && cw_pool_get(&R.slots, v)) { cw_res_add_ref(mkres(v)); return mkres(v); }
  char p[64];
  snprintf(p, sizeof p, "uid:%016llx", (unsigned long long)uid);
  cw_error("resource uid %016llx is not in res.index (pack the project with cwpack)", (unsigned long long)uid);
  return load(p, CW_RES_BINARY, false, NULL, NULL);
}

cw_res cw_res_create(const char *path, cw_res_type type, const void *bytes, size_t len) {
  ensure_init();
  char c[CW_RES_PATH_MAX];
  if (!cw__res_canon(path, c, sizeof c) || strchr(c, '#')) return load(path, type, false, NULL, NULL);
  const char *p = cw_intern(c);
  uint64_t id = find_path(p);
  if (id) { ref_slot(id, cw_pool_get(&R.slots, id)); return mkres(id); }
  res_slot *s;
  id = new_slot(p, type ? type : type_for_path(p), &s);
  s->refs = 1;
  s->file = cw_alloc(len + 1, CW_MEM_RESOURCE);
  if (len) memcpy(s->file, bytes, len);
  s->file[len] = 0;
  s->file_len = len;
  R.loads++;
  start_load(id, false);
  return mkres(id);
}

/* Finish queued loads in order up to and including `until` (0 = the head
 * only per budget rules: see cw_res_frame). */
static void finish_head(uint64_t id, res_slot *s) {
  if (s->sub_wait) {
    s->queued = false;
    s->sub_wait = false;
    if (s->state != CW_RES_STATE_READY) { /* the parent's finish did not register it */
      cw_error("res://%s: %s", s->path, cw_res_get_state(s->parent) == CW_RES_STATE_FAILED ? "its file failed to load" : "no such sub-resource in that file");
      slot_fail(id, s, false);
    } else {
      fire_cbs(id, s);
    }
    return;
  }
  if (s->refs == 0) { /* released while loading: drop the work */
    if (s->job) { cw_job_wait(s->job); s->job = NULL; }
    const cw_res_type_desc *d = type_desc(s->type);
    if (s->decoded && s->decoded != (void *)s && d->discard) d->discard(s->decoded, d->type_user);
    s->decoded = NULL;
    s->queued = false;
    s->cancelled = true;
    s->state = CW_RES_STATE_NONE;
    drop_load_data(s);
    return;
  }
  finish_slot(id, s);
}

static void queue_compact(void) {
  if (R.queue_head && R.queue_head * 2 >= R.queue.count) {
    memmove(R.queue.data, R.queue.data + R.queue_head, (size_t)(R.queue.count - R.queue_head) * sizeof *R.queue.data);
    R.queue.count -= R.queue_head;
    R.queue_head = 0;
  }
}

cw_res_state cw_res_wait(cw_res h) {
  ensure_init();
  res_slot *s = slot_get(h);
  if (!s) return CW_RES_STATE_NONE;
  while (s->queued && R.queue_head < R.queue.count) {
    uint64_t id = R.queue.data[R.queue_head++];
    res_slot *q = cw_pool_get(&R.slots, id);
    if (q && q->queued) finish_head(id, q);
    s = slot_get(h);
    if (!s) return CW_RES_STATE_NONE;
  }
  queue_compact();
  return s->state;
}

void cw_res_wait_all(void) {
  ensure_init();
  while (R.queue_head < R.queue.count) {
    uint64_t id = R.queue.data[R.queue_head++];
    res_slot *q = cw_pool_get(&R.slots, id);
    if (q && q->queued) finish_head(id, q);
  }
  queue_compact();
}

/* ── references ──────────────────────────────────────────────────── */
void cw_res_add_ref(cw_res h) {
  res_slot *s = slot_get(h);
  CW_ASSERT(s || CW_HANDLE_IS_NULL(h), "cw_res_add_ref on a stale handle");
  if (s) ref_slot(h.id, s);
}

void cw_res_release(cw_res h) {
  if (CW_HANDLE_IS_NULL(h) || R.shutting_down) return;
  res_slot *s = slot_get(h);
  CW_ASSERT(s, "cw_res_release on a stale handle (released twice?)");
  if (!s) return;
  CW_ASSERT(s->refs > 0, "cw_res_release of %s with no references left", s->path);
  if (s->refs <= 0) return;
  if (--s->refs > 0) return;
  /* a slot with a parent holds one parent reference while it has any */
  if (!CW_HANDLE_IS_NULL(s->parent)) cw_res_release(s->parent);
  if (s->owned_sub) return;
  s->pending_free = true;
  s->free_at = R.frame + CW_RES_FREE_DELAY_FRAMES;
  cw_array_push(&R.frees, h.id);
}

int cw_res_ref_count(cw_res h) {
  res_slot *s = slot_get(h);
  return s ? s->refs : 0;
}

/* ── queries ─────────────────────────────────────────────────────── */
cw_res cw_res_find(const char *path) {
  ensure_init();
  char c[CW_RES_PATH_MAX];
  if (!cw__res_canon(path, c, sizeof c)) return mkres(0);
  return mkres(find_path(cw_intern(c)));
}
cw_res cw_res_find_uid(uint64_t uid) {
  ensure_init();
  uint64_t v;
  return mkres(cw_map_get(&R.by_uid, uid, &v) && cw_pool_get(&R.slots, v) ? v : 0);
}
cw_res_state cw_res_get_state(cw_res h) {
  res_slot *s = slot_get(h);
  return s ? s->state : CW_RES_STATE_NONE;
}
bool cw_res_is_ready(cw_res h) { return cw_res_get_state(h) == CW_RES_STATE_READY; }
void *cw_res_get(cw_res h) {
  res_slot *s = slot_get(h);
  return s && s->state == CW_RES_STATE_READY ? s->payload : NULL;
}
cw_res_type cw_res_type_of(cw_res h) {
  res_slot *s = slot_get(h);
  return s ? s->type : 0;
}
const char *cw_res_path(cw_res h) {
  res_slot *s = slot_get(h);
  return s ? s->path : NULL;
}
uint64_t cw_res_uid(cw_res h) {
  res_slot *s = slot_get(h);
  return s ? s->uid : 0;
}
uint32_t cw_res_version(cw_res h) {
  res_slot *s = slot_get(h);
  return s ? s->version : 0;
}

void *cw__res_get_typed(cw_res h, cw_res_type t) {
  res_slot *s = slot_get(h);
  return s && s->state == CW_RES_STATE_READY && s->type == t ? s->payload : NULL;
}

/* ── sub-resources ───────────────────────────────────────────────── */
cw_res cw_res_register_sub(cw_res parent, const char *name, cw_res_type type, void *payload) {
  res_slot *ps = slot_get(parent);
  CW_ASSERT(ps, "cw_res_register_sub with a stale parent");
  char path[CW_RES_PATH_MAX];
  snprintf(path, sizeof path, "%s#%s", ps->path, name);
  const char *p = cw_intern(path);
  uint64_t id = find_path(p);
  res_slot *s;
  if (id) {
    s = cw_pool_get(&R.slots, id);
    if (s->type != type && s->state != CW_RES_STATE_READY) s->type = type;
  } else {
    id = new_slot(p, type, &s);
    s->parent = parent;
  }
  s->owned_sub = true;
  s->payload = payload;
  if (s->state != CW_RES_STATE_READY || !s->sub_seen) s->version++;
  s->state = CW_RES_STATE_READY;
  s->sub_seen = true;
  const cw_res_type_desc *d = type_desc(type);
  s->bytes = d && d->size && payload ? (int64_t)d->size(payload, d->type_user) : 0;
  ps = slot_get(parent);
  bool listed = false;
  for (int i = 0; i < ps->sub_count; i++) listed |= ps->subs[i] == id;
  if (!listed) {
    if (ps->sub_count == ps->sub_cap) {
      ps->sub_cap = ps->sub_cap ? ps->sub_cap * 2 : 8;
      ps->subs = cw_realloc(ps->subs, (size_t)ps->sub_cap * sizeof *ps->subs, CW_MEM_RESOURCE);
    }
    ps->subs[ps->sub_count++] = id;
  }
  return mkres(id);
}

/* ── reload ──────────────────────────────────────────────────────── */
bool cw_res_reload_from_memory(cw_res h, const void *bytes, size_t len) {
  ensure_init();
  res_slot *s = slot_get(h);
  if (!s) return false;
  if (s->owned_sub) return cw_res_reload_from_memory(s->parent, bytes, len);
  if (s->state == CW_RES_STATE_LOADING) cw_res_wait(h);
  s = slot_get(h);
  const cw_res_type_desc *d = type_desc(s->type);
  /* load the new data into the same slot without disturbing the payload */
  void *old_payload = s->payload;
  cw_res_state old_state = s->state;
  cw_res *old_deps = s->deps;
  int old_dn = s->dep_count;
  s->deps = NULL;
  s->dep_count = 0;
  s->file = cw_alloc(len + 1, CW_MEM_RESOURCE);
  memcpy(s->file, bytes, len);
  s->file[len] = 0;
  s->file_len = len;
  for (int i = 0; i < s->sub_count; i++) {
    res_slot *c = cw_pool_get(&R.slots, s->subs[i]);
    if (c) c->sub_seen = false;
  }
  bool ok = true;
  if (d->prepare) {
    cw_res_prepare p = { h.id, s, false };
    ok = d->prepare(&p, d->type_user);
    s = slot_get(h);
  }
  void *fresh = NULL;
  if (ok) {
    decode_now(s);
    void *dec = s->decoded;
    s->decoded = NULL;
    if (dec) fresh = d->finish(dec == (void *)s ? (void *)s : dec, h, s->deps, s->dep_count, d->type_user);
    s = slot_get(h);
  }
  drop_load_data(s);
  if (!fresh) {
    cw_error("res://%s: reload failed; keeping the previous data", s->path);
    release_deps(s->deps, s->dep_count);
    s->deps = old_deps;
    s->dep_count = old_dn;
    for (int i = 0; i < s->sub_count; i++) {
      res_slot *c = cw_pool_get(&R.slots, s->subs[i]);
      if (c) c->sub_seen = true;
    }
    return false;
  }
  if (old_state == CW_RES_STATE_READY && old_payload) {
    if (d->reload) d->reload(old_payload, fresh, d->type_user);
    else { d->unload(old_payload, d->type_user); s->payload = fresh; }
  } else {
    s->payload = fresh;
  }
  s->state = CW_RES_STATE_READY;
  s->version++;
  s->bytes = d->size ? (int64_t)d->size(s->payload, d->type_user) : 0;
  for (int i = 0; i < s->sub_count; i++) {
    res_slot *c = cw_pool_get(&R.slots, s->subs[i]);
    if (c && !c->sub_seen) {
      cw_warn("res://%s: gone after the reload of its file", c->path);
      c->payload = NULL;
      c->state = CW_RES_STATE_FAILED;
    }
  }
  release_deps(old_deps, old_dn);
  return true;
}

bool cw_res_reload(cw_res h) {
  res_slot *s = slot_get(h);
  if (!s) return false;
  if (s->owned_sub) return cw_res_reload(s->parent);
  size_t len;
  uint8_t *b = read_file(s->path, &len);
  if (!b) { cw_error("res://%s: reload: file not found", s->path); return false; }
  bool ok = cw_res_reload_from_memory(h, b, len);
  cw_free(b);
  return ok;
}

/* ── frame boundary ──────────────────────────────────────────────── */
void cw_res_set_upload_budget(size_t bytes, double ms) {
  ensure_init();
  R.budget_bytes = bytes;
  R.budget_ms = ms;
}

void cw_res_frame(void) {
  if (!R.init) return;
  R.frame++;
  /* deferred frees, in release order */
  int w = 0;
  for (int i = 0; i < R.frees.count; i++) {
    uint64_t id = R.frees.data[i];
    res_slot *s = cw_pool_get(&R.slots, id);
    if (!s || !s->pending_free) continue;
    if (R.frame < s->free_at || (s->queued && !s->sub_wait) || s->job) { R.frees.data[w++] = id; continue; }
    if (s->queued) s->queued = false;
    free_slot(id, s);
  }
  R.frees.count = w;
  /* callbacks for cache hits and early failures, in request order */
  if (R.notify.count) {
    int n = R.notify.count;
    res_notify *list = cw_alloc((size_t)n * sizeof *list, CW_MEM_RESOURCE);
    memcpy(list, R.notify.data, (size_t)n * sizeof *list);
    R.notify.count = 0;
    for (int i = 0; i < n; i++) {
      res_slot *s = cw_pool_get(&R.slots, list[i].id);
      if (s) list[i].fn(mkres(list[i].id), s->state, list[i].user);
    }
    cw_free(list);
  }
  /* finish decoded loads in request order under the budget */
  bool det = cw_is_deterministic_run();
  double t0 = now_ms();
  size_t spent = 0;
  int finished = 0;
  while (R.queue_head < R.queue.count) {
    uint64_t id = R.queue.data[R.queue_head];
    res_slot *s = cw_pool_get(&R.slots, id);
    if (!s || !s->queued) { R.queue_head++; continue; }
    if (finished && R.budget_bytes && spent >= R.budget_bytes) break;
    if (finished && !det && R.budget_ms > 0 && now_ms() - t0 >= R.budget_ms) break;
    if (!atomic_load_explicit(&s->decode_done, memory_order_acquire) && !det) {
      /* not decoded yet: skip this frame, unless it has been waiting a while
       * (a browser starts wasi-thread workers only when the page yields, so
       * a cart that loads during init may have none yet): then help */
      if (R.head_id != id) { R.head_id = id; R.head_since = R.frame; }
      if (R.frame - R.head_since < 2) break;
    }
    R.queue_head++;
    finish_head(id, s);
    spent += s->cost; /* written by the decode job: read it after the finish waited for it */
    finished++;
  }
  queue_compact();
  R.uploaded_last = spent;
  R.finished_last = finished;
  cw__res_hotreload_tick();
}

void cw_res_get_stats(cw_res_stats *o) {
  memset(o, 0, sizeof *o);
  if (!R.init) return;
  for (int i = 0; i < R.slots.slot_count; i++) {
    res_slot *s = cw_pool_at(&R.slots, i);
    if (!s) continue;
    o->live++;
    if (s->pending_free) o->pending_free++;
    switch (s->state) {
    case CW_RES_STATE_READY:
      o->ready++;
      o->bytes += s->bytes;
      if (s->type < 32) o->by_type[s->type]++;
      break;
    case CW_RES_STATE_LOADING: o->loading++; break;
    case CW_RES_STATE_FAILED: o->failed++; break;
    default: break;
    }
  }
  o->loads = R.loads;
  o->cache_hits = R.cache_hits;
  o->decoded = R.decoded;
  o->freed = R.freed;
  o->uploaded_bytes_last_frame = R.uploaded_last;
  o->finished_last_frame = R.finished_last;
  o->read_ms = R.read_ms;
  o->decode_ms = R.decode_ms;
  o->finish_ms = R.finish_ms;
  o->file_bytes = R.file_bytes;
  cw_mem_stats ms;
  cw_mem_get_stats(&ms);
  o->cpu_bytes = ms.bytes[CW_MEM_RESOURCE];
}

void cw_res_shutdown(void) {
  if (!R.init) return;
  cw_res_hotreload_stop();
  for (int i = R.queue_head; i < R.queue.count; i++) {
    res_slot *s = cw_pool_get(&R.slots, R.queue.data[i]);
    if (s && s->job) { cw_job_wait(s->job); s->job = NULL; }
  }
  /* newest first, so what was loaded on top of something goes before it;
   * releases between slots are moot now */
  R.shutting_down = true;
  for (int i = R.slots.slot_count - 1; i >= 0; i--) {
    res_slot *s = cw_pool_at(&R.slots, i);
    if (!s || s->owned_sub) continue;
    free_slot(cw_pool_id_of(&R.slots, s), s);
  }
  cw__gltf_shutdown();
  cw_pool_free_all(&R.slots);
  cw_map_free(&R.by_path);
  cw_map_free(&R.by_uid);
  cw_map_free(&R.index_uid);
  cw_map_free(&R.index_path);
  cw_array_free(&R.queue);
  cw_array_free(&R.notify);
  cw_array_free(&R.frees);
  memset(&R, 0, sizeof R);
}

/* Hot reload: reread every loaded file (after a repack + remount) and reload
 * the ones whose bytes changed, in slot order. Returns how many. */
int cw__res_reload_changed(void) {
  if (!R.init) return 0;
  int n = 0;
  for (int i = 0; i < R.slots.slot_count; i++) {
    res_slot *s = cw_pool_at(&R.slots, i);
    if (!s || s->owned_sub || !CW_HANDLE_IS_NULL(s->parent) || s->pending_free) continue;
    if (s->state != CW_RES_STATE_READY && s->state != CW_RES_STATE_FAILED) continue;
    if (!strncmp(s->path, "gen:", 4) || !strncmp(s->path, "invalid:", 8) || !strncmp(s->path, "uid:", 4)) continue;
    size_t len;
    uint8_t *b = read_file(s->path, &len);
    if (!b) continue;
    uint64_t h = cw_hash_bytes(b, len, 0);
    uint64_t id = cw_pool_id_of(&R.slots, s);
    if (h != s->file_hash) {
      cw_info("hot reload: res://%s", s->path);
      if (cw_res_reload_from_memory(mkres(id), b, len)) n++;
    }
    cw_free(b);
  }
  return n;
}

/* debug channel: "res [filter]" */
static void res_command(int argc, const char **argv, cw_reply *r) {
  static const char *states[] = { "none", "loading", "ready", "failed" };
  const char *filter = argc > 1 ? argv[1] : NULL;
  cw_res_stats st;
  cw_res_get_stats(&st);
  cw_reply_printf(r, "{\"live\":%d,\"ready\":%d,\"loading\":%d,\"failed\":%d,\"pending_free\":%d,\"bytes\":%lld,\"cpu_bytes\":%lld,\"file_bytes\":%lld,\"read_ms\":%.2f,\"decode_ms\":%.2f,\"finish_ms\":%.2f,\"ktx2_target\":\"%s\",\"resources\":[",
                  st.live, st.ready, st.loading, st.failed, st.pending_free, (long long)st.bytes, (long long)st.cpu_bytes, (long long)st.file_bytes, st.read_ms, st.decode_ms, st.finish_ms, cw_res_ktx2_target());
  int n = 0;
  for (int i = 0; i < R.slots.slot_count; i++) {
    res_slot *s = cw_pool_at(&R.slots, i);
    if (!s || (filter && !strstr(s->path, filter))) continue;
    cw_reply_printf(r, "%s{\"path\":", n++ ? "," : "");
    cw_reply_str(r, s->path);
    cw_reply_printf(r, ",\"type\":\"%s\",\"state\":\"%s\",\"refs\":%d,\"uid\":\"%016llx\",\"version\":%u,\"bytes\":%lld,\"file_bytes\":%zu,\"read_ms\":%.2f,\"decode_ms\":%.2f,\"finish_ms\":%.2f,\"total_ms\":%.2f,\"frames\":%d%s}",
                    cw_res_type_name(s->type), states[s->state], s->refs, (unsigned long long)s->uid, s->version,
                    (long long)s->bytes, s->file_bytes, s->read_ms, s->decode_ms, s->finish_ms, s->total_ms, s->frames_to_ready, s->pending_free ? ",\"pending_free\":true" : "");
  }
  cw_reply_raw(r, "]}");
}

bool cw_res_get_info(cw_res h, cw_res_info *o) {
  res_slot *s = slot_get(h);
  memset(o, 0, sizeof *o);
  if (!s) return false;
  o->type = s->type;
  o->state = s->state;
  o->path = s->path;
  o->uid = s->uid;
  o->version = s->version;
  o->refs = s->refs;
  o->file_bytes = s->file_bytes;
  o->bytes = s->bytes;
  o->read_ms = s->read_ms;
  /* a worker writes decode_ms while it decodes: read it once the decode is published */
  o->decode_ms = atomic_load_explicit(&s->decode_done, memory_order_acquire) ? s->decode_ms : 0;
  o->finish_ms = s->finish_ms;
  o->total_ms = s->total_ms;
  o->frames_to_ready = s->frames_to_ready;
  return true;
}

void cw_res_log_report(void) {
  if (!R.init) return;
  cw_res_stats st;
  cw_res_get_stats(&st);
  cw_info("resources: %d ready, %lld KB packed read, %lld KB payload, %lld KB CPU (resource tag); "
          "read %.1f ms, decode %.1f ms (workers), finish %.1f ms (main thread)",
          st.ready, (long long)(st.file_bytes / 1024), (long long)(st.bytes / 1024), (long long)(st.cpu_bytes / 1024),
          st.read_ms, st.decode_ms, st.finish_ms);
  for (int i = 0; i < R.slots.slot_count; i++) {
    res_slot *s = cw_pool_at(&R.slots, i);
    if (!s || s->owned_sub || !s->file_bytes) continue;
    cw_info("  res://%s  %s  %zu KB -> %lld KB  read %.1f  decode %.1f  finish %.1f  ready after %.1f ms / %d frames",
            s->path, cw_res_type_name(s->type), s->file_bytes / 1024, (long long)(s->bytes / 1024), s->read_ms,
            s->decode_ms, s->finish_ms, s->total_ms, s->frames_to_ready);
  }
}

/* reload: what an editor does after it saved an asset, or a tool after it
 * pushed one into the project layer (vfs write/append) */
static void reload_command(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) {
    cw_reply_error(r, "usage: reload <resource path> [...]   e.g. reload images/hero.png (res lists what is loaded)");
    return;
  }
  cw_asset_remount(); /* natively: a rebuilt pack */
  cw_reply_raw(r, "{\"reloaded\":[");
  for (int i = 1; i < argc; i++) {
    const char *path = !strncmp(argv[i], "res://", 6) ? argv[i] + 6 : argv[i];
    cw_res h = cw_res_find(path);
    if (CW_HANDLE_IS_NULL(h)) {
      cw_reply_error(r, "%s is not loaded, so there is nothing to reload (res %s lists loaded paths; loading it is the game's job)", path, path);
      return;
    }
    size_t len;
    void *bytes = cw_vfs_read(path, &len);
    if (!bytes) {
      cw_reply_error(r, "%s: no file in the project layer or the assets (vfs ls lists the project layer)", path);
      return;
    }
    bool ok = cw_res_reload_from_memory(h, bytes, len);
    free(bytes);
    if (!ok) {
      cw_reply_error(r, "%s: the new bytes did not decode; the old data is kept (the log says why)", path);
      return;
    }
    cw_reply_printf(r, "%s{\"path\":", i > 1 ? "," : "");
    cw_reply_str(r, path);
    cw_reply_printf(r, ",\"bytes\":%zu,\"version\":%u}", len, (unsigned)cw_res_version(h));
  }
  cw_reply_raw(r, "]}");
}

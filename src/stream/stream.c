/* World streaming (cartwheel/stream.h): cells, sources, hysteresis, the
 * deterministic activation in cw_stream_tick and the floating origin.
 *
 * Everything that decides what the simulation sees is double arithmetic on
 * the sources' positions (+ - * and comparisons: IEEE, the same on every
 * host) and runs in cw_stream_tick. Wall clock times are only reported. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/stream.h"

#define MAX_SOURCES 8
#define MAX_GRID 4096

typedef struct st_cell {
  uint8_t state;     /* cw_cell_state */
  bool live;         /* in s->live */
  bool loaded_once;  /* on_load ran (data valid) */
  bool preparing;    /* on_load ran, cell_ready not yet true */
  cw_res res;
  void *data;
  double d2;         /* squared distance to the nearest source at the last tick */
} st_cell;

typedef struct st_source {
  cw_dvec3 p;
  int priority;
  bool used;
} st_source;

struct cw_stream {
  cw_stream_desc d;
  char *path_format;
  st_cell *cells;
  CW_ARRAY(int) live;   /* ids of cells not NONE, ascending */
  st_source src[MAX_SOURCES];
  int nsrc;
  cw_dvec3 origin;      /* world position of local (0, 0, 0) */
  cw_stream_stats st;
  uint64_t tick;
};

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

cw_stream_desc cw_stream_default_desc(void) {
  return (cw_stream_desc){ .cell_size = 512, .load_radius = 1500, .active_radius = 600 };
}

cw_stream *cw_stream_create(const cw_stream_desc *d) {
  if (!d || d->cells_x <= 0 || d->cells_z <= 0 || d->cells_x > MAX_GRID || d->cells_z > MAX_GRID ||
      (int64_t)d->cells_x * d->cells_z > (1 << 22)) {
    cw_error("cw_stream_create: the grid must be 1..%d cells on a side (and at most 4M cells)", MAX_GRID);
    return NULL;
  }
  if (!(d->cell_size > 0) || !(d->active_radius >= 0) || d->active_radius > d->load_radius) {
    cw_error("cw_stream_create: needs cell_size > 0 and 0 <= active_radius <= load_radius (got %g, %g, %g)",
             d->cell_size, (double)d->active_radius, (double)d->load_radius);
    return NULL;
  }
  cw_stream *s = cw_calloc(1, sizeof *s, CW_MEM_GAME);
  s->d = *d;
  if (!(s->d.hysteresis > 0)) s->d.hysteresis = (float)(d->cell_size / 4);
  if (s->d.max_requests_per_tick <= 0) s->d.max_requests_per_tick = 8;
  if (s->d.prepare_per_frame <= 0) s->d.prepare_per_frame = 1;
  if (d->path_format) s->d.path_format = s->path_format = cw_strdup(d->path_format, CW_MEM_GAME);
  s->cells = cw_calloc((size_t)d->cells_x * d->cells_z, sizeof *s->cells, CW_MEM_GAME);
  s->origin = d->origin;
  s->st.activation_hash = 0xcbf29ce484222325ull;
  return s;
}

const cw_stream_desc *cw_stream_get_desc(const cw_stream *s) { return &s->d; }

/* ── coordinates ─────────────────────────────────────────────────────── */
cw_dvec3 cw_stream_origin(const cw_stream *s) { return s->origin; }
t3_vec3 cw_stream_to_local(const cw_stream *s, cw_dvec3 w) {
  return (t3_vec3){ (float)(w.x - s->origin.x), (float)(w.y - s->origin.y), (float)(w.z - s->origin.z) };
}
cw_dvec3 cw_stream_to_world(const cw_stream *s, t3_vec3 l) {
  return (cw_dvec3){ s->origin.x + (double)l.x, s->origin.y + (double)l.y, s->origin.z + (double)l.z };
}

static cw_stream_cell cell_info(const cw_stream *s, int id) {
  const st_cell *c = &s->cells[id];
  int cx = id % s->d.cells_x, cz = id / s->d.cells_x;
  cw_dvec3 o = { s->d.origin.x + cx * s->d.cell_size, s->d.origin.y, s->d.origin.z + cz * s->d.cell_size };
  return (cw_stream_cell){ cx, cz, id, o, cw_stream_to_local(s, o), c->res, c->data };
}

/* ── sources ─────────────────────────────────────────────────────────── */
int cw_stream_source_add(cw_stream *s, cw_dvec3 p, int priority) {
  for (int i = 0; i < MAX_SOURCES; i++)
    if (!s->src[i].used) {
      s->src[i] = (st_source){ p, priority, true };
      if (i >= s->nsrc) s->nsrc = i + 1;
      return i;
    }
  cw_error("cw_stream_source_add: at most %d sources", MAX_SOURCES);
  return -1;
}
void cw_stream_source_move(cw_stream *s, int i, cw_dvec3 p) {
  if (i >= 0 && i < s->nsrc && s->src[i].used) s->src[i].p = p;
}
void cw_stream_source_move_local(cw_stream *s, int i, t3_vec3 p) { cw_stream_source_move(s, i, cw_stream_to_world(s, p)); }

/* ── cells ───────────────────────────────────────────────────────────── */
static void hash_event(cw_stream *s, int id, int on) {
  uint64_t h = s->st.activation_hash;
  uint64_t v[3] = { s->tick, (uint64_t)id, (uint64_t)on };
  const uint8_t *b = (const uint8_t *)v;
  for (size_t i = 0; i < sizeof v; i++) h = (h ^ b[i]) * 0x100000001b3ull;
  s->st.activation_hash = h;
}

/* squared horizontal distance from p to cell id's square, and the nearest
 * source's priority (the best, when several are as near) */
static double cell_d2(const cw_stream *s, int id, int *prio) {
  int cx = id % s->d.cells_x, cz = id / s->d.cells_x;
  double x0 = s->d.origin.x + cx * s->d.cell_size, z0 = s->d.origin.z + cz * s->d.cell_size;
  double x1 = x0 + s->d.cell_size, z1 = z0 + s->d.cell_size, best = INFINITY;
  int bp = 0;
  for (int i = 0; i < s->nsrc; i++) {
    if (!s->src[i].used) continue;
    double px = s->src[i].p.x, pz = s->src[i].p.z;
    double dx = px < x0 ? x0 - px : px > x1 ? px - x1 : 0;
    double dz = pz < z0 ? z0 - pz : pz > z1 ? pz - z1 : 0;
    double d2 = dx * dx + dz * dz;
    if (d2 < best || (d2 == best && s->src[i].priority > bp)) best = d2, bp = s->src[i].priority;
  }
  if (prio) *prio = bp;
  return best;
}

static void live_add(cw_stream *s, int id) {
  int at = s->live.count;
  while (at > 0 && s->live.data[at - 1] > id) at--;
  cw_array_push(&s->live, 0);
  memmove(s->live.data + at + 1, s->live.data + at, (size_t)(s->live.count - 1 - at) * sizeof *s->live.data);
  s->live.data[at] = id;
  s->cells[id].live = true;
}

static void request(cw_stream *s, int id, int prio) {
  st_cell *c = &s->cells[id];
  c->state = CW_CELL_LOADING;
  c->loaded_once = false;
  c->data = NULL;
  if (s->path_format) {
    char path[512];
    snprintf(path, sizeof path, s->path_format, id % s->d.cells_x, id / s->d.cells_x);
    /* nearer first: a priority from the distance in metres (sqrt is exact IEEE everywhere) */
    double m = sqrt(c->d2);
    int p = prio * 1000000 - (int)(m < 999999 ? m : 999999);
    c->res = cw_res_load_async_ex(path, s->d.cell_type, &(cw_res_load_opts){ .lane = s->d.res_lane, .priority = p });
  }
  if (!c->live) live_add(s, id);
  s->st.loads++;
}

static bool file_ready(const st_cell *c) {
  cw_res_state rs = cw_res_get_state(c->res);
  return CW_HANDLE_IS_NULL(c->res) || rs == CW_RES_STATE_READY || rs == CW_RES_STATE_FAILED;
}

static void run_on_load(cw_stream *s, int id) {
  st_cell *c = &s->cells[id];
  double t0 = now_ms();
  if (s->d.on_load && (CW_HANDLE_IS_NULL(c->res) || cw_res_is_ready(c->res))) {
    cw_stream_cell ci = cell_info(s, id);
    c->data = s->d.on_load(s, &ci, s->d.user);
  }
  double ms = now_ms() - t0;
  if (ms > s->st.prepare_ms_max) s->st.prepare_ms_max = ms;
  c->loaded_once = true;
  c->preparing = false;
  if (s->d.cell_ready) {
    cw_stream_cell ci = cell_info(s, id);
    c->preparing = !s->d.cell_ready(s, &ci, false, s->d.user);
  }
  c->state = c->preparing ? CW_CELL_LOADING : CW_CELL_LOADED;
}

static void finish_preparing(cw_stream *s, int id, bool wait) {
  st_cell *c = &s->cells[id];
  if (!c->preparing) return;
  cw_stream_cell ci = cell_info(s, id);
  if (s->d.cell_ready(s, &ci, wait, s->d.user) || wait) {
    c->preparing = false;
    c->state = CW_CELL_LOADED;
  }
}

static void drop(cw_stream *s, int id) {
  st_cell *c = &s->cells[id];
  if (c->state == CW_CELL_ACTIVE) {
    if (s->d.on_deactivate) { cw_stream_cell ci = cell_info(s, id); s->d.on_deactivate(s, &ci, s->d.user); }
    hash_event(s, id, 0);
    s->st.deactivations++;
    c->state = CW_CELL_LOADED;
  }
  if (c->loaded_once) {
    if (s->d.on_unload) { cw_stream_cell ci = cell_info(s, id); s->d.on_unload(s, &ci, s->d.user); }
    s->st.unloads++;
  }
  cw_res_release(c->res); /* a load in flight is cancelled; a ready file may stay in the residency cache */
  c->res = (cw_res){ 0 };
  c->data = NULL;
  c->loaded_once = false;
  c->preparing = false;
  c->state = CW_CELL_NONE;
}

static void rebase(cw_stream *s) {
  if (!(s->d.rebase_distance > 0) || s->nsrc == 0 || !s->src[0].used) return;
  double dx = s->src[0].p.x - s->origin.x, dz = s->src[0].p.z - s->origin.z;
  if (dx * dx + dz * dz <= s->d.rebase_distance * s->d.rebase_distance) return;
  /* whole cells, to the cell corner nearest the source */
  double cs = s->d.cell_size;
  double nx = s->d.origin.x + floor((s->src[0].p.x - s->d.origin.x) / cs + 0.5) * cs;
  double nz = s->d.origin.z + floor((s->src[0].p.z - s->d.origin.z) / cs + 0.5) * cs;
  t3_vec3 shift = { (float)(nx - s->origin.x), 0, (float)(nz - s->origin.z) };
  if (shift.x == 0 && shift.z == 0) return;
  s->origin.x = nx;
  s->origin.z = nz;
  s->st.rebases++;
  if (s->d.on_rebase) s->d.on_rebase(s, shift, s->d.user);
  for (int i = 0; i < s->live.count; i++) {
    int id = s->live.data[i];
    if (s->cells[id].loaded_once && s->d.on_cell_rebase) {
      cw_stream_cell ci = cell_info(s, id);
      s->d.on_cell_rebase(s, &ci, s->d.user);
    }
  }
}

typedef struct { double d2; int prio, id; } st_cand;
static int cand_cmp(const void *a, const void *b) {
  const st_cand *x = a, *y = b;
  if (x->prio != y->prio) return x->prio > y->prio ? -1 : 1;
  if (x->d2 != y->d2) return x->d2 < y->d2 ? -1 : 1;
  return (x->id > y->id) - (x->id < y->id);
}

void cw_stream_tick(cw_stream *s) {
  if (!s) return;
  s->tick++;
  s->st.ticks = s->tick;
  rebase(s);
  const double cs = s->d.cell_size, h = s->d.hysteresis;
  const double L = s->d.load_radius, A = s->d.active_radius;
  const double L2 = L * L, A2 = A * A, Lh2 = (L + h) * (L + h), Ah2 = (A + h) * (A + h);
  /* 1. what left the sets: deactivate, then unload (id order) */
  for (int i = 0; i < s->live.count; i++) {
    int id = s->live.data[i];
    st_cell *c = &s->cells[id];
    c->d2 = cell_d2(s, id, NULL);
    if (c->state == CW_CELL_ACTIVE && c->d2 > Ah2) {
      if (s->d.on_deactivate) { cw_stream_cell ci = cell_info(s, id); s->d.on_deactivate(s, &ci, s->d.user); }
      hash_event(s, id, 0);
      s->st.deactivations++;
      c->state = CW_CELL_LOADED;
    }
  }
  int w = 0;
  for (int i = 0; i < s->live.count; i++) {
    int id = s->live.data[i];
    st_cell *c = &s->cells[id];
    if (c->d2 > Lh2 && c->state != CW_CELL_ACTIVE) {
      drop(s, id);
      c->live = false;
      continue;
    }
    s->live.data[w++] = id;
  }
  s->live.count = w;
  /* 2. what entered the load set: cells within L of any source not loading yet */
  CW_ARRAY(st_cand) cand = { 0 };
  int need_active = 0;
  for (int k = 0; k < s->nsrc; k++) {
    if (!s->src[k].used) continue;
    double px = s->src[k].p.x - s->d.origin.x, pz = s->src[k].p.z - s->d.origin.z;
    int x0 = (int)floor((px - L) / cs), x1 = (int)floor((px + L) / cs);
    int z0 = (int)floor((pz - L) / cs), z1 = (int)floor((pz + L) / cs);
    x0 = CW_MAX(x0, 0), z0 = CW_MAX(z0, 0), x1 = CW_MIN(x1, s->d.cells_x - 1), z1 = CW_MIN(z1, s->d.cells_z - 1);
    for (int cz = z0; cz <= z1; cz++)
      for (int cx = x0; cx <= x1; cx++) {
        int id = cz * s->d.cells_x + cx;
        st_cell *c = &s->cells[id];
        if (c->state != CW_CELL_NONE || c->live) continue;
        int prio;
        double d2 = cell_d2(s, id, &prio);
        if (d2 > L2) continue;
        c->d2 = d2;
        c->live = true; /* marks it a candidate once (sources overlap); undone below */
        st_cand e = { d2, prio, id };
        cw_array_push(&cand, e);
        if (d2 <= A2) need_active++;
      }
  }
  if (cand.count) {
    qsort(cand.data, (size_t)cand.count, sizeof *cand.data, cand_cmp);
    int budget = s->d.max_requests_per_tick;
    for (int i = 0; i < cand.count; i++) {
      st_cand e = cand.data[i];
      s->cells[e.id].live = false;
      /* cells that must activate now are requested whatever the budget */
      if (budget > 0 || e.d2 <= A2) {
        request(s, e.id, e.prio);
        budget--;
      }
    }
  }
  cw_array_free(&cand);
  (void)need_active;
  /* 3. activate what entered the active set, in id order; a cell not loaded yet makes the tick wait */
  for (int i = 0; i < s->live.count; i++) {
    int id = s->live.data[i];
    st_cell *c = &s->cells[id];
    if (c->state == CW_CELL_ACTIVE || c->d2 > A2) continue;
    if (!c->loaded_once || c->preparing) {
      double t0 = now_ms();
      if (!c->loaded_once) {
        if (!CW_HANDLE_IS_NULL(c->res)) cw_res_wait(c->res);
        run_on_load(s, id);
      }
      finish_preparing(s, id, true);
      double ms = now_ms() - t0;
      if (s->tick == 1) { /* the start position: what a game loads before play begins */
        s->st.start_cells++;
        s->st.start_ms += ms;
      } else {
        s->st.stalls++;
        s->st.stall_ms += ms;
        if (ms > s->st.stall_ms_max) s->st.stall_ms_max = ms;
      }
    }
    if (s->d.on_activate) { cw_stream_cell ci = cell_info(s, id); s->d.on_activate(s, &ci, s->d.user); }
    hash_event(s, id, 1);
    s->st.activations++;
    c->state = CW_CELL_ACTIVE;
  }
}

typedef struct { double d2; int id; } st_prep;
static int prep_cmp(const void *a, const void *b) {
  const st_prep *x = a, *y = b;
  if (x->d2 != y->d2) return x->d2 < y->d2 ? -1 : 1;
  return (x->id > y->id) - (x->id < y->id);
}

void cw_stream_frame(cw_stream *s) {
  if (!s) return;
  /* cells whose on_load work finished in the background */
  for (int i = 0; i < s->live.count; i++)
    if (s->cells[s->live.data[i]].preparing) finish_preparing(s, s->live.data[i], false);
  st_prep best[64];
  int n = 0;
  for (int i = 0; i < s->live.count; i++) {
    int id = s->live.data[i];
    st_cell *c = &s->cells[id];
    if (c->state != CW_CELL_LOADING || c->loaded_once || !file_ready(c)) continue;
    st_prep e = { c->d2, id };
    if (n < (int)CW_ARRAY_LEN(best)) best[n++] = e;
    else if (prep_cmp(&e, &best[n - 1]) < 0) best[n - 1] = e;
    else continue;
    qsort(best, (size_t)n, sizeof *best, prep_cmp);
  }
  for (int i = 0; i < n && i < s->d.prepare_per_frame; i++) run_on_load(s, best[i].id);
}

/* ── queries ─────────────────────────────────────────────────────────── */
cw_cell_state cw_stream_cell_state(const cw_stream *s, int cx, int cz) {
  if (!s || cx < 0 || cz < 0 || cx >= s->d.cells_x || cz >= s->d.cells_z) return CW_CELL_NONE;
  return (cw_cell_state)s->cells[cz * s->d.cells_x + cx].state;
}
void *cw_stream_cell_data(const cw_stream *s, int cx, int cz) {
  if (!s || cx < 0 || cz < 0 || cx >= s->d.cells_x || cz >= s->d.cells_z) return NULL;
  const st_cell *c = &s->cells[cz * s->d.cells_x + cx];
  return c->loaded_once ? c->data : NULL;
}
bool cw_stream_cell_at(const cw_stream *s, cw_dvec3 p, int *cx, int *cz) {
  double fx = floor((p.x - s->d.origin.x) / s->d.cell_size), fz = floor((p.z - s->d.origin.z) / s->d.cell_size);
  if (fx < 0 || fz < 0 || fx >= s->d.cells_x || fz >= s->d.cells_z) return false;
  *cx = (int)fx, *cz = (int)fz;
  return true;
}

void cw_stream_get_stats(const cw_stream *s, cw_stream_stats *o) {
  memset(o, 0, sizeof *o);
  if (!s) return;
  *o = s->st;
  o->loading = o->loaded = o->active = 0;
  for (int i = 0; i < s->live.count; i++) {
    const st_cell *c = &s->cells[s->live.data[i]];
    if (c->state == CW_CELL_LOADING) o->loading++;
    else o->loaded++;
    if (c->state == CW_CELL_ACTIVE) o->active++;
  }
}

void cw_stream_destroy(cw_stream *s) {
  if (!s) return;
  for (int i = 0; i < s->live.count; i++) drop(s, s->live.data[i]);
  cw_array_free(&s->live);
  cw_free(s->cells);
  cw_free(s->path_format);
  cw_free(s);
}

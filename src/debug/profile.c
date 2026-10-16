/* The profiler (cartwheel/profile.h): per-thread rings of completed zones, a
 * ring of frame records with counters, a GPU lane, the Chrome trace writer,
 * and the `profile` debug command.
 *
 * Rings are written only by their own thread (the slot, then a release store
 * of the write count) and read by the main thread while it answers a request
 * or writes a trace (start or end of a frame, when the frame's jobs are all
 * finished). A reader skips the oldest RING_SLACK entries so a writer that is
 * still running (a resource decode, the audio thread) never overwrites what is
 * being copied. */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"

void cw__gpu_timing_request(void); /* render/pipeline.c: start the GPU pass timers */

#define MAX_THREADS 64
#define RING_CAP (1u << 16) /* zones per thread */
#define RING_SLACK 1024u
#define FRAME_CAP 1024u
#define MAX_COUNTERS 24
#define MAX_DEPTH 64
#define GPU_TID 1000
#define MAX_GPU 48 /* GPU pass times kept per frame */

typedef struct {
  const char *name;
  uint64_t t0, t1;
} pev;

typedef struct ring {
  pev *ev;
  _Atomic uint64_t head; /* zones written so far */
  int id, tid;
  char name[24];
  /* owner thread only */
  unsigned gen; /* the recording session the stack belongs to */
  const char *stack[MAX_DEPTH];
  uint64_t stack_t0[MAX_DEPTH];
  int depth;
} ring;

typedef struct {
  uint64_t index, tick;
  uint64_t t0, t_render, t_end, t_next; /* t_next 0 while it is the latest frame */
  uint32_t ticks;
  int gpu_n; /* the render passes' GPU times, in the order they arrived */
  struct { const char *pass; float ms; } gpu[MAX_GPU];
  bool done;
  int counter_n;
  struct { const char *name; double v; } counters[MAX_COUNTERS];
} pframe;

int cw__profile_on;
static unsigned session; /* bumped at every start: rings with an older stack forget it */
static ring *rings[MAX_THREADS];
static atomic_int ring_count;
static _Thread_local ring *my_ring;
static _Thread_local bool is_main_thread;
static _Thread_local const char *thread_label; /* cw__profile_thread_name */

static pframe *frames;
static uint64_t frame_count; /* frames begun (index into frames = n % FRAME_CAP) */
static uint64_t frames_ended;
static pframe *cur;          /* the frame being recorded, NULL between frames */

uint64_t cw_profile_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool cw_profile_compiled(void) {
#ifdef CW_PROFILER_OFF
  return false;
#else
  return true;
#endif
}
bool cw_profile_enabled(void) { return __atomic_load_n(&cw__profile_on, __ATOMIC_RELAXED) != 0; }

/* GPU pass times: the timer queries cost CPU time of their own (reading
 * their results back is a driver round trip per pass), so a trace has them
 * only when asked (CW_PROFILE_GPU=1, `profile on gpu`, `profile trace <n> gpu`) */
void cw_profile_gpu(bool on) {
  if (on) cw__gpu_timing_request();
}

void cw_profile_set_enabled(bool on) {
  if (on == cw_profile_enabled()) return;
  if (on) {
    if (!frames) frames = cw_calloc(FRAME_CAP, sizeof *frames, CW_MEM_CORE);
    __atomic_add_fetch(&session, 1, __ATOMIC_RELAXED);
  } else {
    cur = NULL;
  }
  __atomic_store_n(&cw__profile_on, on ? 1 : 0, __ATOMIC_RELEASE);
}

static ring *new_ring(const char *name, int tid) {
  int id = atomic_fetch_add(&ring_count, 1);
  if (id >= MAX_THREADS) {
    atomic_fetch_sub(&ring_count, 1);
    return NULL;
  }
  ring *r = cw_calloc(1, sizeof *r, CW_MEM_CORE);
  r->ev = cw_calloc(RING_CAP, sizeof *r->ev, CW_MEM_CORE);
  r->id = id;
  r->tid = tid >= 0 ? tid : 100 + id;
  snprintf(r->name, sizeof r->name, "%s", name);
  __atomic_store_n(&rings[id], r, __ATOMIC_RELEASE);
  return r;
}

static ring *get_ring(void) {
  ring *r = my_ring;
  if (!r) {
    char name[24];
    int w = cw_jobs_worker_index(), tid = -1;
    if (is_main_thread) snprintf(name, sizeof name, "main"), tid = 0;
    else if (thread_label) snprintf(name, sizeof name, "%s", thread_label);
    else if (w > 0) snprintf(name, sizeof name, "worker %d", w), tid = w;
    else snprintf(name, sizeof name, "thread");
    r = my_ring = new_ring(name, tid);
    if (!r) return NULL;
  }
  unsigned s = __atomic_load_n(&session, __ATOMIC_RELAXED);
  if (r->gen != s) { /* zones left open when recording last stopped are gone */
    r->gen = s;
    r->depth = 0;
  }
  return r;
}

void cw__profile_thread_name(const char *name);
void cw__profile_thread_name(const char *name) { thread_label = name; }

static void push_event(ring *r, const char *name, uint64_t t0, uint64_t t1) {
  uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
  pev *e = &r->ev[h & (RING_CAP - 1)];
  e->name = name;
  e->t0 = t0;
  e->t1 = t1;
  atomic_store_explicit(&r->head, h + 1, memory_order_release);
}

void cw_profile_begin(const char *name) {
  if (!cw_profile_enabled()) return;
  ring *r = get_ring();
  if (!r) return;
  if (r->depth < MAX_DEPTH) {
    r->stack[r->depth] = name;
    r->stack_t0[r->depth] = cw_profile_now_ns();
  }
  r->depth++;
}

void cw__profile_push(const char *name, uint64_t t0, uint64_t t1) {
  if (!cw_profile_enabled()) return;
  ring *r = get_ring();
  if (r) push_event(r, name, t0, t1 < t0 ? t0 : t1);
}

void cw_profile_end(void) {
  ring *r = my_ring;
  if (!r || r->depth <= 0 || r->gen != __atomic_load_n(&session, __ATOMIC_RELAXED)) return; /* opened before recording */
  r->depth--;
  if (r->depth < MAX_DEPTH) push_event(r, r->stack[r->depth], r->stack_t0[r->depth], cw_profile_now_ns());
}

cw__profile_scope cw__profile_scope_begin(const char *name) {
  cw_profile_begin(name);
  return (cw__profile_scope){ .open = my_ring != NULL };
}

void cw__profile_scope_end(cw__profile_scope *s) {
  if (s->open) cw_profile_end();
}

const char *cw_profile_current(void) {
  ring *r = my_ring;
  if (!cw_profile_enabled() || !r || r->depth <= 0 || r->depth > MAX_DEPTH) return NULL;
  if (r->gen != __atomic_load_n(&session, __ATOMIC_RELAXED)) return NULL;
  return r->stack[r->depth - 1];
}

/* "<prefix><name>", interned once per pair of pointers */
#define NAME_CACHE 512
static struct { const char *a, *b, *s; } name_cache[NAME_CACHE];
static atomic_flag name_lock = ATOMIC_FLAG_INIT;
const char *cw__profile_name2(const char *a, const char *b) {
  if (!a || !b) return a ? a : b;
  unsigned h = (unsigned)(((uintptr_t)a * 31u) ^ ((uintptr_t)b * 2654435761u)) % NAME_CACHE;
  while (atomic_flag_test_and_set_explicit(&name_lock, memory_order_acquire)) {}
  const char *s = NULL;
  for (unsigned i = 0; i < 8 && !s; i++) {
    unsigned k = (h + i) % NAME_CACHE;
    if (name_cache[k].a == a && name_cache[k].b == b) s = name_cache[k].s;
    else if (!name_cache[k].a) {
      char buf[160];
      snprintf(buf, sizeof buf, "%s%s", a, b);
      s = cw_intern(buf);
      name_cache[k].a = a, name_cache[k].b = b, name_cache[k].s = s;
    }
  }
  atomic_flag_clear_explicit(&name_lock, memory_order_release);
  if (!s) { /* the probe window is full: intern without caching */
    char buf[160];
    snprintf(buf, sizeof buf, "%s%s", a, b);
    s = cw_intern(buf);
  }
  return s;
}

const char *cw__profile_job_name(void) {
  const char *c = cw_profile_current();
  if (!c) return NULL;
  size_t n = strlen(c);
  if (n > 4 && !strcmp(c + n - 4, "/job")) return c; /* a job submitted from a job keeps the root's name */
  return cw__profile_name2(c, "/job");
}

void cw_profile_counter(const char *name, double v) {
  if (!cur) return;
  for (int i = 0; i < cur->counter_n; i++)
    if (cur->counters[i].name == name || !strcmp(cur->counters[i].name, name)) {
      cur->counters[i].v = v;
      return;
    }
  if (cur->counter_n < MAX_COUNTERS) {
    cur->counters[cur->counter_n].name = name;
    cur->counters[cur->counter_n].v = v;
    cur->counter_n++;
  }
}

/* ── frames ───────────────────────────────────────────────────────── */

static pframe *frame_by_index(uint64_t index) {
  if (!frames) return NULL;
  uint64_t first = frame_count > FRAME_CAP ? frame_count - FRAME_CAP : 0;
  for (uint64_t n = frame_count; n-- > first;) {
    pframe *f = &frames[n % FRAME_CAP];
    if (f->index == index) return f;
    if (f->index < index) break;
  }
  return NULL;
}

void cw__profile_gpu(const char *pass, uint64_t frame, float ms) {
  if (!cw_profile_enabled() || ms < 0) return;
  pframe *f = frame_by_index(frame);
  if (!f || f->gpu_n >= MAX_GPU) return;
  f->gpu[f->gpu_n].pass = pass;
  f->gpu[f->gpu_n].ms = ms;
  f->gpu_n++;
}

/* the native trace file (CW_PROFILE_TRACE) */
static struct {
  const char *path;
  uint64_t start, frames;
  bool armed, written;
} envtrace;
static void write_env_trace(void);
void cw__profile_register_commands(void);

void cw__profile_frame_begin(uint64_t frame, uint64_t tick) {
  static bool env_checked;
  is_main_thread = true;
  if (!env_checked) {
    env_checked = true;
    cw__profile_register_commands();
#if !CW_CART
    const char *g = getenv("CW_PROFILE_GPU");
    if (g && atoi(g)) cw_profile_gpu(true);
    const char *e = getenv("CW_PROFILE");
    if (e && atoi(e)) cw_profile_set_enabled(true);
    const char *t = getenv("CW_PROFILE_TRACE");
    if (t && *t) {
      const char *n = getenv("CW_PROFILE_TRACE_FRAMES"), *s = getenv("CW_PROFILE_TRACE_START");
      envtrace.path = t;
      envtrace.frames = n && atoi(n) > 0 ? (uint64_t)atoi(n) : 120;
      envtrace.start = s && atoi(s) > 0 ? (uint64_t)atoi(s) : 0;
      envtrace.armed = true;
    }
#endif
  }
  if (envtrace.armed && frame >= envtrace.start && !cw_profile_enabled()) cw_profile_set_enabled(true);
  if (!cw_profile_enabled()) return;
  uint64_t now = cw_profile_now_ns();
  if (frame_count) {
    pframe *prev = &frames[(frame_count - 1) % FRAME_CAP];
    if (prev->done && !prev->t_next) prev->t_next = now;
  }
  cur = &frames[frame_count % FRAME_CAP];
  memset(cur, 0, sizeof *cur);
  cur->index = frame;
  cur->tick = tick;
  cur->t0 = now;
  frame_count++;
}

void cw__profile_frame_render(void) {
  if (cur) cur->t_render = cw_profile_now_ns();
}

void cw__profile_frame_end(uint32_t ticks_run) {
  if (!cur) return;
  cur->t_end = cw_profile_now_ns();
  if (!cur->t_render) cur->t_render = cur->t_end;
  cur->ticks = ticks_run;
  cur->done = true;
  uint64_t index = cur->index;
  cur = NULL;
  frames_ended++;
  if (envtrace.armed && index + 1 >= envtrace.start + envtrace.frames) write_env_trace();
}

/* ── reading zones ─────────────────────────────────────────────────── */

typedef CW_ARRAY(pev) pev_array;

static int by_start(const void *a, const void *b) {
  const pev *x = a, *y = b;
  if (x->t0 != y->t0) return x->t0 < y->t0 ? -1 : 1;
  if (x->t1 != y->t1) return x->t1 > y->t1 ? -1 : 1; /* the outer zone first */
  return 0;
}

/* every zone of ring r that starts in [t0, t1), sorted by start (outer first);
 * *lost is set when the ring no longer holds the start of the window */
static void collect(ring *r, uint64_t t0, uint64_t t1, pev_array *out, bool *lost) {
  uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  uint64_t lo = h > RING_CAP - RING_SLACK ? h - (RING_CAP - RING_SLACK) : 0;
  if (lo > 0 && lost) {
    pev first = r->ev[lo & (RING_CAP - 1)];
    if (first.t0 > t0) *lost = true;
  }
  for (uint64_t i = lo; i < h; i++) {
    pev e = r->ev[i & (RING_CAP - 1)];
    if (!e.name || e.t1 < e.t0 || e.t0 < t0 || e.t0 >= t1) continue;
    cw_array_push(out, e);
  }
  if (out->count > 1) qsort(out->data, (size_t)out->count, sizeof *out->data, by_start);
}

static int ring_total(void) {
  int n = atomic_load(&ring_count);
  return n > MAX_THREADS ? MAX_THREADS : n;
}

/* the last n finished frames (0 = all kept): the oldest and newest, and how many */
static int frame_window(int n, pframe **oldest, pframe **newest) {
  *oldest = *newest = NULL;
  if (!frames) return 0;
  uint64_t first = frame_count > FRAME_CAP ? frame_count - FRAME_CAP : 0;
  int got = 0;
  for (uint64_t k = frame_count; k-- > first && (n <= 0 || got < n);) {
    pframe *f = &frames[k % FRAME_CAP];
    if (!f->done) continue;
    if (*newest && f->index + 1 != (*oldest)->index) break; /* a gap: recording was off */
    if (!*newest) *newest = f;
    *oldest = f;
    got++;
  }
  return got;
}

static double cpu_ms(const pframe *f) { return (double)(f->t_end - f->t0) / 1e6; }
static double wall_ms(const pframe *f) { return f->t_next ? (double)(f->t_next - f->t0) / 1e6 : cpu_ms(f); }

/* ── the Chrome trace ─────────────────────────────────────────────── */

typedef struct { char *s; size_t len, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2 + 4096;
    b->s = cw_realloc(b->s, b->cap, CW_MEM_CORE);
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = 0;
}
static void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sbuf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) {
    sb_put(b, tmp, (size_t)n);
    return;
  }
  char *big = cw_alloc((size_t)n + 1, CW_MEM_CORE);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  sb_put(b, big, (size_t)n);
  cw_free(big);
}
static void sb_str(sbuf *b, const char *s) {
  sb_put(b, "\"", 1);
  for (; s && *s; s++) {
    if (*s == '"' || *s == '\\') {
      char e[2] = { '\\', *s };
      sb_put(b, e, 2);
    } else if ((unsigned char)*s < 32) {
      sb_printf(b, "\\u%04x", *s);
    } else {
      sb_put(b, s, 1);
    }
  }
  sb_put(b, "\"", 1);
}

static void sb_event_sep(sbuf *b, int *n) {
  if ((*n)++) sb_put(b, ",\n", 2);
  else sb_put(b, "\n", 1);
}

static const char *cat_of(const char *name) {
  size_t n = strlen(name);
  if (n > 4 && !strcmp(name + n - 4, "/job")) return "job";
  if (!strncmp(name, "pass ", 5)) return "pass";
  return "cpu";
}

static int trace_events; /* events in the last trace built */

char *cw_profile_trace_json(int nframes, size_t *out_len) {
  pframe *oldest, *newest;
  int got = frame_window(nframes, &oldest, &newest);
  if (out_len) *out_len = 0;
  if (!got) return NULL;
  uint64_t base = oldest->t0;
  uint64_t end = newest->t_next ? newest->t_next : newest->t_end + 1;
  sbuf b = { 0 };
  int n = 0;
  bool lost = false;
  sb_printf(&b, "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[");
  sb_event_sep(&b, &n);
  sb_printf(&b, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,\"args\":{\"name\":\"cartwheel\"}}");
  ring *ls[MAX_THREADS];
  int nl = 0;
  for (int i = 0, nt = ring_total(); i < nt; i++) {
    ring *r = __atomic_load_n(&rings[i], __ATOMIC_ACQUIRE);
    if (r) ls[nl++] = r;
  }
  for (int i = 0; i < nl; i++) {
    sb_event_sep(&b, &n);
    sb_printf(&b, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"args\":{\"name\":", ls[i]->tid);
    sb_str(&b, ls[i]->name);
    sb_printf(&b, "}}");
    sb_event_sep(&b, &n);
    sb_printf(&b, "{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"args\":{\"sort_index\":%d}}", ls[i]->tid,
              ls[i]->tid);
  }
  /* frames on the main lane, with their counters */
  double cpu_sum = 0;
  for (uint64_t idx = oldest->index;; idx++) {
    pframe *f = frame_by_index(idx);
    if (f && f->done) {
      cpu_sum += cpu_ms(f);
      sb_event_sep(&b, &n);
      sb_printf(&b,
                "{\"name\":\"frame\",\"cat\":\"frame\",\"ph\":\"X\",\"pid\":1,\"tid\":0,\"ts\":%.3f,\"dur\":%.3f,"
                "\"args\":{\"frame\":%llu,\"tick\":%llu,\"ticks\":%u,\"cpu_ms\":%.4f,\"wall_ms\":%.4f}}",
                (double)(f->t0 - base) / 1e3, (double)(f->t_end - f->t0) / 1e3, (unsigned long long)f->index,
                (unsigned long long)f->tick, f->ticks, cpu_ms(f), wall_ms(f));
      sb_event_sep(&b, &n);
      sb_printf(&b, "{\"name\":\"frame cpu ms\",\"ph\":\"C\",\"pid\":1,\"tid\":0,\"ts\":%.3f,\"args\":{\"ms\":%.4f}}",
                (double)(f->t0 - base) / 1e3, cpu_ms(f));
      for (int c = 0; c < f->counter_n; c++) {
        sb_event_sep(&b, &n);
        sb_printf(&b, "{\"name\":");
        sb_str(&b, f->counters[c].name);
        sb_printf(&b, ",\"ph\":\"C\",\"pid\":1,\"tid\":0,\"ts\":%.3f,\"args\":{\"value\":%.6g}}", (double)(f->t0 - base) / 1e3,
                  f->counters[c].v);
      }
    }
    if (f == newest || idx > newest->index) break;
  }
  /* the GPU lane: each frame's pass times end to end from the start of its
   * render, or from where the previous frame's GPU work ended (the GPU runs
   * one thing at a time): the durations are measured, the positions are not */
  uint64_t gpu_at = 0;
  bool gpu_named = false;
  for (uint64_t idx = oldest->index; idx <= newest->index; idx++) {
    pframe *f = frame_by_index(idx);
    if (!f || !f->done || !f->gpu_n) continue;
    if (!gpu_named) {
      gpu_named = true;
      sb_event_sep(&b, &n);
      sb_printf(&b, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"args\":{\"name\":\"GPU\"}}", GPU_TID);
      sb_event_sep(&b, &n);
      sb_printf(&b, "{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"args\":{\"sort_index\":%d}}", GPU_TID, GPU_TID);
    }
    if (gpu_at < f->t_render) gpu_at = f->t_render;
    for (int k = 0; k < f->gpu_n; k++) {
      uint64_t d = (uint64_t)((double)f->gpu[k].ms * 1e6);
      sb_event_sep(&b, &n);
      sb_printf(&b, "{\"name\":");
      sb_str(&b, f->gpu[k].pass);
      sb_printf(&b, ",\"cat\":\"gpu\",\"ph\":\"X\",\"pid\":1,\"tid\":%d,\"ts\":%.3f,\"dur\":%.3f,\"args\":{\"frame\":%llu}}", GPU_TID,
                (double)(gpu_at - base) / 1e3, (double)d / 1e3, (unsigned long long)f->index);
      gpu_at += d;
    }
  }
  /* zones, per lane in start order (a lane's zones nest: each is a stack's) */
  for (int i = 0; i < nl; i++) {
    pev_array zs = { 0 };
    collect(ls[i], base, end, &zs, &lost);
    for (int k = 0; k < zs.count; k++) {
      pev *e = &zs.data[k];
      sb_event_sep(&b, &n);
      sb_printf(&b, "{\"name\":");
      sb_str(&b, e->name);
      sb_printf(&b, ",\"cat\":\"%s\",\"ph\":\"X\",\"pid\":1,\"tid\":%d,\"ts\":%.3f,\"dur\":%.3f}", cat_of(e->name), ls[i]->tid,
                (double)(e->t0 - base) / 1e3, (double)(e->t1 - e->t0) / 1e3);
    }
    cw_array_free(&zs);
  }
  sb_printf(&b, "\n],\"otherData\":{\"engine\":\"cartwheel\",\"frames\":%d,\"first_frame\":%llu,\"cpu_ms_avg\":%.4f%s}}\n", got,
            (unsigned long long)oldest->index, cpu_sum / got, lost ? ",\"lost_zones\":true" : "");
  trace_events = n;
  if (out_len) *out_len = b.len;
  return b.s;
}

static void write_env_trace(void) {
  envtrace.armed = false;
  if (envtrace.written) return;
  envtrace.written = true;
#if !CW_CART
  size_t len = 0;
  char *j = cw_profile_trace_json((int)envtrace.frames, &len);
  FILE *f = j ? fopen(envtrace.path, "wb") : NULL;
  if (!f) {
    cw_warn("profile: could not write the trace %s (%s)", envtrace.path, j ? "cannot open the file" : "no frame recorded");
  } else {
    fwrite(j, 1, len, f);
    fclose(f);
    cw_info("profile: wrote %s (%d events)", envtrace.path, trace_events);
  }
  cw_free(j);
#endif
}

void cw__profile_shutdown(void) {
  if (envtrace.armed && envtrace.path) write_env_trace();
}

/* ── top: exclusive time per zone name over recent frames ─────────────── */

typedef struct { const char *name; double incl, excl; int calls; uint64_t threads; } top_row;
typedef CW_ARRAY(top_row) top_rows;

static void add_top(top_rows *rows, const char *name, double incl, double excl, int thread) {
  for (int i = 0; i < rows->count; i++)
    if (rows->data[i].name == name || !strcmp(rows->data[i].name, name)) {
      rows->data[i].incl += incl;
      rows->data[i].excl += excl;
      rows->data[i].calls++;
      rows->data[i].threads |= 1ull << (thread & 63);
      return;
    }
  top_row r = { name, incl, excl, 1, 1ull << (thread & 63) };
  cw_array_push(rows, r);
}

static void cmd_top(int argc, const char **argv, cw_reply *r) {
  int nframes = argc > 2 ? atoi(argv[2]) : 60, limit = argc > 3 ? atoi(argv[3]) : 15;
  pframe *oldest, *newest;
  int got = frame_window(nframes < 1 ? 1 : nframes, &oldest, &newest);
  if (!got) {
    cw_reply_error(r, "no frames recorded: `profile on` starts recording");
    return;
  }
  double cpu_sum = 0;
  for (uint64_t idx = oldest->index; idx <= newest->index; idx++) {
    pframe *f = frame_by_index(idx);
    if (f && f->done) cpu_sum += cpu_ms(f);
  }
  top_rows rows = { 0 };
  for (int i = 0, nt = ring_total(); i < nt; i++) {
    ring *rg = __atomic_load_n(&rings[i], __ATOMIC_ACQUIRE);
    if (!rg) continue;
    pev_array zs = { 0 };
    collect(rg, oldest->t0, newest->t_end + 1, &zs, NULL);
    /* exclusive = own duration minus direct children (start order: a stack walk) */
    int st[MAX_DEPTH + 1], sp = 0;
    double *child = cw_calloc((size_t)zs.count + 1, sizeof *child, CW_MEM_CORE);
    for (int k = 0; k < zs.count; k++) {
      pev *e = &zs.data[k];
      while (sp && zs.data[st[sp - 1]].t1 <= e->t0) sp--;
      if (sp) child[st[sp - 1]] += (double)(e->t1 - e->t0);
      if (sp <= MAX_DEPTH) st[sp++] = k;
    }
    for (int k = 0; k < zs.count; k++) {
      pev *e = &zs.data[k];
      double d = (double)(e->t1 - e->t0);
      add_top(&rows, e->name, d / 1e6, (d - child[k] > 0 ? d - child[k] : 0) / 1e6, rg->tid);
    }
    cw_free(child);
    cw_array_free(&zs);
  }
  for (int i = 1; i < rows.count; i++) {
    top_row k = rows.data[i];
    int j = i - 1;
    while (j >= 0 && rows.data[j].excl < k.excl) {
      rows.data[j + 1] = rows.data[j];
      j--;
    }
    rows.data[j + 1] = k;
  }
  cw_reply_printf(r, "{\"frames\":%d,\"cpu_ms_avg\":%.4f,\"top\":[", got, cpu_sum / got);
  for (int i = 0; i < rows.count && i < limit; i++) {
    top_row *t = &rows.data[i];
    cw_reply_printf(r, "%s{\"name\":", i ? "," : "");
    cw_reply_str(r, t->name);
    cw_reply_printf(r, ",\"excl_ms\":%.4f,\"incl_ms\":%.4f,\"calls\":%.2f,\"threads\":%d}", t->excl / got, t->incl / got,
                    (double)t->calls / got, __builtin_popcountll(t->threads));
  }
  cw_reply_raw(r, "]}");
  cw_array_free(&rows);
}

/* ── the command ───────────────────────────────────────────────────── */

/* the trace a `profile trace` request built, read with `profile trace_get` */
static struct {
  bool pending, was_on;
  int frames;
  uint64_t ended_at; /* frames_ended when it was asked */
  char *json;
  size_t len;
} tr;

#define TRACE_CHUNK (96 * 1024)

static void status_json(cw_reply *r) {
  int nt = ring_total();
  cw_reply_printf(r, "{\"enabled\":%s,\"compiled\":%s,\"frames\":%llu,\"threads\":[", cw_profile_enabled() ? "true" : "false",
                  cw_profile_compiled() ? "true" : "false",
                  (unsigned long long)(frame_count < FRAME_CAP ? frame_count : FRAME_CAP));
  for (int i = 0, n = 0; i < nt; i++) {
    ring *rg = __atomic_load_n(&rings[i], __ATOMIC_ACQUIRE);
    if (!rg) continue;
    cw_reply_raw(r, n++ ? "," : "");
    cw_reply_str(r, rg->name);
  }
  cw_reply_printf(r, "],\"trace\":{\"pending\":%s,\"bytes\":%zu}}", tr.pending ? "true" : "false", tr.json ? tr.len : (size_t)0);
}

static bool trace_wait(cw_reply *r, void *state) {
  (void)state;
  if (!r) { /* the request was dropped */
    tr.pending = false;
    if (!tr.was_on) cw_profile_set_enabled(false);
    return true;
  }
  if (frames_ended - tr.ended_at < (uint64_t)tr.frames) return false;
  tr.pending = false;
  cw_free(tr.json);
  tr.json = cw_profile_trace_json(tr.frames, &tr.len);
  if (!tr.was_on) cw_profile_set_enabled(false);
  if (!tr.json) {
    cw_reply_error(r, "no frame was recorded");
    return true;
  }
  cw_reply_printf(r, "{\"frames\":%d,\"events\":%d,\"bytes\":%zu,\"chunk\":%d,\"threads\":[", tr.frames, trace_events, tr.len,
                  TRACE_CHUNK);
  for (int i = 0, n = 0, nt = ring_total(); i < nt; i++) {
    ring *rg = __atomic_load_n(&rings[i], __ATOMIC_ACQUIRE);
    if (!rg) continue;
    cw_reply_raw(r, n++ ? "," : "");
    cw_reply_str(r, rg->name);
  }
  cw_reply_raw(r, "]}");
  return true;
}

static void cmd_trace(int argc, const char **argv, cw_reply *r) {
  int n = argc > 2 ? atoi(argv[2]) : 60;
  if (n < 1 || n > (int)FRAME_CAP - 2) {
    cw_reply_error(r, "profile trace <frames>: 1 to %u frames (got '%s')", FRAME_CAP - 2, argc > 2 ? argv[2] : "");
    return;
  }
  if (tr.pending) {
    cw_reply_error(r, "a trace is already recording: wait for its reply");
    return;
  }
  tr.pending = true;
  tr.frames = n;
  tr.was_on = cw_profile_enabled();
  tr.ended_at = frames_ended;
  cw_profile_set_enabled(true);
  if (argc > 3 && !strcmp(argv[3], "gpu")) cw_profile_gpu(true);
  cw_reply_defer(r, trace_wait, NULL);
}

static void cmd_trace_get(int argc, const char **argv, cw_reply *r) {
  if (!tr.json) {
    cw_reply_error(r, "no trace recorded: `profile trace <frames>` records one");
    return;
  }
  long off = argc > 2 ? atol(argv[2]) : 0;
  if (off < 0 || (size_t)off > tr.len) {
    cw_reply_error(r, "offset %ld is outside the trace (bytes %zu)", off, tr.len);
    return;
  }
  size_t k = tr.len - (size_t)off < TRACE_CHUNK ? tr.len - (size_t)off : TRACE_CHUNK;
  char save = tr.json[off + k];
  tr.json[off + k] = 0;
  cw_reply_printf(r, "{\"offset\":%ld,\"bytes\":%zu,\"total\":%zu,\"more\":%s,\"text\":", off, k, tr.len,
                  (size_t)off + k < tr.len ? "true" : "false");
  cw_reply_str(r, tr.json + off);
  cw_reply_raw(r, "}");
  tr.json[off + k] = save;
}

static void cmd_profile(int argc, const char **argv, cw_reply *r) {
  const char *op = argc > 1 ? argv[1] : "status";
  if (!strcmp(op, "on") || !strcmp(op, "off")) {
    cw_profile_set_enabled(!strcmp(op, "on"));
    if (argc > 2 && !strcmp(argv[2], "gpu") && !strcmp(op, "on")) cw_profile_gpu(true);
    status_json(r);
  } else if (!strcmp(op, "status")) {
    status_json(r);
  } else if (!strcmp(op, "clear")) {
    frame_count = 0;
    cur = NULL;
    for (int i = 0, nt = ring_total(); i < nt; i++) {
      ring *rg = __atomic_load_n(&rings[i], __ATOMIC_ACQUIRE);
      if (rg) memset(rg->ev, 0, RING_CAP * sizeof *rg->ev);
    }
    status_json(r);
  } else if (!strcmp(op, "frames")) {
    int n = argc > 2 ? atoi(argv[2]) : 120;
    pframe *oldest, *newest;
    int got = frame_window(n < 1 ? 1 : n, &oldest, &newest);
    cw_reply_printf(r, "{\"enabled\":%s,\"frames\":[", cw_profile_enabled() ? "true" : "false");
    for (int i = 0; got && i < got; i++) {
      pframe *f = frame_by_index(oldest->index + (uint64_t)i);
      if (!f) continue;
      cw_reply_printf(r, "%s[%llu,%.4f,%.4f,%u]", i ? "," : "", (unsigned long long)f->index, cpu_ms(f), wall_ms(f), f->ticks);
    }
    cw_reply_raw(r, "],\"columns\":[\"frame\",\"cpu_ms\",\"wall_ms\",\"ticks\"]}");
  } else if (!strcmp(op, "top")) {
    cmd_top(argc, argv, r);
  } else if (!strcmp(op, "trace")) {
    cmd_trace(argc, argv, r);
  } else if (!strcmp(op, "trace_get")) {
    cmd_trace_get(argc, argv, r);
  } else {
    cw_reply_error(r, "unknown profile op '%s': on [gpu], off, status, clear, frames [n], top [frames] [n], trace <frames> [gpu], "
                      "trace_get [offset]",
                   op);
  }
}

void cw__profile_register_commands(void);
void cw__profile_register_commands(void) {
  cw_debug_command("profile",
                   "profile on [gpu]|off|status|clear|frames [n]|top [frames] [n]|trace <frames> [gpu]|trace_get [offset]: the "
                   "engine profiler (cartwheel/profile.h). trace records that many frames and answers with the trace's "
                   "size; trace_get returns the Chrome trace JSON (ui.perfetto.dev) in pieces from offset. gpu adds the "
                   "render passes' GPU times (native; the timer queries cost CPU time of their own)",
                   cmd_profile);
}

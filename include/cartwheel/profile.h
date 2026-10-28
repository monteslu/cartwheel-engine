/* cartwheel profiler: named zones on every thread, frame boundaries, GPU pass
 * times, counters, and Chrome / Perfetto trace files.
 *
 *   void physics_step(...) {
 *     CW_PROFILE_SCOPE("physics3d.step");   // a zone until the end of the block
 *     ...
 *   }
 *   CW_PROFILE_COUNTER("render.draw_calls", st.draw_calls); // a per-frame number
 *
 * Cost. Compiled out (CMake -DCW_PROFILER=OFF, or `#define CW_PROFILE_ENABLED 0`
 * before including this header): the macros expand to nothing. Compiled in
 * but off at run time (the default): one relaxed atomic load per zone. On:
 * two monotonic clock reads and one 32-byte write into the thread's own ring.
 *
 * What is recorded. Each thread that records a zone (main, every job worker,
 * the native audio thread) gets its own ring of completed zones, written
 * without locks. A job chunk run on any thread is a zone named after the zone
 * that submitted the job ("physics3d.step/job"), so worker time is attributed
 * to the system that asked for it; chunks under one name that a thread runs
 * back to back are one zone. Natively the render passes' GPU times
 * (GL timer queries read back in a batch every 8 frames, WebGPU timestamp
 * queries) arrive several frames late and
 * land in a "GPU" lane, laid end to end from the start of their frame's
 * render: the durations are measured, the positions are not. They are off
 * unless asked for (cw_profile_gpu): the queries cost CPU time of their own.
 * In split screen each view's run of a pass is its own entry ("ssao@v1"),
 * and the pipeline's own GPU work between passes is timed as steps:
 * "views" (the views laid into the frame) and "resolve" (the multisampled
 * scene resolved, the depth copies the later passes sample). On WebGPU a
 * pass is timed whole, with every render three.c records for it (shadow
 * maps, G-buffers, probe captures).
 * Carts never issue GPU timer queries, so a cart's trace has no GPU lane.
 *
 * Turning it on: cw_profile_set_enabled, `profile on` over the debug channel,
 * CW_PROFILE=1, or a trace request (below). Profiling never changes what the
 * game simulates: it reads clocks and writes its own buffers only.
 *
 * Trace files (Chrome trace event JSON: open in ui.perfetto.dev,
 * chrome://tracing or speedscope):
 *   - natively, CW_PROFILE_TRACE=out.json [CW_PROFILE_TRACE_FRAMES=120]
 *     [CW_PROFILE_TRACE_START=0] records frames START.. START+FRAMES-1 and writes
 *     the file when the last one ends (or at exit, with what was recorded);
 *   - from any host (carts through romdev too), the debug channel:
 *     `profile trace <frames>` records that many frames and then answers with
 *     the trace's size; `profile trace_get [offset]` returns it in pieces
 *     (tools/cwtrace.mjs does both and writes the file);
 *   - from code, cw_profile_trace_json.
 * Other channel requests: profile on|off|status|clear, profile frames [n] (cpu
 * and wall ms per recorded frame), profile top [frames] [n] (zones by
 * exclusive time). */
#ifndef CW_PROFILE_H
#define CW_PROFILE_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CW_PROFILE_ENABLED
#define CW_PROFILE_ENABLED 1
#endif

/* Monotonic nanoseconds (the profiler's clock). */
uint64_t cw_profile_now_ns(void);

/* Recording on/off at run time (main thread). The rings are allocated the
 * first time a thread records while on. */
void cw_profile_set_enabled(bool on);
bool cw_profile_enabled(void); /* recording now (any thread) */
/* GPU pass times into the trace's GPU lane (native only; once on, the timer
 * queries stay on). Off by default: reading the queries back costs CPU time
 * of its own, up to a millisecond or more a frame, so a CPU trace leaves them
 * out. CW_PROFILE_GPU=1 turns them on at start. */
void cw_profile_gpu(bool on);
/* Whether the engine library itself was built with its zones (CW_PROFILER). */
bool cw_profile_compiled(void);

/* A per-frame value (draw calls, bodies, contacts): the latest value set
 * during a frame is kept with it, and becomes a counter track in a trace.
 * name must be a string literal (or interned). Main thread. */
void cw_profile_counter(const char *name, double value);

/* Zones by hand, for code that cannot use a block scope. Every begin needs
 * its end on the same thread. name must outlive the profiler (a literal or
 * cw_intern). */
void cw_profile_begin(const char *name);
void cw_profile_end(void); /* ends this thread's innermost zone */

/* The innermost open zone on this thread (NULL when none or off). */
const char *cw_profile_current(void);

/* The last `frames` finished frames (0 = every recorded one) as a Chrome trace
 * event JSON document, cw_alloc'd (cw_free it); *out_len gets its length.
 * NULL when nothing was recorded. Main thread. */
char *cw_profile_trace_json(int frames, size_t *out_len);

/* ── engine hooks ──────────────────────────────────────────────────── */
void cw__profile_frame_begin(uint64_t frame, uint64_t tick); /* platform, start of a frame */
void cw__profile_frame_render(void);                         /* platform, the render phase starts */
void cw__profile_frame_end(uint32_t ticks_run);              /* platform, the frame's work is done */
void cw__profile_shutdown(void);                              /* platform: a pending trace file is written */
/* "<zone>/job": the name a job submitted from inside the current zone runs
 * under (NULL when off or outside every zone). Any thread. */
const char *cw__profile_job_name(void);
/* A finished zone, recorded after the fact (any thread; times from cw_profile_now_ns). */
void cw__profile_push(const char *name, uint64_t t0, uint64_t t1);
/* A render pass's GPU time for engine frame `frame` (main thread). */
void cw__profile_gpu(const char *pass, uint64_t frame, float ms);
/* "<prefix><name>" interned for zone names (any thread; cached by both pointers). */
const char *cw__profile_name2(const char *prefix, const char *name);

/* ── the macros ───────────────────────────────────────────────────── */
typedef struct cw__profile_scope { uint8_t open; } cw__profile_scope;
extern int cw__profile_on; /* read with a relaxed load by the macros */
cw__profile_scope cw__profile_scope_begin(const char *name);
void cw__profile_scope_end(cw__profile_scope *s);

#define CW__PROFILE_CAT2(a, b) a##b
#define CW__PROFILE_CAT(a, b) CW__PROFILE_CAT2(a, b)
#if CW_PROFILE_ENABLED
static inline void cw__profile_scope_close(cw__profile_scope *s) {
  if (s->open) cw__profile_scope_end(s);
}
#define CW_PROFILE_ON() (__atomic_load_n(&cw__profile_on, __ATOMIC_RELAXED) != 0)
#define CW_PROFILE_SCOPE(name)                                                                                   \
  cw__profile_scope CW__PROFILE_CAT(cw__pz_, __LINE__) __attribute__((cleanup(cw__profile_scope_close))) = \
      (CW_PROFILE_ON() ? cw__profile_scope_begin(name) : (cw__profile_scope){ 0 })
#define CW_PROFILE_COUNTER(name, value) \
  do {                                  \
    if (CW_PROFILE_ON()) cw_profile_counter(name, (double)(value)); \
  } while (0)
#define CW_PROFILE_BEGIN(name) \
  do {                         \
    if (CW_PROFILE_ON()) cw_profile_begin(name); \
  } while (0)
#define CW_PROFILE_END() \
  do {                   \
    if (CW_PROFILE_ON()) cw_profile_end(); \
  } while (0)
#else
#define CW_PROFILE_ON() 0
#define CW_PROFILE_SCOPE(name) ((void)(name))
#define CW_PROFILE_COUNTER(name, value) ((void)0)
#define CW_PROFILE_BEGIN(name) ((void)(name))
#define CW_PROFILE_END() ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif

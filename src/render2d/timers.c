/* render2d GPU timers (native, CW_R2D_TIMERS=1): GL time-elapsed queries,
 * or WebGPU timestamps (wgpu.c), round each part of cw_r2d_end, read back a
 * few frames later and averaged into debug fields "r2d_gpu.<part>" (ms per
 * frame). Off by default: queries cost a little, and a cart never has them,
 * so the fields exist only when asked for and never reach a gate's
 * cross-host comparison. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif

#if defined(__wasm__)
void r2d_timer_begin(int part) { (void)part; }
void r2d_timer_end(void) {}
void r2d_timers_frame(void) {}
void r2d_timers_wg_result(const float ms[R2D_T_COUNT], const bool seen[R2D_T_COUNT]) { (void)ms, (void)seen; }
bool r2d_timers_on(void) { return false; }
#else
/* gi_source: global illumination's source pass (gi is the rest of it); cull: depth culling's pre-passes */
static const char *const NAMES[R2D_T_COUNT] = { "draw", "field", "lights", "gi", "composite", "post", "gi_source", "cull" };
#define POOL 256
typedef struct pending { GLuint q; int part; uint64_t frame; } pending;
static struct {
  int on; /* -1 unknown, 0 off, 1 on */
  GLuint free_q[POOL];
  int nfree;
  pending pend[POOL];
  int npend;
  int open_part;
  uint64_t frame;
  double frame_sum[R2D_T_COUNT];
  uint64_t sum_frame;
  float avg[R2D_T_COUNT];
  bool seen[R2D_T_COUNT];
} T = { .on = -1, .open_part = -1 };

static bool timers_on(void) {
  if (T.on < 0) {
    const char *e = getenv("CW_R2D_TIMERS");
    T.on = e && atoi(e) ? 1 : 0;
    if (T.on && r2d_wg() && !r2d_wg_timers_init()) T.on = 0; /* WebGPU: the device has no timestamps */
    if (T.on) {
      if (!r2d_wg()) glGenQueries(POOL, T.free_q);
      T.nfree = POOL;
      for (int i = 0; i < R2D_T_COUNT; i++)
        cw_debug_f32(cw_intern(cw_frame_printf("r2d_gpu.%s", NAMES[i])), &T.avg[i]);
    }
  }
  return T.on == 1;
}

bool r2d_timers_on(void) { return timers_on(); }

void r2d_timer_begin(int part) {
  if (!timers_on() || T.open_part >= 0) return;
  if (r2d_wg()) {
    if (r2d_wg_timer_mark(part, false)) T.open_part = part;
    return;
  }
  if (!T.nfree || T.npend >= POOL) return;
  GLuint q = T.free_q[--T.nfree];
  glBeginQuery(GL_TIME_ELAPSED_EXT, q);
  T.open_part = part;
  T.pend[T.npend++] = (pending){ q, part, cw_frame_index() };
}

void r2d_timer_end(void) {
  if (T.on != 1 || T.open_part < 0) return;
  if (r2d_wg()) {
    r2d_wg_timer_mark(T.open_part, true);
    T.open_part = -1;
    return;
  }
  glEndQuery(GL_TIME_ELAPSED_EXT);
  T.open_part = -1;
}

/* collect finished queries; each finished frame's sums feed the averages */
/* WebGPU: a frame's parts, read back (wgpu.c's map callback) */
void r2d_timers_wg_result(const float ms[R2D_T_COUNT], const bool seen[R2D_T_COUNT]) {
  for (int k = 0; k < R2D_T_COUNT; k++)
    if (seen[k]) T.avg[k] = T.avg[k] == 0 ? ms[k] : T.avg[k] * 0.9f + ms[k] * 0.1f;
}

void r2d_timers_frame(void) {
  if (!timers_on() || r2d_wg()) return;
  int keep = 0;
  for (int i = 0; i < T.npend; i++) {
    pending p = T.pend[i];
    GLuint avail = 0;
    glGetQueryObjectuiv(p.q, GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) {
      T.pend[keep++] = p;
      continue;
    }
    GLuint ns = 0;
    glGetQueryObjectuiv(p.q, GL_QUERY_RESULT, &ns);
    if (p.frame != T.sum_frame) { /* a new frame's results: the last one's sums are complete */
      for (int k = 0; k < R2D_T_COUNT; k++) {
        if (!T.seen[k]) continue;
        T.avg[k] = T.avg[k] == 0 ? (float)T.frame_sum[k] : T.avg[k] * 0.9f + (float)T.frame_sum[k] * 0.1f;
      }
      memset(T.frame_sum, 0, sizeof T.frame_sum);
      T.sum_frame = p.frame;
    }
    T.frame_sum[p.part] += (double)ns / 1e6;
    T.seen[p.part] = true;
    T.free_q[T.nfree++] = p.q;
  }
  T.npend = keep;
}
#endif

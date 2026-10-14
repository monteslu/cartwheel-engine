/* Frame timing for perf measurement (tools/perf-frame.mjs): the CPU time each
 * frame spends in its phases, on every host, measured with the host's
 * monotonic clock (a cart's is the host's performance.now()).
 *
 *   input    frame start to the end of input polling (resource hook, debug
 *            channel, SDL events, warm-up checks)
 *   sim      the fixed ticks (fixed_update, physics)
 *   update   the app's update
 *   render   the app's render: scene walk, culling, three.c submission and
 *            every GPU API call it makes (wasm and, in a cart, its imports)
 *   present  native: swap / WebGPU present; a WebGPU cart: frame end hand-back;
 *            a GL cart: 0 (the host presents after wc_render returns)
 *   audio    a cart's audio hand-off: waiting for the frame's mix job (it
 *            runs on the job pool during the render) and moving the ring's
 *            write cursor; the whole mix when the frame had no update
 *   frame    start of one frame to the start of the next (wall time)
 *
 * Natively the same phases are also timed on the main thread's own CPU
 * clock ("thread_cpu" in the reply: averages, frame included): wall time in
 * the render phase counts waits on the GPU and the driver (back-pressure when
 * the GPU is behind) and other processes' turns on the core, thread CPU time
 * does not. That is the number to compare when the question is the CPU cost
 * of submitting a frame (splitscreen against one view, say).
 *
 * Read with the debug channel: "perf" (averages, p50, p95, max since the last
 * reset, plus the 3D renderer's draw counters) and "perf reset". Nothing here
 * feeds back into the simulation. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene.h"

enum { PH_INPUT, PH_SIM, PH_UPDATE, PH_RENDER, PH_PRESENT, PH_AUDIO, PH_FRAME, PH_COUNT };
static const char *const ph_names[PH_COUNT] = { "input", "sim", "update", "render", "present", "audio", "frame" };
#define RING 2048

static struct {
  double mark[PH_COUNT];
  double cur[PH_COUNT];
  double last_start;
  float ring[PH_COUNT][RING];
  /* the same phases in the main thread's CPU time (waits on the GPU or the
   * driver, and other processes' turns on the core, left out); -1 = the
   * host has no thread CPU clock */
  double cmark[PH_COUNT], clast_start;
  float cring[PH_COUNT][RING];
  unsigned n; /* frames recorded since the reset */
  bool registered;
} P;

static double thread_cpu_ms(void) {
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(__wasi__) /* (wasi-libc emulates process clocks only on request) */
  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
#endif
  return -1;
}

double cw__perf_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

static int cmp_f(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

static void cmd_perf(int argc, const char **argv, cw_reply *r) {
  if (argc > 1 && !strcmp(argv[1], "reset")) {
    P.n = 0;
    cw_reply_printf(r, "{\"reset\":true}");
    return;
  }
  /* "perf N": the last N frames only (default: all since the reset, up to the ring) */
  unsigned n = P.n < RING ? P.n : RING;
  if (argc > 1 && atoi(argv[1]) > 0 && (unsigned)atoi(argv[1]) < n) n = (unsigned)atoi(argv[1]);
  cw_reply_printf(r, "{\"frames\":%u", n);
  static float tmp[RING];
  for (int k = 0; k < PH_COUNT; k++) {
    double sum = 0;
    for (unsigned i = 0; i < n; i++) sum += tmp[i] = P.ring[k][(P.n - 1 - i) % RING];
    float p50 = 0, p95 = 0, mx = 0;
    if (n) {
      qsort(tmp, n, sizeof tmp[0], cmp_f);
      p50 = tmp[n / 2];
      p95 = tmp[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
      mx = tmp[n - 1];
    }
    cw_reply_printf(r, ",\"%s\":{\"avg\":%.4f,\"p50\":%.4f,\"p95\":%.4f,\"max\":%.4f}", ph_names[k], n ? sum / n : 0.0, p50, p95, mx);
  }
  /* the main thread's CPU time per phase (averages) */
  if (n && P.cring[PH_FRAME][(P.n - 1) % RING] >= 0) {
    cw_reply_raw(r, ",\"thread_cpu\":{");
    for (int k = 0; k < PH_COUNT; k++) {
      double sum = 0;
      for (unsigned i = 0; i < n; i++) sum += P.cring[k][(P.n - 1 - i) % RING];
      cw_reply_printf(r, "%s\"%s\":%.4f", k ? "," : "", ph_names[k], sum / n);
    }
    cw_reply_raw(r, "}");
  }
  cw_tree *t = cw_current_tree();
  if (t) {
    cw_render3d_stats s;
    cw_render3d_get_stats(t, &s);
    cw_reply_printf(r, ",\"draw_calls\":%d,\"triangles\":%d,\"programs\":%d,\"r3d_cpu_ms\":%.4f", s.draw_calls, s.triangles, s.programs, s.cpu_ms);
  }
  cw_reply_raw(r, "}");
}

/* phase k ends now (the frame start is cw__perf_frame_begin) */
void cw__perf_mark(int k) {
  P.mark[k] = cw__perf_now_ms();
  P.cmark[k] = thread_cpu_ms();
}

void cw__perf_frame_begin(void) {
  if (!P.registered) {
    P.registered = true;
    cw_debug_command("perf", "perf [N|reset]: CPU ms per frame phase (input, sim, update, render, present, audio, frame): avg, p50, p95, max over the last N frames (default: all since the last reset); the 3D draw counters", cmd_perf);
  }
  double now = cw__perf_now_ms(), cnow = thread_cpu_ms();
  if (P.last_start > 0) {
    /* the same in thread CPU time */
    double ct = P.clast_start;
    for (int k = 0; k <= PH_PRESENT; k++) {
      double m = P.mark[k] > 0 && P.cmark[k] > ct ? P.cmark[k] : ct;
      P.cring[k][P.n % RING] = cnow < 0 ? -1 : (float)(m - ct);
      ct = m;
    }
    P.cring[PH_AUDIO][P.n % RING] = cnow < 0 ? -1 : P.mark[PH_AUDIO] > 0 && P.cmark[PH_AUDIO] > ct ? (float)(P.cmark[PH_AUDIO] - ct) : 0;
    P.cring[PH_FRAME][P.n % RING] = cnow < 0 ? -1 : (float)(cnow - P.clast_start);
    /* the previous frame's phases, from its marks (a phase that did not run lasts 0) */
    double t = P.last_start;
    for (int k = 0; k <= PH_PRESENT; k++) {
      double m = P.mark[k] > t ? P.mark[k] : t;
      P.cur[k] = m - t;
      t = m;
    }
    P.cur[PH_AUDIO] = P.mark[PH_AUDIO] > t ? P.mark[PH_AUDIO] - t : 0;
    P.cur[PH_FRAME] = now - P.last_start;
    unsigned i = P.n % RING;
    for (int k = 0; k < PH_COUNT; k++) P.ring[k][i] = (float)P.cur[k];
    P.n++;
  }
  memset(P.mark, 0, sizeof P.mark);
  memset(P.cmark, 0, sizeof P.cmark);
  P.last_start = now;
  P.clast_start = cnow;
}

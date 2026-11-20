/* Timer node (cartwheel/gameplay.h): a countdown in fixed ticks. */
#include <string.h>

#include "cartwheel/gameplay.h"

typedef struct timer {
  double wait_time;
  bool one_shot, autostart, paused;
  bool running;
  int64_t elapsed_ticks;
  /* runtime */
  double dt; /* the last fixed dt seen (1/60 until the first tick) */
  bool readied;
} timer;

static cw_node_type timer_type;
static const char *s_timeout;

static int64_t wait_ticks(const timer *t) {
  double k = t->wait_time / t->dt + 0.5; /* IEEE division and a truncation: the same on every host */
  return k < 1 ? 1 : (int64_t)k;
}

void cw_timer_start(cw_node n, double wait_time) {
  timer *t = cw_node_data_t(n, &timer_type);
  if (!t) return;
  if (wait_time > 0) t->wait_time = wait_time;
  t->running = true;
  t->elapsed_ticks = 0;
}
void cw_timer_stop(cw_node n) {
  timer *t = cw_node_data_t(n, &timer_type);
  if (!t) return;
  t->running = false;
  t->elapsed_ticks = 0;
}
bool cw_timer_is_stopped(cw_node n) {
  timer *t = cw_node_data_t(n, &timer_type);
  return !t || !t->running;
}
int64_t cw_timer_ticks_left(cw_node n) {
  timer *t = cw_node_data_t(n, &timer_type);
  if (!t || !t->running) return 0;
  int64_t left = wait_ticks(t) - t->elapsed_ticks;
  return left < 0 ? 0 : left;
}
double cw_timer_time_left(cw_node n) {
  timer *t = cw_node_data_t(n, &timer_type);
  return t ? (double)cw_timer_ticks_left(n) * t->dt : 0;
}

static void t_init(cw_node n, void *d) {
  (void)n;
  timer *t = d;
  t->wait_time = 1;
  t->dt = 1.0 / 60;
}
static void t_ready(cw_node n, void *d) {
  timer *t = d;
  if (!t->readied && t->autostart && !t->running) cw_timer_start(n, 0);
  t->readied = true;
}
static void t_fixed(cw_node n, void *d, double dt) {
  timer *t = d;
  if (dt > 0) t->dt = dt;
  if (!t->running || t->paused) return;
  if (++t->elapsed_ticks < wait_ticks(t)) return;
  if (t->one_shot) t->running = false;
  t->elapsed_ticks = 0;
  cw_emit(n, s_timeout, NULL, 0); /* last: a handler may start, stop or free the timer */
}
static void left_get(cw_node n, void *d, cw_value *o) {
  (void)d;
  *o = cw_val_float(cw_timer_time_left(n));
}
static bool m_start(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)ret, (void)user;
  double w = 0;
  if (argc >= 1) {
    if (args[0].type == CW_T_FLOAT) w = args[0].f;
    else if (args[0].type == CW_T_INT) w = (double)args[0].i;
    else return false;
  }
  cw_timer_start(self, w);
  return true;
}
static bool m_stop(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)ret, (void)user;
  cw_timer_stop(self);
  return true;
}

static const char *const timer_signals[] = { "timeout", NULL };
static const cw_prop timer_props[] = {
  { .name = "wait_time", .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = offsetof(timer, wait_time), .size = sizeof(double),
    .min = 0.001, .max = 86400, .doc = "Seconds between timeouts, counted as whole fixed ticks (at least one)." },
  CW_PROP_FIELD(timer, one_shot, CW_T_BOOL, CW_PROP_DEFAULT, "Stop after the first timeout instead of repeating."),
  CW_PROP_FIELD(timer, autostart, CW_T_BOOL, CW_PROP_DEFAULT, "Start when the node becomes ready."),
  CW_PROP_FIELD(timer, paused, CW_T_BOOL, CW_PROP_DEFAULT, "Hold the count without stopping."),
  CW_PROP_ACCESSOR("time_left", CW_T_FLOAT, CW_PROP_READONLY | CW_PROP_EDITOR, left_get, NULL, "Seconds until the next timeout; 0 when stopped."),
  CW_PROP_FIELD(timer, running, CW_T_BOOL, CW_PROP_EDITOR, "Counting now (not saved in scenes; start and stop set it)."),
  CW_PROP_FIELD(timer, elapsed_ticks, CW_T_INT, CW_PROP_EDITOR, "Fixed ticks counted since the last start or timeout (not saved in scenes)."),
  CW_PROP_END,
};
static cw_node_type timer_type = { .name = "Timer", .parent = "Node", .data_size = sizeof(timer), .props = timer_props,
                                   .signals = timer_signals,
                                   .doc = "A countdown in fixed ticks that emits timeout: the same tick on every host, in replays and under rollback.",
                                   .init = t_init, .ready = t_ready, .fixed_update = t_fixed };

void cw__register_timer_type(void);
void cw__register_timer_type(void) {
  s_timeout = cw_intern("timeout");
  cw_node_type_register(&timer_type);
  cw_node_type_add_method("Timer", "start", "...", m_start, NULL, "Start or restart the count; an optional float sets wait_time first.");
  cw_node_type_add_method("Timer", "stop", NULL, m_stop, NULL, "Stop: no timeout until started again.");
}

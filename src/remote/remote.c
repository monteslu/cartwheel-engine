/* cartwheel/remote.h: requests, replies, states and helpers shared by the
 * transports (native.c: a child process; romdev.c: a cart in romdev). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"

static char last_error[1024];
const char *cw_remote_last_error(void) { return last_error; }

#if CW_CART || defined(_WIN32)
/* A cart cannot spawn a process or open a socket; see remote.h. The
 * transports (native.c, romdev.c) are POSIX: Windows builds are exported
 * games, which are launched, not launchers. */
cw_remote *cw_remote_launch(const cw_remote_desc *d) {
  (void)d;
#if CW_CART
  snprintf(last_error, sizeof last_error,
           "cw_remote_launch: not available in a cart (no process or socket API; wasmcart's wc_peer_* needs a host that "
           "registers a peer to the game, and none does). Run the editor natively, or drive the game cart through romdev");
#else
  snprintf(last_error, sizeof last_error,
           "cw_remote_launch: not available on Windows yet (the native and romdev transports are POSIX); "
           "run the editor on Linux or macOS");
#endif
  return NULL;
}
void cw_remote_close(cw_remote *r) { (void)r; }
bool cw_remote_relaunch(cw_remote *r) { (void)r; return false; }
void cw_remote_stop(cw_remote *r) { (void)r; }
void cw_remote_kill(cw_remote *r) { (void)r; }
cw_remote_state cw_remote_state_of(cw_remote *r) { (void)r; return CW_REMOTE_FAILED; }
const char *cw_remote_state_name(cw_remote_state s) { (void)s; return "failed"; }
const char *cw_remote_status(cw_remote *r) { (void)r; return last_error; }
int cw_remote_pid(cw_remote *r) { (void)r; return 0; }
int cw_remote_launch_count(cw_remote *r) { (void)r; return 0; }
uint32_t cw_remote_send(cw_remote *r, const char *q, int t, cw_remote_cb cb, void *u) { (void)r, (void)q, (void)t, (void)cb, (void)u; return 0; }
int cw_remote_pump(cw_remote *r) { (void)r; return 0; }
bool cw_remote_wait(cw_remote *r, uint32_t id, int t) { (void)r, (void)id, (void)t; return false; }
char *cw_remote_call(cw_remote *r, const char *q, int t, char **e) { (void)r, (void)q, (void)t; if (e) *e = NULL; return NULL; }
char *cw_remote_callf(cw_remote *r, char **e, const char *fmt, ...) { (void)r, (void)fmt; if (e) *e = NULL; return NULL; }
void cw_remote_on_event(cw_remote *r, cw_remote_event_fn fn, void *u) { (void)r, (void)fn, (void)u; }
const char *cw_remote_log_tail(cw_remote *r, int n) { (void)r, (void)n; return ""; }
long long cw_remote_step(cw_remote *r, uint32_t ticks, int t, char **e) { (void)r, (void)ticks, (void)t; if (e) *e = NULL; return -1; }
bool cw_remote_screenshot(cw_remote *r, const char *l, int s, const char *p, void **o, size_t *n, int t, char **e) {
  (void)r, (void)l, (void)s, (void)p, (void)o, (void)n, (void)t;
  if (e) *e = NULL;
  return false;
}
#else

#include <unistd.h>

uint64_t cw__remote_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static char *dup_str(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *c = malloc(n + 1);
  memcpy(c, s, n + 1);
  return c;
}

static char **dup_list(const char *const *l) {
  if (!l) return NULL;
  int n = 0;
  while (l[n]) n++;
  char **c = calloc((size_t)n + 1, sizeof *c);
  for (int i = 0; i < n; i++) c[i] = dup_str(l[i]);
  return c;
}

static void free_list(char **l) {
  if (!l) return;
  for (int i = 0; l[i]; i++) free(l[i]);
  free(l);
}

static const char *state_names[] = { "starting", "running", "exited", "crashed", "failed", "closed" };
const char *cw_remote_state_name(cw_remote_state s) { return s <= CW_REMOTE_CLOSED ? state_names[s] : "?"; }

cw_remote_state cw_remote_state_of(cw_remote *r) { return r ? r->state : CW_REMOTE_FAILED; }
const char *cw_remote_status(cw_remote *r) { return r ? r->status : last_error; }
int cw_remote_pid(cw_remote *r) { return r && r->t->pid ? r->t->pid(r) : 0; }
int cw_remote_launch_count(cw_remote *r) { return r ? r->launches : 0; }

void cw_remote_on_event(cw_remote *r, cw_remote_event_fn fn, void *user) {
  r->ev_fn = fn;
  r->ev_user = user;
}

void cw__remote_event(cw_remote *r, cw_remote_event ev, const char *text) {
  if (r->ev_fn) r->ev_fn(r, ev, text, r->ev_user);
}

void cw__remote_log(cw_remote *r, const char *line) {
  int slot = (r->log_head + r->log_count) % CW__REMOTE_LOG_LINES;
  if (r->log_count == CW__REMOTE_LOG_LINES) {
    free(r->log[r->log_head]);
    r->log_head = (r->log_head + 1) % CW__REMOTE_LOG_LINES;
    slot = (r->log_head + r->log_count - 1) % CW__REMOTE_LOG_LINES;
  } else {
    r->log_count++;
  }
  r->log[slot] = dup_str(line);
  cw__remote_event(r, CW_REMOTE_EV_LOG, line);
}

const char *cw_remote_log_tail(cw_remote *r, int n) {
  free(r->log_tail);
  if (n > r->log_count) n = r->log_count;
  size_t total = 1;
  for (int i = r->log_count - n; i < r->log_count; i++) total += strlen(r->log[(r->log_head + i) % CW__REMOTE_LOG_LINES]) + 1;
  r->log_tail = malloc(total);
  r->log_tail[0] = 0;
  for (int i = r->log_count - n; i < r->log_count; i++) {
    strcat(r->log_tail, r->log[(r->log_head + i) % CW__REMOTE_LOG_LINES]);
    if (i + 1 < r->log_count) strcat(r->log_tail, "\n");
  }
  return r->log_tail;
}

void cw__remote_set_state(cw_remote *r, cw_remote_state s, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->status, sizeof r->status, fmt, ap);
  va_end(ap);
  bool changed = r->state != s;
  r->state = s;
  if (s == CW_REMOTE_EXITED || s == CW_REMOTE_CRASHED || s == CW_REMOTE_FAILED || s == CW_REMOTE_CLOSED) {
    char why[1200];
    snprintf(why, sizeof why, "the game is %s (%s) and cannot answer; cw_remote_relaunch starts it again",
             cw_remote_state_name(s), r->status);
    cw__remote_fail_all(r, why);
  }
  if (changed) cw__remote_event(r, CW_REMOTE_EV_STATE, r->status);
}

static void finish(cw_remote *r, rreq *q) {
  /* unlink from inflight, append to finished */
  for (rreq **p = &r->inflight; *p; p = &(*p)->next)
    if (*p == q) {
      *p = q->next;
      break;
    }
  q->next = NULL;
  q->done = true;
  rreq **t = &r->finished;
  while (*t) t = &(*t)->next;
  *t = q;
}

void cw__remote_complete(cw_remote *r, uint32_t id, const char *json, size_t len, const char *err) {
  rreq *q = r->inflight;
  while (q && q->id != id) q = q->next;
  if (!q) return; /* timed out already */
  if (json) {
    q->json = malloc(len + 1);
    memcpy(q->json, json, len);
    q->json[len] = 0;
    q->len = len;
    /* the channel's errors are exactly {"error":"..."} */
    if (!strncmp(q->json, "{\"error\":", 9)) {
      q->ok = false;
      q->err = cw_remote_json_string(q->json, "error");
    } else {
      q->ok = true;
    }
  } else {
    q->ok = false;
    q->err = dup_str(err ? err : "no reply");
  }
  finish(r, q);
}

void cw__remote_fail_all(cw_remote *r, const char *why) {
  while (r->inflight) cw__remote_complete(r, r->inflight->id, NULL, 0, why);
}

uint32_t cw_remote_send(cw_remote *r, const char *request, int timeout_ms, cw_remote_cb cb, void *user) {
  if (!r || (r->state != CW_REMOTE_RUNNING && r->state != CW_REMOTE_STARTING)) return 0;
  rreq *q = calloc(1, sizeof *q);
  q->id = ++r->next_id;
  if (!q->id) q->id = ++r->next_id;
  q->text = dup_str(request);
  q->deadline = cw__remote_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : r->d.timeout_ms);
  q->cb = cb;
  q->user = user;
  rreq **t = &r->inflight;
  while (*t) t = &(*t)->next;
  *t = q;
  r->t->send(r, q);
  return q->id;
}

static void check_timeouts(cw_remote *r) {
  uint64_t now = cw__remote_ms();
  for (rreq *q = r->inflight, *next; q; q = next) {
    next = q->next;
    if (now < q->deadline) continue;
    char why[600];
    snprintf(why, sizeof why,
             "timed out waiting for `%.200s` (the game is %s: busy, stuck, or the request needs a longer timeout; "
             "cw_remote_status and its log tail say more)", q->text, cw_remote_state_name(r->state));
    cw__remote_complete(r, q->id, NULL, 0, why);
  }
}

static void free_req(rreq *q) {
  free(q->text);
  free(q->json);
  free(q->err);
  free(q);
}

static int deliver(cw_remote *r) {
  int n = 0;
  while (r->finished) {
    rreq *q = r->finished;
    r->finished = q->next;
    if (q->cb) {
      cw_remote_reply rep = { q->id, q->ok, q->json, q->len, q->err };
      q->cb(r, &rep, q->user);
    }
    free_req(q);
    n++;
  }
  return n;
}

int cw_remote_pump(cw_remote *r) {
  if (!r) return 0;
  r->t->poll(r);
  check_timeouts(r);
  return deliver(r);
}

static bool is_done(cw_remote *r, uint32_t id) {
  for (rreq *q = r->inflight; q; q = q->next)
    if (q->id == id) return false;
  return true;
}

bool cw_remote_wait(cw_remote *r, uint32_t id, int timeout_ms) {
  if (!r || !id) return false;
  uint64_t end = cw__remote_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : r->d.timeout_ms);
  for (;;) {
    r->t->poll(r);
    check_timeouts(r);
    if (is_done(r, id)) {
      deliver(r);
      return true;
    }
    if (cw__remote_ms() >= end) return false;
    usleep(500);
  }
}

typedef struct { bool got; char *json, *err; } call_box;
static void call_cb(cw_remote *r, const cw_remote_reply *rep, void *user) {
  (void)r;
  call_box *b = user;
  b->got = true;
  if (rep->ok) b->json = dup_str(rep->json);
  else b->err = dup_str(rep->error ? rep->error : "failed");
}

char *cw_remote_call(cw_remote *r, const char *request, int timeout_ms, char **error_out) {
  if (error_out) *error_out = NULL;
  call_box b = { 0 };
  uint32_t id = cw_remote_send(r, request, timeout_ms, call_cb, &b);
  if (!id) {
    char why[1400];
    snprintf(why, sizeof why, "cannot send `%.200s`: the game is %s (%.1000s); cw_remote_relaunch starts it again", request,
             cw_remote_state_name(cw_remote_state_of(r)), cw_remote_status(r));
    if (error_out) *error_out = dup_str(why);
    return NULL;
  }
  cw_remote_wait(r, id, (timeout_ms > 0 ? timeout_ms : r->d.timeout_ms) + 1000);
  if (!b.got) b.err = dup_str("no reply (the wait ended before the request timed out)");
  if (b.err) {
    if (error_out) *error_out = b.err;
    else free(b.err);
    return NULL;
  }
  return b.json;
}

char *cw_remote_callf(cw_remote *r, char **error_out, const char *fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return cw_remote_call(r, buf, 0, error_out);
}

/* ── launching ──────────────────────────────────────────────────────── */

static bool start(cw_remote *r) {
  r->launches++;
  r->next_id = 0;
  r->state = CW_REMOTE_STARTING;
  snprintf(r->status, sizeof r->status, "starting");
  cw__remote_event(r, CW_REMOTE_EV_STATE, r->status);
  if (!r->t->start(r)) return false;
  /* the first answer: the game is up. A start-paused game is RUNNING once
   * the debugger's pause holds (after frame 1 on both transports). */
  uint64_t end = cw__remote_ms() + (uint64_t)r->d.launch_timeout_ms;
  bool first = true;
  while (cw__remote_ms() < end) {
    char *err = NULL;
    int left = (int)(end - cw__remote_ms());
    const char *q = r->d.start_paused && r->d.transport == CW_REMOTE_ROMDEV && first ? "pause" : "status";
    char *rep = cw_remote_call(r, q, left > 0 ? left : 1, &err);
    first = false;
    if (!rep) {
      if (r->state != CW_REMOTE_STARTING) { /* it died on the way up */
        free(err);
        return false;
      }
      cw__remote_set_state(r, CW_REMOTE_FAILED, "the game did not answer within %d ms (%s)%s%s", r->d.launch_timeout_ms,
                           err ? err : "?", r->log_count ? "; its last output: " : "", r->log_count ? cw_remote_log_tail(r, 3) : "");
      free(err);
      r->t->stop(r, true);
      return false;
    }
    bool paused = false;
    cw_remote_json_bool(rep, "paused", &paused);
    free(rep);
    if (!r->d.start_paused || paused) {
      int pid = cw_remote_pid(r);
      if (pid) cw__remote_set_state(r, CW_REMOTE_RUNNING, "running (pid %d)", pid);
      else cw__remote_set_state(r, CW_REMOTE_RUNNING, "running in romdev (session %s)", r->session);
      return true;
    }
    usleep(5000);
  }
  cw__remote_set_state(r, CW_REMOTE_FAILED, "the game did not pause within %d ms", r->d.launch_timeout_ms);
  r->t->stop(r, true);
  return false;
}

cw_remote *cw_remote_launch(const cw_remote_desc *d) {
  last_error[0] = 0;
  if (!d) {
    snprintf(last_error, sizeof last_error, "cw_remote_launch: pass a cw_remote_desc");
    return NULL;
  }
  if (d->transport == CW_REMOTE_NATIVE && (!d->exe || !*d->exe)) {
    snprintf(last_error, sizeof last_error, "cw_remote_launch: a native launch needs .exe (the game executable's path)");
    return NULL;
  }
  if (d->transport == CW_REMOTE_ROMDEV && (!d->cart || !*d->cart)) {
    snprintf(last_error, sizeof last_error, "cw_remote_launch: a romdev launch needs .cart (the .wasc path)");
    return NULL;
  }
  cw_remote *r = calloc(1, sizeof *r);
  r->d = *d;
  /* absolute: romdev resolves paths against its own directory, and a child
   * with .cwd set resolves against that */
  r->exe = d->exe ? realpath(d->exe, NULL) : NULL;
  if (d->exe && !r->exe) r->exe = dup_str(d->exe);
  r->cwd = dup_str(d->cwd);
  r->cart = d->cart ? realpath(d->cart, NULL) : NULL;
  if (d->cart && !r->cart) r->cart = dup_str(d->cart);
  r->args = dup_list(d->args);
  r->env = dup_list(d->env);
  r->d.exe = r->exe;
  r->d.cwd = r->cwd;
  r->d.cart = r->cart;
  r->d.args = (const char *const *)r->args;
  r->d.env = (const char *const *)r->env;
  if (r->d.timeout_ms <= 0) r->d.timeout_ms = 10000;
  if (r->d.launch_timeout_ms <= 0) r->d.launch_timeout_ms = 20000;
  if (!r->d.seed) r->d.seed = 1;
  if (d->transport == CW_REMOTE_ROMDEV) {
    if (d->romdev_url && *d->romdev_url) r->url = dup_str(d->romdev_url);
    else if (getenv("ROMDEV_URL") && *getenv("ROMDEV_URL")) r->url = dup_str(getenv("ROMDEV_URL"));
    else {
      char u[128];
      snprintf(u, sizeof u, "http://127.0.0.1:%s", getenv("ROMDEV_PORT") && *getenv("ROMDEV_PORT") ? getenv("ROMDEV_PORT") : "7331");
      r->url = dup_str(u);
    }
    if (d->session && *d->session) r->session = dup_str(d->session);
    else {
      char s[96];
      snprintf(s, sizeof s, "cw-remote-%d-%llu", (int)getpid(), (unsigned long long)cw__remote_ms());
      r->session = dup_str(s);
    }
    r->d.romdev_url = r->url;
    r->d.session = r->session;
    r->t = &cw__remote_romdev;
  } else {
    r->t = &cw__remote_native;
  }
  start(r);
  return r;
}

void cw_remote_stop(cw_remote *r) {
  if (!r) return;
  if (r->state == CW_REMOTE_RUNNING || r->state == CW_REMOTE_STARTING) {
    r->t->stop(r, false);
    cw__remote_set_state(r, CW_REMOTE_CLOSED, "stopped by the editor");
  }
}

void cw_remote_kill(cw_remote *r) {
  if (!r) return;
  if (r->state == CW_REMOTE_RUNNING || r->state == CW_REMOTE_STARTING) {
    r->t->stop(r, true);
    cw__remote_set_state(r, CW_REMOTE_CRASHED, "killed by the editor (cw_remote_kill)");
  }
}

bool cw_remote_relaunch(cw_remote *r) {
  if (!r) return false;
  cw_remote_stop(r);
  r->t->poll(r); /* reap */
  deliver(r);
  return start(r);
}

void cw_remote_close(cw_remote *r) {
  if (!r) return;
  cw_remote_stop(r);
  deliver(r);
  r->t->destroy(r);
  for (int i = 0; i < r->log_count; i++) free(r->log[(r->log_head + i) % CW__REMOTE_LOG_LINES]);
  free(r->log_tail);
  free(r->exe);
  free(r->cwd);
  free(r->cart);
  free(r->url);
  free(r->session);
  free_list(r->args);
  free_list(r->env);
  free(r);
}

/* ── helpers ────────────────────────────────────────────────────────── */

long long cw_remote_step(cw_remote *r, uint32_t ticks, int timeout_ms, char **error_out) {
  if (error_out) *error_out = NULL;
  long long tick = -1;
  /* romdev runs one command for at most 600 frames */
  const uint32_t chunk_max = 500;
  while (ticks) {
    uint32_t n = ticks < chunk_max ? ticks : chunk_max;
    char req[64];
    snprintf(req, sizeof req, "step %u", n);
    /* a native game steps as fast as it can; leave room for slow frames */
    int t = timeout_ms > 0 ? timeout_ms : r->d.timeout_ms + (int)n * 50;
    char *rep = cw_remote_call(r, req, t, error_out);
    if (!rep) return -1;
    double v = -1;
    cw_remote_json_number(rep, "tick", &v);
    free(rep);
    tick = (long long)v;
    ticks -= n;
  }
  return tick;
}

static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* appends the decoded bytes; false on bad input */
static bool b64_append(const char *s, unsigned char **buf, size_t *len, size_t *cap) {
  unsigned v = 0;
  int bits = 0;
  for (; *s; s++) {
    if (*s == '=') break;
    int d = b64v((unsigned char)*s);
    if (d < 0) return false;
    v = (v << 6) | (unsigned)d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (*len + 1 > *cap) {
        *cap = *cap ? *cap * 2 : 65536;
        *buf = realloc(*buf, *cap);
      }
      (*buf)[(*len)++] = (unsigned char)(v >> bits);
      v &= (1u << bits) - 1;
    }
  }
  return true;
}

bool cw_remote_screenshot(cw_remote *r, const char *layers, int scale, const char *png_path, void **png_out,
                          size_t *png_len, int timeout_ms, char **error_out) {
  if (error_out) *error_out = NULL;
  if (png_out) *png_out = NULL;
  if (png_len) *png_len = 0;
  char req[128];
  snprintf(req, sizeof req, "screenshot %s %d", layers && *layers ? layers : "all", scale > 0 ? scale : 1);
  char *rep = cw_remote_call(r, req, timeout_ms, error_out);
  if (!rep) return false;
  double id = 0, total = 0;
  cw_remote_json_number(rep, "id", &id);
  cw_remote_json_number(rep, "png_bytes", &total);
  free(rep);
  unsigned char *buf = NULL;
  size_t len = 0, cap = 0;
  while (len < (size_t)total) {
    char q[96];
    snprintf(q, sizeof q, "screenshot_get %u %zu", (unsigned)id, len);
    char *part = cw_remote_call(r, q, timeout_ms, error_out);
    if (!part) {
      free(buf);
      return false;
    }
    char *b64 = cw_remote_json_string(part, "base64");
    size_t before = len;
    bool ok = b64 && b64_append(b64, &buf, &len, &cap);
    free(b64);
    free(part);
    if (!ok || len == before) {
      if (error_out) *error_out = dup_str("screenshot_get returned no or bad base64");
      free(buf);
      return false;
    }
  }
  if (png_path) {
    FILE *f = fopen(png_path, "wb");
    if (!f || fwrite(buf, 1, len, f) != len) {
      if (f) fclose(f);
      if (error_out) {
        char why[1200];
        snprintf(why, sizeof why, "cannot write %s", png_path);
        *error_out = dup_str(why);
      }
      free(buf);
      return false;
    }
    fclose(f);
  }
  if (png_len) *png_len = len;
  if (png_out) *png_out = buf;
  else free(buf);
  return true;
}
#endif /* !CW_CART && !_WIN32 */

/* ── JSON readers (both builds) ─────────────────────────────────────── */

static const char *skip_string(const char *p) { /* p at the opening quote; returns past the closing one */
  for (p++; *p && *p != '"'; p++)
    if (*p == '\\' && p[1]) p++;
  return *p ? p + 1 : p;
}

static const char *skip_value(const char *p) {
  while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
  if (*p == '"') return skip_string(p);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; p++) {
      if (*p == '"') {
        p = skip_string(p) - 1;
        continue;
      }
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') {
        if (--depth == 0) return p + 1;
      }
    }
    return p;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n') p++;
  return p;
}

bool cw__json_find(const char *json, const char *key, const char **start, const char **end) {
  if (!json) return false;
  size_t kl = strlen(key);
  for (const char *p = json; *p;) {
    if (*p != '"') {
      p++;
      continue;
    }
    const char *s = p + 1, *e = skip_string(p);
    p = e;
    if ((size_t)(e - 1 - s) != kl || strncmp(s, key, kl)) continue;
    const char *q = e;
    while (*q == ' ') q++;
    if (*q != ':') continue; /* a string value, not a key */
    q++;
    while (*q == ' ' || *q == '\n' || *q == '\t') q++;
    *start = q;
    *end = skip_value(q);
    return true;
  }
  return false;
}

bool cw_remote_json_number(const char *json, const char *key, double *out) {
  const char *s, *e;
  if (!cw__json_find(json, key, &s, &e)) return false;
  char *stop;
  double v = strtod(s, &stop);
  if (stop == s) return false;
  *out = v;
  return true;
}

bool cw_remote_json_bool(const char *json, const char *key, bool *out) {
  const char *s, *e;
  if (!cw__json_find(json, key, &s, &e)) return false;
  if (!strncmp(s, "true", 4)) *out = true;
  else if (!strncmp(s, "false", 5)) *out = false;
  else return false;
  return true;
}

char *cw_remote_json_string(const char *json, const char *key) {
  const char *s, *e;
  if (!cw__json_find(json, key, &s, &e) || *s != '"') return NULL;
  char *out = malloc((size_t)(e - s) + 1), *o = out;
  for (const char *p = s + 1; p < e - 1; p++) {
    if (*p == '\\' && p + 1 < e - 1) {
      p++;
      switch (*p) {
      case 'n': *o++ = '\n'; break;
      case 't': *o++ = '\t'; break;
      case 'r': *o++ = '\r'; break;
      case 'u': { /* \u00XX (the channel escapes control bytes this way) */
        unsigned v = 0;
        for (int k = 0; k < 4 && p[1]; k++) {
          char c = *++p;
          v = v * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
        }
        if (v < 0x80) *o++ = (char)v;
        else if (v < 0x800) { *o++ = (char)(0xc0 | v >> 6); *o++ = (char)(0x80 | (v & 0x3f)); }
        else { *o++ = (char)(0xe0 | v >> 12); *o++ = (char)(0x80 | ((v >> 6) & 0x3f)); *o++ = (char)(0x80 | (v & 0x3f)); }
        break;
      }
      default: *o++ = *p; break;
      }
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
  return out;
}

char *cw__json_escape(const char *s) {
  size_t n = strlen(s);
  char *out = malloc(n * 6 + 3), *o = out;
  *o++ = '"';
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = (char)c; }
    else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
    else if (c < 32) o += sprintf(o, "\\u%04x", c);
    else *o++ = (char)c;
  }
  *o++ = '"';
  *o = 0;
  return out;
}

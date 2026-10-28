/* cartwheel/remote.h, romdev transport: the game's cart in romdev, driven
 * over romdev's plain HTTP tool route (POST /tool/<name>, one session key per
 * cw_remote). A worker thread owns the HTTP traffic so the editor's frame
 * loop never blocks on it: it runs queued requests (wasm({op:'command'})) in
 * order and, while the game is not paused, steps frames to keep 60 per
 * wall-clock second (romdev only runs a cart when told). */
#include "internal.h"

#if !CW_CART && !defined(_WIN32)
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct job {
  uint32_t id;
  char *text;
  struct job *next;
} job;

typedef struct done_item {
  uint32_t id;
  char *json; /* the game's reply, or NULL */
  char *err;
  int verdict; /* 0 reply/request error, 1 crashed, 2 exited, 3 failed */
  struct done_item *next;
} done_item;

typedef struct {
  pthread_t thread;
  bool thread_on;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  job *jobs;
  done_item *done;
  bool quit;
  bool paused;   /* the game's debugger pause, as the last reply said */
  bool dead;     /* stop stepping: crashed / exited */
  char host[256], port[16];
  uint64_t last_step_ms;
  long long frames_run;
} rom;

/* ── a minimal HTTP/1.1 client (Connection: close) ─────────────────── */

static char *http_post(rom *s, const char *path, const char *session, const char *body, int *status, char *err, size_t errcap) {
  *status = 0;
  struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai = NULL;
  if (getaddrinfo(s->host, s->port, &hints, &ai) || !ai) {
    snprintf(err, errcap, "cannot resolve %s", s->host);
    return NULL;
  }
  int fd = -1;
  for (struct addrinfo *a = ai; a; a = a->ai_next) {
    fd = socket(a->ai_family, SOCK_STREAM, 0);
    if (fd < 0) continue;
    if (!connect(fd, a->ai_addr, a->ai_addrlen)) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(ai);
  if (fd < 0) {
    snprintf(err, errcap, "romdev is not reachable at %s:%s (%s): is the server running? (ss -tlnp | grep %s)", s->host,
             s->port, strerror(errno), s->port);
    return NULL;
  }
  size_t bl = strlen(body);
  char *req = malloc(bl + 1024);
  int hl = snprintf(req, 1024,
                    "POST %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\nx-romdev-session: %s\r\n"
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                    path, s->host, s->port, session, bl);
  memcpy(req + hl, body, bl);
  size_t total = (size_t)hl + bl, off = 0;
  while (off < total) {
    ssize_t w = send(fd, req + off, total - off, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) break;
    off += (size_t)w;
  }
  free(req);
  size_t cap = 65536, len = 0;
  char *resp = malloc(cap);
  for (;;) {
    if (len + 65536 > cap) {
      cap *= 2;
      resp = realloc(resp, cap);
    }
    ssize_t n = recv(fd, resp + len, cap - len - 1, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    len += (size_t)n;
  }
  close(fd);
  resp[len] = 0;
  char *hdr_end = strstr(resp, "\r\n\r\n");
  if (strncmp(resp, "HTTP/1.", 7) || !hdr_end) {
    snprintf(err, errcap, "romdev answered something that is not HTTP (%zu bytes)", len);
    free(resp);
    return NULL;
  }
  *status = atoi(resp + 9);
  char *bodyp = hdr_end + 4;
  /* chunked transfer: join the chunks */
  bool chunked = false;
  for (char *h = resp; h < hdr_end; h++)
    if ((*h == 'T' || *h == 't') && !strncasecmp(h, "transfer-encoding: chunked", 26)) chunked = true;
  char *out;
  if (chunked) {
    out = malloc(len + 1);
    size_t o = 0;
    char *p = bodyp;
    for (;;) {
      char *e;
      unsigned long n = strtoul(p, &e, 16);
      char *nl = strstr(p, "\r\n");
      if (!nl || n == 0) break;
      p = nl + 2;
      if (p + n > resp + len) n = (unsigned long)(resp + len - p);
      memcpy(out + o, p, n);
      o += n;
      p += n + 2;
    }
    out[o] = 0;
  } else {
    size_t n = len - (size_t)(bodyp - resp);
    out = malloc(n + 1);
    memcpy(out, bodyp, n);
    out[n] = 0;
  }
  free(resp);
  return out;
}

/* one tool call. Returns the body (malloc'd) or NULL with err set. A tool
 * error (HTTP 4xx/5xx) comes back as NULL with romdev's message in err. */
static char *tool(rom *s, cw_remote *r, const char *name, const char *args, char *err, size_t errcap) {
  char path[128];
  snprintf(path, sizeof path, "/tool/%s", name);
  int status;
  char *body = http_post(s, path, r->session, args, &status, err, errcap);
  if (!body) return NULL;
  if (status >= 300) {
    char *msg = cw_remote_json_string(body, "error");
    snprintf(err, errcap, "%s", msg ? msg : body);
    free(msg);
    free(body);
    return NULL;
  }
  /* MCP-shaped results carry the payload as text content */
  const char *a, *b;
  if (!strncmp(body, "{\"content\":", 11) && cw__json_find(body, "text", &a, &b)) {
    char *t = cw_remote_json_string(body, "text");
    free(body);
    body = t;
  }
  bool is_err = false;
  if (body && cw_remote_json_bool(body, "isError", &is_err) && is_err) {
    snprintf(err, errcap, "%s", body);
    free(body);
    return NULL;
  }
  return body;
}

static void push_done(rom *s, uint32_t id, char *json, char *err, int verdict) {
  done_item *d = calloc(1, sizeof *d);
  d->id = id;
  d->json = json;
  d->err = err;
  d->verdict = verdict;
  pthread_mutex_lock(&s->mu);
  done_item **t = &s->done;
  while (*t) t = &(*t)->next;
  *t = d;
  pthread_mutex_unlock(&s->mu);
}

static char *dupz(const char *x) {
  size_t n = strlen(x);
  char *c = malloc(n + 1);
  memcpy(c, x, n + 1);
  return c;
}

/* what an error from romdev means for the game */
static int classify(const char *err) {
  if (strstr(err, "No ROM loaded")) return 2;                                          /* host evicted / shut down */
  if (strstr(err, "no debug channel")) return 3;                                       /* not a cartwheel debug cart */
  if (!strncmp(err, "wasm({op:'command'})", 20) || strstr(err, "romdev is not reachable")) return 0; /* request-level */
  return 1; /* a trap ("unreachable", "memory access out of bounds", ...) */
}

static void run_command(rom *s, cw_remote *r, job *j) {
  char *text = cw__json_escape(j->text);
  char *args = malloc(strlen(text) + 96);
  sprintf(args, "{\"op\":\"command\",\"text\":%s,\"maxFrames\":600}", text);
  free(text);
  char err[2048];
  char *body = tool(s, r, "wasm", args, err, sizeof err);
  free(args);
  if (!body) {
    int v = classify(err);
    if (v) {
      pthread_mutex_lock(&s->mu);
      s->dead = true;
      pthread_mutex_unlock(&s->mu);
    }
    push_done(s, j->id, NULL, dupz(err), v);
    return;
  }
  const char *a, *b;
  double fr = 0;
  cw_remote_json_number(body, "framesRun", &fr);
  s->frames_run += (long long)fr;
  if (!cw__json_find(body, "reply", &a, &b)) {
    push_done(s, j->id, NULL, dupz("romdev's command result has no reply field"), 0);
    free(body);
    return;
  }
  char *reply;
  if (*a == '"') { /* the cart answered something that is not JSON: pass it on as text */
    reply = cw_remote_json_string(body, "reply");
  } else {
    reply = malloc((size_t)(b - a) + 1);
    memcpy(reply, a, (size_t)(b - a));
    reply[b - a] = 0;
  }
  free(body);
  /* follow the debugger's pause: frames flow only while it is off */
  bool paused;
  if (!strncmp(reply, "{\"frame\":", 9) && cw_remote_json_bool(reply, "paused", &paused)) {
    pthread_mutex_lock(&s->mu);
    s->paused = paused;
    pthread_mutex_unlock(&s->mu);
  }
  push_done(s, j->id, reply, NULL, 0);
}

static void *worker(void *u) {
  cw_remote *r = u;
  rom *s = r->tp;
  pthread_mutex_lock(&s->mu);
  for (;;) {
    while (!s->quit && !s->jobs && (s->paused || s->dead)) pthread_cond_wait(&s->cv, &s->mu);
    if (s->quit) break;
    job *j = s->jobs;
    if (j) {
      s->jobs = j->next;
      pthread_mutex_unlock(&s->mu);
      run_command(s, r, j);
      free(j->text);
      free(j);
      pthread_mutex_lock(&s->mu);
      s->last_step_ms = cw__remote_ms(); /* a command ran a frame (or more) */
      continue;
    }
    /* the game runs: catch up with the wall clock, at most 6 frames a call */
    uint64_t now = cw__remote_ms();
    if (!s->last_step_ms) s->last_step_ms = now;
    int owed = (int)((now - s->last_step_ms) * 60 / 1000);
    if (owed < 1) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 4 * 1000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&s->cv, &s->mu, &ts);
      continue;
    }
    if (owed > 6) owed = 6;
    s->last_step_ms += (uint64_t)owed * 1000 / 60;
    if (now - s->last_step_ms > 250) s->last_step_ms = now; /* behind: drop it, as the engine does */
    pthread_mutex_unlock(&s->mu);
    char args[96], err[2048];
    snprintf(args, sizeof args, "{\"op\":\"step\",\"frames\":%d}", owed);
    char *body = tool(s, r, "frame", args, err, sizeof err);
    if (body) {
      s->frames_run += owed;
      free(body);
      pthread_mutex_lock(&s->mu);
    } else {
      int v = classify(err);
      pthread_mutex_lock(&s->mu);
      if (v) {
        s->dead = true;
        pthread_mutex_unlock(&s->mu);
        push_done(s, 0, NULL, dupz(err), v); /* a state change, no request */
        pthread_mutex_lock(&s->mu);
      }
    }
  }
  pthread_mutex_unlock(&s->mu);
  return NULL;
}

static bool parse_url(cw_remote *r, rom *s) {
  const char *u = r->url;
  if (strncmp(u, "http://", 7)) {
    cw__remote_set_state(r, CW_REMOTE_FAILED, "romdev_url %s: only http://host:port is supported", u);
    return false;
  }
  u += 7;
  const char *colon = strchr(u, ':'), *slash = strchr(u, '/');
  size_t hl = colon && (!slash || colon < slash) ? (size_t)(colon - u) : slash ? (size_t)(slash - u) : strlen(u);
  snprintf(s->host, sizeof s->host, "%.*s", (int)hl, u);
  if (colon && (!slash || colon < slash)) snprintf(s->port, sizeof s->port, "%.*s", (int)strcspn(colon + 1, "/"), colon + 1);
  else snprintf(s->port, sizeof s->port, "80");
  return true;
}

static void stop_worker(rom *s) {
  if (!s->thread_on) return;
  pthread_mutex_lock(&s->mu);
  s->quit = true;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
  pthread_join(s->thread, NULL);
  s->thread_on = false;
  while (s->jobs) {
    job *j = s->jobs;
    s->jobs = j->next;
    free(j->text);
    free(j);
  }
  while (s->done) {
    done_item *d = s->done;
    s->done = d->next;
    free(d->json);
    free(d->err);
    free(d);
  }
}

static bool r_start(cw_remote *r) {
  rom *s = r->tp;
  if (!s) {
    s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    r->tp = s;
  }
  stop_worker(s);
  s->quit = s->dead = false;
  s->paused = r->d.start_paused; /* no frames before the launch's pause lands */
  s->last_step_ms = 0;
  s->frames_run = 0;
  if (!parse_url(r, s)) return false;
  if (access(r->d.cart, R_OK)) {
    cw__remote_set_state(r, CW_REMOTE_FAILED, "%s is not readable (%s): build the cart first (scripts/build.sh)", r->d.cart,
                         strerror(errno));
    return false;
  }
  char *path = cw__json_escape(r->d.cart);
  char *args = malloc(strlen(path) + 128);
  /* the game is the dual GL + WebGPU cart (the only kind): romdev runs it on GL, as it ran the GL cart */
  sprintf(args, "{\"platform\":\"wasmcart\",\"path\":%s,\"wgpu\":false,\"deterministicSeed\":%u}", path, r->d.seed);
  free(path);
  char err[2048];
  char *body = tool(s, r, "loadMedia", args, err, sizeof err);
  free(args);
  if (!body) {
    cw__remote_set_state(r, CW_REMOTE_FAILED, "romdev loadMedia failed: %s", err);
    return false;
  }
  free(body);
  s->quit = false;
  pthread_create(&s->thread, NULL, worker, r);
  s->thread_on = true;
  return true;
}

static void r_send(cw_remote *r, rreq *q) {
  rom *s = r->tp;
  job *j = calloc(1, sizeof *j);
  j->id = q->id;
  j->text = dupz(q->text);
  pthread_mutex_lock(&s->mu);
  job **t = &s->jobs;
  while (*t) t = &(*t)->next;
  *t = j;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
}

static void r_poll(cw_remote *r) {
  rom *s = r->tp;
  if (!s) return;
  pthread_mutex_lock(&s->mu);
  done_item *list = s->done;
  s->done = NULL;
  pthread_mutex_unlock(&s->mu);
  while (list) {
    done_item *d = list;
    list = d->next;
    bool live = r->state == CW_REMOTE_RUNNING || r->state == CW_REMOTE_STARTING;
    if (d->verdict && live) {
      if (d->verdict == 1) cw__remote_set_state(r, CW_REMOTE_CRASHED, "crashed in romdev: %s", d->err);
      else if (d->verdict == 2) cw__remote_set_state(r, CW_REMOTE_EXITED, "romdev no longer has the game (host evicted or shut down)");
      else cw__remote_set_state(r, CW_REMOTE_FAILED, "romdev: %s", d->err);
    } else if (d->id) {
      cw__remote_complete(r, d->id, d->json, d->json ? strlen(d->json) : 0, d->err);
    }
    free(d->json);
    free(d->err);
    free(d);
  }
}

static void r_stop(cw_remote *r, bool kill_now) {
  (void)kill_now; /* romdev has one way to end a game: shut the session's host down */
  rom *s = r->tp;
  if (!s) return;
  stop_worker(s);
  char err[1024];
  char *body = tool(s, r, "host", "{\"op\":\"shutdown\"}", err, sizeof err);
  free(body);
}

static void r_destroy(cw_remote *r) {
  rom *s = r->tp;
  if (!s) return;
  stop_worker(s);
  pthread_mutex_destroy(&s->mu);
  pthread_cond_destroy(&s->cv);
  free(s);
  r->tp = NULL;
}

const rtransport cw__remote_romdev = { "romdev", r_start, r_send, r_poll, r_stop, r_destroy, NULL };

#elif !CW_CART
static bool w_start(cw_remote *r) {
  cw__remote_set_state(r, CW_REMOTE_FAILED, "the romdev transport is not implemented on Windows yet");
  return false;
}
static void w_send(cw_remote *r, rreq *q) { (void)r, (void)q; }
static void w_poll(cw_remote *r) { (void)r; }
static void w_stop(cw_remote *r, bool k) { (void)r, (void)k; }
static void w_destroy(cw_remote *r) { (void)r; }
const rtransport cw__remote_romdev = { "romdev", w_start, w_send, w_poll, w_stop, w_destroy, NULL };
#endif

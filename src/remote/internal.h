/* cartwheel/remote.h internals: the request table and the transport seam. */
#ifndef CW_REMOTE_INTERNAL_H
#define CW_REMOTE_INTERNAL_H

#include <stdarg.h>

#include "cartwheel/remote.h"

typedef struct rreq {
  uint32_t id;
  char *text;
  uint64_t deadline; /* ms, monotonic */
  cw_remote_cb cb;
  void *user;
  /* completion */
  bool done, ok;
  char *json; /* the game's reply */
  size_t len;
  char *err;
  struct rreq *next;
} rreq;

typedef struct rtransport {
  const char *name;
  /* start the game; on failure set FAILED with the reason and return false */
  bool (*start)(cw_remote *r);
  /* hand a request to the game (it completes later through cw__remote_complete) */
  void (*send)(cw_remote *r, rreq *q);
  /* read what arrived; detect exits */
  void (*poll)(cw_remote *r);
  /* end the game: kill = no quit request first */
  void (*stop)(cw_remote *r, bool kill);
  void (*destroy)(cw_remote *r);
  int (*pid)(cw_remote *r);
} rtransport;

#define CW__REMOTE_LOG_LINES 256

struct cw_remote {
  cw_remote_desc d;
  /* owned copies of d's strings */
  char *exe, *cwd, *cart, *url, *session;
  char **args, **env;
  const rtransport *t;
  void *tp; /* the transport's state */
  cw_remote_state state;
  char status[1024];
  uint32_t next_id;
  rreq *inflight; /* sent, not done (oldest first) */
  rreq *finished; /* done, not yet delivered by pump */
  cw_remote_event_fn ev_fn;
  void *ev_user;
  char *log[CW__REMOTE_LOG_LINES];
  int log_head, log_count;
  char *log_tail;
  int launches;
};

uint64_t cw__remote_ms(void);
/* a reply (or a transport failure: json NULL, err set) for request id */
void cw__remote_complete(cw_remote *r, uint32_t id, const char *json, size_t len, const char *err);
void cw__remote_set_state(cw_remote *r, cw_remote_state s, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void cw__remote_log(cw_remote *r, const char *line);
void cw__remote_event(cw_remote *r, cw_remote_event ev, const char *text);
/* fail everything in flight (the game is gone) */
void cw__remote_fail_all(cw_remote *r, const char *why);
/* the span [*start, *end) of key's value in json (first match, any depth) */
bool cw__json_find(const char *json, const char *key, const char **start, const char **end);
char *cw__json_escape(const char *s); /* "..." with quotes, malloc'd */

extern const rtransport cw__remote_native;
extern const rtransport cw__remote_romdev;

#endif

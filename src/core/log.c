#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

#include "cartwheel/base.h"

void cw__platform_log(cw_log_level level, const char *msg); /* native.c / cart.c */

#define MAX_LISTENERS 8
/* busy: calls in flight per slot, so removal can wait for them to finish
 * (seq_cst on both sides: a logger either sees the slot cleared or the
 * remover sees its busy count) */
static struct { cw_log_listener fn; void *user; int busy; } listeners[MAX_LISTENERS];
static _Thread_local int in_listener; /* this thread is inside a listener call */

int cw_log_add_listener(cw_log_listener fn, void *user) {
  for (int i = 0; i < MAX_LISTENERS; i++)
    if (!listeners[i].fn) {
      listeners[i].user = user;
      __atomic_store_n(&listeners[i].fn, fn, __ATOMIC_RELEASE);
      return i;
    }
  return -1;
}

void cw_log_remove_listener(int id) {
  if (id < 0 || id >= MAX_LISTENERS) return;
  __atomic_store_n(&listeners[id].fn, (cw_log_listener)NULL, __ATOMIC_SEQ_CST);
  /* wait for calls already running on other threads, so the caller may free
   * what the listener uses once this returns. Not from inside a listener on
   * this thread (that call is one of them: waiting would never end). */
  if (in_listener) return;
  while (__atomic_load_n(&listeners[id].busy, __ATOMIC_SEQ_CST)) sched_yield();
}

void cw_log(cw_log_level level, const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  cw__platform_log(level, msg);
  for (int i = 0; i < MAX_LISTENERS; i++) {
    if (!__atomic_load_n(&listeners[i].fn, __ATOMIC_RELAXED)) continue;
    __atomic_add_fetch(&listeners[i].busy, 1, __ATOMIC_SEQ_CST);
    cw_log_listener fn = __atomic_load_n(&listeners[i].fn, __ATOMIC_SEQ_CST);
    if (fn) {
      in_listener++;
      fn(level, msg, listeners[i].user);
      in_listener--;
    }
    __atomic_sub_fetch(&listeners[i].busy, 1, __ATOMIC_SEQ_CST);
  }
}

void cw_fatal(const char *file, int line, const char *fmt, ...) {
  char msg[1024];
  int n = snprintf(msg, sizeof msg, "%s:%d: ", file, line);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
  va_end(ap);
  cw__platform_log(CW_LOG_ERROR, msg);
  abort();
}

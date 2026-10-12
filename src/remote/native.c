/* cartwheel/remote.h, native transport: the game as a child process with a
 * socketpair carrying the debug channel (CW_REMOTE=fd:N on the child's side,
 * src/debug/channel.c) and a pipe carrying its stdout and stderr. POSIX. */
#include "internal.h"

#if !CW_CART && !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

extern char **environ;

typedef struct {
  pid_t pid;
  int sock, out;
  bool sock_eof;
  unsigned char *in;
  size_t in_len, in_cap;
  unsigned char *outq; /* frames the socket did not take yet */
  size_t outq_len, outq_cap;
  char line[4096];
  size_t line_len;
} nat;

static void put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}
static uint32_t get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void append(unsigned char **buf, size_t *len, size_t *cap, const void *data, size_t n) {
  if (*len + n > *cap) {
    *cap = (*len + n) * 2 + 4096;
    *buf = realloc(*buf, *cap);
  }
  memcpy(*buf + *len, data, n);
  *len += n;
}

static void close_fds(nat *s) {
  if (s->sock >= 0) close(s->sock);
  if (s->out >= 0) close(s->out);
  s->sock = s->out = -1;
}

static const char *signame(int sig) {
  switch (sig) {
  case SIGSEGV: return "SIGSEGV";
  case SIGABRT: return "SIGABRT";
  case SIGILL: return "SIGILL";
  case SIGFPE: return "SIGFPE";
  case SIGBUS: return "SIGBUS";
  case SIGKILL: return "SIGKILL";
  case SIGTERM: return "SIGTERM";
  case SIGINT: return "SIGINT";
  case SIGTRAP: return "SIGTRAP";
  default: return "signal";
  }
}

static bool n_start(cw_remote *r) {
  nat *s = r->tp;
  if (!s) {
    s = calloc(1, sizeof *s);
    r->tp = s;
  }
  s->sock = s->out = -1;
  s->pid = 0;
  s->sock_eof = false;
  s->in_len = s->outq_len = s->line_len = 0;
  if (access(r->d.exe, X_OK)) {
    cw__remote_set_state(r, CW_REMOTE_FAILED, "%s is not an executable (%s): build it first (scripts/build.sh)", r->d.exe,
                         strerror(errno));
    return false;
  }
  int sv[2], op[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
    cw__remote_set_state(r, CW_REMOTE_FAILED, "socketpair: %s", strerror(errno));
    return false;
  }
  if (pipe(op)) {
    close(sv[0]);
    close(sv[1]);
    cw__remote_set_state(r, CW_REMOTE_FAILED, "pipe: %s", strerror(errno));
    return false;
  }
  /* everything the child needs is built before fork: after it, only
   * async-signal-safe calls until exec */
  int nenv = 0;
  while (environ[nenv]) nenv++;
  int nextra = 0;
  if (r->d.env)
    while (r->d.env[nextra]) nextra++;
  char **envp = calloc((size_t)(nenv + nextra + 8), sizeof *envp);
  int k = 0;
  char remote[64], seed[64];
  snprintf(remote, sizeof remote, "CW_REMOTE=fd:%d", sv[1]);
  snprintf(seed, sizeof seed, "CW_SEED=%u", r->d.seed);
  /* ours win over the editor's own environment, and the editor's test-run
   * variables (its input script, end-of-run queries, screenshot, asset
   * root) are not the game's */
  static const char *const not_inherited[] = { "CW_REMOTE=", "CW_START_PAUSED=", "CW_SEED=", "CW_FRAMES=", "CW_FRAME_HZ=",
                                               "CW_INPUT=", "CW_DEBUG_CMDS=", "CW_DEBUG_DUMP=", "CW_SCREENSHOT=",
                                               "CW_ASSETS=", "CW_WARMUP_SPREAD=", NULL };
  for (int i = 0; i < nenv; i++) {
    bool skip = r->d.headless && (!strncmp(environ[i], "SDL_VIDEODRIVER=", 16) || !strncmp(environ[i], "SDL_AUDIODRIVER=", 16));
    for (int j = 0; !skip && not_inherited[j]; j++) skip = !strncmp(environ[i], not_inherited[j], strlen(not_inherited[j]));
    /* a variable the desc sets replaces the inherited one (getenv takes the first) */
    const char *eq = strchr(environ[i], '=');
    size_t nlen = eq ? (size_t)(eq - environ[i]) + 1 : 0;
    for (int j = 0; !skip && nlen && j < nextra; j++) skip = !strncmp(environ[i], r->d.env[j], nlen);
    if (!skip) envp[k++] = environ[i];
  }
  envp[k++] = remote;
  envp[k++] = seed;
  if (r->d.start_paused) envp[k++] = "CW_START_PAUSED=1";
  /* the driver steps the game tick by tick: warm up inside the first frame */
  envp[k++] = "CW_WARMUP_SPREAD=0";
  if (r->d.headless) {
    envp[k++] = "SDL_VIDEODRIVER=offscreen";
    envp[k++] = "SDL_AUDIODRIVER=dummy";
  }
  for (int i = 0; i < nextra; i++) envp[k++] = (char *)r->d.env[i];
  envp[k] = NULL;
  int nargs = 0;
  if (r->d.args)
    while (r->d.args[nargs]) nargs++;
  char **argv = calloc((size_t)nargs + 2, sizeof *argv);
  argv[0] = (char *)r->d.exe;
  for (int i = 0; i < nargs; i++) argv[i + 1] = (char *)r->d.args[i];
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0) {
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL); /* the editor dies: its game goes too */
    if (getppid() != parent) _exit(1);
#endif
    (void)parent;
    setpgid(0, 0); /* a terminal's ^C to the editor is not the game's */
    close(sv[0]);
    close(op[0]);
    dup2(op[1], 1);
    dup2(op[1], 2);
    close(op[1]);
    int fl = fcntl(sv[1], F_GETFD);
    fcntl(sv[1], F_SETFD, fl & ~FD_CLOEXEC);
    if (r->d.cwd && chdir(r->d.cwd)) {
      static const char msg[] = "cw_remote: cannot chdir to the game's working directory\n";
      ssize_t w = write(2, msg, sizeof msg - 1);
      (void)w;
      _exit(126);
    }
    execve(r->d.exe, argv, envp);
    static const char msg[] = "cw_remote: exec of the game failed\n";
    ssize_t w = write(2, msg, sizeof msg - 1);
    (void)w;
    _exit(127);
  }
  free(envp);
  free(argv);
  close(sv[1]);
  close(op[1]);
  if (pid < 0) {
    close(sv[0]);
    close(op[0]);
    cw__remote_set_state(r, CW_REMOTE_FAILED, "fork: %s", strerror(errno));
    return false;
  }
  s->pid = pid;
  s->sock = sv[0];
  s->out = op[0];
  fcntl(s->sock, F_SETFL, fcntl(s->sock, F_GETFL) | O_NONBLOCK);
  fcntl(s->out, F_SETFL, fcntl(s->out, F_GETFL) | O_NONBLOCK);
  fcntl(s->sock, F_SETFD, FD_CLOEXEC);
  fcntl(s->out, F_SETFD, FD_CLOEXEC);
  return true;
}

static void flush_out(nat *s) {
  size_t off = 0;
  while (s->sock >= 0 && off < s->outq_len) {
    ssize_t w = send(s->sock, s->outq + off, s->outq_len - off, MSG_NOSIGNAL);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    break; /* EAGAIN: the rest goes next poll; an error shows as EOF on read */
  }
  if (off) memmove(s->outq, s->outq + off, s->outq_len - off);
  s->outq_len -= off;
}

static void n_send(cw_remote *r, rreq *q) {
  nat *s = r->tp;
  size_t n = strlen(q->text);
  unsigned char hdr[8];
  put_u32(hdr, (uint32_t)n);
  put_u32(hdr + 4, q->id);
  append(&s->outq, &s->outq_len, &s->outq_cap, hdr, 8);
  append(&s->outq, &s->outq_len, &s->outq_cap, q->text, n);
  flush_out(s);
}

static void read_output(cw_remote *r, nat *s) {
  while (s->out >= 0) {
    char buf[4096];
    ssize_t n = read(s->out, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      if (n == 0) { /* the child closed its output (exited) */
        close(s->out);
        s->out = -1;
      }
      break;
    }
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] == '\n' || s->line_len == sizeof s->line - 1) {
        s->line[s->line_len] = 0;
        cw__remote_log(r, s->line);
        s->line_len = 0;
        if (buf[i] == '\n') continue;
      }
      s->line[s->line_len++] = buf[i];
    }
  }
}

static void read_socket(cw_remote *r, nat *s) {
  while (s->sock >= 0 && !s->sock_eof) {
    unsigned char buf[65536];
    ssize_t n = recv(s->sock, buf, sizeof buf, 0);
    if (n > 0) {
      append(&s->in, &s->in_len, &s->in_cap, buf, (size_t)n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    s->sock_eof = true;
  }
  size_t off = 0;
  while (s->in_len - off >= 8) {
    uint32_t len = get_u32(s->in + off), id = get_u32(s->in + off + 4);
    if (s->in_len - off - 8 < len) break;
    const char *payload = (const char *)s->in + off + 8;
    if (id == 0) { /* an event */
      char *ev = malloc((size_t)len + 1);
      memcpy(ev, payload, len);
      ev[len] = 0;
      cw__remote_event(r, CW_REMOTE_EV_HELLO, ev);
      free(ev);
    } else {
      cw__remote_complete(r, id, payload, len, NULL);
    }
    off += 8 + (size_t)len;
  }
  if (off) memmove(s->in, s->in + off, s->in_len - off);
  s->in_len -= off;
}

static void reap(cw_remote *r, nat *s, int options) {
  if (s->pid <= 0) return;
  int st = 0;
  pid_t w = waitpid(s->pid, &st, options);
  if (w != s->pid) return;
  int pid = s->pid;
  s->pid = 0;
  read_output(r, s); /* its last words */
  close_fds(s);
  if (r->state != CW_REMOTE_RUNNING && r->state != CW_REMOTE_STARTING) return; /* stopped or killed by us */
  if (WIFSIGNALED(st)) {
    int sig = WTERMSIG(st);
    cw__remote_set_state(r, CW_REMOTE_CRASHED, "crashed: signal %d (%s), pid %d%s%s", sig, signame(sig), pid,
                         r->log_count ? "; last output: " : "", r->log_count ? cw_remote_log_tail(r, 1) : "");
  } else {
    int code = WEXITSTATUS(st);
    cw__remote_set_state(r, code == 0 ? CW_REMOTE_EXITED : CW_REMOTE_CRASHED, "exited with code %d, pid %d%s%s", code, pid,
                         code && r->log_count ? "; last output: " : "", code && r->log_count ? cw_remote_log_tail(r, 1) : "");
  }
}

static void n_poll(cw_remote *r) {
  nat *s = r->tp;
  if (!s) return;
  flush_out(s);
  read_output(r, s);
  read_socket(r, s);
  reap(r, s, WNOHANG);
  /* the socket closed but the process lingers (exiting): wait for it */
  if (s->sock_eof && s->pid > 0) {
    for (int i = 0; i < 200 && s->pid > 0; i++) {
      usleep(10000);
      reap(r, s, WNOHANG);
    }
  }
}

static void n_stop(cw_remote *r, bool kill_now) {
  nat *s = r->tp;
  if (!s || s->pid <= 0) {
    if (s) close_fds(s);
    return;
  }
  if (!kill_now) {
    /* a polite quit, then EOF (the game quits on it), then signals */
    static const unsigned char quit[] = { 4, 0, 0, 0, 0, 0, 0, 0, 'q', 'u', 'i', 't' };
    if (s->sock >= 0) {
      ssize_t w = send(s->sock, quit, sizeof quit, MSG_NOSIGNAL);
      (void)w;
      shutdown(s->sock, SHUT_WR);
    }
    for (int i = 0; i < 300; i++) { /* 3 s */
      int st;
      if (waitpid(s->pid, &st, WNOHANG) == s->pid) {
        s->pid = 0;
        break;
      }
      read_output(r, s);
      usleep(10000);
    }
    if (s->pid > 0) {
      kill(s->pid, SIGTERM);
      for (int i = 0; i < 100 && s->pid > 0; i++) {
        int st;
        if (waitpid(s->pid, &st, WNOHANG) == s->pid) s->pid = 0;
        else usleep(10000);
      }
    }
  }
  if (s->pid > 0) {
    kill(s->pid, SIGKILL);
    int st;
    waitpid(s->pid, &st, 0);
    s->pid = 0;
  }
  read_output(r, s);
  close_fds(s);
}

static void n_destroy(cw_remote *r) {
  nat *s = r->tp;
  if (!s) return;
  n_stop(r, true);
  free(s->in);
  free(s->outq);
  free(s);
  r->tp = NULL;
}

static int n_pid(cw_remote *r) {
  nat *s = r->tp;
  return s ? (int)s->pid : 0;
}

const rtransport cw__remote_native = { "native", n_start, n_send, n_poll, n_stop, n_destroy, n_pid };

#elif !CW_CART
/* Windows: not implemented yet (CreateProcess + a named pipe or a loopback
 * socket would carry the same frames). */
static bool w_start(cw_remote *r) {
  cw__remote_set_state(r, CW_REMOTE_FAILED, "the native transport is not implemented on Windows yet; use the romdev transport");
  return false;
}
static void w_send(cw_remote *r, rreq *q) { (void)r, (void)q; }
static void w_poll(cw_remote *r) { (void)r; }
static void w_stop(cw_remote *r, bool k) { (void)r, (void)k; }
static void w_destroy(cw_remote *r) { (void)r; }
const rtransport cw__remote_native = { "native", w_start, w_send, w_poll, w_stop, w_destroy, NULL };
#endif

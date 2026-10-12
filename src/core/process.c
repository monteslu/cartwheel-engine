/* Child processes for tools (cartwheel/process.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/process.h"

#if CW_CART || defined(_WIN32)

cw_process *cw_process_start(const cw_process_desc *d, char *err, int err_cap) {
  (void)d;
  if (err && err_cap > 0)
    snprintf(err, (size_t)err_cap, CW_CART ? "a cart cannot start processes (wasmcart has no process API): run the tool natively"
                                           : "starting processes is not implemented on Windows yet");
  return NULL;
}
const char *cw_process_read_line(cw_process *p) { (void)p; return NULL; }
bool cw_process_running(cw_process *p) { (void)p; return false; }
int cw_process_exit_code(cw_process *p) { (void)p; return -1; }
void cw_process_kill(cw_process *p, bool kill_hard) { (void)p, (void)kill_hard; }
void cw_process_free(cw_process *p) { (void)p; }

#else

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern char **environ;

struct cw_process {
  pid_t pid;
  int fd;          /* read end, nonblocking; -1 at EOF */
  int status;      /* exit code once reaped */
  bool reaped;
  char *buf;       /* pending output */
  size_t len, cap, line_start;
  char *line;      /* the line handed out */
};

cw_process *cw_process_start(const cw_process_desc *d, char *err, int err_cap) {
  if (!d || !d->argv || !d->argv[0]) {
    if (err && err_cap > 0) snprintf(err, (size_t)err_cap, "no program given");
    return NULL;
  }
  int fds[2], errp[2];
  if (pipe(fds) || pipe(errp)) {
    if (err && err_cap > 0) snprintf(err, (size_t)err_cap, "pipe: %s", strerror(errno));
    return NULL;
  }
  fcntl(errp[1], F_SETFD, FD_CLOEXEC); /* closes on a successful exec: EOF = started */
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid < 0) {
    if (err && err_cap > 0) snprintf(err, (size_t)err_cap, "fork: %s", strerror(errno));
    close(fds[0]), close(fds[1]), close(errp[0]), close(errp[1]);
    return NULL;
  }
  if (pid == 0) {
    setpgid(0, 0);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent) _exit(127);
#endif
    (void)parent;
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    close(errp[0]);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) dup2(devnull, 0);
    if (d->cwd && chdir(d->cwd)) {
      int e = errno;
      if (write(errp[1], &e, sizeof e)) {}
      _exit(127);
    }
    for (const char *const *e = d->env; e && *e; e++) putenv((char *)*e);
    execvp(d->argv[0], (char *const *)d->argv);
    int e = errno;
    if (write(errp[1], &e, sizeof e)) {}
    _exit(127);
  }
  close(fds[1]);
  close(errp[1]);
  setpgid(pid, pid); /* also from the parent: no race with an early kill */
  int child_errno = 0;
  ssize_t n;
  while ((n = read(errp[0], &child_errno, sizeof child_errno)) < 0 && errno == EINTR) {}
  close(errp[0]);
  if (n == (ssize_t)sizeof child_errno) {
    waitpid(pid, NULL, 0);
    close(fds[0]);
    if (err && err_cap > 0)
      snprintf(err, (size_t)err_cap, "cannot start %s%s%s: %s", d->argv[0], d->cwd ? " in " : "", d->cwd ? d->cwd : "", strerror(child_errno));
    return NULL;
  }
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  cw_process *p = cw_alloc(sizeof *p, CW_MEM_GENERAL);
  memset(p, 0, sizeof *p);
  p->pid = pid;
  p->fd = fds[0];
  p->status = -1;
  return p;
}

static void fill(cw_process *p) {
  while (p->fd >= 0) {
    if (p->cap - p->len < 4096) {
      /* drop what was handed out before growing */
      if (p->line_start) {
        memmove(p->buf, p->buf + p->line_start, p->len - p->line_start);
        p->len -= p->line_start;
        p->line_start = 0;
      }
      if (p->cap - p->len < 4096) {
        p->cap = p->cap ? p->cap * 2 : 8192;
        p->buf = cw_realloc(p->buf, p->cap, CW_MEM_GENERAL);
      }
    }
    ssize_t n = read(p->fd, p->buf + p->len, p->cap - p->len - 1);
    if (n > 0) {
      p->len += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    close(p->fd); /* EOF or error */
    p->fd = -1;
  }
}

const char *cw_process_read_line(cw_process *p) {
  if (!p) return NULL;
  fill(p);
  if (p->line_start >= p->len) return NULL;
  char *s = p->buf + p->line_start, *nl = memchr(s, '\n', p->len - p->line_start);
  size_t l;
  if (nl) l = (size_t)(nl - s);
  else if (p->fd < 0) l = p->len - p->line_start; /* the last, unterminated line */
  else return NULL;
  cw_free(p->line);
  p->line = cw_alloc(l + 1, CW_MEM_GENERAL);
  memcpy(p->line, s, l);
  p->line[l] = 0;
  if (l && p->line[l - 1] == '\r') p->line[l - 1] = 0;
  p->line_start += l + (nl ? 1 : 0);
  return p->line;
}

bool cw_process_running(cw_process *p) {
  if (!p || p->reaped) return false;
  int st;
  pid_t r = waitpid(p->pid, &st, WNOHANG);
  if (r == 0) return true;
  p->reaped = true;
  p->status = r < 0 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
  return false;
}

int cw_process_exit_code(cw_process *p) {
  if (!p) return -1;
  cw_process_running(p);
  return p->reaped ? p->status : -1;
}

void cw_process_kill(cw_process *p, bool kill_hard) {
  if (!p || p->reaped) return;
  kill(-p->pid, kill_hard ? SIGKILL : SIGTERM);
}

void cw_process_free(cw_process *p) {
  if (!p) return;
  if (cw_process_running(p)) {
    cw_process_kill(p, false);
    for (int i = 0; i < 200 && cw_process_running(p); i++) usleep(10000);
    if (cw_process_running(p)) {
      cw_process_kill(p, true);
      waitpid(p->pid, NULL, 0);
    }
  }
  if (p->fd >= 0) close(p->fd);
  cw_free(p->buf);
  cw_free(p->line);
  cw_free(p);
}

#endif

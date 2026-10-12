/* Child processes for tools (the editor's build and export): start a
 * program, read its output line by line without blocking, see how it ended.
 *
 *   const char *argv[] = { "node", "tools/cwproject.mjs", "build", dir, NULL };
 *   cw_process *p = cw_process_start(&(cw_process_desc){ .argv = argv }, err, sizeof err);
 *   ... every frame:
 *   for (const char *l; (l = cw_process_read_line(p));) show(l);
 *   if (!cw_process_running(p)) { int code = cw_process_exit_code(p); cw_process_free(p); }
 *
 * Native only (POSIX): stdout and stderr are merged into one pipe; the child
 * runs in its own process group so cw_process_kill stops everything it
 * started (cmake, ninja, compilers). Linux: the child gets SIGTERM when the
 * parent dies. In a cart there are no processes: cw_process_start fails and
 * says so. Windows: not implemented yet (start fails with a message). */
#ifndef CW_PROCESS_H
#define CW_PROCESS_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_process cw_process;

typedef struct cw_process_desc {
  const char *const *argv; /* NULL-terminated; argv[0] is looked up in PATH */
  const char *cwd;         /* NULL = the parent's */
  const char *const *env;  /* NULL-terminated "NAME=value" added to the parent's environment; NULL = none */
} cw_process_desc;

/* NULL on failure, with the reason in err. */
cw_process *cw_process_start(const cw_process_desc *d, char *err, int err_cap);
/* The next complete output line (no newline), or NULL when none is ready.
 * Valid until the next call on p. After the process ended, a last line
 * without a newline is returned too. */
const char *cw_process_read_line(cw_process *p);
/* false once it exited (reaps it). */
bool cw_process_running(cw_process *p);
/* exit status; 128 + signal when killed by one; -1 while running */
int cw_process_exit_code(cw_process *p);
/* SIGTERM to its process group (SIGKILL when kill_hard). */
void cw_process_kill(cw_process *p, bool kill_hard);
/* Kills it when still running, then frees. */
void cw_process_free(cw_process *p);

#ifdef __cplusplus
}
#endif
#endif

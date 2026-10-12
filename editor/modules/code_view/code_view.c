/* Code view, game build and C hot reload (M4.4).
 *
 * cartwheel games are written in C (mostly by agents); the editor does not
 * edit C, it SHOWS it and closes the loop:
 *   - the Code panel lists the project's C sources (project.cwproj
 *     `sources`) and shows one, read-only, with line numbers and C
 *     highlighting; it follows edits made on disk (an agent's);
 *   - the Build panel runs the project's build (`build`, a shell command run
 *     in the project directory) without blocking the editor and lists the
 *     compiler's errors and warnings; clicking one opens file:line;
 *   - after a good build, `code.reload` swaps the new code in twice:
 *     into the RUNNING GAME (play-in-editor runs it under tools/cwhot; the
 *     request `hot_reload <module>` makes it swap now, state kept) and into
 *     the EDITOR's own type registry (the game module's exported
 *     cw_game_register_types), so scenes using the game's node types open
 *     and new properties show in the inspector. A build that fails changes
 *     nothing: the game keeps running the old code and the errors show;
 *   - `code.node_source` jumps from a node to the C that drives it: its
 *     type's registration (cw_node_type_source, recorded by the
 *     cw_node_type_register macro), the type's descriptor and the functions
 *     behind its callbacks and methods.
 *
 * Project keys (project.cwproj, paths relative to the project directory):
 *   sources = game.c, src/        C files and directories (scanned for .c/.h)
 *   build = cmake --build build   the build command (sh -c, in the project dir)
 *   game_module = build/game_hot.so  the game as a loadable module (cw_add_app
 *                                    makes <app>_hot.so; a project's own CMake
 *                                    can too): its node types load into the
 *                                    editor, cwhot runs it for play-in-editor
 * Natively only: a cart editor has no processes or dlopen (the commands say
 * so); its agent workflow builds through romdev instead. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/platform.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"

#if CW_CART
#pragma GCC diagnostic ignored "-Wunused-function" /* helpers of the native-only paths */
#endif

#if !CW_CART
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

#define MAX_ERRORS 200
#define OUT_CAP (256 * 1024)

typedef struct build_error {
  char file[512]; /* as shown: project-relative when inside the project, else as the compiler wrote it */
  char abs[1024]; /* openable path */
  int line, col;
  char kind[16];  /* error, warning, note, fatal error, link */
  char msg[400];
} build_error;

static struct {
  /* build */
  int id;           /* builds started */
  int state;        /* 0 idle, 1 running, 2 ok, 3 failed */
  bool reload_after;
  double t_start, t_end;
  char cmd[1024];
  char *out;        /* output (tail kept) */
  size_t out_len;
  int exit_code;
  build_error errors[MAX_ERRORS];
  int error_count, warning_count;
  /* reload */
  int reloads;
  char reload_editor[512]; /* last editor-side result */
  char reload_game[600];   /* last game-side result */
  bool game_pending;
  double t_reload_start, game_reload_ms, editor_reload_ms;
  int game_generation;
  /* types loaded into the editor */
  int type_loads;
  int game_type_count;
  long long module_mtime;
  /* view */
  char open_abs[1024], open_rel[512];
  long long open_mtime;
  int open_line;
  char last_node_source[300];
} S;

static cw_node code_text, file_list, file_label, status_label, build_status, error_list, out_text;
static bool files_dirty = true, build_dirty = true, view_dirty;
static const char **file_cache;
static int file_cache_count;

static double now_ms(void) {
#if !CW_CART
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#else
  return cw_time() * 1000.0;
#endif
}

static const char *key(const char *k) {
  const char *v = cwed_project_key(k);
  return v && *v ? v : NULL;
}

/* project-relative or absolute -> openable path (frame arena) */
static const char *abs_path(const char *p) {
  const char *dir = cwed_project_dir();
  if (p[0] == '/' || !dir[0]) return cw_frame_printf("%s", p);
  return cw_frame_printf("%s/%s", dir, p);
}

/* an absolute path inside the project -> relative; else unchanged */
static const char *rel_path(const char *p) {
  const char *dir = cwed_project_dir();
  size_t n = strlen(dir);
  if (n && !strncmp(p, dir, n) && p[n] == '/') return p + n + 1;
  return p;
}

static char *read_file(const char *abs, size_t *len) {
#if !CW_CART
  FILE *f = fopen(abs, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  size_t got = fread(b, 1, (size_t)n, f);
  fclose(f);
  b[got] = 0;
  if (len) *len = got;
  return b;
#else
  /* a cart reads the project layer (bundled or imported files) */
  size_t l;
  char *t = cw_vfs_read(rel_path(abs), &l);
  if (len) *len = l;
  return t;
#endif
}

static long long file_mtime(const char *abs) {
#if !CW_CART
  struct stat st;
  if (stat(abs, &st)) return -1;
  return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
  (void)abs;
  return 0;
#endif
}

/* ── sources ─────────────────────────────────────────────────────────── */

typedef CW_ARRAY(const char *) str_list;

static bool is_source(const char *name) {
  size_t n = strlen(name);
  return (n > 2 && (!strcmp(name + n - 2, ".c") || !strcmp(name + n - 2, ".h"))) || (n > 4 && !strcmp(name + n - 4, ".cpp"));
}

#if !CW_CART
static void walk(str_list *l, const char *rel, int depth) {
  if (depth > 6 || l->count > 2000) return;
  DIR *d = opendir(abs_path(rel));
  if (!d) return;
  struct dirent *e;
  str_list here = { 0 };
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    cw_array_push(&here, cw_intern(cw_frame_printf("%s/%s", rel, e->d_name)));
  }
  closedir(d);
  for (int i = 0; i < here.count; i++) {
    struct stat st;
    if (stat(abs_path(here.data[i]), &st)) continue;
    if (S_ISDIR(st.st_mode)) {
      const char *base = strrchr(here.data[i], '/') + 1;
      if (strcmp(base, "build") && strcmp(base, "out")) walk(l, here.data[i], depth + 1);
    } else if (is_source(here.data[i])) cw_array_push(l, here.data[i]);
  }
  cw_array_free(&here);
}
#endif

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* every source file (project-relative where possible), sorted; cached until
 * a project opens or a build finishes */
static int sources(const char ***out) {
  static str_list l;
  if (files_dirty) {
    files_dirty = false;
    cw_array_clear(&l);
    const char *s = key("sources");
    while (s && *s) {
      while (*s == ' ' || *s == ',') s++;
      const char *e = s;
      while (*e && *e != ',') e++;
      char p[512];
      int n = (int)(e - s);
      while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '/')) n--;
      snprintf(p, sizeof p, "%.*s", n, s);
      s = e;
      if (!p[0]) continue;
#if !CW_CART
      struct stat st;
      if (!stat(abs_path(p), &st) && S_ISDIR(st.st_mode)) walk(&l, p, 0);
      else
#endif
        cw_array_push(&l, cw_intern(p));
    }
    if (l.count > 1) qsort(l.data, (size_t)l.count, sizeof *l.data, cmp_str);
    file_cache = l.data;
    file_cache_count = l.count;
  }
  *out = l.data;
  return l.count;
}

/* ── the code view ───────────────────────────────────────────────────── */

static bool open_file(const char *path, int line, char *err, int cap) {
  const char *a = abs_path(path);
  size_t len;
  char *t = read_file(a, &len);
  if (!t) {
    snprintf(err, (size_t)cap, "cannot read '%s' (code.files lists the project's sources)", path);
    return false;
  }
  snprintf(S.open_abs, sizeof S.open_abs, "%s", a);
  snprintf(S.open_rel, sizeof S.open_rel, "%s", rel_path(a));
  S.open_mtime = file_mtime(a);
  S.open_line = line > 0 ? line : 1;
  if (cw_node_valid(code_text)) {
    cw_text_edit_set_text(code_text, t);
    cw_text_edit_set_caret(code_text, S.open_line - 1, 0);
    cw_label_set_text(file_label, cw_frame_printf("%s:%d", S.open_rel, S.open_line));
  }
  free(t);
  view_dirty = true;
  return true;
}

/* the open file changed on disk (an agent edited it): show the new text, same line */
static void follow_open_file(void) {
  if (!S.open_abs[0]) return;
  long long m = file_mtime(S.open_abs);
  if (m == S.open_mtime) return;
  char err[256];
  int line = S.open_line;
  if (cw_node_valid(code_text)) {
    int l, c;
    cw_text_edit_caret(code_text, &l, &c);
    line = l + 1;
  }
  open_file(S.open_abs, line, err, sizeof err);
}

/* ── the build ───────────────────────────────────────────────────────── */

static const char *state_name(int s) { return s == 1 ? "running" : s == 2 ? "ok" : s == 3 ? "failed" : "idle"; }

static void out_append(const char *b, size_t n) {
  if (!S.out) S.out = cw_alloc(OUT_CAP + 1, CW_MEM_EDITOR), S.out_len = 0;
  if (n > OUT_CAP) b += n - OUT_CAP, n = OUT_CAP;
  if (S.out_len + n > OUT_CAP) { /* keep the tail */
    size_t drop = S.out_len + n - OUT_CAP;
    memmove(S.out, S.out + drop, S.out_len - drop);
    S.out_len -= drop;
  }
  memcpy(S.out + S.out_len, b, n);
  S.out_len += n;
  S.out[S.out_len] = 0;
}

/* "path:line:col: error: message" (gcc/clang), "path:line: error: ..." and
 * the linker's "undefined reference" lines */
static void parse_errors(void) {
  S.error_count = S.warning_count = 0;
  if (!S.out) return;
  char *copy = cw_frame_printf("%s", S.out);
  for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
    if (S.error_count >= MAX_ERRORS) break;
    build_error e = { 0 };
    const char *kinds[] = { "fatal error", "error", "warning", "note" };
    char *hit = NULL;
    const char *kind = NULL;
    for (int k = 0; k < 4 && !hit; k++) {
      char pat[32];
      snprintf(pat, sizeof pat, ": %s: ", kinds[k]);
      hit = strstr(line, pat);
      if (hit) kind = kinds[k];
    }
    if (hit) {
      *hit = 0;
      /* path:line[:col] */
      char *c2 = strrchr(line, ':');
      int a = 0, b = 0;
      if (c2 && sscanf(c2 + 1, "%d", &b) == 1) {
        *c2 = 0;
        char *c1 = strrchr(line, ':');
        if (c1 && sscanf(c1 + 1, "%d", &a) == 1) {
          *c1 = 0;
          e.line = a, e.col = b;
        } else e.line = b;
      }
      if (!e.line || !strcmp(kind, "note")) continue; /* "In function", notes: context only */
      snprintf(e.kind, sizeof e.kind, "%s", kind);
      snprintf(e.msg, sizeof e.msg, "%s", hit + strlen(kind) + 4);
      const char *p = line;
#if !CW_CART
      char real[4096]; /* realpath needs PATH_MAX */
      if (p[0] != '/' && cwed_project_dir()[0]) p = abs_path(p);
      if (realpath(p, real)) p = real;
#endif
      snprintf(e.abs, sizeof e.abs, "%.1023s", p);
      snprintf(e.file, sizeof e.file, "%.511s", rel_path(e.abs));
    } else if (strstr(line, "undefined reference to") || strstr(line, "ld returned")) {
      snprintf(e.kind, sizeof e.kind, "link");
      snprintf(e.msg, sizeof e.msg, "%s", line);
    } else continue;
    if (!strcmp(e.kind, "warning")) S.warning_count++;
    S.errors[S.error_count++] = e;
  }
}

#if !CW_CART
static pid_t build_pid;
static int build_fd = -1;
static bool child_done;
static int child_status;
#endif

static void do_reload(void);

static bool start_build(bool reload, char *err, int cap) {
#if CW_CART
  (void)reload;
  snprintf(err, (size_t)cap, "a cart editor cannot run a build (no processes in a cart): build natively, or through romdev's build tool");
  return false;
#else
  if (S.state == 1) {
    snprintf(err, (size_t)cap, "build %d is still running (code.status shows it; wait for it, then build again)", S.id);
    return false;
  }
  const char *cmd = key("build");
  if (!cmd) {
    snprintf(err, (size_t)cap, "the project has no build command: add `build = <command>` to project.cwproj "
                               "(run in the project directory, e.g. `build = cmake --build build`)");
    return false;
  }
  if (!cwed_project_dir()[0]) {
    snprintf(err, (size_t)cap, "the project lives in memory (no directory to build in): open a project directory");
    return false;
  }
  int p[2];
  if (pipe(p)) {
    snprintf(err, (size_t)cap, "pipe: %s", strerror(errno));
    return false;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, p[1], 1);
  posix_spawn_file_actions_adddup2(&fa, p[1], 2);
  posix_spawn_file_actions_addclose(&fa, p[0]);
  posix_spawn_file_actions_addclose(&fa, p[1]);
  posix_spawn_file_actions_addchdir_np(&fa, cwed_project_dir());
  char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
  int rc = posix_spawn(&build_pid, "/bin/sh", &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(p[1]);
  if (rc) {
    close(p[0]);
    snprintf(err, (size_t)cap, "cannot start the build: %s", strerror(rc));
    return false;
  }
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  build_fd = p[0];
  child_done = false;
  S.id++;
  S.state = 1;
  S.reload_after = reload;
  S.t_start = now_ms();
  S.t_end = 0;
  S.exit_code = -1;
  S.out_len = 0;
  if (S.out) S.out[0] = 0;
  S.error_count = S.warning_count = 0;
  snprintf(S.cmd, sizeof S.cmd, "%s", cmd);
  cwed_log(CWED_LOG_INFO, "build %d: %s", S.id, cmd);
  build_dirty = true;
  return true;
#endif
}

static void poll_build(void) {
#if !CW_CART
  if (S.state != 1) return;
  char buf[16384];
  bool eof = false;
  for (int k = 0; k < 8; k++) {
    ssize_t n = read(build_fd, buf, sizeof buf);
    if (n > 0) {
      out_append(buf, (size_t)n);
      build_dirty = true;
    } else {
      eof = n == 0;
      break;
    }
  }
  if (!child_done) {
    pid_t w = waitpid(build_pid, &child_status, WNOHANG);
    if (w == build_pid) child_done = true;
  }
  if (!child_done || !eof) return;
  close(build_fd);
  build_fd = -1;
  S.t_end = now_ms();
  S.exit_code = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
  parse_errors();
  int real_errors = S.error_count - S.warning_count;
  S.state = S.exit_code == 0 ? 2 : 3;
  build_dirty = true;
  files_dirty = true;
  if (S.state == 2)
    cwed_log(CWED_LOG_INFO, "build %d ok in %.0f ms (%d warning(s))", S.id, S.t_end - S.t_start, S.warning_count);
  else {
    cwed_log(CWED_LOG_ERROR, "build %d failed (exit %d) in %.0f ms: %d error(s); the game keeps running the old code", S.id,
             S.exit_code, S.t_end - S.t_start, real_errors);
    for (int i = 0; i < S.error_count; i++)
      if (strcmp(S.errors[i].kind, "warning"))
        cwed_log(CWED_LOG_ERROR, "%s:%d: %s", S.errors[i].file, S.errors[i].line, S.errors[i].msg);
  }
  if (S.state == 2 && S.reload_after) do_reload();
#endif
}

/* ── game types in the editor ────────────────────────────────────────── */

#if !CW_CART
static CW_ARRAY(const char *) loaded_copies; /* interned copy paths, one per load */

static bool in_loaded_module(const void *p) {
  Dl_info info;
  if (!dladdr(p, &info) || !info.dli_fname) return false;
  for (int i = 0; i < loaded_copies.count; i++)
    if (!strcmp(info.dli_fname, loaded_copies.data[i])) return true;
  return false;
}
#endif

/* Load (or reload) the game module's node types into the editor. The
 * editor never runs game code on its documents: a game type keeps its
 * init/destroy (property defaults) and its properties, but its tree
 * callbacks (enter/ready/exit, updates, notifications) are cleared, the way
 * a non-tool script does not run in another engine's editor. */
static bool load_types(const char *module, char *err, int cap, int *new_types) {
#if CW_CART
  (void)module, (void)new_types;
  snprintf(err, (size_t)cap, "a cart editor cannot load game code (no dlopen in a cart): open the game's scenes in the native editor");
  return false;
#else
  double t0 = now_ms();
  const char *a = abs_path(module);
  struct stat st;
  if (stat(a, &st)) {
    snprintf(err, (size_t)cap, "no game module '%s' (build it first: code.build; project.cwproj `game_module` names it)", module);
    return false;
  }
  /* dlopen caches by path: each load from its own copy (removed once mapped) */
  char copy[1200];
  snprintf(copy, sizeof copy, "%s.editor%d", a, S.type_loads + 1);
  size_t len;
  char *bytes = read_file(a, &len);
  FILE *f = bytes ? fopen(copy, "wb") : NULL;
  if (!f) {
    free(bytes);
    snprintf(err, (size_t)cap, "cannot copy %.250s to %.300s", a, copy);
    return false;
  }
  fwrite(bytes, 1, len, f);
  fclose(f);
  free(bytes);
  void *h = dlopen(copy, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    snprintf(err, (size_t)cap, "%s did not load into the editor: %s", module, dlerror());
    remove(copy);
    return false;
  }
  void (*reg)(void) = (void (*)(void))dlsym(h, "cw_game_register_types");
  if (!reg) {
    snprintf(err, (size_t)cap, "%s exports no cw_game_register_types(): add `void cw_game_register_types(void)` registering "
                               "the game's node types (the editor and cwhot call it)", module);
    remove(copy);
    return false;
  }
  cw_array_push(&loaded_copies, cw_intern(copy));
  S.type_loads++;
  int before = cw_node_type_count();
  reg();
  remove(copy);
  int game = 0;
  for (int i = 0; i < cw_node_type_count(); i++) {
    cw_node_type *t = (cw_node_type *)cw_node_type_at(i);
    if (!in_loaded_module(t)) continue;
    game++;
    t->enter_tree = NULL;
    t->ready = NULL;
    t->exit_tree = NULL;
    t->fixed_update = NULL;
    t->update = NULL;
    t->pre_render = NULL;
    t->prop_changed = NULL;
    t->children_changed = NULL;
  }
  S.game_type_count = game;
  S.module_mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
  S.editor_reload_ms = now_ms() - t0;
  if (new_types) *new_types = cw_node_type_count() - before;
  snprintf(S.reload_editor, sizeof S.reload_editor, "editor: %d game type(s) from %s (load %d, %.1f ms)", game, module, S.type_loads,
           S.editor_reload_ms);
  cwed_log(CWED_LOG_INFO, "%s", S.reload_editor);
  return true;
#endif
}

/* ── reload: editor types + the running game ─────────────────────────── */

static void on_game_reload(const char *json, void *user) {
  (void)user;
  S.game_pending = false;
  S.game_reload_ms = now_ms() - S.t_reload_start;
  const char *g = json ? strstr(json, "\"generation\":") : NULL;
  if (g && !strstr(json, "\"error\"")) {
    S.game_generation = atoi(g + 13);
    snprintf(S.reload_game, sizeof S.reload_game, "game: running generation %d (acknowledged in %.0f ms)", S.game_generation,
             S.game_reload_ms);
    cwed_log(CWED_LOG_INFO, "%s", S.reload_game);
  } else {
    snprintf(S.reload_game, sizeof S.reload_game, "game: not reloaded: %.500s", json ? json : "no reply");
    cwed_log(CWED_LOG_WARN, "%s", S.reload_game);
  }
  build_dirty = true;
}

static void do_reload(void) {
  S.reloads++;
  S.t_reload_start = now_ms();
  const char *module = key("game_module");
  if (!module) {
    snprintf(S.reload_editor, sizeof S.reload_editor, "editor: no game_module in project.cwproj (nothing to load)");
    snprintf(S.reload_game, sizeof S.reload_game, "game: no game_module in project.cwproj");
    build_dirty = true;
    return;
  }
  /* the game first: it is what the human is looking at */
  const cwed_play_backend *b = cwed_play_backend_get();
  if (b && b->running && b->running() && b->request) {
    S.game_pending = true;
    snprintf(S.reload_game, sizeof S.reload_game, "game: reload sent");
    if (!b->request(cw_frame_printf("hot_reload %s", cwed_quote(abs_path(module))), on_game_reload, NULL)) {
      S.game_pending = false;
      snprintf(S.reload_game, sizeof S.reload_game, "game: the play backend refused the request");
    }
  } else {
    snprintf(S.reload_game, sizeof S.reload_game, "game: not running (Play starts it with the new code)");
  }
  char err[600];
  int nt = 0;
  if (!load_types(module, err, sizeof err, &nt)) {
    snprintf(S.reload_editor, sizeof S.reload_editor, "editor: %.500s", err);
    cwed_log(CWED_LOG_WARN, "%s", S.reload_editor);
  }
  build_dirty = true;
}

/* ── node -> source ──────────────────────────────────────────────────── */

typedef struct { const char *file; int line; } loc;

static int line_of(const char *text, const char *at) {
  int l = 1;
  for (const char *p = text; p < at; p++)
    if (*p == '\n') l++;
  return l;
}

/* the definition of function `fn` in text: a line with `fn(` not ending in ';' */
static int find_function(const char *text, const char *fn) {
  size_t n = strlen(fn);
  for (const char *p = strstr(text, fn); p; p = strstr(p + 1, fn)) {
    if (p > text && (p[-1] == '_' || (p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9')))
      continue;
    const char *q = p + n;
    while (*q == ' ') q++;
    if (*q != '(') continue;
    const char *bol = p;
    while (bol > text && bol[-1] != '\n') bol--;
    if (bol != p && (bol[0] == ' ' || bol[0] == '\t')) continue; /* a call inside a function */
    const char *eol = strchr(p, '\n');
    if (!eol) eol = p + strlen(p);
    const char *last = eol;
    while (last > p && (last[-1] == ' ' || last[-1] == '\r')) last--;
    if (last > p && last[-1] == ';') continue; /* a prototype */
    return line_of(text, p);
  }
  return 0;
}

static const char *const CALLBACKS[] = { "init", "destroy", "enter_tree", "ready", "exit_tree", "fixed_update", "update",
                                         "pre_render", "prop_changed", "children_changed", NULL };

static void json_loc(cwed_json *j, const char *file, int line) {
  cwed_json_raw(j, "{");
  cwed_json_key(j, "file");
  cwed_json_str(j, rel_path(file));
  cwed_json_key(j, "line");
  cwed_json_printf(j, "%d", line);
  cwed_json_raw(j, "}");
}

/* Describe where type t lives in C; returns the most useful location to open */
static loc describe_type(cwed_json *j, const cw_node_type *t) {
  loc best = { 0 };
  int reg_line = 0;
  const char *file = cw_node_type_source(t, &reg_line);
  bool game = false;
#if !CW_CART
  game = in_loaded_module(cw_node_type_find(t->name));
#endif
  const char **srcs;
  int ns = sources(&srcs);
  if (!game && file)
    for (int i = 0; i < ns && !game; i++)
      if (!strcmp(abs_path(srcs[i]), file)) game = true;
  cwed_json_raw(j, "{");
  cwed_json_key(j, "type");
  cwed_json_str(j, t->name);
  cwed_json_key(j, "game");
  cwed_json_raw(j, game ? "true" : "false");
  cwed_json_key(j, "registered");
  if (file) json_loc(j, file, reg_line), best = (loc){ file, reg_line };
  else cwed_json_raw(j, "null");
  /* the descriptor: `.name = "Type"` in the registering file, else in any source */
  char needle[160];
  snprintf(needle, sizeof needle, "\"%s\"", t->name);
  const char *def_file = NULL;
  char *text = NULL;
  int def_line = 0;
  const char *cands[1 + 512];
  int nc = 0;
  if (file) cands[nc++] = file;
  for (int i = 0; i < ns && nc < 513; i++) cands[nc++] = cw_intern(abs_path(srcs[i]));
  const char *init_at = NULL;
  for (int i = 0; i < nc && !def_file; i++) {
    char *t2 = read_file(cands[i], NULL);
    if (!t2) continue;
    for (const char *p = strstr(t2, ".name"); p; p = strstr(p + 1, ".name")) {
      const char *q = p + 5;
      while (*q == ' ' || *q == '=') q++;
      if (!strncmp(q, needle, strlen(needle))) {
        def_file = cands[i];
        def_line = line_of(t2, p);
        init_at = p;
        break;
      }
    }
    if (def_file) text = t2;
    else free(t2);
  }
  cwed_json_key(j, "defined");
  if (def_file) {
    json_loc(j, def_file, def_line);
    best = (loc){ def_file, def_line };
  } else cwed_json_raw(j, "null");
  cwed_json_key(j, "callbacks");
  cwed_json_raw(j, "[");
  int ncb = 0;
  if (text) {
    const char *end = strstr(init_at, "};");
    if (!end) end = init_at + strlen(init_at);
    for (int k = 0; CALLBACKS[k]; k++) {
      char pat[48];
      snprintf(pat, sizeof pat, ".%s", CALLBACKS[k]);
      for (const char *p = strstr(init_at, pat); p && p < end; p = strstr(p + 1, pat)) {
        const char *q = p + strlen(pat);
        while (*q == ' ') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ') q++;
        char fn[128];
        int n = 0;
        while (n < 127 && (q[n] == '_' || (q[n] >= 'a' && q[n] <= 'z') || (q[n] >= 'A' && q[n] <= 'Z') || (q[n] >= '0' && q[n] <= '9')))
          n++;
        snprintf(fn, sizeof fn, "%.*s", n, q);
        if (!fn[0]) break;
        int fl = find_function(text, fn);
        if (ncb++) cwed_json_raw(j, ",");
        cwed_json_raw(j, "{");
        cwed_json_key(j, "callback");
        cwed_json_str(j, CALLBACKS[k]);
        cwed_json_key(j, "function");
        cwed_json_str(j, fn);
        cwed_json_key(j, "file");
        cwed_json_str(j, rel_path(def_file));
        cwed_json_key(j, "line");
        cwed_json_printf(j, "%d", fl);
        cwed_json_raw(j, "}");
        /* the per-tick behaviour is what "drives" a node: prefer it */
        if (fl && (!strcmp(CALLBACKS[k], "fixed_update") || !strcmp(CALLBACKS[k], "update"))) best = (loc){ def_file, fl };
        break;
      }
    }
  }
  cwed_json_raw(j, "]");
  /* methods: cw_node_type_add_method("Type", "name", ...) */
  cwed_json_key(j, "methods");
  cwed_json_raw(j, "[");
  int nm = 0;
  for (int i = 0; i < nc; i++) {
    char *t2 = read_file(cands[i], NULL);
    if (!t2) continue;
    for (const char *p = strstr(t2, "cw_node_type_add_method("); p; p = strstr(p + 1, "cw_node_type_add_method(")) {
      const char *q = p + 24;
      while (*q == ' ') q++;
      if (strncmp(q, needle, strlen(needle))) continue;
      q += strlen(needle);
      while (*q == ' ' || *q == ',') q++;
      char name[96] = "";
      if (*q == '"') {
        const char *e = strchr(q + 1, '"');
        if (e) snprintf(name, sizeof name, "%.*s", (int)(e - q - 1), q + 1);
      }
      if (nm++) cwed_json_raw(j, ",");
      cwed_json_raw(j, "{");
      cwed_json_key(j, "method");
      cwed_json_str(j, name);
      cwed_json_key(j, "file");
      cwed_json_str(j, rel_path(cands[i]));
      cwed_json_key(j, "line");
      cwed_json_printf(j, "%d", line_of(t2, p));
      cwed_json_raw(j, "}");
    }
    free(t2);
    if (i == 0 && nc > 1 && cands[1] == cands[0]) i++; /* registering file is also a source: read once */
  }
  cwed_json_raw(j, "]");
  cwed_json_raw(j, "}");
  free(text);
  return best;
}

/* ── commands ────────────────────────────────────────────────────────── */

static bool cmd_files(cwed_call *c) {
  const char **f;
  int n = sources(&f);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "files");
  cwed_json_raw(j, "[");
  for (int i = 0; i < n; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, f[i]);
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "sources_key");
  cwed_json_str(j, key("sources") ? key("sources") : "");
  if (!key("sources") && !n) cwed_json_printf(j, ",\"hint\":\"add `sources = <files or directories>` to project.cwproj\"");
  return true;
}

static bool cmd_open(cwed_call *c) {
  char err[400];
  if (!open_file(cwed_arg(c, "path"), (int)cwed_arg_int(c, "line"), err, sizeof err)) return cwed_fail(c, "%s", err);
  cwed_panel_content("code"); /* (the panel may be a background tab; panel.show brings it up) */
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "file");
  cwed_json_str(j, S.open_rel);
  cwed_json_key(j, "line");
  cwed_json_printf(j, "%d", S.open_line);
  if (cw_node_valid(code_text)) {
    cwed_json_key(j, "lines");
    cwed_json_printf(j, "%d", cw_text_edit_line_count(code_text));
  }
  return true;
}

static bool cmd_read(cwed_call *c) {
  const char *path = cwed_arg(c, "path");
  char *t = read_file(abs_path(path), NULL);
  if (!t) return cwed_fail(c, "cannot read '%s' (code.files lists the project's sources)", path);
  int from = (int)cwed_arg_int(c, "from"), count = (int)cwed_arg_int(c, "count");
  if (from < 1) from = 1;
  if (count < 1) count = 200;
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "file");
  cwed_json_str(j, rel_path(abs_path(path)));
  cwed_json_key(j, "from");
  cwed_json_printf(j, "%d", from);
  cwed_json_key(j, "lines");
  cwed_json_raw(j, "[");
  int l = 1, k = 0;
  for (char *p = t; *p && k < count;) {
    char *e = strchr(p, '\n');
    if (l >= from) {
      if (k++) cwed_json_raw(j, ",");
      cwed_json_str(j, cw_frame_printf("%.*s", e ? (int)(e - p) : (int)strlen(p), p));
    }
    if (!e) break;
    p = e + 1;
    l++;
  }
  cwed_json_raw(j, "]");
  free(t);
  return true;
}

static bool cmd_build(cwed_call *c) {
  char err[600];
  if (!start_build(cwed_arg_bool(c, "reload"), err, sizeof err)) return cwed_fail(c, "%s", err);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "build");
  cwed_json_printf(j, "%d", S.id);
  cwed_json_key(j, "command");
  cwed_json_str(j, S.cmd);
  cwed_json_key(j, "reload");
  cwed_json_raw(j, S.reload_after ? "true" : "false");
  cwed_json_printf(j, ",\"next\":\"code.status (state running -> ok | failed)\"");
  return true;
}

/* one line for panels and the agent view */
static const char *summary(void) {
  if (S.state == 1) return cw_frame_printf("build %d running (%.0f ms)", S.id, now_ms() - S.t_start);
  if (S.state == 0) return "no build yet";
  int errs = S.error_count - S.warning_count;
  if (S.state == 3)
    return cw_frame_printf("build %d FAILED in %.0f ms: %d error(s)%s%s; the game keeps the old code", S.id, S.t_end - S.t_start,
                           errs, errs ? ", first: " : "",
                           errs ? cw_frame_printf("%s:%d: %s", S.errors[0].file, S.errors[0].line, S.errors[0].msg) : "");
  return cw_frame_printf("build %d ok in %.0f ms%s%s%s%s", S.id, S.t_end - S.t_start, S.reloads ? "; " : "",
                         S.reloads ? S.reload_game : "", S.reloads ? "; " : "", S.reloads ? S.reload_editor : "");
}

static bool cmd_status(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "build");
  cwed_json_printf(j, "%d", S.id);
  cwed_json_key(j, "state");
  cwed_json_str(j, state_name(S.state));
  cwed_json_key(j, "summary");
  cwed_json_str(j, summary());
  cwed_json_key(j, "command");
  cwed_json_str(j, S.cmd);
  cwed_json_key(j, "exit_code");
  cwed_json_printf(j, "%d", S.exit_code);
  cwed_json_key(j, "ms");
  cwed_json_printf(j, "%.0f", S.state == 1 ? now_ms() - S.t_start : S.t_end - S.t_start);
  cwed_json_key(j, "errors");
  cwed_json_raw(j, "[");
  for (int i = 0; i < S.error_count; i++) {
    build_error *e = &S.errors[i];
    if (i) cwed_json_raw(j, ",");
    cwed_json_raw(j, "{");
    cwed_json_key(j, "file");
    cwed_json_str(j, e->file);
    cwed_json_key(j, "line");
    cwed_json_printf(j, "%d", e->line);
    cwed_json_key(j, "col");
    cwed_json_printf(j, "%d", e->col);
    cwed_json_key(j, "kind");
    cwed_json_str(j, e->kind);
    cwed_json_key(j, "message");
    cwed_json_str(j, e->msg);
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "reloads");
  cwed_json_printf(j, "%d", S.reloads);
  cwed_json_key(j, "reload_pending");
  cwed_json_raw(j, S.game_pending ? "true" : "false");
  cwed_json_key(j, "game");
  cwed_json_str(j, S.reload_game);
  cwed_json_key(j, "game_generation");
  cwed_json_printf(j, "%d", S.game_generation);
  cwed_json_key(j, "editor");
  cwed_json_str(j, S.reload_editor);
  cwed_json_key(j, "game_types");
  cwed_json_printf(j, "%d", S.game_type_count);
  int tail = (int)cwed_arg_int(c, "tail");
  cwed_json_key(j, "output_tail");
  if (S.out && tail > 0) {
    const char *p = S.out + S.out_len;
    int lines = 0;
    while (p > S.out && lines <= tail) {
      p--;
      if (*p == '\n') lines++;
    }
    cwed_json_str(j, p == S.out ? p : p + 1);
  } else cwed_json_str(j, "");
  return true;
}

static bool cmd_reload(cwed_call *c) {
  if (S.state == 1) return cwed_fail(c, "build %d is running: code.build reload=true reloads when it succeeds", S.id);
  if (S.state == 3) return cwed_fail(c, "the last build failed (code.status lists the errors): fix them and code.build reload=true");
  if (!key("game_module")) return cwed_fail(c, "the project has no game_module: add `game_module = <path to the game's _hot.so>` to project.cwproj");
  do_reload();
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "game");
  cwed_json_str(j, S.reload_game);
  cwed_json_key(j, "editor");
  cwed_json_str(j, S.reload_editor);
  return true;
}

static bool cmd_load_types(cwed_call *c) {
  const char *m = cwed_arg_given(c, "module") ? cwed_arg(c, "module") : key("game_module");
  if (!m) return cwed_fail(c, "no module: pass module=<game_hot.so> or set game_module in project.cwproj");
  char err[600];
  int nt = 0;
  if (!load_types(m, err, sizeof err, &nt)) return cwed_fail(c, "%s", err);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "game_types");
  cwed_json_printf(j, "%d", S.game_type_count);
  cwed_json_key(j, "new_types");
  cwed_json_printf(j, "%d", nt);
  cwed_json_key(j, "types");
  cwed_json_raw(j, "[");
  int k = 0;
  (void)k;
#if !CW_CART
  for (int i = 0; i < cw_node_type_count(); i++)
    if (in_loaded_module(cw_node_type_at(i))) {
      if (k++) cwed_json_raw(j, ",");
      cwed_json_str(j, cw_node_type_at(i)->name);
    }
#endif
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_node_source(cwed_call *c) {
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "node");
  cwed_json_str(j, cwed_doc_node_path(cwed_call_doc(c), n));
  cwed_json_key(j, "types");
  cwed_json_raw(j, "[");
  loc open = { 0 };
  bool first_game = true;
  int k = 0;
  for (const cw_node_type *t = cw_node_get_type(n); t; t = t->base) {
    if (k++) cwed_json_raw(j, ",");
    loc l = describe_type(j, t);
    /* open the most derived GAME type (what the agent wrote); engine types
     * only when the node has no game type */
    bool game = false;
#if !CW_CART
    game = in_loaded_module(cw_node_type_find(t->name));
#endif
    const char **srcs;
    int ns = sources(&srcs);
    const char *rf = cw_node_type_source(t, NULL);
    for (int i = 0; rf && i < ns && !game; i++)
      if (!strcmp(abs_path(srcs[i]), rf)) game = true;
    if (l.file && ((game && first_game) || !open.file)) {
      if (game) first_game = false;
      open = l;
    }
  }
  cwed_json_raw(j, "]");
  if (open.file && cwed_arg_bool(c, "open")) {
    char err[300];
    open_file(open.file, open.line, err, sizeof err);
    cwed_json_key(j, "opened");
    json_loc(j, open.file, open.line);
  }
  snprintf(S.last_node_source, sizeof S.last_node_source, "%s", open.file ? cw_frame_printf("%s:%d", rel_path(open.file), open.line) : "");
  return true;
}

/* ── panels ──────────────────────────────────────────────────────────── */

static void on_file_pick(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)argc, (void)u;
  const char **f;
  int k = sources(&f);
  if (a[0].i >= 0 && a[0].i < k) cwed_runf("code.open path=%s", cwed_quote(f[a[0].i]));
}
static void on_build_btn(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)a, (void)argc, (void)u;
  cwed_run("code.build reload=true", NULL);
}
static void on_source_btn(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)a, (void)argc, (void)u;
  cwed_doc *d = cwed_doc_active();
  if (!d) return;
  cwed_runf("code.node_source path=%s", cwed_quote(cwed_selection_count() > 0 ? cwed_doc_node_path(d, cwed_selection_get(0)) : "."));
}
static void on_error_pick(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)argc, (void)u;
  int i = (int)a[0].i;
  if (i < 0 || i >= S.error_count || !S.errors[i].line) return;
  cwed_runf("code.open path=%s line=%d", cwed_quote(S.errors[i].abs), S.errors[i].line);
}

static cw_node button(const char *text, cw_signal_fn fn) {
  cw_node b = cw_node_new("Button", NULL);
  cw_button_set_text(b, text);
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, fn, NULL, 0);
  return b;
}

static cw_node create_code(void *u) {
  (void)u;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node row = cw_node_new("HBoxContainer", "Bar");
  file_label = cw_node_new("Label", "File");
  cw_label_set_text(file_label, "No file open");
  cw_node_set(file_label, "ellipsis", cw_val_bool(true));
  cw_control_set_size_flags(file_label, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_add_child(row, file_label);
  cw_node_add_child(row, button("Node Source", on_source_btn));
  cw_node_add_child(row, button("Build + Reload", on_build_btn));
  cw_node_add_child(box, row);
  cw_node split = cw_node_new("HSplitContainer", "Split");
  cw_control_set_size_flags(split, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  file_list = cw_node_new("ItemList", "Files");
  cw_node_set(file_list, "custom_minimum_size", cw_val_vec2(170, 0));
  cw_connect(file_list, "item_selected", (cw_node){ 0 }, NULL, on_file_pick, NULL, 0);
  code_text = cw_node_new("TextEdit", "Source");
  cw_node_set(code_text, "editable", cw_val_bool(false));
  cw_node_set(code_text, "show_line_numbers", cw_val_bool(true));
  cw_control_set_size_flags(code_text, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_text_edit_set_highlighter(code_text, cw_ui_highlight_c, NULL);
  cw_node_add_child(split, file_list);
  cw_node_add_child(split, code_text);
  cw_node_add_child(box, split);
  return box;
}

static int out_highlight(cw_node te, const char *line, int len, int idx, cw_text_span *out, int cap, void *user) {
  (void)idx, (void)user, (void)cap;
  if (len <= 0) return 0;
  const char *name = strstr(line, "error") && memchr(line, ':', (size_t)len) ? "keyword_color"
                   : strstr(line, "warning:") ? "number_color" : "comment_color";
  out[0] = (cw_text_span){ 0, len, cw_control_theme_color(te, "TextEdit", name) };
  return 1;
}

static cw_node create_build(void *u) {
  (void)u;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node row = cw_node_new("HBoxContainer", "Bar");
  build_status = cw_node_new("Label", "Status");
  cw_node_set(build_status, "ellipsis", cw_val_bool(true));
  cw_control_set_size_flags(build_status, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_add_child(row, build_status);
  cw_node_add_child(row, button("Build + Reload", on_build_btn));
  cw_node_add_child(box, row);
  cw_node split = cw_node_new("HSplitContainer", "Split");
  cw_control_set_size_flags(split, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  error_list = cw_node_new("ItemList", "Errors");
  cw_control_set_size_flags(error_list, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_connect(error_list, "item_selected", (cw_node){ 0 }, NULL, on_error_pick, NULL, 0);
  out_text = cw_node_new("TextEdit", "Output");
  cw_node_set(out_text, "editable", cw_val_bool(false));
  cw_node_set(out_text, "highlight_current_line", cw_val_bool(false));
  cw_control_set_size_flags(out_text, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_text_edit_set_highlighter(out_text, out_highlight, NULL);
  cw_node_add_child(split, error_list);
  cw_node_add_child(split, out_text);
  cw_node_add_child(box, split);
  return box;
}

static void refresh_build_panel(void) {
  build_dirty = false;
  if (!cw_node_valid(build_status)) return;
  cw_label_set_text(build_status, summary());
  if (cw_node_valid(status_label)) cw_label_set_text(status_label, summary());
  if (S.state == 1) return; /* errors and output once it ends (the label shows progress) */
  cw_item_list_clear(error_list);
  for (int i = 0; i < S.error_count; i++) {
    build_error *e = &S.errors[i];
    if (e->line) cw_item_list_add_item(error_list, cw_frame_printf("%s:%d: %s: %s", e->file, e->line, e->kind, e->msg), (cw_texture){ 0 });
    else cw_item_list_add_item(error_list, cw_frame_printf("%s: %s", e->kind, e->msg), (cw_texture){ 0 });
  }
  if (!S.error_count && S.state == 2) cw_item_list_add_item(error_list, "no errors", (cw_texture){ 0 });
  cw_text_edit_set_text(out_text, S.out ? S.out : "");
  int lines = cw_text_edit_line_count(out_text);
  cw_text_edit_set_caret(out_text, lines > 0 ? lines - 1 : 0, 0);
}

static void refresh_files(void) {
  if (!cw_node_valid(file_list)) return;
  const char **f;
  int n = sources(&f);
  if (n == cw_item_list_count(file_list)) {
    bool same = true;
    for (int i = 0; i < n && same; i++) same = !strcmp(cw_item_list_text(file_list, i), f[i]);
    if (same) return;
  }
  cw_item_list_clear(file_list);
  for (int i = 0; i < n; i++) cw_item_list_add_item(file_list, f[i], (cw_texture){ 0 });
}

static void on_project(const cwed_event *e, void *u) {
  (void)e, (void)u;
  files_dirty = true;
  S.open_abs[0] = 0;
#if !CW_CART
  /* the game's node types, so its scenes open (setting code.load_game_types) */
  const char *m = key("game_module");
  if (m && cwed_setting_bool("code.load_game_types")) {
    char err[600];
    if (!load_types(m, err, sizeof err, NULL)) cwed_log(CWED_LOG_WARN, "%s", err);
  }
#endif
}

static int frames;
static void frame(double dt) {
  (void)dt;
  poll_build();
  frames++;
  if (frames % 15 == 0) follow_open_file();
  if (frames % 60 == 0) files_dirty = true;
  if (files_dirty) refresh_files();
  if (build_dirty || (S.state == 1 && frames % 10 == 0)) refresh_build_panel();
  (void)view_dirty;
}

static void init(void) {
  static const cwed_param p_open[] = { { "path", CWED_ARG_STRING, NULL, "a source file (project-relative or absolute)" },
                                       { "line", CWED_ARG_INT, "1", "line to show (1-based)" }, { 0 } };
  static const cwed_param p_read[] = { { "path", CWED_ARG_STRING, NULL, "a source file" },
                                       { "from", CWED_ARG_INT, "1", "first line (1-based)" },
                                       { "count", CWED_ARG_INT, "200", "lines to return" }, { 0 } };
  static const cwed_param p_build[] = { { "reload", CWED_ARG_BOOL, "true", "reload the game and the editor's types when it succeeds" }, { 0 } };
  static const cwed_param p_status[] = { { "tail", CWED_ARG_INT, "0", "also return the last N lines of build output" }, { 0 } };
  static const cwed_param p_types[] = { { "module", CWED_ARG_STRING, "", "the game module (default: project.cwproj game_module)" }, { 0 } };
  static const cwed_param p_src[] = { { "path", CWED_ARG_PATH, NULL, "the node" },
                                      { "open", CWED_ARG_BOOL, "true", "show the location in the Code panel" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "code.files", .title = "List Source Files", .flags = CWED_CMD_QUERY,
    .doc = "The project's C sources (project.cwproj `sources`: files and directories)." , .exec = cmd_files });
  cwed_command_register(&(cwed_command_desc){ .name = "code.open", .title = "Open Source File", .params = p_open,
    .doc = "Show a C source in the Code panel at a line (read-only; it follows edits made on disk).", .exec = cmd_open });
  cwed_command_register(&(cwed_command_desc){ .name = "code.read", .title = "Read Source Lines", .params = p_read,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "Lines of a source file as JSON strings (for agents).", .exec = cmd_read });
  cwed_command_register(&(cwed_command_desc){ .name = "code.build", .title = "Build Game", .params = p_build, .shortcut = "f7",
    .doc = "Run the project's build (project.cwproj `build`) in the background; with reload (default) a good build is "
           "swapped into the running game and the editor. code.status follows it.", .exec = cmd_build });
  cwed_command_register(&(cwed_command_desc){ .name = "code.status", .title = "Build Status", .params = p_status,
    .flags = CWED_CMD_QUERY, .doc = "The last build: state (idle running ok failed), errors with file:line, reload results "
                                    "(game generation, editor types), output tail.", .exec = cmd_status });
  cwed_command_register(&(cwed_command_desc){ .name = "code.reload", .title = "Hot Reload Game Code",
    .doc = "Swap the built game module into the running game (cwhot `hot_reload`, state kept) and into the editor's types.",
    .exec = cmd_reload });
  cwed_command_register(&(cwed_command_desc){ .name = "code.load_types", .title = "Load Game Node Types", .params = p_types,
    .doc = "Load the game module's node types (its cw_game_register_types) into the editor so scenes using them open; "
           "the editor keeps their data and properties, never runs their tree callbacks.", .exec = cmd_load_types });
  cwed_command_register(&(cwed_command_desc){ .name = "code.node_source", .title = "Show Node Source", .params = p_src,
    .flags = CWED_CMD_SCENE, .doc = "Where a node's behaviour lives in C: each type of the node with its registration, its "
                                    "descriptor, the functions behind its callbacks and its methods; opens the most derived game type.",
    .exec = cmd_node_source });
  cwed_setting_define(&(cwed_setting_desc){ .key = "code.load_game_types", .type = CWED_ARG_BOOL, .def = "true",
    .doc = "Load the project's game_module node types when a project opens (natively)." });
  cwed_panel_register(&(cwed_panel_desc){ .id = "code", .title = "Code", .slot = "center", .order = 80, .create = create_code });
  cwed_panel_register(&(cwed_panel_desc){ .id = "build", .title = "Build", .slot = "bottom", .order = 20, .create = create_build });
  cwed_menu_add("Play/Build and Reload Code", "code.build reload=true", 500);
  cwed_menu_add("Play/Reload Code", "code.reload", 510);
  cwed_menu_add("Play/Load Game Node Types", "code.load_types", 520);
  cwed_menu_add("Scene/Show Node Source", "code.node_source", 900);
  cwed_subscribe(CWED_EV_PROJECT, on_project, NULL);
  on_project(NULL, NULL);
}

static void shutdown(void) {
#if !CW_CART
  if (S.state == 1 && build_pid > 0) kill(build_pid, SIGTERM);
#endif
  cw_free(S.out);
  S.out = NULL;
}

CWED_MODULE(code_view) = { .title = "Code", .milestone = "M4.4", .init = init, .frame = frame, .shutdown = shutdown };

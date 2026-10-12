/* Play-in-editor (M4.3), the editor side: the play backend behind the core's
 * play.* commands, built on cartwheel/remote.h.
 *
 * The game never runs in the editor: Play starts the PROJECT'S GAME (its own
 * app, with its own node types and systems) as a separate instance and
 * talks to it over the debug channel:
 *   native  a child process (crash-isolated; it opens its own window unless
 *           play.headless or the editor itself is headless), reading the
 *           project directory the editor saves to (CW_PROJECT);
 *   romdev  the game's .wasc in romdev; the editor pushes the project's
 *           scenes into the cart's project layer (vfs write) first, since a
 *           cart cannot read the editor's files.
 * The cart editor has no transport (a cart cannot spawn a game, and nothing
 * carries the debug channel between two carts): Play says so and the Game
 * panel shows why instead of failing silently.
 *
 * Every launch starts the game paused at tick 1 (CW_START_PAUSED), switches
 * it to the scene being played (change_scene), then resumes unless
 * play.start_paused is set. Which game: play.game_exe / play.game_cart
 * (settings), else project.cwproj's game_exe / game_cart (paths relative to
 * the project), else its game_module run under cwhot (hot reload, M4.4),
 * else its game_app resolved in the editor's own build tree
 * (<build>/samples|games/<app>, <build>/../cart/carts/<app>.wasc). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !CW_CART
#include <limits.h>
#include <unistd.h>
#endif

#include "cartwheel/vfs.h"
#include "play.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation" /* paths and status lines are bounded; a cut one is still useful */
#endif

static struct {
  cw_remote *r;
  int gen, inspected, frame;
  char transport[16], scene[512], game[1024], status[2048];
  char *crash, inspect_path[512];
  bool paused, status_inflight, ended_reported;
  long long tick;
  int pending;
  /* project.cwproj keys this module reads */
  char game_app[128], game_exe[1024], game_cart[1024], game_module[1024], scenes[2048];
  char game_arg[1024]; /* the module cwhot runs (game_module), else "" */
} S = { .transport = "none" };

static bool sync_mode;

/* ── small helpers ────────────────────────────────────────────────────── */

static char *dupf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *dupf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *s = malloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(s, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return s;
}

const char *pl_token(const char *text) {
  if (!text) return "\"\"";
  bool plain = *text && *text != '"';
  for (const char *p = text; plain && *p; p++)
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '"' || *p == '\\' || *p == ';') plain = false;
  /* vec3(1, 2, 3) travels unquoted too (the channel keeps a paren group), but quoting is always safe */
  if (plain) return text;
  size_t n = strlen(text);
  char *o = cw_frame_alloc(n * 2 + 3), *w = o;
  *w++ = '"';
  for (const char *p = text; *p; p++) {
    if (*p == '"' || *p == '\\') *w++ = '\\';
    *w++ = *p;
  }
  *w++ = '"';
  *w = 0;
  return o;
}

void pl_rect_json(cwed_json *j, const char *name, cw_rect r) {
  cwed_json_key(j, name);
  cwed_json_printf(j, "{\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g}", r.x, r.y, r.w, r.h);
}

/* ── state queries ────────────────────────────────────────────────────── */

bool pl_available(void) { return !CW_CART; }
const char *pl_unavailable_why(void) {
  return CW_CART ? "Play is not available in the cart editor: a cart cannot start a game, and no transport carries the "
                   "debug channel between two carts yet. Use the native editor to play, or run the game's cart in romdev beside "
                   "this one and move the project with vfs export / vfs import. A peer transport (over the relay M5.4 uses, or a "
                   "romdev-side bridge) would need no ABI change; whether to add one has not been decided."
                 : NULL;
}
bool pl_running(void) { return S.r && cw_remote_state_of(S.r) == CW_REMOTE_RUNNING; }
bool pl_active(void) { return S.r != NULL; }
int pl_generation(void) { return S.gen; }
const char *pl_transport(void) { return S.transport; }
const char *pl_scene(void) { return S.scene; }
const char *pl_crash(void) { return S.crash; }
bool pl_is_paused(void) { return pl_running() && S.paused; }
long long pl_tick(void) { return S.tick; }
bool pl_sync(void) { return sync_mode || cwed_setting_bool("play.sync_requests"); }
int pl_poll_frames(void) {
  int n = (int)cwed_setting_float("play.poll_frames");
  return n < 1 ? 1 : n;
}

static bool editor_headless(void) {
#if CW_CART
  return true;
#else
  const char *vd = getenv("SDL_VIDEODRIVER");
  return vd && (!strcmp(vd, "offscreen") || !strcmp(vd, "dummy"));
#endif
}
static bool game_headless(void) { return cwed_setting_bool("play.headless") || editor_headless(); }

bool pl_preview_default(void) {
  const char *p = cwed_setting("play.preview");
  if (p && !strcmp(p, "on")) return true;
  if (p && !strcmp(p, "off")) return false;
  /* auto: a native game with a window shows itself; romdev and headless games only here */
  return strcmp(S.transport, "native") || game_headless();
}

const char *pl_status(void) {
  if (!pl_available()) return "Play unavailable in the cart editor";
  if (!S.r) return "not running: Play (F5) runs the current scene, Play/Play Main Scene the main one";
  cw_remote_state st = cw_remote_state_of(S.r);
  if (st == CW_REMOTE_RUNNING)
    snprintf(S.status, sizeof S.status, "%s on %s  ·  %s  ·  tick %lld%s", S.scene, S.transport, S.paused ? "paused" : "running",
             S.tick, cw_remote_pid(S.r) ? cw_frame_printf("  ·  pid %d", cw_remote_pid(S.r)) : "");
  else snprintf(S.status, sizeof S.status, "%s on %s  ·  %s", S.scene, S.transport, cw_remote_status(S.r));
  return S.status;
}

static void status_bar(void) {
  if (!S.r) cwed_status_set("play", 30, "");
  else cwed_status_set("play", 30, cw_frame_printf("game: %s%s", cw_remote_state_name(cw_remote_state_of(S.r)),
                                                   pl_running() && S.paused ? " (paused)" : ""));
}

/* ── requests ─────────────────────────────────────────────────────────── */

typedef struct { pl_cb cb; void *user; int gen; } req;

static void on_reply(cw_remote *r, const cw_remote_reply *rep, void *u) {
  (void)r;
  req *q = u;
  S.pending--;
  if (q->gen == S.gen && q->cb) q->cb(rep, q->user);
  free(q);
}

uint32_t pl_send(const char *request, pl_cb cb, void *user) {
  if (!pl_running()) return 0;
  req *q = malloc(sizeof *q);
  *q = (req){ cb, user, S.gen };
  uint32_t id = cw_remote_send(S.r, request, 0, on_reply, q);
  if (!id) {
    free(q);
    return 0;
  }
  S.pending++;
  /* test runs: complete it now, so what the editor shows depends on frames, not on timing */
  if (pl_sync()) cw_remote_wait(S.r, id, 10000);
  return id;
}

bool pl_wait(uint32_t id, int timeout_ms) { return S.r && id && cw_remote_wait(S.r, id, timeout_ms); }

char *pl_call(const char *request, char **err) {
  if (err) *err = NULL;
  if (!pl_available()) {
    if (err) *err = dupf("%s", pl_unavailable_why());
    return NULL;
  }
  if (!S.r) {
    if (err) *err = dupf("no game is running: play.start (F5) starts one");
    return NULL;
  }
  return cw_remote_call(S.r, request, 0, err);
}

static bool call_ok(const char *request, char *err, int cap) {
  char *e = NULL, *rep = pl_call(request, &e);
  if (!rep) {
    snprintf(err, (size_t)cap, "`%s`: %s", request, e ? e : "no reply");
    free(e);
    return false;
  }
  free(rep);
  return true;
}

static void refresh_status_now(void) {
  char *rep = pl_running() ? pl_call("status", NULL) : NULL;
  if (!rep) return;
  double t;
  bool p;
  if (cw_remote_json_number(rep, "tick", &t)) S.tick = (long long)t;
  if (cw_remote_json_bool(rep, "paused", &p)) S.paused = p;
  free(rep);
}

static void on_status(const cw_remote_reply *r, void *u) {
  (void)u;
  S.status_inflight = false;
  if (!r->ok) return;
  double t;
  bool p;
  if (cw_remote_json_number(r->json, "tick", &t)) S.tick = (long long)t;
  if (cw_remote_json_bool(r->json, "paused", &p)) S.paused = p;
}

/* ── project and game ─────────────────────────────────────────────────── */

static void trim(char *s) {
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '"')) *--e = 0;
  char *b = s;
  while (*b == ' ' || *b == '\t' || *b == '"') b++;
  memmove(s, b, strlen(b) + 1);
}

static void read_project(void) {
  S.game_app[0] = S.game_exe[0] = S.game_cart[0] = S.game_module[0] = S.scenes[0] = 0;
  size_t len;
  char *t = cw_vfs_read("project.cwproj", &len);
  if (!t) return;
  for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
    char *eq = strchr(line, '=');
    if (!eq || line[0] == '#') continue;
    *eq = 0;
    char k[64], v[2048];
    snprintf(k, sizeof k, "%s", line);
    snprintf(v, sizeof v, "%s", eq + 1);
    trim(k);
    trim(v);
    if (!strcmp(k, "game_app")) snprintf(S.game_app, sizeof S.game_app, "%.127s", v);
    else if (!strcmp(k, "game_exe")) snprintf(S.game_exe, sizeof S.game_exe, "%.1023s", v);
    else if (!strcmp(k, "game_cart")) snprintf(S.game_cart, sizeof S.game_cart, "%.1023s", v);
    else if (!strcmp(k, "game_module")) snprintf(S.game_module, sizeof S.game_module, "%.1023s", v);
    else if (!strcmp(k, "scenes")) snprintf(S.scenes, sizeof S.scenes, "%s", v);
  }
  free(t);
}

#if !CW_CART
static bool file_ok(const char *p) { return p && *p && access(p, R_OK) == 0; }

static const char *exe_dir(void) {
  static char d[PATH_MAX];
#if defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", d, sizeof d - 1);
  if (n <= 0) return NULL;
  d[n] = 0;
  char *s = strrchr(d, '/');
  if (s) *s = 0;
  return d;
#else
  return NULL; /* macOS/Windows: name the game with play.game_exe or game_exe */
#endif
}

/* a path from the project file: absolute, or relative to the project directory */
static void project_path(const char *p, char *out, size_t cap) {
  const char *root = cwed_project_dir();
  if (p[0] == '/' || !root || !*root) snprintf(out, cap, "%s", p);
  else snprintf(out, cap, "%s/%s", root, p);
}

static bool find_game(bool cart, char *out, size_t cap, char *why, size_t why_cap) {
  if (!cart) S.game_arg[0] = 0;
  const char *set = cwed_setting(cart ? "play.game_cart" : "play.game_exe");
  if (set && *set) {
    snprintf(out, cap, "%.*s", (int)cap - 1, set);
    if (file_ok(out)) return true;
    snprintf(why, why_cap, "play.%s = %s: no such file", cart ? "game_cart" : "game_exe", set);
    return false;
  }
  const char *key = cart ? S.game_cart : S.game_exe;
  if (*key) {
    project_path(key, out, cap);
    if (file_ok(out)) return true;
    snprintf(why, why_cap, "project.cwproj %s = %s: no such file (%s)", cart ? "game_cart" : "game_exe", key, out);
    return false;
  }
  /* the game as a hot-reloadable module (M4.4): cwhot, next to the editor's build, runs it */
  if (!cart && *S.game_module) {
    char mod[1024];
    project_path(S.game_module, mod, sizeof mod);
    const char *d = exe_dir();
    snprintf(out, cap, "%s/../cwhot", d ? d : ".");
    if (!file_ok(mod)) {
      snprintf(why, why_cap, "project.cwproj game_module = %s: not built yet (%s; the project's build makes it)", S.game_module, mod);
      return false;
    }
    if (!file_ok(out)) {
      snprintf(why, why_cap, "game_module needs cwhot next to the editor's build (%s): build it (scripts/build.sh)", out);
      return false;
    }
    char real[PATH_MAX];
    if (realpath(out, real)) snprintf(out, cap, "%s", real);
    if (realpath(mod, real)) snprintf(S.game_arg, sizeof S.game_arg, "%s", real);
    else snprintf(S.game_arg, sizeof S.game_arg, "%s", mod);
    return true;
  }
  if (*S.game_app) {
    const char *d = exe_dir();
    const char *const native_dirs[] = { "../samples", "../games", "..", NULL };
    const char *const cart_dirs[] = { "../../cart/carts", "../carts", NULL };
    for (const char *const *dir = cart ? cart_dirs : native_dirs; d && *dir; dir++) {
      snprintf(out, cap, "%s/%s/%s%s", d, *dir, S.game_app, cart ? ".wasc" : "");
      if (file_ok(out)) {
        char real[PATH_MAX];
        if (realpath(out, real)) snprintf(out, cap, "%s", real);
        return true;
      }
    }
    snprintf(why, why_cap, "project.cwproj game_app = %s: no %s build of it next to the editor's (build it: scripts/build.sh)",
             S.game_app, cart ? "cart" : "native");
    return false;
  }
  snprintf(why, why_cap, "the project names no game: add game_app (a build target) or game_exe / game_cart to "
                         "project.cwproj, or set play.game_exe / play.game_cart");
  return false;
}
#endif

/* ── launch ───────────────────────────────────────────────────────────── */

static void on_remote_event(cw_remote *r, cw_remote_event ev, const char *text, void *user);
static void reinspect(void);

/* scenes the game may load: the played one, what it instances, the
 * project's list and every open document (saved first by the core) */
typedef struct { char p[64][512]; int n; } path_set;
static void add_path(path_set *s, const char *p) {
  if (!p || !*p || s->n >= 64) return;
  for (int i = 0; i < s->n; i++)
    if (!strcmp(s->p[i], p)) return;
  snprintf(s->p[s->n++], sizeof s->p[0], "%s", p);
}
static void add_scene_deps(path_set *s, const char *scene) {
  size_t len;
  char *t = cw_vfs_read(scene, &len);
  if (!t) return;
  for (char *l = strstr(t, "[ext_scene"); l; l = strstr(l + 1, "[ext_scene")) {
    char *pp = strstr(l, "path=\"");
    char *end = pp ? strchr(pp + 6, '"') : NULL;
    char *nl = strchr(l, '\n');
    if (!pp || !end || (nl && pp > nl)) continue;
    char dep[512];
    snprintf(dep, sizeof dep, "%.*s", (int)(end - pp - 6), pp + 6);
    int before = s->n;
    add_path(s, dep);
    if (s->n > before) add_scene_deps(s, dep);
  }
  free(t);
}

static bool push_file(const char *path, char *err, int cap) {
  size_t len;
  char *data = cw_vfs_read(path, &len);
  if (!data) return true; /* listed but not there: the game reports it if it needs it */
  char *b64 = cw_base64_encode(data, len);
  free(data);
  size_t n = strlen(b64);
  const size_t CHUNK = 3000; /* requests stay under romdev's 4 KiB; a multiple of 4 decodes on its own */
  bool ok = true;
  for (size_t off = 0; ok && (off < n || off == 0); off += CHUNK) {
    size_t k = n - off < CHUNK ? n - off : CHUNK;
    ok = call_ok(cw_frame_printf("vfs %s %s %.*s", off ? "append" : "write", pl_token(path), (int)k, b64 + off), err, cap);
    if (!n) break;
  }
  cw_free(b64);
  return ok;
}

static bool setup(char *err, int cap) {
  if (!strcmp(S.transport, "romdev")) {
    path_set set = { .n = 0 };
    add_path(&set, S.scene);
    add_scene_deps(&set, S.scene);
    char list[2048];
    snprintf(list, sizeof list, "%s", S.scenes);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
      trim(tok);
      add_path(&set, tok);
      add_scene_deps(&set, tok);
    }
    for (int i = 0; i < cwed_doc_count(); i++) add_path(&set, cwed_doc_path(cwed_doc_at(i)));
    for (int i = 0; i < set.n; i++)
      if (!push_file(set.p[i], err, cap)) return false;
    cwed_log(CWED_LOG_INFO, "play: pushed %d project scene%s into the game cart's project layer", set.n, set.n == 1 ? "" : "s");
  }
  if (!call_ok(cw_frame_printf("change_scene %s", pl_token(S.scene)), err, cap)) return false;
  bool stay = cwed_setting_bool("play.start_paused");
  if (!stay && !call_ok("resume", err, cap)) return false;
  S.paused = stay;
  S.tick = 1;
  refresh_status_now();
  return true;
}

static void end_session(void) {
  if (S.r) cw_remote_close(S.r);
  S.r = NULL;
  S.gen++;
  S.inspected = 0;
  status_bar();
}

static bool launch(const char *scene, char *err, int cap) {
  if (!pl_available()) {
    snprintf(err, (size_t)cap, "%s", pl_unavailable_why());
    return false;
  }
#if CW_CART
  (void)scene, (void)on_remote_event;
  return false;
#else
  read_project();
  const char *want = cwed_setting("play.transport");
  char game[1024], why_native[600] = "", why_cart[600] = "";
  bool native = false;
  if (!want || !strcmp(want, "auto")) {
    if (find_game(false, game, sizeof game, why_native, sizeof why_native)) native = true;
    else if (!find_game(true, game, sizeof game, why_cart, sizeof why_cart)) {
      snprintf(err, (size_t)cap, "no game to play: %s", why_native);
      return false;
    }
  } else if (!strcmp(want, "native")) {
    if (!find_game(false, game, sizeof game, why_native, sizeof why_native)) {
      snprintf(err, (size_t)cap, "no native game: %s", why_native);
      return false;
    }
    native = true;
  } else if (!find_game(true, game, sizeof game, why_cart, sizeof why_cart)) {
    snprintf(err, (size_t)cap, "no game cart for romdev: %s", why_cart);
    return false;
  }
  end_session();
  free(S.crash);
  S.crash = NULL;
  S.ended_reported = false;
  snprintf(S.transport, sizeof S.transport, "%s", native ? "native" : "romdev");
  snprintf(S.scene, sizeof S.scene, "%s", scene);
  snprintf(S.game, sizeof S.game, "%s", game);
  char project_env[1200];
  const char *dir = cwed_project_dir();
  char real[PATH_MAX];
  if (dir && *dir && realpath(dir, real)) dir = real;
  snprintf(project_env, sizeof project_env, "CW_PROJECT=%s", dir ? dir : "");
  const char *env[] = { project_env, NULL };
  const char *args[] = { S.game_arg, NULL };
  cw_remote_desc d = { .transport = native ? CW_REMOTE_NATIVE : CW_REMOTE_ROMDEV, .start_paused = true,
                       .headless = native && cwed_setting_bool("play.headless"), .seed = 1 };
  if (native) {
    d.exe = game;
    if (dir && *dir) d.env = env;
    if (S.game_arg[0]) d.args = args;
  } else {
    d.cart = game;
  }
  cwed_log(CWED_LOG_INFO, "play: launching %s%s%s (%s) at %s", game, S.game_arg[0] ? " " : "", S.game_arg, S.transport, scene);
  S.r = cw_remote_launch(&d);
  if (!S.r) {
    snprintf(err, (size_t)cap, "could not launch %s: %s", game, cw_remote_last_error());
    return false;
  }
  cw_remote_on_event(S.r, on_remote_event, NULL);
  S.gen++;
  if (cw_remote_state_of(S.r) != CW_REMOTE_RUNNING) {
    snprintf(err, (size_t)cap, "%s did not start: %s", game, cw_remote_status(S.r));
    end_session();
    return false;
  }
  if (!setup(err, cap)) {
    end_session();
    return false;
  }
  cwed_log(CWED_LOG_INFO, "play: %s", pl_status());
  reinspect();
  status_bar();
  return true;
#endif
}

static bool relaunch(char *err, int cap) {
  if (!S.r) {
    if (!*S.scene) {
      snprintf(err, (size_t)cap, "nothing played yet: play.start (F5) first");
      return false;
    }
    return launch(S.scene, err, cap);
  }
  free(S.crash);
  S.crash = NULL;
  S.ended_reported = false;
  bool ok = cw_remote_relaunch(S.r);
  S.gen++;
  S.inspected = 0;
  if (!ok) {
    snprintf(err, (size_t)cap, "relaunch failed: %s", cw_remote_status(S.r));
    return false;
  }
  if (!setup(err, cap)) return false;
  cwed_log(CWED_LOG_INFO, "play: relaunched (launch %d): %s", cw_remote_launch_count(S.r), pl_status());
  reinspect();
  status_bar();
  return true;
}

/* a crash or an exit: the report goes to the Output panel and the Game panel */
static void report_end(void) {
  if (S.ended_reported || !S.r) return;
  S.ended_reported = true;
  cw_remote_state st = cw_remote_state_of(S.r);
  const char *tail = cw_remote_log_tail(S.r, 8);
  free(S.crash);
  S.crash = dupf("%s %s: %s%s%s", S.scene, st == CW_REMOTE_CRASHED ? "CRASHED" : st == CW_REMOTE_EXITED ? "EXITED" : "STOPPED",
                 cw_remote_status(S.r), tail && *tail ? "\nthe game's last output:\n" : "", tail ? tail : "");
  cwed_log(st == CW_REMOTE_CRASHED ? CWED_LOG_ERROR : CWED_LOG_WARN, "play: %s %s: %s", S.scene,
           st == CW_REMOTE_CRASHED ? "crashed" : st == CW_REMOTE_EXITED ? "exited" : "stopped", cw_remote_status(S.r));
  if (tail && *tail) {
    char *copy = dupf("%s", tail);
    for (char *l = strtok(copy, "\n"); l; l = strtok(NULL, "\n")) cwed_log(CWED_LOG_INFO, "game| %s", l);
    free(copy);
  }
  cwed_log(CWED_LOG_INFO, "play: the editor is unaffected; play.relaunch (Game panel: Relaunch) starts the game again");
  status_bar();
}

static void on_remote_event(cw_remote *r, cw_remote_event ev, const char *text, void *user) {
  (void)r, (void)user;
  if (ev == CW_REMOTE_EV_LOG) cwed_log(CWED_LOG_INFO, "game: %s", text);
  else if (ev == CW_REMOTE_EV_STATE) {
    cw_remote_state st = cw_remote_state_of(S.r);
    if (st == CW_REMOTE_CRASHED || st == CW_REMOTE_EXITED || st == CW_REMOTE_FAILED) report_end();
    status_bar();
  }
}

/* ── the backend ──────────────────────────────────────────────────────── */

static bool be_start(const char *scene, char *err, int cap) { return launch(scene, err, cap); }
static void be_stop(void) {
  if (S.r) cwed_log(CWED_LOG_INFO, "play: stopped %s", S.scene);
  end_session();
}
static bool be_running(void) { return pl_running(); }
static void be_set_paused(bool p) {
  if (!pl_running()) return;
  char *rep = pl_call(p ? "pause 1" : "resume", NULL);
  double t;
  if (rep && cw_remote_json_number(rep, "tick", &t)) S.tick = (long long)t;
  if (rep) S.paused = p;
  free(rep);
}
static long long do_step(int ticks, char **err) {
  long long t = cw_remote_step(S.r, (uint32_t)(ticks < 1 ? 1 : ticks), 0, err);
  if (t >= 0) {
    S.tick = t;
    S.paused = true;
  }
  return t;
}
static void be_step(int ticks) {
  if (!pl_running()) return;
  char *err = NULL;
  if (do_step(ticks, &err) < 0) cwed_log(CWED_LOG_ERROR, "play.step: %s", err ? err : "?");
  free(err);
}

typedef struct { void (*cb)(const char *, void *); void *user; } fwd;
static void fwd_reply(const cw_remote_reply *r, void *u) {
  fwd *f = u;
  if (f->cb) f->cb(r->json ? r->json : cw_frame_printf("{\"error\":%s}", r->error ? "\"transport failure\"" : "null"), f->user);
  free(f);
}
static bool be_request(const char *line, void (*cb)(const char *json, void *user), void *user) {
  fwd *f = malloc(sizeof *f);
  *f = (fwd){ cb, user };
  if (pl_send(line, fwd_reply, f)) return true;
  free(f);
  return false;
}

static void be_poll(void) {
  S.frame++;
  if (S.r) cw_remote_pump(S.r);
  if (pl_running() && !S.status_inflight && S.frame % 15 == 0) {
    S.status_inflight = true;
    if (!pl_send("status", on_status, NULL)) S.status_inflight = false;
  }
}

static const cwed_play_backend BACKEND = { .name = "remote", .start = be_start, .stop = be_stop, .running = be_running,
                                           .set_paused = be_set_paused, .step = be_step, .request = be_request,
                                           .poll = be_poll, .paused = pl_is_paused };

/* ── the played scene in the editor ───────────────────────────────────── */

cwed_doc *pl_doc(void) {
  for (int i = 0; i < cwed_doc_count(); i++)
    if (*S.scene && !strcmp(cwed_doc_path(cwed_doc_at(i)), S.scene)) return cwed_doc_at(i);
  return NULL;
}

const char *pl_doc_path(const char *remote_path) {
  cwed_doc *d = pl_doc();
  if (!d || !remote_path || strncmp(remote_path, "/root/", 6)) return NULL;
  const char *root = cw_node_name(cwed_doc_root(d)), *rest = remote_path + 6;
  size_t n = strlen(root);
  if (strncmp(rest, root, n)) return NULL;
  if (!rest[n]) return ".";
  if (rest[n] != '/') return NULL;
  return cw_frame_printf("%s", rest + n + 1);
}

void pl_inspect(int uid) { S.inspected = uid; }
void pl_inspect_path(const char *path) { snprintf(S.inspect_path, sizeof S.inspect_path, "%s", path ? path : ""); }

/* after a (re)launch: the node inspected before, if the new game has it */
static void reinspect(void) {
  if (!S.inspect_path[0] || !pl_running()) return;
  char *rep = pl_call(cw_frame_printf("node %s", pl_token(S.inspect_path)), NULL);
  pj *n = pj_parse(rep);
  if (pj_num(n, "uid", 0) > 0) S.inspected = (int)pj_num(n, "uid", 0);
  pj_free(n);
  free(rep);
}
int pl_inspected(void) { return S.inspected; }

/* ── commands ─────────────────────────────────────────────────────────── */

static bool need_game(cwed_call *c) {
  if (!pl_available()) return cwed_fail(c, "%s", pl_unavailable_why());
  if (pl_running()) return true;
  if (S.r) return cwed_fail(c, "the game is not running (%s): play.relaunch starts it again", cw_remote_status(S.r));
  return cwed_fail(c, "no game is running: play.start (F5) plays the current scene, play.main the main scene");
}

/* the game's reply members, or a failure with its error */
static bool game_reply(cwed_call *c, const char *request) {
  char *err = NULL, *rep = pl_call(request, &err);
  if (!rep) {
    bool r = cwed_fail(c, "the game refused `%s`: %s", request, err ? err : "no reply");
    free(err);
    return r;
  }
  size_t n = strlen(rep);
  if (n >= 2 && rep[0] == '{' && rep[n - 1] == '}') {
    rep[n - 1] = 0;
    if (rep[1]) cwed_json_raw(cwed_reply(c), rep + 1);
  } else {
    cwed_json_key(cwed_reply(c), "reply");
    cwed_json_raw(cwed_reply(c), rep);
  }
  free(rep);
  return true;
}

static void status_json(cwed_json *j) {
  cwed_json_key(j, "available");
  cwed_json_raw(j, pl_available() ? "true" : "false");
  if (!pl_available()) {
    cwed_json_key(j, "reason");
    cwed_json_str(j, pl_unavailable_why());
  }
  cwed_json_key(j, "transport");
  cwed_json_str(j, S.r ? S.transport : pl_available() ? "none" : "unavailable");
  cwed_json_key(j, "state");
  cwed_json_str(j, S.r ? cw_remote_state_name(cw_remote_state_of(S.r)) : "idle");
  cwed_json_key(j, "status");
  cwed_json_str(j, pl_status());
  cwed_json_key(j, "scene");
  cwed_json_str(j, S.scene);
  cwed_json_key(j, "game");
  cwed_json_str(j, S.game);
  cwed_json_printf(j, ",\"tick\":%lld,\"paused\":%s,\"launches\":%d,\"pid\":%d,\"generation\":%d,\"inspected\":%d", S.tick,
                   pl_is_paused() ? "true" : "false", S.r ? cw_remote_launch_count(S.r) : 0, S.r ? cw_remote_pid(S.r) : 0, S.gen,
                   S.inspected);
  cwed_json_key(j, "crash");
  if (S.crash) cwed_json_str(j, S.crash);
  else cwed_json_raw(j, "null");
}

static bool cmd_status(cwed_call *c) {
  refresh_status_now();
  status_json(cwed_reply(c));
  return true;
}

static bool cmd_main(cwed_call *c) {
  const char *m = cwed_project_main_scene();
  if (!m || !*m) return cwed_fail(c, "the project has no main_scene (project.cwproj)");
  char *json = NULL;
  bool ok = cwed_run(cw_frame_printf("play.start scene=%s", cwed_quote(m)), &json);
  if (!ok) {
    pj *r = pj_parse(json);
    cwed_fail(c, "%s", r && pj_str(r, "error") ? pj_str(r, "error") : "play.start failed");
    pj_free(r);
  } else status_json(cwed_reply(c));
  cw_free(json);
  return ok;
}

static bool cmd_relaunch(cwed_call *c) {
  char err[1024] = "";
  if (!pl_available()) return cwed_fail(c, "%s", pl_unavailable_why());
  if (!relaunch(err, sizeof err)) return cwed_fail(c, "%s", err);
  status_json(cwed_reply(c));
  return true;
}

static bool cmd_pause(cwed_call *c) { /* replaces the core's play.pause: it reports a missing game and the tick */
  if (!need_game(c)) return false;
  bool p = cwed_arg_given(c, "on") ? cwed_arg_bool(c, "on") : !S.paused;
  be_set_paused(p);
  if (S.paused != p) return cwed_fail(c, "the game did not %s", p ? "pause" : "resume");
  cwed_json_printf(cwed_reply(c), "\"paused\":%s,\"tick\":%lld", p ? "true" : "false", S.tick);
  return true;
}

static bool cmd_step(cwed_call *c) { /* replaces the core's play.step: it waits for the ticks and reports them */
  if (!need_game(c)) return false;
  int n = (int)cwed_arg_int(c, "ticks");
  if (n < 1) return cwed_fail(c, "ticks must be 1 or more");
  char *err = NULL;
  long long t = do_step(n, &err);
  if (t < 0) {
    bool r = cwed_fail(c, "step failed: %s", err ? err : "?");
    free(err);
    return r;
  }
  cwed_json_printf(cwed_reply(c), "\"tick\":%lld,\"paused\":true", t);
  return true;
}

static const char *node_token(cwed_call *c) {
  const char *n = cwed_arg(c, "node");
  return pl_token(n);
}

static bool cmd_get(cwed_call *c) {
  if (!need_game(c)) return false;
  char props[1024];
  snprintf(props, sizeof props, "%s", cwed_arg(c, "props"));
  for (char *p = props; *p; p++)
    if (*p == ',') *p = ' ';
  return game_reply(c, cw_frame_printf("get %s %s", node_token(c), props));
}

static bool cmd_set(cwed_call *c) {
  if (!need_game(c)) return false;
  return game_reply(c, cw_frame_printf("set %s %s %s", node_token(c), pl_token(cwed_arg(c, "prop")), pl_token(cwed_arg(c, "value"))));
}

static bool cmd_call(cwed_call *c) {
  if (!need_game(c)) return false;
  return game_reply(c, cw_frame_printf("call %s %s %s", node_token(c), pl_token(cwed_arg(c, "method")), cwed_arg(c, "args")));
}

static bool cmd_request(cwed_call *c) {
  if (!need_game(c)) return false;
  char *err = NULL, *rep = pl_call(cwed_arg(c, "text"), &err);
  if (!rep) {
    bool r = cwed_fail(c, "%s", err ? err : "no reply");
    free(err);
    return r;
  }
  cwed_json_key(cwed_reply(c), "reply");
  cwed_json_raw(cwed_reply(c), rep);
  free(rep);
  return true;
}

static bool cmd_inspect(cwed_call *c) {
  if (!need_game(c)) return false;
  char *err = NULL, *rep = pl_call(cw_frame_printf("node %s", node_token(c)), &err);
  if (!rep) {
    bool r = cwed_fail(c, "%s", err ? err : "no reply");
    free(err);
    return r;
  }
  pj *n = pj_parse(rep);
  int uid = (int)pj_num(n, "uid", 0);
  if (uid > 0) pl_inspect(uid);
  cwed_json_printf(cwed_reply(c), "\"uid\":%d", uid);
  const char *path = pj_str(n, "path");
  cwed_json_key(cwed_reply(c), "path");
  cwed_json_str(cwed_reply(c), path ? path : "");
  const char *dp = pl_doc_path(path);
  cwed_json_key(cwed_reply(c), "scene_path");
  if (dp) cwed_json_str(cwed_reply(c), dp);
  else cwed_json_raw(cwed_reply(c), "null");
  pj_free(n);
  free(rep);
  return uid > 0 ? true : cwed_fail(c, "no uid in the game's reply");
}

static bool cmd_tree(cwed_call *c) {
  pl_tree_json(cwed_reply(c));
  return true;
}

static bool cmd_screenshot(cwed_call *c) {
  if (!need_game(c)) return false;
  const char *layers = cwed_arg(c, "layers");
  int scale = (int)cwed_arg_int(c, "scale");
  const char *path = cwed_arg(c, "path");
  void *png = NULL;
  size_t len = 0;
  char *err = NULL;
  pl_game_preview_drain();
#if CW_CART
  path = "";
#endif
  if (!cw_remote_screenshot(S.r, layers, scale, *path ? path : NULL, &png, &len, 0, &err)) {
    bool r = cwed_fail(c, "screenshot failed: %s", err ? err : "?");
    free(err);
    return r;
  }
  pl_game_show_png(png, len, cw_frame_printf("%s at tick %lld", layers, S.tick));
  int w = 0, h = 0;
  if (len > 24 && !memcmp((char *)png + 12, "IHDR", 4)) {
    const unsigned char *b = png;
    w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
    h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
  }
  free(png);
  cwed_json_printf(cwed_reply(c), "\"png_bytes\":%zu,\"width\":%d,\"height\":%d,\"tick\":%lld", len, w, h, S.tick);
  cwed_json_key(cwed_reply(c), "path");
  cwed_json_str(cwed_reply(c), path);
  return true;
}

static bool cmd_crash(cwed_call *c) {
  if (!need_game(c)) return false;
  const char *kind = cwed_arg(c, "kind");
  if (!strcmp(kind, "auto")) kind = strcmp(S.transport, "romdev") ? "segv" : "trap"; /* a null write does not trap in wasm */
  if (strcmp(kind, "segv") && strcmp(kind, "abort") && strcmp(kind, "trap"))
    return cwed_fail(c, "kind is segv, abort or trap, not '%s'", kind);
  cwed_log(CWED_LOG_WARN, "play: crashing the game on purpose (debug_crash %s)", kind);
  char *err = NULL, *rep = cw_remote_call(S.r, cw_frame_printf("debug_crash %s", kind), 5000, &err);
  free(rep);
  free(err);
  for (int i = 0; i < 2000 && pl_running(); i++) {
    cw_remote_pump(S.r);
    if (pl_running()) {
#if !CW_CART
      usleep(5000);
#endif
    }
  }
  report_end();
  if (pl_running()) return cwed_fail(c, "the game survived debug_crash %s", kind);
  status_json(cwed_reply(c));
  return true;
}

/* play.apply: a live value into the edited scene, as one undoable step */
typedef struct { cw_node node; const char *prop; cw_value old, now; bool have_now; } apply_state;

static bool parse_for(cwed_call *c, cw_node n, const char *prop, const char *text, cw_value *out) {
  const cw_prop *p = cw_node_find_prop(n, prop, NULL);
  if (!p) return cwed_fail(c, "%s (%s) has no property '%s'", cw_node_name(n), cw_node_get_type(n)->name, prop);
  if (p->flags & CW_PROP_READONLY) return cwed_fail(c, "%s is read only (computed by the engine)", prop);
  memset(out, 0, sizeof *out);
  if (p->type == CW_T_ENUM && p->enum_names) {
    for (int k = 0; p->enum_names[k]; k++)
      if (!strcmp(p->enum_names[k], text)) {
        out->type = CW_T_ENUM;
        out->i = k;
        return true;
      }
  }
  if (p->type == CW_T_STRING && text[0] != '"') {
    *out = cw_val_string(text);
    return true;
  }
  if (cw_value_parse(text, p->type, out)) return true;
  return cwed_fail(c, "'%s' is not a value of %s", text, prop);
}

static bool apply_exec(cwed_call *c) {
  apply_state *s = cwed_call_state(c);
  if (s && s->have_now) { /* redo: the value the first run applied, game or not */
    if (!cw_node_valid(s->node) || !cw_node_set(s->node, s->prop, s->now)) return cwed_fail(c, "redo: the node is gone");
    return true;
  }
  cw_node n = cwed_arg_node(c, "path");
  if (CW_HANDLE_IS_NULL(n)) return false;
  const char *prop = cwed_arg(c, "prop");
  char text[2048];
  if (cwed_arg_given(c, "value")) snprintf(text, sizeof text, "%s", cwed_arg(c, "value"));
  else {
    if (!need_game(c)) return false;
    cwed_doc *d = cwed_call_doc(c);
    if (d != pl_doc())
      return cwed_fail(c, "the game plays %s, not this scene: switch to it, or pass value=", *S.scene ? S.scene : "nothing");
    const char *dp = cwed_doc_node_path(d, n);
    const char *rp = cw_frame_printf("/root/%s%s%s", cw_node_name(cwed_doc_root(d)), strcmp(dp, ".") ? "/" : "", strcmp(dp, ".") ? dp : "");
    char *err = NULL, *rep = pl_call(cw_frame_printf("get %s %s", pl_token(rp), pl_token(prop)), &err);
    if (!rep) {
      bool r = cwed_fail(c, "reading %s.%s from the game: %s", rp, prop, err ? err : "?");
      free(err);
      return r;
    }
    pj *v = pj_parse(rep);
    const char *val = pj_str(v, "value");
    snprintf(text, sizeof text, "%s", val ? val : "");
    pj_free(v);
    free(rep);
    if (!val) return cwed_fail(c, "the game gave no value for %s.%s", rp, prop);
  }
  cw_value v, old;
  if (!parse_for(c, n, prop, text, &v)) return false;
  if (!cw_node_get_prop(n, prop, &old)) return cwed_fail(c, "cannot read %s", prop);
  if (!cw_node_set(n, prop, v)) return cwed_fail(c, "%s refused the value (see the log)", prop);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  s->node = n;
  s->prop = cw_intern(prop);
  s->old = old;
  cw_node_get_prop(n, prop, &s->now);
  s->have_now = true;
  char buf[2048];
  cw_value_format(&s->now, buf, sizeof buf);
  cwed_json_key(cwed_reply(c), "value");
  cwed_json_str(cwed_reply(c), buf);
  return true;
}

static void apply_undo(cwed_call *c) {
  apply_state *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node)) cw_node_set(s->node, s->prop, s->old);
}

/* rects of the play UI (tests and agents that drive it by pointer) */
static bool find_button(cw_node n, void *u) {
  void **a = u;
  if (cw_node_is(n, "Button") && !strcmp(cw_button_text(n), (const char *)a[0]) && CW_HANDLE_IS_NULL(*(cw_node *)a[1]) &&
      cw_control_global_rect(n).w > 0) {
    *(cw_node *)a[1] = n;
    return false;
  }
  return true;
}
static cw_rect tab_rect(const char *panel_id) {
  cw_node content = cwed_panel_content(panel_id);
  if (CW_HANDLE_IS_NULL(content)) return (cw_rect){ 0 };
  /* the panel content is the dock page itself: its parent is the slot's TabContainer */
  cw_node tabs = cw_node_parent(content);
  if (!cw_node_valid(tabs) || !cw_node_is(tabs, "TabContainer")) return (cw_rect){ 0 };
  int index = -1;
  for (int i = 0; i < cw_tab_container_page_count(tabs); i++)
    if (cw_tab_container_page(tabs, i).id == content.id) index = i;
  cw_rect r = cw_tab_container_tab_rect(tabs, index);
  cw_rect g = cw_control_global_rect(tabs);
  return (cw_rect){ g.x + r.x, g.y + r.y, r.w, r.h };
}
static bool cmd_widgets(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "toolbar");
  cwed_json_raw(j, "{");
  const char *const names[] = { "Play", "Pause", "Step", "Stop", NULL };
  for (int i = 0; names[i]; i++) {
    cw_node b = { 0 };
    void *a[2] = { (void *)names[i], &b };
    cw_node_walk(cw_tree_root(cwed_ui_tree()), find_button, a);
    pl_rect_json(j, names[i], cw_node_valid(b) ? cw_control_global_rect(b) : (cw_rect){ 0 });
  }
  cwed_json_raw(j, "}");
  cwed_json_key(j, "tabs");
  cwed_json_raw(j, "{");
  const char *const panels[] = { "game", "remote_tree", "live", "output", NULL };
  for (int i = 0; panels[i]; i++) pl_rect_json(j, panels[i], tab_rect(panels[i]));
  cwed_json_raw(j, "}");
  pl_game_widgets(j);
  pl_tree_widgets(j);
  pl_insp_widgets(j);
  return true;
}

/* ── module ───────────────────────────────────────────────────────────── */

static void on_project(const cwed_event *e, void *u) {
  (void)e, (void)u;
  read_project();
}

static void init(void) {
#if !CW_CART
  sync_mode = getenv("CW_FRAMES") != NULL; /* a test run: frame-exact, so requests finish in the frame */
#endif
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.transport", .type = CWED_ARG_STRING, .def = "auto",
    .choices = "auto|native|romdev",
    .doc = "How Play runs the game: auto (native when the project's game is built natively, else its cart in romdev), "
           "native (a child process), romdev (the .wasc in romdev, 127.0.0.1:7331 or $ROMDEV_URL)." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.start_paused", .type = CWED_ARG_BOOL, .def = "false",
    .doc = "Start the game paused at tick 1 (step it with F6, resume with F7)." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.headless", .type = CWED_ARG_BOOL, .def = "false",
    .doc = "Run a native game without its own window (frames show in the Game panel only)." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.preview", .type = CWED_ARG_STRING, .def = "auto", .choices = "auto|on|off",
    .doc = "Show the game's frames in the Game panel: auto = when the game has no window of its own (romdev, headless)." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.poll_frames", .type = CWED_ARG_INT, .def = "6", .min = 1, .max = 120,
    .doc = "Editor frames between refreshes of the Remote tree and the Live inspector." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.game_exe", .type = CWED_ARG_STRING, .def = "",
    .doc = "The native game to run (overrides the project's game_exe / game_app)." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.game_cart", .type = CWED_ARG_STRING, .def = "",
    .doc = "The game cart (.wasc) romdev runs (overrides the project's game_cart / game_app)." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "play.sync_requests", .type = CWED_ARG_BOOL, .def = "false", .hidden = true,
    .doc = "Finish every request to the game inside the frame that sent it (on by itself in CW_FRAMES test runs)." });
  cwed_play_register(&BACKEND);
  read_project();
  cwed_subscribe(CWED_EV_PROJECT, on_project, NULL);

  static const cwed_param pause_p[] = { { "on", CWED_ARG_BOOL, "true", "pause (true) or resume (false); default toggles" }, { 0 } };
  static const cwed_param step_p[] = { { "ticks", CWED_ARG_INT, "1", "fixed ticks to run, then stay paused" }, { 0 } };
  static const cwed_param node_p[] = { { "node", CWED_ARG_STRING, NULL, "#uid, /root/Level/Ball or Level/Ball (the game's tree)" }, { 0 } };
  static const cwed_param get_p[] = { { "node", CWED_ARG_STRING, NULL, "#uid or path in the game" },
                                      { "props", CWED_ARG_STRING, NULL, "property names, comma or space separated" }, { 0 } };
  static const cwed_param set_p[] = { { "node", CWED_ARG_STRING, NULL, "#uid or path in the game" },
                                      { "prop", CWED_ARG_STRING, NULL, "property name" },
                                      { "value", CWED_ARG_STRING, NULL, "value as a scene file writes it: 0.5, vec3(0, 1, 0), \"text\", ENUM" },
                                      { 0 } };
  static const cwed_param call_p[] = { { "node", CWED_ARG_STRING, NULL, "#uid or path in the game" },
                                       { "method", CWED_ARG_STRING, NULL, "method name (methods <node> lists them)" },
                                       { "args", CWED_ARG_STRING, "", "arguments, space separated" }, { 0 } };
  static const cwed_param req_p[] = { { "text", CWED_ARG_STRING, NULL, "a debug-channel request (help lists them)" }, { 0 } };
  static const cwed_param shot_p[] = { { "layers", CWED_ARG_STRING, "all", "world, ui, debug or all, joined with +" },
                                       { "scale", CWED_ARG_INT, "1", "1/scale size (1..16)" },
                                       { "path", CWED_ARG_STRING, "", "also write the PNG here (native)" }, { 0 } };
  static const cwed_param crash_p[] = { { "kind", CWED_ARG_STRING, "auto", "segv, abort, trap; auto = segv natively, trap in a cart" }, { 0 } };
  static const cwed_param apply_p[] = { { "path", CWED_ARG_PATH, NULL, "the node in the edited scene" },
                                        { "prop", CWED_ARG_STRING, NULL, "property name" },
                                        { "value", CWED_ARG_VALUE, "", "the value; omitted = the running game's value now" },
                                        { 0 } };
  const uint32_t Q = CWED_CMD_QUERY | CWED_CMD_HIDDEN;
  cwed_command_register(&(cwed_command_desc){ .name = "play.status", .title = "Game Status", .flags = Q,
    .doc = "The play session: available, transport, state, status line, scene, game, tick, paused, launches, pid, crash report.",
    .exec = cmd_status });
  cwed_command_register(&(cwed_command_desc){ .name = "play.main", .title = "Play Main Scene", .shortcut = "shift+f5",
    .doc = "Run the project's main scene as the game (play.start does the current scene).", .exec = cmd_main });
  cwed_command_register(&(cwed_command_desc){ .name = "play.relaunch", .title = "Relaunch Game",
    .doc = "Start the game again from the same scene (after a crash, or to drop live edits).", .exec = cmd_relaunch });
  cwed_command_register(&(cwed_command_desc){ .name = "play.pause", .title = "Pause Game", .params = pause_p, .shortcut = "f7",
    .doc = "Pause or resume the running game (the debugger's pause: no ticks, frames still render). Replaces the core's "
           "play.pause so a missing game is an error and the reply carries the tick.",
    .exec = cmd_pause });
  cwed_command_register(&(cwed_command_desc){ .name = "play.step", .title = "Step Game", .params = step_p, .shortcut = "f6",
    .doc = "Pause the game and run exactly N fixed ticks; replies with the tick reached. Replaces the core's play.step "
           "(which could not report).",
    .exec = cmd_step });
  cwed_command_register(&(cwed_command_desc){ .name = "play.get", .title = "Get Live Values", .params = get_p, .flags = Q,
    .doc = "Read properties of a node in the running game (one round trip).", .exec = cmd_get });
  cwed_command_register(&(cwed_command_desc){ .name = "play.set", .title = "Set Live Value", .params = set_p,
    .flags = CWED_CMD_HIDDEN,
    .doc = "Change a property in the RUNNING game (type-checked by the game; not the edited scene: play.apply keeps a "
           "value in the scene).",
    .exec = cmd_set });
  cwed_command_register(&(cwed_command_desc){ .name = "play.call", .title = "Call Game Method", .params = call_p,
    .flags = CWED_CMD_HIDDEN, .doc = "Invoke a method on a node in the running game.", .exec = cmd_call });
  cwed_command_register(&(cwed_command_desc){ .name = "play.request", .title = "Game Request", .params = req_p,
    .flags = CWED_CMD_HIDDEN, .doc = "Send any debug-channel request to the running game; the reply is its JSON.",
    .exec = cmd_request });
  cwed_command_register(&(cwed_command_desc){ .name = "play.inspect", .title = "Inspect Game Node", .params = node_p,
    .flags = CWED_CMD_HIDDEN,
    .doc = "Show a node of the running game in the Remote tree and the Live inspector; replies with its uid, game path "
           "and path in the edited scene (null when it is not in it).",
    .exec = cmd_inspect });
  cwed_command_register(&(cwed_command_desc){ .name = "play.tree", .title = "Remote Tree", .flags = Q,
    .doc = "The running game's tree as the Remote panel has it (synced with changes <revision>).", .exec = cmd_tree });
  cwed_command_register(&(cwed_command_desc){ .name = "play.screenshot", .title = "Capture Game Layers", .params = shot_p,
    .flags = CWED_CMD_HIDDEN,
    .doc = "Capture the game's next frame with only the given layers; it shows in the Game panel (and is written to path).",
    .exec = cmd_screenshot });
  cwed_command_register(&(cwed_command_desc){ .name = "play.crash", .title = "Crash Game (test)", .params = crash_p,
    .flags = CWED_CMD_HIDDEN,
    .doc = "Crash the running game on purpose (debug_crash) to see the crash report; the editor stays up.",
    .exec = cmd_crash });
  cwed_command_register(&(cwed_command_desc){ .name = "play.apply", .title = "Keep Live Value in Scene", .params = apply_p,
    .flags = CWED_CMD_UNDO | CWED_CMD_SCENE,
    .doc = "Copy a value from the running game into the edited scene as one undoable step (value omitted: read it from "
           "the game now; the node is the same path under the played scene's root).",
    .example = "play.apply path={sel} prop=visible value=false", .exec = apply_exec, .undo = apply_undo });
  cwed_command_register(&(cwed_command_desc){ .name = "play.widgets", .title = "Play UI Rects", .flags = Q,
    .doc = "Where the play controls are on screen (toolbar buttons, panel tabs, Game buttons, Remote rows by uid, Live "
           "rows by property): for tests and agents that drive the editor by pointer.",
    .exec = cmd_widgets });
  cwed_menu_add("Play/Play Main Scene", "play.main", 100);
  cwed_menu_add("Play/Relaunch", "play.relaunch", 104);
  cwed_menu_add("Play/Capture Layers", "play.screenshot", 200);
  cwed_menu_add("Play/Crash Game (test)", "play.crash", 210);

  pl_game_init();
  pl_tree_init();
  pl_insp_init();
  status_bar();
}

static void frame(double dt) {
  (void)dt;
  pl_tree_frame();
  pl_insp_frame();
  pl_game_frame();
}

static void shutdown(void) {
  if (S.r) cw_remote_close(S.r);
  S.r = NULL;
}

CWED_MODULE(play) = { .title = "Play in Editor", .milestone = "M4.3", .init = init, .frame = frame, .shutdown = shutdown };

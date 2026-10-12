/* cwhot: the native hot-reload host.
 *
 *   cwhot path/to/app_hot.so [args...]
 *
 * Runs an app built as a loadable module (cw_add_app makes <name>_hot.so next
 * to the executable) and swaps in new code when the module file changes:
 * edit, rebuild (ninja), and the running game picks it up within half a
 * second, state intact.
 *
 * What survives a reload: everything in engine memory (the scene tree, nodes,
 * their data, resources, physics worlds) and the app's `user` pointer. What
 * does not: the module's own static variables (keep game state in nodes or
 * behind `user`). Node types re-register themselves with the new callbacks
 * (cw_node_type_register keeps the data layout and swaps behaviour); a module
 * may export `void cw_hot_reload(void)` to re-register types it registers
 * outside init.
 *
 * A module may also export `void cw_game_register_types(void)`: it registers
 * every node type and method the game defines and nothing else; cwhot calls
 * it after each reload and the editor calls it to open the game's scenes
 * (editor/modules/code_view).
 *
 * A module that fails to load (a half-written file, a missing symbol) is
 * reported and the running code stays; the next change is tried again. The
 * file is polled every 6 frames and swapped once its size and mtime held
 * still for one poll (a linker writing it is never caught midway).
 *
 * Debug channel (the editor's play-in-editor sends these after a build):
 *   hot_reload [path]   swap to the module now (default: the watched path);
 *                       the reply has generation, load_ms and types; an
 *                       error leaves the old code running
 *   hot_status          module, generation, last error
 *
 * Program tables: three.c draws a material only with a program a table has
 * (cmake/programs.cmake), and cwhot links the engine's alone. A module that
 * exports `int cw__app_program_tables(const t3_gen_table **gl, const
 * t3_gen_table **wgpu, int cap)` (cw_add_app's <app>_hot does: its app's
 * tables) has its tables registered when it loads; each reload swaps the new
 * generation's tables in, so a state the new code adds draws too. Without
 * them a module's materials are skipped draws (cw.three.missing).
 *
 * Test hooks: CW_HOT_SWAP_AT=<frame> + CW_HOT_SWAP_PATH=<module> swap to
 * another module at an exact frame (with CW_FRAMES), so a test needs no file
 * watching. Old modules are never unloaded: nodes may still point at their
 * type descriptors and strings. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene.h"
#include "gen/programs.h" /* t3_gen_table (three.c) */

int cw_native_main(int argc, char **argv, cw_app_desc (*app_fn)(int, char **));
extern void (*cw__native_frame_hook)(void);

static const char *module_path;
static int g_argc;
static char **g_argv;
static cw_app_desc cur;
static struct { long long mtime_ns, size; } seen, loaded;
static char last_error[2400];
static long swap_at = -1;
static const char *swap_path;
static int generation;

/* the module's program tables, by position: registered with three.c once
 * (it keeps the pointers and reads them at each lookup), refilled from every
 * generation; a generation with fewer tables empties the rest. Old modules
 * are never unloaded, so what a slot points at stays valid. */
#define HOT_TABLES 8
static t3_gen_table hot_gl[HOT_TABLES], hot_wgpu[HOT_TABLES];
static bool hot_registered[HOT_TABLES];

static void adopt_tables(void *h) {
  int (*fn)(const t3_gen_table **, const t3_gen_table **, int) =
      (int (*)(const t3_gen_table **, const t3_gen_table **, int))dlsym(h, "cw__app_program_tables");
  const t3_gen_table *gl[HOT_TABLES] = { 0 }, *wgpu[HOT_TABLES] = { 0 };
  int n = fn ? fn(gl, wgpu, HOT_TABLES) : 0;
  if (n > HOT_TABLES) {
    cw_error("cwhot: the module has %d program tables, cwhot takes %d: the rest are not registered", n, HOT_TABLES);
    n = HOT_TABLES;
  }
  for (int i = 0; i < HOT_TABLES; i++) {
    hot_gl[i] = i < n && gl[i] ? *gl[i] : (t3_gen_table){ 0 };
    hot_wgpu[i] = i < n && wgpu[i] ? *wgpu[i] : (t3_gen_table){ 0 };
    if (i < n && !hot_registered[i]) {
      t3_register_program_table(&hot_gl[i], &hot_wgpu[i]);
      hot_registered[i] = true;
    }
  }
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void file_sig(const char *path, long long *mtime_ns, long long *size) {
  struct stat st;
  if (stat(path, &st) != 0) { *mtime_ns = *size = -1; return; }
  *mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
  *size = (long long)st.st_size;
}

/* Load a generation; false (last_error says why, *d untouched) on failure. */
static bool load(const char *path, cw_app_desc *d) {
  /* dlopen caches by path: load each generation from its own copy */
  char copy[1100];
  snprintf(copy, sizeof copy, "%s.gen%d", path, generation);
  FILE *in = fopen(path, "rb"), *out = in ? fopen(copy, "wb") : NULL;
  if (!in || !out) {
    snprintf(last_error, sizeof last_error, "cannot copy %s to %s", path, copy);
    if (in) fclose(in);
    return false;
  }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  void *h = dlopen(copy, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    snprintf(last_error, sizeof last_error, "%s", dlerror());
    remove(copy);
    return false;
  }
  cw_app_desc (*fn)(int, char **) = (cw_app_desc(*)(int, char **))dlsym(h, "cw_main");
  if (!fn) {
    snprintf(last_error, sizeof last_error, "%s has no cw_main", path);
    return false;
  }
  generation++;
  adopt_tables(h);
  void (*reload)(void) = (void (*)(void))dlsym(h, "cw_hot_reload");
  void (*types)(void) = (void (*)(void))dlsym(h, "cw_game_register_types");
  *d = fn(g_argc, g_argv);
  if (generation > 1) {
    if (types) types();
    if (reload) reload();
  }
  last_error[0] = 0;
  return true;
}

static void cmd_hot_reload(int argc, const char **argv, cw_reply *r);
static void cmd_hot_status(int argc, const char **argv, cw_reply *r);

/* forwarders: the platform holds these; they call whatever code is current */
static void f_init(void *u) {
  cw_debug_command("hot_reload", "hot_reload [module.so]: swap in rebuilt code now (cwhot)", cmd_hot_reload);
  cw_debug_command("hot_status", "hot_status: the module cwhot runs, its generation, the last load error", cmd_hot_status);
  if (cur.init) cur.init(u);
}
static void f_fixed(void *u, double dt) { if (cur.fixed_update) cur.fixed_update(u, dt); }
static void f_update(void *u, double dt) { if (cur.update) cur.update(u, dt); }
static void f_render(void *u, double a) { if (cur.render) cur.render(u, a); }
static void f_shutdown(void *u) { if (cur.shutdown) cur.shutdown(u); }
static void f_event(void *u, const cw_event *e) { if (cur.event) cur.event(u, e); }

static bool swap(const char *path) {
  void *user = cur.user;
  cw_app_desc d;
  if (!load(path, &d)) {
    cw_error("cwhot: %s did not load, the running code stays: %s", path, last_error);
    return false;
  }
  /* callbacks from the new code; the running app's user pointer stays */
  cur.init = d.init;
  cur.fixed_update = d.fixed_update;
  cur.update = d.update;
  cur.render = d.render;
  cur.shutdown = d.shutdown;
  cur.event = d.event;
  cur.user = user;
  cw_info("cwhot: reloaded %s (generation %d)", path, generation - 1);
  return true;
}

static void cmd_hot_reload(int argc, const char **argv, cw_reply *r) {
  const char *path = argc > 1 ? argv[1] : module_path;
  double t0 = now_ms();
  int types_before = cw_node_type_count();
  if (!swap(path)) {
    cw_reply_error(r, "hot_reload %s failed, the old code keeps running: %s (fix the module and send hot_reload again)", path,
                   last_error);
    return;
  }
  if (!strcmp(path, module_path)) file_sig(module_path, &loaded.mtime_ns, &loaded.size), seen = loaded;
  cw_reply_printf(r, "{\"generation\":%d,\"load_ms\":%.2f,\"new_types\":%d,\"module\":", generation - 1,
                  now_ms() - t0, cw_node_type_count() - types_before);
  cw_reply_str(r, path);
  cw_reply_raw(r, "}");
}

static void cmd_hot_status(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  cw_reply_printf(r, "{\"generation\":%d,\"module\":", generation - 1);
  cw_reply_str(r, module_path);
  cw_reply_raw(r, ",\"last_error\":");
  cw_reply_str(r, last_error);
  cw_reply_raw(r, "}");
}

static void frame_hook(void) {
  long f = (long)cw_frame_index();
  if (swap_at >= 0 && f == swap_at && swap_path) {
    swap(swap_path);
    return;
  }
  if (f % 6) return; /* ten polls a second */
  long long m, s;
  file_sig(module_path, &m, &s);
  bool changed = m != loaded.mtime_ns || s != loaded.size;
  bool still = m == seen.mtime_ns && s == seen.size;
  seen.mtime_ns = m, seen.size = s;
  if (m < 0 || !changed || !still) return; /* missing, unchanged, or still being written */
  loaded = seen;
  swap(module_path);
}

static cw_app_desc app(int argc, char **argv) {
  (void)argc;
  (void)argv;
  file_sig(module_path, &loaded.mtime_ns, &loaded.size);
  seen = loaded;
  if (!load(module_path, &cur)) {
    fprintf(stderr, "cwhot: %s\n", last_error);
    exit(1);
  }
  cw_app_desc d = cur;
  d.init = f_init;
  d.fixed_update = f_fixed;
  d.update = f_update;
  d.render = f_render;
  d.shutdown = f_shutdown;
  d.event = cur.event ? f_event : NULL;
  return d;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: cwhot <app_hot.so> [args...]\n");
    return 2;
  }
  module_path = argv[1];
  g_argc = argc - 1;
  g_argv = argv + 1;
  if (getenv("CW_HOT_SWAP_AT")) swap_at = strtol(getenv("CW_HOT_SWAP_AT"), NULL, 0);
  swap_path = getenv("CW_HOT_SWAP_PATH");
  cw__native_frame_hook = frame_hook;
  return cw_native_main(g_argc, g_argv, app);
}

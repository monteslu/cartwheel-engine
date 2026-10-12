/* The scene runner: an app that runs a tree with a main scene. */
#include <stdlib.h>
#include <time.h>

#include "scene_internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/vfs.h"

extern void (*cw__debug_status_hook)(cw_reply *r); /* src/debug/channel.c */

static struct {
  cw_app_desc base;
  const char *main_scene;
  cw_tree *tree;
  int32_t node_count;
  uint32_t tick;
  int32_t draw_calls;
  cw_node current;         /* the main scene's instance (what change_scene replaces) */
  const char *scene_path;  /* interned */
} run;

cw_tree *cw_scene_app_tree(void) { return run.tree; }
const char *cw_scene_app_current_scene(void) { return run.scene_path; }

/* the scene's text: the project layer (cartwheel/vfs.h: an editor's unsaved
 * or in-cart edits) first, then the assets */
static cw_packed_scene *load_scene(const char *path, bool fresh) {
  if (!fresh && !cw_vfs_exists(path)) return cw_scene_load(path);
  size_t len;
  char *text = cw_vfs_read(path, &len);
  if (!text) {
    cw_error("scene '%s' is in neither the project layer nor the assets", path);
    return NULL;
  }
  cw_packed_scene *ps = cw_scene_parse(text, path);
  free(text);
  if (ps) cw__scene_cache_put(path, ps); /* later instances (ext_scene, spawn) see the new text */
  return ps;
}

bool cw_scene_app_change_scene(const char *path, bool reread) {
  if (!run.tree || !path || !*path) return false;
  cw_packed_scene *ps = load_scene(path, reread);
  if (!ps) return false;
  cw_node s = cw_scene_instance(ps);
  if (CW_HANDLE_IS_NULL(s)) return false;
  cw_node root = cw_tree_root(run.tree);
  if (cw_node_valid(run.current)) {
    cw_node_free(run.current);
  } else { /* no main scene (built in init): everything under the root goes */
    while (cw_node_child_count(root)) cw_node_free(cw_node_child(root, cw_node_child_count(root) - 1));
  }
  cw_node_add_child(root, s);
  run.current = s;
  run.scene_path = cw_intern(path);
  run.node_count = cw_tree_node_count(run.tree);
  return true;
}

static void cmd_change_scene(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: change_scene <scene.cwscene>   e.g. change_scene levels/level2.cwscene"); return; }
  if (!cw_scene_app_change_scene(argv[1], false)) {
    cw_reply_error(r, "change_scene %s failed (the log says why; vfs ls lists the project layer, res lists loaded assets)", argv[1]);
    return;
  }
  cw_reply_raw(r, "{\"scene\":");
  cw_reply_str(r, run.scene_path);
  cw_reply_printf(r, ",\"uid\":%llu,\"nodes\":%d}", (unsigned long long)cw_node_uid(run.current), cw_tree_node_count(run.tree));
}

static void cmd_reload_scene(int argc, const char **argv, cw_reply *r) {
  const char *path = argc > 1 ? argv[1] : run.scene_path;
  if (!path) { cw_reply_error(r, "this app has no main scene to reload: name one (reload_scene <scene.cwscene>)"); return; }
  if (!cw_scene_app_change_scene(path, true)) {
    cw_reply_error(r, "reload_scene %s failed: the text did not parse (the log has the line number)", path);
    return;
  }
  cw_reply_raw(r, "{\"scene\":");
  cw_reply_str(r, run.scene_path);
  cw_reply_printf(r, ",\"uid\":%llu,\"nodes\":%d,\"reread\":true}", (unsigned long long)cw_node_uid(run.current),
                  cw_tree_node_count(run.tree));
}

/* status: the tree's side of the debugger's picture */
static void status_hook(cw_reply *r) {
  if (!run.tree) return;
  cw_reply_printf(r, ",\"tree_paused\":%s,\"tree_tick\":%llu,\"nodes\":%d", cw_tree_paused(run.tree) ? "true" : "false",
                  (unsigned long long)cw_tree_tick(run.tree), cw_tree_node_count(run.tree));
  if (run.tree->changes) cw_reply_printf(r, ",\"revision\":%llu", (unsigned long long)(run.tree->changes->rev - 1));
  if (run.scene_path) {
    cw_reply_raw(r, ",\"scene\":");
    cw_reply_str(r, run.scene_path);
  }
}

static void r_init(void *u) {
  (void)u;
  cw_scene_register_builtin_types();
  run.tree = cw_tree_new();
  cw_set_current_tree(run.tree);
  cw_tree_seed(run.tree, cw_host_seed() ? cw_host_seed() : 0x5eed);
  if (run.base.debug) {
    cw_debug_i32("tree.nodes", &run.node_count);
    cw_debug_u32("tree.tick", &run.tick);
    cw_debug_i32("render.draw_calls", &run.draw_calls);
  }
  if (run.main_scene) {
    cw_node s = cw_scene_instance_path(run.main_scene);
    if (CW_HANDLE_IS_NULL(s)) cw_error("main scene '%s' failed to load", run.main_scene);
    else cw_node_add_child(cw_tree_root(run.tree), s);
    run.current = s;
    run.scene_path = cw_intern(run.main_scene);
  }
  cw__debug_status_hook = status_hook;
  cw_debug_command("change_scene", "change_scene <scene.cwscene>: replace the main scene (the project layer's copy wins over the asset)", cmd_change_scene);
  cw_debug_command("reload_scene", "reload_scene [scene.cwscene]: re-read the scene's text (project layer, then assets) and "
                                   "restart it; default the current scene", cmd_reload_scene);
  if (run.base.init) run.base.init(run.base.user);
  run.node_count = cw_tree_node_count(run.tree);
}

static void r_fixed(void *u, double dt) {
  (void)u;
  cw_tree_fixed_update(run.tree, dt);
  /* the app callback is game simulation too: it pauses with the tree */
  if (run.base.fixed_update && !cw_tree_paused(run.tree)) run.base.fixed_update(run.base.user, dt);
  run.tick = (uint32_t)cw_tree_tick(run.tree);
}

static void r_update(void *u, double dt) {
  (void)u;
  cw_tree_update(run.tree, dt);
  if (run.base.update) run.base.update(run.base.user, dt);
  run.node_count = cw_tree_node_count(run.tree);
}

static void warm_render(int w, int h);

static void r_render(void *u, double alpha) {
  (void)u;
  cw_tree_pre_render(run.tree, alpha);
  int w, h;
  cw_drawable_size(&w, &h);
  if (cw_gpu_backend() != CW_GPU_WEBGPU) glViewport(0, 0, w, h);
  if (cw_loading_active()) { /* the warm-up's shader phase (below) */
    warm_render(w, h);
    return;
  }
  /* the tree's render pipeline (pipeline.h): 3D (only when a Camera3D
   * exists: a 2D-only tree never makes a three.c renderer) and any post
   * chain, then the overlay phase: the "canvas" pass (the tree's 2D pass and
   * its UI overlay passes, cartwheel/scene2d.h), then the app's render as
   * the "app" pass, so HUDs land on top of everything */
  cw_pipeline_set_app_render(cw_pipeline_of(run.tree), run.base.render, run.base.user);
  cw_pipeline_render(run.tree, w, h, alpha);
  cw_render3d_stats st;
  cw_render3d_get_stats(run.tree, &st);
  run.draw_calls = st.draw_calls;
}

/* ── the warm-up's shader phase ─────────────────────────────────────
 * After the app's own warm-up (textures, scenes), the tree's 3D scene is
 * rendered under the loading screen a slice at a time, so its programs
 * compile a few per frame instead of all in one long first frame. The first
 * slice draws no objects (the pipeline's own programs: sky, shadows, post);
 * then objects are revealed in drawing order, one a slice while each slice
 * still builds new programs (one heavy lit program can take 100 ms on a
 * slow phone), doubling up to 16 a slice while none appear; a frame renders
 * slices until its time budget is spent (warm_render). The last
 * frame adds the app's own render (its HUD: fonts bake, 2D programs). */
enum { WARM_MAX_FRAMES = 240 }; /* a backstop: the rest goes in one frame */
static struct {
  bool app_done, done, all_shown, final, started;
  int frame, base_done, base_total;
  int shown, step, programs;
  t3_object **objs;
  int n, cap;
} warm;

static void warm_collect(t3_object *o, void *ctx) {
  (void)ctx;
  switch (o->type) {
  case T3_MESH: case T3_INSTANCED_MESH: case T3_SKINNED_MESH: case T3_LINE: case T3_LINE_SEGMENTS:
  case T3_LINE_LOOP: case T3_SPRITE: case T3_POINTS:
    if (warm.n == warm.cap) {
      warm.cap = warm.cap ? warm.cap * 2 : 256;
      warm.objs = realloc(warm.objs, sizeof *warm.objs * (size_t)warm.cap);
    }
    warm.objs[warm.n++] = o;
    break;
  default:
    break;
  }
}

static bool r_warmup(void *u) {
  (void)u;
  if (!warm.app_done) {
    if (run.base.warmup && !run.base.warmup(run.base.user)) return false;
    warm.app_done = true;
    cw_load_stage st;
    cw_loading_get(&warm.base_done, &warm.base_total, &st);
  }
  if (warm.done || !cw_node_valid(cw_render3d_camera(run.tree))) return true;
  /* the bar: the objects shown so far, in 100 parts after the app's steps */
  int total = warm.base_total + 101;
  /* (a deterministic run calls this to the end inside one frame, with no renders between) */
  if (warm.final || warm.frame > WARM_MAX_FRAMES) {
    warm.done = true;
    free(warm.objs);
    warm.objs = NULL;
    warm.n = warm.cap = 0;
    cw_loading_set(total, total, CW_LOAD_SHADERS);
    return true;
  }
  /* frame 0 only names the stage (it shows before the first compile) */
  cw_loading_set(warm.base_total + (warm.n ? 100 * warm.shown / warm.n : 0), total, CW_LOAD_SHADERS);
  warm.frame++;
  return false;
}

/* a warm-up render: the slices of the scene this frame compiles. A frame
 * keeps rendering slices while it has time left (WARM_BUDGET_MS of wall
 * clock), so a fast machine is through the phase in a few frames while a slow
 * one still shows the bar move; at least one slice a frame either way. */
enum { WARM_BUDGET_MS = 12 };
static double warm_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}
static void warm_slice(int w, int h) {
  cw_render3d_stats st;
  cw_render3d_get_stats(run.tree, &st);
  if (!warm.started) warm.started = true, warm.shown = 0, warm.step = 1;
  else if (!warm.all_shown) {
    warm.step = st.programs != warm.programs ? 1 : warm.step < 16 ? warm.step * 2 : 16;
    warm.shown += warm.step;
  }
  warm.programs = st.programs;
  bool last = warm.all_shown || warm.frame >= WARM_MAX_FRAMES;
  cw_pipeline_set_app_render(cw_pipeline_of(run.tree), last ? run.base.render : NULL, run.base.user);
  t3_scene *sc = cw_render3d_scene(run.tree);
  warm.n = 0;
  if (sc) t3_object_traverse(sc, warm_collect, NULL);
  int show = last ? warm.n : warm.shown;
  if (show >= warm.n) warm.shown = warm.n, warm.all_shown = true;
  bool *was = malloc(sizeof *was * (size_t)(warm.n ? warm.n : 1));
  for (int i = 0; i < warm.n; i++) {
    was[i] = warm.objs[i]->visible;
    if (i >= show) warm.objs[i]->visible = false;
  }
  cw_pipeline_render(run.tree, w, h, 0);
  for (int i = 0; i < warm.n; i++) warm.objs[i]->visible = was[i];
  free(was);
  if (last) warm.final = true;
}
static void warm_render(int w, int h) {
  if (warm.frame < 2) return;
  double t0 = warm_now_ms();
  do warm_slice(w, h);
  while (!warm.final && warm_now_ms() - t0 < WARM_BUDGET_MS);
  /* the bar: the objects shown so far */
  cw_load_stage st;
  int done, total;
  cw_loading_get(&done, &total, &st);
  if (!warm.final && warm.n) cw_loading_set(warm.base_total + 100 * warm.shown / warm.n, total, st);
}

static void r_shutdown(void *u) {
  (void)u;
  if (run.base.shutdown) run.base.shutdown(run.base.user);
  cw_tree_free(run.tree);
  run.tree = NULL;
}

cw_app_desc cw_scene_app(const char *main_scene, const cw_app_desc *base) {
  if (base) run.base = *base;
  run.main_scene = main_scene;
  cw_app_desc d = run.base;
  d.init = r_init;
  d.fixed_update = r_fixed;
  d.update = r_update;
  d.render = r_render;
  d.shutdown = r_shutdown;
  d.warmup = r_warmup; /* the app's warm-up (if any), then the shader phase */
  d.user = NULL;
  return d;
}

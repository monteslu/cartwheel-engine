/* Plugin manager (M5.2): the project's plugins (plugins/<name>/plugin.cwplugin,
 * cartwheel/plugin.h) in the editor.
 *
 * Native editor: each enabled plugin's RUNTIME part (node types, ...) and
 * EDITOR part (a CWED_PLUGIN module) are shared libraries the project build
 * makes (<project>/.cwbuild/native/plugins/<name>/<name>_{runtime,editor}.so,
 * `node tools/cwproject.mjs build <dir> plugins`, or plugin.build). They load
 * when the project opens: a copy of each file is dlopen'ed (a fresh path per
 * load, so a rebuilt library is new code), the API version the code was
 * compiled with is checked, the runtime part registers through
 * cw_plugin_load and the editor part's init runs under its registration
 * owner. Hot reload: the built files are watched (plugins.hot_reload); when
 * one changed and held still for a poll, the editor part is shut down, its
 * registrations withdrawn (cwed_owner_remove) and the new code initialised;
 * a runtime part re-registers (node types keep their layout: cartwheel/
 * scene.h). Old code is never unmapped: undo entries of a plugin's commands
 * and nodes made before a reload still point at it.
 *
 * Cart editor: no dlopen; the project build compiles the plugins in (the
 * engine's table for runtime parts, cwed__static_plugins for editor parts).
 * Enable/disable still work (registrations withdrawn and made again); new
 * code needs a rebuilt cart.
 *
 * Disable: open scenes using the plugin's node types are closed first
 * (refused when one has unsaved changes), the editor part is withdrawn, the
 * node types are withdrawn from the registry (cw_plugin_unload), and the
 * manifest gets enabled = false. A scene using its types is then refused by
 * scene.open with a message naming the plugin: never opened lossy.
 *
 * Commands: plugin.list, plugin.info, plugin.enable, plugin.disable,
 * plugin.reload, plugin.build, plugin.build_status, plugin.scan. Panel:
 * Plugins (bottom, after Output). Debug fields plugins.ed_* (list, loaded
 * parts, errors, reloads, a state hash). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/process.h"
#include "cartwheel/project.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation" /* messages are cut to their buffers on purpose */
#endif

#if !CW_CART && !defined(_WIN32)
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#define NATIVE_LOAD 1
#else
#define NATIVE_LOAD 0
#endif

/* The project editor's compiled-in editor plugins (a cart editor built by
 * the project build generates this table; the default editor has none). */
typedef struct cwed_static_plugin {
  const char *name;
  const uint32_t *api;
  const cwed_module *module;
} cwed_static_plugin;
const cwed_static_plugin *cwed__static_plugins(void);
__attribute__((weak)) const cwed_static_plugin *cwed__static_plugins(void) {
  static const cwed_static_plugin none[] = { { 0 } };
  return none;
}

typedef enum { P_NONE, P_DISABLED, P_NOT_BUILT, P_LOADED, P_FAILED, P_REFUSED, P_UNLOADED } part_state;
static const char *const PART_NAMES[] = { "none", "disabled", "not built", "loaded", "failed", "refused", "unloaded" };

typedef struct {
  part_state state;
  char err[512];
  long long mtime, size;           /* the file loaded */
  long long seen_mtime, seen_size; /* last poll */
  int loads;
} part;

typedef struct {
  cw_plugin_manifest m;
  bool manifest_ok;
  char manifest_err[256];
  char dir[512];     /* natively the plugin directory; else "plugins/<name>" */
  bool compiled_in;  /* editor part from cwed__static_plugins / runtime from the engine table */
  part rt, ed;
  const cwed_module *mod;
  int owner;
  int reloads;
} plug;

#define MAX_PLUGS 32
static plug P[MAX_PLUGS];
static int plug_count;
static bool panel_dirty = true;
static int frames;
static struct {
  int32_t count, rt_loaded, ed_loaded, errors, reloads;
  uint32_t hash;
} F;

static plug *find_plug(const char *name) {
  for (int i = 0; i < plug_count; i++)
    if (!strcmp(P[i].m.name, name)) return &P[i];
  return NULL;
}

static void notify(const char *name) {
  panel_dirty = true;
  cwed_emit(&(cwed_event){ .type = CWED_EV_PLUGINS, .line = name });
}

/* ── files ───────────────────────────────────────────────────────────── */

#if NATIVE_LOAD
static const char *build_dir(void) { return cw_frame_printf("%s/.cwbuild/native/plugins", cwed_project_dir()); }
static const char *lib_path(const plug *p, const char *part_name) {
  return cw_frame_printf("%s/%s/%s_%s.so", build_dir(), p->m.name, p->m.name, part_name);
}
#endif

#if NATIVE_LOAD
static bool file_stat(const char *path, long long *mtime, long long *size) {
  struct stat st;
  if (stat(path, &st)) return false;
#ifdef __APPLE__
  *mtime = (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  *mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  *size = (long long)st.st_size;
  return true;
}

static bool copy_file(const char *from, const char *to) {
  FILE *a = fopen(from, "rb");
  if (!a) return false;
  FILE *b = fopen(to, "wb");
  if (!b) {
    fclose(a);
    return false;
  }
  char buf[65536];
  size_t n;
  bool ok = true;
  while ((n = fread(buf, 1, sizeof buf, a)) > 0)
    if (fwrite(buf, 1, n, b) != n) ok = false;
  fclose(a);
  return fclose(b) == 0 && ok;
}

/* dlopen a private copy (a rebuilt file at the same path would be the
 * cached old handle otherwise); NULL + err */
static void *open_copy(const plug *p, const char *which, part *pt, char *err, int cap) {
  const char *src = lib_path(p, which);
  long long mt, sz;
  if (!file_stat(src, &mt, &sz)) {
    snprintf(err, (size_t)cap, "%s is not built: plugin.build (or node tools/cwproject.mjs build <project> plugins)", src);
    return NULL;
  }
  const char *dir = cw_frame_printf("%s/%s/loaded", build_dir(), p->m.name);
  mkdir(dir, 0755);
  static int serial;
  const char *dst = cw_frame_printf("%s/%s_%s.%d.%d.so", dir, p->m.name, which, (int)getpid(), ++serial);
  if (!copy_file(src, dst)) {
    snprintf(err, (size_t)cap, "cannot copy %s to %s", src, dst);
    return NULL;
  }
  void *h = dlopen(dst, RTLD_NOW | RTLD_LOCAL);
  remove(dst); /* mapped: the file is not needed any more */
  if (!h) {
    snprintf(err, (size_t)cap, "dlopen %s: %s", src, dlerror());
    return NULL;
  }
  pt->mtime = pt->seen_mtime = mt;
  pt->size = pt->seen_size = sz;
  return h;
}
#endif

/* ── manifests ───────────────────────────────────────────────────────── */

static void read_manifest(plug *p, const char *text, const char *where) {
  char err[256];
  p->manifest_ok = cw_plugin_manifest_parse(text, &p->m, err, sizeof err);
  if (!p->manifest_ok) snprintf(p->manifest_err, sizeof p->manifest_err, "%s: %s", where, err);
  else p->manifest_err[0] = 0;
}

static plug *add_plug(const char *name) {
  plug *p = find_plug(name);
  if (p) return p;
  if (plug_count >= MAX_PLUGS) return NULL;
  p = &P[plug_count++];
  memset(p, 0, sizeof *p);
  snprintf(p->m.name, sizeof p->m.name, "%s", name);
  p->owner = cwed_owner_id(cw_frame_printf("plugin:%s", name));
  return p;
}

static int cmp_plug(const void *a, const void *b) { return strcmp(((const plug *)a)->m.name, ((const plug *)b)->m.name); }

/* (re)read every manifest; keeps the load state of plugins already known */
static void scan(void) {
#if NATIVE_LOAD
  const char *pdir = cwed_project_dir();
  if (pdir[0]) {
    DIR *d = opendir(cw_frame_printf("%s/plugins", pdir));
    for (struct dirent *e; d && (e = readdir(d));) {
      if (e->d_name[0] == '.') continue;
      const char *mf = cw_frame_printf("%s/plugins/%s/plugin.cwplugin", pdir, e->d_name);
      FILE *f = fopen(mf, "rb");
      if (!f) continue;
      char text[8192];
      size_t n = fread(text, 1, sizeof text - 1, f);
      fclose(f);
      text[n] = 0;
      plug *p = add_plug(e->d_name);
      if (!p) break;
      snprintf(p->dir, sizeof p->dir, "%s/plugins/%s", pdir, e->d_name);
      read_manifest(p, text, cw_frame_printf("plugins/%s/plugin.cwplugin", e->d_name));
      if (p->manifest_ok && strcmp(p->m.name, e->d_name)) {
        p->manifest_ok = false;
        snprintf(p->manifest_err, sizeof p->manifest_err, "plugins/%s/plugin.cwplugin: name = %s must equal the directory name",
                 e->d_name, p->m.name);
        snprintf(p->m.name, sizeof p->m.name, "%s", e->d_name);
      }
    }
    if (d) closedir(d);
  }
#endif
  /* compiled in (cart editor; or a native project editor): the project layer
   * carries the manifests (bundled or edited); without one, the table's name */
  for (const cwed_static_plugin *s = cwed__static_plugins(); s->name; s++) {
    plug *p = find_plug(s->name);
    if (!p) {
      p = add_plug(s->name);
      if (!p) break;
      snprintf(p->dir, sizeof p->dir, "plugins/%s", s->name);
      size_t len;
      char *text = cw_vfs_read(cw_frame_printf("plugins/%s/plugin.cwplugin", s->name), &len);
      if (text) {
        read_manifest(p, text, cw_frame_printf("plugins/%s/plugin.cwplugin", s->name));
        free(text);
      } else {
        const cw_plugin_info *ri = cw_plugin_find(s->name);
        char t[512];
        snprintf(t, sizeof t, "name = %s\nversion = %s\nengine_api = %d.%d\nkind = %s\nprovides = %s\n", s->name,
                 ri ? ri->version : "", CW_ENGINE_API_MAJOR, CW_ENGINE_API_MINOR, ri ? "both" : "editor", ri ? ri->provides : "");
        read_manifest(p, t, "(compiled in)");
      }
    }
    p->compiled_in = true;
  }
  for (int i = 0; i < cw_plugin_count(); i++) { /* runtime plugins built into this editor (no editor part) */
    const cw_plugin_info *ri = cw_plugin_at(i);
    if (find_plug(ri->name) || !ri->plugin) continue;
    plug *p = add_plug(ri->name);
    if (!p) break;
    snprintf(p->dir, sizeof p->dir, "plugins/%s", ri->name);
    char t[512];
    snprintf(t, sizeof t, "name = %s\nversion = %s\nengine_api = %d.%d\nkind = runtime\nprovides = %s\n", ri->name, ri->version,
             CW_ENGINE_API_MAJOR, CW_ENGINE_API_MINOR, ri->provides);
    read_manifest(p, t, "(compiled in)");
    p->compiled_in = true;
  }
  if (plug_count > 1) qsort(P, (size_t)plug_count, sizeof(plug), cmp_plug);
  /* engine-side records for plugins that are known but not loaded: errors about their types name them */
  for (int i = 0; i < plug_count; i++) {
    plug *p = &P[i];
    const cw_plugin_info *ri = cw_plugin_find(p->m.name);
    if (p->manifest_ok && p->m.runtime && (!ri || ri->state != CW_PLUGIN_LOADED))
      cw_plugin_note(p->m.name, p->m.version, p->m.provides, p->m.enabled ? CW_PLUGIN_UNLOADED : CW_PLUGIN_DISABLED,
                     p->m.enabled ? "not loaded in the editor (plugin.list says why)" : "enabled = false");
  }
  panel_dirty = true;
}

/* read one plugin's manifest again (enable / reload act on the file as it is now) */
static void reread_manifest(plug *p) {
  const char *rel = cw_frame_printf("plugins/%s/plugin.cwplugin", p->m.name);
  char name[64];
  snprintf(name, sizeof name, "%s", p->m.name);
  char *text = NULL;
  size_t len = 0;
#if NATIVE_LOAD
  if (!p->compiled_in && p->dir[0]) {
    FILE *f = fopen(cw_frame_printf("%s/plugin.cwplugin", p->dir), "rb");
    if (f) {
      text = malloc(16384);
      len = fread(text, 1, 16383, f);
      text[len] = 0;
      fclose(f);
    }
  }
#endif
  if (!text) text = cw_vfs_read(rel, &len);
  if (!text) return; /* compiled in without a manifest: keep what the table said */
  read_manifest(p, text, rel);
  free(text);
  if (p->manifest_ok && strcmp(p->m.name, name)) {
    p->manifest_ok = false;
    snprintf(p->manifest_err, sizeof p->manifest_err, "%s: name = %s must equal the directory name", rel, p->m.name);
  }
  snprintf(p->m.name, sizeof p->m.name, "%s", name);
}

/* ── loading ─────────────────────────────────────────────────────────── */

/* the manifest's API version (and kind/name) is acceptable: else REFUSED */
static bool manifest_accepts(plug *p, char *err, int cap) {
  if (!p->manifest_ok) {
    snprintf(err, (size_t)cap, "%s", p->manifest_err);
    return false;
  }
  uint32_t api;
  char why[384];
  cw_plugin_api_parse(p->m.engine_api, &api);
  if (!cw_plugin_api_compatible(api, why, sizeof why)) {
    snprintf(err, (size_t)cap, "plugin '%s' refused: %s (engine_api = %s in plugins/%s/plugin.cwplugin)", p->m.name, why,
             p->m.engine_api, p->m.name);
    return false;
  }
  /* dependencies: loaded (or at least enabled and accepted) first */
  char deps[256];
  snprintf(deps, sizeof deps, "%s", p->m.depends);
  for (char *t = strtok(deps, ", "); t; t = strtok(NULL, ", ")) {
    plug *d = find_plug(t);
    if (!d || !d->manifest_ok) {
      snprintf(err, (size_t)cap, "plugin '%s' refused: it depends on plugin '%s', which the project does not have", p->m.name, t);
      return false;
    }
    if (!d->m.enabled) {
      snprintf(err, (size_t)cap, "plugin '%s' refused: it depends on plugin '%s', which is disabled", p->m.name, t);
      return false;
    }
  }
  return true;
}

static void editor_safe_types(plug *p);

static void load_runtime(plug *p) {
  part *pt = &p->rt;
  if (!p->m.runtime) {
    pt->state = P_NONE;
    return;
  }
  const cw_plugin_info *ri = cw_plugin_find(p->m.name);
  if (p->compiled_in && ri && ri->state == CW_PLUGIN_LOADED) {
    /* compiled into this editor: registered at startup (cw_plugins_init) */
    pt->state = P_LOADED;
    editor_safe_types(p);
    return;
  }
#if NATIVE_LOAD
  if (!p->compiled_in) {
    char err[512];
    void *h = open_copy(p, "runtime", pt, err, sizeof err);
    if (!h) {
      pt->state = strstr(err, "is not built") ? P_NOT_BUILT : P_FAILED;
      snprintf(pt->err, sizeof pt->err, "%s", err);
      return;
    }
    const cw_plugin *desc = dlsym(h, cw_frame_printf("cw_plugin_%s", p->m.name));
    if (!desc) {
      pt->state = P_FAILED;
      snprintf(pt->err, sizeof pt->err, "%s has no cw_plugin_%s: define it with CW_PLUGIN(%s, ...)", lib_path(p, "runtime"),
               p->m.name, p->m.name);
      return;
    }
    if (!cw_plugin_load(desc, err, sizeof err)) {
      pt->state = strstr(err, "refused") ? P_REFUSED : P_FAILED;
      snprintf(pt->err, sizeof pt->err, "%s", err);
      return;
    }
    pt->state = P_LOADED;
    pt->err[0] = 0;
    pt->loads++;
    editor_safe_types(p);
    return;
  }
#endif
  /* compiled in, withdrawn by a disable: register its descriptor again */
  const cw_plugin_build_entry *b = NULL;
  extern const cw_plugin_build_entry *cw__plugin_table(void);
  for (const cw_plugin_build_entry *e = cw__plugin_table(); e->name; e++)
    if (!strcmp(e->name, p->m.name)) b = e;
  char err[512];
  if (b && b->plugin && cw_plugin_load(b->plugin, err, sizeof err)) {
    pt->state = P_LOADED;
    pt->err[0] = 0;
    editor_safe_types(p);
    return;
  }
  pt->state = P_FAILED;
  snprintf(pt->err, sizeof pt->err, "%s", b && b->plugin ? err
                                     : "its runtime part is not compiled into this editor (rebuild the project editor with it enabled)");
}

/* The editor never runs game code on its documents (the same rule as the
 * game types M4.4 loads): a plugin's node types keep init/destroy (property
 * defaults) and their properties, their tree callbacks are cleared. */
static void editor_safe_types(plug *p) {
  const cw_node_type *ts[32];
  int n = cw_plugin_types(p->m.name, ts, 32);
  for (int i = 0; i < n; i++) {
    cw_node_type *t = (cw_node_type *)ts[i];
    t->enter_tree = NULL;
    t->ready = NULL;
    t->exit_tree = NULL;
    t->fixed_update = NULL;
    t->update = NULL;
    t->pre_render = NULL;
    t->prop_changed = NULL;
    t->children_changed = NULL;
  }
}

static void run_init(plug *p) {
  cwed_owner_push(p->owner);
  if (p->mod->init) p->mod->init();
  cwed_owner_pop();
}

static void load_editor(plug *p) {
  part *pt = &p->ed;
  if (!p->m.editor) {
    pt->state = P_NONE;
    return;
  }
  const uint32_t *api = NULL;
  const cwed_module *mod = NULL;
  for (const cwed_static_plugin *s = cwed__static_plugins(); s->name; s++)
    if (!strcmp(s->name, p->m.name)) api = s->api, mod = s->module;
#if NATIVE_LOAD
  if (!mod) {
    char err[512];
    void *h = open_copy(p, "editor", pt, err, sizeof err);
    if (!h) {
      pt->state = strstr(err, "is not built") ? P_NOT_BUILT : P_FAILED;
      snprintf(pt->err, sizeof pt->err, "%s", err);
      return;
    }
    api = dlsym(h, cw_frame_printf("cwed_plugin_api_%s", p->m.name));
    mod = dlsym(h, cw_frame_printf("cwed_module_%s", p->m.name));
    if (!api || !mod) {
      pt->state = P_FAILED;
      snprintf(pt->err, sizeof pt->err, "%s has no %s: define the module with CWED_PLUGIN(%s) = { ... }", lib_path(p, "editor"),
               api ? cw_frame_printf("cwed_module_%s", p->m.name) : cw_frame_printf("cwed_plugin_api_%s", p->m.name), p->m.name);
      return;
    }
  }
#endif
  if (!mod) {
    pt->state = P_FAILED;
    snprintf(pt->err, sizeof pt->err, "its editor part is not compiled into this editor (a cart editor: rebuild the project editor)");
    return;
  }
  char why[384];
  if (!cw_plugin_api_compatible(*api, why, sizeof why)) {
    pt->state = P_REFUSED;
    snprintf(pt->err, sizeof pt->err, "plugin '%s' refused: its editor code was compiled for engine API %u.%u; %s", p->m.name,
             *api / 1000u, *api % 1000u, why);
    return;
  }
  p->mod = mod;
  run_init(p);
  pt->state = P_LOADED;
  pt->err[0] = 0;
  pt->loads++;
}

static void unload_editor(plug *p) {
  if (p->ed.state != P_LOADED) return;
  cwed_owner_push(p->owner);
  if (p->mod && p->mod->shutdown) p->mod->shutdown();
  cwed_owner_pop();
  cwed_owner_remove(p->owner);
  p->mod = NULL;
  p->ed.state = P_UNLOADED;
}

/* Open documents with nodes of the plugin's types: close clean ones;
 * false (and err) when one has unsaved changes and !force. */
typedef struct { const char *const *types; int ntypes; bool hit; } uses_ctx;
static bool uses_visit(cw_node n, void *user) {
  uses_ctx *u = user;
  for (int i = 0; i < u->ntypes; i++)
    if (cw_node_is(n, u->types[i])) u->hit = true;
  return !u->hit;
}
static bool close_docs_using(plug *p, bool force, char *err, int cap) {
  const char *types[16];
  int nt = 0;
  char buf[256];
  snprintf(buf, sizeof buf, "%s", p->m.provides);
  const cw_plugin_info *ri = cw_plugin_find(p->m.name);
  if (!buf[0] && ri) snprintf(buf, sizeof buf, "%s", ri->provides);
  for (char *t = strtok(buf, ", "); t && nt < 16; t = strtok(NULL, ", "))
    if (cw_node_type_find(t)) types[nt++] = cw_intern(t);
  if (!nt) return true;
  for (int pass = 0; pass < 2; pass++)
    for (int i = cwed_doc_count() - 1; i >= 0; i--) {
      cwed_doc *d = cwed_doc_at(i);
      uses_ctx u = { types, nt, false };
      cw_node_walk(cwed_doc_root(d), uses_visit, &u);
      if (!u.hit) continue;
      if (pass == 0) {
        if (cwed_doc_dirty(d) && !force) {
          snprintf(err, (size_t)cap, "%s has unsaved changes and uses node types of plugin '%s': save it (scene.save) or pass force=true",
                   cwed_doc_title(d), p->m.name);
          return false;
        }
      } else {
        cwed_log(CWED_LOG_INFO, "plugins: closing %s (it uses plugin %s's node types)", cwed_doc_title(d), p->m.name);
        cwed_runf("scene.close doc=%d force=true", i);
      }
    }
  return true;
}

static bool unload_runtime(plug *p, char *err, int cap) {
  if (p->rt.state != P_LOADED) return true;
  /* modules holding nodes of its types for their own use (the inspector's default-value references) let go now */
  cwed_emit(&(cwed_event){ .type = CWED_EV_PLUGINS, .line = p->m.name });
  if (!cw_plugin_unload(p->m.name, err, cap)) return false;
  p->rt.state = P_UNLOADED;
  return true;
}

static void load_plugin(plug *p) {
  char err[512];
  if (!p->m.enabled) {
    if (p->m.runtime) p->rt.state = P_DISABLED;
    if (p->m.editor) p->ed.state = P_DISABLED;
    return;
  }
  if (!manifest_accepts(p, err, sizeof err)) {
    p->rt.state = p->m.runtime || !p->manifest_ok ? P_REFUSED : P_NONE;
    p->ed.state = p->m.editor ? P_REFUSED : P_NONE;
    snprintf(p->rt.err, sizeof p->rt.err, "%s", err);
    snprintf(p->ed.err, sizeof p->ed.err, "%s", err);
    cwed_log(CWED_LOG_ERROR, "%s", err);
    if (p->m.runtime) cw_plugin_note(p->m.name, p->m.version, p->m.provides, CW_PLUGIN_REFUSED, err);
    return;
  }
  if (p->rt.state != P_LOADED) load_runtime(p);
  /* the editor part works on the runtime part's types: without them it stays off */
  if (p->m.runtime && p->rt.state != P_LOADED) {
    if (p->m.editor) {
      p->ed.state = p->rt.state == P_NOT_BUILT ? P_NOT_BUILT : P_FAILED;
      snprintf(p->ed.err, sizeof p->ed.err, "its runtime part did not load: %s", p->rt.err);
    }
    cwed_log(CWED_LOG_ERROR, "plugin %s: runtime part %s: %s", p->m.name, PART_NAMES[p->rt.state], p->rt.err);
    return;
  }
  if (p->ed.state != P_LOADED) load_editor(p);
  if (p->ed.state == P_LOADED || p->rt.state == P_LOADED)
    cwed_log(CWED_LOG_INFO, "plugin %s %s loaded (runtime: %s, editor: %s)", p->m.name, p->m.version, PART_NAMES[p->rt.state],
             PART_NAMES[p->ed.state]);
  if (p->ed.state != P_LOADED && p->ed.state != P_NONE)
    cwed_log(CWED_LOG_ERROR, "plugin %s: editor part %s: %s", p->m.name, PART_NAMES[p->ed.state], p->ed.err);
}

static void load_all(void) {
  /* dependencies first: a few passes in name order */
  bool done[MAX_PLUGS] = { 0 };
  for (int pass = 0; pass < MAX_PLUGS; pass++) {
    bool progress = false;
    for (int i = 0; i < plug_count; i++) {
      if (done[i]) continue;
      bool ready = true;
      char deps[256];
      snprintf(deps, sizeof deps, "%s", P[i].m.depends);
      for (char *t = strtok(deps, ", "); t; t = strtok(NULL, ", ")) {
        plug *d = find_plug(t);
        if (d && !done[d - P]) ready = false;
      }
      if (!ready && pass < MAX_PLUGS - 1) continue;
      load_plugin(&P[i]);
      done[i] = true;
      progress = true;
    }
    if (!progress) break;
  }
  notify("");
}

static void unload_all(void) {
  for (int i = plug_count - 1; i >= 0; i--) {
    unload_editor(&P[i]);
    char err[256];
    if (!P[i].compiled_in) unload_runtime(&P[i], err, sizeof err);
  }
  plug_count = 0;
}

/* reload one plugin's parts from their files (native) */
static bool reload_plugin(plug *p, char *err, int cap) {
  reread_manifest(p);
  if (p->manifest_ok && p->m.enabled && !manifest_accepts(p, err, cap)) return false; /* e.g. engine_api changed: the loaded code stays */
  if (!p->m.enabled) {
    snprintf(err, (size_t)cap, "plugin '%s' is disabled: plugin.enable %s", p->m.name, p->m.name);
    return false;
  }
  if (p->compiled_in) {
    /* no new code in a cart: re-initialise the compiled-in editor part */
    unload_editor(p);
    load_editor(p);
  } else {
#if NATIVE_LOAD
    if (p->m.runtime) {
      part keep = p->rt;
      p->rt.state = P_UNLOADED; /* load_runtime registers again: same names swap their code */
      load_runtime(p);
      if (p->rt.state != P_LOADED) {
        snprintf(err, (size_t)cap, "runtime part: %s", p->rt.err);
        if (keep.state == P_LOADED) p->rt.state = P_LOADED; /* the old code keeps running */
        return false;
      }
    }
    if (p->m.editor) {
      unload_editor(p);
      load_editor(p);
    }
#endif
  }
  p->reloads++;
  F.reloads++;
  if (p->m.editor && p->ed.state != P_LOADED) {
    snprintf(err, (size_t)cap, "editor part: %s", p->ed.err);
    notify(p->m.name);
    return false;
  }
  cwed_log(CWED_LOG_INFO, "plugin %s reloaded (%d)", p->m.name, p->reloads);
  notify(p->m.name);
  return true;
}

/* ── manifest edits (enabled flag) ───────────────────────────────────── */

static bool set_enabled_in_manifest(plug *p, bool on, char *err, int cap) {
  const char *rel = cw_frame_printf("plugins/%s/plugin.cwplugin", p->m.name);
  char *text = NULL;
  size_t len = 0;
#if NATIVE_LOAD
  const char *abs = cw_frame_printf("%s/plugin.cwplugin", p->dir);
  if (!p->compiled_in) {
    FILE *f = fopen(abs, "rb");
    if (f) {
      text = malloc(16384);
      len = fread(text, 1, 16383, f);
      text[len] = 0;
      fclose(f);
    }
  }
#endif
  if (!text) text = cw_vfs_read(rel, &len);
  if (!text) {
    /* a compiled-in plugin without a manifest in the project layer: write one */
    text = malloc(256);
    snprintf(text, 256, "name = %s\nversion = %s\nengine_api = %s\nkind = %s\n", p->m.name, p->m.version, p->m.engine_api,
             p->m.runtime && p->m.editor ? "both" : p->m.editor ? "editor" : "runtime");
  }
  char *out = cw_project_text_set(text, "enabled", on ? "true" : "false");
  free(text);
  if (!out) {
    snprintf(err, (size_t)cap, "could not edit %s", rel);
    return false;
  }
  bool ok;
#if NATIVE_LOAD
  if (!p->compiled_in) {
    FILE *f = fopen(abs, "wb");
    ok = f && fwrite(out, 1, strlen(out), f) == strlen(out);
    if (f) ok = fclose(f) == 0 && ok;
  } else
#endif
    ok = cw_vfs_write(rel, out, strlen(out));
  free(out);
  if (!ok) snprintf(err, (size_t)cap, "could not write %s", rel);
  else p->m.enabled = on;
  return ok;
}

/* ── commands ────────────────────────────────────────────────────────── */

static void part_json(cwed_json *j, const char *key, const part *pt) {
  cwed_json_key(j, key);
  cwed_json_raw(j, "{\"state\":");
  cwed_json_str(j, PART_NAMES[pt->state]);
  cwed_json_raw(j, ",\"error\":");
  cwed_json_str(j, pt->err);
  cwed_json_printf(j, ",\"loads\":%d}", pt->loads);
}

static void plug_json(cwed_json *j, plug *p) {
  cwed_json_raw(j, "{");
  cwed_json_key(j, "name");
  cwed_json_str(j, p->m.name);
  cwed_json_key(j, "title");
  cwed_json_str(j, p->m.title[0] ? p->m.title : p->m.name);
  cwed_json_key(j, "version");
  cwed_json_str(j, p->m.version);
  cwed_json_key(j, "engine_api");
  cwed_json_str(j, p->m.engine_api);
  cwed_json_key(j, "kind");
  cwed_json_str(j, p->m.runtime && p->m.editor ? "both" : p->m.editor ? "editor" : "runtime");
  cwed_json_key(j, "enabled");
  cwed_json_raw(j, p->m.enabled ? "true" : "false");
  cwed_json_key(j, "compiled_in");
  cwed_json_raw(j, p->compiled_in ? "true" : "false");
  cwed_json_key(j, "provides");
  cwed_json_str(j, p->m.provides);
  cwed_json_key(j, "depends");
  cwed_json_str(j, p->m.depends);
  cwed_json_key(j, "manifest_error");
  cwed_json_str(j, p->manifest_err);
  part_json(j, "runtime", &p->rt);
  part_json(j, "editor", &p->ed);
  cwed_json_key(j, "reloads");
  cwed_json_printf(j, "%d", p->reloads);
  cwed_owner_stats s = cwed_owner_contributions(p->owner);
  cwed_json_key(j, "contributes");
  cwed_json_printf(j, "{\"commands\":%d,\"panels\":%d,\"menu_items\":%d,\"toolbar_buttons\":%d,\"inspectors\":%d,\"gizmos\":%d,"
                      "\"subscriptions\":%d}", s.commands, s.panels, s.menu_items, s.toolbar_buttons, s.inspectors, s.gizmos,
                   s.subscriptions);
  cwed_json_raw(j, "}");
}

static plug *arg_plug(cwed_call *c) {
  plug *p = find_plug(cwed_arg(c, "name"));
  if (!p) cwed_fail(c, "no plugin '%s' in this project (plugin.list; plugins live in <project>/plugins/<name>/plugin.cwplugin)",
                    cwed_arg(c, "name"));
  return p;
}

static bool cmd_list(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "engine_api");
  cwed_json_printf(j, "\"%d.%d\"", CW_ENGINE_API_MAJOR, CW_ENGINE_API_MINOR);
  cwed_json_key(j, "loader");
  cwed_json_str(j, NATIVE_LOAD ? "native (shared libraries, hot reload)" : "compiled in (cart: rebuild the project editor for new code)");
  cwed_json_key(j, "plugins");
  cwed_json_raw(j, "[");
  for (int i = 0; i < plug_count; i++) {
    if (i) cwed_json_raw(j, ",");
    plug_json(j, &P[i]);
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_info(cwed_call *c) {
  plug *p = arg_plug(c);
  if (!p) return false;
  cwed_json_key(cwed_reply(c), "plugin");
  plug_json(cwed_reply(c), p);
  return true;
}

static bool cmd_enable(cwed_call *c) {
  plug *p = arg_plug(c);
  if (!p) return false;
  char err[512];
  reread_manifest(p);
  if (!p->m.enabled && !set_enabled_in_manifest(p, true, err, sizeof err)) return cwed_fail(c, "%s", err);
  load_plugin(p);
  notify(p->m.name);
  bool ok = (!p->m.runtime || p->rt.state == P_LOADED) && (!p->m.editor || p->ed.state == P_LOADED);
  if (!ok) return cwed_fail(c, "plugin '%s' is enabled in its manifest but did not load: %s", p->m.name,
                            p->rt.state != P_LOADED && p->m.runtime ? p->rt.err : p->ed.err);
  cwed_json_key(cwed_reply(c), "plugin");
  plug_json(cwed_reply(c), p);
  return true;
}

static bool cmd_disable(cwed_call *c) {
  plug *p = arg_plug(c);
  if (!p) return false;
  char err[512];
  if (!close_docs_using(p, cwed_arg_bool(c, "force"), err, sizeof err)) return cwed_fail(c, "%s", err);
  unload_editor(p);
  if (!unload_runtime(p, err, sizeof err)) {
    load_editor(p); /* back as it was */
    notify(p->m.name);
    return cwed_fail(c, "%s", err);
  }
  if (!set_enabled_in_manifest(p, false, err, sizeof err)) return cwed_fail(c, "%s", err);
  if (p->m.runtime) {
    p->rt.state = P_DISABLED;
    cw_plugin_note(p->m.name, p->m.version, p->m.provides, CW_PLUGIN_DISABLED, "enabled = false");
  }
  if (p->m.editor) p->ed.state = P_DISABLED;
  cwed_log(CWED_LOG_INFO, "plugin %s disabled", p->m.name);
  notify(p->m.name);
  cwed_json_key(cwed_reply(c), "plugin");
  plug_json(cwed_reply(c), p);
  return true;
}

/* ── build (native: tools/cwproject.mjs build <dir> plugins) ──────────── */

static struct {
  cw_process *proc;
  char state[16]; /* idle running ok failed */
  char reload[64]; /* "" none, "*" all, else a plugin name: reload when the build succeeds */
  int lines, exit_code;
  char last_error[512];
} B = { .state = "idle", .exit_code = -1 };

static const char *engine_dir(void) {
  const char *e = getenv("CARTWHEEL_DIR");
  if (e && *e) return e;
#ifdef CWED_ENGINE_DIR
  return CWED_ENGINE_DIR;
#else
  return ".";
#endif
}

static bool start_build(char *err, int cap) {
  if (CW_CART) {
    snprintf(err, (size_t)cap, "a cart cannot build (no processes): build natively (node tools/cwproject.mjs build <dir> plugins)");
    return false;
  }
  const char *dir = cwed_project_dir();
  if (!dir[0]) {
    snprintf(err, (size_t)cap, "the project lives in memory: plugins build from a project directory (project.open)");
    return false;
  }
  if (B.proc) {
    snprintf(err, (size_t)cap, "a plugin build is running (plugin.build_status)");
    return false;
  }
  const char *tool = cw_frame_printf("%s/tools/cwproject.mjs", engine_dir());
  const char *argv[] = { "node", tool, "build", dir, "plugins", NULL };
  B.proc = cw_process_start(&(cw_process_desc){ .argv = argv }, err, cap);
  if (!B.proc) return false;
  snprintf(B.state, sizeof B.state, "running");
  B.lines = 0;
  B.exit_code = -1;
  B.last_error[0] = 0;
  cwed_log(CWED_LOG_INFO, "plugins: building (%s build %s plugins)", tool, dir);
  panel_dirty = true;
  return true;
}

static bool cmd_build(cwed_call *c) {
  char err[512];
  const char *name = cwed_arg(c, "name");
  if (*name && !find_plug(name)) return cwed_fail(c, "no plugin '%s' (plugin.list)", name);
  if (!start_build(err, sizeof err)) return cwed_fail(c, "%s", err);
  snprintf(B.reload, sizeof B.reload, "%s", cwed_arg_bool(c, "reload") ? (*name ? name : "*") : "");
  cwed_json_key(cwed_reply(c), "state");
  cwed_json_str(cwed_reply(c), B.state);
  return true;
}

static bool cmd_build_status(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "state");
  cwed_json_str(j, B.state);
  cwed_json_key(j, "exit");
  cwed_json_printf(j, "%d", B.exit_code);
  cwed_json_key(j, "lines");
  cwed_json_printf(j, "%d", B.lines);
  cwed_json_key(j, "error");
  cwed_json_str(j, B.last_error);
  return true;
}

static void build_frame(void) {
  if (!B.proc) return;
  for (const char *l; (l = cw_process_read_line(B.proc));) {
    B.lines++;
    bool bad = strstr(l, "error") || strstr(l, "FAILED") || strstr(l, "\"ok\":false");
    if (bad && !B.last_error[0]) snprintf(B.last_error, sizeof B.last_error, "%s", l);
    if (bad || l[0] == '@' || strstr(l, "warning")) cwed_log(bad ? CWED_LOG_ERROR : CWED_LOG_INFO, "plugins build: %s", l);
  }
  if (cw_process_running(B.proc)) return;
  while (cw_process_read_line(B.proc)) {}
  B.exit_code = cw_process_exit_code(B.proc);
  cw_process_free(B.proc);
  B.proc = NULL;
  snprintf(B.state, sizeof B.state, "%s", B.exit_code == 0 ? "ok" : "failed");
  cwed_log(B.exit_code ? CWED_LOG_ERROR : CWED_LOG_INFO, "plugins build: %s (exit %d)", B.exit_code ? "FAILED: the loaded code stays" : "done",
           B.exit_code);
  if (B.exit_code == 0 && B.reload[0]) {
    char err[512];
    for (int i = 0; i < plug_count; i++)
      if ((!strcmp(B.reload, "*") || !strcmp(B.reload, P[i].m.name)) && P[i].m.enabled && !reload_plugin(&P[i], err, sizeof err))
        cwed_log(CWED_LOG_ERROR, "plugin %s: reload failed: %s", P[i].m.name, err);
  }
  B.reload[0] = 0;
  panel_dirty = true;
}

static bool cmd_reload(cwed_call *c) {
  plug *p = arg_plug(c);
  if (!p) return false;
  char err[512];
  if (cwed_arg_bool(c, "build")) {
    if (!start_build(err, sizeof err)) return cwed_fail(c, "%s", err);
    snprintf(B.reload, sizeof B.reload, "%s", p->m.name);
    cwed_json_key(cwed_reply(c), "building");
    cwed_json_raw(cwed_reply(c), "true");
    return true;
  }
  if (!reload_plugin(p, err, sizeof err)) return cwed_fail(c, "plugin '%s' did not reload: %s", p->m.name, err);
  cwed_json_key(cwed_reply(c), "plugin");
  plug_json(cwed_reply(c), p);
  return true;
}

static bool cmd_scan(cwed_call *c) {
  int before = plug_count;
  scan();
  for (int i = 0; i < plug_count; i++)
    if (P[i].rt.state == P_NONE && P[i].ed.state == P_NONE && (P[i].m.runtime || P[i].m.editor)) load_plugin(&P[i]);
  notify("");
  cwed_json_key(cwed_reply(c), "plugins");
  cwed_json_printf(cwed_reply(c), "%d", plug_count);
  cwed_json_key(cwed_reply(c), "new");
  cwed_json_printf(cwed_reply(c), "%d", plug_count - before);
  return true;
}

/* ── hot reload: watch the built libraries ───────────────────────────── */

#if NATIVE_LOAD
static bool watch_part(plug *p, part *pt, const char *which) {
  if (pt->state != P_LOADED) return false;
  long long mt, sz;
  if (!file_stat(lib_path(p, which), &mt, &sz)) return false;
  if (mt == pt->mtime && sz == pt->size) return false;
  /* changed: reload once it held still for a whole poll (a linker writing it) */
  bool stable = mt == pt->seen_mtime && sz == pt->seen_size;
  pt->seen_mtime = mt;
  pt->seen_size = sz;
  return stable;
}
#endif

static void watch_frame(void) {
#if NATIVE_LOAD
  int every = (int)cwed_setting_float("plugins.poll_frames");
  if (!cwed_setting_bool("plugins.hot_reload") || B.proc || every <= 0 || frames % every) return;
  for (int i = 0; i < plug_count; i++) {
    plug *p = &P[i];
    if (p->compiled_in || !p->m.enabled) continue;
    bool rt = watch_part(p, &p->rt, "runtime"), ed = watch_part(p, &p->ed, "editor");
    if (!rt && !ed) continue;
    char err[512];
    cwed_log(CWED_LOG_INFO, "plugins: %s changed on disk: reloading", p->m.name);
    if (!reload_plugin(p, err, sizeof err)) cwed_log(CWED_LOG_ERROR, "plugin %s: hot reload failed: %s", p->m.name, err);
  }
#endif
}

/* ── panel ───────────────────────────────────────────────────────────── */

static cw_node list, header, details, toggle_btn, reload_btn;
static int selected;

static const char *row_text(const plug *p) {
  return cw_frame_printf("%s %s   runtime: %s   editor: %s%s", p->m.title[0] ? p->m.title : p->m.name, p->m.version,
                         p->m.runtime ? PART_NAMES[p->rt.state] : "-", p->m.editor ? PART_NAMES[p->ed.state] : "-",
                         p->m.enabled ? "" : "   (disabled)");
}

static void refresh_panel(void) {
  if (!cw_node_valid(list)) return;
  cw_label_set_text(header, cw_frame_printf("Engine API %d.%d   %d plugin%s   %s", CW_ENGINE_API_MAJOR, CW_ENGINE_API_MINOR, plug_count,
                                            plug_count == 1 ? "" : "s", B.proc ? "building..." : !strcmp(B.state, "failed") ? "last build FAILED" : ""));
  cw_item_list_clear(list);
  for (int i = 0; i < plug_count; i++) cw_item_list_add_item(list, row_text(&P[i]), (cw_texture){ 0 });
  if (selected >= plug_count) selected = plug_count - 1;
  if (selected >= 0 && plug_count) cw_item_list_select(list, selected, true);
  plug *p = selected >= 0 && selected < plug_count ? &P[selected] : NULL;
  if (!p) {
    cw_label_set_text(details, cwed_project_dir()[0] || CW_CART ? "No plugins: add one in <project>/plugins/<name>/plugin.cwplugin"
                                                                : "No project directory");
    cw_button_set_disabled(toggle_btn, true);
    cw_button_set_disabled(reload_btn, true);
    return;
  }
  cwed_owner_stats s = cwed_owner_contributions(p->owner);
  char errs[1200] = "";
  if (p->manifest_err[0]) snprintf(errs + strlen(errs), sizeof errs - strlen(errs), "\nmanifest: %s", p->manifest_err);
  if (p->rt.err[0]) snprintf(errs + strlen(errs), sizeof errs - strlen(errs), "\nruntime: %s", p->rt.err);
  if (p->ed.err[0] && strcmp(p->ed.err, p->rt.err)) snprintf(errs + strlen(errs), sizeof errs - strlen(errs), "\neditor: %s", p->ed.err);
  cw_label_set_text(details, cw_frame_printf("%s\nengine API %s, kind %s%s%s%s\nprovides: %s\nadds %d commands, %d panels, %d inspector "
                                             "plugins, %d gizmos, %d menu items; reloaded %d times%s",
                                             p->m.description[0] ? p->m.description : p->m.name, p->m.engine_api,
                                             p->m.runtime && p->m.editor ? "both" : p->m.editor ? "editor" : "runtime",
                                             p->m.depends[0] ? ", needs " : "", p->m.depends, p->compiled_in ? ", compiled in" : "",
                                             p->m.provides[0] ? p->m.provides : "-", s.commands, s.panels, s.inspectors, s.gizmos,
                                             s.menu_items, p->reloads, errs));
  cw_button_set_text(toggle_btn, p->m.enabled ? "Disable" : "Enable");
  cw_button_set_disabled(toggle_btn, false);
  cw_button_set_disabled(reload_btn, !p->m.enabled);
}

static void on_pick(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)argc, (void)user;
  if (selected == (int)a[0].i) return;
  selected = (int)a[0].i;
  panel_dirty = true;
}
static void on_toggle(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  if (selected < 0 || selected >= plug_count) return;
  cwed_runf("plugin.%s name=%s", P[selected].m.enabled ? "disable" : "enable", P[selected].m.name);
}
static void on_reload(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  if (selected < 0 || selected >= plug_count) return;
  cwed_runf("plugin.reload name=%s build=%s", P[selected].m.name, CW_CART ? "false" : "true");
}
static void on_build(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  cwed_run("plugin.build reload=true", NULL);
}
static void on_scan(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  cwed_run("plugin.scan", NULL);
}

static cw_node button(const char *name, const char *text, cw_signal_fn fn) {
  cw_node b = cw_node_new("Button", name);
  cw_button_set_text(b, text);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, fn, NULL, 0);
  return b;
}

static cw_node create(void *user) {
  (void)user;
  /* in a scroll container: the panel never makes its dock slot bigger than the other panels there need */
  cw_node scroll = cw_node_new("ScrollContainer", "PluginScroll");
  cw_control_set_size_flags(scroll, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node box = cw_node_new("VBoxContainer", "PluginBox");
  cw_control_set_size_flags(box, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_add_child(scroll, box);
  cw_node row = cw_node_new("HBoxContainer", "Row");
  header = cw_node_new("Label", "PluginHeader");
  cw_control_set_size_flags(header, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_set(header, "ellipsis", cw_val_bool(true));
  cw_node_add_child(row, header);
  /* the selected plugin's actions, then the project's (always in view: the row is the panel's first) */
  toggle_btn = button("PluginToggle", "Disable", on_toggle);
  reload_btn = button("PluginReload", CW_CART ? "Reinit" : "Rebuild + Reload", on_reload);
  cw_node_add_child(row, toggle_btn);
  cw_node_add_child(row, reload_btn);
  /* the same widgets on every host (a cart cannot build: the button is disabled there) */
  cw_node build = button("PluginBuild", "Build All", on_build);
  if (CW_CART) cw_button_set_disabled(build, true);
  cw_node_add_child(row, build);
  cw_node_add_child(row, button("PluginScan", "Rescan", on_scan));
  cw_node_add_child(box, row);
  cw_node split = cw_node_new("HBoxContainer", "Split");
  cw_control_set_size_flags(split, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  list = cw_node_new("ItemList", "PluginList");
  cw_control_set_size_flags(list, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_connect(list, "item_selected", (cw_node){ 0 }, NULL, on_pick, NULL, 0);
  cw_node_add_child(split, list);
  details = cw_node_new("Label", "PluginDetails");
  cw_node_set(details, "autowrap", cw_val_bool(true));
  cw_control_set_size_flags(details, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_control_set_stretch_ratio(details, 1.6f); /* the details get the wider half */
  cw_node_add_child(split, details);
  cw_node_add_child(box, split);
  panel_dirty = true;
  return scroll;
}

/* ── module ──────────────────────────────────────────────────────────── */

static void fields_refresh(void) {
  F.count = plug_count;
  F.rt_loaded = F.ed_loaded = F.errors = 0;
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < plug_count; i++) {
    plug *p = &P[i];
    F.rt_loaded += p->rt.state == P_LOADED;
    F.ed_loaded += p->ed.state == P_LOADED;
    F.errors += p->rt.state == P_FAILED || p->rt.state == P_REFUSED || p->ed.state == P_FAILED || p->ed.state == P_REFUSED ||
                !p->manifest_ok;
    const char *s = cw_frame_printf("%s|%d|%d|%d;", p->m.name, p->m.enabled, p->rt.state, p->ed.state);
    h = cw_hash32(s, strlen(s), h);
  }
  F.hash = h;
}

static char loaded_for[1024]; /* the project the plugins were loaded for */

static void on_project(const cwed_event *e, void *user) {
  (void)e, (void)user;
  const char *key = cw_frame_printf("%s|%s", cwed_project_dir(), cwed_project_dir()[0] ? "" : cwed_project_name());
  if (!strcmp(key, loaded_for)) { /* the same project re-read (settings changed): pick up new manifests only */
    int before = plug_count;
    scan();
    for (int i = 0; i < plug_count; i++)
      if (P[i].rt.state == P_NONE && P[i].ed.state == P_NONE && (P[i].m.runtime || P[i].m.editor)) load_plugin(&P[i]);
    if (plug_count != before) notify("");
    return;
  }
  snprintf(loaded_for, sizeof loaded_for, "%s", key);
  unload_all();
  scan();
  load_all();
}

static void frame(double dt) {
  (void)dt;
  frames++;
  build_frame();
  watch_frame();
  /* loaded editor parts get their frame under their owner */
  for (int i = 0; i < plug_count; i++)
    if (P[i].ed.state == P_LOADED && P[i].mod && P[i].mod->frame) {
      cwed_owner_push(P[i].owner);
      P[i].mod->frame(dt);
      cwed_owner_pop();
    }
  if (panel_dirty) {
    panel_dirty = false;
    refresh_panel();
  }
  fields_refresh();
}

static void shutdown(void) {
  for (int i = plug_count - 1; i >= 0; i--)
    if (P[i].ed.state == P_LOADED && P[i].mod && P[i].mod->shutdown) {
      cwed_owner_push(P[i].owner);
      P[i].mod->shutdown();
      cwed_owner_pop();
    }
  if (B.proc) cw_process_free(B.proc);
  B.proc = NULL;
}

static void init(void) {
  static const cwed_param name_p[] = { { "name", CWED_ARG_STRING, NULL, "plugin name (plugin.list)" }, { 0 } };
  static const cwed_param disable_p[] = {
    { "name", CWED_ARG_STRING, NULL, "plugin name (plugin.list)" },
    { "force", CWED_ARG_BOOL, "false", "close scenes using its node types even with unsaved changes" },
    { 0 } };
  static const cwed_param reload_p[] = {
    { "name", CWED_ARG_STRING, NULL, "plugin name (plugin.list)" },
    { "build", CWED_ARG_BOOL, "false", "build the project's plugins first, reload when the build succeeds" },
    { 0 } };
  static const cwed_param build_p[] = {
    { "name", CWED_ARG_STRING, "", "only reload this one afterwards (the build covers every plugin)" },
    { "reload", CWED_ARG_BOOL, "true", "reload the plugins when the build succeeds" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.list", .title = "List Plugins", .flags = CWED_CMD_QUERY,
    .doc = "Every plugin of the project: manifest (version, engine_api, kind, enabled, provides), the state of its runtime and "
           "editor parts with errors, and what it registered (commands, panels, inspector plugins, gizmos, menu items).",
    .exec = cmd_list });
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.info", .title = "Plugin Info", .params = name_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "One plugin, as plugin.list shows it.", .exec = cmd_info });
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.enable", .title = "Enable Plugin", .params = name_p,
    .doc = "Set enabled = true in the plugin's manifest and load it (runtime part: its node types; editor part: its module). "
           "The game gets it on its next build.", .exec = cmd_enable });
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.disable", .title = "Disable Plugin", .params = disable_p,
    .doc = "Unload the plugin (its panels, commands, inspector plugins and gizmos go; its node types are withdrawn, so scenes "
           "using them are refused, never opened lossy; open ones are closed first) and set enabled = false in its manifest.",
    .exec = cmd_disable });
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.reload", .title = "Reload Plugin", .params = reload_p,
    .doc = "Load the plugin's code again: natively from its built libraries (build=true builds first), in a cart the "
           "compiled-in module re-initialised. A failed load keeps the old code running.", .exec = cmd_reload });
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.build", .title = "Build Plugins", .params = build_p,
    .doc = "Build the project's plugins (node tools/cwproject.mjs build <project> plugins) in the background; output goes to "
           "the Output panel; reload=true reloads them when it succeeds (plugin.build_status).", .exec = cmd_build });
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.build_status", .title = "Plugin Build Status",
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "The last plugin build: state (idle running ok failed), exit, first error.",
    .exec = cmd_build_status });
  cwed_command_register(&(cwed_command_desc){ .name = "plugin.scan", .title = "Rescan Plugins",
    .doc = "Read the project's plugin manifests again and load plugins that are new.", .exec = cmd_scan });
  cwed_setting_define(&(cwed_setting_desc){ .key = "plugins.hot_reload", .type = CWED_ARG_BOOL, .def = "true",
    .doc = "Reload a plugin when its built library changes on disk (native editor)" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "plugins.poll_frames", .type = CWED_ARG_INT, .def = "20", .min = 1, .max = 600,
    .doc = "Frames between checks of the plugin libraries (hot reload)" });
  cwed_panel_register(&(cwed_panel_desc){ .id = "plugins", .title = "Plugins", .slot = "bottom", .order = 90, .create = create });
  cwed_menu_add("Project/Plugins...", "panel.show id=plugins", 300);
  if (!CW_CART) cwed_menu_add("Project/Build Plugins", "plugin.build", 301);
  cwed_subscribe(CWED_EV_PROJECT, on_project, NULL);
  cw_debug_i32("plugins.ed_count", &F.count);
  cw_debug_i32("plugins.ed_runtime_loaded", &F.rt_loaded);
  cw_debug_i32("plugins.ed_editor_loaded", &F.ed_loaded);
  cw_debug_i32("plugins.ed_errors", &F.errors);
  cw_debug_i32("plugins.ed_reloads", &F.reloads);
  cw_debug_u32("plugins.ed_hash", &F.hash);
  /* the project is open already: load its plugins now, so their panels dock with everyone's */
  snprintf(loaded_for, sizeof loaded_for, "%s|%s", cwed_project_dir(), cwed_project_dir()[0] ? "" : cwed_project_name());
  scan();
  load_all();
  fields_refresh();
}

CWED_MODULE(plugins) = { .title = "Plugins", .milestone = "M5.2", .init = init, .frame = frame, .shutdown = shutdown };

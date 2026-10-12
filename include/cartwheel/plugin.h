/* cartwheel plugins (M5.2): runtime modules a PROJECT adds to the engine
 * without editing the cartwheel tree.
 *
 * ── What a plugin is ───────────────────────────────────────────────────
 * A directory <project>/plugins/<name>/ holding plugin.cwplugin (the
 * manifest, `key = value` lines like project.cwproj) and C sources:
 *
 *   name = spinner                  C identifier, the same as the directory
 *   version = 1.2.0                 the plugin's own version (free text)
 *   engine_api = 1.0                the engine API it was written for
 *   kind = both                     runtime | editor | both
 *   runtime_sources = runtime       files/directories (.c, recursive), plugin-relative
 *   editor_sources = editor         (defaults: runtime/ and editor/)
 *   provides = Spinner, SpinnerFX   node types it registers (named in errors
 *                                   when a scene uses them and it is off)
 *   depends = other_plugin          plugins it needs (loaded first)
 *   enabled = true
 *   title = Spinner                 optional, for the editor's Plugins panel
 *   description = ...
 *
 * RUNTIME plugins are compiled into the game by the project build (natively
 * and as a cart): cw_add_app finds <its source dir>/plugins/ (or PLUGINS
 * <dir>), compiles each enabled runtime plugin and generates the app's
 * plugin table. The engine registers them at startup, before the app's
 * init (cw_plugins_init), so scenes loading in init see their node types.
 * EDITOR plugins are editor modules (cwed/editor.h, CWED_PLUGIN): the native
 * editor builds and loads them as shared libraries from the project; a cart
 * editor gets them compiled in by the project build.
 *
 * ── The runtime side of a plugin ───────────────────────────────────────
 *
 *     #include "cartwheel/plugin.h"
 *     static void reg(void) { cw_node_type_register(&spinner_type); ... }
 *     CW_PLUGIN(spinner, .version = "1.2.0", .register_fn = reg);
 *
 * register_fn runs once at startup (and again after a hot reload of a
 * native plugin library: re-registering a node type with the same layout
 * swaps its code, cartwheel/scene.h). It registers through the engine's
 * ordinary registries; nothing is plugin-specific:
 *
 *   node types + methods        cartwheel/scene.h  cw_node_type_register, cw_node_type_add_method
 *   scene file sections         cartwheel/scene_file.h  cw_scene_register_section
 *   resource types / importers  cartwheel/resource.h  cw_res_register_type
 *   debug-channel commands      cartwheel/debug.h  cw_debug_command
 *   render pipeline passes      cartwheel/pipeline.h  (from a node type's enter_tree, per tree)
 *
 * ── The plugin contract (API version) ──────────────────────────────────
 * The headers a plugin may include are the engine's public headers
 * (include/cartwheel/, every .h) and, for editor plugins, cwed/editor.h. Their
 * declarations are the plugin API, versioned as CW_ENGINE_API_MAJOR.MINOR:
 *   MINOR goes up when something is ADDED (old plugins still build and run);
 *   MAJOR goes up when something a plugin may use changes or goes away.
 * A plugin is accepted when its major equals the engine's and its minor is
 * not newer. Checked twice: at build time against the manifest's
 * engine_api (a refused plugin is not compiled; the game reports it), and
 * at load time against the CW_ENGINE_API the plugin's code was compiled
 * with (CW_PLUGIN records it), which is what catches a stale native plugin
 * library. Internal headers (src/, editor/core/) are never part of it. */
#ifndef CW_PLUGIN_H
#define CW_PLUGIN_H

#include "base.h"

typedef struct cw_node_type cw_node_type;

#ifdef __cplusplus
extern "C" {
#endif

#define CW_ENGINE_API_MAJOR 1
#define CW_ENGINE_API_MINOR 0
#define CW_API_VERSION(major, minor) ((uint32_t)(major) * 1000u + (uint32_t)(minor))
#define CW_ENGINE_API CW_API_VERSION(CW_ENGINE_API_MAJOR, CW_ENGINE_API_MINOR)

typedef struct cw_plugin {
  uint32_t api;            /* CW_ENGINE_API the code was compiled with (CW_PLUGIN sets it) */
  const char *name;        /* the manifest's name (CW_PLUGIN sets it) */
  const char *version;
  void (*register_fn)(void);
  /* optional: undo what register_fn did that the engine cannot withdraw by
   * itself (node types it can: cw_plugin_unload withdraws them) */
  void (*unregister_fn)(void);
} cw_plugin;

/* Defines the plugin's descriptor, `cw_plugin_<name>` (the symbol the
 * generated table and a native loader look for). */
#define CW_PLUGIN(id, ...) \
  CW_PLUGIN_EXPORT const cw_plugin cw_plugin_##id = { .api = CW_ENGINE_API, .name = #id, __VA_ARGS__ }
#if defined(_WIN32)
#define CW_PLUGIN_EXPORT __declspec(dllexport)
#else
#define CW_PLUGIN_EXPORT __attribute__((visibility("default"), used))
#endif

typedef enum {
  CW_PLUGIN_LOADED,   /* registered */
  CW_PLUGIN_DISABLED, /* enabled = false in its manifest (not built in) */
  CW_PLUGIN_REFUSED,  /* API version mismatch, or a dependency missing / refused */
  CW_PLUGIN_FAILED,   /* its build or its library failed (error says why) */
  CW_PLUGIN_UNLOADED, /* was loaded, then unloaded (native editor) */
} cw_plugin_state;

typedef struct cw_plugin_info {
  const char *name, *version;
  const char *engine_api;   /* manifest text ("1.0"), may be "" */
  const char *provides;     /* comma-separated node types (manifest) */
  cw_plugin_state state;
  const char *error;        /* why refused / failed / disabled ("" when loaded) */
  const cw_plugin *plugin;  /* NULL unless loaded */
  int types_registered;     /* node types its register_fn added */
} cw_plugin_info;

/* The build's plugin table (generated by cw_add_app for apps with plugins;
 * entries for disabled and refused plugins carry no descriptor and say why).
 * Apps without plugins get the engine's empty table. */
typedef struct cw_plugin_build_entry {
  const char *name, *version, *engine_api, *provides;
  const cw_plugin *plugin; /* NULL: not built in */
  int state;               /* cw_plugin_state when plugin is NULL */
  const char *error;
} cw_plugin_build_entry;

/* Register the build's plugins (the platform calls it before the app's
 * init; idempotent). Also registers the `plugins` debug command. */
void cw_plugins_init(void);

/* Check and register one plugin (the build table and native loaders use
 * it). false + err when refused (version) or its name is taken. Loading a
 * plugin of a name already loaded re-runs its register_fn (hot reload). */
bool cw_plugin_load(const cw_plugin *p, char *err, int err_cap);
/* Withdraw a loaded plugin: its unregister_fn, then the node types its
 * register_fn added are withdrawn (cw_node_type_withdraw). false + err
 * when nodes of those types still exist (free them first). */
bool cw_plugin_unload(const char *name, char *err, int err_cap);
/* Record a plugin that is known but not loaded (disabled / refused /
 * failed) so errors about its types can name it. */
void cw_plugin_note(const char *name, const char *version, const char *provides, cw_plugin_state state,
                    const char *error);

/* Whether an API version (major*1000+minor) is accepted by this engine;
 * false fills err with the reason. cw_plugin_api_parse reads "1.0". */
bool cw_plugin_api_compatible(uint32_t api, char *err, int err_cap);
bool cw_plugin_api_parse(const char *text, uint32_t *api); /* "major[.minor]" -> *api; false on bad text */

/* The node types a plugin's register_fn added (registry descriptors). */
int cw_plugin_types(const char *name, const cw_node_type **out, int cap);

int cw_plugin_count(void); /* plugins this build knows (any state) */
const cw_plugin_info *cw_plugin_at(int i); /* engine-owned; NULL out of range */
const cw_plugin_info *cw_plugin_find(const char *name); /* by plugin name; engine-owned; NULL if unknown */
/* For an unknown node type: which plugin provides it and why it is not
 * here ("plugin 'spinner' provides it, but it is disabled (enabled = false in
 * plugins/spinner/plugin.cwplugin)"); NULL when no known plugin does. */
const char *cw_plugin_type_hint(const char *type);
const char *cw_plugin_state_name(cw_plugin_state s); /* static "loaded", "disabled", ...; "?" if out of range */

/* ── manifests (the editor and tools read them; plain text parsing) ──── */
typedef struct cw_plugin_manifest {
  char name[64], version[32], engine_api[16], kind[32];
  char title[96], description[256];
  char runtime_sources[256], editor_sources[256], provides[256], depends[256];
  bool enabled, runtime, editor;
} cw_plugin_manifest;
/* Parse manifest text; false + err for a missing/invalid name, kind or
 * engine_api. Unknown keys are ignored. */
bool cw_plugin_manifest_parse(const char *text, cw_plugin_manifest *m, char *err, int err_cap);

#ifdef __cplusplus
}
#endif
#endif

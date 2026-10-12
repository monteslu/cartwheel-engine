/* cartwheel editor: the extension API (the ONE header editor modules use).
 *
 * The editor is a cartwheel app built with the engine's own UI
 * (cartwheel/ui.h). Its code is split into a small CORE (editor/core/: this
 * API, documents, commands and history, selection, docking, the shell) and
 * MODULES (editor/modules/<name>/: one directory per feature: a panel, a
 * set of commands, inspector plugins, gizmos, a play backend). Modules never
 * include each other's files and never include core internals: everything a
 * module needs is here, so modules are built in parallel by different people
 * (or agents) without touching each other's files.
 *
 * ── Modules ──────────────────────────────────────────────────────────
 * A module is a directory editor/modules/<name>/ whose .c files define
 *
 *     CWED_MODULE(name) = { .title = "History", .init = history_init };
 *
 * The build finds every module directory and generates the registration
 * table (build/.../editor_modules.c, sorted by name), so adding a module
 * edits no shared file. cwed_init calls each module's init in table order
 * after the core registered its own commands and panels; init registers
 * the module's panels, commands, menu items, settings, inspector plugins,
 * gizmos and event subscriptions. Order between modules must not matter
 * (use events, not init order).
 *
 * ── Commands (every editor action) ───────────────────────────────────
 * Every action, from a menu, a shortcut, a panel, the command palette or
 * an agent, is a named COMMAND run through cwed_run() with text arguments:
 *
 *     node.set path=Player/Mesh prop=color value="color(1, 0, 0, 1)"
 *     node.set Player/Mesh color "color(1, 0, 0, 1)"     (positional: same)
 *
 * A command line is the command name, then arguments by position or as
 * key=value (a key must be one of the command's parameter names); quote an
 * argument containing spaces with "..." (\" and \\ escape inside quotes).
 * Arguments stay TEXT: the command parses them (cwed_arg_* helpers), so
 * every command is serializable as the exact line that runs it again.
 * Replies are JSON: {"ok":true, ...} or {"ok":false,"error":"...why, and the
 * call that fixes it"}.
 *
 * Undoable commands (CWED_CMD_UNDO) record an entry in a HISTORY: scene
 * commands (CWED_CMD_SCENE) in the active document's own history, others in
 * the editor-wide one. Undo calls the command's undo; redo calls exec again
 * with the same arguments and the state the first run left (so a redo can
 * reuse what undo kept, e.g. the very node that was deleted). Merging:
 * running a command with the reserved argument merge=<key> folds it into the
 * previous history entry when that entry is the same command with the same
 * key and nothing was undone or sealed in between: one undo step for a whole
 * drag (cwed_history_seal ends the run).
 *
 * The same commands answer over the debug channel (cartwheel/debug.h):
 *     ed <command line>          run it (romdev: wasm({op:'command', text:'ed ...'}))
 *     ed64 <base64 line>         the same, for lines with quotes or newlines
 * plus `ed help`, `ed state`, `ed history`, `ed log` (`ed help` lists every command).
 *
 * ── Documents ─────────────────────────────────────────────────────────
 * A document is one open .cwscene: its own cw_tree (never processed: the
 * edited scene does not run in the editor) holding the scene root under
 * /root, its history, its selection and the path it saves to. Scenes load
 * and save through cartwheel/vfs.h (natively a project directory, in a cart
 * the in-memory project layer) and round-trip byte for byte. Node paths in
 * commands are relative to the scene root: "." is the root, "Player/Mesh"
 * a descendant (the same form .cwscene parent= attributes use).
 *
 * ── Panels and the dock ───────────────────────────────────────────────
 * A panel is a Control subtree the dock places in a SLOT: left_top,
 * left_bottom, center, bottom, right_top, right_bottom. Slots are tab
 * containers inside resizable splits; the user drags tabs between slots
 * (panel.move) and the layout persists in the settings.
 *
 * ── Threads ───────────────────────────────────────────────────────────
 * Everything here is main thread only.
 *
 * Naming: editor API functions are cwed_*; command names are
 * "<area>.<verb>" in lower case (scene.save, node.add, panel.move). */
#ifndef CWED_EDITOR_H
#define CWED_EDITOR_H

#include "cartwheel/plugin.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── modules ──────────────────────────────────────────────────────────── */

typedef struct cwed_module {
  const char *title;     /* human name ("Scene Tree") */
  const char *milestone; /* the development milestone that added it ("M4.1") */
  void (*init)(void);    /* register panels, commands, menus, settings, plugins */
  void (*frame)(double dt); /* once per editor frame, after input (keep it cheap: refresh what an event marked dirty) */
  void (*shutdown)(void);
} cwed_module;
#define CWED_MODULE(name) const cwed_module cwed_module_##name

/* ── plugins (M5.2): editor modules that live in a PROJECT ───────────────
 * An editor plugin is a module in <project>/plugins/<name>/ (manifest
 * plugin.cwplugin, kind editor or both; cartwheel/plugin.h) instead of
 * editor/modules/. It uses this same API and nothing else, and defines
 *
 *     CWED_PLUGIN(name) = { .title = "Turntable tools", .init = init, .frame = frame };
 *
 * (name = the manifest name). The macro records the API version the code
 * was compiled with (CW_ENGINE_API, cartwheel/plugin.h), checked when the
 * plugin loads. The native editor builds it as a shared library from the
 * project and loads it (dlopen), unloads it and reloads it when it is
 * rebuilt; a cart editor gets it compiled in by the project build (no
 * runtime loading in a cart). Everything init registers (commands, panels,
 * menu items, toolbar buttons, inspector plugins, gizmos, subscriptions) is
 * withdrawn when the plugin unloads (registration owners, below), so:
 *   - keep state in statics that init resets (init runs again on reload),
 *   - panels may be registered any time (a plugin's panels are created and
 *     docked when it loads, in their default slot or where the user's
 *     saved layout put them),
 *   - shutdown runs before an unload: free what is not a registration.
 * A plugin's RUNTIME part also loads into the editor (its node types, so
 * scenes using them open); like game types, they keep init/destroy and
 * their properties, but their tree callbacks are cleared: the editor never
 * runs game code on its documents. */
#define CWED_PLUGIN(name) \
  CW_PLUGIN_EXPORT const uint32_t cwed_plugin_api_##name = CW_ENGINE_API; \
  CW_PLUGIN_EXPORT const cwed_module cwed_module_##name

/* Registration owners. Every registration below remembers the owner current
 * when it was made: 0 = the editor (core and built-in modules), else the id
 * of a plugin the plugin manager pushed around that plugin's init, frame and
 * shutdown. cwed_owner_remove withdraws everything an owner registered
 * (panels are freed, menus and the toolbar rebuilt). The plugin manager
 * uses these; ordinary modules never need to. */
typedef struct cwed_owner_stats {
  int commands, panels, menu_items, toolbar_buttons, inspectors, gizmos, subscriptions;
} cwed_owner_stats;
int cwed_owner_id(const char *name); /* the owner id of a name (created on first use) */
void cwed_owner_push(int id); /* registrations made until the pop belong to id; asserts on deep nesting */
void cwed_owner_pop(void); /* back to the previous owner; asserts without a push */
int cwed_owner_current(void); /* the owner new registrations get now (0 = the editor) */
const char *cwed_owner_name(int id); /* the name given to cwed_owner_id; NULL for an unknown id */
cwed_owner_stats cwed_owner_contributions(int id); /* counts of what id has registered now */
void cwed_owner_remove(int id); /* withdraw everything id registered; no-op for id <= 0 */

/* ── JSON replies ──────────────────────────────────────────────────────
 * A small writer for command replies and queries (values, not pretty). */
typedef struct cwed_json cwed_json;
void cwed_json_raw(cwed_json *j, const char *text);            /* appended verbatim */
/* Appended verbatim after formatting (any length); escape strings yourself or use cwed_json_str. */
void cwed_json_printf(cwed_json *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void cwed_json_str(cwed_json *j, const char *s);               /* a quoted, escaped JSON string */
/* "key": prefix with the comma handled: call between members of an object
 * opened with cwed_json_raw(j, "{"); the first key of an object gets no comma. */
void cwed_json_key(cwed_json *j, const char *key);

/* ── commands ─────────────────────────────────────────────────────────── */

typedef enum {
  CWED_CMD_UNDO = 1 << 0,   /* records a history entry; needs undo */
  CWED_CMD_SCENE = 1 << 1,  /* acts on the active document (refused when none); its history is the document's */
  CWED_CMD_HIDDEN = 1 << 2, /* not listed in the command palette (queries, internal steps) */
  CWED_CMD_QUERY = 1 << 3,  /* reads only: never logged as an action, never dirties anything */
} cwed_cmd_flags;

typedef enum {
  CWED_ARG_STRING, /* any text */
  CWED_ARG_INT,
  CWED_ARG_FLOAT,
  CWED_ARG_BOOL,   /* true/false/1/0/on/off */
  CWED_ARG_PATH,   /* a node path in the active document ("." = scene root) */
  CWED_ARG_VALUE,  /* a property value in scene-file form: vec3(0, 1, 0), "text", SPHERE */
} cwed_arg_type;

typedef struct cwed_param {
  const char *name;
  cwed_arg_type type;
  const char *def; /* default text; NULL = required */
  const char *doc;
} cwed_param;

typedef struct cwed_call cwed_call; /* one run of a command (and its history entry) */

typedef struct cwed_command_desc {
  const char *name;          /* "node.set" */
  const char *title;         /* palette/menu label: "Set Property" */
  const char *doc;           /* one or two sentences: what it does, what it returns */
  const cwed_param *params;  /* NULL-name terminated; NULL = no arguments */
  uint32_t flags;            /* cwed_cmd_flags */
  const char *shortcut;      /* "ctrl+shift+z", "f2", "delete"; NULL = none */
  /* A line the command tests run against the demo project (editor/tests):
   * every CWED_CMD_UNDO command MUST have one, and its do/undo/redo round
   * trip is checked automatically. May use "{sel}" for the first selected
   * node's path ({sel2}: a second node). Several lines (separated by '\n'):
   * the first ones set the stage and only the last one is round-tripped (a
   * node.disconnect needs a connection first). */
  const char *example;
  /* Run. First run: state is NULL; later runs (redo): the state the last
   * exec/undo left. Write the reply's members with cwed_reply_* (the core
   * wraps them in {"ok":true,...}). Return false with cwed_fail() to refuse;
   * a refused command must have changed nothing. */
  bool (*exec)(cwed_call *c);
  /* Undo exactly what exec did (CWED_CMD_UNDO). */
  void (*undo)(cwed_call *c);
  /* Fold `next` (already executed) into `prev` (the history top, same
   * command, same merge key): keep prev's undo data, take next's result.
   * Return false to refuse (next becomes its own entry). NULL = never merge. */
  bool (*merge)(cwed_call *prev, cwed_call *next);
  /* Release the state (history entry dropped: redo stack cleared, document
   * closed). NULL = cw_free(state) when non-NULL. */
  void (*free_state)(cwed_call *c, void *state);
  /* Menu items gray out when this returns false; NULL = always enabled. */
  bool (*enabled)(void);
} cwed_command_desc;

/* Register a command (desc copied; strings must be static). Re-registering a
 * name replaces it (a module overriding a core command must say so in its
 * doc). */
void cwed_command_register(const cwed_command_desc *d);
const cwed_command_desc *cwed_command_find(const char *name); /* NULL when unknown; linear scan */
int cwed_command_count(void); /* registered commands */
const cwed_command_desc *cwed_command_at(int i); /* registration order */

/* Run a command line. Returns the JSON reply (cw_alloc'd, cw_free it);
 * json_out may be NULL to discard it. Returns whether it succeeded. Commands
 * may run other commands (a nested run joins no history: only the outermost
 * undoable command is recorded, so composite commands record their own undo). */
bool cwed_run(const char *line, char **json_out);
/* Runs a formatted line through cwed_run (cut at 4095 bytes); the reply is discarded. */
bool cwed_runf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Inside exec/undo/merge: */
const char *cwed_arg(cwed_call *c, const char *name);  /* the text (default applied); "" when absent */
bool cwed_arg_given(cwed_call *c, const char *name);   /* given explicitly (not the default) */
int64_t cwed_arg_int(cwed_call *c, const char *name); /* strtoll base 0 (0x.. ok); 0 when absent or not a number */
double cwed_arg_float(cwed_call *c, const char *name); /* strtod; 0 when absent or not a number */
bool cwed_arg_bool(cwed_call *c, const char *name); /* true for true/1/on/yes; anything else is false */
/* CWED_ARG_PATH: the node in the call's document; null (and the call
 * failed with a message naming the path) when it does not exist. */
cw_node cwed_arg_node(cwed_call *c, const char *name);
/* CWED_ARG_VALUE: parse for a property of node n (enums by name, strings
 * quoted or bare); false (call failed, message says what the property
 * wants) when it does not parse. */
bool cwed_arg_value(cwed_call *c, const char *name, cw_node n, const char *prop, cw_value *out);
/* The document the call acts on (NULL for editor-wide commands). */
struct cwed_doc *cwed_call_doc(cwed_call *c);
/* State kept between exec, undo and redo (one per history entry). */
void *cwed_call_state(cwed_call *c);
void cwed_call_set_state(cwed_call *c, void *state); /* the call owns it from now on (freed via free_state) */
/* Reply members of a successful call (JSON object members, key first). */
cwed_json *cwed_reply(cwed_call *c);
/* Refuse: sets the error (say what to do instead) and returns false. */
bool cwed_fail(cwed_call *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* The canonical line of a call (name + every given argument, quoted as
 * needed): what the history and the log store and what replays it. */
const char *cwed_call_line(cwed_call *c);

/* Quote one argument for a command line when needed ("a b" -> "\"a b\"");
 * frame-arena string. Use it when building lines from user text. */
const char *cwed_quote(const char *arg);

/* ── history ──────────────────────────────────────────────────────────── */
typedef struct cwed_history_info {
  int undo_count, redo_count;
  const char *undo_line, *redo_line; /* next undo / redo (NULL when none) */
} cwed_history_info;
/* doc NULL = the editor-wide history. */
cwed_history_info cwed_history(struct cwed_doc *doc);
/* Entry i (0 = oldest) of the undo+redo list; *is_done false for redo entries. */
const char *cwed_history_line(struct cwed_doc *doc, int i, bool *is_done);
/* End merging: the next command starts a new entry even with the same key. */
void cwed_history_seal(void);

/* ── documents ────────────────────────────────────────────────────────── */
typedef struct cwed_doc cwed_doc;
cwed_doc *cwed_doc_active(void);  /* NULL when no scene is open */
int cwed_doc_count(void); /* open documents */
cwed_doc *cwed_doc_at(int i);     /* open order */
int cwed_doc_index(cwed_doc *d); /* position in open order; -1 when d is not open */
cw_node cwed_doc_root(cwed_doc *d);     /* the scene's root node */
cw_tree *cwed_doc_tree(cwed_doc *d);    /* never processed; the root sits under its /root */
const char *cwed_doc_path(cwed_doc *d); /* project path ("levels/one.cwscene"); "" until first saved */
const char *cwed_doc_title(cwed_doc *d); /* file name, or "untitled N" */
bool cwed_doc_dirty(cwed_doc *d); /* history moved since the last save; false for NULL */
uint32_t cwed_doc_version(cwed_doc *d); /* bumps on every change (do, undo, redo) */
/* Node by scene path ("." root); null when missing. */
cw_node cwed_doc_node(cwed_doc *d, const char *path);
/* Scene path of n ("." for the root); frame-arena string; "" when n is not in d. */
const char *cwed_doc_node_path(cwed_doc *d, cw_node n);
/* The scene as .cwscene text now (cw_alloc'd). */
char *cwed_doc_text(cwed_doc *d);
/* Run fn with d's tree as the current tree (node("path") values and the
 * scene loader resolve against it); the core does this around scene
 * commands, so command code rarely needs it. */
void cwed_doc_enter(cwed_doc *d);
void cwed_doc_leave(void); /* undo the matching cwed_doc_enter (calls nest) */

/* ── selection (per document; shared by every panel) ───────────────────── */
int cwed_selection_count(void);          /* of the active document */
cw_node cwed_selection_get(int i); /* null when out of range or no document */
bool cwed_selection_has(cw_node n); /* n is selected in the active document */
/* Change through the `select` commands (they log and notify); these are
 * what those commands call. */
void cwed_selection_set(const cw_node *nodes, int count);

/* ── events ─────────────────────────────────────────────────────────────
 * Panels keep themselves current by subscribing instead of polling. */
typedef enum {
  CWED_EV_DOC_OPENED = 1 << 0,
  CWED_EV_DOC_CLOSED = 1 << 1,
  CWED_EV_DOC_ACTIVE = 1 << 2,   /* another document became active (or none) */
  CWED_EV_DOC_CHANGED = 1 << 3,  /* the active document's scene changed (do/undo/redo); e.line says by what */
  CWED_EV_DOC_SAVED = 1 << 4,
  CWED_EV_SELECTION = 1 << 5,
  CWED_EV_HISTORY = 1 << 6,      /* any history changed (also on dirty-flag changes) */
  CWED_EV_SETTINGS = 1 << 7,     /* e.key says which setting */
  CWED_EV_PROJECT = 1 << 8,      /* a project was opened */
  CWED_EV_LOG = 1 << 9,          /* a line was added to the log (e.line) */
  CWED_EV_COMMAND = 1 << 10,     /* any command ran (e.line, e.ok) */
  CWED_EV_FILES = 1 << 11,       /* project files changed (written, moved, removed, imported, rescanned):
                                  * e.line = the path, or NULL for "several, rescan"; modules emit it */
  CWED_EV_PLUGINS = 1 << 13,     /* plugins loaded, unloaded or reloaded (e.line = the plugin): rebuild
                                  * anything built from registrations (inspector editors, gizmo lists);
                                  * also sent right BEFORE a plugin's node types are withdrawn: free any
                                  * node you keep for yourself (a type's default values) now */
  CWED_EV_ALL = 0x7fffffff
} cwed_event_type;
typedef struct cwed_event {
  cwed_event_type type;
  cwed_doc *doc;
  const char *line; /* DOC_CHANGED / LOG / COMMAND */
  const char *key;  /* SETTINGS */
  bool ok;          /* COMMAND */
} cwed_event;
typedef void (*cwed_event_fn)(const cwed_event *e, void *user);
/* Calls fn for events in mask (CWED_EV_*); there is no unsubscribe, so subscribe once. */
void cwed_subscribe(uint32_t mask, cwed_event_fn fn, void *user);
void cwed_emit(const cwed_event *e); /* the core emits all of the above; modules may emit LOG and FILES */

/* ── log (the Output panel shows it; `ed log` returns it) ───────────────── */
typedef enum { CWED_LOG_INFO, CWED_LOG_WARN, CWED_LOG_ERROR, CWED_LOG_COMMAND } cwed_log_kind;
/* Adds a line (cut at 1023 bytes) and emits CWED_EV_LOG. */
void cwed_log(cwed_log_kind kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int cwed_log_count(void); /* lines kept, at most 2000 */
const char *cwed_log_line(int i, cwed_log_kind *kind); /* 0 = oldest kept (the last 2000) */

/* ── panels ───────────────────────────────────────────────────────────── */
typedef struct cwed_panel_desc {
  const char *id;           /* "history": unique, used by panel.move / panel.show */
  const char *title;        /* tab title ("History") */
  const char *slot;         /* default slot: left_top left_bottom center bottom right_top right_bottom */
  int order;                /* position among the slot's default panels (lower first) */
  /* Build the panel's content (a Control subtree, not yet in a tree). The
   * dock wraps it in a page named after the title. Called once at startup. */
  cw_node (*create)(void *user);
  void *user;
} cwed_panel_desc;
/* Desc copied (strings must be static). Only from a module's init (asserts after the dock is placed);
 * an unknown slot logs a warning and uses right_top. */
void cwed_panel_register(const cwed_panel_desc *d);
cw_node cwed_panel_content(const char *id); /* what create returned; null when unknown */
bool cwed_panel_visible(const char *id);    /* shown in a slot (it may be a background tab) */
/* Slot names and the panels in a slot, as the layout has them now. */
int cwed_slot_count(void);
const char *cwed_slot_name(int i); /* static name; NULL out of range */

/* ── menus, toolbar, status bar ─────────────────────────────────────────
 * Menu items run command lines. path = "Menu/Item" ("File/Save Scene");
 * order sorts items inside a menu (separators where the hundreds digit
 * changes: 100, 110 | 200). The shortcut shown is the command's. */
void cwed_menu_add(const char *path, const char *command_line, int order);
/* A toolbar button (top bar, right side) running a command line. */
void cwed_toolbar_add(const char *id, const char *label, const char *command_line, int order);
/* Status bar text in a named field (fields sorted by order, left to right). */
void cwed_status_set(const char *field, int order, const char *text);

/* ── settings ─────────────────────────────────────────────────────────
 * Typed key/value pairs with defaults, persisted natively in a file
 * (CW_EDITOR_SETTINGS, else $XDG_CONFIG_HOME/cartwheel/editor.cfg, else
 * ~/.config/cartwheel/editor.cfg; test runs with CW_FRAMES persist only
 * with CW_EDITOR_SETTINGS set) and in a cart in the project layer at
 * .cweditor/settings.cfg (exported with `vfs export`, decision D6). Change
 * them with the settings.set command (undoable, editor-wide history). */
typedef struct cwed_setting_desc {
  const char *key;      /* "interface.scale" */
  cwed_arg_type type;   /* STRING INT FLOAT BOOL */
  const char *def;      /* default as text */
  const char *doc;
  const char *choices;  /* STRING: "dark|light" (an option list in the dialog); NULL = free text */
  double min, max;      /* INT/FLOAT range when min < max */
  bool hidden;          /* not shown in the settings dialog (layout state) */
} cwed_setting_desc;
/* Desc copied (static strings); redefining keeps the current value. */
void cwed_setting_define(const cwed_setting_desc *d);
const char *cwed_setting(const char *key); /* text; NULL when undefined */
double cwed_setting_float(const char *key); /* strtod of the text; 0 when undefined */
bool cwed_setting_bool(const char *key); /* true for true/1/on; false otherwise or undefined */
/* Set without a command (layout bookkeeping); persists and notifies. */
void cwed_setting_put(const char *key, const char *value);

/* ── inspector plugins (property editors; the Inspector panel uses them) ──
 * The inspector asks every plugin whose handles() says yes and takes the one
 * with the highest priority (the built-in editors are priority 0, so a module
 * takes over a type or a single property with priority > 0; ties go to the
 * first registered). Edits go through commands: cwed_prop_commit runs
 * node.set (one node selected) or node.set_many (several) with merge key
 * "inspector:<prop>", so a drag is one undo step. */
typedef struct cwed_prop_ctx {
  cw_node node;          /* the node shown (the first selected) */
  const cw_prop *prop;
  cw_value value;        /* current value */
} cwed_prop_ctx;
typedef struct cwed_inspector_plugin {
  const char *name;
  int priority;
  bool (*handles)(cw_node n, const cw_prop *p);
  /* Build the editor (a Control). Call cwed_prop_commit on edits; refresh
   * is called with a new value when the property changed elsewhere. */
  cw_node (*create)(const cwed_prop_ctx *ctx);
  void (*refresh)(cw_node editor, const cwed_prop_ctx *ctx);
} cwed_inspector_plugin;
void cwed_inspector_register(const cwed_inspector_plugin *p); /* keeps the pointer: p must stay valid (static) */
int cwed_inspector_count(void); /* registered plugins */
const cwed_inspector_plugin *cwed_inspector_at(int i); /* registration order; NULL out of range */
/* The winner (rules above); NULL when none handles it. */
const cwed_inspector_plugin *cwed_inspector_for(cw_node n, const cw_prop *p);
/* Commit a value from a property editor (value in scene-file text form).
 * final = false while dragging (merged into one undo step), true on release. */
bool cwed_prop_commit(const char *prop, const char *value_text, bool final);

/* ── gizmos (viewports draw and drive them; M4.1) ───────────────────────── */
typedef struct cwed_gizmo_ctx cwed_gizmo_ctx; /* defined by the viewport module (M4.1) */
typedef struct cwed_gizmo_desc {
  const char *name;
  const char *node_type;  /* applies to nodes of this type (or derived) */
  bool is_3d;
  void (*draw)(cw_node n, cwed_gizmo_ctx *ctx);
  /* pointer pressed on the gizmo / dragged / released: edit through
   * commands with a merge key so a drag is one undo step. */
  bool (*input)(cw_node n, cwed_gizmo_ctx *ctx, const cw_ui_event *e);
} cwed_gizmo_desc;
void cwed_gizmo_register(const cwed_gizmo_desc *g); /* keeps the pointer: g must stay valid (static) */
int cwed_gizmo_count(void); /* registered gizmos */
const cwed_gizmo_desc *cwed_gizmo_at(int i); /* registration order; NULL out of range */

/* ── play-in-editor backend (M4.3: the game in its own process) ───────────
 * The core owns the play.* commands, the toolbar's Play/Pause/Step/Stop and
 * their state; the M4.3 module provides the backend, built on the remote
 * game client (cartwheel/remote.h): natively a crash-isolated child
 * process, as a cart a peer cart through romdev/wasmcart. Until one is
 * registered, play.* commands fail with a message saying so. */
typedef struct cwed_play_backend {
  const char *name;
  /* start the game at a scene (project path, saved first by the core) */
  bool (*start)(const char *scene_path, char *err, int err_cap);
  void (*stop)(void);
  bool (*running)(void);
  void (*set_paused)(bool paused);
  void (*step)(int ticks);
  /* forward one debug-channel request to the running game; the reply
   * arrives through cb (possibly frames later). */
  bool (*request)(const char *line, void (*cb)(const char *json, void *user), void *user);
  /* called once per editor frame while registered */
  void (*poll)(void);
  /* whether the running game is paused by the debugger (the core toggles
   * play.pause from this; NULL = the core tracks it itself) */
  bool (*paused)(void);
} cwed_play_backend;
/* One backend: replaces any previous one; b is kept by pointer (static). */
void cwed_play_register(const cwed_play_backend *b);
const cwed_play_backend *cwed_play_backend_get(void); /* NULL until one is registered */

/* ── project ──────────────────────────────────────────────────────────── */
/* The project's name and main scene (project.cwproj, key = value lines). */
const char *cwed_project_name(void);
const char *cwed_project_main_scene(void); /* project path; "" when none. Valid until the next project load */
/* Natively: the project directory ("" when the project lives in memory). */
const char *cwed_project_dir(void);
/* Any key of project.cwproj (`key = value` lines; quotes trimmed); NULL when
 * the project has no such key. Re-read when a project opens (CWED_EV_PROJECT). */
const char *cwed_project_key(const char *key);
/* Re-read project.cwproj (name, main scene, scenes) after a module changed
 * it, and emit CWED_EV_PROJECT (M4.6: project settings, rename). */
void cwed_project_reload(void);

/* ── drop targets shared between modules ────────────────────────────────
 * The engine keeps ONE drop target per control (cw_ui_set_drop_target), but
 * several modules may accept drops on the same panel: a scene view takes
 * "file" drags from the FileSystem and "node" drags from the scene tree.
 * Register per (control, kind) here instead: the core installs a single
 * engine drop target on the control that offers each drag to the handlers
 * of its kind, in registration order. */
typedef struct cwed_drop_desc {
  const char *kind; /* "file", "node", ...: only drags of this kind reach the handler */
  bool (*can_drop)(cw_node target, const cw_ui_drag *d, cw_vec2 local, cw_rect *highlight, void *user);
  void (*drop)(cw_node target, const cw_ui_drag *d, cw_vec2 local, void *user);
  void *user;
} cwed_drop_desc;
/* Add a handler (desc copied; kind interned). Adding the same (target,
 * kind, can_drop, user) again is a no-op. Entries of freed controls go away. */
void cwed_drop_add(cw_node target, const cwed_drop_desc *d);

/* ── utilities for panels ───────────────────────────────────────────── */
/* The editor's UI tree (the one the dock lives in). */
cw_tree *cwed_ui_tree(void);
/* Remove and free every child of a Control. */
void cwed_clear_children(cw_node n);

#ifdef __cplusplus
}
#endif
#endif

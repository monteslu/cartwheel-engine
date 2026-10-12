/* Editor core internals: shared by the editor core files only. Modules use
 * cwed/editor.h and nothing else. */
#ifndef CWED_INTERNAL_H
#define CWED_INTERNAL_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"

#define CWED_MAX_ARGS 16

struct cwed_json {
  char *buf;
  size_t len, cap;
};
void cwed__json_reset(cwed_json *j);
void cwed__json_free(cwed_json *j);

/* ── commands (commands.c) ─────────────────────────────────────────── */
struct cwed_call {
  const cwed_command_desc *desc;
  cwed_doc *doc;
  int nargs;
  const char *vals[CWED_MAX_ARGS]; /* interned, by param index; NULL = not given */
  const char *merge;               /* interned merge key or NULL */
  char *line;                      /* canonical line (cw_alloc'd) */
  void *state;
  uint64_t id;                     /* unique, changes when merged into */
  cwed_json reply;
  char error[512];
  bool failed;
};

typedef struct cwed_hist {
  CW_ARRAY(cwed_call *) entries;
  int pos; /* entries[0, pos) are done; [pos, count) can be redone */
} cwed_hist;
uint64_t cwed__history_marker(cwed_hist *h); /* id of the last done entry, 0 = none */
void cwed__history_free(cwed_hist *h);
bool cwed__undo(cwed_hist *h, cwed_doc *doc, char *err, int cap);
bool cwed__redo(cwed_hist *h, cwed_doc *doc, char *err, int cap);
cwed_hist *cwed__global_history(void);
void cwed__commands_init(void);
uint32_t cwed__commands_run_count(void);
uint32_t cwed__commands_error_count(void);
/* tokenizer: splits a command line; returns count, tokens interned; *quoted per token */
int cwed__tokenize(const char *line, const char **tok, bool *quoted, int cap);

/* ── documents (docs.c) ────────────────────────────────────────────── */
struct cwed_doc {
  cw_tree *tree;
  cw_node root;
  char path[512];
  char title[160];
  cwed_hist hist;
  uint64_t saved_marker;
  uint32_t version;
  CW_ARRAY(cw_node) sel;
  uint32_t text_hash; /* hash of the scene text, updated on change */
  int node_count;
};
void cwed__docs_init(void);
void cwed__doc_changed(cwed_doc *d, const char *line); /* bump version, refresh hash, emit */
void cwed__doc_set_active(cwed_doc *d);
void cwed__docs_shutdown(void);
/* every .cwscene the project knows (project.cwproj `scenes`, the native
 * project directory, the in-memory layer), sorted; frame-arena strings */
int cwed__project_scenes(const char ***out);

/* ── selection, events, log, settings ──────────────────────────────── */
void cwed__selection_prune(cwed_doc *d);
uint32_t cwed__selection_hash(void);
void cwed__log_init(void);
void cwed__settings_init(void);
void cwed__settings_load(void);
void cwed__settings_flush(bool force);
void cwed__settings_apply_all(void);
void cwed__project_init(int argc, char **argv);
int cwed__settings_count(void);
const cwed_setting_desc *cwed__settings_at(int i); /* pointer valid until the next define */
void cwed__settings_tick(double dt);
void cwed__log_drain(void);
void cwed__selection_commands(void);
cw_node cwed__dock_doc_bar(void);

/* ── dock, shell (dock.c, shell.c) ─────────────────────────────────── */
void cwed__dock_build(cw_node parent);     /* builds the slots under parent */
void cwed__dock_place_all(void);           /* panels into default slots, then the saved layout */
void cwed__dock_frame(void);               /* per frame: drag placeholders, slot visibility */
uint32_t cwed__dock_layout_hash(void);
void cwed__dock_commands(void);
void cwed__shell_build(void);              /* the whole window: top bar, dock, status bar */
void cwed__shell_frame(double dt);
void cwed__shell_commands(void);
void cwed__shell_refresh_menus(void);
bool cwed__palette_open(void);
void cwed__theme_apply(void);
cw_node cwed__root(void);

/* ── extensions.c ──────────────────────────────────────────────────── */
void cwed__play_commands(void);
void cwed__play_frame(void);

/* ── node commands (node_cmds.c), scene commands (docs.c) ───────────── */
void cwed__node_commands(void);
void cwed__scene_commands(void);
void cwed__editor_commands(void); /* help, editor.state, history.*, log.tail, settings.* */

/* ── registration owners (owners.c, M5.2 plugins) ───────────────────── */
extern int cwed__owner; /* who registers now: 0 = the editor, else a plugin (cwed_owner_push) */
/* per registry: count what an owner registered / withdraw it */
void cwed__commands_owned(int owner, cwed_owner_stats *s);
void cwed__commands_remove_owner(int owner);
void cwed__dock_owned(int owner, cwed_owner_stats *s);
void cwed__dock_remove_owner(int owner);
void cwed__shell_owned(int owner, cwed_owner_stats *s);
void cwed__shell_remove_owner(int owner);
void cwed__extensions_owned(int owner, cwed_owner_stats *s);
void cwed__extensions_remove_owner(int owner);
void cwed__events_owned(int owner, cwed_owner_stats *s);
void cwed__events_remove_owner(int owner);

/* the module table (generated by the build: editor_modules.c) */
extern const cwed_module *const cwed__modules[];
extern const char *const cwed__module_names[];
extern const int cwed__module_count;

/* lifecycle (main.c and the tests) */
void cwed_init(int argc, char **argv);
void cwed_frame(double dt);
void cwed_shutdown(void);
void cwed_debug_fields(void);

#endif

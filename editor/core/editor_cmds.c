/* Editor-wide commands: help, state, undo/redo, history, log. */
#include "internal.h"

static bool cmd_help(cwed_call *c) {
  const char *prefix = cwed_arg(c, "prefix");
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "commands");
  cwed_json_raw(j, "[");
  bool first = true;
  for (int i = 0; i < cwed_command_count(); i++) {
    const cwed_command_desc *d = cwed_command_at(i);
    if (*prefix && strncmp(d->name, prefix, strlen(prefix))) continue;
    cwed_json_raw(j, first ? "{\"name\":" : ",{\"name\":");
    first = false;
    cwed_json_str(j, d->name);
    cwed_json_raw(j, ",\"title\":");
    cwed_json_str(j, d->title ? d->title : d->name);
    if (*prefix) { /* full detail only when narrowed */
      cwed_json_raw(j, ",\"doc\":");
      cwed_json_str(j, d->doc ? d->doc : "");
      cwed_json_raw(j, ",\"params\":[");
      for (int k = 0; d->params && d->params[k].name; k++) {
        static const char *tn[] = { "string", "int", "float", "bool", "path", "value" };
        cwed_json_printf(j, "%s{\"name\":\"%s\",\"type\":\"%s\"", k ? "," : "", d->params[k].name, tn[d->params[k].type]);
        if (d->params[k].def) {
          cwed_json_raw(j, ",\"default\":");
          cwed_json_str(j, d->params[k].def);
        }
        if (d->params[k].doc) {
          cwed_json_raw(j, ",\"doc\":");
          cwed_json_str(j, d->params[k].doc);
        }
        cwed_json_raw(j, "}");
      }
      cwed_json_raw(j, "]");
    }
    if (d->flags & CWED_CMD_UNDO) cwed_json_raw(j, ",\"undoable\":true");
    if (d->shortcut) {
      cwed_json_raw(j, ",\"shortcut\":");
      cwed_json_str(j, d->shortcut);
    }
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_state(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_doc *d = cwed_doc_active();
  cwed_json_key(j, "project");
  cwed_json_str(j, cwed_project_name());
  cwed_json_key(j, "docs");
  cwed_json_printf(j, "%d", cwed_doc_count());
  cwed_json_key(j, "active");
  if (d) {
    cwed_history_info h = cwed_history(d);
    cwed_json_raw(j, "{\"title\":");
    cwed_json_str(j, cwed_doc_title(d));
    cwed_json_raw(j, ",\"path\":");
    cwed_json_str(j, cwed_doc_path(d));
    cwed_json_printf(j, ",\"dirty\":%s,\"nodes\":%d,\"version\":%u,\"undo\":%d,\"redo\":%d,\"text_hash\":%u}",
                     cwed_doc_dirty(d) ? "true" : "false", d->node_count, d->version, h.undo_count, h.redo_count, d->text_hash);
  } else cwed_json_raw(j, "null");
  cwed_json_key(j, "selection");
  cwed_json_raw(j, "[");
  for (int i = 0; i < cwed_selection_count(); i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, cwed_doc_node_path(d, cwed_selection_get(i)));
  }
  cwed_json_raw(j, "]");
  cwed_history_info g = cwed_history(NULL);
  cwed_json_printf(j, ",\"editor_undo\":%d,\"editor_redo\":%d,\"commands_run\":%u,\"command_errors\":%u",
                   g.undo_count, g.redo_count, cwed__commands_run_count(), cwed__commands_error_count());
  cwed_json_printf(j, ",\"layout_hash\":%u,\"palette_open\":%s", cwed__dock_layout_hash(), cwed__palette_open() ? "true" : "false");
  const cwed_play_backend *pb = cwed_play_backend_get();
  cwed_json_key(j, "play");
  if (pb) {
    cwed_json_raw(j, "{\"backend\":");
    cwed_json_str(j, pb->name);
    cwed_json_printf(j, ",\"running\":%s,\"paused\":%s}", pb->running && pb->running() ? "true" : "false",
                     pb->paused && pb->paused() ? "true" : "false");
  } else cwed_json_raw(j, "null");
  return true;
}

static bool cmd_undo(cwed_call *c) {
  char err[256];
  bool editor = !strcmp(cwed_arg(c, "scope"), "editor") || !cwed_doc_active();
  cwed_doc *d = editor ? NULL : cwed_doc_active();
  if (!cwed__undo(d ? &d->hist : cwed__global_history(), d, err, sizeof err)) return cwed_fail(c, "%s", err);
  cwed_history_info h = cwed_history(d);
  cwed_json_printf(cwed_reply(c), "\"undo\":%d,\"redo\":%d", h.undo_count, h.redo_count);
  return true;
}

static bool cmd_redo(cwed_call *c) {
  char err[512];
  bool editor = !strcmp(cwed_arg(c, "scope"), "editor") || !cwed_doc_active();
  cwed_doc *d = editor ? NULL : cwed_doc_active();
  if (!cwed__redo(d ? &d->hist : cwed__global_history(), d, err, sizeof err)) return cwed_fail(c, "%s", err);
  cwed_history_info h = cwed_history(d);
  cwed_json_printf(cwed_reply(c), "\"undo\":%d,\"redo\":%d", h.undo_count, h.redo_count);
  return true;
}

static bool cmd_goto(cwed_call *c) {
  bool editor = !strcmp(cwed_arg(c, "scope"), "editor") || !cwed_doc_active();
  cwed_doc *d = editor ? NULL : cwed_doc_active();
  cwed_hist *h = d ? &d->hist : cwed__global_history();
  int to = (int)cwed_arg_int(c, "index");
  if (to < 0 || to > h->entries.count) return cwed_fail(c, "index is 0..%d (the number of done entries wanted)", h->entries.count);
  char err[512];
  while (h->pos > to)
    if (!cwed__undo(h, d, err, sizeof err)) return cwed_fail(c, "%s", err);
  while (h->pos < to)
    if (!cwed__redo(h, d, err, sizeof err)) return cwed_fail(c, "%s", err);
  cwed_json_printf(cwed_reply(c), "\"undo\":%d,\"redo\":%d", h->pos, h->entries.count - h->pos);
  return true;
}

static bool cmd_history(cwed_call *c) {
  bool editor = !strcmp(cwed_arg(c, "scope"), "editor") || !cwed_doc_active();
  cwed_doc *d = editor ? NULL : cwed_doc_active();
  cwed_hist *h = d ? &d->hist : cwed__global_history();
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "scope");
  cwed_json_str(j, d ? cwed_doc_title(d) : "editor");
  cwed_json_printf(j, ",\"pos\":%d,\"entries\":[", h->pos);
  for (int i = 0; i < h->entries.count; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, h->entries.data[i]->line);
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_log(cwed_call *c) {
  int n = (int)cwed_arg_int(c, "count"), total = cwed_log_count();
  if (n <= 0 || n > total) n = total;
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "lines");
  cwed_json_raw(j, "[");
  static const char *kinds[] = { "info", "warn", "error", "command" };
  for (int i = total - n; i < total; i++) {
    cwed_log_kind k;
    const char *l = cwed_log_line(i, &k);
    cwed_json_printf(j, "%s{\"kind\":\"%s\",\"text\":", i > total - n ? "," : "", kinds[k]);
    cwed_json_str(j, l);
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_quit(cwed_call *c) {
  for (int i = 0; i < cwed_doc_count(); i++)
    if (cwed_doc_dirty(cwed_doc_at(i)) && !cwed_arg_bool(c, "force"))
      return cwed_fail(c, "%s has unsaved changes: scene.save_all, or editor.quit force=true", cwed_doc_title(cwed_doc_at(i)));
  cwed__settings_flush(true);
  cw_quit();
  return true;
}

void cwed__selection_commands(void);

void cwed__editor_commands(void) {
  static const cwed_param help_p[] = { { "prefix", CWED_ARG_STRING, "", "only commands starting with this (and then with docs and params)" }, { 0 } };
  static const cwed_param scope_p[] = { { "scope", CWED_ARG_STRING, "scene", "scene (the active scene's history) or editor" }, { 0 } };
  static const cwed_param goto_p[] = {
    { "index", CWED_ARG_INT, NULL, "how many entries should be done (0 = undo everything)" },
    { "scope", CWED_ARG_STRING, "scene", "scene or editor" }, { 0 } };
  static const cwed_param log_p[] = { { "count", CWED_ARG_INT, "50", "the last N lines" }, { 0 } };
  static const cwed_param quit_p[] = { { "force", CWED_ARG_BOOL, "false", "quit with unsaved changes" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "help", .title = "Help: List Commands", .params = help_p, .flags = CWED_CMD_QUERY,
    .doc = "Every editor command (name, title, shortcut, undoable); with a prefix also each one's doc and parameters.", .exec = cmd_help });
  cwed_command_register(&(cwed_command_desc){ .name = "editor.state", .title = "Editor State", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "What the editor shows: project, open scenes, the active one (dirty, nodes, undo/redo, text hash), selection, layout, play.",
    .exec = cmd_state });
  cwed_command_register(&(cwed_command_desc){ .name = "edit.undo", .title = "Undo", .params = scope_p, .shortcut = "ctrl+z",
    .doc = "Undo the last change of the active scene (scope=editor: the last settings/editor change).", .exec = cmd_undo });
  cwed_command_register(&(cwed_command_desc){ .name = "edit.redo", .title = "Redo", .params = scope_p, .shortcut = "ctrl+shift+z",
    .doc = "Redo the last undone change.", .exec = cmd_redo });
  cwed_command_register(&(cwed_command_desc){ .name = "history.goto", .title = "Go to History Entry", .params = goto_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Undo or redo until `index` entries are done.", .exec = cmd_goto });
  cwed_command_register(&(cwed_command_desc){ .name = "history.list", .title = "History", .params = scope_p, .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The undo history as command lines (pos = how many are done; later ones can be redone). Replaying the done lines "
           "on the saved scene reproduces it.", .exec = cmd_history });
  cwed_command_register(&(cwed_command_desc){ .name = "log.tail", .title = "Log", .params = log_p, .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The last lines of the editor log (commands run, errors, engine warnings).", .exec = cmd_log });
  cwed_command_register(&(cwed_command_desc){ .name = "editor.quit", .title = "Quit", .params = quit_p, .shortcut = "ctrl+q",
    .doc = "Leave the editor (natively; a cart ignores it).", .exec = cmd_quit });
  cwed__selection_commands();
}

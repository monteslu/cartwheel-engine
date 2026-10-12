/* Export presets (M4.6): per-platform presets in project.cwproj, validation,
 * one-click export with progress, output in the Output panel.
 *
 * Presets are flat keys:
 *   export.<preset>.platform = linux-x64 | linux-arm64 | windows-x64 |
 *                              android-arm64 | web | wasc
 *   export.<preset>.output   = exports/<preset>   (relative to the project)
 *   export.<preset>.<key>    = ... (whatever the platform's exporter reads)
 * The editor builds nothing itself: export.run starts the engine's exporter
 * (tools/cwexport.mjs, M5.0) as a child process, exactly as a shell would:
 *   node <engine>/tools/cwexport.mjs --project <dir> --preset a,b [--validate]
 * and reads its `@progress <preset> <0..100> <stage>` and `@result {json}`
 * lines. So an export from the editor and one from the command line are the
 * same files.
 *
 * Commands: export.presets, export.add, export.remove, export.validate,
 * export.run, export.status, export.cancel. Panel: Export. */
/* paths and messages are cut at their buffer on purpose (each limit is far above real use) */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/process.h"
#include "cartwheel/project.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"

#if !CW_CART
#include <sys/stat.h>
#endif

static const char *const PLATFORMS[] = { "linux-x64", "linux-arm64", "windows-x64", "android-arm64", "web", "wasc", "macos", "ios", NULL };

static const char *engine_dir(void) {
#if CW_CART
  return "";
#else
  const char *e = getenv("CARTWHEEL_DIR");
  if (e && *e) return e;
#ifdef CWED_ENGINE_DIR
  return CWED_ENGINE_DIR;
#else
  return "";
#endif
#endif
}

static const char *tool_path(void) { return cw_frame_printf("%s/tools/cwexport.mjs", engine_dir()); }

static bool tool_exists(void) {
#if CW_CART
  return false;
#else
  struct stat st;
  return !stat(tool_path(), &st);
#endif
}

/* ── presets ────────────────────────────────────────────────────────── */

#define MAX_PRESETS 32
static char preset_names[MAX_PRESETS][64];
static int npresets;

static bool valid_preset_name(const char *n) {
  if (!*n || strlen(n) > 48) return false;
  for (const char *s = n; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return false;
  return true;
}

/* presets in file order (from their keys) */
static int presets_scan(void) {
  npresets = 0;
  for (int i = 0; i < cw_project_count(); i++) {
    const char *k = cw_project_key(i);
    if (strncmp(k, "export.", 7)) continue;
    const char *dot = strchr(k + 7, '.');
    if (!dot || dot == k + 7) continue;
    char name[64];
    snprintf(name, sizeof name, "%.*s", (int)(dot - (k + 7)), k + 7);
    bool seen = false;
    for (int p = 0; p < npresets; p++)
      if (!strcmp(preset_names[p], name)) seen = true;
    if (!seen && npresets < MAX_PRESETS) snprintf(preset_names[npresets++], 64, "%s", name);
  }
  return npresets;
}

static bool preset_exists(const char *name) {
  presets_scan();
  for (int i = 0; i < npresets; i++)
    if (!strcmp(preset_names[i], name)) return true;
  return false;
}

static const char *preset_get(const char *preset, const char *key) {
  const char *v = cw_project_get(cw_frame_printf("export.%s.%s", preset, key));
  return v ? v : "";
}

static const char *preset_output(const char *preset) {
  const char *o = preset_get(preset, "output");
  return *o ? o : cw_frame_printf("exports/%s", preset);
}

/* local checks (the exporter adds its own: toolchains, platform keys);
 * NULL = fine, else what is wrong and how to fix it */
static const char *preset_problem(const char *p) {
  if (!valid_preset_name(p)) return cw_frame_printf("preset name '%s': use a-z 0-9 _ -", p);
  const char *plat = preset_get(p, "platform");
  if (!*plat)
    return cw_frame_printf("export.%s.platform is not set: project.set key=export.%s.platform value=<linux-x64|windows-x64|android-arm64|web|wasc>",
                           p, p);
  bool known = false;
  for (int i = 0; PLATFORMS[i]; i++)
    if (!strcmp(PLATFORMS[i], plat)) known = true;
  if (!known)
    return cw_frame_printf("export.%s.platform = %s: unknown platform (linux-x64, linux-arm64, windows-x64, android-arm64, web, wasc, macos, ios)", p, plat);
  const char *out = preset_output(p);
  if (out[0] == '/' || strstr(out, "..") || strchr(out, '\\'))
    return cw_frame_printf("export.%s.output = %s: a directory inside the project (relative, no '..')", p, out);
  return NULL;
}

/* ── file-snapshot undo (export.add / export.remove change several keys) ── */

typedef struct { char *old_text, *new_text; size_t old_len, new_len; } snap;

static void snap_free(cwed_call *c, void *p) {
  (void)c;
  snap *s = p;
  if (!s) return;
  free(s->old_text);
  free(s->new_text);
  cw_free(s);
}

static bool panel_dirty = true;

static void snap_apply(const char *text, size_t len) {
  if (text) cw_vfs_write("project.cwproj", text, len);
  else cw_vfs_remove("project.cwproj");
  cw_project_reload();
  panel_dirty = true;
}

static snap *snap_begin(void) {
  snap *s = cw_alloc(sizeof *s, CW_MEM_GENERAL);
  memset(s, 0, sizeof *s);
  s->old_text = cw_vfs_read("project.cwproj", &s->old_len);
  return s;
}

static void snap_end(snap *s) { s->new_text = cw_vfs_read("project.cwproj", &s->new_len); }

static void undo_snap(cwed_call *c) {
  snap *s = cwed_call_state(c);
  snap_apply(s->old_text, s->old_len);
}

static bool cmd_add(cwed_call *c) {
  snap *s = cwed_call_state(c);
  if (s) { /* redo */
    snap_apply(s->new_text, s->new_len);
    return true;
  }
  const char *name = cwed_arg(c, "name"), *plat = cwed_arg(c, "platform"), *out = cwed_arg(c, "output");
  if (!valid_preset_name(name)) return cwed_fail(c, "name: a preset name of a-z 0-9 _ - (e.g. linux, cart)");
  if (preset_exists(name)) return cwed_fail(c, "preset %s exists: change it with project.set key=export.%s.<key>, or export.remove it", name, name);
  bool known = false;
  for (int i = 0; PLATFORMS[i]; i++)
    if (!strcmp(PLATFORMS[i], plat)) known = true;
  if (!known) return cwed_fail(c, "platform %s: one of linux-x64, linux-arm64, windows-x64, android-arm64, web, wasc, macos, ios", plat);
  if (*out && (out[0] == '/' || strstr(out, ".."))) return cwed_fail(c, "output: a directory inside the project (relative, no '..')");
  s = snap_begin();
  bool ok = cw_project_set(cw_frame_printf("export.%s.platform", name), plat) &&
            cw_project_set(cw_frame_printf("export.%s.output", name), *out ? out : cw_frame_printf("exports/%s", name));
  if (!ok) {
    snap_apply(s->old_text, s->old_len);
    snap_free(c, s);
    return cwed_fail(c, "cannot write project.cwproj");
  }
  snap_end(s);
  cwed_call_set_state(c, s);
  panel_dirty = true;
  cwed_json_key(cwed_reply(c), "preset");
  cwed_json_str(cwed_reply(c), name);
  return true;
}

static bool cmd_remove(cwed_call *c) {
  snap *s = cwed_call_state(c);
  if (s) {
    snap_apply(s->new_text, s->new_len);
    return true;
  }
  const char *name = cwed_arg(c, "name");
  if (!preset_exists(name)) return cwed_fail(c, "no preset %s (export.presets lists them)", name);
  s = snap_begin();
  char prefix[96];
  snprintf(prefix, sizeof prefix, "export.%s.", name);
  for (int again = 1; again;) {
    again = 0;
    for (int i = 0; i < cw_project_count(); i++)
      if (!strncmp(cw_project_key(i), prefix, strlen(prefix))) {
        cw_project_set(cw_project_key(i), NULL); /* reloads: rescan */
        again = 1;
        break;
      }
  }
  snap_end(s);
  cwed_call_set_state(c, s);
  panel_dirty = true;
  cwed_json_key(cwed_reply(c), "removed");
  cwed_json_str(cwed_reply(c), name);
  return true;
}

static bool cmd_presets(cwed_call *c) {
  presets_scan();
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "presets");
  cwed_json_raw(j, "[");
  for (int i = 0; i < npresets; i++) {
    const char *p = preset_names[i], *why = preset_problem(p);
    cwed_json_raw(j, i ? ",{" : "{");
    cwed_json_key(j, "name");
    cwed_json_str(j, p);
    cwed_json_key(j, "platform");
    cwed_json_str(j, preset_get(p, "platform"));
    cwed_json_key(j, "output");
    cwed_json_str(j, preset_output(p));
    cwed_json_key(j, "valid");
    cwed_json_raw(j, why ? "false" : "true");
    if (why) {
      cwed_json_key(j, "problem");
      cwed_json_str(j, why);
    }
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "exporter");
  cwed_json_str(j, CW_CART ? "none (a cart cannot run exporters)" : tool_exists() ? tool_path() : "missing");
  return true;
}

/* "all" or "a,b" -> names; false (with the call failed) on an unknown one */
static bool pick(cwed_call *c, const char *list, char out[MAX_PRESETS][64], int *n) {
  presets_scan();
  *n = 0;
  if (!*list || !strcmp(list, "all")) {
    for (int i = 0; i < npresets; i++) snprintf(out[(*n)++], 64, "%s", preset_names[i]);
    if (!*n) return cwed_fail(c, "the project has no export presets: export.add name=<n> platform=<linux-x64|wasc|...>");
    return true;
  }
  for (const char *s = list; *s && *n < MAX_PRESETS;) {
    const char *e = strchr(s, ',');
    size_t l = e ? (size_t)(e - s) : strlen(s);
    snprintf(out[*n], 64, "%.*s", (int)l, s);
    if (!preset_exists(out[*n])) return cwed_fail(c, "no preset '%s' (export.presets lists them)", out[*n]);
    (*n)++;
    s += l + (e ? 1 : 0);
  }
  return *n > 0 || cwed_fail(c, "presets: all, or names joined by ','");
}

/* ── the exporter job ──────────────────────────────────────────────── */

static struct {
  cw_process *proc;
  char state[16]; /* idle running ok failed canceled */
  bool validate_only;
  char presets[MAX_PRESETS][64];
  int n, progress[MAX_PRESETS];
  char stage[MAX_PRESETS][96];
  char results[8192];
  char error[1024];
  int exit_code, lines, failed;
} J = { .state = "idle", .exit_code = -1 };

static int preset_index(const char *name, size_t len) {
  for (int i = 0; i < J.n; i++)
    if (strlen(J.presets[i]) == len && !strncmp(J.presets[i], name, len)) return i;
  return -1;
}

static bool start_job(cwed_call *c, bool validate_only) {
  char names[MAX_PRESETS][64];
  int n;
  if (!pick(c, cwed_arg(c, "presets"), names, &n)) return false;
  /* local checks first: every problem in one message */
  char problems[2048] = "";
  for (int i = 0; i < n; i++) {
    const char *why = preset_problem(names[i]);
    if (why) snprintf(problems + strlen(problems), sizeof problems - strlen(problems), "%s%s", problems[0] ? "; " : "", why);
  }
  if (problems[0]) {
    cwed_log(CWED_LOG_ERROR, "export: invalid preset: %s", problems);
    return cwed_fail(c, "invalid preset: %s", problems);
  }
  if (CW_CART)
    return cwed_fail(c, "a cart cannot run the exporter (no processes): carry the project out with project.archive and run "
                        "node tools/cwexport.mjs --project <dir> --preset <names> natively");
  const char *dir = cwed_project_dir();
  if (!dir[0]) return cwed_fail(c, "the project lives in memory: create one on disk (project.new) or open one (project.open) to export it");
  if (J.proc) return cwed_fail(c, "an export is running (export.status): wait for it or export.cancel");
  if (!tool_exists())
    return cwed_fail(c, "the exporter %s is not in this engine checkout (M5.0 builds it): set CARTWHEEL_DIR to a checkout that has it",
                     tool_path());
  if (!validate_only)
    for (int i = 0; i < cwed_doc_count(); i++)
      if (cwed_doc_dirty(cwed_doc_at(i)) && cwed_doc_path(cwed_doc_at(i))[0]) cwed_runf("scene.save doc=%d", i);
  char list[2048] = "";
  for (int i = 0; i < n; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? "," : "", names[i]);
  const char *argv[] = { "node", tool_path(), "--project", dir, "--preset", list, validate_only ? "--validate" : NULL, NULL };
  char err[512];
  cw_process *p = cw_process_start(&(cw_process_desc){ .argv = argv }, err, sizeof err);
  if (!p) return cwed_fail(c, "cannot run the exporter: %s", err);
  memset(&J, 0, sizeof J);
  J.proc = p;
  J.validate_only = validate_only;
  J.n = n;
  J.exit_code = -1;
  memcpy(J.presets, names, sizeof names);
  snprintf(J.state, sizeof J.state, "running");
  cwed_log(CWED_LOG_INFO, "export: %s %s (%s)", validate_only ? "validating" : "exporting", list, cwed_project_name());
  cwed_status_set("export", 70, validate_only ? "validating presets..." : "exporting...");
  panel_dirty = true;
  cwed_json_key(cwed_reply(c), "started");
  cwed_json_str(cwed_reply(c), list);
  cwed_json_key(cwed_reply(c), "validate");
  cwed_json_raw(cwed_reply(c), validate_only ? "true" : "false");
  return true;
}

static bool cmd_run(cwed_call *c) { return start_job(c, false); }
static bool cmd_validate(cwed_call *c) {
  /* without an exporter (or in a cart) the local checks are the validation */
  if (CW_CART || !tool_exists() || !cwed_project_dir()[0]) {
    char names[MAX_PRESETS][64];
    int n;
    if (!pick(c, cwed_arg(c, "presets"), names, &n)) return false;
    char problems[2048] = "";
    for (int i = 0; i < n; i++) {
      const char *why = preset_problem(names[i]);
      if (why) snprintf(problems + strlen(problems), sizeof problems - strlen(problems), "%s%s", problems[0] ? "; " : "", why);
    }
    if (problems[0]) {
      cwed_log(CWED_LOG_ERROR, "export: invalid preset: %s", problems);
      return cwed_fail(c, "invalid preset: %s", problems);
    }
    cwed_json_key(cwed_reply(c), "valid");
    cwed_json_raw(cwed_reply(c), "true");
    cwed_json_key(cwed_reply(c), "checked");
    cwed_json_str(cwed_reply(c), "locally (no exporter to ask)");
    return true;
  }
  return start_job(c, true);
}

static bool cmd_cancel(cwed_call *c) {
  if (!J.proc) return cwed_fail(c, "no export is running");
  cw_process_kill(J.proc, false);
  snprintf(J.error, sizeof J.error, "canceled");
  cwed_json_key(cwed_reply(c), "canceled");
  cwed_json_raw(cwed_reply(c), "true");
  return true;
}

static bool cmd_status(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "state");
  cwed_json_str(j, J.state);
  cwed_json_key(j, "validate");
  cwed_json_raw(j, J.validate_only ? "true" : "false");
  cwed_json_key(j, "exit");
  cwed_json_printf(j, "%d", J.exit_code);
  cwed_json_key(j, "lines");
  cwed_json_printf(j, "%d", J.lines);
  cwed_json_key(j, "presets");
  cwed_json_raw(j, "[");
  for (int i = 0; i < J.n; i++) {
    cwed_json_raw(j, i ? ",{" : "{");
    cwed_json_key(j, "name");
    cwed_json_str(j, J.presets[i]);
    cwed_json_key(j, "progress");
    cwed_json_printf(j, "%d", J.progress[i]);
    cwed_json_key(j, "stage");
    cwed_json_str(j, J.stage[i]);
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "results");
  cwed_json_raw(j, "[");
  cwed_json_raw(j, J.results);
  cwed_json_raw(j, "]");
  if (J.error[0]) {
    cwed_json_key(j, "error");
    cwed_json_str(j, J.error);
  }
  return true;
}

static void job_frame(void) {
  if (!J.proc) return;
  for (const char *l; (l = cw_process_read_line(J.proc));) {
    J.lines++;
    if (!strncmp(l, "@progress ", 10)) {
      const char *p = l + 10, *sp = strchr(p, ' ');
      if (!sp) continue;
      int i = preset_index(p, (size_t)(sp - p));
      char *e;
      long pct = strtol(sp + 1, &e, 10);
      if (i >= 0) {
        J.progress[i] = (int)CW_CLAMP(pct, 0, 100);
        snprintf(J.stage[i], sizeof J.stage[i], "%s", *e == ' ' ? e + 1 : "");
        cwed_log(CWED_LOG_INFO, "export: %s %ld%% %s", J.presets[i], pct, J.stage[i]);
      }
      panel_dirty = true;
    } else if (!strncmp(l, "@result ", 8)) {
      size_t have = strlen(J.results);
      if (have + strlen(l) + 2 < sizeof J.results) snprintf(J.results + have, sizeof J.results - have, "%s%s", have ? "," : "", l + 8);
      bool ok = strstr(l, "\"ok\":true") != NULL;
      if (!ok) J.failed++;
      const char *pn = strstr(l, "\"preset\":\"");
      if (pn) {
        pn += 10;
        int i = preset_index(pn, strcspn(pn, "\""));
        if (i >= 0 && ok) J.progress[i] = 100;
      }
      cwed_log(ok ? CWED_LOG_INFO : CWED_LOG_ERROR, "export: %s", l + 8);
      panel_dirty = true;
    } else cwed_log(strstr(l, "error") || strstr(l, "FAILED") ? CWED_LOG_ERROR : strstr(l, "warning") ? CWED_LOG_WARN : CWED_LOG_INFO,
                    "export: %s", l);
  }
  if (cw_process_running(J.proc)) return;
  while (cw_process_read_line(J.proc)) {}
  J.exit_code = cw_process_exit_code(J.proc);
  cw_process_free(J.proc);
  J.proc = NULL;
  bool ok = J.exit_code == 0 && !J.failed;
  snprintf(J.state, sizeof J.state, "%s", !strcmp(J.error, "canceled") ? "canceled" : ok ? "ok" : "failed");
  if (!ok && !J.error[0]) snprintf(J.error, sizeof J.error, "the exporter exited %d (%d preset(s) failed): see the Output panel", J.exit_code, J.failed);
  cwed_log(ok ? CWED_LOG_INFO : CWED_LOG_ERROR, "export: %s %s", J.validate_only ? "validation" : "export", ok ? "done" : J.state);
  cwed_status_set("export", 70, ok ? (J.validate_only ? "presets valid" : "export ok") : (J.validate_only ? "presets INVALID" : "export FAILED"));
  panel_dirty = true;
}

/* ── the Export panel ──────────────────────────────────────────────── */

static cw_node list, platform, name_edit, progress, status;

static void report(bool ok, char *reply) {
  if (!ok && reply) {
    const char *e = strstr(reply, "\"error\":\"");
    cw_label_set_text(status, e ? cw_frame_printf("%.*s", (int)strcspn(e + 9, "\""), e + 9) : reply);
  } else if (ok) cw_label_set_text(status, "");
  cw_free(reply);
}

static void run_line(const char *line) {
  char *reply = NULL;
  bool ok = cwed_run(line, &reply);
  report(ok, reply);
}

static const char *selected_preset(void) {
  int i = cw_item_list_selected(list);
  return i >= 0 && i < npresets ? preset_names[i] : NULL;
}

static void on_add(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  int pi = cw_option_button_selected(platform);
  const char *plat = pi >= 0 && pi < (int)CW_ARRAY_LEN(PLATFORMS) - 1 ? PLATFORMS[pi] : "linux-x64";
  const char *nm = cw_line_edit_text(name_edit);
  run_line(cw_frame_printf("export.add name=%s platform=%s", cwed_quote(*nm ? nm : plat), plat));
}

static void on_remove(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  const char *p = selected_preset();
  if (!p) {
    cw_label_set_text(status, "Select a preset");
    return;
  }
  run_line(cw_frame_printf("export.remove name=%s", cwed_quote(p)));
}

static void on_run(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  const char *what = user;
  if (!strcmp(what, "selected")) {
    const char *p = selected_preset();
    if (!p) {
      cw_label_set_text(status, "Select a preset");
      return;
    }
    run_line(cw_frame_printf("export.run presets=%s", cwed_quote(p)));
  } else if (!strcmp(what, "validate")) run_line("export.validate presets=all");
  else if (!strcmp(what, "cancel")) run_line("export.cancel");
  else run_line("export.run presets=all");
}

static void panel_refresh(void) {
  panel_dirty = false;
  if (!cw_node_valid(list)) return;
  int sel = cw_item_list_selected(list);
  cw_item_list_clear(list);
  presets_scan();
  for (int i = 0; i < npresets; i++) {
    const char *p = preset_names[i], *why = preset_problem(p);
    /* cut long text so the list never widens the dock (export.presets has it whole) */
    cw_item_list_add_item(list, cw_frame_printf("%.24s  [%.16s]  -> %.40s%s", p, preset_get(p, "platform"), preset_output(p), why ? "   (invalid)" : ""),
                          (cw_texture){ 0 });
  }
  if (sel >= 0 && sel < npresets) cw_item_list_select(list, sel, true);
  int total = 0;
  for (int i = 0; i < J.n; i++) total += J.progress[i];
  cw_range_set_value(progress, J.n ? (double)total / J.n : 0);
  if (strcmp(J.state, "idle"))
    cw_label_set_text(status, J.proc ? cw_frame_printf("%s... %s", J.validate_only ? "Validating" : "Exporting", J.n ? J.stage[0] : "")
                                     : !strcmp(J.state, "ok") ? (J.validate_only ? "All presets valid" : "Export finished: see the Output panel")
                                                              : J.error);
}

static cw_node button(const char *name, const char *text, void *what) {
  cw_node b = cw_node_new("Button", name);
  cw_button_set_text(b, text);
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_run, what, 0);
  return b;
}

static cw_node create(void *user) {
  (void)user;
  cw_node box = cw_node_new("VBoxContainer", "ExpBox");
  cw_node row = cw_node_new("HFlowContainer", NULL);
  cw_node_add_child(row, button("ExpRunAll", "Export All", (void *)"all"));
  cw_node_add_child(row, button("ExpRunSelected", "Export Selected", (void *)"selected"));
  cw_node_add_child(row, button("ExpValidate", "Validate", (void *)"validate"));
  cw_node_add_child(row, button("ExpCancel", "Cancel", (void *)"cancel"));
  platform = cw_node_new("OptionButton", "ExpPlatform");
  for (int i = 0; PLATFORMS[i]; i++) cw_option_button_add_item(platform, PLATFORMS[i]);
  cw_node_add_child(row, platform);
  name_edit = cw_node_new("LineEdit", "ExpName");
  cw_node_set(name_edit, "placeholder_text", cw_val_string("preset name"));
  cw_control_set_custom_min_size(name_edit, (cw_vec2){ 110, 0 });
  cw_node_add_child(row, name_edit);
  cw_node add = cw_node_new("Button", "ExpAdd");
  cw_button_set_text(add, "Add Preset");
  cw_connect(add, "pressed", (cw_node){ 0 }, NULL, on_add, NULL, 0);
  cw_node_add_child(row, add);
  cw_node rm = cw_node_new("Button", "ExpRemove");
  cw_button_set_text(rm, "Remove");
  cw_connect(rm, "pressed", (cw_node){ 0 }, NULL, on_remove, NULL, 0);
  cw_node_add_child(row, rm);
  cw_node_add_child(box, row);
  progress = cw_node_new("ProgressBar", "ExpProgress");
  cw_range_set_limits(progress, 0, 100, 1);
  cw_node_add_child(box, progress);
  status = cw_node_new("Label", "ExpStatus");
  cw_node_set(status, "ellipsis", cw_val_bool(true));
  cw_node_add_child(box, status);
  list = cw_node_new("ItemList", "ExpPresets");
  cw_control_set_size_flags(list, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_add_child(box, list);
  panel_refresh();
  return box;
}

static void on_project(const cwed_event *e, void *user) {
  (void)e, (void)user;
  panel_dirty = true;
}

static void on_command(const cwed_event *e, void *user) {
  (void)user;
  if (e->line && !strncmp(e->line, "project.set", 11)) panel_dirty = true;
}

static void init(void) {
  static const cwed_param add_p[] = {
    { "name", CWED_ARG_STRING, NULL, "preset name (a-z 0-9 _ -): linux, windows, cart, web..." },
    { "platform", CWED_ARG_STRING, NULL, "linux-x64, linux-arm64, windows-x64, android-arm64, web, wasc, macos or ios" },
    { "output", CWED_ARG_STRING, "", "output directory inside the project; default exports/<name>" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "export.add", .title = "Add Export Preset...", .params = add_p,
    .flags = CWED_CMD_UNDO, .example = "export.add name=web platform=web",
    .doc = "Add an export preset (export.<name>.platform / .output in project.cwproj; more keys with project.set). Undoable.",
    .exec = cmd_add, .undo = undo_snap, .free_state = snap_free });
  static const cwed_param rm_p[] = { { "name", CWED_ARG_STRING, NULL, "preset name (export.presets)" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "export.remove", .title = "Remove Export Preset...", .params = rm_p,
    .flags = CWED_CMD_UNDO, .example = "export.remove name=demo_cart",
    .doc = "Remove an export preset (every export.<name>.* key). Undoable.", .exec = cmd_remove, .undo = undo_snap,
    .free_state = snap_free });
  cwed_command_register(&(cwed_command_desc){ .name = "export.presets", .title = "List Export Presets", .flags = CWED_CMD_QUERY,
    .doc = "The project's export presets: name, platform, output, valid (+ problem), and the exporter that runs them.",
    .exec = cmd_presets });
  static const cwed_param run_p[] = { { "presets", CWED_ARG_STRING, "all", "all, or preset names joined by ','" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "export.run", .title = "Export", .params = run_p,
    .doc = "Export presets in one action (natively): checks them, saves open scenes, runs the engine's exporter "
           "(tools/cwexport.mjs) in the background; progress and log go to the Output panel; export.status has the results.",
    .exec = cmd_run });
  cwed_command_register(&(cwed_command_desc){ .name = "export.validate", .title = "Validate Export Presets", .params = run_p,
    .doc = "Check presets: locally (platform, output), then by the exporter (--validate: toolchains, platform keys).",
    .exec = cmd_validate });
  cwed_command_register(&(cwed_command_desc){ .name = "export.status", .title = "Export Status", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The running or last export: state (idle running ok failed canceled), per-preset progress, results, error.",
    .exec = cmd_status });
  cwed_command_register(&(cwed_command_desc){ .name = "export.cancel", .title = "Cancel Export",
    .doc = "Stop the running export.", .exec = cmd_cancel });
  cwed_menu_add("Project/Export All", "export.run presets=all", 210);
  cwed_menu_add("Project/Validate Export Presets", "export.validate", 211);
  cwed_panel_register(&(cwed_panel_desc){ .id = "export", .title = "Export", .slot = "center", .order = 70, .create = create });
  cwed_subscribe(CWED_EV_PROJECT, on_project, NULL);
  cwed_subscribe(CWED_EV_COMMAND, on_command, NULL);
}

static void frame(double dt) {
  (void)dt;
  job_frame();
  if (panel_dirty) panel_refresh();
}

static void shutdown(void) {
  if (J.proc) cw_process_free(J.proc);
  J.proc = NULL;
}

CWED_MODULE(export) = { .title = "Export", .milestone = "M4.6", .init = init, .frame = frame, .shutdown = shutdown };

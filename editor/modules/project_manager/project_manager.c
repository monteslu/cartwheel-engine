/* Project manager: create projects from templates, open, recent list,
 * rename, remove, build (native), and carry a project as one archive.
 *
 * A project is a directory (natively) or the in-memory project layer (in a
 * cart) holding project.cwproj, the scenes, src/ (C) and
 * a CMakeLists.txt that builds it native and cart against the engine
 * (tools/cwproject.mjs: the engine tree configured with CW_PROJECT_DIR).
 * Templates live in editor/templates/<id>/ (template.cfg: title, description,
 * order); a cart editor carries them in its bundle under cwtemplates/.
 *
 * Commands: project.templates, project.new, project.recent, project.rename,
 * project.remove, project.archive, project.import, project.build,
 * project.build_status, ui.rect (a test helper: where a named widget
 * is on screen). Panel: Projects. */
/* paths and messages are cut at their buffer on purpose (each limit is far above real use) */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !CW_CART
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cartwheel/platform.h"
#include "cartwheel/process.h"
#include "cartwheel/project.h"
#include "cartwheel/vfs.h"
#include "pm.h"

/* ── small helpers ──────────────────────────────────────────────────── */

const char *pm_engine_dir(void) {
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

const char *pm_get(const char *key) {
  const char *v = cw_project_get(key);
  return v ? v : "";
}

static bool any_dirty(cwed_call *c) {
  for (int i = 0; i < cwed_doc_count(); i++)
    if (cwed_doc_dirty(cwed_doc_at(i)))
      return !cwed_fail(c, "%s has unsaved changes: save it (scene.save) or close it (scene.close force=true) first",
                        cwed_doc_title(cwed_doc_at(i)));
  return false;
}

/* "My Game 2" -> "my_game_2" (a C identifier: the executable and cart name) */
static void app_from_name(const char *name, char *out, int cap) {
  int n = 0;
  bool sep = false;
  for (const char *s = name; *s && n < cap - 1; s++) {
    char ch = *s;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      if (sep && n) out[n++] = '_';
      if (n < cap - 1) out[n++] = ch;
      sep = false;
    } else sep = true;
  }
  out[n] = 0;
  if (!n) snprintf(out, (size_t)cap, "game");
  else if (out[0] >= '0' && out[0] <= '9') {
    char t[128];
    snprintf(t, sizeof t, "game_%s", out);
    snprintf(out, (size_t)cap, "%s", t);
  }
}

static bool valid_name(const char *name) {
  if (!name || !*name || strlen(name) > 100) return false;
  for (const char *s = name; *s; s++)
    if (*s == '\n' || *s == '\r' || *s == '|' || *s == '"') return false;
  return true;
}

/* ── templates ──────────────────────────────────────────────────────── */

typedef struct { char id[64], title[96], desc[512]; int order; } tpl;
static tpl tpls[32];
static int ntpl;

static void cfg_parse(char *text, tpl *t) {
  for (char *sv_, *line = strtok_r(text, "\n", &sv_); line; line = strtok_r(NULL, "\n", &sv_)) {
    char *eq = strchr(line, '=');
    if (!eq || line[0] == '#') continue;
    *eq = 0;
    char *k = line, *v = eq + 1;
    while (*k == ' ') k++;
    for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
    while (*v == ' ') v++;
    size_t vl = strlen(v);
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) v[--vl] = 0;
    if (!strcmp(k, "title")) snprintf(t->title, sizeof t->title, "%s", v);
    else if (!strcmp(k, "description")) snprintf(t->desc, sizeof t->desc, "%s", v);
    else if (!strcmp(k, "order")) t->order = atoi(v);
  }
}

#if !CW_CART
static const char *templates_dir(void) {
  static char dir[PATH_MAX];
  const char *e = getenv("CW_TEMPLATES");
  if (e && *e) snprintf(dir, sizeof dir, "%s", e);
  else snprintf(dir, sizeof dir, "%s/editor/templates", pm_engine_dir());
  return dir;
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)(n > 0 ? n : 0) + 1);
  size_t got = n > 0 ? fread(b, 1, (size_t)n, f) : 0;
  fclose(f);
  b[got] = 0;
  if (len) *len = got;
  return b;
}

static bool mkdirs(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *s = tmp + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      if (mkdir(tmp, 0755) && errno != EEXIST) return false;
      *s = '/';
    }
  return !mkdir(tmp, 0755) || errno == EEXIST;
}

static bool write_file(const char *path, const void *data, size_t len) {
  char dir[PATH_MAX];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    if (!mkdirs(dir)) return false;
  }
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok;
}

static bool dir_exists(const char *p) {
  struct stat st;
  return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static bool dir_empty(const char *p) {
  DIR *d = opendir(p);
  if (!d) return true;
  struct dirent *e;
  bool empty = true;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) empty = false;
  closedir(d);
  return empty;
}
#endif

static int cmp_tpl(const void *a, const void *b) {
  const tpl *x = a, *y = b;
  return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

static void templates_scan(void) {
  ntpl = 0;
#if !CW_CART
  DIR *d = opendir(templates_dir());
  struct dirent *e;
  while (d && (e = readdir(d)) && ntpl < (int)CW_ARRAY_LEN(tpls)) {
    if (e->d_name[0] == '.') continue;
    char cfg[PATH_MAX];
    snprintf(cfg, sizeof cfg, "%s/%s/template.cfg", templates_dir(), e->d_name);
    char *t = read_file(cfg, NULL);
    if (!t) continue;
    tpl *x = &tpls[ntpl++];
    memset(x, 0, sizeof *x);
    snprintf(x->id, sizeof x->id, "%s", e->d_name);
    cfg_parse(t, x);
    free(t);
  }
  if (d) closedir(d);
#else
  size_t len;
  char *idx = cw_asset_load("cwtemplates.txt", &len);
  if (!idx) return;
  for (char *sv_, *line = strtok_r(idx, "\n", &sv_); line; line = strtok_r(NULL, "\n", &sv_)) {
    char *slash = strchr(line, '/');
    if (!slash || strcmp(slash + 1, "template.cfg")) continue;
    *slash = 0;
    char path[256];
    snprintf(path, sizeof path, "cwtemplates/%s/template.cfg", line);
    size_t cl;
    char *t = cw_asset_load(path, &cl);
    if (!t || ntpl == (int)CW_ARRAY_LEN(tpls)) {
      free(t);
      continue;
    }
    char *z = malloc(cl + 1);
    memcpy(z, t, cl);
    z[cl] = 0;
    free(t);
    tpl *x = &tpls[ntpl++];
    memset(x, 0, sizeof *x);
    snprintf(x->id, sizeof x->id, "%s", line);
    cfg_parse(z, x);
    free(z);
  }
  free(idx);
#endif
  qsort(tpls, (size_t)ntpl, sizeof *tpls, cmp_tpl);
}

static const tpl *tpl_find(const char *id) {
  for (int i = 0; i < ntpl; i++)
    if (!strcmp(tpls[i].id, id)) return &tpls[i];
  return NULL;
}

typedef bool (*file_fn)(const char *rel, const void *data, size_t len, void *user);

#if !CW_CART
static bool walk_template(const char *root, const char *rel, file_fn fn, void *user, int *count) {
  char full[PATH_MAX];
  snprintf(full, sizeof full, "%s%s%s", root, *rel ? "/" : "", rel);
  DIR *d = opendir(full);
  if (!d) return false;
  struct dirent *e;
  bool ok = true;
  while (ok && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char r2[PATH_MAX], f2[PATH_MAX];
    snprintf(r2, sizeof r2, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
    snprintf(f2, sizeof f2, "%s/%s", root, r2);
    if (dir_exists(f2)) {
      ok = walk_template(root, r2, fn, user, count);
      continue;
    }
    if (!strcmp(r2, "template.cfg")) continue;
    size_t len;
    char *data = read_file(f2, &len);
    if (!data) continue;
    ok = fn(r2, data, len, user);
    (*count)++;
    free(data);
  }
  closedir(d);
  return ok;
}
#endif

/* every file of template id (template.cfg excluded), sorted by path in a cart */
static int template_files(const char *id, file_fn fn, void *user) {
  int count = 0;
#if !CW_CART
  char root[PATH_MAX];
  snprintf(root, sizeof root, "%s/%s", templates_dir(), id);
  if (!walk_template(root, "", fn, user, &count)) return -1;
#else
  size_t len;
  char *idx = cw_asset_load("cwtemplates.txt", &len);
  if (!idx) return -1;
  size_t pl = strlen(id);
  for (char *sv_, *line = strtok_r(idx, "\n", &sv_); line; line = strtok_r(NULL, "\n", &sv_)) {
    if (strncmp(line, id, pl) || line[pl] != '/' || !strcmp(line + pl + 1, "template.cfg")) continue;
    char path[512];
    snprintf(path, sizeof path, "cwtemplates/%s", line);
    size_t fl;
    void *data = cw_asset_load(path, &fl);
    if (!data) continue;
    bool ok = fn(line + pl + 1, data, fl, user);
    free(data);
    count++;
    if (!ok) {
      count = -1;
      break;
    }
  }
  free(idx);
#endif
  return count;
}

/* ── recent projects (setting projects.recent: entries joined by '|') ── */

#define MAX_RECENT 16
static int recent_get(char out[MAX_RECENT][1024]) {
  const char *s = cwed_setting("projects.recent");
  int n = 0;
  while (s && *s && n < MAX_RECENT) {
    const char *e = strchr(s, '|');
    size_t l = e ? (size_t)(e - s) : strlen(s);
    if (l && l < 1024) {
      memcpy(out[n], s, l);
      out[n][l] = 0;
      n++;
    }
    s = e ? e + 1 : s + l;
  }
  return n;
}

static void recent_set(char list[MAX_RECENT][1024], int n) {
  size_t cap = 16;
  for (int i = 0; i < n; i++) cap += strlen(list[i]) + 1;
  char *b = cw_frame_alloc(cap), *w = b;
  *w = 0;
  for (int i = 0; i < n; i++) w += sprintf(w, "%s%s", i ? "|" : "", list[i]);
  cwed_setting_put("projects.recent", b);
}

static void recent_add(const char *entry) {
  static char list[MAX_RECENT][1024], out[MAX_RECENT][1024];
  int n = recent_get(list), k = 0;
  snprintf(out[k++], 1024, "%s", entry);
  for (int i = 0; i < n && k < MAX_RECENT; i++)
    if (strcmp(list[i], entry)) snprintf(out[k++], 1024, "%s", list[i]);
  recent_set(out, k);
}

static bool recent_remove(const char *entry) {
  static char list[MAX_RECENT][1024], out[MAX_RECENT][1024];
  int n = recent_get(list), k = 0;
  for (int i = 0; i < n; i++)
    if (strcmp(list[i], entry)) snprintf(out[k++], 1024, "%s", list[i]);
  recent_set(out, k);
  return k != n;
}

/* the name a recent entry's project.cwproj gives ("" when it is gone) */
static const char *recent_name(const char *entry) {
  if (!strncmp(entry, "memory:", 7)) return entry + 7;
#if !CW_CART
  char p[PATH_MAX];
  snprintf(p, sizeof p, "%s/project.cwproj", entry);
  char *t = read_file(p, NULL);
  if (!t) return "";
  const char *name = "";
  for (char *sv_, *line = strtok_r(t, "\n", &sv_); line; line = strtok_r(NULL, "\n", &sv_)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    char *k = line;
    while (*k == ' ') k++;
    if (strncmp(k, "name", 4) || (k[4] != ' ' && k[4] != '=')) continue;
    char *v = eq + 1;
    while (*v == ' ' || *v == '"') v++;
    size_t vl = strlen(v);
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '"' || v[vl - 1] == '\r')) v[--vl] = 0;
    name = cw_frame_printf("%s", v);
  }
  free(t);
  return *name ? name : "(unnamed)";
#else
  return "";
#endif
}

static void current_entry(char *out, size_t cap) {
  const char *dir = cwed_project_dir();
  if (dir[0]) {
#if !CW_CART
    char real[PATH_MAX];
    if (realpath(dir, real)) dir = real;
#endif
    snprintf(out, cap, "%s", dir);
  } else snprintf(out, cap, "memory:%s", cwed_project_name());
}

static bool panel_dirty = true;

static void on_project(const cwed_event *e, void *user) {
  (void)e, (void)user;
  char entry[1024];
  current_entry(entry, sizeof entry);
  /* the editor's own demo project (no directory, the bundled name) is not a recent project */
  if (cwed_project_dir()[0] || strcmp(cwed_project_name(), "Demo Project")) recent_add(entry);
  cw_project_reload();
  panel_dirty = true;
}

/* ── project.templates / project.recent ────────────────────────────── */

static bool cmd_templates(cwed_call *c) {
  templates_scan();
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "templates");
  cwed_json_raw(j, "[");
  for (int i = 0; i < ntpl; i++) {
    cwed_json_raw(j, i ? ",{" : "{");
    cwed_json_key(j, "id");
    cwed_json_str(j, tpls[i].id);
    cwed_json_key(j, "title");
    cwed_json_str(j, tpls[i].title);
    cwed_json_key(j, "description");
    cwed_json_str(j, tpls[i].desc);
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_recent(cwed_call *c) {
  static char list[MAX_RECENT][1024];
  int n = recent_get(list);
  char cur[1024];
  current_entry(cur, sizeof cur);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "projects");
  cwed_json_raw(j, "[");
  for (int i = 0; i < n; i++) {
    cwed_json_raw(j, i ? ",{" : "{");
    cwed_json_key(j, "entry");
    cwed_json_str(j, list[i]);
    cwed_json_key(j, "name");
    cwed_json_str(j, recent_name(list[i]));
    cwed_json_key(j, "current");
    cwed_json_raw(j, strcmp(list[i], cur) ? "false" : "true");
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  return true;
}

/* ── project.new ────────────────────────────────────────────────────── */

typedef struct {
  const char *name, *app, *dir; /* dir: native target directory */
  bool failed;
  char err[512];
} new_ctx;

/* project.cwproj gets the project's name and app */
static void *patch_cwproj(const char *rel, const void *data, size_t *len, const new_ctx *n) {
  if (strcmp(rel, "project.cwproj")) return NULL;
  char *t = malloc(*len + 1);
  memcpy(t, data, *len);
  t[*len] = 0;
  char *a = cw_project_text_set(t, "name", n->name);
  char *b = a ? cw_project_text_set(a, "app", n->app) : NULL;
  free(t);
  free(a);
  if (b) *len = strlen(b);
  return b;
}

static bool put_file(const char *rel, const void *data, size_t len, void *user) {
  new_ctx *n = user;
  void *patched = patch_cwproj(rel, data, &len, n);
  if (patched) data = patched;
  bool ok;
#if !CW_CART
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/%s", n->dir, rel);
  ok = write_file(path, data, len);
  if (!ok) snprintf(n->err, sizeof n->err, "cannot write %s: %s", path, strerror(errno));
#else
  ok = cw_vfs_write(rel, data, len);
  if (!ok) snprintf(n->err, sizeof n->err, "cannot write %s into the project layer", rel);
#endif
  free(patched);
  return ok;
}

/* a cart: replace the project layer, keeping the editor's own settings file */
#if CW_CART
static void cart_reset_layer(void) {
  size_t sl = 0;
  void *settings = cw_vfs_read(".cweditor/settings.cfg", &sl);
  while (cwed_doc_count()) cwed_runf("scene.close force=true");
  cw_vfs_clear();
  if (settings) cw_vfs_write(".cweditor/settings.cfg", settings, sl);
  free(settings);
}

static void cart_open_new(void) {
  cw_scene_cache_forget(NULL);
  cw_project_reload();
  cwed_project_reload();
  const char *main_scene = cwed_project_main_scene();
  if (main_scene[0] && cw_vfs_exists(main_scene)) cwed_runf("scene.open path=%s", cwed_quote(main_scene));
}
#endif

static const char *default_projects_dir(void) {
  const char *s = cwed_setting("projects.dir");
  if (s && *s) return s;
  const char *home = getenv("HOME");
  return cw_frame_printf("%s/cartwheel-projects", home && *home ? home : ".");
}

static bool cmd_new(cwed_call *c) {
  const char *name = cwed_arg(c, "name"), *tid = cwed_arg(c, "template");
  if (!valid_name(name)) return cwed_fail(c, "name: give the project a name (up to 100 characters, no quotes, '|' or newlines)");
  templates_scan();
  const tpl *t = tpl_find(tid);
  if (!t) {
    char ids[512] = "";
    for (int i = 0; i < ntpl; i++) snprintf(ids + strlen(ids), sizeof ids - strlen(ids), "%s%s", i ? ", " : "", tpls[i].id);
    return cwed_fail(c, "no template '%s' (project.templates lists them: %s)", tid, ntpl ? ids : "none found");
  }
  if (any_dirty(c)) return false;
  char app[64];
  app_from_name(name, app, sizeof app);
  new_ctx n = { .name = name, .app = app };
#if !CW_CART
  char dir[PATH_MAX];
  if (cwed_arg_given(c, "dir") && cwed_arg(c, "dir")[0]) snprintf(dir, sizeof dir, "%s", cwed_arg(c, "dir"));
  else snprintf(dir, sizeof dir, "%s/%s", default_projects_dir(), app);
  if (dir_exists(dir) && !dir_empty(dir))
    return cwed_fail(c, "%s already exists and is not empty: pick another dir= (or name=), or open it with project.open", dir);
  if (!mkdirs(dir)) return cwed_fail(c, "cannot create %s: %s", dir, strerror(errno));
  char real[PATH_MAX];
  if (realpath(dir, real)) snprintf(dir, sizeof dir, "%s", real);
  n.dir = dir;
  int files = template_files(t->id, put_file, &n);
  if (files <= 0) return cwed_fail(c, "copying template %s into %s failed: %s", t->id, dir, n.err[0] ? n.err : "the template has no files");
  char *reply = NULL;
  bool ok = cwed_run(cw_frame_printf("project.open dir=%s", cwed_quote(dir)), &reply);
  if (!ok) {
    cwed_fail(c, "created %s but opening it failed: %s", dir, reply ? reply : "");
    cw_free(reply);
    return false;
  }
  cw_free(reply);
#else
  cart_reset_layer();
  int files = template_files(t->id, put_file, &n);
  if (files <= 0) return cwed_fail(c, "copying template %s failed: %s", t->id, n.err[0] ? n.err : "the cart bundle has no cwtemplates");
  cart_open_new();
  cwed_emit(&(cwed_event){ .type = CWED_EV_PROJECT });
#endif
  cwed_log(CWED_LOG_INFO, "project: created \"%s\" from template %s (%d files)%s%s", name, t->id, files, CW_CART ? "" : " in ",
           CW_CART ? "" : cwed_project_dir());
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "name");
  cwed_json_str(j, name);
  cwed_json_key(j, "app");
  cwed_json_str(j, app);
  cwed_json_key(j, "template");
  cwed_json_str(j, t->id);
  cwed_json_key(j, "dir");
  cwed_json_str(j, cwed_project_dir());
  cwed_json_key(j, "files");
  cwed_json_printf(j, "%d", files);
  cwed_json_key(j, "storage");
  cwed_json_str(j, CW_CART ? "memory (cart: project.archive carries it out)" : "disk");
  panel_dirty = true;
  return true;
}

/* ── project.rename / project.remove ───────────────────────────────── */

static bool cmd_rename(cwed_call *c) {
  const char *name = cwed_arg(c, "name");
  if (!valid_name(name)) return cwed_fail(c, "name: up to 100 characters, no quotes, '|' or newlines");
  char cur[1024];
  current_entry(cur, sizeof cur);
  const char *entry = cwed_arg_given(c, "project") && cwed_arg(c, "project")[0] ? cwed_arg(c, "project") : cur;
#if !CW_CART
  char real[PATH_MAX];
  if (strncmp(entry, "memory:", 7) && realpath(entry, real)) entry = cw_frame_printf("%s", real);
#endif
  bool is_current = !strcmp(entry, cur);
  if (is_current) {
    if (!cw_project_set("name", name)) return cwed_fail(c, "cannot write project.cwproj");
    if (!cwed_project_dir()[0]) recent_remove(cur);
    cwed_project_reload();
  } else {
#if !CW_CART
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/project.cwproj", entry);
    char *t = read_file(p, NULL);
    if (!t) return cwed_fail(c, "%s is not a project (no project.cwproj): project.recent lists the known ones", entry);
    char *u = cw_project_text_set(t, "name", name);
    free(t);
    bool ok = u && write_file(p, u, strlen(u));
    free(u);
    if (!ok) return cwed_fail(c, "cannot write %s", p);
#else
    return cwed_fail(c, "a cart holds one project at a time (the current one): rename it without project=");
#endif
  }
  cwed_log(CWED_LOG_INFO, "project: %s renamed to \"%s\"", entry, name);
  cwed_json_key(cwed_reply(c), "name");
  cwed_json_str(cwed_reply(c), name);
  cwed_json_key(cwed_reply(c), "project");
  cwed_json_str(cwed_reply(c), is_current ? cwed_project_dir() : entry);
  panel_dirty = true;
  return true;
}

#if !CW_CART
static int rm_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st)) return -1;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        rc |= rm_tree(sub);
      }
    closedir(d);
    return rc | rmdir(path);
  }
  return unlink(path);
}
#endif

static bool cmd_remove(cwed_call *c) {
  const char *entry = cwed_arg(c, "project");
  bool del = cwed_arg_bool(c, "delete_files");
#if !CW_CART
  char real[PATH_MAX];
  if (strncmp(entry, "memory:", 7) && realpath(entry, real)) entry = cw_frame_printf("%s", real);
#endif
  char cur[1024];
  current_entry(cur, sizeof cur);
  if (del) {
#if CW_CART
    return cwed_fail(c, "a cart has no project files to delete: project.remove without delete_files");
#else
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/project.cwproj", entry);
    struct stat st;
    if (stat(p, &st)) return cwed_fail(c, "%s has no project.cwproj: refusing to delete a directory that is not a project", entry);
    if (!strcmp(entry, cur)) return cwed_fail(c, "%s is the open project: open another one first", entry);
    const char *home = getenv("HOME");
    if (!strcmp(entry, "/") || (home && !strcmp(entry, home))) return cwed_fail(c, "refusing to delete %s", entry);
    if (rm_tree(entry)) return cwed_fail(c, "could not delete all of %s: %s", entry, strerror(errno));
#endif
  }
  bool was = recent_remove(entry);
  if (!was && !del) return cwed_fail(c, "%s is not in the recent list (project.recent)", entry);
  cwed_log(CWED_LOG_INFO, "project: %s %s", entry, del ? "deleted" : "removed from the recent list");
  cwed_json_key(cwed_reply(c), "removed");
  cwed_json_str(cwed_reply(c), entry);
  cwed_json_key(cwed_reply(c), "deleted_files");
  cwed_json_raw(cwed_reply(c), del ? "true" : "false");
  panel_dirty = true;
  return true;
}

/* ── archives (the CWVFS1 format of cartwheel/vfs.h) ───────────────── */

typedef CW_ARRAY(char) bytes;

#if !CW_CART
static void bytes_put(bytes *b, const void *d, size_t n) {
  for (size_t i = 0; i < n; i++) cw_array_push(b, ((const char *)d)[i]);
}

static int cmp_cstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* every project file (not .cwbuild/.cwcache/.git or other dot entries, not exports/) */
static void collect(const char *root, const char *rel, char ***list, int *n, int *cap) {
  char full[PATH_MAX];
  snprintf(full, sizeof full, "%s%s%s", root, *rel ? "/" : "", rel);
  DIR *d = opendir(full);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    if (!*rel && !strcmp(e->d_name, "exports")) continue;
    char r2[PATH_MAX], f2[PATH_MAX];
    snprintf(r2, sizeof r2, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
    snprintf(f2, sizeof f2, "%s/%s", root, r2);
    if (dir_exists(f2)) collect(root, r2, list, n, cap);
    else {
      if (*n == *cap) *list = realloc(*list, sizeof(char *) * (size_t)(*cap = *cap ? *cap * 2 : 64));
      (*list)[(*n)++] = strdup(r2);
    }
  }
  closedir(d);
}
#endif

static bool cmd_archive(cwed_call *c) {
  size_t len = 0;
  void *data = NULL;
  int files = 0;
#if !CW_CART
  const char *dir = cwed_project_dir();
  if (!dir[0]) {
    data = cw_vfs_export(&len);
    files = cw_vfs_list(&(const char **){ 0 });
  } else {
    char **list = NULL;
    int n = 0, cap = 0;
    collect(dir, "", &list, &n, &cap);
    qsort(list, (size_t)n, sizeof *list, cmp_cstr);
    bytes b = { 0 };
    bytes_put(&b, "CWVFS1\n", 7);
    for (int i = 0; i < n; i++) {
      char p[PATH_MAX];
      snprintf(p, sizeof p, "%s/%s", dir, list[i]);
      size_t fl;
      char *fd = read_file(p, &fl);
      if (fd) {
        char head[PATH_MAX + 32];
        int hl = snprintf(head, sizeof head, "%s\n%zu\n", list[i], fl);
        bytes_put(&b, head, (size_t)hl);
        bytes_put(&b, fd, fl);
        files++;
      }
      free(fd);
      free(list[i]);
    }
    free(list);
    len = (size_t)b.count;
    data = cw_alloc(len ? len : 1, CW_MEM_GENERAL);
    memcpy(data, b.data, len);
    cw_array_free(&b);
  }
  const char *out = cwed_arg(c, "path");
  if (out[0]) {
    bool ok = write_file(out, data, len);
    cw_free(data);
    if (!ok) return cwed_fail(c, "cannot write %s: %s", out, strerror(errno));
    cwed_json_key(cwed_reply(c), "path");
    cwed_json_str(cwed_reply(c), out);
    cwed_json_key(cwed_reply(c), "bytes");
    cwed_json_printf(cwed_reply(c), "%zu", len);
    cwed_json_key(cwed_reply(c), "files");
    cwed_json_printf(cwed_reply(c), "%d", files);
    return true;
  }
#else
  if (cwed_arg(c, "path")[0]) return cwed_fail(c, "a cart cannot write files: project.archive without path= returns the archive as base64");
  data = cw_vfs_export(&len);
  files = cw_vfs_list(&(const char **){ 0 });
#endif
  char *b64 = cw_base64_encode(data, len);
  cw_free(data);
  cwed_json_key(cwed_reply(c), "bytes");
  cwed_json_printf(cwed_reply(c), "%zu", len);
  cwed_json_key(cwed_reply(c), "files");
  cwed_json_printf(cwed_reply(c), "%d", files);
  cwed_json_key(cwed_reply(c), "archive");
  cwed_json_str(cwed_reply(c), b64);
  cw_free(b64);
  return true;
}

/* the archive bytes of project.import: archive=<base64>, or file=<path>
 * (natively a file; in a cart a project-layer file, e.g. written with
 * `vfs write`/`vfs append`) holding the archive or its base64 */
static void *import_bytes(cwed_call *c, size_t *len) {
  const char *b64 = cwed_arg(c, "archive"), *file = cwed_arg(c, "file");
  void *raw = NULL;
  size_t rl = 0;
  if (b64[0]) return cw_base64_decode(b64, len);
  if (!file[0]) return NULL;
#if !CW_CART
  raw = read_file(file, &rl);
#else
  raw = cw_vfs_read(file, &rl);
#endif
  if (!raw) return NULL;
  void *out;
  if (rl >= 7 && !memcmp(raw, "CWVFS1\n", 7)) {
    out = cw_alloc(rl ? rl : 1, CW_MEM_GENERAL);
    memcpy(out, raw, rl);
    *len = rl;
  } else out = cw_base64_decode(raw, len);
  free(raw);
  return out;
}

static bool cmd_import(cwed_call *c) {
  size_t len = 0;
  void *data = import_bytes(c, &len);
  if (!data) return cwed_fail(c, "give archive=<base64> or file=<path of an archive> (project.archive makes one)");
  if (len < 7 || memcmp(data, "CWVFS1\n", 7)) {
    cw_free(data);
    return cwed_fail(c, "not a project archive (it must start with CWVFS1: project.archive or `vfs export` make one)");
  }
  if (any_dirty(c)) {
    cw_free(data);
    return false;
  }
  int files = 0;
#if !CW_CART
  const char *dir = cwed_arg(c, "dir");
  if (!dir[0]) {
    cw_free(data);
    return cwed_fail(c, "dir: the directory to unpack the project into (natively a project lives on disk)");
  }
  if (dir_exists(dir) && !dir_empty(dir)) {
    cw_free(data);
    return cwed_fail(c, "%s already exists and is not empty: pick another dir=", dir);
  }
  const char *p = (const char *)data + 7, *end = (const char *)data + len;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) break;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%.*s", (int)(nl - p), p);
    char *e2;
    unsigned long long fl = strtoull(nl + 1, &e2, 10);
    if (*e2 != '\n' || e2 + 1 + fl > end || !path[0] || path[0] == '/' || strstr(path, "..")) {
      cw_free(data);
      return cwed_fail(c, "the archive is malformed at '%s'", path);
    }
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", dir, path);
    if (!write_file(full, e2 + 1, (size_t)fl)) {
      cw_free(data);
      return cwed_fail(c, "cannot write %s: %s", full, strerror(errno));
    }
    files++;
    p = e2 + 1 + fl;
  }
  cw_free(data);
  char pf[PATH_MAX];
  snprintf(pf, sizeof pf, "%s/project.cwproj", dir);
  struct stat st;
  if (stat(pf, &st)) return cwed_fail(c, "unpacked %d files into %s, but there is no project.cwproj: not a project archive", files, dir);
  char *reply = NULL;
  bool ok = cwed_run(cw_frame_printf("project.open dir=%s", cwed_quote(dir)), &reply);
  if (!ok) {
    cwed_fail(c, "unpacked into %s but opening it failed: %s", dir, reply ? reply : "");
    cw_free(reply);
    return false;
  }
  cw_free(reply);
#else
  cart_reset_layer();
  if (!cw_vfs_import(data, len)) {
    cw_free(data);
    return cwed_fail(c, "the archive is malformed");
  }
  cw_free(data);
  files = cw_vfs_list(&(const char **){ 0 });
  cart_open_new();
  cwed_emit(&(cwed_event){ .type = CWED_EV_PROJECT });
#endif
  cwed_log(CWED_LOG_INFO, "project: imported \"%s\" (%d files)", cwed_project_name(), files);
  cwed_json_key(cwed_reply(c), "name");
  cwed_json_str(cwed_reply(c), cwed_project_name());
  cwed_json_key(cwed_reply(c), "dir");
  cwed_json_str(cwed_reply(c), cwed_project_dir());
  cwed_json_key(cwed_reply(c), "files");
  cwed_json_printf(cwed_reply(c), "%d", files);
  panel_dirty = true;
  return true;
}

/* ── project.build (native: tools/cwproject.mjs in a child process) ──── */

static struct {
  cw_process *proc;
  char state[16]; /* idle running ok failed */
  char target[16];
  char results[4096];
  int lines, exit_code;
} B = { .state = "idle", .exit_code = -1 };

static bool cmd_build(cwed_call *c) {
  const char *target = cwed_arg(c, "target");
  if (strcmp(target, "native") && strcmp(target, "cart") && strcmp(target, "all"))
    return cwed_fail(c, "target: native, cart or all");
  if (CW_CART) return cwed_fail(c, "a cart cannot run the build (no processes): build natively with tools/cwproject.mjs build <dir>");
  const char *dir = cwed_project_dir();
  if (!dir[0]) return cwed_fail(c, "the project lives in memory: create one on disk (project.new) or open one (project.open)");
  if (B.proc) return cwed_fail(c, "a build is running (project.build_status): wait for it");
  for (int i = 0; i < cwed_doc_count(); i++)
    if (cwed_doc_dirty(cwed_doc_at(i)) && cwed_doc_path(cwed_doc_at(i))[0]) cwed_runf("scene.save doc=%d", i);
  const char *tool = cw_frame_printf("%s/tools/cwproject.mjs", pm_engine_dir());
  const char *argv[] = { "node", tool, "build", dir, target, NULL };
  char err[512];
  B.proc = cw_process_start(&(cw_process_desc){ .argv = argv }, err, sizeof err);
  if (!B.proc) return cwed_fail(c, "cannot run %s: %s", tool, err);
  snprintf(B.state, sizeof B.state, "running");
  snprintf(B.target, sizeof B.target, "%s", target);
  B.results[0] = 0;
  B.lines = 0;
  B.exit_code = -1;
  cwed_log(CWED_LOG_INFO, "build: %s (%s)", cwed_project_name(), target);
  cwed_status_set("build", 60, "building...");
  cwed_json_key(cwed_reply(c), "started");
  cwed_json_str(cwed_reply(c), target);
  panel_dirty = true;
  return true;
}

static bool cmd_build_status(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "state");
  cwed_json_str(j, B.state);
  cwed_json_key(j, "target");
  cwed_json_str(j, B.target);
  cwed_json_key(j, "exit");
  cwed_json_printf(j, "%d", B.exit_code);
  cwed_json_key(j, "lines");
  cwed_json_printf(j, "%d", B.lines);
  cwed_json_key(j, "results");
  cwed_json_raw(j, "[");
  cwed_json_raw(j, B.results);
  cwed_json_raw(j, "]");
  cwed_json_key(j, "game_exe");
  cwed_json_str(j, pm_get("game_exe"));
  cwed_json_key(j, "game_cart");
  cwed_json_str(j, pm_get("game_cart"));
  return true;
}

static void build_frame(void) {
  if (!B.proc) return;
  for (const char *l; (l = cw_process_read_line(B.proc));) {
    B.lines++;
    if (!strncmp(l, "@result ", 8)) {
      size_t have = strlen(B.results);
      if (have + strlen(l) + 2 < sizeof B.results) snprintf(B.results + have, sizeof B.results - have, "%s%s", have ? "," : "", l + 8);
      bool ok = strstr(l, "\"ok\":true") != NULL;
      cwed_log(ok ? CWED_LOG_INFO : CWED_LOG_ERROR, "build: %s", l + 8);
    } else if (strstr(l, "error") || strstr(l, "FAILED") || strstr(l, "Error")) cwed_log(CWED_LOG_ERROR, "build: %s", l);
    else if (l[0] == '=' || strstr(l, "warning")) cwed_log(strstr(l, "warning") ? CWED_LOG_WARN : CWED_LOG_INFO, "build: %s", l);
  }
  if (cw_process_running(B.proc)) return;
  while (cw_process_read_line(B.proc)) {}
  B.exit_code = cw_process_exit_code(B.proc);
  cw_process_free(B.proc);
  B.proc = NULL;
  snprintf(B.state, sizeof B.state, "%s", B.exit_code == 0 ? "ok" : "failed");
  cw_project_reload(); /* the tool wrote game_exe / game_cart */
  cwed_project_reload();
  cwed_log(B.exit_code == 0 ? CWED_LOG_INFO : CWED_LOG_ERROR, "build: %s (exit %d)", B.exit_code == 0 ? "done" : "FAILED", B.exit_code);
  cwed_status_set("build", 60, B.exit_code == 0 ? "build ok" : "build FAILED");
  panel_dirty = true;
}

/* ── ui.rect: where a named widget is (tests drive the UI by pointer) ── */

static cw_node find_named(cw_node n, const char *name, int depth) {
  if (!strcmp(cw_node_name(n), name)) return n;
  if (depth > 64) return (cw_node){ 0 };
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node f = find_named(cw_node_child(n, i), name, depth + 1);
    if (!CW_HANDLE_IS_NULL(f)) return f;
  }
  return (cw_node){ 0 };
}

static void rect_json(cwed_json *j, cw_rect r) {
  cwed_json_printf(j, "{\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g,\"cx\":%d,\"cy\":%d}", r.x, r.y, r.w, r.h, (int)(r.x + r.w / 2),
                   (int)(r.y + r.h / 2));
}

static bool cmd_ui_rect(cwed_call *c) {
  const char *name = cwed_arg(c, "name");
  cw_node n = find_named(cw_tree_root(cwed_ui_tree()), name, 0);
  if (CW_HANDLE_IS_NULL(n)) return cwed_fail(c, "no widget named '%s' in the editor UI", name);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "type");
  cwed_json_str(j, cw_node_get_type(n)->name);
  cwed_json_key(j, "rect");
  rect_json(j, cw_control_global_rect(n));
  cw_value v;
  bool visible = cw_node_get_prop(n, "visible", &v) ? v.b : true;
  for (cw_node p = cw_node_parent(n); visible && !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p))
    if (cw_node_find_prop(p, "visible", NULL) && cw_node_get_prop(p, "visible", &v) && !v.b) visible = false;
  cwed_json_key(j, "visible");
  cwed_json_raw(j, visible ? "true" : "false");
  if (cw_node_find_prop(n, "disabled", NULL) && cw_node_get_prop(n, "disabled", &v)) {
    cwed_json_key(j, "disabled");
    cwed_json_raw(j, v.b ? "true" : "false");
  }
  /* a dock page: its tab in the TabContainer */
  cw_node parent = cw_node_parent(n);
  if (!CW_HANDLE_IS_NULL(parent) && cw_node_is(parent, "TabContainer")) {
    for (int i = 0; i < cw_tab_container_page_count(parent); i++)
      if (cw_tab_container_page(parent, i).id == n.id) {
        cw_rect tr = cw_tab_container_tab_rect(parent, i), pr = cw_control_global_rect(parent);
        tr.x += pr.x, tr.y += pr.y;
        cwed_json_key(j, "tab");
        rect_json(j, tr);
        cwed_json_key(j, "current");
        cwed_json_raw(j, cw_tab_container_current(parent) == i ? "true" : "false");
      }
  }
  return true;
}

/* ── the Projects panel ────────────────────────────────────────────── */

static cw_node P_name, P_loc, P_tpls, P_desc, P_path, P_recent, P_status, P_build_status;
static char P_tpl[64] = "empty_2d";
static char P_recent_entries[MAX_RECENT][1024];
static int P_recent_n;

static cw_node button(const char *name, const char *text, cw_signal_fn fn, void *user) {
  cw_node b = cw_node_new("Button", name);
  cw_button_set_text(b, text);
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, fn, user, 0);
  return b;
}

static void show_result(bool ok, char *reply) {
  if (!ok && reply) {
    /* the error text: {"ok":false,"error":"..."} */
    const char *e = strstr(reply, "\"error\":\"");
    cw_label_set_text(P_status, e ? cw_frame_printf("%.*s", (int)strcspn(e + 9, "\""), e + 9) : reply);
  } else cw_label_set_text(P_status, "");
  cw_free(reply);
}

static void update_path(void) {
  if (!cw_node_valid(P_path)) return;
  char app[64];
  app_from_name(cw_line_edit_text(P_name), app, sizeof app);
  const tpl *t = tpl_find(P_tpl);
  cw_label_set_text(P_desc, t ? t->desc : "");
  cw_label_set_text(P_path, CW_CART ? "In memory (a cart): project.archive carries it out"
                                    : cw_frame_printf("Creates %s/%s", cw_line_edit_text(P_loc), app));
}

static void on_tpl(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)argc;
  if (a[0].b) snprintf(P_tpl, sizeof P_tpl, "%s", (const char *)user);
  update_path();
}

static void on_name_changed(cw_node n, const cw_value *a, int argc, void *user) {
  (void)n, (void)a, (void)argc, (void)user;
  update_path();
}

static void on_create(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  char app[64];
  app_from_name(cw_line_edit_text(P_name), app, sizeof app);
  const char *line = CW_CART ? cw_frame_printf("project.new name=%s template=%s", cwed_quote(cw_line_edit_text(P_name)), P_tpl)
                             : cw_frame_printf("project.new name=%s template=%s dir=%s", cwed_quote(cw_line_edit_text(P_name)), P_tpl,
                                               cwed_quote(cw_frame_printf("%s/%s", cw_line_edit_text(P_loc), app)));
  char *reply = NULL;
  bool ok = cwed_run(line, &reply);
  show_result(ok, reply);
}

static const char *selected_entry(void) {
  int i = cw_item_list_selected(P_recent);
  return i >= 0 && i < P_recent_n ? P_recent_entries[i] : NULL;
}

static void on_open(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  const char *e = selected_entry();
  if (!e) {
    cw_label_set_text(P_status, "Select a recent project");
    return;
  }
  char *reply = NULL;
  bool ok = cwed_run(cw_frame_printf("project.open dir=%s", cwed_quote(e)), &reply);
  show_result(ok, reply);
}

static void on_rename(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  const char *e = selected_entry();
  char *reply = NULL;
  bool ok = cwed_run(e ? cw_frame_printf("project.rename name=%s project=%s", cwed_quote(cw_line_edit_text(P_name)), cwed_quote(e))
                       : cw_frame_printf("project.rename name=%s", cwed_quote(cw_line_edit_text(P_name))),
                     &reply);
  show_result(ok, reply);
}

static void on_remove(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  const char *e = selected_entry();
  if (!e) {
    cw_label_set_text(P_status, "Select a recent project");
    return;
  }
  char *reply = NULL;
  bool ok = cwed_run(cw_frame_printf("project.remove project=%s", cwed_quote(e)), &reply);
  show_result(ok, reply);
}

static void on_build(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  char *reply = NULL;
  bool ok = cwed_run(cw_frame_printf("project.build target=%s", (const char *)user), &reply);
  show_result(ok, reply);
}

static void panel_refresh(void) {
  panel_dirty = false;
  if (!cw_node_valid(P_recent)) return;
  cw_item_list_clear(P_recent);
  P_recent_n = recent_get(P_recent_entries);
  char cur[1024];
  current_entry(cur, sizeof cur);
  for (int i = 0; i < P_recent_n; i++) {
    const char *nm = recent_name(P_recent_entries[i]), *e = P_recent_entries[i];
    /* long paths are cut from the left so the list never widens the dock (project.recent has them whole) */
    int el = (int)strlen(e);
    cw_item_list_add_item(P_recent, cw_frame_printf("%s%.40s  (%s%s)", strcmp(e, cur) ? "" : "* ", *nm ? nm : "missing", el > 48 ? "..." : "",
                                                    el > 48 ? e + el - 45 : e),
                          (cw_texture){ 0 });
  }
  cw_label_set_text(P_build_status, cw_frame_printf("Project: %s   Build: %s", cwed_project_name(), B.state));
  update_path();
}

static cw_node create(void *user) {
  (void)user;
  templates_scan();
  cw_node scroll = cw_node_new("ScrollContainer", "ProjectsScroll");
  cw_node_set(scroll, "horizontal_scroll_mode", cw_val_int(0));
  cw_node box = cw_node_new("VBoxContainer", "ProjectsBox");
  cw_control_set_size_flags(box, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(scroll, box);

  cw_node grid = cw_node_new("GridContainer", "PmGrid");
  cw_node_set(grid, "columns", cw_val_int(2));
  cw_node l = cw_node_new("Label", NULL);
  cw_label_set_text(l, "Name");
  cw_node_add_child(grid, l);
  P_name = cw_node_new("LineEdit", "PmName");
  cw_line_edit_set_text(P_name, "New Game");
  cw_control_set_size_flags(P_name, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(P_name, "text_changed", (cw_node){ 0 }, NULL, on_name_changed, NULL, 0);
  cw_node_add_child(grid, P_name);
  l = cw_node_new("Label", NULL);
  cw_label_set_text(l, "Location");
  cw_node_add_child(grid, l);
  P_loc = cw_node_new("LineEdit", "PmLocation");
  cw_line_edit_set_text(P_loc, CW_CART ? "(memory)" : default_projects_dir());
  cw_node_set(P_loc, "editable", cw_val_bool(!CW_CART));
  cw_control_set_size_flags(P_loc, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(P_loc, "text_changed", (cw_node){ 0 }, NULL, on_name_changed, NULL, 0);
  cw_node_add_child(grid, P_loc);
  cw_node_add_child(box, grid);

  P_tpls = cw_node_new("HFlowContainer", "PmTemplates");
  for (int i = 0; i < ntpl; i++) {
    cw_node b = cw_node_new("Button", cw_frame_printf("PmTemplate_%s", tpls[i].id));
    cw_button_set_text(b, tpls[i].title[0] ? tpls[i].title : tpls[i].id);
    cw_button_set_toggle_mode(b, true);
    cw_button_set_group(b, "pm_templates");
    cw_button_set_pressed(b, !strcmp(tpls[i].id, P_tpl));
    cw_connect(b, "toggled", (cw_node){ 0 }, NULL, on_tpl, (void *)cw_intern(tpls[i].id), 0);
    cw_node_add_child(P_tpls, b);
  }
  cw_node_add_child(box, P_tpls);
  P_desc = cw_node_new("Label", "PmTemplateDesc");
  cw_node_set(P_desc, "autowrap", cw_val_bool(true));
  /* room for three lines whichever template is picked: the buttons below stay put */
  cw_control_set_custom_min_size(P_desc, (cw_vec2){ 0, 3 * cw_ui_line_height(cw_control_theme_font(P_desc, "Label", "font"),
                                                                            cw_control_theme_font_size(P_desc, "Label", "font_size")) });
  cw_node_add_child(box, P_desc);
  P_path = cw_node_new("Label", "PmPath");
  cw_node_set(P_path, "ellipsis", cw_val_bool(true));
  cw_node_add_child(box, P_path);
  cw_node row = cw_node_new("HBoxContainer", NULL);
  cw_node_add_child(row, button("PmCreate", "Create Project", on_create, NULL));
  cw_node_add_child(box, row);

  l = cw_node_new("Label", NULL);
  cw_label_set_text(l, "Recent projects");
  cw_node_add_child(box, l);
  P_recent = cw_node_new("ItemList", "PmRecent");
  cw_control_set_custom_min_size(P_recent, (cw_vec2){ 0, 90 });
  cw_control_set_size_flags(P_recent, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(box, P_recent);
  row = cw_node_new("HFlowContainer", NULL);
  cw_node_add_child(row, button("PmOpen", "Open", on_open, NULL));
  cw_node_add_child(row, button("PmRename", "Rename to Name", on_rename, NULL));
  cw_node_add_child(row, button("PmRemove", "Remove from List", on_remove, NULL));
  cw_node_add_child(box, row);

  P_build_status = cw_node_new("Label", "PmBuildStatus");
  cw_node_set(P_build_status, "ellipsis", cw_val_bool(true));
  cw_node_add_child(box, P_build_status);
  row = cw_node_new("HFlowContainer", NULL);
  cw_node_add_child(row, button("PmBuildAll", "Build Native + Cart", on_build, (void *)"all"));
  cw_node_add_child(row, button("PmBuildNative", "Build Native", on_build, (void *)"native"));
  cw_node_add_child(row, button("PmBuildCart", "Build Cart", on_build, (void *)"cart"));
  cw_node_add_child(box, row);
  P_status = cw_node_new("Label", "PmStatus");
  cw_node_set(P_status, "autowrap", cw_val_bool(true));
  cw_node_add_child(box, P_status);
  panel_refresh();
  return scroll;
}

/* ── module ─────────────────────────────────────────────────────────── */

static void init(void) {
  cwed_setting_define(&(cwed_setting_desc){ .key = "projects.dir", .type = CWED_ARG_STRING, .def = "",
    .doc = "Where new projects are created (natively); empty = ~/cartwheel-projects." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "projects.recent", .type = CWED_ARG_STRING, .def = "", .hidden = true,
    .doc = "Recently opened projects (directories, or memory:<name> in a cart), newest first, joined by '|'." });

  static const cwed_param new_p[] = {
    { "name", CWED_ARG_STRING, NULL, "the project's name (its app name is derived: \"My Game\" -> my_game)" },
    { "template", CWED_ARG_STRING, "empty_2d", "template id (project.templates)" },
    { "dir", CWED_ARG_STRING, "", "natively: the new project's directory (must not exist or be empty); default <projects.dir>/<app>" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.new", .title = "New Project...", .params = new_p,
    .doc = "Create a project from a template and open it: natively a directory (project.cwproj, scenes, src/, CMakeLists.txt "
           "that builds native and cart), in a cart the in-memory project layer (replacing what is there).",
    .exec = cmd_new });
  cwed_command_register(&(cwed_command_desc){ .name = "project.templates", .title = "List Project Templates", .flags = CWED_CMD_QUERY,
    .doc = "The templates project.new can use: id, title, description.", .exec = cmd_templates });
  cwed_command_register(&(cwed_command_desc){ .name = "project.recent", .title = "Recent Projects", .flags = CWED_CMD_QUERY,
    .doc = "Recently opened projects, newest first: entry (directory or memory:<name>), name, current.", .exec = cmd_recent });
  static const cwed_param rename_p[] = {
    { "name", CWED_ARG_STRING, NULL, "the new name (project.cwproj `name`)" },
    { "project", CWED_ARG_STRING, "", "a recent project's entry (project.recent); default the open project" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.rename", .title = "Rename Project...", .params = rename_p,
    .doc = "Change a project's display name (its directory and app name stay).", .exec = cmd_rename });
  static const cwed_param remove_p[] = {
    { "project", CWED_ARG_STRING, NULL, "a recent project's entry (project.recent)" },
    { "delete_files", CWED_ARG_BOOL, "false", "natively: also delete its directory (only a directory holding project.cwproj, never the open project)" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.remove", .title = "Remove Project from List...", .params = remove_p,
    .doc = "Remove a project from the recent list; delete_files=true also deletes it from disk.", .exec = cmd_remove });
  static const cwed_param archive_p[] = {
    { "path", CWED_ARG_STRING, "", "natively: write the archive to this file instead of returning it" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.archive", .title = "Export Project Archive", .params = archive_p,
    .flags = CWED_CMD_QUERY,
    .doc = "The project as one archive (the CWVFS1 format of cartwheel/vfs.h, base64 in `archive`): every project file natively "
           "(not .cwbuild/.cwcache/exports), the project layer in a cart. project.import brings it back.",
    .exec = cmd_archive });
  static const cwed_param import_p[] = {
    { "archive", CWED_ARG_STRING, "", "the archive as base64 (small projects: a channel request is at most 4 KiB)" },
    { "file", CWED_ARG_STRING, "", "a file holding the archive or its base64 (in a cart a project-layer file: `vfs write`/`vfs append`)" },
    { "dir", CWED_ARG_STRING, "", "natively: the directory to unpack into (must not exist or be empty)" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.import", .title = "Import Project Archive...", .params = import_p,
    .doc = "Open a project from an archive (project.archive): natively unpacked into dir=, in a cart it replaces the project layer.",
    .exec = cmd_import });
  static const cwed_param build_p[] = { { "target", CWED_ARG_STRING, "all", "native, cart or all" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "project.build", .title = "Build Project", .params = build_p,
    .doc = "Natively: build the open project (tools/cwproject.mjs build) in the background; output goes to the Output panel, "
           "project.build_status reports the result and the built game_exe / game_cart.",
    .exec = cmd_build });
  cwed_command_register(&(cwed_command_desc){ .name = "project.build_status", .title = "Build Status", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The last build: state (idle running ok failed), target, exit code, per-target results, game_exe, game_cart.",
    .exec = cmd_build_status });
  static const cwed_param rect_p[] = { { "name", CWED_ARG_STRING, NULL, "a widget's node name (e.g. PmCreate)" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "ui.rect", .title = "Widget Rect", .params = rect_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Where a named widget of the editor UI is (global rect, centre, visible, disabled; for a dock page also its tab): "
           "what pointer-driven tests click.",
    .exec = cmd_ui_rect });

  cwed_menu_add("File/New Project...", "panel.show projects", 190);
  cwed_menu_add("Project/New Project...", "panel.show projects", 100);
  cwed_menu_add("Project/Build Native + Cart", "project.build target=all", 200);
  cwed_menu_add("Project/Export Archive", "project.archive", 300);
  cwed_panel_register(&(cwed_panel_desc){ .id = "projects", .title = "Projects", .slot = "center", .order = 60, .create = create });
  cwed_subscribe(CWED_EV_PROJECT, on_project, NULL);
  pm_settings_init();
}

static void frame(double dt) {
  (void)dt;
  build_frame();
  if (panel_dirty) panel_refresh();
  pm_settings_frame();
}

static void shutdown(void) {
  if (B.proc) cw_process_free(B.proc);
  B.proc = NULL;
}

CWED_MODULE(project_manager) = { .title = "Project Manager", .init = init, .frame = frame, .shutdown = shutdown };

/* FileSystem panel: the project's files over every layer
 * (cartwheel/vfs.h: the native project directory, the in-memory layer, the
 * bundled assets), with a folder/file tree, a thumbnail grid (previews from
 * the previews module through `preview.get`), filters, a dependency view
 * ("what uses this?") and file operations that keep references valid:
 *
 *   fs.list / fs.info / fs.deps     queries (JSON)
 *   fs.refresh                      rescan the project (also on every vfs change)
 *   fs.select / fs.filter           the panel's view state
 *   fs.rename / fs.move             move a file or folder; its UID survives
 *                                   (a .cwimport sidecar keeps it), scene
 *                                   references (ext_scene paths, res(0x...)
 *                                   sub-resource UIDs) and .gltf URIs are
 *                                   rewritten; one undo step for all of it
 *   fs.delete                       refused while anything references the file
 *                                   (force=true deletes anyway); undoable
 *   fs.import_file                  copy a file from outside the project in
 *                                   (native editor; a cart takes `vfs import`)
 *   fs.place                        put a model or scene into the open scene
 *                                   (a MeshInstance3D with the model's UID, or
 *                                   a linked instance); one scene undo step
 *
 * Dragging a file (kind "file", text = its path) onto the scene tree or a
 * scene view places it there (fs.place). The module indexes every file's
 * UID with the resource system (cw_res_index_set), so res(0x...) values in
 * an edited scene load from the project even without a pack. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !CW_CART
#include <sys/stat.h>
#endif

#include "cartwheel/input.h"
#include "cartwheel/resource.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"
#include "cwpack.h"

/* ── the project's files ───────────────────────────────────────────────── */

typedef CW_ARRAY(const char *) str_list;

typedef struct fs_file {
  const char *path; /* interned */
  const char *type; /* scene model texture audio font project text other */
  int64_t size;
  cw_vfs_layer layer;
  uint64_t uid;
  bool sidecar;     /* has <path>.cwimport */
  str_list deps;    /* files it references (interned project paths) */
  str_list subs;    /* a model's meshes and materials ("models/x.glb#mesh/0/0"): previewable and placeable */
} fs_file;

static CW_ARRAY(fs_file) files; /* sorted by path */
static uint32_t scanned_gen;
static int scan_count;
static bool need_scan = true, view_dirty = true;

/* the view */
static char sel_path[512], filter_text[128], filter_type[32] = "all", cur_dir[512];

static const char *ext_of(const char *p) {
  const char *dot = strrchr(p, '.'), *slash = strrchr(p, '/');
  return dot && (!slash || dot > slash) ? dot : "";
}
static bool ext_is(const char *p, const char *list) {
  char e[16];
  const char *d = ext_of(p);
  size_t n = strlen(d);
  if (!n || n >= sizeof e) return false;
  for (size_t i = 0; i <= n; i++) e[i] = (char)(d[i] >= 'A' && d[i] <= 'Z' ? d[i] + 32 : d[i]);
  for (const char *s = list; (s = strstr(s, e)); s += n)
    if ((s == list || s[-1] == ' ') && (s[n] == 0 || s[n] == ' ')) return true;
  return false;
}
static const char *type_of(const char *p) {
  const char *h = strchr(p, '#');
  if (h) return !strncmp(h + 1, "mesh/", 5) ? "mesh" : !strncmp(h + 1, "material/", 9) ? "material" : "other";
  if (ext_is(p, ".cwscene")) return "scene";
  if (ext_is(p, ".gltf .glb")) return "model";
  if (ext_is(p, ".png .jpg .jpeg .ktx2 .gif")) return "texture";
  if (ext_is(p, ".wav .ogg .opus .mp3 .flac .xm .mod .s3m .it")) return "audio";
  if (ext_is(p, ".ttf .otf .ttc")) return "font";
  if (ext_is(p, ".cwproj")) return "project";
  if (ext_is(p, ".txt .md .json .cfg .ini .csv .cwtheme .glsl")) return "text";
  return "other";
}

static int find_file(const char *path) {
  int lo = 0, hi = files.count;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    int c = strcmp(files.data[mid].path, path);
    if (!c) return mid;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return -1;
}

/* a file's UID: its sidecar's uid line, else the hash of its path */
static uint64_t sidecar_uid(const char *path, bool *has) {
  size_t n;
  char *t = cw_vfs_read(cw_frame_printf("%s.cwimport", path), &n);
  *has = t != NULL;
  uint64_t uid = 0;
  for (char *line = t; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!strncmp(line, "uid", 3)) {
      char *eq = strchr(line, '=');
      if (eq) uid = strtoull(eq + 1, NULL, 0);
    }
    line = nl ? nl + 1 : NULL;
  }
  free(t);
  return uid ? uid : cw_res_path_uid(path);
}

static void add_dep(fs_file *f, const char *p) {
  p = cw_intern(p);
  for (int i = 0; i < f->deps.count; i++)
    if (f->deps.data[i] == p) return;
  cw_array_push(&f->deps, p);
}

/* "dir/" + uri, normalized ("a/b/../c" -> "a/c"); false when it leaves the project */
static bool join_rel(const char *base_file, const char *uri, char *out, size_t cap) {
  char dec[512], tmp[1024];
  cwp_uri_decode(uri, dec, sizeof dec);
  const char *slash = strrchr(base_file, '/');
  int dl = slash ? (int)(slash - base_file) + 1 : 0;
  snprintf(tmp, sizeof tmp, "%.*s%s", dl, base_file, dec);
  /* normalize */
  char *parts[64];
  int np = 0;
  for (char *s = strtok(tmp, "/"); s; s = strtok(NULL, "/")) {
    if (!strcmp(s, ".")) continue;
    if (!strcmp(s, "..")) {
      if (!np) return false;
      np--;
      continue;
    }
    if (np < 64) parts[np++] = s;
  }
  out[0] = 0;
  size_t w = 0;
  for (int i = 0; i < np; i++) w += (size_t)snprintf(out + w, w < cap ? cap - w : 0, "%s%s", i ? "/" : "", parts[i]);
  return np > 0 && w < cap;
}

typedef struct { fs_file *f; } gltf_ctx;
static void index_sub(const char *name, const char *type, void *user) {
  (void)type;
  fs_file *f = ((gltf_ctx *)user)->f;
  const char *sub = cw_frame_printf("%s#%s", f->path, name);
  cw_res_index_set(cw_res_path_uid(sub), sub);
  if (!strcmp(type, "mesh") || !strcmp(type, "material")) cw_array_push(&f->subs, cw_intern(sub));
}
static void gltf_file(const char *uri, void *user) {
  fs_file *f = ((gltf_ctx *)user)->f;
  char p[512];
  if (join_rel(f->path, uri, p, sizeof p)) add_dep(f, p);
}

/* res(0x...) and [ext_scene path="..."] in scene text */
static void scene_deps(fs_file *f, const char *text) {
  for (const char *line = text; line && *line;) {
    const char *nl = strchr(line, '\n');
    size_t len = nl ? (size_t)(nl - line) : strlen(line);
    if (!strncmp(line, "[ext_scene", 10) || !strncmp(line, "[ext_resource", 13)) {
      const char *p = strstr(line, "path=\"");
      if (p && p < line + len) {
        p += 6;
        const char *e = memchr(p, '"', len - (size_t)(p - line));
        if (e) add_dep(f, cw_frame_printf("%.*s", (int)(e - p), p));
      }
    }
    for (const char *r = line; (r = strstr(r, "res(")) && r < line + len; r += 4) {
      char *end;
      uint64_t uid = strtoull(r + 4, &end, 0);
      if (end == r + 4 || !uid) continue;
      const char *path = cw_res_index_path(uid);
      if (!path) continue;
      const char *hash = strchr(path, '#');
      add_dep(f, hash ? cw_frame_printf("%.*s", (int)(hash - path), path) : path);
    }
    line = nl ? nl + 1 : NULL;
  }
}

/* project.cwproj: main_scene and the scenes list name project files */
static bool is_sep(char ch) { return ch == ' ' || ch == ',' || ch == '\t' || ch == '=' || ch == '"' || ch == '\n' || ch == '\r'; }
static void project_deps(fs_file *f, char *text) {
  for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
    char *eq = strchr(line, '=');
    if (line[0] == '#' || !eq) continue;
    for (char *s = eq + 1; *s;) {
      while (*s && is_sep(*s)) s++;
      char *e = s;
      while (*e && !is_sep(*e)) e++;
      if (e > s) {
        const char *tok = cw_frame_printf("%.*s", (int)(e - s), s);
        if (find_file(tok) >= 0) add_dep(f, tok);
      }
      s = e;
    }
  }
}

static void scan_add(const char *path, const cw_vfs_entry *e, void *user) {
  (void)user;
  if (ext_is(path, ".cwimport")) return; /* import settings travel with their file */
  fs_file f = { cw_intern(path), type_of(path), e->size, e->layer, 0, false, { 0 }, { 0 } };
  cw_array_push(&files, f);
}

static int cmp_file(const void *a, const void *b) { return strcmp(((const fs_file *)a)->path, ((const fs_file *)b)->path); }

static void scan(void) {
  for (int i = 0; i < files.count; i++) cw_array_free(&files.data[i].deps), cw_array_free(&files.data[i].subs);
  cw_array_clear(&files);
  cw_vfs_walk("", 0, scan_add, NULL);
  if (files.count > 1) qsort(files.data, (size_t)files.count, sizeof *files.data, cmp_file); /* find_file searches by path */
  /* pass 1: UIDs (and a glTF's sub-resources), pass 2: references */
  for (int i = 0; i < files.count; i++) {
    fs_file *f = &files.data[i];
    f->uid = sidecar_uid(f->path, &f->sidecar);
    cw_res_index_set(f->uid, f->path);
    if (!strcmp(f->type, "model")) {
      size_t n;
      uint8_t *b = cw_vfs_read(f->path, &n);
      gltf_ctx g = { f };
      if (b) cwp_gltf_subresources(b, n, index_sub, gltf_file, &g);
      free(b);
    }
  }
  for (int i = 0; i < files.count; i++) {
    fs_file *f = &files.data[i];
    bool scene = !strcmp(f->type, "scene"), proj = !strcmp(f->type, "project");
    if (!scene && !proj) continue;
    char *t = cw_vfs_read(f->path, NULL);
    if (t && scene) scene_deps(f, t);
    if (t && proj) project_deps(f, t);
    free(t);
  }
  scanned_gen = cw_vfs_generation();
  scan_count++;
  need_scan = false;
  /* what the project holds now: the panel rebuilds, and others hear of it, only when that changed */
  uint64_t h = 0x84222325cbf29ce4ull;
  for (int i = 0; i < files.count; i++) {
    const fs_file *f = &files.data[i];
    h = cw_hash_bytes(f->path, strlen(f->path), h);
    h = cw_hash_bytes(&f->size, sizeof f->size, h);
    h = cw_hash_bytes(&f->uid, sizeof f->uid, h);
    for (int k = 0; k < f->deps.count; k++) h = cw_hash_bytes(f->deps.data[k], strlen(f->deps.data[k]), h);
  }
  static uint64_t last_h;
  if (h == last_h) return;
  last_h = h;
  view_dirty = true;
  if (sel_path[0] && find_file(sel_path) < 0 && !cw_vfs_is_dir(sel_path)) sel_path[0] = 0;
  cwed_emit(&(cwed_event){ .type = CWED_EV_FILES });
}

static void ensure_scan(void) {
  if (need_scan || scanned_gen != cw_vfs_generation()) scan();
}

/* files that reference path (sorted) */
static int users_of(const char *path, const char **out, int cap) {
  path = cw_intern(path);
  int n = 0;
  for (int i = 0; i < files.count; i++)
    for (int k = 0; k < files.data[i].deps.count; k++)
      if (files.data[i].deps.data[k] == path) {
        if (n < cap) out[n] = files.data[i].path;
        n++;
        break;
      }
  return n;
}

static bool passes_filter(const fs_file *f) {
  if (strcmp(filter_type, "all") && strcmp(filter_type, f->type)) return false;
  if (!filter_text[0]) return true;
  /* case-insensitive substring of the path */
  size_t n = strlen(filter_text);
  for (const char *s = f->path; *s; s++) {
    size_t k = 0;
    while (k < n && s[k] && (s[k] | 32) == (filter_text[k] | 32)) k++;
    if (k == n) return true;
  }
  return false;
}

static void json_file(cwed_json *j, const fs_file *f) {
  static const char *lname[] = { "none", "asset", "disk", "memory" };
  cwed_json_raw(j, "{");
  cwed_json_key(j, "path");
  cwed_json_str(j, f->path);
  cwed_json_key(j, "type");
  cwed_json_str(j, f->type);
  cwed_json_key(j, "size");
  cwed_json_printf(j, "%lld", (long long)f->size);
  cwed_json_key(j, "layer");
  cwed_json_str(j, lname[f->layer]);
  cwed_json_key(j, "uid");
  cwed_json_str(j, cw_frame_printf("0x%016llx", (unsigned long long)f->uid));
  cwed_json_raw(j, "}");
}

/* ── file transactions: the undo state of every file-changing command ──── */

typedef struct fchange {
  const char *path;
  uint8_t *old_b;  /* NULL when the file did not exist */
  size_t old_n;
  cw_vfs_layer old_layer;
  uint8_t *new_b;  /* NULL = removed by the command */
  size_t new_n;
} fchange;
typedef struct ftx {
  CW_ARRAY(fchange) ch;
  str_list touched_scenes; /* scenes rewritten: open copies are reloaded */
  char summary[1100];
} ftx;

static void ftx_free(cwed_call *c, void *state) {
  (void)c;
  ftx *t = state;
  if (!t) return;
  for (int i = 0; i < t->ch.count; i++) cw_free(t->ch.data[i].old_b), cw_free(t->ch.data[i].new_b);
  cw_array_free(&t->ch);
  cw_array_free(&t->touched_scenes);
  cw_free(t);
}

static uint8_t *read_copy(const char *path, size_t *n) {
  size_t len;
  void *d = cw_vfs_read(path, &len);
  if (!d) return NULL;
  uint8_t *b = cw_alloc(len + 1, CW_MEM_EDITOR);
  memcpy(b, d, len);
  b[len] = 0;
  free(d);
  *n = len;
  return b;
}

static fchange *ftx_touch(ftx *t, const char *path) {
  path = cw_intern(path);
  for (int i = 0; i < t->ch.count; i++)
    if (t->ch.data[i].path == path) return &t->ch.data[i];
  fchange c = { .path = path, .old_layer = cw_vfs_where(path) };
  if (c.old_layer != CW_VFS_NONE) {
    c.old_b = read_copy(path, &c.old_n);
    c.new_b = cw_alloc(c.old_n + 1, CW_MEM_EDITOR);
    memcpy(c.new_b, c.old_b, c.old_n + 1);
    c.new_n = c.old_n;
  }
  cw_array_push(&t->ch, c);
  return &t->ch.data[t->ch.count - 1];
}
static void ftx_set(ftx *t, const char *path, const void *data, size_t n) {
  fchange *c = ftx_touch(t, path);
  cw_free(c->new_b);
  c->new_b = cw_alloc(n + 1, CW_MEM_EDITOR);
  memcpy(c->new_b, data, n);
  c->new_b[n] = 0;
  c->new_n = n;
}
static void ftx_remove(ftx *t, const char *path) {
  fchange *c = ftx_touch(t, path);
  cw_free(c->new_b);
  c->new_b = NULL;
  c->new_n = 0;
}

/* open documents showing one of the scenes: refuse while unsaved */
static bool docs_clean(cwed_call *c, ftx *t) {
  for (int i = 0; i < cwed_doc_count(); i++) {
    cwed_doc *d = cwed_doc_at(i);
    const char *p = cw_intern(cwed_doc_path(d));
    for (int k = 0; k < t->ch.count; k++)
      if (t->ch.data[k].path == p && cwed_doc_dirty(d))
        return cwed_fail(c, "%s is open with unsaved changes and this changes its file: save or close it first (scene.save)", p);
  }
  return true;
}

/* reopen clean documents whose files changed, keeping the active one active */
static void reload_docs(ftx *t) {
  cwed_doc *act = cwed_doc_active();
  char active[512];
  snprintf(active, sizeof active, "%s", act ? cwed_doc_path(act) : "");
  bool any = false;
  for (int k = 0; k < t->ch.count; k++) {
    const char *p = t->ch.data[k].path;
    for (int i = 0; i < cwed_doc_count(); i++) {
      if (strcmp(cwed_doc_path(cwed_doc_at(i)), p)) continue;
      cwed_runf("scene.close doc=%s force=true", cwed_quote(p));
      if (t->ch.data[k].new_b && cw_vfs_exists(p)) cwed_runf("scene.open path=%s", cwed_quote(p));
      any = true;
      break;
    }
  }
  if (any && active[0] && cw_vfs_exists(active)) cwed_runf("scene.switch doc=%s", cwed_quote(active));
}

static void ftx_apply(ftx *t, bool forward) {
  for (int k = 0; k < t->ch.count; k++) {
    int i = forward ? k : t->ch.count - 1 - k;
    fchange *c = &t->ch.data[i];
    if (forward) {
      if (c->new_b) cw_vfs_write(c->path, c->new_b, c->new_n);
      else if (cw_vfs_exists(c->path)) cw_vfs_remove(c->path);
    } else if (c->old_layer == CW_VFS_NONE) {
      if (cw_vfs_exists(c->path)) cw_vfs_remove(c->path);
      else cw_vfs_revert(c->path); /* drop a removal mark left behind */
    } else {
      /* a bundled file comes back as the bundle's own copy when that is what it was */
      if (c->old_layer == CW_VFS_ASSET) cw_vfs_revert(c->path);
      size_t n;
      uint8_t *now = cw_vfs_where(c->path) != CW_VFS_NONE ? read_copy(c->path, &n) : NULL;
      if (!now || n != c->old_n || memcmp(now, c->old_b, n)) cw_vfs_write(c->path, c->old_b, c->old_n);
      cw_free(now);
    }
  }
  cw_scene_cache_forget(NULL);
  need_scan = true;
  ensure_scan();
  reload_docs(t);
  cwed_emit(&(cwed_event){ .type = CWED_EV_FILES });
}

static bool ftx_exec_redo(cwed_call *c) {
  ftx *t = cwed_call_state(c);
  if (!t) return false;
  ftx_apply(t, true);
  cwed_json_key(cwed_reply(c), "redo");
  cwed_json_str(cwed_reply(c), t->summary);
  return true;
}
static void ftx_undo(cwed_call *c) {
  ftx *t = cwed_call_state(c);
  if (t) ftx_apply(t, false);
}

static void reply_changes(cwed_call *c, ftx *t) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "changed");
  cwed_json_raw(j, "[");
  for (int i = 0; i < t->ch.count; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_raw(j, "{");
    cwed_json_key(j, "path");
    cwed_json_str(j, t->ch.data[i].path);
    cwed_json_key(j, "op");
    cwed_json_str(j, !t->ch.data[i].new_b ? "removed" : !t->ch.data[i].old_b ? "created" : "rewritten");
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
}

/* ── rename / move ─────────────────────────────────────────────────────── */

static bool valid_path(const char *p) {
  return p && *p && p[0] != '/' && !strstr(p, "..") && !strchr(p, '\\') && p[strlen(p) - 1] != '/' && !strchr(p, '"') &&
         !strchr(p, '\n');
}

/* replace every occurrence of `from` (exact bytes) with `to` in text */
static char *replace_all(const char *text, size_t len, const char *from, const char *to, size_t *out_len, int *count) {
  size_t fl = strlen(from), tl = strlen(to);
  int n = 0;
  for (const char *s = text; (s = strstr(s, from)); s += fl) n++;
  *count = n;
  if (!n) return NULL;
  char *o = cw_alloc(len + (size_t)n * (tl > fl ? tl - fl : 0) + 1, CW_MEM_EDITOR), *w = o;
  const char *s = text;
  for (const char *m; (m = strstr(s, from)); s = m + fl) {
    memcpy(w, s, (size_t)(m - s));
    w += m - s;
    memcpy(w, to, tl);
    w += tl;
  }
  size_t rest = len - (size_t)(s - text);
  memcpy(w, s, rest);
  w += rest;
  *w = 0;
  *out_len = (size_t)(w - o);
  return o;
}

/* replace whole tokens (separated as in project.cwproj values) */
static char *replace_token(const char *text, size_t len, const char *from, const char *to, size_t *out_len, int *count) {
  size_t fl = strlen(from), tl = strlen(to);
  CW_ARRAY(char) o = { 0 };
  int n = 0;
  for (size_t i = 0; i < len;) {
    if (!strncmp(text + i, from, fl) && (i == 0 || is_sep(text[i - 1])) && (i + fl == len || is_sep(text[i + fl]))) {
      for (size_t k = 0; k < tl; k++) cw_array_push(&o, to[k]);
      i += fl;
      n++;
    } else cw_array_push(&o, text[i++]);
  }
  cw_array_push(&o, 0);
  *count = n;
  if (!n) { cw_array_free(&o); return NULL; }
  *out_len = (size_t)o.count - 1;
  return o.data;
}

/* the relative URI from a .gltf at `gltf` to `target` */
static void rel_uri(const char *gltf, const char *target, char *out, size_t cap) {
  const char *gs = strrchr(gltf, '/');
  size_t gd = gs ? (size_t)(gs - gltf) + 1 : 0;
  size_t common = 0; /* common directory prefix (ending at a '/') */
  for (size_t i = 0; i < gd && target[i] && gltf[i] == target[i]; i++)
    if (gltf[i] == '/') common = i + 1;
  out[0] = 0;
  size_t w = 0;
  for (size_t i = common; i < gd; i++)
    if (gltf[i] == '/') w += (size_t)snprintf(out + w, w < cap ? cap - w : 0, "../");
  snprintf(out + w, w < cap ? cap - w : 0, "%s", target + common);
}

/* plan moving one file `from` -> `to` into t: the file, its sidecar (with
 * its UID kept), and every reference to it in other files */
static void plan_move(ftx *t, const char *from, const char *to) {
  size_t n;
  uint8_t *b = read_copy(from, &n);
  if (!b) return;
  ftx_set(t, to, b, n);
  ftx_remove(t, from);
  cw_free(b);
  /* the UID survives the move: the sidecar moves along, or one is written */
  bool has;
  uint64_t uid = sidecar_uid(from, &has);
  const char *sf = cw_frame_printf("%s.cwimport", from), *st = cw_frame_printf("%s.cwimport", to);
  if (has) {
    uint8_t *s = read_copy(sf, &n);
    ftx_set(t, st, s, n);
    ftx_remove(t, sf);
    cw_free(s);
  } else {
    const char *line = cw_frame_printf("# import settings (cwpack sidecar); the uid keeps references valid across renames\n"
                                       "uid = 0x%016llx\n", (unsigned long long)uid);
    ftx_set(t, st, line, strlen(line));
  }
  /* references: scenes (ext_scene paths, sub-resource UIDs), glTF URIs */
  const char *users[256];
  int nu = users_of(from, users, 256);
  for (int i = 0; i < nu && i < 256; i++) {
    const char *u = users[i];
    fchange *c = ftx_touch(t, u);
    if (!c->new_b) continue;
    char *txt = (char *)c->new_b;
    size_t tl = c->new_n, nl;
    int cnt;
    char *r;
    if (!strcmp(type_of(u), "scene")) {
      r = replace_all(txt, tl, cw_frame_printf("path=\"%s\"", from), cw_frame_printf("path=\"%s\"", to), &nl, &cnt);
      if (r) { ftx_set(t, u, r, nl); cw_free(r); c = ftx_touch(t, u); txt = (char *)c->new_b; tl = c->new_n; }
      /* sub-resource UIDs hash their path: res(old) -> res(new) */
      static const char *kinds[] = { "mesh/%d/%d", "material/%d", "texture/%d" };
      for (int k = 0; k < 3; k++)
        for (int a = 0; a < 64; a++)
          for (int q = 0; q < (k ? 1 : 16); q++) {
            const char *sub = k ? cw_frame_printf(kinds[k], a) : cw_frame_printf(kinds[k], a, q);
            uint64_t ou = cw_res_path_uid(cw_frame_printf("%s#%s", from, sub)), nu2 = cw_res_path_uid(cw_frame_printf("%s#%s", to, sub));
            const char *needle = cw_frame_printf("res(0x%llx)", (unsigned long long)ou);
            if (!strstr(txt, needle)) continue;
            r = replace_all(txt, tl, needle, cw_frame_printf("res(0x%llx)", (unsigned long long)nu2), &nl, &cnt);
            if (r) { ftx_set(t, u, r, nl); cw_free(r); c = ftx_touch(t, u); txt = (char *)c->new_b; tl = c->new_n; }
          }
      cw_array_push(&t->touched_scenes, u);
    } else if (!strcmp(type_of(u), "project")) {
      r = replace_token(txt, tl, from, to, &nl, &cnt);
      if (r) { ftx_set(t, u, r, nl); cw_free(r); }
    } else if (ext_is(u, ".gltf")) {
      char ou[512], nu2[512];
      rel_uri(u, from, ou, sizeof ou);
      rel_uri(u, to, nu2, sizeof nu2);
      r = replace_all(txt, tl, cw_frame_printf("\"%s\"", ou), cw_frame_printf("\"%s\"", nu2), &nl, &cnt);
      if (r) { ftx_set(t, u, r, nl); cw_free(r); }
    }
  }
}

typedef struct { str_list *l; } collect_ctx;
static void collect(const char *path, const cw_vfs_entry *e, void *user) {
  (void)e;
  cw_array_push(((collect_ctx *)user)->l, cw_intern(path));
}

static bool rename_exec(cwed_call *c) {
  if (cwed_call_state(c)) return ftx_exec_redo(c);
  ensure_scan();
  const char *from = cwed_arg(c, "from");
  char to[512];
  snprintf(to, sizeof to, "%s", cwed_arg(c, "to"));
  if (cwed_arg_given(c, "dir")) { /* fs.move: keep the name, change the folder */
    const char *dir = cwed_arg(c, "dir"), *base = strrchr(from, '/');
    snprintf(to, sizeof to, "%s%s%s", dir, *dir ? "/" : "", base ? base + 1 : from);
  }
  if (!valid_path(from) || !valid_path(to)) return cwed_fail(c, "paths are project-relative, '/'-separated, without '..' or quotes");
  if (!strcmp(from, to)) return cwed_fail(c, "'%s' is already there", from);
  bool is_dir = !cw_vfs_exists(from) && cw_vfs_is_dir(from);
  if (!is_dir && !cw_vfs_exists(from)) return cwed_fail(c, "no file or folder '%s' (fs.list shows the project)", from);
  if (cw_vfs_exists(to) || cw_vfs_is_dir(to)) return cwed_fail(c, "'%s' already exists: pick another name", to);
  size_t fl = strlen(from);
  if (is_dir && !strncmp(to, from, fl) && to[fl] == '/') return cwed_fail(c, "cannot move a folder into itself");
  if (!is_dir && ext_is(from, ".cwimport")) return cwed_fail(c, "a .cwimport sidecar moves with its file: rename the file");
  for (int i = 0; i < cwed_doc_count(); i++) {
    const char *dp = cwed_doc_path(cwed_doc_at(i));
    if (*dp && (!strcmp(dp, from) || (is_dir && !strncmp(dp, from, fl) && dp[fl] == '/')))
      return cwed_fail(c, "%s is open: close it first (scene.close doc=%s), then move it", dp, dp);
  }
  ftx *t = cw_calloc(1, sizeof *t, CW_MEM_EDITOR);
  if (is_dir) {
    str_list all = { 0 };
    collect_ctx cc = { &all };
    cw_vfs_walk(from, CW_VFS_LIST_HIDDEN, collect, &cc);
    for (int i = 0; i < all.count; i++) {
      const char *p = all.data[i];
      if (ext_is(p, ".cwimport") && cw_vfs_exists(cw_frame_printf("%.*s", (int)(strlen(p) - 9), p))) continue; /* moves with its file */
      plan_move(t, p, cw_frame_printf("%s%s", to, p + fl));
    }
    cw_array_free(&all);
  } else {
    plan_move(t, from, to);
  }
  if (!docs_clean(c, t)) { ftx_free(c, t); return false; }
  snprintf(t->summary, sizeof t->summary, "%s -> %s", from, to);
  cwed_call_set_state(c, t);
  ftx_apply(t, true);
  snprintf(sel_path, sizeof sel_path, "%s", to);
  view_dirty = true;
  cwed_json_key(cwed_reply(c), "from");
  cwed_json_str(cwed_reply(c), from);
  cwed_json_key(cwed_reply(c), "to");
  cwed_json_str(cwed_reply(c), to);
  reply_changes(c, t);
  return true;
}

/* ── delete ───────────────────────────────────────────────────────────── */

static bool delete_exec(cwed_call *c) {
  if (cwed_call_state(c)) return ftx_exec_redo(c);
  ensure_scan();
  const char *path = cwed_arg(c, "path");
  if (!valid_path(path)) return cwed_fail(c, "'%s' is not a project path", path);
  bool is_dir = !cw_vfs_exists(path) && cw_vfs_is_dir(path);
  if (!is_dir && !cw_vfs_exists(path)) return cwed_fail(c, "no file or folder '%s' (fs.list shows the project)", path);
  str_list all = { 0 };
  if (is_dir) {
    collect_ctx cc = { &all };
    cw_vfs_walk(path, CW_VFS_LIST_HIDDEN, collect, &cc);
  } else {
    cw_array_push(&all, cw_intern(path));
    if (cw_vfs_exists(cw_frame_printf("%s.cwimport", path))) cw_array_push(&all, cw_intern(cw_frame_printf("%s.cwimport", path)));
  }
  size_t pl = strlen(path);
  for (int i = 0; i < cwed_doc_count(); i++) {
    const char *dp = cwed_doc_path(cwed_doc_at(i));
    if (*dp && (!strcmp(dp, path) || (is_dir && !strncmp(dp, path, pl) && dp[pl] == '/'))) {
      cw_array_free(&all);
      return cwed_fail(c, "%s is open: close it first (scene.close doc=%s)", dp, dp);
    }
  }
  if (!cwed_arg_bool(c, "force")) {
    /* referenced from outside what is deleted? */
    for (int i = 0; i < all.count; i++) {
      const char *users[16];
      int nu = users_of(all.data[i], users, 16);
      for (int k = 0; k < nu && k < 16; k++) {
        bool inside = false;
        for (int m = 0; m < all.count; m++) inside |= all.data[m] == users[k];
        if (inside) continue;
        cwed_fail(c, "%s is used by %s%s: change those first, or fs.delete %s force=true (they will fail to load)", all.data[i],
                  users[k], nu > 1 ? cw_frame_printf(" and %d more (fs.deps %s lists them)", nu - 1, all.data[i]) : "",
                  cwed_quote(path));
        cw_array_free(&all);
        return false;
      }
    }
  }
  ftx *t = cw_calloc(1, sizeof *t, CW_MEM_EDITOR);
  for (int i = 0; i < all.count; i++) ftx_remove(t, all.data[i]);
  cw_array_free(&all);
  snprintf(t->summary, sizeof t->summary, "delete %s", path);
  cwed_call_set_state(c, t);
  ftx_apply(t, true);
  if (!strcmp(sel_path, path)) sel_path[0] = 0;
  view_dirty = true;
  reply_changes(c, t);
  return true;
}

/* ── import a file from outside the project (native) ──────────────────── */

static bool import_file_exec(cwed_call *c) {
  if (cwed_call_state(c)) return ftx_exec_redo(c);
#if CW_CART
  return cwed_fail(c, "a cart has no file system to copy from: bring files in with `vfs import <archive>` (or vfs write/append), "
                      "then fs.refresh");
#else
  const char *src = cwed_arg(c, "src"), *dest = cwed_arg(c, "dest");
  if (!valid_path(dest)) return cwed_fail(c, "'%s' is not a project path", dest);
  if (cw_vfs_exists(dest)) return cwed_fail(c, "'%s' already exists: pick another name or fs.delete it first", dest);
  FILE *fp = fopen(src, "rb");
  if (!fp) return cwed_fail(c, "cannot read '%s'", src);
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  uint8_t *b = cw_alloc((size_t)n + 1, CW_MEM_EDITOR);
  size_t got = fread(b, 1, (size_t)n, fp);
  fclose(fp);
  ftx *t = cw_calloc(1, sizeof *t, CW_MEM_EDITOR);
  ftx_set(t, dest, b, got);
  cw_free(b);
  snprintf(t->summary, sizeof t->summary, "import %s", dest);
  cwed_call_set_state(c, t);
  ftx_apply(t, true);
  snprintf(sel_path, sizeof sel_path, "%s", dest);
  view_dirty = true;
  reply_changes(c, t);
  return true;
#endif
}

/* ── place a file into the open scene ─────────────────────────────────── */

typedef struct { cw_node node, parent; int index; } place_state;

static void place_free(cwed_call *c, void *state) {
  (void)c;
  place_state *s = state;
  if (s && cw_node_valid(s->node) && CW_HANDLE_IS_NULL(cw_node_parent(s->node))) cw_node_free(s->node);
  cw_free(s);
}

static const char *stem(const char *path) {
  const char *h = strchr(path, '#');
  const char *file = h ? cw_frame_printf("%.*s", (int)(h - path), path) : path;
  const char *b = strrchr(file, '/');
  b = b ? b + 1 : file;
  char *o = cw_frame_printf("%s", b), *dot = strrchr(o, '.');
  if (dot && dot != o) *dot = 0;
  if (h) o = cw_frame_printf("%s_%s", o, h + 1); /* lantern_mesh/0/0 -> lantern_mesh_0_0 below */
  for (char *s = o; *s; s++)
    if (*s == '.' || *s == ':' || *s == '%' || *s == '"' || *s == '/' || *s == ' ') *s = '_';
  return o;
}

static bool place_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  place_state *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node)) { /* redo: the same node */
    int k = cw_node_child_count(s->parent);
    if (s->index < 0 || s->index >= k) cw_node_add_child(s->parent, s->node);
    else cw_node_add_child_at(s->parent, s->node, s->index);
  } else {
    ensure_scan();
    const char *file = cwed_arg(c, "file");
    const char *hash = strchr(file, '#');
    if (!cw_vfs_exists(hash ? cw_frame_printf("%.*s", (int)(hash - file), file) : file))
      return cwed_fail(c, "no file '%s' in the project (fs.list shows it)", file);
    cw_node parent = cwed_arg_node(c, "parent");
    if (CW_HANDLE_IS_NULL(parent)) return false;
    const char *type = type_of(file);
    const char *name = cwed_arg_given(c, "name") && *cwed_arg(c, "name") ? cwed_arg(c, "name") : stem(file);
    cw_node n;
    if (!strcmp(type, "scene")) {
      if (*cwed_doc_path(d) && !strcmp(file, cwed_doc_path(d))) return cwed_fail(c, "a scene cannot instance itself");
      n = cw_scene_instance_linked(file);
      if (CW_HANDLE_IS_NULL(n)) return cwed_fail(c, "%s did not load (the log says why)", file);
    } else if (!strcmp(type, "model") || !strcmp(type, "mesh")) {
      int fi = find_file(file);
      uint64_t uid = fi >= 0 ? files.data[fi].uid : cw_res_path_uid(file);
      cw_res_index_set(uid, file);
      n = cw_node_new("MeshInstance3D", NULL);
      cw_value v = { .type = CW_T_RES };
      v.res = uid;
      if (!cw_node_set(n, "mesh", v)) {
        cw_node_free(n);
        return cwed_fail(c, "MeshInstance3D refused the mesh of %s", file);
      }
    } else {
      return cwed_fail(c, "%s is a %s: fs.place takes models (.gltf .glb), their meshes (file#mesh/m/p) and scenes (.cwscene)", file, type);
    }
    if (cwed_arg_given(c, "position")) {
      cw_value pos;
      if (!cwed_arg_value(c, "position", n, "position", &pos) || !cw_node_set(n, "position", pos)) {
        cw_node_free(n);
        return cwed_arg_given(c, "position") ? cwed_fail(c, "position wants vec3(x, y, z)") : false;
      }
    }
    cw_node_add_child(parent, n);
    /* named after the file (unique among its new siblings) */
    if (!strcmp(type, "model") || !strcmp(type, "mesh") || cwed_arg_given(c, "name")) cw_node_set_name(n, name);
    else cw_node_set_name(n, cw_frame_printf("%s", cw_node_name(n)));
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    s->node = n;
    s->parent = parent;
    s->index = cw_node_index(n);
    cwed_call_set_state(c, s);
  }
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "path");
  cwed_json_str(j, cwed_doc_node_path(d, s->node));
  cwed_json_key(j, "type");
  cwed_json_str(j, cw_node_get_type(s->node)->name);
  cwed_json_key(j, "file");
  cwed_json_str(j, cwed_arg(c, "file"));
  return true;
}
static void place_undo(cwed_call *c) {
  place_state *s = cwed_call_state(c);
  if (s && cw_node_valid(s->node) && cw_node_valid(cw_node_parent(s->node))) cw_node_remove_child(cw_node_parent(s->node), s->node);
}

/* ── queries ──────────────────────────────────────────────────────────── */

static bool list_exec(cwed_call *c) {
  ensure_scan();
  const char *dir = cwed_arg(c, "dir");
  size_t dl = strlen(dir);
  bool rec = cwed_arg_bool(c, "recursive");
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "files");
  cwed_json_raw(j, "[");
  int n = 0;
  for (int i = 0; i < files.count; i++) {
    const fs_file *f = &files.data[i];
    if (dl && (strncmp(f->path, dir, dl) || f->path[dl] != '/')) continue;
    if (!rec && strchr(f->path + (dl ? dl + 1 : 0), '/')) continue;
    if (cwed_arg_bool(c, "filtered") && !passes_filter(f)) continue;
    if (n++) cwed_json_raw(j, ",");
    json_file(j, f);
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "count");
  cwed_json_printf(j, "%d", n);
  return true;
}

static void json_paths(cwed_json *j, const char *key, const char *const *p, int n) {
  cwed_json_key(j, key);
  cwed_json_raw(j, "[");
  for (int i = 0; i < n; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, p[i]);
  }
  cwed_json_raw(j, "]");
}

static bool deps_exec(cwed_call *c) {
  ensure_scan();
  const char *path = cwed_arg(c, "path");
  int i = find_file(path);
  if (i < 0) return cwed_fail(c, "no file '%s' in the project (fs.list shows it)", path);
  const char *users[512];
  int nu = users_of(path, users, 512);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "file");
  json_file(j, &files.data[i]);
  json_paths(j, "uses", files.data[i].deps.data, files.data[i].deps.count);
  json_paths(j, "used_by", users, nu < 512 ? nu : 512);
  return true;
}

static bool refresh_exec(cwed_call *c) {
  cw_vfs_rescan();
  need_scan = true;
  ensure_scan();
  cwed_json_key(cwed_reply(c), "files");
  cwed_json_printf(cwed_reply(c), "%d", files.count);
  return true;
}

/* ── the panel ────────────────────────────────────────────────────────── */

static cw_node panel_box, tree, grid, grid_scroll, info, used_list, filter_edit, type_opt;
static CW_ARRAY(const char *) tree_rows; /* tree item -> path (folders end without '/') */
static CW_ARRAY(const char *) grid_files;
static CW_ARRAY(const char *) used_rows;
static bool syncing;
static int poll_counter;

/* thumbnails: what preview.get last said per path */
typedef struct { const char *path; cw_texture tex; bool ready, error; uint64_t hash; } thumb;
static CW_ARRAY(thumb) thumbs;

static thumb *thumb_for(const char *path) {
  path = cw_intern(path);
  for (int i = 0; i < thumbs.count; i++)
    if (thumbs.data[i].path == path) return &thumbs.data[i];
  thumb t = { path, { 0 }, false, false, 0 };
  cw_array_push(&thumbs, t);
  return &thumbs.data[thumbs.count - 1];
}

static uint64_t json_u64(const char *j, const char *key) {
  const char *k = strstr(j, cw_frame_printf("\"%s\":", key));
  if (!k) return 0;
  k += strlen(key) + 3;
  if (*k == '"') k++;
  return strtoull(k, NULL, 0);
}

/* ask the previews module (through its command, so modules stay apart) */
static void thumb_poll(thumb *t) {
  char *j = NULL;
  if (!cwed_run(cw_frame_printf("preview.get path=%s", cwed_quote(t->path)), &j) || !j) {
    cw_free(j);
    return;
  }
  t->ready = strstr(j, "\"ready\":true") != NULL;
  t->error = strstr(j, "\"error\":\"") != NULL;
  t->tex.id = json_u64(j, "texture");
  t->hash = json_u64(j, "hash");
  cw_free(j);
}

/* the grid: thumbnails of the current folder's files */
#define CELL_W 76.0f
#define CELL_H 92.0f
#define THUMB 64.0f

static const cw_node_type *grid_t;
typedef struct { cw_vec2 press; bool pressed, dragging; int press_cell; float last_w; } grid_data;

static int grid_cols(float w) {
  int c = (int)(w / CELL_W);
  return c < 1 ? 1 : c;
}
static cw_vec2 grid_min(cw_node n, void *data) {
  grid_data *g = data;
  int cols = grid_cols(g->last_w > 0 ? g->last_w : 200);
  int rows = (grid_files.count + cols - 1) / cols;
  return (cw_vec2){ CELL_W, (float)(rows < 1 ? 1 : rows) * CELL_H };
}
static void grid_layout(cw_node n, void *data, cw_vec2 size) {
  grid_data *g = data;
  if (grid_cols(size.x) != grid_cols(g->last_w)) {
    g->last_w = size.x;
    cw_control_update_min_size(n);
  }
  g->last_w = size.x;
}
static cw_rect cell_rect(int i, float w) {
  int cols = grid_cols(w);
  return (cw_rect){ (float)(i % cols) * CELL_W, (float)(i / cols) * CELL_H, CELL_W, CELL_H };
}
static int cell_at(cw_vec2 p, float w) {
  int cols = grid_cols(w);
  int col = (int)(p.x / CELL_W), row = (int)(p.y / CELL_H);
  if (p.x < 0 || p.y < 0 || col >= cols) return -1;
  int i = row * cols + col;
  return i < grid_files.count ? i : -1;
}
static void grid_draw(cw_node n, void *data) {
  grid_data *g = data;
  float w = cw_control_rect(n).w;
  cw_font f = cw_control_theme_font(n, "Label", "font");
  float fs = 12;
  cw_color fg = cw_control_theme_color(n, "Label", "font_color");
  for (int i = 0; i < grid_files.count; i++) {
    cw_rect r = cell_rect(i, w);
    const char *p = grid_files.data[i];
    bool sel = !strcmp(p, sel_path);
    if (sel) cw_ui_draw_round_rect((cw_rect){ r.x + 2, r.y + 2, r.w - 4, r.h - 4 }, 4, (cw_color){ 0.25f, 0.42f, 0.7f, 0.55f });
    cw_rect tr = { r.x + (r.w - THUMB) * 0.5f, r.y + 4, THUMB, THUMB };
    thumb *t = thumb_for(p);
    if (t->ready && t->tex.id) cw_ui_draw_texture(t->tex, tr, (cw_rect){ 0 }, CW_WHITE);
    else {
      cw_ui_draw_rect(tr, (cw_color){ 0.18f, 0.19f, 0.22f, 1 });
      int fi = find_file(p);
      cw_ui_draw_text_in(f, 11, fi >= 0 ? files.data[fi].type : "", tr, CW_HALIGN_CENTER,
                         t->error ? (cw_color){ 0.95f, 0.45f, 0.4f, 1 } : (cw_color){ 0.6f, 0.63f, 0.7f, 1 }, true);
    }
    const char *base = strchr(p, '#') ? strchr(p, '#') - 1 : strrchr(p, '/');
    cw_ui_draw_text_in(f, fs, base ? base + 1 : p, (cw_rect){ r.x + 2, r.y + 4 + THUMB + 2, r.w - 4, 18 }, CW_HALIGN_CENTER, fg, true);
  }
  (void)g;
}
static bool grid_input(cw_node n, void *data, const cw_ui_event *e) {
  grid_data *g = data;
  float w = cw_control_rect(n).w;
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 0) {
    int i = cell_at(e->pos, w);
    g->pressed = true;
    g->dragging = false;
    g->press = e->pos;
    g->press_cell = i;
    if (i >= 0) {
      cwed_runf("fs.select path=%s", cwed_quote(grid_files.data[i]));
      if (e->clicks == 2) {
        const char *p = grid_files.data[i];
        if (!strcmp(type_of(p), "scene")) cwed_runf("scene.open path=%s", cwed_quote(p));
        else if (!strcmp(type_of(p), "model") && cwed_doc_active()) cwed_runf("fs.place file=%s", cwed_quote(p));
      }
    }
    return true;
  }
  if (e->type == CW_UI_MOUSE_MOVE && g->pressed && !g->dragging && (e->buttons & 1) && g->press_cell >= 0) {
    float dx = e->pos.x - g->press.x, dy = e->pos.y - g->press.y;
    if (dx * dx + dy * dy > 36 && g->press_cell < grid_files.count) {
      const char *p = grid_files.data[g->press_cell];
      const char *base = strrchr(p, '/');
      g->dragging = cw_ui_drag_begin(&(cw_ui_drag){ .source = n, .kind = "file", .text = p, .label = base ? base + 1 : p });
    }
    return true;
  }
  if (e->type == CW_UI_MOUSE_UP && e->button == 0) {
    g->pressed = g->dragging = false;
    return true;
  }
  return false;
}
static cw_node_type grid_type = { .name = "EditorFileGrid", .parent = "Control", .data_size = sizeof(grid_data),
                                  .doc = "The FileSystem panel's thumbnail grid (editor only)." };

static void add_tree_items(const char *dir, int parent, int depth) {
  const cw_vfs_entry *e;
  int n = cw_vfs_list_dir(dir, 0, &e);
  if (n <= 0 || depth > 16) return;
  typedef struct { const char *name; bool d; } it;
  it *items = cw_alloc(sizeof *items * (size_t)n, CW_MEM_EDITOR);
  for (int i = 0; i < n; i++) items[i] = (it){ e[i].name, e[i].is_dir };
  for (int i = 0; i < n; i++) {
    const char *p = cw_intern(cw_frame_printf("%s%s%s", dir, *dir ? "/" : "", items[i].name));
    if (items[i].d) {
      /* a folder shows when something inside passes the filter */
      bool any = !filter_text[0] && !strcmp(filter_type, "all");
      size_t pl = strlen(p);
      for (int k = 0; !any && k < files.count; k++)
        any = !strncmp(files.data[k].path, p, pl) && files.data[k].path[pl] == '/' && passes_filter(&files.data[k]);
      if (!any) continue;
      int item = cw_uitree_create_item(tree, parent, cw_frame_printf("%s/", items[i].name));
      cw_uitree_set_metadata(tree, item, tree_rows.count);
      cw_array_push(&tree_rows, p);
      add_tree_items(p, item, depth + 1);
    } else {
      int fi = find_file(p);
      if (fi < 0 || !passes_filter(&files.data[fi])) continue;
      int item = cw_uitree_create_item(tree, parent, items[i].name);
      cw_uitree_set_text(tree, item, 1, files.data[fi].type);
      cw_uitree_set_metadata(tree, item, tree_rows.count);
      cw_array_push(&tree_rows, p);
    }
  }
  cw_free(items);
}

static void rebuild_view(void) {
  syncing = true;
  /* tree */
  cw_uitree_clear(tree);
  cw_array_clear(&tree_rows);
  int root = cw_uitree_create_item(tree, -1, cw_frame_printf("%s/", cwed_project_name()));
  cw_uitree_set_metadata(tree, root, tree_rows.count);
  cw_array_push(&tree_rows, cw_intern(""));
  add_tree_items("", root, 0);
  for (int i = 0; i < tree_rows.count; i++)
    if (!strcmp(tree_rows.data[i], sel_path) && sel_path[0]) {
      for (int it = 0; it < 4096; it++)
        if (cw_uitree_metadata(tree, it) == i && cw_uitree_text(tree, it, 0)) { cw_uitree_select(tree, it); break; }
      break;
    }
  /* grid: the selected folder, or the selected file's folder; with a filter, every match */
  cw_array_clear(&grid_files);
  if (filter_text[0] || strcmp(filter_type, "all")) {
    for (int i = 0; i < files.count; i++)
      if (passes_filter(&files.data[i])) cw_array_push(&grid_files, files.data[i].path);
  } else {
    size_t dl = strlen(cur_dir);
    for (int i = 0; i < files.count; i++) {
      const char *p = files.data[i].path;
      if (dl && (strncmp(p, cur_dir, dl) || p[dl] != '/')) continue;
      if (strchr(p + (dl ? dl + 1 : 0), '/')) continue;
      cw_array_push(&grid_files, p);
    }
  }
  if (!filter_text[0] && !strcmp(filter_type, "all") && sel_path[0]) { /* a model's meshes and materials follow its files */
    char base[512];
    const char *h = strchr(sel_path, '#');
    snprintf(base, sizeof base, "%.*s", h ? (int)(h - sel_path) : (int)strlen(sel_path), sel_path);
    int bi = find_file(base);
    if (bi >= 0)
      for (int k = 0; k < files.data[bi].subs.count; k++) cw_array_push(&grid_files, files.data[bi].subs.data[k]);
  }
  cw_control_update_min_size(grid);
  cw_control_queue_layout(grid);
  /* info + dependency view */
  cw_item_list_clear(used_list);
  cw_array_clear(&used_rows);
  int fi = sel_path[0] ? find_file(sel_path) : -1;
  if (fi >= 0) {
    const fs_file *f = &files.data[fi];
    const char *users[64];
    int nu = users_of(f->path, users, 64);
    cw_label_set_text(info, cw_frame_printf("%s  (%s, %lld bytes, uid 0x%016llx)  uses %d, used by %d", f->path, f->type,
                                            (long long)f->size, (unsigned long long)f->uid, f->deps.count, nu));
    for (int i = 0; i < nu && i < 64; i++) {
      cw_item_list_add_item(used_list, cw_frame_printf("used by  %s", users[i]), (cw_texture){ 0 });
      cw_array_push(&used_rows, users[i]);
    }
    for (int i = 0; i < f->deps.count; i++) {
      cw_item_list_add_item(used_list, cw_frame_printf("uses     %s", f->deps.data[i]), (cw_texture){ 0 });
      cw_array_push(&used_rows, f->deps.data[i]);
    }
    if (!nu && !f->deps.count) cw_item_list_add_item(used_list, "nothing uses this file", (cw_texture){ 0 });
  } else if (strchr(sel_path, '#')) {
    cw_label_set_text(info, cw_frame_printf("%s  (%s of %.*s, uid 0x%016llx)", sel_path, type_of(sel_path), (int)(strchr(sel_path, '#') - sel_path),
                                            sel_path, (unsigned long long)cw_res_path_uid(sel_path)));
  } else {
    cw_label_set_text(info, sel_path[0] ? cw_frame_printf("%s/", sel_path) : "Select a file; drag it into the scene to place it.");
  }
  syncing = false;
  view_dirty = false;
}

static void on_tree_select(cw_node t, const cw_value *a, int argc, void *user) {
  (void)argc, (void)user;
  if (syncing) return;
  int64_t row = cw_uitree_metadata(t, (int)a[0].i);
  if (row < 0 || row >= tree_rows.count) return;
  cwed_runf("fs.select path=%s", cwed_quote(tree_rows.data[row][0] ? tree_rows.data[row] : "."));
}
static void on_used_pick(cw_node l, const cw_value *a, int argc, void *user) {
  (void)l, (void)argc, (void)user;
  if (syncing || a[0].i < 0 || a[0].i >= used_rows.count) return;
  cwed_runf("fs.select path=%s", cwed_quote(used_rows.data[a[0].i]));
}
static void on_filter(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc, (void)user;
  if (!syncing) cwed_runf("fs.filter text=%s type=%s", cwed_quote(a[0].s ? a[0].s : ""), cwed_quote(filter_type));
}
static const char *TYPES[] = { "all", "scene", "model", "texture", "audio", "font", "text", "other" };
static void on_type(cw_node e, const cw_value *a, int argc, void *user) {
  (void)e, (void)argc, (void)user;
  if (!syncing && a[0].i >= 0 && a[0].i < (int64_t)CW_ARRAY_LEN(TYPES))
    cwed_runf("fs.filter text=%s type=%s", cwed_quote(filter_text), TYPES[a[0].i]);
}
static void on_button(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  const char *what = user;
  if (!strcmp(what, "refresh")) cwed_run("fs.refresh", NULL);
  else if (!sel_path[0]) return;
  else if (!strcmp(what, "open")) {
    if (!strcmp(type_of(sel_path), "scene")) cwed_runf("scene.open path=%s", cwed_quote(sel_path));
  } else if (!strcmp(what, "place")) cwed_runf("fs.place file=%s", cwed_quote(sel_path));
  else if (!strcmp(what, "rename"))
    cwed_runf("palette.open text=%s", cwed_quote(cw_frame_printf("fs.rename from=%s to=%s", cwed_quote(sel_path), cwed_quote(sel_path))));
  else if (!strcmp(what, "delete")) cwed_runf("fs.delete path=%s", cwed_quote(sel_path));
}

static cw_node button(const char *text, const char *tip, const char *what) {
  cw_node b = cw_node_new("Button", NULL);
  cw_button_set_text(b, text);
  cw_control_set_tooltip(b, tip);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_button, (void *)what, 0);
  return b;
}

static cw_node create(void *user) {
  (void)user;
  panel_box = cw_node_new("VBoxContainer", NULL);
  cw_node bar = cw_node_new("HBoxContainer", "Filter");
  filter_edit = cw_node_new("LineEdit", "Text");
  cw_node_set(filter_edit, "placeholder_text", cw_val_string("Filter files"));
  cw_control_set_size_flags(filter_edit, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(filter_edit, "text_changed", (cw_node){ 0 }, NULL, on_filter, NULL, 0);
  type_opt = cw_node_new("OptionButton", "Type");
  for (size_t i = 0; i < CW_ARRAY_LEN(TYPES); i++) cw_option_button_add_item(type_opt, TYPES[i]);
  cw_connect(type_opt, "item_selected", (cw_node){ 0 }, NULL, on_type, NULL, 0);
  cw_node_add_child(bar, filter_edit);
  cw_node_add_child(bar, type_opt);
  cw_node_add_child(bar, button("Refresh", "Rescan the project (fs.refresh)", "refresh"));
  cw_node_add_child(panel_box, bar);
  tree = cw_node_new("Tree", "Files");
  cw_uitree_set_columns(tree, 2);
  cw_uitree_set_column_title(tree, 0, "File");
  cw_uitree_set_column_title(tree, 1, "Type");
  cw_uitree_set_column_expand(tree, 0, true, 80);
  cw_uitree_set_column_expand(tree, 1, false, 56);
  cw_control_set_size_flags(tree, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_connect(tree, "item_selected", (cw_node){ 0 }, NULL, on_tree_select, NULL, 0);
  cw_node_add_child(panel_box, tree);
  grid_scroll = cw_node_new("ScrollContainer", "Thumbnails");
  cw_node_set(grid_scroll, "horizontal_scroll_mode", cw_val_int(0));
  cw_control_set_size_flags(grid_scroll, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  grid = cw_node_new("EditorFileGrid", "Grid");
  cw_control_set_size_flags(grid, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_node_add_child(grid_scroll, grid);
  cw_node_add_child(panel_box, grid_scroll);
  info = cw_node_new("Label", "Info");
  cw_node_set(info, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  cw_node_set(info, "ellipsis", cw_val_bool(true));
  cw_node_add_child(panel_box, info);
  used_list = cw_node_new("ItemList", "Dependencies");
  cw_control_set_custom_min_size(used_list, (cw_vec2){ 0, 56 });
  cw_control_set_size_flags(used_list, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  cw_connect(used_list, "item_selected", (cw_node){ 0 }, NULL, on_used_pick, NULL, 0);
  cw_node_add_child(panel_box, used_list);
  cw_node buttons = cw_node_new("HBoxContainer", "Actions");
  cw_node_add_child(buttons, button("Open", "Open the selected scene (scene.open)", "open"));
  cw_node_add_child(buttons, button("Place", "Put the selected model or scene into the open scene (fs.place)", "place"));
  cw_node_add_child(buttons, button("Rename", "Rename or move the selected file; references follow (fs.rename)", "rename"));
  cw_node_add_child(buttons, button("Delete", "Delete the selected file, refused while it is used (fs.delete)", "delete"));
  cw_node_add_child(panel_box, buttons);
  return panel_box;
}

/* ── view commands ────────────────────────────────────────────────────── */

static bool select_exec(cwed_call *c) {
  ensure_scan();
  const char *p = cwed_arg(c, "path");
  if (!strcmp(p, ".") || !*p) {
    sel_path[0] = cur_dir[0] = 0;
  } else if (strchr(p, '#') && find_file(cw_frame_printf("%.*s", (int)(strchr(p, '#') - p), p)) >= 0 &&
             (!strcmp(type_of(p), "mesh") || !strcmp(type_of(p), "material"))) {
    snprintf(sel_path, sizeof sel_path, "%s", p);
    int dl = 0; /* the folder of the model it belongs to */
    for (const char *q = p; q < strchr(p, '#'); q++)
      if (*q == '/') dl = (int)(q - p);
    snprintf(cur_dir, sizeof cur_dir, "%.*s", dl, p);
  } else if (find_file(p) >= 0) {
    snprintf(sel_path, sizeof sel_path, "%s", p);
    const char *slash = strrchr(p, '/');
    snprintf(cur_dir, sizeof cur_dir, "%.*s", slash ? (int)(slash - p) : 0, p);
  } else if (cw_vfs_is_dir(p)) {
    snprintf(sel_path, sizeof sel_path, "%s", p);
    snprintf(cur_dir, sizeof cur_dir, "%s", p);
  } else {
    return cwed_fail(c, "no file or folder '%s' (fs.list shows the project)", p);
  }
  view_dirty = true;
  cwed_json_key(cwed_reply(c), "selected");
  cwed_json_str(cwed_reply(c), sel_path);
  cwed_json_key(cwed_reply(c), "folder");
  cwed_json_str(cwed_reply(c), cur_dir);
  return true;
}

static bool filter_exec(cwed_call *c) {
  const char *type = cwed_arg(c, "type");
  bool ok = false;
  for (size_t i = 0; i < CW_ARRAY_LEN(TYPES); i++) ok |= !strcmp(type, TYPES[i]);
  if (!ok) return cwed_fail(c, "type is one of all scene model texture audio font text other");
  snprintf(filter_text, sizeof filter_text, "%s", cwed_arg(c, "text"));
  snprintf(filter_type, sizeof filter_type, "%s", type);
  syncing = true;
  if (strcmp(cw_line_edit_text(filter_edit), filter_text)) cw_line_edit_set_text(filter_edit, filter_text);
  for (size_t i = 0; i < CW_ARRAY_LEN(TYPES); i++)
    if (!strcmp(TYPES[i], filter_type)) cw_option_button_select(type_opt, (int)i);
  syncing = false;
  ensure_scan();
  view_dirty = true;
  int n = 0;
  for (int i = 0; i < files.count; i++) n += passes_filter(&files.data[i]);
  cwed_json_key(cwed_reply(c), "matches");
  cwed_json_printf(cwed_reply(c), "%d", n);
  return true;
}

/* where a file's cell is on screen (UI units): for pointer-driven tests */
static bool cell_exec(cwed_call *c) {
  if (view_dirty) rebuild_view();
  cw_ui_layout(cwed_ui_tree());
  const char *p = cw_intern(cwed_arg(c, "path"));
  for (int i = 0; i < grid_files.count; i++)
    if (grid_files.data[i] == p) {
      cw_rect g = cw_control_global_rect(grid);
      cw_rect r = cell_rect(i, g.w);
      cw_rect s = cw_control_global_rect(grid_scroll);
      float cx = g.x + r.x + r.w * 0.5f, cy = g.y + r.y + THUMB * 0.5f + 4;
      bool vis = cw_control_visible_in_tree(grid) && cy >= s.y && cy <= s.y + s.h;
      cwed_json *j = cwed_reply(c);
      cwed_json_key(j, "x");
      cwed_json_printf(j, "%d", (int)cx);
      cwed_json_key(j, "y");
      cwed_json_printf(j, "%d", (int)cy);
      cwed_json_key(j, "visible");
      cwed_json_raw(j, vis ? "true" : "false");
      return true;
    }
  return cwed_fail(c, "%s is not in the thumbnail grid (fs.select its folder first)", p);
}

/* ── drops: files onto the scene tree and scene views ──────────────────── */

static bool can_drop_file(cw_node target, const cw_ui_drag *d, cw_vec2 local, cw_rect *hl, void *user) {
  (void)target, (void)local, (void)hl, (void)user;
  if (!cwed_doc_active() || !d->text) return false;
  const char *t = type_of(d->text);
  return !strcmp(t, "model") || !strcmp(t, "scene") || !strcmp(t, "mesh");
}
static void drop_file(cw_node target, const cw_ui_drag *d, cw_vec2 local, void *user) {
  (void)target, (void)local;
  cwed_doc *doc = cwed_doc_active();
  const char *parent = ".";
  if (user && cwed_selection_count()) parent = cwed_doc_node_path(doc, cwed_selection_get(0)); /* the scene tree: under the selection */
  cwed_runf("fs.place file=%s parent=%s", cwed_quote(d->text), cwed_quote(parent));
}

static const char *DROP_PANELS[] = { "scene_tree", "scene_text", "viewport3d", "viewport2d" };
static void register_drops(void) {
  for (size_t i = 0; i < CW_ARRAY_LEN(DROP_PANELS); i++) {
    cw_node n = cwed_panel_content(DROP_PANELS[i]);
    if (cw_node_valid(n))
      cwed_drop_add(n, &(cwed_drop_desc){ .kind = "file", .can_drop = can_drop_file, .drop = drop_file,
                                          .user = i == 0 ? (void *)1 : NULL });
  }
}

/* ── module ───────────────────────────────────────────────────────────── */

static void on_event(const cwed_event *e, void *user) {
  (void)user;
  if (e->type == CWED_EV_PROJECT) {
    cw_vfs_rescan();
    need_scan = true;
    sel_path[0] = cur_dir[0] = 0;
  }
  if (e->type & (CWED_EV_DOC_SAVED | CWED_EV_PROJECT)) need_scan = true;
  view_dirty = true;
}

/* dragging a file row out of the tree (the Tree control selects on press;
 * a move of a few units with the button held starts the "file" drag) */
static void tree_drag(void) {
  static bool down;
  static cw_vec2 at;
  const cw_raw_input *in = cw_input_raw();
  bool held = in && (in->mouse_buttons & 1);
  cw_vec2 p = cw_ui_pointer();
  if (!held) { down = false; return; }
  if (!down) {
    down = true;
    at = p;
    if (cw_ui_hovered().id != tree.id) at.x = -1e9f; /* the press began elsewhere */
    return;
  }
  if (at.x < -1e8f || cw_ui_drag_current()) return;
  float dx = p.x - at.x, dy = p.y - at.y;
  if (dx * dx + dy * dy < 64) return;
  at.x = -1e9f;
  int item = cw_uitree_selected(tree);
  int64_t row = item >= 0 ? cw_uitree_metadata(tree, item) : -1;
  if (row < 0 || row >= tree_rows.count || find_file(tree_rows.data[row]) < 0) return;
  const char *path = tree_rows.data[row], *base = strrchr(path, '/');
  cw_ui_drag_begin(&(cw_ui_drag){ .source = tree, .kind = "file", .text = path, .label = base ? base + 1 : path });
}

static void frame(double dt) {
  (void)dt;
  static int frames;
  tree_drag();
  if (frames++ == 0 || frames % 60 == 0) register_drops();
#if !CW_CART
  /* other programs may change the project directory: rescan now and then */
  if (cw_vfs_root() && frames % 120 == 0) need_scan = true;
#endif
  uint32_t before = scanned_gen;
  int scans = scan_count;
  ensure_scan();
  if (scan_count != scans && before) {
    /* changed files get new thumbnails */
    for (int i = 0; i < thumbs.count; i++) thumbs.data[i].ready = false;
  }
  if (view_dirty) rebuild_view();
  /* thumbnails still rendering: ask again every few frames; every second
   * ask for all shown ones (the previews module may have evicted one) */
  if (++poll_counter % 8 == 0) {
    bool all = poll_counter % 64 == 0;
    if (cw_control_visible_in_tree(grid))
      for (int i = 0; i < grid_files.count; i++) {
        const char *ty = type_of(grid_files.data[i]);
        if (strcmp(ty, "model") && strcmp(ty, "texture") && strcmp(ty, "scene") && strcmp(ty, "mesh") && strcmp(ty, "material"))
          continue; /* no preview: the type label */
        thumb *t = thumb_for(grid_files.data[i]);
        if (all || (!t->ready && !t->error)) thumb_poll(t);
      }
  }
}

static void init(void) {
  cw_node_type_register(&grid_type);
  grid_t = cw_node_type_find("EditorFileGrid");
  cw_ui_class_register("EditorFileGrid", &(cw_ui_class){ .theme_type = "Label", .min_size = grid_min, .layout = grid_layout,
                                                        .draw = grid_draw, .input = grid_input });
  cwed_panel_register(&(cwed_panel_desc){ .id = "filesystem", .title = "FileSystem", .slot = "left_bottom", .order = 10, .create = create });
  cwed_subscribe(CWED_EV_PROJECT | CWED_EV_DOC_SAVED | CWED_EV_FILES, on_event, NULL);

  static const cwed_param list_p[] = {
    { "dir", CWED_ARG_STRING, "", "folder (\"\" = the project root)" },
    { "recursive", CWED_ARG_BOOL, "true", "include sub-folders" },
    { "filtered", CWED_ARG_BOOL, "false", "only files passing the panel's filter" },
    { 0 } };
  static const cwed_param path_p[] = { { "path", CWED_ARG_STRING, NULL, "project path of a file" }, { 0 } };
  static const cwed_param sel_p[] = { { "path", CWED_ARG_STRING, NULL, "file or folder (\".\" = the project root)" }, { 0 } };
  static const cwed_param filter_p[] = {
    { "text", CWED_ARG_STRING, "", "substring of the path (case-insensitive)" },
    { "type", CWED_ARG_STRING, "all", "all scene model texture audio font text other" },
    { 0 } };
  static const cwed_param rename_p[] = {
    { "from", CWED_ARG_STRING, NULL, "file or folder to move" },
    { "to", CWED_ARG_STRING, "", "its new project path" },
    { "dir", CWED_ARG_STRING, "", "or: the folder to move it into, keeping its name" },
    { 0 } };
  static const cwed_param move_p[] = {
    { "from", CWED_ARG_STRING, NULL, "file or folder to move" },
    { "dir", CWED_ARG_STRING, NULL, "folder to move it into (\"\" = the root)" },
    { "to", CWED_ARG_STRING, "", "(unused: fs.rename takes a full path)" },
    { 0 } };
  static const cwed_param del_p[] = {
    { "path", CWED_ARG_STRING, NULL, "file or folder" },
    { "force", CWED_ARG_BOOL, "false", "delete even when other files use it" },
    { 0 } };
  static const cwed_param imp_p[] = {
    { "src", CWED_ARG_STRING, NULL, "a file outside the project (native path)" },
    { "dest", CWED_ARG_STRING, NULL, "its project path" },
    { 0 } };
  static const cwed_param place_p[] = {
    { "file", CWED_ARG_STRING, NULL, "a model (.gltf .glb) or scene (.cwscene) in the project" },
    { "parent", CWED_ARG_PATH, ".", "node to put it under" },
    { "position", CWED_ARG_VALUE, "vec3(0, 0, 0)", "where (parent space)" },
    { "name", CWED_ARG_STRING, "", "node name (default: the file's name)" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "fs.list", .title = "List Project Files", .params = list_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The project's files over every layer (disk, memory, bundled assets): path, type, size, layer, uid.", .exec = list_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.deps", .title = "File Dependencies (What Uses This?)", .params = path_p,
    .flags = CWED_CMD_QUERY,
    .doc = "What a file uses (scenes: sub-scenes and resources; .gltf: buffers and images) and what uses it.", .exec = deps_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.refresh", .title = "Rescan Project Files",
    .doc = "Rescan the project (the editor also rescans on every change it makes).", .exec = refresh_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.select", .title = "Select File", .params = sel_p,
    .flags = CWED_CMD_HIDDEN, .doc = "Select a file or folder in the FileSystem panel (the grid shows its folder).",
    .exec = select_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.filter", .title = "Filter Files", .params = filter_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "Filter the FileSystem panel by path text and type.", .exec = filter_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.cell", .title = "File Cell Position", .params = path_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Where a file's thumbnail is in the FileSystem grid (UI units, its centre): for pointer-driven tests.",
    .exec = cell_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.rename", .title = "Rename / Move File", .params = rename_p,
    .flags = CWED_CMD_UNDO, .example = "fs.rename from=props/crate.cwscene to=props/crate_moved.cwscene",
    .doc = "Move a file or folder. Its UID is kept (a .cwimport sidecar moves along, or one is written), scenes that use it "
           "are rewritten (ext_scene paths, sub-resource UIDs), .gltf files' URIs follow; open scenes that change are "
           "reloaded (refused while they have unsaved changes). One undo step.",
    .exec = rename_exec, .undo = ftx_undo, .free_state = ftx_free });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.move", .title = "Move File to Folder", .params = move_p,
    .flags = CWED_CMD_UNDO | CWED_CMD_HIDDEN, .example = "fs.move from=textures/grid.png dir=props",
    .doc = "fs.rename into another folder, keeping the name.", .exec = rename_exec, .undo = ftx_undo, .free_state = ftx_free });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.delete", .title = "Delete File", .params = del_p,
    .flags = CWED_CMD_UNDO, .example = "fs.delete path=textures/grid.png",
    .doc = "Delete a file (with its .cwimport sidecar) or folder. Refused while another file uses it (the error names it); "
           "force=true deletes anyway. Undo brings the bytes back.",
    .exec = delete_exec, .undo = ftx_undo, .free_state = ftx_free });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.import_file", .title = "Import File into Project...", .params = imp_p,
    .flags = CWED_CMD_HIDDEN,
    .doc = "Native editor: copy a file from outside the project into it (a cart takes files with `vfs import`); "
           "fs.delete takes it out again.",
    .exec = import_file_exec, .free_state = ftx_free });
  cwed_command_register(&(cwed_command_desc){ .name = "fs.place", .title = "Place File in Scene", .params = place_p,
    .flags = CWED_CMD_UNDO | CWED_CMD_SCENE, .example = "fs.place file=props/crate.cwscene parent=Props position=\"vec3(0, 0.5, 2)\"",
    .doc = "Put a project file into the open scene: a model becomes a MeshInstance3D drawing it (mesh = its UID), a scene "
           "a linked instance. Dropping a file on the scene tree or a scene view runs this. One undo step.",
    .exec = place_exec, .undo = place_undo, .free_state = place_free });
  cwed_menu_add("Scene/Place Selected File", "fs.place", 120);
  cwed_menu_add("View/Rescan Project Files", "fs.refresh", 500);
}

CWED_MODULE(filesystem) = { .title = "FileSystem", .init = init, .frame = frame };

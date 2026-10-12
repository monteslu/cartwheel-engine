/* cwpack: import a project's referenced assets and pack them.
 *
 *   cwpack --manifest roots.txt --out DIR [--pak FILE] [--cache DIR] [--name NAME]
 *          [--set key=value]... [--depfile FILE --dep-target T] [--stats FILE]
 *          [--engine-root DIR] [-q]
 *   cwpack --po-export catalog.cwtr [--po-source en.cwtr]   PO text to stdout
 *   cwpack --po-import file.po                              canonical .cwtr to stdout
 *   cwpack --cwtr-check catalog.cwtr...                     validate (exit 1 on an error)
 *
 * roots.txt, one entry per line ('#' comments):
 *   set <key> <value...>          a project-wide import setting
 *   <res path>                    a root: the file at that path beside roots.txt
 *   <res path> = <source path>    a root whose source lives elsewhere
 *                                 (relative to roots.txt; "@engine/..." is
 *                                 relative to --engine-root, the engine
 *                                 checkout cw_add_app passes: engine-provided
 *                                 files such as @engine/deps/fonts/x.ttf)
 * Starting from the roots, cwpack follows references and packs only what is
 * reachable: a glTF's external images (its external buffers are folded into
 * the imported GLB); a .cwscene's [ext_scene path=...] sub-scenes and every
 * res(0x<uid>) value (the file holding that UID: a sidecar uid, a path's
 * FNV-1a 64 hash, or a glTF sub-resource's, found under roots.txt's
 * directory). So a main scene alone can be the manifest. Per-file settings
 * and the UID come from a sidecar (key = value lines; uid = 0x...):
 * "<res path>.cwimport" beside roots.txt, else "<source>.cwimport".
 *
 * Imports are cached in .cwcache/ beside roots.txt (or --cache) by (importer and its
 * version, the settings it reads, every input byte), so a rerun only imports
 * what changed. Outputs, all byte-identical for identical input:
 *   DIR/assets/...       the packed files (a cart's asset dir: cw_add_app
 *                        ASSETS, wasmcart-pack --assets) plus res.index
 *   DIR/manifest.json    wasmcart cart-directory manifest with the `files`
 *                        list (lazy loading over HTTP), entry cart.wasm
 *   --pak FILE           the native .cwpak (src/resource/cwpak.h)
 *   DIR/sources.txt      every file read (hot reload watches these)
 *   --depfile FILE       a Make depfile for the build system
 *   --stats FILE         JSON: which files imported, which came from cache
 * Localization (cartwheel/l10n.h): a .cwl10n project's catalogs are
 * references; a .cwtr catalog is validated and written canonical (its source
 * may be a gettext .po: "l10n/ja.cwtr = po/ja.po" imports it), its @font
 * files are references; the project's per-locale assets
 * (<asset_dir>/<locale>/<asset>) are packed for every catalog locale that has
 * one. A font whose settings say font.subset = l10n keeps only the glyphs
 * for printable ASCII, font.subset_chars and every character of every packed
 * catalog (import_font.c).
 *
 * Exit status 1 with a message naming the file on any missing reference or
 * failed import.
 *
 *   cwpack --raw-dir DIR --pak FILE [-q]
 *
 * packs every file under DIR byte for byte (no import, no res.index; names
 * starting with '.' skipped): the .cwpak of an app that reads loose files
 * (cw_add_app ASSETS), which an export ships in place of the directory. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SDEFL_IMPLEMENTATION
#include "sdefl.h"

#include "cartwheel/resource.h"
#include "cwpack.h"
#include "cwpak.h"
#include "gltf_doc.h"
#include "json.h"
#include "resource_internal.h"
#include "catalog.h"

void cw__platform_log(cw_log_level level, const char *msg) {
  if (level >= CW_LOG_WARN) fprintf(stderr, "cwpack: %s\n", msg);
}

typedef CW_ARRAY(char *) str_list;
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct entry {
  char *res, *src, *type;
  uint8_t *data;
  size_t len, src_len;
  uint64_t uid;
  bool cached;
  char *note;
  str_list deps; /* a scene's direct references (res paths, first-use order) */
  CW_ARRAY(char *) subs; /* "mesh/0/0 mesh" ...: a glTF's sub-resources, for res.index */
  char *subset;          /* font.subset (fonts subset after every catalog is in) */
  char *subset_chars;    /* font.subset_chars */
  char *settings_canon;  /* what the deferred import depends on */
} entry;

static struct {
  entry *e;
  int n, cap;
  CW_ARRAY(char *) sources; /* absolute */
  cwp_settings project;
  char cache[CWP_PATH_MAX + 16];
  char mdir[CWP_PATH_MAX];
  bool quiet;
  int imported, from_cache;
  /* the manifest's roots (res path -> source), and the UID table scene
   * references resolve through (built on first use) */
  CW_ARRAY(char *) root_res;
  CW_ARRAY(char *) root_src;
  cw_map uid_paths; /* uid -> index into uid_names */
  CW_ARRAY(char *) uid_names;
  bool uid_built;
} P;

static void add_source(const char *path) {
  char a[CWP_PATH_MAX];
  cwp_absolute(path, a, sizeof a);
  for (int i = 0; i < P.sources.count; i++)
    if (!strcmp(P.sources.data[i], a)) return;
  cw_array_push(&P.sources, cw_strdup(a, CW_MEM_GENERAL));
}

static const char *type_of(const char *res) {
  const char *dot = strrchr(res, '.'), *slash = strrchr(res, '/');
  if (!dot || (slash && dot < slash)) return "binary";
  char e[16];
  size_t n = strlen(dot);
  if (n >= sizeof e) return "binary";
  for (size_t i = 0; i <= n; i++) e[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
  /* the same table as the engine's built-in types (src/resource) */
  static const struct { const char *ext, *type; } map[] = {
    { ".png", "texture" }, { ".jpg", "texture" }, { ".jpeg", "texture" }, { ".ktx2", "texture" }, { ".gif", "texture" },
    { ".glb", "gltf" }, { ".gltf", "gltf" },
    { ".wav", "audio_clip" }, { ".ogg", "audio_clip" }, { ".opus", "audio_clip" }, { ".mp3", "audio_clip" },
    { ".flac", "audio_clip" }, { ".xm", "audio_clip" }, { ".mod", "audio_clip" }, { ".s3m", "audio_clip" }, { ".it", "audio_clip" },
    { ".ttf", "font" }, { ".otf", "font" }, { ".ttc", "font" },
    { ".json", "json" },
    { ".txt", "text" }, { ".csv", "text" }, { ".md", "text" }, { ".cfg", "text" }, { ".ini", "text" },
    { ".cwproj", "text" }, { ".cwscene", "text" }, /* key = value lines / scene text (scene_file.h) */
    { ".glsl", "text" }, { ".vert", "text" }, { ".frag", "text" },
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(map); i++)
    if (!strcmp(e, map[i].ext)) return map[i].type;
  return "binary";
}

static entry *find_entry(const char *res) {
  for (int i = 0; i < P.n; i++)
    if (!strcmp(P.e[i].res, res)) return &P.e[i];
  return NULL;
}

/* ── the import cache ────────────────────────────────────────────── */
static uint8_t *cache_get(uint64_t key, const char *imp, size_t *len) {
  char p[CWP_PATH_MAX + 64];
  snprintf(p, sizeof p, "%s/%s-%016llx.bin", P.cache, imp, (unsigned long long)key);
  return cwp_read_file(p, len);
}
static void cache_put(uint64_t key, const char *imp, const uint8_t *d, size_t len) {
  char p[CWP_PATH_MAX + 64];
  snprintf(p, sizeof p, "%s/%s-%016llx.bin", P.cache, imp, (unsigned long long)key);
  if (!cwp_write_file(p, d, len)) fprintf(stderr, "cwpack: warning: cannot write the cache file %s\n", p);
}

/* inputs a glTF import reads besides itself */
typedef struct { char **uris; uint8_t **bytes; size_t *lens; int n; } extra_files;
static void *read_extra(void *ctx, const char *uri, size_t *len) {
  extra_files *x = ctx;
  for (int i = 0; i < x->n; i++)
    if (!strcmp(x->uris[i], uri)) {
      uint8_t *c = cw_alloc(x->lens[i] + 1, CW_MEM_RESOURCE);
      memcpy(c, x->bytes[i], x->lens[i]);
      *len = x->lens[i];
      return c;
    }
  return NULL;
}

static bool is_external(const char *uri) { return uri && strncmp(uri, "data:", 5); }

/* ── scenes: their references are dependencies ───────────────────── */
/* where a res path's source is: a manifest root's mapping, else beside roots.txt */
static void source_of(const char *res, char *out, size_t cap) {
  for (int i = 0; i < P.root_res.count; i++)
    if (!strcmp(P.root_res.data[i], res)) { snprintf(out, cap, "%s", P.root_src.data[i]); return; }
  cwp_join(P.mdir, res, out, cap);
}

static void uid_add(const char *res, uint64_t uid) {

  uint64_t v;
  if (cw_map_get(&P.uid_paths, uid, &v)) return; /* first wins: roots, then the scan */
  cw_map_set(&P.uid_paths, uid, (uint64_t)P.uid_names.count);
  cw_array_push(&P.uid_names, cw_strdup(res, CW_MEM_GENERAL));
}

/* a file's UID (sidecar or path hash), and a glTF's sub-resources' */
static void uid_scan_file(const char *res, const char *src) {
  char side[CWP_PATH_MAX + 16];
  uint64_t uid = cw_res_path_uid(res);
  size_t n;
  snprintf(side, sizeof side, "%s%s.cwimport", P.mdir, res);
  char *sc = (char *)cwp_read_file(side, &n);
  if (!sc) { snprintf(side, sizeof side, "%s.cwimport", src); sc = (char *)cwp_read_file(side, &n); }
  if (sc) {
    char *u = strstr(sc, "uid");
    char *eq = u ? strchr(u, '=') : NULL;
    if (eq) uid = strtoull(eq + 1, NULL, 0);
    cw_free(sc);
  }
  uid_add(res, uid);
  const char *t = type_of(res);
  if (strcmp(t, "gltf")) return;
  uint8_t *b = cwp_read_file(src, &n);
  const char *json;
  size_t jl, bl;
  const uint8_t *bin;
  cwj *r = b && cw_glb_split(b, n, &json, &jl, &bin, &bl) ? cwj_parse(json, jl, NULL, 0) : NULL;
  char sub[CWP_PATH_MAX + 64];
  cwj *ms = cwj_get(r, "meshes"), *mats = cwj_get(r, "materials"), *txs = cwj_get(r, "textures");
  for (int m = 0; ms && m < ms->count; m++) {
    cwj *ps = cwj_get(ms->items[m], "primitives");
    for (int q = 0; ps && q < ps->count; q++) {
      snprintf(sub, sizeof sub, "%s#mesh/%d/%d", res, m, q);
      uid_add(sub, cw_res_path_uid(sub));
    }
  }
  for (int i = 0; mats && i < mats->count; i++) { snprintf(sub, sizeof sub, "%s#material/%d", res, i); uid_add(sub, cw_res_path_uid(sub)); }
  for (int i = 0; txs && i < txs->count; i++) { snprintf(sub, sizeof sub, "%s#texture/%d", res, i); uid_add(sub, cw_res_path_uid(sub)); }
  cwj_free(r);
  cw_free(b);
}

static void uid_scan_dir(const char *rel) {
  char path[CWP_PATH_MAX * 2];
  snprintf(path, sizeof path, "%s%s", P.mdir, rel);
  DIR *d = opendir(path[0] ? path : ".");
  if (!d) return;
  str_list names = { 0 };
  struct dirent *de;
  while ((de = readdir(d)))
    if (de->d_name[0] != '.') cw_array_push(&names, cw_strdup(de->d_name, CW_MEM_GENERAL));
  closedir(d);
  qsort(names.data, (size_t)names.count, sizeof *names.data, cmp_str); /* the same order on every machine */
  for (int i = 0; i < names.count; i++) {
    char r2[CWP_PATH_MAX], full[CWP_PATH_MAX * 2];
    snprintf(r2, sizeof r2, "%s%s", rel, names.data[i]);
    snprintf(full, sizeof full, "%s%s", P.mdir, r2);
    struct stat st;
    if (!stat(full, &st)) {
      if (S_ISDIR(st.st_mode)) {
        char sub[CWP_PATH_MAX + 2];
        snprintf(sub, sizeof sub, "%s/", r2);
        uid_scan_dir(sub);
      } else if (!strstr(r2, ".cwimport") && strcmp(r2, "roots.txt")) {
        uid_scan_file(r2, full);
      }
    }
    cw_free(names.data[i]);
  }
  cw_array_free(&names);
}

static const char *uid_lookup(uint64_t uid) {
  if (!P.uid_built) {
    P.uid_built = true;
    cw_map_init(&P.uid_paths, CW_MEM_GENERAL);
    for (int i = 0; i < P.root_res.count; i++) uid_scan_file(P.root_res.data[i], P.root_src.data[i]);
    uid_scan_dir("");
  }
  uint64_t v;
  return cw_map_get(&P.uid_paths, uid, &v) ? P.uid_names.data[v] : NULL;
}

/* [ext_scene path="..."] and res(0x...) values: what a scene needs packed */
static void add_dep(str_list *deps, const char *p) {
  for (int i = 0; i < deps->count; i++)
    if (!strcmp(deps->data[i], p)) return;
  cw_array_push(deps, cw_strdup(p, CW_MEM_GENERAL));
}

static bool scene_refs(const char *res, const uint8_t *text, size_t len, str_list *qr, str_list *qs, str_list *deps) {
  bool ok = true;
  char *t = cw_alloc(len + 1, CW_MEM_GENERAL);
  memcpy(t, text, len);
  t[len] = 0;
  int lineno = 0;
  for (char *line = t; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    lineno++;
    char dep[CWP_PATH_MAX] = "";
    if (!strncmp(line, "[ext_scene", 10) || !strncmp(line, "[ext_resource", 13)) {
      const char *p = strstr(line, "path=\"");
      if (p) {
        p += 6;
        const char *e = strchr(p, '"');
        if (e) snprintf(dep, sizeof dep, "%.*s", (int)(e - p), p);
      }
    }
    for (const char *r = line; ok && (r = strstr(r, "res(")); r += 4) {
      char *end;
      uint64_t uid = strtoull(r + 4, &end, 0);
      if (end == r + 4 || !uid) continue;
      const char *path = uid_lookup(uid);
      if (!path) {
        fprintf(stderr, "cwpack: res://%s:%d: res(0x%016llx) matches no file under %s (a sidecar uid, or a path's hash)\n", res, lineno,
                (unsigned long long)uid, P.mdir[0] ? P.mdir : ".");
        ok = false;
        break;
      }
      char file[CWP_PATH_MAX], src[CWP_PATH_MAX];
      snprintf(file, sizeof file, "%s", path);
      char *hash = strchr(file, '#');
      if (hash) *hash = 0; /* a sub-resource: its file */
      source_of(file, src, sizeof src);
      add_dep(deps, file);
      cw_array_push(qr, cw_strdup(file, CW_MEM_GENERAL));
      cw_array_push(qs, cw_strdup(src, CW_MEM_GENERAL));
    }
    if (dep[0]) {
      char c[CWP_PATH_MAX], src[CWP_PATH_MAX];
      if (!cw__res_canon(dep, c, sizeof c)) {
        fprintf(stderr, "cwpack: res://%s:%d: path \"%s\" leaves the project root\n", res, lineno, dep);
        ok = false;
      } else {
        source_of(c, src, sizeof src);
        add_dep(deps, c);
        cw_array_push(qr, cw_strdup(c, CW_MEM_GENERAL));
        cw_array_push(qs, cw_strdup(src, CW_MEM_GENERAL));
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  cw_free(t);
  return ok;
}


/* localization helpers (below) */
static bool rl_is(const char *path, const char *ext);
static bool enqueue_list(const char *res, const char *list, str_list *qr, str_list *qs, str_list *deps);
static bool l10n_project_refs(const char *res, const char *text, size_t len, str_list *qr, str_list *qs, str_list *deps);

/* ── one file ────────────────────────────────────────────────────── */
static bool process(const char *res, const char *src, const char *referrer, str_list *queue_res, str_list *queue_src) {
  size_t len;
  uint8_t *bytes = cwp_read_file(src, &len);
  if (!bytes) {
    if (referrer) fprintf(stderr, "cwpack: res://%s: source %s not found (referenced by res://%s)\n", res, src, referrer);
    else fprintf(stderr, "cwpack: res://%s: source %s not found (a root in the manifest)\n", res, src);
    return false;
  }
  add_source(src);
  entry *e = &P.e[P.n++];
  memset(e, 0, sizeof *e);
  e->res = cw_strdup(res, CW_MEM_GENERAL);
  e->src = cw_strdup(src, CW_MEM_GENERAL);
  e->type = (char *)type_of(res);
  e->src_len = len;
  /* settings: project, then the sidecar */
  cwp_settings set = { 0 };
  cwp_settings_copy(&set, &P.project);
  /* the sidecar: beside the manifest at <res path>.cwimport (a project-side
   * override, for sources outside the project), else beside the source */
  char side[CWP_PATH_MAX + 16];
  snprintf(side, sizeof side, "%s%s.cwimport", P.mdir, res);
  if (!cwp_exists(side)) snprintf(side, sizeof side, "%s.cwimport", src);
  e->uid = cw_res_path_uid(res);
  size_t sl;
  char *sc = (char *)cwp_read_file(side, &sl);
  if (sc) {
    add_source(side);
    for (char *line = strtok(sc, "\n"); line; line = strtok(NULL, "\n")) {
      char k[128], v[1024];
      if (line[0] == '#' || sscanf(line, " %127[^= ] = %1023[^\n]", k, v) != 2) continue;
      size_t vl = strlen(v);
      while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) v[--vl] = 0;
      if (!strcmp(k, "uid")) e->uid = strtoull(v, NULL, 0);
      else cwp_settings_set(&set, k, v);
    }
    cw_free(sc);
  }
  char sdir[CWP_PATH_MAX], rdir[CWP_PATH_MAX];
  cwp_dirname(src, sdir, sizeof sdir);
  cwp_dirname(res, rdir, sizeof rdir);
  bool ok = true;
  const char *imp;
  char *canon;
  uint64_t key;
  if (!strcmp(e->type, "gltf")) {
    imp = "gltf";
    static const char *pre[] = { "mesh.", "texture." };
    canon = cwp_settings_canon(&set, pre, 2);
    key = cw_hash_bytes("gltf", 4, CWP_IMPORT_GLTF_VERSION);
    key = cw_hash_bytes(canon, strlen(canon), key);
    key = cw_hash_bytes(bytes, len, key);
    /* references: buffers are inputs, images become their own entries */
    extra_files x = { 0 };
    const char *json;
    size_t jl, bl;
    const uint8_t *bin;
    char err[200];
    cwj *root = cw_glb_split(bytes, len, &json, &jl, &bin, &bl) ? cwj_parse(json, jl, err, sizeof err) : NULL;
    if (!root) { fprintf(stderr, "cwpack: res://%s: not a glTF 2.0 file\n", res); ok = false; }
    cwj *bufs = cwj_get(root, "buffers"), *imgs = cwj_get(root, "images");
    for (int i = 0; ok && bufs && i < bufs->count; i++) {
      const char *u = cwj_string(bufs->items[i], "uri");
      if (!is_external(u)) continue;
      char dec[CWP_PATH_MAX], full[CWP_PATH_MAX];
      cwp_uri_decode(u, dec, sizeof dec);
      cwp_join(sdir, dec, full, sizeof full);
      size_t n;
      uint8_t *b = cwp_read_file(full, &n);
      if (!b) { fprintf(stderr, "cwpack: res://%s: buffer \"%s\" not found (%s)\n", res, u, full); ok = false; break; }
      add_source(full);
      x.uris = cw_realloc(x.uris, (size_t)(x.n + 1) * sizeof *x.uris, CW_MEM_GENERAL);
      x.bytes = cw_realloc(x.bytes, (size_t)(x.n + 1) * sizeof *x.bytes, CW_MEM_GENERAL);
      x.lens = cw_realloc(x.lens, (size_t)(x.n + 1) * sizeof *x.lens, CW_MEM_GENERAL);
      x.uris[x.n] = cw_strdup(dec, CW_MEM_GENERAL);
      x.bytes[x.n] = b;
      x.lens[x.n++] = n;
      key = cw_hash_bytes(dec, strlen(dec), key);
      key = cw_hash_bytes(b, n, key);
    }
    for (int i = 0; ok && imgs && i < imgs->count; i++) {
      const char *u = cwj_string(imgs->items[i], "uri");
      if (!is_external(u)) continue;
      char dec[CWP_PATH_MAX], rp[CWP_PATH_MAX], sp[CWP_PATH_MAX], c[CWP_PATH_MAX];
      cwp_uri_decode(u, dec, sizeof dec);
      cwp_join(rdir, dec, rp, sizeof rp);
      cwp_join(sdir, dec, sp, sizeof sp);
      if (!cw__res_canon(rp, c, sizeof c)) { fprintf(stderr, "cwpack: res://%s: image \"%s\" leaves the project root\n", res, u); ok = false; break; }
      cw_array_push(queue_res, cw_strdup(c, CW_MEM_GENERAL));
      cw_array_push(queue_src, cw_strdup(sp, CW_MEM_GENERAL));
    }
    cwj_free(root);
    if (ok) {
      e->data = cache_get(key, imp, &e->len);
      e->cached = e->data != NULL;
      if (!e->data) {
        cwp_gltf_stats st;
        e->data = cwp_import_gltf(res, bytes, len, read_extra, &x, &set, &e->len, &st);
        if (!e->data) ok = false;
        else {
          cache_put(key, imp, e->data, e->len);
          char note[256];
          snprintf(note, sizeof note, "%d/%d prims optimized, verts %d->%d, %d LODs (%d tris -> %d), %d compressed views, %d KTX2",
                   st.prims_optimized, st.prims, st.vertices_in, st.vertices_out, st.lods, st.triangles, st.lod_triangles,
                   st.compressed_views, st.images_ktx2);
          e->note = cw_strdup(note, CW_MEM_GENERAL);
        }
      }
    }
    for (int i = 0; i < x.n; i++) { cw_free(x.uris[i]); cw_free(x.bytes[i]); }
    cw_free(x.uris);
    cw_free(x.bytes);
    cw_free(x.lens);
    if (ok) { /* sub-resources the engine registers: res.index lists them so a UID finds them */
      const char *json2;
      size_t jl2, bl2;
      const uint8_t *bin2;
      cwj *r2 = cw_glb_split(e->data, e->len, &json2, &jl2, &bin2, &bl2) ? cwj_parse(json2, jl2, NULL, 0) : NULL;
      cwj *ms = cwj_get(r2, "meshes");
      char sub[64];
      for (int m = 0; ms && m < ms->count; m++) {
        cwj *ps = cwj_get(ms->items[m], "primitives");
        for (int q = 0; ps && q < ps->count; q++) {
          snprintf(sub, sizeof sub, "mesh mesh/%d/%d", m, q);
          cw_array_push(&e->subs, cw_strdup(sub, CW_MEM_GENERAL));
        }
      }
      cwj *mats = cwj_get(r2, "materials"), *txs = cwj_get(r2, "textures");
      for (int i = 0; mats && i < mats->count; i++) { snprintf(sub, sizeof sub, "material material/%d", i); cw_array_push(&e->subs, cw_strdup(sub, CW_MEM_GENERAL)); }
      for (int i = 0; txs && i < txs->count; i++) { snprintf(sub, sizeof sub, "texture texture/%d", i); cw_array_push(&e->subs, cw_strdup(sub, CW_MEM_GENERAL)); }
      cwj_free(r2);
    }
  } else if (!strcmp(e->type, "texture")) {
    imp = "texture";
    static const char *pre[] = { "texture." };
    canon = cwp_settings_canon(&set, pre, 1);
    /* a standalone image is color data unless its sidecar says otherwise */
    bool srgb = strcmp(cwp_settings_get(&set, "texture.color", "srgb"), "linear") != 0;
    key = cw_hash_bytes("texture", 7, CWP_IMPORT_TEXTURE_VERSION);
    key = cw_hash_bytes(canon, strlen(canon), key);
    key = cw_hash_bytes(&srgb, 1, key);
    key = cw_hash_bytes(bytes, len, key);
    e->data = cache_get(key, imp, &e->len);
    e->cached = e->data != NULL;
    if (!e->data) {
      e->data = cwp_import_texture(res, bytes, len, &set, srgb, &e->len);
      if (!e->data) ok = false;
      else cache_put(key, imp, e->data, e->len);
    }
  } else if (!strcmp(e->type, "audio_clip")) {
    imp = "audio";
    static const char *pre[] = { "audio." };
    canon = cwp_settings_canon(&set, pre, 1);
    key = cw_hash_bytes("audio", 5, CWP_IMPORT_AUDIO_VERSION);
    key = cw_hash_bytes(canon, strlen(canon), key);
    key = cw_hash_bytes(bytes, len, key);
    e->data = cache_get(key, imp, &e->len);
    e->cached = e->data != NULL;
    if (!e->data) {
      char note[160] = "";
      e->data = cwp_import_audio(res, bytes, len, &set, &e->len, note, sizeof note);
      if (!e->data) ok = false;
      else {
        cache_put(key, imp, e->data, e->len);
        if (note[0]) e->note = cw_strdup(note, CW_MEM_GENERAL);
      }
    }
  } else if (rl_is(res, ".cwtr")) {
    /* a translation catalog: validated, written canonical; PO sources import */
    imp = "l10n";
    canon = cw_strdup("", CW_MEM_GENERAL);
    char err[300];
    bool po = rl_is(src, ".po");
    cwtr_catalog *c = po ? cwtr_parse_po((const char *)bytes, len, res, err, sizeof err) : cwtr_parse((const char *)bytes, len, res, err, sizeof err);
    if (!c) {
      fprintf(stderr, "cwpack: %s\n", err);
      ok = false;
    } else if (!cwtr_field(c, "locale")) {
      fprintf(stderr, "cwpack: res://%s: a catalog needs @locale\n", res);
      ok = false;
    } else {
      e->data = (uint8_t *)cwtr_write(c, &e->len);
      if (po) {
        char note[96];
        snprintf(note, sizeof note, "imported from PO: %d entries", c->entries.count);
        e->note = cw_strdup(note, CW_MEM_GENERAL);
      }
      const char *fonts = cwtr_field(c, "font");
      if (fonts) ok &= enqueue_list(res, fonts, queue_res, queue_src, &e->deps);
    }
    cwtr_free(c);
    e->cached = false;
  } else if (rl_is(res, ".cwl10n")) {
    /* a localization project: its catalogs are references */
    imp = "copy";
    canon = cw_strdup("", CW_MEM_GENERAL);
    e->data = bytes;
    e->len = len;
    bytes = NULL;
    ok = l10n_project_refs(res, (const char *)e->data, e->len, queue_res, queue_src, &e->deps);
  } else if (!strcmp(e->type, "font") && cwp_settings_get(&set, "font.subset", NULL)) {
    /* subset after the walk, when every catalog is known */
    static const char *pre[] = { "font." };
    canon = cwp_settings_canon(&set, pre, 1);
    imp = "font";
    e->subset = cw_strdup(cwp_settings_get(&set, "font.subset", ""), CW_MEM_GENERAL);
    e->subset_chars = cw_strdup(cwp_settings_get(&set, "font.subset_chars", ""), CW_MEM_GENERAL);
    e->settings_canon = cw_strdup(canon, CW_MEM_GENERAL);
    e->data = bytes;
    e->len = len;
    bytes = NULL;
  } else {
    /* everything else is packed as it is (no cache: copying is the import) */
    canon = cw_strdup("", CW_MEM_GENERAL);
    imp = "copy";
    e->data = bytes;
    e->len = len;
    e->cached = false;
    bytes = NULL;
  }
  size_t rl = strlen(res);
  if (ok && rl > 8 && !strcmp(res + rl - 8, ".cwscene")) ok = scene_refs(res, e->data, e->len, queue_res, queue_src, &e->deps);
  cw_free(canon);
  cw_free(bytes);
  cwp_settings_free(&set);
  if (ok && strcmp(imp, "copy") && !e->subset) {
    if (e->cached) P.from_cache++;
    else P.imported++;
  }
  if (ok && !P.quiet && !e->subset)
    printf("  %-7s res://%s (%s, %zu -> %zu bytes)%s%s\n", !strcmp(imp, "copy") ? "copy" : e->cached ? "cached" : "import",
           res, e->type, e->src_len, e->len, e->note ? ": " : "", e->note ? e->note : "");
  return ok;
}

/* ── localization ────────────────────────────────────────────────── */

static bool rl_is(const char *path, const char *ext) {
  size_t a = strlen(path), b = strlen(ext);
  return a >= b && !strcmp(path + a - b, ext);
}

/* a comma list of res paths (a catalog's @font): references */
static bool enqueue_list(const char *res, const char *list, str_list *qr, str_list *qs, str_list *deps) {
  char buf[2048];
  snprintf(buf, sizeof buf, "%s", list);
  bool ok = true;
  for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
    while (*tok == ' ') tok++;
    size_t n = strlen(tok);
    while (n && tok[n - 1] == ' ') tok[--n] = 0;
    if (!*tok) continue;
    char c[CWP_PATH_MAX], src[CWP_PATH_MAX];
    if (!cw__res_canon(tok, c, sizeof c)) {
      fprintf(stderr, "cwpack: res://%s: path \"%s\" leaves the project root\n", res, tok);
      ok = false;
      continue;
    }
    source_of(c, src, sizeof src);
    add_dep(deps, c);
    cw_array_push(qr, cw_strdup(c, CW_MEM_GENERAL));
    cw_array_push(qs, cw_strdup(src, CW_MEM_GENERAL));
  }
  return ok;
}

/* "name = value" lines of a .cwl10n; calls fn per line */
typedef void (*kv_fn)(const char *k, const char *v, void *u);
static void l10n_lines(const char *text, size_t len, kv_fn fn, void *u) {
  char *t = cw_alloc(len + 1, CW_MEM_GENERAL);
  memcpy(t, text, len);
  t[len] = 0;
  for (char *line = t; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *eq = strchr(line, '=');
    if (eq) {
      char *k = line, *v = eq + 1, *ke = eq;
      while (*k == ' ' || *k == '\t') k++;
      while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
      *ke = 0;
      while (*v == ' ' || *v == '\t') v++;
      size_t n = strlen(v);
      while (n && (v[n - 1] == ' ' || v[n - 1] == '\r' || v[n - 1] == '\t')) v[--n] = 0;
      fn(k, v, u);
    }
    line = nl ? nl + 1 : NULL;
  }
  cw_free(t);
}

typedef struct { const char *res; str_list *qr, *qs, *deps; bool ok; } proj_ctx;
static void proj_line(const char *k, const char *v, void *u) {
  proj_ctx *p = u;
  if (!strcmp(k, "catalog")) p->ok &= enqueue_list(p->res, v, p->qr, p->qs, p->deps);
}
static bool l10n_project_refs(const char *res, const char *text, size_t len, str_list *qr, str_list *qs, str_list *deps) {
  proj_ctx p = { res, qr, qs, deps, true };
  l10n_lines(text, len, proj_line, &p);
  return p.ok;
}

typedef struct { char dir[CWP_PATH_MAX]; str_list assets; } asset_ctx;
static void asset_line(const char *k, const char *v, void *u) {
  asset_ctx *a = u;
  if (!strcmp(k, "asset_dir")) snprintf(a->dir, sizeof a->dir, "%s", v);
  else if (!strcmp(k, "asset")) cw_array_push(&a->assets, cw_strdup(v, CW_MEM_GENERAL));
}

/* every codepoint of a UTF-8 string into the set */
typedef CW_ARRAY(uint32_t) u32_list;
static void add_cps(cw_map *set, u32_list *list, const char *s) {
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    uint32_t c = *p;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (n > 1) {
      c &= 0x7fu >> n;
      for (int i = 1; i < n && p[i]; i++) c = (c << 6) | (p[i] & 0x3fu);
    }
    uint64_t v;
    if (!cw_map_get(set, c, &v)) {
      cw_map_set(set, c, 1);
      cw_array_push(list, c);
    }
    p += n;
  }
}
static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

/* depth-first, dependencies before what needs them, each path once */
static void preload_walk(const char *res, str_list *order, int depth) {
  for (int i = 0; i < order->count; i++)
    if (!strcmp(order->data[i], res)) return;
  entry *e = find_entry(res);
  if (e && depth < 64)
    for (int k = 0; k < e->deps.count; k++) preload_walk(e->deps.data[k], order, depth + 1);
  for (int i = 0; i < order->count; i++) /* a cycle may have added it meanwhile */
    if (!strcmp(order->data[i], res)) return;
  cw_array_push(order, cw_strdup(res, CW_MEM_GENERAL));
}

/* ── outputs ─────────────────────────────────────────────────────── */
static int cmp_entry(const void *a, const void *b) { return strcmp(((const entry *)a)->res, ((const entry *)b)->res); }

static bool in_set(const char *rel) {
  for (int i = 0; i < P.n; i++)
    if (!strcmp(P.e[i].res, rel)) return true;
  return false;
}

/* delete files under dir that are not packed any more (wasmcart-pack would
 * otherwise ship them) */
static void prune(const char *root, const char *rel) {
  char path[CWP_PATH_MAX];
  snprintf(path, sizeof path, "%s%s%s", root, rel[0] ? "/" : "", rel);
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    char r2[CWP_PATH_MAX], full[CWP_PATH_MAX * 2];
    snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
    snprintf(full, sizeof full, "%s/%s", root, r2);
    struct stat st;
    if (stat(full, &st)) continue;
    if (S_ISDIR(st.st_mode)) {
      prune(root, r2);
      rmdir(full); /* only succeeds when empty */
    } else if (!in_set(r2)) {
      remove(full);
    }
  }
  closedir(d);
}

static bool write_if_changed(const char *path, const void *data, size_t len) {
  size_t ol;
  uint8_t *old = cwp_read_file(path, &ol);
  bool same = old && ol == len && !memcmp(old, data, len);
  cw_free(old);
  return same || cwp_write_file(path, data, len);
}

static bool write_pak(const char *path, bool deflate) {
  struct sdefl *z = cw_alloc(sizeof *z, CW_MEM_GENERAL);
  cwpak_entry *idx = cw_calloc((size_t)P.n + 1, sizeof *idx, CW_MEM_GENERAL);
  CW_ARRAY(uint8_t) out = { 0 };
  cw_array_reserve(&out, 1 << 20);
  out.count = sizeof(cwpak_header);
  memset(out.data, 0, sizeof(cwpak_header));
  size_t strings = 0;
  for (int i = 0; i < P.n; i++) {
    entry *e = &P.e[i];
    while (out.count % CWPAK_ALIGN) cw_array_push(&out, 0);
    const uint8_t *data = e->data;
    size_t size = e->len;
    uint8_t *zb = NULL;
    uint32_t flags = 0;
    if (deflate && e->len >= 256) {
      zb = cw_alloc((size_t)sdefl_bound((int)e->len), CW_MEM_GENERAL);
      memset(z, 0, sizeof *z);
      int zn = sdeflate(z, zb, e->data, (int)e->len, 5);
      if (zn > 0 && (size_t)zn < e->len - e->len / 10) { data = zb; size = (size_t)zn; flags = CWPAK_DEFLATE; }
    }
    idx[i].uid = e->uid;
    idx[i].offset = (uint64_t)out.count;
    idx[i].size = size;
    idx[i].raw_size = e->len;
    idx[i].path_offset = (uint32_t)strings;
    idx[i].path_len = (uint32_t)strlen(e->res);
    idx[i].flags = flags;
    idx[i].hash = cw_hash32(e->data, e->len, CW_HASH32_INIT);
    strings += strlen(e->res) + 1;
    cw_array_reserve(&out, out.count + (int)size);
    memcpy(out.data + out.count, data, size);
    out.count += (int)size;
    cw_free(zb);
  }
  while (out.count % 8) cw_array_push(&out, 0);
  cwpak_header h = { 0 };
  memcpy(h.magic, CWPAK_MAGIC, 8);
  h.version = CWPAK_VERSION;
  h.count = (uint32_t)P.n;
  h.index_offset = (uint64_t)out.count;
  h.align = CWPAK_ALIGN;
  cw_array_reserve(&out, out.count + (int)(P.n * sizeof *idx));
  memcpy(out.data + out.count, idx, (size_t)P.n * sizeof *idx);
  out.count += (int)((size_t)P.n * sizeof *idx);
  h.strings_offset = (uint64_t)out.count;
  h.strings_size = strings;
  for (int i = 0; i < P.n; i++) {
    size_t n = strlen(P.e[i].res) + 1;
    cw_array_reserve(&out, out.count + (int)n);
    memcpy(out.data + out.count, P.e[i].res, n);
    out.count += (int)n;
  }
  memcpy(out.data, &h, sizeof h);
  bool ok = write_if_changed(path, out.data, (size_t)out.count);
  if (!P.quiet) printf("cwpack: %s: %d entries, %d bytes\n", path, P.n, out.count);
  cw_array_free(&out);
  cw_free(idx);
  cw_free(z);
  return ok;
}

/* --raw-dir: every regular file under root/rel, verbatim */
static bool raw_walk(const char *root, const char *rel) {
  char path[CWP_PATH_MAX];
  snprintf(path, sizeof path, "%s%s%s", root, rel[0] ? "/" : "", rel);
  DIR *d = opendir(path);
  if (!d) { fprintf(stderr, "cwpack: cannot open the directory %s\n", path); return false; }
  struct dirent *de;
  bool ok = true;
  while (ok && (de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char r2[CWP_PATH_MAX], full[CWP_PATH_MAX * 2];
    snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
    snprintf(full, sizeof full, "%s/%s", root, r2);
    struct stat st;
    if (stat(full, &st)) { fprintf(stderr, "cwpack: cannot stat %s\n", full); ok = false; break; }
    if (S_ISDIR(st.st_mode)) { ok = raw_walk(root, r2); continue; }
    if (!S_ISREG(st.st_mode)) continue;
    if (P.n == P.cap) {
      P.cap = P.cap ? P.cap * 2 : 64;
      P.e = cw_realloc(P.e, (size_t)P.cap * sizeof *P.e, CW_MEM_GENERAL);
    }
    entry *e = &P.e[P.n];
    memset(e, 0, sizeof *e);
    e->data = cwp_read_file(full, &e->len);
    if (!e->data && st.st_size) { fprintf(stderr, "cwpack: cannot read %s\n", full); ok = false; break; }
    e->res = cw_strdup(r2, CW_MEM_GENERAL);
    e->type = "raw";
    e->uid = cw_res_path_uid(r2);
    P.n++;
  }
  closedir(d);
  return ok;
}

static void usage(void) {
  fprintf(stderr, "usage: cwpack --manifest roots.txt --out DIR [--pak FILE] [--cache DIR] [--name NAME]\n"
                  "              [--set key=value]... [--depfile FILE --dep-target T] [--stats FILE]\n"
                  "              [--engine-root DIR] [-q]\n"
                  "       cwpack --raw-dir DIR --pak FILE [-q]\n"
                  "       cwpack --po-export catalog.cwtr [--po-source en.cwtr] | --po-import file.po | --cwtr-check catalog.cwtr...\n");
}

/* the catalog conversions (translators' PO files both ways), on stdout */
static int l10n_tool(int argc, char **argv) {
  const char *exp = NULL, *src = NULL, *imp = NULL;
  bool check = false;
  int files = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--po-export") && i + 1 < argc) exp = argv[++i];
    else if (!strcmp(argv[i], "--po-source") && i + 1 < argc) src = argv[++i];
    else if (!strcmp(argv[i], "--po-import") && i + 1 < argc) imp = argv[++i];
    else if (!strcmp(argv[i], "--cwtr-check")) check = true;
    else if (check) files++;
  }
  char err[300];
  size_t n, sn;
  if (check) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
      if (argv[i][0] == '-') continue;
      uint8_t *t = cwp_read_file(argv[i], &n);
      bool po = rl_is(argv[i], ".po");
      cwtr_catalog *c = !t ? NULL : po ? cwtr_parse_po((char *)t, n, argv[i], err, sizeof err) : cwtr_parse((char *)t, n, argv[i], err, sizeof err);
      if (!c) bad++, fprintf(stderr, "cwpack: %s\n", t ? err : argv[i]);
      else printf("ok %s: %d entries\n", argv[i], c->entries.count);
      cwtr_free(c);
      cw_free(t);
    }
    return bad ? 1 : 0;
  }
  if (exp) {
    uint8_t *t = cwp_read_file(exp, &n), *s = src ? cwp_read_file(src, &sn) : NULL;
    cwtr_catalog *c = t ? cwtr_parse((char *)t, n, exp, err, sizeof err) : NULL;
    if (!c) { fprintf(stderr, "cwpack: %s\n", t ? err : exp); return 1; }
    cwtr_catalog *sc = s ? cwtr_parse((char *)s, sn, src, err, sizeof err) : NULL;
    if (src && !sc) { fprintf(stderr, "cwpack: %s\n", s ? err : src); return 1; }
    size_t ol;
    char *po = cwtr_write_po(c, sc, &ol);
    fwrite(po, 1, ol, stdout);
    return 0;
  }
  uint8_t *t = cwp_read_file(imp, &n);
  cwtr_catalog *c = t ? cwtr_parse_po((char *)t, n, imp, err, sizeof err) : NULL;
  if (!c) { fprintf(stderr, "cwpack: %s\n", t ? err : imp); return 1; }
  size_t ol;
  char *o = cwtr_write(c, &ol);
  fwrite(o, 1, ol, stdout);
  return 0;
}

/* cwpack is a one-shot build step: its queues and entries live until exit by
 * design, so a sanitizer build checks it for memory errors but not leaks. */
const char *__lsan_default_options(void);
const char *__lsan_default_options(void) { return "detect_leaks=0"; }

int main(int argc, char **argv) {
  const char *manifest = NULL, *out = NULL, *pak = NULL, *cache = NULL, *name = "cart", *depfile = NULL,
             *dep_target = NULL, *stats = NULL, *raw_dir = NULL, *engine_root = NULL;
  cwp_settings cli = { 0 };
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--po-export") || !strcmp(argv[i], "--po-import") || !strcmp(argv[i], "--cwtr-check")) return l10n_tool(argc, argv);
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--manifest") && v) manifest = argv[++i];
    else if (!strcmp(a, "--out") && v) out = argv[++i];
    else if (!strcmp(a, "--pak") && v) pak = argv[++i];
    else if (!strcmp(a, "--cache") && v) cache = argv[++i];
    else if (!strcmp(a, "--name") && v) name = argv[++i];
    else if (!strcmp(a, "--depfile") && v) depfile = argv[++i];
    else if (!strcmp(a, "--dep-target") && v) dep_target = argv[++i];
    else if (!strcmp(a, "--stats") && v) stats = argv[++i];
    else if (!strcmp(a, "--raw-dir") && v) raw_dir = argv[++i];
    else if (!strcmp(a, "--engine-root") && v) engine_root = argv[++i];
    else if (!strcmp(a, "-q")) P.quiet = true;
    else if (!strcmp(a, "--set") && v) {
      char k[128];
      const char *eq = strchr(argv[++i], '=');
      if (!eq) { fprintf(stderr, "cwpack: --set wants key=value\n"); return 2; }
      snprintf(k, sizeof k, "%.*s", (int)(eq - argv[i]), argv[i]);
      cwp_settings_set(&cli, k, eq + 1);
    } else { usage(); return 2; }
  }
  if (raw_dir) {
    if (!pak || manifest) { usage(); return 2; }
    if (!raw_walk(raw_dir, "")) return 1;
    if (!P.n) { fprintf(stderr, "cwpack: %s holds no files\n", raw_dir); return 1; }
    qsort(P.e, (size_t)P.n, sizeof *P.e, cmp_entry);
    if (!write_pak(pak, true)) { fprintf(stderr, "cwpack: cannot write %s\n", pak); return 1; }
    return 0;
  }
  if (!manifest || !out) { usage(); return 2; }
  size_t ml;
  char *mt = (char *)cwp_read_file(manifest, &ml);
  if (!mt) { fprintf(stderr, "cwpack: cannot read the manifest %s\n", manifest); return 1; }
  add_source(manifest);
  char mdir[CWP_PATH_MAX];
  cwp_dirname(manifest, mdir, sizeof mdir);
  snprintf(P.mdir, sizeof P.mdir, "%s", mdir);
  if (cache) snprintf(P.cache, sizeof P.cache, "%s", cache);
  else snprintf(P.cache, sizeof P.cache, "%s.cwcache", mdir); /* the project's own .cwcache/ */
  str_list qr = { 0 }, qs = { 0 }, qref = { 0 };
  int line_no = 0;
  for (char *line = mt; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    line_no++;
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
    if (!*s || *s == '#') { line = nl ? nl + 1 : NULL; continue; }
    if (!strncmp(s, "set ", 4)) {
      char k[128], v[1024];
      if (sscanf(s + 4, " %127s %1023[^\n]", k, v) == 2) cwp_settings_set(&P.project, k, v);
      else { fprintf(stderr, "cwpack: %s:%d: \"set <key> <value>\"\n", manifest, line_no); return 1; }
    } else {
      char res[CWP_PATH_MAX], srcrel[CWP_PATH_MAX], c[CWP_PATH_MAX], src[CWP_PATH_MAX];
      char *eq = strstr(s, " = ");
      if (eq) { *eq = 0; snprintf(res, sizeof res, "%s", s); snprintf(srcrel, sizeof srcrel, "%s", eq + 3); }
      else { snprintf(res, sizeof res, "%s", s); snprintf(srcrel, sizeof srcrel, "%s", s); }
      if (!cw__res_canon(res, c, sizeof c) || strchr(c, '#')) { fprintf(stderr, "cwpack: %s:%d: bad resource path \"%s\"\n", manifest, line_no, res); return 1; }
      if (!strncmp(srcrel, "@engine/", 8)) {
        if (!engine_root) { fprintf(stderr, "cwpack: %s:%d: %s needs --engine-root (cw_add_app passes it)\n", manifest, line_no, srcrel); return 1; }
        char er[CWP_PATH_MAX];
        snprintf(er, sizeof er, "%s/", engine_root);
        cwp_join(er, srcrel + 8, src, sizeof src);
      } else cwp_join(mdir, srcrel, src, sizeof src);
      cw_array_push(&qr, cw_strdup(c, CW_MEM_GENERAL));
      cw_array_push(&qs, cw_strdup(src, CW_MEM_GENERAL));
      cw_array_push(&qref, NULL);
      cw_array_push(&P.root_res, cw_strdup(c, CW_MEM_GENERAL));
      cw_array_push(&P.root_src, cw_strdup(src, CW_MEM_GENERAL));
    }
    line = nl ? nl + 1 : NULL;
  }
  cwp_settings_copy(&P.project, &cli);
  /* walk: roots first, then what they reference, in discovery order */
  bool ok = true;
  for (int qi = 0; qi < qr.count; qi++) {
    entry *have = find_entry(qr.data[qi]);
    if (have) {
      char a1[CWP_PATH_MAX], a2[CWP_PATH_MAX];
      cwp_absolute(have->src, a1, sizeof a1);
      cwp_absolute(qs.data[qi], a2, sizeof a2);
      if (strcmp(a1, a2)) { fprintf(stderr, "cwpack: res://%s comes from two sources: %s and %s\n", qr.data[qi], have->src, qs.data[qi]); ok = false; }
      continue;
    }
    if (P.n == P.cap) {
      P.cap = P.cap ? P.cap * 2 : 64;
      P.e = cw_realloc(P.e, (size_t)P.cap * sizeof *P.e, CW_MEM_GENERAL);
    }
    int before = qr.count;
    if (!process(qr.data[qi], qs.data[qi], qref.data[qi], &qr, &qs)) { ok = false; continue; }
    while (qref.count < qr.count) cw_array_push(&qref, P.e[P.n - 1].res);
    (void)before;
  }
  if (!ok) return 1;
  /* per-locale assets of every localization project, for every catalog locale */
  str_list locales = { 0 };
  for (int i = 0; i < P.n; i++) {
    if (!rl_is(P.e[i].res, ".cwtr")) continue;
    cwtr_catalog *c = cwtr_parse((const char *)P.e[i].data, P.e[i].len, P.e[i].res, NULL, 0);
    const char *loc = c ? cwtr_field(c, "locale") : NULL;
    bool dup = false;
    for (int k = 0; loc && k < locales.count; k++) dup |= !strcmp(locales.data[k], loc);
    if (loc && !dup) cw_array_push(&locales, cw_strdup(loc, CW_MEM_GENERAL));
    cwtr_free(c);
  }
  int before_assets = qr.count;
  for (int i = 0; i < P.n; i++) {
    if (!rl_is(P.e[i].res, ".cwl10n")) continue;
    asset_ctx a = { "", { 0 } };
    l10n_lines((const char *)P.e[i].data, P.e[i].len, asset_line, &a);
    for (int k = 0; a.dir[0] && k < a.assets.count; k++)
      for (int l = 0; l < locales.count; l++) {
        char rp[CWP_PATH_MAX * 3], c[CWP_PATH_MAX], src[CWP_PATH_MAX];
        snprintf(rp, sizeof rp, "%s/%s/%s", a.dir, locales.data[l], a.assets.data[k]);
        if (!cw__res_canon(rp, c, sizeof c)) continue;
        source_of(c, src, sizeof src);
        if (!cwp_exists(src) || find_entry(c)) continue; /* a locale without that variant uses the base asset */
        cw_array_push(&qr, cw_strdup(c, CW_MEM_GENERAL));
        cw_array_push(&qs, cw_strdup(src, CW_MEM_GENERAL));
        cw_array_push(&qref, P.e[i].res);
      }
  }
  for (int qi = before_assets; qi < qr.count; qi++) {
    if (find_entry(qr.data[qi])) continue;
    if (P.n == P.cap) P.e = cw_realloc(P.e, (size_t)(P.cap = P.cap * 2 + 1) * sizeof *P.e, CW_MEM_GENERAL);
    if (!process(qr.data[qi], qs.data[qi], qref.count > qi ? qref.data[qi] : NULL, &qr, &qs)) { ok = false; continue; }
    while (qref.count < qr.count) cw_array_push(&qref, P.e[P.n - 1].res);
  }
  if (!ok) return 1;
  /* font subsets: printable ASCII, the font's extra characters and every
   * character of every packed catalog */
  cw_map cpset;
  cw_map_init(&cpset, CW_MEM_GENERAL);
  u32_list cps = { 0 };
  char ascii[96];
  for (int c = 0x20; c < 0x7f; c++) ascii[c - 0x20] = (char)c;
  ascii[0x5f] = 0;
  add_cps(&cpset, &cps, ascii);
  add_cps(&cpset, &cps, "\xE2\x80\xA6\xC2\xB7"); /* ellipsis (UI truncation), middle dot */
  for (int i = 0; i < P.n; i++) {
    if (!rl_is(P.e[i].res, ".cwtr")) continue;
    cwtr_catalog *c = cwtr_parse((const char *)P.e[i].data, P.e[i].len, P.e[i].res, NULL, 0);
    for (int k = 0; c && k < c->entries.count; k++) add_cps(&cpset, &cps, c->entries.data[k].value);
    for (int k = 0; c && k < c->fields.count; k++) add_cps(&cpset, &cps, c->fields.data[k].value);
    /* what the locale's number and date formatting writes besides catalog text */
    const cwtr_lang *li = cwtr_lang_info(c ? cwtr_field(c, "locale") : "en");
    add_cps(&cpset, &cps, li->decimal);
    add_cps(&cpset, &cps, li->group);
    add_cps(&cpset, &cps, li->date_short);
    add_cps(&cpset, &cps, li->date_medium);
    add_cps(&cpset, &cps, li->date_long);
    const char *digits = c && cwtr_field(c, "number.digits") ? cwtr_field(c, "number.digits") : li->digits;
    if (!strcmp(digits, "arab")) add_cps(&cpset, &cps, "\xD9\xA0\xD9\xA1\xD9\xA2\xD9\xA3\xD9\xA4\xD9\xA5\xD9\xA6\xD9\xA7\xD9\xA8\xD9\xA9");
    cwtr_free(c);
  }
  for (int i = 0; i < P.n; i++) {
    entry *e = &P.e[i];
    if (!e->subset) continue;
    if (strcmp(e->subset, "l10n")) {
      fprintf(stderr, "cwpack: res://%s: font.subset = %s (only 'l10n' exists)\n", e->res, e->subset);
      return 1;
    }
    cw_map own;
    cw_map_init(&own, CW_MEM_GENERAL);
    u32_list fcps = { 0 };
    for (int k = 0; k < cps.count; k++) {
      cw_map_set(&own, cps.data[k], 1);
      cw_array_push(&fcps, cps.data[k]);
    }
    add_cps(&own, &fcps, e->subset_chars);
    qsort(fcps.data, (size_t)fcps.count, sizeof(uint32_t), cmp_u32);
    uint64_t key = cw_hash_bytes("font", 4, CWP_IMPORT_FONT_VERSION);
    key = cw_hash_bytes(e->settings_canon, strlen(e->settings_canon), key);
    key = cw_hash_bytes(fcps.data, sizeof(uint32_t) * (size_t)fcps.count, key);
    key = cw_hash_bytes(e->data, e->len, key);
    size_t ol;
    uint8_t *sub = cache_get(key, "font", &ol);
    e->cached = sub != NULL;
    char note[200] = "";
    if (!sub) {
      sub = cwp_subset_font(e->res, e->data, e->len, fcps.data, fcps.count, &ol, note, sizeof note);
      if (!sub) return 1;
      cache_put(key, "font", sub, ol);
    }
    if (e->cached) P.from_cache++;
    else P.imported++;
    if (!P.quiet)
      printf("  %-7s res://%s (font, %zu -> %zu bytes)%s%s\n", e->cached ? "cached" : "import", e->res, e->len, ol,
             note[0] ? ": " : "", note);
    cw_free(e->data);
    e->data = sub;
    e->len = ol;
    cw_map_free(&own);
    cw_array_free(&fcps);
  }
  cw_map_free(&cpset);
  cw_array_free(&cps);
  /* preload lists: every scene gets <scene>.preload, everything it needs in
   * load order (sub-scenes' needs first, then its own references, then the
   * scene), so cw_res_preload can start every load at once */
  int scenes = P.n;
  for (int i = 0; i < scenes; i++) {
    size_t rl = strlen(P.e[i].res);
    if (rl <= 8 || strcmp(P.e[i].res + rl - 8, ".cwscene")) continue;
    str_list order = { 0 };
    preload_walk(P.e[i].res, &order, 0);
    CW_ARRAY(char) t = { 0 };
    const char *h = "# cwpack preload: what this scene needs, dependencies first\n";
    for (const char *c = h; *c; c++) cw_array_push(&t, *c);
    for (int k = 0; k < order.count; k++) {
      for (const char *c = order.data[k]; *c; c++) cw_array_push(&t, *c);
      cw_array_push(&t, '\n');
      cw_free(order.data[k]);
    }
    cw_array_free(&order);
    if (P.n == P.cap) P.e = cw_realloc(P.e, (size_t)(P.cap = P.cap * 2 + 1) * sizeof *P.e, CW_MEM_GENERAL);
    entry *pe = &P.e[P.n++];
    memset(pe, 0, sizeof *pe);
    char name[CWP_PATH_MAX + 16];
    snprintf(name, sizeof name, "%s.preload", P.e[i].res);
    pe->res = cw_strdup(name, CW_MEM_GENERAL);
    pe->type = "text";
    pe->data = (uint8_t *)t.data;
    pe->len = (size_t)t.count;
    pe->uid = cw_res_path_uid(name);
  }
  /* res.index: uid type path, by path */
  qsort(P.e, (size_t)P.n, sizeof *P.e, cmp_entry);
  CW_ARRAY(char) ix = { 0 };
  const char *hdr = "# cwpack res.index: uid type path\n";
  for (const char *c = hdr; *c; c++) cw_array_push(&ix, *c);
  for (int i = 0; i < P.n; i++) {
    char l[CWP_PATH_MAX + 64];
    int n = snprintf(l, sizeof l, "%016llx %s %s\n", (unsigned long long)P.e[i].uid, P.e[i].type, P.e[i].res);
    for (int k = 0; k < n; k++) cw_array_push(&ix, l[k]);
    for (int k = 0; k < P.e[i].subs.count; k++) {
      char ty[32], nm[64], full[CWP_PATH_MAX + 80];
      sscanf(P.e[i].subs.data[k], "%31s %63s", ty, nm);
      snprintf(full, sizeof full, "%s#%s", P.e[i].res, nm);
      n = snprintf(l, sizeof l, "%016llx %s %s\n", (unsigned long long)cw_res_path_uid(full), ty, full);
      for (int c = 0; c < n; c++) cw_array_push(&ix, l[c]);
    }
  }
  if (P.n == P.cap) P.e = cw_realloc(P.e, (size_t)(P.cap = P.cap + 1) * sizeof *P.e, CW_MEM_GENERAL);
  entry *ie = &P.e[P.n++];
  memset(ie, 0, sizeof *ie);
  ie->res = cw_strdup("res.index", CW_MEM_GENERAL);
  ie->type = "text";
  ie->data = (uint8_t *)ix.data;
  ie->len = (size_t)ix.count;
  ie->uid = cw_res_path_uid("res.index");
  qsort(P.e, (size_t)P.n, sizeof *P.e, cmp_entry);
  /* the asset directory, exactly the packed set */
  char adir[CWP_PATH_MAX], p[CWP_PATH_MAX * 2];
  snprintf(adir, sizeof adir, "%s/assets", out);
  cwp_mkdirs(adir);
  prune(adir, "");
  size_t total = 0;
  for (int i = 0; i < P.n; i++) {
    snprintf(p, sizeof p, "%s/%s", adir, P.e[i].res);
    if (!write_if_changed(p, P.e[i].data, P.e[i].len)) { fprintf(stderr, "cwpack: cannot write %s\n", p); return 1; }
    total += P.e[i].len;
  }
  /* cart-directory manifest with the files list */
  cwj *man = cwj_obj(), *files = cwj_arr();
  cwj_set(man, "name", cwj_str(name));
  cwj_set(man, "entry", cwj_str("cart.wasm"));
  cwj_set(man, "assets", cwj_str("assets/"));
  for (int i = 0; i < P.n; i++) cwj_push(files, cwj_str(P.e[i].res));
  cwj_set(man, "files", files);
  size_t jl;
  char *js = cwj_write(man, &jl);
  snprintf(p, sizeof p, "%s/manifest.json", out);
  js = cw_realloc(js, jl + 2, CW_MEM_RESOURCE);
  js[jl++] = '\n';
  write_if_changed(p, js, jl);
  cw_free(js);
  cwj_free(man);
  if (pak && !write_pak(pak, cwp_settings_bool(&P.project, "pack.deflate", true))) { fprintf(stderr, "cwpack: cannot write %s\n", pak); return 1; }
  /* sources.txt and the depfile */
  qsort(P.sources.data, (size_t)P.sources.count, sizeof *P.sources.data, cmp_str);
  CW_ARRAY(char) sl = { 0 };
  for (int i = 0; i < P.sources.count; i++) {
    for (const char *c = P.sources.data[i]; *c; c++) cw_array_push(&sl, *c);
    cw_array_push(&sl, '\n');
  }
  snprintf(p, sizeof p, "%s/sources.txt", out);
  write_if_changed(p, sl.data ? sl.data : "", (size_t)sl.count);
  if (depfile) {
    FILE *f = fopen(depfile, "wb");
    if (f) {
      fprintf(f, "%s:", dep_target ? dep_target : out);
      for (int i = 0; i < P.sources.count; i++) {
        fputs(" \\\n ", f);
        for (const char *c = P.sources.data[i]; *c; c++) { if (*c == ' ') fputc('\\', f); fputc(*c, f); }
      }
      fputc('\n', f);
      fclose(f);
    }
  }
  if (stats) {
    cwj *s = cwj_obj(), *imp = cwj_arr(), *cached = cwj_arr(), *copied = cwj_arr();
    for (int i = 0; i < P.n; i++) {
      if (!strcmp(P.e[i].res, "res.index")) continue;
      bool copy = strcmp(P.e[i].type, "gltf") && strcmp(P.e[i].type, "texture") && strcmp(P.e[i].type, "audio_clip");
      cwj_push(copy ? copied : P.e[i].cached ? cached : imp, cwj_str(P.e[i].res));
    }
    cwj_set(s, "imported", imp);
    cwj_set(s, "cached", cached);
    cwj_set(s, "copied", copied);
    cwj_set(s, "files", cwj_num(P.n));
    cwj_set(s, "bytes", cwj_num((double)total));
    char *t = cwj_write(s, &jl);
    cwp_write_file(stats, t, jl);
    cw_free(t);
    cwj_free(s);
  }
  if (!P.quiet) printf("cwpack: %d files, %zu bytes (%d imported, %d from cache)\n", P.n, total, P.imported, P.from_cache);
  return 0;
}

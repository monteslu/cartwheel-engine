/* Import dock (M4.2): every importable project file goes through cwpack's
 * own importers in process (tools/cwpack, library cw_importers), so what the
 * editor reports is what the build will pack:
 *   glTF / GLB      cwp_import_gltf (vertex-cache + overdraw + fetch
 *                   order, LODs, optional meshopt compression, embedded
 *                   images); external buffers and images must exist
 *   PNG/JPEG/KTX2   cwp_import_texture (kept, or KTX2 through Basis)
 *   WAV/FLAC/MP3    cwp_import_audio (kept, or Ogg Opus)
 *   Ogg, Opus, modules, fonts   checked (fonts: a TrueType/OpenType header)
 * Per-file settings live beside the asset in its cwpack sidecar
 * "<file>.cwimport" (key = value lines, the uid line kept), so they diff and
 * merge like code and the build reads the very same file.
 *
 * Files are (re)imported when their bytes, their buffers/images or their
 * settings change: results are cached by that content key, a few files per
 * frame, in path order (the same frames on every host). A failed import is
 * an error with the importer's own message (the Output panel and
 * import.info show it), never a silent skip.
 *
 *   import.info <path>              status, message, sizes, settings (JSON)
 *   import.list                     every importable file's status
 *   import.set <path> <key> [value] change a setting (no value: back to the
 *                                   default); undoable, re-imports at once
 *   import.reimport <path>          run the importer again (no cache) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/resource.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"
#include "cwpack.h"

/* ── what each importer reads ──────────────────────────────────────────── */

typedef struct { const char *kind, *key, *def, *choices, *doc; } setting_def;
static const setting_def SETTINGS[] = {
  { "gltf", "mesh.optimize", "true", "true|false", "vertex-cache, overdraw and vertex-fetch order per primitive" },
  { "gltf", "mesh.lods", "0.5 0.25", NULL, "simplified index buffers at these triangle ratios (\"\" for none)" },
  { "gltf", "mesh.lod_error", "0.02", NULL, "the simplifier's error bound, relative to the mesh extent" },
  { "gltf", "mesh.compress", "false", "true|false", "EXT_meshopt_compression on vertex and index data" },
  { "gltf", "texture.format", "keep", "keep|ktx2", "embedded images: kept, or KTX2 (Basis Universal)" },
  { "texture", "texture.format", "keep", "keep|ktx2", "kept as it is, or encoded to KTX2 (Basis Universal)" },
  { "texture", "texture.mode", "uastc", "uastc|etc1s", "KTX2 codec: UASTC (quality) or ETC1S (size)" },
  { "texture", "texture.quality", "-1", NULL, "KTX2 quality 1..100 (-1: the codec's default)" },
  { "texture", "texture.effort", "-1", NULL, "KTX2 effort 0..10 (-1: the codec's default)" },
  { "texture", "texture.mips", "true", "true|false", "a full mip chain in the KTX2 file" },
  { "texture", "texture.color", "srgb", "srgb|linear", "colour data (sRGB) or data such as normals (linear)" },
  { "audio", "audio.format", "keep", "keep|opus", "kept as it is, or encoded to Ogg Opus" },
  { "audio", "audio.bitrate", "", NULL, "Opus bits per second (empty: 96000 stereo, 64000 mono)" },
  { "audio", "audio.complexity", "10", NULL, "Opus encoder complexity 0..10" },
};

static const char *ext_lower(const char *p) {
  static char e[16];
  const char *dot = strrchr(p, '.'), *slash = strrchr(p, '/');
  e[0] = 0;
  if (!dot || (slash && dot < slash) || strlen(dot) >= sizeof e) return e;
  for (size_t i = 0; i <= strlen(dot); i++) e[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
  return e;
}
static bool in_list(const char *e, const char *list) {
  size_t n = strlen(e);
  if (!n) return false;
  for (const char *s = list; (s = strstr(s, e)); s += n)
    if ((s == list || s[-1] == ' ') && (s[n] == 0 || s[n] == ' ')) return true;
  return false;
}
/* the importer for a path, NULL when the file has no import step */
static const char *kind_of(const char *p) {
  const char *e = ext_lower(p);
  if (in_list(e, ".gltf .glb")) return "gltf";
  if (in_list(e, ".png .jpg .jpeg .ktx2")) return "texture";
  if (in_list(e, ".wav .flac .mp3")) return "audio";
  if (in_list(e, ".ogg .opus .xm .mod .s3m .it")) return "audio_keep";
  if (in_list(e, ".ttf .otf .ttc")) return "font";
  return NULL;
}
static const char *settings_kind(const char *kind) { return !strcmp(kind, "audio_keep") ? "audio" : kind; }

/* ── state ─────────────────────────────────────────────────────────────── */

enum { ST_PENDING, ST_OK, ST_ERROR };
static const char *ST_NAME[] = { "pending", "ok", "error" };

typedef struct imp {
  const char *path, *kind;
  int status;
  uint64_t key;     /* content key of the last import (0 = never) */
  uint64_t want;    /* content key now */
  size_t in_bytes, out_bytes;
  char msg[600], note[256];
  bool cached, seen;
} imp;
static CW_ARRAY(imp) imps; /* sorted by path */

/* results by content key: the same bytes + settings never import twice */
typedef struct { uint64_t key; int status; size_t out_bytes; char msg[600], note[256]; } cached_result;
static CW_ARRAY(cached_result) cache;

static uint32_t seen_gen;
static int n_ok, n_err, n_pending, imports_run;
static uint32_t results_hash;
static char shown_path[512];
static bool dock_dirty = true;

static imp *find_imp(const char *path) {
  int lo = 0, hi = imps.count;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    int c = strcmp(imps.data[mid].path, path);
    if (!c) return &imps.data[mid];
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return NULL;
}

/* ── sidecar settings ──────────────────────────────────────────────────── */

static void read_settings(const char *path, cwp_settings *set) {
  char *t = cw_vfs_read(cw_frame_printf("%s.cwimport", path), NULL);
  for (char *line = t ? strtok(t, "\n") : NULL; line; line = strtok(NULL, "\n")) {
    char k[128], v[1024] = "";
    if (line[0] == '#') continue;
    int got = sscanf(line, " %127[^= ] = %1023[^\n]", k, v);
    if (got < 1 || !strchr(line, '=')) continue;
    size_t vl = strlen(v);
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) v[--vl] = 0;
    if (strcmp(k, "uid")) cwp_settings_set(set, k, v);
  }
  free(t);
}

static const char *const PREFIXES[] = { "mesh.", "texture.", "audio." };

/* the content key: importer, settings, the file and what it reads */
typedef struct { const char *base; uint64_t key; bool missing; char missing_uri[256]; } dep_hash;
static void hash_dep(const char *uri, void *user) {
  dep_hash *h = user;
  char dec[512], full[1024];
  cwp_uri_decode(uri, dec, sizeof dec);
  const char *slash = strrchr(h->base, '/');
  snprintf(full, sizeof full, "%.*s%s", slash ? (int)(slash - h->base) + 1 : 0, h->base, dec);
  size_t n;
  void *b = cw_vfs_read(full, &n);
  if (!b) {
    if (!h->missing) snprintf(h->missing_uri, sizeof h->missing_uri, "%s", uri);
    h->missing = true;
    h->key = cw_hash_bytes(full, strlen(full), h->key ^ 0x5a5a);
    return;
  }
  h->key = cw_hash_bytes(full, strlen(full), h->key);
  h->key = cw_hash_bytes(b, n, h->key);
  free(b);
}
static uint64_t content_key(const char *path, const char *kind) {
  size_t n;
  void *b = cw_vfs_read(path, &n);
  uint64_t key = cw_hash_bytes(kind, strlen(kind), 0x1a2b3c4d ^ (uint64_t)(CWP_IMPORT_GLTF_VERSION * 131 + CWP_IMPORT_TEXTURE_VERSION * 17 +
                                                                          CWP_IMPORT_AUDIO_VERSION));
  if (!b) return key ^ 1;
  key = cw_hash_bytes(b, n, key);
  cwp_settings set = { 0 };
  read_settings(path, &set);
  char *canon = cwp_settings_canon(&set, PREFIXES, 3);
  key = cw_hash_bytes(canon, strlen(canon), key);
  cw_free(canon);
  cwp_settings_free(&set);
  if (!strcmp(kind, "gltf")) {
    dep_hash h = { path, key, false, "" };
    cwp_gltf_subresources(b, n, NULL, hash_dep, &h);
    key = h.key;
  }
  free(b);
  return key;
}

/* ── running an importer ──────────────────────────────────────────────── */

static char report_buf[600];
static void reporter(const char *msg, void *user) {
  (void)user;
  if (!report_buf[0]) snprintf(report_buf, sizeof report_buf, "%s", msg);
}

typedef struct { const char *base; } gltf_read_ctx;
static void *gltf_read(void *ctx, const char *uri, size_t *len) {
  gltf_read_ctx *g = ctx;
  char dec[512], full[1024];
  cwp_uri_decode(uri, dec, sizeof dec);
  const char *slash = strrchr(g->base, '/');
  snprintf(full, sizeof full, "%.*s%s", slash ? (int)(slash - g->base) + 1 : 0, g->base, dec);
  size_t n;
  void *b = cw_vfs_read(full, &n);
  if (!b) return NULL;
  uint8_t *c = cw_alloc(n + 1, CW_MEM_RESOURCE);
  memcpy(c, b, n);
  free(b);
  *len = n;
  return c;
}

static void run_import(imp *m, bool use_cache) {
  m->want = content_key(m->path, m->kind);
  if (use_cache)
    for (int i = 0; i < cache.count; i++)
      if (cache.data[i].key == m->want) {
        cached_result *r = &cache.data[i];
        m->status = r->status;
        m->out_bytes = r->out_bytes;
        snprintf(m->msg, sizeof m->msg, "%s", r->msg);
        snprintf(m->note, sizeof m->note, "%s", r->note);
        m->key = m->want;
        m->cached = true;
        return;
      }
  size_t n;
  uint8_t *b = cw_vfs_read(m->path, &n);
  m->in_bytes = b ? n : 0;
  m->msg[0] = m->note[0] = 0;
  m->out_bytes = 0;
  m->cached = false;
  report_buf[0] = 0;
  cwp_set_reporter(reporter, NULL);
  cwp_settings set = { 0 };
  read_settings(m->path, &set);
  bool ok = false;
  if (!b) {
    snprintf(m->msg, sizeof m->msg, "%s: cannot read the file", m->path);
  } else if (!strcmp(m->kind, "gltf")) {
    dep_hash h = { m->path, 0, false, "" };
    bool is_gltf = cwp_gltf_subresources(b, n, NULL, hash_dep, &h);
    if (!is_gltf) {
      snprintf(m->msg, sizeof m->msg, "%s: not a glTF 2.0 file (no JSON with an \"asset\" object, nor a GLB container)", m->path);
    } else if (h.missing) {
      snprintf(m->msg, sizeof m->msg, "%s: references \"%s\", which is not in the project (copy it beside the glTF)", m->path,
               h.missing_uri);
    } else {
      gltf_read_ctx g = { m->path };
      cwp_gltf_stats st;
      size_t out_n = 0;
      uint8_t *out = cwp_import_gltf(m->path, b, n, gltf_read, &g, &set, &out_n, &st);
      if (out) {
        ok = true;
        m->out_bytes = out_n;
        snprintf(m->note, sizeof m->note, "%d/%d primitives optimized, %d -> %d vertices, %d triangles, %d LODs (%d triangles)%s",
                 st.prims_optimized, st.prims, st.vertices_in, st.vertices_out, st.triangles, st.lods, st.lod_triangles,
                 st.compressed_views ? ", meshopt compressed" : "");
        cw_free(out);
      } else {
        snprintf(m->msg, sizeof m->msg, "%s", report_buf[0] ? report_buf : "the glTF importer failed (no message)");
      }
    }
  } else if (!strcmp(m->kind, "texture")) {
    bool srgb = strcmp(cwp_settings_get(&set, "texture.color", "srgb"), "linear") != 0;
    size_t out_n = 0;
    uint8_t *out = cwp_import_texture(m->path, b, n, &set, srgb, &out_n);
    if (out) {
      ok = true;
      m->out_bytes = out_n;
      const char *fmt = cwp_settings_get(&set, "texture.format", "keep");
      snprintf(m->note, sizeof m->note, "%s, %zu -> %zu bytes", !strcmp(fmt, "ktx2") ? "KTX2" : "kept", n, out_n);
      /* "keep" copies whatever it is given: the bytes must still be an image */
      if (strcmp(fmt, "ktx2") && !(n >= 12 && b[0] == 0xAB && b[1] == 'K')) {
        extern unsigned char *stbi_load_from_memory(const unsigned char *, int, int *, int *, int *, int);
        extern void stbi_image_free(void *);
        extern const char *stbi_failure_reason(void);
        int w, h, ch;
        unsigned char *px = stbi_load_from_memory(b, (int)n, &w, &h, &ch, 4);
        if (!px) {
          ok = false;
          snprintf(m->msg, sizeof m->msg, "%s: cannot decode the image (%s)", m->path, stbi_failure_reason());
        } else {
          snprintf(m->note, sizeof m->note, "%dx%d, kept (%zu bytes)", w, h, n);
          stbi_image_free(px);
        }
      }
      cw_free(out);
    } else {
      snprintf(m->msg, sizeof m->msg, "%s", report_buf[0] ? report_buf : "the texture importer failed (no message)");
    }
  } else if (!strcmp(m->kind, "audio")) {
    size_t out_n = 0;
    char note[160] = "";
    uint8_t *out = cwp_import_audio(m->path, b, n, &set, &out_n, note, sizeof note);
    if (out) {
      ok = true;
      m->out_bytes = out_n;
      snprintf(m->note, sizeof m->note, "%s%s%zu -> %zu bytes", note, note[0] ? ", " : "", n, out_n);
      cw_free(out);
    } else {
      snprintf(m->msg, sizeof m->msg, "%s", report_buf[0] ? report_buf : "the audio importer failed (no message)");
    }
  } else if (!strcmp(m->kind, "audio_keep")) {
    ok = n > 4;
    m->out_bytes = n;
    if (ok) snprintf(m->note, sizeof m->note, "packed as it is (%zu bytes)", n);
    else snprintf(m->msg, sizeof m->msg, "%s: empty audio file", m->path);
  } else if (!strcmp(m->kind, "font")) {
    ok = n >= 12 && ((b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) || !memcmp(b, "OTTO", 4) || !memcmp(b, "true", 4) ||
                     !memcmp(b, "ttcf", 4));
    m->out_bytes = n;
    if (ok) snprintf(m->note, sizeof m->note, "font, packed as it is (%zu bytes)", n);
    else snprintf(m->msg, sizeof m->msg, "%s: not a TrueType/OpenType font (bad header)", m->path);
  }
  cwp_set_reporter(NULL, NULL);
  cwp_settings_free(&set);
  free(b);
  m->status = ok ? ST_OK : ST_ERROR;
  m->key = m->want;
  imports_run++;
  cached_result r = { m->key, m->status, m->out_bytes, "", "" };
  snprintf(r.msg, sizeof r.msg, "%s", m->msg);
  snprintf(r.note, sizeof r.note, "%s", m->note);
  cw_array_push(&cache, r);
  if (ok) cwed_log(CWED_LOG_INFO, "imported %s: %s", m->path, m->note);
  else cwed_log(CWED_LOG_ERROR, "import failed: %s", m->msg);
  /* the editor draws from the source: a changed file loaded as a resource reloads */
  cw_res h = cw_res_find(m->path);
  if (!CW_HANDLE_IS_NULL(h) && cw_res_get_state(h) != CW_RES_STATE_LOADING) cw_res_reload(h);
}

/* ── tracking the project's importable files ──────────────────────────── */

static int cmp_imp(const void *a, const void *b) { return strcmp(((const imp *)a)->path, ((const imp *)b)->path); }

static void track(const char *path, const cw_vfs_entry *e, void *user) {
  (void)e, (void)user;
  const char *kind = kind_of(path);
  if (!kind) return;
  imp *m = find_imp(path);
  if (!m) {
    imp n = { .path = cw_intern(path), .kind = kind, .status = ST_PENDING };
    cw_array_push(&imps, n);
    qsort(imps.data, (size_t)imps.count, sizeof *imps.data, cmp_imp);
    m = find_imp(path);
  }
  m->seen = true;
}

static void refresh_counts(void) {
  n_ok = n_err = n_pending = 0;
  results_hash = CW_HASH32_INIT;
  for (int i = 0; i < imps.count; i++) {
    imp *m = &imps.data[i];
    n_ok += m->status == ST_OK;
    n_err += m->status == ST_ERROR;
    n_pending += m->status == ST_PENDING;
    const char *s = cw_frame_printf("%s|%s|%s|%s|%zu\n", m->path, ST_NAME[m->status], m->msg, m->note, m->out_bytes);
    results_hash = cw_hash32(s, strlen(s), results_hash);
  }
}

static void rescan(void) {
  for (int i = 0; i < imps.count; i++) imps.data[i].seen = false;
  cw_vfs_walk("", 0, track, NULL);
  for (int i = imps.count - 1; i >= 0; i--)
    if (!imps.data[i].seen) memmove(imps.data + i, imps.data + i + 1, sizeof(imp) * (size_t)(--imps.count - i));
  /* what changed since its last import is pending again */
  for (int i = 0; i < imps.count; i++) {
    imp *m = &imps.data[i];
    m->want = content_key(m->path, m->kind);
    if (m->want != m->key) m->status = ST_PENDING;
  }
  seen_gen = cw_vfs_generation();
  refresh_counts();
  dock_dirty = true;
}

#define IMPORTS_PER_FRAME 2

static void pump(void) {
  if (seen_gen != cw_vfs_generation()) rescan();
  int done = 0;
  for (int i = 0; i < imps.count && done < IMPORTS_PER_FRAME; i++)
    if (imps.data[i].status == ST_PENDING) {
      run_import(&imps.data[i], true);
      if (!imps.data[i].cached) done++;
      if (!strcmp(imps.data[i].path, shown_path)) dock_dirty = true;
      cwed_emit(&(cwed_event){ .type = CWED_EV_FILES, .line = imps.data[i].path });
    }
  refresh_counts();
}

/* ── commands ──────────────────────────────────────────────────────────── */

static imp *arg_imp(cwed_call *c) {
  const char *path = cwed_arg(c, "path");
  if (seen_gen != cw_vfs_generation()) rescan();
  imp *m = find_imp(path);
  if (!m) {
    if (!cw_vfs_exists(path)) cwed_fail(c, "no file '%s' in the project (fs.list shows the files)", path);
    else cwed_fail(c, "%s has no import step (imported: .gltf .glb .png .jpg .ktx2 .wav .flac .mp3 .ogg .opus fonts)", path);
  }
  return m;
}

static void json_settings(cwed_json *j, const imp *m) {
  cwp_settings set = { 0 };
  read_settings(m->path, &set);
  const char *sk = settings_kind(m->kind);
  cwed_json_key(j, "settings");
  cwed_json_raw(j, "[");
  int n = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(SETTINGS); i++) {
    const setting_def *s = &SETTINGS[i];
    if (strcmp(s->kind, sk)) continue;
    const char *v = cwp_settings_get(&set, s->key, NULL);
    if (n++) cwed_json_raw(j, ",");
    cwed_json_raw(j, "{");
    cwed_json_key(j, "key");
    cwed_json_str(j, s->key);
    cwed_json_key(j, "value");
    cwed_json_str(j, v ? v : s->def);
    cwed_json_key(j, "default");
    cwed_json_str(j, s->def);
    cwed_json_key(j, "set");
    cwed_json_raw(j, v ? "true" : "false");
    if (s->choices) {
      cwed_json_key(j, "choices");
      cwed_json_str(j, s->choices);
    }
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  cwp_settings_free(&set);
}

static void json_imp(cwed_json *j, const imp *m) {
  cwed_json_key(j, "path");
  cwed_json_str(j, m->path);
  cwed_json_key(j, "importer");
  cwed_json_str(j, settings_kind(m->kind));
  cwed_json_key(j, "status");
  cwed_json_str(j, ST_NAME[m->status]);
  if (m->status == ST_ERROR) {
    cwed_json_key(j, "error");
    cwed_json_str(j, m->msg);
  }
  cwed_json_key(j, "note");
  cwed_json_str(j, m->note);
  cwed_json_key(j, "bytes_in");
  cwed_json_printf(j, "%zu", m->in_bytes);
  cwed_json_key(j, "bytes_out");
  cwed_json_printf(j, "%zu", m->out_bytes);
}

static bool info_exec(cwed_call *c) {
  imp *m = arg_imp(c);
  if (!m) return false;
  if (m->status == ST_PENDING) run_import(m, true); /* asking means wanting the answer now */
  refresh_counts();
  cwed_json *j = cwed_reply(c);
  json_imp(j, m);
  cwed_json_key(j, "sidecar");
  cwed_json_str(j, cw_frame_printf("%s.cwimport", m->path));
  json_settings(j, m);
  return true;
}

static bool list_exec(cwed_call *c) {
  if (seen_gen != cw_vfs_generation()) rescan();
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "files");
  cwed_json_raw(j, "[");
  for (int i = 0; i < imps.count; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_raw(j, "{");
    json_imp(j, &imps.data[i]);
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "ok");
  cwed_json_printf(j, "%d", n_ok);
  cwed_json_key(j, "errors");
  cwed_json_printf(j, "%d", n_err);
  cwed_json_key(j, "pending");
  cwed_json_printf(j, "%d", n_pending);
  return true;
}

static bool reimport_exec(cwed_call *c) {
  imp *m = arg_imp(c);
  if (!m) return false;
  run_import(m, false);
  refresh_counts();
  dock_dirty = true;
  json_imp(cwed_reply(c), m);
  return true;
}

/* import.set: the sidecar's text before and after (one undo step) */
typedef struct { const char *path; char *old_t, *new_t; bool old_existed; cw_vfs_layer old_layer; } set_state;

static void set_free(cwed_call *c, void *state) {
  (void)c;
  set_state *s = state;
  if (s) cw_free(s->old_t), cw_free(s->new_t);
  cw_free(s);
}

static void write_text(const char *path, const char *t) {
  if (t) cw_vfs_write(path, t, strlen(t));
  else if (cw_vfs_exists(path)) cw_vfs_remove(path);
  else cw_vfs_revert(path);
}

static void after_change(const char *file) {
  imp *m = find_imp(file);
  if (m) {
    run_import(m, true);
    refresh_counts();
  }
  seen_gen = cw_vfs_generation(); /* our own write: nothing else changed */
  dock_dirty = true;
  cwed_emit(&(cwed_event){ .type = CWED_EV_FILES, .line = file });
}

static bool set_exec(cwed_call *c) {
  set_state *s = cwed_call_state(c);
  if (s) { /* redo */
    write_text(s->path, s->new_t);
    after_change(cwed_arg(c, "path"));
    return true;
  }
  imp *m = arg_imp(c);
  if (!m) return false;
  const char *key = cwed_arg(c, "key"), *value = cwed_arg(c, "value");
  const setting_def *def = NULL;
  for (size_t i = 0; i < CW_ARRAY_LEN(SETTINGS); i++)
    if (!strcmp(SETTINGS[i].kind, settings_kind(m->kind)) && !strcmp(SETTINGS[i].key, key)) def = &SETTINGS[i];
  if (!def) {
    char keys[512] = "";
    for (size_t i = 0; i < CW_ARRAY_LEN(SETTINGS); i++)
      if (!strcmp(SETTINGS[i].kind, settings_kind(m->kind))) snprintf(keys + strlen(keys), sizeof keys - strlen(keys), " %s", SETTINGS[i].key);
    return cwed_fail(c, "%s imports with the %s importer, whose settings are:%s", m->path, settings_kind(m->kind),
                     keys[0] ? keys : " (none)");
  }
  if (*value && def->choices) {
    bool ok = false;
    char ch[128];
    snprintf(ch, sizeof ch, "%s", def->choices);
    for (char *t = strtok(ch, "|"); t; t = strtok(NULL, "|")) ok |= !strcmp(t, value);
    if (!ok) return cwed_fail(c, "%s is one of %s", key, def->choices);
  }
  if (strchr(value, '\n')) return cwed_fail(c, "a setting is one line");
  const char *side = cw_intern(cw_frame_printf("%s.cwimport", m->path));
  s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
  s->path = side;
  s->old_layer = cw_vfs_where(side);
  size_t n;
  char *old = cw_vfs_read(side, &n);
  s->old_existed = old != NULL;
  if (old) s->old_t = cw_strdup(old, CW_MEM_EDITOR);
  /* the new text: the key's line replaced (or removed, or appended), the rest kept */
  CW_ARRAY(char) out = { 0 };
  bool done = false;
  for (char *line = old; line && *line;) {
    char *nl = strchr(line, '\n');
    size_t len = nl ? (size_t)(nl - line) : strlen(line);
    char k[128] = "";
    sscanf(line, " %127[^= \n]", k);
    bool match = line[0] != '#' && !strcmp(k, key) && memchr(line, '=', len);
    if (match) {
      if (*value && !done) {
        const char *l = cw_frame_printf("%s = %s\n", key, value);
        for (const char *q = l; *q; q++) cw_array_push(&out, *q);
      }
      done = true;
    } else {
      for (size_t q = 0; q < len; q++) cw_array_push(&out, line[q]);
      cw_array_push(&out, '\n');
    }
    line = nl ? nl + 1 : NULL;
  }
  if (!done && *value) {
    const char *l = cw_frame_printf("%s%s = %s\n", old ? "" : "# import settings (cwpack sidecar: key = value)\n", key, value);
    for (const char *q = l; *q; q++) cw_array_push(&out, *q);
  }
  cw_array_push(&out, 0);
  free(old);
  if (s->old_t && !strcmp(s->old_t, out.data)) {
    cw_array_free(&out);
    set_free(c, s);
    return cwed_fail(c, "%s is already %s", key, *value ? value : "at its default");
  }
  s->new_t = cw_strdup(out.data, CW_MEM_EDITOR);
  cw_array_free(&out);
  cwed_call_set_state(c, s);
  write_text(side, s->new_t);
  after_change(m->path);
  cwed_json *j = cwed_reply(c);
  json_imp(j, m);
  cwed_json_key(j, "sidecar");
  cwed_json_str(j, side);
  return true;
}
static void set_undo(cwed_call *c) {
  set_state *s = cwed_call_state(c);
  if (!s) return;
  if (s->old_layer == CW_VFS_ASSET) cw_vfs_revert(s->path);
  else write_text(s->path, s->old_existed ? s->old_t : NULL);
  after_change(cwed_arg(c, "path"));
}

/* ── the dock ──────────────────────────────────────────────────────────── */

static cw_node dock, title, status_l, note_l, settings_grid, reimport_b;
static bool syncing;

typedef struct { const char *key; } row_ctx;
static row_ctx rows[16];

static void on_setting(cw_node e, const cw_value *a, int argc, void *user) {
  (void)argc;
  if (syncing || !shown_path[0]) return;
  const row_ctx *r = user;
  const char *v;
  if (cw_node_is(e, "OptionButton")) {
    const char *def = NULL;
    for (size_t i = 0; i < CW_ARRAY_LEN(SETTINGS); i++)
      if (!strcmp(SETTINGS[i].key, r->key) && SETTINGS[i].choices) def = SETTINGS[i].choices;
    if (!def) return;
    char ch[128];
    snprintf(ch, sizeof ch, "%s", def);
    int i = 0;
    v = "";
    for (char *t = strtok(ch, "|"); t; t = strtok(NULL, "|"), i++)
      if (i == a[0].i) v = cw_frame_printf("%s", t);
  } else if (cw_node_is(e, "CheckBox")) {
    v = a[0].b ? "true" : "false";
  } else {
    v = a[0].s ? a[0].s : "";
  }
  cwed_runf("import.set path=%s key=%s value=%s", cwed_quote(shown_path), r->key, cwed_quote(v));
}
static void on_reimport(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc, (void)user;
  if (shown_path[0]) cwed_runf("import.reimport path=%s", cwed_quote(shown_path));
}

static void rebuild_dock(void) {
  dock_dirty = false;
  syncing = true;
  cwed_clear_children(settings_grid);
  imp *m = shown_path[0] ? find_imp(shown_path) : NULL;
  if (!m) {
    cw_label_set_text(title, shown_path[0] ? shown_path : "Import");
    cw_label_set_text(status_l, shown_path[0] ? "no import step for this file" : "Select a file in the FileSystem panel.");
    cw_label_set_text(note_l, "");
    cw_button_set_disabled(reimport_b, true);
    syncing = false;
    return;
  }
  cw_button_set_disabled(reimport_b, false);
  cw_label_set_text(title, m->path);
  cw_control_set_tooltip(title, m->path);
  cw_label_set_text(status_l, m->status == ST_ERROR ? cw_frame_printf("Import failed: %s", m->msg)
                              : m->status == ST_OK  ? cw_frame_printf("Imported (%s importer)", settings_kind(m->kind))
                                                    : "Waiting to import...");
  cw_node_set(status_l, "theme_overrides",
              cw_val_string(m->status == ST_ERROR ? "font_color = #ff7a6e" : m->status == ST_OK ? "font_color = #8fd18f" : "font_color = #c9b45a"));
  cw_label_set_text(note_l, m->note);
  cwp_settings set = { 0 };
  read_settings(m->path, &set);
  int r = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(SETTINGS) && r < 16; i++) {
    const setting_def *s = &SETTINGS[i];
    if (strcmp(s->kind, settings_kind(m->kind))) continue;
    const char *v = cwp_settings_get(&set, s->key, s->def);
    cw_node l = cw_node_new("Label", NULL);
    cw_label_set_text(l, s->key);
    cw_control_set_tooltip(l, s->doc);
    cw_node_set(l, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
    cw_control_set_size_flags(l, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
    cw_node_add_child(settings_grid, l);
    rows[r].key = s->key;
    cw_node e;
    if (s->choices && !strcmp(s->choices, "true|false")) {
      e = cw_node_new("CheckBox", NULL);
      cw_button_set_text(e, "On");
      cw_button_set_pressed(e, !strcmp(v, "true"));
      cw_connect(e, "toggled", (cw_node){ 0 }, NULL, on_setting, &rows[r], 0);
    } else if (s->choices) {
      e = cw_node_new("OptionButton", NULL);
      char ch[128];
      snprintf(ch, sizeof ch, "%s", s->choices);
      int k = 0, at = 0;
      for (char *t = strtok(ch, "|"); t; t = strtok(NULL, "|"), k++) {
        cw_option_button_add_item(e, t);
        if (!strcmp(t, v)) at = k;
      }
      cw_option_button_select(e, at);
      cw_connect(e, "item_selected", (cw_node){ 0 }, NULL, on_setting, &rows[r], 0);
    } else {
      e = cw_node_new("LineEdit", NULL);
      cw_line_edit_set_text(e, v);
      cw_connect(e, "text_submitted", (cw_node){ 0 }, NULL, on_setting, &rows[r], 0);
    }
    cw_control_set_tooltip(e, s->doc);
    cw_control_set_size_flags(e, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
    cw_node_add_child(settings_grid, e);
    r++;
  }
  cwp_settings_free(&set);
  syncing = false;
}

static cw_node create(void *user) {
  (void)user;
  dock = cw_node_new("VBoxContainer", NULL);
  title = cw_node_new("Label", "File");
  cw_node_set(title, "ellipsis", cw_val_bool(true));
  status_l = cw_node_new("Label", "Status");
  cw_node_set(status_l, "autowrap", cw_val_bool(true));
  note_l = cw_node_new("Label", "Note");
  cw_node_set(note_l, "autowrap", cw_val_bool(true));
  cw_node_set(note_l, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  settings_grid = cw_node_new("GridContainer", "Settings");
  cw_node_set(settings_grid, "columns", cw_val_int(1)); /* label over editor: docks are narrow */
  cw_control_set_size_flags(settings_grid, CW_SIZE_EXPAND_FILL, CW_SIZE_FILL);
  reimport_b = cw_node_new("Button", "Reimport");
  cw_button_set_text(reimport_b, "Re-import");
  cw_control_set_tooltip(reimport_b, "Run the importer again (import.reimport)");
  cw_node_set(reimport_b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_connect(reimport_b, "pressed", (cw_node){ 0 }, NULL, on_reimport, NULL, 0);
  cw_node_add_child(dock, title);
  cw_node_add_child(dock, status_l);
  cw_node_add_child(dock, note_l);
  cw_node_add_child(dock, settings_grid);
  cw_node_add_child(dock, reimport_b);
  rebuild_dock();
  return dock;
}

/* the FileSystem's selection arrives as its command line: fs.select path=<p> */
static void on_event(const cwed_event *e, void *user) {
  (void)user;
  if (e->type == CWED_EV_COMMAND && e->ok && e->line && !strncmp(e->line, "fs.select ", 10)) {
    const char *p = strstr(e->line, "path=");
    if (!p) return;
    p += 5;
    char v[512];
    size_t k = 0;
    if (*p == '"') {
      for (p++; *p && *p != '"' && k < sizeof v - 1; p++) {
        if (*p == '\\' && p[1]) p++;
        v[k++] = *p;
      }
    } else {
      while (*p && *p != ' ' && k < sizeof v - 1) v[k++] = *p++;
    }
    v[k] = 0;
    snprintf(shown_path, sizeof shown_path, "%s", strcmp(v, ".") ? v : "");
    dock_dirty = true;
  } else if (e->type == CWED_EV_PROJECT) {
    shown_path[0] = 0;
    seen_gen = 0;
    dock_dirty = true;
  }
}

static int32_t f_ok, f_err, f_pending, f_runs;
static uint32_t f_hash;

static void frame(double dt) {
  (void)dt;
  pump();
  if (dock_dirty) rebuild_dock();
  f_ok = n_ok, f_err = n_err, f_pending = n_pending, f_runs = imports_run, f_hash = results_hash;
}

static void init(void) {
  cwed_panel_register(&(cwed_panel_desc){ .id = "import", .title = "Import", .slot = "right_top", .order = 10, .create = create });
  cwed_subscribe(CWED_EV_COMMAND | CWED_EV_PROJECT, on_event, NULL);
  cw_debug_i32("import.ok", &f_ok);
  cw_debug_i32("import.errors", &f_err);
  cw_debug_i32("import.pending", &f_pending);
  cw_debug_i32("import.runs", &f_runs);
  cw_debug_u32("import.hash", &f_hash);
  static const cwed_param path_p[] = { { "path", CWED_ARG_STRING, NULL, "project path of an importable file" }, { 0 } };
  static const cwed_param set_p[] = {
    { "path", CWED_ARG_STRING, NULL, "project path of an importable file" },
    { "key", CWED_ARG_STRING, NULL, "setting (import.info lists them)" },
    { "value", CWED_ARG_STRING, "", "new value; empty = back to the default (the line leaves the sidecar)" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "import.info", .title = "Import Status", .params = path_p,
    .flags = CWED_CMD_QUERY,
    .doc = "A file's import: importer, status (ok / error with the importer's message / pending), sizes, its settings and "
           "where they are stored (<file>.cwimport).", .exec = info_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "import.list", .title = "Import Status of All Files",
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "Every importable file's status, with ok / errors / pending counts.",
    .exec = list_exec });
  cwed_command_register(&(cwed_command_desc){ .name = "import.set", .title = "Set Import Setting", .params = set_p,
    .flags = CWED_CMD_UNDO, .example = "import.set path=textures/grid.png key=texture.mips value=false",
    .doc = "Change one import setting in the file's sidecar (<file>.cwimport, key = value lines: the same file cwpack reads "
           "at build time) and re-import. One undo step.",
    .exec = set_exec, .undo = set_undo, .free_state = set_free });
  cwed_command_register(&(cwed_command_desc){ .name = "import.reimport", .title = "Re-import File", .params = path_p,
    .doc = "Run the file's importer again now (bypassing the import cache).", .exec = reimport_exec });
}

CWED_MODULE(import) = { .title = "Import", .milestone = "M4.2", .init = init, .frame = frame };

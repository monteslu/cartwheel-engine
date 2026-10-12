/* cwpack internals: settings, file helpers, importers. */
#ifndef CWPACK_H
#define CWPACK_H

#include <stdio.h>

#include "cartwheel/core.h"

#define CWP_PATH_MAX 2048

/* ── settings: "key = value" pairs (project "set" lines, then a file's
 *    .cwimport sidecar on top) ────────────────────────────────────── */
typedef struct cwp_kv { char *key, *value; } cwp_kv;
typedef struct cwp_settings { cwp_kv *kv; int count, cap; } cwp_settings;

void cwp_settings_set(cwp_settings *s, const char *key, const char *value);
const char *cwp_settings_get(const cwp_settings *s, const char *key, const char *def);
bool cwp_settings_bool(const cwp_settings *s, const char *key, bool def);
double cwp_settings_num(const cwp_settings *s, const char *key, double def);
void cwp_settings_copy(cwp_settings *dst, const cwp_settings *src);
void cwp_settings_free(cwp_settings *s);
/* "k=v\n" for every key starting with one of the prefixes, sorted: what an
 * importer's output depends on (the cache key hashes it). */
char *cwp_settings_canon(const cwp_settings *s, const char *const *prefixes, int n);

/* ── files ───────────────────────────────────────────────────────── */
uint8_t *cwp_read_file(const char *path, size_t *len); /* cw_alloc'd + NUL; NULL when missing */
bool cwp_write_file(const char *path, const void *data, size_t len); /* atomic (temp + rename), makes dirs */
bool cwp_mkdirs(const char *dir);
bool cwp_exists(const char *path);
void cwp_dirname(const char *path, char *out, size_t cap); /* "a/b/c.txt" -> "a/b/" ("" for "c.txt") */
/* join dir + relative and normalize "." / ".." segments (on the string) */
void cwp_join(const char *dir, const char *rel, char *out, size_t cap);
/* absolute form of a path (for sources.txt / depfiles) */
void cwp_absolute(const char *path, char *out, size_t cap);
void cwp_uri_decode(const char *s, char *o, size_t cap);

/* ── messages ────────────────────────────────────────────────────
 * Importers report problems through cwp_report: by default "cwpack: <msg>"
 * on stderr; an editor running the importers in process installs a sink
 * (one message per call, no trailing newline). Main thread. */
void cwp_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cwp_set_reporter(void (*fn)(const char *msg, void *user), void *user);

/* ── importers ───────────────────────────────────────────────────── */
#define CWP_IMPORT_GLTF_VERSION 3
#define CWP_IMPORT_TEXTURE_VERSION 2
#define CWP_IMPORT_COPY_VERSION 1
#define CWP_IMPORT_AUDIO_VERSION 1
#define CWP_IMPORT_L10N_VERSION 1
#define CWP_IMPORT_FONT_VERSION 1

/* glTF/GLB -> optimized GLB. read() fetches files beside the source (buffers);
 * every file it read is reported to note(). */
typedef void *(*cwp_read_fn)(void *ctx, const char *uri, size_t *len);
typedef struct cwp_gltf_stats {
  int prims, prims_optimized, lods, vertices_in, vertices_out, triangles, lod_triangles, compressed_views, images_ktx2;
} cwp_gltf_stats;
uint8_t *cwp_import_gltf(const char *name, const uint8_t *src, size_t len, cwp_read_fn read, void *ctx,
                         const cwp_settings *set, size_t *out_len, cwp_gltf_stats *st);

/* The sub-resources the engine registers for a glTF/GLB (see
 * cartwheel/resource.h): cb(name, type) per "mesh/<m>/<p>" (type "mesh"),
 * "material/<i>" and "texture/<i>", in that order; also every external
 * file it references (buffers and images, by URI as written) through
 * file_cb when not NULL. false when the bytes are not glTF 2.0. */
bool cwp_gltf_subresources(const uint8_t *src, size_t len, void (*cb)(const char *name, const char *type, void *user),
                           void (*file_cb)(const char *uri, void *user), void *user);

/* Image -> itself (texture.format = keep) or KTX2 (texture.format = ktx2).
 * srgb: color data (base color, emissive) vs linear (normals, ORM). */
uint8_t *cwp_import_texture(const char *name, const uint8_t *src, size_t len, const cwp_settings *set, bool srgb,
                            size_t *out_len);
/* WAV/FLAC/MP3 -> itself (audio.format = keep) or Ogg Opus (audio.format =
 * opus). note: a one-line summary for the log (may be NULL). */
uint8_t *cwp_import_audio(const char *name, const uint8_t *src, size_t len, const cwp_settings *set,
                          size_t *out_len, char *note, size_t note_cap);
/* TrueType -> the same font with only the glyphs cps reach (plus their GSUB
 * closure and composite components) and no variation tables (import_font.c). */
uint8_t *cwp_subset_font(const char *name, const uint8_t *src, size_t len, const uint32_t *cps, int ncp, size_t *out_len,
                         char *note, size_t note_cap);
/* RGBA8 -> KTX2 through Basis Universal (basis_enc.cpp). */
typedef struct cwp_basis_opts { bool uastc, srgb, mips; int quality, effort; } cwp_basis_opts;
/* Once, before encoding from several threads. */
void cwp_basis_init(void);
bool cwp_basis_encode(const uint8_t *rgba, int w, int h, const cwp_basis_opts *o, uint8_t **out, size_t *out_len,
                      char *err, size_t err_cap);

#endif

/* cartwheel resources: shared, immutable-at-runtime data (textures, meshes,
 * materials, glTF scenes, audio clips, fonts, JSON/text blobs, and whatever
 * other modules register) loaded once and shared by everyone who asks.
 *
 * Addressing. A resource has three names:
 *   - a handle (cw_res): generational, what code holds; stale reads as NULL;
 *   - a path: "res://models/duck.glb" or "models/duck.glb" (the same thing),
 *     relative to the asset root (a .cwpak natively, the cart bundle in a
 *     cart). Files read through the project layer first (cartwheel/vfs.h:
 *     a native project directory, files written or imported into memory),
 *     so an editor's project and a cart's imported files load too. A '#' addresses a sub-resource of a loaded file:
 *       "models/duck.glb#mesh/0/1"   mesh 0, primitive 1 (a cw_mesh)
 *       "models/duck.glb#material/2" glTF material 2 (a t3_material)
 *       "models/duck.glb#texture/0"  glTF texture 0 (a t3_texture)
 *   - a UID: a stable 64-bit id that survives renames. cwpack writes
 *     "res.index" (uid, type, path per line) into every pack; a file's UID
 *     comes from its .cwimport sidecar (uid=0x...), else the hash of its path
 *     (cw_res_path_uid).
 *
 * Lifetime. Every load returns a NEW reference (refcount + 1); release it with
 * cw_res_release. Loading a path that is already loaded (or still loading)
 * returns the same resource. When the last reference goes, the resource is
 * not freed at once: it waits in a deferred-free queue for
 * CW_RES_FREE_DELAY_FRAMES frame boundaries (the GPU may still be drawing
 * with it), and a load of the same path in that window revives it.
 *
 * Loading. cw_res_load is synchronous: it returns with the resource READY or
 * FAILED. cw_res_load_async reads and decodes the file in the background and
 * finishes it on the main thread in cw_res_frame (three.c objects, GL
 * uploads) under a per-frame upload budget:
 *   - read: natively on the job pool, together with the decode, so a
 *     request costs the main thread nothing but bookkeeping. In a cart the
 *     host's asset calls belong to the main thread (a lazily loaded web cart
 *     suspends there while the bytes arrive), so cw_res_frame reads at most
 *     a few queued files per frame (cw_res_set_read_budget). Files of the
 *     project layer (cartwheel/vfs.h) and of types with a prepare step (glTF)
 *     are read at request time, as before.
 *   - decode: on the job pool, at most cw_res_set_max_in_flight at once,
 *     started in finish order, so a late urgent request is not stuck behind
 *     a hundred early ones.
 *   - finish: in ORDER, always: by lane (lower first), then priority (higher
 *     first), then request order (cw_res_load_async_ex). A resource whose
 *     decode ended early waits for the ones before it, so the frame a
 *     resource becomes ready and every id three.c hands out are the same on
 *     every host (the order is a pure function of the requests). Plain
 *     cw_res_load_async is lane 0, priority 0: request order.
 * In a deterministic run (cw_is_deterministic_run) the budget counts bytes
 * only and cw_res_frame waits for the next decode instead of skipping a
 * frame, so readiness never depends on thread timing; otherwise a
 * millisecond budget applies too.
 *
 * Cancelling. Releasing the last reference of a load in flight cancels it
 * without waiting: work not started is dropped, a decode already running
 * finishes on its worker and its output is discarded when it lands.
 *
 * Residency. With cw_res_set_residency_budget, a resource whose last
 * reference goes stays loaded (READY, 0 references) in an LRU cache while
 * the cache fits the budget; a load of its path revives it at once (a cache
 * hit). The least recently released go first when the cache is over budget.
 *
 * Threading: every function here is main-thread only, except the type
 * callbacks documented as worker-safe.
 *
 * Memory: CW_MEM_RESOURCE (bookkeeping, decoded data). Payload sizes are in
 * cw_res_stats. */
#ifndef CW_RESOURCE_H
#define CW_RESOURCE_H

#include "audio.h" /* cw_sound, for cw_res_audio_sound */
#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_res);

typedef enum {
  CW_RES_STATE_NONE,    /* null or stale handle */
  CW_RES_STATE_LOADING, /* queued, decoding, or waiting for its upload turn */
  CW_RES_STATE_READY,
  CW_RES_STATE_FAILED,  /* the log says why; the handle stays valid */
} cw_res_state;

/* Resource types. The engine registers the built-ins in this order at
 * startup, so their ids are fixed; other modules register more
 * (cw_res_register_type). 0 = "infer from the path's extension". */
typedef uint32_t cw_res_type;
enum {
  CW_RES_TYPE_AUTO = 0,
  CW_RES_TEXTURE,     /* .png .jpg .jpeg .ktx2      -> t3_texture *      */
  CW_RES_MESH,        /* glTF sub-resource          -> cw_mesh *         */
  CW_RES_MATERIAL,    /* glTF sub-resource          -> t3_material *     */
  CW_RES_GLTF,        /* .glb .gltf                 -> cw_gltf *         */
  CW_RES_AUDIO_CLIP,  /* .wav .ogg .opus .mp3 .flac .xm .mod .s3m .it -> cw_audio_clip * */
  CW_RES_FONT,        /* .ttf .otf                  -> cw_blob *         */
  CW_RES_JSON,        /* .json (validated at load)  -> cw_blob *         */
  CW_RES_TEXT,        /* .txt .csv .md .cfg .ini .cwproj .cwscene -> cw_blob * */
  CW_RES_BINARY,      /* anything else (.bin)       -> cw_blob *         */
  CW_RES_BUILTIN_COUNT
};

#define CW_RES_FREE_DELAY_FRAMES 2

/* ── loading ──────────────────────────────────────────────────────── */

/* Synchronous load (or a new reference to the loaded one). Never returns a
 * null handle for a well-formed path: check cw_res_state / cw_res_get. A
 * missing file FAILS with "res://x: not found (asset root: ...)" in the log. */
cw_res cw_res_load(const char *path, cw_res_type type);

/* Called on the main thread from cw_res_frame when an async load becomes
 * READY or FAILED (also for a cache hit: then at the next cw_res_frame, never
 * from inside cw_res_load_async). */
typedef void (*cw_res_done_fn)(cw_res res, cw_res_state state, void *user);

/* Asynchronous load. done may be NULL. A '#' sub-resource path loads its file
 * asynchronously and resolves the sub-resource when the file is ready. */
cw_res cw_res_load_async(const char *path, cw_res_type type, cw_res_done_fn done, void *user);

/* Where an async load stands in the finish order (see Loading above). The
 * order must come from game state (distance from the player in fixed-tick
 * positions, a level's needs), never from timing. */
typedef struct cw_res_load_opts {
  int lane;              /* lower lanes finish first (0 = default; e.g. 0 for what the sim needs, 1 for visuals) */
  int priority;          /* within a lane, higher finishes first (0 = default) */
  cw_res_done_fn done;   /* may be NULL */
  void *user;
} cw_res_load_opts;
/* cw_res_load_async with a lane and priority (NULL opts = lane 0, priority
 * 0). Requesting a path already loading with a better (lane, priority)
 * moves it up; never down. A '#' sub-resource and the files a type's
 * prepare step depends on load with the same lane and priority. */
cw_res cw_res_load_async_ex(const char *path, cw_res_type type, const cw_res_load_opts *opts);

/* Load by UID through res.index. A UID the index does not know FAILS. */
cw_res cw_res_load_uid(uint64_t uid, cw_res_type type);

/* The UID table cw_res_load_uid resolves through: res.index entries, plus
 * any added at run time. An editor working on a project that has no pack
 * (so no res.index) indexes its files with this: each file's UID from its
 * .cwimport sidecar, else cw_res_path_uid. Setting a path again replaces
 * its UID. */
void cw_res_index_set(uint64_t uid, const char *path);
/* The indexed path of a UID (interned), NULL when unknown. */
const char *cw_res_index_path(uint64_t uid);

/* Block (running jobs meanwhile) until res is READY or FAILED. Finishes
 * every async load requested before it too, in order. */
cw_res_state cw_res_wait(cw_res res);
void cw_res_wait_all(void); /* finish every queued async load now, in request order (main thread) */

/* ── groups: loads started together (a level's preload, a loading screen) ──
 * A group holds one reference to each resource added; freeing it releases
 * them (they stay cached while anything else holds them). */
typedef struct cw_res_group cw_res_group;
cw_res_group *cw_res_group_new(void); /* empty group, cw_alloc'd; free with cw_res_group_free */
/* Start an async load and keep it in the group. */
cw_res cw_res_group_add(cw_res_group *g, const char *path, cw_res_type type);
/* Everything a scene needs, from the "<scene>.preload" list cwpack writes
 * beside every .cwscene (sub-scenes' needs first, the scene last), all
 * started now; without a list, just the file. Returns how many it added. */
int cw_res_preload(cw_res_group *g, const char *scene_path);
typedef struct cw_res_group_progress {
  int total, ready, failed, loading;
  float fraction;     /* (ready + failed) / total: a loading bar */
  int64_t bytes;      /* payload bytes ready */
  int64_t file_bytes; /* packed bytes read for them */
} cw_res_group_progress;
/* O(group size); fraction is 1 for an empty group */
void cw_res_group_get_progress(const cw_res_group *g, cw_res_group_progress *out);
bool cw_res_group_done(const cw_res_group *g); /* nothing still loading */
void cw_res_group_wait(cw_res_group *g);       /* finish the rest now, in request order */
int cw_res_group_count(const cw_res_group *g); /* loads added (cw_res_group_add and cw_res_preload) */
/* the group's own reference (do not release); null when i is out of range */
cw_res cw_res_group_at(const cw_res_group *g, int i);
/* releases every reference the group holds, then frees it; NULL is a no-op */
void cw_res_group_free(cw_res_group *g);

/* ── references ───────────────────────────────────────────────────── */
void cw_res_add_ref(cw_res res);
void cw_res_release(cw_res res); /* null handle: no-op */
int cw_res_ref_count(cw_res res); /* 0 for a null or stale handle */

/* ── queries (no reference taken) ─────────────────────────────────── */
cw_res cw_res_find(const char *path);     /* loaded / loading / pending free, else null */
cw_res cw_res_find_uid(uint64_t uid); /* by the resource's uid, loaded only (no index lookup); null when absent */
cw_res_state cw_res_get_state(cw_res res); /* CW_RES_STATE_NONE for a null or stale handle */
bool cw_res_is_ready(cw_res res); /* state is READY */
/* The payload (see the type list above), NULL unless READY. */
void *cw_res_get(cw_res res);
cw_res_type cw_res_type_of(cw_res res); /* 0 for a null or stale handle */
const char *cw_res_path(cw_res res);      /* canonical, no "res://"; interned */
uint64_t cw_res_uid(cw_res res); /* 0 for a null or stale handle */
/* Bumped every time the resource is (re)loaded: code that cached something
 * derived from it compares versions. */
uint32_t cw_res_version(cw_res res);
/* The UID a path gets without a sidecar (FNV-1a 64 of the canonical path;
 * identical in cwpack). */
uint64_t cw_res_path_uid(const char *path);

/* ── frame boundary, budget, stats ────────────────────────────────── */

/* The platform calls this at the start of every frame: frees resources whose
 * delay ran out, finishes decoded async loads in request order under the
 * budget, and runs completion callbacks. Safe to call more often. */
void cw_res_frame(void);
/* Per-frame upload budget for async loads: stop finishing resources once
 * `bytes` (payload bytes: pixels uploaded, vertex data built) or `ms`
 * (ignored in deterministic runs) is spent. At least one resource finishes
 * per frame whatever its size. Defaults: 32 MB, 4 ms. 0 = unlimited. */
void cw_res_set_upload_budget(size_t bytes, double ms);
/* Cart reads per frame: at most `files` queued files (at least 1 per frame
 * while any wait), and stop once `ms` is spent (ignored in deterministic
 * runs, where exactly `files` are read). Defaults: 4 files, 2 ms. Natively
 * reads run on the job pool and this does nothing. */
void cw_res_set_read_budget(int files, double ms);
/* Decodes (and native reads) running on the job pool at once; more wait
 * their turn, started in finish order. Default (0): twice the job threads,
 * at least 2. */
void cw_res_set_max_in_flight(int n);
/* The residency cache's size: released READY resources stay loaded while
 * their bytes (payload bytes, or file bytes for types that report none; in
 * a deterministic run always file bytes, which are the same on every host)
 * fit. 0 (default) = no cache: the last release frees after
 * CW_RES_FREE_DELAY_FRAMES. Lowering it evicts at the next cw_res_frame. */
void cw_res_set_residency_budget(size_t bytes);

typedef struct cw_res_stats {
  int live;          /* slots in use (any state, pending free included) */
  int ready, loading, failed, pending_free;
  int64_t bytes;     /* payload bytes of READY resources (CPU + GPU estimate) */
  uint64_t loads;    /* load calls, cache hits included */
  uint64_t cache_hits;
  uint64_t decoded;  /* files decoded (sync + async) */
  uint64_t freed;
  uint64_t uploaded_bytes_last_frame;
  int finished_last_frame;
  int by_type[32];   /* READY resources per type id (first 32 types) */
  /* load cost so far (wall clock: for reports, never for game logic) */
  double read_ms;    /* reading files (main thread; a lazily loaded web cart waits on the network here) */
  double decode_ms;  /* decoding (job pool, summed over workers) */
  double finish_ms;  /* finishing: three.c objects, GPU uploads (main thread) */
  int64_t file_bytes; /* packed bytes read */
  int64_t cpu_bytes; /* CW_MEM_RESOURCE now: bookkeeping, decoded data, kept files */
  /* the background pipeline and the residency cache */
  int waiting;       /* async loads not started yet (a cart: not read yet) */
  int in_flight;     /* reads + decodes running on the job pool */
  int cached;        /* READY with no references, kept by the residency budget */
  int64_t cached_bytes; /* their size as the budget counts it */
  uint64_t evicted;  /* cached resources freed to fit the budget */
  uint64_t cancelled; /* async loads released before they finished */
} cw_res_stats;
void cw_res_get_stats(cw_res_stats *out); /* walks every slot: O(live resources) */

/* One resource's numbers. Times are wall clock (informational). */
typedef struct cw_res_info {
  cw_res_type type;
  cw_res_state state;
  const char *path;
  uint64_t uid;
  uint32_t version;
  int refs;
  size_t file_bytes;           /* packed size read for the last (re)load */
  int64_t bytes;               /* payload (CPU + GPU estimate) */
  double read_ms, decode_ms, finish_ms;
  double total_ms;             /* request to READY */
  int frames_to_ready;         /* frame boundaries between request and READY */
} cw_res_info;
/* false (out zeroed) for a null or stale handle; path is interned */
bool cw_res_get_info(cw_res res, cw_res_info *out);
/* Log a load report: totals, then one line per loaded file (sizes, read /
 * decode / finish times, time and frames to ready). The "res" debug command
 * returns the same numbers as JSON. */
void cw_res_log_report(void);

/* ── compression ──────────────────────────────────────────────────────
 * A file whose path ends in ".zst" (a zstd frame) or ".lz4" (an lz4 frame,
 * what the zstd and lz4 tools and Node's zlib write) loads as the file
 * without that suffix: "cells/3_4.cell.zst" is a ".cell" resource whose
 * bytes are decompressed on the job pool with the decode (natively, the read
 * too), so a cart can ship compressed files with no help from its host.
 * Natively, cwpack also compresses pack entries (pack.compress). Both
 * decoders are BSD licensed (NOTICE); zstd is the one KTX2 supercompression
 * already uses. */
typedef enum { CW_COMPRESS_NONE, CW_COMPRESS_ZSTD, CW_COMPRESS_LZ4 } cw_compression;
/* The compression a path's suffix names (".zst", ".lz4"), else NONE. */
cw_compression cw_compression_of(const char *path);
/* A whole buffer decompressed: a zstd frame (with or without its content
 * size) or an lz4 frame; NONE copies. cw_alloc'd (CW_MEM_RESOURCE) with a
 * NUL after the *out_len bytes; NULL for corrupt or truncated input. Any
 * thread. Cost: zstd decodes at roughly 1 GB/s, lz4 at several GB/s
 * natively (a cart runs at about half that). */
uint8_t *cw_decompress(cw_compression c, const void *src, size_t len, size_t *out_len);
/* Into dst, whose size the caller knows (raw_len): a zstd frame, or a bare
 * lz4 BLOCK (no frame: what a cwpak entry stores). False unless exactly
 * raw_len bytes came out. Any thread. */
bool cw_decompress_block(cw_compression c, const void *src, size_t len, void *dst, size_t raw_len);

/* Free everything now (the platform calls it at shutdown, before the GL
 * context goes). Handles all go stale. */
void cw_res_shutdown(void);

/* ── resource types (for modules that add their own) ──────────────── */

/* What a decode callback gets: the file's bytes plus any extra files and
 * dependencies its prepare callback asked for. */
typedef struct cw_res_decode_args {
  const char *path;          /* canonical */
  const uint8_t *bytes;      /* the file (NUL-terminated, not counted) */
  size_t len;
  int extra_count;           /* files added with cw_res_prepare_read */
  const char *const *extra_paths;
  const uint8_t *const *extra_bytes;
  const size_t *extra_lens;
  void *type_user;
  /* set *take_bytes = true to keep `bytes` as part of the result (free it
   * later with cw_free) instead of copying it */
  bool *take_bytes;
} cw_res_decode_args;

typedef struct cw_res_prepare cw_res_prepare; /* opaque, valid inside prepare only */
/* From prepare: load another resource first (its own async load; this one
 * finishes after it). Returns its handle (this resource holds the reference
 * until it is freed). */
cw_res cw_res_prepare_dep(cw_res_prepare *p, const char *path, cw_res_type type);
/* From prepare: read another file now (main thread) and hand its bytes to
 * decode. false when it does not exist. */
bool cw_res_prepare_read(cw_res_prepare *p, const char *path);
/* The file being prepared. */
const uint8_t *cw_res_prepare_bytes(cw_res_prepare *p, size_t *len);
const char *cw_res_prepare_path(cw_res_prepare *p); /* canonical, interned */

typedef struct cw_res_type_desc {
  const char *name;        /* "texture"; unique */
  const char *extensions;  /* ".png .jpg" (lower case) for CW_RES_TYPE_AUTO */
  /* main thread, optional: called with the file read, before decode; asks
   * for dependencies / extra files. false = FAILED (log why). */
  bool (*prepare)(cw_res_prepare *p, void *type_user);
  /* WORKER-SAFE: bytes -> decoded intermediate. No GL, no three.c object
   * creation (three.c ids must be handed out in order on one thread), no
   * cw_res_* calls. NULL = FAILED (log why). *cost = payload bytes the
   * finish step will upload (for the budget). */
  void *(*decode)(const cw_res_decode_args *a, size_t *cost);
  /* main thread: decoded -> payload (create three.c objects, upload). Owns
   * decoded either way. deps[i] are the prepare dependencies, READY or
   * FAILED. NULL = FAILED. */
  void *(*finish)(void *decoded, cw_res self, const cw_res *deps, int dep_count, void *type_user);
  /* main thread: free a payload. */
  void (*unload)(void *payload, void *type_user);
  /* main thread, optional: drop a decoded intermediate never finished
   * (the load was released before it finished). */
  void (*discard)(void *decoded, void *type_user);
  /* main thread, optional: hot reload. Move fresh into payload IN PLACE so
   * pointers others hold stay valid, then free what is left of fresh.
   * Without it, the handle's payload pointer is swapped and fresh's old
   * payload freed (holders re-read cw_res_get; cw_res_version tells). */
  void (*reload)(void *payload, void *fresh, void *type_user);
  /* optional: payload bytes for stats */
  size_t (*size)(const void *payload, void *type_user);
  void *type_user;
  /* main thread, optional: called at every load request of this type,
   * before the file is read (on a worker, for an async load without a
   * prepare step): set up what decode needs from the main thread (the
   * texture type picks its KTX2 target here). A type with prepare has its
   * file read on the main thread at request time instead. */
  void (*request)(void *type_user);
} cw_res_type_desc;

/* Register a type; returns its id. Registering a name twice returns the
 * existing id (the desc is not replaced). Main thread, before loading. */
cw_res_type cw_res_register_type(const cw_res_type_desc *desc);
cw_res_type cw_res_type_find(const char *name); /* 0 when unknown */
const char *cw_res_type_name(cw_res_type type); /* registered name; "none" when unknown */

/* A resource built from memory instead of a file (procedural content, tests,
 * the editor). Synchronous; path names it in the cache ("gen://noise" etc.;
 * loading that path again returns it). */
cw_res cw_res_create(const char *path, cw_res_type type, const void *bytes, size_t len);

/* Replace a loaded resource's data with new file bytes (hot reload, or the
 * editor/romdev pushing a changed asset). Synchronous; the handle stays
 * valid, cw_res_version goes up, glTF instances are re-linked. false (and
 * the old data kept) if the new bytes fail to decode. */
bool cw_res_reload_from_memory(cw_res res, const void *bytes, size_t len);
/* Reread the file from the asset root (after cw_asset_remount) and reload. */
bool cw_res_reload(cw_res res);

/* Sub-resources: a type's finish step registers payloads it owns under
 * "<path>#<name>". They live as long as the parent; a reference to one keeps
 * the parent loaded. Returns the sub-resource handle. */
cw_res cw_res_register_sub(cw_res parent, const char *name, cw_res_type type, void *payload);

/* ── built-in payload types ───────────────────────────────────────── */

/* Raw bytes (fonts, JSON, text, binary). data is NUL-terminated (not counted). */
typedef struct cw_blob {
  const uint8_t *data;
  size_t len;
} cw_blob;

/* Audio clip: the encoded file plus what the audio module's decoder found
 * (cartwheel/audio.h decodes it; probed on a worker at load). Play it with
 * cw_res_audio_sound. frames is 0 when unknown (tracker modules). */
typedef enum {
  CW_AUDIO_UNKNOWN, CW_AUDIO_WAV, CW_AUDIO_OGG_VORBIS, CW_AUDIO_MP3, CW_AUDIO_FLAC, CW_AUDIO_OGG_OPUS, CW_AUDIO_MODULE
} cw_audio_container;
typedef struct cw_audio_clip {
  const uint8_t *data;
  size_t len;
  cw_audio_container container;
  uint32_t sample_rate, channels;
  uint64_t frames;   /* PCM frames, 0 when unknown */
  double seconds;
} cw_audio_clip;

/* ── typed accessors (NULL unless READY and of that type) ─────────── */
struct t3_texture;
struct t3_material;
struct t3_object;
struct t3_geometry;
struct t3_gltf;

/* owned by the resource (valid while referenced). Its colour space is the
 * file's, as three.js r186's loaders give it: a KTX2's DFD transfer function
 * (cwpack's texture.color, sRGB by default), none (data) for PNG / JPEG,
 * unless a glTF that shows the image (as colour or as data) claimed it before
 * it loaded (a glTF that finds it loaded in the other one uploads its own). It
 * is fixed when the resource loads (on GL the texels upload into an sRGB or a
 * linear format then): import a colour image as KTX2, rather than changing
 * color_space on the loaded texture. */
struct t3_texture *cw_res_texture(cw_res res);
struct t3_material *cw_res_material(cw_res res); /* owned by the resource (valid while referenced) */
const cw_blob *cw_res_blob(cw_res res); /* for FONT / JSON / TEXT / BINARY; owned by the resource */
const cw_audio_clip *cw_res_audio(cw_res res); /* owned by the resource */
/* The clip as a cw_sound (cartwheel/audio.h), made on first call through
 * cw_sound_load_memory with desc (later calls return the same sound; desc is
 * ignored then) and freed with the resource. Needs cw_audio_init. A reload
 * frees it: call again after cw_res_version changes. */
cw_sound cw_res_audio_sound(cw_res res, const cw_sound_desc *desc);

/* Textures. Resource textures are GPU-resident: the upload happens when the
 * resource finishes (under the budget) and the RGBA copy is dropped, so
 * t3_texture.pixels is NULL. Do not bump their version (three.c would
 * re-upload from pixels); reload them through the resource system. flip_y is
 * false (glTF / KTX2 orientation: row 0 is the top, at v = 0).
 * KTX2 (Basis Universal ETC1S / UASTC) transcodes at load to the best format
 * the context has: ASTC 4x4, BC7, ETC2, BC3 (S3TC), else RGBA8. */
typedef struct cw_texture_info {
  int width, height, levels;
  uint32_t gl_internal_format; /* GL_RGBA8, GL_COMPRESSED_RGBA_BPTC_UNORM_EXT, ... */
  const char *format;          /* "rgba8", "bc7", "astc4x4", "etc2", "bc3" */
  bool from_ktx2;
  size_t gpu_bytes;            /* all levels */
} cw_texture_info;
bool cw_res_texture_info(cw_res res, cw_texture_info *out); /* false unless a READY texture */
/* The GPU format KTX2 textures transcode to in this context (after the first
 * KTX2 load; "none" before). */
const char *cw_res_ktx2_target(void);

/* Meshes: one glTF primitive, its LOD chain (cwpack's simplifier output;
 * lod[0] is the full mesh) and its material. Every LOD geometry shares the
 * full mesh's vertex attributes; only the index buffer differs. */
#define CW_MESH_MAX_LODS 8
typedef struct cw_mesh {
  struct t3_geometry *lod[CW_MESH_MAX_LODS];
  float lod_error[CW_MESH_MAX_LODS]; /* simplifier error relative to the mesh extent (0 for lod 0) */
  int lod_triangles[CW_MESH_MAX_LODS];
  int lod_count;
  struct t3_material *material;
  float extent;      /* the largest side of lod[0]'s bounding box: lod_error x extent is the error in mesh units */
} cw_mesh;
const cw_mesh *cw_res_mesh(cw_res res); /* owned by the resource (a glTF sub-resource) */

/* glTF scenes. The resource owns the three.c GLTFLoader result; instances
 * share its geometry, materials and textures. An image the glTF names by
 * file is that file's texture resource (shared with everyone who loads it),
 * except for textures whose glTF sampler is not the default (repeat,
 * linear, trilinear): those get their own upload of the file, sampled as
 * the glTF says. */
typedef struct cw_gltf {
  struct t3_gltf *gltf;      /* gltf->scene is the template; do not add it to a scene */
  int mesh_count;            /* sub-resources "#mesh/m/p" */
  int material_count, texture_count;
  uint64_t mesh_hash;        /* FNV-1a 64 over every geometry's attributes + indices, scene order */
  int vertices, triangles;   /* LOD 0, all primitives once */
  int lod_levels;            /* max LOD count over its meshes */
  int64_t bytes;             /* geometry + texture bytes */
} cw_gltf;
const cw_gltf *cw_res_gltf(cw_res res); /* owned by the resource */

/* A new instance of the scene: a Group (yours: add it to a scene, release it
 * with t3_release) whose children copy the template's nodes and share its
 * geometry and materials. Skinned meshes get their own skeleton bound to the
 * instance's bones; animation clips (gltf->animations) bind by node name. An
 * instance holds a reference to the resource until three.c destroys it. After
 * a hot reload its children are rebuilt from the new template (the Group,
 * and so its transform and parent, stays). NULL unless READY. */
struct t3_object *cw_gltf_instantiate(cw_res scene);
/* Draw every mesh of an instance at LOD `level` (clamped per mesh to what
 * it has). Returns the triangles the instance now draws. */
int cw_gltf_instance_set_lod(struct t3_object *instance, int level);
struct t3_camera;
/* Pick each mesh's LOD from the camera: the coarsest level whose simplifier
 * error, projected to the screen, stays within max_error_px pixels of a
 * viewport viewport_height pixels tall (perspective cameras; an
 * orthographic one uses its own scale). Updates the instance's world
 * matrices first. Returns the triangles the instance now draws. Cost: one
 * pass over its meshes. 0 when instance or camera is NULL. */
int cw_gltf_instance_auto_lod(struct t3_object *instance, const struct t3_camera *camera, float viewport_height,
                              float max_error_px);
/* The coarsest LOD any mesh of the instance draws now (0 = full detail). */
int cw_gltf_instance_lod(const struct t3_object *instance);
/* The same for every live instance of every glTF scene, in creation order.
 * Returns the triangles they draw in total. */
int cw_gltf_auto_lod_all(const struct t3_camera *camera, float viewport_height, float max_error_px);
/* Instances alive for a scene resource. */
int cw_gltf_instance_count(cw_res scene);

/* ── hot reload (native dev builds) ───────────────────────────────── */

/* Watch a cwpack project: every `interval_frames` frames (from
 * cw_res_frame) stat the sources cwpack recorded in <out>/sources.txt; when
 * one changed, rerun cwpack on the project (incremental: only changed
 * sources reimport), remount the pack and reload every loaded resource
 * whose packed bytes changed, in place. cwpack is the executable to run
 * (NULL = $CW_CWPACK, else "cwpack" on PATH); args is its full argument
 * string after the executable (the same one the build used). Cart: no-op,
 * returns false. */
bool cw_res_hotreload_watch(const char *cwpack, const char *args, const char *out_dir, int interval_frames);
/* Poll now (tests); returns how many resources reloaded, -1 when cwpack failed. */
int cw_res_hotreload_poll(void);
void cw_res_hotreload_stop(void); /* stop watching and free the watch state; no-op when not watching (or on web) */
/* An app built with cw_add_app(... PACK roots.txt) knows its own cwpack
 * command natively: watch with exactly what the build ran. */
#if defined(CW_PACK_ARGS) && defined(CW_PACK_CWPACK) && defined(CW_PACK_OUT)
#define CW_RES_HOTRELOAD_FROM_BUILD(interval_frames) \
  cw_res_hotreload_watch(CW_PACK_CWPACK, CW_PACK_ARGS, CW_PACK_OUT, interval_frames)
#else
#define CW_RES_HOTRELOAD_FROM_BUILD(interval_frames) false
#endif

#ifdef __cplusplus
}
#endif
#endif

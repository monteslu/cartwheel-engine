/* cartwheel core: memory, containers, strings, random numbers, hashing.
 *
 * Conventions every subsystem follows:
 *   - Long-lived memory goes through cw_alloc/cw_free with a tag, so the
 *     profiler, romdev and the editor can see bytes per subsystem.
 *   - Per-frame scratch comes from the calling thread's frame arena
 *     (cw_frame_alloc): no free, gone at the next frame.
 *   - Objects a user holds are pool entries addressed by generational handles
 *     (cw_pool): a stale handle reads as NULL instead of as another object.
 *   - Iteration order is insertion order everywhere (maps included), so
 *     simulation code that walks containers stays deterministic. */
#ifndef CW_CORE_H
#define CW_CORE_H

#include <string.h>

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── tagged allocation ─────────────────────────────────────────────── */

typedef enum {
  CW_MEM_GENERAL, CW_MEM_CORE, CW_MEM_SCENE, CW_MEM_RENDER, CW_MEM_RENDER2D, CW_MEM_PHYSICS, CW_MEM_AUDIO,
  CW_MEM_ANIM, CW_MEM_UI, CW_MEM_RESOURCE, CW_MEM_SCRIPT, CW_MEM_GAME, CW_MEM_EDITOR, CW_MEM_NAV, CW_MEM_TAG_COUNT
} cw_mem_tag;

/* 16-byte aligned, counted under tag; never NULL (CW_FATAL on out of memory). Thread-safe. */
void *cw_alloc(size_t size, cw_mem_tag tag);
void *cw_calloc(size_t count, size_t size, cw_mem_tag tag); /* cw_alloc, zeroed; no overflow check on count*size */
/* Always moves (alloc, copy, free); p == NULL acts as cw_alloc. Keeps p's original tag; tag is used only
 * when p is NULL. */
void *cw_realloc(void *p, size_t size, cw_mem_tag tag);
void cw_free(void *p); /* NULL is a no-op; asserts on memory not from cw_alloc or freed twice */
char *cw_strdup(const char *s, cw_mem_tag tag); /* caller frees with cw_free; s must not be NULL */

typedef struct cw_mem_stats {
  int64_t bytes[CW_MEM_TAG_COUNT];
  int64_t allocs[CW_MEM_TAG_COUNT];
  int64_t total_bytes, peak_bytes;
} cw_mem_stats;
void cw_mem_get_stats(cw_mem_stats *out); /* snapshot of live bytes/allocs per tag and the peak */
const char *cw_mem_tag_name(cw_mem_tag tag); /* static string ("render", ...); "?" when out of range */

/* ── arenas ──────────────────────────────────────────────────────────
 * Bump allocation in chained blocks; reset frees everything at once.
 * Alignment is 16 (SIMD loads) unless asked otherwise. */
typedef struct cw_arena_block cw_arena_block;
typedef struct cw_arena {
  cw_arena_block *head, *first;
  size_t block_size;
  cw_mem_tag tag;
  size_t used, high_water;
} cw_arena;

void cw_arena_init(cw_arena *a, size_t block_size, cw_mem_tag tag); /* block_size 0 = 64 KiB; allocates lazily */
void *cw_arena_alloc(cw_arena *a, size_t size); /* 16-byte aligned, uninitialized; oversize requests get a block */
void *cw_arena_alloc_aligned(cw_arena *a, size_t size, size_t align); /* align must be a power of two */
void cw_arena_reset(cw_arena *a); /* rewinds to the first block; keeps every block for reuse (frees none) */
void cw_arena_free(cw_arena *a); /* frees every block and zeroes the arena; init again to reuse */

/* Per-thread frame scratch: valid until the calling thread's next frame
 * (the platform advances the frame counter). Zeroed on request only. */
void *cw_frame_alloc(size_t size); /* 16-byte aligned, uninitialized; never NULL */
void *cw_frame_calloc(size_t count, size_t size); /* cw_frame_alloc, zeroed */
/* Formats into frame scratch; the string dies with the frame like any cw_frame_alloc. */
char *cw_frame_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cw_frame_arena_begin(uint64_t frame); /* platform: once per frame, main thread */

/* ── dynamic arrays ──────────────────────────────────────────────────
 *   CW_ARRAY(int) a = {0};  cw_array_push(&a, 5);  a.data[0];  cw_array_free(&a);
 */
#define CW_ARRAY(T) struct { T *data; int count, cap; }
#define cw_array_reserve(a, n) cw__array_grow((void **)&(a)->data, &(a)->cap, (n), sizeof *(a)->data)
#define cw_array_push(a, ...) \
  (cw_array_reserve((a), (a)->count + 1), (a)->data[(a)->count++] = (__VA_ARGS__))
#define cw_array_pop(a) ((a)->data[--(a)->count])
#define cw_array_clear(a) ((a)->count = 0)
#define cw_array_free(a) (cw_free((a)->data), (a)->data = NULL, (a)->count = (a)->cap = 0)
#define cw_array_remove_swap(a, i) ((a)->data[(i)] = (a)->data[--(a)->count])
void cw__array_grow(void **data, int *cap, int need, size_t elem);

/* ── pools with generational handles ─────────────────────────────────
 * Fixed-size elements, stable addresses (allocated in chunks), handles that
 * go stale when their element is freed. Handle 0 is never issued. */
typedef struct cw_pool {
  size_t elem_size;
  int chunk_elems;
  uint8_t **chunks;
  int chunk_count;
  uint32_t *gens;   /* per slot; odd = live */
  uint32_t *free_list;
  int free_count, slot_count, live;
  cw_mem_tag tag;
} cw_pool;

void cw_pool_init(cw_pool *p, size_t elem_size, cw_mem_tag tag); /* elem_size rounds up to 16; 256 per chunk */
void cw_pool_free_all(cw_pool *p); /* frees every chunk; the pool stays initialized and reusable */
/* Returns the new element (zeroed); its handle in *id. */
void *cw_pool_alloc(cw_pool *p, uint64_t *id);
void *cw_pool_get(const cw_pool *p, uint64_t id); /* NULL when stale/null */
bool cw_pool_release(cw_pool *p, uint64_t id);     /* false when stale */
/* Handle of a live element pointer; 0 when not in the pool or freed. O(chunks). */
uint64_t cw_pool_id_of(const cw_pool *p, const void *elem);
/* Iterate live elements in slot order: for (i = 0; i < pool.slot_count; i++)
 * if ((e = cw_pool_at(&pool, i))) ... */
void *cw_pool_at(const cw_pool *p, int slot);

/* ── hash maps (open addressing, insertion-ordered iteration) ─────────
 * Keys are u64; string keys go through cw_hash_str (or interned pointers). */
typedef struct cw_map {
  uint64_t *keys;
  uint64_t *vals;
  int32_t *slots; /* index into keys/vals, -1 empty, -2 tombstone */
  uint8_t *dead;   /* entries removed but not yet compacted (NULL until the first removal) */
  int count, cap, slot_cap, tombstones, dead_count;
  cw_mem_tag tag;
} cw_map;

void cw_map_init(cw_map *m, cw_mem_tag tag); /* empty map; allocates on the first set */
void cw_map_free(cw_map *m); /* frees storage; the map stays initialized (empty) */
void cw_map_set(cw_map *m, uint64_t key, uint64_t val); /* insert or overwrite; overwrite keeps the old position */
bool cw_map_get(const cw_map *m, uint64_t key, uint64_t *val); /* false when absent; val may be NULL */
bool cw_map_remove(cw_map *m, uint64_t key); /* O(1) amortized; keeps the order of the rest */
/* Iterate in insertion order, skipping removed entries:
 *   for (int i = 0; i < m.count; i++) if (cw_map_live(&m, i)) use(m.keys[i], m.vals[i]); */
#define cw_map_live(m, i) (!(m)->dead || !(m)->dead[(i)])
#define cw_map_size(m) ((m)->count - (m)->dead_count)

/* ── strings ─────────────────────────────────────────────────────────── */
/* Interned strings: equal contents, equal pointer. cw_intern pins: the
 * string lives for the process (names, keys, paths, cw_value strings).
 * Text that changes at run time (a label's text, a node's name) takes a
 * counted reference instead: cw_intern_ref returns the same pointer as
 * cw_intern would for equal contents while any reference or pin holds it,
 * and cw_intern_unref drops one; the last one frees it unless it was pinned.
 * cw_intern_unref on a pinned string, a literal or NULL does nothing.
 * Thread-safe (one global lock). */
const char *cw_intern(const char *s);
const char *cw_intern_n(const char *s, size_t n); /* first n bytes of s */
const char *cw_intern_ref(const char *s); /* a counted reference (see above) */
const char *cw_intern_ref_n(const char *s, size_t n); /* cw_intern_ref of the first n bytes of s */
void cw_intern_unref(const char *s); /* drops one counted reference; frees the string at the last */
/* Pin a counted string (it becomes permanent); returns s. Anything else is
 * returned untouched. For a counted string escaping into something copied
 * freely (a cw_value). */
const char *cw_intern_pin(const char *s);
/* The interned pointer for these contents if one is live, else NULL; takes
 * no reference (compare it now, do not keep it). */
const char *cw_intern_find(const char *s);
const char *cw_intern_find_n(const char *s, size_t n); /* cw_intern_find of the first n bytes of s */
uint32_t cw_intern_counted(void); /* live counted (unpinned) strings, for leak checks */

/* ── hashing (stable across hosts: never pointer-based) ───────────────── */
/* FNV-1a 64 plus an avalanche step; same bytes and seed give the same hash on every host. */
uint64_t cw_hash_bytes(const void *data, size_t len, uint64_t seed);
uint64_t cw_hash_str(const char *s); /* cw_hash_bytes(s, strlen(s), 0) */
uint32_t cw_hash32(const void *data, size_t len, uint32_t h); /* FNV-1a, chainable */
#define CW_HASH32_INIT 2166136261u

/* ── random numbers (xoshiro128**, identical on every host) ───────────── */
typedef struct cw_rng { uint32_t s[4]; } cw_rng;
void cw_rng_seed(cw_rng *r, uint64_t seed); /* splitmix64 fill; any seed (0 included) is valid */
uint32_t cw_rng_u32(cw_rng *r); /* full 32-bit range */
float cw_rng_float(cw_rng *r);                 /* [0, 1) */
float cw_rng_range(cw_rng *r, float lo, float hi); /* [lo, hi) */
int cw_rng_int(cw_rng *r, int lo, int hi_inclusive); /* [lo, hi]; modulo, so tiny bias on huge spans */

#ifdef __cplusplus
}
#endif
#endif

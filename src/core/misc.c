#include <pthread.h>
#include <stddef.h>

#include "cartwheel/core.h"

/* ── hashing ──────────────────────────────────────────────────────── */

uint32_t cw_hash32(const void *data, size_t len, uint32_t h) {
  const uint8_t *p = data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

uint64_t cw_hash_bytes(const void *data, size_t len, uint64_t seed) {
  /* FNV-1a 64 with a final avalanche: stable, endian-independent for bytes */
  const uint8_t *p = data;
  uint64_t h = 1469598103934665603ull ^ seed;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  h ^= h >> 32;
  h *= 0xd6e8feb86659fd93ull;
  h ^= h >> 32;
  return h;
}

uint64_t cw_hash_str(const char *s) { return cw_hash_bytes(s, strlen(s), 0); }

/* ── interning ────────────────────────────────────────────────────── */
/* One table: equal contents share one entry (and pointer) whether pinned or
 * counted. A pinned entry (cw_intern) lives for the process, in an arena; a
 * counted one (cw_intern_ref) is heap-allocated and freed when its last
 * reference goes. Pinning a counted entry makes it permanent. */

#define INTERN_PINNED UINT32_MAX
typedef struct istr {
  struct istr *next; /* same-hash chain */
  uint64_t hash;
  uint32_t refs;     /* INTERN_PINNED: never freed */
  uint32_t len;
  char s[];
} istr;

static cw_map intern_map;     /* hash -> chain head */
static cw_map intern_counted; /* string pointer -> 1 for counted (unpinned) entries */
static cw_arena intern_arena;
static uint32_t intern_live_counted;
static pthread_mutex_t intern_lock = PTHREAD_MUTEX_INITIALIZER;

static istr *intern_lookup(const char *s, size_t n, uint64_t h) {
  if (!intern_arena.block_size) {
    cw_arena_init(&intern_arena, 64 * 1024, CW_MEM_CORE);
    cw_map_init(&intern_map, CW_MEM_CORE);
    cw_map_init(&intern_counted, CW_MEM_CORE);
  }
  uint64_t v;
  if (!cw_map_get(&intern_map, h, &v)) return NULL;
  for (istr *e = (istr *)(uintptr_t)v; e; e = e->next)
    if (e->len == n && !memcmp(e->s, s, n)) return e;
  return NULL;
}
static istr *intern_add(const char *s, size_t n, uint64_t h, bool pinned) {
  istr *e = pinned ? cw_arena_alloc_aligned(&intern_arena, sizeof(istr) + n + 1, _Alignof(istr))
                   : cw_alloc(sizeof(istr) + n + 1, CW_MEM_CORE);
  uint64_t head = 0;
  cw_map_get(&intern_map, h, &head);
  e->next = (istr *)(uintptr_t)head;
  e->hash = h;
  e->refs = pinned ? INTERN_PINNED : 1;
  e->len = (uint32_t)n;
  memcpy(e->s, s, n);
  e->s[n] = 0;
  cw_map_set(&intern_map, h, (uint64_t)(uintptr_t)e);
  if (!pinned) {
    cw_map_set(&intern_counted, (uint64_t)(uintptr_t)e->s, 1);
    intern_live_counted++;
  }
  return e;
}
static void intern_pin(istr *e) {
  if (e->refs == INTERN_PINNED) return;
  e->refs = INTERN_PINNED;
  cw_map_remove(&intern_counted, (uint64_t)(uintptr_t)e->s);
  intern_live_counted--;
}

const char *cw_intern_n(const char *s, size_t n) {
  uint64_t h = cw_hash_bytes(s, n, 0x1157);
  pthread_mutex_lock(&intern_lock);
  istr *e = intern_lookup(s, n, h);
  if (e) intern_pin(e);
  else e = intern_add(s, n, h, true);
  pthread_mutex_unlock(&intern_lock);
  return e->s;
}

const char *cw_intern(const char *s) { return cw_intern_n(s, strlen(s)); }

const char *cw_intern_ref_n(const char *s, size_t n) {
  uint64_t h = cw_hash_bytes(s, n, 0x1157);
  pthread_mutex_lock(&intern_lock);
  istr *e = intern_lookup(s, n, h);
  if (!e) e = intern_add(s, n, h, false);
  else if (e->refs != INTERN_PINNED) e->refs++;
  pthread_mutex_unlock(&intern_lock);
  return e->s;
}
const char *cw_intern_ref(const char *s) { return cw_intern_ref_n(s, strlen(s)); }

void cw_intern_unref(const char *s) {
  if (!s) return;
  pthread_mutex_lock(&intern_lock);
  if (!intern_arena.block_size || !cw_map_get(&intern_counted, (uint64_t)(uintptr_t)s, NULL)) {
    pthread_mutex_unlock(&intern_lock); /* pinned, a literal, or not interned: nothing to drop */
    return;
  }
  istr *e = (istr *)(uintptr_t)(s - offsetof(istr, s));
  if (--e->refs == 0) {
    uint64_t hv = 0;
    cw_map_get(&intern_map, e->hash, &hv);
    istr *head = (istr *)(uintptr_t)hv;
    if (head == e) head = e->next;
    else {
      istr *p = head;
      while (p->next != e) p = p->next;
      p->next = e->next;
    }
    if (head) cw_map_set(&intern_map, e->hash, (uint64_t)(uintptr_t)head);
    else cw_map_remove(&intern_map, e->hash);
    cw_map_remove(&intern_counted, (uint64_t)(uintptr_t)s);
    intern_live_counted--;
    cw_free(e);
  }
  pthread_mutex_unlock(&intern_lock);
}

const char *cw_intern_pin(const char *s) {
  if (!s) return s;
  pthread_mutex_lock(&intern_lock);
  if (intern_arena.block_size && cw_map_get(&intern_counted, (uint64_t)(uintptr_t)s, NULL))
    intern_pin((istr *)(uintptr_t)(s - offsetof(istr, s)));
  pthread_mutex_unlock(&intern_lock);
  return s;
}

const char *cw_intern_find_n(const char *s, size_t n) {
  uint64_t h = cw_hash_bytes(s, n, 0x1157);
  pthread_mutex_lock(&intern_lock);
  istr *e = intern_lookup(s, n, h);
  pthread_mutex_unlock(&intern_lock);
  return e ? e->s : NULL;
}
const char *cw_intern_find(const char *s) { return cw_intern_find_n(s, strlen(s)); }

uint32_t cw_intern_counted(void) {
  pthread_mutex_lock(&intern_lock);
  uint32_t n = intern_live_counted;
  pthread_mutex_unlock(&intern_lock);
  return n;
}

/* ── xoshiro128** ─────────────────────────────────────────────────── */

static uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

void cw_rng_seed(cw_rng *r, uint64_t seed) {
  /* splitmix64 to fill the state; never all zero */
  for (int i = 0; i < 4; i += 2) {
    uint64_t z = (seed += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    r->s[i] = (uint32_t)z;
    r->s[i + 1] = (uint32_t)(z >> 32);
  }
  if (!(r->s[0] | r->s[1] | r->s[2] | r->s[3])) r->s[0] = 1;
}

uint32_t cw_rng_u32(cw_rng *r) {
  uint32_t *s = r->s;
  uint32_t result = rotl(s[1] * 5, 7) * 9, t = s[1] << 9;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = rotl(s[3], 11);
  return result;
}

float cw_rng_float(cw_rng *r) { return (float)(cw_rng_u32(r) >> 8) * (1.0f / 16777216.0f); }
float cw_rng_range(cw_rng *r, float lo, float hi) { return lo + (hi - lo) * cw_rng_float(r); }
int cw_rng_int(cw_rng *r, int lo, int hi) {
  uint32_t span = (uint32_t)(hi - lo) + 1u;
  return span ? lo + (int)(cw_rng_u32(r) % span) : lo + (int)cw_rng_u32(r);
}

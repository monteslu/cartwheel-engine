#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/core.h"

/* Every allocation carries a 16-byte header (size + tag) so cw_free can
 * account without the caller passing the size, and the data stays 16-byte
 * aligned for SIMD. */
typedef struct { uint64_t size; uint32_t tag; uint32_t magic; } hdr;
#define MAGIC 0xC0DEC0DEu
CW_STATIC_ASSERT(sizeof(hdr) == 16, "allocation header keeps 16-byte alignment");

/* the C11 aligned allocator; the Windows CRT has none (its aligned blocks
 * need their own free) */
#if defined(_WIN32)
#include <malloc.h>
#define sys_aligned_alloc(align, size) _aligned_malloc((size), (align))
#define sys_aligned_free(p) _aligned_free(p)
#elif defined(__ANDROID__) && __ANDROID_API__ < 28
/* bionic has aligned_alloc from API 28; posix_memalign blocks free() */
static void *sys_aligned_alloc(size_t align, size_t size) {
  void *p = NULL;
  return posix_memalign(&p, align, size) ? NULL : p;
}
#define sys_aligned_free(p) free(p)
#else
#define sys_aligned_alloc(align, size) aligned_alloc((align), (size))
#define sys_aligned_free(p) free(p)
#endif

static atomic_llong bytes[CW_MEM_TAG_COUNT], allocs[CW_MEM_TAG_COUNT], total, peak;

static void account(cw_mem_tag tag, int64_t size, int64_t count) {
  atomic_fetch_add(&bytes[tag], size);
  atomic_fetch_add(&allocs[tag], count);
  long long t = atomic_fetch_add(&total, size) + size;
  long long p = atomic_load(&peak);
  while (t > p && !atomic_compare_exchange_weak(&peak, &p, t)) {}
}

void *cw_alloc(size_t size, cw_mem_tag tag) {
  hdr *h = sys_aligned_alloc(16, (sizeof(hdr) + size + 15) & ~(size_t)15);
  if (!h) CW_FATAL("out of memory allocating %zu bytes (%s)", size, cw_mem_tag_name(tag));
  h->size = size;
  h->tag = tag;
  h->magic = MAGIC;
  account(tag, (int64_t)size, 1);
  return h + 1;
}

void *cw_calloc(size_t count, size_t size, cw_mem_tag tag) {
  void *p = cw_alloc(count * size, tag);
  memset(p, 0, count * size);
  return p;
}

void cw_free(void *p) {
  if (!p) return;
  hdr *h = (hdr *)p - 1;
  CW_ASSERT(h->magic == MAGIC, "cw_free of memory not from cw_alloc (or freed twice)");
  h->magic = 0;
  account((cw_mem_tag)h->tag, -(int64_t)h->size, -1);
  sys_aligned_free(h);
}

void *cw_realloc(void *p, size_t size, cw_mem_tag tag) {
  if (!p) return cw_alloc(size, tag);
  hdr *h = (hdr *)p - 1;
  void *n = cw_alloc(size, (cw_mem_tag)h->tag);
  memcpy(n, p, h->size < size ? h->size : size);
  cw_free(p);
  return n;
}

char *cw_strdup(const char *s, cw_mem_tag tag) {
  size_t n = strlen(s) + 1;
  char *d = cw_alloc(n, tag);
  memcpy(d, s, n);
  return d;
}

void cw_mem_get_stats(cw_mem_stats *out) {
  for (int i = 0; i < CW_MEM_TAG_COUNT; i++) {
    out->bytes[i] = atomic_load(&bytes[i]);
    out->allocs[i] = atomic_load(&allocs[i]);
  }
  out->total_bytes = atomic_load(&total);
  out->peak_bytes = atomic_load(&peak);
}

const char *cw_mem_tag_name(cw_mem_tag tag) {
  static const char *names[] = { "general", "core", "scene", "render", "render2d", "physics", "audio",
                                 "anim", "ui", "resource", "script", "game", "editor" };
  return tag < CW_MEM_TAG_COUNT ? names[tag] : "?";
}

void cw__array_grow(void **data, int *cap, int need, size_t elem) {
  if (need <= *cap) return;
  int n = *cap ? *cap * 2 : 8;
  while (n < need) n *= 2;
  *data = cw_realloc(*data, (size_t)n * elem, CW_MEM_CORE);
  *cap = n;
}

/* ── arenas ───────────────────────────────────────────────────────── */

struct cw_arena_block {
  cw_arena_block *next;
  size_t cap, used;
  _Alignas(16) uint8_t data[];
};

void cw_arena_init(cw_arena *a, size_t block_size, cw_mem_tag tag) {
  memset(a, 0, sizeof *a);
  a->block_size = block_size ? block_size : 64 * 1024;
  a->tag = tag;
}

void *cw_arena_alloc_aligned(cw_arena *a, size_t size, size_t align) {
  cw_arena_block *b = a->head;
  for (;;) {
    if (b) {
      size_t off = (b->used + align - 1) & ~(align - 1);
      if (off + size <= b->cap) {
        b->used = off + size;
        a->used += size;
        if (a->used > a->high_water) a->high_water = a->used;
        return b->data + off;
      }
      if (b->next) { /* reuse blocks kept from before a reset */
        b = a->head = b->next;
        b->used = 0;
        continue;
      }
    }
    size_t cap = size + align > a->block_size ? size + align : a->block_size;
    cw_arena_block *nb = cw_alloc(sizeof *nb + cap, a->tag);
    nb->next = NULL;
    nb->cap = cap;
    nb->used = 0;
    if (b) b->next = nb;
    else a->first = nb;
    b = a->head = nb;
  }
}

void *cw_arena_alloc(cw_arena *a, size_t size) { return cw_arena_alloc_aligned(a, size, 16); }

void cw_arena_reset(cw_arena *a) {
  a->head = a->first;
  if (a->first) a->first->used = 0;
  a->used = 0;
}

void cw_arena_free(cw_arena *a) {
  for (cw_arena_block *b = a->first, *n; b; b = n) {
    n = b->next;
    cw_free(b);
  }
  memset(a, 0, sizeof *a);
}

/* per-thread frame arenas: reset lazily the first time a thread allocates in
 * a new frame */
static atomic_ullong current_frame;
static _Thread_local cw_arena frame_arena;
static _Thread_local uint64_t frame_arena_frame = ~0ull;

void cw_frame_arena_begin(uint64_t frame) { atomic_store(&current_frame, frame); }

void *cw_frame_alloc(size_t size) {
  uint64_t f = atomic_load(&current_frame);
  if (frame_arena_frame != f) {
    if (!frame_arena.block_size) cw_arena_init(&frame_arena, 1 << 20, CW_MEM_CORE);
    else cw_arena_reset(&frame_arena);
    frame_arena_frame = f;
  }
  return cw_arena_alloc(&frame_arena, size);
}

void *cw_frame_calloc(size_t count, size_t size) {
  void *p = cw_frame_alloc(count * size);
  memset(p, 0, count * size);
  return p;
}

char *cw_frame_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *s = cw_frame_alloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(s, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return s;
}

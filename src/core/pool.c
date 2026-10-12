#include "cartwheel/core.h"

#define CHUNK 256

void cw_pool_init(cw_pool *p, size_t elem_size, cw_mem_tag tag) {
  memset(p, 0, sizeof *p);
  p->elem_size = (elem_size + 15) & ~(size_t)15;
  p->chunk_elems = CHUNK;
  p->tag = tag;
}

void cw_pool_free_all(cw_pool *p) {
  for (int i = 0; i < p->chunk_count; i++) cw_free(p->chunks[i]);
  cw_free(p->chunks);
  cw_free(p->gens);
  cw_free(p->free_list);
  cw_mem_tag tag = p->tag;
  size_t es = p->elem_size;
  memset(p, 0, sizeof *p);
  p->elem_size = es;
  p->chunk_elems = CHUNK;
  p->tag = tag;
}

static void *slot_ptr(const cw_pool *p, int slot) {
  return p->chunks[slot / p->chunk_elems] + (size_t)(slot % p->chunk_elems) * p->elem_size;
}

void *cw_pool_alloc(cw_pool *p, uint64_t *id) {
  int slot;
  if (p->free_count) {
    slot = (int)p->free_list[--p->free_count];
  } else {
    slot = p->slot_count++;
    if (slot / p->chunk_elems >= p->chunk_count) {
      p->chunks = cw_realloc(p->chunks, sizeof *p->chunks * (size_t)(p->chunk_count + 1), p->tag);
      p->chunks[p->chunk_count++] = cw_alloc(p->elem_size * (size_t)p->chunk_elems, p->tag);
      p->gens = cw_realloc(p->gens, sizeof *p->gens * (size_t)p->chunk_count * (size_t)p->chunk_elems, p->tag);
      p->free_list = cw_realloc(p->free_list, sizeof *p->free_list * (size_t)p->chunk_count * (size_t)p->chunk_elems, p->tag);
      memset(p->gens + (size_t)(p->chunk_count - 1) * (size_t)p->chunk_elems, 0, sizeof *p->gens * (size_t)p->chunk_elems);
    }
  }
  p->gens[slot] |= 1; /* odd = live; generation never 0 for a live slot */
  p->live++;
  void *e = slot_ptr(p, slot);
  memset(e, 0, p->elem_size);
  /* slot is stored +1 so handle 0 stays null */
  *id = ((uint64_t)p->gens[slot] << 32) | (uint32_t)(slot + 1);
  return e;
}

void *cw_pool_get(const cw_pool *p, uint64_t id) {
  uint32_t slot1 = (uint32_t)id, gen = (uint32_t)(id >> 32);
  if (!slot1 || (int)slot1 > p->slot_count) return NULL;
  if (p->gens[slot1 - 1] != gen || !(gen & 1)) return NULL;
  return slot_ptr(p, (int)slot1 - 1);
}

bool cw_pool_release(cw_pool *p, uint64_t id) {
  if (!cw_pool_get(p, id)) return false;
  int slot = (int)(uint32_t)id - 1;
  p->gens[slot]++; /* even = free, and the next alloc makes a new odd value */
  p->free_list[p->free_count++] = (uint32_t)slot;
  p->live--;
  return true;
}

uint64_t cw_pool_id_of(const cw_pool *p, const void *elem) {
  for (int c = 0; c < p->chunk_count; c++) {
    const uint8_t *base = p->chunks[c];
    if ((const uint8_t *)elem >= base && (const uint8_t *)elem < base + p->elem_size * (size_t)p->chunk_elems) {
      int slot = c * p->chunk_elems + (int)(((const uint8_t *)elem - base) / p->elem_size);
      if (!(p->gens[slot] & 1)) return 0;
      return ((uint64_t)p->gens[slot] << 32) | (uint32_t)(slot + 1);
    }
  }
  return 0;
}

void *cw_pool_at(const cw_pool *p, int slot) {
  if (slot < 0 || slot >= p->slot_count || !(p->gens[slot] & 1)) return NULL;
  return slot_ptr(p, slot);
}

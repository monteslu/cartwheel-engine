#include "cartwheel/core.h"

static uint64_t mix(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdull;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ull;
  k ^= k >> 33;
  return k;
}

void cw_map_init(cw_map *m, cw_mem_tag tag) {
  memset(m, 0, sizeof *m);
  m->tag = tag;
}

void cw_map_free(cw_map *m) {
  cw_free(m->dead);
  cw_free(m->keys);
  cw_free(m->vals);
  cw_free(m->slots);
  cw_mem_tag t = m->tag;
  memset(m, 0, sizeof *m);
  m->tag = t;
}

static int find_slot(const cw_map *m, uint64_t key) {
  if (!m->slot_cap) return -1;
  uint32_t mask = (uint32_t)m->slot_cap - 1;
  for (uint32_t i = (uint32_t)mix(key) & mask;; i = (i + 1) & mask) {
    int32_t s = m->slots[i];
    if (s == -1) return -1;
    if (s >= 0 && m->keys[s] == key) return (int)i;
  }
}

static void compact(cw_map *m);

static void rehash(cw_map *m, int slot_cap) {
  cw_free(m->slots);
  m->slot_cap = slot_cap;
  m->slots = cw_alloc(sizeof *m->slots * (size_t)slot_cap, m->tag);
  memset(m->slots, 0xff, sizeof *m->slots * (size_t)slot_cap); /* -1 */
  m->tombstones = 0;
  uint32_t mask = (uint32_t)slot_cap - 1;
  for (int e = 0; e < m->count; e++) {
    if (m->dead && m->dead[e]) continue;
    uint32_t i = (uint32_t)mix(m->keys[e]) & mask;
    while (m->slots[i] != -1) i = (i + 1) & mask;
    m->slots[i] = e;
  }
}

void cw_map_set(cw_map *m, uint64_t key, uint64_t val) {
  int s = find_slot(m, key);
  if (s >= 0) {
    m->vals[m->slots[s]] = val;
    return;
  }
  if ((m->count - m->dead_count + m->tombstones + 1) * 4 >= m->slot_cap * 3) {
    if (m->dead_count) compact(m);
    if ((m->count + m->tombstones + 1) * 4 >= m->slot_cap * 3) rehash(m, m->slot_cap ? m->slot_cap * 2 : 16);
  }
  if (m->count == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 8;
    m->keys = cw_realloc(m->keys, sizeof *m->keys * (size_t)m->cap, m->tag);
    m->vals = cw_realloc(m->vals, sizeof *m->vals * (size_t)m->cap, m->tag);
    if (m->dead) {
      m->dead = cw_realloc(m->dead, (size_t)m->cap, m->tag);
      memset(m->dead + m->count, 0, (size_t)(m->cap - m->count));
    }
  }
  int e = m->count++;
  m->keys[e] = key;
  m->vals[e] = val;
  uint32_t mask = (uint32_t)m->slot_cap - 1;
  uint32_t i = (uint32_t)mix(key) & mask;
  while (m->slots[i] >= 0) i = (i + 1) & mask;
  if (m->slots[i] == -2) m->tombstones--;
  m->slots[i] = e;
}

bool cw_map_get(const cw_map *m, uint64_t key, uint64_t *val) {
  int s = find_slot(m, key);
  if (s < 0) return false;
  if (val) *val = m->vals[m->slots[s]];
  return true;
}

/* Drop dead entries, keeping the order of the live ones, and rebuild slots. */
static void compact(cw_map *m) {
  int w = 0;
  for (int e = 0; e < m->count; e++) {
    if (m->dead[e]) continue;
    m->keys[w] = m->keys[e];
    m->vals[w] = m->vals[e];
    w++;
  }
  m->count = w;
  memset(m->dead, 0, (size_t)m->cap);
  m->dead_count = 0;
  rehash(m, m->slot_cap);
}

bool cw_map_remove(cw_map *m, uint64_t key) {
  int s = find_slot(m, key);
  if (s < 0) return false;
  int e = m->slots[s];
  if (!m->dead) m->dead = cw_calloc((size_t)m->cap, 1, m->tag);
  m->dead[e] = 1;
  m->dead_count++;
  m->slots[s] = -2;
  m->tombstones++;
  if (m->dead_count > 16 && m->dead_count * 2 > m->count) compact(m);
  return true;
}

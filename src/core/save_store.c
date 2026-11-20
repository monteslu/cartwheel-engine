/* The save store (cartwheel/save.h): named, versioned, checksummed slots in a caller's block of bytes. */
#include <string.h>

#include "cartwheel/save.h"

#define MAGIC 0x31535743u /* "CWS1" little-endian */
#define HDR 8
#define SLOT_HDR 12
#define NAME_MAX 63

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }
static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h) {
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

typedef struct slot {
  size_t off;        /* of the slot header */
  size_t total;      /* header + padded name + padded data */
  const uint8_t *name;
  int name_len;
  uint16_t version;
  uint32_t size;
  uint8_t *data;
} slot;

static size_t used_of(const cw_save_store *st) { return st && st->base ? rd32(st->base + 4) : 0; }

/* the slot at off, bounds checked against `used`; false when it does not fit there */
static bool slot_at(const cw_save_store *st, size_t off, size_t used, slot *s) {
  if (off + SLOT_HDR > used) return false;
  const uint8_t *p = st->base + off;
  int nl = p[0];
  uint32_t size = rd32(p + 4);
  if (nl < 1 || nl > NAME_MAX || size > used) return false;
  size_t total = SLOT_HDR + pad4((size_t)nl) + pad4(size);
  if (off + total > used) return false;
  s->off = off;
  s->total = total;
  s->name = p + SLOT_HDR;
  s->name_len = nl;
  s->version = rd16(p + 2);
  s->size = size;
  s->data = st->base + off + SLOT_HDR + pad4((size_t)nl);
  return true;
}
static uint32_t slot_sum(const slot *s) { return fnv(s->data, s->size, fnv(s->name, (size_t)s->name_len, 2166136261u)); }

static bool find(const cw_save_store *st, const char *name, slot *out) {
  if (!st || !st->base || !name) return false;
  size_t nl = strlen(name), used = used_of(st);
  for (size_t off = HDR; off < used;) {
    slot s;
    if (!slot_at(st, off, used, &s)) return false;
    if ((size_t)s.name_len == nl && !memcmp(s.name, name, nl)) {
      *out = s;
      return true;
    }
    off += s.total;
  }
  return false;
}

static void format(cw_save_store *st) {
  memset(st->base, 0, st->size);
  wr32(st->base, MAGIC);
  wr32(st->base + 4, HDR);
}

static void remove_slot(cw_save_store *st, const slot *s) {
  size_t used = used_of(st), tail = used - (s->off + s->total);
  memmove(st->base + s->off, st->base + s->off + s->total, tail);
  memset(st->base + used - s->total, 0, s->total);
  wr32(st->base + 4, (uint32_t)(used - s->total));
}

bool cw_save_open(cw_save_store *st, void *block, size_t size, cw_save_legacy_fn legacy, void *user) {
  if (!st) return false;
  st->base = NULL;
  st->size = 0;
  if (!block || size < 16) {
    cw_error("save store: the block must be at least 16 bytes (got %zu): give cw_app_desc.save a buffer, or use cw_shell_save_store", size);
    return false;
  }
  st->base = block;
  st->size = size;
  uint32_t used = rd32(st->base + 4);
  if (rd32(st->base) != MAGIC || used < HDR || used > size) {
    /* not a store: zeros (a first run) or something older */
    bool empty = true;
    for (size_t i = 0; i < size && empty; i++) empty = st->base[i] == 0;
    if (empty || !legacy) {
      if (!empty) cw_warn("save store: the save data is not a store (%zu bytes of something else): starting empty", size);
      format(st);
      return true;
    }
    uint8_t *old = cw_alloc(size, CW_MEM_CORE);
    memcpy(old, st->base, size);
    format(st);
    legacy(st, old, size, user);
    cw_free(old);
    return true;
  }
  /* walk the slots: drop one whose checksum fails, cut at one whose bounds make no sense */
  for (size_t off = HDR; off < used;) {
    slot s;
    if (!slot_at(st, off, used, &s)) {
      cw_warn("save store: damaged after %zu bytes: keeping the slots before that", off);
      memset(st->base + off, 0, size - off);
      wr32(st->base + 4, (uint32_t)off);
      break;
    }
    if (slot_sum(&s) != rd32(st->base + off + 8)) {
      cw_warn("save store: slot '%.*s' is damaged (checksum): dropped", s.name_len, (const char *)s.name);
      remove_slot(st, &s);
      used = rd32(st->base + 4);
      continue;
    }
    off += s.total;
  }
  /* whatever follows the used part is not ours to trust */
  used = rd32(st->base + 4);
  memset(st->base + used, 0, size - used);
  return true;
}

cw_save_result cw_save_read(cw_save_store *st, const char *name, uint16_t version, void *out, size_t size,
                            cw_save_migrate_fn migrate, void *user) {
  if (out && size) memset(out, 0, size);
  slot s;
  if (!out || !find(st, name, &s)) return CW_SAVE_NONE;
  if (s.version == version) {
    memcpy(out, s.data, s.size < size ? s.size : size);
    return s.size == size ? CW_SAVE_OK : CW_SAVE_RESIZED;
  }
  if (s.version > version) return CW_SAVE_NEWER;
  if (!migrate) return CW_SAVE_DISCARDED;
  /* the hook may write slots (moving this one): hand it a copy */
  uint8_t *old = cw_frame_alloc(s.size ? s.size : 1);
  memcpy(old, s.data, s.size);
  uint16_t old_version = s.version;
  size_t old_size = s.size;
  if (migrate(old_version, old, old_size, out, size, user)) return CW_SAVE_MIGRATED;
  memset(out, 0, size);
  return CW_SAVE_DISCARDED;
}

bool cw_save_write(cw_save_store *st, const char *name, uint16_t version, const void *data, size_t size) {
  if (!st || !st->base || !name) return false;
  size_t nl = strlen(name);
  if (nl < 1 || nl > NAME_MAX) {
    cw_error("save store: slot name '%s' must be 1..%d bytes", name, NAME_MAX);
    return false;
  }
  slot s;
  bool have = find(st, name, &s);
  if (have && s.size == size) { /* in place */
    uint8_t *p = st->base + s.off;
    wr16(p + 2, version);
    if (size) memcpy(s.data, data, size);
    s.version = version;
    wr32(p + 8, slot_sum(&s));
    return true;
  }
  size_t total = SLOT_HDR + pad4(nl) + pad4(size), used = used_of(st);
  size_t after = used - (have ? s.total : 0) + total;
  if (size > 0xffffffffu || after > st->size) {
    cw_error("save store: slot '%s' (%zu bytes) does not fit: %zu of %zu bytes used (make the save data larger, or the slot smaller)",
             name, size, used, st->size);
    return false;
  }
  if (have) {
    remove_slot(st, &s);
    used = used_of(st);
  }
  uint8_t *p = st->base + used;
  memset(p, 0, total);
  p[0] = (uint8_t)nl;
  wr16(p + 2, version);
  wr32(p + 4, (uint32_t)size);
  memcpy(p + SLOT_HDR, name, nl);
  if (size) memcpy(p + SLOT_HDR + pad4(nl), data, size);
  wr32(st->base + 4, (uint32_t)(used + total));
  slot ns = { .name = p + SLOT_HDR, .name_len = (int)nl, .size = (uint32_t)size, .data = p + SLOT_HDR + pad4(nl) };
  wr32(p + 8, slot_sum(&ns));
  return true;
}

bool cw_save_erase(cw_save_store *st, const char *name) {
  slot s;
  if (!find(st, name, &s)) return false;
  remove_slot(st, &s);
  return true;
}

bool cw_save_info(const cw_save_store *st, const char *name, uint16_t *version, size_t *size) {
  slot s;
  if (!find(st, name, &s)) return false;
  if (version) *version = s.version;
  if (size) *size = s.size;
  return true;
}

int cw_save_count(const cw_save_store *st) {
  int n = 0;
  size_t used = used_of(st);
  for (size_t off = HDR; off < used; n++) {
    slot s;
    if (!slot_at(st, off, used, &s)) break;
    off += s.total;
  }
  return n;
}

bool cw_save_name(const cw_save_store *st, int index, char *buf, size_t cap) {
  size_t used = used_of(st);
  int n = 0;
  for (size_t off = HDR; off < used; n++) {
    slot s;
    if (!slot_at(st, off, used, &s)) break;
    if (n == index) {
      if (buf && cap) {
        size_t k = (size_t)s.name_len < cap - 1 ? (size_t)s.name_len : cap - 1;
        memcpy(buf, s.name, k);
        buf[k] = 0;
      }
      return true;
    }
    off += s.total;
  }
  return false;
}

size_t cw_save_used(const cw_save_store *st) { return used_of(st); }

size_t cw_save_room(const cw_save_store *st, const char *name) {
  if (!st || !st->base || !name) return 0;
  size_t used = used_of(st), need = SLOT_HDR + pad4(strlen(name));
  if (used + need >= st->size) return 0;
  return (st->size - used - need) & ~(size_t)3;
}

void cw_save_clear(cw_save_store *st) {
  if (st && st->base) format(st);
}

/* Record / replay of tick snapshots.
 *
 * Format (little endian, LEB128 varints):
 *   "CWIR" u8 version(1 or 2) u8 players u8 actions { u8 len, name bytes } * actions
 *   version 2 only: u8 channels (bit 0: the pointer channel, cw_input_pointer_channel)
 *   entries: varint idle, varint n
 *     n > 0: `idle` unchanged ticks, then one tick changing n slots:
 *            n * { varint slot (player * actions + action), u8 flags, i16 raw }
 *            the slot players * actions (the pointer channel only) is the pointer:
 *            u8 flags, varint zigzag(x - previous x), varint zigzag(y - previous y) (1/16 window pixels)
 *     n = 0: the end: `idle` trailing unchanged ticks.
 * A slot's value persists until changed; the initial snapshot is all zero. A recording without the pointer channel is
 * written as version 1, byte for byte as before v0.84.0, and both versions replay. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "input_internal.h"

#define I cw__in
#define MAGIC "CWIR"
#define VERSION 1     /* actions only */
#define VERSION_PTR 2 /* actions and the pointer channel */

typedef CW_ARRAY(uint8_t) bytes;

static struct {
  bool on;
  bool skip_set; /* CW_TEST_SET_UNRECORDED (read at record_begin): the gate control, actions the game set are not recorded */
  bool pointer;  /* the pointer channel was on at record_begin: version 2, the pointer recorded and hashed */
  bool skip_ptr; /* CW_TEST_POINTER_UNRECORDED (read at record_begin): the gate control, the pointer is left out */
  int count; /* actions at record_begin */
  bytes header, body, out;
  uint32_t idle, ticks, hash;
  cw__snapshot prev;
} rec;

static struct {
  bool on;
  uint8_t *data;
  size_t len, pos, body;
  int count, players;
  cw_action map[CW_INPUT_MAX_ACTIONS];
  bool pointer; /* the recording carries the pointer channel */
  uint32_t idle_left;
  size_t pending; /* changes waiting behind idle ticks (0 = none) */
  uint32_t pending_n;
  uint32_t ticks, hash;
  cw__snapshot snap; /* in the recording's action order */
} rep;

static void put(bytes *b, uint8_t v) { cw_array_push(b, v); }
static void put_varint(bytes *b, uint32_t v) {
  do {
    uint8_t c = v & 0x7f;
    v >>= 7;
    put(b, (uint8_t)(c | (v ? 0x80 : 0)));
  } while (v);
}
static uint32_t zigzag(int32_t v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
static int32_t unzigzag(uint32_t v) { return (int32_t)(v >> 1) ^ -(int32_t)(v & 1); }
static bool ptr_same(const cw__ptr *a, const cw__ptr *b) { return a->flags == b->flags && a->x == b->x && a->y == b->y; }

void cw_input_record_begin(void) {
  cw__input_ensure_init();
  cw_array_clear(&rec.header);
  cw_array_clear(&rec.body);
  I.tick_rec = false; /* a tick still open (record_begin inside fixed_update) is not this recording's: it starts at the next */
  rec.on = true;
  rec.skip_set = cw_test_env("CW_TEST_SET_UNRECORDED") != NULL;
  if (rec.skip_set) cw_warn("input: CW_TEST_SET_UNRECORDED: actions the game sets are left out of this recording (a gate control)");
  rec.pointer = I.pointer_on;
  rec.skip_ptr = rec.pointer && cw_test_env("CW_TEST_POINTER_UNRECORDED") != NULL;
  if (rec.skip_ptr) cw_warn("input: CW_TEST_POINTER_UNRECORDED: the pointer is left out of this recording (a gate control)");
  rec.count = I.count;
  rec.idle = rec.ticks = 0;
  rec.hash = CW_HASH32_INIT;
  memset(&rec.prev, 0, sizeof rec.prev);
  for (int i = 0; i < 4; i++) put(&rec.header, (uint8_t)MAGIC[i]);
  put(&rec.header, rec.pointer ? VERSION_PTR : VERSION);
  put(&rec.header, CW_INPUT_MAX_PLAYERS);
  put(&rec.header, (uint8_t)rec.count);
  for (int a = 0; a < rec.count; a++) {
    size_t n = strlen(I.actions[a].name);
    if (n > 255) n = 255;
    put(&rec.header, (uint8_t)n);
    for (size_t i = 0; i < n; i++) put(&rec.header, (uint8_t)I.actions[a].name[i]);
  }
  if (rec.pointer) put(&rec.header, 1); /* channels: the pointer */
}

/* a tick open when the recording ends (record_end inside fixed_update) is still recorded when it closes, with what the
 * game sets in the rest of it: it began while recording */
void cw_input_record_end(void) { rec.on = false; }
bool cw__input_record_skip_set(void) { return rec.skip_set; }
bool cw_input_recording_active(void) { return rec.on; }
uint32_t cw_input_record_ticks(void) { return rec.ticks; }
uint32_t cw_input_record_hash(void) { return rec.hash; }

void cw__input_record_tick(const cw__snapshot *s) {
  if (!rec.header.count) return;
  rec.ticks++;
  rec.hash = cw__snapshot_hash(s, rec.count, rec.hash);
  if (rec.pointer) rec.hash = cw__pointer_hash(&s->ptr, rec.hash);
  uint32_t n = 0;
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++)
    for (int a = 0; a < rec.count; a++)
      if (s->s[p][a].flags != rec.prev.s[p][a].flags || s->s[p][a].raw != rec.prev.s[p][a].raw) n++;
  bool ptr = rec.pointer && !rec.skip_ptr && !ptr_same(&s->ptr, &rec.prev.ptr);
  n += ptr;
  if (!n) {
    rec.idle++;
    return;
  }
  put_varint(&rec.body, rec.idle);
  put_varint(&rec.body, n);
  rec.idle = 0;
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++)
    for (int a = 0; a < rec.count; a++) {
      const cw__slot *v = &s->s[p][a];
      if (v->flags == rec.prev.s[p][a].flags && v->raw == rec.prev.s[p][a].raw) continue;
      put_varint(&rec.body, (uint32_t)(p * rec.count + a));
      put(&rec.body, v->flags);
      put(&rec.body, (uint8_t)(v->raw & 0xff));
      put(&rec.body, (uint8_t)((uint16_t)v->raw >> 8));
      rec.prev.s[p][a] = *v;
    }
  if (ptr) {
    put_varint(&rec.body, (uint32_t)(CW_INPUT_MAX_PLAYERS * rec.count));
    put(&rec.body, s->ptr.flags);
    put_varint(&rec.body, zigzag((int32_t)((uint32_t)s->ptr.x - (uint32_t)rec.prev.ptr.x)));
    put_varint(&rec.body, zigzag((int32_t)((uint32_t)s->ptr.y - (uint32_t)rec.prev.ptr.y)));
    rec.prev.ptr = s->ptr;
  }
}

const uint8_t *cw_input_recording(size_t *len) {
  if (!rec.header.count) {
    if (len) *len = 0;
    return NULL;
  }
  cw_array_clear(&rec.out);
  cw_array_reserve(&rec.out, rec.header.count + rec.body.count + 10);
  memcpy(rec.out.data, rec.header.data, (size_t)rec.header.count);
  if (rec.body.count) memcpy(rec.out.data + rec.header.count, rec.body.data, (size_t)rec.body.count);
  rec.out.count = rec.header.count + rec.body.count;
  put_varint(&rec.out, rec.idle);
  put_varint(&rec.out, 0);
  if (len) *len = (size_t)rec.out.count;
  return rec.out.data;
}

/* ── replay ────────────────────────────────────────────────────────── */

static bool get_varint(const uint8_t *d, size_t len, size_t *pos, uint32_t *out) {
  uint32_t v = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    if (*pos >= len) return false;
    uint8_t c = d[(*pos)++];
    v |= (uint32_t)(c & 0x7f) << shift;
    if (!(c & 0x80)) {
      *out = v;
      return true;
    }
  }
  return false;
}

/* past one change (validated by then): an action slot's 3 bytes, the pointer's flags and two varints */
static bool skip_change(const uint8_t *d, size_t len, size_t *pos, uint32_t slot, uint32_t ptr_slot) {
  if (slot != ptr_slot) {
    if (*pos + 3 > len) return false;
    *pos += 3;
    return true;
  }
  uint32_t dx, dy;
  if (*pos + 1 > len) return false;
  (*pos)++;
  return get_varint(d, len, pos, &dx) && get_varint(d, len, pos, &dy);
}

/* walk the whole stream once: well formed, slots in range (slot `slots` is the pointer, when the recording has it) */
static bool validate(const uint8_t *d, size_t len, size_t pos, int slots, bool pointer) {
  uint32_t ptr_slot = pointer ? (uint32_t)slots : UINT32_MAX;
  for (;;) {
    uint32_t idle, n;
    if (!get_varint(d, len, &pos, &idle) || !get_varint(d, len, &pos, &n)) return false;
    if (n == 0) return pos == len;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t slot;
      if (!get_varint(d, len, &pos, &slot) || (slot >= (uint32_t)slots && slot != ptr_slot)) return false;
      if (!skip_change(d, len, &pos, slot, ptr_slot)) return false;
    }
  }
}

void cw__input_replay_free(void) {
  cw_free(rep.data);
  memset(&rep, 0, sizeof rep);
  cw_array_free(&rec.header);
  cw_array_free(&rec.body);
  cw_array_free(&rec.out);
  memset(&rec, 0, sizeof rec);
}

bool cw_input_replay_begin(const uint8_t *data, size_t len) {
  cw__input_ensure_init();
  cw_input_replay_end();
  if (!data || len < 7 || memcmp(data, MAGIC, 4) != 0 || (data[4] != VERSION && data[4] != VERSION_PTR)) {
    cw_error("input replay: not a cartwheel input recording (CWIR v%d or v%d)", VERSION, VERSION_PTR);
    return false;
  }
  int players = data[5], count = data[6];
  if (players < 1 || players > CW_INPUT_MAX_PLAYERS || count > CW_INPUT_MAX_ACTIONS) { /* a 4-player recording plays on 8 */
    cw_error("input replay: recorded with %d players / %d actions; this build has at most %d / %d", players, count,
             CW_INPUT_MAX_PLAYERS, CW_INPUT_MAX_ACTIONS);
    return false;
  }
  size_t pos = 7;
  cw_action map[CW_INPUT_MAX_ACTIONS];
  for (int a = 0; a < count; a++) {
    if (pos >= len || pos + 1 + data[pos] > len) goto bad;
    char name[256];
    memcpy(name, data + pos + 1, data[pos]);
    name[data[pos]] = 0;
    pos += 1 + (size_t)data[pos];
    map[a] = cw_action_find(name);
    if (map[a] < 0) cw_warn("input replay: recorded action '%s' is not defined here; its input is dropped", name);
  }
  bool pointer = false;
  if (data[4] == VERSION_PTR) { /* the channels byte */
    if (pos >= len) goto bad;
    pointer = data[pos++] & 1u;
  }
  if (!validate(data, len, pos, players * count, pointer)) goto bad;
  rep.data = cw_alloc(len, CW_MEM_CORE);
  memcpy(rep.data, data, len);
  rep.len = len;
  rep.pos = rep.body = pos;
  rep.count = count;
  rep.players = players;
  memcpy(rep.map, map, sizeof map);
  rep.pointer = pointer;
  rep.hash = CW_HASH32_INIT;
  rep.ticks = 0;
  rep.on = true;
  return true;
bad:
  cw_error("input replay: recording is truncated or corrupt");
  return false;
}

void cw_input_replay_end(void) {
  uint32_t ticks = rep.ticks, hash = rep.hash; /* still readable after the end */
  cw_free(rep.data);
  memset(&rep, 0, sizeof rep);
  rep.ticks = ticks;
  rep.hash = hash;
}
bool cw_input_replay_active(void) { return rep.on; }
bool cw__input_replay_pointer(void) { return rep.on && rep.pointer; }
uint32_t cw_input_replay_ticks(void) { return rep.ticks; }
uint32_t cw_input_replay_hash(void) { return rep.hash; }

static void apply(size_t pos, uint32_t n) {
  uint32_t ptr_slot = rep.pointer ? (uint32_t)(rep.players * rep.count) : UINT32_MAX;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t slot = 0;
    get_varint(rep.data, rep.len, &pos, &slot);
    if (slot == ptr_slot) {
      uint32_t dx = 0, dy = 0;
      rep.snap.ptr.flags = rep.data[pos++];
      get_varint(rep.data, rep.len, &pos, &dx);
      get_varint(rep.data, rep.len, &pos, &dy);
      rep.snap.ptr.x = (int32_t)((uint32_t)rep.snap.ptr.x + (uint32_t)unzigzag(dx));
      rep.snap.ptr.y = (int32_t)((uint32_t)rep.snap.ptr.y + (uint32_t)unzigzag(dy));
      continue;
    }
    cw__slot *s = &rep.snap.s[slot / (uint32_t)rep.count][slot % (uint32_t)rep.count];
    s->flags = rep.data[pos];
    s->raw = (int16_t)(uint16_t)(rep.data[pos + 1] | (rep.data[pos + 2] << 8));
    pos += 3;
  }
}

/* advance the recording's own snapshot one tick; false at the end */
static bool advance(void) {
  if (rep.idle_left > 0) {
    rep.idle_left--;
    return true;
  }
  if (rep.pending) {
    apply(rep.pending, rep.pending_n);
    rep.pending = 0;
    return true;
  }
  uint32_t idle, n;
  if (!get_varint(rep.data, rep.len, &rep.pos, &idle) || !get_varint(rep.data, rep.len, &rep.pos, &n)) return false;
  if (n == 0) {
    rep.pos = rep.len;
    if (idle == 0) return false;
    rep.idle_left = idle - 1;
    return true;
  }
  size_t at = rep.pos;
  uint32_t ptr_slot = rep.pointer ? (uint32_t)(rep.players * rep.count) : UINT32_MAX;
  for (uint32_t i = 0; i < n; i++) { /* skip past the changes (validated) */
    uint32_t slot = 0;
    get_varint(rep.data, rep.len, &rep.pos, &slot);
    skip_change(rep.data, rep.len, &rep.pos, slot, ptr_slot);
  }
  if (idle > 0) {
    rep.idle_left = idle - 1;
    rep.pending = at;
    rep.pending_n = n;
    return true;
  }
  apply(at, n);
  return true;
}

bool cw__input_replay_next(void) {
  if (!rep.on) return false;
  if (!advance()) {
    cw_input_replay_end();
    return false;
  }
  rep.ticks++;
  rep.hash = cw__snapshot_hash(&rep.snap, rep.count, rep.hash);
  if (rep.pointer) rep.hash = cw__pointer_hash(&rep.snap.ptr, rep.hash);
  memset(&I.tick_replay, 0, sizeof I.tick_replay);
  I.tick_replay.ptr = rep.snap.ptr;
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++)
    for (int a = 0; a < rep.count; a++)
      if (rep.map[a] >= 0) I.tick_replay.s[p][rep.map[a]] = rep.snap.s[p][a];
  return true;
}

bool cw_input_replay_step(void) {
  if (!cw__input_replay_next()) return false;
  I.tick = &I.tick_replay;
  I.ctx = CW_INPUT_TICK;
  return true;
}

/* ── the debug channel: record ───────────────────────────────────────
 * An editor records a play session (record start ... record stop), fetches
 * it (record fetch, base64 in pieces) and replays it later as a bug report
 * or a regression run (record replay). */
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/vfs.h"

#define REC_CHUNK (96 * 1024)

static void rec_status(cw_reply *r) {
  size_t len = 0;
  cw_input_recording(&len);
  cw_reply_printf(r, "{\"recording\":%s,\"ticks\":%u,\"hash\":%u,\"bytes\":%zu,\"replaying\":%s,\"replay_ticks\":%u,\"tick\":%llu}",
                  rec.on ? "true" : "false", rec.ticks, rec.hash, len, rep.on ? "true" : "false", rep.ticks,
                  (unsigned long long)cw_tick_index());
}

static void cmd_record(int argc, const char **argv, cw_reply *r) {
  const char *op = argc > 1 ? argv[1] : "status";
  if (!strcmp(op, "start")) {
    cw_input_record_begin();
    rec_status(r);
  } else if (!strcmp(op, "stop")) {
    cw_input_record_end();
    rec_status(r);
  } else if (!strcmp(op, "status")) {
    rec_status(r);
  } else if (!strcmp(op, "fetch")) {
    size_t len = 0;
    const uint8_t *d = cw_input_recording(&len);
    if (!d) { cw_reply_error(r, "nothing recorded yet (record start, play, record stop, then record fetch)"); return; }
    long off = argc > 2 ? strtol(argv[2], NULL, 10) : 0;
    if (off < 0 || (size_t)off > len) { cw_reply_error(r, "offset %ld is past the recording's %zu bytes", off, len); return; }
    size_t n = len - (size_t)off < REC_CHUNK ? len - (size_t)off : REC_CHUNK;
    char *b64 = cw_base64_encode(d + off, n);
    cw_reply_printf(r, "{\"offset\":%ld,\"bytes\":%zu,\"total\":%zu,\"ticks\":%u,\"hash\":%u,\"more\":%s,\"base64\":\"", off, n, len,
                    rec.ticks, rec.hash, (size_t)off + n < len ? "true" : "false");
    cw_reply_raw(r, b64);
    cw_reply_raw(r, "\"}");
    cw_free(b64);
  } else if (!strcmp(op, "replay") && argc > 2) {
    size_t len = 0;
    void *d;
    bool from_vfs = !strncmp(argv[2], "vfs:", 4);
    if (from_vfs) d = cw_vfs_read(argv[2] + 4, &len);
    else d = cw_base64_decode(argv[2], &len);
    bool ok = d && cw_input_replay_begin(d, len);
    if (from_vfs) free(d);
    else cw_free(d);
    if (!ok) {
      cw_reply_error(r, "not a CWIR recording (record fetch gives one; over 4 KB, put it in the project layer with "
                        "vfs write/append and replay vfs:<path>)");
      return;
    }
    rec_status(r);
  } else if (!strcmp(op, "replay_stop")) {
    cw_input_replay_end();
    rec_status(r);
  } else {
    cw_reply_error(r, "record takes start, stop, status, fetch [offset], replay <base64 | vfs:path> or replay_stop, not '%s'", op);
  }
}

void cw__input_register_commands(void) {
  cw_debug_command("record", "record start|stop|status|fetch [offset]|replay <base64 | vfs:path>|replay_stop: record the "
                             "input stream (action snapshots per tick), fetch it as base64, replay it", cmd_record);
}

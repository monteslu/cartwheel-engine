/* Rollback netcode on the deterministic fixed step (cartwheel/net.h).
 *
 * Tick T means "simulate the tree from state[T] with every player's input for
 * T, giving state[T+1]". state[] is a ring of tree snapshots; inputs are a
 * ring of known inputs per player plus the input each simulated tick USED
 * (known or predicted), which is what a late input is compared against.
 *
 * RB_INPUT [u8 player][u32 start][u8 count][u32 ack][u32 sender_tick][count * input_size]
 *   ack: how many of the DESTINATION player's inputs the sender has (contiguous
 *   from tick 0), so the destination resends only from there.
 * RB_HASH  [u32 tick][u32 hash]   the confirmed state after `tick` ticks */
#include <stdio.h>
#include <stdlib.h>

#include "net_internal.h"
#include "cartwheel/core.h"
#include "scene/scene_internal.h"

#define IN_RING 256
#define HASH_RING 1024
#define REMOTE_HASHES 64
#define NO_TICK UINT64_MAX

struct cw_rollback {
  cw_rollback_desc d;
  int local;   /* local slot, -1 when every player is local */
  bool started, resim;
  uint64_t tick, sim_tick;
  uint8_t known[IN_RING][CW_NET_MAX_PEERS][CW_ROLLBACK_MAX_INPUT];
  uint8_t used[IN_RING][CW_NET_MAX_PEERS][CW_ROLLBACK_MAX_INPUT];
  bool have[IN_RING][CW_NET_MAX_PEERS];
  uint64_t have_tick[IN_RING][CW_NET_MAX_PEERS]; /* which tick a `have` slot holds */
  uint64_t have_until[CW_NET_MAX_PEERS];         /* every input of p before this tick is known */
  bool dropped[CW_NET_MAX_PEERS];
  uint64_t remote_ack[CW_NET_MAX_PEERS];         /* the peer has our inputs before this tick */
  uint64_t remote_tick[CW_NET_MAX_PEERS];
  bool waited_last;
  int nstates;
  cw_state **states;
  uint64_t *state_tick;
  uint64_t rollback_from;
  uint32_t hash[HASH_RING];
  uint64_t hashed_until; /* hashes exist for ticks < hashed_until */
  struct { uint64_t tick; uint32_t hash; bool set; } rhash[CW_NET_MAX_PEERS][REMOTE_HASHES];
  cw_rollback_stats st;
};

static void free_nothing(void *p) { (void)p; }

cw_rollback *cw_rollback_of(cw_tree *t) { return t ? cw_tree_slot(t, "cw.rollback") : NULL; }
bool cw_rollback_resimulating(cw_rollback *rb) { return rb->resim; }
uint64_t cw_rollback_tick(cw_rollback *rb) { return rb->tick; }
cw_rollback_stats cw_rollback_get_stats(cw_rollback *rb) { return rb->st; }

static uint64_t confirmed(cw_rollback *rb) {
  uint64_t c = NO_TICK;
  for (int p = 0; p < rb->d.players; p++) c = CW_MIN(c, rb->have_until[p]);
  return c;
}
uint64_t cw_rollback_confirmed_tick(cw_rollback *rb) { return confirmed(rb); }

const void *cw_rollback_input(cw_tree *t, int player) {
  static const uint8_t zero[CW_ROLLBACK_MAX_INPUT];
  cw_rollback *rb = cw_rollback_of(t);
  if (!rb || player < 0 || player >= rb->d.players) return zero;
  return rb->used[rb->sim_tick % IN_RING][player];
}

static cw_state *state_at(cw_rollback *rb, uint64_t tick) {
  int i = (int)(tick % (uint64_t)rb->nstates);
  return rb->state_tick[i] == tick ? rb->states[i] : NULL;
}
static void save_state(cw_rollback *rb, uint64_t tick) {
  int i = (int)(tick % (uint64_t)rb->nstates);
  double t0 = cw__net_wall_ms();
  cw_state_save(rb->states[i], rb->d.tree);
  rb->st.save_ms_total += cw__net_wall_ms() - t0;
  rb->state_tick[i] = tick;
  rb->st.state_bytes = (uint32_t)cw_state_bytes(rb->states[i]);
}

/* record a known input; flags a rollback when a simulated tick used another */
static void put_input(cw_rollback *rb, int p, uint64_t t, const uint8_t *in) {
  if (t < rb->have_until[p] || t >= rb->tick + IN_RING / 2) return;
  int slot = (int)(t % IN_RING);
  if (rb->have[slot][p] && rb->have_tick[slot][p] == t) return;
  memcpy(rb->known[slot][p], in, (size_t)rb->d.input_size);
  rb->have[slot][p] = true;
  rb->have_tick[slot][p] = t;
  if (t < rb->tick && memcmp(rb->used[slot][p], in, (size_t)rb->d.input_size)) {
    rb->st.mispredictions++;
    if (rb->rollback_from == NO_TICK || t < rb->rollback_from) rb->rollback_from = t;
  }
  while (rb->have[rb->have_until[p] % IN_RING][p] && rb->have_tick[rb->have_until[p] % IN_RING][p] == rb->have_until[p])
    rb->have_until[p]++;
}

static void compare_hash(cw_rollback *rb, int p, uint64_t tick, uint32_t theirs) {
  uint32_t ours = rb->hash[tick % HASH_RING];
  rb->st.hashes_checked++;
  if (ours != theirs) {
    rb->st.desyncs++;
    if (rb->st.first_desync_tick < 0) {
      rb->st.first_desync_tick = (int64_t)tick;
      cw_error("rollback: DESYNC with player %d at tick %llu (ours %08x, theirs %08x)", p, (unsigned long long)tick, ours, theirs);
    }
  }
}

static void on_input(cw_net_session *s, int from, const uint8_t *data, int len, void *user) {
  (void)s;
  cw_rollback *rb = user;
  cw__rbuf r = { data, len, 0, false };
  int p = cw__r_u8(&r);
  uint32_t start = cw__r_u32(&r);
  int count = cw__r_u8(&r);
  uint32_t ack = cw__r_u32(&r), their_tick = cw__r_u32(&r);
  if (r.bad || p != from || p >= rb->d.players || r.pos + count * rb->d.input_size > len) return;
  if (ack > rb->remote_ack[p]) rb->remote_ack[p] = ack;
  if (their_tick > rb->remote_tick[p]) rb->remote_tick[p] = their_tick;
  for (int i = 0; i < count; i++) put_input(rb, p, (uint64_t)start + (uint64_t)i, data + r.pos + i * rb->d.input_size);
}
static void on_hash(cw_net_session *s, int from, const uint8_t *data, int len, void *user) {
  (void)s;
  cw_rollback *rb = user;
  cw__rbuf r = { data, len, 0, false };
  uint64_t tick = cw__r_u32(&r);
  uint32_t h = cw__r_u32(&r);
  if (r.bad || from >= rb->d.players) return;
  if (tick < rb->hashed_until && tick + HASH_RING > rb->hashed_until) compare_hash(rb, from, tick, h);
  else {
    int i = (int)(tick % REMOTE_HASHES);
    rb->rhash[from][i].tick = tick;
    rb->rhash[from][i].hash = h;
    rb->rhash[from][i].set = true;
  }
}
static void on_peer(cw_net_session *s, int player, bool connected, void *user) {
  (void)s;
  cw_rollback *rb = user;
  if (!connected && player < rb->d.players && rb->started) {
    rb->dropped[player] = true;
    cw_warn("rollback: player %d dropped; its input repeats from now on", player);
  }
}

cw_rollback *cw_rollback_new(const cw_rollback_desc *desc) {
  CW_ASSERT(desc->tree, "cw_rollback_desc.tree is required");
  CW_ASSERT(desc->players >= 1 && desc->players <= CW_NET_MAX_PEERS, "players: 1..CW_NET_MAX_PEERS");
  CW_ASSERT(desc->input_size >= 1 && desc->input_size <= CW_ROLLBACK_MAX_INPUT, "input_size: 1..CW_ROLLBACK_MAX_INPUT bytes");
  CW_ASSERT(desc->sample_input, "cw_rollback_desc.sample_input is required");
  cw_rollback *rb = cw_calloc(1, sizeof *rb, CW_MEM_CORE);
  rb->d = *desc;
  if (rb->d.input_delay < 0) rb->d.input_delay = 0; /* 0 is a real value: no delay */
  if (rb->d.max_prediction <= 0) rb->d.max_prediction = 8;
  CW_ASSERT(rb->d.max_prediction + rb->d.input_delay < IN_RING / 4, "max_prediction + input_delay must stay under 64");
  if (rb->d.hash_interval <= 0) rb->d.hash_interval = 10;
  if (rb->d.dt <= 0) rb->d.dt = 1.0 / 60;
  rb->local = -1;
  rb->rollback_from = NO_TICK;
  rb->st.first_desync_tick = -1;
  rb->nstates = rb->d.max_prediction + 3;
  rb->states = cw_calloc((size_t)rb->nstates, sizeof *rb->states, CW_MEM_CORE);
  rb->state_tick = cw_calloc((size_t)rb->nstates, sizeof *rb->state_tick, CW_MEM_CORE);
  for (int i = 0; i < rb->nstates; i++) {
    rb->states[i] = cw_state_new();
    rb->state_tick[i] = NO_TICK;
  }
  /* the first input_delay ticks have no input: neutral and known for everyone */
  for (int p = 0; p < rb->d.players; p++) {
    for (int t = 0; t < rb->d.input_delay; t++) {
      rb->have[t][p] = true;
      rb->have_tick[t][p] = (uint64_t)t;
    }
    rb->have_until[p] = (uint64_t)rb->d.input_delay;
  }
  cw_tree_set_slot(rb->d.tree, "cw.rollback", rb, free_nothing);
  if (rb->d.session) {
    cw_net_on_message(rb->d.session, CW__NM_RB_INPUT, on_input, rb);
    cw_net_on_message(rb->d.session, CW__NM_RB_HASH, on_hash, rb);
    cw_net_on_peer(rb->d.session, on_peer, rb);
  }
  return rb;
}

void cw_rollback_free(cw_rollback *rb) {
  if (!rb) return;
  if (cw_tree_slot(rb->d.tree, "cw.rollback") == rb) cw_tree_set_slot(rb->d.tree, "cw.rollback", NULL, NULL);
  if (rb->d.session) {
    cw_net_on_message(rb->d.session, CW__NM_RB_INPUT, NULL, NULL);
    cw_net_on_message(rb->d.session, CW__NM_RB_HASH, NULL, NULL);
    cw_net_off_peer(rb->d.session, on_peer, rb);
  }
  for (int i = 0; i < rb->nstates; i++) cw_state_free(rb->states[i]);
  cw_free(rb->states);
  cw_free(rb->state_tick);
  cw_free(rb);
}

bool cw_rollback_state_hash(cw_rollback *rb, uint64_t tick, uint32_t *hash) {
  if (tick >= rb->hashed_until || tick + HASH_RING <= rb->hashed_until) return false;
  *hash = rb->hash[tick % HASH_RING];
  return true;
}

/* simulate tick t from the current tree state (which must be state[t]) */
static void sim_tick(cw_rollback *rb, uint64_t t) {
  int slot = (int)(t % IN_RING);
  for (int p = 0; p < rb->d.players; p++) {
    if (rb->have[slot][p] && rb->have_tick[slot][p] == t) memcpy(rb->used[slot][p], rb->known[slot][p], (size_t)rb->d.input_size);
    else { /* predict: the player's newest known input */
      uint64_t last = rb->have_until[p] - 1;
      memcpy(rb->used[slot][p], rb->known[last % IN_RING][p], (size_t)rb->d.input_size);
    }
  }
  rb->sim_tick = t;
  cw_tree_fixed_update(rb->d.tree, rb->d.dt);
  save_state(rb, t + 1);
}

static void send_inputs(cw_rollback *rb) {
  cw_net_session *s = rb->d.session;
  int me = rb->local;
  if (!s || me < 0) return;
  uint64_t newest = rb->have_until[me]; /* exclusive */
  for (int p = 0; p < rb->d.players; p++) {
    if (p == me || !cw_net_peer_connected(s, p)) continue;
    uint64_t start = CW_MAX(rb->remote_ack[p], newest > 64 ? newest - 64 : 0);
    int count = (int)(newest - start);
    if (count <= 0) count = 0;
    uint8_t buf[CW_NET_MAX_MESSAGE];
    cw__wbuf w = { buf, 0, sizeof buf, false };
    cw__w_u8(&w, (uint8_t)me);
    cw__w_u32(&w, (uint32_t)start);
    cw__w_u8(&w, (uint8_t)count);
    cw__w_u32(&w, (uint32_t)rb->have_until[p]);
    cw__w_u32(&w, (uint32_t)rb->tick);
    for (int i = 0; i < count; i++) cw__w_bytes(&w, rb->known[(start + (uint64_t)i) % IN_RING][me], rb->d.input_size);
    if (!w.overflow) cw_net_send(s, p, CW_NET_UNRELIABLE, CW__NM_RB_INPUT, buf, w.len);
  }
}

static void hash_confirmed(cw_rollback *rb) {
  uint64_t c = CW_MIN(confirmed(rb), rb->tick);
  for (uint64_t t = rb->hashed_until; t <= c; t++) {
    cw_state *st = state_at(rb, t);
    if (!st) { /* fell out of the ring (should not happen: the stall rule keeps it) */
      cw_warn("rollback: state %llu left the ring before it was confirmed", (unsigned long long)t);
      rb->hashed_until = t + 1;
      continue;
    }
    uint32_t h = cw_state_hash(st);
    rb->hash[t % HASH_RING] = h;
    rb->hashed_until = t + 1;
    if (rb->d.session && t % (uint64_t)rb->d.hash_interval == 0) {
      uint8_t buf[8];
      cw__wbuf w = { buf, 0, sizeof buf, false };
      cw__w_u32(&w, (uint32_t)t);
      cw__w_u32(&w, h);
      cw_net_send(rb->d.session, CW_NET_ALL, CW_NET_RELIABLE, CW__NM_RB_HASH, buf, w.len);
    }
    for (int p = 0; p < rb->d.players; p++) {
      int i = (int)(t % REMOTE_HASHES);
      if (rb->rhash[p][i].set && rb->rhash[p][i].tick == t) {
        compare_hash(rb, p, t, rb->rhash[p][i].hash);
        rb->rhash[p][i].set = false;
      }
    }
  }
}

int cw_rollback_advance(cw_rollback *rb, double now) {
  cw_net_session *s = rb->d.session;
  rb->st.frames++;
  if (s) {
    cw_net_poll(s, now);
    rb->local = cw_net_local_player(s);
    if (!rb->started) {
      bool all = rb->local >= 0 && rb->local < rb->d.players;
      for (int p = 0; p < rb->d.players && all; p++)
        if (p != rb->local && !cw_net_peer_connected(s, p)) all = false;
      if (!all) return 0; /* waiting for every player */
      rb->started = true;
      cw_info("rollback: every player is here; we are player %d of %d", rb->local, rb->d.players);
    }
  }
  if (rb->state_tick[0] == NO_TICK && rb->tick == 0) save_state(rb, 0);

  /* 1. correct mispredictions */
  if (rb->rollback_from != NO_TICK) {
    uint64_t from = rb->rollback_from;
    rb->rollback_from = NO_TICK;
    cw_state *st = state_at(rb, from);
    if (!rb->d.no_rollback && st && from < rb->tick) {
      double t0 = cw__net_wall_ms();
      cw_state_load(st, rb->d.tree);
      rb->resim = true;
      for (uint64_t t = from; t < rb->tick; t++) sim_tick(rb, t);
      rb->resim = false;
      double ms = cw__net_wall_ms() - t0;
      rb->st.rollbacks++;
      rb->st.resim_ticks += (uint32_t)(rb->tick - from);
      rb->st.max_depth = CW_MAX(rb->st.max_depth, (uint32_t)(rb->tick - from));
      rb->st.rollback_ms_total += ms;
      rb->st.rollback_ms_max = CW_MAX(rb->st.rollback_ms_max, ms);
    } else if (!st && !rb->d.no_rollback) {
      cw_error("rollback: state %llu is gone; cannot correct (raise max_prediction)", (unsigned long long)from);
    }
  }
  /* dropped players: their input repeats, and counts as known */
  for (int p = 0; p < rb->d.players; p++)
    while (rb->dropped[p] && rb->have_until[p] <= rb->tick) {
      uint64_t t = rb->have_until[p];
      put_input(rb, p, t, rb->known[(t - 1) % IN_RING][p]);
    }
  hash_confirmed(rb);

  /* 2. may we run another tick? */
  int ran = 0;
  uint64_t conf = confirmed(rb);
  bool wait = false;
  if (rb->tick >= conf + (uint64_t)rb->d.max_prediction) {
    rb->st.stalls++;
    wait = true;
  } else if (s && !rb->waited_last) {
    /* time sync: slow down a tick when clearly ahead of a remote */
    for (int p = 0; p < rb->d.players; p++) {
      if (p == rb->local || rb->dropped[p]) continue;
      double rtt = cw_net_get_peer_stats(s, p).rtt;
      double ahead = (double)rb->tick - ((double)rb->remote_tick[p] + rtt * 0.5 / rb->d.dt);
      if (ahead > 2.0) wait = true;
    }
    if (wait) rb->st.sync_waits++;
  }
  rb->waited_last = wait;
  if (!wait) {
    /* 3. sample local input for tick + delay, then simulate this tick */
    uint64_t at = rb->tick + (uint64_t)rb->d.input_delay;
    for (int p = 0; p < rb->d.players; p++) {
      if (s && p != rb->local) continue;
      uint8_t in[CW_ROLLBACK_MAX_INPUT] = { 0 };
      rb->d.sample_input(rb, p, in, rb->d.user);
      put_input(rb, p, at, in);
    }
    sim_tick(rb, rb->tick);
    rb->tick++;
    ran = 1;
    hash_confirmed(rb);
  }
  send_inputs(rb);
  if (s) cw_net_flush(s);
  return ran;
}

const cw_state *cw_rollback_saved_state(cw_rollback *rb, uint64_t tick) { return state_at(rb, tick); }

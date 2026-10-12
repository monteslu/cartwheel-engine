/* Sessions: player slots over transport links, a handshake, reliable-ordered
 * and unreliable messages in shared packets, acks, resends, RTT, timeouts.
 *
 * Packet: [u8 0xC7][u8 from_slot][u8 flags][u8 0][u16 seq][u16 ack][u32 ack_bits]
 *         then messages: [u8 type][u8 channel][u16 len][u16 rseq if reliable][payload]
 * flags bit 0: the sender has our HELLO (we stop repeating it); bit 1: an
 * unsequenced HELLO to a link no slot is bound to yet.
 * ack / ack_bits: the newest packet seq received from the destination and a
 * bit per each of the 32 before it. A reliable message is resent until a
 * packet carrying it is acked, and delivered in rseq order exactly once. */
#include <stdio.h>
#include <stdlib.h>

#include "net_internal.h"
#include "cartwheel/core.h"

#define MAGIC 0xC7
#define HDR 12
#define PKT_RING 256
#define REL_WINDOW 256
#define MAX_LINKS 256
#define KEEPALIVE 0.1
#define ACK_DELAY 0.03

typedef struct pend_msg {
  uint16_t rseq;
  uint8_t type;
  int len;
  uint8_t *data;
  double last_sent;
  bool sent;
} pend_msg;
typedef struct unrel_msg {
  uint8_t type;
  int len;
  uint8_t *data;
} unrel_msg;
typedef struct pkt_rec {
  bool used, acked;
  uint16_t seq;
  double time;
  int nrel;
  uint16_t rseqs[48];
} pkt_rec;
typedef struct rel_slot {
  bool has;
  uint8_t type;
  int len;
  uint8_t *data;
} rel_slot;

typedef struct peer {
  bool connected, they_know_us;
  int link;
  double last_recv, last_sent;
  uint16_t send_seq;
  bool any_recv, ack_owed;
  uint16_t recv_max;
  uint32_t recv_bits;
  uint16_t rel_next, rel_expect;
  CW_ARRAY(pend_msg) pending;
  CW_ARRAY(unrel_msg) unrel;
  pkt_rec ring[PKT_RING];
  rel_slot inbox[REL_WINDOW];
  double rtt;
  bool rtt_init;
  cw_net_peer_stats st;
} peer;

typedef struct link_rec {
  bool open;
  int slot;        /* -1 until a HELLO bound it */
  double last_hello;
  bool refused_logged;
} link_rec;

struct cw_net_session {
  cw_net_transport *tr;
  int local;
  uint32_t protocol;
  double timeout, now;
  peer peers[CW_NET_MAX_PEERS];
  link_rec links[MAX_LINKS];
  struct { cw_net_msg_fn fn; void *user; } handlers[256];
  struct { cw_net_peer_fn fn; void *user; } peer_fns[8];
  cw_tree *tree;
  cw_node root;
  cw_map authority; /* node uid -> player + 1 */
  void *repl;
  uint8_t pkt[CW_NET_MAX_PACKET];
};

static inline bool seq_newer(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }

static void peer_reset(peer *p) {
  for (int i = 0; i < p->pending.count; i++) cw_free(p->pending.data[i].data);
  for (int i = 0; i < p->unrel.count; i++) cw_free(p->unrel.data[i].data);
  cw_array_free(&p->pending);
  cw_array_free(&p->unrel);
  for (int i = 0; i < REL_WINDOW; i++) cw_free(p->inbox[i].data);
  memset(p, 0, sizeof *p);
  p->link = -1;
  p->rtt = 0.2;
}

cw_net_session *cw_net_session_new(const cw_net_session_desc *d) {
  if (!d->transport) {
    cw_error("cw_net_session_new: no transport (cw_net_udp_new / cw_net_wcpeer_new / cw_net_loopback_endpoint)");
    return NULL;
  }
  CW_ASSERT(d->local_player >= -1 && d->local_player < CW_NET_MAX_PEERS, "local_player is a slot 0..CW_NET_MAX_PEERS-1, or -1");
  cw_net_session *s = cw_calloc(1, sizeof *s, CW_MEM_CORE);
  s->tr = d->transport;
  s->local = d->local_player;
  s->protocol = d->protocol;
  s->timeout = d->timeout > 0 ? d->timeout : 5.0;
  for (int i = 0; i < CW_NET_MAX_PEERS; i++) peer_reset(&s->peers[i]);
  for (int i = 0; i < MAX_LINKS; i++) s->links[i].slot = -1;
  cw_map_init(&s->authority, CW_MEM_CORE);
  cw__net_replicate_init(s);
  return s;
}

static void send_bye(cw_net_session *s);
void cw_net_session_free(cw_net_session *s) {
  if (!s) return;
  send_bye(s);
  if (s->tree && cw_tree_slot(s->tree, "cw.net") == s) cw_tree_set_slot(s->tree, "cw.net", NULL, NULL);
  cw__net_replicate_free(s);
  for (int i = 0; i < CW_NET_MAX_PEERS; i++) peer_reset(&s->peers[i]);
  cw_map_free(&s->authority);
  cw_net_transport_destroy(s->tr);
  cw_free(s);
}

bool cw_net_connect(cw_net_session *s, const char *addr) {
  if (!s) {
    cw_error("cw_net_connect: no session (cw_net_session_new returned NULL: check its transport)");
    return false;
  }
  int l = s->tr->open(s->tr, addr);
  if (l < 0) {
    cw_warn("net: cannot open '%s' on the %s transport", addr, s->tr->name);
    return false;
  }
  return true;
}

double cw_net_now(cw_net_session *s) { return s->now; }
int cw_net_local_player(cw_net_session *s) { return s->local; }
bool cw_net_peer_connected(cw_net_session *s, int p) { return p >= 0 && p < CW_NET_MAX_PEERS && s->peers[p].connected; }
int cw_net_peer_count(cw_net_session *s) {
  int n = 0;
  for (int i = 0; i < CW_NET_MAX_PEERS; i++) n += s->peers[i].connected;
  return n;
}
cw_net_peer_stats cw_net_get_peer_stats(cw_net_session *s, int p) {
  if (p < 0 || p >= CW_NET_MAX_PEERS) return (cw_net_peer_stats){ 0 };
  cw_net_peer_stats st = s->peers[p].st;
  st.rtt = s->peers[p].rtt;
  return st;
}
void cw_net_on_message(cw_net_session *s, uint8_t type, cw_net_msg_fn fn, void *user) {
  s->handlers[type].fn = fn;
  s->handlers[type].user = user;
}
static void peer_event(cw_net_session *s, int slot, bool connected) {
  for (int i = 0; i < 8; i++)
    if (s->peer_fns[i].fn) s->peer_fns[i].fn(s, slot, connected, s->peer_fns[i].user);
}
void cw_net_on_peer(cw_net_session *s, cw_net_peer_fn fn, void *user) {
  for (int i = 0; i < 8; i++)
    if (s->peer_fns[i].fn == fn && s->peer_fns[i].user == user) return;
  for (int i = 0; i < 8; i++)
    if (!s->peer_fns[i].fn) {
      s->peer_fns[i].fn = fn;
      s->peer_fns[i].user = user;
      return;
    }
  cw_error("cw_net_on_peer: 8 listeners already");
}
void cw_net_off_peer(cw_net_session *s, cw_net_peer_fn fn, void *user) {
  for (int i = 0; i < 8; i++)
    if (s->peer_fns[i].fn == fn && s->peer_fns[i].user == user) s->peer_fns[i].fn = NULL;
}

bool cw_net_send(cw_net_session *s, int player, cw_net_channel ch, uint8_t type, const void *data, int len) {
  if (len < 0 || len > CW_NET_MAX_MESSAGE) {
    cw_error("cw_net_send: %d bytes is over CW_NET_MAX_MESSAGE (%d): split the message", len, CW_NET_MAX_MESSAGE);
    return false;
  }
  bool any = false;
  for (int i = 0; i < CW_NET_MAX_PEERS; i++) {
    if (!s->peers[i].connected || (player != CW_NET_ALL && player != i)) continue;
    peer *p = &s->peers[i];
    uint8_t *copy = cw_alloc((size_t)CW_MAX(len, 1), CW_MEM_CORE);
    memcpy(copy, data, (size_t)len);
    if (ch == CW_NET_RELIABLE) cw_array_push(&p->pending, ((pend_msg){ p->rel_next++, type, len, copy, 0, false }));
    else cw_array_push(&p->unrel, ((unrel_msg){ type, len, copy }));
    any = true;
  }
  return any;
}

/* ── packets out ─────────────────────────────────────────────────────── */

static void write_header(cw__wbuf *w, cw_net_session *s, peer *p, uint16_t seq, int flags) {
  cw__w_u8(w, MAGIC);
  cw__w_u8(w, (uint8_t)s->local);
  cw__w_u8(w, (uint8_t)flags);
  cw__w_u8(w, 0);
  cw__w_u16(w, seq);
  cw__w_u16(w, p ? p->recv_max : 0);
  cw__w_u32(w, p && p->any_recv ? p->recv_bits : 0);
}
static void write_hello(cw__wbuf *w, cw_net_session *s) {
  cw__w_u8(w, CW__NM_HELLO);
  cw__w_u8(w, CW_NET_UNRELIABLE);
  cw__w_u16(w, 5);
  cw__w_u32(w, s->protocol);
  cw__w_u8(w, (uint8_t)s->local);
}

/* a HELLO-only packet to a link no slot is bound to yet */
static void send_link_hello(cw_net_session *s, int link) {
  cw__wbuf w = { s->pkt, 0, sizeof s->pkt, false };
  write_header(&w, s, NULL, 0, 2); /* unsequenced */
  write_hello(&w, s);
  s->tr->send(s->tr, link, w.p, w.len);
  s->links[link].last_hello = s->now;
}

static bool flush_peer(cw_net_session *s, int slot, bool force) {
  peer *p = &s->peers[slot];
  double rto = CW_MAX(0.05, p->rtt * 1.25 + 0.02);
  bool want = force || p->unrel.count || !p->they_know_us;
  for (int i = 0; i < p->pending.count && !want; i++)
    if (!p->pending.data[i].sent || s->now - p->pending.data[i].last_sent >= rto) want = true;
  if (!want && p->ack_owed && s->now - p->last_sent >= ACK_DELAY) want = true;
  if (!want && s->now - p->last_sent >= KEEPALIVE) want = true;
  if (!want) return false;

  for (int round = 0; round < 16; round++) {
    uint16_t seq = p->send_seq++;
    pkt_rec *rec = &p->ring[seq % PKT_RING];
    if (rec->used && !rec->acked) p->st.packets_lost++;
    *rec = (pkt_rec){ .used = true, .seq = seq, .time = s->now };
    cw__wbuf w = { s->pkt, 0, sizeof s->pkt, false };
    write_header(&w, s, p, seq, 1);
    if (!p->they_know_us) write_hello(&w, s);
    bool more = false;
    /* reliable first (oldest first), then unreliable */
    for (int i = 0; i < p->pending.count; i++) {
      pend_msg *m = &p->pending.data[i];
      if (m->sent && s->now - m->last_sent < rto) continue;
      if (rec->nrel == (int)CW_ARRAY_LEN(rec->rseqs) || w.len + 6 + m->len > w.cap) { more = true; continue; }
      cw__w_u8(&w, m->type);
      cw__w_u8(&w, CW_NET_RELIABLE);
      cw__w_u16(&w, (uint16_t)m->len);
      cw__w_u16(&w, m->rseq);
      cw__w_bytes(&w, m->data, m->len);
      if (m->sent) p->st.resends++;
      m->sent = true;
      m->last_sent = s->now;
      rec->rseqs[rec->nrel++] = m->rseq;
    }
    int kept = 0;
    for (int i = 0; i < p->unrel.count; i++) {
      unrel_msg *m = &p->unrel.data[i];
      if (w.len + 4 + m->len > w.cap) {
        p->unrel.data[kept++] = *m;
        more = true;
        continue;
      }
      cw__w_u8(&w, m->type);
      cw__w_u8(&w, CW_NET_UNRELIABLE);
      cw__w_u16(&w, (uint16_t)m->len);
      cw__w_bytes(&w, m->data, m->len);
      cw_free(m->data);
    }
    p->unrel.count = kept;
    s->tr->send(s->tr, p->link, w.p, w.len);
    p->st.packets_sent++;
    p->st.bytes_sent += (uint32_t)w.len;
    p->last_sent = s->now;
    p->ack_owed = false;
    if (!more) break;
  }
  return true;
}

void cw_net_flush(cw_net_session *s) {
  for (int i = 0; i < CW_NET_MAX_PEERS; i++)
    if (s->peers[i].connected) flush_peer(s, i, false);
}

static void send_bye(cw_net_session *s) {
  for (int i = 0; i < CW_NET_MAX_PEERS; i++) {
    peer *p = &s->peers[i];
    if (!p->connected) continue;
    cw__wbuf w = { s->pkt, 0, sizeof s->pkt, false };
    write_header(&w, s, p, p->send_seq++, 1);
    cw__w_u8(&w, CW__NM_BYE);
    cw__w_u8(&w, CW_NET_UNRELIABLE);
    cw__w_u16(&w, 0);
    s->tr->send(s->tr, p->link, w.p, w.len);
  }
}

/* ── packets in ──────────────────────────────────────────────────────── */

static void peer_disconnect(cw_net_session *s, int slot, const char *why) {
  peer *p = &s->peers[slot];
  if (!p->connected) return;
  cw_info("net: player %d left (%s)", slot, why);
  if (p->link >= 0 && p->link < MAX_LINKS) s->links[p->link].slot = -1;
  peer_reset(p);
  peer_event(s, slot, false);
}

static void deliver(cw_net_session *s, int from, uint8_t type, const uint8_t *data, int len) {
  if (type == CW__NM_BYE) {
    peer_disconnect(s, from, "goodbye");
    return;
  }
  if (s->handlers[type].fn) s->handlers[type].fn(s, from, data, len, s->handlers[type].user);
}

static void bind_peer(cw_net_session *s, int link, int slot) {
  peer *p = &s->peers[slot];
  peer_reset(p);
  p->connected = true;
  p->link = link;
  p->last_recv = s->now;
  p->last_sent = s->now - KEEPALIVE; /* answer at once */
  s->links[link].slot = slot;
  cw_info("net: player %d joined over %s (link %d)", slot, s->tr->name, link);
  peer_event(s, slot, true);
  cw__net_replicate_peer_joined(s, slot);
}

static void acks_in(cw_net_session *s, peer *p, uint16_t ack, uint32_t bits) {
  for (int k = 0; k <= 32; k++) {
    if (k > 0 && !(bits & (1u << (k - 1)))) continue;
    uint16_t seq = (uint16_t)(ack - k);
    pkt_rec *rec = &p->ring[seq % PKT_RING];
    if (!rec->used || rec->acked || rec->seq != seq) continue;
    rec->acked = true;
    double sample = s->now - rec->time;
    if (!p->rtt_init) {
      p->rtt = sample;
      p->rtt_init = true;
    } else p->rtt += (sample - p->rtt) * 0.1;
    for (int r = 0; r < rec->nrel; r++)
      for (int i = 0; i < p->pending.count; i++)
        if (p->pending.data[i].rseq == rec->rseqs[r]) {
          cw_free(p->pending.data[i].data);
          for (int j = i; j + 1 < p->pending.count; j++) p->pending.data[j] = p->pending.data[j + 1];
          p->pending.count--;
          break;
        }
  }
}

/* true when the packet is new (its messages are to be delivered) */
static bool note_received(peer *p, uint16_t seq) {
  if (!p->any_recv) {
    p->any_recv = true;
    p->recv_max = seq;
    p->recv_bits = 0;
    return true;
  }
  if (seq == p->recv_max) return false;
  if (seq_newer(seq, p->recv_max)) {
    int d = (uint16_t)(seq - p->recv_max);
    p->recv_bits = d >= 32 ? 0 : (p->recv_bits << d);
    if (d <= 32) p->recv_bits |= 1u << (d - 1);
    p->recv_max = seq;
    return true;
  }
  int d = (uint16_t)(p->recv_max - seq);
  if (d > 32) return false; /* too old to tell: treat as seen */
  uint32_t bit = 1u << (d - 1);
  if (p->recv_bits & bit) return false;
  p->recv_bits |= bit;
  return true;
}

static void packet_in(cw_net_session *s, int link, const uint8_t *data, int len) {
  if (link < 0 || link >= MAX_LINKS) return;
  cw__rbuf r = { data, len, 0, false };
  if (len < HDR || cw__r_u8(&r) != MAGIC) return;
  int from = cw__r_u8(&r);
  uint8_t flags = cw__r_u8(&r);
  cw__r_u8(&r);
  uint16_t seq = cw__r_u16(&r), ack = cw__r_u16(&r);
  uint32_t bits = cw__r_u32(&r);
  if (from >= CW_NET_MAX_PEERS || s->local < 0) return;
  link_rec *lr = &s->links[link];
  lr->open = true;
  peer *p = lr->slot == from ? &s->peers[from] : NULL;
  if (!p) {
    /* unbound link: only a HELLO binds it */
    int pos = r.pos;
    while (r.pos + 4 <= len) {
      uint8_t type = cw__r_u8(&r), ch = cw__r_u8(&r);
      uint16_t ml = cw__r_u16(&r);
      if (ch == CW_NET_RELIABLE) cw__r_u16(&r);
      if (r.bad || r.pos + ml > len) break;
      if (type == CW__NM_HELLO && ml >= 5) {
        cw__rbuf h = { data + r.pos, ml, 0, false };
        uint32_t proto = cw__r_u32(&h);
        int slot = cw__r_u8(&h);
        if (proto != s->protocol || slot != from || slot == s->local || (s->peers[slot].connected && s->peers[slot].link != link)) {
          if (!lr->refused_logged)
            cw_warn("net: refused link %d: protocol %08x (ours %08x), slot %d (ours %d)%s", link, proto, s->protocol, slot,
                    s->local, s->peers[slot].connected ? ", slot taken" : "");
          lr->refused_logged = true;
          return;
        }
        bind_peer(s, link, slot);
        p = &s->peers[slot];
        break;
      }
      r.pos += ml;
    }
    if (!p) return;
    r.pos = pos;
  }
  p->last_recv = s->now;
  if (flags & 2) return; /* a link HELLO: not part of the sequence */
  p->st.packets_received++;
  p->st.bytes_received += (uint32_t)len;
  if (flags & 1) p->they_know_us = true;
  acks_in(s, p, ack, bits);
  if (!note_received(p, seq)) return; /* a duplicate: acks only */
  p->ack_owed = true;
  while (r.pos + 4 <= len && p->connected) {
    uint8_t type = cw__r_u8(&r), ch = cw__r_u8(&r);
    uint16_t ml = cw__r_u16(&r);
    uint16_t rseq = ch == CW_NET_RELIABLE ? cw__r_u16(&r) : 0;
    if (r.bad || r.pos + ml > len) break;
    const uint8_t *payload = data + r.pos;
    r.pos += ml;
    if (type == CW__NM_HELLO) continue;
    if (ch != CW_NET_RELIABLE) {
      deliver(s, from, type, payload, ml);
      continue;
    }
    int ahead = (uint16_t)(rseq - p->rel_expect);
    if (ahead >= REL_WINDOW) continue; /* already delivered (or absurdly far ahead) */
    if (ahead > 0) {
      rel_slot *b = &p->inbox[rseq % REL_WINDOW];
      if (!b->has) {
        b->has = true;
        b->type = type;
        b->len = ml;
        b->data = cw_alloc((size_t)CW_MAX(ml, 1), CW_MEM_CORE);
        memcpy(b->data, payload, ml);
      }
      continue;
    }
    p->rel_expect++;
    deliver(s, from, type, payload, ml);
    while (p->connected) {
      rel_slot *b = &p->inbox[p->rel_expect % REL_WINDOW];
      if (!b->has) break;
      uint8_t *d = b->data;
      uint8_t t = b->type;
      int l = b->len;
      *b = (rel_slot){ 0 };
      p->rel_expect++;
      deliver(s, from, t, d, l);
      cw_free(d);
    }
  }
}

void cw_net_poll(cw_net_session *s, double now) {
  if (now > s->now) s->now = now;
  if (s->local < 0 && s->tr->assigned_slot) {
    int a = s->tr->assigned_slot(s->tr);
    if (a >= 0 && a < CW_NET_MAX_PEERS) {
      s->local = a;
      cw_info("net: the %s assigned us player %d", s->tr->name, a);
    }
  }
  cw_net_event e;
  while (s->tr->poll(s->tr, s->now, &e)) {
    if (e.link < 0 || e.link >= MAX_LINKS) continue;
    link_rec *lr = &s->links[e.link];
    if (e.type == CW_NET_EV_CONNECT) {
      lr->open = true;
      lr->last_hello = -1;
    } else if (e.type == CW_NET_EV_DISCONNECT) {
      lr->open = false;
      if (lr->slot >= 0) peer_disconnect(s, lr->slot, "transport closed");
      lr->slot = -1;
    } else packet_in(s, e.link, e.data, e.len);
  }
  for (int i = 0; i < CW_NET_MAX_PEERS; i++)
    if (s->peers[i].connected && s->now - s->peers[i].last_recv > s->timeout) peer_disconnect(s, i, "timed out");
  if (s->local >= 0)
    for (int l = 0; l < MAX_LINKS; l++)
      if (s->links[l].open && s->links[l].slot < 0 && s->now - s->links[l].last_hello >= KEEPALIVE) send_link_hello(s, l);
  cw__net_replicate_poll(s);
  cw_net_flush(s);
}

/* ── the scene side's view ───────────────────────────────────────────── */

static void free_nothing(void *p) { (void)p; }
void cw_net_attach(cw_net_session *s, cw_tree *t, cw_node root) {
  cw_net_register_types();
  s->tree = t;
  s->root = cw_node_valid(root) ? root : cw_tree_root(t);
  cw_tree_set_slot(t, "cw.net", s, free_nothing);
}
cw_net_session *cw_net_session_of(cw_tree *t) { return t ? cw_tree_slot(t, "cw.net") : NULL; }
cw_node cw__net_root(cw_net_session *s) { return s->tree && cw_node_valid(s->root) ? s->root : (cw_node){ 0 }; }
cw_tree *cw__net_tree(cw_net_session *s) { return s->tree; }
void *cw__net_replicate_state(cw_net_session *s) { return s->repl; }
void cw__net_set_replicate_state(cw_net_session *s, void *st) { s->repl = st; }
int cw__net_authority_get(cw_net_session *s, uint64_t uid, bool *found) {
  uint64_t v;
  *found = cw_map_get(&s->authority, uid, &v);
  return *found ? (int)v - 1 : 0;
}
void cw__net_authority_set(cw_net_session *s, uint64_t uid, int player) { cw_map_set(&s->authority, uid, (uint64_t)(player + 1)); }

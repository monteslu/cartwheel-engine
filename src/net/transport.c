/* Transports that need no OS: the in-process loopback hub and the
 * deterministic network simulator. */
#include <stdio.h>
#include <stdlib.h>

#include "net_internal.h"
#include "cartwheel/core.h"

void cw_net_transport_destroy(cw_net_transport *t) {
  if (t && t->destroy) t->destroy(t);
}

/* ── an event queue with owned payloads (shared by the transports) ───── */
typedef struct evq_item {
  cw_net_event_type type;
  int link, len;
  uint8_t *data;
} evq_item;
typedef struct evq {
  CW_ARRAY(evq_item) items;
  int head;
  uint8_t *last; /* payload handed out by the previous pop (freed on the next) */
} evq;
static void evq_push(evq *q, cw_net_event_type type, int link, const void *data, int len) {
  evq_item it = { type, link, len, NULL };
  if (len > 0) {
    it.data = cw_alloc((size_t)len, CW_MEM_CORE);
    memcpy(it.data, data, (size_t)len);
  }
  cw_array_push(&q->items, it);
}
static bool evq_pop(evq *q, cw_net_event *out) {
  cw_free(q->last);
  q->last = NULL;
  if (q->head >= q->items.count) {
    q->items.count = q->head = 0;
    return false;
  }
  evq_item it = q->items.data[q->head++];
  q->last = it.data;
  *out = (cw_net_event){ it.type, it.link, it.data, it.len };
  return true;
}
static void evq_free(evq *q) {
  for (int i = q->head; i < q->items.count; i++) cw_free(q->items.data[i].data);
  cw_free(q->last);
  cw_array_free(&q->items);
}

/* ── loopback ─────────────────────────────────────────────────────────── */
typedef struct loop_ep loop_ep;
struct cw_net_loopback {
  CW_ARRAY(loop_ep *) eps;
};
typedef struct loop_link {
  loop_ep *to;
  int remote_link; /* the link id on the other side */
  bool open;
} loop_link;
struct loop_ep {
  cw_net_transport t;
  cw_net_loopback *hub;
  const char *name;
  CW_ARRAY(loop_link) links;
  evq q;
};

cw_net_loopback *cw_net_loopback_new(void) { return cw_calloc(1, sizeof(cw_net_loopback), CW_MEM_CORE); }
void cw_net_loopback_free(cw_net_loopback *hub) {
  if (!hub) return;
  CW_ASSERT(hub->eps.count == 0, "destroy every loopback endpoint before its hub");
  cw_array_free(&hub->eps);
  cw_free(hub);
}

static int loop_open(cw_net_transport *t, const char *addr) {
  loop_ep *e = (loop_ep *)t;
  const char *name = !strncmp(addr, "loop:", 5) ? addr + 5 : addr;
  loop_ep *to = NULL;
  for (int i = 0; i < e->hub->eps.count; i++)
    if (!strcmp(e->hub->eps.data[i]->name, name)) to = e->hub->eps.data[i];
  if (!to || to == e) return -1;
  int a = e->links.count, b = to->links.count;
  cw_array_push(&e->links, ((loop_link){ to, b, true }));
  cw_array_push(&to->links, ((loop_link){ e, a, true }));
  evq_push(&e->q, CW_NET_EV_CONNECT, a, NULL, 0);
  evq_push(&to->q, CW_NET_EV_CONNECT, b, NULL, 0);
  return a;
}
static bool loop_send(cw_net_transport *t, int link, const void *data, int len) {
  loop_ep *e = (loop_ep *)t;
  if (link < 0 || link >= e->links.count || !e->links.data[link].open) return false;
  loop_link *l = &e->links.data[link];
  evq_push(&l->to->q, CW_NET_EV_MESSAGE, l->remote_link, data, len);
  return true;
}
static bool loop_poll(cw_net_transport *t, double now, cw_net_event *out) {
  (void)now;
  return evq_pop(&((loop_ep *)t)->q, out);
}
static void loop_close(cw_net_transport *t, int link) {
  loop_ep *e = (loop_ep *)t;
  if (link < 0 || link >= e->links.count || !e->links.data[link].open) return;
  loop_link *l = &e->links.data[link];
  l->open = false;
  if (l->to) {
    l->to->links.data[l->remote_link].open = false;
    evq_push(&l->to->q, CW_NET_EV_DISCONNECT, l->remote_link, NULL, 0);
  }
}
static void loop_destroy(cw_net_transport *t) {
  loop_ep *e = (loop_ep *)t;
  for (int i = 0; i < e->links.count; i++) loop_close(t, i);
  /* forget us on every other endpoint's links */
  for (int i = 0; i < e->hub->eps.count; i++) {
    loop_ep *o = e->hub->eps.data[i];
    for (int k = 0; k < o->links.count; k++)
      if (o->links.data[k].to == e) o->links.data[k].to = NULL;
  }
  for (int i = 0; i < e->hub->eps.count; i++)
    if (e->hub->eps.data[i] == e) {
      for (int k = i; k + 1 < e->hub->eps.count; k++) e->hub->eps.data[k] = e->hub->eps.data[k + 1];
      e->hub->eps.count--;
      break;
    }
  cw_array_free(&e->links);
  evq_free(&e->q);
  cw_free(e);
}

cw_net_transport *cw_net_loopback_endpoint(cw_net_loopback *hub, const char *name) {
  loop_ep *e = cw_calloc(1, sizeof *e, CW_MEM_CORE);
  e->t = (cw_net_transport){ .name = "loopback", .props = CW_NET_TRANSPORT_RELIABLE | CW_NET_TRANSPORT_ORDERED,
                             .open = loop_open, .send = loop_send, .poll = loop_poll, .close = loop_close,
                             .destroy = loop_destroy };
  e->hub = hub;
  e->name = cw_intern(name);
  cw_array_push(&hub->eps, e);
  return &e->t;
}

/* ── the network simulator ─────────────────────────────────────────── */
typedef struct sim_pkt {
  double at;
  uint64_t order; /* ties leave in send order */
  int link, len;
  uint8_t *data;
} sim_pkt;
typedef struct sim_t {
  cw_net_transport t;
  cw_net_transport *base;
  cw_net_sim_desc d;
  cw_rng rng;
  double now;
  uint64_t order;
  CW_ARRAY(sim_pkt) q;
  cw_net_sim_stats stats;
} sim_t;

static void sim_enqueue(sim_t *s, int link, const void *data, int len) {
  double delay = s->d.latency + (s->d.jitter > 0 ? s->d.jitter * (double)cw_rng_float(&s->rng) : 0);
  sim_pkt p = { s->now + delay, s->order++, link, len, cw_alloc((size_t)CW_MAX(len, 1), CW_MEM_CORE) };
  memcpy(p.data, data, (size_t)len);
  cw_array_push(&s->q, p);
}
static int sim_open(cw_net_transport *t, const char *addr) { return ((sim_t *)t)->base->open(((sim_t *)t)->base, addr); }
static bool sim_send(cw_net_transport *t, int link, const void *data, int len) {
  sim_t *s = (sim_t *)t;
  s->stats.sent++;
  /* the draws happen for every datagram, so loss does not shift the delays of the rest */
  float lr = cw_rng_float(&s->rng), dr = cw_rng_float(&s->rng);
  if (lr < s->d.loss) {
    s->stats.dropped++;
    return true; /* a lost datagram looks sent */
  }
  sim_enqueue(s, link, data, len);
  if (dr < s->d.dup) {
    s->stats.duplicated++;
    sim_enqueue(s, link, data, len);
  }
  return true;
}
static void sim_release(sim_t *s) {
  /* hand every due datagram to the base, earliest first */
  for (;;) {
    int best = -1;
    for (int i = 0; i < s->q.count; i++) {
      sim_pkt *p = &s->q.data[i];
      if (p->at > s->now) continue;
      if (best < 0 || p->at < s->q.data[best].at || (p->at == s->q.data[best].at && p->order < s->q.data[best].order)) best = i;
    }
    if (best < 0) return;
    sim_pkt p = s->q.data[best];
    s->q.data[best] = s->q.data[--s->q.count];
    s->base->send(s->base, p.link, p.data, p.len);
    s->stats.delivered++;
    cw_free(p.data);
  }
}
static bool sim_poll(cw_net_transport *t, double now, cw_net_event *out) {
  sim_t *s = (sim_t *)t;
  if (now > s->now) s->now = now;
  sim_release(s);
  return s->base->poll(s->base, now, out);
}
static void sim_close(cw_net_transport *t, int link) { ((sim_t *)t)->base->close(((sim_t *)t)->base, link); }
static int sim_slot(cw_net_transport *t) {
  sim_t *s = (sim_t *)t;
  return s->base->assigned_slot ? s->base->assigned_slot(s->base) : -1;
}
static void sim_destroy(cw_net_transport *t) {
  sim_t *s = (sim_t *)t;
  for (int i = 0; i < s->q.count; i++) cw_free(s->q.data[i].data);
  cw_array_free(&s->q);
  cw_net_transport_destroy(s->base);
  cw_free(s);
}

cw_net_transport *cw_net_sim_new(cw_net_transport *base, const cw_net_sim_desc *desc) {
  if (!base) return NULL;
  sim_t *s = cw_calloc(1, sizeof *s, CW_MEM_CORE);
  s->t = (cw_net_transport){ .name = "netsim", .props = 0, .open = sim_open, .send = sim_send, .poll = sim_poll,
                             .close = sim_close, .destroy = sim_destroy, .assigned_slot = sim_slot };
  s->base = base;
  s->d = *desc;
  cw_rng_seed(&s->rng, desc->seed);
  return &s->t;
}
cw_net_sim_stats cw_net_sim_get_stats(cw_net_transport *sim) {
  return sim && sim->destroy == sim_destroy ? ((sim_t *)sim)->stats : (cw_net_sim_stats){ 0 };
}
void cw_net_sim_set(cw_net_transport *sim, const cw_net_sim_desc *desc) {
  if (sim && sim->destroy == sim_destroy) {
    uint64_t seed = ((sim_t *)sim)->d.seed;
    ((sim_t *)sim)->d = *desc;
    ((sim_t *)sim)->d.seed = seed;
  }
}

/* ── the room relay client ────────────────────────────────────────────
 * Server -> client: [0xFF 'W' slot count] welcome (our slot), [0xFF 'J' slot]
 * a member joined, [0xFF 'L' slot] a member left, [src_slot payload...] data.
 * Client -> server: [dst_slot payload...] (dst 0xFE = every member). */
typedef struct relay_t {
  cw_net_transport t;
  cw_net_transport *base;
  int base_link;
  int slot;
  bool member[256];
  evq q;
  uint8_t buf[CW_NET_MAX_PACKET + 1];
} relay_t;

static int relay_open(cw_net_transport *t, const char *addr) {
  relay_t *r = (relay_t *)t;
  if (r->base_link >= 0) return -1; /* one relay per transport */
  r->base_link = r->base->open(r->base, addr);
  return r->base_link >= 0 ? 0x7fff : -1; /* the members appear as their own links */
}
static bool relay_send(cw_net_transport *t, int link, const void *data, int len) {
  relay_t *r = (relay_t *)t;
  if (r->base_link < 0 || link < 0 || link > 0xfd || len > CW_NET_MAX_PACKET || !r->member[link]) return false;
  r->buf[0] = (uint8_t)link;
  memcpy(r->buf + 1, data, (size_t)len);
  return r->base->send(r->base, r->base_link, r->buf, len + 1);
}
static bool relay_poll(cw_net_transport *t, double now, cw_net_event *out) {
  relay_t *r = (relay_t *)t;
  cw_net_event e;
  while (r->base->poll(r->base, now, &e)) {
    if (e.link != r->base_link) continue;
    if (e.type == CW_NET_EV_DISCONNECT) {
      for (int i = 0; i < 256; i++)
        if (r->member[i]) {
          r->member[i] = false;
          evq_push(&r->q, CW_NET_EV_DISCONNECT, i, NULL, 0);
        }
      r->base_link = -1;
      continue;
    }
    if (e.type != CW_NET_EV_MESSAGE || e.len < 2) continue;
    uint8_t h = e.data[0];
    if (h == 0xff) {
      int slot = e.data[2 < e.len ? 2 : 1];
      if (e.data[1] == 'W' && e.len >= 3) r->slot = e.data[2];
      else if (e.data[1] == 'J' && e.len >= 3 && slot != r->slot && !r->member[slot]) {
        r->member[slot] = true;
        evq_push(&r->q, CW_NET_EV_CONNECT, slot, NULL, 0);
      } else if (e.data[1] == 'L' && e.len >= 3 && r->member[slot]) {
        r->member[slot] = false;
        evq_push(&r->q, CW_NET_EV_DISCONNECT, slot, NULL, 0);
      }
      continue;
    }
    if (!r->member[h]) { /* data from a member we were not told about yet: it is one */
      r->member[h] = true;
      evq_push(&r->q, CW_NET_EV_CONNECT, h, NULL, 0);
    }
    evq_push(&r->q, CW_NET_EV_MESSAGE, h, e.data + 1, e.len - 1);
  }
  return evq_pop(&r->q, out);
}
static void relay_close(cw_net_transport *t, int link) {
  relay_t *r = (relay_t *)t;
  if (link >= 0 && link < 256) r->member[link] = false;
}
static int relay_slot(cw_net_transport *t) { return ((relay_t *)t)->slot; }
static void relay_destroy(cw_net_transport *t) {
  relay_t *r = (relay_t *)t;
  if (r->base_link >= 0) r->base->close(r->base, r->base_link);
  cw_net_transport_destroy(r->base);
  evq_free(&r->q);
  cw_free(r);
}
cw_net_transport *cw_net_relay_new(cw_net_transport *base) {
  if (!base) return NULL;
  relay_t *r = cw_calloc(1, sizeof *r, CW_MEM_CORE);
  r->t = (cw_net_transport){ .name = "relay", .props = base->props, .open = relay_open, .send = relay_send,
                             .poll = relay_poll, .close = relay_close, .destroy = relay_destroy, .assigned_slot = relay_slot };
  r->base = base;
  r->base_link = -1;
  r->slot = -1;
  return &r->t;
}

void cw__net_evq_push(void *q, cw_net_event_type type, int link, const void *data, int len) { evq_push(q, type, link, data, len); }
bool cw__net_evq_pop(void *q, cw_net_event *out) { return evq_pop(q, out); }
void *cw__net_evq_new(void) { return cw_calloc(1, sizeof(evq), CW_MEM_CORE); }
void cw__net_evq_free(void *q) {
  if (!q) return;
  evq_free(q);
  cw_free(q);
}

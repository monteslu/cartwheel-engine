#ifndef CW_NET_INTERNAL_H
#define CW_NET_INTERNAL_H

#include <string.h>

#include "cartwheel/net.h"

/* engine message types (0..31) */
enum {
  CW__NM_HELLO = 1,
  CW__NM_BYE = 2,
  CW__NM_RPC = 3,
  CW__NM_SYNC = 4,
  CW__NM_SPAWN = 5,
  CW__NM_DESPAWN = 6,
  CW__NM_RB_INPUT = 7,
  CW__NM_RB_HASH = 8,
};

/* ── little-endian byte writer / reader ─────────────────────────────── */
typedef struct cw__wbuf {
  uint8_t *p;
  int len, cap;
  bool overflow;
} cw__wbuf;
static inline void cw__w_bytes(cw__wbuf *w, const void *d, int n) {
  if (w->len + n > w->cap) { w->overflow = true; return; }
  memcpy(w->p + w->len, d, (size_t)n);
  w->len += n;
}
static inline void cw__w_u8(cw__wbuf *w, uint8_t v) { cw__w_bytes(w, &v, 1); }
static inline void cw__w_u16(cw__wbuf *w, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; cw__w_bytes(w, b, 2); }
static inline void cw__w_u32(cw__wbuf *w, uint32_t v) {
  uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
  cw__w_bytes(w, b, 4);
}
static inline void cw__w_u64(cw__wbuf *w, uint64_t v) { cw__w_u32(w, (uint32_t)v); cw__w_u32(w, (uint32_t)(v >> 32)); }
static inline void cw__w_f32(cw__wbuf *w, float f) { uint32_t u; memcpy(&u, &f, 4); cw__w_u32(w, u); }
static inline void cw__w_f64(cw__wbuf *w, double f) { uint64_t u; memcpy(&u, &f, 8); cw__w_u64(w, u); }
static inline void cw__w_str(cw__wbuf *w, const char *s) {
  int n = s ? (int)strlen(s) : 0;
  if (n > 0xffff) { w->overflow = true; return; }
  cw__w_u16(w, (uint16_t)n);
  cw__w_bytes(w, s, n);
}

typedef struct cw__rbuf {
  const uint8_t *p;
  int len, pos;
  bool bad;
} cw__rbuf;
static inline bool cw__r_bytes(cw__rbuf *r, void *d, int n) {
  if (r->bad || r->pos + n > r->len) { r->bad = true; if (d) memset(d, 0, (size_t)n); return false; }
  if (d) memcpy(d, r->p + r->pos, (size_t)n);
  r->pos += n;
  return true;
}
static inline uint8_t cw__r_u8(cw__rbuf *r) { uint8_t v; cw__r_bytes(r, &v, 1); return v; }
static inline uint16_t cw__r_u16(cw__rbuf *r) { uint8_t b[2]; cw__r_bytes(r, b, 2); return (uint16_t)(b[0] | b[1] << 8); }
static inline uint32_t cw__r_u32(cw__rbuf *r) {
  uint8_t b[4];
  cw__r_bytes(r, b, 4);
  return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static inline uint64_t cw__r_u64(cw__rbuf *r) { uint64_t lo = cw__r_u32(r); return lo | (uint64_t)cw__r_u32(r) << 32; }
static inline float cw__r_f32(cw__rbuf *r) { uint32_t u = cw__r_u32(r); float f; memcpy(&f, &u, 4); return f; }
static inline double cw__r_f64(cw__rbuf *r) { uint64_t u = cw__r_u64(r); double f; memcpy(&f, &u, 8); return f; }
/* a string as an interned pointer ("" for empty); NULL on a bad buffer */
const char *cw__r_str(cw__rbuf *r);

/* cw_value on the wire; node values travel as paths relative to `root` */
void cw__w_value(cw__wbuf *w, const cw_value *v, cw_node root);
bool cw__r_value(cw__rbuf *r, cw_value *v, cw_node root);

/* session internals the scene layer uses (replicate.c) */
cw_node cw__net_root(cw_net_session *s);
cw_tree *cw__net_tree(cw_net_session *s);
void cw__net_replicate_poll(cw_net_session *s);      /* synchronizers + spawners */
void cw__net_replicate_peer_joined(cw_net_session *s, int player);
void cw__net_replicate_init(cw_net_session *s);      /* message handlers */
void cw__net_replicate_free(cw_net_session *s);
void *cw__net_replicate_state(cw_net_session *s);
void cw__net_set_replicate_state(cw_net_session *s, void *st);
/* authority table, keyed by node uid */
int cw__net_authority_get(cw_net_session *s, uint64_t uid, bool *found);
void cw__net_authority_set(cw_net_session *s, uint64_t uid, int player);

/* the transports' event queue (transport.c): payloads are copied in and
 * stay valid until the next pop */
void *cw__net_evq_new(void);
void cw__net_evq_free(void *q);
void cw__net_evq_push(void *q, cw_net_event_type type, int link, const void *data, int len);
bool cw__net_evq_pop(void *q, cw_net_event *out);

/* monotonic wall clock in milliseconds (measurement only, never simulation) */
double cw__net_wall_ms(void);

#endif

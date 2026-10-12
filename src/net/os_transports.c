/* Transports that reach outside the process: UDP sockets natively, the
 * wasmcart peer connection (wc_peer_*) in carts. Each build compiles one of
 * them and a NULL-returning stub for the other. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "net_internal.h"
#include "cartwheel/core.h"

double cw__net_wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#if !CW_CART
/* ── UDP (POSIX sockets; Winsock on Windows) ────────────────────────── */
#include <errno.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET udp_fd;
#define udp_closefd closesocket
#define UDP_BAD INVALID_SOCKET
static void udp_nonblock(udp_fd fd) { u_long on = 1; ioctlsocket(fd, FIONBIO, &on); }
static bool udp_startup(void) {
  static bool done;
  WSADATA w;
  return done || (done = WSAStartup(MAKEWORD(2, 2), &w) == 0);
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int udp_fd;
#define udp_closefd close
#define UDP_BAD (-1)
static void udp_nonblock(udp_fd fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
static bool udp_startup(void) { return true; }
#endif

typedef struct udp_t {
  cw_net_transport t;
  udp_fd fd;
  int port;
  CW_ARRAY(struct sockaddr_in) links;
  void *q;
  uint8_t buf[65536];
} udp_t;

static int udp_link_of(udp_t *u, const struct sockaddr_in *a, bool add) {
  for (int i = 0; i < u->links.count; i++)
    if (u->links.data[i].sin_addr.s_addr == a->sin_addr.s_addr && u->links.data[i].sin_port == a->sin_port) return i;
  if (!add) return -1;
  cw_array_push(&u->links, *a);
  cw__net_evq_push(u->q, CW_NET_EV_CONNECT, u->links.count - 1, NULL, 0);
  return u->links.count - 1;
}
static int udp_open(cw_net_transport *t, const char *addr) {
  udp_t *u = (udp_t *)t;
  if (!strncmp(addr, "udp:", 4)) addr += 4;
  char host[256];
  const char *colon = strrchr(addr, ':');
  if (!colon || colon == addr || (size_t)(colon - addr) >= sizeof host) {
    cw_warn("udp: address '%s' is not host:port", addr);
    return -1;
  }
  memcpy(host, addr, (size_t)(colon - addr));
  host[colon - addr] = 0;
  struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
  if (getaddrinfo(host, colon + 1, &hints, &res) || !res) {
    cw_warn("udp: cannot resolve '%s'", addr);
    return -1;
  }
  struct sockaddr_in a = *(struct sockaddr_in *)res->ai_addr;
  freeaddrinfo(res);
  int l = udp_link_of(u, &a, false);
  return l >= 0 ? l : udp_link_of(u, &a, true);
}
static bool udp_send(cw_net_transport *t, int link, const void *data, int len) {
  udp_t *u = (udp_t *)t;
  if (link < 0 || link >= u->links.count) return false;
  long n = (long)sendto(u->fd, data, len, 0, (struct sockaddr *)&u->links.data[link], sizeof(struct sockaddr_in));
  return n == len;
}
static bool udp_poll(cw_net_transport *t, double now, cw_net_event *out) {
  (void)now;
  udp_t *u = (udp_t *)t;
  for (;;) {
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    long n = (long)recvfrom(u->fd, (char *)u->buf, (int)sizeof u->buf, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) break; /* EAGAIN: drained (other errors, e.g. ICMP refused, are a lost datagram) */
    int l = udp_link_of(u, &from, true);
    cw__net_evq_push(u->q, CW_NET_EV_MESSAGE, l, u->buf, (int)n);
  }
  return cw__net_evq_pop(u->q, out);
}
static void udp_close(cw_net_transport *t, int link) { (void)t; (void)link; /* connectionless */ }
static void udp_destroy(cw_net_transport *t) {
  udp_t *u = (udp_t *)t;
  udp_closefd(u->fd);
  cw_array_free(&u->links);
  cw__net_evq_free(u->q);
  cw_free(u);
}

cw_net_transport *cw_net_udp_new(int port) {
  if (!udp_startup()) {
    cw_warn("udp: the socket library did not start");
    return NULL;
  }
  udp_fd fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == UDP_BAD) {
    cw_warn("udp: socket: %s", strerror(errno));
    return NULL;
  }
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
    cw_warn("udp: bind port %d: %s", port, strerror(errno));
    udp_closefd(fd);
    return NULL;
  }
  udp_nonblock(fd);
  socklen_t al = sizeof a;
  getsockname(fd, (struct sockaddr *)&a, &al);
  udp_t *u = cw_calloc(1, sizeof *u, CW_MEM_CORE);
  u->t = (cw_net_transport){ .name = "udp", .props = 0, .open = udp_open, .send = udp_send, .poll = udp_poll,
                             .close = udp_close, .destroy = udp_destroy };
  u->fd = fd;
  u->port = ntohs(a.sin_port);
  u->q = cw__net_evq_new();
  return &u->t;
}
int cw_net_udp_port(cw_net_transport *t) { return t && t->destroy == udp_destroy ? ((udp_t *)t)->port : 0; }

cw_net_transport *cw_net_wcpeer_new(void) {
  cw_warn("net: the wasmcart peer transport exists only in carts (natively use cw_net_udp_new)");
  return NULL;
}

#else
/* ── wasmcart peer connections ───────────────────────────────────────── */
#define WC_USE_NET_PEER
#include "wasmcart.h"

/* The host delivers events through these exports before each wc_render; they
 * land in one queue the transport drains. The pointers are temporary, so the
 * queue copies. Events that arrive while no transport exists are kept for
 * the first one (a host may register a peer before the game made one). */
static void *peer_q;
static void *peer_queue(void) { return peer_q ? peer_q : (peer_q = cw__net_evq_new()); }

__attribute__((export_name("wc_peer_on_connect"))) void wc_peer_on_connect(int32_t id, const char *name, uint32_t len) {
  (void)name;
  (void)len;
  cw__net_evq_push(peer_queue(), CW_NET_EV_CONNECT, id, NULL, 0);
}
__attribute__((export_name("wc_peer_on_message"))) void wc_peer_on_message(int32_t id, const void *data, uint32_t len) {
  cw__net_evq_push(peer_queue(), CW_NET_EV_MESSAGE, id, data, (int)len);
}
__attribute__((export_name("wc_peer_on_disconnect"))) void wc_peer_on_disconnect(int32_t id) {
  cw__net_evq_push(peer_queue(), CW_NET_EV_DISCONNECT, id, NULL, 0);
}
__attribute__((export_name("wc_peer_on_error"))) void wc_peer_on_error(int32_t id) {
  cw_warn("net: peer %d reported an error", (int)id);
}

static int wcp_open(cw_net_transport *t, const char *addr) {
  (void)t;
  int id = wc_peer_open(addr, (unsigned)strlen(addr));
  if (id < 0)
    cw_warn("net: wc_peer_open(\"%s\") refused: the cart needs cw_app_desc.net and a manifest net.domains grant "
            "covering that host (cw_add_app NET <domain>)", addr);
  return id;
}
static bool wcp_send(cw_net_transport *t, int link, const void *data, int len) {
  (void)t;
  return wc_peer_send(link, data, (unsigned)len) == len;
}
static bool wcp_poll(cw_net_transport *t, double now, cw_net_event *out) {
  (void)t;
  (void)now;
  return cw__net_evq_pop(peer_queue(), out);
}
static void wcp_close(cw_net_transport *t, int link) {
  (void)t;
  wc_peer_close(link);
}
static bool wcp_alive;
static void wcp_destroy(cw_net_transport *t) {
  wcp_alive = false;
  cw_free(t);
}
cw_net_transport *cw_net_wcpeer_new(void) {
  CW_ASSERT(!wcp_alive, "one wasmcart peer transport per cart: the peer exports are global");
  wcp_alive = true;
  cw_net_transport *t = cw_calloc(1, sizeof *t, CW_MEM_CORE);
  /* the reference hosts' WebSocket peers are reliable and ordered; the session
   * still runs its own acks, so an unreliable host transport works too */
  *t = (cw_net_transport){ .name = "wc_peer", .props = 0, .open = wcp_open, .send = wcp_send, .poll = wcp_poll,
                           .close = wcp_close, .destroy = wcp_destroy };
  return t;
}

cw_net_transport *cw_net_udp_new(int port) {
  (void)port;
  cw_warn("net: carts have no sockets; use cw_net_wcpeer_new (wasmcart peer connections)");
  return NULL;
}
int cw_net_udp_port(cw_net_transport *t) { (void)t; return 0; }
#endif

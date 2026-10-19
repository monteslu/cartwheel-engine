/* cartwheel networking: transports, sessions, channels, RPC, replication,
 * tree snapshots and rollback netcode.
 *
 * Layers, bottom up:
 *
 *   transport   moves datagrams over LINKS (one link per remote endpoint):
 *               UDP sockets (native), the wasmcart peer connection
 *               (wc_peer_*, carts), an in-process loopback hub (tests, local
 *               split-screen), a room relay on top of any of them, and a
 *               deterministic network simulator wrapping any of them
 *               (latency, jitter, loss, duplication).
 *   session     players by SLOT (0..CW_NET_MAX_PEERS-1) instead of links: a
 *               handshake, reliable-ordered and unreliable channels, acks,
 *               resends, RTT, timeouts. Games send typed messages.
 *   scene       RPC on nodes (methods called by name on every peer),
 *               authority per node, NetSynchronizer (replicates reflected
 *               properties) and NetSpawner (replicates node creation and
 *               removal): the Godot MultiplayerSynchronizer / Spawner model.
 *   state       a snapshot of a whole tree through reflection (structure,
 *               every writable property, the tree's tick and random state),
 *               restorable in place, with a host-stable hash.
 *   rollback    GGPO-style netcode on the deterministic fixed step: input
 *               delay, prediction (repeat the last input), rollback +
 *               re-simulation from a tree snapshot when a prediction was
 *               wrong, time sync, and in-band desync detection by hashing
 *               confirmed states.
 *
 * The wire format is engine-defined and the same on every host, so a native
 * build and a cart talk to each other wherever a transport reaches both.
 * Everything here runs on the main thread. Times are seconds on whatever
 * clock the caller passes to cw_net_poll (a test passes a virtual clock, so
 * a simulated network is reproducible bit for bit).
 *
 * Physics: a tree snapshot carries each physics world. A 2D world is Box2D's own image of it (v3.2 snapshots):
 * dynamic bodies come back with their velocities, contact warm-start caches and sleeping islands, and Area2D overlaps
 * with their sensor state, so a restored pile re-simulates bit for bit like one that never rolled back (tests/unit/
 * test_net.c proves it, and fails the same run when the image is left out). The desync hash folds in Box2D's own
 * host-stable state hash of the world. Box3D has no snapshot: a 3D world is restored through the tree, the physics
 * nodes put back what they keep beside the properties (a CharacterBody3D's mover state and its on_floor / normals
 * outputs travel in the snapshot as private state, every body is moved back to its node's pose), so 3D characters
 * (move_and_slide) against static and kinematic bodies, and the engine's own math, re-simulate bit-exactly, and
 * two things do not, because the library keeps their state where no restore reaches: DYNAMIC rigid bodies (contact
 * warm-start caches: a restored pile diverges from one that never rolled back) and Area3D overlap signals. A 3D
 * rollback game keeps its simulated objects out of dynamic rigid bodies and detects overlaps with queries or its own
 * math; cosmetic debris outside the rolled-back tree is fine. */
#ifndef CW_NET_H
#define CW_NET_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_NET_MAX_PEERS 8       /* player slots per session */
#define CW_NET_MAX_PACKET 1200   /* bytes per datagram (fits every MTU) */
#define CW_NET_MAX_MESSAGE 1100  /* bytes per message payload */

/* ── transports ──────────────────────────────────────────────────────────
 * A transport moves whole datagrams between LINKS (small non-negative
 * ints). open() dials an address (its grammar is the transport's) and
 * returns a link at once; a CONNECT event follows when the link is usable.
 * Endpoints that reach us first appear as a CONNECT on a new link.
 * poll() returns the next event (data valid until the next poll), false
 * when none is queued. Bring your own by filling the table. */
typedef enum { CW_NET_EV_CONNECT, CW_NET_EV_MESSAGE, CW_NET_EV_DISCONNECT } cw_net_event_type;
typedef struct cw_net_event {
  cw_net_event_type type;
  int link;
  const uint8_t *data;
  int len;
} cw_net_event;

enum { CW_NET_TRANSPORT_RELIABLE = 1, CW_NET_TRANSPORT_ORDERED = 2 };

typedef struct cw_net_transport cw_net_transport;
struct cw_net_transport {
  const char *name;
  uint32_t props; /* CW_NET_TRANSPORT_* the transport guarantees (0 = datagram semantics) */
  int (*open)(cw_net_transport *t, const char *addr);                  /* link, or -1 */
  bool (*send)(cw_net_transport *t, int link, const void *data, int len);
  bool (*poll)(cw_net_transport *t, double now, cw_net_event *out);
  void (*close)(cw_net_transport *t, int link);
  void (*destroy)(cw_net_transport *t);
  /* optional: the player slot an intermediary assigned us (relay), -1 = none */
  int (*assigned_slot)(cw_net_transport *t);
};
void cw_net_transport_destroy(cw_net_transport *t); /* calls t->destroy (closes links, frees t); NULL is a no-op */

/* In-process loopback: a hub joins endpoints by name, "loop:<name>". Delivery
 * is immediate and in order; wrap an endpoint in the simulator for latency
 * and loss. Endpoints are destroyed with cw_net_transport_destroy, the hub
 * after all of them. */
typedef struct cw_net_loopback cw_net_loopback;
cw_net_loopback *cw_net_loopback_new(void); /* an empty hub; free with cw_net_loopback_free */
void cw_net_loopback_free(cw_net_loopback *hub); /* asserts every endpoint is destroyed first; NULL is a no-op */
/* Joins hub as "loop:<name>" (reliable + ordered). The session that takes it destroys it. */
cw_net_transport *cw_net_loopback_endpoint(cw_net_loopback *hub, const char *name);

/* UDP (native): bound to `port` on every interface (0 = any free port);
 * open("udp:host:port" or "host:port"), IPv4. A datagram from an unknown
 * address opens a new link. NULL in carts (no sockets) and on bind failure. */
cw_net_transport *cw_net_udp_new(int port);
int cw_net_udp_port(cw_net_transport *t); /* the bound port */

/* The wasmcart peer connection (carts): open() is wc_peer_open (the host
 * interprets the address: "ws://..." on the reference hosts, gated by the
 * manifest's net.domains), and peers the host registers itself appear as
 * CONNECT events. One per cart (the peer exports are global). The app must
 * set cw_app_desc.net. NULL natively. */
cw_net_transport *cw_net_wcpeer_new(void);

/* Room relay client over a base transport (tools/cwrelay.mjs is the server):
 * open() dials the relay through `base`; the relay assigns this endpoint a
 * slot (assigned_slot) and announces the other members, each of which
 * appears as a link whose id is its slot. Datagrams are forwarded as-is. Owns
 * `base`. */
cw_net_transport *cw_net_relay_new(cw_net_transport *base);

/* Deterministic network simulator around `base` (owned): every outgoing
 * datagram is dropped with probability `loss`, duplicated with `dup`, and
 * otherwise handed to `base` after latency + uniform(0, jitter) seconds (so
 * jitter reorders). The random stream is seeded, and time is whatever the
 * owner passes to poll, so the same calls give the same network. */
typedef struct cw_net_sim_desc {
  double latency; /* seconds, one way */
  double jitter;  /* seconds, uniform 0..jitter added per datagram */
  float loss;     /* 0..1 */
  float dup;      /* 0..1 */
  uint64_t seed;
} cw_net_sim_desc;
/* Copies desc; NULL when base is NULL. */
cw_net_transport *cw_net_sim_new(cw_net_transport *base, const cw_net_sim_desc *desc);
typedef struct cw_net_sim_stats { uint32_t sent, dropped, duplicated, delivered; } cw_net_sim_stats;
cw_net_sim_stats cw_net_sim_get_stats(cw_net_transport *sim); /* zeros when sim is not a simulator */
void cw_net_sim_set(cw_net_transport *sim, const cw_net_sim_desc *desc); /* change conditions mid-run (keeps the rng) */

/* ── sessions ────────────────────────────────────────────────────────────
 * Players are slots. Every peer announces its slot and the game's protocol
 * id in a handshake; a peer with another protocol, or a slot already taken,
 * is refused (logged). Messages carry a type: 0..31 are the engine's,
 * 32..255 the game's. RELIABLE messages arrive exactly once, in order;
 * UNRELIABLE ones may be lost or reordered (sequence them yourself if order
 * matters). Messages are queued and leave in packets on cw_net_flush (or at
 * the end of cw_net_poll); acks ride on every packet and a keepalive goes
 * out every 0.1 s, so RTT and loss are measured continuously. */
typedef struct cw_net_session cw_net_session;
typedef enum { CW_NET_RELIABLE = 0, CW_NET_UNRELIABLE = 1 } cw_net_channel;
#define CW_NET_MSG_GAME 32
#define CW_NET_ALL (-1)

typedef struct cw_net_session_desc {
  cw_net_transport *transport; /* owned by the session from now on */
  int local_player;            /* this peer's slot; -1 = take the slot the transport assigns (relay) */
  uint32_t protocol;           /* the game's protocol id + version */
  double timeout;              /* seconds without a packet before a peer drops (5) */
} cw_net_session_desc;

/* Takes ownership of desc->transport. NULL (logged) without a transport. Free with cw_net_session_free. */
cw_net_session *cw_net_session_new(const cw_net_session_desc *desc);
void cw_net_session_free(cw_net_session *s); /* says goodbye, destroys the transport */
bool cw_net_connect(cw_net_session *s, const char *addr); /* open a link; the handshake runs in poll */
/* Pump the transport: handshakes, acks, resends, timeouts, received messages
 * (handlers run in here), replication (NetSynchronizer / NetSpawner of an
 * attached tree), then flush. `now` in seconds, monotonic. */
void cw_net_poll(cw_net_session *s, double now);
void cw_net_flush(cw_net_session *s); /* send queued messages to every connected peer now */
double cw_net_now(cw_net_session *s); /* the last poll's time */

int cw_net_local_player(cw_net_session *s); /* -1 until a relay assigned one */
bool cw_net_peer_connected(cw_net_session *s, int player); /* handshake done and not timed out; false out of range */
int cw_net_peer_count(cw_net_session *s); /* connected remote peers */
typedef struct cw_net_peer_stats {
  double rtt;                    /* smoothed round trip, seconds */
  uint32_t packets_sent, packets_received, packets_lost; /* lost: never acked */
  uint32_t resends;              /* reliable messages sent again */
  uint32_t bytes_sent, bytes_received;
} cw_net_peer_stats;
cw_net_peer_stats cw_net_get_peer_stats(cw_net_session *s, int player); /* zeros for an out-of-range player */

/* player = CW_NET_ALL for every connected peer. false when the payload is
 * too big or no such peer is connected. */
bool cw_net_send(cw_net_session *s, int player, cw_net_channel ch, uint8_t type, const void *data, int len);
/* One handler per message type (a later call replaces it; NULL removes it).
 * data is valid during the call only. */
typedef void (*cw_net_msg_fn)(cw_net_session *s, int from, const uint8_t *data, int len, void *user);
/* Handler for one message type (see above). Types below CW_NET_MSG_GAME belong to the engine: replacing
 * one (rollback uses some) breaks that feature. */
void cw_net_on_message(cw_net_session *s, uint8_t type, cw_net_msg_fn fn, void *user);
/* Peer joined / left listeners (up to 8; adding the same fn + user twice is one). */
typedef void (*cw_net_peer_fn)(cw_net_session *s, int player, bool connected, void *user);
void cw_net_on_peer(cw_net_session *s, cw_net_peer_fn fn, void *user); /* see above; logs when all 8 are taken */
/* Removes that fn + user pair; absent = no-op. */
void cw_net_off_peer(cw_net_session *s, cw_net_peer_fn fn, void *user);

/* ── the scene side: authority, RPC, replication ─────────────────────────
 * Attach a session to a tree: node paths on the wire are relative to `root`
 * (null = the tree root), so both peers must build the same tree under it.
 * One session per tree; the tree is found from any node in it. */
void cw_net_attach(cw_net_session *s, cw_tree *t, cw_node root);
cw_net_session *cw_net_session_of(cw_tree *t); /* the attached session, or NULL */

/* Authority: which player owns a node (simulates it, may change its
 * replicated state). Inherited from the parent; the session root defaults
 * to player 0 (the host). Setting it is local, as in Godot: every peer
 * makes the same call (a NetSpawner's children carry theirs with the spawn). */
void cw_net_set_authority(cw_node n, int player);
int cw_net_authority(cw_node n); /* nearest set authority up the parents; 0 when none or no session */
bool cw_net_is_authority(cw_node n); /* authority == the local player */

/* RPC: a node method (cw_node_type_add_method) callable from other peers.
 * Only configured methods are callable remotely. AUTHORITY: only the node's
 * authority may call it; ANY_PEER: anyone. call_local: cw_net_rpc also runs
 * it here. */
typedef enum { CW_RPC_AUTHORITY, CW_RPC_ANY_PEER } cw_rpc_mode;
/* Marks type.method as an RPC (subtypes inherit it); calling again replaces the config. Same on every peer. */
void cw_net_rpc_config(const char *type, const char *method, cw_rpc_mode mode, cw_net_channel ch, bool call_local);
/* Call on every connected peer (and locally when configured). false when the
 * method is not configured, the node is not under the session root, or the
 * local player may not call it. */
bool cw_net_rpc(cw_node n, const char *method, const cw_value *args, int argc);
/* One player only, never run locally; false (logged) when that player is not connected. */
bool cw_net_rpc_to(cw_node n, int player, const char *method, const cw_value *args, int argc);
/* Inside an RPC: the calling player (the local player for a local call); -1 outside. */
int cw_net_rpc_sender(void);

/* Node types registered by cw_net_register_types() (also called by
 * cw_net_attach):
 *
 *   NetSynchronizer  root_path ("..")     the node whose properties replicate
 *                    properties ("position")  comma-separated property names
 *                    interval (0)          seconds between updates (0 = every poll)
 *                    reliable (false)      channel
 *     The authority of the root node sends the listed properties; every
 *     other peer applies them (newer updates only).
 *
 *   NetSpawner       spawn_path ("..")     the node whose children replicate
 *                    spawnable ("")        comma-separated scene asset paths
 *                                          (*.cwscene) or node type names
 *                    spawn_properties ("") properties sent with each spawn
 *     When the spawner's authority adds a child instanced from a listed
 *     scene (or of a listed type) under spawn_path, every peer creates the
 *     same child with the same name; freeing it frees it everywhere. Peers
 *     that join later receive every live child. */
/* Call before loading scenes that contain these types (cw_net_attach calls
 * it; the scene module does not, so games without networking link none of it). */
void cw_net_register_types(void);

/* ── tree snapshots ─────────────────────────────────────────────────────
 * Save: every node (type, name, parent, child index), every writable
 * reflected property of every node (except Node.name, which is structure),
 * the tree's tick and random state.
 * Load: frees nodes the snapshot does not have, recreates (by type) nodes it
 * has and the tree lost, restores order, names and properties (a property is
 * written only when it differs, so an unchanged tree costs reads only). Node
 * identity is the node uid; node-valued properties are remapped. Must not be
 * called while the tree is processing. The hash covers types, structure and
 * values (not uids, names or pointers): equal on every host for equal state. */
typedef struct cw_state cw_state;
cw_state *cw_state_new(void); /* empty; cw_state_save fills it, cw_state_free frees it */
void cw_state_free(cw_state *st); /* NULL is a no-op */
void cw_state_save(cw_state *st, cw_tree *t); /* overwrites st (buffers reused); applies deferred tree changes first */
void cw_state_load(const cw_state *st, cw_tree *t); /* see above; asserts st was saved from this tree (same root uid) */
uint32_t cw_state_hash(const cw_state *st); /* FNV-1a; host-stable (see above) */
size_t cw_state_bytes(const cw_state *st); /* memory held */
int cw_state_node_count(const cw_state *st); /* nodes in the snapshot, root included */
/* Text form (one line per node and per value) for diffing two peers' states
 * after a desync. snprintf contract: returns the full length, writes at most
 * cap - 1 bytes + NUL (buf may be NULL to measure). */
size_t cw_state_dump(const cw_state *st, char *buf, size_t cap);
/* Leave a property out of snapshots and hashes (derived or cosmetic state). */
void cw_state_skip_prop(const char *type, const char *prop);
/* Private (unreflected) state a type keeps: save writes at most cap bytes
 * and returns the count; load gets them back. */
typedef int (*cw_state_save_fn)(cw_node n, void *data, uint8_t *out, int cap);
typedef void (*cw_state_load_fn)(cw_node n, void *data, const uint8_t *in, int len);
/* One pair per type (replaces); save gets cap 1024; up to 64 types. */
void cw_state_type_hook(const char *type, cw_state_save_fn save, cw_state_load_fn load);

/* ── rollback ───────────────────────────────────────────────────────────
 * Drives a tree's fixed step from inputs. Each player's input for a tick is
 * input_size opaque bytes (the game packs buttons and quantized axes). The
 * local input for tick T + input_delay is sampled at tick T and sent to every
 * peer (redundantly, unreliable); a remote input not yet received is
 * predicted as that player's last received one. When a received input
 * differs from the prediction a past tick used, the tree is restored to the
 * snapshot before that tick and re-simulated up to now. The local sim never
 * runs more than max_prediction ticks past the last tick whose inputs are
 * all known (it stalls instead), and slows down a tick when it is ahead of
 * the remote by more than one (time sync). Every confirmed state is hashed;
 * every hash_interval ticks the hash is exchanged and compared (desyncs).
 *
 * Inside the simulation (node fixed_update), cw_rollback_input(tree, p)
 * returns player p's input for the tick being simulated: never sample
 * devices there, and keep every random number on the tree (cw_tree_rng). */
#define CW_ROLLBACK_MAX_INPUT 32
typedef struct cw_rollback cw_rollback;
typedef struct cw_rollback_desc {
  cw_net_session *session; /* NULL: every player is local (sample_input is asked for each) */
  cw_tree *tree;
  int players;          /* 1..CW_NET_MAX_PEERS, slots 0..players-1 */
  int input_size;       /* bytes per player per tick, 1..CW_ROLLBACK_MAX_INPUT */
  int input_delay;      /* ticks; 0 = none (every prediction can miss); 2 hides 2 ticks of latency */
  int max_prediction;   /* ticks (8) */
  int hash_interval;    /* ticks between desync checks (10) */
  double dt;            /* fixed step seconds (1/60) */
  /* the local player's input for the tick about to be scheduled */
  void (*sample_input)(cw_rollback *rb, int player, void *out, void *user);
  void *user;
  bool no_rollback;     /* test control: wrong predictions are never corrected */
} cw_rollback_desc;

/* Asserts on a bad desc. Defaults apply for 0 fields except input_delay, where 0 means no delay.
 * Hooks the session's rollback messages and becomes the tree's cw_rollback_of. */
cw_rollback *cw_rollback_new(const cw_rollback_desc *desc);
void cw_rollback_free(cw_rollback *rb); /* unhooks the session and tree; frees neither. NULL is a no-op */
/* One platform tick: poll the session (at `now` seconds), correct
 * mispredictions (restore + re-simulate), then simulate one new tick unless
 * stalled. Returns the number of NEW ticks simulated (0 or 1). */
int cw_rollback_advance(cw_rollback *rb, double now);
/* The player's input_size bytes; all zero without a rollback or for a bad player. */
const void *cw_rollback_input(cw_tree *t, int player);
cw_rollback *cw_rollback_of(cw_tree *t); /* the tree's rollback, or NULL */
bool cw_rollback_resimulating(cw_rollback *rb); /* inside a re-simulation (skip effects) */
uint64_t cw_rollback_tick(cw_rollback *rb);           /* ticks simulated (the tree's current tick) */
uint64_t cw_rollback_confirmed_tick(cw_rollback *rb); /* every input before it is known */
/* The hash of the confirmed state after `tick` ticks; false when not
 * confirmed yet or already out of the history (1024 ticks). */
bool cw_rollback_state_hash(cw_rollback *rb, uint64_t tick, uint32_t *hash);
/* The snapshot of the state after `tick` ticks while it is still in the ring
 * (the last max_prediction + 3 ticks), NULL otherwise: cw_state_dump it to
 * diff two peers after a desync. */
const cw_state *cw_rollback_saved_state(cw_rollback *rb, uint64_t tick);

typedef struct cw_rollback_stats {
  uint32_t rollbacks;      /* restores */
  uint32_t resim_ticks;    /* ticks re-simulated */
  uint32_t max_depth;      /* deepest rollback, ticks */
  uint32_t stalls;         /* ticks not advanced: prediction window full */
  uint32_t sync_waits;     /* ticks not advanced: time sync */
  uint32_t mispredictions; /* received inputs that differed from the prediction used */
  uint32_t hashes_checked, desyncs;
  int64_t first_desync_tick; /* -1 = none */
  double rollback_ms_total, rollback_ms_max; /* restore + re-simulation wall time */
  double save_ms_total;    /* snapshot wall time */
  uint32_t frames;         /* advance calls */
  uint32_t state_bytes;    /* last snapshot's size */
} cw_rollback_stats;
cw_rollback_stats cw_rollback_get_stats(cw_rollback *rb); /* cumulative since cw_rollback_new */

#ifdef __cplusplus
}
#endif
#endif

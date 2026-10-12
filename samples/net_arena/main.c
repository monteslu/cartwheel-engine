/* net_arena: a two-player 2D arena on cartwheel/net.h rollback netcode.
 *
 * The simulation is a scene tree (Arena: two ArenaPlayer nodes, a Bullets
 * node whose ArenaBullet children are spawned and freed as they fly) driven
 * ONLY by cw_rollback_input: move bits, fire, and an aim angle per player
 * per tick. Bullets bounce off nothing: they die on walls and pillars, or
 * hit the other player (score, and a respawn at a random spot drawn from the
 * tree's rng when hit points run out). A bot plays each side (a schedule
 * hashed from the tick), unless a pad or the keyboard moves (then pad 0 /
 * WASD + space drive the local player).
 *
 * Modes:
 *   pair  (default) BOTH peers in this process over the loopback hub through
 *         the deterministic network simulator; the left half draws peer A's
 *         world, the right half peer B's. Runs on every host bit for bit.
 *   udp   (native) CW_NET_UDP=<port> CW_NET_PEER=<host:port> CW_NET_PLAYER=0|1:
 *         one peer per process over UDP (the simulator still injects
 *         latency, jitter and loss on top), real clock; quits once the
 *         sync tick is compared with the other side.
 *   peer  (carts) the debug command `net_join <ws url>` (or NET_ARENA_JOIN at
 *         build time) joins a room on tools/cwrelay.mjs through the wasmcart
 *         peer connection; the relay assigns the player slot. net_arena_peer
 *         joins ws://localhost:47731/arena at once; net_arena_peer_desync is
 *         the same with the desync control built in (tests/net.mjs).
 *
 * Network conditions (native env, pair and udp): CW_NET_LATENCY_MS (80),
 * CW_NET_JITTER_MS (40), CW_NET_LOSS (0.1). Controls the gates must see
 * fail: CW_NET_NO_ROLLBACK=1 (wrong predictions stay), CW_NET_DESYNC=1 (peer
 * B, or player 1 in udp mode, miscounts a score once at tick 300).
 * CW_NET_BALLAST=N adds N passive nodes to the simulated tree (snapshot
 * cost scaling).
 *
 * Debug fields: net.sync_tick, net.a.hash / net.b.hash (the confirmed state
 * hash at sync_tick; 0 until known), net.match, net.a.* / net.b.* rollback
 * stats, arena.* (peer A's world at the end), net.a.rb_ms (rollback ms per
 * frame, average) and friends (timing: never compared across hosts). */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/net.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

#define W 640
#define H 360
#define ARENA_W 600.0f
#define ARENA_H 300.0f
#define SYNC_TICK 600   /* the confirmed tick both peers report */
#define BOT_TICKS 640   /* bots play this long, then idle */
#define PLAYER_R 10.0f
#define BULLET_R 3.0f
#define MAX_HP 3
#define PROTOCOL 0x4E415231u /* "NAR1" */
/* net.a.trace: the confirmed hash at TRACE0 + i * TRACESTEP (bisect a divergence
 * between two runs by narrowing these). -DDUMP_TICK=n logs the whole confirmed
 * state at tick n (cw_state_dump) to diff two runs line by line. */
#ifndef TRACE0
#define TRACE0 0
#define TRACESTEP 40
#endif

static const cw_rect PILLARS[] = { { 140, 70, 40, 60 }, { 420, 170, 40, 60 }, { 280, 130, 40, 40 }, { 80, 220, 70, 20 }, { 450, 60, 70, 20 } };

/* ── the simulated node types ─────────────────────────────────────── */

typedef struct aplayer {
  int player, hp, score, cooldown, invuln, deaths;
  cw_vec2 vel;
  float aim;
} aplayer;
static const cw_prop aplayer_props[] = {
  CW_PROP_FIELD(aplayer, player, CW_T_INT, CW_PROP_DEFAULT, "input slot"),
  CW_PROP_FIELD(aplayer, hp, CW_T_INT, CW_PROP_DEFAULT, "hit points"),
  CW_PROP_FIELD(aplayer, score, CW_T_INT, CW_PROP_DEFAULT, "hits landed"),
  CW_PROP_FIELD(aplayer, cooldown, CW_T_INT, CW_PROP_DEFAULT, "ticks until the next shot"),
  CW_PROP_FIELD(aplayer, invuln, CW_T_INT, CW_PROP_DEFAULT, "ticks of invulnerability after a respawn"),
  CW_PROP_FIELD(aplayer, deaths, CW_T_INT, CW_PROP_DEFAULT, "respawns"),
  CW_PROP_FIELD(aplayer, vel, CW_T_VEC2, CW_PROP_DEFAULT, "velocity, units per tick"),
  CW_PROP_FIELD(aplayer, aim, CW_T_FLOAT, CW_PROP_DEFAULT, "aim angle, radians"),
  CW_PROP_END,
};
typedef struct abullet {
  cw_vec2 vel;
  int owner, ttl;
} abullet;
static const cw_prop abullet_props[] = {
  CW_PROP_FIELD(abullet, vel, CW_T_VEC2, CW_PROP_DEFAULT, "velocity, units per tick"),
  CW_PROP_FIELD(abullet, owner, CW_T_INT, CW_PROP_DEFAULT, "who fired it"),
  CW_PROP_FIELD(abullet, ttl, CW_T_INT, CW_PROP_DEFAULT, "ticks left"),
  CW_PROP_END,
};

enum { IN_LEFT = 1, IN_RIGHT = 2, IN_UP = 4, IN_DOWN = 8, IN_FIRE = 16 };

/* injected desync (control): which tree nudges itself, once */
static cw_tree *desync_tree;

static bool circle_hits_rect(cw_vec2 c, float r, cw_rect b, cw_vec2 *push) {
  float nx = CW_CLAMP(c.x, b.x, b.x + b.w), ny = CW_CLAMP(c.y, b.y, b.y + b.h);
  float dx = c.x - nx, dy = c.y - ny, d2 = dx * dx + dy * dy;
  if (d2 >= r * r) return false;
  if (push) {
    if (d2 > 1e-6f) {
      float d = sqrtf(d2);
      *push = cw_v2(dx / d * (r - d), dy / d * (r - d));
    } else { /* centre inside: out along the shallowest side */
      float l = c.x - b.x + r, rr = b.x + b.w - c.x + r, t = c.y - b.y + r, bt = b.y + b.h - c.y + r;
      float m = CW_MIN(CW_MIN(l, rr), CW_MIN(t, bt));
      *push = m == l ? cw_v2(-l, 0) : m == rr ? cw_v2(rr, 0) : m == t ? cw_v2(0, -t) : cw_v2(0, bt);
    }
  }
  return true;
}

/* #region player */
static void player_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  aplayer *p = data;
  cw_tree *t = cw_node_tree(n);
  const uint8_t *in = cw_rollback_input(t, p->player);
  float ax = (float)(!!(in[0] & IN_RIGHT) - !!(in[0] & IN_LEFT)), ay = (float)(!!(in[0] & IN_DOWN) - !!(in[0] & IN_UP));
  p->vel = cw_v2(p->vel.x * 0.85f + ax * 0.6f, p->vel.y * 0.85f + ay * 0.6f);
  p->aim = (float)in[1] * (6.2831853f / 256.0f);
/* #endregion */
  cw_vec2 pos = cw_v2_add(cw_node2d_position(n), p->vel);
  if (desync_tree == t && cw_tree_tick(t) == 300 && p->player == 0) p->score += 1; /* the control: one peer miscounts */
  pos.x = CW_CLAMP(pos.x, PLAYER_R, ARENA_W - PLAYER_R);
  pos.y = CW_CLAMP(pos.y, PLAYER_R, ARENA_H - PLAYER_R);
  for (size_t i = 0; i < CW_ARRAY_LEN(PILLARS); i++) {
    cw_vec2 push;
    if (circle_hits_rect(pos, PLAYER_R, PILLARS[i], &push)) pos = cw_v2_add(pos, push);
  }
  cw_node2d_set_position(n, pos);
  if (p->cooldown > 0) p->cooldown--;
  if (p->invuln > 0) p->invuln--;
  if ((in[0] & IN_FIRE) && p->cooldown == 0) {
    p->cooldown = 12;
    float s, c;
    cw_sincos(p->aim, &s, &c);
    char name[32];
    snprintf(name, sizeof name, "b%llu_%d", (unsigned long long)cw_tree_tick(t), p->player);
    cw_node b = cw_node_new("ArenaBullet", name);
    abullet *bd = cw_node_data(b, "ArenaBullet");
    bd->vel = cw_v2(c * 6, s * 6);
    bd->owner = p->player;
    bd->ttl = 90;
    cw_node2d_set_position(b, cw_v2(pos.x + c * (PLAYER_R + 4), pos.y + s * (PLAYER_R + 4)));
    cw_node_add_child(cw_node_get(cw_tree_root(t), "Arena/Bullets"), b); /* deferred: tree order is stable */
  }
}

static void bullet_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  abullet *b = data;
  cw_tree *t = cw_node_tree(n);
  cw_vec2 pos = cw_v2_add(cw_node2d_position(n), b->vel);
  cw_node2d_set_position(n, pos);
  bool dead = --b->ttl <= 0 || pos.x < 0 || pos.y < 0 || pos.x > ARENA_W || pos.y > ARENA_H;
  for (size_t i = 0; i < CW_ARRAY_LEN(PILLARS) && !dead; i++) dead = circle_hits_rect(pos, BULLET_R, PILLARS[i], NULL);
  for (int k = 0; k < 2 && !dead; k++) {
    if (k == b->owner) continue;
    cw_node pn = cw_node_get(cw_tree_root(t), k ? "Arena/P1" : "Arena/P0");
    aplayer *p = cw_node_data(pn, "ArenaPlayer");
    cw_vec2 d = cw_v2_sub(cw_node2d_position(pn), pos);
    if (p->invuln || cw_v2_dot(d, d) >= (PLAYER_R + BULLET_R) * (PLAYER_R + BULLET_R)) continue;
    dead = true;
    aplayer *shooter = cw_node_data(cw_node_get(cw_tree_root(t), b->owner ? "Arena/P1" : "Arena/P0"), "ArenaPlayer");
    shooter->score++;
    if (--p->hp <= 0) { /* respawn somewhere free, picked with the tree's rng */
      cw_rng *rng = cw_tree_rng(t);
      cw_vec2 at;
      bool free_spot;
      do {
        /* two statements: argument evaluation order is unspecified in C, and x86
         * and wasm clang really do draw these in different orders */
        at.x = cw_rng_range(rng, 30, ARENA_W - 30);
        at.y = cw_rng_range(rng, 30, ARENA_H - 30);
        free_spot = true;
        for (size_t i = 0; i < CW_ARRAY_LEN(PILLARS); i++) free_spot &= !circle_hits_rect(at, PLAYER_R * 2, PILLARS[i], NULL);
      } while (!free_spot);
      cw_node2d_set_position(pn, at);
      p->vel = cw_v2(0, 0);
      p->hp = MAX_HP;
      p->invuln = 60;
      p->deaths++;
    }
  }
  if (dead) cw_node_free(n);
}

static cw_node_type player_type = { .name = "ArenaPlayer", .parent = "Node2D", .data_size = sizeof(aplayer),
                                    .props = aplayer_props, .fixed_update = player_fixed,
                                    .doc = "net_arena: a player driven by its rollback input slot." };
static cw_node_type bullet_type = { .name = "ArenaBullet", .parent = "Node2D", .data_size = sizeof(abullet),
                                    .props = abullet_props, .fixed_update = bullet_fixed,
                                    .doc = "net_arena: a bullet; dies on walls, pillars and the other player." };

static cw_tree *make_world(int ballast) {
  cw_tree *t = cw_tree_new();
  cw_tree_seed(t, 0x5eed);
  cw_node arena = cw_node_new("Node2D", "Arena");
  cw_node_add_child(cw_tree_root(t), arena);
  for (int i = 0; i < 2; i++) {
    cw_node p = cw_node_new("ArenaPlayer", i ? "P1" : "P0");
    aplayer *d = cw_node_data(p, "ArenaPlayer");
    d->player = i;
    d->hp = MAX_HP;
    cw_node2d_set_position(p, i ? cw_v2(ARENA_W - 60, ARENA_H - 60) : cw_v2(60, 60));
    cw_node_add_child(arena, p);
  }
  cw_node_add_child(arena, cw_node_new("Node2D", "Bullets"));
  if (ballast > 0) {
    cw_node b = cw_node_new("Node2D", "Ballast");
    cw_node_add_child(arena, b);
    for (int i = 0; i < ballast; i++) {
      char name[16];
      snprintf(name, sizeof name, "n%d", i);
      cw_node n = cw_node_new("Node2D", name);
      cw_node2d_set_position(n, cw_v2((float)i, (float)(i * 7 % 300)));
      cw_node_add_child(b, n);
    }
  }
  return t;
}

/* ── peers ─────────────────────────────────────────────────────────── */

typedef struct peer_t {
  const char *label;
  cw_tree *tree;
  cw_net_session *session;
  cw_rollback *rb;
  uint32_t sync_hash;
  int32_t sync_known;
} peer_t;

static struct {
  int mode; /* 0 pair, 1 udp, 2 peer */
  peer_t a, b;
  cw_net_transport *sim_a, *sim_b; /* the simulators (pair and udp), for stats */
  cw_net_loopback *hub;
  cw_font font;
  bool human;
  int quit_in;
  /* debug fields */
  int32_t sync_tick, match, done, ticks, confirmed;
  uint32_t a_hash, b_hash;
  int32_t a_rollbacks, b_rollbacks, a_resim, b_resim, a_depth, b_depth, a_desyncs, b_desyncs, a_checked, b_checked;
  int32_t a_mispred, b_mispred, a_stalls, a_waits, local_player, state_bytes, state_nodes;
  float p0[2], p1[2];
  int32_t scores[2], deaths[2], bullets;
  float a_rb_ms, a_rb_ms_max, a_save_ms, b_rb_ms;
  uint32_t trace[16]; /* peer A's confirmed hash at TRACE0 + i * TRACESTEP */
  int32_t sim_sent, sim_dropped, sim_dup;
} G;

/* the local player's controls (IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN, IN_FIRE):
 * actions, read from the tick's snapshot, so a tap shorter than a tick still
 * reaches the rollback input */
static cw_action act[5];
static void add_actions(void) {
  static const char *const names[5] = { "left", "right", "up", "down", "fire" };
  static const char *const binds[5] = { "key:a, axis:lx-, pad:left", "key:d, axis:lx+, pad:right", "key:w, axis:ly-, pad:up",
                                        "key:s, axis:ly+, pad:down", "key:space, pad:south" };
  for (int i = 0; i < 5; i++) {
    act[i] = cw_action_add(names[i], 0.3f);
    cw_action_bind_text(act[i], binds[i]);
  }
}

static void sample(cw_rollback *rb, int player, void *out, void *user) {
  peer_t *me = user;
  uint8_t *in = out;
  uint8_t bits = 0;
  for (int i = 0; i < 5; i++)
    if (cw_action_pressed(0, act[i])) bits |= (uint8_t)(1u << i);
  if (G.mode != 0 && bits) G.human = true;
  if (G.human && G.mode != 0) { /* the local player is a person now */
    in[0] = bits;
    in[1] = (uint8_t)((cw_rollback_tick(rb) * 3) & 255); /* a sweeping aim */
    return;
  }
  uint64_t t = cw_rollback_tick(rb);
  if (t >= BOT_TICKS) return; /* idle: the sync tick is quiet on both sides */
  /* a bot that changes its mind every 9 or 14 ticks */
  uint32_t h = (uint32_t)(t / (uint64_t)(9 + player * 5)) * 2654435761u ^ (uint32_t)(player + 1) * 0x9E3779B9u;
  h ^= h >> 15;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  in[0] = (uint8_t)(h & 15);
  if ((h >> 4) % 4 != 0) in[0] |= IN_FIRE;
  /* aim at the other player as THIS peer sees it (a person aims at what is on
   * screen: the predicted state), give or take a few degrees */
  cw_node root = cw_tree_root(me->tree);
  cw_vec2 a = cw_node2d_position(cw_node_get(root, player ? "Arena/P1" : "Arena/P0"));
  cw_vec2 b = cw_node2d_position(cw_node_get(root, player ? "Arena/P0" : "Arena/P1"));
  float ang = cw_atan2(b.y - a.y, b.x - a.x);
  int idx = (int)(ang * (256.0f / 6.2831853f) + 256.5f) + (int)((h >> 8) % 9) - 4;
  in[1] = (uint8_t)(idx & 255);
}

static double env_num(const char *name, double def) {
  const char *v = getenv(name);
  return v && *v ? atof(v) : def;
}
/* #region peer */
static cw_net_sim_desc sim_desc(uint64_t seed) {
  return (cw_net_sim_desc){ .latency = env_num("CW_NET_LATENCY_MS", 80) / 1000.0, .jitter = env_num("CW_NET_JITTER_MS", 40) / 1000.0,
                            .loss = (float)env_num("CW_NET_LOSS", 0.1), .dup = 0.02f, .seed = seed };
}

static void make_peer(peer_t *p, const char *label, cw_net_transport *tr, int slot) {
  p->label = label;
  p->tree = make_world((int)env_num("CW_NET_BALLAST", 0));
  p->session = cw_net_session_new(&(cw_net_session_desc){ tr, slot, PROTOCOL, 3.0 });
  p->rb = cw_rollback_new(&(cw_rollback_desc){ .session = p->session, .tree = p->tree, .players = 2, .input_size = 2,
                                               .input_delay = 2, .max_prediction = 10, .hash_interval = 10, .dt = 1.0 / 60,
                                               .sample_input = sample, .user = p, .no_rollback = env_num("CW_NET_NO_ROLLBACK", 0) != 0 });
}
/* #endregion */

static void net_join(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) {
    cw_reply_error(r, "usage: net_join <ws url of a cwrelay room>");
    return;
  }
  if (G.a.session) {
    cw_reply_error(r, "already joined");
    return;
  }
  cw_net_sim_desc d = sim_desc(7);
  cw_net_transport *tr = cw_net_sim_new(cw_net_relay_new(cw_net_wcpeer_new()), &d);
  if (!tr) {
    cw_reply_error(r, "no peer transport on this build (carts only)");
    return;
  }
  G.mode = 2;
  G.sim_a = tr;
  make_peer(&G.a, "A", tr, -1);
  bool ok = cw_net_connect(G.a.session, argv[1]);
  cw_reply_printf(r, "{\"joined\":%s}", ok ? "true" : "false");
}

static void init(void *u) {
  (void)u;
  cw_node_type_register(&player_type);
  cw_node_type_register(&bullet_type);
  add_actions();
  G.font = cw_font_load("fonts/PressStart2P-Regular.ttf", &(cw_font_desc){ .mode = CW_FONT_BITMAP, .bake_size = 8, .threshold = true });
  G.sync_tick = SYNC_TICK;
  G.local_player = -1;
  if (getenv("CW_NET_UDP")) {
    G.mode = 1;
    int me = (int)env_num("CW_NET_PLAYER", 0);
    cw_net_transport *udp = cw_net_udp_new((int)env_num("CW_NET_UDP", 0));
    if (!udp) CW_FATAL("net_arena: cannot bind UDP port %s", getenv("CW_NET_UDP"));
    cw_net_sim_desc d = sim_desc(100u + (uint64_t)me);
    G.sim_a = cw_net_sim_new(udp, &d);
    make_peer(&G.a, "A", G.sim_a, me);
    if (env_num("CW_NET_DESYNC", 0) && me == 1) desync_tree = G.a.tree;
    if (getenv("CW_NET_PEER")) cw_net_connect(G.a.session, getenv("CW_NET_PEER"));
    cw_info("net_arena: udp port %d as player %d", cw_net_udp_port(udp), me);
  } else {
#ifdef NET_ARENA_JOIN /* the peer build: join the relay room at once */
    G.mode = 2;
    cw_net_sim_desc d = sim_desc(7);
    G.sim_a = cw_net_sim_new(cw_net_relay_new(cw_net_wcpeer_new()), &d);
    make_peer(&G.a, "A", G.sim_a, -1);
    if (!G.a.session) { /* natively there is no wc_peer: say so and leave cleanly */
      cw_warn("net_arena: the peer build joins over wc_peer, which only carts have (run the .wasc)");
      cw_quit();
      return;
    }
    cw_net_connect(G.a.session, NET_ARENA_JOIN);
#ifdef NET_ARENA_DESYNC
    desync_tree = G.a.tree; /* the control build: this peer miscounts at tick 300 */
#endif
#else
    /* #region pair */
    G.mode = 0;
    G.hub = cw_net_loopback_new();
    cw_net_sim_desc da = sim_desc(1), db = sim_desc(2);
    G.sim_a = cw_net_sim_new(cw_net_loopback_endpoint(G.hub, "a"), &da);
    G.sim_b = cw_net_sim_new(cw_net_loopback_endpoint(G.hub, "b"), &db);
    make_peer(&G.a, "A", G.sim_a, 0);
    make_peer(&G.b, "B", G.sim_b, 1);
    if (env_num("CW_NET_DESYNC", 0)) desync_tree = G.b.tree;
    cw_net_connect(G.b.session, "loop:a");
    /* #endregion */
#endif
  }
  cw_debug_command("net_join", "<ws url>", net_join);
  cw_debug_i32("net.sync_tick", &G.sync_tick);
  cw_debug_field("net.a.trace", G.trace, CW_DBG_U32, 16);
  cw_debug_u32("net.a.hash", &G.a_hash);
  cw_debug_u32("net.b.hash", &G.b_hash);
  cw_debug_i32("net.match", &G.match);
  cw_debug_i32("net.done", &G.done);
  cw_debug_i32("net.ticks", &G.ticks);
  cw_debug_i32("net.confirmed", &G.confirmed);
  cw_debug_i32("net.local_player", &G.local_player);
  cw_debug_i32("net.a.rollbacks", &G.a_rollbacks);
  cw_debug_i32("net.b.rollbacks", &G.b_rollbacks);
  cw_debug_i32("net.a.resim", &G.a_resim);
  cw_debug_i32("net.b.resim", &G.b_resim);
  cw_debug_i32("net.a.max_depth", &G.a_depth);
  cw_debug_i32("net.b.max_depth", &G.b_depth);
  cw_debug_i32("net.a.mispredictions", &G.a_mispred);
  cw_debug_i32("net.b.mispredictions", &G.b_mispred);
  cw_debug_i32("net.a.desyncs", &G.a_desyncs);
  cw_debug_i32("net.b.desyncs", &G.b_desyncs);
  cw_debug_i32("net.a.checked", &G.a_checked);
  cw_debug_i32("net.b.checked", &G.b_checked);
  cw_debug_i32("net.a.stalls", &G.a_stalls);
  cw_debug_i32("net.a.sync_waits", &G.a_waits);
  cw_debug_i32("net.state_bytes", &G.state_bytes);
  cw_debug_i32("net.state_nodes", &G.state_nodes);
  cw_debug_i32("net.sim.sent", &G.sim_sent);
  cw_debug_i32("net.sim.dropped", &G.sim_dropped);
  cw_debug_i32("net.sim.duplicated", &G.sim_dup);
  cw_debug_f32_array("arena.p0", G.p0, 2);
  cw_debug_f32_array("arena.p1", G.p1, 2);
  cw_debug_field("arena.scores", G.scores, CW_DBG_I32, 2);
  cw_debug_field("arena.deaths", G.deaths, CW_DBG_I32, 2);
  cw_debug_i32("arena.bullets", &G.bullets);
  cw_debug_f32("net.a.rb_ms", &G.a_rb_ms);
  cw_debug_f32("net.a.rb_ms_max", &G.a_rb_ms_max);
  cw_debug_f32("net.a.save_ms", &G.a_save_ms);
  cw_debug_f32("net.b.rb_ms", &G.b_rb_ms);
}

static void stats_out(peer_t *p, uint32_t *hash, int32_t *rb, int32_t *resim, int32_t *depth, int32_t *desync, int32_t *checked,
                      int32_t *mispred, float *rb_ms) {
  if (!p->rb) return;
  if (!p->sync_known && cw_rollback_state_hash(p->rb, SYNC_TICK, &p->sync_hash)) p->sync_known = 1;
  *hash = p->sync_known ? p->sync_hash : 0;
  cw_rollback_stats st = cw_rollback_get_stats(p->rb);
  *rb = (int32_t)st.rollbacks;
  *resim = (int32_t)st.resim_ticks;
  *depth = (int32_t)st.max_depth;
  *desync = (int32_t)st.desyncs;
  *checked = (int32_t)st.hashes_checked;
  *mispred = (int32_t)st.mispredictions;
  *rb_ms = st.frames ? (float)(st.rollback_ms_total / st.frames) : 0;
}

static void fixed(void *u, double dt) {
  (void)u;
  /* #region advance */
  double now = G.mode == 1 ? cw_time() : (double)cw_tick_index() * dt; /* pair: a virtual clock */
  if (G.a.rb) cw_rollback_advance(G.a.rb, now);
  if (G.b.rb) cw_rollback_advance(G.b.rb, now);
  /* #endregion */
  stats_out(&G.a, &G.a_hash, &G.a_rollbacks, &G.a_resim, &G.a_depth, &G.a_desyncs, &G.a_checked, &G.a_mispred, &G.a_rb_ms);
  stats_out(&G.b, &G.b_hash, &G.b_rollbacks, &G.b_resim, &G.b_depth, &G.b_desyncs, &G.b_checked, &G.b_mispred, &G.b_rb_ms);
  if (G.a.rb) {
    cw_rollback_stats st = cw_rollback_get_stats(G.a.rb);
#ifdef DUMP_TICK
    static bool dumped;
    const cw_state *ds = cw_rollback_saved_state(G.a.rb, DUMP_TICK);
    if (!dumped && ds && cw_rollback_confirmed_tick(G.a.rb) > DUMP_TICK + 1) {
      dumped = true;
      static char txt[1 << 15];
      cw_state_dump(ds, txt, sizeof txt);
      for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) cw_info("DUMP %s", l);
    }
#endif
    for (int i = 0; i < 16; i++)
      if (!G.trace[i]) cw_rollback_state_hash(G.a.rb, TRACE0 + (uint64_t)i * TRACESTEP, &G.trace[i]);
    G.a_rb_ms_max = (float)st.rollback_ms_max;
    G.a_save_ms = st.frames ? (float)(st.save_ms_total / st.frames) : 0;
    G.a_stalls = (int32_t)st.stalls;
    G.a_waits = (int32_t)st.sync_waits;
    G.state_bytes = (int32_t)st.state_bytes;
    G.ticks = (int32_t)cw_rollback_tick(G.a.rb);
    G.confirmed = (int32_t)cw_rollback_confirmed_tick(G.a.rb);
    G.local_player = G.a.session ? cw_net_local_player(G.a.session) : -1; /* the peer build natively: no session */
    cw_node root = cw_tree_root(G.a.tree);
    G.state_nodes = cw_tree_node_count(G.a.tree);
    for (int i = 0; i < 2; i++) {
      cw_node pn = cw_node_get(root, i ? "Arena/P1" : "Arena/P0");
      cw_vec2 pos = cw_node2d_position(pn);
      aplayer *pd = cw_node_data(pn, "ArenaPlayer");
      float *dst = i ? G.p1 : G.p0;
      dst[0] = pos.x;
      dst[1] = pos.y;
      G.scores[i] = pd->score;
      G.deaths[i] = pd->deaths;
    }
    G.bullets = cw_node_child_count(cw_node_get(root, "Arena/Bullets"));
  }
  cw_net_sim_stats ss = cw_net_sim_get_stats(G.sim_a), sb = cw_net_sim_get_stats(G.sim_b);
  G.sim_sent = (int32_t)(ss.sent + sb.sent);
  G.sim_dropped = (int32_t)(ss.dropped + sb.dropped);
  G.sim_dup = (int32_t)(ss.duplicated + sb.duplicated);
  if (G.mode == 0) G.match = G.a.sync_known && G.b.sync_known && G.a_hash == G.b_hash;
  /* done: the sync tick is hashed here, and every hash up to it was compared
   * with the other side (pair: both sides) */
  int want = SYNC_TICK / 10 + 1;
  bool done = G.a.sync_known && G.a_checked >= want && (G.mode != 0 || (G.b.sync_known && G.b_checked >= want));
  if (done && !G.done) {
    G.done = 1;
    cw_info("net_arena: sync tick %d hash %08x (%s), %d hashes compared, %d desyncs, %d rollbacks (%d ticks re-simulated, "
            "deepest %d), rollback %.3f ms/frame avg, %.3f ms max, snapshot %.3f ms/frame, %d bytes",
            SYNC_TICK, G.a_hash, G.mode == 0 ? (G.match ? "both peers match" : "PEERS DIFFER") : "local", G.a_checked,
            G.a_desyncs, G.a_rollbacks, G.a_resim, G.a_depth, G.a_rb_ms, G.a_rb_ms_max, G.a_save_ms, G.state_bytes);
    if (G.mode == 1) G.quit_in = 60; /* keep answering the peer a second, then leave */
  }
  if (G.quit_in > 0 && --G.quit_in == 0) cw_quit();
}

/* ── drawing ───────────────────────────────────────────────────────── */

static void draw_world(peer_t *p, float ox, float oy, float sc, const char *title) {
  cw_r2d_rect(cw_rect_make(ox, oy, ARENA_W * sc, ARENA_H * sc), cw_hex(0x1d2330ff));
  cw_r2d_rect_lines(cw_rect_make(ox, oy, ARENA_W * sc, ARENA_H * sc), 1, cw_hex(0x5a6a88ff));
  for (size_t i = 0; i < CW_ARRAY_LEN(PILLARS); i++) {
    cw_rect r = PILLARS[i];
    cw_r2d_rect(cw_rect_make(ox + r.x * sc, oy + r.y * sc, r.w * sc, r.h * sc), cw_hex(0x46506aff));
  }
  if (!p->tree) return;
  cw_node root = cw_tree_root(p->tree);
  cw_node bullets = cw_node_get(root, "Arena/Bullets");
  for (int i = 0; i < cw_node_child_count(bullets); i++) {
    cw_node b = cw_node_child(bullets, i);
    abullet *bd = cw_node_data(b, "ArenaBullet");
    cw_vec2 pos = cw_node2d_position(b);
    cw_r2d_circle(cw_v2(ox + pos.x * sc, oy + pos.y * sc), BULLET_R * sc + 0.5f, 8, bd->owner ? cw_hex(0xffa040ff) : cw_hex(0x50d0ffff));
  }
  static const uint32_t col[2] = { 0x3aa0ffff, 0xff7a2aff };
  for (int i = 0; i < 2; i++) {
    cw_node pn = cw_node_get(root, i ? "Arena/P1" : "Arena/P0");
    aplayer *pd = cw_node_data(pn, "ArenaPlayer");
    cw_vec2 pos = cw_node2d_position(pn);
    cw_vec2 c = cw_v2(ox + pos.x * sc, oy + pos.y * sc);
    cw_color k = cw_hex(col[i]);
    if (pd->invuln && (pd->invuln / 4) % 2) k.a = 0.35f;
    cw_r2d_circle(c, PLAYER_R * sc, 20, k);
    float s, co;
    cw_sincos(pd->aim, &s, &co);
    cw_r2d_line(c, cw_v2(c.x + co * PLAYER_R * 1.8f * sc, c.y + s * PLAYER_R * 1.8f * sc), 2, cw_hex(0xffffffff));
    for (int h = 0; h < pd->hp; h++) cw_r2d_rect(cw_rect_make(c.x - 6 + (float)h * 5, c.y - PLAYER_R * sc - 6, 4, 3), cw_hex(0x60ff80ff));
  }
  char line[96];
  cw_text_style st = { .size = 8, .color = cw_hex(0xe8ecf4ff) };
  aplayer *a0 = cw_node_data(cw_node_get(root, "Arena/P0"), "ArenaPlayer"), *a1 = cw_node_data(cw_node_get(root, "Arena/P1"), "ArenaPlayer");
  snprintf(line, sizeof line, "%s  %d : %d", title, a0->score, a1->score);
  cw_r2d_text(G.font, line, cw_v2(ox, oy - 12), &st);
  if (p->rb) {
    cw_rollback_stats rs = cw_rollback_get_stats(p->rb);
    snprintf(line, sizeof line, "T%llu C%llu RB%u D%u", (unsigned long long)cw_rollback_tick(p->rb),
             (unsigned long long)cw_rollback_confirmed_tick(p->rb), rs.rollbacks, rs.desyncs);
    cw_r2d_text(G.font, line, cw_v2(ox, oy + ARENA_H * sc + 4), &st);
  }
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x0e1118ff) });
  if (G.mode == 0) {
    draw_world(&G.a, 12, 40, 0.5f, "PEER A");
    draw_world(&G.b, 328, 40, 0.5f, "PEER B");
  } else {
    char title[32];
    snprintf(title, sizeof title, "PLAYER %d", G.local_player);
    draw_world(&G.a, 20, 30, 1.0f, G.a.rb && G.local_player >= 0 ? title : "WAITING");
  }
  char line[96];
  cw_text_style st = { .size = 8, .color = G.match || (G.mode && G.done && !G.a_desyncs) ? cw_hex(0x80ff90ff) : cw_hex(0xc8cdd8ff) };
  if (G.mode == 0)
    snprintf(line, sizeof line, "SYNC T%d  A %08X  B %08X  %s", SYNC_TICK, G.a_hash, G.b_hash,
             !G.a_hash || !G.b_hash ? "..." : G.match ? "MATCH" : "DIFFER");
  else snprintf(line, sizeof line, "SYNC T%d  %08X  DESYNCS %d", SYNC_TICK, G.a_hash, G.a_desyncs);
  cw_r2d_text(G.font, line, cw_v2(12, H - 18), &st);
  cw_r2d_end();
}

static void shutdown_app(void *u) {
  (void)u;
  peer_t *ps[2] = { &G.a, &G.b };
  for (int i = 0; i < 2; i++) {
    if (ps[i]->rb) cw_rollback_free(ps[i]->rb);
    if (ps[i]->session) cw_net_session_free(ps[i]->session);
    if (ps[i]->tree) cw_tree_free(ps[i]->tree);
  }
  if (G.hub) cw_net_loopback_free(G.hub);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_scene_register_builtin_types();
  return (cw_app_desc){ .title = "net_arena", .width = W, .height = H, .debug = true, .net = true, .init = init,
                        .fixed_update = fixed, .render = render, .shutdown = shutdown_app };
}

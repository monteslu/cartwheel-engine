/* Rollback Duel (the multiplayer-rollback tutorial): two racers chase a coin.
 * The simulated world is world.cwscene; every peer instances its own copy
 * into a private tree and cartwheel/net.h rollback drives that tree's fixed
 * step from the players' inputs only.
 *
 * Modes:
 *   pair (default, every host) both peers in this process over the
 *        loopback hub, through the deterministic network simulator; peer A
 *        is drawn on the left, peer B on the right.
 *   udp  (native) DUEL_UDP=<port> DUEL_PEER=<host:port> DUEL_PLAYER=0|1:
 *        one peer per process over UDP, real clock.
 *
 * Env (native): DUEL_LATENCY_MS (80), DUEL_JITTER_MS (30), DUEL_LOSS (0.1),
 * DUEL_NO_ROLLBACK=1 (wrong predictions are never corrected: the tutorial's
 * control), DUEL_DESYNC=1 (peer B miscounts a score once).
 *
 * Debug fields: duel.ticks, duel.hash_a / duel.hash_b (the confirmed state
 * hash at tick 600, 0 until known), duel.match, duel.desyncs,
 * duel.checked, duel.rollbacks, duel.resim, duel.mispredictions,
 * duel.score (peer A's view: player 0, player 1), duel.dropped. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/net.h"
#include "cartwheel/project.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

#define SYNC_TICK 600            /* the tick whose confirmed hash both peers report */
#define PROTOCOL 0x44554C31u     /* "DUL1": peers with another protocol are refused */
#define ARENA_W 150.0f
#define ARENA_H 110.0f
enum { IN_LEFT = 1, IN_RIGHT = 2, IN_UP = 4, IN_DOWN = 8 };

/* #region racer */
/* A Racer is a Polygon2D (it draws as one) with game state of its own. The
 * fields are reflected properties, so tree snapshots save them, restore
 * them and hash them: state that is not reflected would not roll back. */
typedef struct racer {
  int player, score;
  cw_vec2 vel;
} racer;
static const cw_prop racer_props[] = {
  CW_PROP_FIELD(racer, player, CW_T_INT, CW_PROP_DEFAULT, "input slot"),
  CW_PROP_FIELD(racer, score, CW_T_INT, CW_PROP_DEFAULT, "coins collected"),
  CW_PROP_FIELD(racer, vel, CW_T_VEC2, CW_PROP_DEFAULT, "velocity, units per tick"),
  CW_PROP_END,
};

static cw_tree *desync_tree; /* DUEL_DESYNC: the tree that miscounts once */

static void racer_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  racer *r = data;
  cw_tree *t = cw_node_tree(n);
  /* the input for the tick being simulated: never read devices in here */
  uint8_t in = *(const uint8_t *)cw_rollback_input(t, r->player);
  float ax = (float)(!!(in & IN_RIGHT) - !!(in & IN_LEFT));
  float ay = (float)(!!(in & IN_DOWN) - !!(in & IN_UP));
  r->vel = cw_v2(r->vel.x * 0.8f + ax * 0.4f, r->vel.y * 0.8f + ay * 0.4f);
  cw_vec2 p = cw_v2_add(cw_node2d_position(n), r->vel);
  p.x = CW_CLAMP(p.x, 5, ARENA_W - 5);
  p.y = CW_CLAMP(p.y, 5, ARENA_H - 5);
  cw_node2d_set_position(n, p);

  cw_node coin = cw_node_get(n, "../Coin");
  cw_vec2 d = cw_v2_sub(cw_node2d_position(coin), p);
  if (cw_v2_dot(d, d) < 10 * 10) {
    r->score++;
    /* the next coin: random, from the TREE's generator (it is in the
     * snapshot, so a re-simulation draws the same numbers) */
    cw_rng *rng = cw_tree_rng(t);
    float x = cw_rng_range(rng, 10, ARENA_W - 10); /* two statements: C leaves */
    float y = cw_rng_range(rng, 10, ARENA_H - 10); /* argument order unspecified */
    cw_node2d_set_position(coin, cw_v2(x, y));
  }
  if (t == desync_tree && cw_tree_tick(t) == 300 && r->player == 0) r->score += 1;
}

static cw_node_type racer_type = { .name = "Racer", .parent = "Polygon2D", .data_size = sizeof(racer),
                                   .props = racer_props, .fixed_update = racer_fixed,
                                   .doc = "Rollback Duel: a racer driven by its rollback input slot." };
/* #endregion */

/* #region peer_state */
typedef struct peer {
  cw_tree *world;          /* this peer's private copy of world.cwscene */
  cw_net_session *session;
  cw_rollback *rb;
  cw_texture view;         /* the world is drawn into this target */
  uint32_t sync_hash;
  bool sync_known;
} peer;
/* #endregion */

static struct {
  bool udp;
  peer a, b;
  cw_net_loopback *hub;
  cw_net_transport *sim_a, *sim_b;
  cw_action move[4]; /* project.cwproj input.move_left, _right, _up, _down */
  /* debug fields */
  int32_t ticks, match, desyncs, checked, rollbacks, resim, mispredictions, dropped;
  uint32_t hash_a, hash_b;
  int32_t score[2];
  int quit_in;
} G;

static double env_num(const char *name, double def) {
  const char *v = getenv(name);
  return v && *v ? atof(v) : def;
}

/* #region world */
static cw_tree *make_world(void) {
  cw_tree *t = cw_tree_new();
  cw_tree_seed(t, 0xd0e1); /* both peers start from the same seed */
  cw_node_add_child(cw_tree_root(t), cw_scene_instance_path("world.cwscene"));
  return t;
}
/* #endregion */

/* #region input */
/* Asked once per tick for the LOCAL player's input, outside the
 * simulation: here reading devices is fine. A pad or the keyboard drives
 * player 0 on peer A (in udp mode: this process's player); with nobody at the controls a bot steers toward the
 * coin as this peer sees it (a person also reacts to the predicted state on
 * screen). One byte per player per tick. */
static void sample_input(cw_rollback *rb, int player, void *out, void *user) {
  peer *me = user;
  uint8_t *in = out; /* zeroed */
  if (G.udp || player == 0) {
    for (int i = 0; i < 4; i++)
      if (cw_action_pressed(0, G.move[i])) *in |= (uint8_t)(1u << i); /* IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN */
    if (*in) return;
  }
  cw_node arena = cw_node_get(cw_tree_root(me->world), "Arena");
  cw_vec2 at = cw_node2d_position(cw_node_get(arena, player ? "P1" : "P0"));
  cw_vec2 coin = cw_node2d_position(cw_node_get(arena, "Coin"));
  uint64_t tick = cw_rollback_tick(rb);
  uint32_t wobble = (uint32_t)(tick / (7 + 4 * (uint64_t)player)) * 2654435761u >> 28; /* changes its mind now and then */
  *in = (uint8_t)((coin.x < at.x - 2 ? IN_LEFT : coin.x > at.x + 2 ? IN_RIGHT : 0) |
                  (coin.y < at.y - 2 ? IN_UP : coin.y > at.y + 2 ? IN_DOWN : 0));
  if (wobble < 4) *in = (uint8_t)(1u << wobble); /* a quarter of the time: a wrong turn */
}
/* #endregion */

/* #region simulator */
/* The network simulator wraps any transport: latency, jitter (which
 * reorders), loss and duplicates, from a seeded generator and the clock the
 * caller passes, so the same run gives the same network on every host. */
static cw_net_transport *simulated(cw_net_transport *base, uint64_t seed) {
  cw_net_sim_desc d = { .latency = env_num("DUEL_LATENCY_MS", 80) / 1000.0,
                        .jitter = env_num("DUEL_JITTER_MS", 30) / 1000.0,
                        .loss = (float)env_num("DUEL_LOSS", 0.1), .dup = 0.02f, .seed = seed };
  return cw_net_sim_new(base, &d);
}
/* #endregion */

/* #region make_peer */
static void make_peer(peer *p, cw_net_transport *transport, int slot) {
  p->world = make_world();
  p->session = cw_net_session_new(&(cw_net_session_desc){ .transport = transport, .local_player = slot,
                                                          .protocol = PROTOCOL, .timeout = 3.0 });
  p->rb = cw_rollback_new(&(cw_rollback_desc){
      .session = p->session, .tree = p->world, .players = 2, .input_size = 1,
      .input_delay = 2,     /* ticks: hides up to 2 ticks of latency with no rollback at all */
      .max_prediction = 10, /* ticks the sim may run ahead of confirmed input before it stalls */
      .hash_interval = 10,  /* compare confirmed state hashes every 10 ticks */
      .dt = 1.0 / 60, .sample_input = sample_input, .user = p,
      .no_rollback = env_num("DUEL_NO_ROLLBACK", 0) != 0 });
  p->view = cw_target_create(&(cw_target_desc){ .width = (int)ARENA_W, .height = (int)ARENA_H, .filter = CW_FILTER_NEAREST });
}
/* #endregion */

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  static const char *const moves[4] = { "move_left", "move_right", "move_up", "move_down" };
  for (int i = 0; i < 4; i++) G.move[i] = cw_action_find(moves[i]);
  cw_node_type_register(&racer_type);
  /* #region transports */
  if (getenv("DUEL_UDP")) {
    /* natively: one peer per process over UDP (the simulator still adds
     * latency and loss on top, so a quiet LAN behaves like the internet) */
    G.udp = true;
    int me = (int)env_num("DUEL_PLAYER", 0);
    cw_net_transport *udp = cw_net_udp_new((int)env_num("DUEL_UDP", 0));
    if (!udp) CW_FATAL("rollback_duel: cannot bind UDP port %s", getenv("DUEL_UDP"));
    G.sim_a = simulated(udp, 100u + (uint64_t)me);
    make_peer(&G.a, G.sim_a, me);
    if (getenv("DUEL_PEER")) cw_net_connect(G.a.session, getenv("DUEL_PEER"));
  } else {
    /* everywhere: both peers in this process, joined by a loopback hub */
    G.hub = cw_net_loopback_new();
    G.sim_a = simulated(cw_net_loopback_endpoint(G.hub, "a"), 1);
    G.sim_b = simulated(cw_net_loopback_endpoint(G.hub, "b"), 2);
    make_peer(&G.a, G.sim_a, 0);
    make_peer(&G.b, G.sim_b, 1);
    cw_net_connect(G.b.session, "loop:a"); /* B dials A; the handshake runs in poll */
  }
  /* #endregion */
  if (env_num("DUEL_DESYNC", 0)) desync_tree = G.udp ? (env_num("DUEL_PLAYER", 0) == 1 ? G.a.world : NULL) : G.b.world;
  /* #region debug */
  cw_debug_i32("duel.ticks", &G.ticks);
  cw_debug_u32("duel.hash_a", &G.hash_a);
  cw_debug_u32("duel.hash_b", &G.hash_b);
  cw_debug_i32("duel.match", &G.match);
  cw_debug_i32("duel.desyncs", &G.desyncs);
  cw_debug_i32("duel.checked", &G.checked);
  cw_debug_i32("duel.rollbacks", &G.rollbacks);
  cw_debug_i32("duel.resim", &G.resim);
  cw_debug_i32("duel.mispredictions", &G.mispredictions);
  cw_debug_i32("duel.dropped", &G.dropped);
  cw_debug_field("duel.score", G.score, CW_DBG_I32, 2);
  /* #endregion */
}

/* #region sync_hash */
/* Both peers hash every confirmed state; SYNC_TICK's hash is kept for the
 * test, and the in-band check (hash_interval) counts disagreements. */
static void read_sync(peer *p, uint32_t *out) {
  if (!p->rb) return;
  if (!p->sync_known) p->sync_known = cw_rollback_state_hash(p->rb, SYNC_TICK, &p->sync_hash);
  *out = p->sync_known ? p->sync_hash : 0;
  cw_rollback_stats st = cw_rollback_get_stats(p->rb);
  G.desyncs += (int32_t)st.desyncs;
  G.checked += (int32_t)st.hashes_checked;
  G.rollbacks += (int32_t)st.rollbacks;
  G.resim += (int32_t)st.resim_ticks;
  G.mispredictions += (int32_t)st.mispredictions;
}
/* #endregion */

/* the debug fields, the HUD line, and (udp) when to quit */
static void report(void) {
  G.desyncs = G.checked = G.rollbacks = G.resim = G.mispredictions = 0;
  read_sync(&G.a, &G.hash_a);
  read_sync(&G.b, &G.hash_b);
  G.match = G.udp ? 0 : (G.a.sync_known && G.b.sync_known && G.hash_a == G.hash_b);
  G.ticks = (int32_t)cw_rollback_tick(G.a.rb);
  cw_node arena = cw_node_get(cw_tree_root(G.a.world), "Arena");
  for (int i = 0; i < 2; i++) G.score[i] = ((racer *)cw_node_data(cw_node_get(arena, i ? "P1" : "P0"), "Racer"))->score;
  cw_net_sim_stats sa = cw_net_sim_get_stats(G.sim_a), sb = G.sim_b ? cw_net_sim_get_stats(G.sim_b) : (cw_net_sim_stats){ 0 };
  G.dropped = (int32_t)(sa.dropped + sb.dropped);
  /* udp: once tick 600 is confirmed and compared, keep answering the other
   * side for a second, then quit */
  if (G.udp && G.hash_a && G.checked > SYNC_TICK / 10 && !G.quit_in) G.quit_in = 60;
  if (G.quit_in > 0 && --G.quit_in == 0) cw_quit();

  char line[160];
  if (G.udp)
    snprintf(line, sizeof line, "player %d  score %d : %d  tick %d\nrollbacks %d  sync %08X  desyncs %d", cw_net_local_player(G.a.session),
             G.score[0], G.score[1], G.ticks, G.rollbacks, G.hash_a, G.desyncs);
  else
    snprintf(line, sizeof line, "score %d : %d  tick %d  rollbacks %d\nsync A %08X  B %08X  %s", G.score[0], G.score[1], G.ticks,
             G.rollbacks, G.hash_a, G.hash_b, !G.hash_a || !G.hash_b ? "..." : G.match ? "MATCH" : "DIFFER");
  cw_label_set_text(cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main/HUD/Status"), line);
}

/* #region tick */
static void fixed_update(void *user, double dt) {
  (void)user;
  /* the pair runs on a virtual clock (ticks * dt): the simulated network
   * then delivers the same packets on the same ticks on every host */
  double now = G.udp ? cw_time() : (double)cw_tick_index() * dt;
  if (G.a.rb) cw_rollback_advance(G.a.rb, now);
  if (G.b.rb) cw_rollback_advance(G.b.rb, now);
  report();
}
/* #endregion */

/* #region draw */
/* After the main scene (the HUD) is drawn: each peer's world into its
 * target through a camera of ours, then both targets onto the canvas. */
static void render(void *user, double alpha) {
  (void)user;
  peer *ps[2] = { &G.a, &G.b };
  for (int i = 0; i < 2; i++)
    if (ps[i]->world)
      cw_canvas_draw_view(ps[i]->world, &(cw_canvas_view){ .target = ps[i]->view, .use_camera = true,
                                                           .camera = { .position = { ARENA_W / 2, ARENA_H / 2 } },
                                                           .clear = true, .clear_color = cw_hex(0x1d2330ff) }, alpha);
  /* a window pass (no clear) over the HUD, in window pixels like the
   * HUD's controls (the views above put back the layer settings they used,
   * so layer 0 is still screen space) */
  cw_r2d_begin(&(cw_r2d_pass_desc){ 0 });
  for (int i = 0; i < 2; i++)
    if (ps[i]->world) cw_r2d_sprite(&(cw_sprite){ .texture = ps[i]->view, .pos = { i ? 330.0f : 10.0f, 64 }, .scale = { 2, 2 } });
  cw_r2d_end();
}
/* #endregion */

static void shutdown_app(void *user) {
  (void)user;
  peer *ps[2] = { &G.a, &G.b };
  for (int i = 0; i < 2; i++) {
    if (ps[i]->rb) cw_rollback_free(ps[i]->rb);
    if (ps[i]->session) cw_net_session_free(ps[i]->session); /* destroys its transport */
    if (ps[i]->world) cw_tree_free(ps[i]->world);
  }
  if (G.hub) cw_net_loopback_free(G.hub);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc app = { .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed_update,
                             .render = render, .shutdown = shutdown_app };
  cw_project_apply_app(&app);
  const char *main_scene = cw_project_get("main_scene");
  return cw_scene_app(main_scene ? main_scene : "main.cwscene", &app);
}

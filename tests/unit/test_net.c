/* cartwheel/net.h: channels over a lossy simulated network, the handshake,
 * tree snapshots, RPC, NetSynchronizer / NetSpawner and rollback (with its
 * own must-fail control: the same run without rollback must desync).
 * Everything runs on a virtual clock over the loopback hub, so the results
 * (folded into unit.checksum) are identical on every host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/net.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene_file.h"
#include "scene/scene_internal.h"
#include "test.h"

#define DT (1.0 / 60)

static cw_net_transport *lossy(cw_net_loopback *hub, const char *name, uint64_t seed, float loss) {
  cw_net_sim_desc d = { .latency = 0.05, .jitter = 0.03, .loss = loss, .dup = 0.05f, .seed = seed };
  return cw_net_sim_new(cw_net_loopback_endpoint(hub, name), &d);
}

/* ── channels ──────────────────────────────────────────────────────── */
static struct { int rel_next, rel_bad, unrel, unrel_dup_or_bad; } rx;
static void on_rel(cw_net_session *s, int from, const uint8_t *d, int len, void *u) {
  (void)s; (void)from; (void)u;
  int v;
  if (len != 4) { rx.rel_bad++; return; }
  memcpy(&v, d, 4);
  if (v != rx.rel_next) rx.rel_bad++;
  rx.rel_next = v + 1;
}
static void on_unrel(cw_net_session *s, int from, const uint8_t *d, int len, void *u) {
  (void)s; (void)from; (void)d; (void)u;
  if (len != 4) rx.unrel_dup_or_bad++;
  rx.unrel++;
}

static void test_channels(void) {
  cw_net_loopback *hub = cw_net_loopback_new();
  cw_net_session *a = cw_net_session_new(&(cw_net_session_desc){ lossy(hub, "a", 1, 0.3f), 0, 0xC0DE, 0 });
  cw_net_session *b = cw_net_session_new(&(cw_net_session_desc){ lossy(hub, "b", 2, 0.3f), 1, 0xC0DE, 0 });
  /* an intruder with another protocol must be refused */
  cw_net_session *x = cw_net_session_new(&(cw_net_session_desc){ cw_net_loopback_endpoint(hub, "x"), 2, 0xBAD, 0 });
  cw_net_on_message(b, 40, on_rel, NULL);
  cw_net_on_message(b, 41, on_unrel, NULL);
  CHECK(cw_net_connect(b, "loop:a"));
  CHECK(cw_net_connect(x, "loop:a"));
  memset(&rx, 0, sizeof rx);
  int sent_rel = 0, sent_unrel = 0;
  double now = 0;
  for (int step = 0; step < 900; step++, now += DT) {
    cw_net_poll(a, now);
    cw_net_poll(b, now);
    cw_net_poll(x, now);
    if (cw_net_peer_connected(a, 1) && step < 600) {
      for (int k = 0; k < 3; k++, sent_rel++) cw_net_send(a, 1, CW_NET_RELIABLE, 40, &sent_rel, 4);
      cw_net_send(a, 1, CW_NET_UNRELIABLE, 41, &sent_unrel, 4);
      sent_unrel++;
    }
  }
  CHECK(cw_net_peer_connected(a, 1));
  CHECK(cw_net_peer_connected(b, 0));
  CHECK(!cw_net_peer_connected(a, 2)); /* the intruder */
  CHECK_EQ_INT(cw_net_peer_count(a), 1);
  CHECK(sent_rel > 1000);
  CHECK_EQ_INT(rx.rel_next, sent_rel); /* every reliable message, once, in order */
  CHECK_EQ_INT(rx.rel_bad, 0);
  CHECK(rx.unrel > sent_unrel / 2 && rx.unrel < sent_unrel); /* lossy, but most arrive */
  cw_net_peer_stats st = cw_net_get_peer_stats(a, 1);
  CHECK(st.resends > 0);
  CHECK(st.rtt > 0.08 && st.rtt < 0.3); /* 2 x (50 ms + up to 30 ms jitter) + ack delay */
  cw_info("net channels: %d reliable in order, %d/%d unreliable, %u resends, rtt %.0f ms", rx.rel_next, rx.unrel,
          sent_unrel, st.resends, st.rtt * 1000);
  FOLD(rx);
  FOLD(st.resends);
  cw_net_session_free(x);
  cw_net_session_free(a);
  cw_net_session_free(b);
  cw_net_loopback_free(hub);
}

/* ── snapshots ─────────────────────────────────────────────────────── */
typedef struct counter { int visible_count; int hidden; } counter;
static const cw_prop counter_props[] = { CW_PROP_FIELD(counter, visible_count, CW_T_INT, CW_PROP_DEFAULT, "reflected"), CW_PROP_END };
static int counter_save(cw_node n, void *d, uint8_t *out, int cap) { (void)n; (void)cap; memcpy(out, &((counter *)d)->hidden, 4); return 4; }
static void counter_load(cw_node n, void *d, const uint8_t *in, int len) { (void)n; if (len == 4) memcpy(&((counter *)d)->hidden, in, 4); }
static cw_node_type counter_type = { .name = "TestCounter", .parent = "Node2D", .data_size = sizeof(counter), .props = counter_props };

static void test_state(void) {
  cw_scene_register_builtin_types();
  if (!cw_node_type_find("TestCounter")) cw_node_type_register(&counter_type);
  cw_state_type_hook("TestCounter", counter_save, counter_load);
  cw_tree *t = cw_tree_new();
  cw_tree_seed(t, 7);
  cw_node world = cw_node_new("Node2D", "World");
  cw_node_add_child(cw_tree_root(t), world);
  cw_node a = cw_node_new("TestCounter", "A"), b = cw_node_new("Node2D", "B"), c = cw_node_new("Node2D", "C");
  cw_node_add_child(world, a);
  cw_node_add_child(world, b);
  cw_node_add_child(b, c);
  cw_node2d_set_position(a, cw_v2(1, 2));
  cw_node2d_set_position(c, cw_v2(5, 6));
  cw_node_set(a, "visible_count", cw_val_int(3));
  ((counter *)cw_node_data(a, "TestCounter"))->hidden = 42;
  uint64_t uid_b = cw_node_uid(b), uid_c = cw_node_uid(c);

  cw_state *s0 = cw_state_new(), *s1 = cw_state_new();
  cw_state_save(s0, t);
  uint32_t h0 = cw_state_hash(s0);
  CHECK_EQ_INT(cw_state_node_count(s0), 5);

  /* mutate everything a snapshot covers */
  cw_node2d_set_position(a, cw_v2(9, 9));
  cw_node_set(a, "visible_count", cw_val_int(4));
  ((counter *)cw_node_data(a, "TestCounter"))->hidden = 7;
  cw_node_free(b); /* and C with it */
  cw_node d = cw_node_new("Node2D", "D");
  cw_node_add_child(world, d);
  cw_node_move_child(world, d, 0);
  cw_rng_u32(cw_tree_rng(t));
  cw_tree_fixed_update(t, DT);
  cw_state_save(s1, t);
  CHECK(cw_state_hash(s1) != h0);

  cw_state_load(s0, t);
  cw_state_save(s1, t);
  CHECK_EQ_INT(cw_state_hash(s1), h0);
  CHECK_EQ_INT(cw_tree_node_count(t), 5);
  CHECK(!cw_node_valid(d));
  cw_node b2 = cw_node_get(world, "B"), c2 = cw_node_get(world, "B/C");
  CHECK(cw_node_valid(b2) && cw_node_valid(c2));
  CHECK_EQ_INT(cw_node_uid(b2), uid_b);
  CHECK_EQ_INT(cw_node_uid(c2), uid_c);
  CHECK_EQ_INT(cw_node_index(cw_node_get(world, "A")), 0);
  CHECK(cw_node2d_position(c2).x == 5 && cw_node2d_position(a).y == 2);
  cw_value v;
  cw_node_get_prop(a, "visible_count", &v);
  CHECK_EQ_INT(v.i, 3);
  CHECK_EQ_INT(((counter *)cw_node_data(a, "TestCounter"))->hidden, 42);
  /* the hash ignores uids and names: a second tree built the same way agrees */
  cw_tree *t2 = cw_tree_new();
  cw_tree_seed(t2, 7);
  cw_node w2 = cw_node_new("Node2D", "Elsewhere");
  cw_node_add_child(cw_tree_root(t2), w2);
  cw_node a2 = cw_node_new("TestCounter", "A"), bb = cw_node_new("Node2D", "B"), cc = cw_node_new("Node2D", "C");
  cw_node_add_child(w2, a2);
  cw_node_add_child(w2, bb);
  cw_node_add_child(bb, cc);
  cw_node2d_set_position(a2, cw_v2(1, 2));
  cw_node2d_set_position(cc, cw_v2(5, 6));
  cw_node_set(a2, "visible_count", cw_val_int(3));
  ((counter *)cw_node_data(a2, "TestCounter"))->hidden = 42;
  cw_state_save(s1, t2);
  if (cw_state_hash(s1) != h0 && getenv("CW_NET_DUMP")) {
    static char da[1 << 16], db[1 << 16];
    cw_state_dump(s0, da, sizeof da);
    cw_state_dump(s1, db, sizeof db);
    fprintf(stderr, "=== A\n%s=== B\n%s", da, db);
  }
  CHECK_EQ_INT(cw_state_hash(s1), h0);
  FOLD(h0);
  cw_state_free(s0);
  cw_state_free(s1);
  cw_tree_free(t);
  cw_tree_free(t2);
}

/* ── RPC, NetSynchronizer, NetSpawner ──────────────────────────────── */
static int hits, hit_sender = -1, hit_value;
static bool m_hit(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)self; (void)ret; (void)user;
  hits++;
  hit_sender = cw_net_rpc_sender();
  hit_value = argc > 0 ? (int)args[0].i : -1;
  return true;
}

static cw_tree *make_world(cw_net_session *s) {
  cw_tree *t = cw_tree_new();
  cw_node world = cw_node_new("Node2D", "World");
  cw_node_add_child(cw_tree_root(t), world);
  cw_node sp = cw_node_new("NetSpawner", "Spawner");
  cw_node_set(sp, "spawnable", cw_val_string("test/mover.cwscene"));
  cw_node_set(sp, "spawn_properties", cw_val_string("position"));
  cw_node_add_child(world, sp);
  cw_net_attach(s, t, (cw_node){ 0 });
  return t;
}

static void test_replication(void) {
  cw_net_register_types();
  cw__scene_cache_put("test/mover.cwscene", cw_scene_parse("[scene format=1]\n"
                                                           "[node name=\"Mover\" type=\"Node2D\"]\n"
                                                           "[node name=\"Sync\" parent=\".\" type=\"NetSynchronizer\"]\n"
                                                           "properties = \"position, rotation\"\n",
                                                           "test/mover.cwscene"));
  cw_node_type_add_method("Node2D", "hit", "int", m_hit, NULL, "test RPC");
  cw_node_type_add_method("Node2D", "paint", "", m_hit, NULL, "authority-only test RPC");
  cw_net_rpc_config("Node2D", "hit", CW_RPC_ANY_PEER, CW_NET_RELIABLE, false);
  cw_net_rpc_config("Node2D", "paint", CW_RPC_AUTHORITY, CW_NET_RELIABLE, false);
  cw_net_loopback *hub = cw_net_loopback_new();
  cw_net_session *hs = cw_net_session_new(&(cw_net_session_desc){ lossy(hub, "host", 3, 0.2f), 0, 0xC0DE, 0 });
  cw_net_session *cs = cw_net_session_new(&(cw_net_session_desc){ lossy(hub, "client", 4, 0.2f), 1, 0xC0DE, 0 });
  cw_tree *ht = make_world(hs), *ct = make_world(cs);
  cw_node hworld = cw_node_get(cw_tree_root(ht), "World"), cworld = cw_node_get(cw_tree_root(ct), "World");
  cw_net_connect(cs, "loop:host");
  double now = 0;
  int phase = 0;
  bool paint_refused = false;
  hits = 0;
  for (int step = 0; step < 1200; step++, now += DT) {
    if (phase == 0 && cw_net_peer_connected(hs, 1)) {
      for (int i = 0; i < 3; i++) {
        cw_node m = cw_scene_instance_linked("test/mover.cwscene");
        char name[8];
        snprintf(name, sizeof name, "M%d", i);
        cw_node_set_name(m, name);
        cw_node2d_set_position(m, cw_v2((float)(10 * i), 0));
        cw_node_add_child(hworld, m);
        if (i == 2) cw_net_set_authority(m, 1); /* the client drives M2 */
      }
      phase = 1;
    }
    /* each side moves what it has authority over */
    for (int i = 0; i < 3; i++) {
      char name[8];
      snprintf(name, sizeof name, "M%d", i);
      cw_node hm = cw_node_get(hworld, name), cm = cw_node_get(cworld, name);
      if (cw_node_valid(hm) && cw_net_is_authority(hm)) cw_node2d_set_position(hm, cw_v2((float)(10 * i), (float)step));
      if (cw_node_valid(cm) && cw_net_is_authority(cm)) cw_node2d_set_position(cm, cw_v2(-1, (float)step * 2));
    }
    if (step == 600) {
      cw_node cm0 = cw_node_get(cworld, "M0");
      CHECK(cw_node_valid(cm0));
      CHECK(cw_net_rpc(cm0, "hit", &(cw_value){ .type = CW_T_INT, .i = 7 }, 1));
      paint_refused = !cw_net_rpc(cm0, "paint", NULL, 0); /* the client is not M0's authority */
      /* M5.3 finding: a call to a player that is not connected is not "sent" */
      CHECK(!cw_net_rpc_to(cm0, 3, "hit", &(cw_value){ .type = CW_T_INT, .i = 1 }, 1));
      cw_node_free(cw_node_get(hworld, "M1"));
    }
    cw_net_poll(hs, now);
    cw_net_poll(cs, now);
  }
  CHECK(paint_refused);
  CHECK_EQ_INT(hits, 1);
  CHECK_EQ_INT(hit_sender, 1);
  CHECK_EQ_INT(hit_value, 7);
  cw_node hm0 = cw_node_get(hworld, "M0"), cm0 = cw_node_get(cworld, "M0");
  cw_node hm2 = cw_node_get(hworld, "M2"), cm2 = cw_node_get(cworld, "M2");
  CHECK(cw_node_valid(cm0) && cw_node_valid(cm2));
  CHECK(!cw_node_valid(cw_node_get(cworld, "M1"))); /* despawned everywhere */
  CHECK_EQ_INT(cw_net_authority(cm2), 1);
  CHECK_EQ_INT(cw_net_authority(cm0), 0);
  /* replicated within the last few updates (the network is 50-80 ms one way) */
  float lag0 = cw_node2d_position(hm0).y - cw_node2d_position(cm0).y;
  float lag2 = cw_node2d_position(cm2).y - cw_node2d_position(hm2).y;
  CHECK(lag0 >= 0 && lag0 < 12);
  CHECK(lag2 >= 0 && lag2 < 24);
  CHECK(cw_node2d_position(hm2).x == -1);
  cw_info("net replication: M0 lag %.0f ticks, M2 lag %.0f ticks", lag0, lag2 / 2);
  FOLD(lag0);
  FOLD(lag2);
  cw_net_session_free(hs);
  cw_net_session_free(cs);
  cw_tree_free(ht);
  cw_tree_free(ct);
  cw_net_loopback_free(hub);
}

/* ── rollback ──────────────────────────────────────────────────────── */
typedef struct rbm { cw_vec2 vel; int player; } rbm;
static const cw_prop rbm_props[] = {
  CW_PROP_FIELD(rbm, vel, CW_T_VEC2, CW_PROP_DEFAULT, "velocity"),
  CW_PROP_FIELD(rbm, player, CW_T_INT, CW_PROP_DEFAULT, "input slot"),
  CW_PROP_END,
};
static void rbm_fixed(cw_node n, void *data, double dt) {
  rbm *m = data;
  const uint8_t *in = cw_rollback_input(cw_node_tree(n), m->player);
  float ax = (float)((in[0] & 1) - ((in[0] >> 1) & 1)), ay = (float)(((in[0] >> 2) & 1) - ((in[0] >> 3) & 1));
  m->vel.x = m->vel.x * 0.95f + ax * 0.5f;
  m->vel.y = m->vel.y * 0.95f + ay * 0.5f;
  if (in[0] & 16) m->vel.x += cw_rng_float(cw_tree_rng(cw_node_tree(n))) - 0.5f; /* tree randomness rolls back too */
  cw_vec2 p = cw_node2d_position(n);
  cw_node2d_set_position(n, cw_v2(p.x + m->vel.x * (float)dt * 60, p.y + m->vel.y * (float)dt * 60));
}
static cw_node_type rbm_type = { .name = "TestRBMover", .parent = "Node2D", .data_size = sizeof(rbm), .props = rbm_props,
                                 .fixed_update = rbm_fixed };
static void rb_sample(cw_rollback *rb, int player, void *out, void *user) {
  (void)user;
  uint64_t t = cw_rollback_tick(rb);
  /* a bot that changes its mind every 7..19 ticks: plenty of mispredictions */
  uint32_t h = (uint32_t)((t / (uint64_t)(7 + player * 12)) * 2654435761u + (uint32_t)player * 40503u);
  ((uint8_t *)out)[0] = (uint8_t)((h >> 13) & 31);
}

typedef struct rb_run { uint32_t hash_a, hash_b; uint32_t desyncs, rollbacks, resim, checked; } rb_run;
static rb_run run_rollback(bool no_rollback) {
  if (!cw_node_type_find("TestRBMover")) cw_node_type_register(&rbm_type);
  cw_net_loopback *hub = cw_net_loopback_new();
  cw_net_session *s[2];
  cw_tree *t[2];
  cw_rollback *rb[2];
  for (int i = 0; i < 2; i++) {
    cw_net_sim_desc sd = { .latency = 0.06, .jitter = 0.04, .loss = 0.15f, .seed = 10u + (uint64_t)i };
    s[i] = cw_net_session_new(&(cw_net_session_desc){ cw_net_sim_new(cw_net_loopback_endpoint(hub, i ? "p1" : "p0"), &sd), i, 0xAB, 0 });
    t[i] = cw_tree_new();
    cw_tree_seed(t[i], 99);
    for (int p = 0; p < 2; p++) {
      cw_node m = cw_node_new("TestRBMover", p ? "P1" : "P0");
      cw_node_set(m, "player", cw_val_int(p));
      cw_node_add_child(cw_tree_root(t[i]), m);
    }
    rb[i] = cw_rollback_new(&(cw_rollback_desc){ .session = s[i], .tree = t[i], .players = 2, .input_size = 1, .input_delay = 2,
                                                 .max_prediction = 8, .hash_interval = 5, .dt = DT, .sample_input = rb_sample,
                                                 .no_rollback = no_rollback });
  }
  cw_net_connect(s[1], "loop:p0");
  double now = 0;
  for (int step = 0; step < 700; step++, now += DT) {
    cw_rollback_advance(rb[0], now);
    cw_rollback_advance(rb[1], now);
  }
  rb_run r = { 0 };
  uint64_t probe = 500;
  CHECK(cw_rollback_confirmed_tick(rb[0]) > probe && cw_rollback_confirmed_tick(rb[1]) > probe);
  cw_rollback_state_hash(rb[0], probe, &r.hash_a);
  cw_rollback_state_hash(rb[1], probe, &r.hash_b);
  for (int i = 0; i < 2; i++) {
    cw_rollback_stats st = cw_rollback_get_stats(rb[i]);
    r.desyncs += st.desyncs;
    r.rollbacks += st.rollbacks;
    r.resim += st.resim_ticks;
    r.checked += st.hashes_checked;
  }
  for (int i = 0; i < 2; i++) {
    cw_rollback_free(rb[i]);
    cw_net_session_free(s[i]);
    cw_tree_free(t[i]);
  }
  cw_net_loopback_free(hub);
  return r;
}

static void test_rollback(void) {
  rb_run ok = run_rollback(false);
  CHECK_EQ_INT(ok.hash_a, ok.hash_b);
  CHECK_EQ_INT(ok.desyncs, 0);
  CHECK(ok.rollbacks > 10);
  CHECK(ok.checked > 100);
  /* the control: the same network without corrections must diverge, and the
   * in-band check must say so */
  rb_run bad = run_rollback(true);
  CHECK(bad.hash_a != bad.hash_b);
  CHECK(bad.desyncs > 0);
  cw_info("net rollback: %u rollbacks, %u ticks re-simulated, %u hashes compared, 0 desyncs; without rollback %u desyncs",
          ok.rollbacks, ok.resim, ok.checked, bad.desyncs);
  FOLD(ok);
}

/* rollback through the physics nodes: CharacterBody2D movers (move_and_slide,
 * query-based) against StaticBody2D ground and walls, touching each other */
typedef struct rbw { int player; } rbw;
static const cw_prop rbw_props[] = { CW_PROP_FIELD(rbw, player, CW_T_INT, CW_PROP_DEFAULT, "input slot"), CW_PROP_END };
static void rbw_fixed(cw_node n, void *data, double dt) {
  rbw *w = data;
  const uint8_t *in = cw_rollback_input(cw_node_tree(n), w->player);
  cw_value v, fl;
  cw_node_get_prop(n, "velocity", &v);
  cw_node_get_prop(n, "on_floor", &fl);
  float vx = (float)((in[0] & 1) - ((in[0] >> 1) & 1)) * 140.0f, vy = v.v[1] + 900.0f * (float)dt;
  if ((in[0] & 4) && fl.b) vy = -330;
  cw_node_set(n, "velocity", cw_val_vec2(vx, vy));
  cw_character2d_move_and_slide(n, (float)dt);
}
static cw_node_type rbw_type = { .name = "TestRBWalker", .parent = "CharacterBody2D", .data_size = sizeof(rbw), .props = rbw_props,
                                 .fixed_update = rbw_fixed };
static cw_node rb_shape(cw_node body, int kind, float w, float h) {
  cw_node s = cw_node_new("CollisionShape2D", "Shape");
  cw_value v = { CW_T_ENUM };
  v.i = kind;
  cw_node_set(s, "shape", v);
  cw_node_set(s, "size", cw_val_vec2(w, h));
  cw_node_set(s, "radius", cw_val_float(w * 0.5));
  cw_node_add_child(body, s);
  return s;
}
static cw_tree *phys_world(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  const float walls[3][4] = { { 200, 200, 1000, 20 }, { -60, 120, 20, 200 }, { 460, 120, 20, 200 } };
  for (int i = 0; i < 3; i++) {
    char name[8];
    snprintf(name, sizeof name, "W%d", i);
    cw_node g = cw_node_new("StaticBody2D", name);
    cw_node2d_set_position(g, cw_v2(walls[i][0], walls[i][1]));
    rb_shape(g, 0, walls[i][2], walls[i][3]);
    cw_node_add_child(root, g);
  }
  for (int p = 0; p < 2; p++) {
    cw_node c = cw_node_new("TestRBWalker", p ? "P1" : "P0");
    cw_node_set(c, "player", cw_val_int(p));
    cw_node2d_set_position(c, cw_v2(p ? 300.0f : 100.0f, 150));
    rb_shape(c, 1, 16, 16); /* circles */
    cw_node_add_child(root, c);
  }
  return t;
}
static void rbw_sample(cw_rollback *rb, int player, void *out, void *user) {
  (void)user;
  uint64_t t = cw_rollback_tick(rb);
  uint32_t h = (uint32_t)((t / (uint64_t)(11 + player * 6)) * 2654435761u + (uint32_t)player * 40503u);
  h ^= h >> 15;
  ((uint8_t *)out)[0] = (uint8_t)((h & 3) == 3 ? (h & 1) : (h & 3)) | (uint8_t)(((h >> 7) % 5 == 0) << 2);
}
static void rbw3_fixed(cw_node n, void *data, double dt) {
  rbw *w = data;
  const uint8_t *in = cw_rollback_input(cw_node_tree(n), w->player);
  cw_value v, fl;
  cw_node_get_prop(n, "velocity", &v);
  cw_node_get_prop(n, "on_floor", &fl);
  float vx = (float)((in[0] & 1) - ((in[0] >> 1) & 1)) * 4.0f, vy = v.v[1] - 20.0f * (float)dt;
  if ((in[0] & 4) && fl.b) vy = 7;
  cw_node_set(n, "velocity", cw_val_vec3(vx, vy, (in[0] & 8) ? 2.0f : -1.0f));
  cw_character3d_move_and_slide(n, (float)dt);
}
static cw_node_type rbw3_type = { .name = "TestRBWalker3", .parent = "CharacterBody3D", .data_size = sizeof(rbw), .props = rbw_props,
                                  .fixed_update = rbw3_fixed };
static cw_node rb_shape3(cw_node body, int kind, float x, float y, float z) {
  cw_node s = cw_node_new("CollisionShape3D", "Shape");
  cw_value v = { CW_T_ENUM };
  v.i = kind;
  cw_node_set(s, "shape", v);
  cw_node_set(s, "size", cw_val_vec3(x, y, z));
  cw_node_set(s, "radius", cw_val_float(x * 0.5));
  cw_node_set(s, "height", cw_val_float(y));
  cw_node_add_child(body, s);
  return s;
}
static cw_tree *phys_world3(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  const float walls[5][6] = { { 0, -0.5f, 0, 40, 1, 40 }, { -6, 1, 0, 1, 4, 40 }, { 6, 1, 0, 1, 4, 40 }, { 0, 1, -6, 40, 4, 1 }, { 0, 1, 6, 40, 4, 1 } };
  for (int i = 0; i < 5; i++) {
    char name[8];
    snprintf(name, sizeof name, "W%d", i);
    cw_node g = cw_node_new("StaticBody3D", name);
    cw_node3d_set_position(g, (t3_vec3){ walls[i][0], walls[i][1], walls[i][2] });
    rb_shape3(g, 0, walls[i][3], walls[i][4], walls[i][5]);
    cw_node_add_child(root, g);
  }
  for (int p = 0; p < 2; p++) {
    cw_node c = cw_node_new("TestRBWalker3", p ? "P1" : "P0");
    cw_node_set(c, "player", cw_val_int(p));
    cw_node3d_set_position(c, (t3_vec3){ p ? 2.0f : -2.0f, 1, 0 });
    rb_shape3(c, 2, 0.6f, 1.8f, 0.6f); /* capsules */
    cw_node_add_child(root, c);
  }
  return t;
}
static rb_run run_phys(bool no_rollback, bool three) {
  if (!cw_node_type_find("TestRBWalker")) cw_node_type_register(&rbw_type);
  if (!cw_node_type_find("TestRBWalker3")) cw_node_type_register(&rbw3_type);
  cw_net_loopback *hub = cw_net_loopback_new();
  cw_net_session *s[2];
  cw_tree *t[2];
  cw_rollback *rb[2];
  for (int i = 0; i < 2; i++) {
    cw_net_sim_desc sd = { .latency = 0.07, .jitter = 0.03, .loss = 0.1f, .seed = 20u + (uint64_t)i };
    s[i] = cw_net_session_new(&(cw_net_session_desc){ cw_net_sim_new(cw_net_loopback_endpoint(hub, i ? "p1" : "p0"), &sd), i, 0xAC, 0 });
    t[i] = three ? phys_world3() : phys_world();
    rb[i] = cw_rollback_new(&(cw_rollback_desc){ .session = s[i], .tree = t[i], .players = 2, .input_size = 1, .input_delay = 2,
                                                 .max_prediction = 8, .hash_interval = 5, .dt = DT, .sample_input = rbw_sample,
                                                 .no_rollback = no_rollback });
  }
  cw_net_connect(s[1], "loop:p0");
  double now = 0;
  for (int step = 0; step < 600; step++, now += DT) {
    cw_rollback_advance(rb[0], now);
    cw_rollback_advance(rb[1], now);
  }
  rb_run r = { 0 };
  cw_rollback_state_hash(rb[0], 500, &r.hash_a);
  cw_rollback_state_hash(rb[1], 500, &r.hash_b);
  for (int i = 0; i < 2; i++) {
    cw_rollback_stats st = cw_rollback_get_stats(rb[i]);
    r.desyncs += st.desyncs;
    r.rollbacks += st.rollbacks;
    r.resim += st.resim_ticks;
    r.checked += st.hashes_checked;
  }
  for (int i = 0; i < 2; i++) {
    cw_rollback_free(rb[i]);
    cw_net_session_free(s[i]);
    cw_tree_free(t[i]);
  }
  cw_net_loopback_free(hub);
  return r;
}
static void rollback_physics(bool three) {
  rb_run ok = run_phys(false, three);
  CHECK(ok.hash_a != 0);
  CHECK_EQ_INT(ok.hash_a, ok.hash_b);
  CHECK_EQ_INT(ok.desyncs, 0);
  CHECK(ok.rollbacks > 10);
  rb_run bad = run_phys(true, three);
  CHECK(bad.desyncs > 0);
  cw_info("net rollback (CharacterBody%s): %u rollbacks, %u ticks re-simulated, %u hashes compared, %u desyncs; without rollback %u",
          three ? "3D" : "2D", ok.rollbacks, ok.resim, ok.checked, ok.desyncs, bad.desyncs);
  FOLD(ok);
}
static void test_rollback_physics(void) { rollback_physics(false); }
static void test_rollback_physics3d(void) { rollback_physics(true); }

/* M5.3 finding: input_delay 0 means no delay (it used to mean 2) */
typedef struct rbp { int pad; } rbp;
static uint8_t rbp_seen[8];
static void rbp_fixed(cw_node n, void *data, double dt) {
  (void)data, (void)dt;
  cw_rollback *rb = cw_rollback_of(cw_node_tree(n));
  uint64_t t = cw_rollback_tick(rb);
  if (t < 8) rbp_seen[t] = ((const uint8_t *)cw_rollback_input(cw_node_tree(n), 0))[0];
}
static cw_node_type rbp_type = { .name = "TestRBProbe", .parent = "Node", .data_size = sizeof(rbp), .fixed_update = rbp_fixed };
static void rbp_sample(cw_rollback *rb, int player, void *out, void *user) {
  (void)player, (void)user;
  ((uint8_t *)out)[0] = (uint8_t)(cw_rollback_tick(rb) + 1); /* sampled at tick T: 1 + T */
}
static int first_input_tick(int delay) {
  if (!cw_node_type_find("TestRBProbe")) cw_node_type_register(&rbp_type);
  memset(rbp_seen, 0, sizeof rbp_seen);
  cw_tree *t = cw_tree_new();
  cw_node_add_child(cw_tree_root(t), cw_node_new("TestRBProbe", "Probe"));
  cw_rollback *rb = cw_rollback_new(&(cw_rollback_desc){ .tree = t, .players = 1, .input_size = 1, .input_delay = delay,
                                                         .dt = DT, .sample_input = rbp_sample });
  for (int i = 0; i < 6; i++) cw_rollback_advance(rb, i * DT);
  cw_rollback_free(rb);
  cw_tree_free(t);
  for (int k = 0; k < 8; k++)
    if (rbp_seen[k] == 1) return k;
  return -1;
}
static void test_rollback_delay(void) {
  CHECK_EQ_INT(first_input_tick(0), 0);
  CHECK_EQ_INT(first_input_tick(2), 2);
  CHECK_EQ_INT(first_input_tick(-1), 0);
}

CW_TEST_SUITE("net", { "channels", test_channels }, { "state", test_state }, { "replication", test_replication },
              { "rollback", test_rollback }, { "rollback_physics", test_rollback_physics },
              { "rollback_physics3d", test_rollback_physics3d },
              { "rollback_delay", test_rollback_delay })

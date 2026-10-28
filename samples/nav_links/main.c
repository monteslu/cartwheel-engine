/* nav_links: navigation links (cartwheel/nav.h): a jump up a 1.2 m ledge, a
 * jump pad to a 3.5 m deck and a drop back down, declared as
 * NavigationLink3D nodes in the scene file.
 *
 * The level (samples/common/nav_links_level.h: a floor, a ledge walking
 * cannot climb, a deck and a low wall) is built as MeshInstance3D boxes
 * under a NavigationRegion3D that bakes on the first tick with the links
 * under it. Eight agents walk a circuit: floor, ledge, floor, deck, floor.
 * Each crossing is announced by the agent's link_entered signal (link id, tag,
 * landing point) and link_exited; the agent's body is carried along the
 * link's arc, so it hops up the ledge, flies off the pad and drops off the
 * deck. debug_draw shows the navmesh and the links (green arcs: paths can use
 * them).
 *
 * NAV_LINKS_OFF=1 removes the links before the bake (the gate's control):
 * nobody can reach the ledge or the deck.
 *
 * Debug fields (equal on every host):
 *   links.count, links.connected: links baked, and those whose ends both reached the navmesh;
 *   links.jumps, links.pads, links.drops: crossings begun, by tag; links.landed: crossings ended;
 *   links.ledge, links.deck: arrivals standing on the ledge and on the deck; links.arrivals: all arrivals;
 *   links.hash, links.hash_hi: every tick's crowd hash, folded. */
#include <stdint.h>
#include <stdlib.h>

#include "../common/nav_links_level.h"
#include "cartwheel/debug.h"
#include "cartwheel/nav.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

#define AGENTS 8

static const t3_vec3 circuit[4] = { { 8, NAV_LINKS_LEDGE_TOP, -8 }, { 10, 0, 6 }, { -12, NAV_LINKS_DECK_TOP, -9 }, { -5, 0, -9 } };

static struct {
  cw_node region;
  cw_node nav[AGENTS];
  int leg[AGENTS];
  uint64_t hash;
  uint32_t hash_lo, hash_hi;
  int32_t count, connected, jumps, pads, drops, landed, ledge, deck, arrivals;
} G;

/* #region signals */
static void on_link_entered(cw_node agent, const cw_value *args, int argc, void *user) {
  (void)agent, (void)user;
  if (argc < 3) return;
  /* args: the link's id, its tag, where the crossing lands */
  switch (args[1].i) {
  case CW_NAV_LINK_JUMP: G.jumps++; break;
  case CW_NAV_LINK_PAD: G.pads++; break;
  case CW_NAV_LINK_DROP: G.drops++; break;
  default: break;
  }
}

static void on_link_exited(cw_node agent, const cw_value *args, int argc, void *user) {
  (void)agent, (void)args, (void)argc, (void)user;
  G.landed++;
}
/* #endregion */

static void on_reached(cw_node agent, const cw_value *args, int argc, void *user) {
  (void)args, (void)argc;
  int i = (int)(intptr_t)user;
  G.arrivals++;
  /* counted only when it stands up there (an unreachable target ends at the nearest reachable point) */
  float y = cw_node3d_global_position(cw_node_parent(agent)).y;
  if (G.leg[i] == 0 && y > NAV_LINKS_LEDGE_TOP - 0.3f) G.ledge++;
  if (G.leg[i] == 2 && y > NAV_LINKS_DECK_TOP - 0.3f) G.deck++;
  G.leg[i] = (G.leg[i] + 1) % 4;
  t3_vec3 t = circuit[G.leg[i]];
  cw_node_set(agent, "target_position", cw_val_vec3(t.x, t.y, t.z));
}

static void build_level(cw_node region) {
  nav_links_box b[8];
  int n = nav_links_boxes(b, 8);
  static const float tint[4][3] = { { 0.09f, 0.10f, 0.12f }, { 0.55f, 0.42f, 0.28f }, { 0.30f, 0.45f, 0.62f }, { 0.4f, 0.4f, 0.44f } };
  for (int i = 0; i < n; i++) {
    cw_node m = cw_node_new("MeshInstance3D", "Block");
    cw_node_add_child(region, m);
    cw_node3d_set_position(m, b[i].center);
    cw_node_set(m, "size", cw_val_vec3(b[i].half.x * 2, b[i].half.y * 2, b[i].half.z * 2));
    const float *c = tint[i < 4 ? i : 3];
    cw_node_set(m, "color", cw_val_color(c[0], c[1], c[2], 1));
  }
}

static void spawn_agents(cw_node parent) {
  for (int i = 0; i < AGENTS; i++) {
    cw_node body = cw_node_new("Node3D", "Agent");
    cw_node_add_child(parent, body);
    cw_node3d_set_position(body, (t3_vec3){ 6.0f + (float)(i % 4) * 2.5f, 0, 4.0f + (float)(i / 4) * 2.5f });
    cw_node mesh = cw_node_new("MeshInstance3D", "Body");
    cw_node_add_child(body, mesh);
    cw_node_set(mesh, "primitive", cw_val_enum("CYLINDER"));
    cw_node_set(mesh, "size", cw_val_vec3(0.8f, 1.6f, 0.8f));
    cw_node3d_set_position(mesh, (t3_vec3){ 0, 0.8f, 0 });
    float h = (float)i / AGENTS;
    cw_node_set(mesh, "color", cw_val_color(0.4f + 0.6f * h, 0.9f - 0.5f * h, 0.3f + 0.4f * (1 - h), 1));
    /* #region agent */
    cw_node nav = cw_node_new("NavigationAgent3D", "Nav");
    cw_node_add_child(body, nav);
    cw_node_set(nav, "radius", cw_val_float(0.45));
    cw_node_set(nav, "max_speed", cw_val_float(3.5f));
    G.leg[i] = i % 4;
    t3_vec3 t = circuit[G.leg[i]];
    cw_node_set(nav, "target_position", cw_val_vec3(t.x, t.y, t.z));
    cw_connect(nav, "target_reached", (cw_node){ 0 }, NULL, on_reached, (void *)(intptr_t)i, 0);
    cw_connect(nav, "link_entered", (cw_node){ 0 }, NULL, on_link_entered, NULL, 0);
    cw_connect(nav, "link_exited", (cw_node){ 0 }, NULL, on_link_exited, NULL, 0);
    /* #endregion */
    G.nav[i] = nav;
  }
}

static void init(void *u) {
  (void)u;
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_tree_root(t);
  G.region = cw_node_get(root, "Level/Nav");
  const char *off = getenv("NAV_LINKS_OFF");
  if (off && off[0] == '1') {
    const char *names[] = { "LedgeJump", "Pad", "Drop" };
    for (int i = 0; i < 3; i++) cw_node_free(cw_node_get(G.region, names[i]));
  }
  build_level(G.region);
  spawn_agents(cw_node_get(root, "Level/Agents"));
  cw_debug_i32("links.count", &G.count);
  cw_debug_i32("links.connected", &G.connected);
  cw_debug_i32("links.jumps", &G.jumps);
  cw_debug_i32("links.pads", &G.pads);
  cw_debug_i32("links.drops", &G.drops);
  cw_debug_i32("links.landed", &G.landed);
  cw_debug_i32("links.ledge", &G.ledge);
  cw_debug_i32("links.deck", &G.deck);
  cw_debug_i32("links.arrivals", &G.arrivals);
  cw_debug_u32("links.hash", &G.hash_lo);
  cw_debug_u32("links.hash_hi", &G.hash_hi);
}

static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  cw_navmesh nm = cw_nav_region_navmesh(G.region);
  cw_crowd cr = cw_nav_region_crowd(G.region);
  if (!cw_navmesh_valid(nm)) return;
  cw_navmesh_stats s = cw_navmesh_get_stats(nm);
  G.count = s.links;
  G.connected = s.links_connected;
  uint64_t h = cw_crowd_hash(cr);
  G.hash = cw_hash_bytes(&h, sizeof h, G.hash);
  G.hash_lo = (uint32_t)G.hash;
  G.hash_hi = (uint32_t)(G.hash >> 32);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "nav_links", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}

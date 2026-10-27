/* Navigation unit suite (cartwheel/nav.h): navmesh builds (static and
 * dynamic) and their bytes at several job-pool sizes, save and load, path,
 * raycast, nearest and random-point queries, batched and budgeted paths,
 * tile-cache obstacles, and crowds (arrival, separation, ORCA head-on and
 * RVO2's circle, the same per-tick hashes at every pool size). Hashes and
 * numeric results fold into unit.checksum, which the gates compare across
 * hosts and pool sizes. */
#include <math.h>

#include "cartwheel/jobs.h"
#include "cartwheel/nav.h"
#include "../../samples/common/nav_level.h"
#include "test.h"

static void fold64(uint64_t v) { FOLD(v); }

static cw_nav_geom level(void) {
  cw_nav_geom g = { 0 };
  nav_level_geom(&g);
  return g;
}

/* Natively the suite rebuilds and reruns at several job-pool sizes and checks
 * the results are equal. A cart keeps the pool its host gave it (a browser's
 * main thread cannot join threads); the gates run carts at several sizes
 * instead (CW_WORKERS on every host). */
#if CW_CART
static int pool_sizes[] = { -1 };
static void set_workers(int n) { (void)n; }
#else
static int pool_sizes[] = { 0, 1, 3, 7 };
static void set_workers(int n) {
  cw_jobs_shutdown();
  cw_jobs_init(n);
}
#endif
static int restore_workers;

static void test_build(void) {
  cw_nav_geom g = level();
  cw_nav_config c = cw_nav_config_default();
  uint64_t first = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(pool_sizes); i++) {
    set_workers(pool_sizes[i]);
    cw_navmesh nm = cw_navmesh_build(&c, &g);
    CHECK(cw_navmesh_valid(nm));
    uint64_t h = cw_navmesh_hash(nm);
    size_t len = 0;
    void *bytes = cw_navmesh_save(nm, &len);
    uint64_t hb = cw_hash_bytes(bytes, len, 0);
    if (i == 0) {
      first = h ^ hb;
      cw_navmesh_stats s = cw_navmesh_get_stats(nm);
      CHECK(s.tiles > 1); /* the level spans several tiles */
      CHECK(s.polys > 20);
      FOLD(s.tiles);
      FOLD(s.polys);
      FOLD(s.verts);
      fold64(h);
      fold64(hb);
      cw_info("nav: %d x %d tiles, %d with polygons, %d polys, %lld bytes, %.1f ms", s.tiles_x, s.tiles_z, s.tiles,
              s.polys, (long long)s.bytes, (double)s.build_ms);
    }
    CHECK(first == (h ^ hb)); /* same bytes at every pool size */
    /* save / load round trip: the same tiles and the same bytes again */
    cw_navmesh nm2 = cw_navmesh_load(bytes, len);
    CHECK(cw_navmesh_valid(nm2));
    CHECK(cw_navmesh_hash(nm2) == h);
    size_t len2 = 0;
    void *bytes2 = cw_navmesh_save(nm2, &len2);
    CHECK(len2 == len && !memcmp(bytes, bytes2, len));
    cw_free(bytes2);
    cw_free(bytes);
    cw_navmesh_free(nm2);
    cw_navmesh_free(nm);
  }
  /* one tile over everything (tile_size 0) and the other partitions */
  set_workers(restore_workers);
  cw_nav_partition parts[] = { CW_NAV_PARTITION_WATERSHED, CW_NAV_PARTITION_MONOTONE, CW_NAV_PARTITION_LAYERS };
  for (size_t i = 0; i < CW_ARRAY_LEN(parts); i++) {
    cw_nav_config c1 = c;
    c1.tile_size = i == 0 ? 0 : 32;
    c1.partition = parts[i];
    cw_navmesh nm = cw_navmesh_build(&c1, &g);
    CHECK(cw_navmesh_valid(nm));
    cw_navmesh_stats s = cw_navmesh_get_stats(nm);
    CHECK(s.polys > 10);
    if (i == 0) CHECK_EQ_INT(s.tiles_x * s.tiles_z, 1);
    fold64(cw_navmesh_hash(nm));
    cw_navmesh_free(nm);
  }
  /* bad input is refused */
  cw_nav_geom empty = { 0 };
  CHECK(!cw_navmesh_valid(cw_navmesh_build(&c, &empty)));
  CHECK(!cw_navmesh_valid(cw_navmesh_load("junk", 4)));
  cw_nav_geom_free(&g);
}

static void test_queries(void) {
  cw_nav_geom g = level();
  cw_nav_config c = cw_nav_config_default();
  cw_navmesh nm = cw_navmesh_build(&c, &g);
  /* across both walls, through a doorway */
  t3_vec3 a = { -25, 0, -25 }, b = { 25, 0, 0 };
  t3_vec3 pts[64];
  bool partial = true;
  int n = cw_nav_find_path(nm, a, b, NULL, pts, 64, &partial);
  CHECK(n >= 3);
  CHECK(!partial);
  CHECK(fabsf(pts[n - 1].x - b.x) < 0.5f && fabsf(pts[n - 1].z - b.z) < 0.5f);
  float len = 0;
  for (int i = 0; i + 1 < n; i++) {
    float dx = pts[i + 1].x - pts[i].x, dz = pts[i + 1].z - pts[i].z;
    len += sqrtf(dx * dx + dz * dz);
  }
  CHECK(len > 50 && len < 80); /* longer than the straight line (51 m): around the walls */
  for (int i = 0; i < n; i++) FOLD(pts[i]);
  /* up the ramp onto the deck */
  n = cw_nav_find_path(nm, (t3_vec3){ 24, 0, 5 }, (t3_vec3){ 24, 1.5f, 25 }, NULL, pts, 64, &partial);
  CHECK(n >= 2 && !partial);
  CHECK(n >= 2 && pts[n - 1].y > 1.2f);
  /* a wall stops a ray */
  cw_nav_hit hit;
  CHECK(cw_nav_raycast(nm, (t3_vec3){ -20, 0, -25 }, (t3_vec3){ 0, 0, -25 }, NULL, &hit));
  CHECK(hit.hit && hit.t < 1 && hit.point.x < -10);
  FOLD(hit.t);
  FOLD(hit.point);
  FOLD(hit.normal);
  CHECK(cw_nav_raycast(nm, (t3_vec3){ -20, 0, -20 }, (t3_vec3){ -15, 0, -20 }, NULL, &hit));
  CHECK(!hit.hit && hit.t == 1);
  /* nearest: inside a pillar snaps out of it; far away fails */
  t3_vec3 p;
  CHECK(cw_nav_nearest(nm, (t3_vec3){ -6, 0, -25 }, NULL, &p));
  CHECK(fabsf(p.x + 6) > 0.6f || fabsf(p.z + 25) > 0.6f);
  CHECK(!cw_nav_nearest(nm, (t3_vec3){ 500, 0, 500 }, NULL, &p));
  float y;
  CHECK(cw_nav_height(nm, (t3_vec3){ 24, 1.4f, 24 }, &y) && fabsf(y - 1.5f) < 0.3f);
  /* excluding an area: nothing is walkable */
  cw_nav_filter f = cw_nav_filter_default();
  f.exclude_areas = 1ull << CW_NAV_AREA_WALK;
  CHECK(cw_nav_find_path(nm, a, b, &f, pts, 64, NULL) == 0);
  /* 1000 seeded queries: one hash */
  cw_rng rng;
  cw_rng_seed(&rng, 1234);
  uint32_t qh = CW_HASH32_INIT;
  int found = 0;
  for (int i = 0; i < 1000; i++) {
    t3_vec3 s, e;
    CHECK(cw_nav_random_point(nm, NULL, &rng, &s));
    if (!cw_nav_random_point_near(nm, s, 15, NULL, &rng, &e)) e = s;
    int k = cw_nav_find_path(nm, s, e, NULL, pts, 64, NULL);
    found += k > 0;
    qh = cw_hash32(&s, sizeof s, qh);
    qh = cw_hash32(&e, sizeof e, qh);
    qh = cw_hash32(pts, sizeof(t3_vec3) * (size_t)k, qh);
    if (cw_nav_raycast(nm, s, e, NULL, &hit)) {
      qh = cw_hash32(&hit.t, sizeof hit.t, qh);
      qh = cw_hash32(&hit.point, sizeof hit.point, qh);
    }
  }
  CHECK(found > 990);
  FOLD(qh);
  cw_info("nav: 1000-query hash %08x", qh);
  /* batched paths: the same answers as one by one */
  enum { NB = 64 };
  cw_nav_path_req reqs[NB];
  cw_rng_seed(&rng, 99);
  for (int i = 0; i < NB; i++) {
    cw_nav_random_point(nm, NULL, &rng, &reqs[i].from);
    cw_nav_random_point(nm, NULL, &rng, &reqs[i].to);
    reqs[i].filter = NULL;
  }
  static t3_vec3 bpts[NB * 32];
  int counts[NB];
  cw_nav_find_paths(nm, reqs, NB, bpts, 32, counts);
  int same = 0;
  for (int i = 0; i < NB; i++) {
    int k = cw_nav_find_path(nm, reqs[i].from, reqs[i].to, NULL, pts, 32, NULL);
    same += k == counts[i] && !memcmp(pts, &bpts[i * 32], sizeof(t3_vec3) * (size_t)k);
  }
  CHECK_EQ_INT(same, NB);
  /* the budget: 3 requests a tick, oldest first */
  uint32_t tickets[10];
  for (int i = 0; i < 10; i++) tickets[i] = cw_nav_request_path(nm, reqs[i].from, reqs[i].to, NULL);
  CHECK_EQ_INT(cw_nav_pending_paths(nm), 10);
  cw_nav_tick(nm, 3, 0);
  CHECK_EQ_INT(cw_nav_pending_paths(nm), 7);
  int cnt;
  CHECK(cw_nav_take_path(nm, tickets[0], pts, 64, &cnt) == CW_NAV_PATH_DONE);
  CHECK(cnt == counts[0] && !memcmp(pts, &bpts[0], sizeof(t3_vec3) * (size_t)cnt));
  CHECK(cw_nav_take_path(nm, tickets[0], pts, 64, &cnt) == CW_NAV_PATH_UNKNOWN);
  CHECK(cw_nav_take_path(nm, tickets[5], pts, 64, &cnt) == CW_NAV_PATH_PENDING);
  /* a tight iteration budget gives partial paths across the level */
  cw_nav_tick(nm, 0, 4);
  CHECK_EQ_INT(cw_nav_pending_paths(nm), 0);
  uint32_t t = cw_nav_request_path(nm, a, b, NULL);
  cw_nav_tick(nm, 1, 3);
  cw_nav_path_status st = cw_nav_take_path(nm, t, pts, 64, &cnt);
  CHECK(st == CW_NAV_PATH_PARTIAL);
  for (int i = 1; i < 10; i++) cw_nav_cancel_path(nm, tickets[i]);
  CHECK(cw_nav_take_path(nm, tickets[5], pts, 64, &cnt) == CW_NAV_PATH_UNKNOWN);
  cw_navmesh_free(nm);
  cw_nav_geom_free(&g);
}

static void test_obstacles(void) {
  cw_nav_geom g = level();
  cw_nav_config c = cw_nav_config_default();
  c.dynamic = true;
  uint64_t h0 = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(pool_sizes); i++) {
    set_workers(pool_sizes[i]);
    cw_navmesh nm = cw_navmesh_build(&c, &g);
    CHECK(cw_navmesh_valid(nm));
    cw_navmesh_stats s0 = cw_navmesh_get_stats(nm);
    CHECK(s0.polys > 20);
    /* block the left wall's two doorways: the path must take the long way, or there is none */
    t3_vec3 a = { -25, 0, -25 }, b = { 0, 0, -6 }, pts[64];
    int n0 = cw_nav_find_path(nm, a, b, NULL, pts, 64, NULL);
    CHECK(n0 > 0);
    uint32_t o1 = cw_nav_obstacle_add_box(nm, (t3_vec3){ -10, 1, -6 }, (t3_vec3){ 1, 1, 4.5f }, 0);
    uint32_t o2 = cw_nav_obstacle_add_cylinder(nm, (t3_vec3){ -10, 0, 14 }, 4.5f, 2);
    CHECK(o1 && o2);
    int rebuilt = cw_nav_update(nm);
    CHECK(rebuilt > 0);
    bool partial = false;
    int n1 = cw_nav_find_path(nm, a, b, NULL, pts, 64, &partial);
    CHECK(n1 > 0 && partial); /* walled off: the closest reachable point */
    uint64_t hb = cw_navmesh_hash(nm);
    /* save keeps the obstacles */
    size_t len;
    void *bytes = cw_navmesh_save(nm, &len);
    cw_navmesh nm2 = cw_navmesh_load(bytes, len);
    CHECK(cw_navmesh_valid(nm2) && cw_navmesh_get_stats(nm2).obstacles == 2);
    CHECK(cw_nav_find_path(nm2, a, b, NULL, pts, 64, &partial) > 0 && partial);
    cw_free(bytes);
    cw_navmesh_free(nm2);
    /* a rotated box in the open, then removal */
    uint32_t o3 = cw_nav_obstacle_add_box(nm, (t3_vec3){ 0, 1, 0 }, (t3_vec3){ 2, 1, 0.5f }, 0.7f);
    CHECK(o3 != 0);
    cw_nav_update(nm);
    CHECK(cw_nav_obstacle_remove(nm, o1));
    CHECK(cw_nav_obstacle_remove(nm, o3));
    CHECK(!cw_nav_obstacle_remove(nm, o3));
    CHECK(cw_nav_update(nm) > 0);
    int n2 = cw_nav_find_path(nm, a, b, NULL, pts, 64, &partial);
    CHECK(n2 > 0 && !partial);
    uint64_t h = cw_navmesh_hash(nm) ^ hb;
    if (i == 0) {
      h0 = h;
      fold64(h);
    }
    CHECK(h == h0);
    /* a static navmesh refuses obstacles */
    cw_navmesh_free(nm);
  }
  set_workers(restore_workers);
  cw_nav_config sc = cw_nav_config_default();
  cw_navmesh st = cw_navmesh_build(&sc, &g);
  CHECK(cw_nav_obstacle_add_cylinder(st, (t3_vec3){ 0, 0, 0 }, 1, 1) == 0);
  /* a static rebuild of an area from changed geometry */
  cw_nav_geom g2 = level();
  cw_nav_geom_add_box(&g2, (t3_vec3){ -15, 1, 0 }, (t3_vec3){ 3, 1, 3 }, CW_NAV_AREA_WALK);
  uint64_t before = cw_navmesh_hash(st);
  CHECK(cw_navmesh_rebuild_area(st, &g2, (t3_vec3){ -18, 0, -3 }, (t3_vec3){ -12, 2, 3 }));
  CHECK(cw_navmesh_hash(st) != before);
  t3_vec3 p;
  CHECK(cw_nav_nearest(st, (t3_vec3){ -15, 2.1f, 0 }, NULL, &p) && p.y > 1.8f); /* the new block's top */
  cw_navmesh_free(st);
  cw_nav_geom_free(&g2);
  cw_nav_geom_free(&g);
}

/* Two agents walking at each other along one line pass without touching. */
static void test_head_on(void) {
  cw_nav_geom g = { 0 };
  cw_nav_geom_add_box(&g, (t3_vec3){ 0, -0.5f, 0 }, (t3_vec3){ 20, 0.5f, 20 }, CW_NAV_AREA_WALK);
  cw_nav_config c = cw_nav_config_default();
  cw_navmesh nm = cw_navmesh_build(&c, &g);
  cw_crowd cr = cw_crowd_new(nm, 4);
  cw_crowd_agent a = cw_crowd_add(cr, (t3_vec3){ -8, 0, 0 }, NULL);
  cw_crowd_agent b = cw_crowd_add(cr, (t3_vec3){ 8, 0, 0 }, NULL);
  cw_crowd_set_target(cr, a, (t3_vec3){ 8, 0, 0 });
  cw_crowd_set_target(cr, b, (t3_vec3){ -8, 0, 0 });
  float closest = 1e9f;
  for (int i = 0; i < 400; i++) {
    cw_crowd_step(cr, 1.0f / 60);
    t3_vec3 pa = cw_crowd_position(cr, a), pb = cw_crowd_position(cr, b);
    float d = sqrtf((pa.x - pb.x) * (pa.x - pb.x) + (pa.z - pb.z) * (pa.z - pb.z));
    closest = d < closest ? d : closest;
  }
  cw_crowd_agent_state sa, sb;
  CHECK(cw_crowd_get_state(cr, a, &sa) && cw_crowd_get_state(cr, b, &sb));
  CHECK(sa.status == CW_CROWD_ARRIVED && sb.status == CW_CROWD_ARRIVED);
  CHECK(closest > 0.95f); /* radii 0.5 + 0.5, a little numerical slack */
  cw_info("nav: head-on closest approach %.3f m", (double)closest);
  FOLD(closest);
  fold64(cw_crowd_hash(cr));
  cw_navmesh_free(nm); /* frees the crowd too */
  CHECK(!cw_crowd_valid(cr));
  cw_nav_geom_free(&g);
}

/* RVO2's circle benchmark: agents on a circle walk to the opposite point, all
 * through the middle at once. ORCA lets every one of them through. */
static void test_circle(void) {
  cw_nav_geom g = { 0 };
  cw_nav_geom_add_box(&g, (t3_vec3){ 0, -0.5f, 0 }, (t3_vec3){ 40, 0.5f, 40 }, CW_NAV_AREA_WALK);
  cw_nav_config c = cw_nav_config_default();
  c.tile_size = 64;
  cw_navmesh nm = cw_navmesh_build(&c, &g);
  enum { N = 100 };
  cw_crowd cr = cw_crowd_new(nm, N);
  cw_crowd_agent h[N];
  cw_crowd_agent_params ap = cw_crowd_agent_params_default();
  ap.max_speed = 2;
  ap.time_horizon = 5;
  for (int i = 0; i < N; i++) {
    float s, co;
    cw_sincos((float)i * CW_TAU / N, &s, &co);
    h[i] = cw_crowd_add(cr, (t3_vec3){ 25 * co, 0, 25 * s }, &ap);
    cw_crowd_set_target(cr, h[i], (t3_vec3){ -25 * co, 0, -25 * s });
  }
  int t = 0, arrived = 0;
  for (; t < 3600 && arrived < N; t++) {
    cw_crowd_step(cr, 1.0f / 30);
    arrived = 0;
    for (int i = 0; i < N; i++) {
      cw_crowd_agent_state st;
      cw_crowd_get_state(cr, h[i], &st);
      arrived += st.status == CW_CROWD_ARRIVED;
    }
  }
  cw_info("nav: circle of %d: %d arrived after %.1f s", N, arrived, t / 30.0);
  CHECK_EQ_INT(arrived, N);
  FOLD(t);
  fold64(cw_crowd_hash(cr));
  cw_navmesh_free(nm);
  cw_nav_geom_free(&g);
}

/* 300 agents walk across the level for 1200 ticks: per-tick hashes equal at every pool size. */
static uint64_t run_crowd(cw_navmesh nm, int agents, int ticks, int *arrived, float *min_sep) {
  cw_crowd cr = cw_crowd_new(nm, agents);
  cw_crowd_set_plan_budget(cr, 16, 0);
  cw_rng rng;
  cw_rng_seed(&rng, 7);
  cw_crowd_agent *h = cw_alloc(sizeof *h * (size_t)agents, CW_MEM_GAME);
  for (int i = 0; i < agents; i++) {
    /* start on either side of the walls, walk to anywhere on the level */
    float side = (i & 1) ? 1.0f : -1.0f;
    t3_vec3 p = { side * (13 + cw_rng_range(&rng, 0, 16)), 0, cw_rng_range(&rng, -29, 29) };
    h[i] = cw_crowd_add(cr, p, NULL);
    cw_crowd_set_target(cr, h[i], (t3_vec3){ cw_rng_range(&rng, -29, 29), 0, cw_rng_range(&rng, -29, 29) });
  }
  uint64_t x = 0;
  for (int t = 0; t < ticks; t++) {
    cw_crowd_step(cr, 1.0f / 30);
    x = cw_hash_bytes(&(uint64_t){ cw_crowd_hash(cr) }, 8, x);
  }
  *arrived = 0;
  *min_sep = 1e9f;
  for (int i = 0; i < agents; i++) {
    cw_crowd_agent_state s;
    cw_crowd_get_state(cr, h[i], &s);
    *arrived += s.status == CW_CROWD_ARRIVED;
    for (int j = i + 1; j < agents; j++) {
      t3_vec3 a = cw_crowd_position(cr, h[i]), b = cw_crowd_position(cr, h[j]);
      float d = sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
      if (d < *min_sep) *min_sep = d;
    }
  }
  cw_free(h);
  cw_crowd_free(cr);
  return x;
}

static void test_crowd(void) {
  cw_nav_geom g = level();
  cw_nav_config c = cw_nav_config_default();
  cw_navmesh nm = cw_navmesh_build(&c, &g);
  uint64_t first = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(pool_sizes); i++) {
    set_workers(pool_sizes[i]);
    int arrived;
    float sep;
    uint64_t x = run_crowd(nm, 300, 1200, &arrived, &sep);
    if (i == 0) {
      first = x;
      fold64(x);
      FOLD(arrived);
      FOLD(sep);
      cw_info("nav: crowd of 300: %d arrived, closest pair %.3f m, hash %016llx", arrived, (double)sep,
              (unsigned long long)x);
      CHECK(arrived > 250);
      CHECK(sep > 0.5f); /* never on top of each other (radius 0.5 each) */
    }
    CHECK(x == first);
  }
  set_workers(restore_workers);
  /* agents API odds and ends */
  cw_crowd cr = cw_crowd_new(nm, 2);
  cw_crowd_agent a = cw_crowd_add(cr, (t3_vec3){ -25, 0, -25 }, NULL);
  cw_crowd_agent b = cw_crowd_add(cr, (t3_vec3){ -24, 0, -25 }, NULL);
  CHECK(cw_crowd_count(cr) == 2);
  CHECK(CW_HANDLE_IS_NULL(cw_crowd_add(cr, (t3_vec3){ 0, 0, 0 }, NULL))); /* full */
  CHECK(CW_HANDLE_IS_NULL(cw_crowd_add(cr, (t3_vec3){ 900, 0, 0 }, NULL)));
  cw_crowd_remove(cr, b);
  CHECK(!cw_crowd_agent_valid(cr, b));
  CHECK(cw_crowd_count(cr) == 1);
  cw_crowd_set_velocity(cr, a, (t3_vec3){ 2, 0, 0 });
  for (int i = 0; i < 60; i++) cw_crowd_step(cr, 1.0f / 30);
  t3_vec3 p = cw_crowd_position(cr, a);
  CHECK(p.x > -22.5f); /* moved about 4 m (accelerating) */
  cw_crowd_set_velocity(cr, a, (t3_vec3){ 0, 0, -3 }); /* into the floor's edge: the surface stops it */
  for (int i = 0; i < 240; i++) cw_crowd_step(cr, 1.0f / 30);
  p = cw_crowd_position(cr, a);
  CHECK(p.z > -32);
  cw_crowd_set_target(cr, a, (t3_vec3){ 300, 0, 0 }); /* off the navmesh */
  cw_crowd_agent_state s;
  cw_crowd_get_state(cr, a, &s);
  CHECK(s.status == CW_CROWD_NO_PATH);
  cw_crowd_free(cr);
  cw_navmesh_free(nm);
  cw_nav_geom_free(&g);
}

/* A crowd agent handle is 64 bits (generation on top). One kept in 32 bits is never valid: it must be reported
 * loudly by every call, not quietly answered with a zero position; the full handle must not trip the report, and
 * the slot accessor is the safe small number. */
static void test_handles(void) {
  cw_nav_geom g = level();
  cw_nav_config c = cw_nav_config_default();
  cw_navmesh nm = cw_navmesh_build(&c, &g);
  cw_crowd cr = cw_crowd_new(nm, 4);
  cw_crowd_agent a = cw_crowd_add(cr, (t3_vec3){ -25, 0, -25 }, NULL);
  cw_crowd_agent b = cw_crowd_add(cr, (t3_vec3){ -20, 0, -25 }, NULL);
  CHECK(!CW_HANDLE_IS_NULL(a) && !CW_HANDLE_IS_TRUNCATED(a)); /* a live handle always has a generation */
  uint32_t before = cw_handle_truncated_count();
  t3_vec3 p = cw_crowd_position(cr, a);
  CHECK(p.x < -24 && p.x > -26); /* control: the full handle answers and reports nothing */
  CHECK(cw_crowd_agent_valid(cr, a));
  CHECK_EQ_INT(cw_handle_truncated_count(), before);
  CHECK_EQ_INT(cw_crowd_agent_slot(cr, a), 0);
  CHECK_EQ_INT(cw_crowd_agent_slot(cr, b), 1);
  uint32_t cut = (uint32_t)a.id; /* the mistake: the handle kept in 32 bits */
  cw_crowd_agent bad = { cut };
  CHECK(CW_HANDLE_IS_TRUNCATED(bad));
  p = cw_crowd_position(cr, bad);
  CHECK(p.x == 0 && p.z == 0); /* still invalid ... */
  CHECK_EQ_INT(cw_handle_truncated_count(), before + 1); /* ... but no longer silent */
  CHECK(!cw_crowd_agent_valid(cr, bad));
  CHECK_EQ_INT(cw_crowd_agent_slot(cr, bad), -1);
  CHECK_EQ_INT(cw_handle_truncated_count(), before + 3);
  cw_crowd_remove(cr, bad); /* and a cut handle cannot remove anything */
  CHECK(cw_crowd_agent_valid(cr, a));
  CHECK(!CW_HANDLE_IS_TRUNCATED((cw_crowd_agent){ 0 })); /* null is just null */
  cw_crowd_free(cr);
  cw_navmesh_free(nm);
  cw_nav_geom_free(&g);
}

/* The crowd replans around an obstacle dropped on its path. */
static void test_crowd_obstacle(void) {
  cw_nav_geom g = level();
  cw_nav_config c = cw_nav_config_default();
  c.dynamic = true;
  cw_navmesh nm = cw_navmesh_build(&c, &g);
  cw_crowd cr = cw_crowd_new(nm, 1);
  cw_crowd_agent a = cw_crowd_add(cr, (t3_vec3){ -25, 0, 14 }, NULL);
  cw_crowd_set_target(cr, a, (t3_vec3){ 0, 0, 14 });
  for (int i = 0; i < 10; i++) cw_crowd_step(cr, 1.0f / 30);
  cw_crowd_agent_state s0;
  cw_crowd_get_state(cr, a, &s0);
  CHECK(cw_nav_obstacle_add_cylinder(nm, (t3_vec3){ -10, 0, 14 }, 4.5f, 2) != 0); /* plug the doorway it is heading for */
  cw_nav_update(nm);
  for (int i = 0; i < 900; i++) cw_crowd_step(cr, 1.0f / 30);
  cw_crowd_agent_state s1;
  cw_crowd_get_state(cr, a, &s1);
  CHECK(s1.path_version > s0.path_version); /* it replanned */
  CHECK(s1.status == CW_CROWD_ARRIVED);
  CHECK(fabsf(s1.position.x) < 0.5f);
  fold64(cw_crowd_hash(cr));
  cw_navmesh_free(nm);
  cw_nav_geom_free(&g);
}

static void test_geom(void) {
  cw_nav_geom g = { 0 };
  /* a mirrored transform keeps triangles facing up */
  t3_mat4 m;
  t3_mat4_compose(&m, (t3_vec3){ 1, 2, 3 }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ -1, 1, 1 });
  const float v[9] = { 0, 0, 0, 0, 0, 1, 1, 0, 0 };
  cw_nav_geom_add_tris(&g, v, 3, NULL, 1, &m, CW_NAV_AREA_WALK);
  CHECK_EQ_INT(g.tris.count, 3);
  const float *a = &g.verts.data[g.tris.data[0] * 3], *b = &g.verts.data[g.tris.data[1] * 3], *c = &g.verts.data[g.tris.data[2] * 3];
  float ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
  CHECK(ny > 0);
  t3_vec3 mn, mx;
  CHECK(cw_nav_geom_bounds(&g, &mn, &mx) && mn.x == 0 && mx.x == 1 && mn.y == 2);
  cw_nav_geom_clear(&g);
  CHECK(!cw_nav_geom_bounds(&g, NULL, NULL));
  /* a physics world's static shapes, in creation order */
  cw_world3d_def wd = cw_world3d_default_def();
  cw_world3d w = cw_world3d_create(&wd);
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body3d ground = cw_body3d_create(w, &bd);
  cw_shape3d_box(ground, NULL, (t3_vec3){ 10, 0.5f, 10 }, cw_xform3_at(0, -0.5f, 0));
  cw_shape3d_sphere(ground, NULL, (t3_vec3){ 3, 1, 0 }, 1);
  cw_shape3d_capsule(ground, NULL, (t3_vec3){ -3, 0.5f, 0 }, (t3_vec3){ -3, 2, 0 }, 0.5f);
  bd.type = CW_BODY_DYNAMIC;
  cw_body3d box = cw_body3d_create(w, &bd); /* dynamic: not navmesh input */
  cw_shape3d_box(box, NULL, (t3_vec3){ 1, 1, 1 }, cw_xform3_identity());
  int tris = cw_nav_geom_add_world3d(&g, w, NULL, CW_NAV_AREA_WALK);
  CHECK_EQ_INT(tris, 12 + 2 * 28);
  cw_nav_config cfg = cw_nav_config_default();
  cw_navmesh nm = cw_navmesh_build(&cfg, &g);
  CHECK(cw_navmesh_valid(nm));
  t3_vec3 p;
  CHECK(cw_nav_nearest(nm, (t3_vec3){ 3, 0, 0 }, NULL, &p)); /* snapped off the sphere's footprint */
  CHECK(fabsf(p.x - 3) > 0.9f || fabsf(p.z) > 0.9f);
  fold64(cw_navmesh_hash(nm));
  cw_navmesh_free(nm);
  cw_world3d_destroy(w);
  cw_nav_geom_free(&g);
}

/* Building, querying, carving and crowding, then freeing everything, gives
 * back every byte (CW_MEM_NAV): measured after one warm-up round, since the
 * handle pools keep their chunks. */
static void nav_round(void) {
  cw_nav_geom g = level();
  cw_nav_config c = cw_nav_config_default();
  c.dynamic = true;
  cw_navmesh nm = cw_navmesh_build(&c, &g);
  t3_vec3 pts[32];
  cw_nav_find_path(nm, (t3_vec3){ -25, 0, -25 }, (t3_vec3){ 25, 0, 0 }, NULL, pts, 32, NULL);
  uint32_t t = cw_nav_request_path(nm, (t3_vec3){ -25, 0, -25 }, (t3_vec3){ 25, 0, 0 }, NULL);
  cw_nav_tick(nm, 0, 0);
  cw_nav_request_path(nm, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 5, 0, 5 }, NULL); /* left queued: freed with the navmesh */
  cw_nav_take_path(nm, t, pts, 32, NULL);
  cw_nav_obstacle_add_cylinder(nm, (t3_vec3){ 0, 0, 0 }, 2, 2);
  cw_nav_update(nm);
  size_t len;
  void *bytes = cw_navmesh_save(nm, &len);
  cw_navmesh nm2 = cw_navmesh_load(bytes, len);
  cw_free(bytes);
  cw_crowd cr = cw_crowd_new(nm, 32);
  for (int i = 0; i < 32; i++) {
    cw_crowd_agent a = cw_crowd_add(cr, (t3_vec3){ -20.0f + i, 0, -20 }, NULL);
    cw_crowd_set_target(cr, a, (t3_vec3){ 20, 0, 20 });
  }
  for (int i = 0; i < 30; i++) cw_crowd_step(cr, 1.0f / 30);
  cw_phys_lines lines = { 0 };
  cw_nav_debug_draw(nm, &lines, CW_NAV_DRAW_DEFAULT | CW_NAV_DRAW_TILES);
  cw_crowd_debug_draw(cr, &lines, CW_NAV_DRAW_DEFAULT);
  CHECK(cw_phys_lines_count(&lines) > 100);
  cw_phys_lines_free(&lines);
  cw_navmesh_free(nm2);
  cw_navmesh_free(nm); /* and the crowd */
  cw_nav_geom_free(&g);
}

static void test_memory(void) {
  nav_round();
  cw_mem_stats a, b;
  cw_mem_get_stats(&a);
  nav_round();
  cw_mem_get_stats(&b);
  CHECK_EQ_INT(b.bytes[CW_MEM_NAV], a.bytes[CW_MEM_NAV]);
  CHECK_EQ_INT(b.allocs[CW_MEM_NAV], a.allocs[CW_MEM_NAV]);
}

static void test_setup(void) { restore_workers = cw_jobs_thread_count() - 1; }

CW_TEST_SUITE("nav", { "setup", test_setup }, { "geom", test_geom }, { "build", test_build },
              { "queries", test_queries }, { "obstacles", test_obstacles }, { "head_on", test_head_on }, { "circle", test_circle },
              { "crowd", test_crowd }, { "handles", test_handles }, { "crowd_obstacle", test_crowd_obstacle }, { "memory", test_memory })

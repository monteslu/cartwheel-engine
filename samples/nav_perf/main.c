/* nav_perf (tests/nav-perf.mjs): navigation timings, measured on the 30th
 * fixed tick (in a browser a cart's pool threads start only once the page
 * has run a few frames, so init would measure one thread).
 *
 *   - navmesh builds of a 256 x 256 m world (the nav_demo level tiled 4 x 4,
 *     324 tiles), static and dynamic, best of 5;
 *   - 64 obstacles added and applied (the touched tiles rebuild);
 *   - 1000 batched paths across the world;
 *   - crowd steps: 500 agents in the demo level and 2000 in the big world,
 *     walking to random points, mean and worst step over 600 steps after 60.
 *
 * Timings are debug fields; so are the results' hashes, which must be the
 * same at every worker count and on every host. */
#include <stdlib.h>
#include <time.h>

#include "../common/nav_level.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/jobs.h"
#include "cartwheel/nav.h"
#include "cartwheel/platform.h"

static float build_ms, build_dyn_ms, update_ms, paths_ms, step500_ms, step500_max, step2000_ms, step2000_max;
static int32_t threads, tiles, polys, done;
static uint32_t mesh_hash, crowd500_hash, crowd2000_hash, paths_hash;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void world_geom(cw_nav_geom *g, int n) {
  nav_level_box b[64];
  int count = nav_level_boxes(b, 64);
  for (int gz = 0; gz < n; gz++)
    for (int gx = 0; gx < n; gx++)
      for (int i = 0; i < count; i++) {
        t3_vec3 c = { b[i].center.x + (float)(gx * 64 - (n - 1) * 32), b[i].center.y, b[i].center.z + (float)(gz * 64 - (n - 1) * 32) };
        cw_nav_geom_add_box(g, c, b[i].half, CW_NAV_AREA_WALK);
      }
}

static float best_build(const cw_nav_config *c, const cw_nav_geom *g, uint32_t *hash) {
  float best = 1e9f;
  for (int k = 0; k < 5; k++) {
    double t0 = now_ms();
    cw_navmesh nm = cw_navmesh_build(c, g);
    float ms = (float)(now_ms() - t0);
    best = ms < best ? ms : best;
    if (hash) *hash = (uint32_t)cw_navmesh_hash(nm);
    if (k == 0) {
      cw_navmesh_stats s = cw_navmesh_get_stats(nm);
      tiles = s.tiles;
      polys = s.polys;
    }
    cw_navmesh_free(nm);
  }
  return best;
}

static void run_crowd(cw_navmesh nm, int agents, float half, float *mean, float *worst, uint32_t *hash) {
  /* #region crowd */
  cw_crowd cr = cw_crowd_new(nm, agents);
  cw_crowd_set_plan_budget(cr, 32, 0);
  cw_rng rng;
  cw_rng_seed(&rng, 11);
  cw_crowd_agent *h = cw_alloc(sizeof *h * (size_t)agents, CW_MEM_GAME);
  for (int i = 0; i < agents; i++) {
    h[i] = cw_crowd_add(cr, (t3_vec3){ cw_rng_range(&rng, -half, half), 0, cw_rng_range(&rng, -half, half) }, NULL);
    cw_crowd_set_target(cr, h[i], (t3_vec3){ cw_rng_range(&rng, -half, half), 0, cw_rng_range(&rng, -half, half) });
  }
  /* #endregion */
  double sum = 0;
  float mx = 0;
  uint64_t x = 0;
  for (int t = 0; t < 660; t++) {
    /* agents that arrived go on to another point */
    if (t % 30 == 0)
      for (int i = 0; i < agents; i++) {
        cw_crowd_agent_state s;
        if (cw_crowd_get_state(cr, h[i], &s) && s.status == CW_CROWD_ARRIVED)
          cw_crowd_set_target(cr, h[i], (t3_vec3){ cw_rng_range(&rng, -half, half), 0, cw_rng_range(&rng, -half, half) });
      }
    double t0 = now_ms();
    cw_crowd_step(cr, 1.0f / 60);
    float ms = (float)(now_ms() - t0);
    if (t >= 60) {
      sum += ms;
      mx = ms > mx ? ms : mx;
    }
    uint64_t ch = cw_crowd_hash(cr);
    x = cw_hash_bytes(&ch, sizeof ch, x);
  }
  *mean = (float)(sum / 600);
  *worst = mx;
  *hash = (uint32_t)x;
  cw_free(h);
  cw_crowd_free(cr);
}

static void bench(void);

static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  static int tick;
  if (++tick == 30) bench();
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("perf.threads", &threads);
  cw_debug_i32("perf.tiles", &tiles);
  cw_debug_i32("perf.polys", &polys);
  cw_debug_f32("perf.build_ms", &build_ms);
  cw_debug_f32("perf.build_dynamic_ms", &build_dyn_ms);
  cw_debug_f32("perf.update_ms", &update_ms);
  cw_debug_f32("perf.paths_ms", &paths_ms);
  cw_debug_f32("perf.step500_ms", &step500_ms);
  cw_debug_f32("perf.step500_max_ms", &step500_max);
  cw_debug_f32("perf.step2000_ms", &step2000_ms);
  cw_debug_f32("perf.step2000_max_ms", &step2000_max);
  cw_debug_u32("perf.mesh_hash", &mesh_hash);
  cw_debug_u32("perf.paths_hash", &paths_hash);
  cw_debug_u32("perf.crowd500_hash", &crowd500_hash);
  cw_debug_u32("perf.crowd2000_hash", &crowd2000_hash);
  cw_debug_i32("perf.done", &done);
}

static void bench(void) {
  threads = cw_jobs_thread_count();

  /* #region build */
  cw_nav_geom big = { 0 }, small = { 0 };
  world_geom(&big, 4);
  world_geom(&small, 1);
  cw_nav_config c = cw_nav_config_default();
  /* #endregion */
  build_ms = best_build(&c, &big, &mesh_hash);
  int static_tiles = tiles, static_polys = polys;
  cw_nav_config cd = c;
  cd.dynamic = true;
  build_dyn_ms = best_build(&cd, &big, NULL);
  tiles = static_tiles;
  polys = static_polys;

  /* 64 obstacles across the dynamic world, applied in one update */
  cw_navmesh dyn = cw_navmesh_build(&cd, &big);
  cw_rng rng;
  cw_rng_seed(&rng, 5);
  for (int i = 0; i < 64; i++)
    cw_nav_obstacle_add_cylinder(dyn, (t3_vec3){ cw_rng_range(&rng, -120, 120), 0, cw_rng_range(&rng, -120, 120) }, 1.5f, 2);
  double t0 = now_ms();
  cw_nav_update(dyn);
  update_ms = (float)(now_ms() - t0);
  cw_navmesh_free(dyn);

  /* 1000 paths across the static world, batched */
  cw_navmesh nm = cw_navmesh_build(&c, &big);
  enum { NP = 1000, MAXP = 64 };
  cw_nav_path_req *reqs = cw_alloc(sizeof *reqs * NP, CW_MEM_GAME);
  t3_vec3 *pts = cw_alloc(sizeof *pts * NP * MAXP, CW_MEM_GAME);
  int *counts = cw_alloc(sizeof *counts * NP, CW_MEM_GAME);
  for (int i = 0; i < NP; i++) {
    cw_nav_random_point(nm, NULL, &rng, &reqs[i].from);
    cw_nav_random_point(nm, NULL, &rng, &reqs[i].to);
    reqs[i].filter = NULL;
  }
  t0 = now_ms();
  /* #region paths */
  cw_nav_find_paths(nm, reqs, NP, pts, MAXP, counts);
  /* #endregion */
  paths_ms = (float)(now_ms() - t0);
  paths_hash = CW_HASH32_INIT;
  for (int i = 0; i < NP; i++) paths_hash = cw_hash32(&pts[i * MAXP], sizeof(t3_vec3) * (size_t)counts[i], paths_hash);
  cw_free(reqs);
  cw_free(pts);
  cw_free(counts);

  run_crowd(nm, 2000, 120, &step2000_ms, &step2000_max, &crowd2000_hash);
  cw_navmesh_free(nm);
  cw_navmesh small_nm = cw_navmesh_build(&c, &small);
  run_crowd(small_nm, 500, 29, &step500_ms, &step500_max, &crowd500_hash);
  cw_navmesh_free(small_nm);
  cw_nav_geom_free(&big);
  cw_nav_geom_free(&small);
  done = 1;
  cw_info("nav_perf (%d threads): build %.1f ms static, %.1f ms dynamic (%d tiles, %d polys); 64 obstacles %.2f ms; "
          "1000 paths %.2f ms; crowd step 500: %.3f ms (max %.3f), 2000: %.3f ms (max %.3f)",
          threads, (double)build_ms, (double)build_dyn_ms, tiles, polys, (double)update_ms, (double)paths_ms,
          (double)step500_ms, (double)step500_max, (double)step2000_ms, (double)step2000_max);
}

static void render(void *u, double a) {
  (void)u, (void)a;
  glClearColor(0.1f, 0.3f, 0.5f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "nav_perf", .width = 64, .height = 64, .debug = true, .init = init, .fixed_update = fixed,
                          .render = render };
}

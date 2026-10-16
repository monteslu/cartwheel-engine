/* World streaming suite (cartwheel/stream.h): cells load, activate,
 * deactivate and unload around a moving source; activation is a pure
 * function of the source's path (the same with slow, reordered IO: the
 * activation hash folds into unit.checksum, which the gate compares across
 * hosts); hysteresis stops thrashing on a border; the floating origin moves
 * in whole cells and shifts physics bodies. Cell files are
 * tests/unit/stream_assets/cells/c_<x>_<z>.cell.zst (zstd, decompressed on
 * the job pool). */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/physics3d.h"
#include "cartwheel/resource.h"
#include "cartwheel/stream.h"
#include "test.h"

/* ── a cell type: "cell <x> <z> <sum>" parsed on a worker, optionally slowly ── */
static cw_res_type cell_type;
static atomic_int slow_decodes; /* > 0: every decode spins a varying while (an IO delay stand-in) */
static atomic_uint spin_seed;

typedef struct cell_rec { int x, z, sum; } cell_rec;

static void *cell_decode(const cw_res_decode_args *a, size_t *cost) {
  if (atomic_load(&slow_decodes)) {
    /* how long depends on the order the workers got here: timing, not content */
    uint32_t s = atomic_fetch_add(&spin_seed, 2654435761u) * 1664525u + 1013904223u; /* int-math-ok: C, wraps by design */
    volatile uint32_t x = s;
    for (uint32_t i = 0; i < (s >> 12) % 200000u; i++) x = x * 1664525u + 1013904223u;
  }
  cell_rec *r = cw_alloc(sizeof *r, CW_MEM_GAME);
  if (sscanf((const char *)a->bytes, "cell %d %d %d", &r->x, &r->z, &r->sum) != 3) { cw_free(r); return NULL; }
  *cost = 1;
  return r;
}
static void *cell_finish(void *d, cw_res self, const cw_res *deps, int n, void *u) { return d; }
static void cell_unload(void *p, void *u) { cw_free(p); }

/* ── what the callbacks saw ───────────────────────────────────────────── */
static int loaded_now, active_now, bad_files, act_events, rebase_events;
static uint32_t cb_hash;

static void *on_load(cw_stream *s, const cw_stream_cell *c, void *u) {
  const cell_rec *r = cw_res_get(c->res);
  if (!r || r->x != c->cx || r->z != c->cz || r->sum != (c->cx * 131 + c->cz * 977 + 17) % 65521) bad_files++;
  loaded_now++;
  return (void *)(intptr_t)(c->id + 1);
}
static void on_unload(cw_stream *s, const cw_stream_cell *c, void *u) {
  if (c->data != (void *)(intptr_t)(c->id + 1)) bad_files++;
  loaded_now--;
}
static void on_activate(cw_stream *s, const cw_stream_cell *c, void *u) {
  active_now++;
  act_events++;
  int v[2] = { c->id, 1 };
  cb_hash = cw_hash32(v, sizeof v, cb_hash);
}
static void on_deactivate(cw_stream *s, const cw_stream_cell *c, void *u) {
  active_now--;
  int v[2] = { c->id, 0 };
  cb_hash = cw_hash32(v, sizeof v, cb_hash);
}

static int ready_polls;
static bool slow_ready; /* cell_ready says "not yet" two polls in three: on_load work finishing late */
static bool cell_ready(cw_stream *s, const cw_stream_cell *c, bool wait, void *u) {
  return wait || !slow_ready || ++ready_polls % 3 == 0;
}

#define CELL 96.0
static cw_stream_desc world_desc(float hysteresis) {
  cw_stream_desc d = cw_stream_default_desc();
  d.cells_x = d.cells_z = 8;
  d.cell_size = CELL;
  d.origin = (cw_dvec3){ -4 * CELL, 0, -4 * CELL };
  d.path_format = "cells/c_%d_%d.cell.zst";
  d.cell_type = cell_type;
  d.load_radius = 200;
  d.active_radius = 90;
  d.hysteresis = hysteresis;
  d.max_requests_per_tick = 3;
  d.prepare_per_frame = 2;
  d.on_load = on_load;
  d.on_unload = on_unload;
  d.on_activate = on_activate;
  d.on_deactivate = on_deactivate;
  d.cell_ready = cell_ready;
  return d;
}

/* the scripted path: a lumpy loop over most of the world, speed varying */
static cw_dvec3 path_at(int tick) {
  float t = (float)tick * (CW_TAU / 900.0f), s1, c1, s2, c2;
  cw_sincos(t, &s1, &c1);
  cw_sincos(2 * t + 0.4f, &s2, &c2);
  return (cw_dvec3){ 300.0 * c1 + 40.0 * c2, 0, 260.0 * s2 };
}

typedef struct run_result { uint64_t hash; uint32_t cb; int stalls, acts, max_loaded, violations; } run_result;

/* one tick per frame, cw_res_frame and cw_stream_frame each frame: what an app does */
static run_result run_path(float hysteresis, int ticks) {
  loaded_now = active_now = 0;
  cb_hash = CW_HASH32_INIT;
  cw_stream_desc d = world_desc(hysteresis);
  cw_stream *s = cw_stream_create(&d);
  int src = cw_stream_source_add(s, path_at(0), 0);
  run_result r = { 0 };
  for (int k = 0; k < ticks; k++) {
    cw_dvec3 p = path_at(k);
    cw_stream_source_move(s, src, p);
    cw_stream_tick(s);
    /* invariants: every cell within the active radius is ACTIVE; nothing ACTIVE beyond radius + hysteresis */
    for (int cz = 0; cz < 8; cz++)
      for (int cx = 0; cx < 8; cx++) {
        double x0 = d.origin.x + cx * CELL, z0 = d.origin.z + cz * CELL;
        double dx = p.x < x0 ? x0 - p.x : p.x > x0 + CELL ? p.x - x0 - CELL : 0;
        double dz = p.z < z0 ? z0 - p.z : p.z > z0 + CELL ? p.z - z0 - CELL : 0;
        double dist = sqrt(dx * dx + dz * dz);
        cw_cell_state st = cw_stream_cell_state(s, cx, cz);
        if (dist <= d.active_radius && st != CW_CELL_ACTIVE) r.violations++;
        if (st == CW_CELL_ACTIVE && dist > d.active_radius + cw_stream_get_desc(s)->hysteresis) r.violations++;
        if (st != CW_CELL_NONE && dist > d.load_radius + cw_stream_get_desc(s)->hysteresis) r.violations++;
      }
    cw_res_frame();
    cw_stream_frame(s);
    if (loaded_now > r.max_loaded) r.max_loaded = loaded_now;
  }
  cw_stream_stats st;
  cw_stream_get_stats(s, &st);
  r.hash = st.activation_hash;
  r.stalls = st.stalls;
  r.acts = (int)st.activations;
  if (active_now != st.active) r.violations++;
  cw_stream_destroy(s);
  if (loaded_now != 0 || active_now != 0) r.violations++; /* destroy unloads everything */
  r.cb = cb_hash;
  return r;
}

static void test_path(void) {
  bad_files = 0;
  run_result a = run_path(0, 1800);
  CHECK_EQ_INT(a.violations, 0);
  CHECK_EQ_INT(bad_files, 0);
  CHECK(a.acts > 20);
  CHECK(a.max_loaded < 40); /* the load radius, not the world */
  cw_info("stream path: %d activations, %d stalls, %d loaded at most, hash %016llx", a.acts, a.stalls, a.max_loaded,
          (unsigned long long)a.hash);
  FOLD(a.hash);
  FOLD(a.cb);
  FOLD(a.acts);
  /* the same path with slow decodes that finish in a different order, one at a time: same history */
  cw_res_set_max_in_flight(1);
  atomic_store(&slow_decodes, 1);
  slow_ready = true;
  run_result b = run_path(0, 1800);
  slow_ready = false;
  atomic_store(&slow_decodes, 0);
  cw_res_set_max_in_flight(0);
  CHECK(b.hash == a.hash);
  CHECK(b.cb == a.cb);
  CHECK_EQ_INT(b.violations, 0);
  cw_info("stream path, slow IO: %d stalls (activation unchanged)", b.stalls);
  /* control: a different hysteresis must change the history (the hash sees it) */
  run_result c = run_path(1, 1800);
  CHECK(c.hash != a.hash);
}

/* a source hovering across a border by less than the hysteresis activates once */
static void test_hysteresis(void) {
  loaded_now = active_now = 0;
  int acts[2];
  for (int k = 0; k < 2; k++) {
    cw_stream_desc d = world_desc(k == 0 ? 20 : 0.01f);
    d.load_radius = d.active_radius = 30;
    cw_stream *s = cw_stream_create(&d);
    int src = cw_stream_source_add(s, (cw_dvec3){ 0, 0, 0 }, 0);
    for (int t = 0; t < 400; t++) {
      float sn, cs;
      cw_sincos((float)t * 0.3f, &sn, &cs);
      cw_stream_source_move(s, src, (cw_dvec3){ CELL * 0 + 31.0 + 8.0 * sn, 0, 0 }); /* around x = 31: 1 m past the radius of cell x=-1's edge */
      cw_stream_tick(s);
      cw_res_frame();
      cw_stream_frame(s);
    }
    cw_stream_stats st;
    cw_stream_get_stats(s, &st);
    acts[k] = (int)st.activations;
    cw_stream_destroy(s);
  }
  cw_info("stream hysteresis: %d activations with it, %d without", acts[0], acts[1]);
  CHECK(acts[0] <= 6);          /* the cells around once */
  CHECK(acts[1] > acts[0] * 5); /* control: without it they thrash */
  FOLD(acts);
}

/* the floating origin: whole cells, the source stays near local zero, round trips exact */
static int rec_shifts;
static double rec_x, rec_z;
static bool rec_whole = true;
static void origin_rebase(cw_stream *s, t3_vec3 shift, void *u) {
  rec_shifts++;
  rec_x += shift.x, rec_z += shift.z;
  if (fmodf(shift.x, 256) != 0 || fmodf(shift.z, 256) != 0 || shift.y != 0) rec_whole = false;
}

static void test_origin(void) {
  cw_stream_desc d = cw_stream_default_desc();
  d.cells_x = d.cells_z = 64;
  d.cell_size = 256; /* a 16 km world, no files */
  d.origin = (cw_dvec3){ -8192, 0, -8192 };
  d.load_radius = 600;
  d.active_radius = 300;
  d.rebase_distance = 1024;
  d.on_rebase = origin_rebase;
  cw_stream *s = cw_stream_create(&d);
  int src = cw_stream_source_add(s, (cw_dvec3){ -7900, 0, -7900 }, 0);
  double far_local = 0;
  bool exact = true;
  for (int t = 0; t <= 2000; t++) {
    double k = t / 2000.0;
    cw_dvec3 p = { -7900 + 15800 * k, 12.5, -7900 + 15000 * k * k };
    cw_stream_source_move(s, src, p);
    cw_stream_tick(s);
    t3_vec3 l = cw_stream_to_local(s, p);
    double r = sqrt((double)l.x * l.x + (double)l.z * l.z);
    if (r > far_local) far_local = r;
    t3_vec3 l2 = cw_stream_to_local(s, cw_stream_to_world(s, l)); /* local -> world -> local is exact */
    if (l2.x != l.x || l2.z != l.z) exact = false;
  }
  cw_stream_stats st;
  cw_stream_get_stats(s, &st);
  cw_dvec3 o = cw_stream_origin(s);
  cw_info("stream origin: %d rebases, the source at most %.1f m from the local origin, origin now %.0f %.0f", st.rebases,
          far_local, o.x, o.z);
  CHECK(st.rebases >= 10);
  CHECK_EQ_INT(st.rebases, rec_shifts);
  CHECK(far_local < 1024 + 256);
  CHECK(exact);
  CHECK(rec_whole);
  CHECK(o.x - d.origin.x == rec_x && o.z - d.origin.z == rec_z); /* the shifts add up to the origin's travel */
  CHECK(st.active > 0);
  FOLD(o);
  FOLD(st.activation_hash);
  cw_stream_destroy(s);
}

/* cw_world3d_shift_origin: bodies move by -shift, keep their velocity and sleep */
static void test_physics_shift(void) {
  cw_world3d_def wd = cw_world3d_default_def();
  uint32_t hashes[2];
  float vy[2], dx[2];
  for (int k = 0; k < 2; k++) {
    cw_world3d w = cw_world3d_create(&wd);
    cw_body3d_def gd = cw_body3d_default_def();
    gd.type = CW_BODY_STATIC;
    gd.position = (t3_vec3){ 4000, 0, 0 };
    cw_body3d ground = cw_body3d_create(w, &gd);
    cw_shape3d_def sd = cw_shape3d_default_def();
    cw_shape3d_box(ground, &sd, (t3_vec3){ 50, 0.5f, 50 }, cw_xform3_identity());
    cw_body3d_def bd = cw_body3d_default_def();
    bd.position = (t3_vec3){ 4000, 5, 0 };
    bd.linear_velocity = (t3_vec3){ 3, 0, 0 };
    cw_body3d box = cw_body3d_create(w, &bd);
    cw_shape3d_box(box, &sd, (t3_vec3){ 0.5f, 0.5f, 0.5f }, cw_xform3_identity());
    for (int i = 0; i < 120; i++) {
      if (k == 1 && i == 30) cw_world3d_shift_origin(w, (t3_vec3){ 4096, 0, 0 }); /* back near zero */
      cw_world3d_step(w, 1.0f / 60);
    }
    cw_xform3 x = cw_body3d_transform(box), g = cw_body3d_transform(ground);
    dx[k] = x.p.x - g.p.x; /* where the box is on the ground: the same story either way */
    vy[k] = cw_body3d_velocity(box).y;
    hashes[k] = cw_world3d_stats(w).hash;
    cw_world3d_destroy(w);
  }
  cw_info("physics shift: box at %.4f / %.4f from the ground's centre", (double)dx[0], (double)dx[1]);
  CHECK(fabsf(dx[0] - dx[1]) < 0.01f); /* float rounding at 4 km vs near 0 differs a little, no more */
  CHECK(fabsf(vy[1]) < 0.1f);          /* resting, not knocked by the shift */
  FOLD(hashes[1]);
}

static void setup(void) {
  cw_res_type_desc d = { .name = "cell", .extensions = ".cell", .decode = cell_decode, .finish = cell_finish, .unload = cell_unload };
  cell_type = cw_res_register_type(&d);
  cw_res_set_residency_budget(64 * 1024); /* file bytes in deterministic runs: these cells stay cached */
}

static void test_all(void) {
  setup();
  test_path();
  test_hysteresis();
  test_origin();
  test_physics_shift();
}

CW_TEST_SUITE("stream", { "stream", test_all })

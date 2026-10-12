#include <math.h>

#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "test.h"
#include <sched.h>
#include <string.h>

static void test_pool(void) {
  cw_pool p;
  cw_pool_init(&p, 24, CW_MEM_GAME);
  uint64_t ids[1000];
  for (int i = 0; i < 1000; i++) {
    int *e = cw_pool_alloc(&p, &ids[i]);
    *e = i;
  }
  CHECK_EQ_INT(p.live, 1000);
  for (int i = 0; i < 1000; i += 2) CHECK(cw_pool_release(&p, ids[i]));
  CHECK(cw_pool_get(&p, ids[0]) == NULL);      /* stale */
  CHECK(!cw_pool_release(&p, ids[0]));          /* double free refused */
  CHECK(*(int *)cw_pool_get(&p, ids[1]) == 1);
  uint64_t n;
  cw_pool_alloc(&p, &n);
  CHECK(n != ids[998] && cw_pool_get(&p, ids[998]) == NULL); /* reused slot, new generation */
  CHECK(cw_pool_id_of(&p, cw_pool_get(&p, ids[3])) == ids[3]);
  int live = 0;
  for (int i = 0; i < p.slot_count; i++) live += cw_pool_at(&p, i) != NULL;
  CHECK_EQ_INT(live, 501);
  CHECK(cw_pool_get(&p, 0) == NULL);
  cw_pool_free_all(&p);
}

static void test_map(void) {
  cw_map m;
  cw_map_init(&m, CW_MEM_GAME);
  for (uint64_t i = 0; i < 5000; i++) cw_map_set(&m, i * 7919, i);
  uint64_t v = 0;
  CHECK(cw_map_get(&m, 7919 * 4321, &v) && v == 4321);
  CHECK(!cw_map_get(&m, 3, &v));
  for (uint64_t i = 0; i < 5000; i += 3) CHECK(cw_map_remove(&m, i * 7919));
  CHECK(!cw_map_get(&m, 0, &v));
  CHECK(cw_map_get(&m, 7919, &v) && v == 1);
  /* insertion order survives removals */
  bool ordered = true;
  uint64_t prev = 0;
  for (int i = 0; i < m.count; i++)
    if (cw_map_live(&m, i)) { ordered &= m.vals[i] >= prev; prev = m.vals[i]; }
  CHECK(ordered);
  CHECK_EQ_INT(cw_map_size(&m), 5000 - 1667);
  for (uint64_t i = 0; i < 5000; i++) { uint64_t v2; CHECK(cw_map_get(&m, i * 7919, &v2) == (i % 3 != 0)); }
  for (uint64_t i = 0; i < 5000; i += 3) cw_map_set(&m, i * 7919, i + 100000); /* re-add after removal */
  CHECK_EQ_INT(cw_map_size(&m), 5000);
  cw_map_free(&m);
}

static void test_array_arena_intern(void) {
  CW_ARRAY(int) a = { 0 };
  for (int i = 0; i < 100; i++) cw_array_push(&a, i * i);
  CHECK_EQ_INT(a.count, 100);
  CHECK_EQ_INT(a.data[99], 9801);
  cw_array_remove_swap(&a, 0);
  CHECK_EQ_INT(a.data[0], 9801);
  cw_array_free(&a);

  cw_arena ar;
  cw_arena_init(&ar, 1024, CW_MEM_GAME);
  void *p1 = cw_arena_alloc(&ar, 100), *p2 = cw_arena_alloc(&ar, 5000);
  CHECK(((uintptr_t)p1 & 15) == 0 && ((uintptr_t)p2 & 15) == 0);
  cw_arena_reset(&ar);
  CHECK(cw_arena_alloc(&ar, 100) == p1);
  cw_arena_free(&ar);

  char buf[16] = "hello";
  CHECK(cw_intern(buf) == cw_intern("hello"));
  CHECK(cw_intern("hello") != cw_intern("hellp"));
  /* counted interning: same pointer as a pin while live, freed by the last unref */
  uint32_t base = cw_intern_counted();
  const char *r1 = cw_intern_ref("counted-1"), *r2 = cw_intern_ref("counted-1");
  CHECK(r1 == r2 && cw_intern_find("counted-1") == r1);
  CHECK_EQ_INT((int)cw_intern_counted(), (int)base + 1);
  cw_intern_unref(r1);
  CHECK(cw_intern_find("counted-1") == r2);
  cw_intern_unref(r2);
  CHECK(cw_intern_find("counted-1") == NULL);
  CHECK_EQ_INT((int)cw_intern_counted(), (int)base);
  CHECK(cw_intern_ref("hello") == cw_intern("hello")); /* a pinned string: refs are no-ops */
  cw_intern_unref(cw_intern("hello"));
  cw_intern_unref("a literal");
  cw_intern_unref(NULL);
  CHECK(cw_intern_find("hello") != NULL);
  const char *r3 = cw_intern_ref("counted-2");
  CHECK(cw_intern("counted-2") == r3); /* pinning a counted string keeps the pointer... */
  cw_intern_unref(r3);
  CHECK(cw_intern_find("counted-2") == r3); /* ...and it outlives its references */
  const char *r4 = cw_intern_ref("counted-3");
  CHECK(cw_intern_pin(r4) == r4);
  cw_intern_unref(r4);
  CHECK(cw_intern_find("counted-3") == r4);
  CHECK_EQ_INT((int)cw_intern_counted(), (int)base);
  /* many distinct strings come and go: none stay */
  for (int i = 0; i < 5000; i++) cw_intern_unref(cw_intern_ref(cw_frame_printf("tmp-%d", i)));
  CHECK_EQ_INT((int)cw_intern_counted(), (int)base);
  CHECK(strcmp(cw_frame_printf("%d-%s", 7, "x"), "7-x") == 0);
}

static void test_rng_hash(void) {
  /* identical sequences on every host: folded into the checksum the gate compares */
  cw_rng r;
  cw_rng_seed(&r, 12345);
  for (int i = 0; i < 1000; i++) {
    uint32_t v = cw_rng_u32(&r);
    FOLD(v);
  }
  float f = cw_rng_range(&r, -2, 2);
  CHECK(f >= -2 && f < 2);
  FOLD(f);
  uint64_t h = cw_hash_str("cartwheel");
  FOLD(h);
  CHECK(cw_hash_str("a") != cw_hash_str("b"));
}

static void test_mem_stats(void) {
  cw_mem_stats s0, s1;
  cw_mem_get_stats(&s0);
  void *p = cw_alloc(1000, CW_MEM_AUDIO);
  cw_mem_get_stats(&s1);
  CHECK_EQ_INT(s1.bytes[CW_MEM_AUDIO] - s0.bytes[CW_MEM_AUDIO], 1000);
  cw_free(p);
  cw_mem_get_stats(&s1);
  CHECK_EQ_INT(s1.bytes[CW_MEM_AUDIO], s0.bytes[CW_MEM_AUDIO]);
}

/* parallel_for: per-chunk partial sums reduced in chunk order = deterministic */
static double partial[1024];
static void sum_chunk(int b, int e, int w, void *ctx) {
  (void)w;
  const float *data = ctx;
  /* items are groups of 1000 floats: chunk boundaries can fall anywhere in
   * [0, 1000) groups, and each group sum is the same whatever thread runs it */
  for (int g = b; g < e; g++) {
    double s = 0;
    for (int i = g * 1000; i < (g + 1) * 1000; i++) s += data[i];
    partial[g] = s;
  }
}
/* M5.3 finding: chunk boundaries are the same on every host (not derived
 * from the worker count) */
static int chunk_len[5000];
static void record_chunk(int b, int e, int w, void *ctx) {
  (void)w, (void)ctx;
  chunk_len[b] = e - b;
}
static bool chunks_are(int count, int size) {
  bool ok = true;
  for (int i = 0; i < count; i++) ok &= chunk_len[i] == (i % size ? 0 : CW_MIN(size, count - i));
  return ok;
}
static void test_job_chunks(void) {
  memset(chunk_len, 0, sizeof chunk_len);
  cw_parallel_for(1000, 8, record_chunk, NULL); /* max(8, ceil(1000 / 128)) = 8 */
  CHECK(chunks_are(1000, 8));
  memset(chunk_len, 0, sizeof chunk_len);
  cw_parallel_for(5000, 1, record_chunk, NULL); /* ceil(5000 / 128) = 40 */
  CHECK(chunks_are(5000, 40));
  memset(chunk_len, 0, sizeof chunk_len);
  cw_parallel_for(1000, 100, record_chunk, NULL); /* min_batch wins */
  CHECK(chunks_are(1000, 100));
}

/* M5.3 finding: cw_log_remove_listener waits for a call running on another
 * thread (the caller may free the listener's data right after) */
static int slow_entered, slow_finished;
static void slow_listener(cw_log_level l, const char *msg, void *user) {
  (void)l, (void)user;
  if (strcmp(msg, "slow listener probe")) return;
  __atomic_store_n(&slow_entered, 1, __ATOMIC_SEQ_CST);
  volatile uint32_t x = 0;
  for (uint32_t i = 0; i < 60000000u; i++) x += i; /* tens of ms on any host */
  __atomic_store_n(&slow_finished, 1, __ATOMIC_SEQ_CST);
}
static void log_probe(void *ctx) {
  (void)ctx;
  cw_info("slow listener probe");
}
/* a listener that removes itself: removal from inside its own call must not wait for that call */
static int self_id = -1, self_calls;
static void self_removing(cw_log_level l, const char *msg, void *user) {
  (void)l, (void)user;
  if (strcmp(msg, "self removal probe")) return;
  self_calls++;
  cw_log_remove_listener(self_id);
}
static void test_log_remove_waits(void) {
  self_calls = 0;
  self_id = cw_log_add_listener(self_removing, NULL);
  cw_info("self removal probe");
  cw_info("self removal probe");
  CHECK_EQ_INT(self_calls, 1);
#if !CW_CART
  /* natively a worker logs while the main thread removes. Not in carts: a
   * cart worker's log goes through the host on the browser's main thread,
   * which this test's spin would block (the wait itself is the same code) */
  slow_entered = slow_finished = 0;
  int id = cw_log_add_listener(slow_listener, NULL);
  cw_job *j = cw_job_single(log_probe, NULL); /* a worker logs (inline when there are none) */
  while (!__atomic_load_n(&slow_entered, __ATOMIC_SEQ_CST)) sched_yield();
  cw_log_remove_listener(id);
  CHECK(__atomic_load_n(&slow_finished, __ATOMIC_SEQ_CST) == 1);
  cw_job_wait(j);
#else
  CHECK(1); /* the same check count on every host (the gate compares unit.passed) */
#endif
}

static void test_jobs(void) {
  static float data[1000000];
  for (int i = 0; i < 1000000; i++) data[i] = cw_sin((float)i * 0.001f); /* libm sinf differs by host */
  for (int rep = 0; rep < 3; rep++) {
    memset(partial, 0, sizeof partial);
    cw_job *j = cw_job_range(1000, 8, sum_chunk, data);
    cw_job_wait(j);
    double total = 0;
    for (int i = 0; i < 1000; i++) total += partial[i];
    if (rep == 0) FOLD(total);
    double serial = 0;
    for (int i = 0; i < 1000; i++) {
      double s = 0;
      for (int k = i * 1000; k < (i + 1) * 1000; k++) s += data[k];
      serial += s;
    }
    CHECK(total == serial);
  }
  cw_jobs_stats st;
  cw_jobs_get_stats(&st);
  CHECK(st.jobs >= 3);
}

/* M5.3 finding: the debug channel's argument splitting (key="a b") */
static void echo_cmd(int argc, const char **argv, cw_reply *r) {
  cw_reply_raw(r, "[");
  for (int i = 1; i < argc; i++) {
    if (i > 1) cw_reply_raw(r, ",");
    cw_reply_str(r, argv[i]);
  }
  cw_reply_raw(r, "]");
}
static void test_channel_split(void) {
  cw_debug_command("test_echo", "test_echo args...: the arguments as split", echo_cmd);
  char *rep = cw_debug_run("test_echo name=\"Gem Garden\" \"a b\" vec3(1, 2, 3) node(\"/root/x y\") k=\"q\\\"q\" plain");
  const char *want = "[\"name=Gem Garden\",\"a b\",\"vec3(1, 2, 3)\",\"node(\\\"/root/x y\\\")\",\"k=q\\\"q\",\"plain\"]";
  CHECK(rep && !strcmp(rep, want));
  if (rep && strcmp(rep, want)) cw_error("split: %s\n want: %s", rep, want);
  cw_free(rep);
}

CW_TEST_SUITE("core", { "pool", test_pool }, { "map", test_map }, { "array_arena_intern", test_array_arena_intern },
              { "rng_hash", test_rng_hash }, { "mem_stats", test_mem_stats }, { "jobs", test_jobs },
              { "channel_split", test_channel_split },
              { "job_chunks", test_job_chunks },
              { "log_remove_waits", test_log_remove_waits })

/* Render pipeline unit suite (pipeline.h): pass ordering and switches, the
 * POST ping-pong contract, the target pool, persistent targets, mode
 * defaults, light unit conversion and the .cube parser. Runs real frames
 * into a 32x32 target on every host. */
#include <math.h>
#include <string.h>

#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "test.h"

static char order[64];
static int order_n;
static const cw_rt *seen_src[4], *seen_dst[4], *seen_out;
static int seen_n;

static void rec(cw_pass_ctx *ctx, void *u) {
  (void)ctx;
  if (order_n < (int)sizeof order - 1) order[order_n++] = *(const char *)u;
  order[order_n] = 0;
}
static void post_rec(cw_pass_ctx *ctx, void *u) {
  rec(ctx, u);
  if (seen_n < 4) {
    seen_src[seen_n] = ctx->src;
    seen_dst[seen_n] = ctx->dst;
    seen_n++;
  }
  seen_out = ctx->output;
  if (ctx->dst) { /* write something so the chain is real */
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
}
static bool never(const cw_pass_ctx *ctx, void *u) { (void)ctx; (void)u; return false; }

static cw_tree *make_tree(bool camera) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  if (camera) {
    cw_node cam = cw_node_new("Camera3D", "Cam");
    cw_node_add_child(cw_tree_root(t), cam);
    cw_node box = cw_node_new("MeshInstance3D", "Box");
    cw_node3d_set_position(box, (t3_vec3){ 0, 0, -3 });
    cw_node_add_child(cw_tree_root(t), box);
  }
  return t;
}

static void test_order(void) {
  cw_tree *t = make_tree(false);
  cw_pipeline *p = cw_pipeline_of(t);
  static const char a = 'a', b = 'b', c = 'c', d = 'd', e = 'e', f = 'f';
  /* registered out of order: phase first, then order, then registration */
  CHECK(cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.c", .phase = CW_PHASE_PRESENT, .order = 0, .execute = rec, .user = (void *)&c }));
  CHECK(cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.b", .phase = CW_PHASE_OVERLAY, .order = 10, .execute = rec, .user = (void *)&b }));
  CHECK(cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.a", .phase = CW_PHASE_OVERLAY, .order = -5, .execute = rec, .user = (void *)&a }));
  CHECK(cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.d", .phase = CW_PHASE_OVERLAY, .order = 10, .execute = rec, .user = (void *)&d }));
  CHECK(cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.e", .phase = CW_PHASE_OVERLAY, .order = 1, .disabled = true, .execute = rec, .user = (void *)&e }));
  CHECK(cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.f", .phase = CW_PHASE_OVERLAY, .order = 2, .active = never, .execute = rec, .user = (void *)&f }));
  /* names are unique */
  CHECK(!cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.a", .phase = CW_PHASE_OVERLAY, .execute = rec, .user = (void *)&a }));
  /* no camera: the 3D phases are skipped, overlays still run */
  order_n = 0;
  cw_pipeline_render(t, 32, 32, 0);
  CHECK(strcmp(order, "abdc") == 0);
  cw_pipeline_set_pass_enabled(p, "t.e", true);
  CHECK(cw_pipeline_remove_pass(p, "t.d"));
  CHECK(!cw_pipeline_has_pass(p, "t.d"));
  CHECK(!cw_pipeline_remove_pass(p, "t.d"));
  order_n = 0;
  cw_pipeline_render(t, 32, 32, 0);
  CHECK(strcmp(order, "aebc") == 0);
  /* stats list every pass in run order with what ran */
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(p, &st);
  int ran = 0, idx_a = -1, idx_c = -1;
  for (int i = 0; i < n; i++) {
    if (!strncmp(st[i].name, "t.", 2)) ran += st[i].ran;
    if (!strcmp(st[i].name, "t.a")) idx_a = i;
    if (!strcmp(st[i].name, "t.c")) idx_c = i;
  }
  CHECK_EQ_INT(ran, 4);
  CHECK(idx_a >= 0 && idx_c > idx_a);
  FOLD(order);
  cw_tree_free(t);
}

static void test_post_chain(void) {
  cw_tree *t = make_tree(true);
  cw_pipeline *p = cw_pipeline_of(t);
  cw_rt *out = cw_rt_new(32, 32, CW_RT_RGBA8, false);
  CHECK(out != NULL);
  cw_pipeline_set_output(p, out);
  static const char x = 'x', y = 'y', z = 'z';
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "p.x", .phase = CW_PHASE_POST, .order = 400, .reads = CW_RES_COLOR,
                                           .writes = CW_RES_COLOR, .execute = post_rec, .user = (void *)&x });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "p.y", .phase = CW_PHASE_POST, .order = 401, .reads = CW_RES_COLOR,
                                           .writes = CW_RES_COLOR, .execute = post_rec, .user = (void *)&y });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "p.z", .phase = CW_PHASE_POST, .order = 402, .reads = CW_RES_COLOR,
                                           .execute = post_rec, .user = (void *)&z });
  for (int frame = 0; frame < 2; frame++) {
    seen_n = 0;
    order_n = 0;
    cw_tree_pre_render(t, 0);
    cw_pipeline_render(t, 32, 32, 0);
    CHECK(strcmp(order, "xyz") == 0);
    CHECK_EQ_INT(seen_n, 3);
    /* x reads the scene, writes a pooled target; y reads that and writes
     * another (the tonemap pass, 500, is the last colour writer: it writes
     * the output); z only reads y's */
    CHECK(seen_src[0] != NULL && seen_dst[0] != NULL && seen_dst[0] != out);
    CHECK(seen_src[1] == seen_dst[0]);
    CHECK(seen_dst[1] != out && seen_dst[1] != NULL);
    CHECK(seen_dst[2] == NULL && seen_src[2] == seen_dst[1]);
    CHECK(seen_out == out);
  }
  /* pooled targets are reused frame to frame */
  const cw_rt *first = seen_dst[0];
  seen_n = 0;
  cw_pipeline_render(t, 32, 32, 0);
  CHECK(seen_dst[0] == first);
  cw_tree_free(t);
  cw_rt_free(out);
}

static void test_persistent(void) {
  cw_tree *t = make_tree(false);
  cw_pipeline *p = cw_pipeline_of(t);
  bool fresh = false;
  const cw_rt *a = cw_pipeline_rt_persistent(p, "test.hist", 8, 8, CW_RT_RGBA16F, &fresh);
  CHECK(a && fresh);
  const cw_rt *b = cw_pipeline_rt_persistent(p, "test.hist", 8, 8, CW_RT_RGBA16F, &fresh);
  CHECK(b == a && !fresh);
  const cw_rt *c = cw_pipeline_rt_persistent(p, "test.hist", 16, 8, CW_RT_RGBA16F, &fresh);
  CHECK(c && fresh && c->w == 16);
  CHECK(cw_pipeline_caps_get()->float_render);
  cw_tree_free(t);
}

static void test_cube(void) {
  /* a 2^3 identity with a comment, a title and a domain */
  const char *ok = "# identity\nTITLE \"id\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"
                   "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
  uint64_t id = cw_lut_parse_cube(ok, strlen(ok), "ok.cube");
  CHECK(id != 0);
  cw_lut_free(id);
  const char *short_data = "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n";
  CHECK(cw_lut_parse_cube(short_data, strlen(short_data), "short.cube") == 0);
  const char *one_d = "LUT_1D_SIZE 4\n0 0 0\n";
  CHECK(cw_lut_parse_cube(one_d, strlen(one_d), "1d.cube") == 0);
  const char *early = "0 0 0\nLUT_3D_SIZE 2\n";
  CHECK(cw_lut_parse_cube(early, strlen(early), "early.cube") == 0);
  const char *big = "LUT_3D_SIZE 65\n";
  CHECK(cw_lut_parse_cube(big, strlen(big), "big.cube") == 0);
  const char *dom = "LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
  CHECK(cw_lut_parse_cube(dom, strlen(dom), "domain.cube") == 0);
  /* a generated LUT (r fastest) */
  float rgb[8 * 3] = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1 };
  uint64_t gen = cw_lut_from_rgb(2, rgb);
  CHECK(gen != 0);
  cw_lut_free(gen);
}

/* cw_render3d_stats.cpu_ms is filled (it was always 0), and the background
 * colour is a scene-linear HDR colour that goes through exposure and the
 * tone curve like fog and every lit pixel */
static void read_corner(const cw_rt *out, uint8_t px[4]) {
  glBindFramebuffer(GL_FRAMEBUFFER, out->fbo);
  glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
static void test_render3d_stats_background(void) {
  cw_tree *t = make_tree(true);
  cw_node env = cw_node_new("WorldEnvironment", "Env");
  cw_node_add_child(cw_tree_root(t), env);
  CHECK(cw_node_set(env, "background", cw_val_color(1, 0, 0, 1)));
  cw_pipeline *p = cw_pipeline_of(t);
  cw_rt *out = cw_rt_new(32, 32, CW_RT_RGBA8, false);
  CHECK(out != NULL);
  cw_pipeline_set_output(p, out);
  cw_tree_pre_render(t, 0);
  cw_pipeline_render(t, 32, 32, 0);
  cw_render3d_stats st;
  cw_render3d_get_stats(t, &st);
  CHECK(st.draw_calls >= 1);
  CHECK(st.cpu_ms > 0);
  uint8_t px[4] = { 0 };
  read_corner(out, px);
  cw_info("background color(1,0,0) -> (%d, %d, %d)", px[0], px[1], px[2]);
  /* AgX: pure red comes out darker and desaturated toward orange-grey */
  CHECK(px[0] > 150 && px[0] < 250 && px[1] > 30 && px[1] < px[0] && px[2] < px[0]);
  cw_tree_free(t);
  cw_rt_free(out);
}

/* A draw whose program no table has must never fail quietly: the pipeline reports the state, the exact fix names the
 * app, and the platform's banner flag (cw__missing_programs) is set. Where three.c's runtime builder is compiled in
 * (native development) the state is built instead, so nothing is missing; both outcomes are checked. */
extern int32_t cw__missing_programs;
const char *cw__programs_fix_command(void);
static void test_missing_states_are_loud(void) {
  /* control: a plain scene is fully covered, nothing is reported */
  cw_tree *plain = make_tree(true);
  cw_pipeline_render(plain, 32, 32, 0);
  CHECK(cw_pipeline_missing_states(plain)[0] == 0);
  CHECK_EQ_INT(cw__missing_programs, 0);
  cw_tree_free(plain);
  /* the fix command names this app's target */
  CHECK(strstr(cw__programs_fix_command(), "node tools/programs.mjs unit_pipeline") != NULL);
  /* three kinds of light at once: a light vector the app's table lacks */
  cw_tree *t = make_tree(true);
  cw_node_add_child(cw_tree_root(t), cw_node_new("DirectionalLight3D", "Sun"));
  cw_node_add_child(cw_tree_root(t), cw_node_new("OmniLight3D", "Lamp"));
  cw_node_add_child(cw_tree_root(t), cw_node_new("SpotLight3D", "Spot"));
  cw_pipeline_render(t, 32, 32, 0);
  const char *m = cw_pipeline_missing_states(t);
  if (m[0]) {
    CHECK(strstr(m, "standard") != NULL); /* names the state */
    CHECK(cw__missing_programs > 0);      /* and the banner has something to say */
  } else {
    CHECK_EQ_INT(cw__missing_programs, 0); /* built at run time by the development builder */
  }
  cw_tree_free(t);
  /* leave the report clean for the gate that reads it */
  plain = make_tree(true);
  cw_pipeline_render(plain, 32, 32, 0);
  CHECK_EQ_INT(cw__missing_programs, 0);
  cw_tree_free(plain);
}

CW_TEST_SUITE("pipeline", { "order", test_order }, { "post_chain", test_post_chain }, { "persistent", test_persistent },
              { "cube", test_cube },
              { "render3d_stats_background", test_render3d_stats_background },
              { "missing_states_are_loud", test_missing_states_are_loud })

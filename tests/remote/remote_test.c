/* Play-in-editor gate: drives samples/remote_target through
 * cartwheel/remote.h exactly as the editor will, on one transport:
 *
 *   remote_test native <game exe> [--control]
 *   remote_test romdev <game.wasc> [--control]
 *
 * It launches the game paused, edits a property while paused, steps N ticks
 * and checks the GAME's state moved by the edited value (not just that the
 * edit echoed), calls methods, follows the change log, takes a screenshot
 * per layer and checks what each layer drew, records input, reloads a
 * resource and a scene, then crashes the game on purpose and kills it, and
 * relaunches each time. Each check that could pass vacuously has a control
 * beside it that must fail. --control skips the edit but keeps expecting its
 * effect: that run MUST fail (tests/remote.mjs checks it does), so the effect
 * check is proven able to fail.
 *
 * The last line is JSON with the measured values tests/remote.mjs compares
 * across transports and against a plain CW_FRAMES run. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cartwheel/remote.h"
#include "stb_image.h"

static int fails, checks;
#define CHECK(cond, ...) \
  do { \
    checks++; \
    if (!(cond)) { \
      fails++; \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static cw_remote *G;
static const char *transport;

/* a call that must succeed; aborts the run when the game cannot answer */
static char *must(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *must(const char *fmt, ...) {
  char q[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(q, sizeof q, fmt, ap);
  va_end(ap);
  char *err = NULL;
  char *rep = cw_remote_call(G, q, 20000, &err);
  if (!rep) {
    printf("FAIL `%s`: %s (game: %s)\n", q, err ? err : "?", cw_remote_status(G));
    fails++;
    free(err);
    return calloc(1, 1);
  }
  return rep;
}

/* a call that must be REFUSED by the game; returns the error text */
static char *refused(const char *q) {
  char *err = NULL;
  char *rep = cw_remote_call(G, q, 20000, &err);
  checks++;
  if (rep) {
    printf("FAIL `%s` was accepted (%s) but must be refused\n", q, rep);
    fails++;
    free(rep);
    return calloc(1, 1);
  }
  return err ? err : calloc(1, 1);
}

static double num(const char *json, const char *key) {
  double v = NAN;
  cw_remote_json_number(json, key, &v);
  return v;
}

/* "vec2(5, 6)" */
static bool vec2_of(const char *json, const char *prop, float *x, float *y) {
  const char *k = strstr(json, prop);
  if (!k) return false;
  const char *v = strstr(k, "vec2(");
  return v && sscanf(v, "vec2(%f, %f)", x, y) == 2;
}

static void on_event(cw_remote *r, cw_remote_event ev, const char *text, void *user) {
  (void)r;
  (void)user;
  if (ev == CW_REMOTE_EV_STATE) printf("  [state] %s\n", text);
}

static bool launch(cw_remote_desc *d) {
  G = cw_remote_launch(d);
  if (!G) {
    printf("FAIL launch: %s\n", cw_remote_last_error());
    return false;
  }
  cw_remote_on_event(G, on_event, NULL);
  return cw_remote_state_of(G) == CW_REMOTE_RUNNING;
}

/* pump until the game is no longer running (it crashed or exited) */
static bool wait_dead(int ms) {
  for (int i = 0; i < ms / 5; i++) {
    cw_remote_pump(G);
    cw_remote_state s = cw_remote_state_of(G);
    if (s != CW_REMOTE_RUNNING && s != CW_REMOTE_STARTING) return true;
    usleep(5000);
  }
  return false;
}

typedef struct { int opaque /* not black */, magenta, green, orange, light; } pix_stats;
static bool png_stats(const void *png, size_t len, pix_stats *st, int *w, int *h) {
  int n;
  unsigned char *px = stbi_load_from_memory(png, (int)len, w, h, &n, 4);
  if (!px) return false;
  memset(st, 0, sizeof *st);
  for (int i = 0; i < *w * *h; i++) {
    unsigned char *p = px + i * 4;
    if (p[0] + p[1] + p[2] > 30) st->opaque++; /* drawn: a layer without WORLD starts from black */
    if (p[3] > 200 && p[0] > 200 && p[1] < 60 && p[2] > 200) st->magenta++;
    if (p[3] > 200 && p[0] < 90 && p[1] > 120 && p[2] < 110) st->green++;
    if (p[3] > 200 && p[0] > 200 && p[1] > 70 && p[1] < 140 && p[2] < 60) st->orange++;
    if (p[3] > 200 && p[0] > 200 && p[1] > 200 && p[2] > 200) st->light++;
  }
  stbi_image_free(px);
  return true;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: remote_test native <exe> | romdev <wasc> [--control]\n");
    return 2;
  }
  transport = argv[1];
  bool control = argc > 3 && !strcmp(argv[3], "--control");
  bool native = !strcmp(transport, "native");
  cw_remote_desc d = { .transport = native ? CW_REMOTE_NATIVE : CW_REMOTE_ROMDEV, .start_paused = true, .headless = true,
                       .timeout_ms = 20000 };
  if (native) d.exe = argv[2];
  else d.cart = argv[2];
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* ── launch, paused at tick 1 ───────────────────────────────────── */
  if (!launch(&d)) {
    printf("FAIL launch: %s\n", G ? cw_remote_status(G) : cw_remote_last_error());
    return 1;
  }
  printf("ok   launched: %s\n", cw_remote_status(G));
  char *st = must("status");
  bool paused = false;
  cw_remote_json_bool(st, "paused", &paused);
  CHECK(paused, "start_paused game reports paused: %s", st);
  CHECK(num(st, "tick") == 1, "paused at tick 1: %s", st);
  free(st);

  /* nodes by uid */
  char *nd = must("node Level/Mover");
  int uid = (int)num(nd, "uid");
  CHECK(uid > 0, "Mover has a uid: %s", nd);
  free(nd);

  char *v0 = must("get #%d position speed ticks", uid);
  float x0 = 0, y0 = 0;
  CHECK(vec2_of(v0, "position", &x0, &y0), "position: %s", v0);
  CHECK(strstr(v0, "\"speed\":{\"value\":\"33.7\""), "speed 33.7 at start: %s", v0);
  int ticks0 = 0;
  { const char *t = strstr(v0, "\"ticks\":{\"value\":\""); if (t) ticks0 = atoi(t + 18); }
  free(v0);

  /* ── the debugger's pause holds: frames pass, ticks do not ───────── */
  usleep(200000);
  char *st2 = must("status");
  CHECK(num(st2, "tick") == 1, "no tick runs while paused (200 ms later): %s", st2);
  free(st2);

  /* ── live edit, type-checked ─────────────────────────────────────── */
  char *e1 = refused("set #999999 speed 90");
  CHECK(strstr(e1, "no node with uid 999999"), "a dead uid is named: %s", e1);
  free(e1);
  char *e2 = refused("set #0 speed vec3(1, 2, 3)");
  free(e2);
  char e2q[128];
  snprintf(e2q, sizeof e2q, "set #%d speed vec3(1, 2, 3)", uid);
  char *e3 = refused(e2q);
  CHECK(strstr(e3, "wants float") && strstr(e3, "set #"), "a type error names the type and the fixing call: %s", e3);
  free(e3);
  snprintf(e2q, sizeof e2q, "set #%d ticks 5", uid);
  char *e4 = refused(e2q);
  CHECK(strstr(e4, "read-only"), "read-only refused: %s", e4);
  free(e4);
  if (!control) {
    char *s1 = must("set #%d speed 90", uid);
    CHECK(strstr(s1, "\"value\":\"90\""), "set echoes 90: %s", s1);
    free(s1);
  }

  /* ── step N ticks: the edit moved the game ───────────────────────── */
  const int N = 30;
  char *err = NULL;
  long long tick = cw_remote_step(G, N, 0, &err);
  CHECK(tick == 1 + N, "step %d lands on tick %d: got %lld (%s)", N, 1 + N, tick, err ? err : "");
  free(err);
  char *v1 = must("get #%d position ticks", uid);
  float x1 = 0, y1 = 0;
  vec2_of(v1, "position", &x1, &y1);
  int ticks1 = 0;
  { const char *t = strstr(v1, "\"ticks\":{\"value\":\""); if (t) ticks1 = atoi(t + 18); }
  free(v1);
  float want = x0 + N * 90.0f / 60.0f, unedited = x0 + N * 33.7f / 60.0f;
  CHECK(fabsf(x1 - want) < 0.01f, "after the edit and %d ticks x = %g, want %g (speed 90)", N, x1, want);
  CHECK(fabsf(x1 - unedited) > 1.0f, "control: x %g must differ from the unedited %g", x1, unedited);
  CHECK(ticks1 == ticks0 + N, "the mover ran exactly %d ticks: %d -> %d", N, ticks0, ticks1);
  st = must("status");
  CHECK(num(st, "tick") == 1 + N, "still paused at tick %d after the step: %s", 1 + N, st);
  free(st);

  /* ── call ────────────────────────────────────────────────────────── */
  char *c1 = must("call #%d boost 10", uid);
  CHECK(strstr(c1, "\"result\":\"100\""), "boost returns the new speed: %s", c1);
  free(c1);
  snprintf(e2q, sizeof e2q, "call #%d boost", uid);
  char *e5 = refused(e2q);
  CHECK(strstr(e5, "takes 1 argument"), "arity checked: %s", e5);
  free(e5);
  snprintf(e2q, sizeof e2q, "call #%d fly", uid);
  char *e6 = refused(e2q);
  CHECK(strstr(e6, "no method 'fly'") && strstr(e6, "methods #"), "unknown method names the fix: %s", e6);
  free(e6);
  cw_remote_step(G, 6, 0, NULL);
  char *v2 = must("get #%d position", uid);
  float x2 = 0, y2 = 0;
  vec2_of(v2, "value", &x2, &y2);
  free(v2);
  CHECK(fabsf(x2 - (x1 + 6 * 100.0f / 60.0f)) < 0.01f, "boost took effect: %g -> %g", x1, x2);

  /* ── change notifications ───────────────────────────────────────── */
  char *tr = must("tree /root 1");
  double rev = num(tr, "revision");
  CHECK(rev >= 0, "tree carries a revision: %.200s", tr);
  free(tr);
  char *sp = must("spawn Polygon2D /root/Level Extra");
  int extra = (int)num(sp, "uid");
  free(sp);
  char *ch = must("changes %.0f", rev);
  char want_add[96];
  snprintf(want_add, sizeof want_add, "\"kind\":\"added\",\"uid\":%d", extra);
  CHECK(strstr(ch, want_add) && strstr(ch, "\"name\":\"Extra\""), "changes reports the spawn: %s", ch);
  double rev2 = num(ch, "revision");
  free(ch);
  char *nothing = must("changes %.0f", rev2);
  CHECK(strstr(nothing, "\"events\":[]"), "control: nothing new since the last revision: %s", nothing);
  free(nothing);
  free(must("free #%d", extra));
  ch = must("changes %.0f", rev2);
  snprintf(want_add, sizeof want_add, "\"kind\":\"removed\",\"uid\":%d", extra);
  CHECK(strstr(ch, want_add), "changes reports the free: %s", ch);
  free(ch);
  char *rs = must("changes 99999999");
  CHECK(strstr(rs, "\"resync\":true"), "a revision from the future resyncs: %s", rs);
  free(rs);

  /* ── screenshots by layer ───────────────────────────────────────── */
  struct { const char *layers; pix_stats s; int w, h; } shots[4] = { { "all" }, { "world" }, { "ui" }, { "debug" } };
  for (int i = 0; i < 4; i++) {
    void *png = NULL;
    size_t len = 0;
    char *serr = NULL;
    bool ok = cw_remote_screenshot(G, shots[i].layers, 1, NULL, &png, &len, 20000, &serr);
    CHECK(ok && png_stats(png, len, &shots[i].s, &shots[i].w, &shots[i].h), "screenshot %s: %s", shots[i].layers, serr ? serr : "");
    if (ok && i == 0 && getenv("REMOTE_SHOT_DIR")) {
      char p[1024];
      snprintf(p, sizeof p, "%s/remote-%s-all.png", getenv("REMOTE_SHOT_DIR"), transport);
      FILE *f = fopen(p, "wb");
      if (f) { fwrite(png, 1, len, f); fclose(f); }
    }
    free(png);
    free(serr);
  }
  pix_stats *all = &shots[0].s, *world = &shots[1].s, *ui = &shots[2].s, *dbg = &shots[3].s;
  int area = shots[0].w * shots[0].h;
  printf("  layers (drawn/magenta/green/orange/light): all %d/%d/%d/%d/%d world %d/%d/%d/%d/%d ui %d/%d/%d/%d/%d debug %d/%d/%d/%d/%d\n",
         all->opaque, all->magenta, all->green, all->orange, all->light, world->opaque, world->magenta, world->green, world->orange,
         world->light, ui->opaque, ui->magenta, ui->green, ui->orange, ui->light, dbg->opaque, dbg->magenta, dbg->green, dbg->orange,
         dbg->light);
  CHECK(all->magenta > 20 && all->green > 1000 && all->orange > 50 && all->light > 20, "all layers in the full frame");
  CHECK(world->green > 1000 && world->orange > 50 && world->magenta == 0 && world->light == 0, "world: ground and mover, no debug, no text");
  CHECK(ui->light > 20 && ui->green == 0 && ui->magenta == 0 && ui->opaque < area / 10, "ui: text only, on black");
  CHECK(dbg->magenta > 20 && dbg->green == 0 && dbg->light == 0 && dbg->opaque < area / 10, "debug: the outline only");
  char *e7 = refused("screenshot sky");
  CHECK(strstr(e7, "world, ui, debug or all"), "bad layer names the choices: %s", e7);
  free(e7);

  /* ── record ─────────────────────────────────────────────────────── */
  free(must("record start"));
  cw_remote_step(G, 12, 0, NULL);
  char *rc = must("record stop");
  CHECK(num(rc, "ticks") == 12, "recorded the 12 stepped ticks: %s", rc);
  free(rc);
  char *rf = must("record fetch");
  char *b64 = cw_remote_json_string(rf, "base64");
  CHECK(b64 && !strncmp(b64, "Q1dJUg", 6), "the recording is a CWIR stream: %.80s", rf); /* base64 of "CWIR" */
  free(b64);
  free(rf);

  /* ── reload a resource: the game reacts ──────────────────────────── */
  /* "config v2\n" pushed into the project layer, as a tool or the editor does */
  free(must("vfs write data/config.txt Y29uZmlnIHYyCg=="));
  char *rl = must("reload data/config.txt");
  CHECK(strstr(rl, "\"version\":2"), "reload bumps the version: %s", rl);
  free(rl);
  cw_remote_step(G, 1, 0, NULL); /* the game's update sees the new version */
  char *hud = must("get Level/Hud text");
  CHECK(strstr(hud, "config v2"), "the game shows the reloaded text: %s", hud);
  free(hud);
  char *e8 = refused("reload data/nope.txt");
  CHECK(strstr(e8, "not loaded"), "reloading something not loaded is refused: %s", e8);
  free(e8);

  /* ── change_scene / reload_scene ────────────────────────────────── */
  char *cs = must("change_scene alt.cwscene");
  CHECK(strstr(cs, "alt.cwscene"), "change_scene: %s", cs);
  free(cs);
  char *ta = must("tree /root 1");
  CHECK(strstr(ta, "AltLevel") && !strstr(ta, "\"Level\""), "the alt scene replaced the main one: %.300s", ta);
  free(ta);
  char *rsn = must("reload_scene");
  CHECK(strstr(rsn, "\"reread\":true"), "reload_scene: %s", rsn);
  free(rsn);
  free(must("change_scene main.cwscene"));
  char *e9 = refused("change_scene nope.cwscene");
  free(e9);

  /* ── resume runs the game, pause freezes it again ──────────────────── */
  char *rsm = must("resume");
  double t_resume = num(rsm, "tick");
  free(rsm);
  usleep(400000);
  char *ps = must("pause");
  double t_pause = num(ps, "tick");
  free(ps);
  CHECK(t_pause > t_resume + 5, "resumed game ran ticks on its own: %g -> %g", t_resume, t_pause);
  usleep(150000);
  st = must("status");
  CHECK(num(st, "tick") == t_pause, "pause holds: %g vs %s", t_pause, st);
  free(st);

  /* ── crash isolation ─────────────────────────────────────────────── */
  int pid1 = cw_remote_pid(G);
  char *cerr = NULL;
  char *crep = cw_remote_call(G, native ? "debug_crash segv" : "debug_crash trap", 20000, &cerr);
  CHECK(!crep, "a crashing request gets no reply");
  free(crep);
  CHECK(wait_dead(10000), "the crash is detected");
  CHECK(cw_remote_state_of(G) == CW_REMOTE_CRASHED, "state crashed: %s (%s)", cw_remote_state_name(cw_remote_state_of(G)),
        cw_remote_status(G));
  CHECK(strstr(cw_remote_status(G), native ? "SIGSEGV" : "unreachable"), "the crash is named: %s", cw_remote_status(G));
  printf("  crash: %s (request error: %s)\n", cw_remote_status(G), cerr ? cerr : "");
  free(cerr);
  char *after = NULL;
  after = cw_remote_call(G, "status", 2000, &cerr);
  CHECK(!after && cerr && strstr(cerr, "relaunch"), "requests to a dead game fail and name the fix: %s", cerr ? cerr : "");
  free(after);
  free(cerr);
  cerr = NULL;

  CHECK(cw_remote_relaunch(G), "relaunch after the crash: %s", cw_remote_status(G));
  CHECK(cw_remote_launch_count(G) == 2, "second launch");
  if (native) CHECK(cw_remote_pid(G) != pid1 && cw_remote_pid(G) > 0, "a new process: %d vs %d", cw_remote_pid(G), pid1);
  /* a fresh game: the edits are gone, the clock restarted */
  char *fresh = must("get Level/Mover speed");
  CHECK(strstr(fresh, "\"value\":\"33.7\""), "the relaunched game is fresh (speed 33.7): %s", fresh);
  free(fresh);
  st = must("status");
  CHECK(num(st, "tick") == 1, "relaunched paused at tick 1: %s", st);
  free(st);

  /* deterministic stepping: N ticks from a fresh start, compared by
   * tests/remote.mjs with a CW_FRAMES run of 1 + N frames */
  cw_remote_step(G, N, 0, NULL);
  char *ref = must("get Level/Mover position");
  float rx = 0, ry = 0;
  vec2_of(ref, "value", &rx, &ry);
  free(ref);

  /* ── kill ────────────────────────────────────────────────────────── */
  cw_remote_kill(G);
  CHECK(cw_remote_state_of(G) == CW_REMOTE_CRASHED && strstr(cw_remote_status(G), "killed"), "killed: %s", cw_remote_status(G));
  CHECK(cw_remote_relaunch(G), "relaunch after a kill: %s", cw_remote_status(G));
  char *alive = must("status");
  CHECK(num(alive, "tick") == 1, "alive again: %s", alive);
  free(alive);

  /* ── a clean stop: quit, exit code 0 ────────────────────────────── */
  cw_remote_stop(G);
  CHECK(cw_remote_state_of(G) == CW_REMOTE_CLOSED, "stopped: %s", cw_remote_status(G));
  CHECK(cw_remote_send(G, "status", 0, NULL, NULL) == 0, "a closed game takes no requests");
  cw_remote_close(G);

  printf("{\"transport\":\"%s\",\"x_after_edit\":%.9g,\"x_fresh_step\":%.9g,\"ticks\":%d,\"checks\":%d,\"fails\":%d}\n", transport,
         x1, rx, N, checks, fails);
  printf("%s remote %s: %d checks, %d failed\n", fails ? "FAIL" : "ok  ", transport, checks, fails);
  return fails ? 1 : 0;
}

/* Play-in-editor exit criterion, scripted (M4.3, the editor side):
 * "Edit a property in a running game; pause, step, inspect."
 *
 *   editor_play_test <native|romdev> <editor exe> <project dir> <out dir> [--control]
 *
 * Starts the native EDITOR (headless) on a copy of the play_demo project and
 * drives it only through its own debug channel with `ed <command>` requests,
 * as an agent would. The editor plays the project's game on the given
 * transport (a native child process of the editor, or the game's cart in
 * romdev) and the test checks, through the editor:
 *   - Play starts the current scene paused at tick 1; the Remote panel syncs
 *     the game's tree; the Live panel loads the ball's properties;
 *   - a live edit (gravity_scale 0) is type-checked, and after `play.step 30`
 *     the GAME's state shows it: the ball did not fall, and the game's own
 *     HUD logic reports that height (not just the echo of the edit);
 *   - resume runs ticks, pause holds them; runtime spawns and frees reach the
 *     Remote tree through `changes`; layer screenshots show their layers;
 *   - play.apply copies the live value into the scene as one undoable step,
 *     and the next Play runs with it;
 *   - a deliberate crash is reported (state, signal, the game's output, the
 *     Game panel banner) while the editor keeps answering; relaunch works.
 * --control skips the live edit but still expects its effect: that run MUST
 * fail (tests/editor-play.mjs checks it does). */
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

static cw_remote *E; /* the editor */
static bool native;

/* an `ed` request; returns the editor's reply (malloc'd, never NULL) */
static char *ed(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *ed(const char *fmt, ...) {
  char q[4096] = "ed ";
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(q + 3, sizeof q - 3, fmt, ap);
  va_end(ap);
  char *err = NULL;
  char *rep = cw_remote_call(E, q, 60000, &err);
  if (!rep) {
    /* the editor's own {"ok":false} replies come back as game-side errors */
    char *s = malloc(strlen(err ? err : "?") + 64);
    sprintf(s, "{\"ok\":false,\"error\":\"%s\"}", err ? err : "?");
    free(err);
    return s;
  }
  return rep;
}
static bool ok_of(const char *rep) { return strstr(rep, "\"ok\":true") != NULL; }

/* an `ed` request that must succeed */
static char *must(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *must(const char *fmt, ...) {
  char q[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(q, sizeof q, fmt, ap);
  va_end(ap);
  char *r = ed("%s", q);
  checks++;
  if (!ok_of(r)) {
    fails++;
    printf("FAIL `ed %s`: %.600s\n", q, r);
  }
  return r;
}

static double num(const char *json, const char *key) {
  double v = NAN;
  cw_remote_json_number(json, key, &v);
  return v;
}

/* let the editor run frames (its panels poll the game between requests) */
static void frames(int n) {
  for (int i = 0; i < n; i++) free(ed("editor.state"));
}

/* poll an `ed` request until its reply contains `want` (or n tries pass) */
static char *until(const char *want, int tries, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static char *until(const char *want, int tries, const char *fmt, ...) {
  char q[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(q, sizeof q, fmt, ap);
  va_end(ap);
  char *r = NULL;
  for (int i = 0; i < tries; i++) {
    free(r);
    r = ed("%s", q);
    if (strstr(r, want)) return r;
    usleep(30000);
  }
  return r;
}

static bool vec3_of(const char *json, float *x, float *y, float *z) {
  const char *v = strstr(json, "vec3(");
  return v && sscanf(v, "vec3(%f, %f, %f)", x, y, z) == 3;
}

typedef struct { int drawn, orange, green, light; } pix;
static bool png_file_stats(const char *path, pix *p) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = malloc((size_t)len);
  size_t got = fread(buf, 1, (size_t)len, f);
  fclose(f);
  int w, h, n;
  unsigned char *px = stbi_load_from_memory(buf, (int)got, &w, &h, &n, 4);
  free(buf);
  if (!px) return false;
  memset(p, 0, sizeof *p);
  for (int i = 0; i < w * h; i++) {
    unsigned char *c = px + i * 4;
    if (c[0] + c[1] + c[2] > 40) p->drawn++;
    if (c[0] > 180 && c[0] > c[1] + 40 && c[1] > c[2] + 20) p->orange++;      /* the ball (AgX: about 226, 155, 116) */
    if (c[1] > 110 && c[0] < c[1] - 30 && c[2] < c[1] - 30) p->green++;       /* the floor */
    if (c[0] > 200 && c[1] > 200 && c[2] > 200) p->light++;                    /* HUD text */
  }
  stbi_image_free(px);
  return true;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: editor_play_test native|romdev <editor exe> <project dir> <out dir> [--control]\n");
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *transport = argv[1], *editor = argv[2], *project = argv[3], *out = argv[4];
  native = !strcmp(transport, "native");
  bool control = argc > 5 && !strcmp(argv[5], "--control");
  char settings[1024], env_settings[1100];
  snprintf(settings, sizeof settings, "%s/settings-%s%s.cfg", out, transport, control ? "-control" : "");
  unlink(settings);
  snprintf(env_settings, sizeof env_settings, "CW_EDITOR_SETTINGS=%s", settings);
  const char *env[] = { env_settings, NULL };
  const char *args[] = { project, NULL };
  E = cw_remote_launch(&(cw_remote_desc){ .transport = CW_REMOTE_NATIVE, .exe = editor, .args = args, .env = env,
                                          .headless = true, .timeout_ms = 60000, .launch_timeout_ms = 30000 });
  if (!E || cw_remote_state_of(E) != CW_REMOTE_RUNNING) {
    printf("FAIL the editor did not start: %s\n", E ? cw_remote_status(E) : cw_remote_last_error());
    return 1;
  }
  int editor_pid = cw_remote_pid(E);
  printf("ok   editor running (pid %d), game transport %s%s\n", editor_pid, transport, control ? " (CONTROL: no live edit)" : "");

  free(must("settings.set play.transport %s", transport));
  free(must("settings.set play.start_paused true"));

  /* ── nothing to inspect before Play: errors, not silence ───────────── */
  char *e0 = ed("play.step 5");
  CHECK(!ok_of(e0) && strstr(e0, "play.start"), "step with no game names the fix: %s", e0);
  free(e0);

  /* ── Play the current scene ───────────────────────────────────────── */
  char *st = must("play.start");
  CHECK(strstr(st, "\"scene\":\"main.cwscene\""), "plays the active scene: %s", st);
  free(st);
  st = must("play.status");
  CHECK(strstr(st, "\"state\":\"running\"") && strstr(st, "\"paused\":true") && num(st, "tick") == 1,
        "running, paused at tick 1: %.400s", st);
  CHECK(strstr(st, native ? "\"transport\":\"native\"" : "\"transport\":\"romdev\""), "transport %s: %.200s", transport, st);
  int game_pid = (int)num(st, "pid");
  if (native) CHECK(game_pid > 0 && game_pid != editor_pid, "the game is its own process: %d (editor %d)", game_pid, editor_pid);
  free(st);

  /* ── Remote tree: synced from the game ────────────────────────────── */
  char *tr = until("\"name\":\"Ball\"", 200, "play.tree");
  CHECK(strstr(tr, "\"synced\":true") && strstr(tr, "\"name\":\"Ball\"") && strstr(tr, "\"name\":\"Hud\""), "remote tree: %.500s", tr);
  CHECK(!strstr(tr, "(runtime)"), "every node of the played scene maps to the edited one: %.500s", tr);
  int nodes0 = (int)num(tr, "nodes");
  free(tr);

  /* ── inspect the ball ─────────────────────────────────────────────── */
  char *in = must("play.inspect Level/Ball");
  int uid = (int)num(in, "uid");
  CHECK(uid > 0 && strstr(in, "\"scene_path\":\"Ball\""), "the ball, and where it is in the scene: %s", in);
  free(in);
  char *w = until("\"gravity_scale\":{", 200, "play.widgets");
  CHECK(strstr(w, "\"gravity_scale\":{") && strstr(w, "vec3(-1.5, 4, 0)"), "Live panel rows (start values): %.300s",
        strstr(w, "\"live\"") ? strstr(w, "\"live\"") : w);
  free(w);

  /* ── a live edit, type-checked by the game ────────────────────────── */
  char *bad = ed("play.set node=#%d prop=gravity_scale value=\"vec3(1, 2, 3)\"", uid);
  CHECK(!ok_of(bad) && strstr(bad, "wants float"), "a wrong type is refused with the type named: %.300s", bad);
  free(bad);
  bad = ed("play.set node=#999999 prop=gravity_scale value=0");
  CHECK(!ok_of(bad) && strstr(bad, "999999"), "a missing node is named: %.300s", bad);
  free(bad);
  /* a string with spaces and quotes survives both channels as one value: the debug channel unescapes \" and \\ inside a quoted token, ed re-quotes it for the editor, the editor quotes it for the game (a key="a b" token would be split: use ed64 for those) */
  char *str = must("play.set Level/Hud text \"\\\"two words \\\\\\\"q\\\\\\\"\\\"\"");
  CHECK(strstr(str, "two words"), "string set: %s", str);
  free(str);
  str = must("play.get node=Level/Hud props=text");
  CHECK(strstr(str, "two words \\\\\\\"q\\\\\\\"") != NULL, "the game holds the exact string: %s", str);
  free(str);
  /* the HUD set above reaches the Remote panel's change log first: count from there */
  char *base_tr = until("\"set_events\":1", 60, "play.tree");
  int base_sets = (int)num(base_tr, "set_events");
  CHECK(base_sets == 1, "the Remote panel saw the HUD set in the change log: %.200s", base_tr);
  free(base_tr);
  if (!control) {
    char *s = must("play.set node=#%d prop=gravity_scale value=0", uid);
    CHECK(strstr(s, "\"value\":\"0\""), "set echoes 0: %s", s);
    free(s);
  }
  char *ch = until("\"set_events\":2", 60, "play.tree");
  CHECK(num(ch, "set_events") == base_sets + 1, "the Remote panel saw the live edit in the change log: %.200s", ch);
  free(ch);

  /* ── step: the game ran with the edited value ─────────────────────── */
  char *sp = must("play.step ticks=30");
  CHECK(num(sp, "tick") == 31, "step 30 from tick 1 lands on 31: %s", sp);
  free(sp);
  char *pos = must("play.get node=Level/Ball props=position");
  float x = 0, y = 0, z = 0;
  vec3_of(pos, &x, &y, &z);
  free(pos);
  CHECK(fabsf(y - 4.0f) < 1e-4f, "gravity_scale 0: the ball stayed at height 4 after 30 ticks (y %g)", y);
  CHECK(y > 3.5f, "control: an unedited ball falls about 1.3 m in 30 ticks (y %g)", y);
  char *hud = must("play.get node=Level/Hud props=text");
  CHECK(strstr(hud, "ball height 4.00 m") && strstr(hud, "tick 31"), "the game's own HUD logic reports it: %s", hud);
  free(hud);
  w = until("\"tick\":31,", 100, "play.widgets");
  CHECK(strstr(w, "\"live\":{\"uid\"") && strstr(w, "\"tick\":31,"), "the Live panel polled the stepped game: %.200s", strstr(w, "\"live\"") ? strstr(w, "\"live\"") : w);
  free(w);

  /* ── pause holds, resume runs ─────────────────────────────────────── */
  usleep(300000);
  st = must("play.status");
  CHECK(num(st, "tick") == 31, "paused: no ticks while 300 ms pass: %.300s", st);
  free(st);
  char *rs = must("play.pause on=false");
  free(rs);
  usleep(400000);
  char *ps = must("play.pause on=true");
  double t_paused = num(ps, "tick");
  CHECK(t_paused > 31 + 5, "resumed, the game ran ticks on its own: %s", ps);
  free(ps);
  usleep(200000);
  st = must("play.status");
  CHECK(num(st, "tick") == t_paused, "paused again it holds: %g vs %.200s", t_paused, st);
  free(st);

  /* ── the Remote tree follows runtime changes ──────────────────────── */
  free(must("play.request \"spawn OmniLight3D /root/Level Lamp\""));
  tr = until("Lamp", 100, "play.tree");
  CHECK(strstr(tr, "\"name\":\"Lamp\"") && strstr(tr, "(runtime)"), "a spawned node appears, marked runtime: %.600s", tr);
  CHECK(num(tr, "nodes") == nodes0 + 1, "one more node: %g vs %d", num(tr, "nodes"), nodes0);
  free(tr);
  free(must("play.request \"free /root/Level/Lamp\""));
  tr = until("\"nodes\":", 1, "play.tree");
  for (int i = 0; i < 100 && strstr(tr, "Lamp"); i++) {
    free(tr);
    tr = ed("play.tree");
  }
  CHECK(!strstr(tr, "Lamp") && num(tr, "nodes") == nodes0, "a freed node leaves: %.300s", tr);
  free(tr);

  /* ── layer screenshots ────────────────────────────────────────────── */
  char pw[1200], pu[1200], pa[1200];
  snprintf(pw, sizeof pw, "%s/%s%s-world.png", out, transport, control ? "-control" : "");
  snprintf(pu, sizeof pu, "%s/%s%s-ui.png", out, transport, control ? "-control" : "");
  snprintf(pa, sizeof pa, "%s/%s%s-all.png", out, transport, control ? "-control" : "");
  free(must("play.screenshot layers=world path=%s", pw));
  free(must("play.screenshot layers=ui path=%s", pu));
  free(must("play.screenshot layers=all path=%s", pa));
  pix sw, su, sa;
  CHECK(png_file_stats(pw, &sw) && png_file_stats(pu, &su) && png_file_stats(pa, &sa), "layer PNGs readable");
  printf("  layers (drawn/orange/green/light): world %d/%d/%d/%d  ui %d/%d/%d/%d  all %d/%d/%d/%d\n", sw.drawn, sw.orange, sw.green,
         sw.light, su.drawn, su.orange, su.green, su.light, sa.drawn, sa.orange, sa.green, sa.light);
  CHECK(sw.orange > 200 && sw.green > 2000 && sw.light < 20, "world: ball and floor, no HUD text");
  CHECK(su.light > 30 && su.orange == 0 && su.green == 0, "ui: the HUD text only");
  CHECK(sa.orange > 200 && sa.light > 30, "all: both");
  w = ed("play.widgets");
  CHECK(strstr(w, "\"has_frame\":true"), "the Game panel shows the capture: %.200s", strstr(w, "\"game\"") ? strstr(w, "\"game\"") : w);
  free(w);

  /* ── keep the live value in the scene (undoable) ──────────────────── */
  char *ap = must("play.apply path=Ball prop=gravity_scale");
  CHECK(strstr(ap, "\"value\":\"0\""), "the live value is applied: %s", ap);
  free(ap);
  char *g = must("node.get Ball gravity_scale");
  CHECK(strstr(g, "\"value\":\"0\""), "the edited scene has it: %s", g);
  free(g);
  char *hl = must("history.list");
  CHECK(strstr(hl, "play.apply path=Ball prop=gravity_scale"), "one history entry: %.300s", hl);
  free(hl);
  free(must("edit.undo"));
  g = must("node.get Ball gravity_scale");
  CHECK(strstr(g, "\"value\":\"1\""), "undo restores the scene value: %s", g);
  free(g);
  free(must("edit.redo"));
  g = must("node.get Ball gravity_scale");
  CHECK(strstr(g, "\"value\":\"0\""), "redo applies it again without the game: %s", g);
  free(g);

  /* ── crash on purpose: reported, the editor unaffected ────────────── */
  char *cr = must("play.crash kind=%s", native ? "segv" : "trap");
  CHECK(strstr(cr, "\"state\":\"crashed\""), "crashed: %.300s", cr);
  CHECK(strstr(cr, native ? "SIGSEGV" : "unreachable"), "the crash is named: %.400s", cr);
  free(cr);
  char *lt = must("log.tail 30");
  CHECK(strstr(lt, "crashed") && strstr(lt, "play.relaunch"), "the Output panel has the report and the way back: %.200s", lt);
  free(lt);
  w = must("play.widgets");
  CHECK(strstr(w, "CRASHED"), "the Game panel banner shows the crash: %.300s", strstr(w, "\"banner\"") ? strstr(w, "\"banner\"") : w);
  free(w);
  char *es = must("editor.state");
  CHECK(strstr(es, "\"running\":false"), "the editor answers, the game is down: %.200s", strstr(es, "\"play\"") ? strstr(es, "\"play\"") : es);
  free(es);
  char *dead = ed("play.get node=Level/Ball props=position");
  CHECK(!ok_of(dead) && strstr(dead, "relaunch"), "a request to the dead game names the fix: %s", dead);
  free(dead);

  /* ── relaunch: a fresh game ───────────────────────────────────────── */
  char *rl = must("play.relaunch");
  CHECK(strstr(rl, "\"state\":\"running\"") && num(rl, "launches") == 2 && num(rl, "tick") == 1, "relaunched: %.300s", rl);
  if (native) CHECK((int)num(rl, "pid") != game_pid, "a new process");
  free(rl);
  g = must("play.get node=Level/Ball props=gravity_scale");
  CHECK(strstr(g, "\"value\":\"1\""), "relaunch replays the SAVED scene (the kept value is not saved yet): %s", g);
  free(g);

  /* ── Play again: the core saves the scene first, the kept value plays ─ */
  free(must("play.start"));
  g = must("play.get node=Level/Ball props=gravity_scale");
  CHECK(strstr(g, "\"value\":\"0\""), "the kept value is in the played scene now: %s", g);
  free(g);
  free(must("play.step ticks=30"));
  pos = must("play.get node=Level/Ball props=position");
  float y2 = 0;
  vec3_of(pos, &x, &y2, &z);
  free(pos);
  CHECK(fabsf(y2 - 4.0f) < 1e-4f, "and the game behaves by it with no live edit (y %g)", y2);

  /* the editor's own picture with the game in it */
  char pe[1200];
  snprintf(pe, sizeof pe, "%s/%s%s-editor.png", out, transport, control ? "-control" : "");
  free(must("panel.show id=game"));
  free(must("panel.show id=remote_tree"));
  free(must("panel.show id=live"));
  free(must("play.inspect Level/Ball"));
  frames(30);
  char *serr = NULL;
  if (!cw_remote_screenshot(E, "all", 1, pe, NULL, NULL, 20000, &serr)) printf("  (editor screenshot failed: %s)\n", serr ? serr : "?");
  free(serr);

  free(must("play.stop"));
  st = must("play.status");
  CHECK(strstr(st, "\"state\":\"idle\""), "stopped: %.200s", st);
  free(st);
  cw_remote_close(E);
  printf("{\"transport\":\"%s\",\"y_after_edit\":%.9g,\"y_kept\":%.9g,\"checks\":%d,\"fails\":%d}\n", transport, y, y2, checks, fails);
  printf("%s editor-play %s: %d checks, %d failed\n", fails ? "FAIL" : "ok  ", transport, checks, fails);
  return fails ? 1 : 0;
}

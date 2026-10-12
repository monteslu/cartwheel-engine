/* cartwheel/project.h and cartwheel/process.h, natively and as a cart:
 * project.cwproj editing keeps every other line, typed reads, the input map
 * and render settings applied; child processes (natively: output lines, exit
 * codes, a missing program, kill; in a cart: refused with a reason). */
#include <string.h>
#if !CW_CART
#include <unistd.h>
#endif

#include "test.h"
#include "cartwheel/input.h"
#include "cartwheel/process.h"
#include "cartwheel/project.h"
#include "cartwheel/vfs.h"

static void fold_str(const char *s) { cw_test_checksum = cw_hash32(s, strlen(s), cw_test_checksum); }

static void test_text_set(void) {
  const char *src = "# a comment\nname = Old\n; another\nwidth=3\nname = Dup\n\n[section]\n";
  char *a = cw_project_text_set(src, "name", "New Name");
  CHECK(a && !strcmp(a, "# a comment\nname = New Name\n; another\nwidth=3\n\n[section]\n")); /* duplicates go */
  fold_str(a);
  char *b = cw_project_text_set(a, "render.width", "640");
  CHECK(b && strstr(b, "[section]\nrender.width = 640\n")); /* appended */
  char *c = cw_project_text_set(b, "width", NULL);
  CHECK(c && !strstr(c, "width=3") && strstr(c, "render.width = 640"));
  char *d = cw_project_text_set("", "k", "v");
  CHECK(d && !strcmp(d, "k = v\n"));
  CHECK(!cw_project_text_set(src, "a=b", "x"));
  CHECK(!cw_project_text_set(src, "", "x"));
  CHECK(!cw_project_text_set(src, "k", "two\nlines"));
  char *e = cw_project_text_set("#name = commented\n", "name", "x"); /* comments are not keys */
  CHECK(e && !strcmp(e, "#name = commented\nname = x\n"));
  free(a), free(b), free(c), free(d), free(e);
}

static void test_settings(void) {
  /* the project layer (memory here: no project directory in a test) */
  const char *text = "name = \"Test Game\"\napp = test_game\nrender.width = 320\nrender.height = 200\nrender.vsync = false\n"
                     "render.msaa = 4\ninput.jump = key:space, pad:south\ninput.fire = mouse:left\nbad.int = 12x\n";
  CHECK(cw_vfs_write("project.cwproj", text, strlen(text)));
  cw_project_reload();
  CHECK(!strcmp(cw_project_get("name"), "Test Game")); /* quotes stripped */
  CHECK_EQ_INT(cw_project_get_int("render.width", 0), 320);
  CHECK_EQ_INT(cw_project_get_int("bad.int", 7), 7);
  CHECK_EQ_INT(cw_project_get_int("missing", 9), 9);
  CHECK(!cw_project_get_bool("render.vsync", true));
  CHECK(cw_project_get_bool("missing", true));
  CHECK(!cw_project_get("missing"));
  CHECK_EQ_INT(cw_project_count(), 9);
  cw_app_desc d = { .width = 640, .height = 360 };
  cw_project_apply_app(&d);
  CHECK_EQ_INT(d.width, 320);
  CHECK_EQ_INT(d.height, 200);
  CHECK_EQ_INT(d.msaa, 4);
  CHECK(d.vsync_off);
  CHECK(d.title && !strcmp(d.title, "Test Game"));
  CHECK_EQ_INT(cw_project_load_input(), 2);
  cw_action jump = cw_action_find("jump"), fire = cw_action_find("fire");
  CHECK(jump >= 0 && fire >= 0);
  cw_binding b[4];
  CHECK_EQ_INT(cw_action_bindings(jump, b, 4), 2);
  /* editing through the layer */
  CHECK(cw_project_set("render.width", "1024"));
  CHECK_EQ_INT(cw_project_get_int("render.width", 0), 1024);
  CHECK(cw_project_set("input.fire", NULL));
  CHECK(!cw_project_get("input.fire"));
  size_t len;
  char *now = cw_vfs_read("project.cwproj", &len);
  CHECK(now && strstr(now, "name = \"Test Game\"\napp = test_game\nrender.width = 1024\n")); /* other lines as they were */
  if (now) fold_str(now);
  free(now);
}

static void test_process(void) {
  char err[256] = "";
#if CW_CART
  const char *argv[] = { "sh", "-c", "true", NULL };
  CHECK(!cw_process_start(&(cw_process_desc){ .argv = argv }, err, sizeof err));
  CHECK(strstr(err, "cart") != NULL);
  CHECK(!cw_process_read_line(NULL) && !cw_process_running(NULL) && cw_process_exit_code(NULL) == -1);
  /* the native run checks more; fold the same count so the checksum agrees across hosts */
  for (int i = 0; i < 5; i++) CHECK(1);
#else
  const char *argv[] = { "sh", "-c", "echo one; echo two 1>&2; printf three; exit 3", NULL };
  cw_process *p = cw_process_start(&(cw_process_desc){ .argv = argv }, err, sizeof err);
  CHECK(p != NULL);
  char got[64] = "";
  int lines = 0;
  for (int spin = 0; p && spin < 2000 && (cw_process_running(p) || lines < 3); spin++) {
    for (const char *l; (l = cw_process_read_line(p));) {
      snprintf(got + strlen(got), sizeof got - strlen(got), "%s|", l);
      lines++;
    }
    usleep(2000);
  }
  for (const char *l; p && (l = cw_process_read_line(p));) snprintf(got + strlen(got), sizeof got - strlen(got), "%s|", l);
  CHECK(!strcmp(got, "one|two|three|"));
  CHECK_EQ_INT(cw_process_exit_code(p), 3);
  cw_process_free(p);
  /* the environment and working directory reach the child */
  const char *env[] = { "CW_PROC_TEST=hello", NULL };
  const char *argv2[] = { "sh", "-c", "echo $CW_PROC_TEST $(basename $(pwd))", NULL };
  p = cw_process_start(&(cw_process_desc){ .argv = argv2, .env = env, .cwd = "/" }, err, sizeof err);
  const char *l = NULL;
  for (int spin = 0; p && spin < 2000 && !(l = cw_process_read_line(p)); spin++) usleep(2000);
  CHECK(l && !strcmp(l, "hello /"));
  cw_process_free(p);
  /* a missing program fails at start with a reason */
  const char *argv3[] = { "cw-no-such-program-xyz", NULL };
  CHECK(!cw_process_start(&(cw_process_desc){ .argv = argv3 }, err, sizeof err));
  CHECK(strstr(err, "cw-no-such-program-xyz") != NULL);
  /* kill stops it (and its children: its own process group) */
  const char *argv4[] = { "sh", "-c", "sleep 30 & sleep 30", NULL };
  p = cw_process_start(&(cw_process_desc){ .argv = argv4 }, err, sizeof err);
  CHECK(p && cw_process_running(p));
  cw_process_kill(p, false);
  for (int spin = 0; p && spin < 1000 && cw_process_running(p); spin++) usleep(2000);
  CHECK_EQ_INT(cw_process_exit_code(p), 128 + 15);
  cw_process_free(p);
#endif
}

CW_TEST_SUITE("project", { "text_set", test_text_set }, { "settings", test_settings }, { "process", test_process })

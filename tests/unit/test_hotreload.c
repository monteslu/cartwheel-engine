/* Hot reload (native dev builds): a copy of the resource suite's project is
 * packed by cwpack, loaded, then its sources are edited on disk; one poll of
 * the watcher reruns cwpack, remounts the pack and reloads what changed IN
 * PLACE: handles keep working, a texture keeps its t3_texture, a glTF
 * instance keeps its Group and gets the new geometry, untouched resources are
 * not reloaded. Native only (a cart reads its bundle, not files); the
 * scratch project lives beside the executable in the build tree. */
#include <stdlib.h>
#include <unistd.h>

#include "cartwheel/resource.h"
#include "test.h"
#include "three.h"

static char exe_dir[1024], proj[1200], out[1200], cwpack[1200], args[4096];

static bool sh(const char *fmt, const char *a, const char *b) {
  char cmd[4096];
  snprintf(cmd, sizeof cmd, fmt, a, b);
  return system(cmd) == 0;
}

static bool setup(void) {
  char self[1024];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0) return false;
  self[n] = 0;
  snprintf(exe_dir, sizeof exe_dir, "%s", self);
  *strrchr(exe_dir, '/') = 0;
  /* build/native/tests/unit -> the repo's tests/unit/res_project */
  char src[1200];
  snprintf(src, sizeof src, "%s/../../../../tests/unit/res_project", exe_dir);
  snprintf(proj, sizeof proj, "%s/hotreload.project", exe_dir);
  snprintf(out, sizeof out, "%s/hotreload.out", exe_dir);
  snprintf(cwpack, sizeof cwpack, "%s/../../tools/cwpack/cwpack", exe_dir);
  sh("rm -rf '%s' '%s'", proj, out);
  if (!sh("cp -R '%s' '%s'", src, proj)) return false;
  sh("rm -rf '%s/.cwcache'%s", proj, "");
  /* the deps the manifest points at are relative to the original project */
  snprintf(args, sizeof args, "--manifest '%s/roots.txt' --out '%s' --pak '%s/game.cwpak' -q", proj, out, out);
  char cmd[8800];
  snprintf(cmd, sizeof cmd, "sed -i 's#= \\.\\./\\.\\./\\.\\./deps#= %s/../../../../deps#' '%s/roots.txt'", exe_dir, proj);
  if (system(cmd)) return false;
  snprintf(cmd, sizeof cmd, "'%s' %s", cwpack, args);
  if (system(cmd)) return false;
  snprintf(cmd, sizeof cmd, "%s/game.cwpak", out);
  setenv("CW_PAK", cmd, 1);
  cw_asset_remount();
  return true;
}

static uint64_t geo_hash(t3_object *o, uint64_t h) {
  if (o->type == T3_MESH) {
    t3_attribute *p = ((t3_mesh *)o)->geometry->attributes[T3_ATTR_POSITION];
    h = cw_hash_bytes(p->array, (size_t)p->count * 12, h);
  }
  for (int i = 0; i < o->child_count; i++) h = geo_hash(o->children[i], h);
  return h;
}

static void test_hotreload(void) {
  bool ok = setup();
  CHECK(ok);
  if (!ok) return;
  cw_res text = cw_res_load("text/hello.txt", 0);
  cw_res tex = cw_res_load("textures/small.png", 0);
  cw_res quad = cw_res_load("models/quad.gltf", 0);
  cw_res json = cw_res_load("data/config.json", 0);
  CHECK(cw_res_is_ready(text) && cw_res_is_ready(tex) && cw_res_is_ready(quad) && cw_res_is_ready(json));
  t3_texture *t3t = cw_res_texture(tex);
  int w0 = t3t ? t3t->width : 0;
  t3_object *inst = cw_gltf_instantiate(quad);
  uint64_t h0 = inst ? geo_hash(inst, 0) : 0;
  CHECK(cw_res_hotreload_watch(cwpack, args, out, 1000000)); /* polled by hand below */
  CHECK_EQ_INT(cw_res_hotreload_poll(), 0); /* nothing changed yet */
  /* edit three sources */
  char p[1400], q[1400];
  snprintf(p, sizeof p, "%s/text/hello.txt", proj);
  FILE *f = fopen(p, "wb");
  fputs("hello again\n", f);
  fclose(f);
  snprintf(p, sizeof p, "%s/textures/checker.png", proj);
  snprintf(q, sizeof q, "%s/textures/small.png", proj);
  sh("cp '%s' '%s'", p, q);
  snprintf(p, sizeof p, "%s/models/quad.bin", proj);
  f = fopen(p, "r+b");
  float y;
  fseek(f, 4, SEEK_SET);
  if (fread(&y, 4, 1, f) == 1) {
    y += 0.25f;
    fseek(f, 4, SEEK_SET);
    fwrite(&y, 4, 1, f);
  }
  fclose(f);
  int n = cw_res_hotreload_poll();
  CHECK_EQ_INT(n, 3); /* text, small.png, quad.gltf (its .bin changed); checker.png, quad's image, did not */
  CHECK(cw_res_blob(text) && cw_res_blob(text)->len == 12 && !memcmp(cw_res_blob(text)->data, "hello again\n", 12));
  CHECK_EQ_INT(cw_res_version(text), 2);
  CHECK(cw_res_texture(tex) == t3t && w0 == 16 && t3t->width == 64); /* same t3_texture, new image */
  CHECK_EQ_INT(cw_res_version(json), 1); /* untouched: not reloaded */
  CHECK_EQ_INT(cw_gltf_instance_count(quad), 1);
  CHECK(inst && inst->child_count == 1 && geo_hash(inst, 0) != h0); /* same Group, rebuilt children */
  CHECK_EQ_INT(cw_res_version(quad), 2);
  CHECK_EQ_INT(cw_res_hotreload_poll(), 0); /* settled */
  cw_res_hotreload_stop();
  t3_release(inst);
  cw_res_release(text);
  cw_res_release(tex);
  cw_res_release(quad);
  cw_res_release(json);
}

CW_TEST_SUITE("hotreload", { "hotreload", test_hotreload })

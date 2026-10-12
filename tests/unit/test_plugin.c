/* Plugins (cartwheel/plugin.h, M5.2), on every host: the build's plugin
 * table (cw_add_app compiled plugin_fixtures/good in, recorded future as
 * refused by API version, off as disabled, needs_future as refused by its
 * dependency), the API version rules, load-time refusal of code compiled
 * for another API, unload (refused while nodes exist) and load again,
 * errors that name the plugin providing an unknown node type, and manifest
 * parsing. */
#include <string.h>

#include "cartwheel/plugin.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "test.h"

int good_register_count(void); /* plugin_fixtures/good */

static bool contains(const char *s, const char *sub) { return s && strstr(s, sub) != NULL; }

static void test_table(void) {
  /* the platform ran cw_plugins_init before init: the table is registered */
  const cw_plugin_info *g = cw_plugin_find("good"), *f = cw_plugin_find("future"), *o = cw_plugin_find("off"),
                       *n = cw_plugin_find("needs_future");
  CHECK(g && f && o && n);
  if (!g || !f || !o || !n) return;
  CHECK_EQ_INT(g->state, CW_PLUGIN_LOADED);
  CHECK(!strcmp(g->version, "2.3.4"));
  CHECK_EQ_INT(g->types_registered, 1);
  CHECK_EQ_INT(good_register_count(), 1);
  CHECK(cw_node_type_find("PluginThing") != NULL);
  CHECK_EQ_INT(f->state, CW_PLUGIN_REFUSED);
  CHECK(contains(f->error, "needs engine API 2.0") && contains(f->error, "provides 1.0"));
  CHECK_EQ_INT(o->state, CW_PLUGIN_DISABLED);
  CHECK(contains(o->error, "enabled = false"));
  CHECK_EQ_INT(n->state, CW_PLUGIN_REFUSED);
  CHECK(contains(n->error, "depends on plugin 'future'"));
  /* nothing of the refused / disabled ones exists */
  CHECK(cw_node_type_find("FutureThing") == NULL && cw_node_type_find("OffThing") == NULL && cw_node_type_find("NeedsThing") == NULL);
  int32_t states = 0;
  for (int i = 0; i < cw_plugin_count(); i++) states = states * 7 + (int32_t)cw_plugin_at(i)->state;
  FOLD(states);
  /* the debug command the plugin registered answers */
  char *r = cw_debug_run("good_plugin");
  CHECK(contains(r, "\"good\":1"));
  cw_free(r);
  r = cw_debug_run("plugins");
  CHECK(contains(r, "\"name\":\"good\"") && contains(r, "\"state\":\"refused\"") && contains(r, "\"state\":\"disabled\""));
  cw_free(r);
}

static void test_versions(void) {
  char err[512];
  CHECK(cw_plugin_api_compatible(CW_API_VERSION(1, 0), err, sizeof err));
  CHECK(!cw_plugin_api_compatible(CW_API_VERSION(1, 1), err, sizeof err) && contains(err, "provides only 1.0"));
  CHECK(!cw_plugin_api_compatible(CW_API_VERSION(2, 0), err, sizeof err) && contains(err, "different major"));
  CHECK(!cw_plugin_api_compatible(CW_API_VERSION(0, 9), err, sizeof err));
  uint32_t api = 0;
  CHECK(cw_plugin_api_parse("1.0", &api) && api == CW_API_VERSION(1, 0));
  CHECK(cw_plugin_api_parse("1", &api) && api == CW_API_VERSION(1, 0));
  CHECK(cw_plugin_api_parse("3.12", &api) && api == CW_API_VERSION(3, 12));
  CHECK(!cw_plugin_api_parse("banana", &api) && !cw_plugin_api_parse("1.x", &api) && !cw_plugin_api_parse("", &api));
  FOLD(api);
}

/* code compiled against another API (a stale native plugin library) is refused at load */
static int stale_registered;
static void stale_reg(void) { stale_registered++; }
static void test_load_refusal(void) {
  static const cw_plugin stale = { .api = CW_API_VERSION(1, 7), .name = "stale", .version = "9", .register_fn = stale_reg };
  char err[512] = "";
  CHECK(!cw_plugin_load(&stale, err, sizeof err));
  CHECK(contains(err, "compiled for engine API 1.7") && contains(err, "provides only 1.0"));
  CHECK_EQ_INT(stale_registered, 0);
  const cw_plugin_info *s = cw_plugin_find("stale");
  CHECK(s && s->state == CW_PLUGIN_REFUSED);
  static const cw_plugin fine = { .api = CW_ENGINE_API, .name = "stale", .version = "9", .register_fn = stale_reg };
  CHECK(cw_plugin_load(&fine, err, sizeof err));
  CHECK_EQ_INT(stale_registered, 1);
  CHECK(s && s->state == CW_PLUGIN_LOADED);
}

static void test_unload(void) {
  char err[512] = "";
  cw_node a = cw_node_new("PluginThing", "A");
  CHECK(cw_node_valid(a));
  cw_node first;
  CHECK_EQ_INT(cw_node_type_live_count(cw_node_type_find("PluginThing"), &first), 1);
  CHECK(first.id == a.id);
  /* refused while a node of its type exists */
  CHECK(!cw_plugin_unload("good", err, sizeof err) && contains(err, "PluginThing still exist"));
  CHECK(cw_node_type_find("PluginThing") != NULL);
  cw_node_free(a);
  CHECK(cw_plugin_unload("good", err, sizeof err));
  CHECK(cw_node_type_find("PluginThing") == NULL);
  CHECK(cw_node_new("PluginThing", "B").id == 0);
  CHECK(contains(cw_plugin_type_hint("PluginThing"), "was unloaded"));
  CHECK_EQ_INT(cw_plugin_find("good")->state, CW_PLUGIN_UNLOADED);
  /* load it again: the same type comes back (register_fn runs again) */
  extern const cw_plugin cw_plugin_good;
  CHECK(cw_plugin_load(&cw_plugin_good, err, sizeof err));
  CHECK_EQ_INT(good_register_count(), 2);
  const cw_node_type *t = cw_node_type_find("PluginThing");
  CHECK(t != NULL && !cw_node_type_withdrawn(t));
  cw_node b = cw_node_new("PluginThing", "B");
  CHECK(cw_node_valid(b) && cw_node_set(b, "value", cw_val_float(2.5)));
  cw_value v;
  CHECK(cw_node_get_prop(b, "value", &v) && v.f == 2.5);
  cw_node_free(b);
  CHECK_EQ_INT(cw_plugin_find("good")->state, CW_PLUGIN_LOADED);
}

/* errors about a type a plugin provides name the plugin and why it is missing */
static char captured[2048];
static void capture(cw_log_level level, const char *msg, void *user) {
  (void)user;
  if (level == CW_LOG_ERROR && strlen(captured) + strlen(msg) + 2 < sizeof captured) strcat(strcat(captured, msg), "\n");
}
static void test_hints(void) {
  CHECK(contains(cw_plugin_type_hint("OffThing"), "plugin 'off' provides it, but it is disabled"));
  CHECK(contains(cw_plugin_type_hint("FutureThing"), "refused") && contains(cw_plugin_type_hint("FutureThing"), "2.0"));
  CHECK(cw_plugin_type_hint("NoSuchThing") == NULL);
  captured[0] = 0;
  int id = cw_log_add_listener(capture, NULL);
  cw_packed_scene *ps = cw_scene_parse("[scene format=1]\n\n[node name=\"Root\" type=\"Node3D\"]\n\n"
                                       "[node name=\"Off\" parent=\".\" type=\"OffThing\"]\n\n"
                                       "[node name=\"Child\" parent=\"Off\" type=\"Node3D\"]\n",
                                       "fixture.cwscene");
  cw_node root = cw_scene_instance(ps);
  cw_log_remove_listener(id);
  CHECK(cw_node_valid(root) && cw_node_child_count(root) == 0);
  CHECK(contains(captured, "unknown node type 'OffThing': plugin 'off' provides it, but it is disabled"));
  CHECK(contains(captured, "fixture.cwscene: node 'Off' (type OffThing) and its children were not created"));
  cw_node_free(root);
  cw_packed_scene_free(ps);
}

static void test_manifest(void) {
  cw_plugin_manifest m;
  char err[256];
  CHECK(cw_plugin_manifest_parse("# c\nname = spin_2\nversion = \"1.2\"\nengine_api = 1.0\nkind = both\nprovides = A, B\n"
                                 "depends = x\nenabled = false\ntitle = Spin\n", &m, err, sizeof err));
  CHECK(!strcmp(m.name, "spin_2") && !strcmp(m.version, "1.2") && m.runtime && m.editor && !m.enabled);
  CHECK(!strcmp(m.provides, "A, B") && !strcmp(m.depends, "x") && !strcmp(m.title, "Spin"));
  CHECK(!strcmp(m.runtime_sources, "runtime") && !strcmp(m.editor_sources, "editor"));
  CHECK(cw_plugin_manifest_parse("name = r\nengine_api = 1\n", &m, err, sizeof err) && m.runtime && !m.editor && m.enabled);
  CHECK(cw_plugin_manifest_parse("name = e\nengine_api = 1.0\nkind = editor\n", &m, err, sizeof err) && !m.runtime && m.editor);
  CHECK(!cw_plugin_manifest_parse("name = Bad-Name\nengine_api = 1.0\n", &m, err, sizeof err) && contains(err, "lower_case"));
  CHECK(!cw_plugin_manifest_parse("name = ok\n", &m, err, sizeof err) && contains(err, "engine_api"));
  CHECK(!cw_plugin_manifest_parse("name = ok\nengine_api = 1.0\nkind = sometimes\n", &m, err, sizeof err) && contains(err, "kind"));
}

CW_TEST_SUITE("plugin", { "table", test_table }, { "versions", test_versions }, { "load_refusal", test_load_refusal },
              { "unload", test_unload }, { "hints", test_hints }, { "manifest", test_manifest })

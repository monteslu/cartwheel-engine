#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/vfs.h"
#include "test.h"
#include "../../src/scene/render3d_internal.h"
void cw__mesh3d_set_lightmap(cw_node n, t3_geometry *g, t3_texture *lm, float intensity, bool baked);
#include <string.h>
void cw__scene_cache_put(const char *path, cw_packed_scene *ps); /* engine internal (scene_file.c) */
t3_mesh *cw__mesh3d_t3(cw_node n);                                 /* engine internal (render3d.c) */

/* a test node type that records its lifecycle calls */
typedef struct { int value; float speed; int fixed_calls, update_calls; } probe;
static char log_buf[4096];
static void logc(cw_node n, const char *what) {
  char tmp[128];
  snprintf(tmp, sizeof tmp, "%s:%s ", what, cw_node_name(n));
  strncat(log_buf, tmp, sizeof log_buf - strlen(log_buf) - 1);
}
static void p_enter(cw_node n, void *d) { (void)d; logc(n, "enter"); }
static void p_ready(cw_node n, void *d) { (void)d; logc(n, "ready"); }
static void p_exit(cw_node n, void *d) { (void)d; logc(n, "exit"); }
static void p_fixed(cw_node n, void *d, double dt) { (void)n; (void)dt; ((probe *)d)->fixed_calls++; }
static void p_update(cw_node n, void *d, double dt) { (void)n; (void)dt; ((probe *)d)->update_calls++; }
static const cw_prop probe_props[] = {
  CW_PROP_FIELD(probe, value, CW_T_INT, CW_PROP_DEFAULT, "an int"),
  CW_PROP_FIELD_RANGE(probe, speed, CW_T_FLOAT, 0, 10, "a clamped float"),
  CW_PROP_END,
};
static const char *const probe_signals[] = { "hit", NULL };
static cw_node_type probe_type = { .name = "Probe", .parent = "Node3D", .data_size = sizeof(probe), .props = probe_props,
                                   .signals = probe_signals, .enter_tree = p_enter, .ready = p_ready, .exit_tree = p_exit,
                                   .fixed_update = p_fixed, .update = p_update };

static void setup(void) {
  cw_scene_register_builtin_types();
  cw_node_type_register(&probe_type);
}

static void test_lifecycle_and_paths(void) {
  setup();
  cw_tree *t = cw_tree_new();
  log_buf[0] = 0;
  cw_node a = cw_node_new("Probe", "A"), b = cw_node_new("Probe", "B"), c = cw_node_new("Probe", "C");
  cw_node_add_child(a, b);
  cw_node_add_child(b, c);
  cw_node_add_child(cw_tree_root(t), a);
  CHECK(strcmp(log_buf, "enter:A enter:B enter:C ready:C ready:B ready:A ") == 0);
  CHECK(cw_node_get(a, "B/C").id == c.id);
  CHECK(cw_node_get(c, "../..").id == a.id);
  CHECK(cw_node_get(c, "/root/A/B").id == b.id);
  CHECK(cw_node_get(a, "%C").id == c.id);
  char path[256];
  cw_node_path(c, (cw_node){ 0 }, path, sizeof path);
  CHECK(strcmp(path, "/root/A/B/C") == 0);
  cw_node_path(c, a, path, sizeof path);
  CHECK(strcmp(path, "B/C") == 0);
  /* unique sibling names */
  cw_node b2 = cw_node_new("Probe", "B");
  cw_node_add_child(a, b2);
  CHECK(strcmp(cw_node_name(b2), "B2") == 0);
  CHECK(cw_node_by_uid(t, cw_node_uid(c)).id == c.id);
  /* exit order: children first */
  log_buf[0] = 0;
  cw_node_free(b);
  CHECK(strcmp(log_buf, "exit:C exit:B ") == 0);
  CHECK(!cw_node_valid(c) && !cw_node_valid(b));
  CHECK_EQ_INT(cw_node_child_count(a), 1);
  CHECK_EQ_INT(cw_tree_node_count(t), 3); /* root, A, B2 */
  /* names are counted strings: nodes with unique names (one per spawn) come
   * and go without piling up, and lookups of names nothing has find nothing */
  uint32_t base = cw_intern_counted();
  for (int i = 0; i < 2000; i++) {
    cw_node s = cw_node_new("Probe", cw_frame_printf("spawn_%d", i));
    cw_node_add_child(a, s);
    CHECK(cw_node_get(a, cw_frame_printf("spawn_%d", i)).id == s.id);
    if (i % 3 == 0) cw_node_set_name(s, cw_frame_printf("renamed_%d", i));
    cw_node_free(s);
  }
  CHECK_EQ_INT((int)cw_intern_counted(), (int)base);
  CHECK(CW_HANDLE_IS_NULL(cw_node_get(a, "spawn_7")) && CW_HANDLE_IS_NULL(cw_node_get(a, "%never_named")));
  CHECK(cw_node_get(a, "B2").id == b2.id);
  cw_tree_free(t);
}

static cw_node to_free;
static void freeing_fixed(cw_node n, void *d, double dt) {
  (void)d; (void)dt;
  if (cw_node_valid(to_free)) cw_node_free(to_free); /* deferred: we are inside processing */
  (void)n;
}
static cw_node_type freeing_type = { .name = "Freeing", .parent = "Node", .fixed_update = freeing_fixed };

static void test_processing_and_deferred(void) {
  setup();
  cw_node_type_register(&freeing_type);
  cw_tree *t = cw_tree_new();
  cw_node f = cw_node_new("Freeing", NULL), p = cw_node_new("Probe", "P");
  cw_node_add_child(cw_tree_root(t), f);
  cw_node_add_child(cw_tree_root(t), p);
  to_free = p;
  probe *pd = cw_node_data(p, "Probe");
  cw_tree_fixed_update(t, 1.0 / 60);
  /* p was freed during processing: it still ran this phase? It was queued
   * BEFORE its turn, so it must not run after being queued for free. */
  CHECK(!cw_node_valid(p));
  (void)pd;
  cw_node q = cw_node_new("Probe", "Q");
  cw_node_add_child(cw_tree_root(t), q);
  probe *qd = cw_node_data(q, "Probe");
  cw_tree_fixed_update(t, 1.0 / 60);
  cw_tree_update(t, 1.0 / 60);
  CHECK_EQ_INT(qd->fixed_calls, 1);
  CHECK_EQ_INT(qd->update_calls, 1);
  cw_tree_set_paused(t, true);
  cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(qd->fixed_calls, 1);
  cw_node_set_pause_mode(q, CW_PAUSE_ALWAYS);
  cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(qd->fixed_calls, 2);
  /* groups in tree order */
  cw_node g1 = cw_node_new("Node", "G1"), g2 = cw_node_new("Node", "G2");
  cw_node_add_child(cw_tree_root(t), g2);
  cw_node_add_child_at(cw_tree_root(t), g1, 0);
  cw_node_add_group(g2, "enemies");
  cw_node_add_group(g1, "enemies");
  cw_node *list;
  int k = cw_tree_group(t, "enemies", &list);
  CHECK_EQ_INT(k, 2);
  CHECK(k == 2 && list[0].id == g1.id && list[1].id == g2.id);
  cw_node_move_child(cw_tree_root(t), g1, 10);
  k = cw_tree_group(t, "enemies", &list);
  CHECK(k == 2 && list[0].id == g2.id);
  cw_tree_free(t);
}

static int hits, deferred_hits;
static float last_arg;
static void on_hit(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)u; hits++; if (n) last_arg = (float)a[0].f; }
static void on_hit_deferred(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)a; (void)n; (void)u; deferred_hits++; }

static void test_signals(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node a = cw_node_new("Probe", "A"), b = cw_node_new("Probe", "B");
  cw_node_add_child(cw_tree_root(t), a);
  cw_node_add_child(cw_tree_root(t), b);
  hits = deferred_hits = 0;
  uint64_t c1 = cw_connect(a, "hit", b, "on_hit", on_hit, NULL, 0);
  cw_connect(a, "hit", b, NULL, on_hit_deferred, NULL, CW_CONNECT_DEFERRED);
  cw_connect(a, "hit", b, NULL, on_hit, NULL, CW_CONNECT_ONESHOT);
  cw_value arg = cw_val_float(2.5);
  cw_emit(a, "hit", &arg, 1);
  CHECK_EQ_INT(hits, 2);
  CHECK(last_arg == 2.5f);
  CHECK_EQ_INT(deferred_hits, 0);
  cw_tree_update(t, 0); /* deferred ones run at the end of the phase */
  CHECK_EQ_INT(deferred_hits, 1);
  cw_emit(a, "hit", &arg, 1);
  CHECK_EQ_INT(hits, 3); /* oneshot gone */
  cw_disconnect(c1);
  cw_emit(a, "hit", &arg, 1);
  CHECK_EQ_INT(hits, 3);
  cw_node_free(b); /* connections to b are dropped */
  cw_tree_update(t, 0);
  cw_emit(a, "hit", &arg, 1);
  cw_tree_update(t, 0);
  CHECK_EQ_INT(deferred_hits, 1); /* freeing the target dropped its connections */
  cw_tree_free(t);
}

static void test_reflection(void) {
  setup();
  cw_node n = cw_node_new("Probe", "R");
  CHECK(cw_node_set(n, "value", cw_val_int(7)));
  CHECK(cw_node_set(n, "speed", cw_val_float(25))); /* clamped to 10 */
  CHECK(cw_node_set(n, "position", cw_val_vec3(1, 2, 3)));
  CHECK(!cw_node_set(n, "nope", cw_val_int(1)));
  CHECK(!cw_node_set(n, "position", cw_val_string("x")));
  cw_value v;
  CHECK(cw_node_get_prop(n, "speed", &v) && v.type == CW_T_FLOAT && v.f == 10);
  CHECK(cw_node_get_prop(n, "value", &v) && v.i == 7);
  CHECK(cw_node_set(n, "speed", cw_val_int(3))); /* int coerces to float */
  CHECK(cw_node_get_prop(n, "speed", &v) && v.f == 3);
  /* rotation <-> quaternion stay in sync, deterministically */
  CHECK(cw_node_set(n, "rotation", cw_val_vec3(0.3f, -1.1f, 0.7f)));
  CHECK(cw_node_get_prop(n, "quaternion", &v));
  FOLD(v.v);
  cw_value back;
  cw_node_set(n, "quaternion", v);
  cw_node_get_prop(n, "rotation", &back);
  CHECK(fabsf(back.v[0] - 0.3f) < 1e-4f && fabsf(back.v[1] + 1.1f) < 1e-4f && fabsf(back.v[2] - 0.7f) < 1e-4f);
  /* enums by name */
  cw_node m = cw_node_new("MeshInstance3D", "M");
  CHECK(cw_node_set(m, "primitive", cw_val_enum("SPHERE")));
  CHECK(cw_node_get_prop(m, "primitive", &v) && v.i == 2);
  CHECK(!cw_node_set(m, "primitive", cw_val_enum("TEAPOT")));
  cw_node_free(m);
  char buf[128];
  cw_value_format(&(cw_value){ .type = CW_T_VEC3, .v = { 1, 0.5f, -2 } }, buf, sizeof buf);
  CHECK(strcmp(buf, "vec3(1, 0.5, -2)") == 0);
  CHECK(cw_value_parse("color(1, 0.25, 0)", CW_T_COLOR, &v) && v.type == CW_T_COLOR && v.v[3] == 1);
  const cw_prop *props[64];
  int np = cw_node_props(n, props, NULL, 64);
  CHECK(np >= 10 && !strcmp(props[0]->name, "name")); /* base type's first */
  cw_node_free(n);
}

static const char *SUB =
  "[scene format=1]\n"
  "\n[node name=\"Player\" type=\"Node3D\"]\n"
  "position = vec3(0, 1, 0)\n"
  "\n[node name=\"Body\" parent=\".\" type=\"MeshInstance3D\"]\n"
  "primitive = SPHERE\n";

static void test_scene_roundtrip(void) {
  setup();
  /* sub-scene from memory: register it in the loader cache via parse+load path */
  cw_packed_scene *sub = cw_scene_parse(SUB, "player.cwscene");
  CHECK(sub != NULL);
  cw_node p = cw_scene_instance(sub);
  CHECK(cw_node_is(p, "Node3D"));
  CHECK(cw_node_is(cw_node_get(p, "Body"), "MeshInstance3D"));
  char *text = cw_scene_save(p);
  CHECK(strcmp(text, SUB) == 0);
  if (strcmp(text, SUB)) cw_error("saved:\n%s", text);
  cw_free(text);
  cw_node_free(p);

  const char *MAIN =
    "[scene format=1]\n"
    "\n[node name=\"Level\" type=\"Node3D\"]\n"
    "\n[node name=\"Floor\" parent=\".\" type=\"MeshInstance3D\" groups=\"static,ground\"]\n"
    "size = vec3(10, 0.1, 10)\n"
    "color = color(0.2, 0.6, 0.3, 1)\n"
    "\n[node name=\"Probe\" parent=\"Floor\" type=\"Probe\"]\n"
    "value = 42\n"
    "\n[node name=\"Light\" parent=\".\" type=\"DirectionalLight3D\"]\n"
    "rotation = vec3(-0.8, 0.4, 0)\n"
    "energy = 2.5\n"
    "\n[connection signal=\"hit\" from=\"Floor/Probe\" to=\".\" method=\"on_hit\"]\n";
  cw_packed_scene *ps = cw_scene_parse(MAIN, "main.cwscene");
  CHECK(ps != NULL);
  cw_node lvl = cw_scene_instance(ps);
  CHECK(cw_node_in_group(cw_node_get(lvl, "Floor"), "ground"));
  cw_value v;
  CHECK(cw_node_get_prop(cw_node_get(lvl, "Floor/Probe"), "value", &v) && v.i == 42);
  text = cw_scene_save(lvl);
  CHECK(strcmp(text, MAIN) == 0);
  if (strcmp(text, MAIN)) cw_error("saved:\n%s", text);
  cw_free(text);
  cw_node_free(lvl);
  CHECK(cw_scene_parse("[node name=\"x\"]\n", "bad.cwscene") == NULL); /* root without type */
}

/* every registered type, every property: get -> set(same) -> get is
 * stable, and value_format -> value_parse reproduces it (the scene format
 * and the debug channel depend on both) */
static void test_all_types_reflect(void) {
  setup();
  int types_checked = 0, props_checked = 0;
  for (int i = 0; i < cw_node_type_count(); i++) {
    const cw_node_type *t = cw_node_type_at(i);
    cw_node n = cw_node_new(t->name, NULL);
    CHECK(!CW_HANDLE_IS_NULL(n));
    if (CW_HANDLE_IS_NULL(n)) continue;
    types_checked++;
    const cw_prop *props[256];
    int np = cw_node_props(n, props, NULL, 256);
    for (int k = 0; k < np && k < 256; k++) {
      const cw_prop *pr = props[k];
      cw_value a, b;
      CHECK(cw_node_get_prop(n, pr->name, &a));
      if (pr->type == CW_T_NODE || pr->type == CW_T_NONE) continue;
      if (!(pr->flags & CW_PROP_READONLY)) {
        bool ok = cw_node_set(n, pr->name, a);
        CHECK(ok);
        if (!ok) cw_error("type %s prop %s: set(get()) refused", t->name, pr->name);
        CHECK(cw_node_get_prop(n, pr->name, &b));
        char fa[512], fb[512];
        cw_value_format(&a, fa, sizeof fa);
        cw_value_format(&b, fb, sizeof fb);
        CHECK(strcmp(fa, fb) == 0);
        if (strcmp(fa, fb)) cw_error("type %s prop %s: %s became %s", t->name, pr->name, fa, fb);
      }
      if (pr->type != CW_T_ENUM && pr->type != CW_T_RES) {
        char fa[512];
        cw_value_format(&a, fa, sizeof fa);
        cw_value parsed;
        CHECK(cw_value_parse(fa, pr->type, &parsed));
        char fp[512];
        cw_value_format(&parsed, fp, sizeof fp);
        CHECK(strcmp(fa, fp) == 0);
        if (strcmp(fa, fp)) cw_error("type %s prop %s: text %s parsed back as %s", t->name, pr->name, fa, fp);
      }
      props_checked++;
    }
    cw_node_free(n);
  }
  cw_info("reflected %d types, %d properties", types_checked, props_checked);
  CHECK(types_checked >= 10);
  FOLD(types_checked);
}

/* a scene instancing a scene that itself holds an instance and a
 * connection saves only what the outer scene adds: the nested instance is
 * the instanced scene's node (an override at most) and the inner scene's
 * own connection is not written again (both used to grow on every save). */
static void test_nested_instance_save(void) {
  setup();
  const char *INNER =
    "[scene format=1]\n"
    "\n[node name=\"Gadget\" type=\"Probe\"]\n"
    "\n[node name=\"Part\" parent=\".\" type=\"MeshInstance3D\"]\n"
    "\n[connection signal=\"hit\" from=\".\" to=\".\" method=\"on_hit\"]\n";
  const char *OUTER =
    "[scene format=1]\n"
    "[ext_scene id=1 path=\"nested_inner.cwscene\"]\n"
    "\n[node name=\"Rig\" type=\"Node3D\"]\n"
    "\n[node name=\"Gadget\" parent=\".\" instance=1]\n"
    "position = vec3(1, 0, 0)\n";
  const char *LEVEL =
    "[scene format=1]\n"
    "[ext_scene id=1 path=\"nested_outer.cwscene\"]\n"
    "\n[node name=\"Level\" type=\"Node3D\"]\n"
    "\n[node name=\"Rig\" parent=\".\" instance=1]\n"
    "position = vec3(0, 2, 0)\n"
    "\n[node name=\"Part\" parent=\"Rig/Gadget\"]\n"
    "visible = false\n"
    "\n[node name=\"Extra\" parent=\"Rig\" type=\"Node3D\"]\n"
    "\n[connection signal=\"hit\" from=\"Rig/Gadget\" to=\".\" method=\"on_level_hit\"]\n";
  /* the sub-scenes load by path: put them in the project layer */
  CHECK(cw_vfs_write("nested_inner.cwscene", INNER, strlen(INNER)));
  CHECK(cw_vfs_write("nested_outer.cwscene", OUTER, strlen(OUTER)));
  cw_scene_cache_forget(NULL);
  cw_packed_scene *ps = cw_scene_parse(LEVEL, "nested_level.cwscene");
  CHECK(ps != NULL);
  cw_node lvl = cw_scene_instance(ps);
  CHECK_EQ_INT(cw_node_child_count(cw_node_get(lvl, "Rig")), 2); /* Gadget + Extra, no second Gadget */
  CHECK_EQ_INT(cw_node_connections(cw_node_get(lvl, "Rig/Gadget"), false, NULL, 0), 2);
  char *text = cw_scene_save(lvl);
  CHECK(strcmp(text, LEVEL) == 0);
  if (strcmp(text, LEVEL)) cw_error("saved:\n%s", text);
  /* and a second round is a fixed point */
  cw_packed_scene *again = cw_scene_parse(text, "nested_level2.cwscene");
  cw_node lvl2 = cw_scene_instance(again);
  char *text2 = cw_scene_save(lvl2);
  CHECK(strcmp(text2, LEVEL) == 0);
  CHECK_EQ_INT(cw_node_connections(cw_node_get(lvl2, "Rig/Gadget"), false, NULL, 0), 2);
  /* the connection query: outgoing in order, incoming on the target */
  cw_connection_info ci[4];
  int k = cw_node_connections(cw_node_get(lvl2, "Rig/Gadget"), false, ci, 4);
  CHECK(k == 2 && !strcmp(ci[0].signal, "hit") && ci[0].method && !strcmp(ci[0].method, "on_hit") && !strcmp(ci[1].method, "on_level_hit"));
  CHECK_EQ_INT(cw_node_connections(lvl2, true, NULL, 0), 1);
  FOLD(k);
  cw_free(text), cw_free(text2);
  cw_node_free(lvl), cw_node_free(lvl2);
}

/* found while writing the docs */
static void test_look_at_rotated_parent(void) {
  setup();
  cw_tree *t = cw_tree_new();
  cw_node parent = cw_node_new("Node3D", "P");
  cw_node3d_set_rotation(parent, (t3_vec3){ 0, 1.5707963f, 0.3f });
  cw_node3d_set_position(parent, (t3_vec3){ 1, 2, 3 });
  cw_node_add_child(cw_tree_root(t), parent);
  cw_node cam = cw_node_new("Node3D", "Cam");
  cw_node3d_set_position(cam, (t3_vec3){ 0, 0, 5 });
  cw_node_add_child(parent, cam);
  t3_vec3 target = { -4, 1, 2 };
  cw_node3d_look_at(cam, target, (t3_vec3){ 0, 1, 0 });
  const t3_mat4 *m = cw_node3d_global_transform(cam);
  t3_vec3 eye = { m->e[12], m->e[13], m->e[14] };
  t3_vec3 fwd = { -m->e[8], -m->e[9], -m->e[10] }; /* world -Z */
  t3_vec3 want = { target.x - eye.x, target.y - eye.y, target.z - eye.z };
  float lf = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z), lw = sqrtf(want.x * want.x + want.y * want.y + want.z * want.z);
  float dot = (fwd.x * want.x + fwd.y * want.y + fwd.z * want.z) / (lf * lw);
  CHECK(dot > 0.9999f);
  /* the world up stays up-ish: camera x axis has no world y */
  CHECK(fabsf(m->e[1]) < 1e-4f);
  cw_tree_free(t);
}

static void test_null_tree(void) {
  CHECK(CW_HANDLE_IS_NULL(cw_tree_root(NULL)));
  CHECK_EQ_INT(cw_tree_node_count(NULL), 0);
  CHECK_EQ_INT(cw_tree_tick(NULL), 0);
  CHECK(!cw_tree_paused(NULL));
  CHECK(cw_tree_rng(NULL) == NULL);
  cw_tree_set_paused(NULL, true);
  cw_tree_seed(NULL, 1);
  cw_tree_fixed_update(NULL, 1.0 / 60);
  cw_tree_update(NULL, 1.0 / 60);
  cw_tree_pre_render(NULL, 1);
  cw_tree_set_slot(NULL, "x", NULL, NULL);
  cw_node *g = (cw_node *)1;
  CHECK_EQ_INT(cw_tree_group(NULL, "g", &g), 0);
  CHECK(g == NULL);
}

/* node-valued properties in scene files: paths from the scene root,
 * resolved after the instance is built, so they work wherever the scene is
 * instanced (and before it is in any tree) */
static void test_scene_node_refs(void) {
  setup();
  const char *MENU =
    "[scene format=1]\n"
    "\n[node name=\"Menu\" type=\"VBoxContainer\"]\n"
    "focus_next = node(\".\")\n"
    "\n[node name=\"Play\" parent=\".\" type=\"Button\"]\n"
    "focus_neighbor_bottom = node(\"Quit\")\n"
    "\n[node name=\"Quit\" parent=\".\" type=\"Button\"]\n"
    "focus_neighbor_top = node(\"Play\")\n";
  cw_tree *t = cw_tree_new();
  cw_set_current_tree(t);
  cw_packed_scene *ps = cw_scene_parse(MENU, "menu_refs.cwscene");
  CHECK(ps != NULL);
  cw_node m = cw_scene_instance(ps);
  cw_value v;
  CHECK(cw_node_get_prop(cw_node_get(m, "Play"), "focus_neighbor_bottom", &v) && v.node.id == cw_node_get(m, "Quit").id);
  CHECK(cw_node_get_prop(cw_node_get(m, "Quit"), "focus_neighbor_top", &v) && v.node.id == cw_node_get(m, "Play").id);
  CHECK(cw_node_get_prop(m, "focus_next", &v) && v.node.id == m.id);
  /* placed deep in a tree, saved from there: the same text */
  cw_node holder = cw_node_new("Node", "Holder");
  cw_node_add_child(cw_tree_root(t), holder);
  cw_node_add_child(holder, m);
  char *text = cw_scene_save(m);
  CHECK(!strcmp(text, MENU));
  if (strcmp(text, MENU)) cw_error("saved:\n%s", text);
  cw_free(text);
  /* instanced as a sub-scene: links equal to the sub-scene's are not repeated */
  cw__scene_cache_put("menu_refs.cwscene", ps);
  cw_node outer = cw_node_new("Node", "Outer");
  cw_node_add_child(cw_tree_root(t), outer);
  cw_node inst = cw_scene_instance_linked("menu_refs.cwscene");
  cw_node_add_child(outer, inst);
  text = cw_scene_save(outer);
  CHECK(strstr(text, "focus_") == NULL);
  if (strstr(text, "focus_")) cw_error("saved:\n%s", text);
  cw_free(text);
  /* an override that changes a link inside the instance round-trips */
  cw_value nv = { CW_T_NODE };
  nv.node = cw_node_get(inst, "Quit");
  cw_node_set(cw_node_get(inst, "Quit"), "focus_neighbor_top", nv);
  text = cw_scene_save(outer);
  CHECK(strstr(text, "focus_neighbor_top = node(\"Menu/Quit\")") != NULL);
  cw_packed_scene *again = cw_scene_parse(text, "outer_refs.cwscene");
  cw_node o2 = cw_scene_instance(again);
  CHECK(cw_node_get_prop(cw_node_get(o2, "Menu/Quit"), "focus_neighbor_top", &v) && v.node.id == cw_node_get(o2, "Menu/Quit").id);
  cw_free(text);
  cw_node_free(o2);
  cw_packed_scene_free(again);
  cw_set_current_tree(NULL);
  cw_tree_free(t);
}

/* 2D passes: cw_r2d_canvas_to_window / window_to_canvas keep describing the
 * last WINDOW pass after a render-target pass (they mixed the window
 * pass's rect with the target pass's virtual size), and a canvas draw
 * puts back the r2d layer settings it used (cw_canvas_draw_view left its
 * camera on layer 0, shifting the next window pass) */
static bool near2(cw_vec2 a, cw_vec2 b) { return fabsf(a.x - b.x) < 0.01f && fabsf(a.y - b.y) < 0.01f; }
static void test_canvas_passes(void) {
  cw_texture rt = cw_target_create(&(cw_target_desc){ .width = 16, .height = 16 });
  CHECK(cw_texture_valid(rt));
  cw_r2d_begin(&(cw_r2d_pass_desc){ .virtual_width = 32, .virtual_height = 16, .scale = CW_SCALE_FIT });
  cw_r2d_end();
  cw_rect win = cw_r2d_canvas_rect();
  cw_vec2 br = cw_r2d_canvas_to_window(cw_v2(32, 16));
  float pr = cw_pixel_ratio(); /* the window is logical, the canvas rect drawable pixels */
  CHECK(near2(cw_v2(br.x * pr, br.y * pr), cw_v2(win.x + win.w, win.y + win.h)));
  CHECK(near2(cw_r2d_window_to_canvas(br), cw_v2(32, 16)));
  cw_r2d_begin(&(cw_r2d_pass_desc){ .target = rt });
  cw_r2d_end();
  CHECK(near2(cw_r2d_canvas_to_window(cw_v2(32, 16)), br));
  CHECK(near2(cw_r2d_window_to_canvas(br), cw_v2(32, 16)));

  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node poly = cw_node_new("Polygon2D", "Poly");
  cw_node_add_child(cw_tree_root(t), poly);
  cw_r2d_layer_set(0, NULL);
  cw_r2d_layer_set(1, &(cw_r2d_layer_desc){ .y_sort = true });
  CHECK(cw_canvas_draw_view(t, &(cw_canvas_view){ .target = rt, .use_camera = true, .camera = { .position = { 50, 50 }, .zoom = 1 },
                                                  .clear = true }, 0));
  CHECK(!cw_r2d_layer_get(0)->has_camera);
  CHECK(cw_r2d_layer_get(1)->y_sort);
  cw_r2d_layer_set(1, NULL);
  /* the documented CanvasLayer defaults (scene2d.h) */
  cw_node cl = cw_node_new("CanvasLayer", "HUD");
  cw_node_add_child(cw_tree_root(t), cl);
  cw_value v;
  CHECK(cw_node_get_prop(cl, "layer", &v) && v.i == 1);
  CHECK(cw_node_get_prop(cl, "follow_viewport", &v) && !v.b);
  cw_tree_free(t);
  cw_texture_destroy(rt);
}

/* MeshInstance3D primitives with the same look draw with one material (so
 * they batch); a property change moves a mesh to its new look's material */
static t3_material *mat_of(cw_node n) { return cw__mesh3d_t3(n)->materials[0]; }
static void test_mesh_materials_shared(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node m[3];
  for (int i = 0; i < 3; i++) {
    m[i] = cw_node_new("MeshInstance3D", NULL);
    cw_node_add_child(cw_tree_root(t), m[i]);
  }
  cw_node_set(m[0], "color", cw_val_color(0.2f, 0.4f, 0.6f, 1));
  cw_node_set(m[1], "color", cw_val_color(0.2f, 0.4f, 0.6f, 1));
  cw_node_set(m[2], "color", cw_val_color(0.9f, 0.1f, 0.1f, 1));
  cw_tree_pre_render(t, 0);
  CHECK(mat_of(m[0]) == mat_of(m[1]) && mat_of(m[0]) != mat_of(m[2]));
  CHECK(mat_of(m[0])->color.g == 0.4f && mat_of(m[2])->color.r == 0.9f);
  /* m[1] takes m[2]'s look: m[2]'s material; m[0]'s stays as it was */
  cw_node_set(m[1], "color", cw_val_color(0.9f, 0.1f, 0.1f, 1));
  cw_tree_pre_render(t, 0);
  CHECK(mat_of(m[1]) == mat_of(m[2]) && mat_of(m[0]) != mat_of(m[1]) && mat_of(m[0])->color.g == 0.4f);
  /* a physical lobe: a MeshPhysicalMaterial of its own */
  cw_node_set(m[1], "clearcoat", cw_val_float(0.5f));
  cw_tree_pre_render(t, 0);
  CHECK(mat_of(m[1])->type == T3_MESH_PHYSICAL_MATERIAL && mat_of(m[1])->clearcoat == 0.5f);
  CHECK(mat_of(m[2])->type == T3_MESH_STANDARD_MATERIAL && mat_of(m[1]) != mat_of(m[2]));
  cw_node_set(m[2], "clearcoat", cw_val_float(0.5f));
  cw_tree_pre_render(t, 0);
  CHECK(mat_of(m[1]) == mat_of(m[2]));
  cw_tree_free(t);
}

/* more lights of a kind than three.c lights with: one warning naming the counts */
static int cap_warnings, order_warnings;
static char cap_msg[1024];
static void cap_listener(cw_log_level level, const char *msg, void *user) {
  (void)user;
  if (level == CW_LOG_WARN && strstr(msg, "LIGHT CAP")) {
    cap_warnings++;
    snprintf(cap_msg, sizeof cap_msg, "%s", msg);
  }
  if (level == CW_LOG_WARN && strstr(msg, "LIGHT ORDER")) order_warnings++;
}
static void test_light_cap_warning(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node cam = cw_node_new("Camera3D", "Cam");
  cw_node_add_child(cw_tree_root(t), cam);
  for (int i = 0; i < 8; i++) cw_node_add_child(cw_tree_root(t), cw_node_new("OmniLight3D", NULL));
  int id = cw_log_add_listener(cap_listener, NULL);
  cw_tree_pre_render(t, 0);
  CHECK(cw_render3d_prepare(t, 16, 16) != NULL);
  CHECK(cap_warnings == 0); /* 8: at the cap */
  cw_node extra = cw_node_new("OmniLight3D", NULL);
  cw_node_add_child(cw_tree_root(t), extra);
  for (int f = 0; f < 120; f++) cw_render3d_prepare(t, 16, 16);
  CHECK(cap_warnings == 1 && strstr(cap_msg, "9 point") && strstr(cap_msg, "other 1"));
  for (int i = 0; i < 4; i++) cw_node_add_child(cw_tree_root(t), cw_node_new("OmniLight3D", NULL));
  for (int f = 0; f < 120; f++) cw_render3d_prepare(t, 16, 16);
  CHECK(cap_warnings == 1); /* once per kind */
  CHECK(order_warnings == 0);
  cw_log_remove_listener(id);
  cw_tree_free(t);

  /* a shadowed spot light made after a plain one: three.c draws nothing lit */
  t = cw_tree_new();
  cam = cw_node_new("Camera3D", "Cam");
  cw_node_add_child(cw_tree_root(t), cam);
  cw_node shadowed = cw_node_new("SpotLight3D", "First");
  cw_node_set(shadowed, "shadow", cw_val_bool(true));
  cw_node_add_child(cw_tree_root(t), shadowed);
  cw_node_add_child(cw_tree_root(t), cw_node_new("SpotLight3D", "Plain"));
  id = cw_log_add_listener(cap_listener, NULL);
  cw_tree_pre_render(t, 0);
  cw_render3d_prepare(t, 16, 16);
  CHECK(order_warnings == 0); /* the caster first: fine */
  cw_node late = cw_node_new("SpotLight3D", "Late");
  cw_node_set(late, "shadow", cw_val_bool(true));
  cw_node_add_child(cw_tree_root(t), late);
  cw_tree_pre_render(t, 0);
  for (int f = 0; f < 120; f++) cw_render3d_prepare(t, 16, 16);
  CHECK(order_warnings == 1 && cap_warnings == 1);
  cw_log_remove_listener(id);
  cw_tree_free(t);
}

/* a light's energy runs to 100000: 9000 passes untouched; past the range it is
 * clamped with one warning per light (the control: 2e6 must clamp and warn) */
static int range_warnings;
static void range_listener(cw_log_level level, const char *msg, void *user) {
  (void)user;
  if (level == CW_LOG_WARN && strstr(msg, "outside its range")) range_warnings++;
}
static void test_light_energy_range(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node l = cw_node_new("OmniLight3D", "Lamp");
  cw_node_add_child(cw_tree_root(t), l);
  int id = cw_log_add_listener(range_listener, NULL);
  cw_value v;
  cw_node_set(l, "energy", cw_val_float(9000));
  CHECK(cw_node_get_prop(l, "energy", &v) && v.f == 9000);
  CHECK(range_warnings == 0);
  cw_node_set(l, "energy", cw_val_float(2e6f));
  CHECK(cw_node_get_prop(l, "energy", &v) && v.f == 1e5f);
  CHECK(range_warnings == 1);
  cw_node_set(l, "energy", cw_val_float(3e6f));
  CHECK(range_warnings == 1); /* once per light */
  cw_node other = cw_node_new("OmniLight3D", "Other");
  cw_node_add_child(cw_tree_root(t), other);
  cw_node_set(other, "energy", cw_val_float(-5));
  CHECK(cw_node_get_prop(other, "energy", &v) && v.f == 0);
  CHECK(range_warnings == 2);
  cw_log_remove_listener(id);
  cw_tree_free(t);
}

static void test_procedural_mesh(void) {
  cw_scene_register_builtin_types();
  cw_tree *t=cw_tree_new();
  cw_node n=cw_node_new("MeshInstance3D","Procedural");
  cw_node other=cw_node_new("MeshInstance3D","Other");
  cw_node_add_child(cw_tree_root(t),n);
  cw_node_add_child(cw_tree_root(t),other);
  t3_geometry *g=t3_box_geometry_new(2,3,4,1,1,1);
  t3_material *mat=t3_mesh_standard_material_new(0xff3366);
  t3_mesh *mesh=t3_mesh_new(g,mat);
  mesh->base.position=(t3_vec3){12,3,-4};
  mesh->base.cast_shadow=true;
  /* Replacing a baked primitive must release its bake and restore LODs
   * when returning from procedural geometry to a normal primitive. */
  cw_node_set(n,"primitive",cw_val_enum("SPHERE"));
  cw_tree_pre_render(t,0);
  t3_mesh *primitive=cw__mesh3d_t3(n);
  unsigned char pixel[4]={255,255,255,255};
  t3_texture *lm=t3_texture_new(1,1,pixel);
  cw__mesh3d_set_lightmap(n,primitive->geometry,lm,1,false);
  CHECK_EQ_INT(lm->_refs,2);
  CHECK(!cw_mesh3d_set_procedural(cw_tree_root(t),mesh));
  CHECK(cw_mesh3d_set_procedural(n,mesh));
  CHECK_EQ_INT(lm->_refs,1);
  CHECK(cw_mesh3d_set_procedural(n,mesh));
  CHECK(!cw_mesh3d_set_procedural(other,mesh));
  cw_node_set(n,"color",cw_val_color(0,1,0,1));
  cw_tree_pre_render(t,0);
  CHECK(cw__mesh3d_t3(n)==mesh && mesh->materials[0]==mat);
  CHECK(mesh->base.position.x==12 && mesh->base.cast_shadow);
  /* Raw three.c parent visibility participates in scene mesh discovery. */
  t3_object *old_parent = mesh->base.parent;
  t3_object *outer = t3_group_new(), *inner = t3_group_new();
  t3_object_add(old_parent, outer);
  t3_object_add(outer, inner);
  t3_object_add(inner, &mesh->base);
  cw__mesh_info nested_info;
  CHECK(cw__mesh3d_info(n, &nested_info));
  outer->visible = false;
  CHECK(!cw__mesh3d_info(n, &nested_info));
  outer->visible = true; inner->visible = false;
  CHECK(!cw__mesh3d_info(n, &nested_info));
  inner->visible = true;
  CHECK(cw__mesh3d_info(n, &nested_info));
  t3_object_add(old_parent, &mesh->base);
  t3_object_remove(old_parent, outer);
  t3_release(inner); t3_release(outer);
  CHECK(cw_mesh3d_set_procedural(n,NULL));
  CHECK(mesh->base.parent==NULL);
  cw_tree_pre_render(t,0);
  CHECK(cw__mesh3d_t3(n)!=mesh && cw__mesh3d_t3(n)!=NULL);
  cw__mesh_info info;
  CHECK(cw__mesh3d_info(n,&info));
  CHECK_EQ_INT(info.lod_levels,4);
  t3_release(lm);
  CHECK(cw_mesh3d_set_procedural(other,mesh));
  cw_tree_free(t);
  CHECK(mesh->base.parent==NULL);
  t3_release(mesh);t3_release(mat);t3_release(g);
}

CW_TEST_SUITE("scene", { "all_types_reflect", test_all_types_reflect }, { "lifecycle_and_paths", test_lifecycle_and_paths }, { "processing_and_deferred", test_processing_and_deferred },
              { "signals", test_signals }, { "reflection", test_reflection }, { "scene_roundtrip", test_scene_roundtrip },
              { "nested_instance_save", test_nested_instance_save },
              { "look_at_rotated_parent", test_look_at_rotated_parent }, { "null_tree", test_null_tree },
              { "scene_node_refs", test_scene_node_refs }, { "canvas_passes", test_canvas_passes },
              { "mesh_materials_shared", test_mesh_materials_shared }, { "light_cap_warning", test_light_cap_warning },
              { "light_energy_range", test_light_energy_range }, { "procedural_mesh", test_procedural_mesh })

/* MultiMeshInstance3D (cartwheel/multimesh.h): the buffer, the stamp, cells
 * and their bounds, what is rebuilt when, the one-draw path rewritten in
 * place, the cells a camera sees, the GRID fill, the scene-file round trip.
 * Nothing is drawn here (the suite has no program table); the pictures and
 * the timings are the multimesh gate's. */
#include <math.h>
#include <string.h>

#include "cartwheel/multimesh.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "test.h"

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}
static t3_mat4 at(float x, float y, float z, float s) {
  return (t3_mat4){ { s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, x, y, z, 1 } };
}
/* the instanced draws under a node, their instances summed */
static int draws_of(cw_node n, int *instances) {
  t3_object *o = cw_node3d_object(n);
  int d = 0;
  *instances = 0;
  for (int i = 0; i < o->child_count; i++)
    if (o->children[i]->type == T3_INSTANCED_MESH) {
      d++;
      *instances += ((t3_instanced_mesh *)o->children[i])->count;
    }
  return d;
}
static cw_node make(cw_node root, const char *name, cw_node *stamp) {
  cw_node mm = add(root, "MultiMeshInstance3D", name);
  cw_node s = add(mm, "MeshInstance3D", "Stamp");
  cw_node3d_set_position(s, (t3_vec3){ 0, 0.5f, 0 });
  if (stamp) *stamp = s;
  return mm;
}
static void frames(cw_tree *t, int n) {
  for (int i = 0; i < n; i++) cw_tree_pre_render(t, 1.0);
}

static void test_buffer(void) {
  cw_tree *t = cw_tree_new();
  cw_node mm = make(cw_tree_root(t), "Field", NULL);
  CHECK_EQ_INT(cw_multimesh_instance_count(mm), 0);
  CHECK(cw_multimesh_transforms(mm) == NULL);
  cw_multimesh_set_instance_count(mm, 3);
  CHECK_EQ_INT(cw_multimesh_instance_count(mm), 3);
  t3_mat4 m, got;
  CHECK(cw_multimesh_get_transform(mm, 2, &got) && got.e[0] == 1 && got.e[12] == 0); /* identity */
  m = at(4, 5, 6, 2);
  cw_multimesh_set_transform(mm, 1, &m);
  CHECK(cw_multimesh_get_transform(mm, 1, &got) && !memcmp(&got, &m, sizeof m));
  float cu[4] = { 1, 2, 3, 4 }, cg[4];
  cw_multimesh_set_custom(mm, 1, cu);
  CHECK(cw_multimesh_get_custom(mm, 1, cg) && !memcmp(cu, cg, sizeof cu));
  /* growing keeps what was there; the property and the call are the same count */
  cw_node_set(mm, "instance_count", cw_val_int(10));
  CHECK_EQ_INT(cw_multimesh_instance_count(mm), 10);
  CHECK(cw_multimesh_get_transform(mm, 1, &got) && !memcmp(&got, &m, sizeof m));
  CHECK(cw_multimesh_get_custom(mm, 1, cg) && cg[3] == 4);
  cw_value v;
  CHECK(cw_node_get_prop(mm, "instance_count", &v) && v.i == 10);
  /* out of range: refused */
  cw_multimesh_set_transform(mm, 10, &m);
  cw_multimesh_set_transform(mm, -1, &m);
  CHECK(!cw_multimesh_get_transform(mm, 10, &got));
  CHECK(!cw_multimesh_get_custom(mm, 99, cg));
  /* another node type: nothing */
  CHECK_EQ_INT(cw_multimesh_instance_count(cw_tree_root(t)), 0);
  cw_tree_free(t);
}

static void test_cells(void) {
  cw_tree *t = cw_tree_new();
  cw_node stamp, mm = make(cw_tree_root(t), "Field", &stamp);
  cw_node_set(mm, "chunk_size", cw_val_float(10));
  cw_multimesh_set_instance_count(mm, 6);
  /* three cells: two instances near the origin, three around x = 50, one high up */
  t3_mat4 m[6] = { at(1, 0, 1, 1), at(3, 0, 2, 1), at(51, 0, 1, 1), at(55, 0, 5, 3), at(58, 0, 9, 1), at(1, 40, 1, 1) };
  for (int i = 0; i < 6; i++) cw_multimesh_set_transform(mm, i, &m[i]);
  frames(t, 2); /* the stamp builds its mesh in its own pre_render: the second frame has it */
  cw_multimesh_stats st;
  cw_multimesh_get_stats(mm, &st);
  CHECK_EQ_INT(st.parts, 1);
  CHECK_EQ_INT(st.chunks, 3);
  CHECK_EQ_INT(st.instances, 6);
  int inst, d = draws_of(mm, &inst);
  CHECK_EQ_INT(d, 3);
  CHECK_EQ_INT(inst, 6);
  /* the stamp is hidden, and its own transform is in every instance */
  cw_value v;
  CHECK(cw_node_get_prop(stamp, "visible", &v) && !v.b);
  t3_object *o = cw_node3d_object(mm);
  bool found = false;
  for (int i = 0; i < o->child_count; i++) {
    if (o->children[i]->type != T3_INSTANCED_MESH) continue;
    t3_instanced_mesh *im = (t3_instanced_mesh *)o->children[i];
    const float *a = im->instance_matrix->array;
    for (int k = 0; k < im->count; k++)
      if (a[k * 16 + 12] == 55 && a[k * 16 + 13] == 1.5f && a[k * 16] == 3) found = true; /* at (55, 0, 5) x3, the stamp 0.5 up */
    /* each draw's bounds hold its instances */
    for (int k = 0; k < im->count; k++) {
      float dx = a[k * 16 + 12] - im->_isph[0], dy = a[k * 16 + 13] - im->_isph[1], dz = a[k * 16 + 14] - im->_isph[2];
      CHECK(sqrtf(dx * dx + dy * dy + dz * dz) <= im->_isph[3]);
    }
  }
  CHECK(found);
  int builds = st.builds;
  /* nothing changed: nothing is rebuilt */
  frames(t, 5);
  cw_multimesh_get_stats(mm, &st);
  CHECK_EQ_INT(st.builds, builds);
  /* a touch rebuilds once */
  cw_multimesh_touch(mm);
  frames(t, 3);
  cw_multimesh_get_stats(mm, &st);
  CHECK_EQ_INT(st.builds, builds + 1);
  /* the first N only */
  cw_node_set(mm, "visible_instance_count", cw_val_int(2));
  frames(t, 1);
  cw_multimesh_get_stats(mm, &st);
  CHECK_EQ_INT(st.instances, 2);
  CHECK_EQ_INT(st.chunks, 1);
  draws_of(mm, &inst);
  CHECK_EQ_INT(inst, 2);
  cw_node_set(mm, "visible_instance_count", cw_val_int(-1));
  /* one draw for everything; rewritten in place: the same three.c mesh after a touch */
  cw_node_set(mm, "chunk_size", cw_val_float(0));
  frames(t, 1);
  CHECK_EQ_INT(draws_of(mm, &inst), 1);
  CHECK_EQ_INT(inst, 6);
  t3_object *first = NULL;
  for (int i = 0; i < o->child_count; i++)
    if (o->children[i]->type == T3_INSTANCED_MESH) first = o->children[i];
  float *xf = cw_multimesh_transforms(mm);
  xf[12] = 7;
  cw_multimesh_touch(mm);
  frames(t, 1);
  t3_object *second = NULL;
  for (int i = 0; i < o->child_count; i++)
    if (o->children[i]->type == T3_INSTANCED_MESH) second = o->children[i];
  CHECK(first && first == second);
  CHECK(((float *)((t3_instanced_mesh *)second)->instance_matrix->array)[12] == 7);
  /* a stamp that changes is stamped again */
  cw_node3d_set_position(stamp, (t3_vec3){ 0, 2, 0 });
  frames(t, 2);
  for (int i = 0; i < o->child_count; i++)
    if (o->children[i]->type == T3_INSTANCED_MESH) second = o->children[i];
  CHECK(((float *)((t3_instanced_mesh *)second)->instance_matrix->array)[13] == 2);
  /* freeing the node frees its draws with it */
  cw_node_free(mm);
  cw_tree_free(t);
}

static void test_visible(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node cam = add(root, "Camera3D", "Camera"); /* at the origin, looking down -Z */
  (void)cam;
  cw_node mm = make(root, "Field", NULL);
  cw_node_set(mm, "chunk_size", cw_val_float(8));
  cw_multimesh_set_instance_count(mm, 40);
  for (int i = 0; i < 40; i++) {
    /* 30 in front of the camera, 10 behind it */
    t3_mat4 m = i < 30 ? at((float)(i % 5) * 2 - 4, -1, -20 - (float)(i / 5) * 6, 1) : at((float)(i % 5) * 2 - 4, -1, 60 + (float)i, 1);
    cw_multimesh_set_transform(mm, i, &m);
  }
  frames(t, 2);
  cw_render3d_prepare(t, 64, 36); /* the camera's matrices, as a frame makes them */
  frames(t, 1);
  cw_multimesh_stats st;
  cw_multimesh_get_stats(mm, &st);
  CHECK_EQ_INT(st.instances, 40);
  CHECK_EQ_INT(st.visible_instances, 30);
  CHECK(st.visible_chunks > 0 && st.visible_chunks < st.chunks);
  CHECK_EQ_INT(st.draws, st.visible_chunks);
  cw_value v;
  CHECK(cw_node_get_prop(mm, "visible_instances", &v) && v.i == 30);
  FOLD(st.chunks);
  cw_tree_free(t);
}

static void test_scene_file(void) {
  static const char *text =
    "[scene format=1]\n\n"
    "[node name=\"Posts\" type=\"MultiMeshInstance3D\"]\n"
    "source = \"Pole\"\n"
    "chunk_size = 0\n"
    "cast_shadow = true\n"
    "use_colors = true\n"
    "fill = GRID\n"
    "fill_count = vec3(4, 1, 3)\n"
    "fill_spacing = vec3(2, 1, 5)\n\n"
    "[node name=\"Decoy\" parent=\".\" type=\"MeshInstance3D\"]\n\n"
    "[node name=\"Pole\" parent=\".\" type=\"MeshInstance3D\"]\n"
    "primitive = CYLINDER\n";
  cw_packed_scene *ps = cw_scene_parse(text, NULL);
  CHECK(ps != NULL);
  if (!ps) return;
  cw_tree *t = cw_tree_new();
  cw_node n = cw_scene_instance(ps);
  cw_node_add_child(cw_tree_root(t), n);
  frames(t, 2);
  CHECK_EQ_INT(cw_multimesh_instance_count(n), 12);
  cw_multimesh_stats st;
  cw_multimesh_get_stats(n, &st);
  CHECK_EQ_INT(st.chunks, 1);
  CHECK_EQ_INT(st.parts, 1);
  t3_mat4 m;
  CHECK(cw_multimesh_get_transform(n, 0, &m) && m.e[12] == -3 && m.e[14] == -5); /* centred in x and z */
  CHECK(cw_multimesh_get_transform(n, 11, &m) && m.e[12] == 3 && m.e[14] == 5);
  /* the named source is the stamp (hidden), not the first mesh child */
  cw_value v;
  CHECK(cw_node_get_prop(cw_node_get(n, "Pole"), "visible", &v) && !v.b);
  CHECK(cw_node_get_prop(cw_node_get(n, "Decoy"), "visible", &v) && v.b);
  char *s1 = cw_scene_save(n);
  CHECK(strstr(s1, "type=\"MultiMeshInstance3D\"") && strstr(s1, "source = \"Pole\"") && strstr(s1, "fill = GRID") &&
        strstr(s1, "fill_count = vec3(4, 1, 3)") && strstr(s1, "chunk_size = 0") && strstr(s1, "use_colors = true"));
  CHECK(!strstr(s1, "visible_instances") && !strstr(s1, "visible_chunks"));
  cw_packed_scene *ps2 = cw_scene_parse(s1, NULL);
  CHECK(ps2 != NULL);
  if (ps2) {
    cw_node n2 = cw_scene_instance(ps2);
    cw_node_add_child(add(cw_tree_root(t), "Node3D", "Other"), n2);
    frames(t, 2);
    CHECK_EQ_INT(cw_multimesh_instance_count(n2), 12);
    char *s2 = cw_scene_save(n2);
    CHECK(!strcmp(s1, s2));
    uint32_t h = cw_hash32(s2, strlen(s2), CW_HASH32_INIT);
    FOLD(h);
    cw_free(s2);
    cw_packed_scene_free(ps2);
  }
  cw_free(s1);
  cw_tree_free(t);
  cw_packed_scene_free(ps);
}

CW_TEST_SUITE("multimesh", { "buffer", test_buffer }, { "cells", test_cells }, { "visible", test_visible }, { "scene_file", test_scene_file })

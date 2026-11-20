/* SubViewport (cartwheel/subviewport.h): names and "viewport://" references,
 * the update modes, a world of its own built in code and from a scene file's
 * property, a camera view queued behind the frame's own picture (which it
 * must not disturb), the textures for 2D and 3D, the scene-file round trip.
 * The worlds here hold cameras and backgrounds only (no meshes: the suite has
 * no program table); what the pictures look like is the subviewport gate's. */
#include <string.h>

#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/subviewport.h"
#include "test.h"

#define DT (1.0 / 60)

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

/* one frame of a tree as the scene runner draws it */
static void frame(cw_tree *t) {
  cw_tree_fixed_update(t, DT);
  cw_tree_pre_render(t, 1.0);
  cw_pipeline_render(t, 64, 36, 1.0);
}

static void test_names(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node a = add(root, "SubViewport", "Monitor"), b = add(root, "SubViewport", "Other");
  cw_node_set(b, "texture_name", cw_val_string("Minimap"));
  CHECK(cw_subviewport_find("viewport://Monitor").id == a.id);
  CHECK(cw_subviewport_find("Monitor").id == a.id);
  CHECK(cw_subviewport_find("viewport://Minimap").id == b.id);
  CHECK(!cw_node_valid(cw_subviewport_find("viewport://Other"))); /* texture_name replaces the node's name */
  CHECK(!cw_node_valid(cw_subviewport_find("viewport://Nobody")));
  CHECK(!cw_node_valid(cw_subviewport_find(NULL)));
  /* a freed viewport's name is gone */
  cw_node_free(a);
  cw_tree_fixed_update(t, DT);
  CHECK(!cw_node_valid(cw_subviewport_find("viewport://Monitor")));
  /* not a SubViewport: every call answers "none" */
  CHECK(!cw_texture_valid(cw_subviewport_texture(root)));
  CHECK(cw_subviewport_t3_texture(root) == NULL);
  CHECK(cw_subviewport_tree(root) == NULL);
  CHECK_EQ_INT(cw_subviewport_renders(root), 0);
  cw_tree_free(t);
  CHECK(!cw_node_valid(cw_subviewport_find("viewport://Minimap")));
}

/* a world of its own, built in code: a camera and a background */
static cw_node own_world(cw_node root, const char *name) {
  cw_node v = add(root, "SubViewport", name);
  cw_node_set(v, "size", cw_val_vec2(48, 32));
  cw_tree *w = cw_subviewport_tree(v);
  CHECK(w != NULL && w != cw_node_tree(v));
  add(cw_tree_root(w), "Camera3D", "Camera");
  return v;
}

static void test_modes(void) {
  cw_tree *t = cw_tree_new();
  cw_tree *seat = cw_current_tree();
  cw_node root = cw_tree_root(t);
  cw_node always = own_world(root, "Always"), once = own_world(root, "Once"), off = own_world(root, "Off"),
          vis = own_world(root, "Vis"), slow = own_world(root, "Slow");
  CHECK(cw_current_tree() == seat); /* making a world never takes the current-tree seat */
  cw_node_set(once, "update_mode", cw_val_enum("ONCE"));
  cw_node_set(off, "update_mode", cw_val_enum("DISABLED"));
  cw_node_set(vis, "update_mode", cw_val_enum("WHEN_VISIBLE"));
  cw_node_set(slow, "update_every", cw_val_int(3));
  for (int i = 0; i < 6; i++) frame(t);
  CHECK_EQ_INT(cw_subviewport_renders(always), 6);
  CHECK_EQ_INT(cw_subviewport_renders(once), 1);
  CHECK_EQ_INT(cw_subviewport_renders(off), 0);
  CHECK_EQ_INT(cw_subviewport_renders(vis), 1);  /* filled once, then nothing showed it */
  CHECK(cw_subviewport_renders(slow) >= 2 && cw_subviewport_renders(slow) <= 3);
  CHECK(cw_current_tree() == seat);
  /* the worlds tick with their owner */
  CHECK_EQ_INT(cw_tree_tick(cw_subviewport_tree(always)), 6);
  /* asking for the texture is showing it: WHEN_VISIBLE renders while that goes on */
  for (int i = 0; i < 4; i++) {
    CHECK(cw_texture_valid(cw_subviewport_texture(vis)));
    frame(t);
  }
  int v1 = cw_subviewport_renders(vis);
  CHECK(v1 >= 4);
  for (int i = 0; i < 4; i++) frame(t);
  CHECK(cw_subviewport_renders(vis) <= v1 + 1); /* and stops a frame after the last ask */
  /* ONCE again on request; a change of size is a new, empty target and fills it */
  cw_subviewport_request_update(once);
  frame(t);
  frame(t);
  CHECK_EQ_INT(cw_subviewport_renders(once), 2);
  cw_node_set(once, "size", cw_val_vec2(20, 10));
  frame(t);
  CHECK_EQ_INT(cw_subviewport_renders(once), 3);
  int tw = 0, th = 0;
  cw_texture_size(cw_subviewport_texture(once), &tw, &th);
  CHECK(tw == 20 && th == 10);
  /* the three.c texture is one object across a resize, at the new size */
  t3_texture *x = cw_subviewport_t3_texture(once);
  CHECK(x != NULL);
  cw_node_set(once, "size", cw_val_vec2(16, 16));
  frame(t);
  CHECK(cw_subviewport_t3_texture(once) == x);
  /* DISABLED never renders, even on request */
  cw_subviewport_request_update(off);
  frame(t);
  CHECK_EQ_INT(cw_subviewport_renders(off), 0);
  /* out of range sizes are clamped */
  cw_node_set(always, "size", cw_val_vec2(0, 100000));
  frame(t);
  cw_texture_size(cw_subviewport_texture(always), &tw, &th);
  CHECK(tw == 1 && th == 4096);
  cw_tree_free(t);
}

static int passes_ran;
static void count_exec(cw_pass_ctx *ctx, void *user) { (void)ctx, (void)user; passes_ran++; }

/* a camera of this world: drawn after the frame's own picture, which keeps its camera, output and history */
static void test_camera_view(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node main_cam = add(root, "Camera3D", "Main");
  cw_node other = add(root, "Camera3D", "Other");
  cw_node_set(other, "current", cw_val_bool(false));
  cw_node3d_set_position(other, (t3_vec3){ 5, 0, 0 });
  cw_node v = add(root, "SubViewport", "Mirror");
  cw_node_set(v, "size", cw_val_vec2(40, 24));
  cw_node_set(v, "camera", cw_val_string("../Other"));
  cw_pipeline *p = cw_pipeline_of(t);
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "t.count", .phase = CW_PHASE_EFFECTS, .order = 5000, .execute = count_exec, .webgpu = true });
  cw_rt *out = cw_rt_new(64, 36, CW_RT_RGBA8, false);
  cw_pipeline_set_output(p, out);
  passes_ran = 0;
  for (int i = 0; i < 3; i++) {
    cw_tree_fixed_update(t, DT);
    cw_tree_pre_render(t, 1.0);
    cw_pipeline_render(t, 64, 36, 1.0);
    CHECK(cw_render3d_camera(t).id == main_cam.id); /* the tree's camera is the main one again */
  }
  CHECK_EQ_INT(cw_subviewport_renders(v), 3);
  CHECK_EQ_INT(passes_ran, 6); /* each frame: the picture, then the view */
  /* a second view, and a viewport whose camera path is wrong (logged once, never rendered) */
  cw_node v2 = add(root, "SubViewport", "Mirror2");
  cw_node_set(v2, "camera", cw_val_string("../Other"));
  cw_node bad = add(root, "SubViewport", "Bad");
  cw_node_set(bad, "camera", cw_val_string("../Nowhere"));
  passes_ran = 0;
  cw_tree_pre_render(t, 1.0);
  cw_pipeline_render(t, 64, 36, 1.0);
  CHECK_EQ_INT(passes_ran, 3);
  CHECK_EQ_INT(cw_subviewport_renders(v2), 1);
  CHECK_EQ_INT(cw_subviewport_renders(bad), 0);
  /* disabled: the frame is the picture alone */
  cw_node_set(v, "update_mode", cw_val_enum("DISABLED"));
  cw_node_set(v2, "update_mode", cw_val_enum("DISABLED"));
  passes_ran = 0;
  cw_tree_pre_render(t, 1.0);
  cw_pipeline_render(t, 64, 36, 1.0);
  CHECK_EQ_INT(passes_ran, 1);
  cw_pipeline_set_output(p, NULL);
  cw_tree_free(t);
  cw_rt_free(out);
}

static void test_scene_file(void) {
  static const char *text =
    "[scene format=1]\n\n"
    "[node name=\"Root\" type=\"Node3D\"]\n\n"
    "[node name=\"Cam\" parent=\".\" type=\"Camera3D\"]\n"
    "current = false\n\n"
    "[node name=\"Monitor\" parent=\".\" type=\"SubViewport\"]\n"
    "size = vec2(320, 180)\n"
    "update_mode = WHEN_VISIBLE\n"
    "update_every = 2\n"
    "camera = \"../Cam\"\n"
    "texture_name = \"Feed\"\n\n"
    "[node name=\"Screen\" parent=\".\" type=\"MeshInstance3D\"]\n"
    "viewport_texture = \"viewport://Feed\"\n"
    "viewport_slot = COLOR\n\n"
    "[node name=\"Badge\" parent=\".\" type=\"Sprite2D\"]\n"
    "texture = \"viewport://Feed\"\n";
  cw_packed_scene *ps = cw_scene_parse(text, NULL);
  CHECK(ps != NULL);
  if (!ps) return;
  cw_node n = cw_scene_instance(ps);
  cw_value v;
  cw_node m = cw_node_get(n, "Monitor");
  CHECK(cw_node_is(m, "SubViewport"));
  CHECK(cw_node_get_prop(m, "size", &v) && v.v[0] == 320 && v.v[1] == 180);
  CHECK(cw_node_get_prop(m, "update_every", &v) && v.i == 2);
  CHECK(cw_node_get_prop(m, "camera", &v) && !strcmp(v.s, "../Cam"));
  CHECK(cw_node_get_prop(cw_node_get(n, "Screen"), "viewport_texture", &v) && !strcmp(v.s, "viewport://Feed"));
  char *s1 = cw_scene_save(n);
  CHECK(strstr(s1, "type=\"SubViewport\"") && strstr(s1, "size = vec2(320, 180)") && strstr(s1, "update_mode = WHEN_VISIBLE") &&
        strstr(s1, "update_every = 2") && strstr(s1, "camera = \"../Cam\"") && strstr(s1, "texture_name = \"Feed\""));
  CHECK(strstr(s1, "viewport_texture = \"viewport://Feed\"") && strstr(s1, "viewport_slot = COLOR"));
  CHECK(strstr(s1, "texture = \"viewport://Feed\""));
  CHECK(!strstr(s1, "renders"));
  cw_packed_scene *ps2 = cw_scene_parse(s1, NULL);
  CHECK(ps2 != NULL);
  if (ps2) {
    cw_node n2 = cw_scene_instance(ps2);
    char *s2 = cw_scene_save(n2);
    CHECK(!strcmp(s1, s2));
    uint32_t h = cw_hash32(s2, strlen(s2), CW_HASH32_INIT);
    FOLD(h);
    cw_free(s2);
    cw_node_free(n2);
    cw_packed_scene_free(ps2);
  }
  cw_free(s1);
  cw_node_free(n);
  cw_packed_scene_free(ps);
}

CW_TEST_SUITE("subviewport", { "names", test_names }, { "modes", test_modes }, { "camera_view", test_camera_view },
              { "scene_file", test_scene_file })

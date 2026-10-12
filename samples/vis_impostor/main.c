/* vis_impostor: a 7x7 grove of generated trees (a cwpack'd glTF with
 * simplified LODs) under a VisibilityManager; each tree switches to an
 * octahedral impostor at its impostor_distance, crossfading over the last 8
 * m (the impostor fades in over the first half of the band, then the mesh
 * fades out under it). The camera stays still while impostor_distance
 * sweeps from 60 m down to 6 m and back over 480 ticks, so every tree
 * crosses the band both ways and nothing else in the image moves.
 *
 * The pop check: every frame the whole image is read back and compared with
 * the previous one; vimp.max_jump is the most pixels that changed by more
 * than 24 levels between two consecutive frames. A hard switch from mesh to
 * impostor changes a whole tree at once (about a thousand pixels here); the
 * crossfade spreads it over many frames (under twenty). vimp.pop_free =
 * max_jump under 60 pixels. VIS_NO_FADE=1 sets every impostor_fade to 0
 * (hard switches): the gate's control, which must fail.
 *
 * Debug state: vimp.tick, vimp.max_jump, vimp.pop_free, vimp.impostors (drawn
 * as impostors only), vimp.fading, vimp.lod (vec4 counts), vimp.atlases,
 * vimp.visible. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/visibility.h"

#define N 7
static cw_tree *tree;
static cw_node cam;
static uint32_t tick;
static int32_t max_jump, pop_free, impostors, fading, atlases, visible, lod[4];
static uint8_t *prev, *cur;
static int have_prev;
static bool no_fade;

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

static void init(void *u) {
  (void)u;
  tree = cw_scene_app_tree();
  cw_debug_u32("vimp.tick", &tick);
  cw_debug_i32("vimp.max_jump", &max_jump);
  cw_debug_i32("vimp.pop_free", &pop_free);
  cw_debug_i32("vimp.impostors", &impostors);
  cw_debug_i32("vimp.fading", &fading);
  cw_debug_i32("vimp.atlases", &atlases);
  cw_debug_i32("vimp.visible", &visible);
  cw_debug_field("vimp.lod", lod, CW_DBG_I32, 4);
  const char *e = getenv("VIS_NO_FADE");
  no_fade = e && *e == '1';
  cw_node root = cw_tree_root(tree);
  cw_node w = add(root, "Node3D", "World");
  cw_node env = add(w, "WorldEnvironment", "Env");
  cw_node_set(env, "background", cw_val_color(0.26327f, 0.42003f, 0.69207f, 1));
  cw_node_set(env, "ambient_energy", cw_val_float(1.09956));
  cw_node_set(env, "hemi_energy", cw_val_float(1.5708));
  cw_node sun = add(w, "DirectionalLight3D", "Sun");
  cw_node3d_set_rotation(sun, (t3_vec3){ -0.8f, 0.6f, 0 });
  cw_node_set(sun, "energy", cw_val_float(5.02655));
  add(w, "VisibilityManager", "Vis");
  cam = add(w, "Camera3D", "Camera");
  cw_node_set(cam, "far", cw_val_float(400));
  cw_node ground = add(w, "MeshInstance3D", "Ground");
  cw_node_set(ground, "primitive", cw_val_enum("PLANE"));
  cw_node_set(ground, "size", cw_val_vec3(400, 1, 400));
  cw_node_set(ground, "color", cw_val_color(0.10048f, 0.14732f, 0.05088f, 1));
  uint64_t uid = cw_res_path_uid("models/tree.glb");
  for (int z = 0; z < N; z++)
    for (int x = 0; x < N; x++) {
      cw_node t = add(w, "MeshInstance3D", "Tree");
      cw_node_set(t, "mesh", (cw_value){ .type = CW_T_RES, .res = uid });
      cw_node3d_set_position(t, (t3_vec3){ (x - N / 2) * 6.0f + (z & 1) * 2.0f, 0, -(float)z * 6.0f });
      cw_node3d_set_rotation(t, (t3_vec3){ 0, (float)(x * 7 + z * 3) * 0.7f, 0 });
      cw_node_set(t, "static", cw_val_bool(true));
      cw_node_set(t, "impostor", cw_val_bool(true));
      cw_node_set(t, "impostor_distance", cw_val_float(30));
      cw_node_set(t, "impostor_fade", cw_val_float(no_fade ? 0 : 8));
    }
  int W, H;
  cw_drawable_size(&W, &H);
  prev = cw_alloc((size_t)W * H * 4, CW_MEM_GAME);
  cur = cw_alloc((size_t)W * H * 4, CW_MEM_GAME);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  /* the camera stays; the impostor distance sweeps from 60 m down to 6 m
   * over 240 ticks and back, so every tree crosses the band both ways while
   * nothing else in the image changes */
  cw_node3d_set_position(cam, (t3_vec3){ 0, 4.0f, 12 });
  cw_node3d_set_rotation(cam, (t3_vec3){ -0.16f, 0, 0 });
  float t = (float)(tick % 480) / 240.0f;
  float k = t < 1 ? t : 2 - t;
  float D = 60 - 54 * k;
  cw_node w = cw_node_get(cw_tree_root(tree), "World");
  for (int i = 0; i < cw_node_child_count(w); i++) {
    cw_node c = cw_node_child(w, i);
    if (!strcmp(cw_node_name(c), "Tree") || !strncmp(cw_node_name(c), "Tree", 4)) cw_node_set(c, "impostor_distance", cw_val_float(D));
  }
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_visibility_stats st;
  cw_visibility_get_stats(tree, &st);
  impostors = st.impostors;
  fading = st.impostor_fading;
  atlases = st.impostor_atlases;
  visible = st.visible;
  for (int k = 0; k < 4; k++) lod[k] = st.lod_counts[k];
  int W, H;
  cw_drawable_size(&W, &H);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, cur);
  if (have_prev && tick > 20) {
    int jump = 0;
    for (int i = 0; i < W * H; i++) {
      const uint8_t *p = prev + 4 * i, *c = cur + 4 * i;
      int d = abs(p[0] - c[0]);
      d = CW_MAX(d, abs(p[1] - c[1]));
      d = CW_MAX(d, abs(p[2] - c[2]));
      jump += d > 24;
    }
    if (jump > max_jump) max_jump = jump;
  }
  uint8_t *t = prev;
  prev = cur;
  cur = t;
  have_prev = 1;
  pop_free = max_jump < 60;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "vis_impostor", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render };
  return cw_scene_app(NULL, &base);
}

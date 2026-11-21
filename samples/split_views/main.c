/* split_views: what each split screen view draws, chosen per view (cw_object_set_views), in one pass.
 *
 * Four players stand round a small yard, each with a camera of its own (two views side by side, or four:
 * SV_VIEWS=4). What a view hook used to show and hide, view by view, is said once a frame as each object's views:
 *   - a marker in each player's colour, drawn only in that player's view;
 *   - three statues with two levels of detail each: the full one in the views whose camera is near it, the plain one
 *     in the others (chosen per view from each camera's distance, as the cameras move);
 *   - a group drawn in views 0 and 1 holding a crystal drawn in views 1 and 2 (so only in view 1) and a block drawn
 *     wherever its group is;
 *   - a pillar drawn in every view but view 0: it casts no sun shadow (the sun's shadow map is drawn once a frame, from
 *     what view 0 draws).
 * The frame stays in one pass (sv.array 1). SV_HOOK=1 does the same with a view hook that shows and hides each
 * object before its view draws (the frame is then drawn view by view): the gate holds the two pictures equal.
 * SV_WRONG=1 gives every object the next view's mask (the gate's control: the picture differs).
 * The other cases, each also by SV_MODE=<name>: SV_VM=1 (vm) a first person gun per camera, each on its own layer and its
 * camera's viewmodel_layers, over 4x MSAA; SV_WATER=1 (water) a pond with planar reflection and refraction;
 * SV_VIS=1 (vis) a VisibilityManager culling a ring of posts to each view's frustum; SV_VIGNETTE=1 (vignette) a
 * vignette in each view (vignette_per_view; 2: once over the screen); SV_OWN_POST=1 a post chain of each view's own;
 * SV_SPOTS=1 (spots) clustered spot lights with shadows (the shadow atlas) over the statues, a depth prepass, a scorch
 * decal and a plume of particles beside them: each view's spots cast the shadows of the statues that view draws, its
 * own level of detail, and of the casters the Shadows setting keeps from its own camera.
 *
 * Debug state: sv.views, sv.array (the last frame drew its views in one pass), sv.why (why not), sv.masks (objects with a mask),
 * sv.tick. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

enum { STATUES = 3 };
static int32_t views = 2, array_used, masks, tick;
static char why[48];
static bool hook_mode, wrong;
static cw_node cams[CW_MAX_VIEWS], marker[CW_MAX_VIEWS], statue_hi[STATUES], statue_lo[STATUES], group, crystal, block, pillar;
/* this frame's choice, per object: the views it is drawn in */
static uint32_t m_marker[CW_MAX_VIEWS], m_hi[STATUES], m_lo[STATUES], m_group, m_crystal, m_pillar;

static cw_node prim(cw_node parent, const char *name, const char *shape, t3_vec3 at, t3_vec3 size, float r, float g, float b) {
  cw_node n = cw_node_new("MeshInstance3D", name);
  cw_node_set(n, "primitive", cw_val_enum(shape));
  cw_node_set(n, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(n, "color", cw_val_color(r, g, b, 1));
  cw_node_set(n, "cast_shadow", cw_val_bool(true));
  cw_node_set(n, "receive_shadow", cw_val_bool(true));
  cw_node3d_set_position(n, at);
  cw_node_add_child(parent, n);
  return n;
}

static void cam_place(int k) {
  /* each camera on its side of the yard, swaying in and out so the statues it is near change */
  static const float side[CW_MAX_VIEWS][2] = { { 0, 1 }, { 1, 0 }, { 0, -1 }, { -1, 0 } };
  float s = (float)tick * 0.012f + (float)k * 1.3f, r = 13.0f + 6.0f * cw_sin(s);
  t3_vec3 at = { side[k][0] * r + side[k][1] * 3 * cw_sin(s * 0.7f), 3.0f + (float)k * 0.6f, side[k][1] * r - side[k][0] * 3 * cw_sin(s * 0.7f) };
  cw_node3d_set_position(cams[k], at);
  cw_node3d_look_at(cams[k], (t3_vec3){ 0, 1.2f, 0 }, (t3_vec3){ 0, 1, 0 });
}

static uint32_t shift(uint32_t m) { return wrong ? ((m << 1) | (m >> (views - 1))) & ((1u << views) - 1) : m; }

/* the views each object is drawn in, from where the cameras are now */
static void choose(void) {
  uint32_t all = (1u << views) - 1;
  for (int k = 0; k < CW_MAX_VIEWS; k++) m_marker[k] = k < views ? 1u << k : 0;
  for (int i = 0; i < STATUES; i++) {
    t3_vec3 p = cw_node3d_position(statue_hi[i]);
    m_hi[i] = 0;
    for (int k = 0; k < views; k++) {
      t3_vec3 c = cw_node3d_position(cams[k]);
      float dx = p.x - c.x, dz = p.z - c.z;
      if (dx * dx + dz * dz < 15.0f * 15.0f) m_hi[i] |= 1u << k;
    }
    m_lo[i] = all & ~m_hi[i];
  }
  m_group = 0x3;
  m_crystal = 0x6;
  m_pillar = all & ~1u;
}

static void apply_masks(void) {
  for (int k = 0; k < CW_MAX_VIEWS; k++) cw_node3d_set_views(marker[k], shift(m_marker[k]));
  for (int i = 0; i < STATUES; i++) cw_node3d_set_views(statue_hi[i], shift(m_hi[i])), cw_node3d_set_views(statue_lo[i], shift(m_lo[i]));
  cw_node3d_set_views(group, shift(m_group));
  cw_node3d_set_views(crystal, shift(m_crystal));
  cw_node3d_set_views(pillar, shift(m_pillar));
}

/* the old way: before each view draws, its objects shown and the rest hidden */
static void show(cw_node n, uint32_t m, int view) {
  t3_object *o = cw_node3d_object(n);
  if (o) o->visible = (m >> view & 1) != 0;
}
static void view_hook(int view, void *u) {
  (void)u;
  for (int k = 0; k < CW_MAX_VIEWS; k++) show(marker[k], m_marker[k], view);
  for (int i = 0; i < STATUES; i++) show(statue_hi[i], m_hi[i], view), show(statue_lo[i], m_lo[i], view);
  show(group, m_group, view);
  show(crystal, m_crystal, view);
  show(pillar, m_pillar, view);
}

static cw_node cam_new(cw_node world, int k) {
  char name[16];
  snprintf(name, sizeof name, "Camera%d", k + 1);
  cw_node c = cw_node_new("Camera3D", name);
  cw_node_add_child(world, c);
  cw_node_set(c, "fov", cw_val_float(60));
  cw_node_set(c, "near", cw_val_float(0.1f));
  cw_node_set(c, "far", cw_val_float(300));
  return c;
}

/* a switch: its own variable, or SV_MODE naming it (one variable for the program survey: SV_MODE=vm|water|vis|vignette|v4) */
static bool sv(const char *name, const char *mode) {
  const char *e = getenv(name), *m = getenv("SV_MODE");
  return (e && atoi(e)) || (m && !strcmp(m, mode));
}

/* the gate's other cases: a viewmodel per camera, a pond with planar water, a VisibilityManager, a vignette, spots */
static void features(cw_node world) {
  const char *e;
  if (sv("SV_SPOTS", "spots")) {
    cw_node_set(cw_node_get(world, "Lighting"), "mode", cw_val_enum("CLUSTERED"));
    cw_node_set(cw_node_get(world, "Post"), "depth_prepass", cw_val_bool(true));
    cw_node_set(cw_node_get(world, "Sun"), "energy", cw_val_float(0.15f)); /* (dusk: the spots light the yard) */
    for (int i = 0; i < STATUES; i++) { /* a spot over each statue, aimed past it so its shadow falls on the yard */
      float a = (float)i * 2.0943951f;
      char name[16];
      snprintf(name, sizeof name, "Spot%d", i);
      cw_node sp = cw_node_new("SpotLight3D", name);
      cw_node_add_child(world, sp);
      cw_node3d_set_position(sp, (t3_vec3){ 10.5f * cw_sin(a + 0.5f), 6.5f, 10.5f * cw_cos(a + 0.5f) });
      cw_node3d_look_at(sp, (t3_vec3){ 5 * cw_sin(a - 0.2f), 0, 5 * cw_cos(a - 0.2f) }, (t3_vec3){ 0, 1, 0 });
      cw_node_set(sp, "color", cw_val_color(1, 0.9f, 0.75f, 1));
      cw_node_set(sp, "energy", cw_val_float(420));
      cw_node_set(sp, "range", cw_val_float(30));
      cw_node_set(sp, "angle", cw_val_float(0.7f));
      cw_node_set(sp, "penumbra", cw_val_float(0.3f));
      cw_node_set(sp, "shadow", cw_val_bool(true));
      cw_node_set(sp, "shadow_size", cw_val_int(1024));
      cw_node_set(sp, "shadow_bias", cw_val_float(-0.0005f));
    }
    cw_node d = cw_node_new("Decal3D", "Scorch");
    cw_node_add_child(world, d);
    cw_node3d_set_position(d, (t3_vec3){ 4.5f, 0, 2.5f });
    cw_node3d_set_rotation(d, (t3_vec3){ -CW_PI / 2, 0, 0.4f });
    cw_node_set(d, "size", cw_val_vec3(4, 4, 1.2f));
    cw_node_set(d, "texture", cw_val_string("decals/scorch.png"));
    cw_node_set(d, "blend", cw_val_enum("MULTIPLY"));
    cw_node_set(d, "normal_fade", cw_val_float(0));
    cw_node plume = cw_node_new("GPUParticles3D", "Plume");
    cw_node_add_child(world, plume);
    cw_node3d_set_position(plume, (t3_vec3){ -4.5f, 0.3f, 3 });
    cw_node_set(plume, "amount", cw_val_int(200));
    cw_node_set(plume, "lifetime", cw_val_float(1.6f));
    cw_node_set(plume, "emission_shape", cw_val_enum("SPHERE"));
    cw_node_set(plume, "emission_radius", cw_val_float(0.6f));
    cw_node_set(plume, "speed_min", cw_val_float(0.6f));
    cw_node_set(plume, "speed_max", cw_val_float(1.4f));
    cw_node_set(plume, "gravity", cw_val_vec3(0, 0.8f, 0));
    cw_node_set(plume, "size", cw_val_float(0.9f));
    cw_node_set(plume, "color", cw_val_color(0.55f, 0.5f, 0.45f, 0.5f));
    cw_node_set(plume, "seed", cw_val_int(3));
  }
  if (sv("SV_VM", "vm")) cw_node_set(cw_node_get(world, "Post"), "msaa", cw_val_int(4)); /* (the guns over a multisampled scene) */
  if (sv("SV_VM", "vm"))
    for (int k = 0; k < CW_MAX_VIEWS; k++) {
      /* each view's gun on a layer of its own, which only its camera's viewmodel pass draws */
      cw_node_set(cams[k], "viewmodel_layers", cw_val_int(1 << (4 + k)));
      cw_node_set(cams[k], "viewmodel_fov", cw_val_float(50));
      cw_node gun = prim(cams[k], "Gun", "BOX", (t3_vec3){ 0.25f, -0.22f, -0.6f }, (t3_vec3){ 0.08f, 0.1f, 0.5f }, 0.3f + 0.2f * (float)k, 0.3f, 0.35f);
      cw_node_set(gun, "layers", cw_val_int(1 << (4 + k)));
      cw_node_set(gun, "cast_shadow", cw_val_bool(false));
    }
  if (sv("SV_WATER", "water")) {
    cw_node w = cw_node_new("Water3D", "Pond");
    cw_node_add_child(world, w);
    cw_node3d_set_position(w, (t3_vec3){ 0, 0.15f, 0 });
    cw_node_set(w, "size", cw_val_vec2(10, 10));
    cw_node_set(w, "subdivisions", cw_val_int(48));
    cw_node_set(w, "reflection", cw_val_bool(true));
    cw_node_set(w, "refraction", cw_val_bool(true));
  }
  if (sv("SV_VIS", "vis")) {
    cw_node vis = cw_node_new("VisibilityManager", "Visibility");
    cw_node_set(vis, "frustum_cull", cw_val_bool(true));
    cw_node_set(vis, "occlusion_cull", cw_val_bool(false));
    cw_node_set(vis, "lod", cw_val_bool(false));
    cw_node_set(vis, "impostors", cw_val_bool(false));
    cw_node_add_child(world, vis);
    for (int i = 0; i < 24; i++) { /* a ring of posts round the yard, most outside some view */
      char name[16];
      snprintf(name, sizeof name, "Post%d", i);
      float a = (float)i * 0.2617994f;
      prim(world, name, "BOX", (t3_vec3){ 20 * cw_sin(a), 1, 20 * cw_cos(a) }, (t3_vec3){ 0.6f, 2, 0.6f }, 0.6f, 0.4f, 0.3f);
    }
  }
  if (sv("SV_VIGNETTE", "vignette")) {
    e = getenv("SV_VIGNETTE");
    cw_node post = cw_node_get(world, "Post");
    cw_node_set(post, "bloom", cw_val_bool(false));
    cw_node_set(post, "vignette", cw_val_bool(true));
    cw_node_set(post, "vignette_intensity", cw_val_float(0.45f));
    cw_node_set(post, "vignette_smoothness", cw_val_float(0.5f));
    cw_node_set(post, "vignette_per_view", cw_val_bool(!e || atoi(e) == 1));
  }
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("sv.views", &views);
  cw_debug_i32("sv.array", &array_used);
  cw_debug_i32("sv.masks", &masks);
  cw_debug_i32("sv.tick", &tick);
  cw_debug_bytes("sv.why", why, sizeof why);
  const char *e = getenv("SV_VIEWS");
  views = (e && atoi(e) == 4) || sv("SV_V4", "v4") ? 4 : 2;
  hook_mode = (e = getenv("SV_HOOK")) && atoi(e);
  wrong = (e = getenv("SV_WRONG")) && atoi(e);
  cw_node world = cw_node_get(cw_tree_root(cw_scene_app_tree()), "World");
  static const float col[CW_MAX_VIEWS][3] = { { 0.9f, 0.15f, 0.1f }, { 0.1f, 0.4f, 0.95f }, { 0.15f, 0.8f, 0.2f }, { 0.95f, 0.8f, 0.1f } };
  for (int k = 0; k < CW_MAX_VIEWS; k++) {
    cams[k] = cam_new(world, k);
    char name[16];
    snprintf(name, sizeof name, "Marker%d", k + 1);
    marker[k] = prim(world, name, "BOX", (t3_vec3){ -1.5f + (float)k, 2.6f, -1.5f + (float)k }, (t3_vec3){ 0.7f, 0.7f, 0.7f }, col[k][0], col[k][1], col[k][2]);
  }
  for (int i = 0; i < STATUES; i++) {
    float a = (float)i * 2.0943951f;
    t3_vec3 at = { 7 * cw_sin(a), 1.2f, 7 * cw_cos(a) };
    char name[16];
    snprintf(name, sizeof name, "StatueHi%d", i);
    statue_hi[i] = prim(world, name, "SPHERE", at, (t3_vec3){ 2.2f, 2.4f, 2.2f }, 0.8f, 0.78f, 0.7f);
    snprintf(name, sizeof name, "StatueLo%d", i);
    statue_lo[i] = prim(world, name, "BOX", at, (t3_vec3){ 1.6f, 2.4f, 1.6f }, 0.55f, 0.5f, 0.45f);
  }
  group = cw_node_new("Node3D", "Group");
  cw_node_add_child(world, group);
  cw_node3d_set_position(group, (t3_vec3){ 3, 0, -3 });
  crystal = prim(group, "Crystal", "SPHERE", (t3_vec3){ 0, 2.2f, 0 }, (t3_vec3){ 1, 1.6f, 1 }, 0.5f, 0.2f, 0.9f);
  block = prim(group, "Block", "BOX", (t3_vec3){ 0, 0.6f, 0 }, (t3_vec3){ 1.4f, 1.2f, 1.4f }, 0.2f, 0.7f, 0.7f);
  pillar = prim(world, "Pillar", "CYLINDER", (t3_vec3){ -3, 2.5f, -3 }, (t3_vec3){ 0.8f, 5, 0.8f }, 0.75f, 0.75f, 0.8f);
  features(world);
  for (int k = 0; k < views; k++) cam_place(k);
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  cw_pipeline_split(p, cams, views);
  cw_pipeline_set_views_shared_post(p, !((e = getenv("SV_OWN_POST")) && atoi(e)));
  if (hook_mode) cw_pipeline_set_view_hook(p, view_hook, NULL);
  choose();
  if (!hook_mode) apply_masks();
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  for (int k = 0; k < views; k++) cam_place(k);
  choose();
  if (!hook_mode) apply_masks();
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  const char *w = NULL;
  array_used = cw_pipeline_views_array_used(cw_pipeline_of(cw_scene_app_tree()), &w) ? 1 : 0;
  snprintf(why, sizeof why, "%s", w ? w : "");
  masks = 0;
  for (int k = 0; k < CW_MAX_VIEWS; k++) masks += cw_object_views(cw_node3d_object(marker[k])) != CW_VIEWS_ALL;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "split_views", .width = 1280, .height = 720, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}

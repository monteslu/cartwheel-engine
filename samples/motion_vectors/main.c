/* motion_vectors: the velocity target with the motion of the things that move in it, under TAA.
 *
 * Three movers in front of a still yard and a still camera, each posed from the frame number so every host
 * draws the same frame:
 *   Spinner   a node turning 0.17 rad a frame (a mesh: its world matrix)
 *   Walker    a skinned glTF character (Khronos CesiumMan, CC-BY-4.0, from three.c's test assets) walking in
 *             place at three times its clip's speed while its node slides side to side (bone matrices)
 *   Swarm     one InstancedMesh of 48 small boxes on two counter-turning rings (instance matrices)
 * Frames 1 to 60 show the picture under TAA, frames 61 to 90 the velocity buffer (PostProcess debug_view =
 * VELOCITY: 0.5 grey is still), then the picture again.
 *
 * MV_REF (tests/motion-vectors.mjs, read as cw_test_env reads): 1 draws the frames without TAA (with
 * CW_RENDER_SCALE: the supersampled picture a TAA frame is measured against), 2 the same without the movers,
 * 3 the yard without the movers under TAA (a still picture: TAA must not soften it).
 *
 * MV_CROWD=1 (read as cw_test_env reads) adds a crowd from the frame it is first seen: 32 more walkers in rows behind,
 * 12 swarms of 240 boxes whose instance matrices are written every frame (each box's scale breathing, as a
 * crossfade rewrites a scatter cell's) and 16 small turning bars: about 60 meshes in the velocity target, the
 * velocity pass's cost on a busy frame (tests/velocity-objects.mjs).
 *
 * Debug fields: cw.velocity.objects (meshes drawn into the velocity target), mv.frame; gpu.scene, gpu.velocity and
 * gpu.frame, the GPU ms of the scene pass, the velocity pass and the frame (averages, -1 until the pass timers run:
 * CW_GPU_TIMERS=1 or the "pipeline" debug command). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/scene_file.h"

#define SWARM 48
#define CROWD_WALKERS 32
#define CROWD_SWARMS 12
#define CROWD_SWARM 240
#define CROWD_BARS 16
static t3_gltf *gltf;
static t3_instanced_mesh *swarm;
static int32_t frame_no;
static float gpu_scene = -1, gpu_velocity = -1, gpu_frame = -1;
static cw_node yard;
static struct {
  bool on;
  t3_gltf *man[CROWD_WALKERS];
  t3_animation_mixer *mix[CROWD_WALKERS];
  t3_instanced_mesh *swarm[CROWD_SWARMS];
  t3_mesh *bar[CROWD_BARS];
} crowd;

/* the crowd, under the yard's 3D object (MV_CROWD) */
static void crowd_make(void) {
  crowd.on = true;
  t3_object *root = cw_node3d_object(yard);
  size_t len;
  void *glb = cw_asset_load("CesiumMan.glb", &len);
  if (!glb) return;
  for (int k = 0; k < CROWD_WALKERS; k++) {
    crowd.man[k] = t3_gltf_parse(glb, len, NULL, NULL);
    t3_object *m = crowd.man[k]->scene;
    t3_object_set_position(m, (float)(k % 8) * 1.3f - 4.55f, 0, -0.6f - (float)(k / 8) * 0.55f);
    t3_object_set_scale(m, 1.1f, 1.1f, 1.1f);
    t3_object_add(root, m);
    crowd.mix[k] = t3_animation_mixer_new(m);
    t3_animation_action_play(t3_animation_mixer_clip_action(crowd.mix[k], crowd.man[k]->animations[0]));
    t3_animation_mixer_update(crowd.mix[k], 0.37f * (float)k);
  }
  free(glb);
  t3_geometry *g = t3_box_geometry_new(0.06f, 0.06f, 0.06f, 1, 1, 1);
  for (int k = 0; k < CROWD_SWARMS; k++) {
    t3_material *m = t3_mesh_standard_material_new(0x3fa0c8u + (uint32_t)k * 0x0a0400u);
    crowd.swarm[k] = t3_instanced_mesh_new(g, m, CROWD_SWARM);
    crowd.swarm[k]->mesh.base.frustum_culled = false;
    t3_object_set_position(crowd.swarm[k], (float)(k % 6) * 1.6f - 4.0f, 0.15f + (float)(k / 6) * 2.6f, 1.6f);
    t3_object_add(root, crowd.swarm[k]);
    t3_release(m);
  }
  t3_release(g);
  t3_geometry *bg = t3_box_geometry_new(0.5f, 0.05f, 0.05f, 1, 1, 1);
  t3_material *bm = t3_mesh_standard_material_new(0xe0c040);
  for (int k = 0; k < CROWD_BARS; k++) {
    crowd.bar[k] = t3_mesh_new(bg, bm);
    t3_object_set_position(crowd.bar[k], (float)(k % 8) * 1.1f - 3.85f, 2.6f + (float)(k / 8) * 0.4f, 1.0f);
    t3_object_add(root, crowd.bar[k]);
  }
  t3_release(bg);
  t3_release(bm);
}

static void crowd_pose(float t) {
  for (int k = 0; k < CROWD_WALKERS; k++)
    if (crowd.mix[k]) t3_animation_mixer_update(crowd.mix[k], 1.0f / 60);
  for (int k = 0; k < CROWD_SWARMS; k++) {
    t3_instanced_mesh *im = crowd.swarm[k];
    for (int i = 0; i < CROWD_SWARM; i++) {
      float x = (float)(i % 20) * 0.07f - 0.67f, z = (float)(i / 20) * 0.07f - 0.4f;
      float sc = 0.6f + 0.4f * (0.5f + 0.5f * sinf(t * 0.05f + (float)i * 0.31f + (float)k));
      t3_mat4 mtx;
      t3_mat4_compose(&mtx, (t3_vec3){ x, 0, z }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ sc, sc, sc });
      t3_instanced_mesh_set_matrix_at(im, i, &mtx);
    }
    im->instance_matrix->version++;
  }
  for (int k = 0; k < CROWD_BARS; k++) t3_object_set_rotation(crowd.bar[k], 0, 0, t * 0.05f * (float)(k % 5 + 1));
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("mv.frame", &frame_no);
  cw_debug_f32("gpu.scene", &gpu_scene);
  cw_debug_f32("gpu.velocity", &gpu_velocity);
  cw_debug_f32("gpu.frame", &gpu_frame);
  yard = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Yard");
  size_t len;
  void *glb = cw_asset_load("CesiumMan.glb", &len);
  if (!glb) { cw_error("motion_vectors: CesiumMan.glb missing from the assets"); return; }
  gltf = t3_gltf_parse(glb, len, NULL, NULL); /* kept until shutdown: its objects use names it owns */
  free(glb);
  cw_node3d_set_object(cw_node_get(yard, "Walker/Model"), gltf->scene);
  cw_anim walk = cw_anim_from_t3_clip(gltf->animations[0], "Walker/Model");
  cw_node anim = cw_node_get(yard, "Anim");
  cw_anim_player_add(anim, "walk", walk);
  cw_anim_release(walk);
  cw_anim_player_play_ex(anim, "walk", 0, 3.0f, false);

  t3_geometry *g = t3_box_geometry_new(0.16f, 0.16f, 0.16f, 1, 1, 1);
  t3_material *m = t3_mesh_standard_material_new(0x3fc8a0);
  m->roughness = 0.6f;
  swarm = t3_instanced_mesh_new(g, m, SWARM);
  swarm->mesh.base.frustum_culled = false;
  t3_release(g);
  t3_release(m);
  cw_node3d_set_object(cw_node_get(yard, "Swarm"), (t3_object *)swarm);
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  frame_no++;
  const char *ref = cw_test_env("MV_REF");
  int mode = ref ? atoi(ref) : 0;
  cw_node post = cw_node_get(yard, "Post");
  cw_node_set(post, "taa", cw_val_bool(mode == 0 || mode == 3));
  cw_node_set(post, "debug_view", cw_val_enum(frame_no > 60 && frame_no <= 90 && mode == 0 ? "VELOCITY" : "NONE"));
  const char *const movers[] = { "Spinner", "Walker", "Swarm" };
  for (int i = 0; i < 3; i++) cw_node_set(cw_node_get(yard, movers[i]), "visible", cw_val_bool(mode != 2 && mode != 3));
  const char *cr = cw_test_env("MV_CROWD");
  if (cr && *cr == '1' && !crowd.on) crowd_make();

  float t = (float)frame_no;
  cw_node3d_set_rotation(cw_node_get(yard, "Spinner"), (t3_vec3){ 0, 0, t * 0.17f });
  float s, c;
  cw_sincos(t * 0.11f, &s, &c);
  cw_node3d_set_position(cw_node_get(yard, "Walker"), (t3_vec3){ s * 0.9f, 0, 0.6f });
  if (swarm) {
    for (int i = 0; i < SWARM; i++) {
      int ring = i & 1;
      float a = (float)i * (6.2831853f / SWARM) + t * (ring ? -0.13f : 0.09f);
      float r = ring ? 0.75f : 1.1f, sa, ca;
      cw_sincos(a, &sa, &ca);
      t3_quat q = { 0, sa * 0.5f, 0, 1 };
      float qn = 1.0f / sqrtf(q.y * q.y + 1);
      q.y *= qn, q.w *= qn;
      t3_mat4 mtx;
      t3_mat4_compose(&mtx, (t3_vec3){ ca * r, sa * r, ring ? 0.25f : -0.25f }, q, (t3_vec3){ 1, 1, 1 });
      t3_instanced_mesh_set_matrix_at(swarm, i, &mtx);
    }
    swarm->instance_matrix->version++;
  }
  if (crowd.on) crowd_pose(t);
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  const cw_pass_stat *st;
  int n = p ? cw_pipeline_pass_stats(p, &st) : 0;
  for (int i = 0; i < n; i++) {
    if (!strcmp(st[i].name, "scene")) gpu_scene = st[i].gpu_ms_avg;
    if (!strcmp(st[i].name, "velocity")) gpu_velocity = st[i].gpu_ms_avg;
  }
  if (p) gpu_frame = cw_pipeline_gpu_ms(p);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "motion_vectors", .width = 640, .height = 360, .debug = true, .init = init, .update = update };
  return cw_scene_app("main.cwscene", &base);
}

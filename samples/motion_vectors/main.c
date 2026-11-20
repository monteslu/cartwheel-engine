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
 * CW_RENDER_SCALE: the supersampled picture a TAA frame is measured against), 2 the same without the movers.
 *
 * Debug fields: cw.velocity.objects (meshes drawn into the velocity target), mv.frame. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/scene_file.h"

#define SWARM 48
static t3_gltf *gltf;
static t3_instanced_mesh *swarm;
static int32_t frame_no;
static cw_node yard;

static void init(void *u) {
  (void)u;
  cw_debug_i32("mv.frame", &frame_no);
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
  cw_node_set(post, "taa", cw_val_bool(mode == 0));
  cw_node_set(post, "debug_view", cw_val_enum(frame_no > 60 && frame_no <= 90 && mode == 0 ? "VELOCITY" : "NONE"));
  const char *const movers[] = { "Spinner", "Walker", "Swarm" };
  for (int i = 0; i < 3; i++) cw_node_set(cw_node_get(yard, movers[i]), "visible", cw_val_bool(mode != 2));

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
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "motion_vectors", .width = 640, .height = 360, .debug = true, .init = init, .update = update };
  return cw_scene_app("main.cwscene", &base);
}

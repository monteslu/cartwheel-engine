/* anim_feet: FootPlacement3D on a runner over stony ground (cartwheel/ik.h). A generated person (the coat figure of
 * samples/anim_robe, make-figures.mjs, its garment left hanging) runs at 5.4 m/s through a field of stones from
 * 5 cm to 50 cm high, turns right at speed, turns right again sharply, stops, turns half round on the spot, runs
 * again over a 22 cm platform with a left turn and stops. Its feet are placed at full influence the whole time. A
 * twin plays the same clips with a FootPlacement3D whose rays hit nothing (collision_mask 0): its legs are the
 * animation's.
 *
 * A fixed script, tick by tick:
 *     0 ..  59  stand
 *    60 .. 329  run: up to 5.4 m/s in 0.3 s, a quarter turn right over a second (140 .. 200), a sharp quarter turn
 *               right (230 .. 260), a stop (306 .. 330)
 *   330 .. 419  stand
 *   420 .. 449  a half turn on the spot
 *   450 .. 479  stand
 *   480 .. 659  run: over the platform, a quarter turn left (540 .. 600), a stop (636 .. 660)
 *   660 .. 779  stand
 * The body's height eases after the floor and the platform under its middle (it steps over the stones, which sit on
 * a collision layer of their own); the taller stones lie beside its path, under its swinging feet.
 *
 * Debug fields (fp.*):
 *   jump       each leg bone's (thighs, shins, feet) turn in one tick, in the model's frame, less the twin's turn of
 *              the same bone that tick: how far the drawn leg moved past the animation; the largest (degrees)
 *   jump_run, jump_stand   the same in the runs (pace > 0) and while standing or turning on the spot
 *   jump_at, jump_bone     the tick and the bone (0..5: UpperLegL, LowerLegL, FootL, UpperLegR, LowerLegR, FootR)
 *   raised     stance ticks of a foot on ground 5 cm or more above the body's (a stone, the platform) whose ground
 *              had stayed within 5 mm for 8 ticks
 *   placed     the share of them with the ankle within 2 cm of the ground's height plus its animated lift (the
 *              feet are still placed)
 *   hash_win   the runner's pose hash after each 60-tick window
 * and fp.ok: jump <= 12 degrees, raised >= 30 and placed >= 0.9.
 *
 * Test switches (cw_test_env, read at tick 2): FP_OFF=1 (the runner's FootPlacement3D at influence 0: nothing
 * placed, the placement check's control). The engine's own CW_TEST_FOOT_NO_REACH=1 and CW_TEST_FOOT_NO_RATE=1 turn
 * off its two guards (the jump check's controls). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/ik.h"
#include "cartwheel/physics3d.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene_file.h"

#define TICKS 780
#define RUN 5.4f       /* m/s */
#define JUMP_MAX 12.0f /* degrees a tick past the animation */
#define NB 6
#define MAX_STONES 160
#define PI 3.14159265f

typedef struct person {
  cw_node mover, model, anim, feet;
  t3_gltf *gltf;
  float x, z, y, yaw;
  bool running;
} person;

static person P[2]; /* the runner, the twin */
static cw_node level, camera;
static const char *BONES[NB] = { "UpperLegL", "LowerLegL", "FootL", "UpperLegR", "LowerLegR", "FootR" };
static int bone_i[2][NB];
static bool bones_found;
static t3_quat prev_q[2][NB];
static float ground_hist[2][8];
static int32_t tick_i, jump_at, jump_bone, raised, placed_n, ok;
static float jump, jump_run, jump_stand, placed, fold = 9;
static uint32_t hash_win[13];

static t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

static float qangle(t3_quat a, t3_quat b) {
  float c = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 2 * cw_acos(c > 1 ? 1 : c);
}

/* #region path and course */
static float pace_at(int t) {
  if (t >= 60 && t < 330) return cw_smoothstep(60, 78, (float)t) * (1 - cw_smoothstep(306, 330, (float)t));
  if (t >= 480 && t < 660) return cw_smoothstep(480, 498, (float)t) * (1 - cw_smoothstep(636, 660, (float)t));
  return 0;
}
static float yaw_at(int t) {
  float y = 0;
  y -= 0.5f * PI * cw_smoothstep(140, 200, (float)t);
  y -= 0.5f * PI * cw_smoothstep(230, 260, (float)t);
  y += PI * cw_smoothstep(420, 450, (float)t);
  y += 0.5f * PI * cw_smoothstep(540, 600, (float)t);
  return y;
}

static uint32_t rng = 0x9e3779b9u;
static float rnd(void) {
  rng = rng * 1664525u + 1013904223u;
  return (float)(rng >> 8) / 16777216.0f;
}

static void box(const char *name, t3_vec3 at, t3_vec3 size, float yaw, float shade, int layer) {
  cw_node b = add(level, "StaticBody3D", name);
  cw_node_set(b, "collision_layer", cw_val_int(layer));
  cw_node3d_set_position(b, at);
  cw_node3d_set_rotation(b, v3(0, yaw, 0));
  cw_node s = add(b, "CollisionShape3D", "Shape");
  cw_node_set(s, "shape", cw_val_enum("BOX"));
  cw_node_set(s, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node m = add(b, "MeshInstance3D", "Mesh");
  cw_node_set(m, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(m, "color", cw_val_color(0.22f * shade, 0.2f * shade, 0.17f * shade, 1));
  cw_node_set(m, "cast_shadow", cw_val_bool(true));
  cw_node_set(m, "receive_shadow", cw_val_bool(true));
}

/* stones along the runner's path (its ticks run in advance), every 0.45 m of the runs, from 5 to 50 cm high; the
 * ones over 30 cm lie at least 0.24 m off the path's middle, under the feet and beside the body; a 22 cm platform
 * across the second run */
static void course(void) {
  float x = 0, z = 0, since = 0;
  char name[24];
  int n = 0;
  for (int t = 0; t < TICKS && n < MAX_STONES; t++) {
    float s, c, yaw = yaw_at(t), d = RUN * pace_at(t) / 60;
    cw_sincos(yaw, &s, &c);
    x += s * d, z += c * d;
    since += d;
    if (t >= 480 && t < 500) continue; /* the platform's edge, clear */
    if (since < 0.45f || t < 80) continue;
    since = 0;
    /* one draw a statement: C leaves the order of a call's arguments open, and the hosts' compilers differ */
    float r = rnd();
    float h = 0.05f + 0.45f * r * sqrtf(r);
    float o = (rnd() * 2 - 1) * 0.55f;
    if (h > 0.3f && fabsf(o) < 0.24f) o = o < 0 ? -0.24f - 0.2f * rnd() : 0.24f + 0.2f * rnd();
    float sx = 0.18f + 0.3f * rnd();
    float sz = 0.18f + 0.3f * rnd();
    float yaw_s = rnd() * PI;
    float shade = 0.8f + 0.5f * rnd();
    snprintf(name, sizeof name, "Stone%d", n++);
    box(name, v3(x + c * o, h * 0.5f, z - s * o), v3(sx, h, sz), yaw_s, shade, 1);
  }
  /* the platform: the second run starts at the stand's place, facing back the way it came */
  float s, c;
  int t0 = 500;
  x = z = 0;
  for (int t = 0; t < t0; t++) {
    float sn, cs, d = RUN * pace_at(t) / 60;
    cw_sincos(yaw_at(t), &sn, &cs);
    x += sn * d, z += cs * d;
  }
  cw_sincos(yaw_at(t0), &s, &c);
  box("Platform", v3(x + s * 1.2f, 0.11f, z + c * 1.2f), v3(1.6f, 0.22f, 3.0f), yaw_at(t0) + 0.5f * PI, 1.2f, 0);
}
/* #endregion */

static float ground_at(float x, float z, float from_y) {
  cw_world3d w = cw_physics3d_world(cw_scene_app_tree());
  cw_ray_hit3d hit;
  cw_phys_filter f = cw_phys_filter_layers(1); /* the floor and the platform (layer 0), not the stones (layer 1) */
  if (!cw_world3d_valid(w) || !cw_world3d_raycast(w, v3(x, from_y, z), v3(0, -from_y - 1, 0), &f, &hit)) return 0;
  return hit.point.y;
}

static void load_person(int i, const void *glb, size_t len) {
  person *p = &P[i];
  char path[32];
  snprintf(path, sizeof path, "%s/Model", cw_node_name(p->mover));
  p->gltf = t3_gltf_parse(glb, len, NULL, NULL);
  cw_node3d_set_object(p->model, p->gltf->scene);
  for (int k = 0; k < p->gltf->animation_count; k++) {
    const char *name = p->gltf->animations[k]->name;
    cw_anim a = cw_anim_from_t3_clip(p->gltf->animations[k], path);
    cw_anim_set_loop(a, CW_LOOP_LINEAR);
    cw_anim_player_add(p->anim, !strcmp(name, "Run") ? "run" : "idle", a);
    cw_anim_release(a);
  }
  cw_anim_player_play(p->anim, "idle", 0);
}

static void init(void *u) {
  (void)u;
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  camera = cw_node_get(level, "Camera");
  static const char *movers[2] = { "Runner", "Twin" };
  for (int i = 0; i < 2; i++) {
    P[i].mover = cw_node_get(level, movers[i]);
    P[i].model = cw_node_get(P[i].mover, "Model");
    P[i].feet = cw_node_get(P[i].model, "Feet");
    P[i].anim = add(level, "AnimationPlayer", i ? "AnimTwin" : "AnimRunner");
  }
  P[1].x = 30; /* the twin runs the same path 30 m off, its rays hitting nothing */
  course();
  cw_debug_i32("fp.tick", &tick_i);
  cw_debug_f32("fp.jump", &jump);
  cw_debug_f32("fp.jump_run", &jump_run);
  cw_debug_f32("fp.jump_stand", &jump_stand);
  cw_debug_i32("fp.jump_at", &jump_at);
  cw_debug_i32("fp.jump_bone", &jump_bone);
  cw_debug_i32("fp.raised", &raised);
  cw_debug_f32("fp.placed", &placed);
  cw_debug_f32("fp.fold", &fold);
  cw_debug_field("fp.hash_win", hash_win, CW_DBG_U32, 13);
  cw_debug_i32("fp.ok", &ok);
}

static void start(void) {
  size_t len;
  void *glb = cw_asset_load("coated.glb", &len);
  if (!glb) { cw_error("anim_feet: coated.glb missing from the assets"); return; }
  for (int i = 0; i < 2; i++) load_person(i, glb, len);
  free(glb);
  if (cw_test_env("FP_OFF")) cw_node_set(P[0].feet, "influence", cw_val_float(0));
}

/* #region probe */
static void probe(int t) {
  const cw_pose *pz[2] = { cw_skeleton_pose(P[0].model), cw_skeleton_pose(P[1].model) };
  if (!pz[0] || !pz[1]) return;
  if (!bones_found)
    for (int i = 0; i < 2; i++)
      for (int b = 0; b < NB; b++) bone_i[i][b] = cw_pose_find(pz[i], BONES[b]);
  bones_found = true;
  t3_quat q[2][NB];
  for (int i = 0; i < 2; i++)
    for (int b = 0; b < NB; b++) q[i][b] = cw_pose_model_rotation(pz[i], bone_i[i][b]);
  if (t > 6)
    for (int b = 0; b < NB; b++) {
      float j = (qangle(prev_q[0][b], q[0][b]) - qangle(prev_q[1][b], q[1][b])) * 57.29578f;
      bool run = pace_at(t) > 0 || pace_at(t - 1) > 0;
      if (run && j > jump_run) jump_run = j;
      if (!run && j > jump_stand) jump_stand = j;
      if (j > jump) jump = j, jump_at = t, jump_bone = b;
    }
  memcpy(prev_q, q, sizeof q);
  /* fold: each drawn ankle's height under its hip against the least the reach may leave it (half the animated
   * height, or a quarter of the leg when that is less); the smallest share */
  for (int l = 0; l < 2; l++) {
    int up = bone_i[0][l * 3], an = bone_i[0][l * 3 + 2], kn = bone_i[0][l * 3 + 1];
    t3_vec3 h1 = cw_pose_model_position(pz[1], up), k1 = cw_pose_model_position(pz[1], kn), a1 = cw_pose_model_position(pz[1], an);
    float gap1 = h1.y - a1.y, len = sqrtf((k1.x - h1.x) * (k1.x - h1.x) + (k1.y - h1.y) * (k1.y - h1.y) + (k1.z - h1.z) * (k1.z - h1.z)) +
                               sqrtf((a1.x - k1.x) * (a1.x - k1.x) + (a1.y - k1.y) * (a1.y - k1.y) + (a1.z - k1.z) * (a1.z - k1.z));
    float keep = 0.5f * gap1 < 0.25f * len ? 0.5f * gap1 : 0.25f * len;
    if (t < 20 || keep < 0.05f) continue;
    float gap0 = cw_pose_model_position(pz[0], up).y - cw_pose_model_position(pz[0], an).y;
    if (gap0 / keep < fold) fold = gap0 / keep;
  }
  /* placed: a planted foot on raised, steady ground sits on it */
  float fh = 0.0835f;
  for (int l = 0; l < 2; l++) {
    cw_foot_state st;
    if (!cw_foot_placement_leg(P[0].feet, l, &st)) continue;
    float g = st.hit ? st.ground.y : -1;
    bool steady = st.hit;
    for (int k = 0; k < 8; k++) steady = steady && fabsf(ground_hist[l][k] - g) < 0.005f;
    memmove(&ground_hist[l][1], &ground_hist[l][0], sizeof(float) * 7);
    ground_hist[l][0] = g;
    if (t < 20 || !steady || st.lift > fh + 0.02f || st.offset < 0.05f) continue;
    t3_vec3 a = cw_skeleton_bone_world(P[0].model, l ? "FootR" : "FootL");
    float err = fabsf((a.y - g) - st.lift);
    raised++;
    if (err < 0.02f) placed_n++;
  }
  placed = raised ? (float)placed_n / (float)raised : 0;
  ok = jump <= JUMP_MAX && raised >= 30 && placed >= 0.9f && fold >= 0.9f;
}
/* #endregion */

static void move(person *p, int t, float dt, bool climb) {
  float pace = pace_at(t);
  bool run = pace > 0.3f;
  if (run != p->running) cw_anim_player_play(p->anim, run ? "run" : "idle", 0.15f);
  p->running = run;
  p->yaw = yaw_at(t);
  float s, c;
  cw_sincos(p->yaw, &s, &c);
  p->x += s * RUN * pace * dt;
  p->z += c * RUN * pace * dt;
  if (climb) { /* a controller's body: on the floor and the platform, eased (it steps over the stones) */
    float g = ground_at(p->x, p->z, p->y + 0.35f);
    p->y += (g - p->y) * (1 - cw_exp(-10 * dt));
  }
  cw_node3d_set_position(p->mover, v3(p->x, p->y, p->z));
  cw_node3d_set_rotation(p->mover, v3(0, cw_wrap_angle(p->yaw), 0));
}

static void fixed(void *u, double dt) {
  (void)u;
  int t = (int)cw_tree_tick(cw_scene_app_tree());
  tick_i = t;
  if (t == 2) start();
  if (!P[0].gltf) return;
  /* this tick's modifiers already ran (after the nodes, before this callback) */
  if (t > 3) probe(t);
  if (t % 60 == 59 && t / 60 < 13) hash_win[t / 60] = cw_skeleton_hash(P[0].model);
  move(&P[0], t, (float)dt, true);
  move(&P[1], t, (float)dt, false);
  /* the camera from the runner's left and behind, easing after it */
  float s, c;
  cw_sincos(P[0].yaw, &s, &c);
  t3_vec3 at = v3(P[0].x, P[0].y + 0.7f, P[0].z), cp = cw_node3d_position(camera);
  t3_vec3 want = v3(at.x - s * 3.2f + c * 2.4f, at.y + 1.1f, at.z - c * 3.2f - s * 2.4f);
  float k = t < 4 ? 1 : 1 - cw_exp(-3 * (float)dt);
  cw_node3d_set_position(camera, v3(cp.x + (want.x - cp.x) * k, cp.y + (want.y - cp.y) * k, cp.z + (want.z - cp.z) * k));
  cw_node3d_look_at(camera, at, v3(0, 1, 0));
}

static void shutdown(void *u) {
  (void)u;
  for (int i = 0; i < 2; i++)
    if (P[i].gltf) {
      t3_release(P[i].gltf);
      P[i].gltf = NULL;
    }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "anim_feet", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}

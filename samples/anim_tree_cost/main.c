/* anim_tree_cost: what one AnimationTree evaluation costs on a game-sized character, measured (docs/guides/animation.md,
 * "Cost"). No art: the skeleton and the clips are made here, shaped like a shooter's soldier.
 *
 *   skeleton  60 bones: pelvis, three spine bones, neck, head; per side a clavicle, upper arm (and its twist), lower
 *             arm (and its twist), hand and fifteen finger bones; thigh (and its twist), calf (and its twist), foot, ball
 *   clips     every bone's position and rotation keyed 30 times a second over one second (120 tracks, as a glTF
 *             exporter writes them); a lean keys three spine bones
 *   graph     a state machine of two synced 2D blend spaces (idle and eight directions each, forward and backward)
 *             and an air state, under a time scale; four additive leans; the arms (hold and kick) layered on the
 *             clavicles and below; a death blend; eleven one-shots with bone masks; a time scale on top
 *
 * ATC_CROWD=<n> characters (default 32), each its own model, player and tree. Each tick sets the parameters a game sets
 * (eleven, by name) and advances every tree once (MANUAL). Debug fields, host clock, never compared across hosts:
 * atc.tree_us (one advance, microseconds, mean over ticks 30 on), atc.tree_us_min (the quietest tick), atc.param_us
 * (the eleven parameter sets), atc.bones, atc.tracks (per full-body clip), atc.ticks. ATC_DIVISOR=<d> sets an update
 * divisor on every tree: atc.tree_us is then the mean per advance call, about 1/d of the cost.
 * atc.hash: the trees' pose hashes folded (the same on every host: the result does not depend on the timing). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"
#include "cartwheel/scene.h"

#define MAX_CROWD 256
#define NB 60

static const char *BONE[NB];
static int PARENT[NB];
static char names[NB][24];
static int nb;

static int bone(const char *name, int parent) {
  snprintf(names[nb], sizeof names[nb], "%s", name);
  BONE[nb] = names[nb], PARENT[nb] = parent;
  return nb++;
}

static void make_skeleton(void) {
  int pelvis = bone("pelvis", -1), s1 = bone("spine_01", pelvis), s2 = bone("spine_02", s1), s3 = bone("spine_03", s2);
  int neck = bone("neck_01", s3);
  bone("Head", neck);
  for (int side = 0; side < 2; side++) {
    const char *x = side ? "r" : "l";
    char n[24];
    snprintf(n, sizeof n, "clavicle_%s", x);
    int cl = bone(n, s3);
    snprintf(n, sizeof n, "upperarm_%s", x);
    int ua = bone(n, cl);
    snprintf(n, sizeof n, "upperarm_twist_%s", x);
    bone(n, ua);
    snprintf(n, sizeof n, "lowerarm_%s", x);
    int la = bone(n, ua);
    snprintf(n, sizeof n, "lowerarm_twist_%s", x);
    bone(n, la);
    snprintf(n, sizeof n, "hand_%s", x);
    int h = bone(n, la);
    static const char *const F[5] = { "thumb", "index", "middle", "ring", "pinky" };
    for (int f = 0; f < 5; f++) {
      int p = h;
      for (int k = 1; k <= 3; k++) {
        snprintf(n, sizeof n, "%s_%02d_%s", F[f], k, x);
        p = bone(n, p);
      }
    }
    snprintf(n, sizeof n, "thigh_%s", x);
    int th = bone(n, pelvis);
    snprintf(n, sizeof n, "thigh_twist_%s", x);
    bone(n, th);
    snprintf(n, sizeof n, "calf_%s", x);
    int ca = bone(n, th);
    snprintf(n, sizeof n, "calf_twist_%s", x);
    bone(n, ca);
    snprintf(n, sizeof n, "foot_%s", x);
    int ft = bone(n, ca);
    snprintf(n, sizeof n, "ball_%s", x);
    bone(n, ft);
  }
}

static t3_object *model_object(void) {
  t3_object *root = t3_group_new(), *b[NB];
  for (int i = 0; i < nb; i++) {
    b[i] = t3_bone_new();
    b[i]->name = BONE[i];
    b[i]->position = (t3_vec3){ 0, PARENT[i] < 0 ? 1.0f : 0.1f, 0 };
    t3_object_add(PARENT[i] < 0 ? root : b[PARENT[i]], b[i]);
    t3_release(b[i]); /* the parent holds it */
  }
  return root;
}

static t3_quat axis_angle(float ax, float ay, float az, float a) {
  float s = sinf(a * 0.5f), l = sqrtf(ax * ax + ay * ay + az * az);
  return (t3_quat){ ax / l * s, ay / l * s, az / l * s, cosf(a * 0.5f) };
}

/* a full-body clip: every bone's position and rotation, 31 keys over a second; `seed` makes clips differ */
static int tracks_per_clip;
static cw_anim full_clip(const char *name, int seed, cw_loop_mode loop) {
  cw_anim a = cw_anim_new(name, 1, loop);
  cw_anim_set_stored(a, false);
  int tracks = 0;
  for (int i = 0; i < nb; i++) {
    char path[64];
    snprintf(path, sizeof path, "Model@%s:position", BONE[i]);
    int tp = cw_anim_add_value_track(a, path, CW_INTERP_LINEAR);
    snprintf(path, sizeof path, "Model@%s:quaternion", BONE[i]);
    int tq = cw_anim_add_value_track(a, path, CW_INTERP_LINEAR);
    tracks += 2;
    for (int k = 0; k <= 30; k++) {
      float t = (float)k / 30.0f, ph = 6.2831853f * t + 0.37f * (float)(i + seed);
      cw_anim_add_key(a, tp, t, cw_val_vec3(0.002f * sinf(ph), (PARENT[i] < 0 ? 1.0f : 0.1f) + 0.002f * cosf(ph), 0));
      t3_quat q = axis_angle(1, 0.3f * (float)(seed % 3), 0.1f, 0.4f * sinf(ph));
      cw_anim_add_key(a, tq, t, (cw_value){ CW_T_QUAT, .v = { q.x, q.y, q.z, q.w } });
    }
  }
  tracks_per_clip = tracks;
  return a;
}

/* a lean: three spine bones turned a little about one axis (an additive's target pose) */
static cw_anim lean_clip(const char *name, float ax, float az) {
  cw_anim a = cw_anim_new(name, 1, CW_LOOP_LINEAR);
  cw_anim_set_stored(a, false);
  static const char *const S[3] = { "spine_01", "spine_02", "spine_03" };
  for (int i = 0; i < 3; i++) {
    char path[64];
    snprintf(path, sizeof path, "Model@%s:quaternion", S[i]);
    int tq = cw_anim_add_value_track(a, path, CW_INTERP_LINEAR);
    t3_quat q = axis_angle(ax, 0, az, 0.08f);
    cw_anim_add_key(a, tq, 0, (cw_value){ CW_T_QUAT, .v = { q.x, q.y, q.z, q.w } });
  }
  return a;
}

#define NSHOT 11
static const char *const SHOTS[NSHOT] = { "reload", "throw", "bash", "hit_f", "hit_b", "hit_l", "hit_r", "hit_head", "wave", "point", "land" };
static const char *const LOCO_F[8] = { "run_f", "run_fl", "run_l", "run_bl", "run_b", "run_br", "run_r", "run_fr" };
static const char *const LOCO_B[8] = { "back_f", "back_fl", "back_l", "back_bl", "back_b", "back_br", "back_r", "back_fr" };
static const char *const LEANS[4] = { "lean_l", "lean_r", "lean_fw", "lean_bk" };
static const char *const PARAMS[11] = { "mx", "my", "back", "air", "rate", "lean_l", "lean_r", "lean_fw", "lean_bk", "armed", "kick" };

static void filter(cw_node t, cw_blend_id id, const char *spec) {
  const char *nm[24];
  char buf[256];
  snprintf(buf, sizeof buf, "%s", spec);
  int n = 0;
  for (char *p = strtok(buf, "|"); p && n < 24; p = strtok(NULL, "|")) nm[n++] = p;
  cw_animtree_set_filter(t, id, nm, n);
}

static void make_graph(cw_node t) {
  cw_blend_id lf = cw_animtree_blend2d(t, "mx", "my"), lb = cw_animtree_blend2d(t, "mx", "my");
  cw_animtree_blend2d_add(t, lf, cw_animtree_clip(t, "idle"), 0, 0);
  cw_animtree_blend2d_add(t, lb, cw_animtree_clip(t, "idle"), 0, 0);
  for (int k = 0; k < 8; k++) {
    float a = 0.785398f * (float)k;
    cw_animtree_blend2d_add(t, lf, cw_animtree_clip(t, LOCO_F[k]), 4 * sinf(a), 4 * cosf(a));
    cw_animtree_blend2d_add(t, lb, cw_animtree_clip(t, LOCO_B[k]), 3 * sinf(a), 3 * cosf(a));
  }
  cw_animtree_set_sync(t, lf, true);
  cw_animtree_set_sync(t, lb, true);
  cw_blend_id sm = cw_animtree_state_machine(t);
  cw_animtree_sm_add_state(t, sm, "front", lf);
  cw_animtree_sm_add_state(t, sm, "back", lb);
  cw_animtree_sm_add_state(t, sm, "air", cw_animtree_clip(t, "air"));
  cw_transition tr = { .xfade = 0.2f, .advance = CW_ADVANCE_AUTO, .sync = true };
  tr.condition = "back > 0.5", cw_animtree_sm_add_transition(t, sm, "front", "back", &tr);
  tr.condition = "back < 0.5", cw_animtree_sm_add_transition(t, sm, "back", "front", &tr);
  tr.sync = false, tr.condition = "air > 0.5";
  cw_animtree_sm_add_transition(t, sm, "front", "air", &tr);
  cw_animtree_sm_add_transition(t, sm, "back", "air", &tr);
  tr.xfade = 0.1f, tr.condition = "air < 0.5", cw_animtree_sm_add_transition(t, sm, "air", "front", &tr);
  cw_animtree_sm_start(t, sm, "front");
  cw_blend_id base = cw_animtree_time_scale(t, sm, "rate");
  cw_blend_id arms = cw_animtree_blend2(t, cw_animtree_clip(t, "hold"), cw_animtree_clip(t, "kick"), "kick");
  for (int k = 0; k < 4; k++) base = cw_animtree_add(t, base, cw_animtree_clip(t, LEANS[k]), LEANS[k]);
  base = cw_animtree_layer(t, base, arms, "armed");
  filter(t, base, "clavicle_l+|clavicle_r+");
  base = cw_animtree_blend2(t, base, cw_animtree_clip(t, "dead"), "lying");
  static const char *const MASK[NSHOT] = {
    NULL, "pelvis|spine_01|thigh_l+|thigh_r+", "clavicle_l+|clavicle_r+", "spine_02|spine_03|clavicle_l+|clavicle_r+", "spine_01+",
    "spine_01|spine_02|spine_03|neck_01|Head", "spine_01|spine_02|spine_03|neck_01|Head", "spine_01|spine_02|spine_03|neck_01|Head",
    "spine_01|spine_02|spine_03|neck_01|Head", "spine_01|spine_02|spine_03|neck_01|Head", NULL };
  for (int k = 0; k < NSHOT; k++) {
    base = cw_animtree_oneshot(t, base, cw_animtree_clip(t, SHOTS[k]), SHOTS[k], 0.05f, 0.15f);
    if (MASK[k]) filter(t, base, MASK[k]);
  }
  cw_animtree_set_root(t, cw_animtree_time_scale(t, base, "all"));
  cw_animtree_set_param(t, "all", 1);
  cw_animtree_set_param(t, "rate", 1);
  cw_animtree_set_param(t, "armed", 1);
}

static cw_tree *tree;
static cw_node tree_of[MAX_CROWD];
static t3_object *obj_of[MAX_CROWD];
static int32_t crowd = 32, bones_n, tracks_n, ticks, divisor = 1;
static uint32_t hash;
static double tree_ns, param_ns;
static int timed;
static float tree_us, tree_us_min = 1e9f, param_us;

static void init(void *u) {
  (void)u;
  const char *e = getenv("ATC_CROWD");
  if (e) crowd = CW_CLAMP(atoi(e), 1, MAX_CROWD);
  e = getenv("ATC_DIVISOR");
  if (e) divisor = CW_CLAMP(atoi(e), 1, 64);
  make_skeleton();
  cw_scene_register_builtin_types();
  tree = cw_tree_new();
  cw_node level = cw_node_new("Node3D", "Level");
  cw_node_add_child(cw_tree_root(tree), level);
  /* the library: every clip once, shared by every character's player */
  cw_anim lib[64];
  const char *lib_name[64];
  int nl = 0, seed = 0;
  lib_name[nl] = "idle", lib[nl++] = full_clip("idle", seed++, CW_LOOP_LINEAR);
  for (int k = 0; k < 8; k++) lib_name[nl] = LOCO_F[k], lib[nl++] = full_clip(LOCO_F[k], seed++, CW_LOOP_LINEAR);
  for (int k = 0; k < 8; k++) lib_name[nl] = LOCO_B[k], lib[nl++] = full_clip(LOCO_B[k], seed++, CW_LOOP_LINEAR);
  lib_name[nl] = "air", lib[nl++] = full_clip("air", seed++, CW_LOOP_LINEAR);
  lib_name[nl] = "hold", lib[nl++] = full_clip("hold", seed++, CW_LOOP_LINEAR);
  lib_name[nl] = "kick", lib[nl++] = full_clip("kick", seed++, CW_LOOP_LINEAR);
  lib_name[nl] = "dead", lib[nl++] = full_clip("dead", seed++, CW_LOOP_NONE);
  for (int k = 0; k < NSHOT; k++) lib_name[nl] = SHOTS[k], lib[nl++] = full_clip(SHOTS[k], seed++, CW_LOOP_NONE);
  static const float LA[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
  for (int k = 0; k < 4; k++) lib_name[nl] = LEANS[k], lib[nl++] = lean_clip(LEANS[k], LA[k][0], LA[k][1]);
  for (int i = 0; i < crowd; i++) {
    char n[32];
    snprintf(n, sizeof n, "C%d", i);
    cw_node c = cw_node_new("Node3D", n);
    cw_node_add_child(level, c);
    cw_node model = cw_node_new("Node3D", "Model");
    cw_node_add_child(c, model);
    obj_of[i] = model_object();
    t3_object_add(cw_node3d_object(model), obj_of[i]);
    cw_node anim = cw_node_new("AnimationPlayer", "Anim");
    cw_node_add_child(c, anim);
    cw_node_set(anim, "root_node", cw_val_string(".."));
    for (int k = 0; k < nl; k++) cw_anim_player_add(anim, lib_name[k], lib[k]);
    cw_node t = cw_node_new("AnimationTree", "Tree");
    cw_node_add_child(c, t);
    cw_node_set(t, "player", cw_val_string("../Anim"));
    cw_node_set(t, "process", cw_val_enum("MANUAL"));
    make_graph(t);
    if (divisor > 1) cw_animtree_set_update_divisor(t, divisor);
    tree_of[i] = t;
  }
  for (int k = 0; k < nl; k++) cw_anim_release(lib[k]);
  bones_n = nb, tracks_n = tracks_per_clip;
  cw_debug_i32("atc.crowd", &crowd);
  cw_debug_i32("atc.bones", &bones_n);
  cw_debug_i32("atc.tracks", &tracks_n);
  cw_debug_i32("atc.ticks", &ticks);
  cw_debug_u32("atc.hash", &hash);
  cw_debug_f32("atc.tree_us", &tree_us);         /* host clock: never compared */
  cw_debug_f32("atc.tree_us_min", &tree_us_min); /* the quietest tick */
  cw_debug_f32("atc.param_us", &param_us);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  ticks++;
  uint64_t t0 = cw_profile_now_ns();
  for (int i = 0; i < crowd; i++) {
    /* each runs a slow circle at its own phase: the 2D blend has three clips weighted, as a running character's */
    float ph = 0.01f * (float)ticks + 0.5f * (float)i;
    float v[11] = { 3 * sinf(ph), 3 * cosf(ph), 0, 0, 1, 0.2f, 0, 0.1f, 0, 1, 0 };
    for (int k = 0; k < 11; k++) cw_animtree_set_param(tree_of[i], PARAMS[k], v[k]);
    if ((ticks + i * 13) % 120 == 0) cw_animtree_set_param(tree_of[i], SHOTS[(ticks / 120 + i) % NSHOT], 1);
  }
  uint64_t t1 = cw_profile_now_ns();
  for (int i = 0; i < crowd; i++) cw_animtree_advance(tree_of[i], dt);
  uint64_t t2 = cw_profile_now_ns();
  if (ticks >= 30) {
    timed++;
    tree_ns += (double)(t2 - t1), param_ns += (double)(t1 - t0);
    tree_us = (float)(tree_ns / timed / 1000.0 / crowd);
    param_us = (float)(param_ns / timed / 1000.0 / crowd);
    float now = (float)((double)(t2 - t1) / 1000.0 / crowd);
    if (now < tree_us_min) tree_us_min = now;
  }
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < crowd; i++) h = cw_hash32(&(uint32_t){ cw_animtree_hash(tree_of[i]) }, 4, h);
  hash = h;
}

static void render(void *u, double a) {
  (void)u, (void)a;
  cw_gpu_clear_window(0.1f, 0.2f, 0.3f, 1);
}

static void shutdown(void *u) {
  (void)u;
  cw_tree_free(tree);
  for (int i = 0; i < crowd; i++) t3_release(obj_of[i]);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "anim_tree_cost", .width = 64, .height = 64, .debug = true,
                        .init = init, .fixed_update = fixed_update, .render = render, .shutdown = shutdown };
}

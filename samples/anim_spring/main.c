/* anim_spring: secondary motion with SpringBone3D (cartwheel/ik.h). A
 * generated person in a long coat (samples/anim_spring/make-character.mjs)
 * has a ponytail and two coat tails on bone chains; the SpringBone3D under
 * the model (main.cwscene) swings them, and capsules on the body, the
 * shoulders and the thighs and a sphere on the head keep them out of it.
 *
 * A fixed script, tick by tick:
 *     0 ..  89  stands (the chains settle under their gravity)
 *    90 .. 209  walks forward (the tails trail behind)
 *   210 .. 299  stands again (they swing back and settle)
 *   300 .. 399  spins on the spot: three turns, up to 17 rad/s
 *   400 .. 479  stands
 *   480         teleports 6 m back along the course (a twin behind, made
 *               in code from the scene's nodes, does all the same but this)
 *   540         cw_spring_bone3d_reset
 *   541 .. 570  turns to face back along the course
 *   572 ..      walks back
 *
 * Debug fields (sb.*), per chain (ponytail, left tail, right tail):
 *   lag        the most the tip trailed its animated place while walking (m)
 *   settle     ticks after the walk until the tip is back within 1 cm of
 *              where it hung before the walk, for good
 *   pen        the deepest any simulated joint of any chain went inside any
 *              collision shape, over the whole run, and during the spin (m;
 *              <= 0 means outside)
 *   spin_lag   the most a tip trailed during the spin (the spin moved them)
 *   whip       the most a tip's offset differs from the twin's over the
 *              second after the teleport (m)
 *   reset_ok   the tick of the reset call put every tip on its animated place
 *   hash_win   the model's pose hash after each 60-tick window
 *   us         modifier phase microseconds per character (host clock, never compared)
 * and the oks the gates expect: motion_ok, collide_ok, teleport_ok.
 *
 * Test switches (cw_test_env, read at tick 2): SPRING_OFF (the modifier
 * inactive), SPRING_NO_COLLIDE (collisions false), SPRING_NO_TELEPORT
 * (teleport_distance 0), SPRING_CROWD=n (n characters, for the cost). */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/ik.h"
#include "cartwheel/scene_file.h"

#define MAXC 16
#define NCH 3
#define SPEED 1.38f /* m/s: the walk cycle's stride */

typedef struct person {
  cw_node mover, model, anim, springs;
  t3_gltf *gltf;
  float x, z, yaw;
  bool walking;
} person;

static person P[MAXC];
static int crowd = 2;
static cw_node level, camera, chains[NCH], twin[NCH], shapes[8];
static int nshapes;

/* debug state */
static int32_t tick_i, settle[NCH], motion_ok, collide_ok, teleport_ok, reset_ok, resets;
static float lag[NCH], pen_all, pen_spin, spin_lag[NCH], whip[NCH], rest_dev[NCH], us, us_min = 1e9f;
static uint32_t hash_win[10];
static t3_vec3 rest_off[NCH];
static int last_far[NCH];
static double us_sum;
static int us_n;

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

/* #region copy */
/* another character's SpringBone3D: the scene's, property for property */
static void copy_props(cw_node from, cw_node to) {
  for (const cw_node_type *t = cw_node_get_type(from); t && strcmp(t->name, "Node"); t = t->base)
    for (const cw_prop *p = t->props; p && p->name; p++) {
      cw_value v;
      if (!(p->flags & CW_PROP_STORED) || (p->flags & CW_PROP_READONLY) || !cw_node_get_prop(from, p->name, &v)) continue;
      cw_node_set(to, p->name, v);
    }
}
static cw_node copy_tree(cw_node from, cw_node parent) {
  cw_node n = add(parent, cw_node_get_type(from)->name, cw_node_name(from));
  copy_props(from, n);
  for (int i = 0; i < cw_node_child_count(from); i++) copy_tree(cw_node_child(from, i), n);
  return n;
}
/* #endregion */

static void set_shadows(t3_object *o) {
  if (o->type == T3_SKINNED_MESH || o->type == T3_MESH) o->cast_shadow = true;
  for (int i = 0; i < o->child_count; i++) set_shadows(o->children[i]);
}

static void load_person(int i, const void *glb, size_t len) {
  person *p = &P[i];
  char path[32];
  snprintf(path, sizeof path, "%s/Model", cw_node_name(p->mover));
  p->gltf = t3_gltf_parse(glb, len, NULL, NULL);
  cw_node3d_set_object(p->model, p->gltf->scene);
  set_shadows(p->gltf->scene);
  for (int k = 0; k < p->gltf->animation_count; k++) {
    const char *name = p->gltf->animations[k]->name;
    cw_anim a = cw_anim_from_t3_clip(p->gltf->animations[k], path);
    cw_anim_set_loop(a, CW_LOOP_LINEAR);
    cw_anim_player_add(p->anim, !strcmp(name, "Walk") ? "walk" : "idle", a);
    cw_anim_release(a);
  }
  p->x = -3;
  p->z = -1.4f * (float)i;
  p->yaw = 1.5707963f;
  cw_anim_player_play(p->anim, "idle", 0);
  if (i >= 2) cw_anim_player_seek(p->anim, 0.37f * (float)i, false); /* the crowd out of step; the twin in step */
}

static void make_person(int i) {
  char name[32];
  snprintf(name, sizeof name, "Mover%d", i);
  person *p = &P[i];
  p->mover = add(level, "Node3D", name);
  p->model = add(p->mover, "Node3D", "Model");
  p->springs = copy_tree(P[0].springs, p->model);
  snprintf(name, sizeof name, "Anim%d", i);
  p->anim = add(level, "AnimationPlayer", name);
}

static void init(void *u) {
  (void)u;
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  camera = cw_node_get(level, "Camera");
  P[0].mover = cw_node_get(level, "Mover");
  P[0].model = cw_node_get(level, "Mover/Model");
  P[0].anim = cw_node_get(level, "Anim");
  P[0].springs = cw_node_get(P[0].model, "Springs");
  static const char *cn[NCH] = { "Ponytail", "CoatL", "CoatR" };
  for (int c = 0; c < NCH; c++) chains[c] = cw_node_get(P[0].springs, cn[c]);
  for (int i = 0; i < cw_node_child_count(P[0].springs) && nshapes < 8; i++) {
    cw_node ch = cw_node_child(P[0].springs, i);
    if (cw_node_is(ch, "SpringBoneCollision3D")) shapes[nshapes++] = ch;
  }
  for (int c = 0; c < NCH; c++) settle[c] = -1;

  cw_debug_i32("sb.tick", &tick_i);
  cw_debug_f32_array("sb.lag", lag, NCH);
  cw_debug_field("sb.settle", settle, CW_DBG_I32, NCH);
  cw_debug_f32_array("sb.rest_dev", rest_dev, NCH);
  cw_debug_f32("sb.pen", &pen_all);
  cw_debug_f32("sb.pen_spin", &pen_spin);
  cw_debug_f32_array("sb.spin_lag", spin_lag, NCH);
  cw_debug_f32_array("sb.whip", whip, NCH);
  cw_debug_i32("sb.resets", &resets);
  cw_debug_i32("sb.reset_ok", &reset_ok);
  cw_debug_i32("sb.motion_ok", &motion_ok);
  cw_debug_i32("sb.collide_ok", &collide_ok);
  cw_debug_i32("sb.teleport_ok", &teleport_ok);
  cw_debug_field("sb.hash_win", hash_win, CW_DBG_U32, 10);
  cw_debug_f32("sb.us", &us);         /* host clock: never compared */
  cw_debug_f32("sb.us_min", &us_min); /* the quietest tick */
  pen_all = pen_spin = -1;
}

/* the crowd, and the glTF for everyone: at tick 2, when a cart host's test switches have arrived */
static void start(void) {
  const char *e = cw_test_env("SPRING_CROWD");
  crowd = e ? atoi(e) : 2;
  if (crowd < 2) crowd = 2;
  if (crowd > MAXC) crowd = MAXC;
  for (int i = 1; i < crowd; i++) make_person(i);
  size_t len;
  void *glb = cw_asset_load("walker.glb", &len);
  if (!glb) { cw_error("anim_spring: walker.glb missing from the assets"); return; }
  for (int i = 0; i < crowd; i++) load_person(i, glb, len);
  free(glb);
  for (int c = 0; c < NCH; c++) twin[c] = cw_node_get(P[1].springs, cw_node_name(chains[c]));
  for (int i = 0; i < crowd; i++) {
    if (cw_test_env("SPRING_OFF")) cw_node_set(P[i].springs, "active", cw_val_bool(false));
    if (cw_test_env("SPRING_NO_COLLIDE")) cw_node_set(P[i].springs, "collisions", cw_val_bool(false));
    if (cw_test_env("SPRING_NO_TELEPORT")) cw_node_set(P[i].springs, "teleport_distance", cw_val_float(0));
  }
}

static t3_vec3 tip_offset_of(cw_node chain) {
  cw_value v;
  if (!cw_node_get_prop(chain, "tip_offset", &v)) return (t3_vec3){ 0, 0, 0 };
  return (t3_vec3){ v.v[0], v.v[1], v.v[2] };
}
static t3_vec3 tip_offset(int c) { return tip_offset_of(chains[c]); }
static float vlen3(t3_vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static t3_vec3 vsub3(t3_vec3 a, t3_vec3 b) { return (t3_vec3){ a.x - b.x, a.y - b.y, a.z - b.z }; }

/* #region probe */
/* the deepest any simulated joint sits inside any shape (m; negative: the nearest is outside by that much) */
static float penetration(void) {
  float worst = -1;
  for (int c = 0; c < NCH; c++) {
    t3_vec3 j;
    for (int i = 0; cw_spring_chain3d_joint_world(chains[c], i, &j); i++)
      for (int s = 0; s < nshapes; s++) {
        t3_vec3 a, b;
        float r;
        if (!cw_spring_collision3d_world(shapes[s], &a, &b, &r)) continue;
        t3_vec3 ab = vsub3(b, a), aj = vsub3(j, a);
        float ll = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z, t = ll > 0 ? (aj.x * ab.x + aj.y * ab.y + aj.z * ab.z) / ll : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        t3_vec3 q = { a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t };
        float d = r - vlen3(vsub3(j, q));
        if (d > worst) worst = d;
      }
  }
  return worst;
}
/* #endregion */

static void probe(int t) {
  float pen = penetration();
  if (t > 2 && pen > pen_all) pen_all = pen;
  if (t >= 300 && t < 430 && pen > pen_spin) pen_spin = pen;
  for (int c = 0; c < NCH; c++) {
    t3_vec3 o = tip_offset(c);
    float d = vlen3(o);
    if (t == 89) rest_off[c] = o;
    if (t >= 90 && t < 210 && d > lag[c]) lag[c] = d;
    if (t >= 210 && t < 300) {
      float dev = vlen3(vsub3(o, rest_off[c]));
      if (dev >= 0.01f) last_far[c] = t;
      rest_dev[c] = dev;
      if (t == 299) settle[c] = last_far[c] >= 299 ? 999 : last_far[c] < 210 ? 0 : last_far[c] - 209;
    }
    if (t >= 300 && t < 400 && d > spin_lag[c]) spin_lag[c] = d;
    if (t >= 480 && t < 540) {
      float w = vlen3(vsub3(o, tip_offset_of(twin[c])));
      if (w > whip[c]) whip[c] = w;
    }
  }
  if (t == 541) { /* the reset asked for at 540 ran on this tick */
    reset_ok = 1;
    for (int c = 0; c < NCH; c++)
      if (vlen3(tip_offset(c)) != 0) reset_ok = 0;
  }
  motion_ok = collide_ok = teleport_ok = 1;
  for (int c = 0; c < NCH; c++) {
    if (!(lag[c] > 0.03f) || settle[c] < 0 || settle[c] > 75) motion_ok = 0;
    if (!(spin_lag[c] > 0.05f)) collide_ok = 0;
    if (!(whip[c] < 0.001f)) teleport_ok = 0;
  }
  if (!(pen_spin <= 0)) collide_ok = 0;
  cw_value v;
  if (cw_node_get_prop(P[0].springs, "resets", &v)) resets = (int32_t)v.i;
}

static void move(person *p, bool is_twin, int t, float dt) {
  bool walk = (t >= 90 && t < 210) || t >= 572;
  if (walk != p->walking) cw_anim_player_play(p->anim, walk ? "walk" : "idle", 0.2f);
  p->walking = walk;
  /* the pace eases up and down over a third of a second (a person does not start or stop in one step) */
  float pace = t < 300 ? cw_smoothstep(90, 110, (float)t) * (1 - cw_smoothstep(190, 210, (float)t)) : cw_smoothstep(572, 592, (float)t);
  p->x += (t >= 572 ? -1 : 1) * SPEED * pace * dt;
  /* three turns in 100 ticks (up to 17 rad/s); later a half turn to face back along the course */
  if (t >= 300 && t <= 400) p->yaw = cw_wrap_angle(1.5707963f + 6 * 3.1415927f * cw_smoothstep(300, 400, (float)t));
  if (t >= 541 && t <= 570) p->yaw = 1.5707963f + 3.1415927f * cw_smoothstep(541, 570, (float)t);
  if (t == 480 && !is_twin) p->x -= 6;
  cw_node3d_set_position(p->mover, (t3_vec3){ p->x, 0, p->z });
  cw_node3d_set_rotation(p->mover, (t3_vec3){ 0, p->yaw, 0 });
}

static void fixed(void *u, double dt) {
  (void)u;
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  int t = (int)tick;
  tick_i = t;
  if (t == 2) start();
  if (!P[0].gltf) return;
  /* this tick's modifiers already ran (after the nodes, before this callback): probe them, then move for the next tick */
  probe(t);
  if (t % 60 == 59 && t / 60 < 10) hash_win[t / 60] = cw_skeleton_hash(P[0].model);
  if (t > 30) {
    float now = 0;
    for (int i = 0; i < crowd; i++) now += cw_skeleton_last_us(P[i].model);
    us_sum += now;
    us_n++;
    us = (float)(us_sum / us_n);
    if (now < us_min) us_min = now;
  }
  if (t == 540)
    for (int i = 0; i < crowd; i++) cw_spring_bone3d_reset(P[i].springs);
  for (int i = 0; i < crowd; i++) move(&P[i], i == 1, t, (float)dt);
  /* the camera keeps the person in view from their left */
  t3_vec3 cp = cw_node3d_position(camera);
  float k = t == 480 ? 1 : 1 - cw_exp(-4 * (float)dt);
  cw_node3d_set_position(camera, (t3_vec3){ cp.x + (P[0].x - 0.4f - cp.x) * k, cp.y, cp.z });
}

static void shutdown(void *u) {
  (void)u;
  for (int i = 0; i < crowd; i++)
    if (P[i].gltf) {
      t3_release(P[i].gltf);
      P[i].gltf = NULL;
    }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "anim_spring", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}

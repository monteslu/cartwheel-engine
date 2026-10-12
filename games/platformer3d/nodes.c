/* platformer3d node types: the game logic a script would hold. Each type
 * extends an engine node type and is placed in .cwscene files by name, with
 * its own reflected properties:
 *
 *   Hero         < CharacterBody3D  run, jump + double jump, dash, coyote
 *                                    time and jump buffering, facing, the
 *                                    AnimationTree parameters (idle/run blend,
 *                                    jump, fall, dash, cheer)
 *   FollowCamera < Camera3D         smoothed chase camera; a ray cast from the
 *                                    target to the wanted spot pulls it in
 *                                    front of walls
 *   Pickup       < Area3D           coins and gems: score, sound, pop tween
 *   Hazard       < Area3D           lava, saws: costs a life
 *   Checkpoint   < Area3D           moves the respawn point, flag turns green
 *   Goal         < Area3D           ends the level
 *   LevelInfo    < Node3D           every scene's root: title, kind, next
 *                                    scene, music, kill height
 *
 * Area signals reach these through [connection] lines in the scenes
 * (method="collect" / "hurt" / "reach" / "finish"), resolved by name in
 * game_resolve_method. Only cartwheel/math.h transcendentals are used:
 * everything here feeds the simulation. */
#include <string.h>

#include "game.h"

/* ── small helpers ──────────────────────────────────────────────────── */

static t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }
static t3_vec3 vadd(t3_vec3 a, t3_vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static t3_vec3 vscale(t3_vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float vlen(t3_vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static t3_vec3 get_vec3(cw_node n, const char *prop) {
  cw_value v;
  cw_node_get_prop(n, prop, &v);
  return v3(v.v[0], v.v[1], v.v[2]);
}
static bool get_bool(cw_node n, const char *prop) {
  cw_value v;
  return cw_node_get_prop(n, prop, &v) && v.b;
}

/* ── Hero ───────────────────────────────────────────────────────────── */

typedef struct hero {
  /* tuning (reflected: tweak live with `set Level/Player jump_speed 9`) */
  float speed, accel, air_accel, jump_speed, double_jump_speed, gravity, fall_gravity, max_fall;
  float dash_speed, dash_time, coyote_time, buffer_time, turn_speed;
  /* state */
  t3_vec3 run;        /* horizontal velocity we steer (kept apart from the clipped velocity) */
  t3_vec3 dash_dir;
  float facing;       /* model yaw, radians (+Z = 0) */
  float coyote, buffer, dash_t, air_time;
  int air_jumps;
  bool dash_ready, jump_cut, was_floor, cheering;
  /* animation */
  cw_blend_id sm;
} hero;
static const cw_node_type *hero_t;

static void hero_init(cw_node n, void *d) {
  (void)n;
  hero *h = d;
  h->speed = 6.5f;
  h->accel = 45;
  h->air_accel = 22;
  h->jump_speed = 9.2f;
  h->double_jump_speed = 8.2f;
  h->gravity = 26;
  h->fall_gravity = 38;
  h->max_fall = 24;
  h->dash_speed = 15;
  h->dash_time = 0.2f;
  h->coyote_time = 0.1f;
  h->buffer_time = 0.12f;
  h->turn_speed = 14;
}

static const char *const sm_states[] = { "ground", "jump", "fall", "dash", "cheer" };

static void hero_ready(cw_node n, void *d) {
  hero *h = d;
  cw_node tree = cw_node_get(n, "Tree");
  if (CW_HANDLE_IS_NULL(tree)) {
    cw_error("Hero: needs an AnimationTree child named Tree");
    return;
  }
  /* ground: idle..run blended by horizontal speed */
  cw_blend_id ground = cw_animtree_blend1d(tree, "speed");
  cw_animtree_blend1d_add(tree, ground, cw_animtree_clip(tree, "idle"), 0);
  cw_animtree_blend1d_add(tree, ground, cw_animtree_clip(tree, "run"), h->speed);
  h->sm = cw_animtree_state_machine(tree);
  cw_animtree_sm_add_state(tree, h->sm, "ground", ground);
  cw_animtree_sm_add_state(tree, h->sm, "jump", cw_animtree_clip(tree, "jump"));
  cw_animtree_sm_add_state(tree, h->sm, "fall", cw_animtree_clip(tree, "fall"));
  cw_animtree_sm_add_state(tree, h->sm, "dash", cw_animtree_clip(tree, "dash"));
  cw_animtree_sm_add_state(tree, h->sm, "cheer", cw_animtree_clip(tree, "cheer"));
  struct { const char *from, *to, *cond; float fade; } tr[] = {
    { "ground", "cheer", "cheering", 0.15f }, { "jump", "cheer", "cheering", 0.15f }, { "fall", "cheer", "cheering", 0.15f },
    { "ground", "dash", "dashing", 0.04f },   { "jump", "dash", "dashing", 0.04f },   { "fall", "dash", "dashing", 0.04f },
    { "ground", "jump", "jumping", 0.06f },   { "ground", "fall", "falling", 0.12f },
    { "jump", "ground", "floor", 0.08f },     { "jump", "fall", "falling", 0.18f },
    { "fall", "ground", "floor", 0.08f },     { "fall", "jump", "jumping", 0.06f },
    { "dash", "fall", "!dashing", 0.1f },     { "cheer", "ground", "!cheering", 0.2f },
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(tr); i++)
    cw_animtree_sm_add_transition(tree, h->sm, tr[i].from, tr[i].to,
                                  &(cw_transition){ .xfade = tr[i].fade, .condition = tr[i].cond });
  cw_animtree_set_root(tree, h->sm);
  h->facing = CW_PI; /* away from a camera at yaw 0 */
  h->dash_ready = true;
}

void hero_respawn(cw_node n, t3_vec3 at) {
  hero *h = cw_node_data_t(n, hero_t);
  if (!h) return;
  cw_node3d_set_position(n, at);
  cw_node_set(n, "velocity", cw_val_vec3(0, 0, 0));
  h->run = v3(0, 0, 0);
  h->dash_t = h->coyote = h->buffer = h->air_time = 0;
  h->air_jumps = 1;
  h->dash_ready = true;
  h->facing = cw_wrap_angle(G.cam_yaw + CW_PI); /* back to the camera */
  cw_node3d_set_rotation(cw_node_get(n, "Model"), v3(0, h->facing, 0));
  cw_node3d_snapshot(n);
}

void hero_set_cheer(cw_node n, bool on) {
  hero *h = cw_node_data_t(n, hero_t);
  if (h) h->cheering = on;
}

static void squash(cw_node model, t3_vec3 from) {
  cw_tween tw = cw_tween_new(cw_node_tree(model));
  cw_tween_bind(tw, model);
  cw_tween_add_property(tw, model, "scale", cw_val_vec3(1, 1, 1), 0.22f, CW_EASE_OUT_BACK);
  cw_tween_from(tw, cw_val_vec3(from.x, from.y, from.z));
}

static void hero_fixed(cw_node n, void *d, double dtd) {
  hero *h = d;
  float dt = (float)dtd;
  bool control = G.state == GS_PLAYING;
  cw_vec2 mv = { 0, 0 };
  bool jump_pressed = false, jump_held = false, dash_pressed = false;
  if (control) {
    mv = cw_input_vector(0, G.a_left, G.a_right, G.a_forward, G.a_back, CW_DEADZONE_RADIAL, 0.2f);
    jump_pressed = cw_action_just_pressed(0, G.a_jump);
    jump_held = cw_action_pressed(0, G.a_jump);
    dash_pressed = cw_action_just_pressed(0, G.a_dash);
  }
  if (G.state == GS_DYING) return; /* frozen (hidden) until the respawn */

  /* camera-relative direction on the ground plane: stick up = away from the camera */
  float s, c;
  cw_sincos(G.cam_yaw, &s, &c);
  t3_vec3 right = v3(c, 0, -s), fwd = v3(-s, 0, -c);
  t3_vec3 want = vadd(vscale(right, mv.x), vscale(fwd, -mv.y));
  float want_len = vlen(want);
  t3_vec3 vel = get_vec3(n, "velocity");
  bool on_floor = get_bool(n, "on_floor");

  if (on_floor) {
    h->coyote = h->coyote_time;
    h->air_jumps = 1;
    h->dash_ready = true;
  } else {
    h->coyote -= dt;
  }
  h->buffer = jump_pressed ? h->buffer_time : h->buffer - dt;

  /* horizontal: steer toward the stick; a dash overrides it */
  cw_node model = cw_node_get(n, "Model");
  if (dash_pressed && h->dash_ready && h->dash_t <= 0) {
    float fs, fc;
    cw_sincos(h->facing, &fs, &fc);
    h->dash_dir = want_len > 0.2f ? vscale(want, 1 / want_len) : v3(fs, 0, fc);
    h->dash_t = h->dash_time;
    h->dash_ready = on_floor; /* one dash per air time */
    G.dashes++;
    game_sfx(SFX_DASH, NULL);
  }
  if (h->dash_t > 0) {
    h->dash_t -= dt;
    h->run = vscale(h->dash_dir, h->dash_speed);
    vel.y = 0;
    if (h->dash_t <= 0) h->run = vscale(h->dash_dir, h->speed); /* come out of it at run speed */
  } else {
    t3_vec3 target = vscale(want, h->speed);
    float k = (on_floor ? h->accel : h->air_accel) * dt;
    t3_vec3 delta = vsub(target, h->run);
    float dl = sqrtf(delta.x * delta.x + delta.z * delta.z);
    if (dl > k) delta = vscale(delta, k / dl);
    h->run.x += delta.x;
    h->run.z += delta.z;
  }

  /* jumps: buffered press + coyote time on the ground, one more in the air */
  if (h->buffer > 0 && h->coyote > 0) {
    vel.y = h->jump_speed;
    h->coyote = h->buffer = 0;
    h->jump_cut = false;
    G.jumps++;
    game_sfx(SFX_JUMP, NULL);
    squash(model, v3(0.8f, 1.25f, 0.8f));
  } else if (jump_pressed && h->coyote <= 0 && h->air_jumps > 0 && h->dash_t <= 0) {
    vel.y = h->double_jump_speed;
    h->air_jumps--;
    h->buffer = 0;
    h->jump_cut = false;
    G.double_jumps++;
    game_sfx(SFX_DOUBLE_JUMP, NULL);
    squash(model, v3(0.75f, 1.3f, 0.75f));
  }
  /* releasing jump early cuts the rise: tap = hop, hold = full jump */
  if (!jump_held && vel.y > 0 && !h->jump_cut && !on_floor) {
    vel.y *= 0.5f;
    h->jump_cut = true;
  }
  if (h->dash_t <= 0) vel.y -= (vel.y > 0 ? h->gravity : h->fall_gravity) * dt;
  if (vel.y < -h->max_fall) vel.y = -h->max_fall;
  vel.x = h->run.x;
  vel.z = h->run.z;
  cw_node_set(n, "velocity", cw_val_vec3(vel.x, vel.y, vel.z));
  float rising = vel.y;
  cw_character3d_move_and_slide(n, dt);

  /* after the move */
  vel = get_vec3(n, "velocity");
  if (rising > 3 && get_bool(n, "on_ceiling")) game_sfx(SFX_BUMP, NULL);
  on_floor = get_bool(n, "on_floor");
  if (get_bool(n, "on_wall")) {
    /* running into a wall: stop pushing into it so the run speed does not build up */
    t3_vec3 wn = get_vec3(n, "wall_normal");
    float into = h->run.x * wn.x + h->run.z * wn.z;
    if (into < 0) h->run = vsub(h->run, vscale(v3(wn.x, 0, wn.z), into));
  }
  if (on_floor && !h->was_floor && h->air_time > 0.25f) {
    game_sfx(SFX_LAND, NULL);
    squash(model, v3(1.25f, 0.75f, 1.25f));
  }
  h->air_time = on_floor ? 0 : h->air_time + dt;
  h->was_floor = on_floor;

  /* face the way we steer */
  if (want_len > 0.2f || h->dash_t > 0) {
    t3_vec3 dir = h->dash_t > 0 ? h->dash_dir : want;
    float target = cw_atan2(dir.x, dir.z);
    float diff = cw_wrap_angle(target - h->facing);
    float step = h->turn_speed * dt;
    h->facing = cw_wrap_angle(h->facing + CW_CLAMP(diff, -step, step));
  }
  cw_node3d_set_rotation(model, v3(0, h->facing, 0));

  /* animation parameters (the tree child evaluates right after this) */
  cw_node tree = cw_node_get(n, "Tree");
  float hs = sqrtf(vel.x * vel.x + vel.z * vel.z);
  if (on_floor) {
    t3_vec3 pv = get_vec3(n, "platform_velocity");
    hs = sqrtf((vel.x - pv.x) * (vel.x - pv.x) + (vel.z - pv.z) * (vel.z - pv.z));
  }
  hs = floorf(hs * 64 + 0.5f) / 64; /* the blend weight need not see the last bits of the slide */
  cw_animtree_set_param(tree, "speed", hs);
  cw_animtree_set_param(tree, "floor", on_floor ? 1 : 0);
  cw_animtree_set_param(tree, "jumping", !on_floor && vel.y > 0.5f ? 1 : 0);
  cw_animtree_set_param(tree, "falling", !on_floor && vel.y < -1.0f && h->air_time > 0.08f ? 1 : 0);
  cw_animtree_set_param(tree, "dashing", h->dash_t > 0 ? 1 : 0);
  cw_animtree_set_param(tree, "cheering", h->cheering ? 1 : 0);
  const char *st = cw_animtree_sm_current(tree, h->sm);
  for (int i = 0; i < (int)CW_ARRAY_LEN(sm_states); i++)
    if (st && !strcmp(st, sm_states[i])) G.anim_state = i;
  G.floor = on_floor;

  /* fell off the world */
  if (control && G.info && cw_node3d_position(n).y < G.info->kill_y) game_hurt();
}

static const cw_prop hero_props[] = {
  CW_PROP_FIELD_RANGE(hero, speed, CW_T_FLOAT, 0, 50, "Run speed, m/s."),
  CW_PROP_FIELD_RANGE(hero, accel, CW_T_FLOAT, 0, 500, "Ground acceleration, m/s^2."),
  CW_PROP_FIELD_RANGE(hero, air_accel, CW_T_FLOAT, 0, 500, "Air steering acceleration, m/s^2."),
  CW_PROP_FIELD_RANGE(hero, jump_speed, CW_T_FLOAT, 0, 50, "Take-off speed of a jump, m/s."),
  CW_PROP_FIELD_RANGE(hero, double_jump_speed, CW_T_FLOAT, 0, 50, "Take-off speed of the air jump, m/s."),
  CW_PROP_FIELD_RANGE(hero, gravity, CW_T_FLOAT, 0, 200, "Gravity while rising, m/s^2."),
  CW_PROP_FIELD_RANGE(hero, fall_gravity, CW_T_FLOAT, 0, 200, "Gravity while falling, m/s^2 (snappier landings)."),
  CW_PROP_FIELD_RANGE(hero, max_fall, CW_T_FLOAT, 0, 200, "Terminal fall speed, m/s."),
  CW_PROP_FIELD_RANGE(hero, dash_speed, CW_T_FLOAT, 0, 100, "Dash speed, m/s."),
  CW_PROP_FIELD_RANGE(hero, dash_time, CW_T_FLOAT, 0, 2, "Dash duration, s."),
  CW_PROP_FIELD_RANGE(hero, coyote_time, CW_T_FLOAT, 0, 1, "A jump still works this long after leaving a ledge, s."),
  CW_PROP_FIELD_RANGE(hero, buffer_time, CW_T_FLOAT, 0, 1, "A jump pressed this long before landing still happens, s."),
  CW_PROP_FIELD_RANGE(hero, turn_speed, CW_T_FLOAT, 0, 100, "Facing turn rate, rad/s."),
  CW_PROP_END,
};
static cw_node_type hero_type = { .name = "Hero", .parent = "CharacterBody3D", .data_size = sizeof(hero), .props = hero_props,
                                  .doc = "platformer3d's player: run, jump, double jump, dash (actions from input.cfg).",
                                  .init = hero_init, .ready = hero_ready, .fixed_update = hero_fixed };

/* ── FollowCamera ───────────────────────────────────────────────────── */

typedef struct follow_cam {
  const char *target;
  float distance, height, look_height, yaw, smooth, vertical_smooth;
  float orbit; /* extra yaw while the level-clear celebration plays */
  t3_vec3 pivot, pos;
  bool started;
} follow_cam;

static void cam_init(cw_node n, void *d) {
  (void)n;
  follow_cam *c = d;
  c->distance = 7.5f;
  c->height = 3.6f;
  c->look_height = 1.1f;
  c->smooth = 7;
  c->vertical_smooth = 3.5f;
}

static void cam_fixed(cw_node n, void *d, double dtd) {
  follow_cam *c = d;
  float dt = (float)dtd;
  if (!c->target || !c->target[0]) return;
  cw_node t = cw_node_get(n, c->target);
  if (CW_HANDLE_IS_NULL(t)) return;
  t3_vec3 want_pivot = vadd(cw_node3d_global_position(t), v3(0, c->look_height, 0));
  if (!c->started || G.cam_snap) c->pivot = want_pivot;
  float k = 1 - cw_exp(-c->smooth * dt), ky = 1 - cw_exp(-c->vertical_smooth * dt);
  c->pivot.x += (want_pivot.x - c->pivot.x) * k;
  c->pivot.z += (want_pivot.z - c->pivot.z) * k;
  c->pivot.y += (want_pivot.y - c->pivot.y) * ky;
  /* the celebration: swing round to the hero's front */
  if (G.state == GS_CLEAR) c->orbit += (CW_PI - c->orbit) * (1 - cw_exp(-1.5f * dt));
  float s, co;
  cw_sincos(c->yaw + c->orbit, &s, &co);
  t3_vec3 off = v3(s * c->distance, c->height, co * c->distance);
  t3_vec3 want = vadd(c->pivot, off);
  /* collision avoidance: a ray from the pivot to the wanted spot against the
   * level (world layer); a hit pulls the camera in front of the wall */
  cw_ray_hit3d hit;
  cw_phys_filter f = cw_phys_filter_layers(1u << 0);
  bool blocked = false;
  if (cw_world3d_raycast(cw_physics3d_world(cw_node_tree(n)), c->pivot, off, &f, &hit)) {
    float frac = hit.fraction - 0.35f / vlen(off);
    if (frac < 0.12f) frac = 0.12f;
    want = vadd(c->pivot, vscale(off, frac));
    blocked = true;
  }
  if (!c->started || G.cam_snap) c->pos = want;
  else if (blocked) {
    /* move in fast when a wall cuts the view, ease back out when it clears */
    float kb = 1 - cw_exp(-25 * dt);
    c->pos = vadd(c->pos, vscale(vsub(want, c->pos), kb));
  } else {
    c->pos = vadd(c->pos, vscale(vsub(want, c->pos), k));
  }
  c->started = true;
  cw_node3d_set_position(n, c->pos);
  cw_node3d_look_at(n, c->pivot, v3(0, 1, 0));
  G.cam_yaw = c->yaw;
}

static const cw_prop cam_props[] = {
  CW_PROP_FIELD(follow_cam, target, CW_T_STRING, CW_PROP_DEFAULT, "Path of the node to follow."),
  CW_PROP_FIELD_RANGE(follow_cam, distance, CW_T_FLOAT, 0, 100, "Horizontal distance behind the target, m."),
  CW_PROP_FIELD_RANGE(follow_cam, height, CW_T_FLOAT, -100, 100, "Height above the look point, m."),
  CW_PROP_FIELD_RANGE(follow_cam, look_height, CW_T_FLOAT, -10, 10, "Look point above the target's origin, m."),
  CW_PROP_FIELD_RANGE(follow_cam, yaw, CW_T_FLOAT, -6.3, 6.3, "Direction the camera looks from (0 = from +Z looking -Z)."),
  CW_PROP_FIELD_RANGE(follow_cam, smooth, CW_T_FLOAT, 0, 100, "Horizontal follow rate, 1/s."),
  CW_PROP_FIELD_RANGE(follow_cam, vertical_smooth, CW_T_FLOAT, 0, 100, "Vertical follow rate, 1/s (lower keeps jumps calm)."),
  CW_PROP_END,
};
static cw_node_type cam_type = { .name = "FollowCamera", .parent = "Camera3D", .data_size = sizeof(follow_cam),
                                 .props = cam_props, .doc = "Chase camera with ray-cast collision avoidance.",
                                 .init = cam_init, .fixed_update = cam_fixed };

/* ── Pickup / Hazard / Checkpoint / Goal ───────────────────────────── */

typedef struct pickup {
  int32_t value;
  int kind; /* 0 coin, 1 gem */
  bool taken;
} pickup;
static const char *const pickup_kinds[] = { "COIN", "GEM", NULL };
static const cw_node_type *pickup_t;
static void pickup_init(cw_node n, void *d) {
  (void)n;
  ((pickup *)d)->value = 10;
}
static const cw_prop pickup_props[] = {
  CW_PROP_FIELD_RANGE(pickup, value, CW_T_INT, 0, 100000, "Score when collected."),
  { .name = "kind", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(pickup, kind), .size = sizeof(int),
    .enum_names = pickup_kinds, .doc = "COIN or GEM (counter, sound)." },
  CW_PROP_END,
};
static cw_node_type pickup_type = { .name = "Pickup", .parent = "Area3D", .data_size = sizeof(pickup), .props = pickup_props,
                                    .doc = "A collectible; body_entered -> collect.", .init = pickup_init };

static bool is_hero(const cw_value *args, int argc) { return argc >= 1 && args[0].type == CW_T_NODE && cw_node_is(args[0].node, "Hero"); }

static void collect(cw_node n, const cw_value *args, int argc, void *user) {
  (void)user;
  pickup *p = cw_node_data_t(n, pickup_t);
  if (!p || p->taken || !is_hero(args, argc) || G.state != GS_PLAYING) return;
  p->taken = true;
  cw_node_set(n, "monitoring", cw_val_bool(false));
  G.score += p->value;
  if (p->kind == 1) G.gems++;
  else G.coins++;
  t3_vec3 at = cw_node3d_global_position(n);
  game_sfx(p->kind == 1 ? SFX_GEM : SFX_COIN, &at);
  /* pop: rise, grow, shrink away; then the node goes */
  t3_vec3 pos = cw_node3d_position(n);
  cw_tween tw = cw_tween_new(cw_node_tree(n));
  cw_tween_bind(tw, n);
  cw_tween_add_property(tw, n, "position", cw_val_vec3(pos.x, pos.y + 1.2f, pos.z), 0.35f, CW_EASE_OUT_CUBIC);
  cw_tween_parallel(tw);
  cw_tween_add_property(tw, n, "scale", cw_val_vec3(1.7f, 1.7f, 1.7f), 0.2f, CW_EASE_OUT_BACK);
  cw_tween_add_property(tw, n, "scale", cw_val_vec3(0.01f, 0.01f, 0.01f), 0.15f, CW_EASE_IN_QUAD);
  game_free_later(n, 24);
}

static cw_node_type hazard_type = { .name = "Hazard", .parent = "Area3D", .doc = "Touching it costs a life; body_entered -> hurt." };
static void hurt(cw_node n, const cw_value *args, int argc, void *user) {
  (void)n;
  (void)user;
  if (is_hero(args, argc) && G.state == GS_PLAYING) game_hurt();
}

typedef struct checkpoint {
  bool active;
} checkpoint;
static const cw_node_type *checkpoint_t;
static cw_node_type checkpoint_type = { .name = "Checkpoint", .parent = "Area3D", .data_size = sizeof(checkpoint),
                                        .doc = "Sets the respawn point; body_entered -> reach." };
static void reach(cw_node n, const cw_value *args, int argc, void *user) {
  (void)user;
  checkpoint *c = cw_node_data_t(n, checkpoint_t);
  if (!c || c->active || !is_hero(args, argc) || G.state != GS_PLAYING) return;
  c->active = true;
  G.checkpoint++;
  G.respawn = cw_node3d_global_position(n);
  G.toast_ticks = 120;
  t3_vec3 at = G.respawn;
  game_sfx(SFX_CHECKPOINT, &at);
  cw_node flag = cw_node_get(n, "Flag"), lamp = cw_node_get(n, "Lamp");
  cw_tween tw = cw_tween_new(cw_node_tree(n));
  cw_tween_bind(tw, n);
  cw_tween_add_property(tw, flag, "color", cw_val_color(0.006f, 0.7f, 0.047f, 1), 0.4f, CW_EASE_OUT_QUAD);
  cw_tween_parallel(tw);
  cw_tween_add_property(tw, flag, "emissive", cw_val_color(0.0f, 0.1f, 0.004f, 1), 0.4f, CW_EASE_OUT_QUAD);
  cw_tween_parallel(tw);
  cw_tween_add_property(tw, lamp, "emissive", cw_val_color(0.03f, 1.0f, 0.13f, 1), 0.4f, CW_EASE_OUT_QUAD);
  cw_tween_parallel(tw);
  cw_tween_add_property(tw, flag, "position", cw_val_vec3(0.45f, 2.3f, 0), 0.5f, CW_EASE_OUT_BOUNCE);
  cw_tween_from(tw, cw_val_vec3(0.45f, 0.6f, 0));
}

static cw_node_type goal_type = { .name = "Goal", .parent = "Area3D", .doc = "Reaching it clears the level; body_entered -> finish." };
static void finish(cw_node n, const cw_value *args, int argc, void *user) {
  (void)user;
  if (!is_hero(args, argc) || G.state != GS_PLAYING) return;
  G.state = GS_CLEAR;
  G.state_ticks = 0;
  t3_vec3 at = cw_node3d_global_position(n);
  game_sfx(SFX_GOAL, &at);
  hero_set_cheer(args[0].node, true);
}

cw_signal_fn game_resolve_method(cw_node target, const char *method, void **user) {
  (void)target;
  *user = NULL;
  if (!strcmp(method, "collect")) return collect;
  if (!strcmp(method, "hurt")) return hurt;
  if (!strcmp(method, "reach")) return reach;
  if (!strcmp(method, "finish")) return finish;
  cw_warn("platformer3d: no method named '%s'", method);
  return NULL;
}

/* ── LevelInfo ──────────────────────────────────────────────────────── */

static const char *const level_kinds[] = { "TITLE", "PLAY", "END", NULL };
static void info_init(cw_node n, void *d) {
  (void)n;
  level_info *l = d;
  l->kind = LEVEL_PLAY;
  l->kill_y = -12;
}
static const cw_prop info_props[] = {
  CW_PROP_FIELD(level_info, title, CW_T_STRING, CW_PROP_DEFAULT, "Level name for the banner and HUD."),
  { .name = "kind", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(level_info, kind), .size = sizeof(int),
    .enum_names = level_kinds, .doc = "TITLE, PLAY or END: what the game does in this scene." },
  CW_PROP_FIELD(level_info, next_scene, CW_T_STRING, CW_PROP_DEFAULT, "Scene loaded after the goal (or from the title)."),
  CW_PROP_FIELD(level_info, music, CW_T_STRING, CW_PROP_DEFAULT, "Streamed music asset."),
  CW_PROP_FIELD(level_info, kill_y, CW_T_FLOAT, CW_PROP_DEFAULT, "Falling below this height costs a life."),
  CW_PROP_FIELD_RANGE(level_info, index, CW_T_INT, 0, 99, "Level number shown in the HUD."),
  CW_PROP_END,
};
static cw_node_type info_type = { .name = "LevelInfo", .parent = "Node3D", .data_size = sizeof(level_info),
                                  .props = info_props, .doc = "Root of every platformer3d scene.", .init = info_init };

/* Every node type this game defines and nothing else (no scene, no state):
 * the editor calls it to open the game's scenes, cwhot after a reload. */
void cw_game_register_types(void);
void cw_game_register_types(void) { game_register_types(); }

void game_register_types(void) {
  cw_node_type_register(&hero_type);
  hero_t = &hero_type;
  cw_node_type_register(&cam_type);
  cw_node_type_register(&pickup_type);
  pickup_t = &pickup_type;
  cw_node_type_register(&hazard_type);
  cw_node_type_register(&checkpoint_type);
  checkpoint_t = &checkpoint_type;
  cw_node_type_register(&goal_type);
  cw_node_type_register(&info_type);
}

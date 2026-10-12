/* action2d's node types: what a script would hold in an engine with a
 * scripting language. Each is a C node type registered once and placed by
 * the .cwscene files (rooms instance actors/<name>.cwscene).
 *
 *   Room         the root of a room scene: title, music, index
 *   TileWalls    a StaticBody2D that builds its RECT colliders from a
 *                TileMap2D layer's solid tiles (runs merged into rectangles)
 *   Hero         the player: moves by the move actions, swings a sword (an
 *                Area2D hitbox enabled for a few ticks), knockback and
 *                invulnerability frames when hurt
 *   Enemy        the shared enemy body: hp, score, speed, hurt flash,
 *                knockback, death puff. Slime (patrols, chases when close),
 *                Bat (chases with a wobble), Spitter (stands, aims orbs),
 *                Boss (chase / wind up / radial bursts, enraged at half hp)
 *   Projectile   an Area2D orb flying straight until a wall or the hero
 *   Pickup       hearts heal, gems score
 *   Door         opens when the room's enemies are gone; its Exit area
 *                leads to the next room
 *
 * Everything runs in fixed_update from the tick input snapshot and the tree's
 * rng, so a run is a pure function of the input script. */
#include <stdlib.h>
#include <string.h>

#include "game.h"

#define DT_TICKS(t) ((t) * 60)

static const cw_node_type *T_HERO, *T_ENEMY, *T_WALLS, *T_SHOT, *T_PICKUP, *T_DOOR;

static cw_vec2 vget(cw_node n, const char *prop) {
  cw_value v;
  return cw_node_get_prop(n, prop, &v) ? cw_v2(v.v[0], v.v[1]) : cw_v2(0, 0);
}
static bool bget(cw_node n, const char *prop) {
  cw_value v;
  return cw_node_get_prop(n, prop, &v) && v.b;
}
static void set_velocity(cw_node n, cw_vec2 v) { cw_node_set(n, "velocity", cw_val_vec2(v.x, v.y)); }
static float len2(cw_vec2 a) { return a.x * a.x + a.y * a.y; }
static cw_vec2 norm(cw_vec2 a) {
  float l = sqrtf(len2(a));
  return l > 1e-6f ? cw_v2(a.x / l, a.y / l) : cw_v2(0, 0);
}
static cw_vec2 hero_pos(void) { return cw_node_valid(G.hero) ? cw_node2d_position(G.hero) : cw_v2(-9999, -9999); }

/* ── Room ──────────────────────────────────────────────────────────── */

typedef struct room {
  const char *title, *music;
  int32_t index;
} room;
static const cw_prop room_props[] = {
  CW_PROP_FIELD(room, title, CW_T_STRING, CW_PROP_DEFAULT, "Shown as a banner on entering."),
  CW_PROP_FIELD(room, music, CW_T_STRING, CW_PROP_DEFAULT, "Music track (audio/<music>.ogg)."),
  CW_PROP_FIELD(room, index, CW_T_INT, CW_PROP_DEFAULT, "1-based room number."),
  CW_PROP_END,
};
static cw_node_type room_type = { .name = "Room", .parent = "Node2D", .data_size = sizeof(room), .props = room_props,
                                  .doc = "action2d: the root of a room scene." };

/* ── TileWalls ─────────────────────────────────────────────────────── */

typedef struct walls {
  const char *tilemap, *solid;
  int32_t layer;
  /* runtime */
  bool is_solid[256];
  cw_node map;
  bool built;
} walls;

static void walls_parse(walls *w) {
  memset(w->is_solid, 0, sizeof w->is_solid);
  const char *p = w->solid ? w->solid : "";
  while (*p) {
    char *e;
    long a = strtol(p, &e, 10);
    if (e == p) { p++; continue; }
    long b = a;
    if (*e == '-') b = strtol(e + 1, &e, 10);
    for (long k = a; k <= b && k < 256; k++)
      if (k >= 0) w->is_solid[k] = true;
    p = e;
  }
}

static void walls_ready(cw_node n, void *d) {
  walls *w = d;
  walls_parse(w);
  w->map = cw_node_get(n, w->tilemap ? w->tilemap : "");
  if (!cw_node_valid(w->map)) { cw_error("TileWalls: tilemap '%s' not found (a path from the TileWalls node)", w->tilemap); return; }
  int mw, mh;
  cw_tilemap2d_size(w->map, &mw, &mh);
  cw_vec2 ts = vget(w->map, "tile_size");
  cw_vec2 off = cw_v2_sub(cw_node2d_position(w->map), cw_node2d_position(n));
  /* horizontal runs, merged downwards while the run below is identical */
  typedef struct { int x0, x1, y0, y1; bool open; } rect;
  CW_ARRAY(rect) rects = { 0 };
  for (int y = 0; y < mh; y++) {
    for (int i = 0; i < rects.count; i++) rects.data[i].open = rects.data[i].y1 == y; /* touched the last row */
    for (int x = 0; x < mw;) {
      int t = cw_tilemap2d_get_cell(w->map, w->layer, x, y);
      if (t < 0 || t > 255 || !w->is_solid[t]) { x++; continue; }
      int x0 = x;
      while (x < mw) {
        int u = cw_tilemap2d_get_cell(w->map, w->layer, x, y);
        if (u < 0 || u > 255 || !w->is_solid[u]) break;
        x++;
      }
      bool merged = false;
      for (int i = 0; i < rects.count && !merged; i++)
        if (rects.data[i].open && rects.data[i].x0 == x0 && rects.data[i].x1 == x) {
          rects.data[i].y1 = y + 1;
          merged = true;
        }
      if (!merged) cw_array_push(&rects, (rect){ x0, x, y, y + 1, false });
    }
  }
  for (int i = 0; i < rects.count; i++) {
    rect r = rects.data[i];
    cw_node s = cw_node_new("CollisionShape2D", cw_frame_printf("Wall%d", i + 1));
    cw_node_set(s, "shape", cw_val_enum("RECT"));
    cw_node_set(s, "size", cw_val_vec2((float)(r.x1 - r.x0) * ts.x, (float)(r.y1 - r.y0) * ts.y));
    cw_node2d_set_position(s, cw_v2(off.x + (float)(r.x0 + r.x1) * 0.5f * ts.x, off.y + (float)(r.y0 + r.y1) * 0.5f * ts.y));
    cw_node_add_child(n, s);
  }
  cw_array_free(&rects);
  w->built = true;
}

static const cw_prop walls_props[] = {
  CW_PROP_FIELD(walls, tilemap, CW_T_STRING, CW_PROP_DEFAULT, "Path to the TileMap2D (from this node)."),
  CW_PROP_FIELD(walls, layer, CW_T_INT, CW_PROP_DEFAULT, "The map layer whose tiles collide."),
  CW_PROP_FIELD(walls, solid, CW_T_STRING, CW_PROP_DEFAULT, "Solid tile ids: \"8-12, 24\"."),
  CW_PROP_END,
};
static cw_node_type walls_type = { .name = "TileWalls", .parent = "StaticBody2D", .data_size = sizeof(walls),
                                   .props = walls_props, .ready = walls_ready,
                                   .doc = "action2d: RECT colliders built from a TileMap2D layer's solid tiles." };

bool actors_wall_at(cw_vec2 p) {
  cw_node wn = cw_node_get(G.room_node, "Walls");
  walls *w = cw_node_data_t(wn, T_WALLS);
  if (!w || !cw_node_valid(w->map)) return false;
  int x, y;
  cw_tilemap2d_local_to_cell(w->map, cw_v2_sub(p, cw_node2d_position(w->map)), &x, &y);
  int t = cw_tilemap2d_get_cell(w->map, w->layer, x, y);
  return t >= 0 && t < 256 && w->is_solid[t];
}

/* the current room's collision grid (the bot's path finder reads it) */
bool actors_cell_solid(int x, int y) {
  cw_node wn = cw_node_get(G.room_node, "Walls");
  walls *w = cw_node_data_t(wn, T_WALLS);
  if (!w || !cw_node_valid(w->map)) return true;
  int mw, mh;
  cw_tilemap2d_size(w->map, &mw, &mh);
  if (x < 0 || y < 0 || x >= mw || y >= mh) return true;
  int t = cw_tilemap2d_get_cell(w->map, w->layer, x, y);
  return t >= 0 && t < 256 && w->is_solid[t];
}
void actors_room_size(int *w, int *h) {
  cw_node wn = cw_node_get(G.room_node, "Walls");
  walls *wl = cw_node_data_t(wn, T_WALLS);
  *w = *h = 0;
  if (wl && cw_node_valid(wl->map)) cw_tilemap2d_size(wl->map, w, h);
}

/* ── shared helpers ────────────────────────────────────────────────── */

cw_node game_spawn(const char *scene, cw_vec2 pos) {
  cw_node a = cw_scene_instance_path(scene);
  if (!cw_node_valid(a)) return a;
  cw_node2d_set_position(a, pos);
  cw_node_add_child(G.room_node, a);
  return a;
}

static void play(cw_node n, const char *child, const char *anim, bool restart) {
  cw_node s = cw_node_get(n, child);
  if (cw_node_valid(s)) cw_animated_sprite2d_play(s, anim, restart);
}

/* ── Enemy (base) ──────────────────────────────────────────────────── */

enum { EN_IDLE, EN_CHASE, EN_WINDUP, EN_BURST, EN_INTRO, EN_DYING };

typedef struct enemy {
  int32_t hp, score;
  float speed, radius;
  cw_vec2 patrol;
  int32_t phase;
  /* runtime */
  int32_t flash, knock, timer, state, cool, age, bursts, max_hp;
  cw_vec2 knock_vel;
  bool moved, dying;
  const char *anim; /* the animation to return to after a hurt flash */
} enemy;

static enemy *EN(cw_node n) { return cw_node_data_t(n, T_ENEMY); }

static void enemy_ready(cw_node n, void *d) {
  enemy *e = d;
  e->max_hp = e->hp;
  e->cool = 70 + e->phase;
  if (!e->anim) e->anim = cw_intern("move");
  (void)n;
}

static void enemy_set_anim(cw_node n, enemy *e, const char *anim) {
  e->anim = cw_intern(anim);
  if (!e->flash) play(n, "Sprite", anim, false);
}

static void enemy_fixed(cw_node n, void *d, double dt) {
  enemy *e = d;
  e->moved = false;
  if (e->dying) return;
  e->age++;
  if (e->flash > 0 && --e->flash == 0) play(n, "Sprite", e->anim, true);
  if (e->knock > 0) {
    float k = (float)e->knock / 8.0f;
    e->knock--;
    set_velocity(n, cw_v2_scale(e->knock_vel, k));
    cw_character2d_move_and_slide(n, (float)dt);
    e->moved = true;
  }
}

static bool enemy_can_act(cw_node n, enemy *e) {
  if (e->moved || e->dying) return false;
  if (G.state != ST_PLAY || !cw_node_valid(G.hero)) {
    set_velocity(n, cw_v2(0, 0));
    return false;
  }
  return true;
}

static void enemy_move(cw_node n, cw_vec2 vel, double dt) {
  set_velocity(n, vel);
  cw_character2d_move_and_slide(n, (float)dt);
  cw_node s = cw_node_get(n, "Sprite");
  if (vel.x < -1) cw_node_set(s, "flip_h", cw_val_bool(true));
  else if (vel.x > 1) cw_node_set(s, "flip_h", cw_val_bool(false));
}

static void enemy_die(cw_node n, enemy *e);

static void enemy_damage(cw_node n, int dmg, cw_vec2 from) {
  enemy *e = EN(n);
  if (!e || e->hp <= 0 || e->dying) return;
  e->hp -= dmg;
  bool boss = cw_node_is(n, "Boss");
  e->flash = boss ? 5 : 7;
  play(n, "Sprite", "hurt", true);
  e->knock = boss ? 3 : 8;
  e->knock_vel = cw_v2_scale(norm(cw_v2_sub(cw_node2d_position(n), from)), boss ? 60.0f : 160.0f);
  game_sfx(SFX_HIT);
  game_score(10);
  if (e->hp <= 0) enemy_die(n, e);
}

static void enemy_die(cw_node n, enemy *e) {
  cw_vec2 p = cw_node2d_position(n);
  if (cw_node_is(n, "Boss")) {
    e->dying = true;
    e->timer = 110;
    play(n, "Sprite", "dead", true);
    set_velocity(n, cw_v2(0, 0));
    game_sfx(SFX_BOOM);
    game_shake(true);
    game_flash("flash");
    /* every orb in the room dies with it */
    cw_node *shots;
    int ns = cw_tree_group(cw_node_tree(n), "shots", &shots);
    for (int i = 0; i < ns; i++) cw_node_free(shots[i]);
    return;
  }
  game_spawn("actors/puff.cwscene", p);
  game_score(e->score);
  G.kills++;
  game_sfx(SFX_DIE);
  cw_node_free(n);
}

static const cw_prop enemy_props[] = {
  CW_PROP_FIELD(enemy, hp, CW_T_INT, CW_PROP_DEFAULT, "Hit points (a sword hit takes 1)."),
  CW_PROP_FIELD(enemy, score, CW_T_INT, CW_PROP_DEFAULT, "Points for the kill."),
  CW_PROP_FIELD(enemy, speed, CW_T_FLOAT, CW_PROP_DEFAULT, "Move speed, px/s."),
  CW_PROP_FIELD(enemy, radius, CW_T_FLOAT, CW_PROP_DEFAULT, "Contact-damage radius, px."),
  CW_PROP_FIELD(enemy, patrol, CW_T_VEC2, CW_PROP_DEFAULT, "Slime: patrol direction (flips at walls)."),
  CW_PROP_FIELD(enemy, phase, CW_T_INT, CW_PROP_DEFAULT, "Spitter: extra ticks before the first shot."),
  CW_PROP_END,
};
static cw_node_type enemy_type = { .name = "Enemy", .parent = "CharacterBody2D", .data_size = sizeof(enemy),
                                   .props = enemy_props, .ready = enemy_ready, .fixed_update = enemy_fixed,
                                   .doc = "action2d: shared enemy body (hp, knockback, hurt flash, death)." };

/* Slime: patrols along `patrol`, turning at walls; chases when the hero is near. */
static void slime_fixed(cw_node n, void *d, double dt) {
  (void)d;
  enemy *e = EN(n);
  if (!enemy_can_act(n, e)) return;
  cw_vec2 to = cw_v2_sub(hero_pos(), cw_node2d_position(n));
  bool chase = len2(to) < 76.0f * 76.0f;
  cw_vec2 dir = chase ? norm(to) : norm(e->patrol);
  enemy_move(n, cw_v2_scale(dir, e->speed * (chase ? 1.35f : 1.0f)), dt);
  if (!chase && bget(n, "on_wall")) e->patrol = cw_v2_scale(e->patrol, -1);
}
static cw_node_type slime_type = { .name = "Slime", .parent = "Enemy", .fixed_update = slime_fixed,
                                   .doc = "action2d: patrols, chases when the hero is within 76 px." };

/* Bat: wobbles toward the hero when it is within 170 px, else hovers. */
static void bat_fixed(cw_node n, void *d, double dt) {
  (void)d;
  enemy *e = EN(n);
  if (!enemy_can_act(n, e)) return;
  cw_vec2 to = cw_v2_sub(hero_pos(), cw_node2d_position(n));
  cw_vec2 v;
  if (len2(to) < 170.0f * 170.0f) {
    cw_vec2 f = norm(to), side = cw_v2(-f.y, f.x);
    float w = cw_sin((float)e->age * 0.12f) * 0.7f;
    v = cw_v2_scale(norm(cw_v2_add(f, cw_v2_scale(side, w))), e->speed);
  } else {
    v = cw_v2(cw_cos((float)e->age * 0.05f) * 12.0f, cw_sin((float)e->age * 0.07f) * 9.0f);
  }
  enemy_move(n, v, dt);
}
static cw_node_type bat_type = { .name = "Bat", .parent = "Enemy", .fixed_update = bat_fixed,
                                 .doc = "action2d: chases with a sideways wobble." };

static void fire(cw_vec2 from, cw_vec2 dir, float speed, bool boss) {
  cw_node o = game_spawn("actors/orb.cwscene", cw_v2_add(from, cw_v2_scale(dir, 8)));
  if (!cw_node_valid(o)) return;
  cw_node_set(o, "velocity", cw_val_vec2(dir.x * speed, dir.y * speed));
  if (boss) {
    play(o, "Sprite", "boss", true);
    cw_node_set(cw_node_get(o, "Glow"), "color", cw_val_color(0.75f, 0.35f, 1, 1));
  }
}

/* Spitter: stands; when the hero is within 190 px it winds up and spits an orb at it. */
static void spitter_fixed(cw_node n, void *d, double dt) {
  (void)d, (void)dt;
  enemy *e = EN(n);
  if (!enemy_can_act(n, e)) return;
  set_velocity(n, cw_v2(0, 0));
  if (e->cool > 0) e->cool--;
  cw_vec2 p = cw_node2d_position(n), to = cw_v2_sub(hero_pos(), p);
  if (e->state == EN_IDLE && e->cool == 0 && len2(to) < 190.0f * 190.0f) {
    e->state = EN_WINDUP;
    e->timer = 28;
    enemy_set_anim(n, e, "charge");
  } else if (e->state == EN_WINDUP && --e->timer <= 0) {
    fire(cw_v2_add(p, cw_v2(0, 1)), norm(to), 92.0f, false);
    game_sfx(SFX_SHOOT);
    e->state = EN_IDLE;
    e->cool = 125;
    enemy_set_anim(n, e, "idle");
  }
}
static void spitter_ready(cw_node n, void *d) {
  (void)d;
  enemy *e = EN(n);
  e->anim = cw_intern("idle");
}
static cw_node_type spitter_type = { .name = "Spitter", .parent = "Enemy", .ready = spitter_ready,
                                     .fixed_update = spitter_fixed, .doc = "action2d: a skull totem that aims orbs." };

/* Boss: intro roar, then chase -> wind up -> radial burst, faster and denser below half hp. */
static void boss_fixed(cw_node n, void *d, double dt) {
  (void)d;
  enemy *e = EN(n);
  G.boss_hp = e->hp > 0 ? e->hp : 0;
  if (e->dying) {
    if (e->timer % 12 == 0) {
      cw_rng *r = cw_tree_rng(cw_node_tree(n));
      cw_vec2 p = cw_node2d_position(n);
      /* one draw per statement: C leaves argument order unspecified, and x86
       * and wasm clang draw in opposite orders */
      float dx = cw_rng_range(r, -14, 14);
      float dy = cw_rng_range(r, -12, 12);
      game_spawn("actors/puff.cwscene", cw_v2(p.x + dx, p.y + dy));
      game_sfx(SFX_DIE);
    }
    if (--e->timer == 0) {
      game_score(e->score);
      G.kills++;
      game_boss_defeated();
    }
    return;
  }
  if (!enemy_can_act(n, e)) return;
  cw_vec2 p = cw_node2d_position(n), to = cw_v2_sub(hero_pos(), p);
  bool enraged = e->hp * 2 <= e->max_hp;
  switch (e->state) {
  case EN_IDLE: /* waiting: wakes when the hero comes close */
    set_velocity(n, cw_v2(0, 0));
    if (len2(to) < 120.0f * 120.0f || e->age > 150) {
      e->state = EN_INTRO;
      e->timer = 50;
      enemy_set_anim(n, e, "charge");
      game_sfx(SFX_ROAR);
      game_shake(true);
    }
    break;
  case EN_INTRO:
    set_velocity(n, cw_v2(0, 0));
    if (--e->timer <= 0) { e->state = EN_CHASE; e->timer = 90; enemy_set_anim(n, e, "idle"); }
    break;
  case EN_CHASE:
    enemy_move(n, cw_v2_scale(norm(to), e->speed * (enraged ? 1.4f : 1.0f)), dt);
    if (--e->timer <= 0) { e->state = EN_WINDUP; e->timer = enraged ? 30 : 42; enemy_set_anim(n, e, "charge"); }
    break;
  case EN_WINDUP:
    set_velocity(n, cw_v2(0, 0));
    if (--e->timer <= 0) {
      int count = enraged ? 14 : 10;
      float base = (float)e->bursts * 0.31f;
      for (int i = 0; i < count; i++) {
        float s, c;
        cw_sincos(base + (float)i * CW_TAU / (float)count, &s, &c);
        fire(cw_v2_add(p, cw_v2(0, 2)), cw_v2(c, s), enraged ? 84.0f : 72.0f, true);
      }
      if (enraged) fire(p, norm(to), 110.0f, true);
      e->bursts++;
      game_sfx(SFX_SHOOT);
      game_shake(false);
      e->state = EN_CHASE;
      e->timer = enraged ? 80 : 120;
      enemy_set_anim(n, e, "idle");
    }
    break;
  default: break;
  }
}
static void boss_ready(cw_node n, void *d) {
  (void)d;
  enemy *e = EN(n);
  e->anim = cw_intern("idle");
  e->state = EN_IDLE;
}
static cw_node_type boss_type = { .name = "Boss", .parent = "Enemy", .ready = boss_ready, .fixed_update = boss_fixed,
                                  .doc = "action2d: the Warden: chase, wind up, radial orb bursts." };

int actors_enemies_alive(void) {
  if (!cw_node_valid(G.room_node)) return 0;
  cw_node *list;
  int n = cw_tree_group(cw_node_tree(G.room_node), "enemies", &list), alive = 0;
  for (int i = 0; i < n; i++) {
    enemy *e = EN(list[i]);
    if (e && e->hp > 0 && cw_node_parent(list[i]).id == G.room_node.id) alive++;
  }
  return alive;
}

uint32_t actors_hash(uint32_t h) {
  if (!cw_node_valid(G.room_node)) return h;
  cw_node *list;
  int n = cw_tree_group(cw_node_tree(G.room_node), "enemies", &list);
  for (int i = 0; i < n; i++) {
    enemy *e = EN(list[i]);
    if (!e) continue;
    cw_vec2 p = cw_node2d_position(list[i]);
    h = cw_hash32(&p, sizeof p, h);
    h = cw_hash32(&e->hp, sizeof e->hp, h);
    h = cw_hash32(&e->state, sizeof e->state, h);
  }
  cw_node *shots;
  int ns = cw_tree_group(cw_node_tree(G.room_node), "shots", &shots);
  for (int i = 0; i < ns; i++) {
    cw_vec2 p = cw_node2d_position(shots[i]);
    h = cw_hash32(&p, sizeof p, h);
  }
  return h;
}

/* ── Hero ──────────────────────────────────────────────────────────── */

enum { FACE_DOWN, FACE_UP, FACE_SIDE };

typedef struct hero {
  float speed;
  /* runtime */
  int32_t facing;
  bool left, dead;
  int32_t atk_timer, atk_cool, invuln, knock;
  cw_vec2 knock_vel;
} hero;

static void hero_hit_enemy(cw_node hitbox, const cw_value *args, int argc, void *user) {
  (void)user;
  if (argc < 1 || !EN(args[0].node)) return;
  enemy_damage(args[0].node, 1, cw_node2d_position(cw_node_parent(hitbox)));
}

static void hero_init(cw_node n, void *d) {
  (void)n;
  ((hero *)d)->speed = 86;
}

static void hero_ready(cw_node n, void *d) {
  (void)d;
  G.hero = n;
  cw_connect(cw_node_get(n, "Hitbox"), "body_entered", n, NULL, hero_hit_enemy, NULL, 0);
}

static void hero_exit(cw_node n, void *d) {
  (void)d;
  if (G.hero.id == n.id) G.hero = (cw_node){ 0 };
}

/* a hit from `from` (enemy contact, orb): false while invulnerable */
static bool hero_take_hit(cw_node n, int dmg, cw_vec2 from) {
  hero *h = cw_node_data_t(n, T_HERO);
  if (!h || h->invuln > 0 || h->dead || G.state != ST_PLAY) return false;
  h->invuln = 60;
  h->knock = 10;
  cw_vec2 away = norm(cw_v2_sub(cw_node2d_position(n), from));
  if (len2(away) == 0) away = cw_v2(0, 1);
  h->knock_vel = cw_v2_scale(away, 170);
  cw_node fx = cw_node_get(n, "Fx");
  cw_anim_player_stop(fx, false);
  cw_anim_player_play(fx, "hurt", 0);
  game_hero_hurt(dmg);
  if (G.hp <= 0) {
    h->dead = true;
    play(n, "Sprite", "dead", true);
    cw_anim_player_stop(fx, false);
    cw_node_set(cw_node_get(n, "Sprite"), "visible", cw_val_bool(true));
  }
  return true;
}

static const char *const FACE_NAME[] = { "down", "up", "side" };

static void hero_attack_end(cw_node n) {
  cw_node_set(cw_node_get(n, "Hitbox/Shape"), "disabled", cw_val_bool(true));
  cw_node_set(cw_node_get(n, "Slash"), "visible", cw_val_bool(false));
}

static void hero_fixed(cw_node n, void *d, double dt) {
  hero *h = d;
  cw_vec2 pos = cw_node2d_position(n);
  G.player[0] = pos.x;
  G.player[1] = pos.y;
  if (h->dead || G.state != ST_PLAY) {
    set_velocity(n, cw_v2(0, 0));
    if (!h->dead) play(n, "Sprite", cw_frame_printf("idle_%s", FACE_NAME[h->facing]), false);
    if (h->atk_timer) { h->atk_timer = 0; hero_attack_end(n); }
    return;
  }
  if (h->invuln > 0) h->invuln--;
  if (h->atk_cool > 0) h->atk_cool--;
  cw_vec2 mv = cw_input_vector(0, G.a_left, G.a_right, G.a_up, G.a_down, CW_DEADZONE_RADIAL, 0.3f);
  bool moving = len2(mv) > 0.01f;
  if (moving && h->atk_timer == 0) {
    if (mv.x * mv.x > mv.y * mv.y + 0.01f) { h->facing = FACE_SIDE; h->left = mv.x < 0; }
    else h->facing = mv.y < 0 ? FACE_UP : FACE_DOWN;
  }
  if (cw_action_just_pressed(0, G.a_attack) && h->atk_cool == 0 && h->knock == 0) {
    static const cw_vec2 reach[] = { { 0, 11 }, { 0, -10 }, { 12, 1 } };
    static const float rot[] = { 1.5707964f, -1.5707964f, 0 };
    cw_vec2 r = reach[h->facing];
    float a = rot[h->facing];
    if (h->facing == FACE_SIDE && h->left) { r.x = -r.x; a = 3.1415927f; }
    h->atk_timer = 9;
    h->atk_cool = 18;
    cw_node hb = cw_node_get(n, "Hitbox");
    cw_node2d_set_position(hb, r);
    cw_node_set(cw_node_get(hb, "Shape"), "disabled", cw_val_bool(false));
    cw_node slash = cw_node_get(n, "Slash");
    cw_node2d_set_position(slash, r);
    cw_node2d_set_rotation(slash, a);
    cw_node_set(slash, "visible", cw_val_bool(true));
    cw_animated_sprite2d_play(slash, "slash", true);
    G.attacks++;
    game_sfx(SFX_SWING);
  } else if (h->atk_timer > 0 && --h->atk_timer == 0) {
    hero_attack_end(n);
  }
  cw_vec2 vel;
  if (h->knock > 0) {
    vel = cw_v2_scale(h->knock_vel, (float)h->knock / 10.0f);
    h->knock--;
  } else {
    vel = cw_v2_scale(mv, h->speed * (h->atk_timer > 0 ? 0.3f : 1.0f));
  }
  set_velocity(n, vel);
  cw_character2d_move_and_slide(n, (float)dt);
  const char *f = FACE_NAME[h->facing];
  const char *anim = h->atk_timer > 0 ? cw_frame_printf("attack_%s", f)
                     : moving        ? cw_frame_printf("walk_%s", f)
                                     : cw_frame_printf("idle_%s", f);
  cw_node spr = cw_node_get(n, "Sprite");
  cw_animated_sprite2d_play(spr, anim, false);
  cw_node_set(spr, "flip_h", cw_val_bool(h->facing == FACE_SIDE && h->left));
  /* contact damage: the nearest live enemy touching us */
  pos = cw_node2d_position(n);
  G.player[0] = pos.x;
  G.player[1] = pos.y;
  if (h->invuln == 0) {
    cw_node *list;
    int cnt = cw_tree_group(cw_node_tree(n), "enemies", &list);
    for (int i = 0; i < cnt; i++) {
      enemy *e = EN(list[i]);
      if (!e || e->hp <= 0 || e->dying) continue;
      cw_vec2 ep = cw_node2d_position(list[i]);
      float r = e->radius + 5;
      if (len2(cw_v2_sub(ep, pos)) < r * r) {
        hero_take_hit(n, 1, ep);
        break;
      }
    }
  }
}

static cw_node_type hero_type = { .name = "Hero", .parent = "CharacterBody2D", .data_size = sizeof(hero),
                                  .init = hero_init, .ready = hero_ready, .exit_tree = hero_exit,
                                  .fixed_update = hero_fixed,
                                  .doc = "action2d: the player: moves, swings a sword, takes hits with i-frames." };

/* ── Projectile ────────────────────────────────────────────────────── */

typedef struct shot {
  cw_vec2 velocity;
  int32_t damage;
  float life;
} shot;
static void shot_init(cw_node n, void *d) {
  (void)n;
  shot *s = d;
  s->damage = 1;
  s->life = 4;
}
static void shot_fixed(cw_node n, void *d, double dt) {
  shot *s = d;
  if (G.state != ST_PLAY) return;
  cw_vec2 p = cw_v2_add(cw_node2d_position(n), cw_v2_scale(s->velocity, (float)dt));
  cw_node2d_set_position(n, p);
  s->life -= (float)dt;
  if (s->life <= 0 || actors_wall_at(p)) cw_node_free(n);
}
static void orb_hit(cw_node orb, const cw_value *args, int argc, void *user) {
  (void)user;
  if (argc < 1 || !cw_node_is(args[0].node, "Hero")) return;
  shot *s = cw_node_data_t(orb, T_SHOT);
  if (hero_take_hit(args[0].node, s ? s->damage : 1, cw_node2d_position(orb))) cw_node_free(orb);
}
static const cw_prop shot_props[] = {
  CW_PROP_FIELD(shot, velocity, CW_T_VEC2, CW_PROP_DEFAULT, "px/s."),
  CW_PROP_FIELD(shot, damage, CW_T_INT, CW_PROP_DEFAULT, "Half hearts taken from the hero."),
  CW_PROP_FIELD(shot, life, CW_T_FLOAT, CW_PROP_DEFAULT, "Seconds before it fizzles."),
  CW_PROP_END,
};
static cw_node_type shot_type = { .name = "Projectile", .parent = "Area2D", .data_size = sizeof(shot), .props = shot_props,
                                  .init = shot_init, .fixed_update = shot_fixed,
                                  .doc = "action2d: an orb flying straight until a wall or the hero." };

/* ── Pickup ────────────────────────────────────────────────────────── */

typedef struct pickup {
  int32_t kind, amount;
  bool taken;
} pickup;
static const char *const pickup_kinds[] = { "HEART", "GEM", NULL };
static void collect(cw_node n, const cw_value *args, int argc, void *user) {
  (void)user;
  pickup *p = cw_node_data_t(n, T_PICKUP);
  if (!p || p->taken || argc < 1 || !cw_node_is(args[0].node, "Hero") || G.state != ST_PLAY) return;
  if (p->kind == 0 && G.hp >= HERO_MAX_HP) return; /* full health: leave the heart for later */
  p->taken = true;
  G.pickups++;
  if (p->kind == 0) {
    game_hero_heal(p->amount);
    game_sfx(SFX_PICKUP);
  } else {
    game_score(p->amount);
    game_sfx(SFX_GEM);
  }
  cw_node_free(n);
}
static const cw_prop pickup_props[] = {
  { .name = "kind", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(pickup, kind), .size = 4,
    .enum_names = pickup_kinds, .doc = "HEART heals `amount` half hearts; GEM scores `amount`." },
  CW_PROP_FIELD(pickup, amount, CW_T_INT, CW_PROP_DEFAULT, "How much."),
  CW_PROP_END,
};
static cw_node_type pickup_type = { .name = "Pickup", .parent = "Area2D", .data_size = sizeof(pickup),
                                    .props = pickup_props, .doc = "action2d: a heart or a gem." };

/* ── Door ──────────────────────────────────────────────────────────── */

typedef struct door {
  int32_t next_room;
  bool open;
} door;
static void door_enter(cw_node exit, const cw_value *args, int argc, void *user) {
  (void)user;
  cw_node dn = cw_node_parent(exit);
  door *dr = cw_node_data_t(dn, T_DOOR);
  if (!dr || !dr->open || argc < 1 || !cw_node_is(args[0].node, "Hero") || G.state != ST_PLAY) return;
  game_room_exit(dr->next_room);
}
void actors_open_doors(void) {
  for (int i = 0; i < cw_node_child_count(G.room_node); i++) {
    cw_node dn = cw_node_child(G.room_node, i);
    door *dr = cw_node_data_t(dn, T_DOOR);
    if (!dr || dr->open) continue;
    dr->open = true;
    cw_node_set(cw_node_get(dn, "Sprite"), "region", (cw_value){ .type = CW_T_VEC4, .v = { 96, 32, 32, 16 } });
    cw_node_set(cw_node_get(dn, "Block/Shape"), "disabled", cw_val_bool(true));
    cw_node_set(cw_node_get(dn, "Glow"), "enabled", cw_val_bool(true));
    G.doors++;
    game_sfx(SFX_DOOR);
    game_shake(false);
  }
}
bool actors_door(cw_vec2 *pos) {
  for (int i = 0; i < cw_node_child_count(G.room_node); i++) {
    cw_node dn = cw_node_child(G.room_node, i);
    door *dr = cw_node_data_t(dn, T_DOOR);
    if (!dr) continue;
    *pos = cw_node2d_position(dn);
    return dr->open;
  }
  return false;
}
int actors_hero_cooldown(void) {
  hero *h = cw_node_data_t(G.hero, T_HERO);
  return h ? h->atk_cool + h->atk_timer : 0;
}
bool actors_enemy_alive(cw_node n, float *radius) {
  enemy *e = EN(n);
  if (radius) *radius = e ? e->radius : 0;
  return e && e->hp > 0 && !e->dying;
}
int actors_enemy_state(cw_node n, int *timer) {
  enemy *e = EN(n);
  if (timer) *timer = e ? e->timer : 0;
  return e ? e->state : -1;
}

static const cw_prop door_props[] = {
  CW_PROP_FIELD(door, next_room, CW_T_INT, CW_PROP_DEFAULT, "Room the exit leads to."),
  CW_PROP_FIELD(door, open, CW_T_BOOL, CW_PROP_DEFAULT, "Open: the exit works."),
  CW_PROP_END,
};
static cw_node_type door_type = { .name = "Door", .parent = "Node2D", .data_size = sizeof(door), .props = door_props,
                                  .doc = "action2d: opens when the room is clear." };

/* ── registration and scene-file methods ───────────────────────────── */

static void free_self(cw_node n, const cw_value *a, int c, void *u) {
  (void)a, (void)c, (void)u;
  cw_node_free(n);
}

cw_signal_fn actors_resolve(const char *method) {
  if (!strcmp(method, "collect")) return collect;
  if (!strcmp(method, "orb_hit")) return orb_hit;
  if (!strcmp(method, "door_enter")) return door_enter;
  if (!strcmp(method, "free_self")) return free_self;
  return NULL;
}

/* Every node type this game defines and nothing else (no scene, no state):
 * the editor calls it to open the game's scenes, cwhot after a reload. */
void cw_game_register_types(void);
void cw_game_register_types(void) { actors_register(); }

void actors_register(void) {
  cw_node_type_register(&room_type);
  cw_node_type_register(&walls_type);
  cw_node_type_register(&enemy_type);
  cw_node_type_register(&slime_type);
  cw_node_type_register(&bat_type);
  cw_node_type_register(&spitter_type);
  cw_node_type_register(&boss_type);
  cw_node_type_register(&hero_type);
  cw_node_type_register(&shot_type);
  cw_node_type_register(&pickup_type);
  cw_node_type_register(&door_type);
  T_HERO = &hero_type;
  T_ENEMY = &enemy_type;
  T_WALLS = &walls_type;
  T_SHOT = &shot_type;
  T_PICKUP = &pickup_type;
  T_DOOR = &door_type;
}

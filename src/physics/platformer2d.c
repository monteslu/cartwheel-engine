/* Platformer controller on cw_mover2d (cartwheel/platformer2d.h). Simulation code: + - * / sqrt only. */
#include <math.h>
#include <string.h>

#include "cartwheel/platformer2d.h"

cw_platformer2d_def cw_platformer2d_default_def(void) {
  return (cw_platformer2d_def){
    .mover = { 0 }, .run_speed = 8, .ground_accel = 70, .ground_brake = 90, .air_accel = 45, .air_brake = 25,
    .gravity = 38, .fall_gravity = 1.4f, .jump_cut_gravity = 3, .apex_gravity = 0.55f, .apex_speed = 1.5f,
    .max_fall = 22, .jump_height = 3.2f, .coyote_time = 0.1f, .jump_buffer = 0.12f, .air_jumps = 0,
    .air_jump_scale = 0.9f, .wall_jump = false, .wall_slide_speed = 3, .wall_jump_speed_x = 8,
    .wall_jump_speed_y = 14.5f, .wall_jump_lock = 0.16f, .dash_speed = 0, .dash_time = 0.16f,
    .dash_cooldown = 0.35f, .swim_speed = 4.5f, .swim_accel = 18, .swim_stroke = 7, .water_gravity = 0.18f,
  };
}

void cw_platformer2d_init(cw_platformer2d *p, cw_world2d w, const cw_platformer2d_def *def, cw_pos2 feet) {
  memset(p, 0, sizeof *p);
  p->def = def ? *def : cw_platformer2d_default_def();
  if (!def) p->def.mover = cw_mover2d_default_def();
  cw_mover2d_init(&p->body, w, &p->def.mover, feet);
  p->facing = 1;
  p->max_air_jumps = p->def.air_jumps;
  p->can_dash = p->def.dash_speed > 0;
  p->can_wall_jump = p->def.wall_jump;
}

void cw_platformer2d_teleport(cw_platformer2d *p, cw_pos2 feet) {
  cw_mover2d_teleport(&p->body, feet);
  p->body.velocity = (cw_vec2){ 0, 0 };
  p->coyote = p->buffer = p->wall_coyote = p->wall_lock = p->dash_t = p->dash_cd = p->drop_t = 0;
  p->wall_side = 0;
  p->submerged = 0;
  p->current = (cw_vec2){ 0, 0 };
}

static float approach(float v, float target, float step) {
  if (v < target) return v + step < target ? v + step : target;
  return v - step > target ? v - step : target;
}
static float fabs_(float x) { return x < 0 ? -x : x; }
static float dec(float t, float dt) { return t > dt ? t - dt : 0; }

void cw_platformer2d_step(cw_platformer2d *p, const cw_platformer2d_input *in, float dt) {
  const cw_platformer2d_def *d = &p->def;
  cw_mover2d *b = &p->body;
  cw_vec2 v = b->velocity;
  uint32_t ev = 0;
  bool was_floor = b->on_floor;
  bool wet = p->submerged > 0.5f;

  /* timers */
  if (was_floor) {
    p->coyote = d->coyote_time;
    p->air_jumps_left = p->max_air_jumps;
    p->dash_ready = true;
  } else p->coyote = dec(p->coyote, dt);
  p->buffer = in->jump_pressed ? d->jump_buffer : dec(p->buffer, dt);
  p->dash_cd = dec(p->dash_cd, dt);
  p->wall_lock = dec(p->wall_lock, dt);
  p->drop_t = dec(p->drop_t, dt);
  if (b->on_wall && !was_floor) {
    p->wall_side = b->wall_normal.x > 0 ? -1 : 1;
    p->wall_coyote = d->coyote_time;
  } else p->wall_coyote = dec(p->wall_coyote, dt);

  if (wet && !p->was_wet) ev |= CW_PF_ENTER_WATER;
  if (!wet && p->was_wet) ev |= CW_PF_LEAVE_WATER;
  float impact = was_floor ? 0 : -v.y;

  /* drop through a one-way platform */
  b->drop_through = p->drop_t > 0;
  if (in->jump_pressed && in->down_held && was_floor && b->on_one_way) {
    p->drop_t = 0.22f;
    b->drop_through = true;
    p->buffer = 0;
    p->coyote = 0;
    v.y = -1;
    ev |= CW_PF_DROP;
  }

  /* dash */
  if (in->dash_pressed && p->can_dash && d->dash_speed > 0 && p->dash_cd <= 0 && p->dash_ready && p->dash_t <= 0) {
    p->dash_t = d->dash_time;
    p->dash_cd = d->dash_cooldown;
    p->dash_ready = false;
    int dir = in->move_x > 0.3f ? 1 : in->move_x < -0.3f ? -1 : p->facing;
    p->facing = dir;
    ev |= CW_PF_DASH;
  }

  if (p->dash_t > 0) {
    p->dash_t = dec(p->dash_t, dt);
    v.x = (float)p->facing * d->dash_speed;
    v.y = 0;
  } else if (wet) {
    float f = p->submerged;
    float tx = in->move_x * d->swim_speed + p->current.x;
    float ty = in->move_y * d->swim_speed + p->current.y;
    v.x = approach(v.x, tx, d->swim_accel * dt);
    /* sink slowly when the stick is idle: gravity scaled by depth */
    ty += -d->water_gravity * 8.0f * (in->move_y == 0 ? 1 : 0) * f;
    if (v.y > ty) v.y = approach(v.y, ty, d->swim_accel * 1.6f * dt);
    else v.y = approach(v.y, ty, d->swim_accel * dt);
    if (in->jump_pressed) {
      v.y = d->swim_stroke;
      ev |= CW_PF_STROKE;
      p->buffer = 0;
    }
    if (was_floor && in->move_y <= 0) v.y = 0;
  } else {
    /* run */
    float target = in->move_x * d->run_speed;
    bool want = fabs_(in->move_x) > 0.05f;
    bool turning = want && v.x * target < 0;
    float rate;
    if (was_floor) rate = (want && !turning) ? d->ground_accel : d->ground_brake;
    else rate = want ? d->air_accel : d->air_brake;
    if (turning) rate = was_floor ? d->ground_brake : d->air_accel * 1.5f;
    if (p->wall_lock > 0) rate *= 0.15f;
    v.x = approach(v.x, target, rate * dt);
    if (want) p->facing = in->move_x > 0 ? 1 : -1;
    /* gravity */
    float g = d->gravity;
    if (v.y < 0) g *= d->fall_gravity;
    else if (!in->jump_held) g *= d->jump_cut_gravity;
    else if (v.y < d->apex_speed) g *= d->apex_gravity;
    v.y -= g * dt;
    if (v.y < -d->max_fall) v.y = -d->max_fall;
    /* wall slide */
    if (p->can_wall_jump && !was_floor && b->on_wall && v.y < 0 &&
        ((p->wall_side > 0 && in->move_x > 0.2f) || (p->wall_side < 0 && in->move_x < -0.2f)) && v.y < -d->wall_slide_speed)
      v.y = -d->wall_slide_speed;
    /* jumps */
    if (p->buffer > 0) {
      float jv = sqrtf(2 * d->gravity * d->jump_height);
      if (p->coyote > 0) {
        v.y = jv;
        p->coyote = p->buffer = 0;
        ev |= CW_PF_JUMP;
      } else if (p->can_wall_jump && p->wall_coyote > 0 && p->wall_side != 0) {
        v.y = d->wall_jump_speed_y;
        v.x = (float)-p->wall_side * d->wall_jump_speed_x;
        p->facing = -p->wall_side;
        p->wall_lock = d->wall_jump_lock;
        p->wall_coyote = p->coyote = p->buffer = 0;
        p->wall_side = 0;
        ev |= CW_PF_WALL_JUMP;
      } else if (p->air_jumps_left > 0) {
        v.y = jv * d->air_jump_scale;
        p->air_jumps_left--;
        p->buffer = 0;
        ev |= CW_PF_AIR_JUMP;
      }
    }
  }
  if (!wet && p->dash_t <= 0 && was_floor && !(ev & CW_PF_JUMP)) p->dash_ready = true;

  b->velocity = v;
  cw_mover2d_move(b, dt);
  if (b->on_floor && !was_floor && !wet) {
    ev |= CW_PF_LAND;
    p->land_speed = impact > 0 ? impact : 0;
  }
  if (b->on_ceiling && b->velocity.y <= 0 && v.y > 1) ev |= CW_PF_BONK;
  p->was_wet = wet;
  p->events = ev;
}

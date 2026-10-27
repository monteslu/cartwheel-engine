#include "cartwheel/aim.h"

cw_aim_curve cw_aim_curve_default(void) {
  return (cw_aim_curve){ .deadzone = 0.15f, .power = 1.8f, .yaw_rate = 3.4f, .pitch_rate = 2.4f, .ramp_seconds = 0.35f,
                         .ramp_gain = 0.6f, .ramp_edge = 0.9f, .assist = 0 };
}

cw_vec2 cw_aim_step(cw_aim_state *s, const cw_aim_curve *c, cw_vec2 stick, float dt, float on_target) {
  float len = cw_v2_length(stick);
  if (len > 1) stick = (cw_vec2){ stick.x / len, stick.y / len }, len = 1;
  if (len <= c->deadzone) {
    s->held = 0;
    return (cw_vec2){ 0, 0 };
  }
  float r = (len - c->deadzone) / (1 - c->deadzone); /* 0..1 past the dead zone */
  float k = c->power == 1 ? r : cw_pow(r, c->power);
  /* the boost this tick is what the stick has held so far; it builds while the stick stays at its edge and drains when it leaves */
  float boost = 1;
  if (c->ramp_seconds > 0) boost += c->ramp_gain * cw_clampf(s->held / c->ramp_seconds, 0, 1);
  if (c->ramp_seconds > 0 && len >= c->ramp_edge) s->held += dt;
  else s->held -= dt * 2;
  if (s->held < 0) s->held = 0;
  float slow = 1 - cw_clampf(c->assist, 0, 1) * cw_clampf(on_target, 0, 1);
  float m = k * boost * slow * dt / len; /* the direction is stick / len */
  return (cw_vec2){ stick.x * m * c->yaw_rate, stick.y * m * c->pitch_rate };
}

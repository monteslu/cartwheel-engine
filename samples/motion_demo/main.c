/* motion_demo: the command buffer and the framing camera (cartwheel/motion.h,
 * cartwheel/framing.h) with no art.
 *
 * Player 0's stick and buttons (arrows or WASD, J) feed a command buffer each
 * tick. The demo reads three motions (quarter circle + J, rising strike +
 * J, and a dash) and counts how many times each fired; the last frames of
 * input show as numpad digits. Two dots on a line stand for two fighters:
 * the framing fit is computed for them (distance and look-at x), with and
 * without the distance limit. All of it is published as debug fields. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/framing.h"
#include "cartwheel/gl.h"
#include "cartwheel/input.h"
#include "cartwheel/motion.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"

static cw_action left, right, up, down, fire;
static cw_cmd_buf buf;
static cw_motion qcf, dp, dash;
static int32_t fireballs, uppercuts, dashes, ticks;
static float fit_dist, fit_x, apart = 4;
static char hist[64];

static void init(void *u) {
  (void)u;
  left = cw_action_add("left", 0.5f); right = cw_action_add("right", 0.5f);
  up = cw_action_add("up", 0.5f); down = cw_action_add("down", 0.5f);
  fire = cw_action_add("fire", 0.5f);
  cw_action_bind_text(left, "key:left, key:a, pad:left, axis:lx-");
  cw_action_bind_text(right, "key:right, key:d, pad:right, axis:lx+");
  cw_action_bind_text(up, "key:up, key:w, pad:up, axis:ly-");
  cw_action_bind_text(down, "key:down, key:s, pad:down, axis:ly+");
  cw_action_bind_text(fire, "key:j, pad:west");
  cw_motion_parse("236", &qcf);
  cw_motion_parse("623", &dp);
  cw_motion_parse("!6!6", &dash);
  cw_debug_i32("demo.fireballs", &fireballs); cw_debug_i32("demo.uppercuts", &uppercuts); cw_debug_i32("demo.dashes", &dashes);
  cw_debug_i32("demo.ticks", &ticks); cw_debug_f32("demo.fit_dist", &fit_dist); cw_debug_f32("demo.fit_x", &fit_x);
  cw_cmd_clear(&buf);
}

static void fixed(void *u, double dt) {
  (void)u; (void)dt;
  ticks++;
  cw_cmd_push(&buf, cw_action_pressed(0, left), cw_action_pressed(0, right), cw_action_pressed(0, up), cw_action_pressed(0, down), 1,
              cw_action_pressed(0, fire) ? 1 : 0);
  if (cw_special_match(&buf, &qcf, 1, 16, 5) >= 0) { fireballs++; cw_cmd_consume(&buf); }
  else if (cw_special_match(&buf, &dp, 1, 16, 5) >= 0) { uppercuts++; cw_cmd_consume(&buf); }
  else if (cw_motion_match(&buf, &dash, 12) == 0) { dashes++; cw_cmd_consume(&buf); }
  /* two points that drift apart and together; the fit for them */
  apart = 3.0f + 3.0f * (1.0f + cw_sin((float)ticks * 0.02f));
  cw_frame_params p = { .yaw = 0, .pitch = -0.1f, .fov_y = 0.6f, .aspect = 16.0f / 9, .margin = 0.12f, .min_dist = 5, .max_dist = 14 };
  cw_frame_point pts[2] = { { -apart * 0.5f, 1, 0, 0.5f }, { apart * 0.5f, 1, 0, 0.5f } };
  cw_frame_result r;
  if (cw_frame_fit(&p, pts, 2, &r)) { fit_dist = r.dist; fit_x = r.target[0]; }
  /* the history line: the newest first */
  int n = 0;
  for (int a = 0; a < 24 && n < 60; a++) {
    int idx = (int)buf.count - 1 - a;
    if (idx < 0) break;
    n += snprintf(hist + n, sizeof hist - (size_t)n, "%d", buf.f[idx & (CW_CMD_HISTORY - 1)].dir);
  }
}

static void render(void *u, double alpha) {
  (void)u; (void)alpha;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_rgba(0.08f, 0.09f, 0.12f, 1) });
  char b[96];
  cw_text_style st = { .size = 18, .color = cw_rgba(1, 1, 1, 1) };
  cw_font f = cw_ui_font_sans();
  snprintf(b, sizeof b, "history (newest first): %s", hist);
  cw_r2d_text(f, b, (cw_vec2){ 20, 20 }, &st);
  snprintf(b, sizeof b, "fireballs %d   uppercuts %d   dashes %d", fireballs, uppercuts, dashes);
  cw_r2d_text(f, b, (cw_vec2){ 20, 50 }, &st);
  snprintf(b, sizeof b, "framing: two points %.1f m apart need distance %.2f", apart, fit_dist);
  cw_r2d_text(f, b, (cw_vec2){ 20, 80 }, &st);
  cw_r2d_circle((cw_vec2){ 320 - apart * 20, 220 }, 12, 16, cw_rgba(1, 0.5f, 0.3f, 1));
  cw_r2d_circle((cw_vec2){ 320 + apart * 20, 220 }, 12, 16, cw_rgba(0.3f, 0.7f, 1, 1));
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc; (void)argv;
  return (cw_app_desc){ .title = "motion_demo", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed, .render = render };
}

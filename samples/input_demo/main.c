/* input_demo: the input module end to end, at a 120 Hz fixed rate so every
 * 60 fps frame runs two ticks.
 *
 *   - actions from assets/bindings.txt (defaults), then a "user override"
 *     applied the way a game applies its saved settings;
 *   - a simulation (fixed_update, tick snapshot) counting presses, releases
 *     and held ticks and integrating a movement vector;
 *   - UI counters (update, frame snapshot) for the same actions, and the
 *     three deadzone modes of the same vector;
 *   - rebinding: pressing "rebind" captures the next input into "special";
 *   - touch: an on-screen stick (touch:0) and button (touch:1);
 *   - record/replay: the whole run is recorded from the first tick; pressing
 *     "verify" re-simulates it from the recording alone inside one frame and
 *     publishes whether the replayed state equals the live one.
 * Everything a test needs is a named debug field. */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"

#define W 640
#define H 360

typedef struct {
  int32_t jump_jp, jump_jr, jump_ticks;
  int32_t fire_jp, fire_ticks;
  int32_t special_jp;
  int32_t zoom;
  int32_t verify;
  uint32_t ticks;
  float pos[2];
  float axis_sum;
} sim_state;

typedef struct {
  cw_action jump, fire, left, right, up, down, zoom_in, zoom_out, verify, rebind, special;
  sim_state sim, replayed;
  /* frame (UI) side */
  int32_t ui_jump_jp, ui_jump_jr, ui_jump_frames, ui_fire_jp, ui_touch_button;
  float vec_axial[2], vec_radial[2], vec_scaled[2], stick[2];
  /* bindings */
  int32_t bind_changes;
  uint32_t saved_hash, special_binding;
  /* record / replay */
  int32_t replay_done, replay_match, hash_match;
  uint32_t replay_ticks, record_ticks, record_bytes, record_hash, replay_hash;
} demo;

static demo app;

/* #region persist */
/* The game owns persistence: here it hashes what it would save. */
static void bindings_changed(void *user) {
  demo *d = user;
  char text[2048];
  size_t n = cw_input_save_bindings(text, sizeof text, true);
  d->bind_changes++;
  d->saved_hash = cw_hash32(text, n < sizeof text ? n : sizeof text - 1, CW_HASH32_INIT);
  cw_binding b[1];
  if (cw_action_bindings(d->special, b, 1) > 0) d->special_binding = (uint32_t)b[0].kind << 16 | b[0].code;
}
/* #endregion */

/* #region sim_step */
static void sim_step(demo *d, sim_state *s) {
  const float dt = 1.0f / 120.0f;
  if (cw_action_just_pressed(0, d->jump)) s->jump_jp++;
  if (cw_action_just_released(0, d->jump)) s->jump_jr++;
  if (cw_action_pressed(0, d->jump)) s->jump_ticks++;
  if (cw_action_just_pressed(0, d->fire)) s->fire_jp++;
  if (cw_action_pressed(0, d->fire)) s->fire_ticks++;
  if (cw_action_just_pressed(0, d->special)) s->special_jp++;
  if (cw_action_just_pressed(0, d->zoom_in)) s->zoom++;
  if (cw_action_just_pressed(0, d->zoom_out)) s->zoom--;
  if (cw_action_just_pressed(0, d->verify)) s->verify = 1;
  cw_vec2 v = cw_input_vector(0, d->left, d->right, d->up, d->down, CW_DEADZONE_SCALED_RADIAL, 0.2f);
  s->pos[0] += v.x * 100.0f * dt;
  s->pos[1] += v.y * 100.0f * dt;
  s->axis_sum += cw_input_axis(0, d->left, d->right);
  s->ticks++;
}
/* #endregion */

static void init(void *user) {
  demo *d = user;
  /* #region bindings */
  /* touch controls first: their ids are what the bindings name */
  cw_touch_add_stick(0, 200, 200, 160, 50, false); /* touch:0 */
  cw_touch_add_button(520, 260, 100, 80);          /* touch:1 */
  if (!cw_input_load_bindings_asset("bindings.txt")) cw_error("input_demo: default bindings did not load");
  cw_input_commit_defaults();
  /* the user's saved overrides (a game reads these from its own save data) */
  const char *user_overrides = "fire = axis:rt+@0.5, pad:west, key:j, key:k, mouse:right\n";
  cw_input_load_bindings(user_overrides, strlen(user_overrides));
  cw_input_on_bindings_changed(bindings_changed, d);
  /* #endregion */
  d->jump = cw_action_find("jump");
  d->fire = cw_action_find("fire");
  d->left = cw_action_find("left");
  d->right = cw_action_find("right");
  d->up = cw_action_find("up");
  d->down = cw_action_find("down");
  d->zoom_in = cw_action_find("zoom_in");
  d->zoom_out = cw_action_find("zoom_out");
  d->verify = cw_action_find("verify");
  d->rebind = cw_action_find("rebind");
  d->special = cw_action_find("special");
  cw_input_record_begin(); /* from the first tick */

  cw_debug_i32("sim.jump_jp", &d->sim.jump_jp);
  cw_debug_i32("sim.jump_jr", &d->sim.jump_jr);
  cw_debug_i32("sim.jump_ticks", &d->sim.jump_ticks);
  cw_debug_i32("sim.fire_jp", &d->sim.fire_jp);
  cw_debug_i32("sim.fire_ticks", &d->sim.fire_ticks);
  cw_debug_i32("sim.special_jp", &d->sim.special_jp);
  cw_debug_i32("sim.zoom", &d->sim.zoom);
  cw_debug_i32("sim.verify", &d->sim.verify);
  cw_debug_u32("sim.ticks", &d->sim.ticks);
  cw_debug_f32_array("sim.pos", d->sim.pos, 2);
  cw_debug_f32("sim.axis_sum", &d->sim.axis_sum);
  cw_debug_i32("ui.jump_jp", &d->ui_jump_jp);
  cw_debug_i32("ui.jump_jr", &d->ui_jump_jr);
  cw_debug_i32("ui.jump_frames", &d->ui_jump_frames);
  cw_debug_i32("ui.fire_jp", &d->ui_fire_jp);
  cw_debug_i32("ui.touch_button", &d->ui_touch_button);
  cw_debug_f32_array("ui.vec_axial", d->vec_axial, 2);
  cw_debug_f32_array("ui.vec_radial", d->vec_radial, 2);
  cw_debug_f32_array("ui.vec_scaled", d->vec_scaled, 2);
  cw_debug_f32_array("ui.stick", d->stick, 2);
  cw_debug_i32("bind.changes", &d->bind_changes);
  cw_debug_u32("bind.saved_hash", &d->saved_hash);
  cw_debug_u32("bind.special", &d->special_binding);
  cw_debug_i32("replay.done", &d->replay_done);
  cw_debug_i32("replay.match", &d->replay_match);
  cw_debug_i32("replay.hash_match", &d->hash_match);
  cw_debug_u32("replay.ticks", &d->replay_ticks);
  cw_debug_u32("replay.hash", &d->replay_hash);
  cw_debug_u32("record.ticks", &d->record_ticks);
  cw_debug_u32("record.bytes", &d->record_bytes);
  cw_debug_u32("record.hash", &d->record_hash);
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  demo *d = user;
  sim_step(d, &d->sim);
}

/* #region replay */
/* Re-simulate the recorded run from its recording alone and compare. */
static void verify_replay(demo *d) {
  cw_input_record_end();
  size_t len;
  const uint8_t *rec = cw_input_recording(&len);
  d->record_ticks = cw_input_record_ticks();
  d->record_bytes = (uint32_t)len;
  d->record_hash = cw_input_record_hash();
  memset(&d->replayed, 0, sizeof d->replayed);
  if (cw_input_replay_begin(rec, len))
    while (cw_input_replay_step()) sim_step(d, &d->replayed);
  cw_input_set_context(CW_INPUT_FRAME);
  d->replay_ticks = cw_input_replay_ticks();
  d->replay_hash = cw_input_replay_hash();
  d->hash_match = d->replay_hash == d->record_hash;
  d->replay_match = memcmp(&d->replayed, &d->sim, sizeof d->sim) == 0;
  d->replay_done = 1;
  cw_info("replay: %u ticks from %u bytes, state %s", d->replay_ticks, d->record_bytes,
          d->replay_match ? "identical" : "DIFFERENT");
}
/* #endregion */

static void update(void *user, double dt) {
  (void)dt;
  demo *d = user;
  if (cw_action_just_pressed(0, d->jump)) d->ui_jump_jp++;
  if (cw_action_just_released(0, d->jump)) d->ui_jump_jr++;
  if (cw_action_pressed(0, d->jump)) d->ui_jump_frames++;
  if (cw_action_just_pressed(0, d->fire)) d->ui_fire_jp++;
  if (cw_touch_control_active(1)) d->ui_touch_button++;
  cw_vec2 a = cw_input_vector(0, d->left, d->right, d->up, d->down, CW_DEADZONE_AXIAL, 0.2f);
  cw_vec2 r = cw_input_vector(0, d->left, d->right, d->up, d->down, CW_DEADZONE_RADIAL, 0.2f);
  cw_vec2 s = cw_input_vector(0, d->left, d->right, d->up, d->down, CW_DEADZONE_SCALED_RADIAL, 0.2f);
  cw_vec2 st = cw_touch_stick_value(0);
  memcpy(d->vec_axial, &a, sizeof a);
  memcpy(d->vec_radial, &r, sizeof r);
  memcpy(d->vec_scaled, &s, sizeof s);
  memcpy(d->stick, &st, sizeof st);
  /* #region rebind */
  /* rebinding: the next thing pressed becomes "special" */
  cw_binding b;
  if (cw_input_capture_poll(&b)) cw_action_set_binding(d->special, 0, b);
  if (cw_action_just_pressed(0, d->rebind))
    cw_input_capture_begin(0, CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE, true);
  /* #endregion */
  if (d->sim.verify && !d->replay_done) verify_replay(d);
}

static void rect(int x, int y, int w, int h, float r, float g, float b) {
  glScissor(x, H - y - h, w, h); /* top-left origin */
  glClearColor(r, g, b, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void render(void *user, double alpha) {
  (void)alpha;
  demo *d = user;
  glViewport(0, 0, W, H);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.10f, 0.12f, 0.16f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  /* touch controls */
  rect(0, 200, 200, 160, 0.16f, 0.18f, 0.24f);
  rect(520, 260, 100, 80, cw_touch_control_active(1) ? 0.9f : 0.25f, 0.3f, 0.3f);
  cw_vec2 c = cw_touch_stick_center(0);
  rect((int)(c.x + d->stick[0] * 50) - 8, (int)(c.y + d->stick[1] * 50) - 8, 16, 16, 0.8f, 0.8f, 0.8f);
  /* the simulated player */
  int px = W / 2 + (int)d->sim.pos[0], py = H / 2 + (int)d->sim.pos[1];
  rect(px - 12, py - 12, 24, 24, 0.2f, 0.6f, 1.0f);
  /* held indicators (frame context) */
  rect(10, 10, 30, 30, cw_action_pressed(0, d->jump) ? 0.2f : 0.1f, cw_action_pressed(0, d->jump) ? 0.9f : 0.2f, 0.2f);
  rect(50, 10, 30, 30, cw_action_pressed(0, d->fire) ? 0.9f : 0.2f, 0.2f, 0.2f);
  /* counters as bars */
  rect(10, 50, 6 * d->sim.jump_jp, 6, 0.3f, 0.9f, 0.3f);
  rect(10, 60, 2 * d->sim.jump_ticks, 6, 0.3f, 0.7f, 0.3f);
  rect(10, 70, 6 * d->sim.fire_jp, 6, 0.9f, 0.3f, 0.3f);
  rect(10, 80, 6 * (d->sim.zoom + 10), 6, 0.6f, 0.6f, 0.9f);
  rect(10, 90, 6 * d->sim.special_jp, 6, 0.9f, 0.9f, 0.3f);
  if (d->replay_done) rect(W - 40, 10, 30, 30, d->replay_match ? 0.2f : 1.0f, d->replay_match ? 1.0f : 0.1f, 0.2f);
  glDisable(GL_SCISSOR_TEST);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){
    .title = "cartwheel input_demo",
    .width = W,
    .height = H,
    .fixed_hz = 120,
    .init = init,
    .fixed_update = fixed_update,
    .update = update,
    .render = render,
    .debug = true,
    .user = &app,
  };
}

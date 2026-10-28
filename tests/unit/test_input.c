/* The input module driven directly (cw_input_frame_begin / tick_begin /
 * ticks_end) with synthetic raw input: the tick-vs-frame edge cases a host
 * cannot script (a press inside one frame's event batch, frames with no
 * tick), deadzones, vectors, text bindings, capture, touch and replay. */
#include <string.h>

#include "cartwheel/aim.h"
#include "cartwheel/input.h"
#include "cartwheel/math.h"
#include "test.h"

static cw_raw_input raw;

/* one frame with n ticks; records what the action looked like on each tick */
typedef struct { int pressed, jp, jr; } seen;
static seen run_frame(cw_action a, int ticks) {
  seen s = { 0 };
  cw_input_frame_begin(&raw);
  for (int t = 0; t < ticks; t++) {
    cw_input_tick_begin();
    s.pressed += cw_action_pressed(0, a);
    s.jp += cw_action_just_pressed(0, a);
    s.jr += cw_action_just_released(0, a);
  }
  cw_input_ticks_end();
  memset(raw.keys_pressed, 0, sizeof raw.keys_pressed);
  raw.mouse_pressed = 0;
  for (int i = 0; i < CW_MAX_PADS; i++) raw.pads[i].pressed = 0;
  for (int i = 0; i < CW_MAX_TOUCHES; i++) raw.touches[i].tapped = false;
  raw.wheel_x = raw.wheel_y = 0;
  return s;
}

static void fresh(void) {
  cw_input_reset();
  memset(&raw, 0, sizeof raw);
  raw.pads[0].connected = raw.pads[1].connected = true;
}

static void test_tick_latch(void) {
  fresh();
  cw_action jump = cw_action_add("jump", 0.5f);
  CHECK(cw_action_bind_text(jump, "key:space, pad:south"));
  { /* a dead zone item in a text binding sets the action's dead zone (a touch stick) */
    cw_action dzt = cw_action_add("dz_text", 0.5f);
    CHECK(cw_action_bind_text(dzt, "touch:0.left, deadzone:0.18"));
    CHECK(cw_action_deadzone(dzt) > 0.179f && cw_action_deadzone(dzt) < 0.181f);
    CHECK(!cw_action_bind_text(dzt, "deadzone:2"));
  }
  /* 30 Hz simulation at 60 fps: a one-frame press on a frame without a tick */
  raw.keys[44] = 1;
  seen f0 = run_frame(jump, 0);
  CHECK(cw_action_pressed(0, jump) && cw_action_just_pressed(0, jump)); /* frame context sees it now */
  raw.keys[44] = 0;
  seen f1 = run_frame(jump, 1);
  CHECK_EQ_INT(f0.pressed + f0.jp, 0);
  CHECK_EQ_INT(f1.pressed, 1); /* latched */
  CHECK_EQ_INT(f1.jp, 1);
  CHECK(cw_action_just_released(0, jump)); /* frame context: released this frame */
  seen f2 = run_frame(jump, 0);
  seen f3 = run_frame(jump, 1);
  CHECK_EQ_INT(f2.jr + f3.jr, 1);
  CHECK_EQ_INT(f3.pressed, 0);

  /* two ticks in one frame: just_pressed fires on the first only */
  raw.pads[0].buttons = 1;
  seen g = run_frame(jump, 2);
  CHECK_EQ_INT(g.pressed, 2);
  CHECK_EQ_INT(g.jp, 1);
  g = run_frame(jump, 2);
  CHECK_EQ_INT(g.jp, 0);
  CHECK_EQ_INT(g.pressed, 2);
  raw.pads[0].buttons = 0;
  g = run_frame(jump, 2);
  CHECK_EQ_INT(g.jr, 1);
  CHECK_EQ_INT(g.pressed, 0);

  /* a press released inside one frame's event batch: only the "went down" bit */
  raw.keys_pressed[44] = 1;
  g = run_frame(jump, 1);
  CHECK_EQ_INT(g.pressed, 1);
  CHECK_EQ_INT(g.jp, 1);
  g = run_frame(jump, 1);
  CHECK_EQ_INT(g.jr, 1);
  CHECK_EQ_INT(g.pressed, 0);

  /* the same with no tick in that frame: still not lost */
  raw.pads[0].pressed = 1;
  g = run_frame(jump, 0);
  g = run_frame(jump, 1);
  CHECK_EQ_INT(g.jp, 1);

  /* a held press that started on a tickless frame fires once */
  raw.keys[44] = 1;
  run_frame(jump, 0);
  g = run_frame(jump, 1);
  CHECK_EQ_INT(g.jp, 1);
  g = run_frame(jump, 2);
  CHECK_EQ_INT(g.jp, 0);
  CHECK_EQ_INT(g.pressed, 2);
}

static void test_strength_and_vector(void) {
  fresh();
  cw_action l = cw_action_add("left", 0.5f), r = cw_action_add("right", 0.5f);
  cw_action u = cw_action_add("up", 0.5f), d = cw_action_add("down", 0.5f);
  cw_action fire = cw_action_add("fire", 0.25f);
  CHECK(cw_action_bind_text(l, "axis:lx-, key:a"));
  CHECK(cw_action_bind_text(r, "axis:lx+, key:d"));
  CHECK(cw_action_bind_text(u, "axis:ly-"));
  CHECK(cw_action_bind_text(d, "axis:ly+"));
  CHECK(cw_action_bind_text(fire, "axis:rt+@0.5"));
  raw.pads[0].axes[CW_AXIS_LX] = 0.75f;
  raw.pads[0].axes[CW_AXIS_LY] = -0.3f;
  raw.pads[0].axes[CW_AXIS_RT] = 0.4f; /* under the binding threshold */
  run_frame(r, 1);
  CHECK(cw_action_pressed(0, r));
  CHECK(!cw_action_pressed(0, u)); /* 0.3 < deadzone 0.5 */
  CHECK(fabsf(cw_action_raw_strength(0, u) - 0.3f) < 1e-4f);
  CHECK(fabsf(cw_action_strength(0, r) - 0.5f) < 1e-4f); /* (0.75-0.5)/(1-0.5) */
  CHECK(!cw_action_pressed(0, fire));
  CHECK(cw_action_raw_strength(0, fire) == 0);
  float ax = cw_input_axis(0, l, r);
  FOLD(ax);
  cw_vec2 va = cw_input_vector(0, l, r, u, d, CW_DEADZONE_AXIAL, 0.4f);
  cw_vec2 vr = cw_input_vector(0, l, r, u, d, CW_DEADZONE_RADIAL, 0.4f);
  cw_vec2 vs = cw_input_vector(0, l, r, u, d, CW_DEADZONE_SCALED_RADIAL, 0.4f);
  FOLD(va);
  FOLD(vr);
  FOLD(vs);
  CHECK(va.y == 0 && fabsf(va.x - (0.75f - 0.4f) / 0.6f) < 1e-4f);
  CHECK(fabsf(vr.x - 0.75f) < 1e-4f && fabsf(vr.y + 0.3f) < 1e-4f);
  float len = sqrtf(0.75f * 0.75f + 0.09f);
  CHECK(fabsf(sqrtf(vs.x * vs.x + vs.y * vs.y) - (len - 0.4f) / 0.6f) < 1e-4f);
  raw.pads[0].axes[CW_AXIS_LX] = 0.2f;
  raw.pads[0].axes[CW_AXIS_LY] = 0.2f;
  run_frame(r, 1);
  vr = cw_input_vector(0, l, r, u, d, CW_DEADZONE_RADIAL, 0.4f);
  CHECK(vr.x == 0 && vr.y == 0);
  raw.pads[0].axes[CW_AXIS_LX] = 1;
  raw.pads[0].axes[CW_AXIS_LY] = 1;
  raw.pads[0].axes[CW_AXIS_RT] = 0.8f;
  run_frame(r, 1);
  vr = cw_input_vector(0, l, r, u, d, CW_DEADZONE_RADIAL, 0.1f);
  CHECK(fabsf(sqrtf(vr.x * vr.x + vr.y * vr.y) - 1) < 1e-5f); /* clamped */
  CHECK(cw_action_pressed(0, fire));
  /* quantized: a host that delivers int16/32767 gives the same bits */
  raw.pads[0].axes[CW_AXIS_LX] = 0.6f;
  run_frame(r, 1);
  float s1 = cw_action_raw_strength(0, r);
  raw.pads[0].axes[CW_AXIS_LX] = (float)(int)(0.6f * 32767 + 0.5f) / 32767.0f;
  run_frame(r, 1);
  CHECK(s1 == cw_action_raw_strength(0, r));
  FOLD(s1);
}

static void test_players(void) {
  fresh();
  cw_action jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump, "pad:south, key:space");
  raw.pads[1].buttons = 1;
  run_frame(jump, 1);
  CHECK(!cw_action_pressed(0, jump));
  CHECK(cw_action_pressed(1, jump));
  raw.keys[44] = 1;
  run_frame(jump, 1);
  CHECK(cw_action_pressed(0, jump));
  cw_input_assign(1, -1, CW_DEVICE_KEYBOARD);
  cw_input_assign(0, 1, 0);
  raw.keys[44] = 0;
  raw.pads[1].buttons = 0;
  run_frame(jump, 1);
  raw.keys[44] = 1;
  run_frame(jump, 1);
  CHECK(cw_action_pressed(1, jump) && !cw_action_pressed(0, jump));
  CHECK_EQ_INT(cw_input_player_pad(0), 1);
}

static int changed_calls;
static void on_changed(void *u) {
  (void)u;
  changed_calls++;
}

static void test_text(void) {
  fresh();
  const char *forms[] = { "key:space", "key:a", "key:0", "key:f5", "key:left", "key:lshift", "key:300", "mouse:right",
                          "wheel:up", "wheel:left", "pad:south", "pad:rshoulder", "axis:lx-", "axis:rt+@0.5",
                          "touch:3", "touch:1.down" };
  for (size_t i = 0; i < CW_ARRAY_LEN(forms); i++) {
    cw_binding b;
    char buf[48];
    bool ok = cw_binding_parse(forms[i], &b);
    CHECK(ok);
    if (ok) {
      cw_binding_format(b, buf, sizeof buf);
      if (strcmp(buf, forms[i])) cw_error("format %s -> %s", forms[i], buf);
      CHECK(!strcmp(buf, forms[i]));
    }
  }
  cw_binding b;
  CHECK(!cw_binding_parse("key:nope", &b));
  CHECK(!cw_binding_parse("pad:x", &b));
  CHECK(!cw_binding_parse("axis:lx", &b));
  CHECK(!cw_binding_parse("axis:lx+@2", &b));
  CHECK(!cw_binding_parse("touch:99", &b));
  CHECK(!cw_binding_parse("space", &b));

  const char *defaults = "# defaults\n"
                         "jump = key:space, pad:south\n"
                         "fire = axis:rt+@0.5, key:j, deadzone:0.3\r\n"
                         "\n"
                         "special =\n";
  CHECK(cw_input_load_bindings(defaults, strlen(defaults)));
  CHECK_EQ_INT(cw_action_count(), 3);
  cw_action fire = cw_action_find("fire");
  CHECK(fabsf(cw_action_deadzone(fire) - 0.3f) < 1e-6f);
  cw_binding list[8];
  CHECK_EQ_INT(cw_action_bindings(fire, list, 8), 2);
  cw_input_commit_defaults();
  char out[512];
  size_t n = cw_input_save_bindings(out, sizeof out, true);
  CHECK_EQ_INT(n, 0);
  cw_input_on_bindings_changed(on_changed, NULL);
  changed_calls = 0;
  cw_action special = cw_action_find("special");
  cw_binding east;
  cw_binding_parse("pad:east", &east);
  CHECK(cw_action_bind(special, east));
  CHECK(!cw_action_bind(special, east)); /* already there */
  CHECK_EQ_INT(changed_calls, 1);
  n = cw_input_save_bindings(out, sizeof out, true);
  CHECK(!strcmp(out, "special = pad:east\n"));
  /* snprintf contract */
  char tiny[8];
  CHECK_EQ_INT(cw_input_save_bindings(tiny, sizeof tiny, true), n);
  CHECK_EQ_INT(strlen(tiny), 7);
  /* a bad line changes nothing and reports false */
  const char *bad = "jump = key:space, pad:nope\nspecial = key:k\n";
  CHECK(!cw_input_load_bindings(bad, strlen(bad)));
  CHECK_EQ_INT(cw_action_bindings(cw_action_find("jump"), list, 8), 2);
  CHECK_EQ_INT(cw_action_bindings(special, list, 8), 1);
  CHECK_EQ_INT(list[0].kind, CW_BIND_KEY);
  /* the user's overrides round-trip through text */
  size_t full = cw_input_save_bindings(out, sizeof out, false);
  CHECK(full > 0 && full < sizeof out);
  uint32_t h = cw_hash32(out, full, CW_HASH32_INIT);
  FOLD(h);
  cw_input_reset_to_defaults();
  CHECK_EQ_INT(cw_action_bindings(special, list, 8), 0);
  CHECK(cw_input_load_bindings(out, full));
  char again[512];
  cw_input_save_bindings(again, sizeof again, false);
  CHECK(!strcmp(out, again));
}

static void test_capture(void) {
  fresh();
  cw_action_add("jump", 0.5f);
  cw_input_capture_begin(0, CW_DEVICE_KEYBOARD, true);
  cw_binding b;
  run_frame(0, 1);
  CHECK(!cw_input_capture_poll(&b));
  raw.pads[0].axes[CW_AXIS_LY] = -0.8f;
  run_frame(0, 1);
  CHECK(cw_input_capture_poll(&b));
  CHECK(b.kind == CW_BIND_PAD_AXIS && b.code == CW_AXIS_LY && b.dir == -1);
  CHECK(!cw_input_capturing());
  cw_input_capture_begin(0, CW_DEVICE_KEYBOARD, false);
  raw.keys[7] = 1;
  run_frame(0, 1);
  CHECK(cw_input_capture_poll(&b) && b.kind == CW_BIND_KEY && b.code == 7);
}

static void test_touch(void) {
  fresh();
  cw_action fire = cw_action_add("fire", 0.5f);
  cw_action right = cw_action_add("right", 0.5f);
  int btn = cw_touch_add_button(500, 300, 100, 60);
  int stick = cw_touch_add_stick(0, 200, 200, 160, 50, true);
  char spec[32];
  snprintf(spec, sizeof spec, "touch:%d", btn);
  CHECK(cw_action_bind_text(fire, spec));
  snprintf(spec, sizeof spec, "touch:%d.right", stick);
  CHECK(cw_action_bind_text(right, spec));
  raw.touches[2] = (cw_touch_state){ .active = true, .x = 100, .y = 280 };
  run_frame(fire, 1);
  CHECK(cw_touch_control_active(stick));
  CHECK(cw_action_raw_strength(0, right) == 0); /* floating: centered where it landed */
  raw.touches[2].x = 140; /* 40 / 50 */
  raw.touches[3] = (cw_touch_state){ .active = true, .x = 550, .y = 330 };
  seen s = run_frame(fire, 1);
  CHECK_EQ_INT(s.jp, 1);
  CHECK(fabsf(cw_action_raw_strength(0, right) - 0.8f) < 1e-4f);
  raw.touches[2].x = 400; /* slid off the stick's rect: still steering, clamped */
  run_frame(fire, 1);
  CHECK(cw_action_raw_strength(0, right) == 1);
  cw_vec2 v = cw_touch_stick_value(stick);
  FOLD(v);
  /* a touch that starts outside every control is nobody's */
  raw.touches[2].active = false;
  raw.touches[3].active = false;
  run_frame(fire, 1);
  raw.touches[4] = (cw_touch_state){ .active = true, .x = 300, .y = 10 };
  run_frame(fire, 1);
  CHECK(!cw_touch_control_active(btn) && !cw_touch_control_active(stick));
  /* a tap inside one frame */
  raw.touches[4].active = false;
  raw.touches[5] = (cw_touch_state){ .active = false, .tapped = true, .x = 510, .y = 310 };
  s = run_frame(fire, 1);
  CHECK_EQ_INT(s.jp, 1);
  /* touch belongs to player 0 only by default */
  CHECK(!cw_action_pressed(1, fire));
}

static bool near2(cw_vec2 a, float x, float y) { return fabsf(a.x - x) < 1e-3f && fabsf(a.y - y) < 1e-3f; }

/* Land a thumb at (lx, ly), slide it to (mx, my), lift it, then land a second
 * one at (sx, sy): true when the stick's centre stayed at its home throughout
 * (what the player sees: the ring does not jump to where the thumb landed). */
static bool home_stays(int stick, float lx, float ly, float mx, float my, float sx, float sy) {
  cw_vec2 home = cw_touch_home(stick);
  bool same = near2(cw_touch_stick_center(stick), home.x, home.y);
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = lx, .y = ly };
  run_frame(-1, 1);
  same = same && cw_touch_control_active(stick) && near2(cw_touch_stick_center(stick), home.x, home.y);
  raw.touches[0].x = mx, raw.touches[0].y = my;
  run_frame(-1, 1);
  same = same && near2(cw_touch_stick_center(stick), home.x, home.y);
  raw.touches[0].active = false;
  run_frame(-1, 1);
  same = same && !cw_touch_control_active(stick) && near2(cw_touch_stick_center(stick), home.x, home.y);
  raw.touches[1] = (cw_touch_state){ .active = true, .tapped = true, .x = sx, .y = sy };
  run_frame(-1, 1);
  same = same && cw_touch_control_active(stick) && near2(cw_touch_stick_center(stick), home.x, home.y);
  raw.touches[1].active = false;
  run_frame(-1, 1);
  return same;
}

/* The anchored controls: a thumbstick with a home that stays put, sizes in
 * touch units that follow the area, round buttons, the overlay's switch. */
static void test_thumbstick(void) {
  fresh();
  cw_action right = cw_action_add("right", 0.5f), down = cw_action_add("down", 0.5f), jump = cw_action_add("jump", 0.5f);
  cw_touch_set_area(0, 0, 960, 540); /* 540 high: one touch unit is one window unit */
  int stick = cw_touch_add_thumbstick(NULL);
  int btn = cw_touch_add_round_button(&(cw_touch_button_desc){ .label = "JUMP" });
  CHECK(cw_action_bind_text(right, "touch:0.right") && cw_action_bind_text(down, "touch:0.down"));
  CHECK(cw_action_bind_text(jump, "touch:1"));
  CHECK(fabsf(cw_touch_unit() - 1) < 1e-6f);
  /* the defaults: home 150,140 from the bottom left, throw 70; the button 110,110 from the bottom right, radius 48 */
  CHECK(near2(cw_touch_home(stick), 150, 400));
  CHECK(fabsf(cw_touch_radius(stick) - 70) < 1e-4f);
  CHECK(near2(cw_touch_home(btn), 850, 430));
  CHECK(fabsf(cw_touch_radius(btn) - 48) < 1e-4f);

  /* a thumb landing anywhere in the zone steers relative to HOME */
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = 185, .y = 400 };
  run_frame(right, 1);
  CHECK(cw_touch_control_active(stick));
  CHECK(near2(cw_touch_stick_center(stick), 150, 400));
  CHECK(fabsf(cw_action_raw_strength(0, right) - 0.5f) < 1e-3f); /* 35 / 70, not 0 as a floating stick reads */
  raw.touches[0].active = false;
  run_frame(right, 1);
  /* far corner of the zone: still the stick (2.4 x 70 = 168 past home), clamped to full */
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = 310, .y = 535 };
  run_frame(right, 1);
  CHECK(cw_touch_control_active(stick));
  cw_vec2 v = cw_touch_stick_value(stick);
  CHECK(fabsf(sqrtf(v.x * v.x + v.y * v.y) - 1) < 1e-4f && v.x > 0 && v.y > 0);
  FOLD(v);
  raw.touches[0].active = false;
  run_frame(right, 1);
  /* just outside the zone: nobody's (the rest of the screen is the game's) */
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = 330, .y = 400 };
  run_frame(right, 1);
  CHECK(!cw_touch_control_active(stick));
  raw.touches[0].active = false;
  run_frame(right, 1);

  /* the gate's claim: the home does not move, wherever in the zone the thumb lands */
  CHECK(home_stays(stick, 20, 520, 260, 300, 300, 250));
  /* the control that must fail: the older floating feel moves the ring to the thumb */
  int floating = cw_touch_add_thumbstick(&(cw_touch_stick_desc){ .corner = CW_TOUCH_TOP_RIGHT, .floating = true });
  CHECK(near2(cw_touch_home(floating), 810, 140));
  CHECK(!home_stays(floating, 900, 40, 860, 200, 700, 100));
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = 900, .y = 40 };
  run_frame(right, 1);
  CHECK(near2(cw_touch_stick_center(floating), 900, 40) && near2(cw_touch_stick_value(floating), 0, 0));
  raw.touches[0].active = false;
  run_frame(right, 1);
  CHECK(near2(cw_touch_stick_center(floating), 810, 140)); /* let go: the ring is home again */

  /* a button inside the stick's zone takes the touch (nearer, more specific) */
  int inner = cw_touch_add_round_button(&(cw_touch_button_desc){ .corner = CW_TOUCH_BOTTOM_LEFT, .x = 290, .y = 60, .radius = 20 });
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = 290, .y = 480 };
  run_frame(right, 1);
  CHECK(cw_touch_control_active(inner) && !cw_touch_control_active(stick));
  raw.touches[0].active = false;
  run_frame(right, 1);
  /* off: claims nothing; the zone under it is the stick's again */
  cw_touch_set_enabled(inner, false);
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = 290, .y = 480 };
  run_frame(right, 1);
  CHECK(!cw_touch_control_active(inner) && cw_touch_control_active(stick));
  raw.touches[0].active = false;
  run_frame(right, 1);

  /* the round button: within 1.25 x its radius counts (thumbs miss), and it is an action */
  raw.touches[2] = (cw_touch_state){ .active = true, .tapped = true, .x = 850 + 55, .y = 430 };
  seen s = run_frame(jump, 1);
  CHECK_EQ_INT(s.jp, 1);
  raw.touches[2].active = false;
  run_frame(jump, 1);
  raw.touches[2] = (cw_touch_state){ .active = true, .tapped = true, .x = 850 + 62, .y = 430 };
  s = run_frame(jump, 1);
  CHECK_EQ_INT(s.jp, 0);
  raw.touches[2].active = false;
  run_frame(jump, 1);

  /* sizes follow the area's shorter side, laid out again on the next frame (a resize, a turn of the phone) */
  cw_touch_set_area(0, 0, 1920, 1080);
  run_frame(right, 1);
  CHECK(fabsf(cw_touch_unit() - 2) < 1e-6f);
  CHECK(near2(cw_touch_home(stick), 300, 800));
  CHECK(fabsf(cw_touch_radius(stick) - 140) < 1e-3f);
  CHECK(near2(cw_touch_home(btn), 1700, 860));
  CHECK(home_stays(stick, 40, 1040, 520, 600, 600, 500));
  cw_touch_set_area(0, 0, 1080, 1920); /* portrait: the width is the shorter side */
  run_frame(right, 1);
  CHECK(fabsf(cw_touch_unit() - 2) < 1e-6f && near2(cw_touch_home(stick), 300, 1640));
  cw_touch_set_area(100, 50, 1280, 720); /* an inset area (a notch, one player's half) */
  run_frame(right, 1);
  CHECK(near2(cw_touch_home(stick), 100 + 150 * (720 / 540.0f), 50 + 720 - 140 * (720 / 540.0f)));
  /* a throw in touch units, scaled with the rest */
  cw_touch_set_radius(stick, 100);
  CHECK(fabsf(cw_touch_radius(stick) - 100 * (720 / 540.0f)) < 1e-3f);
  /* the whole window again: follows cw_window_size */
  cw_touch_set_area(0, 0, 0, 0);
  run_frame(right, 1);
  int ww, wh;
  cw_window_size(&ww, &wh);
  if (ww > 0 && wh > 0) CHECK(fabsf(cw_touch_unit() - (float)(ww < wh ? ww : wh) / 540.0f) < 1e-5f);
  /* re-placing: a left-handed layout puts the stick bottom right */
  cw_touch_set_area(0, 0, 960, 540);
  cw_touch_set_thumbstick(stick, &(cw_touch_stick_desc){ .corner = CW_TOUCH_BOTTOM_RIGHT });
  CHECK(near2(cw_touch_home(stick), 810, 400));
  cw_touch_set_rect(stick, 0, 0, 10, 10); /* an anchored control ignores set_rect */
  CHECK(near2(cw_touch_home(stick), 810, 400));

  /* the overlay: AUTO shows once a touch lands (a desktop is not touch-first), a pad press hides it */
  CHECK(cw_touch_overlay_visible()); /* touches landed above */
  raw.pads[0].pressed = 1;
  run_frame(right, 1);
  CHECK(cw_platform_touch_primary() || !cw_touch_overlay_visible());
  cw_touch_set_overlay(CW_TOUCH_OVERLAY_ALWAYS);
  CHECK(cw_touch_overlay_visible());
  cw_touch_set_overlay(CW_TOUCH_OVERLAY_OFF);
  CHECK(!cw_touch_overlay_visible());
  cw_touch_set_overlay(CW_TOUCH_OVERLAY_AUTO);
  /* bad ids */
  CHECK(near2(cw_touch_home(99), 0, 0) && cw_touch_radius(-1) == 0);
}

/* random input over frames with 0..3 ticks; replaying the recording must give
 * the same tick snapshots, tick by tick, and the same hash */
static void test_record_replay(void) {
  fresh();
  const char *b = "jump = key:space, pad:south\nfire = axis:rt+@0.5\nleft = axis:lx-, key:a\n"
                  "right = axis:lx+, key:d\nwheel = wheel:up\n";
  CHECK(cw_input_load_bindings(b, strlen(b)));
  enum { TICKS_MAX = 4000 };
  static float strength[TICKS_MAX][4];
  static uint8_t flags[TICKS_MAX][4];
  int nt = 0;
  cw_rng rng;
  cw_rng_seed(&rng, 1234);
  cw_input_record_begin();
  for (int f = 0; f < 1200; f++) {
    if (cw_rng_int(&rng, 0, 9) == 0) raw.keys[44] ^= 1;
    if (cw_rng_int(&rng, 0, 19) == 0) raw.keys_pressed[7] = 1;
    if (cw_rng_int(&rng, 0, 7) == 0) raw.pads[0].axes[CW_AXIS_LX] = cw_rng_range(&rng, -1, 1);
    if (cw_rng_int(&rng, 0, 7) == 0) raw.pads[0].axes[CW_AXIS_RT] = cw_rng_float(&rng);
    if (cw_rng_int(&rng, 0, 29) == 0) raw.wheel_y = 1;
    if (f > 600 && f < 700) { /* a quiet stretch: idle ticks */
      memset(raw.keys, 0, sizeof raw.keys);
      raw.pads[0].axes[CW_AXIS_LX] = raw.pads[0].axes[CW_AXIS_RT] = 0;
    }
    int ticks = cw_rng_int(&rng, 0, 3);
    cw_input_frame_begin(&raw);
    for (int t = 0; t < ticks && nt < TICKS_MAX; t++, nt++) {
      cw_input_tick_begin();
      for (int a = 0; a < 4; a++) {
        strength[nt][a] = cw_action_strength(0, a);
        flags[nt][a] = (uint8_t)(cw_action_pressed(0, a) | cw_action_just_pressed(0, a) << 1 |
                                 cw_action_just_released(0, a) << 2);
      }
    }
    cw_input_ticks_end();
    memset(raw.keys_pressed, 0, sizeof raw.keys_pressed);
    raw.wheel_y = 0;
  }
  cw_input_record_end();
  size_t len;
  const uint8_t *rec = cw_input_recording(&len);
  CHECK_EQ_INT(cw_input_record_ticks(), nt);
  cw_info("recorded %d ticks in %zu bytes", nt, len);
  CHECK(len < (size_t)nt * 4); /* busy random input: a few bytes per tick */
  uint32_t rh = cw_input_record_hash();
  FOLD(rh);
  uint32_t lh = cw_hash32(rec, len, CW_HASH32_INIT);
  FOLD(lh);
  /* replay into a module that defines the actions in another order */
  uint8_t *copy = cw_alloc(len, CW_MEM_GENERAL);
  memcpy(copy, rec, len);
  fresh();
  const char *b2 = "wheel = wheel:up\nright = axis:lx+\nleft = axis:lx-\nfire = axis:rt+@0.5\njump = pad:south\n";
  CHECK(cw_input_load_bindings(b2, strlen(b2)));
  CHECK(cw_input_replay_begin(copy, len));
  cw_free(copy);
  cw_action ids[4] = { cw_action_find("jump"), cw_action_find("fire"), cw_action_find("left"), cw_action_find("right") };
  int bad = 0, i = 0;
  while (cw_input_replay_step()) {
    for (int a = 0; a < 4; a++) {
      uint8_t fl = (uint8_t)(cw_action_pressed(0, ids[a]) | cw_action_just_pressed(0, ids[a]) << 1 |
                             cw_action_just_released(0, ids[a]) << 2);
      if (i >= nt || fl != flags[i][a] || cw_action_strength(0, ids[a]) != strength[i][a]) bad++;
    }
    i++;
  }
  cw_input_set_context(CW_INPUT_FRAME);
  CHECK_EQ_INT(bad, 0);
  CHECK_EQ_INT(i, nt);
  CHECK_EQ_INT(cw_input_replay_ticks(), nt);
  CHECK(cw_input_replay_hash() == rh);
  CHECK(!cw_input_replay_active());
  /* idle ticks cost (almost) nothing */
  fresh();
  cw_action_add("jump", 0.5f);
  cw_input_record_begin();
  size_t empty;
  cw_input_recording(&empty);
  for (int f = 0; f < 3000; f++) run_frame(0, 1);
  cw_input_recording(&len);
  CHECK(len <= empty + 2);
  /* a corrupt recording is refused */
  uint8_t junk[16] = "CWIR\x01\x04\x01\x03jum";
  CHECK(!cw_input_replay_begin(junk, sizeof junk));
}

static void test_aim(void) {
  cw_aim_curve c = cw_aim_curve_default();
  cw_aim_state st = { 0 };
  /* inside the dead zone: nothing */
  cw_vec2 t = cw_aim_step(&st, &c, (cw_vec2){ 0.1f, 0 }, 1.0f / 60, 0);
  CHECK(t.x == 0 && t.y == 0);
  /* half stick turns less than half the full rate (the power curve) */
  cw_aim_state a = { 0 }, b = { 0 };
  float half = cw_aim_step(&a, &c, (cw_vec2){ 0.5f, 0 }, 1.0f / 60, 0).x;
  float full = cw_aim_step(&b, &c, (cw_vec2){ 1, 0 }, 1.0f / 60, 0).x;
  CHECK(half > 0 && half < full * 0.5f);
  CHECK(full > c.yaw_rate / 60 * 0.99f && full < c.yaw_rate / 60 * 1.01f);
  /* holding the edge ramps the turn up to (1 + gain) times, and releasing drains it */
  float last = full;
  for (int i = 0; i < 40; i++) last = cw_aim_step(&b, &c, (cw_vec2){ 1, 0 }, 1.0f / 60, 0).x;
  CHECK(last > full * (1 + c.ramp_gain) * 0.99f);
  cw_aim_step(&b, &c, (cw_vec2){ 0, 0 }, 1.0f / 3, 0);
  CHECK(b.held == 0);
  /* pitch is slower, the direction is kept, and assist slows the turn on a target */
  cw_aim_state d = { 0 };
  cw_vec2 diag = cw_aim_step(&d, &c, (cw_vec2){ 0.7f, 0.7f }, 1.0f / 60, 0);
  CHECK(diag.x > 0 && diag.y > 0 && diag.y < diag.x);
  c.assist = 0.5f;
  cw_aim_state e = { 0 };
  float on = cw_aim_step(&e, &c, (cw_vec2){ 1, 0 }, 1.0f / 60, 1).x;
  CHECK(on > full * 0.49f && on < full * 0.51f);
  /* the control: a stick past 1 is clamped, never faster than full */
  cw_aim_state f = { 0 };
  CHECK(cw_aim_step(&f, &c, (cw_vec2){ 5, 0 }, 1.0f / 60, 0).x <= full * 1.001f);
}

CW_TEST_SUITE("input", { "tick_latch", test_tick_latch }, { "strength_vector", test_strength_and_vector },
              { "players", test_players }, { "text", test_text }, { "capture", test_capture },
              { "touch", test_touch }, { "thumbstick", test_thumbstick }, { "record_replay", test_record_replay }, { "aim", test_aim })

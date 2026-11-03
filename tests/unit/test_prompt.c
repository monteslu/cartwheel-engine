/* Button prompts (cartwheel/prompt.h) driven with synthetic raw input: which
 * device each player is on, which binding an action shows there, rebinding
 * on the same frame, an action with nothing on the device, typos in the
 * markup, movement pairs (and a pad with no stick), shared prompts, markup in
 * translated strings, the test override, and that none of it reaches the
 * simulation's input stream. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/input.h"
#include "cartwheel/l10n.h"
#include "cartwheel/prompt.h"
#include "cartwheel/ui.h"
#include "test.h"

#define FACE(bit) (1u << 16 | (bit))
#define KEYCAP(sc) (9u << 16 | (sc))
#define MOUSE(bit) (10u << 16 | (bit))
#define STICK(lit) (4u << 16 | (lit))
#define DPAD(lit) (5u << 16 | (lit))
#define TOUCHB(id) (11u << 16 | (id))
enum { L = 1, R = 2, U = 4, D = 8 };

static cw_raw_input raw;

static void frame(void) {
  cw_input_frame_begin(&raw);
  cw_input_tick_begin();
  cw_input_ticks_end();
  memset(raw.keys_pressed, 0, sizeof raw.keys_pressed);
  raw.mouse_pressed = 0;
  for (int i = 0; i < CW_MAX_PADS; i++) raw.pads[i].pressed = 0;
  for (int i = 0; i < CW_MAX_TOUCHES; i++) raw.touches[i].tapped = false;
  raw.wheel_x = raw.wheel_y = 0;
}
/* one press and release of a key, a pad button or a touch */
static void key(int sc) { raw.keys[sc] = 1, raw.keys_pressed[sc] = 1; frame(); raw.keys[sc] = 0; frame(); }
static void pad_button(int pad, int b) {
  raw.pads[pad].buttons |= 1u << b, raw.pads[pad].pressed |= 1u << b;
  frame();
  raw.pads[pad].buttons &= ~(1u << b);
  frame();
}
static void touch(void) {
  raw.touches[0] = (cw_touch_state){ .active = true, .tapped = true, .x = 50, .y = 50 };
  frame();
  raw.touches[0].active = false;
  frame();
}

static void fresh(void) {
  unsetenv("CW_TEST_PROMPT_DEVICE");
  cw_input_reset();
  memset(&raw, 0, sizeof raw);
  raw.pads[0].connected = true;
  frame();
}

/* the prompt debug request's numbers */
static unsigned reply_num(const char *field) {
  char *r = cw_debug_run("prompt");
  const char *p = strstr(r, field);
  unsigned v = p ? (unsigned)strtoul(p + strlen(field) + 1, NULL, 10) : 9999u;
  cw_free(r);
  return v;
}

static uint32_t code1(cw_action a, int player) {
  uint32_t c[8] = { 0 };
  int n = cw_prompt_action_codes(&a, 1, player, c, 8);
  return n == 1 ? c[0] : 0xffffffffu;
}

static void test_device_and_binding(void) {
  fresh();
  cw_action jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump, "key:space, pad:south, touch:0");
  cw_action fire = cw_action_add("fire", 0.5f);
  cw_action_bind_text(fire, "mouse:left, pad:west");
  cw_touch_add_round_button(&(cw_touch_button_desc){ .label = "Jump" });
  /* nothing used yet: the connected pad */
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_PAD);
  CHECK_EQ_INT(code1(jump, 0), FACE(1));
  /* a key: the keyboard, and the first key binding */
  key(44);
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_KEYBOARD);
  CHECK_EQ_INT(code1(jump, 0), KEYCAP(44));
  CHECK_EQ_INT(code1(fire, 0), MOUSE(1)); /* no key: the mouse stands in */
  /* a pad button back to the pad */
  pad_button(0, CW_PAD_EAST);
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_PAD);
  CHECK_EQ_INT(code1(fire, 0), FACE(4));
  /* a stick past half way is the pad too; under half is nothing */
  key(44);
  raw.pads[0].axes[CW_AXIS_LX] = 0.3f;
  frame();
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_KEYBOARD);
  raw.pads[0].axes[CW_AXIS_LX] = 0.8f;
  frame();
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_PAD);
  raw.pads[0].axes[CW_AXIS_LX] = 0;
  /* a touch landing */
  touch();
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_TOUCH);
  CHECK_EQ_INT(code1(jump, 0), TOUCHB(0));
  /* the pad unplugged hands a pad player back to the keyboard */
  pad_button(0, CW_PAD_SOUTH);
  raw.pads[0].connected = false;
  frame();
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_KEYBOARD);
  /* a pad-only player on an unplugged pad still answers pad; one with nothing: none */
  CHECK_EQ_INT(cw_prompt_device_of(1), CW_PROMPT_PAD);
  cw_input_assign(2, -1, 0);
  CHECK_EQ_INT(cw_prompt_device_of(2), CW_PROMPT_NONE);
  CHECK_EQ_INT(cw_prompt_device_of(7), CW_PROMPT_NONE);
  /* a second player's pad is theirs alone */
  raw.pads[0].connected = raw.pads[1].connected = true;
  frame();
  key(44);
  pad_button(1, CW_PAD_SOUTH);
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_KEYBOARD);
  CHECK_EQ_INT(cw_prompt_device_of(1), CW_PROMPT_PAD);
}

static void test_rebind_same_frame(void) {
  fresh();
  cw_action jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump, "key:space, pad:south");
  CHECK_EQ_INT(code1(jump, 0), FACE(1));
  /* moved to north: the next query (this frame) lights north */
  cw_binding north;
  CHECK(cw_binding_parse("pad:north", &north));
  CHECK(cw_action_set_binding(jump, 1, north));
  CHECK_EQ_INT(code1(jump, 0), FACE(8));
  /* by capture, as a remap screen does */
  cw_input_capture_begin(0, 0, true);
  raw.pads[0].buttons = raw.pads[0].pressed = 1u << CW_PAD_EAST;
  frame();
  cw_binding b;
  CHECK(cw_input_capture_poll(&b));
  cw_action_set_binding(jump, 1, b);
  CHECK_EQ_INT(code1(jump, 0), FACE(2));
  raw.pads[0].buttons = 0;
}

static void test_missing(void) {
  fresh();
  cw_action aim = cw_action_add("aim_only_mouse", 0.5f);
  cw_action_bind_text(aim, "mouse:right");
  unsigned before = reply_num("\"missing\"");
  uint32_t c[4];
  CHECK_EQ_INT(cw_prompt_action_codes(&aim, 1, 0, c, 4), 0); /* the pad: nothing, never a guess */
  CHECK_EQ_INT(cw_prompt_action_size(aim, 0, 20).x, 0);
  CHECK_EQ_INT(reply_num("\"missing\""), before + 1); /* logged once */
  cw_prompt_action_codes(&aim, 1, 0, c, 4);
  CHECK_EQ_INT(reply_num("\"missing\""), before + 1);
  key(4); /* the keyboard: the mouse stands in, nothing missing */
  CHECK_EQ_INT(cw_prompt_action_codes(&aim, 1, 0, c, 4), 1);
  CHECK_EQ_INT(c[0], MOUSE(2));
}

static void test_typo(void) {
  fresh();
  cw_action jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump, "key:space, pad:south");
  cw_font f = cw_ui_font_sans();
  unsigned before = reply_num("\"errors\"");
  cw_vec2 ok = cw_prompt_text_size(f, "{jump}: hop", 0, 20);
  CHECK_EQ_INT(reply_num("\"errors\""), before);
  cw_vec2 bad = cw_prompt_text_size(f, "{jupm}: hop", 0, 20);
  CHECK_EQ_INT(reply_num("\"errors\""), before + 1);
  CHECK(bad.x < ok.x); /* the typo draws nothing */
  cw_prompt_text_size(f, "{jupm}: hop", 0, 20); /* the same typo again: logged and counted once */
  CHECK_EQ_INT(reply_num("\"errors\""), before + 1);
  cw_prompt_text_size(f, "{pad:sooth} and {a|b|c} and {unclosed", 0, 20); /* three more */
  CHECK_EQ_INT(reply_num("\"errors\""), before + 4);
  /* {{ and }} are braces, not markup */
  cw_vec2 br = cw_prompt_text_size(f, "{{jump}}", 0, 20);
  CHECK_EQ_INT(reply_num("\"errors\""), before + 4);
  CHECK(br.x > 0);
}

static void test_movement(void) {
  fresh();
  cw_action m[4] = { cw_action_add("left", 0.3f), cw_action_add("right", 0.3f), cw_action_add("up", 0.3f), cw_action_add("down", 0.3f) };
  cw_action_bind_text(m[0], "key:a, pad:left, axis:lx-");
  cw_action_bind_text(m[1], "key:d, pad:right, axis:lx+");
  cw_action_bind_text(m[2], "key:w, pad:up, axis:ly-");
  cw_action_bind_text(m[3], "key:s, pad:down, axis:ly+");
  uint32_t c[8];
  /* a pair on a pad with both: one stick glyph, left and right */
  CHECK_EQ_INT(cw_prompt_action_codes(m, 2, 0, c, 8), 1);
  CHECK_EQ_INT(c[0], STICK((L | R) << 4));
  CHECK_EQ_INT(cw_prompt_action_codes(m + 2, 2, 0, c, 8), 1);
  CHECK_EQ_INT(c[0], STICK((U | D) << 4));
  CHECK_EQ_INT(cw_prompt_action_codes(m, 4, 0, c, 8), 1);
  CHECK_EQ_INT(c[0], STICK((L | R | U | D) << 4));
  /* a pad used only through its d-pad (no stick): the d-pad */
  pad_button(0, CW_PAD_LEFT);
  CHECK_EQ_INT(cw_prompt_action_codes(m, 2, 0, c, 8), 1);
  CHECK_EQ_INT(c[0], DPAD(L | R));
  /* the stick used once: the stick again */
  raw.pads[0].axes[CW_AXIS_LY] = -1;
  frame();
  raw.pads[0].axes[CW_AXIS_LY] = 0;
  frame();
  CHECK_EQ_INT(cw_prompt_action_codes(m, 4, 0, c, 8), 1);
  CHECK_EQ_INT(c[0], STICK((L | R | U | D) << 4));
  /* the right stick only */
  cw_action look[2] = { cw_action_add("look_l", 0.3f), cw_action_add("look_r", 0.3f) };
  cw_action_bind_text(look[0], "axis:rx-");
  cw_action_bind_text(look[1], "axis:rx+");
  CHECK_EQ_INT(cw_prompt_action_codes(look, 2, 0, c, 8), 1);
  CHECK_EQ_INT(c[0], STICK(1 | (L | R) << 4));
  /* nothing in common (shoulders): each action's own glyph */
  cw_action sh[2] = { cw_action_add("prev", 0.5f), cw_action_add("next", 0.5f) };
  cw_action_bind_text(sh[0], "pad:lshoulder");
  cw_action_bind_text(sh[1], "pad:rshoulder");
  CHECK_EQ_INT(cw_prompt_action_codes(sh, 2, 0, c, 8), 2);
  CHECK_EQ_INT(c[0], 2u << 16 | 1);
  CHECK_EQ_INT(c[1], 2u << 16 | 2);
  /* the keyboard: each key */
  key(4);
  CHECK_EQ_INT(cw_prompt_action_codes(m, 2, 0, c, 8), 2);
  CHECK_EQ_INT(c[0], KEYCAP(4));
  CHECK_EQ_INT(c[1], KEYCAP(7));
  /* three is no movement set */
  CHECK_EQ_INT(cw_prompt_action_codes(m, 3, 0, c, 8), 0);
}

static void test_shared(void) {
  fresh();
  cw_action jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump, "key:space, pad:south");
  uint32_t c[8];
  /* only the pad in use: the diamond */
  CHECK_EQ_INT(cw_prompt_action_codes(&jump, 1, -1, c, 8), 1);
  CHECK_EQ_INT(c[0], FACE(1));
  /* seat 2 on a pad, seat 1 on the keyboard: both */
  raw.pads[1].connected = true;
  frame();
  key(44);
  CHECK_EQ_INT(cw_prompt_action_codes(&jump, 1, -1, c, 8), 2);
  CHECK_EQ_INT(c[0], FACE(1));
  CHECK_EQ_INT(c[1], KEYCAP(44));
  /* per seat: each its own */
  CHECK_EQ_INT(code1(jump, 0), KEYCAP(44));
  CHECK_EQ_INT(code1(jump, 1), FACE(1));
  /* seats with nothing connected are not counted */
  raw.pads[1].connected = false;
  frame();
  CHECK_EQ_INT(cw_prompt_action_codes(&jump, 1, -1, c, 8), 1);
  CHECK_EQ_INT(c[0], KEYCAP(44));
}

static void test_translation(void) {
  fresh();
  cw_action jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump, "key:space, pad:south");
  static const char FR[] = "@locale = fr\nhint.jump = {jump} : sauter un trou\nhint.score = {jump} {n} points\n";
  CHECK(cw_l10n_add_catalog(FR, sizeof FR - 1, "fr"));
  CHECK(cw_l10n_set_locale("fr"));
  const char *t = cw_tr("hint.jump");
  CHECK(strstr(t, "{jump}") != NULL); /* the markup survives the catalog */
  const char *f = cw_trf("hint.score", "n", "3", NULL);
  CHECK(!strcmp(f, "{jump} 3 points")); /* and a formatted string: unknown names are kept */
  cw_font font = cw_ui_font_sans();
  unsigned before = reply_num("\"errors\"");
  cw_vec2 with = cw_prompt_text_size(font, t, 0, 20), plain = cw_prompt_text_size(font, " : sauter un trou", 0, 20);
  CHECK(with.x > plain.x + 15); /* the glyph takes room */
  CHECK_EQ_INT(reply_num("\"errors\""), before);
  /* a pseudo-locale keeps the markup unaccented */
  cw_l10n_reset();
}

static void test_determinism(void) {
  /* the same input twice, once with prompts drawn and devices switching and forced, once without: one stream hash */
  uint32_t h[2];
  for (int run = 0; run < 2; run++) {
    fresh();
    cw_action jump = cw_action_add("jump", 0.5f);
    cw_action_bind_text(jump, "key:space, pad:south");
    for (int i = 0; i < 20; i++) {
      if (i == 3) raw.keys[44] = 1, raw.keys_pressed[44] = 1;
      if (i == 5) raw.keys[44] = 0;
      if (i == 8) raw.pads[0].buttons = raw.pads[0].pressed = 1;
      if (i == 9) raw.pads[0].buttons = 0;
      if (run) {
        cw_prompt_action_codes(&jump, 1, 0, &(uint32_t){ 0 }, 1);
        cw_prompt_force_device(0, i == 12 ? CW_PROMPT_TOUCH : CW_PROMPT_NONE);
        (void)cw_prompt_device_of(0);
      }
      frame();
    }
    h[run] = cw_input_stream_hash();
  }
  CHECK_EQ_INT(h[0], h[1]);
#ifdef NDEBUG
  /* from fixed_update: a release build logs once and counts it (a debug build asserts) */
  unsigned before = reply_num("\"errors\"");
  cw_input_frame_begin(&raw);
  cw_input_tick_begin();
  (void)cw_prompt_device_of(0);
  cw_input_ticks_end();
  CHECK_EQ_INT(reply_num("\"errors\""), before + 1);
#endif
}

static void test_override(void) {
  fresh();
  setenv("CW_TEST_PROMPT_DEVICE", "touch", 1);
  cw_input_reset(); /* read when the module starts */
  memset(&raw, 0, sizeof raw);
  raw.pads[0].connected = true;
  frame();
  pad_button(0, CW_PAD_SOUTH);
  key(44);
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_TOUCH);
  CHECK_EQ_INT(cw_prompt_device_of(1), CW_PROMPT_TOUCH);
  unsetenv("CW_TEST_PROMPT_DEVICE");
  /* the debug request forces one player, auto ends it */
  fresh();
  cw_free(cw_debug_run("prompt device 0 keyboard"));
  pad_button(0, CW_PAD_SOUTH);
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_KEYBOARD);
  cw_free(cw_debug_run("prompt device 0 auto"));
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_PAD);
  /* an input script's first event on another device switches it, as a player would */
  key(44);
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_KEYBOARD);
  raw.mouse_buttons = raw.mouse_pressed = 1;
  frame();
  raw.mouse_buttons = 0;
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_MOUSE);
  raw.wheel_y = 1;
  pad_button(0, CW_PAD_START);
  CHECK_EQ_INT(cw_prompt_device_of(0), CW_PROMPT_PAD);
}

static void test_codes(void) {
  fresh();
  static const struct { const char *b; uint32_t code; } table[] = {
    { "pad:south", FACE(1) }, { "pad:east", FACE(2) }, { "pad:west", FACE(4) }, { "pad:north", FACE(8) },
    { "pad:lshoulder", 2u << 16 | 1 }, { "axis:rt+", 3u << 16 | 2 }, { "axis:lx-", STICK(L << 4) },
    { "axis:ry+", STICK(1 | D << 4) }, { "pad:rstick", STICK(0x101) }, { "pad:up", DPAD(U) }, { "pad:start", 6u << 16 },
    { "pad:back", 7u << 16 }, { "pad:guide", 8u << 16 }, { "key:return", KEYCAP(40) }, { "mouse:middle", MOUSE(4) },
    { "wheel:down", MOUSE(D << 4) }, { "touch:3", TOUCHB(3) }, { "touch:1.up", 12u << 16 | 1 | U << 8 },
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(table); i++) {
    cw_binding b;
    CHECK(cw_binding_parse(table[i].b, &b));
    CHECK_EQ_INT(cw_prompt_glyph_code(b), table[i].code);
    FOLD(table[i].code);
  }
  cw_binding none = { 0 };
  CHECK_EQ_INT(cw_prompt_glyph_code(none), 0);
  CHECK_EQ_INT(cw_prompt_binding_size(none, 20).x, 0);
  cw_binding s;
  cw_binding_parse("pad:south", &s);
  CHECK_EQ_INT(cw_prompt_binding_size(s, 20).y, 20);
}

CW_TEST_SUITE("prompt", { "device_and_binding", test_device_and_binding }, { "rebind_same_frame", test_rebind_same_frame },
              { "missing", test_missing }, { "typo", test_typo }, { "movement", test_movement }, { "shared", test_shared },
              { "translation", test_translation }, { "determinism", test_determinism }, { "override", test_override },
              { "codes", test_codes })

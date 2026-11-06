/* Button prompts (cartwheel/prompt.h): which device each player is on, the
 * binding an action shows there, the vector glyphs, and text with glyphs in
 * it. Presentation only: nothing here reaches a snapshot or the stream hash. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/prompt.h"
#include "cartwheel/a11y.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/l10n.h"
#include "cartwheel/platform.h"
#include "cartwheel/ui.h"
#include "../input/input_internal.h"
#include "../render2d/text_raw.h"

#define I cw__in

enum { PK_NONE, PK_FACE, PK_SHOULDER, PK_TRIGGER, PK_STICK, PK_DPAD, PK_START, PK_BACK, PK_GUIDE, PK_KEY, PK_MOUSE,
       PK_TOUCH_BUTTON, PK_TOUCH_STICK };
/* direction bits: left, right, up, down */
enum { DIR_L = 1, DIR_R = 2, DIR_U = 4, DIR_D = 8 };
/* SDL scancodes of the arrow keys */
enum { SC_RIGHT = 79, SC_LEFT = 80, SC_DOWN = 81, SC_UP = 82 };

typedef struct glyph {
  uint8_t kind;
  uint32_t lit;   /* what is solid (prompt.h, cw_prompt_glyph_code) */
  char label[32]; /* keycap text, a touch button's label */
  cw_color tint;  /* a touch button's fill */
} glyph;
enum { MAX_SET = 8 };
typedef struct gset {
  glyph g[MAX_SET];
  int n;
} gset;

static struct {
  bool fields;
  cw_font font;
  int32_t dbg_devices[CW_INPUT_MAX_PLAYERS];
  uint32_t errors, missing;
  uint32_t logged[128]; /* hashes of the messages logged once */
  int nlogged;
  bool break_wrong_dot, break_ignore_force, break_stick_first; /* CW_TEST_PROMPT_BREAK: a gate's control */
} P;

static const char *const DEVICE_NAMES[CW_PROMPT_DEVICE_COUNT] = { "none", "pad", "keyboard", "mouse", "touch" };
const char *cw_prompt_device_name(cw_prompt_device d) {
  return d >= 0 && d < CW_PROMPT_DEVICE_COUNT ? DEVICE_NAMES[d] : "none";
}
static cw_prompt_device device_by_name(const char *s) {
  for (int i = 0; i < CW_PROMPT_DEVICE_COUNT; i++)
    if (s && !strcmp(s, DEVICE_NAMES[i])) return (cw_prompt_device)i;
  if (s && !strcmp(s, "auto")) return CW_PROMPT_NONE;
  return (cw_prompt_device)-1;
}

/* a message logged once per distinct text; true the first time */
static bool once(const char *msg) {
  uint32_t h = cw_hash32(msg, strlen(msg), CW_HASH32_INIT);
  for (int i = 0; i < P.nlogged; i++)
    if (P.logged[i] == h) return false;
  if (P.nlogged < (int)CW_ARRAY_LEN(P.logged)) P.logged[P.nlogged++] = h;
  return true;
}
static void prompt_error(const char *msg) {
  if (!once(msg)) return;
  cw_error("%s", msg);
  P.errors++;
}

static void ensure_fields(void) {
  if (P.fields) return;
  P.fields = true;
  cw_debug_field("prompt.devices", P.dbg_devices, CW_DBG_I32, CW_INPUT_MAX_PLAYERS);
  cw_debug_u32("prompt.errors", &P.errors);
  cw_debug_u32("prompt.missing", &P.missing);
  cw_debug_u32("prompt.count", &I.prompt_count);
  cw_debug_field("prompt.glyphs", I.prompt_codes, CW_DBG_U32, 16);
}

/* every prompt call: never from fixed_update (the answer follows the player's hands, not the simulation) */
static void check_context(void) {
  ensure_fields();
  bool tick = cw_input_context_now() == CW_INPUT_TICK;
  CW_ASSERT(!tick, "prompts are presentation: draw and query them from update or render, never fixed_update");
  if (tick) prompt_error("prompt: called from fixed_update; prompts are presentation (call them from update or render)");
}

/* ── devices ─────────────────────────────────────────────────────────── */

/* the frame's raw input, as the input module last saw it (a unit test's synthetic input too) */
static bool pad_connected(int pad) { return pad >= 0 && pad < CW_MAX_PADS && I.prev_raw.pads[pad].connected; }

static cw_prompt_device device_of(int p) {
  if (!I.init || p < 0 || p >= CW_INPUT_MAX_PLAYERS) return CW_PROMPT_NONE;
  if (I.prompt_force[p] && !P.break_ignore_force) return (cw_prompt_device)I.prompt_force[p];
  uint32_t dev = I.devices[p];
  int pad = I.pad[p];
  switch ((cw_prompt_device)I.prompt_last[p]) {
  case CW_PROMPT_PAD: if (pad_connected(pad)) return CW_PROMPT_PAD; break;
  case CW_PROMPT_KEYBOARD: if (dev & CW_DEVICE_KEYBOARD) return CW_PROMPT_KEYBOARD; break;
  case CW_PROMPT_MOUSE: if (dev & CW_DEVICE_MOUSE) return CW_PROMPT_MOUSE; break;
  case CW_PROMPT_TOUCH: if (dev & CW_DEVICE_TOUCH) return CW_PROMPT_TOUCH; break;
  default: break;
  }
  if (pad_connected(pad)) return CW_PROMPT_PAD;
  if ((dev & CW_DEVICE_TOUCH) && cw_platform_touch_primary()) return CW_PROMPT_TOUCH;
  if (dev & CW_DEVICE_KEYBOARD) return CW_PROMPT_KEYBOARD;
  if (dev & CW_DEVICE_MOUSE) return CW_PROMPT_MOUSE;
  if (pad >= 0) return CW_PROMPT_PAD;
  return CW_PROMPT_NONE;
}

cw_prompt_device cw_prompt_device_of(int player) {
  check_context();
  return device_of(player);
}

void cw_prompt_force_device(int player, cw_prompt_device d) {
  cw__input_ensure_init();
  if (d < 0 || d >= CW_PROMPT_DEVICE_COUNT) d = CW_PROMPT_NONE;
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++)
    if (player < 0 || player == p) I.prompt_force[p] = (uint8_t)d;
}

/* called by the input module each frame, after it saw the frame's devices */
void cw__prompt_frame(void) {
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++) P.dbg_devices[p] = (int32_t)device_of(p);
}

/* a seat is present: its pad is connected, or it holds the keyboard, mouse or touch */
static bool present(int p) {
  return pad_connected(I.pad[p]) || (I.devices[p] & (CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH));
}

/* ── the debug request and test switches ────────────────────────────── */

static void cmd_prompt(int argc, const char **argv, cw_reply *r) {
  if (argc >= 4 && !strcmp(argv[1], "device")) {
    cw_prompt_device d = device_by_name(argv[3]);
    if ((int)d < 0) {
      cw_reply_error(r, "prompt device <player|all> <pad|keyboard|mouse|touch|auto>: '%s' is no device", argv[3]);
      return;
    }
    int p = !strcmp(argv[2], "all") ? -1 : atoi(argv[2]);
    if (p >= CW_INPUT_MAX_PLAYERS) {
      cw_reply_error(r, "prompt device: players are 0..%d or all", CW_INPUT_MAX_PLAYERS - 1);
      return;
    }
    cw_prompt_force_device(p, d);
  } else if (argc > 1) {
    cw_reply_error(r, "usage: prompt [device <player|all> <pad|keyboard|mouse|touch|auto>]");
    return;
  }
  cw_reply_printf(r, "{\"devices\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"errors\":%u,\"missing\":%u}", DEVICE_NAMES[device_of(0)],
                  DEVICE_NAMES[device_of(1)], DEVICE_NAMES[device_of(2)], DEVICE_NAMES[device_of(3)], P.errors, P.missing);
}

void cw__prompt_input_init(void) {
  static bool registered;
  if (!registered) {
    registered = true;
    cw_debug_command("prompt", "prompt [device <player|all> <pad|keyboard|mouse|touch|auto>]: each player's prompt device, "
                     "or force one (auto ends the forcing)", cmd_prompt);
  }
  const char *e = getenv("CW_TEST_PROMPT_DEVICE");
  cw_prompt_device d = e && *e ? device_by_name(e) : CW_PROMPT_NONE;
  if ((int)d < 0) {
    cw_error("CW_TEST_PROMPT_DEVICE=%s: use pad, keyboard, mouse or touch", e);
    d = CW_PROMPT_NONE;
  }
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++) I.prompt_force[p] = (uint8_t)d;
  const char *b = getenv("CW_TEST_PROMPT_BREAK");
  P.break_wrong_dot = b && strstr(b, "wrong_dot");
  P.break_ignore_force = b && strstr(b, "ignore_force");
  P.break_stick_first = b && strstr(b, "stick_first");
}

/* ── which binding ───────────────────────────────────────────────────── */

static bool on_device(cw_binding b, cw_prompt_device d) {
  switch ((cw_binding_kind)b.kind) {
  case CW_BIND_KEY: return d == CW_PROMPT_KEYBOARD;
  case CW_BIND_MOUSE:
  case CW_BIND_WHEEL: return d == CW_PROMPT_MOUSE;
  case CW_BIND_PAD_BUTTON:
  case CW_BIND_PAD_AXIS: return d == CW_PROMPT_PAD;
  case CW_BIND_TOUCH_BUTTON:
  case CW_BIND_TOUCH_STICK: return d == CW_PROMPT_TOUCH;
  default: return false;
  }
}

/* the first binding of the action on the device; the keyboard and mouse stand in for each other */
static bool binding_for(cw_action a, cw_prompt_device d, cw_binding *out) {
  cw_binding bs[CW_INPUT_MAX_BINDINGS];
  int n = cw_action_bindings(a, bs, CW_INPUT_MAX_BINDINGS);
  for (int i = 0; i < n; i++)
    if (on_device(bs[i], d)) return *out = bs[i], true;
  cw_prompt_device alt = d == CW_PROMPT_KEYBOARD ? CW_PROMPT_MOUSE : d == CW_PROMPT_MOUSE ? CW_PROMPT_KEYBOARD : CW_PROMPT_NONE;
  for (int i = 0; alt && i < n; i++)
    if (on_device(bs[i], alt)) return *out = bs[i], true;
  return false;
}

/* shared prompts hold back a device's missing binding until they know whether any device drew something */
static struct { bool on; int n; cw_action a[8]; uint8_t d[8]; } held;

static void missing(cw_action a, cw_prompt_device d) {
  if (held.on) {
    if (held.n < 8) held.a[held.n] = a, held.d[held.n++] = (uint8_t)d;
    return;
  }
  const char *name = cw_action_name(a);
  const char *msg = cw_frame_printf("prompt: action '%s' has no binding on the %s: nothing is drawn for it (bind one, "
                                    "e.g. cw_action_bind_text(a, \"%s\"))", name ? name : "?", DEVICE_NAMES[d],
                                    d == CW_PROMPT_PAD ? "pad:south" : d == CW_PROMPT_TOUCH ? "touch:0" : "key:space");
  if (!once(msg)) return;
  cw_warn("%s", msg);
  P.missing++;
}

/* ── glyphs from bindings ───────────────────────────────────────────── */

static uint32_t dir_bit_axis(int axis, int dir) {
  bool x = axis == CW_AXIS_LX || axis == CW_AXIS_RX;
  return x ? (dir < 0 ? DIR_L : DIR_R) : (dir < 0 ? DIR_U : DIR_D);
}

static glyph glyph_of(cw_binding b) {
  glyph g = { 0 };
  switch ((cw_binding_kind)b.kind) {
  case CW_BIND_PAD_BUTTON:
    switch ((cw_pad_button)b.code) {
    case CW_PAD_SOUTH: g.kind = PK_FACE, g.lit = 1; break;
    case CW_PAD_EAST: g.kind = PK_FACE, g.lit = 2; break;
    case CW_PAD_WEST: g.kind = PK_FACE, g.lit = 4; break;
    case CW_PAD_NORTH: g.kind = PK_FACE, g.lit = 8; break;
    case CW_PAD_BACK: g.kind = PK_BACK; break;
    case CW_PAD_GUIDE: g.kind = PK_GUIDE; break;
    case CW_PAD_START: g.kind = PK_START; break;
    case CW_PAD_LSTICK: g.kind = PK_STICK, g.lit = 0x100; break;
    case CW_PAD_RSTICK: g.kind = PK_STICK, g.lit = 0x101; break;
    case CW_PAD_LSHOULDER: g.kind = PK_SHOULDER, g.lit = 1; break;
    case CW_PAD_RSHOULDER: g.kind = PK_SHOULDER, g.lit = 2; break;
    case CW_PAD_UP: g.kind = PK_DPAD, g.lit = DIR_U; break;
    case CW_PAD_DOWN: g.kind = PK_DPAD, g.lit = DIR_D; break;
    case CW_PAD_LEFT: g.kind = PK_DPAD, g.lit = DIR_L; break;
    case CW_PAD_RIGHT: g.kind = PK_DPAD, g.lit = DIR_R; break;
    default: break;
    }
    break;
  case CW_BIND_PAD_AXIS:
    if (b.code == CW_AXIS_LT || b.code == CW_AXIS_RT) g.kind = PK_TRIGGER, g.lit = b.code == CW_AXIS_LT ? 1 : 2;
    else if (b.code < CW_AXIS_LT) g.kind = PK_STICK, g.lit = (b.code >= CW_AXIS_RX ? 1u : 0u) | dir_bit_axis(b.code, b.dir) << 4;
    break;
  case CW_BIND_KEY:
    g.kind = PK_KEY, g.lit = b.code;
    if (b.code < SC_RIGHT || b.code > SC_UP) cw_binding_display_name(b, g.label, sizeof g.label);
    break;
  case CW_BIND_MOUSE: g.kind = PK_MOUSE, g.lit = 1u << (b.code > 2 ? 2 : b.code); break;
  case CW_BIND_WHEEL: g.kind = PK_MOUSE, g.lit = (b.code ? (b.dir < 0 ? DIR_L : DIR_R) : (b.dir < 0 ? DIR_D : DIR_U)) << 4; break;
  case CW_BIND_TOUCH_BUTTON:
  case CW_BIND_TOUCH_STICK: {
    bool stick = b.kind == CW_BIND_TOUCH_STICK;
    g.kind = stick ? PK_TOUCH_STICK : PK_TOUCH_BUTTON, g.lit = b.code;
    if (stick) g.lit |= (uint32_t)(1u << (b.dir & 3)) << 8;
    if (b.code < I.ncontrols) {
      snprintf(g.label, sizeof g.label, "%s", I.controls[b.code].label);
      g.tint = I.controls[b.code].color;
    }
    if (g.tint.a == 0) g.tint = cw_rgba(1, 1, 1, 1);
    break;
  }
  default: break;
  }
  return g;
}

uint32_t cw_prompt_glyph_code(cw_binding b) {
  glyph g = glyph_of(b);
  return g.kind ? (uint32_t)g.kind << 16 | g.lit : 0;
}

/* ── movement: several actions as one glyph ─────────────────────────── */

static bool same_glyph(const glyph *a, const glyph *b) { return a->kind == b->kind && a->lit == b->lit && !strcmp(a->label, b->label); }
static void set_add(gset *s, glyph g) {
  if (!g.kind || s->n >= MAX_SET) return;
  for (int i = 0; i < s->n; i++)
    if (same_glyph(&s->g[i], &g)) return;
  s->g[s->n++] = g;
}

/* the stick (0 left, 1 right) every action has a binding on, -1 none */
static int common_stick(const cw_action *acts, int n) {
  for (int st = 0; st < 2; st++) {
    bool all = true;
    for (int i = 0; i < n && all; i++) {
      cw_binding bs[CW_INPUT_MAX_BINDINGS];
      int k = cw_action_bindings(acts[i], bs, CW_INPUT_MAX_BINDINGS);
      bool has = false;
      for (int j = 0; j < k && !has; j++)
        has = bs[j].kind == CW_BIND_PAD_AXIS && bs[j].code < CW_AXIS_LT && (bs[j].code >= CW_AXIS_RX) == (st == 1);
      all = has;
    }
    if (all) return st;
  }
  return -1;
}
static bool all_dpad(const cw_action *acts, int n) {
  for (int i = 0; i < n; i++) {
    cw_binding bs[CW_INPUT_MAX_BINDINGS];
    int k = cw_action_bindings(acts[i], bs, CW_INPUT_MAX_BINDINGS);
    bool has = false;
    for (int j = 0; j < k && !has; j++) has = bs[j].kind == CW_BIND_PAD_BUTTON && bs[j].code >= CW_PAD_UP && bs[j].code <= CW_PAD_RIGHT;
    if (!has) return false;
  }
  return true;
}
static bool is_stick_dir(cw_binding b) { return b.kind == CW_BIND_PAD_AXIS && b.code < CW_AXIS_LT; }
static bool is_dpad_arm(cw_binding b) { return b.kind == CW_BIND_PAD_BUTTON && b.code >= CW_PAD_UP && b.code <= CW_PAD_RIGHT; }
/* a movement action (its first pad binding a stick direction or a d-pad arm) on both a stick and a d-pad arm draws
 * that arm, whichever comes first: {lean_fwd} alone lights one arm */
static bool dpad_over_stick(cw_action a, cw_binding *out) {
  cw_binding bs[CW_INPUT_MAX_BINDINGS], first;
  if (!binding_for(a, CW_PROMPT_PAD, &first) || !(is_stick_dir(first) || is_dpad_arm(first))) return false;
  int k = cw_action_bindings(a, bs, CW_INPUT_MAX_BINDINGS), dpad = -1;
  bool stick = false;
  for (int j = 0; j < k; j++) {
    if (is_stick_dir(bs[j])) stick = true;
    if (dpad < 0 && is_dpad_arm(bs[j])) dpad = j;
  }
  if (!stick || dpad < 0) return false;
  return *out = bs[dpad], true;
}
static bool touch_stick_of(const cw_action *acts, int n, int *id) {
  int found = -1;
  for (int i = 0; i < n; i++) {
    cw_binding b;
    if (!binding_for(acts[i], CW_PROMPT_TOUCH, &b) || b.kind != CW_BIND_TOUCH_STICK) return false;
    if (found >= 0 && b.code != found) return false;
    found = b.code;
  }
  *id = found;
  return found >= 0;
}

/* the movement bits of the action at index i of a set of n: a pair is left, right (or up, down); four are l, r, u, d */
static uint32_t move_bit(const cw_action *acts, int n, int i) {
  if (n == 4) return 1u << i;
  /* a pair: which axis, from the first action's stick or d-pad binding */
  cw_binding bs[CW_INPUT_MAX_BINDINGS];
  int k = cw_action_bindings(acts[0], bs, CW_INPUT_MAX_BINDINGS);
  bool vertical = false;
  for (int j = 0; j < k; j++) {
    if (bs[j].kind == CW_BIND_PAD_AXIS && bs[j].code < CW_AXIS_LT) { vertical = bs[j].code == CW_AXIS_LY || bs[j].code == CW_AXIS_RY; break; }
    if (bs[j].kind == CW_BIND_PAD_BUTTON && (bs[j].code == CW_PAD_UP || bs[j].code == CW_PAD_DOWN)) { vertical = true; break; }
    if (bs[j].kind == CW_BIND_PAD_BUTTON && (bs[j].code == CW_PAD_LEFT || bs[j].code == CW_PAD_RIGHT)) break;
  }
  return vertical ? (i ? DIR_D : DIR_U) : (i ? DIR_R : DIR_L);
}

/* the glyphs of n actions on one device, added to s */
static void device_glyphs(const cw_action *acts, int n, cw_prompt_device d, gset *s) {
  if (n >= 2 && d == CW_PROMPT_PAD) {
    /* the d-pad whenever every action is on it, even when a stick is bound too: the stick only for stick-only actions */
    int st = common_stick(acts, n);
    bool dp = all_dpad(acts, n) && !(P.break_stick_first && st >= 0); /* the control: the stick when both */
    if (dp) st = -1;
    if (st >= 0 || dp) {
      glyph g = { .kind = dp ? PK_DPAD : PK_STICK };
      for (int i = 0; i < n; i++) g.lit |= dp ? move_bit(acts, n, i) : move_bit(acts, n, i) << 4;
      if (st == 1) g.lit |= 1;
      set_add(s, g);
      return;
    }
  }
  int tid;
  if (n >= 2 && d == CW_PROMPT_TOUCH && touch_stick_of(acts, n, &tid)) {
    glyph g = { .kind = PK_TOUCH_STICK, .lit = (uint32_t)tid };
    for (int i = 0; i < n; i++) g.lit |= move_bit(acts, n, i) << 8;
    set_add(s, g);
    return;
  }
  /* each action's own glyph; four directions on keys read as the familiar cluster: up, left, down, right */
  static const int WASD[4] = { 2, 0, 3, 1 };
  bool keys = n == 4 && (d == CW_PROMPT_KEYBOARD || d == CW_PROMPT_MOUSE);
  for (int j = 0; j < n; j++) {
    int i = keys ? WASD[j] : j;
    cw_binding b;
    if (d == CW_PROMPT_PAD && !P.break_stick_first && dpad_over_stick(acts[i], &b)) set_add(s, glyph_of(b));
    else if (binding_for(acts[i], d, &b)) set_add(s, glyph_of(b));
    else missing(acts[i], d);
  }
}

/* the glyph set of n actions for a player (-1: every present seat) */
static void action_set(const cw_action *acts, int n, int player, gset *s) {
  s->n = 0;
  for (int i = 0; i < n; i++)
    if (cw_action_name(acts[i]) == NULL) return;
  if (player >= 0) {
    cw_prompt_device d = device_of(player);
    if (d == CW_PROMPT_NONE) {
      for (int i = 0; i < n; i++) missing(acts[i], d);
      return;
    }
    device_glyphs(acts, n, d, s);
    return;
  }
  bool pad = false, desk = false, touch = false;
  cw_prompt_device desk_dev = CW_PROMPT_KEYBOARD;
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++) {
    if (!present(p)) continue;
    cw_prompt_device d = device_of(p);
    if (d == CW_PROMPT_PAD) pad = true;
    else if (d == CW_PROMPT_KEYBOARD || d == CW_PROMPT_MOUSE) {
      if (!desk) desk_dev = d;
      desk = true;
    } else if (d == CW_PROMPT_TOUCH) touch = true;
  }
  /* a device with nothing for these actions is left out while another shows something (a menu's keys on a phone) */
  held.on = true, held.n = 0;
  if (pad) device_glyphs(acts, n, CW_PROMPT_PAD, s);
  if (desk) device_glyphs(acts, n, desk_dev, s);
  if (touch) device_glyphs(acts, n, CW_PROMPT_TOUCH, s);
  held.on = false;
  if (!s->n)
    for (int i = 0; i < held.n; i++) missing(held.a[i], (cw_prompt_device)held.d[i]);
}

static uint32_t utf8_next(const char **p);

/* ── drawing ─────────────────────────────────────────────────────────── */

static cw_font prompt_font(void) {
  if (!P.font.id) P.font = cw_ui_font_sans();
  return P.font;
}
void cw_prompt_set_font(cw_font f) { P.font = f; }

typedef struct pen {
  cw_color fill, dark;
  float s, w; /* size, stroke width */
} pen;

static pen pen_of(float size, cw_color c) {
  if (c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0) c = cw_rgba(1, 1, 1, 1);
  pen p = { .fill = c, .dark = cw_rgba(0.03f, 0.03f, 0.06f, 0.82f * c.a), .s = size };
  p.w = fmaxf(size * 0.075f, 1.25f);
  return p;
}

static float label_size(float s) { return s * 0.56f; }
/* one character on a keycap (J, W, 7) is drawn larger and heavier than a word: at the minimum size the thin legend of
 * a J read as an I in HUD text */
static bool one_char(const char *t) {
  const char *p = t;
  if (!*p) return false;
  utf8_next(&p);
  return !*p;
}
static cw_text_style key_style(const char *t, float s, cw_color c) {
  if (one_char(t)) return (cw_text_style){ .size = s * 0.66f, .color = c, .outline_width = s * 0.035f, .outline_color = c };
  return (cw_text_style){ .size = label_size(s), .color = c };
}
static float label_width(const char *t, float s) {
  if (!t[0]) return 0;
  cw_text_style st = { .size = label_size(s) };
  return cw__font_measure_raw(prompt_font(), t, &st).width;
}
static const char *start_word(void) {
  const char *t = cw_tr_find("prompt.start");
  return t ? t : "Start";
}

static float glyph_width(const glyph *g, float s) {
  switch (g->kind) {
  case PK_SHOULDER: return s * 1.4f;
  case PK_TRIGGER: return s * 1.15f;
  case PK_STICK: return s * 1.42f;
  case PK_START: return label_width(start_word(), s) + s * 0.75f;
  case PK_BACK: return s * 1.15f;
  case PK_KEY:
    if (!g->label[0]) return s * 0.92f;
    if (one_char(g->label)) {
      cw_text_style st = key_style(g->label, s, cw_rgba(1, 1, 1, 1));
      return fmaxf(s * 0.92f, cw__font_measure_raw(prompt_font(), g->label, &st).width + s * 0.4f);
    }
    return fmaxf(s * 0.92f, label_width(g->label, s) + s * 0.5f);
  case PK_MOUSE: return s * 0.72f;
  case PK_NONE: return 0;
  default: return s;
  }
}

static void ring(const pen *p, cw_vec2 c, float r, cw_color col) { cw_r2d_circle_lines(c, r, p->w, 0, col); }
static void dot(cw_vec2 c, float r, cw_color col) { cw_r2d_circle(c, r, 0, col); }
/* a solid or outlined rounded rect: the same outer edge either way */
static void box(const pen *p, cw_rect r, float rad, bool solid) {
  if (solid) cw_r2d_round_rect(r, rad, 0, p->fill);
  else cw_r2d_round_rect_lines(r, rad, p->w, 0, p->fill);
}
/* a filled triangle pointing along dir (unit), its tip at tip */
static void arrow(cw_vec2 tip, cw_vec2 dir, float len, float half, cw_color col) {
  cw_vec2 b = { tip.x - dir.x * len, tip.y - dir.y * len };
  cw_vec2 n = { -dir.y * half, dir.x * half };
  cw_vec2 pts[3] = { tip, { b.x + n.x, b.y + n.y }, { b.x - n.x, b.y - n.y } };
  cw_r2d_polygon(pts, 3, col);
}
static const cw_vec2 DIRS[4] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }; /* left, right, up, down */

static void draw_face(const pen *p, cw_vec2 o, uint32_t lit) {
  float s = p->s;
  cw_vec2 c = { o.x + s * 0.5f, o.y + s * 0.5f };
  dot(c, s * 0.5f, p->dark);
  float d = s * 0.29f, r = s * 0.115f;
  /* south, east, west, north */
  cw_vec2 at[4] = { { c.x, c.y + d }, { c.x + d, c.y }, { c.x - d, c.y }, { c.x, c.y - d } };
  for (int i = 0; i < 4; i++) {
    if (lit & (1u << i)) dot(at[i], r + p->w * 0.5f, p->fill);
    else ring(p, at[i], r, p->fill);
  }
}

static void draw_shoulder(const pen *p, cw_vec2 o, uint32_t lit, bool trigger) {
  float s = p->s, W = glyph_width(&(glyph){ .kind = trigger ? PK_TRIGGER : PK_SHOULDER }, s);
  if (trigger) {
    cw_r2d_round_rect(cw_rect_make(o.x, o.y, W, s), s * 0.22f, 0, p->dark);
    float tw = s * 0.34f, th = s * 0.7f, ty = o.y + s * 0.15f;
    box(p, cw_rect_make(o.x + s * 0.14f, ty, tw, th), tw * 0.5f, lit & 1);
    box(p, cw_rect_make(o.x + W - s * 0.14f - tw, ty, tw, th), tw * 0.5f, lit & 2);
  } else { /* the bumpers on top of a pad's outline */
    cw_r2d_round_rect(cw_rect_make(o.x, o.y + s * 0.06f, W, s * 0.88f), s * 0.22f, 0, p->dark);
    float bw = s * 0.48f, bh = s * 0.24f, by = o.y + s * 0.18f;
    box(p, cw_rect_make(o.x + s * 0.13f, by, bw, bh), bh * 0.5f, lit & 1);
    box(p, cw_rect_make(o.x + W - s * 0.13f - bw, by, bw, bh), bh * 0.5f, lit & 2);
    cw_color dim = { p->fill.r, p->fill.g, p->fill.b, p->fill.a * 0.6f };
    cw_r2d_round_rect_lines(cw_rect_make(o.x + s * 0.13f, o.y + s * 0.54f, W - s * 0.26f, s * 0.3f), s * 0.15f, fmaxf(p->w * 0.75f, 1.0f), 0, dim);
  }
}

static void ellipse(cw_vec2 c, float rx, float ry, cw_color col) {
  cw_vec2 pts[28];
  for (int i = 0; i < 28; i++) {
    float a = (float)i * 2.0f * CW_PI / 28.0f;
    pts[i] = (cw_vec2){ c.x + cosf(a) * rx, c.y + sinf(a) * ry };
  }
  cw_r2d_fan(c, pts, 28, true, col);
}

/* One thumbstick seen from the front and a little above: a flat socket, a short post and a round cap whose top face
 * shows. cx: its middle; sy: the socket's middle; s: its size; press: the cap pushed down; m grows every part (the dark
 * backing pass). */
static void stick_shape(float cx, float sy, float s, float m, bool press, cw_color c, cw_color socket, cw_color rim) {
  ellipse((cw_vec2){ cx, sy }, s * 0.44f + m, s * 0.12f + m, socket);
  float crx = s * 0.32f, cry = s * 0.1f, side = s * 0.12f, top = sy - s * (press ? 0.36f : 0.44f), pw = s * 0.2f;
  cw_r2d_rect(cw_rect_make(cx - pw * 0.5f - m, top, pw + 2 * m, sy - top), c);
  ellipse((cw_vec2){ cx, top + side }, crx + m, cry + m, c);
  cw_r2d_rect(cw_rect_make(cx - crx - m, top, 2 * (crx + m), side), c);
  ellipse((cw_vec2){ cx, top }, crx + m, cry + m, c);
  if (m == 0 && s >= 14) { /* the rim between the top face and the side */
    cw_vec2 arc[13];
    for (int i = 0; i <= 12; i++) {
      float a = (float)i * CW_PI / 12.0f;
      arc[i] = (cw_vec2){ cx + cosf(a) * crx * 0.98f, top + sinf(a) * cry };
    }
    cw_r2d_polyline(arc, 13, fmaxf(s * 0.05f, 1.0f), false, rim);
  }
}

/* A pad's thumbstick from the front, the other stick smaller and dimmer beside it (which one: the left stick has its
 * twin on its right). Arrows beside the cap say left and right, above it up, under the socket down; pressing in is an
 * arrow down onto a lowered cap. No ring round a dot anywhere: that read as an eye (two side arrows round a ring) or a
 * target. dark: the backing pass, every part grown by the margin. */
static void stick_parts(const pen *p, cw_vec2 o, float W, bool right, uint32_t dirs, bool press, bool dark) {
  float s = p->s, m = dark ? fmaxf(p->w * 0.8f, 1.0f) : 0;
  /* dim: the ink a little over half way to the backing, opaque (overlapping parts must not show through) */
  cw_color ink = dark ? p->dark : p->fill;
  cw_color dim = dark ? p->dark : cw_rgba(p->fill.r * 0.55f + 0.014f, p->fill.g * 0.55f + 0.014f, p->fill.b * 0.55f + 0.027f, p->fill.a);
  float cx = right ? o.x + W - s * 0.52f : o.x + s * 0.52f; /* the stick in use */
  float ox = right ? o.x + s * 0.2f : o.x + W - s * 0.2f;   /* the other one */
  float sy = o.y + s * 0.72f;
  cw_color rim = cw_rgba(p->dark.r, p->dark.g, p->dark.b, p->fill.a);
  stick_shape(ox, sy, s * 0.55f, m, false, dim, dim, rim);
  stick_shape(cx, sy, s, m, press, ink, dim, rim);
  float top = sy - s * (press ? 0.36f : 0.44f), len = s * 0.17f, half = s * 0.16f;
  cw_vec2 tips[4] = { { cx - s * 0.53f, top + s * 0.06f }, { cx + s * 0.53f, top + s * 0.06f }, { cx, o.y },
                      { cx, o.y + s * 1.02f } };
  for (int i = 0; i < 5; i++) {
    cw_vec2 tip, d;
    if (i < 4) {
      if (!(dirs & (1u << i))) continue;
      tip = tips[i], d = DIRS[i];
    } else {
      if (!press) continue;
      tip = (cw_vec2){ cx, top - s * 0.13f }, d = DIRS[3];
    }
    float l = d.y == 0 ? len * 1.12f : len, h = d.y == 0 ? half * 1.08f : half; /* the side arrows have the room */
    if (dark) arrow((cw_vec2){ tip.x + d.x * m * 1.6f, tip.y + d.y * m * 1.6f }, d, l + m * 2.6f, h + m * 1.6f, ink);
    else arrow(tip, d, l, h, ink);
  }
}

static void draw_stick(const pen *p, cw_vec2 o, uint32_t lit) {
  float W = glyph_width(&(glyph){ .kind = PK_STICK }, p->s);
  for (int pass = 0; pass < 2; pass++) stick_parts(p, o, W, lit & 1, (lit >> 4) & 15, lit & 0x100, pass == 0);
}

static void draw_dpad(const pen *p, cw_vec2 o, uint32_t lit) {
  float s = p->s, aw = s * 0.3f, inner = s * 0.1f, outer = s * 0.46f;
  cw_vec2 c = { o.x + s * 0.5f, o.y + s * 0.5f };
  float m = p->w;
  cw_r2d_round_rect(cw_rect_make(c.x - outer - m, c.y - aw * 0.5f - m, (outer + m) * 2, aw + 2 * m), s * 0.08f, 0, p->dark);
  cw_r2d_round_rect(cw_rect_make(c.x - aw * 0.5f - m, c.y - outer - m, aw + 2 * m, (outer + m) * 2), s * 0.08f, 0, p->dark);
  float len = outer - inner, rad = s * 0.05f;
  cw_rect arms[4] = { cw_rect_make(c.x - outer, c.y - aw * 0.5f, len, aw), cw_rect_make(c.x + inner, c.y - aw * 0.5f, len, aw),
                      cw_rect_make(c.x - aw * 0.5f, c.y - outer, aw, len), cw_rect_make(c.x - aw * 0.5f, c.y + inner, aw, len) };
  for (int i = 0; i < 4; i++) box(p, arms[i], rad, lit & (1u << i));
}

static void label_at(const pen *p, const char *t, cw_vec2 centre, float s) {
  cw_text_style st = { .size = label_size(s), .color = p->fill };
  cw_text_metrics m = cw__font_measure_raw(prompt_font(), t, &st);
  cw__r2d_text_raw(prompt_font(), t, (cw_vec2){ centre.x - m.width * 0.5f, centre.y - m.height * 0.5f }, &st, CW__TEXT_UNCOUNTED);
}

/* r grown by the pen's dark margin: an outline drawn inside r then has dark on both sides, on any background */
static cw_rect grow(const pen *p, cw_rect r) {
  float m = fmaxf(p->w * 0.8f, 1.0f);
  return cw_rect_make(r.x - m, r.y - m, r.w + 2 * m, r.h + 2 * m);
}

static void draw_start(const pen *p, cw_vec2 o) {
  float s = p->s, W = glyph_width(&(glyph){ .kind = PK_START }, s);
  cw_rect r = cw_rect_make(o.x + s * 0.04f, o.y + s * 0.16f, W - s * 0.08f, s * 0.68f);
  cw_r2d_round_rect(grow(p, r), r.h * 0.5f + 1, 0, p->dark);
  cw_r2d_round_rect_lines(r, r.h * 0.5f, p->w, 0, p->fill);
  label_at(p, start_word(), (cw_vec2){ o.x + W * 0.5f, o.y + s * 0.5f }, s);
}

static void draw_back(const pen *p, cw_vec2 o) {
  float s = p->s, W = s * 1.15f;
  cw_r2d_round_rect(cw_rect_make(o.x, o.y + s * 0.22f, W, s * 0.56f), s * 0.28f, 0, p->dark);
  float pw = s * 0.36f, ph = s * 0.24f, py = o.y + s * 0.38f;
  box(p, cw_rect_make(o.x + s * 0.15f, py, pw, ph), ph * 0.5f, true);
  box(p, cw_rect_make(o.x + W - s * 0.15f - pw, py, pw, ph), ph * 0.5f, false);
}

static void draw_guide(const pen *p, cw_vec2 o) {
  float s = p->s;
  cw_vec2 c = { o.x + s * 0.5f, o.y + s * 0.5f };
  dot(c, s * 0.5f, p->dark);
  dot(c, s * 0.34f, p->fill);
}

static void draw_key(const pen *p, cw_vec2 o, const glyph *g) {
  float s = p->s, W = glyph_width(g, s);
  cw_rect r = cw_rect_make(o.x + s * 0.04f, o.y + s * 0.08f, W - s * 0.08f, s * 0.84f);
  cw_r2d_round_rect(grow(p, r), s * 0.16f + 1, 0, p->dark);
  cw_r2d_round_rect_lines(r, s * 0.16f, p->w, 0, p->fill);
  cw_vec2 c = { o.x + W * 0.5f, o.y + s * 0.5f };
  if (g->label[0]) {
    cw_text_style st = key_style(g->label, s, p->fill);
    cw_text_metrics m = cw__font_measure_raw(prompt_font(), g->label, &st);
    if (one_char(g->label)) {
      /* centred on its capital height, a letter that hangs below the line (J, Q) lifted by half its tail;
       * an outline of its own colour makes the stroke heavier */
      float y = c.y + st.size * 0.35f - m.ascent - (strchr("JQgjpqy", g->label[0]) ? st.size * 0.08f : 0);
      cw__r2d_text_raw(prompt_font(), g->label, (cw_vec2){ c.x - m.width * 0.5f, y }, &st, CW__TEXT_UNCOUNTED);
    } else cw__r2d_text_raw(prompt_font(), g->label, (cw_vec2){ c.x - m.width * 0.5f, c.y - m.height * 0.5f }, &st, CW__TEXT_UNCOUNTED);
  } else { /* an arrow key */
    int i = g->lit == SC_LEFT ? 0 : g->lit == SC_RIGHT ? 1 : g->lit == SC_UP ? 2 : 3;
    cw_vec2 tip = { c.x + DIRS[i].x * s * 0.2f, c.y + DIRS[i].y * s * 0.2f };
    arrow(tip, DIRS[i], s * 0.36f, s * 0.2f, p->fill);
  }
}

static void draw_mouse(const pen *p, cw_vec2 o, uint32_t lit) {
  float s = p->s, bw = s * 0.6f, bx = o.x + s * 0.06f, by = o.y + s * 0.02f, bh = s * 0.96f;
  bx += s * 0.03f, by += s * 0.04f, bw -= s * 0.06f, bh -= s * 0.08f;
  cw_rect body = cw_rect_make(bx, by, bw, bh);
  cw_r2d_round_rect(grow(p, body), bw * 0.5f + 1, 0, p->dark);
  float split = by + s * 0.42f, cx = bx + bw * 0.5f, w = p->w;
  /* the buttons: the top of the body, left and right of the middle */
  if (lit & 1) {
    cw_r2d_arc((cw_vec2){ cx, by + bw * 0.5f }, 0, bw * 0.5f, CW_PI, CW_PI * 1.5f, 0, p->fill);
    cw_r2d_rect(cw_rect_make(bx, by + bw * 0.5f - 0.5f, bw * 0.5f, split - (by + bw * 0.5f) + 0.5f), p->fill);
  }
  if (lit & 2) {
    cw_r2d_arc((cw_vec2){ cx, by + bw * 0.5f }, 0, bw * 0.5f, CW_PI * 1.5f, CW_PI * 2.0f, 0, p->fill);
    cw_r2d_rect(cw_rect_make(cx, by + bw * 0.5f - 0.5f, bw * 0.5f, split - (by + bw * 0.5f) + 0.5f), p->fill);
  }
  cw_r2d_round_rect_lines(body, bw * 0.5f, w, 0, p->fill);
  cw_r2d_line((cw_vec2){ bx, split }, (cw_vec2){ bx + bw, split }, w, p->fill);
  cw_r2d_line((cw_vec2){ cx, by }, (cw_vec2){ cx, split }, w, p->fill);
  if (lit & (4 | 0xf0)) { /* the wheel */
    cw_rect wr = cw_rect_make(cx - s * 0.07f, by + s * 0.12f, s * 0.14f, s * 0.22f);
    cw_r2d_round_rect(wr, s * 0.07f, 0, p->dark);
    cw_r2d_round_rect(wr, s * 0.07f, 0, p->fill);
  }
  for (int i = 0; i < 4; i++) {
    if (!(lit & (16u << i))) continue;
    cw_vec2 c = { cx, by + bh * 0.72f };
    cw_vec2 tip = { c.x + DIRS[i].x * s * 0.14f, c.y + DIRS[i].y * s * 0.14f };
    arrow(tip, DIRS[i], s * 0.2f, s * 0.13f, p->fill);
  }
}

static void draw_touch_button(const pen *p, cw_vec2 o, const glyph *g) {
  float s = p->s;
  cw_vec2 c = { o.x + s * 0.5f, o.y + s * 0.5f };
  dot(c, s * 0.5f, p->dark);
  dot(c, s * 0.44f, cw_rgba(g->tint.r, g->tint.g, g->tint.b, 0.45f * p->fill.a));
  ring(p, c, s * 0.5f - p->w * 0.5f, p->fill);
  if (g->label[0]) {
    cw_text_style st = { .size = s * 0.36f, .color = p->fill, .outline_width = s * 0.03f, .outline_color = p->dark };
    cw_text_metrics m = cw__font_measure_raw(prompt_font(), g->label, &st);
    float sz = m.width > s * 0.66f ? st.size * s * 0.66f / m.width : st.size; /* a long label shrinks to fit the disc */
    st.size = sz;
    m = cw__font_measure_raw(prompt_font(), g->label, &st);
    cw__r2d_text_raw(prompt_font(), g->label, (cw_vec2){ c.x - m.width * 0.5f, c.y - m.height * 0.5f }, &st, CW__TEXT_UNCOUNTED);
  }
}

/* the touch stick as the overlay shows it, a knob on a pale base: the base is filled, not a ring, and the arrows sit
 * inside it, so it reads as neither an eye nor a target */
static void draw_touch_stick(const pen *p, cw_vec2 o, uint32_t lit) {
  float s = p->s;
  cw_vec2 c = { o.x + s * 0.5f, o.y + s * 0.5f };
  uint32_t dirs = (lit >> 8) & 15;
  dot(c, s * 0.5f, p->dark);
  cw_color base = cw_rgba(p->fill.r * 0.5f + 0.02f, p->fill.g * 0.5f + 0.02f, p->fill.b * 0.5f + 0.03f, p->fill.a);
  dot(c, s * 0.5f - fmaxf(p->w * 0.6f, 1.0f), base);
  int nd = 0, one = 0;
  for (int i = 0; i < 4; i++)
    if (dirs & (1u << i)) nd++, one = i;
  cw_vec2 k = nd == 1 ? (cw_vec2){ c.x + DIRS[one].x * s * 0.07f, c.y + DIRS[one].y * s * 0.07f } : c;
  dot(k, s * 0.17f + fmaxf(p->w * 0.5f, 1.0f), p->dark);
  dot(k, s * 0.17f, p->fill);
  for (int i = 0; i < 4; i++) {
    if (!(dirs & (1u << i))) continue;
    cw_vec2 tip = { c.x + DIRS[i].x * s * 0.46f, c.y + DIRS[i].y * s * 0.46f };
    arrow(tip, DIRS[i], s * 0.19f, s * 0.16f, p->fill);
  }
}

static void record(const glyph *g, uint32_t lit) {
  if (I.prompt_count < CW_ARRAY_LEN(I.prompt_codes)) I.prompt_codes[I.prompt_count] = (uint32_t)g->kind << 16 | lit;
  I.prompt_count++;
}

static void draw_glyph(const glyph *g, cw_vec2 o, float s, cw_color color) {
  pen p = pen_of(s, color);
  uint32_t lit = g->lit;
  if (g->kind == PK_FACE && P.break_wrong_dot) lit = ((lit << 1) | (lit >> 3)) & 15; /* a gate's control: the wrong dot */
  switch (g->kind) {
  case PK_FACE: draw_face(&p, o, lit); break;
  case PK_SHOULDER: draw_shoulder(&p, o, lit, false); break;
  case PK_TRIGGER: draw_shoulder(&p, o, lit, true); break;
  case PK_STICK: draw_stick(&p, o, lit); break;
  case PK_DPAD: draw_dpad(&p, o, lit); break;
  case PK_START: draw_start(&p, o); break;
  case PK_BACK: draw_back(&p, o); break;
  case PK_GUIDE: draw_guide(&p, o); break;
  case PK_KEY: draw_key(&p, o, g); break;
  case PK_MOUSE: draw_mouse(&p, o, lit); break;
  case PK_TOUCH_BUTTON: draw_touch_button(&p, o, g); break;
  case PK_TOUCH_STICK: draw_touch_stick(&p, o, lit); break;
  default: return;
  }
  record(g, lit);
}

static float set_gap(float s) { return s * 0.22f; }
static cw_vec2 set_size(const gset *s, float size) {
  float w = 0;
  for (int i = 0; i < s->n; i++) w += glyph_width(&s->g[i], size) + (i ? set_gap(size) : 0);
  return s->n ? (cw_vec2){ w, size } : (cw_vec2){ 0, 0 };
}
static cw_vec2 set_draw(const gset *s, cw_vec2 pos, float size, cw_color color) {
  float x = pos.x;
  for (int i = 0; i < s->n; i++) {
    draw_glyph(&s->g[i], (cw_vec2){ x, pos.y }, size, color);
    x += glyph_width(&s->g[i], size) + set_gap(size);
  }
  return set_size(s, size);
}

/* ── the public calls ────────────────────────────────────────────────── */

/* the player's text scale (cartwheel/render2d.h) applies once, here: glyphs grow with the text they sit in */
static float scaled(cw_font f, float size) { return size * cw__text_scale_for(f.id ? f : prompt_font(), size); }

cw_vec2 cw_prompt_actions_size(const cw_action *acts, int n, int player, float size) {
  check_context();
  size = scaled((cw_font){ 0 }, size);
  if (!acts || (n != 1 && n != 2 && n != 4)) return (cw_vec2){ 0, 0 };
  gset s;
  action_set(acts, n, player, &s);
  return set_size(&s, size);
}
cw_vec2 cw_prompt_actions(const cw_action *acts, int n, int player, cw_vec2 pos, float size, cw_color color) {
  check_context();
  if (!acts || (n != 1 && n != 2 && n != 4)) {
    prompt_error(cw_frame_printf("prompt: cw_prompt_actions takes 1, 2 or 4 actions, not %d", n));
    return (cw_vec2){ 0, 0 };
  }
  size = scaled((cw_font){ 0 }, size);
  gset s;
  action_set(acts, n, player, &s);
  return set_draw(&s, pos, size, color);
}
int cw_prompt_action_codes(const cw_action *acts, int n, int player, uint32_t *out, int cap) {
  check_context();
  if (!acts || (n != 1 && n != 2 && n != 4)) return 0;
  gset s;
  action_set(acts, n, player, &s);
  for (int i = 0; i < s.n && i < cap; i++) out[i] = (uint32_t)s.g[i].kind << 16 | s.g[i].lit;
  return s.n;
}
cw_vec2 cw_prompt_action(cw_action a, int player, cw_vec2 pos, float size, cw_color color) {
  return cw_prompt_actions(&a, 1, player, pos, size, color);
}
cw_vec2 cw_prompt_action_size(cw_action a, int player, float size) { return cw_prompt_actions_size(&a, 1, player, size); }

cw_vec2 cw_prompt_binding_size(cw_binding b, float size) {
  check_context();
  size = scaled((cw_font){ 0 }, size);
  glyph g = glyph_of(b);
  return g.kind ? (cw_vec2){ glyph_width(&g, size), size } : (cw_vec2){ 0, 0 };
}
cw_vec2 cw_prompt_binding(cw_binding b, cw_vec2 pos, float size, cw_color color) {
  check_context();
  glyph g = glyph_of(b);
  if (!g.kind) return (cw_vec2){ 0, 0 };
  size = scaled((cw_font){ 0 }, size);
  draw_glyph(&g, pos, size, color);
  return (cw_vec2){ glyph_width(&g, size), size };
}

/* ── text markup ─────────────────────────────────────────────────────── */

typedef struct item {
  const char *text; /* a run of text (frame arena), or NULL for a glyph set */
  const char *core; /* the run without its edge spaces */
  float lead, trail; /* the advance of its leading and trailing spaces */
  gset set;
  float w;
} item;
enum { MAX_ITEMS = 32 };

static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t c = s[0];
  int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  if (n == 2 && s[1]) c = (c & 0x1F) << 6 | (s[1] & 0x3F);
  else if (n == 3 && s[1] && s[2]) c = (c & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F);
  else if (n == 4 && s[1] && s[2] && s[3]) c = (c & 0x07) << 18 | (s[1] & 0x3F) << 12 | (s[2] & 0x3F) << 6 | (s[3] & 0x3F);
  else n = 1;
  *p += n;
  return c;
}
static bool is_rtl(uint32_t c) {
  return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) || c == 0x200F;
}
static bool is_strong_ltr(uint32_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x0590 && c != 0xD7 && c != 0xF7) ||
         (c >= 0x0900 && c < 0x2000) || (c >= 0x3040 && c < 0xFB1D) || c == 0x200E;
}
/* the line's direction: its first strong character outside the markup */
static bool line_rtl(const char *s, const char *end) {
  for (const char *p = s; p < end && *p;) {
    if (*p == '{' && p[1] != '{') {
      const char *e = memchr(p, '}', (size_t)(end - p));
      if (e) { p = e + 1; continue; }
    }
    uint32_t c = utf8_next(&p);
    if (is_rtl(c)) return true;
    if (is_strong_ltr(c)) return false;
  }
  return false;
}

static void text_style(cw_text_style *st, float size, cw_color color, bool rtl) {
  if (color.r == 0 && color.g == 0 && color.b == 0 && color.a == 0) color = cw_rgba(1, 1, 1, 1);
  *st = (cw_text_style){ .size = size, .color = color, .outline_width = size * 0.07f,
                         .outline_color = cw_rgba(0.03f, 0.03f, 0.06f, 0.85f * color.a),
                         .direction = rtl ? CW_TEXT_DIR_RTL : CW_TEXT_DIR_LTR };
}

/* the glyph height in a line of text at this size */
static float glyph_size(float size) { return size * 1.0f; }

/* one {...}: its glyph set, or an error (false) */
static bool markup(const char *in, size_t len, const char *whole, int player, gset *s) {
  s->n = 0;
  char buf[160];
  if (len == 0 || len >= sizeof buf) {
    prompt_error(cw_frame_printf("prompt: empty or overlong {} in \"%s\" (an action name, a binding such as pad:west, or a|b)", whole));
    return false;
  }
  memcpy(buf, in, len);
  buf[len] = 0;
  if (strchr(buf, ':') && !strchr(buf, '|')) {
    cw_binding b;
    if (!cw_binding_parse(buf, &b)) {
      prompt_error(cw_frame_printf("prompt: {%s} in \"%s\" is not a binding (pad:west, key:space, axis:lx-, mouse:left)", buf, whole));
      return false;
    }
    set_add(s, glyph_of(b));
    return true;
  }
  cw_action acts[4];
  int n = 0;
  for (char *t = buf, *e; t; t = e ? e + 1 : NULL) {
    e = strchr(t, '|');
    if (e) *e = 0;
    while (*t == ' ') t++;
    size_t tl = strlen(t);
    while (tl && t[tl - 1] == ' ') t[--tl] = 0;
    if (n == 4) { n = 5; break; }
    cw_action a = cw_action_find(t);
    if (a < 0) {
      prompt_error(cw_frame_printf("prompt: {%.*s} in \"%s\": no action is named '%s' (a typo? cw_action_add names them)", (int)len, in,
                                   whole, t));
      return false;
    }
    acts[n++] = a;
  }
  if (n == 3 || n > 4) {
    prompt_error(cw_frame_printf("prompt: {%.*s} in \"%s\": give one action, a pair (left|right) or four (left|right|up|down)", (int)len, in, whole));
    return false;
  }
  action_set(acts, n, player, s);
  return true;
}

/* a run's advance: measuring leaves out trailing spaces, which separate it from the next glyph */
/* A run's advance, its spaces counted on their own: text layout drops a run's edge spaces, and in a right-to-left
 * line its trailing spaces sit on the left. *core is the run without them (frame arena). */
static float run_width(cw_font f, const char *run, const cw_text_style *st, float *lead, float *trail, const char **core) {
  size_t n = strlen(run), a = 0, b = 0;
  while (a < n && run[a] == ' ') a++;
  while (b < n - a && run[n - 1 - b] == ' ') b++;
  char *c = cw_frame_alloc(n - a - b + 1);
  memcpy(c, run + a, n - a - b);
  c[n - a - b] = 0;
  float sp = cw__font_advance_raw(f, ' ', st->size);
  *lead = (float)a * sp, *trail = (float)b * sp, *core = c;
  return *lead + *trail + (c[0] ? cw__font_measure_raw(f, c, st).width : 0);
}

/* the text gathered so far becomes an item */
static int push_run(cw_font f, const cw_text_style *st, item *it, int n, char **run, int *rl) {
  (*run)[*rl] = 0;
  it[n].text = *run;
  it[n].set.n = 0;
  it[n].w = run_width(f, *run, st, &it[n].lead, &it[n].trail, &it[n].core);
  *run += *rl + 1;
  *rl = 0;
  return n + 1;
}

/* lay out one line [s, end) into items; returns the count */
static int layout_line(cw_font f, const char *s, const char *end, const char *whole, int player, float size, bool rtl, item *it) {
  int n = 0;
  cw_text_style st;
  text_style(&st, size, cw_rgba(1, 1, 1, 1), rtl);
  char *run = cw_frame_alloc((size_t)(end - s) + 1);
  int rl = 0;
  float gs = glyph_size(size), pad = size * 0.08f;
  for (const char *p = s; p < end;) {
    bool flush = false;
    item g = { 0 };
    if (p[0] == '{' && p + 1 < end && p[1] == '{') { run[rl++] = '{'; p += 2; continue; }
    if (p[0] == '}' && p + 1 < end && p[1] == '}') { run[rl++] = '}'; p += 2; continue; }
    if (*p == '{') {
      const char *e = memchr(p, '}', (size_t)(end - p));
      if (!e) {
        prompt_error(cw_frame_printf("prompt: a { with no } in \"%s\" (write {{ for a brace)", whole));
        run[rl++] = *p++;
        continue;
      }
      if (markup(p + 1, (size_t)(e - p - 1), whole, player, &g.set) && g.set.n) {
        g.w = set_size(&g.set, gs).x + 2 * pad;
        flush = true;
      }
      p = e + 1;
    } else run[rl++] = *p++;
    if (flush && rl && n < MAX_ITEMS) n = push_run(f, &st, it, n, &run, &rl);
    if (flush && n < MAX_ITEMS) it[n++] = g;
  }
  if (rl && n < MAX_ITEMS) n = push_run(f, &st, it, n, &run, &rl);
  return n;
}

static cw_vec2 prompt_text(cw_font f, const char *text, int player, cw_vec2 pos, float size, cw_color color, cw_text_align align, bool draw) {
  check_context();
  if (!text) return (cw_vec2){ 0, 0 };
  if (!f.id) f = prompt_font();
  cw_text_style ref;
  text_style(&ref, size, color, false);
  cw_text_metrics lm = cw__font_measure_raw(f, "Ag", &ref);
  float lh = lm.line_height > 0 ? lm.line_height : size * 1.25f, gs = glyph_size(size), pad = size * 0.08f;
  float lineh = fmaxf(lh, gs), y = pos.y, maxw = 0;
  item *it = cw_frame_alloc(sizeof(item) * MAX_ITEMS);
  for (const char *s = text;;) {
    const char *nl = strchr(s, '\n'), *end = nl ? nl : s + strlen(s);
    bool rtl = line_rtl(s, end);
    int n = layout_line(f, s, end, text, player, size, rtl, it);
    float w = 0;
    for (int i = 0; i < n; i++) w += it[i].w;
    maxw = fmaxf(maxw, w);
    if (draw) {
      float x0 = align == CW_ALIGN_CENTER ? pos.x - w * 0.5f : align == CW_ALIGN_RIGHT ? pos.x - w : pos.x;
      float x = rtl ? x0 + w : x0;
      cw_text_style st;
      text_style(&st, size, color, rtl);
      for (int i = 0; i < n; i++) {
        float ix = rtl ? x - it[i].w : x;
        if (it[i].text && it[i].core[0]) /* the leading spaces come first in reading order: on the right in an RTL line */
          cw__r2d_text_raw(f, it[i].core, (cw_vec2){ ix + (rtl ? it[i].trail : it[i].lead), y + (lineh - lh) * 0.5f }, &st, 0);
        else set_draw(&it[i].set, (cw_vec2){ ix + pad, y + (lineh - gs) * 0.5f }, gs, color);
        x = rtl ? x - it[i].w : x + it[i].w;
      }
    }
    y += lineh;
    if (!nl) break;
    s = nl + 1;
  }
  return (cw_vec2){ maxw, y - pos.y };
}

cw_vec2 cw_prompt_text(cw_font f, const char *text, int player, cw_vec2 pos, float size, cw_color color, cw_text_align align) {
  return prompt_text(f, text, player, pos, scaled(f, size), color, align, true);
}
cw_vec2 cw_prompt_text_size(cw_font f, const char *text, int player, float size) {
  return prompt_text(f, text, player, (cw_vec2){ 0, 0 }, scaled(f, size), cw_rgba(1, 1, 1, 1), CW_ALIGN_LEFT, false);
}

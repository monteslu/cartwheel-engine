/* Native test hook: CW_INPUT=file drives pad 0, the keyboard, the mouse, the
 * wheel and touches from a script, so a native run can replay the same input
 * a cart host injects.
 *
 * One line per change, applying from that frame on:
 *   <frame> <buttons> <lx> <ly> <rx> <ry> <lt> <rt> [k=<sc>,<sc>...] [m=<x>,<y>,<buttons>]
 *           [w=<dx>,<dy>] [t<id>=<x>,<y> ...] [x=<hex>]
 * buttons is a cw_pad_button bitmask (decimal or 0x hex); axes are floats.
 * The optional fields (any order) are the WHOLE state from that frame on:
 *   k  SDL scancodes held (absent: none)
 *   m  mouse at x,y (logical px) with a button mask (absent: no mouse)
 *   w  wheel notches for that frame only (+y up)
 *   t  touch slot id (0..9) down at x,y (absent: lifted)
 *   x  UTF-8 text typed on that frame only, as hex bytes (CW_EVENT_TEXT)
 *   p  pad 1 (a second player): p=<buttons>,<lx>,<ly> (absent: pad 1 not connected);
 *      p2= and p3= the same for pads 2 and 3 (the third and fourth players)
 * Old 8-number lines mean what they always did. tools/run-cart.mjs writes
 * these from its JSON input scripts. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

typedef struct {
  uint64_t frame;
  cw_pad_state pad;
  uint8_t keys[CW_KEY_COUNT];
  bool mouse;
  float mx, my;
  uint32_t mbuttons;
  float wheel_x, wheel_y;
  cw_touch_state touches[CW_MAX_TOUCHES];
  char text[128]; /* typed text delivered on that frame only (x=) */
  cw_pad_state more[3]; /* p= p2= p3=: pads 1-3 (each connected when given) */
  int text_len;
} step;
static step *steps;
static int nsteps, cursor;
/* the state applied on the previous frame, for the press edges */
static uint32_t prev_pad[4], prev_mouse;
static uint8_t prev_keys[CW_KEY_COUNT];
static bool loaded, active;
static step current;

static float as_delivered(int axis, float v);

static void parse_extra(step *s, char *p) {
  for (;;) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n' || *p == '\r') return;
    char *end;
    if (p[0] == 'k' && p[1] == '=') {
      p += 2;
      for (;;) {
        long sc = strtol(p, &end, 0);
        if (end == p) break;
        if (sc >= 0 && sc < CW_KEY_COUNT) s->keys[sc] = 1;
        p = end;
        if (*p != ',') break;
        p++;
      }
    } else if (p[0] == 'm' && p[1] == '=') {
      p += 2;
      s->mouse = true;
      s->mx = strtof(p, &end), p = end + (*end == ',');
      s->my = strtof(p, &end), p = end + (*end == ',');
      s->mbuttons = (uint32_t)strtoul(p, &end, 0), p = end;
    } else if (p[0] == 'w' && p[1] == '=') {
      p += 2;
      s->wheel_x = strtof(p, &end), p = end + (*end == ',');
      s->wheel_y = strtof(p, &end), p = end;
    } else if (p[0] == 'x' && p[1] == '=') {
      p += 2;
      while (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) && s->text_len < (int)sizeof s->text - 1) {
        char hx[3] = { p[0], p[1], 0 };
        s->text[s->text_len++] = (char)strtol(hx, NULL, 16);
        p += 2;
      }
    } else if (p[0] == 'p' && (p[1] == '=' || ((p[1] == '2' || p[1] == '3') && p[2] == '='))) {
      cw_pad_state *pd = &s->more[p[1] == '=' ? 0 : p[1] - '1'];
      p += p[1] == '=' ? 2 : 3;
      pd->connected = true;
      pd->buttons = (uint32_t)strtoul(p, &end, 0), p = end + (*end == ',');
      pd->axes[CW_AXIS_LX] = as_delivered(CW_AXIS_LX, strtof(p, &end)), p = end + (*end == ',');
      pd->axes[CW_AXIS_LY] = as_delivered(CW_AXIS_LY, strtof(p, &end)), p = end;
    } else if (p[0] == 't') {
      long id = strtol(p + 1, &end, 10);
      if (end == p + 1 || *end != '=' || id < 0 || id >= CW_MAX_TOUCHES) {
        cw_error("CW_INPUT: bad touch field '%s' (want t<id>=x,y)", p);
        return;
      }
      p = end + 1;
      s->touches[id].active = true;
      s->touches[id].x = strtof(p, &end), p = end + (*end == ',');
      s->touches[id].y = strtof(p, &end), p = end;
    } else {
      cw_error("CW_INPUT: unknown field '%s' (k= m= w= t<id>= x= p= p2= p3=)", p);
      return;
    }
  }
}

/* Deliver an axis the way a cart host does, so a scripted native run reads
 * the same bits: hosts send Sint16 (sticks round(v*32767), triggers 0..32767);
 * wasmcart-sdl2 hands triggers to SDL's joystick layer as 2t-32768, and SDL's
 * game-controller mapping scales that back to 0..32767 in float, truncating. */
static float as_delivered(int axis, float v) {
  v = v < -1 ? -1 : v > 1 ? 1 : v;
  int q = (int)lroundf(v * 32767.0f);
  if (axis >= CW_AXIS_LT) {
    if (q < 0) q = 0;
    float normalized = (float)(q * 2) / 65535;
    q = (int)(normalized * 32767);
  }
  return (float)q / 32767.0f;
}

static void load(void) {
  loaded = true;
  const char *path = getenv("CW_INPUT");
  if (!path) return;
  FILE *f = fopen(path, "r");
  if (!f) { cw_error("CW_INPUT: cannot open %s", path); return; }
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    step s;
    memset(&s, 0, sizeof s);
    char *end;
    unsigned long long fr = strtoull(line, &end, 0);
    if (end == line) continue;
    s.frame = fr;
    s.pad.buttons = (uint32_t)strtoul(end, &end, 0);
    for (int a = 0; a < CW_AXIS_COUNT; a++) s.pad.axes[a] = as_delivered(a, strtof(end, &end));
    s.pad.connected = true;
    parse_extra(&s, end);
    steps = realloc(steps, sizeof *steps * (size_t)(nsteps + 1));
    steps[nsteps++] = s;
  }
  fclose(f);
  active = nsteps > 0;
  current.pad.connected = true;
}

/* a test run (CW_FRAMES) sees only scripted input, never the machine's own
 * devices: a gamepad plugged into the build box must not change a golden.
 * Pad 0 present and neutral, as cart test hosts report it. CW_TEST_PADS=1
 * (without an input script) lets the machine's game controllers through: the
 * test of the SDL pad path itself (tests/native-pads.mjs, virtual controllers). */
static void neutral(cw_raw_input *in) {
  memset(in->pads, 0, sizeof in->pads);
  in->pads[0].connected = true;
  memset(in->keys, 0, sizeof in->keys);
  memset(in->keys_pressed, 0, sizeof in->keys_pressed);
  in->mouse_buttons = in->mouse_pressed = 0;
  in->wheel_x = in->wheel_y = 0;
  memset(in->touches, 0, sizeof in->touches);
}

void cw__input_script_apply(cw_raw_input *in, uint64_t frame) {
  if (!loaded) load();
  if (!active) {
    const char *real = getenv("CW_TEST_PADS");
    if (getenv("CW_FRAMES") && !(real && real[0] == '1')) neutral(in);
    return;
  }
  bool wheel_now = false;
  while (cursor < nsteps && steps[cursor].frame <= frame) {
    wheel_now = steps[cursor].frame == frame;
    current = steps[cursor++];
  }
  neutral(in);
  in->pads[0] = current.pad;
  for (int i = 0; i < 3; i++)
    if (current.more[i].connected) in->pads[i + 1] = current.more[i];
  memcpy(in->keys, current.keys, sizeof in->keys);
  if (current.mouse) {
    in->mouse_present = true;
    in->mouse_x = current.mx;
    in->mouse_y = current.my;
    in->mouse_buttons = current.mbuttons;
  }
  /* the press edges SDL's events give a real run (sdl_common.c): a button,
   * key or mouse button down now and not on the previous frame */
  for (int i = 0; i < 4; i++) in->pads[i].pressed = in->pads[i].buttons & ~prev_pad[i];
  for (int k = 0; k < CW_KEY_COUNT; k++) in->keys_pressed[k] = in->keys[k] && !prev_keys[k];
  in->mouse_pressed = in->mouse_buttons & ~prev_mouse;
  for (int i = 0; i < 4; i++) prev_pad[i] = in->pads[i].buttons;
  memcpy(prev_keys, in->keys, sizeof prev_keys);
  prev_mouse = in->mouse_buttons;
  if (wheel_now && current.text_len) cw__platform_queue_text(current.text, current.text_len);
  if (wheel_now) {
    in->wheel_x = current.wheel_x;
    in->wheel_y = current.wheel_y;
  }
  memcpy(in->touches, current.touches, sizeof in->touches);
}

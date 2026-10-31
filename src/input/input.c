/* Named actions over the platform's raw input: bindings, the frame and tick
 * snapshots, vectors, players, touch controls and rebinding capture.
 * Text bindings live in bindings.c, record/replay in replay.c. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "input_internal.h"
#include "../platform/hooks.h"

cw__input_state cw__in;
#define I cw__in

static bool debug_registered;

int16_t cw__input_quant(float v) {
  if (!(v > 0)) return 0;
  if (v >= 1) return 32767;
  return (int16_t)(v * 32767.0f + 0.5f);
}

static void reset_players(void) {
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++) {
    I.pad[p] = p;
    I.devices[p] = p == 0 ? (CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH) : 0;
  }
}

void cw__input_ensure_init(void) {
  if (I.init) return;
  memset(&I, 0, sizeof I);
  I.init = true;
  reset_players();
  I.tick = &I.tick_live;
  for (int i = 0; i < CW_MAX_TOUCHES; i++) I.touch_owner[i] = -1;
  if (!debug_registered) {
    debug_registered = true;
    cw_debug_u32("input.hash", &I.stream_hash);
    cw_debug_u32("input.ticks", &I.stream_ticks);
    cw__input_register_commands();
  }
  I.stream_hash = CW_HASH32_INIT;
  static const cw__input_hooks hooks = { cw_input_frame_begin, cw_input_tick_begin, cw_input_ticks_end };
  cw__platform_hook_input(&hooks);
}

void cw_input_reset(void) {
  cw__input_replay_free();
  I.init = false;
  cw__input_ensure_init();
}

void cw__input_changed(void) {
  if (I.changed_fn) I.changed_fn(I.changed_user);
}

/* ── actions ───────────────────────────────────────────────────────── */

static bool valid(cw_action a) { return I.init && a >= 0 && a < I.count; }

cw_action cw_action_add(const char *name, float deadzone) {
  cw__input_ensure_init();
  CW_ASSERT(name && *name, "give the action a name");
  cw_action a = cw_action_find(name);
  if (a < 0) {
    if (I.count >= CW_INPUT_MAX_ACTIONS) {
      cw_error("input: more than %d actions; '%s' not added (raise CW_INPUT_MAX_ACTIONS)", CW_INPUT_MAX_ACTIONS, name);
      return -1;
    }
    a = I.count++;
    memset(&I.actions[a], 0, sizeof I.actions[a]);
    I.actions[a].name = cw_intern(name);
  }
  I.actions[a].deadzone = cw_clampf(deadzone, 0, 1);
  I.actions[a].dz_q = cw__input_quant(I.actions[a].deadzone);
  return a;
}

cw_action cw_action_find(const char *name) {
  if (!I.init || !name) return -1;
  const char *n = cw_intern(name);
  for (int i = 0; i < I.count; i++)
    if (I.actions[i].name == n) return i;
  return -1;
}
const char *cw_action_name(cw_action a) { return valid(a) ? I.actions[a].name : NULL; }
int cw_action_count(void) { return I.init ? I.count : 0; }
void cw_action_set_deadzone(cw_action a, float deadzone) {
  if (!valid(a)) return;
  I.actions[a].deadzone = cw_clampf(deadzone, 0, 1);
  I.actions[a].dz_q = cw__input_quant(I.actions[a].deadzone);
  cw__input_changed();
}
float cw_action_deadzone(cw_action a) { return valid(a) ? I.actions[a].deadzone : 0; }

static bool same_binding(cw_binding x, cw_binding y) {
  return x.kind == y.kind && x.dir == y.dir && x.code == y.code && x.threshold == y.threshold;
}

bool cw__action_bind_quiet(cw_action a, cw_binding b) {
  if (!valid(a) || b.kind == CW_BIND_NONE) return false;
  cw__action_def *d = &I.actions[a];
  for (int i = 0; i < d->nbind; i++)
    if (same_binding(d->bind[i], b)) return false;
  if (d->nbind >= CW_INPUT_MAX_BINDINGS) {
    cw_warn("input: action '%s' already has %d bindings; unbind one first", d->name, CW_INPUT_MAX_BINDINGS);
    return false;
  }
  d->bind[d->nbind++] = b;
  return true;
}

bool cw_action_bind(cw_action a, cw_binding b) {
  bool ok = cw__action_bind_quiet(a, b);
  if (ok) cw__input_changed();
  return ok;
}

bool cw_action_bind_text(cw_action a, const char *list) {
  if (!valid(a) || !list) return false;
  bool ok = true, any = false;
  char item[64];
  const char *p = list;
  while (*p) {
    const char *e = strchr(p, ',');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= sizeof item) n = sizeof item - 1;
    memcpy(item, p, n);
    item[n] = 0;
    cw_binding b;
    char *t = item;
    while (*t == ' ' || *t == '\t') t++;
    size_t tl = strlen(t);
    while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\t')) t[--tl] = 0;
    if (*t) {
      if (!strncmp(t, "deadzone:", 9)) { /* as in a bindings file: the action's dead zone */
        char *end;
        float dz = strtof(t + 9, &end);
        if (end == t + 9 || *end || dz < 0 || dz > 1) {
          cw_error("input: deadzone '%s' must be 0..1", t + 9);
          ok = false;
        } else cw_action_set_deadzone(a, dz);
      } else if (cw_binding_parse(t, &b)) any |= cw__action_bind_quiet(a, b);
      else {
        cw_error("input: '%s' is not a binding (key:space, pad:south, axis:lx-@0.2, mouse:left, touch:0, ...)", t);
        ok = false;
      }
    }
    if (!e) break;
    p = e + 1;
  }
  if (any) cw__input_changed();
  return ok;
}

bool cw_action_set_binding(cw_action a, int slot, cw_binding b) {
  if (!valid(a) || slot < 0 || slot > I.actions[a].nbind || slot >= CW_INPUT_MAX_BINDINGS) return false;
  cw__action_def *d = &I.actions[a];
  if (slot == d->nbind) d->nbind++;
  d->bind[slot] = b;
  cw__input_changed();
  return true;
}

bool cw_action_unbind(cw_action a, int slot) {
  if (!valid(a) || slot < 0 || slot >= I.actions[a].nbind) return false;
  cw__action_def *d = &I.actions[a];
  memmove(&d->bind[slot], &d->bind[slot + 1], sizeof d->bind[0] * (size_t)(d->nbind - slot - 1));
  d->nbind--;
  cw__input_changed();
  return true;
}

void cw_action_clear_bindings(cw_action a) {
  if (!valid(a)) return;
  I.actions[a].nbind = 0;
  cw__input_changed();
}

int cw_action_bindings(cw_action a, cw_binding *out, int cap) {
  if (!valid(a)) return 0;
  for (int i = 0; i < I.actions[a].nbind && i < cap; i++) out[i] = I.actions[a].bind[i];
  return I.actions[a].nbind;
}

void cw_input_on_bindings_changed(cw_bindings_changed_fn fn, void *user) {
  cw__input_ensure_init();
  I.changed_fn = fn;
  I.changed_user = user;
}

/* ── players ───────────────────────────────────────────────────────── */

void cw_input_assign(int player, int pad, uint32_t devices) {
  cw__input_ensure_init();
  CW_ASSERT(player >= 0 && player < CW_INPUT_MAX_PLAYERS, "players are 0..CW_INPUT_MAX_PLAYERS-1");
  I.pad[player] = pad >= 0 && pad < CW_MAX_PADS ? pad : -1;
  I.devices[player] = devices;
}
int cw_input_player_pad(int player) {
  return I.init && player >= 0 && player < CW_INPUT_MAX_PLAYERS ? I.pad[player] : -1;
}
uint32_t cw_input_player_devices(int player) {
  return I.init && player >= 0 && player < CW_INPUT_MAX_PLAYERS ? I.devices[player] : 0;
}

/* ── rumble ────────────────────────────────────────────────────────── */

uint32_t cw__rumble_req[5]; /* count, pad, low and high in thousandths, ms: the debug field cw.rumble */

static float rumble_unit(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; } /* clamps, and NaN is 0 */

bool cw_input_has_rumble(int player) { return cw__platform_pad_has_rumble(cw_input_player_pad(player)); }
void cw_input_rumble(int player, float low, float high, int ms) {
  int pad = cw_input_player_pad(player);
  low = rumble_unit(low), high = rumble_unit(high);
  ms = ms < 0 ? 0 : ms > 5000 ? 5000 : ms;
  cw__rumble_req[0]++, cw__rumble_req[1] = pad < 0 ? 0xffffffffu : (uint32_t)pad;
  cw__rumble_req[2] = (uint32_t)(low * 1000.0f + 0.5f), cw__rumble_req[3] = (uint32_t)(high * 1000.0f + 0.5f), cw__rumble_req[4] = (uint32_t)ms;
  cw__platform_pad_rumble(pad, low, high, ms);
}
void cw_input_rumble_stop(int player) { cw_input_rumble(player, 0, 0, 0); }

/* ── queries ───────────────────────────────────────────────────────── */

static const cw__slot *slot_of(int player, cw_action a) {
  if (!valid(a) || player < 0 || player >= CW_INPUT_MAX_PLAYERS) return NULL;
  const cw__snapshot *s = I.ctx == CW_INPUT_TICK ? I.tick : &I.frame;
  return &s->s[player][a];
}

bool cw_action_pressed(int player, cw_action a) {
  const cw__slot *s = slot_of(player, a);
  return s && (s->flags & CW__PRESSED);
}
bool cw_action_just_pressed(int player, cw_action a) {
  const cw__slot *s = slot_of(player, a);
  return s && (s->flags & CW__JUST_PRESSED);
}
bool cw_action_just_released(int player, cw_action a) {
  const cw__slot *s = slot_of(player, a);
  return s && (s->flags & CW__JUST_RELEASED);
}
float cw_action_raw_strength(int player, cw_action a) {
  const cw__slot *s = slot_of(player, a);
  return s ? (float)s->raw / 32767.0f : 0;
}
float cw_action_strength(int player, cw_action a) {
  const cw__slot *s = slot_of(player, a);
  if (!s || !(s->flags & CW__PRESSED)) return 0;
  int dz = I.actions[a].dz_q;
  if (dz >= 32767) return 1;
  int v = s->raw - dz;
  if (v <= 0) return 0;
  return (float)v / (float)(32767 - dz);
}
float cw_input_axis(int player, cw_action neg, cw_action pos) {
  return cw_action_strength(player, pos) - cw_action_strength(player, neg);
}

static float rescale(float v, float dz) { return dz >= 1 ? (v >= 1 ? 1.0f : 0.0f) : (v - dz) / (1 - dz); }

cw_vec2 cw_input_vector(int player, cw_action left, cw_action right, cw_action up, cw_action down,
                        cw_deadzone_mode mode, float dz) {
  cw_vec2 v = { cw_action_raw_strength(player, right) - cw_action_raw_strength(player, left),
                cw_action_raw_strength(player, down) - cw_action_raw_strength(player, up) };
  dz = cw_clampf(dz, 0, 1);
  float len = sqrtf(v.x * v.x + v.y * v.y);
  switch (mode) {
  case CW_DEADZONE_AXIAL: {
    float ax = fabsf(v.x), ay = fabsf(v.y);
    v.x = ax < dz || ax == 0 ? 0 : copysignf(rescale(ax > 1 ? 1 : ax, dz), v.x);
    v.y = ay < dz || ay == 0 ? 0 : copysignf(rescale(ay > 1 ? 1 : ay, dz), v.y);
    len = sqrtf(v.x * v.x + v.y * v.y);
    break;
  }
  case CW_DEADZONE_RADIAL:
    if (len < dz || len == 0) return (cw_vec2){ 0, 0 };
    break;
  case CW_DEADZONE_SCALED_RADIAL: {
    if (len < dz || len == 0) return (cw_vec2){ 0, 0 };
    float m = rescale(len > 1 ? 1 : len, dz);
    v.x = v.x / len * m;
    v.y = v.y / len * m;
    len = m;
    break;
  }
  }
  if (len > 1) {
    v.x /= len;
    v.y /= len;
  }
  return v;
}

cw_vec2 cw_input_vector_named(int player, const char *left, const char *right, const char *up, const char *down,
                              cw_deadzone_mode mode, float dz) {
  return cw_input_vector(player, cw_action_find(left), cw_action_find(right), cw_action_find(up), cw_action_find(down),
                         mode, dz);
}

cw_input_context cw_input_set_context(cw_input_context c) {
  cw_input_context old = I.ctx;
  I.ctx = c;
  return old;
}
cw_input_context cw_input_context_now(void) { return I.ctx; }

/* ── evaluation ────────────────────────────────────────────────────── */

static int16_t eval_binding(int player, cw_binding b, const cw_raw_input *raw, bool *sub) {
  uint32_t dev = I.devices[player];
  int pad = I.pad[player];
  const cw_pad_state *ps = pad >= 0 && pad < CW_MAX_PADS && raw->pads[pad].connected ? &raw->pads[pad] : NULL;
  switch ((cw_binding_kind)b.kind) {
  case CW_BIND_KEY:
    if (!(dev & CW_DEVICE_KEYBOARD) || b.code >= CW_KEY_COUNT) return 0;
    if (raw->keys_pressed[b.code]) *sub = true;
    return raw->keys[b.code] ? 32767 : 0;
  case CW_BIND_MOUSE:
    if (!(dev & CW_DEVICE_MOUSE) || b.code > 31) return 0;
    if (raw->mouse_pressed & (1u << b.code)) *sub = true;
    return raw->mouse_buttons & (1u << b.code) ? 32767 : 0;
  case CW_BIND_WHEEL:
    if (!(dev & CW_DEVICE_MOUSE)) return 0;
    return cw__input_quant((b.code ? raw->wheel_x : raw->wheel_y) * (float)b.dir);
  case CW_BIND_PAD_BUTTON:
    if (!ps || b.code >= CW_PAD_BUTTON_COUNT) return 0;
    if (ps->pressed & (1u << b.code)) *sub = true;
    return ps->buttons & (1u << b.code) ? 32767 : 0;
  case CW_BIND_PAD_AXIS: {
    if (!ps || b.code >= CW_AXIS_COUNT) return 0;
    int16_t q = cw__input_quant(ps->axes[b.code] * (float)b.dir);
    return q > 0 && q >= cw__input_quant(b.threshold) ? q : 0;
  }
  case CW_BIND_TOUCH_BUTTON:
    if (!(dev & CW_DEVICE_TOUCH) || b.code >= I.ncontrols) return 0;
    if (I.controls[b.code].tapped) *sub = true;
    return I.controls[b.code].active ? 32767 : 0;
  case CW_BIND_TOUCH_STICK: {
    if (!(dev & CW_DEVICE_TOUCH) || b.code >= I.ncontrols) return 0;
    cw_vec2 v = I.controls[b.code].value;
    float c = b.dir == 0 ? -v.x : b.dir == 1 ? v.x : b.dir == 2 ? -v.y : v.y;
    return cw__input_quant(c);
  }
  case CW_BIND_NONE:
    break;
  }
  return 0;
}

static void capture_update(const cw_raw_input *raw);

void cw_input_frame_begin(const cw_raw_input *raw) {
  if (!I.init) return;
  cw__touch_update(raw);
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++) {
    for (int a = 0; a < I.count; a++) {
      const cw__action_def *d = &I.actions[a];
      int16_t q = 0;
      bool sub = false;
      for (int i = 0; i < d->nbind; i++) {
        int16_t v = eval_binding(p, d->bind[i], raw, &sub);
        if (v > q) q = v;
      }
      bool held = q > 0 && q >= d->dz_q;
      cw__slot *f = &I.frame.s[p][a];
      bool was_pressed = f->flags & CW__PRESSED;
      bool pressed = held || sub;
      bool jp = (held && !I.frame_held[p][a]) || sub;
      f->flags = (uint8_t)((pressed ? CW__PRESSED : 0) | (jp ? CW__JUST_PRESSED : 0) |
                           (was_pressed && !pressed ? CW__JUST_RELEASED : 0));
      f->raw = sub && !held ? 32767 : q;
      I.frame_held[p][a] = held;
      if (jp) I.pend_jp[p][a] = true;
      if (pressed && f->raw > I.pend_peak[p][a]) I.pend_peak[p][a] = f->raw;
    }
  }
  for (int i = 0; i < CW_MAX_PADS; i++) I.pend_pad[i] |= raw->pads[i].pressed;
  for (int k = 0; k < CW_KEY_COUNT; k++) I.pend_keys[k] |= raw->keys_pressed[k];
  I.pend_mouse |= raw->mouse_pressed;
  if (I.capture.on) capture_update(raw);
  I.prev_raw = *raw;
  I.ctx = CW_INPUT_FRAME;
}

static void live_tick(void) {
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++) {
    for (int a = 0; a < I.count; a++) {
      cw__slot *t = &I.tick_live.s[p][a];
      const cw__slot *f = &I.frame.s[p][a];
      bool held = I.frame_held[p][a];
      bool was = t->flags & CW__PRESSED;
      bool pressed = held || I.pend_jp[p][a];
      t->flags = (uint8_t)((pressed ? CW__PRESSED : 0) | (I.pend_jp[p][a] ? CW__JUST_PRESSED : 0) |
                           (was && !pressed ? CW__JUST_RELEASED : 0));
      t->raw = !held && pressed ? I.pend_peak[p][a] : f->raw;
      I.pend_jp[p][a] = false;
      I.pend_peak[p][a] = 0;
    }
  }
}

uint32_t cw__snapshot_hash(const cw__snapshot *s, int count, uint32_t h) {
  for (int p = 0; p < CW_INPUT_MAX_PLAYERS; p++)
    for (int a = 0; a < count; a++) {
      uint8_t b[3] = { s->s[p][a].flags, (uint8_t)(s->s[p][a].raw & 0xff), (uint8_t)((uint16_t)s->s[p][a].raw >> 8) };
      h = cw_hash32(b, 3, h);
    }
  return h;
}

void cw_input_tick_begin(void) {
  if (!I.init) return;
  live_tick();
  memcpy(I.tick_pad, I.pend_pad, sizeof I.tick_pad);
  memset(I.pend_pad, 0, sizeof I.pend_pad);
  memcpy(I.tick_keys, I.pend_keys, sizeof I.tick_keys);
  memset(I.pend_keys, 0, sizeof I.pend_keys);
  I.tick_mouse = I.pend_mouse;
  I.pend_mouse = 0;
  I.tick = &I.tick_live;
  if (cw__input_replay_next()) I.tick = &I.tick_replay;
  I.stream_hash = cw__snapshot_hash(I.tick, I.count, I.stream_hash);
  I.stream_ticks++;
  cw__input_record_tick(I.tick);
  I.ctx = CW_INPUT_TICK;
}

void cw_input_ticks_end(void) {
  if (I.init) I.ctx = CW_INPUT_FRAME;
}

uint32_t cw__input_pad_pressed(int pad) {
  const cw_raw_input *raw = cw_input_raw();
  if (pad < 0 || pad >= CW_MAX_PADS || !raw->pads[pad].connected) return 0;
  return I.ctx == CW_INPUT_TICK ? I.tick_pad[pad] : raw->pads[pad].pressed;
}
bool cw__input_key_pressed(int key) {
  if (key < 0 || key >= CW_KEY_COUNT) return false;
  return I.ctx == CW_INPUT_TICK ? I.tick_keys[key] != 0 : cw_input_raw()->keys_pressed[key] != 0;
}
uint32_t cw__input_mouse_pressed(void) { return I.ctx == CW_INPUT_TICK ? I.tick_mouse : cw_input_raw()->mouse_pressed; }

uint32_t cw_input_stream_hash(void) { return I.stream_hash; }

/* ── rebinding capture ─────────────────────────────────────────────── */

void cw_input_capture_begin(int player, uint32_t devices, bool pad) {
  cw__input_ensure_init();
  I.capture.on = true;
  I.capture.done = false;
  I.capture.player = player >= 0 && player < CW_INPUT_MAX_PLAYERS ? player : 0;
  I.capture.devices = devices;
  I.capture.pad = pad;
}
bool cw_input_capture_poll(cw_binding *out) {
  if (!I.init || !I.capture.done) return false;
  I.capture.done = false;
  if (out) *out = I.capture.result;
  return true;
}
void cw_input_capture_cancel(void) {
  if (I.init) I.capture.on = I.capture.done = false;
}
bool cw_input_capturing(void) { return I.init && I.capture.on; }

static void capture_found(cw_binding b) {
  I.capture.result = b;
  I.capture.done = true;
  I.capture.on = false;
}

static void capture_update(const cw_raw_input *raw) {
  const cw_raw_input *pr = &I.prev_raw;
  if (I.capture.devices & CW_DEVICE_KEYBOARD)
    for (int k = 0; k < CW_KEY_COUNT; k++)
      if (raw->keys_pressed[k] || (raw->keys[k] && !pr->keys[k])) {
        capture_found((cw_binding){ .kind = CW_BIND_KEY, .code = (uint16_t)k });
        return;
      }
  if (I.capture.devices & CW_DEVICE_MOUSE) {
    uint32_t down = raw->mouse_pressed | (raw->mouse_buttons & ~pr->mouse_buttons);
    for (int b = 0; b < 3; b++)
      if (down & (1u << b)) {
        capture_found((cw_binding){ .kind = CW_BIND_MOUSE, .code = (uint16_t)b });
        return;
      }
    if (raw->wheel_y != 0 || raw->wheel_x != 0) {
      bool vert = raw->wheel_y != 0;
      float v = vert ? raw->wheel_y : raw->wheel_x;
      capture_found((cw_binding){ .kind = CW_BIND_WHEEL, .code = vert ? 0 : 1, .dir = (int8_t)(v > 0 ? 1 : -1) });
      return;
    }
  }
  int pad = I.pad[I.capture.player];
  if (I.capture.pad && pad >= 0 && raw->pads[pad].connected) {
    const cw_pad_state *ps = &raw->pads[pad], *pp = &pr->pads[pad];
    uint32_t down = ps->pressed | (ps->buttons & ~pp->buttons);
    for (int b = 0; b < CW_PAD_BUTTON_COUNT; b++)
      if (down & (1u << b)) {
        capture_found((cw_binding){ .kind = CW_BIND_PAD_BUTTON, .code = (uint16_t)b });
        return;
      }
    for (int a = 0; a < CW_AXIS_COUNT; a++) {
      float v = ps->axes[a], was = pp->axes[a];
      if (fabsf(v) >= 0.5f && fabsf(was) < 0.5f) {
        capture_found((cw_binding){ .kind = CW_BIND_PAD_AXIS, .code = (uint16_t)a, .dir = (int8_t)(v > 0 ? 1 : -1) });
        return;
      }
    }
  }
}

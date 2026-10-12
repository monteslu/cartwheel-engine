/* Bindings as text: one binding ("axis:lx-@0.25"), and whole binding sets
 * ("jump = key:space, pad:south"), plus the committed defaults. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "input_internal.h"

#define I cw__in

typedef struct { const char *name; uint16_t code; } named;

/* SDL scancodes (USB HID usage ids) by name; anything else is key:<number> */
static const named key_names[] = {
  { "return", 40 }, { "enter", 40 }, { "escape", 41 }, { "backspace", 42 }, { "tab", 43 }, { "space", 44 },
  { "minus", 45 }, { "equals", 46 }, { "leftbracket", 47 }, { "rightbracket", 48 }, { "backslash", 49 },
  { "semicolon", 51 }, { "apostrophe", 52 }, { "grave", 53 }, { "comma", 54 }, { "period", 55 }, { "slash", 56 },
  { "capslock", 57 }, { "printscreen", 70 }, { "scrolllock", 71 }, { "pause", 72 }, { "insert", 73 },
  { "home", 74 }, { "pageup", 75 }, { "delete", 76 }, { "end", 77 }, { "pagedown", 78 }, { "right", 79 },
  { "left", 80 }, { "down", 81 }, { "up", 82 }, { "lctrl", 224 }, { "lshift", 225 }, { "lalt", 226 },
  { "lgui", 227 }, { "rctrl", 228 }, { "rshift", 229 }, { "ralt", 230 }, { "rgui", 231 },
};
static const char *pad_names[CW_PAD_BUTTON_COUNT] = { "south", "east", "west", "north", "back", "guide", "start",
                                                      "lstick", "rstick", "lshoulder", "rshoulder", "up", "down",
                                                      "left", "right" };
static const char *axis_names[CW_AXIS_COUNT] = { "lx", "ly", "rx", "ry", "lt", "rt" };
static const char *mouse_names[3] = { "left", "right", "middle" };
static const char *dir_names[4] = { "left", "right", "up", "down" };

static int find_name(const char *const *names, int n, const char *s, size_t len) {
  for (int i = 0; i < n; i++)
    if (strlen(names[i]) == len && strncmp(names[i], s, len) == 0) return i;
  return -1;
}

static bool parse_uint(const char *s, size_t len, long max, long *out) {
  if (!len) return false;
  long v = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > max) return false;
  }
  *out = v;
  return true;
}

bool cw_binding_parse(const char *text, cw_binding *out) {
  if (!text) return false;
  const char *colon = strchr(text, ':');
  if (!colon) return false;
  size_t kl = (size_t)(colon - text);
  const char *v = colon + 1;
  size_t vl = strlen(v);
  cw_binding b = { 0 };
  long n;
  if (kl == 3 && !strncmp(text, "key", 3)) {
    b.kind = CW_BIND_KEY;
    if (vl == 1 && v[0] >= 'a' && v[0] <= 'z') b.code = (uint16_t)(4 + v[0] - 'a');
    else if (vl == 1 && v[0] >= '1' && v[0] <= '9') b.code = (uint16_t)(30 + v[0] - '1');
    else if (vl == 1 && v[0] == '0') b.code = 39;
    else if ((vl == 2 || vl == 3) && v[0] == 'f' && parse_uint(v + 1, vl - 1, 12, &n) && n >= 1) b.code = (uint16_t)(57 + n);
    else if (parse_uint(v, vl, CW_KEY_COUNT - 1, &n)) b.code = (uint16_t)n;
    else {
      size_t i;
      for (i = 0; i < CW_ARRAY_LEN(key_names); i++)
        if (!strcmp(key_names[i].name, v)) break;
      if (i == CW_ARRAY_LEN(key_names)) return false;
      b.code = key_names[i].code;
    }
  } else if (kl == 5 && !strncmp(text, "mouse", 5)) {
    int i = find_name(mouse_names, 3, v, vl);
    if (i < 0) return false;
    b.kind = CW_BIND_MOUSE;
    b.code = (uint16_t)i;
  } else if (kl == 5 && !strncmp(text, "wheel", 5)) {
    int i = find_name(dir_names, 4, v, vl);
    if (i < 0) return false;
    b.kind = CW_BIND_WHEEL;
    b.code = i < 2 ? 1 : 0;                         /* left/right horizontal, up/down vertical */
    b.dir = (int8_t)(i == 1 || i == 2 ? 1 : -1);   /* right and up are positive */
  } else if (kl == 3 && !strncmp(text, "pad", 3)) {
    int i = find_name(pad_names, CW_PAD_BUTTON_COUNT, v, vl);
    if (i < 0) return false;
    b.kind = CW_BIND_PAD_BUTTON;
    b.code = (uint16_t)i;
  } else if (kl == 4 && !strncmp(text, "axis", 4)) {
    const char *at = strchr(v, '@');
    size_t nl = at ? (size_t)(at - v) : vl;
    if (nl < 3 || (v[nl - 1] != '+' && v[nl - 1] != '-')) return false;
    int i = find_name(axis_names, CW_AXIS_COUNT, v, nl - 1);
    if (i < 0) return false;
    b.kind = CW_BIND_PAD_AXIS;
    b.code = (uint16_t)i;
    b.dir = (int8_t)(v[nl - 1] == '+' ? 1 : -1);
    if (at) {
      char *end;
      b.threshold = strtof(at + 1, &end);
      if (end == at + 1 || *end || b.threshold < 0 || b.threshold > 1) return false;
    }
  } else if (kl == 5 && !strncmp(text, "touch", 5)) {
    const char *dot = strchr(v, '.');
    size_t nl = dot ? (size_t)(dot - v) : vl;
    if (!parse_uint(v, nl, CW_INPUT_MAX_TOUCH_CONTROLS - 1, &n)) return false;
    b.code = (uint16_t)n;
    if (dot) {
      int i = find_name(dir_names, 4, dot + 1, strlen(dot + 1));
      if (i < 0) return false;
      b.kind = CW_BIND_TOUCH_STICK;
      b.dir = (int8_t)i;
    } else {
      b.kind = CW_BIND_TOUCH_BUTTON;
    }
  } else {
    return false;
  }
  *out = b;
  return true;
}

const char *cw_binding_format(cw_binding b, char *buf, size_t cap) {
  if (!cap) return buf;
  buf[0] = 0;
  switch ((cw_binding_kind)b.kind) {
  case CW_BIND_KEY: {
    if (b.code >= 4 && b.code <= 29) snprintf(buf, cap, "key:%c", 'a' + (b.code - 4));
    else if (b.code >= 30 && b.code <= 38) snprintf(buf, cap, "key:%c", '1' + (b.code - 30));
    else if (b.code == 39) snprintf(buf, cap, "key:0");
    else if (b.code >= 58 && b.code <= 69) snprintf(buf, cap, "key:f%d", b.code - 57);
    else {
      snprintf(buf, cap, "key:%u", b.code);
      for (size_t i = 0; i < CW_ARRAY_LEN(key_names); i++)
        if (key_names[i].code == b.code) {
          snprintf(buf, cap, "key:%s", key_names[i].name);
          break;
        }
    }
    break;
  }
  case CW_BIND_MOUSE: snprintf(buf, cap, "mouse:%s", b.code < 3 ? mouse_names[b.code] : "left"); break;
  case CW_BIND_WHEEL:
    snprintf(buf, cap, "wheel:%s", b.code ? (b.dir > 0 ? "right" : "left") : (b.dir > 0 ? "up" : "down"));
    break;
  case CW_BIND_PAD_BUTTON: snprintf(buf, cap, "pad:%s", b.code < CW_PAD_BUTTON_COUNT ? pad_names[b.code] : "south"); break;
  case CW_BIND_PAD_AXIS:
    if (b.threshold > 0)
      snprintf(buf, cap, "axis:%s%c@%g", axis_names[b.code % CW_AXIS_COUNT], b.dir > 0 ? '+' : '-', (double)b.threshold);
    else
      snprintf(buf, cap, "axis:%s%c", axis_names[b.code % CW_AXIS_COUNT], b.dir > 0 ? '+' : '-');
    break;
  case CW_BIND_TOUCH_BUTTON: snprintf(buf, cap, "touch:%u", b.code); break;
  case CW_BIND_TOUCH_STICK: snprintf(buf, cap, "touch:%u.%s", b.code, dir_names[b.dir & 3]); break;
  case CW_BIND_NONE: break;
  }
  return buf;
}

/* ── binding sets ──────────────────────────────────────────────────── */

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
  return s;
}

bool cw_input_load_bindings(const char *text, size_t len) {
  cw__input_ensure_init();
  bool ok = true;
  size_t pos = 0;
  int line_no = 0;
  char line[1024];
  while (pos < len) {
    size_t e = pos;
    while (e < len && text[e] != '\n') e++;
    line_no++;
    size_t n = e - pos < sizeof line - 1 ? e - pos : sizeof line - 1;
    memcpy(line, text + pos, n);
    line[n] = 0;
    pos = e + 1;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *l = trim(line);
    if (!*l) continue;
    char *eq = strchr(l, '=');
    if (!eq) {
      cw_error("bindings line %d: '%s' needs 'action = binding, binding'", line_no, l);
      ok = false;
      continue;
    }
    *eq = 0;
    char *name = trim(l);
    if (!*name) {
      cw_error("bindings line %d: missing action name before '='", line_no);
      ok = false;
      continue;
    }
    /* parse everything first: a line with a typo changes nothing */
    cw_binding list[CW_INPUT_MAX_BINDINGS];
    int count = 0;
    float dz = 0.5f;
    bool line_ok = true;
    char *item = eq + 1;
    while (item) {
      char *comma = strchr(item, ',');
      if (comma) *comma = 0;
      char *t = trim(item);
      if (*t) {
        cw_binding b;
        if (!strncmp(t, "deadzone:", 9)) {
          char *end;
          dz = strtof(t + 9, &end);
          if (end == t + 9 || *end || dz < 0 || dz > 1) {
            cw_error("bindings line %d: deadzone '%s' must be 0..1", line_no, t + 9);
            line_ok = false;
          }
        } else if (!cw_binding_parse(t, &b)) {
          cw_error("bindings line %d: '%s' is not a binding (key:space, pad:south, axis:lx-@0.2, mouse:left, "
                   "wheel:up, touch:0, touch:1.left)", line_no, t);
          line_ok = false;
        } else if (count >= CW_INPUT_MAX_BINDINGS) {
          cw_error("bindings line %d: more than %d bindings for '%s'", line_no, CW_INPUT_MAX_BINDINGS, name);
          line_ok = false;
        } else {
          list[count++] = b;
        }
      }
      item = comma ? comma + 1 : NULL;
    }
    if (!line_ok) {
      ok = false;
      continue;
    }
    cw_action a = cw_action_add(name, dz);
    if (a < 0) {
      ok = false;
      continue;
    }
    I.actions[a].nbind = 0;
    for (int i = 0; i < count; i++) cw__action_bind_quiet(a, list[i]);
  }
  return ok;
}

bool cw_input_load_bindings_asset(const char *path) {
  size_t len;
  char *text = cw_asset_load(path, &len);
  if (!text) {
    cw_error("bindings: asset '%s' not found (ship it in the app's assets directory)", path);
    return false;
  }
  bool ok = cw_input_load_bindings(text, len);
  free(text);
  return ok;
}

void cw_input_commit_defaults(void) {
  cw__input_ensure_init();
  memcpy(I.defaults, I.actions, sizeof I.actions);
  I.default_count = I.count;
}

static const cw__action_def *default_of(const char *name) {
  for (int i = 0; i < I.default_count; i++)
    if (I.defaults[i].name == name) return &I.defaults[i];
  return NULL;
}

void cw_input_reset_to_defaults(void) {
  cw__input_ensure_init();
  for (int a = 0; a < I.count; a++) {
    const cw__action_def *d = default_of(I.actions[a].name);
    if (d) I.actions[a] = *d;
  }
  cw__input_changed();
}

static bool same_def(const cw__action_def *x, const cw__action_def *y) {
  if (x->deadzone != y->deadzone || x->nbind != y->nbind) return false;
  for (int i = 0; i < x->nbind; i++) {
    const cw_binding *a = &x->bind[i], *b = &y->bind[i];
    if (a->kind != b->kind || a->dir != b->dir || a->code != b->code || a->threshold != b->threshold) return false;
  }
  return true;
}

size_t cw_input_save_bindings(char *dst, size_t cap, bool overrides_only) {
  cw__input_ensure_init();
  size_t need = 0;
#define EMIT(...) \
  do { \
    int k_ = snprintf(dst && need < cap ? dst + need : NULL, dst && need < cap ? cap - need : 0, __VA_ARGS__); \
    if (k_ > 0) need += (size_t)k_; \
  } while (0)
  for (int a = 0; a < I.count; a++) {
    const cw__action_def *d = &I.actions[a];
    if (overrides_only) {
      const cw__action_def *def = default_of(d->name);
      if (def && same_def(def, d)) continue;
    }
    EMIT("%s =", d->name);
    for (int i = 0; i < d->nbind; i++) {
      char b[48];
      EMIT("%s %s", i ? "," : "", cw_binding_format(d->bind[i], b, sizeof b));
    }
    if (d->deadzone != 0.5f) EMIT("%s deadzone:%g", d->nbind ? "," : "", (double)d->deadzone);
    EMIT("\n");
  }
#undef EMIT
  if (dst && cap) dst[need < cap ? need : cap - 1] = 0;
  return need;
}

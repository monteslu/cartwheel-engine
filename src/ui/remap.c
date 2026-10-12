/* Input remapping support (cartwheel/a11y.h): binding display names
 * (translatable) and RemapButton, a Button that captures a new binding for
 * one action slot. */
#include "ui_internal.h"
#include "cartwheel/a11y.h"
#include "cartwheel/l10n.h"

/* ── display names ──────────────────────────────────────────────────── */

typedef struct { const char *id, *en; } name_pair;
static const name_pair key_en[] = {
  { "return", "Enter" }, { "escape", "Esc" }, { "backspace", "Backspace" }, { "tab", "Tab" }, { "space", "Space" },
  { "minus", "-" }, { "equals", "=" }, { "leftbracket", "[" }, { "rightbracket", "]" }, { "backslash", "\\" },
  { "semicolon", ";" }, { "apostrophe", "'" }, { "grave", "`" }, { "comma", "," }, { "period", "." }, { "slash", "/" },
  { "capslock", "Caps Lock" }, { "printscreen", "Print Screen" }, { "scrolllock", "Scroll Lock" }, { "pause", "Pause" },
  { "insert", "Insert" }, { "home", "Home" }, { "pageup", "Page Up" }, { "delete", "Delete" }, { "end", "End" },
  { "pagedown", "Page Down" }, { "right", "Right" }, { "left", "Left" }, { "down", "Down" }, { "up", "Up" },
  { "lctrl", "Left Ctrl" }, { "lshift", "Left Shift" }, { "lalt", "Left Alt" }, { "lgui", "Left Super" },
  { "rctrl", "Right Ctrl" }, { "rshift", "Right Shift" }, { "ralt", "Right Alt" }, { "rgui", "Right Super" } };
static const name_pair pad_en[] = {
  { "south", "South Button" }, { "east", "East Button" }, { "west", "West Button" }, { "north", "North Button" },
  { "back", "Back" }, { "guide", "Guide" }, { "start", "Start" }, { "lstick", "Left Stick Press" },
  { "rstick", "Right Stick Press" }, { "lshoulder", "Left Shoulder" }, { "rshoulder", "Right Shoulder" },
  { "up", "D-pad Up" }, { "down", "D-pad Down" }, { "left", "D-pad Left" }, { "right", "D-pad Right" } };
static const name_pair axis_en[] = {
  { "lx-", "Left Stick Left" }, { "lx+", "Left Stick Right" }, { "ly-", "Left Stick Up" }, { "ly+", "Left Stick Down" },
  { "rx-", "Right Stick Left" }, { "rx+", "Right Stick Right" }, { "ry-", "Right Stick Up" }, { "ry+", "Right Stick Down" },
  { "lt+", "Left Trigger" }, { "lt-", "Left Trigger" }, { "rt+", "Right Trigger" }, { "rt-", "Right Trigger" } };
static const name_pair misc_en[] = {
  { "mouse.left", "Left Mouse Button" }, { "mouse.right", "Right Mouse Button" }, { "mouse.middle", "Middle Mouse Button" },
  { "wheel.up", "Wheel Up" }, { "wheel.down", "Wheel Down" }, { "wheel.left", "Wheel Left" }, { "wheel.right", "Wheel Right" } };

static const char *lookup(const name_pair *t, size_t n, const char *id) {
  for (size_t i = 0; i < n; i++)
    if (!strcmp(t[i].id, id)) return t[i].en;
  return NULL;
}

const char *cw_binding_display_name(cw_binding b, char *buf, size_t cap) {
  if (!cap) return buf;
  char text[64];
  cw_binding_format(b, text, sizeof text);
  if (!text[0]) {
    const char *t = cw_tr_find("remap.unbound");
    snprintf(buf, cap, "%s", t ? t : "Unbound");
    return buf;
  }
  char *colon = strchr(text, ':');
  *colon = 0;
  const char *kind = text, *id = colon + 1;
  char idb[32];
  snprintf(idb, sizeof idb, "%s", id);
  char *at = strchr(idb, '@'); /* axis thresholds are not part of the name */
  if (at) *at = 0;
  /* a catalog may name it: input.key.space, input.pad.south, input.axis.lx-, input.mouse.left */
  const char *tr = cw_tr_find(cw_frame_printf("input.%s.%s", kind, idb));
  if (tr) {
    snprintf(buf, cap, "%s", tr);
    return buf;
  }
  const char *en = NULL;
  if (!strcmp(kind, "key")) {
    en = lookup(key_en, CW_ARRAY_LEN(key_en), idb);
    if (!en && strlen(idb) == 1) {
      snprintf(buf, cap, "%c", idb[0] >= 'a' && idb[0] <= 'z' ? idb[0] - 32 : idb[0]);
      return buf;
    }
    if (!en && idb[0] == 'f' && idb[1] >= '0' && idb[1] <= '9') {
      snprintf(buf, cap, "F%s", idb + 1);
      return buf;
    }
    if (!en) {
      snprintf(buf, cap, "Key %s", idb);
      return buf;
    }
  } else if (!strcmp(kind, "pad")) en = lookup(pad_en, CW_ARRAY_LEN(pad_en), idb);
  else if (!strcmp(kind, "axis")) en = lookup(axis_en, CW_ARRAY_LEN(axis_en), idb);
  else en = lookup(misc_en, CW_ARRAY_LEN(misc_en), cw_frame_printf("%s.%s", kind, idb));
  snprintf(buf, cap, "%s", en ? en : colon + 1);
  return buf;
}

/* ── RemapButton ────────────────────────────────────────────────────── */

typedef struct {
  const char *action;
  int32_t slot;
  int32_t device; /* 0 keyboard + mouse, 1 gamepad */
  bool listening;
} remap_data;
static const cw_node_type *remap_t;
static CW_ARRAY(cw_node) live; /* every RemapButton, for the per-frame refresh */

static const char *wanted_text(remap_data *r) {
  if (r->listening) {
    const char *t = cw_tr_find(r->device ? "remap.listening_pad" : "remap.listening");
    return t ? t : r->device ? "Press a button..." : "Press a key...";
  }
  cw_action a = cw_action_find(ui_s(r->action));
  cw_binding bs[CW_INPUT_MAX_BINDINGS];
  int k = a >= 0 ? cw_action_bindings(a, bs, CW_INPUT_MAX_BINDINGS) : 0;
  char buf[128];
  cw_binding none = { 0 };
  return cw_frame_printf("%s", cw_binding_display_name(r->slot >= 0 && r->slot < k ? bs[r->slot] : none, buf, sizeof buf));
}

static void remap_pressed(cw_node n, const cw_value *args, int argc, void *user) {
  (void)args, (void)argc, (void)user;
  remap_data *r = cw_node_data_t(n, remap_t);
  if (!r || r->listening || cw_action_find(ui_s(r->action)) < 0) {
    if (r && cw_action_find(ui_s(r->action)) < 0) cw_warn("RemapButton '%s': no action named '%s'", cw_node_name(n), ui_s(r->action));
    return;
  }
  if (cw_node_valid(UI.remap_capture) && UI.remap_capture.id != n.id) {
    remap_data *o = cw_node_data_t(UI.remap_capture, remap_t);
    if (o) o->listening = false;
  }
  r->listening = true;
  UI.remap_capture = n;
  if (r->device) cw_input_capture_begin(0, 0, true);
  else cw_input_capture_begin(0, CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE, false);
}

static void remap_finish(cw_node n, remap_data *r, const cw_binding *b) {
  r->listening = false;
  UI.remap_capture = (cw_node){ 0 };
  if (!b) {
    cw_input_capture_cancel();
    return;
  }
  cw_action a = cw_action_find(ui_s(r->action));
  int count = cw_action_bindings(a, NULL, 0);
  int slot = r->slot < 0 ? 0 : r->slot > count ? count : r->slot;
  cw_action_set_binding(a, slot, *b);
  char t[64];
  ui_emit_str(n, "remapped", cw_binding_format(*b, t, sizeof t));
}

void ui_remap_frame(void) {
  /* an active capture */
  cw_node n = UI.remap_capture;
  remap_data *r = cw_node_valid(n) ? cw_node_data_t(n, remap_t) : NULL;
  if (r && r->listening) {
    const cw_raw_input *raw = cw_input_raw();
    if (raw && (raw->keys_pressed[41] || (raw->keys[41] && !UI.prev_keys[41]))) remap_finish(n, r, NULL); /* Escape */
    else {
      cw_binding b;
      if (cw_input_capture_poll(&b)) {
        bool cancel = b.kind == CW_BIND_PAD_BUTTON && b.code == CW_PAD_BACK; /* the pad's way out */
        remap_finish(n, r, cancel ? NULL : &b);
      }
    }
  } else UI.remap_capture = (cw_node){ 0 };
  /* labels follow the bindings and the locale */
  for (int i = 0; i < live.count; i++) {
    cw_node b = live.data[i];
    remap_data *d = cw_node_valid(b) ? cw_node_data_t(b, remap_t) : NULL;
    if (!d) continue;
    const char *want = wanted_text(d);
    if (strcmp(want, cw_button_text(b))) cw_button_set_text(b, want);
  }
}

static void remap_init(cw_node n, void *d) {
  remap_data *r = d;
  r->slot = 0;
  CTL(n)->auto_translate = false; /* the text is already the display name */
  cw_array_push(&live, n);
  cw_connect(n, "pressed", n, NULL, remap_pressed, NULL, 0);
}

static void remap_destroy(cw_node n, void *d) {
  (void)d;
  if (UI.remap_capture.id == n.id) {
    cw_input_capture_cancel();
    UI.remap_capture = (cw_node){ 0 };
  }
  for (int i = 0; i < live.count; i++)
    if (live.data[i].id == n.id) {
      live.data[i] = live.data[--live.count];
      break;
    }
}

static const char *const device_names[] = { "KEYBOARD_MOUSE", "GAMEPAD", NULL };
static const cw_prop remap_props[] = {
  CW_PROP_FIELD(remap_data, action, CW_T_STRING, CW_PROP_DEFAULT, "The input action to rebind."),
  CW_PROP_FIELD(remap_data, slot, CW_T_INT, CW_PROP_DEFAULT, "Which of the action's bindings (0 = the first)."),
  { .name = "device", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(remap_data, device), .size = 4,
    .enum_names = device_names, .doc = "Capture from the keyboard and mouse, or from the gamepad." },
  CW_PROP_END
};
static const char *const remap_signals[] = { "remapped", NULL };
static cw_node_type remap_type = { .name = "RemapButton", .parent = "Button", .data_size = sizeof(remap_data),
                                   .props = remap_props, .signals = remap_signals, .init = remap_init,
                                   .destroy = remap_destroy,
                                   .doc = "Shows an action's binding; press it, then press a key or button to rebind." };

bool cw__remap_capturing(void) { return cw_node_valid(UI.remap_capture); }

void ui_register_remap(void) {
  cw_node_type_register(&remap_type);
  remap_t = &remap_type;
  cw_ui_class_register("RemapButton", &(cw_ui_class){ .theme_type = "Button" });
}

/* Button, CheckBox, CheckButton, OptionButton; button groups. */
#include "ui_internal.h"

typedef struct {
  const char *text;
  bool disabled, toggle_mode, button_pressed, flat;
  const char *button_group;
  int32_t alignment; /* cw_halign */
  bool pressing;     /* held by the pointer */
} button_data;
static const cw_node_type *button_t;
enum { KIND_BUTTON, KIND_CHECKBOX, KIND_CHECKBUTTON, KIND_OPTION };

static int kind_of(cw_node n) {
  const cw_node_type *t = cw_node_get_type(n);
  if (cw_node_type_is(t, "OptionButton")) return KIND_OPTION;
  if (cw_node_type_is(t, "CheckButton")) return KIND_CHECKBUTTON;
  if (cw_node_type_is(t, "CheckBox")) return KIND_CHECKBOX;
  return KIND_BUTTON;
}

static const char *state_style(cw_node n, button_data *b) {
  cw_control *c = CTL(n);
  if (b->disabled) return "disabled";
  if ((b->pressing && (c->_flags & CF_HOVERED)) || (b->button_pressed && kind_of(n) == KIND_BUTTON)) return "pressed";
  if (c->_flags & CF_HOVERED) return "hover";
  return "normal";
}

/* ── OptionButton items ─────────────────────────────────────────────── */
typedef struct {
  const char *items; /* '\n' separated */
  int32_t selected;
  cw_node popup;     /* internal PopupMenu (orphan) */
} option_data;
static const cw_node_type *option_t;

static int item_count(const char *items) {
  if (!items || !*items) return 0;
  int n = 1;
  for (const char *p = items; *p; p++) n += *p == '\n';
  return n;
}
static const char *item_at(const char *items, int i) {
  const char *s = items ? items : "";
  for (int k = 0; k < i; k++) {
    s = strchr(s, '\n');
    if (!s) return "";
    s++;
  }
  const char *e = strchr(s, '\n');
  return cw_frame_printf("%.*s", e ? (int)(e - s) : (int)strlen(s), s);
}

static const char *display_text(cw_node n, button_data *b) {
  if (kind_of(n) == KIND_OPTION) {
    option_data *o = cw_node_data_t(n, option_t);
    if (o->selected >= 0 && o->selected < item_count(o->items)) return ui_tr(n, cw_intern(item_at(o->items, o->selected)));
    return "";
  }
  return ui_tr(n, ui_s(b->text));
}

static cw_vec2 button_min(cw_node n, void *d) {
  button_data *b = d;
  /* the widest content margins of any state, so no state ellipsizes */
  cw_ui_style st = T_STYLE(n, "normal");
  static const char *const states[] = { "hover", "pressed", "disabled" };
  for (int i = 0; i < 3; i++) {
    cw_ui_style o = T_STYLE(n, states[i]);
    for (int k = 0; k < 4; k++) st.margin[k] = CW_MAX(st.margin[k], o.margin[k]);
  }
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  float lh = cw_ui_line_height(f, fs);
  float sep = T_CONST(n, "h_separation");
  float tw = cw_ui_text_width(f, fs, display_text(n, b));
  float w = tw, h = lh;
  switch (kind_of(n)) {
  case KIND_CHECKBOX: {
    float cs = T_CONST(n, "check_size");
    w = cs + (tw > 0 ? sep + tw : 0);
    h = CW_MAX(lh, cs);
    break;
  }
  case KIND_CHECKBUTTON: {
    float sw = T_CONST(n, "switch_width"), sh = T_CONST(n, "switch_height");
    w = sw + (tw > 0 ? sep + tw : 0);
    h = CW_MAX(lh, sh);
    break;
  }
  case KIND_OPTION: {
    option_data *o = cw_node_data_t(n, option_t);
    int k = item_count(o->items);
    for (int i = 0; i < k; i++) w = CW_MAX(w, cw_ui_text_width(f, fs, ui_tr(n, cw_intern(item_at(o->items, i)))));
    w += sep + lh * 0.7f; /* the arrow sits in a square of the text height */
    break;
  }
  default: break;
  }
  return (cw_vec2){ ceilf(w + SM_W(&st)), ceilf(h + SM_H(&st)) };
}

static void button_draw(cw_node n, void *d) {
  button_data *b = d;
  cw_control *c = CTL(n);
  const char *state = state_style(n, b);
  int kind = kind_of(n);
  cw_ui_style st = T_STYLE(n, state);
  cw_rect r = { 0, 0, c->rect.w, c->rect.h };
  if (!b->flat || strcmp(state, "normal")) cw_ui_draw_style(&st, r);
  cw_rect in = style_content(&st, r);
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  const char *fcn = b->disabled ? "font_disabled_color" : !strcmp(state, "pressed") ? "font_pressed_color"
                    : !strcmp(state, "hover") ? "font_hover_color" : "font_color";
  cw_color fc = T_COLOR(n, fcn);
  if (fc.a <= 0) fc = T_COLOR(n, "font_color");
  float sep = T_CONST(n, "h_separation");
  const char *text = display_text(n, b);
  bool rtl = ui_draw_rtl(); /* the box, switch and arrow sit at the other end */
  if (kind == KIND_CHECKBOX) {
    float cs = T_CONST(n, "check_size");
    cw_rect box = { rtl ? in.x + in.w - cs : in.x, in.y + ui_round((in.h - cs) * 0.5f), cs, cs };
    cw_ui_style bs = T_STYLE(n, b->button_pressed ? "box_checked" : "box");
    cw_ui_draw_style(&bs, box);
    if (b->button_pressed) cw_ui_draw_icon(CW_ICON_CHECK, (cw_rect){ box.x + 2, box.y + 2, cs - 4, cs - 4 }, T_COLOR(n, "check_color"));
    if (!rtl) in.x += cs + sep;
    in.w -= cs + sep;
  } else if (kind == KIND_CHECKBUTTON) {
    float sw = T_CONST(n, "switch_width"), sh = T_CONST(n, "switch_height");
    cw_rect tr = { rtl ? in.x : in.x + in.w - sw, in.y + ui_round((in.h - sh) * 0.5f), sw, sh };
    cw_ui_style ts = T_STYLE(n, b->button_pressed ? "track_on" : "track");
    cw_ui_draw_style(&ts, tr);
    float kr = sh * 0.5f - 3;
    float kx = b->button_pressed != rtl ? tr.x + tr.w - sh * 0.5f : tr.x + sh * 0.5f;
    cw_ui_draw_circle((cw_vec2){ kx, tr.y + sh * 0.5f }, kr, T_COLOR(n, "knob_color"));
    if (rtl) in.x += sw + sep;
    in.w -= sw + sep;
  } else if (kind == KIND_OPTION) {
    float a = cw_ui_line_height(f, fs) * 0.7f;
    cw_ui_draw_icon(CW_ICON_ARROW_DOWN, (cw_rect){ rtl ? in.x : in.x + in.w - a, in.y + (in.h - a) * 0.5f, a, a }, T_COLOR(n, "arrow_color"));
    if (rtl) in.x += a + sep;
    in.w -= a + sep;
  }
  cw_halign al = kind == KIND_BUTTON ? (cw_halign)b->alignment : CW_HALIGN_LEFT;
  cw_ui_draw_text_in(f, fs, text, in, al, fc, true);
}

/* ── behaviour ──────────────────────────────────────────────────────── */

typedef struct { const char *group; cw_node except; } group_ctx;
static bool ungroup_fn(cw_node n, void *u) {
  group_ctx *g = u;
  button_data *b = cw_node_data_t(n, button_t);
  if (b && n.id != g->except.id && b->button_group == g->group && b->button_pressed) {
    b->button_pressed = false;
    ui_emit_bool(n, "toggled", false);
  }
  return true;
}

void cw_button_set_pressed(cw_node n, bool p) {
  button_data *b = cw_node_data_t(n, button_t);
  if (!b || b->button_pressed == p) return;
  b->button_pressed = p;
  if (p && b->button_group && *b->button_group) {
    group_ctx g = { b->button_group, n };
    cw_tree *t = cw_node_tree(n);
    if (t) cw_node_walk(cw_tree_root(t), ungroup_fn, &g);
  }
  ui_emit_bool(n, "toggled", p);
}
bool cw_button_pressed(cw_node n) {
  button_data *b = cw_node_data_t(n, button_t);
  return b && b->button_pressed;
}

static void option_open(cw_node n);

static void activate(cw_node n, button_data *b) {
  if (b->disabled) return;
  if (kind_of(n) == KIND_OPTION) {
    option_open(n);
    ui_emit0(n, "pressed");
    return;
  }
  if (b->toggle_mode) {
    bool grouped = b->button_group && *b->button_group;
    if (!(grouped && b->button_pressed)) cw_button_set_pressed(n, !b->button_pressed);
  }
  ui_emit0(n, "pressed");
}

static bool button_input(cw_node n, void *d, const cw_ui_event *e) {
  button_data *b = d;
  cw_control *c = CTL(n);
  switch (e->type) {
  case CW_UI_MOUSE_DOWN:
    if (e->button != 0) return false;
    if (!b->disabled) b->pressing = true;
    return true;
  case CW_UI_MOUSE_UP:
    if (e->button != 0 || !b->pressing) return false;
    b->pressing = false;
    if (!e->canceled && rect_has((cw_rect){ 0, 0, c->rect.w, c->rect.h }, e->pos)) activate(n, b);
    return true;
  case CW_UI_MOUSE_MOVE: return b->pressing;
  case CW_UI_NAV:
    if (e->nav != CW_NAV_ACCEPT || e->repeat) return false;
    activate(n, b);
    return true;
  default: return false;
  }
}

static void button_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
}
static void checkbox_init(cw_node n, void *d) {
  (void)d;
  ((button_data *)cw_node_data_t(n, button_t))->toggle_mode = true;
}
static void button_changed(cw_node n, void *d, const cw_prop *p) {
  button_data *b = d;
  if (!strcmp(p->name, "button_pressed")) {
    /* through cw_node_set: keep groups exclusive */
    bool v = b->button_pressed;
    b->button_pressed = !v;
    cw_button_set_pressed(n, v);
  }
  if (!strcmp(p->name, "disabled") && b->disabled) {
    b->pressing = false;
    cw_control_release_focus(n);
  }
  ui_mark_min_dirty(n);
}

void cw_button_set_text(cw_node n, const char *t) {
  button_data *b = cw_node_data_t(n, button_t);
  if (!b) return;
  const char *it = cw_intern_ref(t ? t : ""); /* counted: per-frame text must not pile up */
  cw_intern_unref(b->text);
  b->text = it;
  ui_mark_min_dirty(n);
}
const char *cw_button_text(cw_node n) {
  button_data *b = cw_node_data_t(n, button_t);
  return b ? ui_s(b->text) : "";
}
void cw_button_set_disabled(cw_node n, bool v) { cw_node_set(n, "disabled", cw_val_bool(v)); }
void cw_button_set_toggle_mode(cw_node n, bool v) {
  button_data *b = cw_node_data_t(n, button_t);
  if (b) b->toggle_mode = v;
}
void cw_button_set_group(cw_node n, const char *g) {
  button_data *b = cw_node_data_t(n, button_t);
  if (b) b->button_group = g ? cw_intern(g) : NULL;
}

/* ── OptionButton ───────────────────────────────────────────────────── */

static void option_chosen(cw_node menu, const cw_value *args, int argc, void *user) {
  (void)user;
  cw_node n = CTL(menu)->_owner; /* never pass a 64-bit handle through a void * (wasm32 truncates it) */
  if (argc < 1 || !cw_node_valid(n)) return;
  int idx = (int)args[0].i;
  option_data *o = cw_node_data_t(n, option_t);
  if (o->selected != idx) {
    o->selected = idx;
    ui_mark_min_dirty(n);
  }
  ui_emit_int(n, "item_selected", idx);
}

static void option_open(cw_node n) {
  option_data *o = cw_node_data_t(n, option_t);
  if (!cw_node_valid(o->popup)) {
    o->popup = cw_node_new("PopupMenu", "OptionPopup");
    cw_control *pc = CTL(o->popup);
    pc->_flags |= CF_INTERNAL;
    pc->_owner = n;
    cw_connect(o->popup, "id_pressed", n, NULL, option_chosen, NULL, 0);
  }
  if (cw_popup_visible(o->popup)) {
    cw_popup_hide(o->popup);
    return;
  }
  cw_popup_menu_clear(o->popup);
  int k = item_count(o->items);
  for (int i = 0; i < k; i++) {
    int at = cw_popup_menu_add_check_item(o->popup, item_at(o->items, i), i);
    cw_popup_menu_set_checked(o->popup, at, i == o->selected);
  }
  cw_rect g = cw_control_global_rect(n);
  cw_control_set_custom_min_size(o->popup, (cw_vec2){ g.w, 0 });
  cw_popup_menu_popup(o->popup, (cw_vec2){ g.x, g.y + g.h + 2 });
}

static void option_destroy(cw_node n, void *d) {
  (void)n;
  option_data *o = d;
  if (cw_node_valid(o->popup)) cw_node_free(o->popup);
}
static void option_init(cw_node n, void *d) {
  (void)n;
  ((option_data *)d)->selected = -1;
}
static void option_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}

void cw_option_button_add_item(cw_node n, const char *label) {
  option_data *o = cw_node_data_t(n, option_t);
  if (!o) return;
  const char *it = ui_s(o->items);
  o->items = cw_intern(*it ? cw_frame_printf("%s\n%s", it, label) : label);
  if (o->selected < 0) o->selected = 0;
  ui_mark_min_dirty(n);
}
void cw_option_button_clear(cw_node n) {
  option_data *o = cw_node_data_t(n, option_t);
  if (!o) return;
  o->items = NULL;
  o->selected = -1;
  ui_mark_min_dirty(n);
}
void cw_option_button_select(cw_node n, int i) {
  option_data *o = cw_node_data_t(n, option_t);
  if (!o) return;
  o->selected = CW_CLAMP(i, -1, item_count(o->items) - 1);
  ui_mark_min_dirty(n);
}
int cw_option_button_selected(cw_node n) {
  option_data *o = cw_node_data_t(n, option_t);
  return o ? o->selected : -1;
}
int cw_option_button_item_count(cw_node n) {
  option_data *o = cw_node_data_t(n, option_t);
  return o ? item_count(o->items) : 0;
}

static const char *const halign_names[] = { "LEFT", "CENTER", "RIGHT", "FILL", NULL };
static const cw_prop button_props[] = {
  CW_PROP_FIELD(button_data, text, CW_T_STRING, CW_PROP_DEFAULT, "Label."),
  CW_PROP_FIELD(button_data, disabled, CW_T_BOOL, CW_PROP_DEFAULT, "Ignores input and draws the disabled style."),
  CW_PROP_FIELD(button_data, toggle_mode, CW_T_BOOL, CW_PROP_DEFAULT, "Stays pressed until pressed again."),
  CW_PROP_FIELD(button_data, button_pressed, CW_T_BOOL, CW_PROP_DEFAULT, "Toggle state."),
  CW_PROP_FIELD(button_data, button_group, CW_T_STRING, CW_PROP_DEFAULT, "Toggle buttons with the same group name are exclusive."),
  { .name = "alignment", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(button_data, alignment),
    .size = 4, .enum_names = halign_names, .doc = "Text alignment." },
  CW_PROP_FIELD(button_data, flat, CW_T_BOOL, CW_PROP_DEFAULT, "No background until hovered or pressed."),
  CW_PROP_END
};
static const char *const button_signals[] = { "pressed", "toggled", NULL };
static cw_node_type button_type = { .name = "Button", .parent = "Control", .data_size = sizeof(button_data),
                                    .props = button_props, .signals = button_signals, .init = button_init,
                                    .prop_changed = button_changed, .doc = "A push or toggle button." };
static cw_node_type checkbox_type = { .name = "CheckBox", .parent = "Button", .init = checkbox_init,
                                      .doc = "A toggle drawn as a check box." };
static cw_node_type checkbutton_type = { .name = "CheckButton", .parent = "Button", .init = checkbox_init,
                                         .doc = "A toggle drawn as a switch." };
static const cw_prop option_props[] = {
  CW_PROP_FIELD(option_data, items, CW_T_STRING, CW_PROP_DEFAULT, "Choices, one per line."),
  CW_PROP_FIELD(option_data, selected, CW_T_INT, CW_PROP_DEFAULT, "Index of the chosen item (-1 none)."),
  CW_PROP_END
};
static const char *const option_signals[] = { "item_selected", NULL };
static cw_node_type option_type = { .name = "OptionButton", .parent = "Button", .data_size = sizeof(option_data),
                                    .props = option_props, .signals = option_signals, .init = option_init,
                                    .destroy = option_destroy, .prop_changed = option_changed,
                                    .doc = "A button that opens a list of choices." };

void ui_register_buttons(void) {
  cw_node_type_register(&button_type);
  button_t = &button_type;
  cw_ui_class_register("Button", &(cw_ui_class){ .min_size = button_min, .draw = button_draw, .input = button_input });
  cw_node_type_register(&checkbox_type);
  cw_ui_class_register("CheckBox", &(cw_ui_class){ 0 });
  cw_node_type_register(&checkbutton_type);
  cw_ui_class_register("CheckButton", &(cw_ui_class){ 0 });
  cw_node_type_register(&option_type);
  option_t = &option_type;
  cw_ui_class_register("OptionButton", &(cw_ui_class){ 0 });
}

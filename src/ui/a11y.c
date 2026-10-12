/* The accessibility tree of the UI (cartwheel/a11y.h): roles, names,
 * values and states from Control nodes, the event log a host bridge
 * follows, live announcements, the `a11y` debug command. */
#include "ui_internal.h"
#include "cartwheel/a11y.h"
#include "cartwheel/debug.h"
#include "cartwheel/l10n.h"

/* ── events ─────────────────────────────────────────────────────────── */

#define RING 64
typedef struct { cw_a11y_event e; char text[200]; } ring_entry;
static struct {
  ring_entry ring[RING];
  uint32_t seq;
  cw_a11y_listener_fn fn;
  void *user;
} EV;

static void push_event(cw_a11y_event_type type, cw_node n, const char *text, bool assertive) {
  ring_entry *r = &EV.ring[EV.seq % RING];
  EV.seq++;
  snprintf(r->text, sizeof r->text, "%s", text ? text : "");
  r->e = (cw_a11y_event){ type, EV.seq, n, r->text, assertive };
  if (EV.fn) EV.fn(&r->e, EV.user);
}

void cw_a11y_set_listener(cw_a11y_listener_fn fn, void *user) {
  EV.fn = fn;
  EV.user = user;
}
void cw_a11y_announce(const char *text, bool assertive) { push_event(CW_A11Y_EVENT_ANNOUNCE, (cw_node){ 0 }, text, assertive); }
uint32_t cw_a11y_event_seq(void) { return EV.seq; }

int cw_a11y_events_since(uint32_t since, cw_a11y_event *out, int cap) {
  uint32_t first = EV.seq > RING ? EV.seq - RING + 1 : 1;
  if (since + 1 > first) first = since + 1;
  int n = 0;
  for (uint32_t s = first; s <= EV.seq && n < cap; s++) out[n++] = EV.ring[(s - 1) % RING].e;
  return n;
}

void ui_a11y_focus_changed(cw_node n) { push_event(CW_A11Y_EVENT_FOCUS, n, "", false); }

void ui_a11y_value_changed(cw_node n, const char *signal) {
  static const char *const value_signals[] = { "value_changed", "toggled", "text_changed", "item_selected", "tab_changed",
                                               "color_changed", "remapped" };
  for (size_t i = 0; i < CW_ARRAY_LEN(value_signals); i++)
    if (!strcmp(signal, value_signals[i])) {
      push_event(CW_A11Y_EVENT_VALUE, n, signal, false);
      return;
    }
}

/* ── roles and text ─────────────────────────────────────────────────── */

static const char *const role_names[CW_ROLE_COUNT] = {
  "group", "panel", "label", "button", "togglebutton", "checkbox", "switch", "combobox", "slider", "spinbutton",
  "progressbar", "textfield", "textarea", "list", "listitem", "tree", "tablist", "tab", "menu", "menubar", "menuitem",
  "dialog", "scrollarea", "image", "colorpicker", "caption" };
const char *cw_a11y_role_name(cw_a11y_role r) { return (int)r >= 0 && r < CW_ROLE_COUNT ? role_names[r] : "group"; }

static bool prop_bool(cw_node n, const char *p) {
  cw_value v;
  return cw_node_find_prop(n, p, NULL) && cw_node_get_prop(n, p, &v) && v.type == CW_T_BOOL && v.b;
}
static const char *prop_str(cw_node n, const char *p) {
  cw_value v;
  if (cw_node_find_prop(n, p, NULL) && cw_node_get_prop(n, p, &v) && v.type == CW_T_STRING && v.s) return v.s;
  return "";
}
static double prop_num(cw_node n, const char *p) {
  cw_value v;
  if (!cw_node_find_prop(n, p, NULL) || !cw_node_get_prop(n, p, &v)) return 0;
  return v.type == CW_T_FLOAT ? v.f : v.type == CW_T_INT || v.type == CW_T_ENUM ? (double)v.i : 0;
}

/* -1: the control is transparent (its children belong to its parent entry) */
static int role_of(cw_node n) {
  if (cw_node_is(n, "AcceptDialog")) return CW_ROLE_DIALOG;
  if (cw_node_is(n, "PopupMenu")) return CW_ROLE_MENU;
  if (cw_node_is(n, "MenuBar")) return CW_ROLE_MENUBAR;
  if (cw_node_is(n, "OptionButton")) return CW_ROLE_COMBOBOX;
  if (cw_node_is(n, "CheckButton")) return CW_ROLE_SWITCH;
  if (cw_node_is(n, "CheckBox")) return CW_ROLE_CHECKBOX;
  if (cw_node_is(n, "Button")) return prop_bool(n, "toggle_mode") ? CW_ROLE_TOGGLE_BUTTON : CW_ROLE_BUTTON;
  if (cw_node_is(n, "SpinBox")) return CW_ROLE_SPINBUTTON;
  if (cw_node_is(n, "ProgressBar")) return CW_ROLE_PROGRESSBAR;
  if (cw_node_is(n, "HSlider") || cw_node_is(n, "VSlider")) return CW_ROLE_SLIDER;
  if (cw_node_is(n, "LineEdit")) return CW_ROLE_TEXTFIELD;
  if (cw_node_is(n, "TextEdit")) return CW_ROLE_TEXTAREA;
  if (cw_node_is(n, "ItemList")) return CW_ROLE_LIST;
  if (cw_node_is(n, "Tree")) return CW_ROLE_TREE;
  if (cw_node_is(n, "TabContainer")) return CW_ROLE_TABLIST;
  if (cw_node_is(n, "ColorPicker")) return CW_ROLE_COLORPICKER;
  if (cw_node_is(n, "CaptionPanel")) return CW_ROLE_CAPTION;
  if (cw_node_is(n, "Label") || cw_node_is(n, "RichTextLabel")) return CW_ROLE_LABEL;
  if (cw_node_is(n, "ScrollContainer")) return CW_ROLE_SCROLLAREA;
  bool named = *ui_s(CTL(n)->accessibility_name);
  if (cw_node_is(n, "TextureRect") || cw_node_is(n, "NinePatchRect")) return named ? CW_ROLE_IMAGE : -1; /* decorative */
  if (cw_node_is(n, "Panel") || cw_node_is(n, "PanelContainer")) return named ? CW_ROLE_PANEL : -1;
  return named ? CW_ROLE_GROUP : -1;
}

/* BBCode tags out (RichTextLabel) */
static const char *plain_text(const char *s) {
  size_t n = strlen(s);
  char *o = cw_frame_alloc(n + 1);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '[') {
      const char *e = strchr(s + i, ']');
      if (e && (s[i + 1] == '/' || (s[i + 1] >= 'a' && s[i + 1] <= 'z'))) {
        i = (size_t)(e - s);
        continue;
      }
    }
    o[k++] = s[i];
  }
  o[k] = 0;
  return o;
}

/* the text of a visible Label right before n among its siblings (form rows) */
static const char *label_before(cw_node n) {
  cw_node p = cw_node_parent(n);
  int i = cw_node_index(n);
  for (int k = i - 1; k >= 0 && !CW_HANDLE_IS_NULL(p); k--) {
    cw_node s = cw_node_child(p, k);
    cw_control *c = CTL(s);
    if (!c || !c->visible) continue;
    if (cw_node_is(s, "Label")) return ui_tr(s, cw_label_text(s));
    break;
  }
  return "";
}

static const char *fmt_num(double v, double step) {
  int dec = 0;
  for (double s = step; s > 0 && s < 1 && dec < 4; s *= 10) dec++;
  return cw_l10n_format_number(v, dec);
}

static void describe(cw_node n, int role, cw_a11y_node *o) {
  cw_control *c = CTL(n);
  o->name = "";
  o->value = "";
  if (*ui_s(c->accessibility_name)) o->name = ui_tr(n, c->accessibility_name);
  else if (role == CW_ROLE_LABEL) o->name = cw_node_is(n, "RichTextLabel") ? plain_text(ui_tr(n, prop_str(n, "text"))) : ui_tr(n, cw_label_text(n));
  else if (role == CW_ROLE_BUTTON || role == CW_ROLE_TOGGLE_BUTTON || role == CW_ROLE_CHECKBOX || role == CW_ROLE_SWITCH)
    o->name = ui_tr(n, cw_button_text(n));
  else if (role == CW_ROLE_DIALOG) o->name = ui_tr(n, prop_str(n, "title"));
  if (!*o->name && role != CW_ROLE_LABEL && role != CW_ROLE_GROUP && role != CW_ROLE_PANEL) o->name = label_before(n);
  if (!*o->name && role == CW_ROLE_TEXTFIELD) o->name = ui_tr(n, prop_str(n, "placeholder_text"));
  if (cw_node_is(n, "RemapButton")) { /* named by its action's label; the binding is the value */
    o->value = cw_button_text(n);
    if (!*ui_s(c->accessibility_name)) {
      const char *l = label_before(n);
      o->name = *l ? l : o->value;
    }
  }
  const char *d = *ui_s(c->accessibility_description) ? c->accessibility_description : ui_s(c->tooltip_text);
  o->description = *d ? ui_tr(n, d) : "";
  switch (role) {
  case CW_ROLE_SLIDER: case CW_ROLE_SPINBUTTON: case CW_ROLE_PROGRESSBAR: {
    o->value_now = cw_range_value(n);
    o->value_min = prop_num(n, "min_value");
    o->value_max = prop_num(n, "max_value");
    double step = prop_num(n, "step");
    if (role == CW_ROLE_PROGRESSBAR && o->value_max > o->value_min) {
      double pct = (o->value_now - o->value_min) / (o->value_max - o->value_min) * 100.0;
      o->value = cw_frame_printf("%s%%", cw_l10n_format_number(pct, 0));
    } else o->value = fmt_num(o->value_now, step);
    break;
  }
  case CW_ROLE_TEXTFIELD:
    o->value = cw_line_edit_text(n);
    if (prop_bool(n, "secret")) {
      int k = ui_utf8_count(o->value, (int)strlen(o->value));
      char *b = cw_frame_alloc((size_t)k * 3 + 1);
      for (int i = 0; i < k; i++) memcpy(b + 3 * i, "\xE2\x80\xA2", 3);
      b[k * 3] = 0;
      o->value = b;
    }
    break;
  case CW_ROLE_TEXTAREA: o->value = cw_text_edit_text(n); break;
  case CW_ROLE_COMBOBOX: {
    int sel = cw_option_button_selected(n);
    /* the shown text: the selected item, translated */
    const char *items = prop_str(n, "items");
    for (int k = 0; sel >= 0 && items; k++) {
      const char *e = strchr(items, '\n');
      if (k == sel) {
        o->value = ui_tr(n, cw_intern(cw_frame_printf("%.*s", e ? (int)(e - items) : (int)strlen(items), items)));
        break;
      }
      items = e ? e + 1 : NULL;
    }
    break;
  }
  case CW_ROLE_TABLIST: {
    int cur = cw_tab_container_current(n);
    const char *t = cw_tab_container_title(n, cur);
    o->value = t ? t : "";
    break;
  }
  case CW_ROLE_LIST: {
    int sel = cw_item_list_selected(n);
    if (sel >= 0) o->value = ui_tr(n, cw_item_list_text(n, sel));
    break;
  }
  case CW_ROLE_TREE: {
    int sel = cw_uitree_selected(n);
    if (sel >= 0) o->value = cw_uitree_text(n, sel, 0);
    break;
  }
  default: break;
  }
  uint32_t st = 0;
  bool disabled = prop_bool(n, "disabled");
  if (c->focus_mode != CW_FOCUS_NONE && !disabled) st |= CW_A11Y_FOCUSABLE;
  if (cw_control_has_focus(n)) st |= CW_A11Y_FOCUSED;
  if (disabled) st |= CW_A11Y_DISABLED;
  if (role == CW_ROLE_CHECKBOX || role == CW_ROLE_SWITCH || role == CW_ROLE_TOGGLE_BUTTON) {
    st |= CW_A11Y_CHECKABLE;
    if (cw_button_pressed(n)) st |= CW_A11Y_CHECKED;
  }
  if (role == CW_ROLE_COMBOBOX || role == CW_ROLE_MENUBAR) st |= CW_A11Y_HAS_POPUP;
  if (role == CW_ROLE_TEXTFIELD && !prop_bool(n, "editable")) st |= CW_A11Y_READONLY;
  if (role == CW_ROLE_TEXTFIELD && prop_bool(n, "secret")) st |= CW_A11Y_PASSWORD;
  if (role == CW_ROLE_TEXTAREA) st |= CW_A11Y_MULTILINE | (prop_bool(n, "editable") ? 0 : CW_A11Y_READONLY);
  if (role == CW_ROLE_DIALOG) st |= CW_A11Y_MODAL;
  if (cw_control_is_rtl(n)) st |= CW_A11Y_RTL;
  o->states = st;
  o->bounds = cw_control_global_rect(n);
}

/* ── the walk ───────────────────────────────────────────────────────── */

typedef struct { cw_a11y_node *out; int cap, n; } snap;

static int add(snap *s, const cw_a11y_node *e) {
  if (s->n < s->cap) s->out[s->n] = *e;
  return s->n++;
}

static void add_items(snap *s, cw_node n, int role, int parent) {
  uint64_t uid = cw_node_uid(n);
  if (role == CW_ROLE_LIST) {
    int k = cw_item_list_count(n);
    for (int i = 0; i < k; i++) {
      cw_a11y_node e = { .id = uid * 4096 + (uint64_t)i + 1, .parent = parent, .role = CW_ROLE_LISTITEM, .node = n, .item = i,
                         .name = ui_tr(n, cw_item_list_text(n, i)), .value = "", .description = "",
                         .bounds = cw_control_global_rect(n) };
      if (cw_item_list_is_selected(n, i)) e.states |= CW_A11Y_SELECTED;
      add(s, &e);
    }
  } else if (role == CW_ROLE_TABLIST) {
    int k = cw_tab_container_page_count(n), cur = cw_tab_container_current(n);
    cw_rect g = cw_control_global_rect(n);
    for (int i = 0; i < k; i++) {
      cw_rect r = cw_tab_container_tab_rect(n, i);
      const char *t = cw_tab_container_title(n, i);
      cw_a11y_node e = { .id = uid * 4096 + (uint64_t)i + 1, .parent = parent, .role = CW_ROLE_TAB, .node = n, .item = i,
                         .name = t ? t : "", .value = "", .description = "",
                         .bounds = { g.x + r.x, g.y + r.y, r.w, r.h } };
      if (i == cur) e.states |= CW_A11Y_SELECTED;
      add(s, &e);
    }
  } else if (role == CW_ROLE_MENU) {
    int k = cw_popup_menu_item_count(n);
    for (int i = 0; i < k; i++) {
      const char *t = cw_popup_menu_item_text(n, i);
      if (!t || !*t) continue; /* separators */
      cw_a11y_node e = { .id = uid * 4096 + (uint64_t)i + 1, .parent = parent, .role = CW_ROLE_MENUITEM, .node = n, .item = i,
                         .name = ui_tr(n, t), .value = "", .description = "", .bounds = cw_control_global_rect(n) };
      if (cw_popup_menu_checked(n, i)) e.states |= CW_A11Y_CHECKABLE | CW_A11Y_CHECKED;
      add(s, &e);
    }
  }
}

static void walk(snap *s, ui_tree_state *ts, cw_node n, int parent) {
  cw_control *c = CTL(n);
  if (!c || !c->visible) return;
  int role = role_of(n), me = parent;
  if (role >= 0) {
    cw_a11y_node e = { .id = cw_node_uid(n), .parent = parent, .role = (cw_a11y_role)role, .node = n, .item = -1 };
    describe(n, role, &e);
    if (role == CW_ROLE_COMBOBOX)
      for (int i = 0; i < ts->popups.count; i++)
        if (ts->popups.data[i].owner.id == n.id) e.states |= CW_A11Y_EXPANDED;
    me = add(s, &e);
    add_items(s, n, role, me);
  }
  int k = cw_node_child_count(n);
  for (int i = 0; i < k; i++) {
    cw_node ch = cw_node_child(n, i);
    cw_control *cc = CTL(ch);
    if (cc && !(cc->_flags & CF_TOP_LEVEL)) walk(s, ts, ch, me);
  }
}

int cw_a11y_snapshot(cw_tree *t, cw_a11y_node *out, int cap) {
  if (!t || !UI.inited) return 0;
  ui_tree_state *ts = ui_state(t);
  ui_layout_tree(ts);
  snap s = { out, cap, 0 };
  for (int i = 0; i < ts->roots.count; i++) walk(&s, ts, ts->roots.data[i], -1);
  for (int i = 0; i < ts->popups.count; i++) walk(&s, ts, ts->popups.data[i].node, -1);
  return s.n;
}

/* ── JSON ───────────────────────────────────────────────────────────── */

typedef CW_ARRAY(char) sbuf;
static void put(sbuf *b, const char *s) {
  size_t n = strlen(s);
  cw_array_reserve(b, b->count + (int)n + 1);
  memcpy(b->data + b->count, s, n);
  b->count += (int)n;
  b->data[b->count] = 0;
}
static void put_str(sbuf *b, const char *s) {
  put(b, "\"");
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    char t[8];
    if (*p == '"' || *p == '\\') t[0] = '\\', t[1] = (char)*p, t[2] = 0;
    else if (*p == '\n') strcpy(t, "\\n");
    else if (*p < 0x20) snprintf(t, sizeof t, "\\u%04x", *p);
    else t[0] = (char)*p, t[1] = 0;
    put(b, t);
  }
  put(b, "\"");
}

static const char *const state_names[] = { "focusable", "focused", "disabled", "checkable", "checked", "selected",
                                           "expanded", "haspopup", "readonly", "multiline", "password", "modal", "rtl" };

static void node_json(sbuf *b, const cw_a11y_node *e) {
  char tmp[256];
  snprintf(tmp, sizeof tmp, "{\"id\":%llu,\"parent\":%d,\"role\":\"%s\",\"name\":", (unsigned long long)e->id, e->parent,
           cw_a11y_role_name(e->role));
  put(b, tmp);
  put_str(b, e->name ? e->name : "");
  if (e->value && *e->value) {
    put(b, ",\"value\":");
    put_str(b, e->value);
  }
  if (e->description && *e->description) {
    put(b, ",\"description\":");
    put_str(b, e->description);
  }
  if (e->role == CW_ROLE_SLIDER || e->role == CW_ROLE_SPINBUTTON || e->role == CW_ROLE_PROGRESSBAR) {
    snprintf(tmp, sizeof tmp, ",\"range\":[%g,%g,%g]", e->value_min, e->value_now, e->value_max);
    put(b, tmp);
  }
  put(b, ",\"states\":[");
  int k = 0;
  for (int i = 0; i < (int)CW_ARRAY_LEN(state_names); i++)
    if (e->states & (1u << i)) {
      if (k++) put(b, ",");
      put_str(b, state_names[i]);
    }
  char path[512] = "";
  cw_node_path(e->node, (cw_node){ 0 }, path, sizeof path);
  snprintf(tmp, sizeof tmp, "],\"bounds\":[%g,%g,%g,%g],\"node\":", (double)e->bounds.x, (double)e->bounds.y,
           (double)e->bounds.w, (double)e->bounds.h);
  put(b, tmp);
  put_str(b, path);
  if (e->item >= 0) {
    snprintf(tmp, sizeof tmp, ",\"item\":%d", e->item);
    put(b, tmp);
  }
  put(b, "}");
}

char *cw_a11y_json(cw_tree *t) {
  int n = cw_a11y_snapshot(t, NULL, 0);
  cw_a11y_node *nodes = cw_frame_alloc(sizeof *nodes * (size_t)(n + 1));
  n = cw_a11y_snapshot(t, nodes, n);
  sbuf b = { 0 };
  put(&b, "{\"nodes\":[");
  for (int i = 0; i < n; i++) {
    if (i) put(&b, ",");
    node_json(&b, &nodes[i]);
  }
  char tail[96];
  snprintf(tail, sizeof tail, "],\"count\":%d,\"event_seq\":%u}", n, EV.seq);
  put(&b, tail);
  return b.data;
}

/* ── the debug command ──────────────────────────────────────────────── */

static void reply_owned(cw_reply *r, char *json) {
  cw_reply_raw(r, json);
  cw_free(json);
}

static void cmd_a11y(int argc, const char **argv, cw_reply *r) {
  cw_tree *t = cw_current_tree();
  const char *sub = argc >= 2 ? argv[1] : "tree";
  if (!strcmp(sub, "tree")) {
    if (!t) {
      cw_reply_error(r, "no scene tree (the accessibility tree describes the current tree's Control nodes)");
      return;
    }
    reply_owned(r, cw_a11y_json(t));
    return;
  }
  if (!strcmp(sub, "focus")) {
    cw_node f = cw_ui_focus_owner();
    int n = t ? cw_a11y_snapshot(t, NULL, 0) : 0;
    cw_a11y_node *nodes = cw_frame_alloc(sizeof *nodes * (size_t)(n + 1));
    n = t ? cw_a11y_snapshot(t, nodes, n) : 0;
    for (int i = 0; i < n; i++)
      if (nodes[i].node.id == f.id && nodes[i].item < 0) {
        sbuf b = { 0 };
        put(&b, "{\"focus\":");
        node_json(&b, &nodes[i]);
        put(&b, "}");
        reply_owned(r, b.data);
        return;
      }
    cw_reply_raw(r, "{\"focus\":null}");
    return;
  }
  if (!strcmp(sub, "events")) {
    uint32_t since = argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
    cw_a11y_event ev[RING];
    int n = cw_a11y_events_since(since, ev, RING);
    sbuf b = { 0 };
    put(&b, "{\"events\":[");
    for (int i = 0; i < n; i++) {
      static const char *const tn[] = { "focus", "value", "announce" };
      char tmp[160], path[512] = "";
      if (cw_node_valid(ev[i].node)) cw_node_path(ev[i].node, (cw_node){ 0 }, path, sizeof path);
      snprintf(tmp, sizeof tmp, "%s{\"seq\":%u,\"type\":\"%s\",\"node\":", i ? "," : "", ev[i].seq, tn[ev[i].type]);
      put(&b, tmp);
      put_str(&b, path);
      put(&b, ",\"text\":");
      put_str(&b, ev[i].text ? ev[i].text : "");
      if (ev[i].assertive) put(&b, ",\"assertive\":true");
      put(&b, "}");
    }
    char tail[64];
    snprintf(tail, sizeof tail, "],\"seq\":%u}", EV.seq);
    put(&b, tail);
    reply_owned(r, b.data);
    return;
  }
  if (!strcmp(sub, "announce") && argc >= 3) {
    cw_a11y_announce(argv[2], argc >= 4 && !strcmp(argv[3], "assertive"));
    cw_reply_printf(r, "{\"seq\":%u}", EV.seq);
    return;
  }
  if (!strcmp(sub, "set") && argc >= 4) {
    const char *k = argv[2], *v = argv[3];
    if (!strcmp(k, "text_scale")) cw_ui_set_text_scale((float)atof(v));
    else if (!strcmp(k, "reduced_motion")) cw_ui_set_reduced_motion(atoi(v) != 0);
    else if (!strcmp(k, "captions")) cw_captions_set_enabled(atoi(v) != 0);
    else if (!strcmp(k, "theme")) {
      cw_theme th = !strcmp(v, "high_contrast") ? cw_theme_high_contrast() : !strcmp(v, "colorblind") ? cw_theme_colorblind()
                  : !strcmp(v, "light") ? cw_theme_default_light() : cw_theme_default_dark();
      cw_ui_set_theme(th);
    } else {
      cw_reply_error(r, "unknown setting '%s' (text_scale, reduced_motion, captions, theme)", k);
      return;
    }
    cw_reply_printf(r, "{\"text_scale\":%g,\"reduced_motion\":%s,\"captions\":%s}", (double)cw_ui_text_scale(),
                    cw_ui_reduced_motion() ? "true" : "false", cw_captions_enabled() ? "true" : "false");
    return;
  }
  cw_reply_error(r, "usage: a11y [tree | focus | events [since] | announce <text> [assertive] | set <text_scale|"
                    "reduced_motion|captions|theme> <value>]");
}

void ui_a11y_init(void) {
  cw_debug_command("a11y", "a11y [tree | focus | events [since] | announce <text> | set <key> <value>]: the UI's "
                           "accessibility tree (role, name, value, states, bounds per control), the focused entry, the "
                           "focus/value/announcement event log a screen-reader bridge follows, accessibility settings",
                   cmd_a11y);
}

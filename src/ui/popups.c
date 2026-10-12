/* PopupMenu (with submenus), MenuBar, AcceptDialog, ConfirmationDialog. */
#include "ui_internal.h"

/* ── PopupMenu ──────────────────────────────────────────────────────── */

typedef struct {
  const char *title; /* MenuBar title (else the node name) */
  CW_ARRAY(ui_menu_item) items;
  int hover;
  bool pressed_inside;
  cw_node open_sub;
} menu_data;
static const cw_node_type *menu_t;
static const cw_node_type *menubar_t;

static menu_data *MD(cw_node n) { return cw_node_data_t(n, menu_t); }

static float item_h(cw_node n) {
  cw_ui_style it = T_STYLE(n, "item");
  return ceilf(cw_ui_line_height(T_FONT(n), T_FSIZE(n)) + SM_H(&it));
}
static float sep_h(cw_node n) {
  cw_ui_style it = T_STYLE(n, "item");
  return ceilf(SM_H(&it) * 0.5f + 1);
}
static float check_w(cw_node n) { return ceilf(cw_ui_line_height(T_FONT(n), T_FSIZE(n)) * 0.8f); }

static cw_vec2 menu_min(cw_node n, void *d) {
  menu_data *m = d;
  cw_ui_style p = T_STYLE(n, "panel"), it = T_STYLE(n, "item");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), w = 0, h = 0, sep = T_CONST(n, "h_separation"), sw = 0;
  bool any_check = false, any_sub = false;
  for (int i = 0; i < m->items.count; i++) {
    ui_menu_item *e = &m->items.data[i];
    if (e->separator) {
      h += sep_h(n);
      continue;
    }
    h += item_h(n);
    w = CW_MAX(w, cw_ui_text_width(f, fs, ui_tr(n, e->label)));
    if (e->shortcut) sw = CW_MAX(sw, cw_ui_text_width(f, fs, e->shortcut));
    any_check |= e->checkable;
    any_sub |= e->submenu != NULL;
  }
  if (any_check) w += check_w(n) + sep;
  if (any_sub) w += check_w(n) + sep;
  if (sw > 0) w += sw + sep * 3;
  return (cw_vec2){ ceilf(w + SM_W(&it) + SM_W(&p)), ceilf(h + SM_H(&p)) };
}

static float item_y(cw_node n, menu_data *m, int idx, float *h) {
  cw_ui_style p = T_STYLE(n, "panel");
  float y = p.margin[1];
  for (int i = 0; i < m->items.count; i++) {
    float ih = m->items.data[i].separator ? sep_h(n) : item_h(n);
    if (i == idx) {
      *h = ih;
      return y;
    }
    y += ih;
  }
  *h = 0;
  return y;
}

static int item_at(cw_node n, menu_data *m, cw_vec2 pos) {
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel");
  if (pos.x < 0 || pos.x >= c->rect.w) return -1;
  for (int i = 0; i < m->items.count; i++) {
    float h, y = item_y(n, m, i, &h);
    if (pos.y >= y && pos.y < y + h) return m->items.data[i].separator || m->items.data[i].disabled ? -1 : i;
  }
  (void)p;
  return -1;
}

static void menu_draw(cw_node n, void *d) {
  menu_data *m = d;
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel"), it = T_STYLE(n, "item"), hv = T_STYLE(n, "hover");
  cw_ui_draw_style(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), sep = T_CONST(n, "h_separation"), cwid = check_w(n);
  bool any_check = false;
  for (int i = 0; i < m->items.count; i++) any_check |= m->items.data[i].checkable;
  for (int i = 0; i < m->items.count; i++) {
    ui_menu_item *e = &m->items.data[i];
    float h, y = item_y(n, m, i, &h);
    cw_rect r = { p.margin[0], y, c->rect.w - SM_W(&p), h };
    if (e->separator) {
      cw_ui_draw_rect((cw_rect){ r.x + it.margin[0] * 0.5f, ui_round(y + h * 0.5f), r.w - it.margin[0], 1 }, T_COLOR(n, "separator_color"));
      continue;
    }
    bool hot = i == m->hover && !e->disabled;
    if (hot) cw_ui_draw_style(&hv, r);
    cw_color fc = T_COLOR(n, e->disabled ? "font_disabled_color" : hot ? "font_hover_color" : "font_color");
    cw_rect tr = style_content(&it, r);
    if (any_check) {
      if (e->checkable && e->checked) cw_ui_draw_icon(CW_ICON_CHECK, (cw_rect){ tr.x, tr.y + (tr.h - cwid) * 0.5f, cwid, cwid }, fc);
      tr.x += cwid + sep, tr.w -= cwid + sep;
    }
    if (e->submenu) {
      cw_ui_draw_icon(CW_ICON_ARROW_RIGHT, (cw_rect){ tr.x + tr.w - cwid, tr.y + (tr.h - cwid) * 0.5f, cwid, cwid }, fc);
      tr.w -= cwid + sep;
    }
    if (e->shortcut) {
      cw_color sc = hot ? fc : T_COLOR(n, "font_shortcut_color");
      float sw = cw_ui_text_width(f, fs, e->shortcut);
      cw_ui_draw_text_in(f, fs, e->shortcut, (cw_rect){ tr.x + tr.w - sw, tr.y, sw, tr.h }, CW_HALIGN_RIGHT, sc, false);
      tr.w -= sw + sep * 3;
    }
    cw_ui_draw_text_in(f, fs, ui_tr(n, e->label), tr, CW_HALIGN_LEFT, fc, true);
  }
}

static cw_node find_submenu(cw_node n, const char *name) {
  int k = cw_node_child_count(n);
  for (int i = 0; i < k; i++) {
    cw_node ch = cw_node_child(n, i);
    if (cw_node_name(ch) == name || !strcmp(cw_node_name(ch), name)) return ch;
  }
  return (cw_node){ 0 };
}

static void close_menus(cw_node n) {
  /* the whole chain: walk to the root menu through popup owners */
  ui_tree_state *s = ui_state_of(n);
  if (!s) return;
  for (int i = s->popups.count - 1; i >= 0; i--)
    if (i < s->popups.count && s->popups.data[i].close_outside) ui_popup_close(s->popups.data[i].node);
}

static void open_sub(cw_node n, menu_data *m, int i) {
  ui_menu_item *e = &m->items.data[i];
  if (cw_node_valid(m->open_sub) && cw_popup_visible(m->open_sub)) {
    if (e->submenu && cw_node_name(m->open_sub) == e->submenu) return;
    cw_popup_hide(m->open_sub);
  }
  m->open_sub = (cw_node){ 0 };
  if (!e->submenu) return;
  cw_node sub = find_submenu(n, e->submenu);
  if (!cw_node_valid(sub) || !MD(sub)) return;
  cw_rect g = cw_control_global_rect(n);
  float h, y = item_y(n, m, i, &h);
  cw_ui_style p = T_STYLE(n, "panel");
  cw_vec2 sm = cw_control_min_size(sub);
  cw_vec2 cs = cw_ui_canvas_size();
  float x = g.x + g.w - p.margin[2];
  if (x + sm.x > cs.x) x = g.x - sm.x + p.margin[0];
  float yy = g.y + y - T_STYLE(sub, "panel").margin[1];
  if (yy + sm.y > cs.y) yy = cs.y - sm.y;
  cw_control *sc = CTL(sub);
  sc->rect = (cw_rect){ x, CW_MAX(0, yy), sm.x, sm.y };
  ui_popup_open(sub, false, true, n);
  MD(sub)->hover = -1;
  m->open_sub = sub;
}

static void activate(cw_node n, menu_data *m, int i) {
  ui_menu_item *e = &m->items.data[i];
  if (e->disabled || e->separator) return;
  if (e->submenu) {
    open_sub(n, m, i);
    if (cw_node_valid(m->open_sub)) MD(m->open_sub)->hover = 0;
    return;
  }
  if (e->checkable) e->checked = !e->checked;
  int id = e->id;
  close_menus(n);
  ui_emit_int(n, "id_pressed", id);
}

void ui_menubar_step(cw_node bar, int dir);

static int step_item(menu_data *m, int from, int dir) {
  int k = m->items.count;
  for (int s = 1; s <= k; s++) {
    int i = ((from < 0 ? (dir > 0 ? -1 : 0) : from) + dir * s + k * 4) % k;
    if (!m->items.data[i].separator && !m->items.data[i].disabled) return i;
  }
  return -1;
}

static bool menu_input(cw_node n, void *d, const cw_ui_event *e) {
  menu_data *m = d;
  switch (e->type) {
  case CW_UI_MOUSE_MOVE: {
    int i = item_at(n, m, e->pos);
    if (i != m->hover && i >= 0) {
      m->hover = i;
      open_sub(n, m, i);
    }
    return true;
  }
  case CW_UI_MOUSE_DOWN:
    m->pressed_inside = true;
    return true;
  case CW_UI_MOUSE_UP: {
    if (!m->pressed_inside) return true;
    m->pressed_inside = false;
    int i = item_at(n, m, e->pos);
    if (i >= 0) activate(n, m, i);
    return true;
  }
  case CW_UI_NAV: {
    if (!m->items.count) return true;
    cw_control *c = CTL(n);
    switch (e->nav) {
    case CW_NAV_UP: m->hover = step_item(m, m->hover, -1); return true;
    case CW_NAV_DOWN: m->hover = step_item(m, m->hover, 1); return true;
    case CW_NAV_RIGHT:
      if (m->hover >= 0 && m->items.data[m->hover].submenu) {
        activate(n, m, m->hover);
        return true;
      }
      if (cw_node_valid(c->_owner) && cw_node_data_t(c->_owner, menubar_t)) ui_menubar_step(c->_owner, 1);
      return true;
    case CW_NAV_LEFT:
      if (cw_node_valid(c->_owner) && MD(c->_owner)) {
        cw_popup_hide(n);
        return true;
      }
      if (cw_node_valid(c->_owner) && cw_node_data_t(c->_owner, menubar_t)) ui_menubar_step(c->_owner, -1);
      return true;
    case CW_NAV_ACCEPT:
      if (m->hover >= 0) activate(n, m, m->hover);
      return true;
    case CW_NAV_CANCEL: cw_popup_hide(n); return true;
    default: return true;
    }
  }
  case CW_UI_KEY: return false; /* becomes NAV */
  default: return true;
  }
}

static ui_menu_item *add(cw_node n, const char *label, int id) {
  menu_data *m = MD(n);
  if (!m) return NULL;
  ui_menu_item it = { .label = cw_strdup(label ? label : "", CW_MEM_UI), .id = id < 0 ? m->items.count : id };
  cw_array_push(&m->items, it);
  ui_mark_min_dirty(n);
  return &m->items.data[m->items.count - 1];
}
int cw_popup_menu_add_item(cw_node n, const char *label, int id) {
  return add(n, label, id) ? MD(n)->items.count - 1 : -1;
}
int cw_popup_menu_add_check_item(cw_node n, const char *label, int id) {
  ui_menu_item *it = add(n, label, id);
  if (!it) return -1;
  it->checkable = true;
  return MD(n)->items.count - 1;
}
void cw_popup_menu_add_separator(cw_node n) {
  ui_menu_item *it = add(n, "", -1);
  if (it) it->separator = true;
}
int cw_popup_menu_add_submenu(cw_node n, const char *label, const char *child) {
  ui_menu_item *it = add(n, label, -1);
  if (!it) return -1;
  it->submenu = cw_intern(child);
  return MD(n)->items.count - 1;
}
void cw_popup_menu_set_checked(cw_node n, int i, bool c) {
  menu_data *m = MD(n);
  if (m && i >= 0 && i < m->items.count) m->items.data[i].checked = c, m->items.data[i].checkable = true;
}
bool cw_popup_menu_checked(cw_node n, int i) {
  menu_data *m = MD(n);
  return m && i >= 0 && i < m->items.count && m->items.data[i].checked;
}
void cw_popup_menu_set_shortcut(cw_node n, int i, const char *text) {
  menu_data *m = MD(n);
  if (!m || i < 0 || i >= m->items.count) return;
  m->items.data[i].shortcut = text && *text ? cw_intern(text) : NULL;
  ui_mark_min_dirty(n);
}
const char *cw_popup_menu_item_text(cw_node n, int i) {
  menu_data *m = MD(n);
  return m && i >= 0 && i < m->items.count ? m->items.data[i].label : NULL;
}
int cw_popup_menu_item_id(cw_node n, int i) {
  menu_data *m = MD(n);
  return m && i >= 0 && i < m->items.count ? m->items.data[i].id : -1;
}
void cw_popup_menu_set_disabled(cw_node n, int i, bool v) {
  menu_data *m = MD(n);
  if (m && i >= 0 && i < m->items.count) m->items.data[i].disabled = v;
}
int cw_popup_menu_item_count(cw_node n) {
  menu_data *m = MD(n);
  return m ? m->items.count : 0;
}
void cw_popup_menu_clear(cw_node n) {
  menu_data *m = MD(n);
  if (!m) return;
  for (int i = 0; i < m->items.count; i++) cw_free(m->items.data[i].label);
  cw_array_clear(&m->items);
  m->hover = -1;
  ui_mark_min_dirty(n);
}

static void popup_at(cw_node n, cw_vec2 at, cw_node owner) {
  cw_control *c = CTL(n);
  cw_vec2 sz = cw_control_min_size(n);
  cw_vec2 cs = cw_ui_canvas_size();
  if (at.x + sz.x > cs.x) at.x = CW_MAX(0, cs.x - sz.x);
  if (at.y + sz.y > cs.y) at.y = CW_MAX(0, cs.y - sz.y);
  c->rect = (cw_rect){ at.x, at.y, sz.x, sz.y };
  MD(n)->hover = -1;
  MD(n)->pressed_inside = false;
  ui_popup_open(n, false, true, owner);
}
void cw_popup_menu_popup(cw_node n, cw_vec2 at) {
  if (!MD(n)) return;
  cw_control *c = CTL(n);
  popup_at(n, at, (c->_flags & CF_INTERNAL) ? c->_owner : (cw_node){ 0 });
}

static void get_items(cw_node n, void *d, cw_value *o) {
  (void)n;
  menu_data *m = d;
  size_t cap = 64;
  for (int i = 0; i < m->items.count; i++) cap += strlen(m->items.data[i].label) + 48;
  char *s = cw_frame_alloc(cap), *p = s;
  *p = 0;
  for (int i = 0; i < m->items.count; i++) {
    ui_menu_item *e = &m->items.data[i];
    if (i) *p++ = '\n';
    if (e->separator) { *p++ = '-'; *p = 0; continue; }
    p += sprintf(p, "%s%s%s", e->disabled ? "!" : "", e->checkable ? (e->checked ? "[x]" : "[ ]") : "", e->label);
    if (e->submenu) p += sprintf(p, ">%s", e->submenu);
    else if (e->id != i) p += sprintf(p, "|%d", e->id);
  }
  *o = cw_val_string(s);
}
static void set_items(cw_node n, void *d, const cw_value *v) {
  (void)d;
  cw_popup_menu_clear(n);
  const char *s = v->s ? v->s : "";
  while (*s) {
    const char *e = strchr(s, '\n');
    char line[512];
    snprintf(line, sizeof line, "%.*s", e ? (int)(e - s) : (int)strlen(s), s);
    char *l = line;
    if (!strcmp(l, "-")) cw_popup_menu_add_separator(n);
    else if (*l) {
      bool dis = *l == '!';
      if (dis) l++;
      int check = !strncmp(l, "[x]", 3) ? 2 : !strncmp(l, "[ ]", 3) ? 1 : 0;
      if (check) l += 3;
      char *sub = strchr(l, '>'), *bar = strchr(l, '|');
      int id = -1;
      if (sub) *sub++ = 0;
      if (bar) *bar = 0, id = atoi(bar + 1);
      int at = sub ? cw_popup_menu_add_submenu(n, l, sub) : check ? cw_popup_menu_add_check_item(n, l, id) : cw_popup_menu_add_item(n, l, id);
      if (check == 2) cw_popup_menu_set_checked(n, at, true);
      if (dis) cw_popup_menu_set_disabled(n, at, true);
    }
    if (!e) break;
    s = e + 1;
  }
}

static void popup_init(cw_node n, void *d) {
  (void)d;
  cw_control *c = CTL(n);
  c->_flags |= CF_TOP_LEVEL;
  c->visible = false;
}
static void menu_init(cw_node n, void *d) {
  ((menu_data *)d)->hover = -1;
  (void)n;
}
static void menu_destroy(cw_node n, void *d) {
  menu_data *m = d;
  cw_popup_menu_clear(n);
  cw_array_free(&m->items);
}
static void menu_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
  cw_node par = cw_node_parent(n);
  if (cw_node_valid(par) && cw_node_data_t(par, menubar_t)) ui_mark_min_dirty(par);
}

static const cw_prop menu_props[] = {
  CW_PROP_FIELD(menu_data, title, CW_T_STRING, CW_PROP_DEFAULT, "Title in a MenuBar (default: the node name)."),
  CW_PROP_ACCESSOR("items", CW_T_STRING, CW_PROP_DEFAULT, get_items, set_items,
                   "One item per line: 'Label|id', '-' separator, '[x]'/'[ ]' check prefix, '!' disabled, 'Label>ChildMenu' submenu."),
  CW_PROP_END
};
static const char *const menu_signals[] = { "id_pressed", NULL };
static cw_node_type popup_type = { .name = "Popup", .parent = "Control", .init = popup_init,
                                   .doc = "Base of controls shown above everything (menus, dialogs)." };
static cw_node_type menu_type = { .name = "PopupMenu", .parent = "Popup", .data_size = sizeof(menu_data), .props = menu_props,
                                  .signals = menu_signals, .init = menu_init, .destroy = menu_destroy,
                                  .prop_changed = menu_changed, .doc = "A menu of items, checks, separators and submenus." };

/* ── PopupPanel ────────────────────────────────────────────────────── */

static cw_vec2 ppanel_min(cw_node n, void *d) {
  (void)d;
  cw_ui_style p = T_STYLE(n, "panel");
  cw_node *ch;
  int k = ui_children(n, &ch);
  cw_vec2 m = { 0, 0 };
  for (int i = 0; i < k; i++) {
    cw_vec2 c = cw_control_min_size(ch[i]);
    m.x = CW_MAX(m.x, c.x);
    m.y = CW_MAX(m.y, c.y);
  }
  return (cw_vec2){ ceilf(m.x + SM_W(&p)), ceilf(m.y + SM_H(&p)) };
}
static void ppanel_layout(cw_node n, void *d, cw_vec2 s) {
  (void)d;
  cw_ui_style p = T_STYLE(n, "panel");
  cw_node *ch;
  int k = ui_children(n, &ch);
  for (int i = 0; i < k; i++) cw_control_fit_child(ch[i], style_content(&p, (cw_rect){ 0, 0, s.x, s.y }));
}
static void ppanel_draw(cw_node n, void *d) {
  (void)d;
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel");
  cw_ui_draw_style(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
}
static bool ppanel_input(cw_node n, void *d, const cw_ui_event *e) {
  (void)n, (void)d;
  return e->type == CW_UI_MOUSE_DOWN || e->type == CW_UI_MOUSE_UP; /* clicks on the panel itself stay inside */
}
static cw_node_type ppanel_type = { .name = "PopupPanel", .parent = "Popup",
                                    .doc = "A popup that draws a panel and fits its children inside it (cw_popup_show)." };

void cw_popup_show(cw_node n, cw_rect r, bool modal) {
  cw_control *c = CTL(n);
  if (!c || !cw_node_is(n, "Popup")) return;
  c->_flags |= CF_MIN_DIRTY;
  cw_vec2 m = cw_control_min_size(n);
  cw_vec2 cs = cw_ui_canvas_size();
  float w = ui_round(CW_MAX(r.w, m.x)), h = ui_round(CW_MAX(r.h, m.y));
  float x = CW_CLAMP(r.x, 0, CW_MAX(0, cs.x - w)), y = CW_CLAMP(r.y, 0, CW_MAX(0, cs.y - h));
  c->rect = (cw_rect){ ui_round(x), ui_round(y), w, h };
  ui_mark_layout_dirty(n);
  ui_popup_open(n, modal, !modal, (cw_node){ 0 });
}

/* ── MenuBar ────────────────────────────────────────────────────────── */

typedef struct { int open, hover; } bar_data;

static int bar_menus(cw_node n, cw_node **out) {
  int k = cw_node_child_count(n), m = 0;
  cw_node *a = cw_frame_alloc(sizeof(cw_node) * (size_t)(k ? k : 1));
  for (int i = 0; i < k; i++) {
    cw_node ch = cw_node_child(n, i);
    if (MD(ch)) a[m++] = ch;
  }
  *out = a;
  return m;
}
static const char *bar_title(cw_node menu) {
  menu_data *m = MD(menu);
  return ui_tr(menu, m->title && *m->title ? m->title : cw_node_name(menu));
}
static float bar_x(cw_node n, cw_node *menus, int i, float *w) {
  cw_ui_style st = T_STYLE(n, "normal");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), x = 0;
  for (int k = 0;; k++) {
    float tw = cw_ui_text_width(f, fs, bar_title(menus[k])) + SM_W(&st);
    if (k == i) {
      *w = tw;
      return x;
    }
    x += tw;
  }
}
static cw_vec2 bar_min(cw_node n, void *d) {
  (void)d;
  cw_node *ms;
  int k = bar_menus(n, &ms);
  cw_ui_style st = T_STYLE(n, "normal");
  float w = 0;
  if (k) {
    float lw;
    float x = bar_x(n, ms, k - 1, &lw);
    w = x + lw;
  }
  return (cw_vec2){ w, ceilf(cw_ui_line_height(T_FONT(n), T_FSIZE(n)) + SM_H(&st)) };
}
static void bar_draw(cw_node n, void *d) {
  bar_data *b = d;
  cw_control *c = CTL(n);
  cw_node *ms;
  int k = bar_menus(n, &ms);
  if (b->open >= 0 && (b->open >= k || !cw_popup_visible(ms[b->open]))) b->open = -1;
  for (int i = 0; i < k; i++) {
    float w, x = bar_x(n, ms, i, &w);
    const char *state = i == b->open ? "pressed" : (i == b->hover && ((c->_flags & CF_HOVERED) || cw_control_has_focus(n))) ? "hover" : "normal";
    cw_ui_style st = T_STYLE(n, state);
    cw_rect r = { x, 0, w, c->rect.h };
    cw_ui_draw_style(&st, r);
    cw_ui_draw_text_in(T_FONT(n), T_FSIZE(n), bar_title(ms[i]), style_content(&st, r), CW_HALIGN_CENTER,
                       T_COLOR(n, i == b->open ? "font_pressed_color" : "font_color"), false);
  }
}
static void bar_open(cw_node n, bar_data *b, int i) {
  cw_node *ms;
  int k = bar_menus(n, &ms);
  if (b->open >= 0 && b->open < k) cw_popup_hide(ms[b->open]);
  b->open = -1;
  if (i < 0 || i >= k) return;
  float w, x = bar_x(n, ms, i, &w);
  cw_rect g = cw_control_global_rect(n);
  popup_at(ms[i], (cw_vec2){ g.x + x, g.y + g.h }, n);
  b->open = i;
}
void ui_menubar_step(cw_node n, int dir) {
  bar_data *b = cw_node_data_t(n, menubar_t);
  cw_node *ms;
  int k = bar_menus(n, &ms);
  if (!b || !k) return;
  bar_open(n, b, ((b->open < 0 ? 0 : b->open) + dir + k) % k);
  MD(ms[b->open])->hover = step_item(MD(ms[b->open]), -1, 1);
}
static int bar_at(cw_node n, cw_vec2 p) {
  cw_node *ms;
  int k = bar_menus(n, &ms);
  for (int i = 0; i < k; i++) {
    float w, x = bar_x(n, ms, i, &w);
    if (p.x >= x && p.x < x + w) return i;
  }
  return -1;
}
static bool bar_input(cw_node n, void *d, const cw_ui_event *e) {
  bar_data *b = d;
  cw_node *ms;
  int k = bar_menus(n, &ms);
  if (b->open >= 0 && (b->open >= k || !cw_popup_visible(ms[b->open]))) b->open = -1;
  switch (e->type) {
  case CW_UI_MOUSE_MOVE: {
    int i = bar_at(n, e->pos);
    b->hover = i;
    if (b->open >= 0 && i >= 0 && i != b->open) bar_open(n, b, i); /* slide across titles while open */
    return false;
  }
  case CW_UI_MOUSE_DOWN: {
    if (e->button != 0) return true;
    int i = bar_at(n, e->pos);
    if (i < 0) return false;
    if (i == b->open) bar_open(n, b, -1);
    else bar_open(n, b, i);
    return true;
  }
  case CW_UI_NAV:
    if (e->nav == CW_NAV_ACCEPT || e->nav == CW_NAV_DOWN) {
      bar_open(n, b, b->hover >= 0 ? b->hover : 0);
      if (b->open >= 0) MD(ms[b->open])->hover = step_item(MD(ms[b->open]), -1, 1);
      return true;
    }
    if (e->nav == CW_NAV_LEFT || e->nav == CW_NAV_RIGHT) {
      int h = b->hover < 0 ? 0 : b->hover;
      int nh = h + (e->nav == CW_NAV_RIGHT ? 1 : -1);
      if (nh < 0 || nh >= k) return false;
      b->hover = nh;
      return true;
    }
    return false;
  default: return false;
  }
}
static void bar_init(cw_node n, void *d) {
  bar_data *b = d;
  b->open = b->hover = -1;
  CTL(n)->focus_mode = CW_FOCUS_CLICK;
}
static cw_node_type menubar_type = { .name = "MenuBar", .parent = "Control", .data_size = sizeof(bar_data), .init = bar_init,
                                     .children_changed = NULL, .doc = "A row of menu titles; each child PopupMenu is a menu." };
static void bar_children(cw_node n, void *d) {
  (void)d;
  ui_mark_min_dirty(n);
}

/* ── AcceptDialog / ConfirmationDialog ──────────────────────────────── */

typedef struct {
  const char *title, *dialog_text, *ok_text, *cancel_text;
  bool has_cancel;
  int focus_button; /* 0 ok, 1 cancel */
  int pressing, hover;
} dlg_data;
static const cw_node_type *dlg_t;

typedef struct { cw_rect title, text, content, ok, cancel; float text_w; } dlg_geo;

static float dlg_text_w(cw_node n) { return cw_ui_text_width(T_FONT(n), T_FSIZE(n), "M") * 30; }

static cw_vec2 btn_size(cw_node n, const char *t) {
  cw_ui_style bs = cw_control_theme_style(n, "Button", "normal");
  cw_font f = cw_control_theme_font(n, "Button", "font");
  float fs = cw_control_theme_font_size(n, "Button", "font_size");
  float w = cw_ui_text_width(f, fs, t) + SM_W(&bs);
  return (cw_vec2){ CW_MAX(w, cw_ui_text_width(f, fs, "M") * 5), cw_ui_line_height(f, fs) + SM_H(&bs) };
}

static const char *ok_t(dlg_data *g) { return g->ok_text && *g->ok_text ? g->ok_text : "OK"; }
static const char *cancel_t(dlg_data *g) { return g->cancel_text && *g->cancel_text ? g->cancel_text : "Cancel"; }

static dlg_geo dlg_layout_geo(cw_node n, dlg_data *g, cw_vec2 size) {
  dlg_geo o;
  cw_ui_style p = T_STYLE(n, "panel");
  float sep = T_CONST(n, "separation");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), tfs = T_CONST(n, "title_font_size");
  float th = cw_ui_line_height(f, tfs > 0 ? tfs : fs);
  cw_rect in = style_content(&p, (cw_rect){ 0, 0, size.x, size.y });
  o.title = (cw_rect){ in.x, in.y, in.w, th };
  float tw = dlg_text_w(n);
  o.text_w = CW_MIN(tw, in.w);
  cw_text_metrics tm = cw_font_measure(f, ui_tr(n, ui_s(g->dialog_text)), &(cw_text_style){ .size = fs, .wrap_width = o.text_w });
  float ty = in.y + th + sep;
  o.text = (cw_rect){ in.x, ty, in.w, g->dialog_text && *g->dialog_text ? tm.height : 0 };
  cw_vec2 ob = btn_size(n, ui_tr(n, ok_t(g))), cb = btn_size(n, ui_tr(n, cancel_t(g)));
  float bh = CW_MAX(ob.y, cb.y);
  float by = in.y + in.h - bh;
  o.content = (cw_rect){ in.x, o.text.y + o.text.h + (o.text.h > 0 ? sep : 0), in.w, 0 };
  o.content.h = CW_MAX(0, by - sep - o.content.y);
  float bx = in.x + in.w;
  if (g->has_cancel) {
    o.cancel = (cw_rect){ bx - cb.x, by, cb.x, bh };
    bx -= cb.x + sep;
  } else o.cancel = (cw_rect){ 0 };
  o.ok = (cw_rect){ bx - ob.x, by, ob.x, bh };
  return o;
}

static cw_vec2 dlg_min(cw_node n, void *d) {
  dlg_data *g = d;
  cw_ui_style p = T_STYLE(n, "panel");
  float sep = T_CONST(n, "separation");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), tfs = T_CONST(n, "title_font_size");
  float th = cw_ui_line_height(f, tfs > 0 ? tfs : fs);
  float tw = cw_ui_text_width(f, tfs > 0 ? tfs : fs, ui_tr(n, ui_s(g->title)));
  cw_text_metrics tm = cw_font_measure(f, ui_tr(n, ui_s(g->dialog_text)), &(cw_text_style){ .size = fs, .wrap_width = dlg_text_w(n) });
  cw_vec2 ob = btn_size(n, ui_tr(n, ok_t(g))), cb = btn_size(n, ui_tr(n, cancel_t(g)));
  float bw = ob.x + (g->has_cancel ? cb.x + sep : 0), bh = CW_MAX(ob.y, cb.y);
  cw_node *ch;
  int k = ui_children(n, &ch);
  cw_vec2 cm = { 0, 0 };
  for (int i = 0; i < k; i++) {
    cw_vec2 m = cw_control_min_size(ch[i]);
    cm.x = CW_MAX(cm.x, m.x);
    cm.y = CW_MAX(cm.y, m.y);
  }
  float text_h = g->dialog_text && *g->dialog_text ? tm.height + sep : 0;
  float w = CW_MAX(CW_MAX(tw, tm.width), CW_MAX(bw, cm.x));
  float h = th + sep + text_h + (k ? cm.y + sep : 0) + bh;
  return (cw_vec2){ ceilf(w + SM_W(&p)), ceilf(h + SM_H(&p)) };
}

static void dlg_layout(cw_node n, void *d, cw_vec2 s) {
  dlg_geo o = dlg_layout_geo(n, d, s);
  cw_node *ch;
  int k = ui_children(n, &ch);
  for (int i = 0; i < k; i++) cw_control_fit_child(ch[i], o.content);
}

static void draw_btn(cw_node n, dlg_data *g, cw_rect r, const char *t, int idx) {
  const char *state = g->pressing == idx && g->hover == idx ? "pressed" : g->hover == idx ? "hover" : "normal";
  cw_ui_style st = cw_control_theme_style(n, "Button", state);
  cw_ui_draw_style(&st, r);
  if (g->focus_button == idx) {
    cw_ui_style fo = cw_control_theme_style(n, "Button", "focus");
    cw_ui_draw_style(&fo, r);
  }
  cw_ui_draw_text_in(cw_control_theme_font(n, "Button", "font"), cw_control_theme_font_size(n, "Button", "font_size"), t,
                     style_content(&st, r), CW_HALIGN_CENTER, cw_control_theme_color(n, "Button", "font_color"), false);
}

static void dlg_draw(cw_node n, void *d) {
  dlg_data *g = d;
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel");
  cw_ui_draw_style(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
  dlg_geo o = dlg_layout_geo(n, g, (cw_vec2){ c->rect.w, c->rect.h });
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), tfs = T_CONST(n, "title_font_size");
  cw_ui_draw_text(f, tfs > 0 ? tfs : fs, ui_tr(n, ui_s(g->title)), (cw_vec2){ o.title.x, o.title.y }, T_COLOR(n, "title_color"));
  if (g->dialog_text && *g->dialog_text) {
    cw_text_style st = { .size = fs, .wrap_width = o.text_w, .color = T_COLOR(n, "font_color") };
    cw_vec2 og = ui_draw_origin();
    ui_draw_text_styled(f, ui_tr(n, g->dialog_text), (cw_vec2){ og.x + o.text.x, og.y + o.text.y }, &st);
  }
  draw_btn(n, g, o.ok, ui_tr(n, ok_t(g)), 0);
  if (g->has_cancel) draw_btn(n, g, o.cancel, ui_tr(n, cancel_t(g)), 1);
}

static void dlg_finish(cw_node n, dlg_data *g, bool ok) {
  (void)g;
  cw_popup_hide(n);
  ui_emit0(n, ok ? "confirmed" : "canceled");
}

static int dlg_btn_at(cw_node n, dlg_data *g, cw_vec2 p) {
  cw_control *c = CTL(n);
  dlg_geo o = dlg_layout_geo(n, g, (cw_vec2){ c->rect.w, c->rect.h });
  if (rect_has(o.ok, p)) return 0;
  if (g->has_cancel && rect_has(o.cancel, p)) return 1;
  return -1;
}

static bool dlg_input(cw_node n, void *d, const cw_ui_event *e) {
  dlg_data *g = d;
  switch (e->type) {
  case CW_UI_MOUSE_MOVE: g->hover = dlg_btn_at(n, g, e->pos); return true;
  case CW_UI_MOUSE_DOWN: g->pressing = dlg_btn_at(n, g, e->pos); return true;
  case CW_UI_MOUSE_UP: {
    int b = dlg_btn_at(n, g, e->pos);
    if (b >= 0 && b == g->pressing && !e->canceled) dlg_finish(n, g, b == 0);
    g->pressing = -1;
    return true;
  }
  case CW_UI_NAV:
    switch (e->nav) {
    case CW_NAV_LEFT: case CW_NAV_RIGHT:
      if (g->has_cancel) g->focus_button = 1 - g->focus_button;
      return true;
    case CW_NAV_ACCEPT: dlg_finish(n, g, g->focus_button == 0); return true;
    case CW_NAV_CANCEL: dlg_finish(n, g, false); return true;
    default: return true;
    }
  default: return false;
  }
}

void cw_dialog_popup_centered(cw_node n) {
  dlg_data *g = cw_node_data_t(n, dlg_t);
  if (!g) return;
  cw_control *c = CTL(n);
  c->_flags |= CF_MIN_DIRTY;
  cw_vec2 sz = cw_control_min_size(n);
  cw_vec2 cs = cw_ui_canvas_size();
  c->rect = (cw_rect){ ui_round((cs.x - sz.x) * 0.5f), ui_round((cs.y - sz.y) * 0.5f), sz.x, sz.y };
  g->focus_button = 0;
  g->pressing = g->hover = -1;
  ui_popup_open(n, true, false, (cw_node){ 0 });
}

static void dlg_init(cw_node n, void *d) {
  dlg_data *g = d;
  g->pressing = g->hover = -1;
  CTL(n)->focus_mode = CW_FOCUS_NONE;
}
static void confirm_init(cw_node n, void *d) {
  (void)d;
  ((dlg_data *)cw_node_data_t(n, dlg_t))->has_cancel = true;
}
static void dlg_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}
static const cw_prop dlg_props[] = {
  CW_PROP_FIELD(dlg_data, title, CW_T_STRING, CW_PROP_DEFAULT, "Title line."),
  CW_PROP_FIELD(dlg_data, dialog_text, CW_T_STRING, CW_PROP_DEFAULT, "Message (wrapped)."),
  CW_PROP_FIELD(dlg_data, ok_text, CW_T_STRING, CW_PROP_DEFAULT, "OK button label (default OK)."),
  CW_PROP_FIELD(dlg_data, cancel_text, CW_T_STRING, CW_PROP_DEFAULT, "Cancel button label (ConfirmationDialog)."),
  CW_PROP_END
};
static const char *const dlg_signals[] = { "confirmed", "canceled", NULL };
static cw_node_type dlg_type = { .name = "AcceptDialog", .parent = "Popup", .data_size = sizeof(dlg_data), .props = dlg_props,
                                 .signals = dlg_signals, .init = dlg_init, .prop_changed = dlg_changed,
                                 .doc = "A modal message with an OK button; children become its content." };
static cw_node_type confirm_type = { .name = "ConfirmationDialog", .parent = "AcceptDialog", .init = confirm_init,
                                     .doc = "An AcceptDialog with OK and Cancel." };

void ui_register_popups(void) {
  cw_node_type_register(&popup_type);
  cw_ui_class_register("Popup", &(cw_ui_class){ .theme_type = "Popup" });
  cw_node_type_register(&ppanel_type);
  cw_ui_class_register("PopupPanel", &(cw_ui_class){ .min_size = ppanel_min, .layout = ppanel_layout, .draw = ppanel_draw,
                                                     .input = ppanel_input });
  cw_node_type_register(&menu_type);
  menu_t = &menu_type;
  cw_ui_class_register("PopupMenu", &(cw_ui_class){ .min_size = menu_min, .draw = menu_draw, .input = menu_input });
  menubar_type.children_changed = bar_children;
  cw_node_type_register(&menubar_type);
  menubar_t = &menubar_type;
  cw_ui_class_register("MenuBar", &(cw_ui_class){ .min_size = bar_min, .draw = bar_draw, .input = bar_input });
  cw_node_type_register(&dlg_type);
  dlg_t = &dlg_type;
  cw_node_type_register(&confirm_type);
  cw_ui_class_register("AcceptDialog", &(cw_ui_class){ .min_size = dlg_min, .layout = dlg_layout, .draw = dlg_draw,
                                                       .input = dlg_input });
  cw_ui_class_register("ConfirmationDialog", &(cw_ui_class){ .theme_type = "AcceptDialog" });
}

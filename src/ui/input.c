/* UI input routing: pointer (mouse and touch) to the topmost control with
 * capture during drags, keys and text to the focus owner, keyboard and
 * gamepad navigation, popups, tooltips; the module's frame hook. */
#include "platform/hooks.h"
#include "ui_internal.h"
#include "cartwheel/debug.h"

#define DOUBLE_CLICK_S 0.4
#define KEY_DELAY_S 0.42
#define KEY_RATE_S 0.04
#define NAV_DELAY_S 0.42
#define NAV_RATE_S 0.11
#define TOOLTIP_S 0.6
#define TOUCH_SLOP 8.0f

/* scancodes */
enum { SC_TAB = 43, SC_RETURN = 40, SC_ESCAPE = 41, SC_SPACE = 44, SC_RIGHT = 79, SC_LEFT = 80, SC_DOWN = 81, SC_UP = 82,
       SC_KP_ENTER = 88, SC_LCTRL = 224, SC_LSHIFT = 225, SC_LALT = 226, SC_LGUI = 227, SC_RCTRL = 228, SC_RSHIFT = 229,
       SC_RALT = 230, SC_RGUI = 231 };

static void frame_hook(double dt);
static void event_hook(const cw_event *e);

/* UI units -> canvas units. The window pass's canvas is logical (r2d scales
 * it to the drawable: cw_r2d_pixel_scale), so density never enters here. */
float ui_canvas_scale(void) { return UI.scale > 0 ? UI.scale : 1; }

void ui_init_once(void) {
  if (UI.inited) return;
  UI.inited = true;
  if (UI.scale <= 0) UI.scale = 1;
  if (!UI.layer) UI.layer = CW_R2D_MAX_LAYERS - 1;
  UI.dpi = 1;
  if (UI.text_scale <= 0) UI.text_scale = 1;
  ui_l10n_listen();
  ui_a11y_init();
  static const cw__ui_hooks hooks = { frame_hook, event_hook };
  cw__platform_hook_ui(&hooks);
  static const char *names[6] = { "ui_up", "ui_down", "ui_left", "ui_right", "ui_accept", "ui_cancel" };
  static const char *binds[6] = { "pad:up, axis:ly-", "pad:down, axis:ly+", "pad:left, axis:lx-", "pad:right, axis:lx+",
                                  "pad:south", "pad:east" };
  for (int i = 0; i < 6; i++) {
    bool existed = cw_action_find(names[i]) >= 0;
    UI.nav_actions[i] = cw_action_add(names[i], 0.5f);
    if (!existed && cw_action_bindings(UI.nav_actions[i], NULL, 0) == 0) cw_action_bind_text(UI.nav_actions[i], binds[i]);
  }
}

void cw_ui_set_scale(float s) {
  UI.scale = s > 0 ? s : 1;
  cw_tree *t = cw_current_tree();
  if (t) {
    ui_tree_state *st = ui_state(t);
    ui_roots_refresh(st);
    for (int i = 0; i < st->roots.count; i++) ui_mark_layout_dirty(st->roots.data[i]);
  }
}
float cw_ui_scale(void) { return UI.scale > 0 ? UI.scale : 1; }
void cw_ui_set_layer(int l) { UI.layer = CW_CLAMP(l, 0, CW_R2D_MAX_LAYERS - 1); }
int cw_ui_layer(void) { return UI.layer ? UI.layer : CW_R2D_MAX_LAYERS - 1; }

bool cw_ui_wants_pointer(void) { return UI.wants_pointer; }
bool cw_ui_wants_keyboard(void) { return UI.wants_keyboard; }
cw_vec2 cw_ui_pointer(void) { return UI.pointer; }
void cw_ui_set_unhandled_input(cw_ui_unhandled_fn fn, void *user) {
  UI.unhandled = fn;
  UI.unhandled_user = user;
}

static ui_tree_state *cur(void) { return cw_current_tree() ? ui_state(cw_current_tree()) : NULL; }
cw_node cw_ui_focus_owner(void) {
  ui_tree_state *s = cur();
  return s && cw_node_valid(s->focus) ? s->focus : (cw_node){ 0 };
}
cw_node cw_ui_hovered(void) {
  ui_tree_state *s = cur();
  return s && cw_node_valid(s->hover) ? s->hover : (cw_node){ 0 };
}

bool ui_shift(void) {
  const cw_raw_input *r = cw_input_raw();
  return r->keys[SC_LSHIFT] || r->keys[SC_RSHIFT];
}
bool ui_ctrl(void) {
  const cw_raw_input *r = cw_input_raw();
  return r->keys[SC_LCTRL] || r->keys[SC_RCTRL] || r->keys[SC_LGUI] || r->keys[SC_RGUI];
}
static bool ui_alt(void) {
  const cw_raw_input *r = cw_input_raw();
  return r->keys[SC_LALT] || r->keys[SC_RALT];
}

void cw_ui_feed_event(const cw_event *e) {
  if (e->type == CW_EVENT_TEXT) {
    int n = (int)strlen(e->text.utf8);
    if (UI.text_len + n < (int)sizeof UI.text_queue) {
      memcpy(UI.text_queue + UI.text_len, e->text.utf8, (size_t)n);
      UI.text_len += n;
      UI.text_queue[UI.text_len] = 0;
    }
  }
}
static void event_hook(const cw_event *e) { cw_ui_feed_event(e); }

/* ── stats ──────────────────────────────────────────────────────────── */

const cw_ui_stats *cw_ui_get_stats(void) {
  ui_tree_state *s = cur();
  UI.stats.popups = s ? (uint32_t)s->popups.count : 0;
  return &UI.stats;
}
void cw_ui_debug_register(const char *prefix) {
  ui_init_once();
#define R(f, ty) cw_debug_field(cw_frame_printf("%s." #f, prefix), &UI.stats.f, ty, 1)
  /* names must outlive the call: intern them */
#undef R
#define R(f, ty) cw_debug_field(cw_intern(cw_frame_printf("%s." #f, prefix)), &UI.stats.f, ty, 1)
  R(controls, CW_DBG_U32);
  R(drawn, CW_DBG_U32);
  R(layout_passes, CW_DBG_U32);
  R(measured, CW_DBG_U32);
  R(arranged, CW_DBG_U32);
  R(draw_calls, CW_DBG_U32);
  R(commands, CW_DBG_U32);
  R(popups, CW_DBG_U32);
#undef R
}

/* ── hit testing ────────────────────────────────────────────────────── */

static cw_node hit(cw_node n, cw_vec2 porigin, cw_vec2 p) {
  cw_control *c = CTL(n);
  if (!c || !c->visible) return (cw_node){ 0 };
  cw_vec2 o = { porigin.x + c->rect.x, porigin.y + c->rect.y };
  cw_vec2 l = { p.x - o.x, p.y - o.y };
  bool inside = l.x >= 0 && l.y >= 0 && l.x < c->rect.w && l.y < c->rect.h;
  if (c->clip_contents && !inside) return (cw_node){ 0 };
  int k = cw_node_child_count(n);
  if (k && c->_vt->children_point && !c->_vt->children_point(n, cw_node_data_t(n, c->_vt->children_point_t), l)) k = 0;
  if (k) {
    cw_vec2 off = ui_child_offset(n);
    cw_vec2 co = { o.x + off.x, o.y + off.y };
    for (int i = k - 1; i >= 0; i--) {
      cw_node ch = cw_node_child(n, i);
      cw_control *cc = CTL(ch);
      if (!cc || (cc->_flags & CF_TOP_LEVEL)) continue;
      cw_node r = hit(ch, co, p);
      if (!CW_HANDLE_IS_NULL(r)) return r;
    }
  }
  if (inside && c->mouse_filter != CW_MOUSE_IGNORE &&
      (!c->_vt->has_point || c->_vt->has_point(n, cw_node_data_t(n, c->_vt->has_point_t), l)))
    return n;
  return (cw_node){ 0 };
}

/* topmost popup index containing p, -1 none */
static int popup_at(ui_tree_state *s, cw_vec2 p, cw_node *target) {
  for (int i = s->popups.count - 1; i >= 0; i--) {
    cw_node pn = s->popups.data[i].node;
    cw_control *c = CTL(pn);
    if (!c || !c->visible) continue;
    if (rect_has(c->rect, p)) {
      *target = hit(pn, (cw_vec2){ 0, 0 }, p);
      if (CW_HANDLE_IS_NULL(*target)) *target = pn;
      return i;
    }
  }
  return -1;
}

static int top_modal(ui_tree_state *s) {
  for (int i = s->popups.count - 1; i >= 0; i--)
    if (s->popups.data[i].modal) return i;
  return -1;
}

static cw_node hit_all(ui_tree_state *s, cw_vec2 p, int *popup_index) {
  cw_node t = { 0 };
  int pi = popup_at(s, p, &t);
  if (popup_index) *popup_index = pi;
  if (pi >= 0) return t;
  if (top_modal(s) >= 0) return (cw_node){ 0 };
  for (int i = s->roots.count - 1; i >= 0; i--) {
    t = hit(s->roots.data[i], (cw_vec2){ 0, 0 }, p);
    if (!CW_HANDLE_IS_NULL(t)) return t;
  }
  return (cw_node){ 0 };
}

/* ── delivery ───────────────────────────────────────────────────────── */

static bool call_input(cw_node n, cw_ui_event *e) {
  cw_control *c = CTL(n);
  if (!c || !c->_vt->input) return false;
  cw_rect g = cw_control_global_rect(n);
  e->pos = (cw_vec2){ e->global.x - g.x, e->global.y - g.y };
  return c->_vt->input(n, cw_node_data_t(n, c->_vt->input_t), e);
}

/* bubbles pointer events while the filter is PASS; returns consumed */
static bool deliver_pointer(cw_node target, cw_ui_event *e) {
  cw_node n = target;
  for (int guard = 0; guard < 256 && cw_node_valid(n); guard++) {
    cw_control *c = CTL(n);
    if (!c) return false;
    if (call_input(n, e)) return true;
    if (c->mouse_filter == CW_MOUSE_STOP) return true;
    n = ui_parent_control(n);
  }
  return false;
}

/* keys bubble from the focus owner up to the top level */
static bool deliver_key(cw_node target, cw_ui_event *e) {
  cw_node n = target;
  for (int guard = 0; guard < 256 && cw_node_valid(n); guard++) {
    if (call_input(n, e)) return true;
    n = ui_parent_control(n);
  }
  return false;
}

static void unhandled(const cw_ui_event *e) {
  if (UI.unhandled) UI.unhandled(e, UI.unhandled_user);
}

/* ── focus ──────────────────────────────────────────────────────────── */

static bool is_disabled(cw_node n) {
  cw_value v;
  const cw_prop *p = cw_node_find_prop(n, "disabled", NULL);
  return p && cw_node_get_prop(n, "disabled", &v) && v.type == CW_T_BOOL && v.b;
}

bool ui_focusable(cw_node n, bool keyboard) {
  cw_control *c = CTL(n);
  if (!c) return false;
  if (keyboard ? c->focus_mode != CW_FOCUS_ALL : c->focus_mode == CW_FOCUS_NONE) return false;
  if (!cw_control_visible_in_tree(n) || is_disabled(n)) return false;
  return true;
}

static void scroll_into_view(cw_node n) {
  for (cw_node p = ui_parent_control(n); !CW_HANDLE_IS_NULL(p); p = ui_parent_control(p))
    if (cw_node_is(p, "ScrollContainer")) cw_scroll_ensure_visible(p, n);
}

static void set_focus(ui_tree_state *s, cw_node n) {
  if (s->focus.id == n.id) return;
  cw_node old = s->focus;
  s->focus = n;
  if (cw_node_valid(old)) {
    cw_control *oc = CTL(old);
    if (oc) {
      oc->_flags &= ~CF_FOCUSED;
      if (oc->_vt->focus_changed) oc->_vt->focus_changed(old, cw_node_data_t(old, oc->_vt->focus_changed_t), false);
      ui_emit0(old, "focus_exited");
    }
  }
  if (cw_node_valid(n)) {
    cw_control *c = CTL(n);
    c->_flags |= CF_FOCUSED;
    if (c->_vt->focus_changed) c->_vt->focus_changed(n, cw_node_data_t(n, c->_vt->focus_changed_t), true);
    ui_emit0(n, "focus_entered");
  }
  UI.dbg_focus_hash++;
  ui_a11y_focus_changed(n);
}

void cw_control_grab_focus(cw_node n) {
  ui_tree_state *s = ui_state_of(n);
  if (!s || !CTL(n)) return;
  set_focus(s, n);
}
void cw_control_release_focus(cw_node n) {
  ui_tree_state *s = ui_state_of(n);
  if (s && s->focus.id == n.id) set_focus(s, (cw_node){ 0 });
}
bool cw_control_has_focus(cw_node n) {
  cw_control *c = CTL(n);
  return c && (c->_flags & CF_FOCUSED);
}

typedef struct { CW_ARRAY(cw_node) list; bool keyboard; } focus_collect;
static bool collect_focus(cw_node n, void *u) {
  focus_collect *fc = u;
  cw_control *c = CTL(n);
  if (c && !c->visible) return false;
  if (c && (c->_flags & CF_TOP_LEVEL) && !ui_popup_is_open(n)) return false;
  if (c && ui_focusable(n, fc->keyboard)) cw_array_push(&fc->list, n);
  return true;
}

/* focusable controls in tree order, inside the top modal popup if one is open */
static void focus_candidates(ui_tree_state *s, focus_collect *fc) {
  int m = top_modal(s);
  if (m >= 0) cw_node_walk(s->popups.data[m].node, collect_focus, fc);
  else {
    for (int i = 0; i < s->roots.count; i++) cw_node_walk(s->roots.data[i], collect_focus, fc);
  }
}

static void focus_tab(ui_tree_state *s, bool backward) {
  cw_control *fc0 = cw_node_valid(s->focus) ? CTL(s->focus) : NULL;
  if (fc0) {
    cw_node ex = backward ? fc0->focus_previous : fc0->focus_next;
    if (cw_node_valid(ex) && ui_focusable(ex, false)) {
      set_focus(s, ex);
      return;
    }
  }
  focus_collect fc = { .keyboard = true };
  focus_candidates(s, &fc);
  if (fc.list.count) {
    int at = -1;
    for (int i = 0; i < fc.list.count; i++)
      if (fc.list.data[i].id == s->focus.id) at = i;
    int next = at < 0 ? (backward ? fc.list.count - 1 : 0) : (at + (backward ? fc.list.count - 1 : 1)) % fc.list.count;
    set_focus(s, fc.list.data[next]);
    scroll_into_view(s->focus);
  }
  cw_array_free(&fc.list);
}

/* nearest focusable control in a direction (0 left 1 up 2 right 3 down) */
static void focus_dir(ui_tree_state *s, int dir) {
  if (!cw_node_valid(s->focus)) {
    focus_tab(s, false);
    return;
  }
  cw_control *c = CTL(s->focus);
  cw_node ex = dir == 0 ? c->focus_neighbor_left : dir == 1 ? c->focus_neighbor_top : dir == 2 ? c->focus_neighbor_right
                                                                                              : c->focus_neighbor_bottom;
  if (cw_node_valid(ex) && ui_focusable(ex, false)) {
    set_focus(s, ex);
    scroll_into_view(ex);
    return;
  }
  focus_collect fc = { .keyboard = true };
  focus_candidates(s, &fc);
  cw_rect a = cw_control_global_rect(s->focus);
  float acx = a.x + a.w * 0.5f, acy = a.y + a.h * 0.5f;
  float best = 1e30f;
  cw_node pick = { 0 };
  for (int i = 0; i < fc.list.count; i++) {
    cw_node n = fc.list.data[i];
    if (n.id == s->focus.id) continue;
    cw_rect b = cw_control_global_rect(n);
    float bcx = b.x + b.w * 0.5f, bcy = b.y + b.h * 0.5f;
    float main, cross;
    switch (dir) {
    case 0: main = a.x - (b.x + b.w); cross = fabsf(bcy - acy); if (bcx >= acx) continue; break;
    case 2: main = b.x - (a.x + a.w); cross = fabsf(bcy - acy); if (bcx <= acx) continue; break;
    case 1: main = a.y - (b.y + b.h); cross = fabsf(bcx - acx); if (bcy >= acy) continue; break;
    default: main = b.y - (a.y + a.h); cross = fabsf(bcx - acx); if (bcy <= acy) continue; break;
    }
    if (main < -1) main = 0; /* overlapping on the main axis: still in that direction by centre */
    /* rects that overlap on the cross axis are strongly preferred */
    bool overlap = dir == 0 || dir == 2 ? (b.y < a.y + a.h && b.y + b.h > a.y) : (b.x < a.x + a.w && b.x + b.w > a.x);
    float score = main + cross * (overlap ? 0.5f : 3.0f) + (overlap ? 0 : 1000);
    if (score < best) best = score, pick = n;
  }
  cw_array_free(&fc.list);
  if (!CW_HANDLE_IS_NULL(pick)) {
    set_focus(s, pick);
    scroll_into_view(pick);
  }
}

/* ── popups ─────────────────────────────────────────────────────────── */

void ui_popup_open(cw_node n, bool modal, bool close_outside, cw_node owner) {
  ui_tree_state *s = ui_state_of(cw_node_valid(owner) ? owner : n);
  if (!s) s = cur();
  cw_control *c = CTL(n);
  if (!s || !c) return;
  for (int i = 0; i < s->popups.count; i++)
    if (s->popups.data[i].node.id == n.id) return;
  c->_flags |= CF_TOP_LEVEL;
  c->visible = true;
  c->_flags |= CF_NEVER_LAID | CF_LAYOUT_DIRTY | CF_MIN_DIRTY;
  ui_popup p = { n, modal, close_outside, owner, s->focus };
  cw_array_push(&s->popups, p);
  UI.tip_shown = false;
}

static void popup_remove_at(ui_tree_state *s, int i) {
  cw_node n = s->popups.data[i].node;
  cw_node restore = s->popups.data[i].prev_focus;
  memmove(s->popups.data + i, s->popups.data + i + 1, sizeof(ui_popup) * (size_t)(s->popups.count - i - 1));
  s->popups.count--;
  cw_control *c = CTL(n);
  if (c) c->visible = false;
  /* focus inside a closed popup (or none) goes back to where it was before
   * the popup opened, so keyboard and gamepad users keep their place */
  bool inside = !cw_node_valid(s->focus);
  for (cw_node q = s->focus; cw_node_valid(q); q = cw_node_parent(q))
    if (q.id == n.id) {
      inside = true;
      break;
    }
  if (inside) set_focus(s, cw_node_valid(restore) && ui_focusable(restore, false) ? restore : (cw_node){ 0 });
  /* close popups this one owns (submenus) */
  for (int k = s->popups.count - 1; k >= 0; k--)
    if (k < s->popups.count && s->popups.data[k].owner.id == n.id) popup_remove_at(s, k);
}

void ui_popup_close(cw_node n) {
  ui_tree_state *s = ui_state_of(n);
  if (!s) s = cur();
  if (!s) return;
  for (int i = s->popups.count - 1; i >= 0; i--)
    if (s->popups.data[i].node.id == n.id) {
      popup_remove_at(s, i);
      return;
    }
}

bool ui_popup_is_open(cw_node n) {
  ui_tree_state *s = ui_state_of(n);
  if (!s) s = cur();
  if (!s) return false;
  for (int i = 0; i < s->popups.count; i++)
    if (s->popups.data[i].node.id == n.id) return true;
  return false;
}

void ui_popups_close_all(ui_tree_state *s) {
  while (s->popups.count) popup_remove_at(s, s->popups.count - 1);
}

void cw_popup_hide(cw_node n) { ui_popup_close(n); }
bool cw_popup_visible(cw_node n) { return ui_popup_is_open(n); }

void ui_set_capture(cw_node n) {
  ui_tree_state *s = ui_state_of(n);
  if (s) s->capture = n;
}

void ui_forget_node(cw_node n) {
  ui_tree_state *s = ui_state_of(n);
  if (!s) s = cur();
  if (!s) return;
  if (s->focus.id == n.id) set_focus(s, (cw_node){ 0 });
  if (s->hover.id == n.id) s->hover = (cw_node){ 0 };
  if (s->capture.id == n.id) s->capture = (cw_node){ 0 };
  if (UI.tip_node.id == n.id) UI.tip_node = (cw_node){ 0 }, UI.tip_shown = false;
  if (UI.drop_target.id == n.id) UI.drop_target = (cw_node){ 0 };
  for (int i = s->popups.count - 1; i >= 0; i--)
    if (i < s->popups.count && s->popups.data[i].node.id == n.id) popup_remove_at(s, i);
}

/* ── text input ─────────────────────────────────────────────────────── */

void ui_text_input_update(ui_tree_state *s) {
  bool want = false;
  if (cw_node_valid(s->focus)) {
    cw_control *c = CTL(s->focus);
    want = c && c->_vt->wants_text;
  }
  if (want && !UI.text_input_on) cw_text_input_start();
  if (!want && UI.text_input_on) cw_text_input_stop();
  UI.text_input_on = want;
}

/* ── the frame ──────────────────────────────────────────────────────── */

static cw_ui_event ev(cw_ui_event_type t) {
  cw_ui_event e = { .type = t, .global = UI.pointer, .buttons = UI.buttons, .touch = UI.from_touch };
  e.shift = ui_shift(), e.ctrl = ui_ctrl(), e.alt = ui_alt();
  return e;
}

static void update_hover(ui_tree_state *s, cw_node now) {
  if (now.id == s->hover.id) return;
  if (cw_node_valid(s->hover)) {
    cw_control *c = CTL(s->hover);
    if (c) c->_flags &= ~CF_HOVERED;
    ui_emit0(s->hover, "mouse_exited");
  }
  s->hover = now;
  if (cw_node_valid(now)) {
    CTL(now)->_flags |= CF_HOVERED;
    ui_emit0(now, "mouse_entered");
  }
  UI.tip_timer = 0;
  UI.tip_shown = false;
}

/* key routing target: the top keyboard-grabbing popup unless focus is inside it */
static cw_node key_target(ui_tree_state *s) {
  for (int i = s->popups.count - 1; i >= 0; i--) {
    ui_popup *p = &s->popups.data[i];
    if (!p->modal && !p->close_outside) continue;
    for (cw_node q = s->focus; cw_node_valid(q); q = ui_parent_control(q))
      if (q.id == p->node.id) return s->focus;
    return p->node;
  }
  return s->focus;
}

static void send_nav(ui_tree_state *s, int nav, bool repeat) {
  cw_ui_event e = ev(CW_UI_NAV);
  e.nav = nav;
  e.repeat = repeat;
  cw_node t = key_target(s);
  if (cw_node_valid(t) && deliver_key(t, &e)) return;
  if (nav <= CW_NAV_RIGHT) {
    static const int dirs[4] = { 1, 3, 0, 2 }; /* up down left right -> 0 left 1 up 2 right 3 down */
    s->focus_visible = true;
    focus_dir(s, dirs[nav]);
    return;
  }
  if (nav == CW_NAV_CANCEL && s->popups.count && !s->popups.data[s->popups.count - 1].modal) {
    ui_popup_close(s->popups.data[s->popups.count - 1].node);
    return;
  }
  unhandled(&e);
}

static int key_nav(int sc) {
  switch (sc) {
  case SC_UP: return CW_NAV_UP;
  case SC_DOWN: return CW_NAV_DOWN;
  case SC_LEFT: return CW_NAV_LEFT;
  case SC_RIGHT: return CW_NAV_RIGHT;
  case SC_RETURN: case SC_KP_ENTER: case SC_SPACE: return CW_NAV_ACCEPT;
  case SC_ESCAPE: return CW_NAV_CANCEL;
  default: return -1;
  }
}

static bool is_modifier(int sc) { return sc >= SC_LCTRL && sc <= SC_RGUI; }

static void send_key(ui_tree_state *s, int sc, bool repeat) {
  cw_ui_event e = ev(CW_UI_KEY);
  e.key = sc;
  e.repeat = repeat;
  cw_node t = key_target(s);
  if (cw_node_valid(t) && deliver_key(t, &e)) return;
  if (sc == SC_TAB) {
    s->focus_visible = true;
    focus_tab(s, e.shift);
    return;
  }
  int nav = key_nav(sc);
  if (nav >= 0 && !e.ctrl && !e.alt) {
    send_nav(s, nav, repeat);
    return;
  }
  unhandled(&e);
}

bool ui_scroll_steal(cw_node scroll, cw_vec2 from, cw_vec2 now);

static void pointer_frame(ui_tree_state *s, const cw_raw_input *raw) {
  /* pointer source: the first active touch acts as the mouse */
  cw_vec2 p = UI.pointer;
  uint32_t buttons = 0, pressed = 0;
  bool valid = false, touch = false;
  for (int i = 0; i < CW_MAX_TOUCHES; i++) {
    const cw_touch_state *t = &raw->touches[i];
    if (t->active || t->tapped) {
      p = (cw_vec2){ t->x, t->y };
      buttons = t->active ? 1 : 0;
      pressed = t->tapped ? 1 : 0;
      valid = touch = true;
      break;
    }
  }
  /* a touch just lifted: release it where it was before the mouse (if any)
   * takes over next frame, so a drag never sees the pointer jump */
  bool lifted = !valid && UI.from_touch && UI.buttons;
  if (!valid && !lifted && raw->mouse_present) {
    p = (cw_vec2){ raw->mouse_x, raw->mouse_y };
    buttons = raw->mouse_buttons;
    pressed = raw->mouse_pressed;
    valid = true;
  }
  bool have_pos = valid;
  if (lifted) {
    valid = true;
    touch = true;
  }
  if (have_pos) {
    float sc = ui_canvas_scale();
    if (UI.draw_virtual) {
      cw_vec2 c = cw_r2d_window_to_canvas(p);
      p = (cw_vec2){ c.x / sc, c.y / sc };
    } else p = (cw_vec2){ p.x / sc, p.y / sc };
  } else p = UI.pointer;
  bool moved = valid && (p.x != UI.pointer.x || p.y != UI.pointer.y || !UI.pointer_valid);
  UI.pointer_valid = valid || UI.pointer_valid;
  UI.pointer = p;
  UI.from_touch = touch;
  UI.prev_buttons = UI.buttons;

  /* hover (not while a drag holds the capture) */
  int pi = -1;
  cw_node under = valid ? hit_all(s, p, &pi) : (cw_node){ 0 };
  if (!cw_node_valid(s->capture)) update_hover(s, touch && !buttons && !pressed ? (cw_node){ 0 } : under);

  if (moved) {
    UI.tip_timer = 0;
    if (UI.tip_shown) UI.tip_shown = false;
    cw_node t = cw_node_valid(s->capture) ? s->capture : under;
    if (UI.drag_active) {
      /* a drag in progress: moves feed the drop search only */
    } else if (cw_node_valid(t)) {
      cw_ui_event e = ev(CW_UI_MOUSE_MOVE);
      e.buttons = buttons;
      if (!deliver_pointer(t, &e)) unhandled(&e);
    } else {
      cw_ui_event e = ev(CW_UI_MOUSE_MOVE);
      unhandled(&e);
    }
    /* a touch drag past the slop inside a scroll area becomes a scroll */
    if (touch && cw_node_valid(s->capture) && !cw_node_is(s->capture, "ScrollContainer")) {
      cw_vec2 d = cw_v2_sub(p, UI.press_pos);
      if (fabsf(d.x) > TOUCH_SLOP || fabsf(d.y) > TOUCH_SLOP) {
        for (cw_node a = ui_parent_control(s->capture); !CW_HANDLE_IS_NULL(a); a = ui_parent_control(a))
          if (cw_node_is(a, "ScrollContainer") && ui_scroll_steal(a, UI.press_pos, p)) {
            cw_ui_event e = ev(CW_UI_MOUSE_UP);
            e.button = 0;
            e.canceled = true;
            call_input(s->capture, &e);
            s->capture = a;
            break;
          }
      }
    }
  }

  if (UI.drag_active) ui_dnd_update(under, p);

  /* presses */
  for (int b = 0; b < 3; b++) {
    uint32_t bit = 1u << b;
    bool down_now = (pressed & bit) || ((buttons & bit) && !(UI.buttons & bit));
    if (down_now) {
      UI.tip_shown = false;
      UI.tip_timer = 0;
      cw_ui_event e = ev(CW_UI_MOUSE_DOWN);
      e.button = b;
      e.buttons = buttons | bit;
      bool dbl = UI.time - UI.last_click_time < DOUBLE_CLICK_S && fabsf(p.x - UI.last_click_pos.x) < 5 &&
                 fabsf(p.y - UI.last_click_pos.y) < 5;
      UI.click_count = dbl ? UI.click_count + 1 : 1;
      e.clicks = UI.click_count;
      UI.last_click_time = UI.time;
      UI.last_click_pos = p;
      UI.press_pos = p;
      /* popups: a press outside closes the menus above the one it hits and is
       * consumed; outside a modal popup nothing below gets it */
      bool consumed = false;
      if (s->popups.count) {
        int m = top_modal(s);
        for (int i = s->popups.count - 1; i > pi && i > m; i--)
          if (i < s->popups.count && s->popups.data[i].close_outside) {
            popup_remove_at(s, i);
            if (pi < 0) consumed = true;
          }
        if (pi < 0 && m >= 0) consumed = true;
      }
      if (consumed) continue;
      cw_node t = under;
      if (pi < 0 && top_modal(s) >= 0) t = (cw_node){ 0 };
      if (cw_node_valid(t)) {
        /* focus on click */
        cw_node f = t;
        while (cw_node_valid(f) && !ui_focusable(f, false)) f = ui_parent_control(f);
        if (cw_node_valid(f)) {
          s->focus_visible = false;
          set_focus(s, f);
        } else if (cw_node_valid(s->focus) && CTL(s->focus)->_vt->wants_text) {
          set_focus(s, (cw_node){ 0 });
        }
        s->capture = t;
        if (!deliver_pointer(t, &e)) unhandled(&e);
      } else {
        if (cw_node_valid(s->focus) && b == 0 && top_modal(s) < 0) set_focus(s, (cw_node){ 0 });
        unhandled(&e);
      }
    }
  }
  /* releases (also of presses that began and ended within this frame) */
  for (int b = 0; b < 3; b++) {
    uint32_t bit = 1u << b;
    bool was = (UI.buttons & bit) || (pressed & bit);
    if (was && !(buttons & bit)) {
      cw_ui_event e = ev(CW_UI_MOUSE_UP);
      e.button = b;
      e.buttons = buttons;
      if (UI.drag_active && b == 0) {
        UI.buttons = buttons; /* the drop may start a new drag only after this release */
        ui_dnd_release();
        e.canceled = true; /* the drag source must not treat this as a click */
      } else if (UI.drag_cancel_release && b == 0) {
        e.canceled = true; /* the drag was canceled (Escape) before this release */
      }
      if (b == 0) UI.drag_cancel_release = false;
      cw_node t = cw_node_valid(s->capture) ? s->capture : under;
      if (cw_node_valid(t)) {
        if (!deliver_pointer(t, &e)) unhandled(&e);
      } else unhandled(&e);
    }
  }
  UI.buttons = buttons;
  if (!buttons && cw_node_valid(s->capture)) {
    s->capture = (cw_node){ 0 };
    if (!touch) update_hover(s, valid ? hit_all(s, p, NULL) : (cw_node){ 0 });
  }
  if (touch && !buttons) update_hover(s, (cw_node){ 0 });

  /* wheel */
  if (raw->wheel_x != 0 || raw->wheel_y != 0) {
    cw_ui_event e = ev(CW_UI_WHEEL);
    e.wheel = (cw_vec2){ raw->wheel_x, raw->wheel_y };
    cw_node t = under;
    if (cw_node_valid(t)) {
      if (!deliver_pointer(t, &e)) unhandled(&e);
    } else if (!s->popups.count || top_modal(s) < 0) unhandled(&e);
  }

  UI.wants_pointer = cw_node_valid(under) || cw_node_valid(s->capture) || top_modal(s) >= 0;
}

static void keys_frame(ui_tree_state *s, const cw_raw_input *raw, double dt) {
  for (int sc = 0; sc < CW_KEY_COUNT; sc++) {
    bool down = raw->keys_pressed[sc] || (raw->keys[sc] && !UI.prev_keys[sc]);
    if (!down || is_modifier(sc)) continue;
    if (UI.drag_active && sc == SC_ESCAPE) {
      cw_ui_drag_cancel();
      continue;
    }
    UI.repeat_key = sc;
    UI.repeat_at = UI.time + KEY_DELAY_S;
    send_key(s, sc, false);
  }
  memcpy(UI.prev_keys, raw->keys, sizeof UI.prev_keys);
  if (UI.repeat_key && raw->keys[UI.repeat_key]) {
    while (UI.time >= UI.repeat_at) {
      UI.repeat_at += KEY_RATE_S;
      send_key(s, UI.repeat_key, true);
    }
  } else UI.repeat_key = 0;
  (void)dt;
  /* text */
  if (UI.text_len) {
    cw_node t = key_target(s);
    cw_control *c = cw_node_valid(t) ? CTL(t) : NULL;
    cw_ui_event e = ev(CW_UI_TEXT);
    e.text = UI.text_queue;
    if (!(c && c->_vt->wants_text && call_input(t, &e))) unhandled(&e);
    UI.text_len = 0;
    UI.text_queue[0] = 0;
  }
  /* gamepad navigation (actions) */
  for (int i = 0; i < 6; i++) {
    cw_action a = UI.nav_actions[i];
    if (cw_action_just_pressed(0, a)) {
      UI.repeat_nav = i;
      UI.nav_repeat_at = UI.time + NAV_DELAY_S;
      s->focus_visible = true;
      send_nav(s, i, false);
    }
  }
  if (UI.repeat_nav >= 0 && UI.repeat_nav < 4 && cw_action_pressed(0, UI.nav_actions[UI.repeat_nav])) {
    while (UI.time >= UI.nav_repeat_at) {
      UI.nav_repeat_at += NAV_RATE_S;
      send_nav(s, UI.repeat_nav, true);
    }
  } else UI.repeat_nav = -1;
}

static void tooltip_frame(ui_tree_state *s, double dt) {
  cw_node h = s->hover;
  if (!cw_node_valid(h) || UI.buttons || UI.from_touch) {
    UI.tip_shown = false;
    UI.tip_timer = 0;
    return;
  }
  /* the nearest ancestor-or-self with a tooltip */
  cw_node t = h;
  while (cw_node_valid(t) && !(CTL(t)->tooltip_text && *CTL(t)->tooltip_text)) t = ui_parent_control(t);
  if (!cw_node_valid(t)) {
    UI.tip_shown = false;
    return;
  }
  UI.tip_timer += dt;
  if (!UI.tip_shown && (UI.tip_timer >= TOOLTIP_S || UI.reduced_motion)) {
    UI.tip_shown = true;
    UI.tip_node = t;
    UI.tip_pos = UI.pointer;
  }
}

void ui_input_frame(ui_tree_state *s, double dt) {
  const cw_raw_input *raw = cw_input_raw();
  if (!raw) return;
  ui_captions_frame(dt);
  bool capturing = cw_node_valid(UI.remap_capture);
  ui_remap_frame();
  if (capturing) { /* a RemapButton owns this frame's input (and the one that ended it) */
    memcpy(UI.prev_keys, raw->keys, sizeof UI.prev_keys);
    UI.buttons = raw->mouse_buttons;
    UI.wants_keyboard = UI.wants_pointer = true;
    return;
  }
  pointer_frame(s, raw);
  keys_frame(s, raw, dt);
  tooltip_frame(s, dt);
  UI.wants_keyboard = cw_node_valid(s->focus);
  ui_text_input_update(s);
}

void cw_ui_process(cw_tree *t, double dt) {
  if (!t || !UI.inited) return;
  ui_tree_state *s = ui_state(t);
  float dpi = cw_pixel_ratio(); /* only for the anti-aliasing width (draw.c) */
  if (dpi != UI.dpi) {
    UI.dpi = dpi;
    ui_roots_refresh(s);
    for (int i = 0; i < s->roots.count; i++) ui_mark_layout_dirty(s->roots.data[i]);
  }
  UI.time += dt;
  ui_layout_tree(s);
  if (!cw_node_valid(s->focus)) s->focus = (cw_node){ 0 };
  ui_input_frame(s, dt);
  ui_layout_tree(s);
  UI.stats.popups = (uint32_t)s->popups.count;
}

void cw_ui_layout(cw_tree *t) {
  if (!t || !UI.inited) return;
  ui_layout_tree(ui_state(t));
}

static void frame_hook(double dt) {
  cw_tree *t = cw_current_tree();
  if (t) cw_ui_process(t, dt);
}

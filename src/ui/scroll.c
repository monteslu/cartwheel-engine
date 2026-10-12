/* Scrollbars (a helper drawn and driven by their owner) and ScrollContainer. */
#include "ui_internal.h"

/* ── scrollbar helper ───────────────────────────────────────────────── */

float ui_scrollbar_thickness(cw_node owner) { return cw_control_theme_constant(owner, "ScrollBar", "thickness"); }

static void grabber(cw_node owner, const ui_scrollbar *sb, float *start, float *len, float *track0, float *track_len) {
  cw_ui_style tr = cw_control_theme_style(owner, "ScrollBar", "scroll");
  float t0 = sb->vertical ? sb->rect.y + tr.margin[1] : sb->rect.x + tr.margin[0];
  float tl = sb->vertical ? sb->rect.h - SM_H(&tr) : sb->rect.w - SM_W(&tr);
  float minl = cw_control_theme_constant(owner, "ScrollBar", "minimum_grabber");
  float l = sb->max > 0 ? tl * sb->page / sb->max : tl;
  l = CW_CLAMP(l, CW_MIN(minl, tl), tl);
  float range = sb->max - sb->page;
  float s = t0 + (range > 0 ? (tl - l) * sb->value / range : 0);
  *start = s, *len = l, *track0 = t0, *track_len = tl;
}

void ui_scrollbar_draw(cw_node owner, ui_scrollbar *sb) {
  if (sb->rect.w <= 0 || sb->rect.h <= 0) return;
  cw_ui_style tr = cw_control_theme_style(owner, "ScrollBar", "scroll");
  cw_ui_draw_style(&tr, sb->rect);
  float s, l, t0, tl;
  grabber(owner, sb, &s, &l, &t0, &tl);
  cw_ui_style g = cw_control_theme_style(owner, "ScrollBar", sb->dragging ? "grabber_pressed" : sb->hovered ? "grabber_hover" : "grabber");
  cw_rect r = sb->vertical ? (cw_rect){ sb->rect.x + tr.margin[0], s, sb->rect.w - SM_W(&tr), l }
                           : (cw_rect){ s, sb->rect.y + tr.margin[1], l, sb->rect.h - SM_H(&tr) };
  cw_ui_draw_style(&g, r);
}

bool ui_scrollbar_input(cw_node owner, ui_scrollbar *sb, const cw_ui_event *e, bool *changed) {
  *changed = false;
  if (sb->rect.w <= 0 || sb->rect.h <= 0) return false;
  float at = sb->vertical ? e->pos.y : e->pos.x;
  float s, l, t0, tl;
  grabber(owner, sb, &s, &l, &t0, &tl);
  switch (e->type) {
  case CW_UI_MOUSE_MOVE: {
    bool h = rect_has(sb->rect, e->pos);
    sb->hovered = h;
    if (!sb->dragging) return false;
    float range = sb->max - sb->page, room = tl - l;
    float v = sb->drag_from_value + (room > 0 ? (at - sb->drag_from_pos) * range / room : 0);
    v = ui_scrollbar_clamp(sb, v);
    if (v != sb->value) sb->value = v, *changed = true;
    return true;
  }
  case CW_UI_MOUSE_DOWN:
    if (e->button != 0 || !rect_has(sb->rect, e->pos)) return false;
    if (at >= s && at < s + l) {
      sb->dragging = true;
      sb->drag_from_pos = at;
      sb->drag_from_value = sb->value;
    } else {
      float v = ui_scrollbar_clamp(sb, sb->value + (at < s ? -sb->page : sb->page));
      if (v != sb->value) sb->value = v, *changed = true;
    }
    return true;
  case CW_UI_MOUSE_UP:
    if (!sb->dragging) return false;
    sb->dragging = false;
    return true;
  default: return false;
  }
}

/* ── ScrollContainer ────────────────────────────────────────────────── */

typedef struct {
  int32_t horizontal_scroll_mode, vertical_scroll_mode; /* 0 disabled 1 auto 2 always */
  float scroll_horizontal, scroll_vertical;
  ui_scrollbar hb, vb;
  cw_rect view;    /* content viewport, local */
  cw_vec2 content; /* content size */
  bool touch_drag;
  cw_vec2 drag_from, drag_scroll;
} scroll_data;
static const cw_node_type *scroll_t;

static cw_node content_child(cw_node n) {
  cw_node *ch;
  int k = ui_children(n, &ch);
  return k ? ch[0] : (cw_node){ 0 };
}

static cw_vec2 scroll_min(cw_node n, void *d) {
  scroll_data *s = d;
  cw_ui_style p = T_STYLE(n, "panel");
  cw_vec2 m = { 0, 0 };
  cw_node c = content_child(n);
  float th = ui_scrollbar_thickness(n);
  if (cw_node_valid(c)) {
    cw_vec2 cm = cw_control_min_size(c);
    if (s->horizontal_scroll_mode == 0) m.x = cm.x;
    if (s->vertical_scroll_mode == 0) m.y = cm.y;
  }
  if (s->vertical_scroll_mode == 2) m.x += th;
  if (s->horizontal_scroll_mode == 2) m.y += th;
  return (cw_vec2){ m.x + SM_W(&p), m.y + SM_H(&p) };
}

static void sync_bars(scroll_data *s) {
  s->scroll_horizontal = ui_scrollbar_clamp(&s->hb, s->scroll_horizontal);
  s->scroll_vertical = ui_scrollbar_clamp(&s->vb, s->scroll_vertical);
  s->hb.value = s->scroll_horizontal;
  s->vb.value = s->scroll_vertical;
}

static void scroll_layout(cw_node n, void *d, cw_vec2 sz) {
  scroll_data *s = d;
  cw_ui_style p = T_STYLE(n, "panel");
  cw_rect in = style_content(&p, (cw_rect){ 0, 0, sz.x, sz.y });
  float th = ui_scrollbar_thickness(n);
  cw_node c = content_child(n);
  cw_vec2 cm = cw_node_valid(c) ? cw_control_min_size(c) : (cw_vec2){ 0, 0 };
  bool vbar = s->vertical_scroll_mode == 2, hbar = s->horizontal_scroll_mode == 2;
  for (int pass = 0; pass < 2; pass++) { /* a bar on one axis can make the other needed */
    float vw = in.w - (vbar ? th : 0), vh = in.h - (hbar ? th : 0);
    if (s->vertical_scroll_mode == 1) vbar = cm.y > vh;
    if (s->horizontal_scroll_mode == 1) hbar = cm.x > vw;
  }
  s->view = (cw_rect){ in.x, in.y, in.w - (vbar ? th : 0), in.h - (hbar ? th : 0) };
  float cw_ = s->horizontal_scroll_mode == 0 ? s->view.w : CW_MAX(cm.x, s->view.w);
  float ch_ = s->vertical_scroll_mode == 0 ? s->view.h : CW_MAX(cm.y, s->view.h);
  /* content that does not expand on a scrolling axis keeps its minimum */
  if (cw_node_valid(c)) {
    cw_control *cc = CTL(c);
    if (s->horizontal_scroll_mode && !(cc->size_flags_horizontal & CW_SIZE_EXPAND)) cw_ = cm.x;
    if (s->vertical_scroll_mode && !(cc->size_flags_vertical & CW_SIZE_EXPAND)) ch_ = cm.y;
    if (s->horizontal_scroll_mode && (cc->size_flags_horizontal & CW_SIZE_FILL) && cw_ < s->view.w) cw_ = s->view.w;
    if (s->vertical_scroll_mode && (cc->size_flags_vertical & CW_SIZE_FILL) && ch_ < s->view.h) ch_ = s->view.h;
    ui_arrange(c, (cw_rect){ 0, 0, cw_, ch_ });
    cw_rect r = CTL(c)->rect;
    cw_ = r.w, ch_ = r.h;
  }
  s->content = (cw_vec2){ cw_, ch_ };
  s->vb = (ui_scrollbar){ .vertical = true, .value = s->scroll_vertical, .page = s->view.h, .max = ch_,
                          .dragging = s->vb.dragging, .hovered = s->vb.hovered, .drag_from_value = s->vb.drag_from_value,
                          .drag_from_pos = s->vb.drag_from_pos,
                          .rect = vbar ? (cw_rect){ in.x + in.w - th, in.y, th, s->view.h } : (cw_rect){ 0 } };
  s->hb = (ui_scrollbar){ .vertical = false, .value = s->scroll_horizontal, .page = s->view.w, .max = cw_,
                          .dragging = s->hb.dragging, .hovered = s->hb.hovered, .drag_from_value = s->hb.drag_from_value,
                          .drag_from_pos = s->hb.drag_from_pos,
                          .rect = hbar ? (cw_rect){ in.x, in.y + in.h - th, s->view.w, th } : (cw_rect){ 0 } };
  sync_bars(s);
}

static cw_vec2 scroll_offset(cw_node n, void *d) {
  (void)n;
  scroll_data *s = d;
  return (cw_vec2){ s->view.x - s->scroll_horizontal, s->view.y - s->scroll_vertical };
}

static bool scroll_children_point(cw_node n, void *d, cw_vec2 p) {
  (void)n;
  return rect_has(((scroll_data *)d)->view, p);
}

static void scroll_draw(cw_node n, void *d) {
  scroll_data *s = d;
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel");
  cw_ui_draw_style(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
  cw_ui_push_clip(s->view);
}

static void scroll_draw_over(cw_node n, void *d) {
  scroll_data *s = d;
  cw_ui_pop_clip();
  if (s->vb.rect.w > 0) ui_scrollbar_draw(n, &s->vb);
  if (s->hb.rect.w > 0) ui_scrollbar_draw(n, &s->hb);
}

static void set_scroll(cw_node n, scroll_data *s, float h, float v) {
  float oh = s->scroll_horizontal, ov = s->scroll_vertical;
  s->scroll_horizontal = h;
  s->scroll_vertical = v;
  sync_bars(s);
  if (oh != s->scroll_horizontal || ov != s->scroll_vertical) ui_emit0(n, "scroll_changed");
}

static bool scroll_input(cw_node n, void *d, const cw_ui_event *e) {
  scroll_data *s = d;
  bool ch;
  if (!s->touch_drag) {
    if (ui_scrollbar_input(n, &s->vb, e, &ch)) {
      if (ch) set_scroll(n, s, s->scroll_horizontal, s->vb.value);
      return true;
    }
    if (ui_scrollbar_input(n, &s->hb, e, &ch)) {
      if (ch) set_scroll(n, s, s->hb.value, s->scroll_vertical);
      return true;
    }
  }
  switch (e->type) {
  case CW_UI_WHEEL: {
    float step = cw_ui_line_height(T_FONT(n), T_FSIZE(n)) * 3;
    bool vert = s->vertical_scroll_mode && ui_scrollbar_needed(&s->vb) && !e->shift && e->wheel.y != 0;
    if (vert) {
      float before = s->scroll_vertical;
      set_scroll(n, s, s->scroll_horizontal, s->scroll_vertical - e->wheel.y * step);
      return s->scroll_vertical != before;
    }
    if (s->horizontal_scroll_mode && ui_scrollbar_needed(&s->hb)) {
      float before = s->scroll_horizontal;
      float amt = e->wheel.x != 0 ? e->wheel.x : e->wheel.y;
      set_scroll(n, s, s->scroll_horizontal - amt * step, s->scroll_vertical);
      return s->scroll_horizontal != before;
    }
    return false;
  }
  case CW_UI_MOUSE_DOWN:
    if (e->touch && rect_has(s->view, e->pos)) {
      s->touch_drag = true;
      s->drag_from = e->global;
      s->drag_scroll = (cw_vec2){ s->scroll_horizontal, s->scroll_vertical };
      return true;
    }
    return false;
  case CW_UI_MOUSE_MOVE:
    if (!s->touch_drag) return false;
    set_scroll(n, s, s->drag_scroll.x - (e->global.x - s->drag_from.x), s->drag_scroll.y - (e->global.y - s->drag_from.y));
    return true;
  case CW_UI_MOUSE_UP:
    if (!s->touch_drag) return false;
    s->touch_drag = false;
    return true;
  default: return false;
  }
}

bool ui_scroll_steal(cw_node n, cw_vec2 from, cw_vec2 now) {
  scroll_data *s = cw_node_data_t(n, scroll_t);
  if (!s) return false;
  cw_vec2 d = cw_v2_sub(now, from);
  bool vert = fabsf(d.y) >= fabsf(d.x);
  bool can = vert ? s->vertical_scroll_mode && ui_scrollbar_needed(&s->vb) : s->horizontal_scroll_mode && ui_scrollbar_needed(&s->hb);
  if (!can) return false;
  s->touch_drag = true;
  s->drag_from = from;
  s->drag_scroll = (cw_vec2){ s->scroll_horizontal, s->scroll_vertical };
  set_scroll(n, s, s->drag_scroll.x - d.x, s->drag_scroll.y - d.y);
  return true;
}

void cw_scroll_set(cw_node n, cw_vec2 o) {
  scroll_data *s = cw_node_data_t(n, scroll_t);
  if (s) set_scroll(n, s, o.x, o.y);
}
cw_vec2 cw_scroll_get(cw_node n) {
  scroll_data *s = cw_node_data_t(n, scroll_t);
  return s ? (cw_vec2){ s->scroll_horizontal, s->scroll_vertical } : (cw_vec2){ 0, 0 };
}
cw_vec2 cw_scroll_max(cw_node n) {
  scroll_data *s = cw_node_data_t(n, scroll_t);
  if (!s) return (cw_vec2){ 0, 0 };
  return (cw_vec2){ CW_MAX(s->content.x - s->view.w, 0), CW_MAX(s->content.y - s->view.h, 0) };
}
void cw_scroll_ensure_visible(cw_node n, cw_node desc) {
  scroll_data *s = cw_node_data_t(n, scroll_t);
  if (!s) return;
  cw_rect sr = cw_control_global_rect(n), r = cw_control_global_rect(desc);
  float vx = sr.x + s->view.x, vy = sr.y + s->view.y;
  float h = s->scroll_horizontal, v = s->scroll_vertical;
  if (r.y < vy) v -= vy - r.y;
  else if (r.y + r.h > vy + s->view.h) v += CW_MIN(r.y + r.h - (vy + s->view.h), r.y - vy);
  if (r.x < vx) h -= vx - r.x;
  else if (r.x + r.w > vx + s->view.w) h += CW_MIN(r.x + r.w - (vx + s->view.w), r.x - vx);
  set_scroll(n, s, h, v);
}

static void scroll_init(cw_node n, void *d) {
  scroll_data *s = d;
  s->horizontal_scroll_mode = s->vertical_scroll_mode = 1;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
}
static void scroll_changed(cw_node n, void *d, const cw_prop *p) {
  scroll_data *s = d;
  if (!strncmp(p->name, "scroll_", 7)) sync_bars(s);
  else ui_mark_min_dirty(n);
}
static const char *const mode_names[] = { "DISABLED", "AUTO", "ALWAYS", NULL };
static const cw_prop scroll_props[] = {
  { .name = "horizontal_scroll_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT,
    .offset = (int)offsetof(scroll_data, horizontal_scroll_mode), .size = 4, .enum_names = mode_names, .doc = "Horizontal scrolling and bar." },
  { .name = "vertical_scroll_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT,
    .offset = (int)offsetof(scroll_data, vertical_scroll_mode), .size = 4, .enum_names = mode_names, .doc = "Vertical scrolling and bar." },
  CW_PROP_FIELD(scroll_data, scroll_horizontal, CW_T_FLOAT, CW_PROP_DEFAULT, "Horizontal scroll offset, UI units."),
  CW_PROP_FIELD(scroll_data, scroll_vertical, CW_T_FLOAT, CW_PROP_DEFAULT, "Vertical scroll offset, UI units."),
  CW_PROP_END
};
static const char *const scroll_signals[] = { "scroll_changed", NULL };
static cw_node_type scroll_type = { .name = "ScrollContainer", .parent = "Container", .data_size = sizeof(scroll_data),
                                    .props = scroll_props, .signals = scroll_signals, .init = scroll_init,
                                    .prop_changed = scroll_changed, .doc = "Scrolls one content child." };

void ui_register_scroll(void) {
  cw_node_type_register(&scroll_type);
  scroll_t = &scroll_type;
  cw_ui_class_register("ScrollContainer", &(cw_ui_class){ .min_size = scroll_min, .layout = scroll_layout,
                                                          .draw = scroll_draw, .draw_over = scroll_draw_over,
                                                          .input = scroll_input, .child_offset = scroll_offset,
                                                          .children_point = scroll_children_point });
}

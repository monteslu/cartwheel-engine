/* Containers: Container, HBox/VBox, Grid, Margin, Center, Panel,
 * PanelContainer, HSplit/VSplit, TabContainer, HFlow/VFlow, AspectRatio. */
#include "ui_internal.h"

int ui_children(cw_node n, cw_node **out) {
  int k = cw_node_child_count(n), m = 0;
  cw_node *a = cw_frame_alloc(sizeof(cw_node) * (size_t)(k ? k : 1));
  for (int i = 0; i < k; i++) {
    cw_node ch = cw_node_child(n, i);
    cw_control *c = CTL(ch);
    if (c && c->visible && !(c->_flags & CF_TOP_LEVEL)) a[m++] = ch;
  }
  *out = a;
  return m;
}

static cw_vec2 max_child_min(cw_node n) {
  cw_node *ch;
  int k = ui_children(n, &ch);
  cw_vec2 m = { 0, 0 };
  for (int i = 0; i < k; i++) {
    cw_vec2 c = cw_control_min_size(ch[i]);
    m.x = CW_MAX(m.x, c.x);
    m.y = CW_MAX(m.y, c.y);
  }
  return m;
}

static void fit_all(cw_node n, cw_rect r) {
  cw_node *ch;
  int k = ui_children(n, &ch);
  for (int i = 0; i < k; i++) cw_control_fit_child(ch[i], r);
}

/* ── Container ──────────────────────────────────────────────────────── */

static void container_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
}
static cw_vec2 container_min(cw_node n, void *d) {
  (void)d;
  return max_child_min(n);
}
static void container_layout(cw_node n, void *d, cw_vec2 s) {
  (void)d;
  fit_all(n, (cw_rect){ 0, 0, s.x, s.y });
}
static cw_node_type container_type = { .name = "Container", .parent = "Control", .init = container_init,
                                       .doc = "Base for controls that place their children (anchors are ignored)." };

/* ── BoxContainer ───────────────────────────────────────────────────── */

typedef struct { int32_t alignment; bool vertical; } box_data;
static const cw_node_type *box_t;

static float box_sep(cw_node n) { return T_CONST(n, "separation"); }

static cw_vec2 box_min(cw_node n, void *d) {
  box_data *b = d;
  cw_node *ch;
  int k = ui_children(n, &ch);
  float main = 0, cross = 0;
  for (int i = 0; i < k; i++) {
    cw_vec2 m = cw_control_min_size(ch[i]);
    main += b->vertical ? m.y : m.x;
    cross = CW_MAX(cross, b->vertical ? m.x : m.y);
  }
  if (k > 1) main += box_sep(n) * (float)(k - 1);
  return b->vertical ? (cw_vec2){ cross, main } : (cw_vec2){ main, cross };
}

static void box_layout(cw_node n, void *d, cw_vec2 s) {
  box_data *b = d;
  cw_node *ch;
  int k = ui_children(n, &ch);
  if (!k) return;
  float sep = box_sep(n);
  float size = b->vertical ? s.y : s.x, cross = b->vertical ? s.x : s.y;
  float *mins = cw_frame_alloc(sizeof(float) * (size_t)k), *fin = cw_frame_alloc(sizeof(float) * (size_t)k);
  bool *st = cw_frame_alloc(sizeof(bool) * (size_t)k);
  float stretch_min = 0, stretch_avail = 0, ratio_total = 0;
  int expanders = 0;
  for (int i = 0; i < k; i++) {
    cw_control *c = CTL(ch[i]);
    cw_vec2 m = cw_control_min_size(ch[i]);
    mins[i] = fin[i] = b->vertical ? m.y : m.x;
    stretch_min += mins[i];
    int fl = b->vertical ? c->size_flags_vertical : c->size_flags_horizontal;
    st[i] = (fl & CW_SIZE_EXPAND) != 0 && c->size_flags_stretch_ratio > 0;
    if (st[i]) {
      stretch_avail += mins[i];
      ratio_total += c->size_flags_stretch_ratio;
      expanders++;
    }
  }
  float avail = size - sep * (float)(k - 1);
  float diff = avail - stretch_min;
  if (diff < 0) diff = 0;
  stretch_avail += diff;
  /* children whose share would be below their minimum keep the minimum and
   * leave the share to the others (Godot's refit) */
  while (ratio_total > 0) {
    bool refit = true;
    for (int i = 0; i < k; i++) {
      if (!st[i]) continue;
      float share = stretch_avail * CTL(ch[i])->size_flags_stretch_ratio / ratio_total;
      if (share < mins[i]) {
        st[i] = false;
        ratio_total -= CTL(ch[i])->size_flags_stretch_ratio;
        stretch_avail -= mins[i];
        fin[i] = mins[i];
        refit = false;
        break;
      }
      fin[i] = share;
    }
    if (refit) break;
  }
  float ofs = 0;
  if (!expanders) {
    if (b->alignment == 1) ofs = ui_round(diff * 0.5f);
    else if (b->alignment == 2) ofs = diff;
  }
  for (int i = 0; i < k; i++) {
    float from = ui_round(ofs), to = ui_round(ofs + fin[i]);
    cw_rect r = b->vertical ? (cw_rect){ 0, from, cross, to - from } : (cw_rect){ from, 0, to - from, cross };
    cw_control_fit_child(ch[i], r);
    ofs += fin[i] + sep;
  }
}

static void hbox_init(cw_node n, void *d) {
  (void)d;
  ((box_data *)cw_node_data_t(n, box_t))->vertical = false;
}
static void vbox_init(cw_node n, void *d) {
  (void)d;
  ((box_data *)cw_node_data_t(n, box_t))->vertical = true;
}
static const char *const align_names[] = { "BEGIN", "CENTER", "END", NULL };
static const cw_prop box_props[] = {
  { .name = "alignment", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(box_data, alignment),
    .size = 4, .enum_names = align_names, .doc = "Where children sit when none expands." },
  CW_PROP_END
};
static cw_node_type box_type = { .name = "BoxContainer", .parent = "Container", .data_size = sizeof(box_data),
                                 .props = box_props, .doc = "Children in a row or column with theme separation." };
static cw_node_type hbox_type = { .name = "HBoxContainer", .parent = "BoxContainer", .init = hbox_init,
                                  .doc = "Children left to right." };
static cw_node_type vbox_type = { .name = "VBoxContainer", .parent = "BoxContainer", .init = vbox_init,
                                  .doc = "Children top to bottom." };

/* ── GridContainer ──────────────────────────────────────────────────── */

typedef struct { int32_t columns; } grid_data;

typedef struct { int nc, nr; float *cw, *rh; bool *cx, *rx; } grid_info;
static grid_info grid_measure(cw_node n, grid_data *g, cw_node *ch, int k) {
  grid_info gi;
  gi.nc = g->columns > 0 ? g->columns : 1;
  gi.nr = (k + gi.nc - 1) / gi.nc;
  gi.cw = cw_frame_calloc((size_t)gi.nc, sizeof(float));
  gi.rh = cw_frame_calloc((size_t)(gi.nr ? gi.nr : 1), sizeof(float));
  gi.cx = cw_frame_calloc((size_t)gi.nc, sizeof(bool));
  gi.rx = cw_frame_calloc((size_t)(gi.nr ? gi.nr : 1), sizeof(bool));
  for (int i = 0; i < k; i++) {
    int c = i % gi.nc, r = i / gi.nc;
    cw_vec2 m = cw_control_min_size(ch[i]);
    gi.cw[c] = CW_MAX(gi.cw[c], m.x);
    gi.rh[r] = CW_MAX(gi.rh[r], m.y);
    if (CTL(ch[i])->size_flags_horizontal & CW_SIZE_EXPAND) gi.cx[c] = true;
    if (CTL(ch[i])->size_flags_vertical & CW_SIZE_EXPAND) gi.rx[r] = true;
  }
  (void)n;
  return gi;
}

static cw_vec2 grid_min(cw_node n, void *d) {
  cw_node *ch;
  int k = ui_children(n, &ch);
  grid_info gi = grid_measure(n, d, ch, k);
  float hs = T_CONST(n, "h_separation"), vs = T_CONST(n, "v_separation");
  cw_vec2 m = { 0, 0 };
  int cols = CW_MIN(gi.nc, k);
  for (int c = 0; c < cols; c++) m.x += gi.cw[c];
  for (int r = 0; r < gi.nr; r++) m.y += gi.rh[r];
  if (cols > 1) m.x += hs * (float)(cols - 1);
  if (gi.nr > 1) m.y += vs * (float)(gi.nr - 1);
  return m;
}

/* distribute `extra` over expanding tracks equally, keeping minimums */
static void spread(float *sz, const bool *ex, int n, float total) {
  float used = 0;
  int ne = 0;
  for (int i = 0; i < n; i++) {
    used += sz[i];
    if (ex[i]) ne++;
  }
  if (!ne || total <= used) return;
  /* expanding tracks all become equal (at least their min), Godot-style */
  bool *fixed = cw_frame_calloc((size_t)n, sizeof(bool));
  for (;;) {
    float rest = total;
    int cnt = 0;
    for (int i = 0; i < n; i++)
      if (!ex[i] || fixed[i]) rest -= sz[i];
      else cnt++;
    if (!cnt) return;
    float each = rest / (float)cnt;
    bool again = false;
    for (int i = 0; i < n; i++)
      if (ex[i] && !fixed[i] && sz[i] > each) fixed[i] = true, again = true;
    if (again) continue;
    for (int i = 0; i < n; i++)
      if (ex[i] && !fixed[i]) sz[i] = each;
    return;
  }
}

static void grid_layout(cw_node n, void *d, cw_vec2 s) {
  cw_node *ch;
  int k = ui_children(n, &ch);
  if (!k) return;
  grid_info gi = grid_measure(n, d, ch, k);
  float hs = T_CONST(n, "h_separation"), vs = T_CONST(n, "v_separation");
  int cols = CW_MIN(gi.nc, k);
  spread(gi.cw, gi.cx, cols, s.x - hs * (float)(cols - 1));
  spread(gi.rh, gi.rx, gi.nr, s.y - vs * (float)(gi.nr - 1));
  float *cx = cw_frame_alloc(sizeof(float) * (size_t)(cols + 1)), *ry = cw_frame_alloc(sizeof(float) * (size_t)(gi.nr + 1));
  float acc = 0;
  for (int c = 0; c < cols; c++) cx[c] = acc, acc += gi.cw[c] + hs;
  acc = 0;
  for (int r = 0; r < gi.nr; r++) ry[r] = acc, acc += gi.rh[r] + vs;
  for (int i = 0; i < k; i++) {
    int c = i % gi.nc, r = i / gi.nc;
    float x0 = ui_round(cx[c]), x1 = ui_round(cx[c] + gi.cw[c]), y0 = ui_round(ry[r]), y1 = ui_round(ry[r] + gi.rh[r]);
    cw_control_fit_child(ch[i], (cw_rect){ x0, y0, x1 - x0, y1 - y0 });
  }
}
static void grid_init(cw_node n, void *d) {
  (void)n;
  ((grid_data *)d)->columns = 1;
}
static const cw_prop grid_props[] = {
  CW_PROP_FIELD_RANGE(grid_data, columns, CW_T_INT, 1, 1024, "Children per row."),
  CW_PROP_END
};
static cw_node_type grid_type = { .name = "GridContainer", .parent = "Container", .data_size = sizeof(grid_data),
                                  .props = grid_props, .init = grid_init,
                                  .doc = "Children in a grid of `columns`; a column/row expands when a child in it does." };

/* ── MarginContainer ────────────────────────────────────────────────── */

static void margins(cw_node n, float m[4]) {
  m[0] = T_CONST(n, "margin_left");
  m[1] = T_CONST(n, "margin_top");
  m[2] = T_CONST(n, "margin_right");
  m[3] = T_CONST(n, "margin_bottom");
}
static cw_vec2 margin_min(cw_node n, void *d) {
  (void)d;
  float m[4];
  margins(n, m);
  cw_vec2 c = max_child_min(n);
  return (cw_vec2){ c.x + m[0] + m[2], c.y + m[1] + m[3] };
}
static void margin_layout(cw_node n, void *d, cw_vec2 s) {
  (void)d;
  float m[4];
  margins(n, m);
  fit_all(n, (cw_rect){ m[0], m[1], s.x - m[0] - m[2], s.y - m[1] - m[3] });
}
static cw_node_type margin_type = { .name = "MarginContainer", .parent = "Container",
                                    .doc = "Children inset by theme constants margin_left/top/right/bottom." };

/* ── CenterContainer ────────────────────────────────────────────────── */

typedef struct { bool use_top_left; } center_data;
static void center_layout(cw_node n, void *d, cw_vec2 s) {
  center_data *c = d;
  cw_node *ch;
  int k = ui_children(n, &ch);
  for (int i = 0; i < k; i++) {
    cw_vec2 m = cw_control_min_size(ch[i]);
    float x = c->use_top_left ? -ui_round(m.x * 0.5f) : ui_round((s.x - m.x) * 0.5f);
    float y = c->use_top_left ? -ui_round(m.y * 0.5f) : ui_round((s.y - m.y) * 0.5f);
    ui_arrange(ch[i], (cw_rect){ x, y, m.x, m.y });
  }
}
static const cw_prop center_props[] = {
  CW_PROP_FIELD(center_data, use_top_left, CW_T_BOOL, CW_PROP_DEFAULT, "Centre children on the top-left corner."),
  CW_PROP_END
};
static cw_node_type center_type = { .name = "CenterContainer", .parent = "Container", .data_size = sizeof(center_data),
                                    .props = center_props, .doc = "Children at their minimum size, centred." };

/* ── Panel / PanelContainer ─────────────────────────────────────────── */

static void panel_draw(cw_node n, void *d) {
  (void)d;
  cw_control *c = CTL(n);
  cw_ui_style s = T_STYLE(n, "panel");
  cw_ui_draw_style(&s, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
}
static cw_vec2 panelc_min(cw_node n, void *d) {
  (void)d;
  cw_ui_style s = T_STYLE(n, "panel");
  cw_vec2 m = max_child_min(n);
  return (cw_vec2){ m.x + SM_W(&s), m.y + SM_H(&s) };
}
static void panelc_layout(cw_node n, void *d, cw_vec2 sz) {
  (void)d;
  cw_ui_style s = T_STYLE(n, "panel");
  fit_all(n, style_content(&s, (cw_rect){ 0, 0, sz.x, sz.y }));
}
static void panelc_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->mouse_filter = CW_MOUSE_STOP;
}
static cw_node_type panel_type = { .name = "Panel", .parent = "Control", .doc = "Draws the theme's panel style; children by anchors." };
static cw_node_type panelc_type = { .name = "PanelContainer", .parent = "Container", .init = panelc_init,
                                    .doc = "Draws the panel style; children fit inside its content margins." };

/* ── SplitContainer ─────────────────────────────────────────────────── */

typedef struct {
  float split_offset;
  bool collapsed;
  bool vertical;
  bool dragging, hover;
  float drag_from, drag_offset;
  float sep_pos; /* computed separator position (main axis) */
} split_data;
static const cw_node_type *split_t;

static cw_vec2 split_min(cw_node n, void *d) {
  split_data *sp = d;
  cw_node *ch;
  int k = ui_children(n, &ch);
  float sep = T_CONST(n, "separation");
  float main = 0, cross = 0;
  for (int i = 0; i < k && i < 2; i++) {
    cw_vec2 m = cw_control_min_size(ch[i]);
    main += sp->vertical ? m.y : m.x;
    cross = CW_MAX(cross, sp->vertical ? m.x : m.y);
  }
  if (k >= 2 && !sp->collapsed) main += sep;
  return sp->vertical ? (cw_vec2){ cross, main } : (cw_vec2){ main, cross };
}

static void split_layout(cw_node n, void *d, cw_vec2 s) {
  split_data *sp = d;
  cw_node *ch;
  int k = ui_children(n, &ch);
  if (k == 0) return;
  if (k == 1) {
    cw_control_fit_child(ch[0], (cw_rect){ 0, 0, s.x, s.y });
    return;
  }
  float sep = sp->collapsed ? 0 : T_CONST(n, "separation");
  float size = sp->vertical ? s.y : s.x;
  cw_vec2 m0 = cw_control_min_size(ch[0]), m1 = cw_control_min_size(ch[1]);
  float min0 = sp->vertical ? m0.y : m0.x, min1 = sp->vertical ? m1.y : m1.x;
  int f0 = sp->vertical ? CTL(ch[0])->size_flags_vertical : CTL(ch[0])->size_flags_horizontal;
  int f1 = sp->vertical ? CTL(ch[1])->size_flags_vertical : CTL(ch[1])->size_flags_horizontal;
  bool e0 = f0 & CW_SIZE_EXPAND, e1 = f1 & CW_SIZE_EXPAND;
  float base;
  if (e0 && e1) {
    float r0 = CTL(ch[0])->size_flags_stretch_ratio, r1 = CTL(ch[1])->size_flags_stretch_ratio;
    base = (size - sep) * (r0 / (r0 + r1 > 0 ? r0 + r1 : 1));
  } else if (e0) base = size - sep - min1;
  else base = min0;
  float pos = ui_round(base + (sp->collapsed ? 0 : sp->split_offset));
  pos = CW_CLAMP(pos, min0, CW_MAX(min0, size - sep - min1));
  sp->sep_pos = pos;
  if (sp->vertical) {
    cw_control_fit_child(ch[0], (cw_rect){ 0, 0, s.x, pos });
    cw_control_fit_child(ch[1], (cw_rect){ 0, pos + sep, s.x, s.y - pos - sep });
  } else {
    cw_control_fit_child(ch[0], (cw_rect){ 0, 0, pos, s.y });
    cw_control_fit_child(ch[1], (cw_rect){ pos + sep, 0, s.x - pos - sep, s.y });
  }
}

static cw_rect split_bar(cw_node n, split_data *sp) {
  cw_control *c = CTL(n);
  float sep = T_CONST(n, "separation");
  return sp->vertical ? (cw_rect){ 0, sp->sep_pos, c->rect.w, sep } : (cw_rect){ sp->sep_pos, 0, sep, c->rect.h };
}

static void split_draw(cw_node n, void *d) {
  split_data *sp = d;
  cw_node *ch;
  if (sp->collapsed || ui_children(n, &ch) < 2) return;
  cw_rect b = split_bar(n, sp);
  cw_color col = T_COLOR(n, sp->dragging || sp->hover ? "grabber_hover_color" : "grabber_color");
  float len = 24, th = 2;
  if (sp->vertical) cw_ui_draw_round_rect((cw_rect){ b.x + (b.w - len) * 0.5f, b.y + (b.h - th) * 0.5f, len, th }, 1, col);
  else cw_ui_draw_round_rect((cw_rect){ b.x + (b.w - th) * 0.5f, b.y + (b.h - len) * 0.5f, th, len }, 1, col);
}

static bool split_has_point(cw_node n, void *d, cw_vec2 p) {
  split_data *sp = d;
  cw_node *ch;
  if (sp->collapsed || ui_children(n, &ch) < 2) return false;
  return rect_has(split_bar(n, sp), p);
}

static bool split_input(cw_node n, void *d, const cw_ui_event *e) {
  split_data *sp = d;
  float at = sp->vertical ? e->pos.y : e->pos.x;
  switch (e->type) {
  case CW_UI_MOUSE_DOWN:
    if (e->button != 0 || !split_has_point(n, d, e->pos)) return false;
    sp->dragging = true;
    sp->drag_from = at;
    sp->drag_offset = sp->split_offset;
    return true;
  case CW_UI_MOUSE_MOVE:
    sp->hover = split_has_point(n, d, e->pos);
    if (!sp->dragging) return false;
    {
      /* offsets beyond what the children's minimums allow are clamped so a
       * drag back responds at once */
      float before = sp->sep_pos;
      sp->split_offset = sp->drag_offset + (at - sp->drag_from);
      ui_mark_layout_dirty(n);
      cw_control *c = CTL(n);
      ui_arrange(n, c->rect);
      float delta = sp->sep_pos - before;
      (void)delta;
      ui_emit_float(n, "dragged", sp->split_offset);
    }
    return true;
  case CW_UI_MOUSE_UP:
    if (!sp->dragging) return false;
    sp->dragging = false;
    /* settle the offset to what was applied */
    return true;
  default: return false;
  }
}

static void hsplit_init(cw_node n, void *d) {
  (void)d;
  ((split_data *)cw_node_data_t(n, split_t))->vertical = false;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
}
static void vsplit_init(cw_node n, void *d) {
  (void)d;
  ((split_data *)cw_node_data_t(n, split_t))->vertical = true;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
}
static void split_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}
void cw_split_set_offset(cw_node n, float o) {
  split_data *sp = cw_node_data_t(n, split_t);
  if (!sp) return;
  sp->split_offset = o;
  ui_mark_layout_dirty(n);
}
float cw_split_offset(cw_node n) {
  split_data *sp = cw_node_data_t(n, split_t);
  return sp ? sp->split_offset : 0;
}
static const cw_prop split_props[] = {
  CW_PROP_FIELD(split_data, split_offset, CW_T_FLOAT, CW_PROP_DEFAULT, "Separator offset from its natural spot, UI units."),
  CW_PROP_FIELD(split_data, collapsed, CW_T_BOOL, CW_PROP_DEFAULT, "No separator; the first child keeps its natural size."),
  CW_PROP_END
};
static const char *const split_signals[] = { "dragged", NULL };
static cw_node_type split_type = { .name = "SplitContainer", .parent = "Container", .data_size = sizeof(split_data),
                                   .props = split_props, .signals = split_signals, .prop_changed = split_changed,
                                   .doc = "Two children with a draggable separator." };
static cw_node_type hsplit_type = { .name = "HSplitContainer", .parent = "SplitContainer", .init = hsplit_init,
                                    .doc = "Side by side, vertical separator." };
static cw_node_type vsplit_type = { .name = "VSplitContainer", .parent = "SplitContainer", .init = vsplit_init,
                                    .doc = "Stacked, horizontal separator." };

/* ── TabContainer ───────────────────────────────────────────────────── */

typedef struct {
  int32_t current_tab;
  const char *tab_titles; /* '\n' separated overrides (empty line = child name) */
  int hovered;
  float bar_h;
  bool drag_to_rearrange;
  int press_tab;      /* tab pressed (a drag may start from it), -1 none */
  cw_vec2 press_pos;  /* UI units, canvas origin */
} tabs_data;
static const cw_node_type *tabs_t;

static int tab_pages(cw_node n, cw_node **out) {
  int k = cw_node_child_count(n), m = 0;
  cw_node *a = cw_frame_alloc(sizeof(cw_node) * (size_t)(k ? k : 1));
  for (int i = 0; i < k; i++) {
    cw_node ch = cw_node_child(n, i);
    cw_control *c = CTL(ch);
    if (c && !(c->_flags & CF_TOP_LEVEL)) a[m++] = ch;
  }
  *out = a;
  return m;
}

static const char *tab_title(cw_node n, tabs_data *t, int i, cw_node page) {
  const char *s = t->tab_titles;
  for (int k = 0; s && *s; k++) {
    const char *e = strchr(s, '\n');
    int len = e ? (int)(e - s) : (int)strlen(s);
    if (k == i && len > 0) return ui_tr(n, cw_intern(cw_frame_printf("%.*s", len, s)));
    if (!e) break;
    s = e + 1;
  }
  return ui_tr(n, cw_node_name(page));
}

static cw_vec2 tab_size(cw_node n, const char *title) {
  cw_ui_style st = T_STYLE(n, "tab_unselected");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  return (cw_vec2){ cw_ui_text_width(f, fs, title) + SM_W(&st), cw_ui_line_height(f, fs) + SM_H(&st) };
}

static cw_vec2 tabs_min(cw_node n, void *d) {
  tabs_data *t = d;
  cw_node *pg;
  int k = tab_pages(n, &pg);
  cw_ui_style panel = T_STYLE(n, "panel");
  float bar_w = 0, bar_h = 0, sep = T_CONST(n, "tab_separation");
  for (int i = 0; i < k; i++) {
    cw_vec2 ts = tab_size(n, tab_title(n, t, i, pg[i]));
    bar_w += ts.x + (i ? sep : 0);
    bar_h = CW_MAX(bar_h, ts.y);
  }
  if (!k) bar_h = tab_size(n, "").y;
  t->bar_h = bar_h;
  cw_vec2 m = { 0, 0 };
  for (int i = 0; i < k; i++) {
    cw_vec2 c = cw_control_min_size(pg[i]);
    m.x = CW_MAX(m.x, c.x);
    m.y = CW_MAX(m.y, c.y);
  }
  return (cw_vec2){ CW_MAX(m.x + SM_W(&panel), bar_w), m.y + SM_H(&panel) + bar_h };
}

static void tabs_layout(cw_node n, void *d, cw_vec2 s) {
  tabs_data *t = d;
  cw_node *pg;
  int k = tab_pages(n, &pg);
  if (k && (t->current_tab < 0 || t->current_tab >= k)) t->current_tab = CW_CLAMP(t->current_tab, 0, k - 1);
  cw_ui_style panel = T_STYLE(n, "panel");
  cw_rect body = style_content(&panel, (cw_rect){ 0, t->bar_h, s.x, s.y - t->bar_h });
  for (int i = 0; i < k; i++) {
    cw_control *c = CTL(pg[i]);
    bool show = i == t->current_tab;
    if (c->visible != show) {
      c->visible = show;
      if (!show) ui_forget_node(pg[i]);
      c->_flags |= CF_NEVER_LAID | CF_LAYOUT_DIRTY;
    }
    if (show) cw_control_fit_child(pg[i], body);
  }
}

static float tab_x(cw_node n, tabs_data *t, cw_node *pg, int i) {
  float x = 0, sep = T_CONST(n, "tab_separation");
  for (int j = 0; j < i; j++) x += tab_size(n, tab_title(n, t, j, pg[j])).x + sep;
  return x;
}

static void tabs_draw(cw_node n, void *d) {
  tabs_data *t = d;
  cw_control *c = CTL(n);
  cw_node *pg;
  int k = tab_pages(n, &pg);
  cw_ui_style panel = T_STYLE(n, "panel");
  cw_ui_draw_style(&panel, (cw_rect){ 0, t->bar_h, c->rect.w, c->rect.h - t->bar_h });
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  for (int i = 0; i < k; i++) {
    const char *title = tab_title(n, t, i, pg[i]);
    cw_vec2 ts = tab_size(n, title);
    float x = tab_x(n, t, pg, i);
    bool sel = i == t->current_tab, hov = i == t->hovered && !sel && (CTL(n)->_flags & CF_HOVERED);
    cw_ui_style st = T_STYLE(n, sel ? "tab_selected" : hov ? "tab_hovered" : "tab_unselected");
    cw_rect r = { x, sel ? 0 : 2, ts.x, t->bar_h - (sel ? 0 : 2) };
    if (sel) r.h += 1; /* covers the panel's top border */
    cw_ui_draw_style(&st, r);
    cw_color fc = T_COLOR(n, sel ? "font_selected_color" : hov ? "font_hovered_color" : "font_unselected_color");
    cw_ui_draw_text_in(f, fs, title, (cw_rect){ x + st.margin[0], r.y, ts.x - SM_W(&st), t->bar_h - (sel ? 0 : 2) }, CW_HALIGN_LEFT, fc, false);
  }
}

static int tab_at(cw_node n, tabs_data *t, cw_vec2 p) {
  if (p.y < 0 || p.y >= t->bar_h) return -1;
  cw_node *pg;
  int k = tab_pages(n, &pg);
  for (int i = 0; i < k; i++) {
    float x = tab_x(n, t, pg, i), w = tab_size(n, tab_title(n, t, i, pg[i])).x;
    if (p.x >= x && p.x < x + w) return i;
  }
  return -1;
}

void cw_tab_container_set_current(cw_node n, int i) {
  tabs_data *t = cw_node_data_t(n, tabs_t);
  if (!t) return;
  cw_node *pg;
  int k = tab_pages(n, &pg);
  if (!k) return;
  i = CW_CLAMP(i, 0, k - 1);
  if (i == t->current_tab) return;
  t->current_tab = i;
  ui_mark_layout_dirty(n);
  ui_emit_int(n, "tab_changed", i);
}
int cw_tab_container_current(cw_node n) {
  tabs_data *t = cw_node_data_t(n, tabs_t);
  return t ? t->current_tab : -1;
}
void cw_tab_container_set_title(cw_node n, int index, const char *title) {
  tabs_data *t = cw_node_data_t(n, tabs_t);
  if (!t || index < 0 || index > 65535) return;
  if (!title) title = "";
  /* rebuild the '\n' list with this entry replaced, sized exactly, so any
   * number or length of titles fits. A title is one line: a '\n' in it
   * would split it and shift every later title, so it becomes a space. */
  const char *old = t->tab_titles ? t->tab_titles : "";
  char *buf = cw_frame_alloc(strlen(old) + strlen(title) + (size_t)index + 2);
  size_t len = 0;
  const char *s = old;
  for (int k = 0;; k++) {
    const char *e = strchr(s, '\n');
    size_t l = e ? (size_t)(e - s) : strlen(s);
    if (k) buf[len++] = '\n';
    if (k == index) {
      for (const char *c = title; *c; c++) buf[len++] = *c == '\n' || *c == '\r' ? ' ' : *c;
    } else {
      memcpy(buf + len, s, l);
      len += l;
    }
    if (!e && k >= index) break;
    s = e ? e + 1 : s + l;
  }
  buf[len] = 0;
  t->tab_titles = cw_intern(buf);
  ui_mark_min_dirty(n);
}

int cw_tab_container_page_count(cw_node n) {
  cw_node *pg;
  return cw_node_data_t(n, tabs_t) ? tab_pages(n, &pg) : 0;
}
cw_node cw_tab_container_page(cw_node n, int i) {
  cw_node *pg;
  int k = cw_node_data_t(n, tabs_t) ? tab_pages(n, &pg) : 0;
  return i >= 0 && i < k ? pg[i] : (cw_node){ 0 };
}
const char *cw_tab_container_title(cw_node n, int i) {
  tabs_data *t = cw_node_data_t(n, tabs_t);
  cw_node *pg;
  if (!t || i < 0 || i >= tab_pages(n, &pg)) return NULL;
  return tab_title(n, t, i, pg[i]);
}
int cw_tab_container_tab_at(cw_node n, cw_vec2 local) {
  tabs_data *t = cw_node_data_t(n, tabs_t);
  return t ? tab_at(n, t, local) : -1;
}
cw_rect cw_tab_container_tab_rect(cw_node n, int i) {
  tabs_data *t = cw_node_data_t(n, tabs_t);
  cw_node *pg;
  if (!t || i < 0 || i >= tab_pages(n, &pg)) return (cw_rect){ 0 };
  return (cw_rect){ tab_x(n, t, pg, i), 0, tab_size(n, tab_title(n, t, i, pg[i])).x, t->bar_h };
}

static bool tabs_input(cw_node n, void *d, const cw_ui_event *e) {
  tabs_data *t = d;
  switch (e->type) {
  case CW_UI_MOUSE_MOVE:
    t->hovered = tab_at(n, t, e->pos);
    if (t->drag_to_rearrange && t->press_tab >= 0 && (e->buttons & 1)) {
      float dx = e->global.x - t->press_pos.x, dy = e->global.y - t->press_pos.y;
      if (dx * dx + dy * dy > 36) {
        cw_node *pg;
        int k = tab_pages(n, &pg);
        if (t->press_tab < k) {
          const char *title = tab_title(n, t, t->press_tab, pg[t->press_tab]);
          cw_ui_drag_begin(&(cw_ui_drag){ .source = n, .kind = "tab", .payload = t->press_tab, .text = title, .label = title });
        }
        t->press_tab = -1;
        return true;
      }
    }
    return false;
  case CW_UI_MOUSE_DOWN: {
    int i = tab_at(n, t, e->pos);
    if (i < 0 || e->button != 0) return false;
    cw_tab_container_set_current(n, i);
    t->press_tab = i;
    t->press_pos = e->global;
    return true;
  }
  case CW_UI_MOUSE_UP:
    t->press_tab = -1;
    return false;
  case CW_UI_NAV:
    if (!cw_control_has_focus(n)) return false;
    if (e->nav == CW_NAV_LEFT && t->current_tab > 0) { cw_tab_container_set_current(n, t->current_tab - 1); return true; }
    if (e->nav == CW_NAV_RIGHT) {
      cw_node *pg;
      if (t->current_tab + 1 < tab_pages(n, &pg)) { cw_tab_container_set_current(n, t->current_tab + 1); return true; }
    }
    return false;
  default: return false;
  }
}

static bool tabs_has_point(cw_node n, void *d, cw_vec2 p) {
  (void)n;
  return p.y < ((tabs_data *)d)->bar_h;
}

static void tabs_init(cw_node n, void *d) {
  tabs_data *t = d;
  t->hovered = -1;
  t->press_tab = -1;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
}
static void tabs_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}
static const cw_prop tabs_props[] = {
  CW_PROP_FIELD(tabs_data, current_tab, CW_T_INT, CW_PROP_DEFAULT, "Index of the shown page."),
  CW_PROP_FIELD(tabs_data, tab_titles, CW_T_STRING, CW_PROP_DEFAULT, "Tab titles, one per line (empty = child name)."),
  CW_PROP_FIELD(tabs_data, drag_to_rearrange, CW_T_BOOL, CW_PROP_DEFAULT,
                "Dragging a tab starts a UI drag of kind \"tab\" (payload = tab index, text = title); drop targets decide where it goes."),
  CW_PROP_END
};
static const char *const tabs_signals[] = { "tab_changed", NULL };
static cw_node_type tabs_type = { .name = "TabContainer", .parent = "Container", .data_size = sizeof(tabs_data),
                                  .props = tabs_props, .signals = tabs_signals, .init = tabs_init,
                                  .prop_changed = tabs_changed, .doc = "One page per child, chosen by tabs." };

/* ── FlowContainer ──────────────────────────────────────────────────── */

typedef struct { bool vertical; float last_main; } flow_data;
static const cw_node_type *flow_t;

/* lay children into lines along `main` (width for HFlow); returns the cross extent */
static float flow_run(cw_node n, flow_data *f, float main, bool place) {
  cw_node *ch;
  int k = ui_children(n, &ch);
  float hs = T_CONST(n, "h_separation"), vs = T_CONST(n, "v_separation");
  float ms = f->vertical ? vs : hs, cs = f->vertical ? hs : vs;
  float pos = 0, line = 0, line_cross = 0;
  int line_start = 0;
  for (int i = 0; i <= k; i++) {
    cw_vec2 m = i < k ? cw_control_min_size(ch[i]) : (cw_vec2){ 0, 0 };
    float mm = f->vertical ? m.y : m.x, mc = f->vertical ? m.x : m.y;
    bool wrap = i == k || (pos > 0 && pos + mm > main);
    if (wrap) {
      if (place) {
        float p = 0;
        for (int j = line_start; j < i; j++) {
          cw_vec2 mj = cw_control_min_size(ch[j]);
          float w = f->vertical ? mj.y : mj.x;
          cw_rect r = f->vertical ? (cw_rect){ line, p, line_cross, w } : (cw_rect){ p, line, w, line_cross };
          cw_control_fit_child(ch[j], r);
          p += w + ms;
        }
      }
      if (i == k) break;
      line += line_cross + cs;
      line_cross = 0;
      pos = 0;
      line_start = i;
    }
    pos += mm + ms;
    line_cross = CW_MAX(line_cross, mc);
  }
  return line + line_cross;
}

static cw_vec2 flow_min(cw_node n, void *d) {
  flow_data *f = d;
  cw_vec2 m = max_child_min(n);
  float main = f->last_main > 0 ? f->last_main : 1e9f;
  float cross = flow_run(n, f, main, false);
  return f->vertical ? (cw_vec2){ cross, m.y } : (cw_vec2){ m.x, cross };
}
static void flow_layout(cw_node n, void *d, cw_vec2 s) {
  flow_data *f = d;
  float main = f->vertical ? s.y : s.x;
  if (main != f->last_main) {
    f->last_main = main;
    float cross = flow_run(n, f, main, false);
    cw_control *c = CTL(n);
    if (cross != (f->vertical ? c->_content_min.x : c->_content_min.y)) ui_mark_min_dirty(n); /* height follows width */
  }
  flow_run(n, f, main, true);
}
static void hflow_init(cw_node n, void *d) {
  (void)d;
  ((flow_data *)cw_node_data_t(n, flow_t))->vertical = false;
}
static void vflow_init(cw_node n, void *d) {
  (void)d;
  ((flow_data *)cw_node_data_t(n, flow_t))->vertical = true;
}
static cw_node_type flow_type = { .name = "FlowContainer", .parent = "Container", .data_size = sizeof(flow_data),
                                  .doc = "Children wrap into lines." };
static cw_node_type hflow_type = { .name = "HFlowContainer", .parent = "FlowContainer", .init = hflow_init,
                                   .doc = "Rows that wrap at the width." };
static cw_node_type vflow_type = { .name = "VFlowContainer", .parent = "FlowContainer", .init = vflow_init,
                                   .doc = "Columns that wrap at the height." };

/* ── AspectRatioContainer ───────────────────────────────────────────── */

typedef struct { float ratio; int32_t stretch_mode, alignment_horizontal, alignment_vertical; } aspect_data;
static void aspect_layout(cw_node n, void *d, cw_vec2 s) {
  aspect_data *a = d;
  cw_node *ch;
  int k = ui_children(n, &ch);
  float ratio = a->ratio > 0 ? a->ratio : 1;
  for (int i = 0; i < k; i++) {
    cw_vec2 m = cw_control_min_size(ch[i]);
    float w = s.x, h = s.y;
    switch (a->stretch_mode) {
    case 0: h = w / ratio; break;
    case 1: w = h * ratio; break;
    case 2: if (s.x / s.y > ratio) w = h * ratio; else h = w / ratio; break;
    default: if (s.x / s.y > ratio) h = w / ratio; else w = h * ratio; break;
    }
    w = ui_round(CW_MAX(w, m.x));
    h = ui_round(CW_MAX(h, m.y));
    float ax = a->alignment_horizontal * 0.5f, ay = a->alignment_vertical * 0.5f;
    ui_arrange(ch[i], (cw_rect){ ui_round((s.x - w) * ax), ui_round((s.y - h) * ay), w, h });
  }
}
static void aspect_init(cw_node n, void *d) {
  (void)n;
  aspect_data *a = d;
  a->ratio = 1;
  a->stretch_mode = 2;
  a->alignment_horizontal = a->alignment_vertical = 1;
}
static void any_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}
static const char *const stretch_names[] = { "WIDTH_CONTROLS_HEIGHT", "HEIGHT_CONTROLS_WIDTH", "FIT", "COVER", NULL };
static const cw_prop aspect_props[] = {
  CW_PROP_FIELD(aspect_data, ratio, CW_T_FLOAT, CW_PROP_DEFAULT, "Width / height of the children."),
  { .name = "stretch_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(aspect_data, stretch_mode),
    .size = 4, .enum_names = stretch_names, .doc = "How the ratio is kept." },
  { .name = "alignment_horizontal", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT,
    .offset = (int)offsetof(aspect_data, alignment_horizontal), .size = 4, .enum_names = align_names, .doc = "Horizontal placement." },
  { .name = "alignment_vertical", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT,
    .offset = (int)offsetof(aspect_data, alignment_vertical), .size = 4, .enum_names = align_names, .doc = "Vertical placement." },
  CW_PROP_END
};
static cw_node_type aspect_type = { .name = "AspectRatioContainer", .parent = "Container", .data_size = sizeof(aspect_data),
                                    .props = aspect_props, .init = aspect_init, .prop_changed = any_changed,
                                    .doc = "Children keep a width/height ratio." };

static void box_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}

void ui_register_containers(void) {
  cw_node_type_register(&container_type);
  cw_ui_class_register("Container", &(cw_ui_class){ .theme_type = "Container", .min_size = container_min, .layout = container_layout });
  box_type.prop_changed = box_changed;
  cw_node_type_register(&box_type);
  box_t = &box_type;
  cw_node_type_register(&hbox_type);
  cw_node_type_register(&vbox_type);
  cw_ui_class_register("BoxContainer", &(cw_ui_class){ .theme_type = "BoxContainer", .min_size = box_min, .layout = box_layout });
  grid_type.prop_changed = box_changed;
  cw_node_type_register(&grid_type);
  cw_ui_class_register("GridContainer", &(cw_ui_class){ .min_size = grid_min, .layout = grid_layout });
  cw_node_type_register(&margin_type);
  cw_ui_class_register("MarginContainer", &(cw_ui_class){ .min_size = margin_min, .layout = margin_layout });
  center_type.prop_changed = box_changed;
  cw_node_type_register(&center_type);
  cw_ui_class_register("CenterContainer", &(cw_ui_class){ .layout = center_layout });
  cw_node_type_register(&panel_type);
  cw_ui_class_register("Panel", &(cw_ui_class){ .draw = panel_draw });
  cw_node_type_register(&panelc_type);
  cw_ui_class_register("PanelContainer", &(cw_ui_class){ .min_size = panelc_min, .layout = panelc_layout, .draw = panel_draw });
  cw_node_type_register(&split_type);
  split_t = &split_type;
  cw_node_type_register(&hsplit_type);
  cw_node_type_register(&vsplit_type);
  cw_ui_class_register("SplitContainer", &(cw_ui_class){ .min_size = split_min, .layout = split_layout, .draw_over = split_draw,
                                                         .input = split_input, .has_point = split_has_point });
  cw_node_type_register(&tabs_type);
  tabs_t = &tabs_type;
  cw_ui_class_register("TabContainer", &(cw_ui_class){ .min_size = tabs_min, .layout = tabs_layout, .draw = tabs_draw,
                                                       .input = tabs_input, .has_point = tabs_has_point });
  cw_node_type_register(&flow_type);
  flow_t = &flow_type;
  cw_node_type_register(&hflow_type);
  cw_node_type_register(&vflow_type);
  cw_ui_class_register("FlowContainer", &(cw_ui_class){ .min_size = flow_min, .layout = flow_layout });
  cw_node_type_register(&aspect_type);
  cw_ui_class_register("AspectRatioContainer", &(cw_ui_class){ .layout = aspect_layout });
}

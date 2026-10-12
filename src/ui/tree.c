/* Tree: hierarchical rows with columns, expand/collapse and selection (the
 * editor's scene tree). */
#include "ui_internal.h"

#define TREE_MAX_COLS 8

typedef struct {
  bool alive, collapsed;
  int parent;
  CW_ARRAY(int) children;
  char *text[TREE_MAX_COLS];
  int64_t meta;
  bool selected;                 /* select_mode MULTI: part of the selection */
  uint8_t has_color;             /* bit per column: color[] overrides font_color */
  cw_color color[TREE_MAX_COLS];
} t_item;

typedef struct { const char *title; bool expand; float min_width; } t_col;

typedef struct {
  int32_t columns;
  bool hide_root, column_titles_visible;
  CW_ARRAY(t_item) items;
  int root;
  t_col cols[TREE_MAX_COLS];
  int selected, hover;
  float scroll_y;
  ui_scrollbar vb;
  CW_ARRAY(int) rows;      /* visible items in order */
  CW_ARRAY(int) row_depth;
  bool rows_dirty;
  int32_t select_mode;     /* 0 single, 1 multi (ctrl toggles, shift extends) */
  bool drag_items;         /* dragging a row starts a UI drag of kind "tree_item" */
  int press_item;          /* the row pressed (drag source), -1 = none */
  cw_vec2 press_pos;
  bool press_deferred;     /* multi: a plain press on a selected row selects it alone on release (not on a drag) */
  int anchor;              /* shift-click range start */
} tree_data;
static const cw_node_type *tree_t;

static t_item *IT(tree_data *t, int i) { return i >= 0 && i < t->items.count && t->items.data[i].alive ? &t->items.data[i] : NULL; }

static void rows_rec(tree_data *t, int i, int depth) {
  t_item *it = IT(t, i);
  if (!it) return;
  bool show = !(t->hide_root && i == t->root);
  if (show) {
    cw_array_push(&t->rows, i);
    cw_array_push(&t->row_depth, depth);
  }
  if (it->collapsed && show) return;
  for (int k = 0; k < it->children.count; k++) rows_rec(t, it->children.data[k], show ? depth + 1 : depth);
}
static void rows(tree_data *t) {
  if (!t->rows_dirty) return;
  t->rows_dirty = false;
  cw_array_clear(&t->rows);
  cw_array_clear(&t->row_depth);
  if (t->root >= 0) rows_rec(t, t->root, 0);
}

static float row_h(cw_node n) {
  cw_ui_style it = T_STYLE(n, "item");
  return ceilf(cw_ui_line_height(T_FONT(n), T_FSIZE(n)) + SM_H(&it));
}
static float pitch(cw_node n) { return row_h(n) + T_CONST(n, "v_separation"); }
static bool titles_on(tree_data *t) {
  if (t->column_titles_visible) return true;
  for (int i = 0; i < t->columns; i++)
    if (t->cols[i].title && *t->cols[i].title) return true;
  return false;
}
static float title_h(cw_node n, tree_data *t) {
  if (!titles_on(t)) return 0;
  cw_ui_style s = T_STYLE(n, "title_button");
  return ceilf(cw_ui_line_height(T_FONT(n), T_FSIZE(n)) + SM_H(&s));
}
static cw_rect inner(cw_node n) {
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel");
  return style_content(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
}
static cw_rect body(cw_node n, tree_data *t) {
  cw_rect in = inner(n);
  float th = title_h(n, t);
  return (cw_rect){ in.x, in.y + th, in.w, in.h - th };
}

static void bars(cw_node n, tree_data *t) {
  rows(t);
  cw_rect b = body(n, t);
  float th = ui_scrollbar_thickness(n);
  t->vb.vertical = true;
  t->vb.page = b.h;
  t->vb.max = (float)t->rows.count * pitch(n);
  t->vb.rect = (cw_rect){ b.x + b.w - th, b.y, th, b.h };
  t->scroll_y = ui_scrollbar_clamp(&t->vb, t->scroll_y);
  t->vb.value = t->scroll_y;
}

/* column x positions within width w */
static void col_layout(cw_node n, tree_data *t, float w, float *x, float *cw_) {
  int nc = CW_CLAMP(t->columns, 1, TREE_MAX_COLS);
  float fixed = 0, sep = T_CONST(n, "column_separation");
  int ne = 0;
  for (int i = 0; i < nc; i++) {
    if (t->cols[i].expand) ne++;
    else fixed += t->cols[i].min_width;
  }
  float rest = w - fixed - sep * (float)(nc - 1);
  float acc = 0;
  for (int i = 0; i < nc; i++) {
    float cwi = t->cols[i].expand ? CW_MAX(t->cols[i].min_width, ne ? rest / (float)ne : 0) : t->cols[i].min_width;
    x[i] = ui_round(acc);
    cw_[i] = ui_round(acc + cwi) - x[i];
    acc += cwi + sep;
  }
}

static cw_vec2 tree_min(cw_node n, void *d) {
  tree_data *t = d;
  cw_ui_style p = T_STYLE(n, "panel");
  float w = 0;
  for (int i = 0; i < CW_CLAMP(t->columns, 1, TREE_MAX_COLS); i++) w += t->cols[i].min_width;
  return (cw_vec2){ ceilf(w + SM_W(&p) + ui_scrollbar_thickness(n)), ceilf(row_h(n) + title_h(n, t) + SM_H(&p)) };
}

static void tree_draw(cw_node n, void *d) {
  tree_data *t = d;
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel"), it = T_STYLE(n, "item"), sel = T_STYLE(n, "selected"), hov = T_STYLE(n, "hovered");
  cw_ui_draw_style(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
  bars(n, t);
  cw_rect in = inner(n), b = body(n, t);
  float rw = b.w - (ui_scrollbar_needed(&t->vb) ? t->vb.rect.w : 0);
  int nc = CW_CLAMP(t->columns, 1, TREE_MAX_COLS);
  float cx[TREE_MAX_COLS], cwid[TREE_MAX_COLS];
  col_layout(n, t, rw, cx, cwid);
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), rh = row_h(n), pt = pitch(n), lh = cw_ui_line_height(f, fs), indent = T_CONST(n, "item_margin");
  if (titles_on(t)) {
    cw_ui_style ts = T_STYLE(n, "title_button");
    for (int i = 0; i < nc; i++) {
      cw_rect r = { in.x + cx[i], in.y, cwid[i], title_h(n, t) };
      cw_ui_draw_style(&ts, r);
      cw_ui_draw_text_in(f, fs, ui_tr(n, ui_s(t->cols[i].title)), style_content(&ts, r), CW_HALIGN_LEFT, T_COLOR(n, "title_font_color"), true);
    }
  }
  cw_color fc = T_COLOR(n, "font_color"), fsc = T_COLOR(n, "font_selected_color"), ac = T_COLOR(n, "arrow_color");
  cw_color gc = T_COLOR(n, "guide_color");
  cw_ui_push_clip(b);
  int first = (int)(t->scroll_y / pt), last = (int)((t->scroll_y + b.h) / pt) + 1;
  for (int r = CW_MAX(first, 0); r < t->rows.count && r <= last; r++) {
    int i = t->rows.data[r], depth = t->row_depth.data[r];
    t_item *e = IT(t, i);
    cw_rect rr = { b.x, b.y + (float)r * pt - t->scroll_y, rw, rh };
    if (i == t->selected || (t->select_mode && e->selected)) cw_ui_draw_style(&sel, rr);
    else if (i == t->hover && (c->_flags & CF_HOVERED)) cw_ui_draw_style(&hov, rr);
    cw_rect tr = style_content(&it, rr);
    /* guides: one line per ancestor level */
    for (int k = 1; k <= depth; k++) {
      float gx = tr.x + indent * ((float)k - 0.5f);
      cw_ui_draw_rect((cw_rect){ ui_round(gx), rr.y, 1, pt }, gc);
    }
    float x0 = tr.x + indent * (float)depth;
    if (e->children.count) {
      float a = lh * 0.65f;
      cw_ui_draw_icon(e->collapsed ? CW_ICON_ARROW_RIGHT : CW_ICON_ARROW_DOWN,
                      (cw_rect){ x0 + (indent - a) * 0.5f, tr.y + (tr.h - a) * 0.5f, a, a }, ac);
    }
    x0 += indent;
    bool on = i == t->selected || (t->select_mode && e->selected);
    for (int k = 0; k < nc; k++) {
      cw_color col = (e->has_color & (1u << k)) ? e->color[k] : on ? fsc : fc;
      float colx = b.x + cx[k] + (k ? it.margin[0] : 0);
      float left = k == 0 ? x0 : colx;
      float right = b.x + cx[k] + cwid[k] - it.margin[2];
      if (e->text[k]) cw_ui_draw_text_in(f, fs, e->text[k], (cw_rect){ left, tr.y, right - left, tr.h }, CW_HALIGN_LEFT, col, true);
    }
  }
  cw_ui_pop_clip();
  if (ui_scrollbar_needed(&t->vb)) ui_scrollbar_draw(n, &t->vb);
}

static int row_at(cw_node n, tree_data *t, cw_vec2 p) {
  cw_rect b = body(n, t);
  if (!rect_has(b, p)) return -1;
  float pt = pitch(n);
  int r = (int)floorf((p.y - b.y + t->scroll_y) / pt);
  return r >= 0 && r < t->rows.count ? r : -1;
}

static void ensure_row(cw_node n, tree_data *t, int r) {
  cw_rect b = body(n, t);
  float pt = pitch(n), y = (float)r * pt;
  if (y < t->scroll_y) t->scroll_y = y;
  if (y + row_h(n) > t->scroll_y + b.h) t->scroll_y = y + row_h(n) - b.h;
  bars(n, t);
}

static void select_item(cw_node n, tree_data *t, int i) {
  if (t->selected == i) return;
  t->selected = i;
  ui_emit_int(n, "item_selected", i);
}

/* multi mode: change one item's membership, telling listeners */
static void multi_set(cw_node n, tree_data *t, int i, bool on) {
  t_item *e = IT(t, i);
  if (!e || e->selected == on) return;
  e->selected = on;
  cw_value a[2] = { cw_val_int(i), cw_val_bool(on) };
  cw_emit(n, "multi_selected", a, 2);
}
/* multi mode: exactly i selected (and focused) */
static void multi_only(cw_node n, tree_data *t, int i) {
  for (int k = 0; k < t->items.count; k++)
    if (k != i && t->items.data[k].alive && t->items.data[k].selected) multi_set(n, t, k, false);
  multi_set(n, t, i, true);
  t->anchor = i;
  select_item(n, t, i);
}
/* multi mode: rows from the anchor to i (visible order) */
static void multi_range(cw_node n, tree_data *t, int i, bool keep) {
  int ra = -1, ri = -1;
  for (int r = 0; r < t->rows.count; r++) {
    if (t->rows.data[r] == t->anchor) ra = r;
    if (t->rows.data[r] == i) ri = r;
  }
  if (ra < 0 || ri < 0) {
    multi_only(n, t, i);
    return;
  }
  int lo = CW_MIN(ra, ri), hi = CW_MAX(ra, ri);
  for (int r = 0; r < t->rows.count; r++) {
    bool in = r >= lo && r <= hi;
    if (in) multi_set(n, t, t->rows.data[r], true);
    else if (!keep) multi_set(n, t, t->rows.data[r], false);
  }
  select_item(n, t, i);
}

static void set_collapsed(cw_node n, tree_data *t, int i, bool c) {
  t_item *e = IT(t, i);
  if (!e || e->collapsed == c) return;
  e->collapsed = c;
  t->rows_dirty = true;
  cw_value a[2] = { cw_val_int(i), cw_val_bool(c) };
  cw_emit(n, "item_collapsed", a, 2);
}

static void nav_pick(cw_node n, tree_data *t, int i) {
  if (t->select_mode) multi_only(n, t, i);
  else select_item(n, t, i);
}

static bool tree_input(cw_node n, void *d, const cw_ui_event *e) {
  tree_data *t = d;
  bool ch;
  rows(t);
  if (ui_scrollbar_input(n, &t->vb, e, &ch)) {
    if (ch) t->scroll_y = t->vb.value;
    return true;
  }
  switch (e->type) {
  case CW_UI_MOUSE_MOVE: {
    int r = row_at(n, t, e->pos);
    t->hover = r >= 0 ? t->rows.data[r] : -1;
    if (t->drag_items && t->press_item >= 0 && (e->buttons & 1) && !cw_ui_drag_current()) {
      float dx = e->pos.x - t->press_pos.x, dy = e->pos.y - t->press_pos.y;
      if (dx * dx + dy * dy > 36) {
        t_item *it = IT(t, t->press_item);
        int count = 0;
        if (t->select_mode)
          for (int k = 0; k < t->items.count; k++) count += t->items.data[k].alive && t->items.data[k].selected;
        const char *label = it ? (count > 1 ? cw_frame_printf("%d items", count) : ui_s(it->text[0])) : "";
        if (it && cw_ui_drag_begin(&(cw_ui_drag){ .source = n, .kind = "tree_item", .payload = it->meta,
                                                   .text = ui_s(it->text[0]), .label = label })) {
          t->press_item = -1;
          t->press_deferred = false;
        }
      }
    }
    return false;
  }
  case CW_UI_MOUSE_UP: {
    if (e->button == 0 && t->press_deferred && !e->canceled && IT(t, t->press_item)) multi_only(n, t, t->press_item);
    t->press_item = -1;
    t->press_deferred = false;
    return true;
  }
  case CW_UI_WHEEL: {
    float before = t->scroll_y;
    t->scroll_y -= e->wheel.y * pitch(n) * 3;
    bars(n, t);
    return before != t->scroll_y;
  }
  case CW_UI_MOUSE_DOWN: {
    if (e->button != 0) return true;
    int r = row_at(n, t, e->pos);
    if (r < 0) return true;
    int i = t->rows.data[r];
    t_item *it = IT(t, i);
    cw_ui_style is = T_STYLE(n, "item");
    float indent = T_CONST(n, "item_margin");
    float ax = body(n, t).x + is.margin[0] + indent * (float)t->row_depth.data[r];
    if (it->children.count && e->pos.x >= ax && e->pos.x < ax + indent) {
      set_collapsed(n, t, i, !it->collapsed);
      return true;
    }
    t->press_item = i;
    t->press_pos = e->pos;
    t->press_deferred = false;
    if (t->select_mode) {
      if (e->ctrl) {
        multi_set(n, t, i, !it->selected);
        t->anchor = i;
        if (it->selected) select_item(n, t, i);
      } else if (e->shift) {
        multi_range(n, t, i, false);
      } else if (it->selected) {
        t->press_deferred = true; /* keep a multi-selection for a drag; a click selects it alone on release */
        select_item(n, t, i);
      } else {
        multi_only(n, t, i);
      }
    } else {
      select_item(n, t, i);
    }
    if (e->clicks == 2) {
      if (it->children.count) set_collapsed(n, t, i, !it->collapsed);
      ui_emit_int(n, "item_activated", i);
    }
    return true;
  }
  case CW_UI_NAV: {
    if (!t->rows.count) return false;
    int r = -1;
    for (int k = 0; k < t->rows.count; k++)
      if (t->rows.data[k] == t->selected) r = k;
    t_item *cur = IT(t, t->selected);
    switch (e->nav) {
    case CW_NAV_UP:
      if (r <= 0) { if (r < 0) { nav_pick(n, t, t->rows.data[0]); return true; } return false; }
      nav_pick(n, t, t->rows.data[r - 1]);
      ensure_row(n, t, r - 1);
      return true;
    case CW_NAV_DOWN:
      if (r + 1 >= t->rows.count) return false;
      nav_pick(n, t, t->rows.data[r + 1]);
      ensure_row(n, t, r + 1);
      return true;
    case CW_NAV_LEFT:
      if (!cur) return false;
      if (cur->children.count && !cur->collapsed) set_collapsed(n, t, t->selected, true);
      else if (IT(t, cur->parent) && !(t->hide_root && cur->parent == t->root)) nav_pick(n, t, cur->parent);
      else return false;
      rows(t);
      return true;
    case CW_NAV_RIGHT:
      if (!cur || !cur->children.count) return false;
      if (cur->collapsed) set_collapsed(n, t, t->selected, false);
      else nav_pick(n, t, cur->children.data[0]);
      rows(t);
      return true;
    case CW_NAV_ACCEPT:
      if (!cur) return false;
      ui_emit_int(n, "item_activated", t->selected);
      return true;
    default: return false;
    }
  }
  default: return false;
  }
}

/* ── API ────────────────────────────────────────────────────────────── */

static tree_data *TD(cw_node n) { return cw_node_data_t(n, tree_t); }

int cw_uitree_create_item(cw_node n, int parent, const char *text) {
  tree_data *t = TD(n);
  if (!t) return -1;
  if (parent < 0 && t->root >= 0) parent = t->root;
  t_item it = { .alive = true, .parent = parent };
  it.text[0] = cw_strdup(text ? text : "", CW_MEM_UI);
  cw_array_push(&t->items, it);
  int id = t->items.count - 1;
  if (parent < 0) t->root = id;
  else if (IT(t, parent)) cw_array_push(&IT(t, parent)->children, id);
  t->rows_dirty = true;
  ui_mark_min_dirty(n);
  return id;
}

static void free_rec(tree_data *t, int i) {
  t_item *e = IT(t, i);
  if (!e) return;
  for (int k = 0; k < e->children.count; k++) free_rec(t, e->children.data[k]);
  e = IT(t, i);
  for (int k = 0; k < TREE_MAX_COLS; k++) cw_free(e->text[k]);
  cw_array_free(&e->children);
  e->alive = false;
}

void cw_uitree_remove_item(cw_node n, int i) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  if (!e) return;
  t_item *p = IT(t, e->parent);
  if (p)
    for (int k = 0; k < p->children.count; k++)
      if (p->children.data[k] == i) {
        memmove(p->children.data + k, p->children.data + k + 1, sizeof(int) * (size_t)(p->children.count - k - 1));
        p->children.count--;
        break;
      }
  free_rec(t, i);
  if (i == t->root) t->root = -1;
  if (!IT(t, t->selected)) t->selected = -1;
  if (!IT(t, t->press_item)) t->press_item = -1;
  t->rows_dirty = true;
}

void cw_uitree_clear(cw_node n) {
  tree_data *t = TD(n);
  if (!t) return;
  for (int i = 0; i < t->items.count; i++)
    if (t->items.data[i].alive) {
      for (int k = 0; k < TREE_MAX_COLS; k++) cw_free(t->items.data[i].text[k]);
      cw_array_free(&t->items.data[i].children);
    }
  cw_array_clear(&t->items);
  t->root = t->selected = t->hover = t->press_item = t->anchor = -1;
  t->press_deferred = false;
  t->rows_dirty = true;
  t->scroll_y = 0;
}

void cw_uitree_set_text(cw_node n, int i, int col, const char *text) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  if (!e || col < 0 || col >= TREE_MAX_COLS) return;
  cw_free(e->text[col]);
  e->text[col] = cw_strdup(text ? text : "", CW_MEM_UI);
}
const char *cw_uitree_text(cw_node n, int i, int col) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  return e && col >= 0 && col < TREE_MAX_COLS && e->text[col] ? e->text[col] : "";
}
void cw_uitree_set_collapsed(cw_node n, int i, bool c) {
  tree_data *t = TD(n);
  if (t) set_collapsed(n, t, i, c);
}
bool cw_uitree_collapsed(cw_node n, int i) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  return e && e->collapsed;
}
void cw_uitree_set_columns(cw_node n, int count) {
  tree_data *t = TD(n);
  if (!t) return;
  t->columns = CW_CLAMP(count, 1, TREE_MAX_COLS);
  ui_mark_min_dirty(n);
}
void cw_uitree_set_column_title(cw_node n, int col, const char *title) {
  tree_data *t = TD(n);
  if (!t || col < 0 || col >= TREE_MAX_COLS) return;
  t->cols[col].title = title ? cw_intern(title) : NULL;
  ui_mark_min_dirty(n);
}
void cw_uitree_set_column_expand(cw_node n, int col, bool expand, float min_width) {
  tree_data *t = TD(n);
  if (!t || col < 0 || col >= TREE_MAX_COLS) return;
  t->cols[col].expand = expand;
  t->cols[col].min_width = min_width;
  ui_mark_min_dirty(n);
}
void cw_uitree_set_metadata(cw_node n, int i, int64_t m) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  if (e) e->meta = m;
}
int64_t cw_uitree_metadata(cw_node n, int i) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  return e ? e->meta : 0;
}
void cw_uitree_select(cw_node n, int i) {
  tree_data *t = TD(n);
  if (!t || !IT(t, i)) return;
  select_item(n, t, i);
  /* reveal: expand ancestors, scroll to it */
  for (int p = IT(t, i)->parent; IT(t, p); p = IT(t, p)->parent)
    if (IT(t, p)->collapsed) {
      IT(t, p)->collapsed = false;
      t->rows_dirty = true;
    }
  rows(t);
  for (int r = 0; r < t->rows.count; r++)
    if (t->rows.data[r] == i && CTL(n)->rect.h > 0) ensure_row(n, t, r);
}
int cw_uitree_selected(cw_node n) {
  tree_data *t = TD(n);
  return t ? t->selected : -1;
}
int cw_uitree_item_parent(cw_node n, int i) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  return e ? e->parent : -1;
}
int cw_uitree_item_child_count(cw_node n, int i) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  return e ? e->children.count : 0;
}
int cw_uitree_item_child(cw_node n, int i, int k) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  return e && k >= 0 && k < e->children.count ? e->children.data[k] : -1;
}
void cw_uitree_set_selected(cw_node n, int i, bool on) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  if (!e) return;
  e->selected = on;
  if (on) t->anchor = i;
}
bool cw_uitree_is_selected(cw_node n, int i) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  return e && (t->select_mode ? e->selected : i == t->selected);
}
void cw_uitree_deselect_all(cw_node n) {
  tree_data *t = TD(n);
  if (!t) return;
  for (int k = 0; k < t->items.count; k++) t->items.data[k].selected = false;
  t->selected = -1;
}
void cw_uitree_set_custom_color(cw_node n, int i, int col, const cw_color *c) {
  tree_data *t = TD(n);
  t_item *e = t ? IT(t, i) : NULL;
  if (!e || col < 0 || col >= TREE_MAX_COLS) return;
  if (c) {
    e->color[col] = *c;
    e->has_color |= (uint8_t)(1u << col);
  } else {
    e->has_color &= (uint8_t)~(1u << col);
  }
}
int cw_uitree_item_at(cw_node n, cw_vec2 local, int *section) {
  tree_data *t = TD(n);
  if (section) *section = 0;
  if (!t) return -1;
  rows(t);
  bars(n, t);
  int r = row_at(n, t, local);
  if (r < 0) return -1;
  if (section) {
    cw_rect b = body(n, t);
    float pt = pitch(n), y = local.y - (b.y + (float)r * pt - t->scroll_y);
    *section = y < pt * 0.25f ? -1 : y > pt * 0.75f ? 1 : 0;
  }
  return t->rows.data[r];
}
int cw_uitree_visible_rows(cw_node n) {
  tree_data *t = TD(n);
  if (!t) return 0;
  rows(t);
  return t->rows.count;
}

/* `items` text: two spaces of indent per level, '|' between columns */
static void ser_rec(tree_data *t, int i, int depth, char **buf, size_t *len, size_t *cap) {
  t_item *e = IT(t, i);
  if (!e) return;
  char line[1024];
  int k = snprintf(line, sizeof line, "%*s", depth * 2, "");
  for (int c = 0; c < CW_CLAMP(t->columns, 1, TREE_MAX_COLS); c++)
    k += snprintf(line + k, sizeof line - (size_t)k, c ? "|%s" : "%s", e->text[c] ? e->text[c] : "");
  if (*len + (size_t)k + 2 > *cap) {
    *cap = (*cap + (size_t)k + 2) * 2;
    char *nb = cw_frame_alloc(*cap);
    memcpy(nb, *buf, *len);
    *buf = nb;
  }
  if (*len) (*buf)[(*len)++] = '\n';
  memcpy(*buf + *len, line, (size_t)k);
  *len += (size_t)k;
  (*buf)[*len] = 0;
  for (int c = 0; c < e->children.count; c++) ser_rec(t, e->children.data[c], depth + 1, buf, len, cap);
}
static void get_items(cw_node n, void *d, cw_value *o) {
  (void)n;
  tree_data *t = d;
  size_t cap = 256, len = 0;
  char *buf = cw_frame_alloc(cap);
  buf[0] = 0;
  if (t->root >= 0) ser_rec(t, t->root, 0, &buf, &len, &cap);
  *o = cw_val_string(buf);
}
static void set_items(cw_node n, void *d, const cw_value *v) {
  tree_data *t = d;
  cw_uitree_clear(n);
  int stack[64];
  const char *s = v->s ? v->s : "";
  while (*s) {
    const char *e = strchr(s, '\n');
    int len = e ? (int)(e - s) : (int)strlen(s);
    int ind = 0;
    while (ind < len && s[ind] == ' ') ind++;
    int depth = CW_MIN(ind / 2, 63);
    if (len > ind) {
      int parent = depth > 0 ? stack[depth - 1] : -1;
      if (depth == 0 && t->root >= 0) parent = t->root; /* extra top-level lines go under the root */
      const char *p = s + ind;
      int plen = len - ind;
      const char *bar = memchr(p, '|', (size_t)plen);
      int id = cw_uitree_create_item(n, parent, cw_frame_printf("%.*s", bar ? (int)(bar - p) : plen, p));
      for (int c = 1; bar && c < TREE_MAX_COLS; c++) {
        const char *q = bar + 1;
        const char *nb = memchr(q, '|', (size_t)(s + len - q));
        cw_uitree_set_text(n, id, c, cw_frame_printf("%.*s", nb ? (int)(nb - q) : (int)(s + len - q), q));
        bar = nb;
      }
      stack[depth] = id;
    }
    if (!e) break;
    s = e + 1;
  }
}

static void tree_init(cw_node n, void *d) {
  tree_data *t = d;
  t->columns = 1;
  t->root = t->selected = t->hover = t->press_item = t->anchor = -1;
  t->cols[0].expand = true;
  t->rows_dirty = true;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
}
static void tree_destroy(cw_node n, void *d) {
  tree_data *t = d;
  cw_uitree_clear(n);
  cw_array_free(&t->items);
  cw_array_free(&t->rows);
  cw_array_free(&t->row_depth);
}
static void tree_changed(cw_node n, void *d, const cw_prop *p) {
  tree_data *t = d;
  (void)p;
  t->columns = CW_CLAMP(t->columns, 1, TREE_MAX_COLS);
  for (int i = 0; i < t->columns; i++)
    if (!t->cols[i].expand && t->cols[i].min_width == 0) t->cols[i].expand = true;
  t->rows_dirty = true;
  ui_mark_min_dirty(n);
}
static const cw_prop tree_props[] = {
  CW_PROP_FIELD_RANGE(tree_data, columns, CW_T_INT, 1, TREE_MAX_COLS, "Column count."),
  CW_PROP_FIELD(tree_data, hide_root, CW_T_BOOL, CW_PROP_DEFAULT, "Do not show the root item."),
  CW_PROP_FIELD(tree_data, column_titles_visible, CW_T_BOOL, CW_PROP_DEFAULT, "Show the title row even without titles."),
  CW_PROP_FIELD_RANGE(tree_data, select_mode, CW_T_INT, 0, 1, "0 single selection, 1 multiple (ctrl+click toggles, shift+click extends; multi_selected reports each change)."),
  CW_PROP_FIELD(tree_data, drag_items, CW_T_BOOL, CW_PROP_DEFAULT, "Dragging a row starts a UI drag of kind \"tree_item\" (payload = the item's metadata, text = its first column)."),
  CW_PROP_ACCESSOR("items", CW_T_STRING, CW_PROP_DEFAULT, get_items, set_items, "Indented item text: two spaces per level, '|' between columns."),
  CW_PROP_END
};
static const char *const tree_signals[] = { "item_selected", "item_activated", "item_collapsed", "multi_selected", NULL };
static cw_node_type tree_type = { .name = "Tree", .parent = "Control", .data_size = sizeof(tree_data), .props = tree_props,
                                  .signals = tree_signals, .init = tree_init, .destroy = tree_destroy,
                                  .prop_changed = tree_changed, .doc = "Hierarchical rows with columns." };

void ui_register_tree(void) {
  cw_node_type_register(&tree_type);
  tree_t = &tree_type;
  cw_ui_class_register("Tree", &(cw_ui_class){ .min_size = tree_min, .draw = tree_draw, .input = tree_input });
}
cw_rect cw_uitree_item_rect(cw_node n, int i) {
  tree_data *t = TD(n);
  if (!t || !IT(t, i)) return (cw_rect){ 0 };
  rows(t);
  cw_rect b = body(n, t), g = cw_control_global_rect(n);
  float pt = pitch(n), rh = row_h(n);
  for (int r = 0; r < t->rows.count; r++) {
    if (t->rows.data[r] != i) continue;
    float y = b.y + (float)r * pt - t->scroll_y;
    if (y + rh <= b.y || y >= b.y + b.h) return (cw_rect){ 0 }; /* scrolled out of view */
    return (cw_rect){ g.x + b.x, g.y + y, b.w, rh };
  }
  return (cw_rect){ 0 };
}

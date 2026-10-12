/* ItemList: a scrolling list of items with optional icons, single or
 * multiple selection. */
#include "ui_internal.h"

typedef struct { char *text; cw_texture icon; bool selected; } li_item;

typedef struct {
  int32_t select_mode; /* 0 single, 1 multi */
  CW_ARRAY(li_item) items;
  int hover, cursor, shift_anchor;
  float scroll;
  ui_scrollbar vb;
} il_data;
static const cw_node_type *il_t;

static float row_h(cw_node n) {
  cw_ui_style it = T_STYLE(n, "item");
  return ceilf(cw_ui_line_height(T_FONT(n), T_FSIZE(n)) + SM_H(&it));
}
static cw_rect inner(cw_node n) {
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel");
  return style_content(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
}
static float pitch(cw_node n) { return row_h(n) + T_CONST(n, "v_separation"); }

static void bars(cw_node n, il_data *l) {
  cw_rect in = inner(n);
  float th = ui_scrollbar_thickness(n);
  l->vb.vertical = true;
  l->vb.page = in.h;
  l->vb.max = (float)l->items.count * pitch(n);
  l->vb.rect = (cw_rect){ in.x + in.w - th, in.y, th, in.h };
  l->scroll = ui_scrollbar_clamp(&l->vb, l->scroll);
  l->vb.value = l->scroll;
}

static cw_vec2 il_min(cw_node n, void *d) {
  il_data *l = d;
  cw_ui_style p = T_STYLE(n, "panel"), it = T_STYLE(n, "item");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), w = 0, lh = cw_ui_line_height(f, fs);
  for (int i = 0; i < l->items.count; i++) {
    float iw = cw_ui_text_width(f, fs, ui_tr(n, l->items.data[i].text));
    if (!CW_HANDLE_IS_NULL(l->items.data[i].icon)) iw += lh + T_CONST(n, "h_separation");
    w = CW_MAX(w, iw);
  }
  return (cw_vec2){ ceilf(w + SM_W(&it) + SM_W(&p) + ui_scrollbar_thickness(n)), ceilf(row_h(n) + SM_H(&p)) };
}

static void il_draw(cw_node n, void *d) {
  il_data *l = d;
  cw_control *c = CTL(n);
  cw_ui_style p = T_STYLE(n, "panel"), it = T_STYLE(n, "item"), sel = T_STYLE(n, "selected"), hov = T_STYLE(n, "hovered");
  cw_ui_draw_style(&p, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
  bars(n, l);
  cw_rect in = inner(n);
  float rw = in.w - (ui_scrollbar_needed(&l->vb) ? l->vb.rect.w : 0);
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), rh = row_h(n), pt = pitch(n), lh = cw_ui_line_height(f, fs);
  cw_color fc = T_COLOR(n, "font_color"), fsc = T_COLOR(n, "font_selected_color");
  cw_ui_push_clip(in);
  int first = (int)(l->scroll / pt), last = (int)((l->scroll + in.h) / pt) + 1;
  for (int i = CW_MAX(first, 0); i < l->items.count && i <= last; i++) {
    li_item *e = &l->items.data[i];
    cw_rect r = { in.x, in.y + (float)i * pt - l->scroll, rw, rh };
    if (e->selected) cw_ui_draw_style(&sel, r);
    else if (i == l->hover && (c->_flags & CF_HOVERED)) cw_ui_draw_style(&hov, r);
    cw_rect tr = style_content(&it, r);
    if (!CW_HANDLE_IS_NULL(e->icon)) {
      cw_ui_draw_texture(e->icon, (cw_rect){ tr.x, tr.y + (tr.h - lh) * 0.5f, lh, lh }, (cw_rect){ 0 }, CW_WHITE);
      float a = lh + T_CONST(n, "h_separation");
      tr.x += a, tr.w -= a;
    }
    cw_ui_draw_text_in(f, fs, ui_tr(n, e->text), tr, CW_HALIGN_LEFT, e->selected ? fsc : fc, true);
  }
  cw_ui_pop_clip();
  if (ui_scrollbar_needed(&l->vb)) ui_scrollbar_draw(n, &l->vb);
}

static int index_at(cw_node n, il_data *l, cw_vec2 p) {
  cw_rect in = inner(n);
  if (!rect_has(in, p)) return -1;
  float pt = pitch(n);
  int i = (int)floorf((p.y - in.y + l->scroll) / pt);
  if (i < 0 || i >= l->items.count) return -1;
  if (p.y - in.y + l->scroll - (float)i * pt > row_h(n)) return -1;
  return i;
}

void cw_item_list_ensure_visible(cw_node n, int i) {
  il_data *l = cw_node_data_t(n, il_t);
  if (!l || i < 0 || i >= l->items.count) return;
  cw_rect in = inner(n);
  float pt = pitch(n), y = (float)i * pt;
  if (y < l->scroll) l->scroll = y;
  if (y + row_h(n) > l->scroll + in.h) l->scroll = y + row_h(n) - in.h;
  bars(n, l);
}

static void select_one(cw_node n, il_data *l, int i, bool single) {
  if (single)
    for (int k = 0; k < l->items.count; k++) l->items.data[k].selected = false;
  l->items.data[i].selected = true;
  l->cursor = i;
}

static bool il_input(cw_node n, void *d, const cw_ui_event *e) {
  il_data *l = d;
  bool ch;
  if (ui_scrollbar_input(n, &l->vb, e, &ch)) {
    if (ch) l->scroll = l->vb.value;
    return true;
  }
  switch (e->type) {
  case CW_UI_MOUSE_MOVE: l->hover = index_at(n, l, e->pos); return false;
  case CW_UI_WHEEL: {
    float before = l->scroll;
    l->scroll -= e->wheel.y * pitch(n) * 3;
    bars(n, l);
    return l->scroll != before;
  }
  case CW_UI_MOUSE_DOWN: {
    if (e->button != 0) return true;
    int i = index_at(n, l, e->pos);
    if (i < 0) return true;
    if (l->select_mode == 1 && e->ctrl) {
      l->items.data[i].selected = !l->items.data[i].selected;
      l->cursor = i;
      ui_emit_int(n, "item_selected", i);
    } else if (l->select_mode == 1 && e->shift && l->shift_anchor >= 0) {
      int a = CW_MIN(l->shift_anchor, i), b = CW_MAX(l->shift_anchor, i);
      for (int k = 0; k < l->items.count; k++) l->items.data[k].selected = k >= a && k <= b;
      l->cursor = i;
      ui_emit_int(n, "item_selected", i);
    } else {
      select_one(n, l, i, true);
      l->shift_anchor = i;
      ui_emit_int(n, "item_selected", i);
    }
    if (e->clicks == 2) ui_emit_int(n, "item_activated", i);
    return true;
  }
  case CW_UI_NAV: {
    if (!l->items.count) return false;
    int c = l->cursor;
    if (e->nav == CW_NAV_UP) c = c <= 0 ? 0 : c - 1;
    else if (e->nav == CW_NAV_DOWN) c = c < 0 ? 0 : CW_MIN(c + 1, l->items.count - 1);
    else if (e->nav == CW_NAV_ACCEPT) {
      if (l->cursor >= 0) ui_emit_int(n, "item_activated", l->cursor);
      return true;
    } else return false;
    if (c == l->cursor && !e->repeat) return false; /* at an end: navigate out */
    select_one(n, l, c, true);
    l->shift_anchor = c;
    cw_item_list_ensure_visible(n, c);
    ui_emit_int(n, "item_selected", c);
    return true;
  }
  default: return false;
  }
}

int cw_item_list_add_item(cw_node n, const char *text, cw_texture icon) {
  il_data *l = cw_node_data_t(n, il_t);
  if (!l) return -1;
  li_item it = { cw_strdup(text ? text : "", CW_MEM_UI), icon, false };
  cw_array_push(&l->items, it);
  ui_mark_min_dirty(n);
  return l->items.count - 1;
}
void cw_item_list_clear(cw_node n) {
  il_data *l = cw_node_data_t(n, il_t);
  if (!l) return;
  for (int i = 0; i < l->items.count; i++) cw_free(l->items.data[i].text);
  cw_array_clear(&l->items);
  l->cursor = l->hover = l->shift_anchor = -1;
  l->scroll = 0;
  ui_mark_min_dirty(n);
}
int cw_item_list_count(cw_node n) {
  il_data *l = cw_node_data_t(n, il_t);
  return l ? l->items.count : 0;
}
const char *cw_item_list_text(cw_node n, int i) {
  il_data *l = cw_node_data_t(n, il_t);
  return l && i >= 0 && i < l->items.count ? l->items.data[i].text : "";
}
void cw_item_list_select(cw_node n, int i, bool single) {
  il_data *l = cw_node_data_t(n, il_t);
  if (l && i >= 0 && i < l->items.count) select_one(n, l, i, single || l->select_mode == 0);
}
void cw_item_list_deselect_all(cw_node n) {
  il_data *l = cw_node_data_t(n, il_t);
  if (!l) return;
  for (int i = 0; i < l->items.count; i++) l->items.data[i].selected = false;
}
bool cw_item_list_is_selected(cw_node n, int i) {
  il_data *l = cw_node_data_t(n, il_t);
  return l && i >= 0 && i < l->items.count && l->items.data[i].selected;
}
int cw_item_list_selected(cw_node n) {
  il_data *l = cw_node_data_t(n, il_t);
  if (!l) return -1;
  for (int i = 0; i < l->items.count; i++)
    if (l->items.data[i].selected) return i;
  return -1;
}

static void get_items(cw_node n, void *d, cw_value *o) {
  (void)n;
  il_data *l = d;
  size_t sz = 1;
  for (int i = 0; i < l->items.count; i++) sz += strlen(l->items.data[i].text) + 1;
  char *s = cw_frame_alloc(sz), *p = s;
  *p = 0;
  for (int i = 0; i < l->items.count; i++) p += sprintf(p, i ? "\n%s" : "%s", l->items.data[i].text);
  *o = cw_val_string(s);
}
static void set_items(cw_node n, void *d, const cw_value *v) {
  (void)d;
  cw_item_list_clear(n);
  const char *s = v->s ? v->s : "";
  while (*s) {
    const char *e = strchr(s, '\n');
    cw_item_list_add_item(n, cw_frame_printf("%.*s", e ? (int)(e - s) : (int)strlen(s), s), (cw_texture){ 0 });
    if (!e) break;
    s = e + 1;
  }
}

static void il_init(cw_node n, void *d) {
  il_data *l = d;
  l->hover = l->cursor = l->shift_anchor = -1;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
  CTL(n)->clip_contents = false;
}
static void il_destroy(cw_node n, void *d) {
  (void)n;
  il_data *l = d;
  for (int i = 0; i < l->items.count; i++) cw_free(l->items.data[i].text);
  cw_array_free(&l->items);
}
static const char *const sel_names[] = { "SINGLE", "MULTI", NULL };
static const cw_prop il_props[] = {
  CW_PROP_ACCESSOR("items", CW_T_STRING, CW_PROP_DEFAULT, get_items, set_items, "Item texts, one per line."),
  { .name = "select_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(il_data, select_mode),
    .size = 4, .enum_names = sel_names, .doc = "SINGLE or MULTI (ctrl/shift click)." },
  CW_PROP_END
};
static const char *const il_signals[] = { "item_selected", "item_activated", NULL };
static cw_node_type il_type = { .name = "ItemList", .parent = "Control", .data_size = sizeof(il_data), .props = il_props,
                                .signals = il_signals, .init = il_init, .destroy = il_destroy,
                                .doc = "A scrolling list of selectable items." };

void ui_register_lists(void) {
  cw_node_type_register(&il_type);
  il_t = &il_type;
  cw_ui_class_register("ItemList", &(cw_ui_class){ .min_size = il_min, .draw = il_draw, .input = il_input });
}

/* Range and its widgets: HSlider, VSlider, SpinBox, ProgressBar. */
#include "ui_internal.h"

typedef struct {
  double min_value, max_value, step, value, page;
} range_data;
static const cw_node_type *range_t;

static double snap_value(range_data *r, double v) {
  if (r->step > 0) v = r->min_value + floor((v - r->min_value) / r->step + 0.5) * r->step;
  double hi = r->max_value - r->page;
  if (v > hi) v = hi;
  if (v < r->min_value) v = r->min_value;
  return v;
}

static bool set_value(cw_node n, range_data *r, double v) {
  v = snap_value(r, v);
  if (v == r->value) return false;
  r->value = v;
  ui_emit_float(n, "value_changed", v);
  return true;
}

void cw_range_set_value(cw_node n, double v) {
  range_data *r = cw_node_data_t(n, range_t);
  if (r) set_value(n, r, v);
}
double cw_range_value(cw_node n) {
  range_data *r = cw_node_data_t(n, range_t);
  return r ? r->value : 0;
}
void cw_range_set_limits(cw_node n, double mn, double mx, double step) {
  range_data *r = cw_node_data_t(n, range_t);
  if (!r) return;
  r->min_value = mn, r->max_value = mx, r->step = step;
  set_value(n, r, r->value);
  ui_mark_min_dirty(n);
}

static double ratio(range_data *r) {
  double span = r->max_value - r->page - r->min_value;
  return span > 0 ? (r->value - r->min_value) / span : 0;
}
static double nudge(range_data *r) { return r->step > 0 ? r->step : (r->max_value - r->min_value) / 20; }

static void range_init(cw_node n, void *d) {
  range_data *r = d;
  r->max_value = 100;
  r->step = 1;
  (void)n;
}
static void range_changed(cw_node n, void *d, const cw_prop *p) {
  range_data *r = d;
  if (!strcmp(p->name, "value")) {
    double v = r->value;
    r->value = snap_value(r, v);
    ui_emit_float(n, "value_changed", r->value);
  } else r->value = snap_value(r, r->value);
  ui_mark_min_dirty(n);
}

/* ── sliders ────────────────────────────────────────────────────────── */

typedef struct { bool vertical, dragging; } slider_data;
static const cw_node_type *slider_t;

static cw_vec2 slider_min(cw_node n, void *d) {
  slider_data *s = d;
  float gr = T_CONST(n, "grabber_radius");
  cw_ui_style tr = T_STYLE(n, "slider");
  float cross = CW_MAX(gr * 2, SM_H(&tr)), main = gr * 4;
  return s->vertical ? (cw_vec2){ cross, main } : (cw_vec2){ main, cross };
}

static void slider_geom(cw_node n, slider_data *s, float *a, float *len, float *cross) {
  cw_control *c = CTL(n);
  float gr = T_CONST(n, "grabber_radius");
  *a = gr;
  *len = (s->vertical ? c->rect.h : c->rect.w) - gr * 2;
  *cross = (s->vertical ? c->rect.w : c->rect.h) * 0.5f;
}

static void slider_draw(cw_node n, void *d) {
  slider_data *s = d;
  range_data *r = cw_node_data_t(n, range_t);
  float a, len, cr;
  slider_geom(n, s, &a, &len, &cr);
  cw_ui_style tr = T_STYLE(n, "slider"), fill = T_STYLE(n, "grabber_area");
  float th = SM_H(&tr);
  float t = (float)ratio(r);
  float gr = T_CONST(n, "grabber_radius");
  if (!s->vertical) {
    cw_ui_draw_style(&tr, (cw_rect){ a - th * 0.5f, cr - th * 0.5f, len + th, th });
    cw_ui_draw_style(&fill, (cw_rect){ a - th * 0.5f, cr - th * 0.5f, len * t + th, th });
    cw_vec2 g = { a + len * t, cr };
    cw_ui_draw_circle(g, gr, T_COLOR(n, "grabber_border_color"));
    cw_ui_draw_circle(g, gr - 2, T_COLOR(n, "grabber_color"));
  } else {
    float y = a + len * (1 - t);
    cw_ui_draw_style(&tr, (cw_rect){ cr - th * 0.5f, a - th * 0.5f, th, len + th });
    cw_ui_draw_style(&fill, (cw_rect){ cr - th * 0.5f, y - th * 0.5f, th, a + len - y + th });
    cw_vec2 g = { cr, y };
    cw_ui_draw_circle(g, gr, T_COLOR(n, "grabber_border_color"));
    cw_ui_draw_circle(g, gr - 2, T_COLOR(n, "grabber_color"));
  }
}

static void slider_set_from(cw_node n, slider_data *s, range_data *r, cw_vec2 p) {
  float a, len, cr;
  slider_geom(n, s, &a, &len, &cr);
  double t = len > 0 ? ((s->vertical ? (a + len - p.y) : (p.x - a)) / len) : 0;
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  set_value(n, r, r->min_value + t * (r->max_value - r->page - r->min_value));
}

static bool slider_input(cw_node n, void *d, const cw_ui_event *e) {
  slider_data *s = d;
  range_data *r = cw_node_data_t(n, range_t);
  switch (e->type) {
  case CW_UI_MOUSE_DOWN:
    if (e->button != 0) return false;
    s->dragging = true;
    slider_set_from(n, s, r, e->pos);
    return true;
  case CW_UI_MOUSE_MOVE:
    if (!s->dragging) return false;
    slider_set_from(n, s, r, e->pos);
    return true;
  case CW_UI_MOUSE_UP:
    if (!s->dragging) return false;
    s->dragging = false;
    return true;
  case CW_UI_WHEEL:
    set_value(n, r, r->value + (e->wheel.y > 0 ? nudge(r) : -nudge(r)));
    return true;
  case CW_UI_NAV: {
    int inc = s->vertical ? CW_NAV_UP : CW_NAV_RIGHT, dec = s->vertical ? CW_NAV_DOWN : CW_NAV_LEFT;
    if (e->nav == inc) { set_value(n, r, r->value + nudge(r)); return true; }
    if (e->nav == dec) { set_value(n, r, r->value - nudge(r)); return true; }
    return false;
  }
  default: return false;
  }
}

static void slider_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
}
static void hslider_init(cw_node n, void *d) {
  (void)d;
  ((slider_data *)cw_node_data_t(n, slider_t))->vertical = false;
  CTL(n)->size_flags_vertical = CW_SIZE_SHRINK_CENTER;
}
static void vslider_init(cw_node n, void *d) {
  (void)d;
  ((slider_data *)cw_node_data_t(n, slider_t))->vertical = true;
  CTL(n)->size_flags_horizontal = CW_SIZE_SHRINK_CENTER;
}

/* ── SpinBox ────────────────────────────────────────────────────────── */

typedef struct {
  const char *prefix, *suffix;
  bool editing;
  ui_str edit;
  int held; /* arrow being held: +1 / -1 */
} spin_data;
static const cw_node_type *spin_t;

static const char *fmt_value(range_data *r) {
  int dec = 0;
  if (r->step > 0 && r->step < 1)
    for (double s = r->step; s < 0.9999 && dec < 6; s *= 10) dec++;
  if (r->step <= 0) dec = 2;
  return cw_frame_printf("%.*f", dec, r->value);
}

static const char *spin_text(spin_data *s, range_data *r) {
  if (s->editing) return ui_str_c(&s->edit);
  return cw_frame_printf("%s%s%s", ui_s(s->prefix), fmt_value(r), ui_s(s->suffix));
}

static cw_vec2 spin_min(cw_node n, void *d) {
  spin_data *s = d;
  range_data *r = cw_node_data_t(n, range_t);
  cw_ui_style st = cw_control_theme_style(n, "LineEdit", "normal");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  /* wide enough for the extremes */
  double v0 = r->value;
  r->value = r->max_value;
  float w = cw_ui_text_width(f, fs, spin_text(s, r));
  r->value = r->min_value;
  w = CW_MAX(w, cw_ui_text_width(f, fs, spin_text(s, r)));
  r->value = v0;
  return (cw_vec2){ ceilf(w + SM_W(&st) + T_CONST(n, "buttons_width")), ceilf(cw_ui_line_height(f, fs) + SM_H(&st)) };
}

static void spin_draw(cw_node n, void *d) {
  spin_data *s = d;
  range_data *r = cw_node_data_t(n, range_t);
  cw_control *c = CTL(n);
  cw_rect all = { 0, 0, c->rect.w, c->rect.h };
  cw_ui_style st = cw_control_theme_style(n, "LineEdit", "normal");
  cw_ui_draw_style(&st, all);
  float bw = T_CONST(n, "buttons_width");
  cw_rect in = style_content(&st, all);
  in.w -= bw;
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  cw_color fc = cw_control_theme_color(n, "LineEdit", "font_color");
  cw_ui_draw_text_in(f, fs, spin_text(s, r), in, CW_HALIGN_LEFT, fc, true);
  if (s->editing && cw_control_has_focus(n)) {
    float x = in.x + cw_ui_text_width(f, fs, ui_str_c(&s->edit));
    float lh = cw_ui_line_height(f, fs);
    cw_ui_draw_rect((cw_rect){ x, in.y + (in.h - lh) * 0.5f, 1, lh }, cw_control_theme_color(n, "LineEdit", "caret_color"));
  }
  float h2 = c->rect.h * 0.5f;
  cw_color ac = T_COLOR(n, "arrow_color");
  float as = CW_MIN(bw, h2) * 0.8f;
  float ax = c->rect.w - bw + (bw - as) * 0.5f - 2;
  cw_ui_draw_icon(CW_ICON_ARROW_UP, (cw_rect){ ax, (h2 - as) * 0.5f + 2, as, as }, ac);
  cw_ui_draw_icon(CW_ICON_ARROW_DOWN, (cw_rect){ ax, h2 + (h2 - as) * 0.5f - 2, as, as }, ac);
  if (cw_control_has_focus(n)) {
    cw_ui_style fo = cw_control_theme_style(n, "LineEdit", "focus");
    cw_ui_draw_style(&fo, all);
  }
}

static void spin_commit(cw_node n, spin_data *s, range_data *r) {
  if (!s->editing) return;
  s->editing = false;
  const char *t = ui_str_c(&s->edit);
  const char *p = ui_s(s->prefix);
  if (*p && !strncmp(t, p, strlen(p))) t += strlen(p);
  char *end;
  double v = strtod(t, &end);
  if (end != t) set_value(n, r, v);
  ui_mark_min_dirty(n);
}

static bool spin_input(cw_node n, void *d, const cw_ui_event *e) {
  spin_data *s = d;
  range_data *r = cw_node_data_t(n, range_t);
  cw_control *c = CTL(n);
  float bw = T_CONST(n, "buttons_width");
  switch (e->type) {
  case CW_UI_MOUSE_DOWN:
    if (e->button != 0) return false;
    if (e->pos.x >= c->rect.w - bw) {
      spin_commit(n, s, r);
      set_value(n, r, r->value + (e->pos.y < c->rect.h * 0.5f ? nudge(r) : -nudge(r)));
    }
    return true;
  case CW_UI_WHEEL:
    if (!cw_control_has_focus(n)) return false;
    spin_commit(n, s, r);
    set_value(n, r, r->value + (e->wheel.y > 0 ? nudge(r) : -nudge(r)));
    return true;
  case CW_UI_TEXT: {
    if (!s->editing) {
      s->editing = true;
      ui_str_set(&s->edit, "", 0);
    }
    for (const char *p = e->text; *p; p++)
      if ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+' || *p == 'e') ui_str_insert(&s->edit, s->edit.len, p, 1);
    return true;
  }
  case CW_UI_KEY:
    if (e->key == 42 /* backspace */) {
      if (!s->editing) {
        s->editing = true;
        ui_str_set(&s->edit, fmt_value(r), -1);
      }
      if (s->edit.len) ui_str_erase(&s->edit, ui_utf8_prev(s->edit.s, s->edit.len), s->edit.len - ui_utf8_prev(s->edit.s, s->edit.len));
      return true;
    }
    if (e->key == 40 || e->key == 88) { /* enter */
      if (!s->editing) return false;
      spin_commit(n, s, r);
      return true;
    }
    if (e->key == 41 && s->editing) { /* escape */
      s->editing = false;
      return true;
    }
    return false;
  case CW_UI_NAV:
    if (e->nav == CW_NAV_UP) { spin_commit(n, s, r); set_value(n, r, r->value + nudge(r)); return true; }
    if (e->nav == CW_NAV_DOWN) { spin_commit(n, s, r); set_value(n, r, r->value - nudge(r)); return true; }
    return false;
  default: return false;
  }
}

static void spin_focus(cw_node n, void *d, bool focused) {
  spin_data *s = d;
  if (!focused) spin_commit(n, s, cw_node_data_t(n, range_t));
}
static void spin_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
}
static void spin_destroy(cw_node n, void *d) {
  (void)n;
  ui_str_free(&((spin_data *)d)->edit);
}
static void any_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}

/* ── ProgressBar ────────────────────────────────────────────────────── */

typedef struct { bool show_percentage; } progress_data;

static cw_vec2 progress_min(cw_node n, void *d) {
  progress_data *p = d;
  cw_ui_style bg = T_STYLE(n, "background");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  float h = p->show_percentage ? cw_ui_line_height(f, fs) : 0;
  float w = p->show_percentage ? cw_ui_text_width(f, fs, "100%") : 0;
  return (cw_vec2){ ceilf(w + SM_W(&bg)), ceilf(h + SM_H(&bg)) };
}
static void progress_draw(cw_node n, void *d) {
  progress_data *p = d;
  range_data *r = cw_node_data_t(n, range_t);
  cw_control *c = CTL(n);
  cw_rect all = { 0, 0, c->rect.w, c->rect.h };
  cw_ui_style bg = T_STYLE(n, "background"), fill = T_STYLE(n, "fill");
  cw_ui_draw_style(&bg, all);
  float t = (float)ratio(r);
  if (t > 0) {
    float minw = fill.radius[0] + fill.radius[1];
    cw_ui_draw_style(&fill, (cw_rect){ 0, 0, CW_MAX(c->rect.w * t, CW_MIN(minw, c->rect.w)), c->rect.h });
  }
  if (p->show_percentage) {
    char b[16];
    snprintf(b, sizeof b, "%d%%", (int)floor(t * 100 + 0.5));
    cw_ui_draw_text_in(T_FONT(n), T_FSIZE(n), b, all, CW_HALIGN_CENTER, T_COLOR(n, "font_color"), false);
  }
}
static void progress_init(cw_node n, void *d) {
  ((progress_data *)d)->show_percentage = true;
  CTL(n)->mouse_filter = CW_MOUSE_IGNORE;
  range_data *r = cw_node_data_t(n, range_t);
  r->step = 0.01;
}

static const cw_prop range_props[] = {
  CW_PROP_FIELD(range_data, min_value, CW_T_FLOAT, CW_PROP_DEFAULT, "Lowest value."),
  CW_PROP_FIELD(range_data, max_value, CW_T_FLOAT, CW_PROP_DEFAULT, "Highest value (value + page <= max)."),
  CW_PROP_FIELD(range_data, step, CW_T_FLOAT, CW_PROP_DEFAULT, "Values snap to min + k*step (0 = continuous)."),
  CW_PROP_FIELD(range_data, page, CW_T_FLOAT, CW_PROP_DEFAULT, "Page size (scrollbars)."),
  CW_PROP_FIELD(range_data, value, CW_T_FLOAT, CW_PROP_DEFAULT, "Current value."),
  CW_PROP_END
};
static const char *const range_signals[] = { "value_changed", NULL };
static cw_node_type range_type = { .name = "Range", .parent = "Control", .data_size = sizeof(range_data),
                                   .props = range_props, .signals = range_signals, .init = range_init,
                                   .prop_changed = range_changed, .doc = "A value in [min, max] with a step." };
static cw_node_type slider_type = { .name = "Slider", .parent = "Range", .data_size = sizeof(slider_data),
                                    .init = slider_init, .doc = "Base of HSlider/VSlider." };
static cw_node_type hslider_type = { .name = "HSlider", .parent = "Slider", .init = hslider_init, .doc = "Horizontal slider." };
static cw_node_type vslider_type = { .name = "VSlider", .parent = "Slider", .init = vslider_init, .doc = "Vertical slider." };
static const cw_prop spin_props[] = {
  CW_PROP_FIELD(spin_data, prefix, CW_T_STRING, CW_PROP_DEFAULT, "Text before the number."),
  CW_PROP_FIELD(spin_data, suffix, CW_T_STRING, CW_PROP_DEFAULT, "Text after the number."),
  CW_PROP_END
};
static cw_node_type spin_type = { .name = "SpinBox", .parent = "Range", .data_size = sizeof(spin_data), .props = spin_props,
                                  .init = spin_init, .destroy = spin_destroy, .prop_changed = any_changed,
                                  .doc = "A number field with step arrows; type to edit." };
static const cw_prop progress_props[] = {
  CW_PROP_FIELD(progress_data, show_percentage, CW_T_BOOL, CW_PROP_DEFAULT, "Draw the percentage."),
  CW_PROP_END
};
static cw_node_type progress_type = { .name = "ProgressBar", .parent = "Range", .data_size = sizeof(progress_data),
                                      .props = progress_props, .init = progress_init, .prop_changed = any_changed,
                                      .doc = "Shows how far value is between min and max." };

void ui_register_ranges(void) {
  cw_node_type_register(&range_type);
  range_t = &range_type;
  cw_ui_class_register("Range", &(cw_ui_class){ .theme_type = "Range" });
  cw_node_type_register(&slider_type);
  slider_t = &slider_type;
  cw_node_type_register(&hslider_type);
  cw_node_type_register(&vslider_type);
  cw_ui_class_register("Slider", &(cw_ui_class){ .min_size = slider_min, .draw = slider_draw, .input = slider_input });
  cw_node_type_register(&spin_type);
  spin_t = &spin_type;
  cw_ui_class_register("SpinBox", &(cw_ui_class){ .min_size = spin_min, .draw = spin_draw, .input = spin_input,
                                                  .focus_changed = spin_focus, .wants_text = true });
  cw_node_type_register(&progress_type);
  cw_ui_class_register("ProgressBar", &(cw_ui_class){ .min_size = progress_min, .draw = progress_draw });
}

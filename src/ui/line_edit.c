/* LineEdit: a single-line text field with caret, selection, clipboard,
 * undo, placeholder, max length and secret mode. */
#include "ui_internal.h"

enum { K_BACKSPACE = 42, K_DELETE = 76, K_HOME = 74, K_END = 77, K_LEFT = 80, K_RIGHT = 79, K_UP = 82, K_DOWN = 81,
       K_RETURN = 40, K_KP_ENTER = 88, K_A = 4, K_C = 6, K_V = 25, K_X = 27, K_Z = 29, K_Y = 28 };

/* byte offset in s[0..len) whose caret position is nearest to x */
int ui_byte_at_x(cw_font f, float size, const char *s, int len, float x) {
  if (x <= 0 || len == 0) return 0;
  char *buf = cw_frame_alloc((size_t)len + 1);
  memcpy(buf, s, (size_t)len);
  buf[len] = 0;
  float prev = 0;
  int pi = 0;
  for (int i = ui_utf8_next(buf, len, 0);; i = ui_utf8_next(buf, len, i)) {
    float cx = cw_font_caret_x(f, buf, i, size);
    if (cx >= x) return (x - prev < cx - x) ? pi : i;
    if (i >= len) return len;
    prev = cx;
    pi = i;
  }
}

int ui_word_left(const char *s, int i) {
  while (i > 0) {
    int p = ui_utf8_prev(s, i), q = p;
    if (ui_is_word_char(ui_utf8_decode(s, i, &q))) break;
    i = p;
  }
  while (i > 0) {
    int p = ui_utf8_prev(s, i), q = p;
    if (!ui_is_word_char(ui_utf8_decode(s, i, &q))) break;
    i = p;
  }
  return i;
}
int ui_word_right(const char *s, int len, int i) {
  while (i < len) {
    int q = i;
    if (ui_is_word_char(ui_utf8_decode(s, len, &q))) break;
    i = q;
  }
  while (i < len) {
    int q = i;
    if (!ui_is_word_char(ui_utf8_decode(s, len, &q))) break;
    i = q;
  }
  return i;
}

typedef struct { char *text; int caret; } undo_rec;

typedef struct {
  const char *placeholder_text;
  int32_t max_length;
  bool secret, editable;
  const char *secret_character;
  /* state */
  ui_str text;
  int caret, anchor;
  float scroll_x;
  bool dragging;
  double blink_t0;
  CW_ARRAY(undo_rec) undo;
} le_data;
static const cw_node_type *le_t;

static int sel_from(le_data *l) { return CW_MIN(l->caret, l->anchor); }
static int sel_to(le_data *l) { return CW_MAX(l->caret, l->anchor); }
static bool has_sel(le_data *l) { return l->caret != l->anchor; }

static const char *secret_ch(le_data *l) { return l->secret_character && *l->secret_character ? l->secret_character : "\xE2\x80\xA2"; }

/* the string drawn, and the drawn byte for a text byte */
static const char *display(le_data *l, int *dlen) {
  if (!l->secret) {
    *dlen = l->text.len;
    return ui_str_c(&l->text);
  }
  int n = ui_utf8_count(ui_str_c(&l->text), l->text.len);
  int cl = (int)strlen(secret_ch(l));
  char *d = cw_frame_alloc((size_t)(n * cl + 1));
  for (int i = 0; i < n; i++) memcpy(d + i * cl, secret_ch(l), (size_t)cl);
  d[n * cl] = 0;
  *dlen = n * cl;
  return d;
}
static int to_display(le_data *l, int byte) {
  if (!l->secret) return byte;
  return ui_utf8_count(ui_str_c(&l->text), byte) * (int)strlen(secret_ch(l));
}
static int from_display(le_data *l, int dbyte) {
  if (!l->secret) return dbyte;
  int idx = dbyte / (int)strlen(secret_ch(l)), b = 0;
  for (int i = 0; i < idx && b < l->text.len; i++) b = ui_utf8_next(l->text.s, l->text.len, b);
  return b;
}

static void push_undo(le_data *l) {
  if (l->undo.count >= 64) {
    cw_free(l->undo.data[0].text);
    memmove(l->undo.data, l->undo.data + 1, sizeof(undo_rec) * (size_t)(l->undo.count - 1));
    l->undo.count--;
  }
  undo_rec u = { cw_strdup(ui_str_c(&l->text), CW_MEM_UI), l->caret };
  cw_array_push(&l->undo, u);
}

static void changed(cw_node n, le_data *l) {
  l->blink_t0 = UI.time;
  ui_emit_str(n, "text_changed", ui_str_c(&l->text));
  (void)l;
}

static bool delete_sel(le_data *l) {
  if (!has_sel(l)) return false;
  int a = sel_from(l), b = sel_to(l);
  ui_str_erase(&l->text, a, b - a);
  l->caret = l->anchor = a;
  return true;
}

static void insert_text(cw_node n, le_data *l, const char *t) {
  /* no newlines, and at most max_length characters */
  int tl = (int)strlen(t);
  char *clean = cw_frame_alloc((size_t)tl + 1);
  int k = 0;
  for (int i = 0; i < tl; i++)
    if (t[i] != '\n' && t[i] != '\r') clean[k++] = t[i];
  clean[k] = 0;
  if (!k && !has_sel(l)) return;
  push_undo(l);
  delete_sel(l);
  if (l->max_length > 0) {
    int room = l->max_length - ui_utf8_count(l->text.s ? l->text.s : "", l->text.len);
    int cut = 0, chars = 0;
    while (cut < k && chars < room) cut = ui_utf8_next(clean, k, cut), chars++;
    k = room <= 0 ? 0 : cut;
  }
  ui_str_insert(&l->text, l->caret, clean, k);
  l->caret += k;
  l->anchor = l->caret;
  changed(n, l);
}

static cw_rect content_rect(cw_node n, le_data *l) {
  cw_control *c = CTL(n);
  cw_ui_style st = T_STYLE(n, l->editable ? "normal" : "read_only");
  return style_content(&st, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
}

static void keep_caret_visible(cw_node n, le_data *l) {
  cw_rect in = content_rect(n, l);
  int dl;
  const char *d = display(l, &dl);
  float cx = cw_font_caret_x(T_FONT(n), d, to_display(l, l->caret), T_FSIZE(n));
  if (cx - l->scroll_x > in.w - 1) l->scroll_x = cx - in.w + 1;
  if (cx - l->scroll_x < 0) l->scroll_x = cx;
  float full = cw_ui_text_width(T_FONT(n), T_FSIZE(n), d);
  if (l->scroll_x > 0 && full - l->scroll_x < in.w - 1) l->scroll_x = CW_MAX(0, full - in.w + 1);
}

static cw_vec2 le_min(cw_node n, void *d) {
  (void)d;
  cw_ui_style st = T_STYLE(n, "normal");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  float w = cw_ui_text_width(f, fs, "M") * T_CONST(n, "minimum_character_width");
  return (cw_vec2){ ceilf(w + SM_W(&st)), ceilf(cw_ui_line_height(f, fs) + SM_H(&st)) };
}

static void le_draw(cw_node n, void *d) {
  le_data *l = d;
  cw_control *c = CTL(n);
  cw_rect all = { 0, 0, c->rect.w, c->rect.h };
  cw_ui_style st = T_STYLE(n, l->editable ? "normal" : "read_only");
  cw_ui_draw_style(&st, all);
  cw_rect in = style_content(&st, all);
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  float lh = cw_ui_line_height(f, fs);
  float ty = in.y + ui_round((in.h - lh) * 0.5f);
  bool focused = cw_control_has_focus(n);
  int dl;
  const char *disp = display(l, &dl);
  cw_ui_push_clip((cw_rect){ in.x - 1, all.y, in.w + 2, all.h });
  if (has_sel(l) && focused) {
    float a = cw_font_caret_x(f, disp, to_display(l, sel_from(l)), fs), b = cw_font_caret_x(f, disp, to_display(l, sel_to(l)), fs);
    cw_ui_draw_rect((cw_rect){ in.x + a - l->scroll_x, ty, b - a, lh }, T_COLOR(n, "selection_color"));
  }
  if (l->text.len == 0) {
    if (l->placeholder_text) cw_ui_draw_text(f, fs, ui_tr(n, l->placeholder_text), (cw_vec2){ in.x, ty }, T_COLOR(n, "font_placeholder_color"));
  } else cw_ui_draw_text(f, fs, disp, (cw_vec2){ in.x - l->scroll_x, ty }, T_COLOR(n, "font_color"));
  if (focused && l->editable && (UI.reduced_motion || fmod(UI.time - l->blink_t0, 1.1) < 0.65)) {
    float cx = cw_font_caret_x(f, disp, to_display(l, l->caret), fs);
    cw_ui_draw_rect((cw_rect){ in.x + cx - l->scroll_x, ty, CW_MAX(1.0f, ui_round(fs / 14)), lh }, T_COLOR(n, "caret_color"));
  }
  cw_ui_pop_clip();
  if (focused) {
    cw_ui_style fo = T_STYLE(n, "focus");
    cw_ui_draw_style(&fo, all);
  }
}

static int byte_at(cw_node n, le_data *l, float x) {
  cw_rect in = content_rect(n, l);
  int dl;
  const char *disp = display(l, &dl);
  return from_display(l, ui_byte_at_x(T_FONT(n), T_FSIZE(n), disp, dl, x - in.x + l->scroll_x));
}

static void move_caret(le_data *l, int to, bool extend) {
  l->caret = CW_CLAMP(to, 0, l->text.len);
  if (!extend) l->anchor = l->caret;
  l->blink_t0 = UI.time;
}

static bool le_input(cw_node n, void *d, const cw_ui_event *e) {
  le_data *l = d;
  const char *s = ui_str_c(&l->text);
  switch (e->type) {
  case CW_UI_MOUSE_DOWN: {
    if (e->button != 0) return true;
    int b = byte_at(n, l, e->pos.x);
    if (e->clicks == 2) {
      l->anchor = ui_word_left(s, b);
      l->caret = ui_word_right(s, l->text.len, b);
    } else if (e->clicks >= 3) {
      l->anchor = 0;
      l->caret = l->text.len;
    } else {
      move_caret(l, b, e->shift);
      l->dragging = true;
    }
    keep_caret_visible(n, l);
    return true;
  }
  case CW_UI_MOUSE_MOVE:
    if (!l->dragging) return false;
    move_caret(l, byte_at(n, l, e->pos.x), true);
    keep_caret_visible(n, l);
    return true;
  case CW_UI_MOUSE_UP:
    l->dragging = false;
    return true;
  case CW_UI_TEXT:
    if (!l->editable || e->ctrl) return true;
    insert_text(n, l, e->text);
    keep_caret_visible(n, l);
    return true;
  case CW_UI_KEY: {
    bool sh = e->shift, ctl = e->ctrl;
    switch (e->key) {
    case K_LEFT:
      if (has_sel(l) && !sh) move_caret(l, sel_from(l), false);
      else move_caret(l, ctl ? ui_word_left(s, l->caret) : ui_utf8_prev(s, l->caret), sh);
      break;
    case K_RIGHT:
      if (has_sel(l) && !sh) move_caret(l, sel_to(l), false);
      else move_caret(l, ctl ? ui_word_right(s, l->text.len, l->caret) : ui_utf8_next(s, l->text.len, l->caret), sh);
      break;
    case K_HOME: move_caret(l, 0, sh); break;
    case K_END: move_caret(l, l->text.len, sh); break;
    case K_BACKSPACE:
      if (!l->editable) return true;
      push_undo(l);
      if (!delete_sel(l) && l->caret > 0) {
        int p = ctl ? ui_word_left(s, l->caret) : ui_utf8_prev(s, l->caret);
        ui_str_erase(&l->text, p, l->caret - p);
        l->caret = l->anchor = p;
      }
      changed(n, l);
      break;
    case K_DELETE:
      if (!l->editable) return true;
      push_undo(l);
      if (!delete_sel(l) && l->caret < l->text.len) {
        int q = ctl ? ui_word_right(s, l->text.len, l->caret) : ui_utf8_next(s, l->text.len, l->caret);
        ui_str_erase(&l->text, l->caret, q - l->caret);
      }
      changed(n, l);
      break;
    case K_RETURN: case K_KP_ENTER:
      ui_emit_str(n, "text_submitted", ui_str_c(&l->text));
      break;
    case K_A:
      if (!ctl) return false;
      l->anchor = 0;
      l->caret = l->text.len;
      break;
    case K_C: case K_X:
      if (!ctl) return false;
      if (has_sel(l) && !l->secret) {
        char *t = cw_frame_alloc((size_t)(sel_to(l) - sel_from(l)) + 1);
        memcpy(t, s + sel_from(l), (size_t)(sel_to(l) - sel_from(l)));
        t[sel_to(l) - sel_from(l)] = 0;
        cw_clipboard_set(t);
        if (e->key == K_X && l->editable) {
          push_undo(l);
          delete_sel(l);
          changed(n, l);
        }
      }
      break;
    case K_V:
      if (!ctl) return false;
      if (l->editable) insert_text(n, l, cw_clipboard_get());
      break;
    case K_Z:
      if (!ctl) return false;
      if (l->undo.count && l->editable) {
        undo_rec u = cw_array_pop(&l->undo);
        ui_str_set(&l->text, u.text, -1);
        l->caret = l->anchor = CW_MIN(u.caret, l->text.len);
        cw_free(u.text);
        changed(n, l);
      }
      break;
    default:
      /* printable keys arrive as TEXT; consume them so they never navigate */
      if (ctl) return false;
      return e->key == 44 || (e->key >= 4 && e->key <= 39) || (e->key >= 45 && e->key <= 56);
    }
    keep_caret_visible(n, l);
    return true;
  }
  case CW_UI_NAV:
    /* a pad moves the caret; at an end it navigates out instead */
    if (e->nav == CW_NAV_LEFT && l->caret > 0) { move_caret(l, ui_utf8_prev(s, l->caret), false); keep_caret_visible(n, l); return true; }
    if (e->nav == CW_NAV_RIGHT && l->caret < l->text.len) { move_caret(l, ui_utf8_next(s, l->text.len, l->caret), false); keep_caret_visible(n, l); return true; }
    if (e->nav == CW_NAV_ACCEPT) { ui_emit_str(n, "text_submitted", ui_str_c(&l->text)); return true; }
    return false;
  default: return false;
  }
}

static void le_focus(cw_node n, void *d, bool f) {
  le_data *l = d;
  (void)n;
  l->blink_t0 = UI.time;
  if (!f) l->anchor = l->caret, l->dragging = false;
}

static void le_init(cw_node n, void *d) {
  le_data *l = d;
  l->editable = true;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
  CTL(n)->size_flags_vertical = CW_SIZE_SHRINK_CENTER;
}
static void le_destroy(cw_node n, void *d) {
  (void)n;
  le_data *l = d;
  ui_str_free(&l->text);
  for (int i = 0; i < l->undo.count; i++) cw_free(l->undo.data[i].text);
  cw_array_free(&l->undo);
}
static void le_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}

void cw_line_edit_set_text(cw_node n, const char *t) {
  le_data *l = cw_node_data_t(n, le_t);
  if (!l) return;
  ui_str_set(&l->text, t ? t : "", -1);
  l->caret = l->anchor = CW_MIN(l->caret, l->text.len);
  l->scroll_x = 0;
}
const char *cw_line_edit_text(cw_node n) {
  le_data *l = cw_node_data_t(n, le_t);
  return l ? ui_str_c(&l->text) : "";
}
/* a byte offset clamped to the text and moved back to the start of the
 * code point it falls inside (callers pass bytes; editing needs whole
 * characters) */
static int cp_floor(le_data *l, int b) {
  const char *s = ui_str_c(&l->text);
  b = CW_CLAMP(b, 0, l->text.len);
  while (b > 0 && b < l->text.len && ((unsigned char)s[b] & 0xC0) == 0x80) b--;
  return b;
}
void cw_line_edit_set_caret(cw_node n, int b) {
  le_data *l = cw_node_data_t(n, le_t);
  if (l) move_caret(l, cp_floor(l, b), false);
}
int cw_line_edit_caret(cw_node n) {
  le_data *l = cw_node_data_t(n, le_t);
  return l ? l->caret : 0;
}
void cw_line_edit_select(cw_node n, int a, int b) {
  le_data *l = cw_node_data_t(n, le_t);
  if (!l) return;
  l->anchor = cp_floor(l, a);
  l->caret = cp_floor(l, b);
}
void cw_line_edit_selection(cw_node n, int *a, int *b) {
  le_data *l = cw_node_data_t(n, le_t);
  if (a) *a = l ? sel_from(l) : 0;
  if (b) *b = l ? sel_to(l) : 0;
}

static void get_text(cw_node n, void *d, cw_value *o) {
  (void)n;
  *o = cw_val_string(ui_str_c(&((le_data *)d)->text));
}
static void set_text(cw_node n, void *d, const cw_value *v) {
  (void)d;
  cw_line_edit_set_text(n, v->s);
}

static const cw_prop le_props[] = {
  CW_PROP_ACCESSOR("text", CW_T_STRING, CW_PROP_DEFAULT, get_text, set_text, "The text."),
  CW_PROP_FIELD(le_data, placeholder_text, CW_T_STRING, CW_PROP_DEFAULT, "Shown dimmed while empty."),
  CW_PROP_FIELD(le_data, max_length, CW_T_INT, CW_PROP_DEFAULT, "Maximum characters (0 = no limit)."),
  CW_PROP_FIELD(le_data, secret, CW_T_BOOL, CW_PROP_DEFAULT, "Draw secret_character instead of the text."),
  CW_PROP_FIELD(le_data, secret_character, CW_T_STRING, CW_PROP_DEFAULT, "Drawn per character in secret mode (default a bullet)."),
  CW_PROP_FIELD(le_data, editable, CW_T_BOOL, CW_PROP_DEFAULT, "False = read-only (still selectable)."),
  CW_PROP_END
};
static const char *const le_signals[] = { "text_changed", "text_submitted", NULL };
static cw_node_type le_type = { .name = "LineEdit", .parent = "Control", .data_size = sizeof(le_data), .props = le_props,
                                .signals = le_signals, .init = le_init, .destroy = le_destroy, .prop_changed = le_changed,
                                .doc = "Single-line text input." };

void ui_register_line_edit(void) {
  cw_node_type_register(&le_type);
  le_t = &le_type;
  cw_ui_class_register("LineEdit", &(cw_ui_class){ .min_size = le_min, .draw = le_draw, .input = le_input,
                                                   .focus_changed = le_focus, .wants_text = true });
}

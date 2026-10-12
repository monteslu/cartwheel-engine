/* TextEdit: multi-line editing with line numbers, scrolling, selection,
 * clipboard, undo and a syntax highlighter hook; plus a C highlighter. */
#include "ui_internal.h"

enum { K_BACKSPACE = 42, K_DELETE = 76, K_HOME = 74, K_END = 77, K_PGUP = 75, K_PGDN = 78, K_LEFT = 80, K_RIGHT = 79,
       K_UP = 82, K_DOWN = 81, K_RETURN = 40, K_KP_ENTER = 88, K_TAB = 43, K_A = 4, K_C = 6, K_V = 25, K_X = 27, K_Z = 29 };

typedef struct { int line, col; } tpos;

typedef struct {
  bool editable, show_line_numbers, highlight_current_line;
  int32_t syntax_highlighting; /* 0 none (or the C hook), 1 built-in C */
  CW_ARRAY(ui_str) lines;
  tpos caret, anchor;
  float want_x; /* column memory for up/down */
  float scroll_x, scroll_y;
  ui_scrollbar vb, hb;
  bool dragging;
  double blink_t0;
  cw_highlight_fn hl;
  void *hl_user;
  CW_ARRAY(char *) undo;
  float max_w; /* widest line (cached) */
  bool max_w_dirty;
} te_data;
static const cw_node_type *te_t;

static int tcmp(tpos a, tpos b) { return a.line != b.line ? a.line - b.line : a.col - b.col; }
static tpos tmin(tpos a, tpos b) { return tcmp(a, b) <= 0 ? a : b; }
static tpos tmax(tpos a, tpos b) { return tcmp(a, b) >= 0 ? a : b; }
static bool has_sel(te_data *t) { return tcmp(t->caret, t->anchor) != 0; }
static ui_str *L(te_data *t, int i) { return &t->lines.data[i]; }

static void set_text_raw(te_data *t, const char *s) {
  for (int i = 0; i < t->lines.count; i++) ui_str_free(L(t, i));
  cw_array_clear(&t->lines);
  const char *p = s ? s : "";
  for (;;) {
    const char *e = strchr(p, '\n');
    int len = e ? (int)(e - p) : (int)strlen(p);
    ui_str line = { 0 };
    /* tabs become 4 spaces: the editor's code view is space-indented */
    for (int i = 0; i < len; i++) {
      if (p[i] == '\t') ui_str_insert(&line, line.len, "    ", 4);
      else if (p[i] != '\r') ui_str_insert(&line, line.len, p + i, 1);
    }
    if (!line.s) ui_str_set(&line, "", 0);
    cw_array_push(&t->lines, line);
    if (!e) break;
    p = e + 1;
  }
  t->max_w_dirty = true;
}

static char *join(te_data *t) {
  size_t n = 0;
  for (int i = 0; i < t->lines.count; i++) n += (size_t)L(t, i)->len + 1;
  char *s = cw_frame_alloc(n + 1), *o = s;
  for (int i = 0; i < t->lines.count; i++) {
    memcpy(o, ui_str_c(L(t, i)), (size_t)L(t, i)->len);
    o += L(t, i)->len;
    if (i + 1 < t->lines.count) *o++ = '\n';
  }
  *o = 0;
  return s;
}

static char *range_text(te_data *t, tpos a, tpos b) {
  size_t n = 0;
  for (int i = a.line; i <= b.line; i++) n += (size_t)L(t, i)->len + 1;
  char *s = cw_frame_alloc(n + 1), *o = s;
  for (int i = a.line; i <= b.line; i++) {
    int from = i == a.line ? a.col : 0, to = i == b.line ? b.col : L(t, i)->len;
    memcpy(o, ui_str_c(L(t, i)) + from, (size_t)(to - from));
    o += to - from;
    if (i < b.line) *o++ = '\n';
  }
  *o = 0;
  return s;
}

static void push_undo(te_data *t) {
  if (t->undo.count >= 64) {
    cw_free(t->undo.data[0]);
    memmove(t->undo.data, t->undo.data + 1, sizeof(char *) * (size_t)(t->undo.count - 1));
    t->undo.count--;
  }
  cw_array_push(&t->undo, cw_strdup(join(t), CW_MEM_UI));
}

static void delete_range(te_data *t, tpos a, tpos b) {
  if (a.line == b.line) {
    ui_str_erase(L(t, a.line), a.col, b.col - a.col);
  } else {
    ui_str *first = L(t, a.line), *last = L(t, b.line);
    first->len = a.col;
    first->s[a.col] = 0;
    ui_str_insert(first, a.col, ui_str_c(last) + b.col, last->len - b.col);
    for (int i = a.line + 1; i <= b.line; i++) ui_str_free(L(t, i));
    int gone = b.line - a.line;
    memmove(t->lines.data + a.line + 1, t->lines.data + b.line + 1, sizeof(ui_str) * (size_t)(t->lines.count - b.line - 1));
    t->lines.count -= gone;
  }
  t->caret = t->anchor = a;
  t->max_w_dirty = true;
}

static bool delete_sel(te_data *t) {
  if (!has_sel(t)) return false;
  delete_range(t, tmin(t->caret, t->anchor), tmax(t->caret, t->anchor));
  return true;
}

static void insert_at_caret(te_data *t, const char *s) {
  delete_sel(t);
  for (const char *p = s; *p;) {
    const char *e = strchr(p, '\n');
    int len = e ? (int)(e - p) : (int)strlen(p);
    for (int i = 0; i < len; i++) {
      if (p[i] == '\r') continue;
      if (p[i] == '\t') {
        ui_str_insert(L(t, t->caret.line), t->caret.col, "    ", 4);
        t->caret.col += 4;
      } else {
        ui_str_insert(L(t, t->caret.line), t->caret.col, p + i, 1);
        t->caret.col++;
      }
    }
    if (e) { /* split the line */
      ui_str *cur = L(t, t->caret.line);
      ui_str nl = { 0 };
      ui_str_set(&nl, ui_str_c(cur) + t->caret.col, cur->len - t->caret.col);
      cur->len = t->caret.col;
      cur->s[cur->len] = 0;
      cw_array_push(&t->lines, (ui_str){ 0 });
      memmove(t->lines.data + t->caret.line + 2, t->lines.data + t->caret.line + 1,
              sizeof(ui_str) * (size_t)(t->lines.count - t->caret.line - 2));
      t->lines.data[t->caret.line + 1] = nl;
      t->caret.line++;
      t->caret.col = 0;
      p = e + 1;
    } else break;
  }
  t->anchor = t->caret;
  t->max_w_dirty = true;
}

/* ── metrics ────────────────────────────────────────────────────────── */

typedef struct { cw_font f; float fs, lh, gutter; cw_rect in, view; } te_m;

static te_m metrics(cw_node n, te_data *t) {
  te_m m;
  cw_control *c = CTL(n);
  m.f = T_FONT(n);
  m.fs = T_FSIZE(n);
  m.lh = ceilf(cw_ui_line_height(m.f, m.fs));
  m.gutter = 0;
  if (t->show_line_numbers) {
    char b[16];
    snprintf(b, sizeof b, "%d", CW_MAX(t->lines.count, 100) * 10 - 1);
    m.gutter = ceilf(cw_ui_text_width(m.f, m.fs, b + 1)) + T_CONST(n, "gutter_separation");
  }
  cw_ui_style st = T_STYLE(n, "normal");
  m.in = style_content(&st, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
  float th = ui_scrollbar_thickness(n);
  m.view = (cw_rect){ m.in.x + m.gutter, m.in.y, m.in.w - m.gutter - th, m.in.h - th };
  return m;
}

static float line_x(te_m *m, te_data *t, int line, int col) {
  return cw_font_caret_x(m->f, ui_str_c(L(t, line)), col, m->fs);
}

static float widest(te_m *m, te_data *t) {
  if (!t->max_w_dirty) return t->max_w;
  float w = 0;
  for (int i = 0; i < t->lines.count; i++)
    if (L(t, i)->len) w = CW_MAX(w, cw_ui_text_width(m->f, m->fs, ui_str_c(L(t, i))));
  t->max_w = w;
  t->max_w_dirty = false;
  return w;
}

static void update_bars(te_m *m, te_data *t) {
  float th = m->in.w > 0 ? m->in.x + m->in.w - m->view.x - m->view.w : 0;
  t->vb.vertical = true;
  t->vb.page = m->view.h;
  t->vb.max = (float)t->lines.count * m->lh;
  t->vb.rect = (cw_rect){ m->view.x + m->view.w, m->in.y, th, m->view.h };
  t->hb.vertical = false;
  t->hb.page = m->view.w;
  t->hb.max = widest(m, t) + m->lh;
  t->hb.rect = (cw_rect){ m->view.x, m->view.y + m->view.h, m->view.w, th };
  t->scroll_y = ui_scrollbar_clamp(&t->vb, t->scroll_y);
  t->scroll_x = ui_scrollbar_clamp(&t->hb, t->scroll_x);
  t->vb.value = t->scroll_y;
  t->hb.value = t->scroll_x;
}

static void caret_visible(cw_node n, te_data *t) {
  if (CTL(n)->rect.w <= 0) { t->want_x = -1; return; } /* not laid out yet: nothing to scroll */
  te_m m = metrics(n, t);
  float y = (float)t->caret.line * m.lh;
  if (y < t->scroll_y) t->scroll_y = y;
  if (y + m.lh > t->scroll_y + m.view.h) t->scroll_y = y + m.lh - m.view.h;
  float x = line_x(&m, t, t->caret.line, t->caret.col);
  if (x < t->scroll_x) t->scroll_x = x;
  if (x + 2 > t->scroll_x + m.view.w) t->scroll_x = x + 2 - m.view.w;
  update_bars(&m, t);
}

/* ── drawing ────────────────────────────────────────────────────────── */

static cw_vec2 te_min(cw_node n, void *d) {
  te_data *t = d;
  te_m m = metrics(n, t);
  cw_ui_style st = T_STYLE(n, "normal");
  return (cw_vec2){ ceilf(m.gutter + cw_ui_text_width(m.f, m.fs, "MMMMMMMM") + SM_W(&st)), ceilf(m.lh * 2 + SM_H(&st)) };
}

static void te_draw(cw_node n, void *d) {
  te_data *t = d;
  cw_control *c = CTL(n);
  cw_rect all = { 0, 0, c->rect.w, c->rect.h };
  cw_ui_style st = T_STYLE(n, "normal");
  cw_ui_draw_style(&st, all);
  te_m m = metrics(n, t);
  update_bars(&m, t);
  bool focused = cw_control_has_focus(n);
  int first = (int)(t->scroll_y / m.lh), last = (int)((t->scroll_y + m.view.h) / m.lh) + 1;
  if (last > t->lines.count) last = t->lines.count;
  cw_color fc = T_COLOR(n, "font_color"), ln = T_COLOR(n, "line_number_color"), selc = T_COLOR(n, "selection_color");
  tpos s0 = tmin(t->caret, t->anchor), s1 = tmax(t->caret, t->anchor);
  /* gutter */
  if (t->show_line_numbers) {
    cw_ui_push_clip((cw_rect){ m.in.x, m.in.y, m.gutter, m.view.h });
    for (int i = first; i < last; i++) {
      char b[16];
      snprintf(b, sizeof b, "%d", i + 1);
      float y = m.in.y + (float)i * m.lh - t->scroll_y;
      float w = cw_ui_text_width(m.f, m.fs, b);
      cw_ui_draw_text(m.f, m.fs, b, (cw_vec2){ m.in.x + m.gutter - T_CONST(n, "gutter_separation") - w, y },
                      i == t->caret.line && focused ? fc : ln);
    }
    cw_ui_pop_clip();
  }
  cw_ui_push_clip(m.view);
  if (t->highlight_current_line && focused && !has_sel(t))
    cw_ui_draw_rect((cw_rect){ m.view.x, m.view.y + (float)t->caret.line * m.lh - t->scroll_y, m.view.w, m.lh },
                    T_COLOR(n, "current_line_color"));
  cw_text_span spans[128];
  for (int i = first; i < last; i++) {
    float y = m.view.y + (float)i * m.lh - t->scroll_y;
    float x0 = m.view.x - t->scroll_x;
    const char *line = ui_str_c(L(t, i));
    int len = L(t, i)->len;
    if (has_sel(t) && i >= s0.line && i <= s1.line) {
      int a = i == s0.line ? s0.col : 0, b = i == s1.line ? s1.col : len;
      float xa = line_x(&m, t, i, a), xb = line_x(&m, t, i, b) + (i < s1.line ? m.fs * 0.35f : 0);
      cw_ui_draw_rect((cw_rect){ x0 + xa, y, xb - xa, m.lh }, selc);
    }
    if (!len) continue;
    cw_highlight_fn hlf = t->hl ? t->hl : t->syntax_highlighting == 1 ? cw_ui_highlight_c : NULL;
    int ns = hlf ? hlf(n, line, len, i, spans, 128, t->hl_user) : 0;
    if (!ns) {
      cw_ui_draw_text(m.f, m.fs, line, (cw_vec2){ x0, y }, fc);
      continue;
    }
    /* runs: highlighted spans, default colour between them */
    int at = 0;
    for (int k = 0; k <= ns; k++) {
      int sa = k < ns ? CW_CLAMP(spans[k].start, at, len) : len, sb = k < ns ? CW_CLAMP(spans[k].end, sa, len) : len;
      if (sa > at) {
        char *seg = cw_frame_printf("%.*s", sa - at, line + at);
        cw_ui_draw_text(m.f, m.fs, seg, (cw_vec2){ x0 + line_x(&m, t, i, at), y }, fc);
      }
      if (k < ns && sb > sa) {
        char *seg = cw_frame_printf("%.*s", sb - sa, line + sa);
        cw_ui_draw_text(m.f, m.fs, seg, (cw_vec2){ x0 + line_x(&m, t, i, sa), y }, spans[k].color);
      }
      at = k < ns ? sb : len;
    }
  }
  if (focused && t->editable && (UI.reduced_motion || fmod(UI.time - t->blink_t0, 1.1) < 0.65)) {
    float x = m.view.x - t->scroll_x + line_x(&m, t, t->caret.line, t->caret.col);
    float y = m.view.y + (float)t->caret.line * m.lh - t->scroll_y;
    cw_ui_draw_rect((cw_rect){ x, y, CW_MAX(1.0f, ui_round(m.fs / 14)), m.lh }, T_COLOR(n, "caret_color"));
  }
  cw_ui_pop_clip();
  if (ui_scrollbar_needed(&t->vb)) ui_scrollbar_draw(n, &t->vb);
  if (ui_scrollbar_needed(&t->hb)) ui_scrollbar_draw(n, &t->hb);
  if (focused) {
    cw_ui_style fo = T_STYLE(n, "focus");
    cw_ui_draw_style(&fo, all);
  }
}

/* ── input ──────────────────────────────────────────────────────────── */

static tpos pos_at(cw_node n, te_data *t, cw_vec2 p) {
  te_m m = metrics(n, t);
  int line = (int)floorf((p.y - m.view.y + t->scroll_y) / m.lh);
  line = CW_CLAMP(line, 0, t->lines.count - 1);
  int col = ui_byte_at_x(m.f, m.fs, ui_str_c(L(t, line)), L(t, line)->len, p.x - m.view.x + t->scroll_x);
  return (tpos){ line, col };
}

static void move(te_data *t, tpos p, bool extend) {
  p.line = CW_CLAMP(p.line, 0, t->lines.count - 1);
  p.col = CW_CLAMP(p.col, 0, L(t, p.line)->len);
  t->caret = p;
  if (!extend) t->anchor = p;
  t->blink_t0 = UI.time;
}

static void vmove(cw_node n, te_data *t, int dl, bool extend) {
  te_m m = metrics(n, t);
  if (t->want_x < 0) t->want_x = line_x(&m, t, t->caret.line, t->caret.col);
  int line = CW_CLAMP(t->caret.line + dl, 0, t->lines.count - 1);
  int col = ui_byte_at_x(m.f, m.fs, ui_str_c(L(t, line)), L(t, line)->len, t->want_x);
  float keep = t->want_x;
  move(t, (tpos){ line, col }, extend);
  t->want_x = keep;
}

static void edited(cw_node n, te_data *t) {
  t->blink_t0 = UI.time;
  t->want_x = -1;
  ui_emit0(n, "text_changed");
}

static bool te_input(cw_node n, void *d, const cw_ui_event *e) {
  te_data *t = d;
  bool ch;
  if (ui_scrollbar_input(n, &t->vb, e, &ch)) {
    if (ch) t->scroll_y = t->vb.value;
    return true;
  }
  if (ui_scrollbar_input(n, &t->hb, e, &ch)) {
    if (ch) t->scroll_x = t->hb.value;
    return true;
  }
  switch (e->type) {
  case CW_UI_WHEEL: {
    te_m m = metrics(n, t);
    float before = t->scroll_y, bx = t->scroll_x;
    if (e->shift || e->wheel.x != 0) t->scroll_x -= (e->wheel.x != 0 ? e->wheel.x : e->wheel.y) * m.lh * 3;
    else t->scroll_y -= e->wheel.y * m.lh * 3;
    update_bars(&m, t);
    return before != t->scroll_y || bx != t->scroll_x;
  }
  case CW_UI_MOUSE_DOWN: {
    if (e->button != 0) return true;
    tpos p = pos_at(n, t, e->pos);
    if (e->clicks == 2) {
      const char *s = ui_str_c(L(t, p.line));
      t->anchor = (tpos){ p.line, ui_word_left(s, p.col) };
      t->caret = (tpos){ p.line, ui_word_right(s, L(t, p.line)->len, p.col) };
    } else if (e->clicks >= 3) {
      t->anchor = (tpos){ p.line, 0 };
      t->caret = (tpos){ p.line, L(t, p.line)->len };
    } else {
      move(t, p, e->shift);
      t->dragging = true;
    }
    t->want_x = -1;
    return true;
  }
  case CW_UI_MOUSE_MOVE:
    if (!t->dragging) return false;
    move(t, pos_at(n, t, e->pos), true);
    caret_visible(n, t);
    return true;
  case CW_UI_MOUSE_UP:
    t->dragging = false;
    return true;
  case CW_UI_TEXT:
    if (!t->editable || e->ctrl) return true;
    push_undo(t);
    insert_at_caret(t, e->text);
    edited(n, t);
    caret_visible(n, t);
    return true;
  case CW_UI_KEY: {
    bool sh = e->shift, ctl = e->ctrl;
    const char *s = ui_str_c(L(t, t->caret.line));
    int len = L(t, t->caret.line)->len;
    switch (e->key) {
    case K_LEFT:
      if (has_sel(t) && !sh) move(t, tmin(t->caret, t->anchor), false);
      else if (t->caret.col > 0) move(t, (tpos){ t->caret.line, ctl ? ui_word_left(s, t->caret.col) : ui_utf8_prev(s, t->caret.col) }, sh);
      else if (t->caret.line > 0) move(t, (tpos){ t->caret.line - 1, L(t, t->caret.line - 1)->len }, sh);
      t->want_x = -1;
      break;
    case K_RIGHT:
      if (has_sel(t) && !sh) move(t, tmax(t->caret, t->anchor), false);
      else if (t->caret.col < len) move(t, (tpos){ t->caret.line, ctl ? ui_word_right(s, len, t->caret.col) : ui_utf8_next(s, len, t->caret.col) }, sh);
      else if (t->caret.line + 1 < t->lines.count) move(t, (tpos){ t->caret.line + 1, 0 }, sh);
      t->want_x = -1;
      break;
    case K_UP: vmove(n, t, -1, sh); break;
    case K_DOWN: vmove(n, t, 1, sh); break;
    case K_PGUP: case K_PGDN: {
      te_m m = metrics(n, t);
      int page = CW_MAX(1, (int)(m.view.h / m.lh) - 1);
      vmove(n, t, e->key == K_PGUP ? -page : page, sh);
      break;
    }
    case K_HOME: {
      if (ctl) { move(t, (tpos){ 0, 0 }, sh); break; }
      int ind = 0;
      while (ind < len && s[ind] == ' ') ind++;
      move(t, (tpos){ t->caret.line, t->caret.col == ind ? 0 : ind }, sh); /* smart home */
      t->want_x = -1;
      break;
    }
    case K_END:
      if (ctl) move(t, (tpos){ t->lines.count - 1, L(t, t->lines.count - 1)->len }, sh);
      else move(t, (tpos){ t->caret.line, len }, sh);
      t->want_x = -1;
      break;
    case K_BACKSPACE:
      if (!t->editable) return true;
      push_undo(t);
      if (!delete_sel(t)) {
        if (t->caret.col > 0) {
          int p = ctl ? ui_word_left(s, t->caret.col) : ui_utf8_prev(s, t->caret.col);
          delete_range(t, (tpos){ t->caret.line, p }, t->caret);
        } else if (t->caret.line > 0) {
          tpos a = { t->caret.line - 1, L(t, t->caret.line - 1)->len };
          delete_range(t, a, t->caret);
        }
      }
      edited(n, t);
      break;
    case K_DELETE:
      if (!t->editable) return true;
      push_undo(t);
      if (!delete_sel(t)) {
        if (t->caret.col < len) delete_range(t, t->caret, (tpos){ t->caret.line, ctl ? ui_word_right(s, len, t->caret.col) : ui_utf8_next(s, len, t->caret.col) });
        else if (t->caret.line + 1 < t->lines.count) delete_range(t, t->caret, (tpos){ t->caret.line + 1, 0 });
      }
      edited(n, t);
      break;
    case K_RETURN: case K_KP_ENTER: {
      if (!t->editable) return true;
      push_undo(t);
      int ind = 0;
      while (ind < len && s[ind] == ' ' && ind < t->caret.col) ind++;
      char *ins = cw_frame_alloc((size_t)ind + 2);
      ins[0] = '\n';
      memset(ins + 1, ' ', (size_t)ind);
      ins[ind + 1] = 0;
      insert_at_caret(t, ins);
      edited(n, t);
      break;
    }
    case K_TAB:
      if (!t->editable || ctl) return false;
      push_undo(t);
      if (sh) { /* unindent the caret line */
        int k = 0;
        while (k < 4 && k < len && s[k] == ' ') k++;
        if (k) {
          ui_str_erase(L(t, t->caret.line), 0, k);
          t->caret.col = CW_MAX(0, t->caret.col - k);
          t->anchor = t->caret;
        }
      } else insert_at_caret(t, cw_frame_printf("%.*s", 4 - (t->caret.col % 4), "    "));
      edited(n, t);
      break;
    case K_A:
      if (!ctl) return true;
      t->anchor = (tpos){ 0, 0 };
      t->caret = (tpos){ t->lines.count - 1, L(t, t->lines.count - 1)->len };
      break;
    case K_C: case K_X:
      if (!ctl) return true;
      if (has_sel(t)) {
        cw_clipboard_set(range_text(t, tmin(t->caret, t->anchor), tmax(t->caret, t->anchor)));
        if (e->key == K_X && t->editable) {
          push_undo(t);
          delete_sel(t);
          edited(n, t);
        }
      }
      break;
    case K_V:
      if (!ctl) return true;
      if (t->editable) {
        push_undo(t);
        insert_at_caret(t, cw_clipboard_get());
        edited(n, t);
      }
      break;
    case K_Z:
      if (!ctl) return true;
      if (t->undo.count && t->editable) {
        char *u = cw_array_pop(&t->undo);
        tpos keep = t->caret;
        set_text_raw(t, u);
        cw_free(u);
        move(t, keep, false);
        edited(n, t);
      }
      break;
    default:
      if (ctl) return false;
      return e->key == 44 || (e->key >= 4 && e->key <= 39) || (e->key >= 45 && e->key <= 56);
    }
    caret_visible(n, t);
    return true;
  }
  case CW_UI_NAV:
    /* a pad moves the caret; cancel leaves the field */
    if (e->nav == CW_NAV_UP) { vmove(n, t, -1, false); caret_visible(n, t); return true; }
    if (e->nav == CW_NAV_DOWN) { vmove(n, t, 1, false); caret_visible(n, t); return true; }
    return false;
  default: return false;
  }
}

static void te_focus(cw_node n, void *d, bool f) {
  (void)n;
  te_data *t = d;
  t->blink_t0 = UI.time;
  if (!f) t->dragging = false;
}

static void te_init(cw_node n, void *d) {
  te_data *t = d;
  t->editable = true;
  t->highlight_current_line = true;
  t->want_x = -1;
  set_text_raw(t, "");
  CTL(n)->focus_mode = CW_FOCUS_ALL;
  CTL(n)->clip_contents = false;
}
static void te_destroy(cw_node n, void *d) {
  (void)n;
  te_data *t = d;
  for (int i = 0; i < t->lines.count; i++) ui_str_free(L(t, i));
  cw_array_free(&t->lines);
  for (int i = 0; i < t->undo.count; i++) cw_free(t->undo.data[i]);
  cw_array_free(&t->undo);
}
static void te_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}

void cw_text_edit_set_text(cw_node n, const char *s) {
  te_data *t = cw_node_data_t(n, te_t);
  if (!t) return;
  set_text_raw(t, s);
  move(t, (tpos){ CW_MIN(t->caret.line, t->lines.count - 1), t->caret.col }, false);
  t->scroll_x = t->scroll_y = 0;
}
const char *cw_text_edit_text(cw_node n) {
  te_data *t = cw_node_data_t(n, te_t);
  return t ? join(t) : "";
}
int cw_text_edit_line_count(cw_node n) {
  te_data *t = cw_node_data_t(n, te_t);
  return t ? t->lines.count : 0;
}
const char *cw_text_edit_line(cw_node n, int i) {
  te_data *t = cw_node_data_t(n, te_t);
  if (!t || i < 0 || i >= t->lines.count) return "";
  return cw_frame_printf("%s", ui_str_c(L(t, i)));
}
void cw_text_edit_set_caret(cw_node n, int line, int col) {
  te_data *t = cw_node_data_t(n, te_t);
  if (!t) return;
  move(t, (tpos){ line, col }, false);
  caret_visible(n, t);
}
void cw_text_edit_caret(cw_node n, int *line, int *col) {
  te_data *t = cw_node_data_t(n, te_t);
  if (line) *line = t ? t->caret.line : 0;
  if (col) *col = t ? t->caret.col : 0;
}
void cw_text_edit_insert(cw_node n, const char *s) {
  te_data *t = cw_node_data_t(n, te_t);
  if (!t) return;
  push_undo(t);
  insert_at_caret(t, s);
  edited(n, t);
}
void cw_text_edit_set_highlighter(cw_node n, cw_highlight_fn fn, void *user) {
  te_data *t = cw_node_data_t(n, te_t);
  if (!t) return;
  t->hl = fn;
  t->hl_user = user;
}

static void get_text(cw_node n, void *d, cw_value *o) {
  (void)n;
  *o = cw_val_string(join(d));
}
static void set_text_prop(cw_node n, void *d, const cw_value *v) {
  (void)d;
  cw_text_edit_set_text(n, v->s);
}

/* ── the C highlighter ──────────────────────────────────────────────── */

static bool in_list(const char *w, int n, const char *const *list) {
  for (int i = 0; list[i]; i++)
    if ((int)strlen(list[i]) == n && !strncmp(list[i], w, (size_t)n)) return true;
  return false;
}

int cw_ui_highlight_c(cw_node n, const char *s, int len, int line, cw_text_span *out, int cap, void *user) {
  (void)line;
  static const char *const kw[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
                                    "return", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "const",
                                    "extern", "inline", "volatile", "register", "restrict", "_Static_assert", "true",
                                    "false", "NULL", "nullptr", NULL };
  static const char *const ty[] = { "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
                                    "bool", "_Bool", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
                                    "uint16_t", "uint32_t", "uint64_t", "uintptr_t", NULL };
  (void)user;
  cw_color ck = cw_control_theme_color(n, "TextEdit", "keyword_color"), ct = cw_control_theme_color(n, "TextEdit", "type_color");
  cw_color cs = cw_control_theme_color(n, "TextEdit", "string_color"), cn = cw_control_theme_color(n, "TextEdit", "number_color");
  cw_color cc = cw_control_theme_color(n, "TextEdit", "comment_color"), cp = cw_control_theme_color(n, "TextEdit", "preprocessor_color");
  int k = 0, i = 0;
#define SPAN(a, b, c) do { if (k < cap) out[k++] = (cw_text_span){ a, b, c }; } while (0)
  while (i < len && s[i] == ' ') i++;
  if (i < len && s[i] == '#') {
    int e = i + 1;
    while (e < len && (s[e] == ' ' || (s[e] >= 'a' && s[e] <= 'z'))) e++;
    SPAN(i, e, cp);
    i = e;
  }
  while (i < len) {
    char c = s[i];
    if (c == '/' && i + 1 < len && s[i + 1] == '/') {
      SPAN(i, len, cc);
      break;
    }
    if (c == '/' && i + 1 < len && s[i + 1] == '*') {
      int e = i + 2;
      while (e + 1 < len && !(s[e] == '*' && s[e + 1] == '/')) e++;
      e = e + 1 < len ? e + 2 : len;
      SPAN(i, e, cc);
      i = e;
      continue;
    }
    if (c == '"' || c == '\'') {
      int e = i + 1;
      while (e < len && s[e] != c) e += s[e] == '\\' ? 2 : 1;
      e = CW_MIN(e + 1, len);
      SPAN(i, e, cs);
      i = e;
      continue;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9')) {
      int e = i;
      while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '.' || s[e] == '_')) e++;
      SPAN(i, e, cn);
      i = e;
      continue;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      int e = i;
      while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
      if (in_list(s + i, e - i, kw)) SPAN(i, e, ck);
      else if (in_list(s + i, e - i, ty) || (e - i > 2 && !strncmp(s + e - 2, "_t", 2))) SPAN(i, e, ct);
      i = e;
      continue;
    }
    i++;
  }
#undef SPAN
  return k;
}

static const char *const syntax_names[] = { "NONE", "C", NULL };
static const cw_prop te_props[] = {
  CW_PROP_ACCESSOR("text", CW_T_STRING, CW_PROP_DEFAULT, get_text, set_text_prop, "The text (lines joined with '\\n')."),
  CW_PROP_FIELD(te_data, editable, CW_T_BOOL, CW_PROP_DEFAULT, "False = read-only (still selectable)."),
  CW_PROP_FIELD(te_data, show_line_numbers, CW_T_BOOL, CW_PROP_DEFAULT, "A gutter with line numbers."),
  CW_PROP_FIELD(te_data, highlight_current_line, CW_T_BOOL, CW_PROP_DEFAULT, "Tint the caret's line while focused."),
  { .name = "syntax_highlighting", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(te_data, syntax_highlighting),
    .size = 4, .enum_names = syntax_names, .doc = "Built-in highlighter: NONE or C (cw_text_edit_set_highlighter overrides)." },
  CW_PROP_END
};
static const char *const te_signals[] = { "text_changed", NULL };
static cw_node_type te_type = { .name = "TextEdit", .parent = "Control", .data_size = sizeof(te_data), .props = te_props,
                                .signals = te_signals, .init = te_init, .destroy = te_destroy, .prop_changed = te_changed,
                                .doc = "Multi-line text editor (the editor's code view)." };

void ui_register_text_edit(void) {
  cw_node_type_register(&te_type);
  te_t = &te_type;
  cw_ui_class_register("TextEdit", &(cw_ui_class){ .min_size = te_min, .draw = te_draw, .input = te_input,
                                                   .focus_changed = te_focus, .wants_text = true });
}

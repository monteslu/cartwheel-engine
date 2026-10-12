/* Label and RichTextLabel. */
#include "ui_internal.h"

/* ── Label ──────────────────────────────────────────────────────────── */

typedef struct {
  const char *text;
  int32_t horizontal_alignment, vertical_alignment;
  bool autowrap, clip_text, ellipsis, uppercase;
  float last_w;
} label_data;
static const cw_node_type *label_t;

static const char *label_str(cw_node node, label_data *l) {
  const char *s = ui_tr(node, ui_s(l->text));
  if (!l->uppercase) return s;
  size_t n = strlen(s);
  char *u = cw_frame_alloc(n + 1);
  for (size_t i = 0; i <= n; i++) u[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
  return u;
}

static cw_vec2 label_min(cw_node n, void *d) {
  label_data *l = d;
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  const char *s = label_str(n, l);
  float lh = cw_ui_line_height(f, fs);
  if (l->autowrap) {
    float w = l->last_w > 0 ? l->last_w : CTL(n)->rect.w;
    if (w <= 0) w = 1e6f;
    cw_text_metrics m = cw_font_measure(f, s, &(cw_text_style){ .size = fs, .wrap_width = w });
    return (cw_vec2){ 0, CW_MAX(m.height, lh) };
  }
  cw_text_metrics m = cw_font_measure(f, s, &(cw_text_style){ .size = fs });
  return (cw_vec2){ (l->clip_text || l->ellipsis) ? 0 : ceilf(m.width), CW_MAX(m.height, lh) };
}

static void label_layout(cw_node n, void *d, cw_vec2 sz) {
  label_data *l = d;
  if (l->autowrap && sz.x != l->last_w) {
    l->last_w = sz.x;
    cw_vec2 before = CTL(n)->_content_min;
    CTL(n)->_flags |= CF_MIN_DIRTY;
    cw_vec2 now = label_min(n, d);
    CTL(n)->_flags &= ~CF_MIN_DIRTY;
    if (now.y != before.y) ui_mark_min_dirty(n); /* height follows width: the parent re-lays out */
  }
}

static void label_draw(cw_node n, void *d) {
  label_data *l = d;
  cw_control *c = CTL(n);
  const char *s = label_str(n, l);
  if (!*s) return;
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n);
  cw_color col = T_COLOR(n, "font_color");
  cw_rect r = { 0, 0, c->rect.w, c->rect.h };
  static const cw_text_align al[4] = { CW_ALIGN_LEFT, CW_ALIGN_CENTER, CW_ALIGN_RIGHT, CW_ALIGN_LEFT };
  if (l->ellipsis && !l->autowrap && !strchr(s, '\n')) {
    cw_ui_draw_text_in(f, fs, s, r, (cw_halign)l->horizontal_alignment, col, true);
    return;
  }
  /* right to left: LEFT and RIGHT mean start and end */
  int ha = l->horizontal_alignment;
  if (ui_draw_rtl()) ha = ha == CW_HALIGN_LEFT ? CW_HALIGN_RIGHT : ha == CW_HALIGN_RIGHT ? CW_HALIGN_LEFT : ha;
  cw_text_style st = { .size = fs, .wrap_width = l->autowrap ? r.w : 0, .align = al[ha & 3],
                       .direction = ui_draw_rtl() ? CW_TEXT_DIR_RTL : CW_TEXT_DIR_AUTO };
  cw_text_metrics m = cw_font_measure(f, s, &st);
  float x = 0, y = 0;
  if (!l->autowrap) {
    if (ha == CW_HALIGN_CENTER) x = ui_round((r.w - m.width) * 0.5f);
    else if (ha == CW_HALIGN_RIGHT) x = r.w - m.width;
  }
  if (l->vertical_alignment == CW_VALIGN_CENTER) y = ui_round((r.h - m.height) * 0.5f);
  else if (l->vertical_alignment == CW_VALIGN_BOTTOM) y = r.h - m.height;
  if (l->clip_text) cw_ui_push_clip(r);
  cw_vec2 o = ui_draw_origin();
  cw_color mc = col;
  (void)mc;
  if (st.wrap_width > 0 || st.align != CW_ALIGN_LEFT) {
    /* multi-line alignment needs the renderer's layout */
    st.color = col;
    cw_vec2 p = { o.x + x, o.y + y };
    /* modulate through the helper's colour path: draw via a 1-line helper when simple */
    ui_draw_text_styled(f, s, p, &st);
  } else {
    cw_ui_draw_text(f, fs, s, (cw_vec2){ x, y }, col);
  }
  if (l->clip_text) cw_ui_pop_clip();
}

static void label_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->mouse_filter = CW_MOUSE_IGNORE;
  CTL(n)->size_flags_vertical = CW_SIZE_SHRINK_CENTER;
}
static void label_changed(cw_node n, void *d, const cw_prop *p) {
  (void)p;
  ((label_data *)d)->last_w = 0;
  ui_mark_min_dirty(n);
}
void cw_label_set_text(cw_node n, const char *t) {
  label_data *l = cw_node_data_t(n, label_t);
  if (!l) return;
  /* counted, not pinned: per-frame text (a score, a timer) must not pile up */
  const char *it = cw_intern_ref(t ? t : "");
  if (it == l->text) {
    cw_intern_unref(it);
    return;
  }
  cw_intern_unref(l->text);
  l->text = it;
  l->last_w = 0;
  ui_mark_min_dirty(n);
}
const char *cw_label_text(cw_node n) {
  label_data *l = cw_node_data_t(n, label_t);
  return l ? ui_s(l->text) : "";
}

static const char *const halign_names[] = { "LEFT", "CENTER", "RIGHT", "FILL", NULL };
static const char *const valign_names[] = { "TOP", "CENTER", "BOTTOM", "FILL", NULL };
static const cw_prop label_props[] = {
  CW_PROP_FIELD(label_data, text, CW_T_STRING, CW_PROP_DEFAULT, "The text ('\\n' breaks lines)."),
  { .name = "horizontal_alignment", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT,
    .offset = (int)offsetof(label_data, horizontal_alignment), .size = 4, .enum_names = halign_names, .doc = "Horizontal alignment." },
  { .name = "vertical_alignment", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT,
    .offset = (int)offsetof(label_data, vertical_alignment), .size = 4, .enum_names = valign_names, .doc = "Vertical alignment." },
  CW_PROP_FIELD(label_data, autowrap, CW_T_BOOL, CW_PROP_DEFAULT, "Wrap at the width; minimum width becomes 0."),
  CW_PROP_FIELD(label_data, clip_text, CW_T_BOOL, CW_PROP_DEFAULT, "Clip to the rect; minimum width becomes 0."),
  CW_PROP_FIELD(label_data, ellipsis, CW_T_BOOL, CW_PROP_DEFAULT, "Cut with an ellipsis to the width."),
  CW_PROP_FIELD(label_data, uppercase, CW_T_BOOL, CW_PROP_DEFAULT, "Draw in capitals."),
  CW_PROP_END
};
static cw_node_type label_type = { .name = "Label", .parent = "Control", .data_size = sizeof(label_data),
                                   .props = label_props, .init = label_init, .prop_changed = label_changed,
                                   .doc = "Text; wraps, aligns, clips or ellipsizes." };

/* ── RichTextLabel ──────────────────────────────────────────────────── */

typedef struct {
  int start, len;   /* into the plain text */
  bool bold, italic;
  cw_color color;
  bool has_color;
  float size;       /* 0 = theme */
  int url;          /* index into urls, -1 none */
} rt_run;

typedef struct {
  float x, y, w, h, ascent;
  int run;
  int start, len;   /* into the plain text */
} rt_piece;

typedef struct {
  const char *text;
  bool fit_content;
  /* parsed */
  const char *parsed_of;
  char *plain;
  bool laid_rtl;
  CW_ARRAY(rt_run) runs;
  CW_ARRAY(char *) urls;
  /* laid out */
  CW_ARRAY(rt_piece) pieces;
  float laid_w, laid_h;
  int hover_url;
} rich_data;
static const cw_node_type *rich_t;

static bool parse_named_color(const char *s, cw_color *c) {
  static const struct { const char *n; uint32_t v; } names[] = {
    { "red", 0xff5c5cff }, { "green", 0x6fdc6fff }, { "blue", 0x5c9cffff }, { "yellow", 0xffd84dff },
    { "orange", 0xffa040ff }, { "purple", 0xc28cffff }, { "cyan", 0x5ce0f0ff }, { "white", 0xffffffff },
    { "black", 0x000000ff }, { "gray", 0x9aa0a8ff }, { "grey", 0x9aa0a8ff }, { "pink", 0xff8cc8ff } };
  for (size_t i = 0; i < CW_ARRAY_LEN(names); i++)
    if (!strcmp(s, names[i].n)) {
      *c = cw_hex(names[i].v);
      return true;
    }
  if (*s == '#') {
    char t[64];
    snprintf(t, sizeof t, "x.c = %s", s);
    /* reuse the theme colour reader via a throwaway theme would be heavy: parse inline */
    unsigned v = 0;
    int n = 0;
    for (const char *p = s + 1; *p && isxdigit((unsigned char)*p); p++, n++) v = v * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
    if (n == 6) *c = (cw_color){ (float)((v >> 16) & 255) / 255, (float)((v >> 8) & 255) / 255, (float)(v & 255) / 255, 1 };
    else if (n == 8) *c = cw_hex(v);
    else if (n == 3) *c = (cw_color){ (float)((v >> 8) & 15) / 15, (float)((v >> 4) & 15) / 15, (float)(v & 15) / 15, 1 };
    else return false;
    return true;
  }
  return false;
}

static void rich_parse(cw_node node, rich_data *r) {
  const char *shown = ui_tr(node, ui_s(r->text));
  if (r->parsed_of == shown && r->plain) return;
  r->parsed_of = shown;
  r->laid_w = -1;
  cw_free(r->plain);
  for (int i = 0; i < r->urls.count; i++) cw_free(r->urls.data[i]);
  cw_array_clear(&r->urls);
  cw_array_clear(&r->runs);
  const char *s = shown;
  size_t n = strlen(s);
  r->plain = cw_alloc(n + 1, CW_MEM_UI);
  int out = 0;
  /* style stack */
  struct { int b, i; cw_color col[16]; int ncol; float size[16]; int nsize; int url; } st = { 0 };
  st.url = -1;
  int run_start = 0;
  rt_run cur = { 0, 0, false, false, { 0 }, false, 0, -1 };
#define FLUSH() do { \
    if (out > run_start) { cur.start = run_start; cur.len = out - run_start; cw_array_push(&r->runs, cur); } \
    run_start = out; \
    cur.bold = st.b > 0; cur.italic = st.i > 0; \
    cur.has_color = st.ncol > 0; cur.color = st.ncol ? st.col[st.ncol - 1] : (cw_color){ 0 }; \
    cur.size = st.nsize ? st.size[st.nsize - 1] : 0; cur.url = st.url; \
  } while (0)
  FLUSH();
  for (size_t i = 0; i < n;) {
    if (s[i] == '[') {
      const char *e = strchr(s + i, ']');
      if (e) {
        char tag[256];
        snprintf(tag, sizeof tag, "%.*s", (int)(e - s - i - 1), s + i + 1);
        bool close = tag[0] == '/';
        const char *name = close ? tag + 1 : tag;
        char *eq = strchr(tag, '=');
        if (eq) *eq = 0;
        const char *arg = eq ? eq + 1 : "";
        bool known = true;
        if (!strcmp(name, "b")) st.b += close ? -1 : 1;
        else if (!strcmp(name, "i")) st.i += close ? -1 : 1;
        else if (!strcmp(name, "color")) {
          cw_color c;
          if (close) { if (st.ncol) st.ncol--; }
          else if (parse_named_color(arg, &c) && st.ncol < 16) st.col[st.ncol++] = c;
        } else if (!strcmp(name, "size")) {
          if (close) { if (st.nsize) st.nsize--; }
          else if (st.nsize < 16) st.size[st.nsize++] = (float)atof(arg);
        } else if (!strcmp(name, "url")) {
          if (close) st.url = -1;
          else {
            cw_array_push(&r->urls, cw_strdup(arg, CW_MEM_UI));
            st.url = r->urls.count - 1;
          }
        } else known = false;
        if (known) {
          if (st.b < 0) st.b = 0;
          if (st.i < 0) st.i = 0;
          FLUSH();
          i = (size_t)(e - s) + 1;
          continue;
        }
      }
    }
    r->plain[out++] = s[i++];
  }
  r->plain[out] = 0;
  FLUSH();
  /* [url] without an argument: the text is the meta */
  for (int k = 0; k < r->runs.count; k++) {
    rt_run *ru = &r->runs.data[k];
    if (ru->url >= 0 && !*r->urls.data[ru->url]) {
      cw_free(r->urls.data[ru->url]);
      char *m = cw_alloc((size_t)ru->len + 1, CW_MEM_UI);
      memcpy(m, r->plain + ru->start, (size_t)ru->len);
      m[ru->len] = 0;
      r->urls.data[ru->url] = m;
    }
  }
#undef FLUSH
  r->laid_w = -1;
}

static cw_font run_font(cw_node n, const rt_run *ru, bool *synth_b, bool *synth_i) {
  cw_font f = T_FONT(n);
  *synth_b = *synth_i = false;
  if (ru->bold && ru->italic) {
    cw_font bi = { 0 };
    if (ui_lookup(n, NULL, "bold_italics_font", TI_FONT, &bi) && !CW_HANDLE_IS_NULL(bi)) return bi;
  }
  if (ru->bold) {
    cw_font b = { 0 };
    if (ui_lookup(n, NULL, "bold_font", TI_FONT, &b) && !CW_HANDLE_IS_NULL(b)) f = b;
    else *synth_b = true;
  }
  if (ru->italic) {
    cw_font it = { 0 };
    if (!ru->bold && ui_lookup(n, NULL, "italics_font", TI_FONT, &it) && !CW_HANDLE_IS_NULL(it)) f = it;
    else *synth_i = true;
  }
  return f;
}

static float piece_width(cw_font f, float size, const char *s, int len, bool synth_b) {
  char *b = cw_frame_alloc((size_t)len + 1);
  memcpy(b, s, (size_t)len);
  b[len] = 0;
  /* caret x at the end keeps trailing spaces (measure trims them) */
  return cw_font_caret_x(f, b, len, size) + (synth_b ? CW_MAX(0.6f, size / 22.0f) : 0);
}

static void rich_layout_text(cw_node n, rich_data *r, float width) {
  rich_parse(n, r);
  bool rtl = cw_control_is_rtl(n);
  if (r->laid_w == width && r->laid_rtl == rtl) return;
  r->laid_w = width;
  r->laid_rtl = rtl;
  cw_array_clear(&r->pieces);
  float fs0 = T_FSIZE(n);
  float x = 0, y = 0, line_h = 0, line_asc = 0;
  int line_first = 0;
  const char *P = r->plain;
#define END_LINE() do { \
    if (line_h <= 0) { cw_font df = T_FONT(n); cw_text_metrics mm = cw_font_measure(df, "", &(cw_text_style){ .size = fs0 }); \
                       line_h = mm.line_height; line_asc = mm.ascent; } \
    for (int q = line_first; q < r->pieces.count; q++) { rt_piece *pc = &r->pieces.data[q]; pc->y = y + (line_asc - pc->ascent); } \
    y += line_h; x = 0; line_h = 0; line_asc = 0; line_first = r->pieces.count; \
  } while (0)
  for (int k = 0; k < r->runs.count; k++) {
    rt_run *ru = &r->runs.data[k];
    bool sb, si;
    cw_font f = run_font(n, ru, &sb, &si);
    float fs = ru->size > 0 ? ru->size : fs0;
    cw_text_metrics mm = cw_font_measure(f, "", &(cw_text_style){ .size = fs });
    int i = ru->start, end = ru->start + ru->len;
    while (i < end) {
      if (P[i] == '\n') {
        line_h = CW_MAX(line_h, mm.line_height);
        line_asc = CW_MAX(line_asc, mm.ascent);
        END_LINE();
        i++;
        continue;
      }
      /* next word (with its trailing spaces) */
      int j = i;
      while (j < end && P[j] != ' ' && P[j] != '\n') j++;
      while (j < end && P[j] == ' ') j++;
      int wl = j - i;
      int trimmed = wl;
      while (trimmed > 0 && P[i + trimmed - 1] == ' ') trimmed--;
      float w = piece_width(f, fs, P + i, wl, sb);
      float wt = piece_width(f, fs, P + i, trimmed, sb);
      if (x > 0 && x + wt > width && width > 0) END_LINE();
      /* merge with the previous piece of the same run on this line */
      rt_piece *last = r->pieces.count > line_first ? &r->pieces.data[r->pieces.count - 1] : NULL;
      if (last && last->run == k && last->start + last->len == i) {
        last->len += wl;
        last->w = piece_width(f, fs, P + last->start, last->len, sb);
        x = last->x + last->w;
      } else {
        rt_piece pc = { .x = x, .w = w, .h = mm.line_height, .ascent = mm.ascent, .run = k, .start = i, .len = wl };
        cw_array_push(&r->pieces, pc);
        x += w;
      }
      line_h = CW_MAX(line_h, mm.line_height);
      line_asc = CW_MAX(line_asc, mm.ascent);
      i = j;
    }
  }
  if (r->pieces.count > line_first || y == 0) END_LINE();
#undef END_LINE
  r->laid_h = y;
  /* right to left: each line's pieces run from the right edge (word order
   * of an RTL paragraph; LTR phrases inside it keep their own word order
   * only within one piece) */
  if (rtl)
    for (int q = 0; q < r->pieces.count; q++) r->pieces.data[q].x = width - r->pieces.data[q].x - r->pieces.data[q].w;
}

static cw_vec2 rich_min(cw_node n, void *d) {
  rich_data *r = d;
  if (!r->fit_content) return (cw_vec2){ 0, 0 };
  float w = CTL(n)->rect.w > 0 ? CTL(n)->rect.w : 1e6f;
  rich_layout_text(n, r, w);
  return (cw_vec2){ 0, r->laid_h };
}

static void rich_layout(cw_node n, void *d, cw_vec2 sz) {
  rich_data *r = d;
  float h0 = r->laid_h;
  rich_layout_text(n, r, sz.x);
  if (r->fit_content && r->laid_h != h0) ui_mark_min_dirty(n);
}

static void rich_draw(cw_node n, void *d) {
  rich_data *r = d;
  cw_control *c = CTL(n);
  rich_layout_text(n, r, c->rect.w);
  cw_color base = T_COLOR(n, "font_color"), link = T_COLOR(n, "link_color");
  float fs0 = T_FSIZE(n);
  cw_vec2 o = ui_draw_origin();
  for (int i = 0; i < r->pieces.count; i++) {
    rt_piece *pc = &r->pieces.data[i];
    rt_run *ru = &r->runs.data[pc->run];
    bool sb, si;
    cw_font f = run_font(n, ru, &sb, &si);
    float fs = ru->size > 0 ? ru->size : fs0;
    cw_color col = ru->has_color ? ru->color : ru->url >= 0 ? link : base;
    char *t = cw_frame_alloc((size_t)pc->len + 1);
    memcpy(t, r->plain + pc->start, (size_t)pc->len);
    t[pc->len] = 0;
    cw_vec2 p = { o.x + pc->x, o.y + pc->y };
    if (si) { /* synthetic italic: shear about the baseline */
      float base_y = p.y + pc->ascent, k = 0.2f;
      cw_xform2d m = { 1, 0, -k, 1, k * base_y, 0 };
      cw_r2d_set_transform(&m);
    }
    cw_text_style st = { .size = fs, .color = col };
    ui_draw_text_styled(f, t, p, &st);
    if (sb) ui_draw_text_styled(f, t, (cw_vec2){ p.x + CW_MAX(0.6f, fs / 22.0f), p.y }, &st);
    if (si) cw_r2d_set_transform(NULL);
    if (ru->url >= 0) {
      float uy = pc->y + pc->ascent + CW_MAX(1.0f, fs / 12.0f);
      float tw = pc->w;
      int tl = pc->len;
      while (tl > 0 && t[tl - 1] == ' ') tl--;
      if (tl != pc->len) tw = piece_width(f, fs, t, tl, sb);
      cw_ui_draw_rect((cw_rect){ pc->x, uy, tw, CW_MAX(1.0f, fs / 15.0f) }, col);
    }
  }
}

static bool rich_input(cw_node n, void *d, const cw_ui_event *e) {
  rich_data *r = d;
  (void)n;
  if (e->type != CW_UI_MOUSE_DOWN || e->button != 0) return false;
  for (int i = 0; i < r->pieces.count; i++) {
    rt_piece *pc = &r->pieces.data[i];
    rt_run *ru = &r->runs.data[pc->run];
    if (ru->url < 0) continue;
    if (rect_has((cw_rect){ pc->x, pc->y, pc->w, pc->h }, e->pos)) {
      ui_emit_str(n, "meta_clicked", r->urls.data[ru->url]);
      return true;
    }
  }
  return false;
}

static void rich_init(cw_node n, void *d) {
  rich_data *r = d;
  r->fit_content = true;
  r->laid_w = -1;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
}
static void rich_destroy(cw_node n, void *d) {
  (void)n;
  rich_data *r = d;
  cw_free(r->plain);
  for (int i = 0; i < r->urls.count; i++) cw_free(r->urls.data[i]);
  cw_array_free(&r->urls);
  cw_array_free(&r->runs);
  cw_array_free(&r->pieces);
}
static void rich_changed(cw_node n, void *d, const cw_prop *p) {
  (void)p;
  rich_data *r = d;
  r->laid_w = -1;
  ui_mark_min_dirty(n);
}
static const cw_prop rich_props[] = {
  CW_PROP_FIELD(rich_data, text, CW_T_STRING, CW_PROP_DEFAULT, "BBCode: [b] [i] [color=#hex|name] [size=N] [url=meta]."),
  CW_PROP_FIELD(rich_data, fit_content, CW_T_BOOL, CW_PROP_DEFAULT, "Minimum height = the laid-out text."),
  CW_PROP_END
};
static const char *const rich_signals[] = { "meta_clicked", NULL };
static cw_node_type rich_type = { .name = "RichTextLabel", .parent = "Control", .data_size = sizeof(rich_data),
                                  .props = rich_props, .signals = rich_signals, .init = rich_init, .destroy = rich_destroy,
                                  .prop_changed = rich_changed, .doc = "Wrapped text with a small BBCode subset." };

void ui_register_labels(void) {
  cw_node_type_register(&label_type);
  label_t = &label_type;
  cw_ui_class_register("Label", &(cw_ui_class){ .min_size = label_min, .layout = label_layout, .draw = label_draw });
  cw_node_type_register(&rich_type);
  rich_t = &rich_type;
  cw_ui_class_register("RichTextLabel", &(cw_ui_class){ .min_size = rich_min, .layout = rich_layout, .draw = rich_draw,
                                                        .input = rich_input });
}

/* Themes: items by (theme type, name), the text format, the built-in dark and
 * light themes, and lookups for a control through overrides, the nearest
 * control theme, the project theme and the default. */
#include "ui_internal.h"
#include "cartwheel/vfs.h"

typedef struct titem {
  uint8_t kind;
  const char *type, *name; /* interned */
  union {
    cw_color c;
    float k;
    cw_font f;
    cw_ui_style s;
  } v;
  const char *font_path; /* how a font was named in text (for save) */
} titem;

typedef struct theme_t {
  CW_ARRAY(titem) items;
  cw_map idx;
  cw_theme base;
  bool builtin;
} theme_t;

static cw_pool themes;
static bool themes_ready;
static cw_theme dark, light, high_contrast, colorblind;
static cw_map theme_cache; /* asset path (interned) -> theme id */
static cw_map font_cache;  /* asset path (interned) -> font id */

static theme_t *TH(cw_theme t) { return themes_ready ? cw_pool_get(&themes, t.id) : NULL; }

static void ensure_pool(void) {
  if (themes_ready) return;
  themes_ready = true;
  cw_pool_init(&themes, sizeof(theme_t), CW_MEM_UI);
  cw_map_init(&theme_cache, CW_MEM_UI);
  cw_map_init(&font_cache, CW_MEM_UI);
}

static uint64_t ikey(const char *type, const char *name, int kind) {
  return ((uint64_t)(uintptr_t)type * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)name * 0xC2B2AE3D27D4EB4Full) ^
         (uint64_t)kind;
}

cw_theme cw_theme_new(cw_theme base) {
  ensure_pool();
  uint64_t id;
  theme_t *t = cw_pool_alloc(&themes, &id);
  cw_map_init(&t->idx, CW_MEM_UI);
  t->base = base;
  return (cw_theme){ id };
}

void cw_theme_destroy(cw_theme h) {
  theme_t *t = TH(h);
  if (!t || t->builtin) return;
  cw_array_free(&t->items);
  cw_map_free(&t->idx);
  cw_pool_release(&themes, h.id);
  UI.theme_epoch++;
}

static titem *put(cw_theme h, const char *type, const char *name, int kind) {
  theme_t *t = TH(h);
  if (!t) return NULL;
  type = cw_intern(type ? type : "");
  name = cw_intern(name);
  uint64_t k = ikey(type, name, kind), v;
  UI.theme_epoch++;
  if (cw_map_get(&t->idx, k, &v)) return &t->items.data[v];
  titem it = { .kind = (uint8_t)kind, .type = type, .name = name };
  cw_array_push(&t->items, it);
  cw_map_set(&t->idx, k, (uint64_t)(t->items.count - 1));
  return &t->items.data[t->items.count - 1];
}

void cw_theme_set_color(cw_theme t, const char *type, const char *name, cw_color c) {
  titem *i = put(t, type, name, TI_COLOR);
  if (i) i->v.c = c;
}
void cw_theme_set_constant(cw_theme t, const char *type, const char *name, float v) {
  titem *i = put(t, type, name, TI_CONST);
  if (i) i->v.k = v;
}
void cw_theme_set_font(cw_theme t, const char *type, const char *name, cw_font f) {
  titem *i = put(t, type, name, TI_FONT);
  if (i) i->v.f = f;
}
void cw_theme_set_style(cw_theme t, const char *type, const char *name, const cw_ui_style *s) {
  titem *i = put(t, type, name, TI_STYLE);
  if (i) i->v.s = *s;
}

/* lookup in one theme chain with interned type/name */
static const titem *find(cw_theme h, const char *type, const char *name, int kind) {
  for (int guard = 0; guard < 16; guard++) {
    theme_t *t = TH(h);
    if (!t) return NULL;
    uint64_t v;
    if (cw_map_get(&t->idx, ikey(type, name, kind), &v)) return &t->items.data[v];
    h = t->base;
  }
  return NULL;
}

static bool get_any(cw_theme t, const char *type, const char *name, int kind, void *out) {
  const titem *i = find(t, cw_intern(type ? type : ""), cw_intern(name), kind);
  if (!i) return false;
  switch (kind) {
  case TI_COLOR: *(cw_color *)out = i->v.c; break;
  case TI_CONST: *(float *)out = i->v.k; break;
  case TI_FONT: *(cw_font *)out = i->v.f; break;
  default: *(cw_ui_style *)out = i->v.s; break;
  }
  return true;
}
bool cw_theme_get_color(cw_theme t, const char *ty, const char *n, cw_color *o) { return get_any(t, ty, n, TI_COLOR, o); }
bool cw_theme_get_constant(cw_theme t, const char *ty, const char *n, float *o) { return get_any(t, ty, n, TI_CONST, o); }
bool cw_theme_get_font(cw_theme t, const char *ty, const char *n, cw_font *o) { return get_any(t, ty, n, TI_FONT, o); }
bool cw_theme_get_style(cw_theme t, const char *ty, const char *n, cw_ui_style *o) { return get_any(t, ty, n, TI_STYLE, o); }

cw_ui_style cw_ui_style_flat(cw_color bg, float radius, float mh, float mv) {
  cw_ui_style s = { .draw = true, .draw_center = true, .bg = bg };
  for (int i = 0; i < 4; i++) s.radius[i] = radius;
  s.margin[0] = s.margin[2] = mh;
  s.margin[1] = s.margin[3] = mv;
  return s;
}

/* ── text format ────────────────────────────────────────────────────── */

static const char *skip(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static bool parse_color(const char *s, cw_color *out, const char **end) {
  s = skip(s);
  if (*s == '#') {
    const char *p = s + 1;
    int n = 0;
    while (isxdigit((unsigned char)p[n])) n++;
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
      char c = p[i];
      v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    float r, g, b, a = 1;
    if (n == 3) r = (float)((v >> 8) & 15) / 15, g = (float)((v >> 4) & 15) / 15, b = (float)(v & 15) / 15;
    else if (n == 4) r = (float)((v >> 12) & 15) / 15, g = (float)((v >> 8) & 15) / 15, b = (float)((v >> 4) & 15) / 15, a = (float)(v & 15) / 15;
    else if (n == 6) r = (float)((v >> 16) & 255) / 255, g = (float)((v >> 8) & 255) / 255, b = (float)(v & 255) / 255;
    else if (n == 8) r = (float)(v >> 24) / 255, g = (float)((v >> 16) & 255) / 255, b = (float)((v >> 8) & 255) / 255, a = (float)(v & 255) / 255;
    else return false;
    *out = (cw_color){ r, g, b, a };
    if (end) *end = p + n;
    return true;
  }
  if (!strncmp(s, "color(", 6)) {
    cw_value v;
    if (!cw_value_parse(s, CW_T_COLOR, &v)) return false;
    *out = (cw_color){ v.v[0], v.v[1], v.v[2], v.v[3] };
    if (end) *end = strchr(s, ')') ? strchr(s, ')') + 1 : s + strlen(s);
    return true;
  }
  return false;
}

/* up to 4 numbers separated by spaces/commas */
static int parse_nums(const char *s, float *out, int cap, const char **end) {
  int n = 0;
  for (;;) {
    s = skip(s);
    char *e;
    double d = strtod(s, &e);
    if (e == s || n >= cap) break;
    out[n++] = (float)d;
    s = skip(e);
    if (*s == ',' && n < cap) {
      /* a comma may also end the field: only continue when a number follows */
      const char *q = skip(s + 1);
      if ((*q >= '0' && *q <= '9') || *q == '-' || *q == '.') { s = q; continue; }
      break;
    }
    if ((*s >= '0' && *s <= '9') || *s == '-' || *s == '.') continue; /* space-separated */
    break;
  }
  if (end) *end = s;
  return n;
}

static bool parse_style(cw_theme into, const char *s, cw_ui_style *st, const char *ctx) {
  s = skip(s);
  memset(st, 0, sizeof *st);
  if (!strncmp(s, "empty", 5)) return true;
  if (strncmp(s, "style(", 6)) return false;
  st->draw = true;
  st->draw_center = true;
  s += 6;
  for (;;) {
    s = skip(s);
    if (*s == ')' || !*s) break;
    const char *k = s;
    while (*s && *s != '=' && *s != ')' && *s != ',') s++;
    if (*s != '=') return false;
    int kl = (int)(s - k);
    while (kl > 0 && (k[kl - 1] == ' ')) kl--;
    s++;
    float nums[4];
    int n;
    const char *e = s;
#define KEY(x) (kl == (int)strlen(x) && !strncmp(k, x, (size_t)kl))
    if (KEY("bg")) {
      if (!parse_color(s, &st->bg, &e)) return false;
    } else if (KEY("border_color")) {
      if (!parse_color(s, &st->border_color, &e)) return false;
    } else if (KEY("shadow")) {
      if (!parse_color(s, &st->shadow, &e)) return false;
    } else if (KEY("base")) {
      const char *b = skip(s);
      e = b;
      while (*e && *e != ',' && *e != ')' && *e != ' ') e++;
      char ref[128];
      snprintf(ref, sizeof ref, "%.*s", (int)(e - b), b);
      char *dot = strrchr(ref, '.');
      if (!dot) return false;
      *dot = 0;
      cw_ui_style base;
      if (!cw_theme_get_style(into, ref, dot + 1, &base)) {
        cw_warn("theme %s: style base '%s.%s' is not defined above", ctx, ref, dot + 1);
        return false;
      }
      *st = base;
    } else if (KEY("border")) {
      n = parse_nums(s, nums, 4, &e);
      if (n == 1) for (int i = 0; i < 4; i++) st->border[i] = nums[0];
      else if (n == 4) memcpy(st->border, nums, sizeof nums);
      else return false;
    } else if (KEY("radius")) {
      n = parse_nums(s, nums, 4, &e);
      if (n == 1) for (int i = 0; i < 4; i++) st->radius[i] = nums[0];
      else if (n == 4) memcpy(st->radius, nums, sizeof nums);
      else return false;
    } else if (KEY("margin")) {
      n = parse_nums(s, nums, 4, &e);
      if (n == 1) for (int i = 0; i < 4; i++) st->margin[i] = nums[0];
      else if (n == 2) st->margin[0] = st->margin[2] = nums[0], st->margin[1] = st->margin[3] = nums[1];
      else if (n == 4) memcpy(st->margin, nums, sizeof nums);
      else return false;
    } else if (KEY("shadow_size")) {
      if (parse_nums(s, nums, 1, &e) != 1) return false;
      st->shadow_size = nums[0];
    } else if (KEY("shadow_offset")) {
      if (parse_nums(s, nums, 2, &e) != 2) return false;
      st->shadow_offset = (cw_vec2){ nums[0], nums[1] };
    } else if (KEY("draw_center")) {
      if (parse_nums(s, nums, 1, &e) != 1) return false;
      st->draw_center = nums[0] != 0;
    } else {
      cw_warn("theme %s: unknown style field '%.*s' (bg border border_color radius margin shadow shadow_size "
              "shadow_offset draw_center base)", ctx, kl, k);
      return false;
    }
#undef KEY
    s = skip(e);
    if (*s == ',') s++;
  }
  return true;
}

cw_font ui_font_load_cached(const char *path) {
  ensure_pool();
  if (!strcmp(path, "builtin:sans")) return cw_ui_font_sans();
  if (!strcmp(path, "builtin:mono")) return cw_ui_font_mono();
  const char *k = cw_intern(path);
  uint64_t v;
  if (cw_map_get(&font_cache, (uint64_t)(uintptr_t)k, &v)) return (cw_font){ v };
  cw_font f = cw_font_load(path, &(cw_font_desc){ .mode = CW_FONT_SDF });
  cw_map_set(&font_cache, (uint64_t)(uintptr_t)k, f.id);
  return f;
}

/* "Type.name = value" (type may be empty: ".name"); bare top-level keys
 * (default_font, default_font_size) go to type "". */
static bool set_line(cw_theme t, const char *line, const char *ctx) {
  const char *eq = strchr(line, '=');
  if (!eq) return false;
  char key[160];
  int kl = (int)(eq - line);
  while (kl > 0 && (line[kl - 1] == ' ' || line[kl - 1] == '\t')) kl--;
  const char *ks = skip(line);
  kl -= (int)(ks - line);
  if (kl <= 0 || kl >= (int)sizeof key) return false;
  memcpy(key, ks, (size_t)kl);
  key[kl] = 0;
  const char *val = skip(eq + 1);
  char *dot = strrchr(key, '.');
  const char *type = "", *name = key;
  if (dot) {
    *dot = 0;
    type = key;
    name = dot + 1;
  }
  if (!strcmp(name, "extends") && !*type) {
    theme_t *th = TH(t);
    if (!strncmp(val, "light", 5)) th->base = cw_theme_default_light();
    else if (!strncmp(val, "high_contrast", 13)) th->base = cw_theme_high_contrast();
    else if (!strncmp(val, "colorblind", 10)) th->base = cw_theme_colorblind();
    else if (!strncmp(val, "none", 4)) th->base = (cw_theme){ 0 };
    else th->base = cw_theme_default_dark();
    return true;
  }
  cw_color c;
  cw_ui_style st;
  if (*val == '#' || !strncmp(val, "color(", 6)) {
    if (!parse_color(val, &c, NULL)) return false;
    cw_theme_set_color(t, type, name, c);
  } else if (!strncmp(val, "style(", 6) || !strncmp(val, "empty", 5)) {
    if (!parse_style(t, val, &st, ctx)) return false;
    cw_theme_set_style(t, type, name, &st);
  } else if (!strncmp(val, "font(", 5) || !strncmp(val, "builtin:", 8) || strstr(val, ".ttf") || strstr(val, ".otf")) {
    char path[256];
    const char *p = val;
    if (!strncmp(p, "font(", 5)) p += 5;
    p = skip(p);
    if (*p == '"') p++;
    int n = 0;
    while (p[n] && p[n] != '"' && p[n] != ')' && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && n < 255) n++;
    snprintf(path, sizeof path, "%.*s", n, p);
    cw_font f = ui_font_load_cached(path);
    if (CW_HANDLE_IS_NULL(f)) return false;
    titem *i = put(t, type, name, TI_FONT);
    i->v.f = f;
    i->font_path = cw_intern(path);
  } else {
    float k;
    if (parse_nums(val, &k, 1, NULL) != 1) return false;
    cw_theme_set_constant(t, type, name, k);
  }
  return true;
}

bool cw_theme_set_text(cw_theme t, const char *line) { return TH(t) && set_line(t, line, "(set_text)"); }

cw_theme cw_theme_parse(const char *text, const char *name) {
  cw_theme t = cw_theme_new(cw_theme_default_dark());
  int ln = 0;
  bool ok = true;
  for (const char *p = text; p && *p;) {
    const char *e = strchr(p, '\n');
    int len = e ? (int)(e - p) : (int)strlen(p);
    ln++;
    char line[1024];
    snprintf(line, sizeof line, "%.*s", len, p);
    /* '#' starts a comment unless it is a colour (follows '=' or '(' or ',') */
    for (char *q = line; *q; q++)
      if (*q == '#') {
        char *b = q;
        while (b > line && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (b == line || (b[-1] != '=' && b[-1] != '(' && b[-1] != ',')) { *q = 0; break; }
      }
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    size_t sl = strlen(s);
    while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '\r' || s[sl - 1] == '\t')) s[--sl] = 0;
    if (*s) {
      char ctx[160];
      snprintf(ctx, sizeof ctx, "%s:%d", name ? name : "?", ln);
      if (!set_line(t, s, ctx)) {
        cw_error("theme %s:%d: cannot read '%s' (want Type.name = #color | number | style(...) | empty | font path)",
                 name ? name : "?", ln, s);
        ok = false;
      }
    }
    p = e ? e + 1 : NULL;
  }
  if (!ok) {
    cw_theme_destroy(t);
    return (cw_theme){ 0 };
  }
  return t;
}

cw_theme cw_theme_load(const char *path) {
  ensure_pool();
  const char *k = cw_intern(path);
  uint64_t v;
  if (cw_map_get(&theme_cache, (uint64_t)(uintptr_t)k, &v) && TH((cw_theme){ v })) return (cw_theme){ v };
  size_t len;
  char *txt = cw_vfs_read(path, &len) /* the project layer first (an editor on a project), then the assets */;
  if (!txt) {
    cw_error("theme '%s': asset not found", path);
    return (cw_theme){ 0 };
  }
  cw_theme t = cw_theme_parse(txt, path);
  free(txt);
  if (!CW_HANDLE_IS_NULL(t)) cw_map_set(&theme_cache, (uint64_t)(uintptr_t)k, t.id);
  return t;
}

static int fmt_color(char *b, int cap, cw_color c) {
  int r = (int)lroundf(c.r * 255), g = (int)lroundf(c.g * 255), bl = (int)lroundf(c.b * 255), a = (int)lroundf(c.a * 255);
  if (a == 255) return snprintf(b, (size_t)cap, "#%02x%02x%02x", r, g, bl);
  return snprintf(b, (size_t)cap, "#%02x%02x%02x%02x", r, g, bl, a);
}

static int fmt_nums(char *b, int cap, const float *v, int n) {
  bool same = true;
  for (int i = 1; i < n; i++) same &= v[i] == v[0];
  if (same) return snprintf(b, (size_t)cap, "%g", (double)v[0]);
  int k = 0;
  for (int i = 0; i < n; i++) k += snprintf(b + k, (size_t)(cap > k ? cap - k : 0), i ? " %g" : "%g", (double)v[i]);
  return k;
}

char *cw_theme_save(cw_theme h) {
  theme_t *t = TH(h);
  if (!t) return NULL;
  size_t cap = 256 + (size_t)t->items.count * 256, n = 0;
  char *out = cw_alloc(cap, CW_MEM_UI);
  out[0] = 0;
#define OUT(...) (n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__))
  OUT("# cartwheel UI theme\n");
  if (CW_HANDLE_IS_NULL(t->base)) OUT("extends = none\n");
  else if (t->base.id == light.id) OUT("extends = light\n");
  else if (t->base.id == high_contrast.id) OUT("extends = high_contrast\n");
  else if (t->base.id == colorblind.id) OUT("extends = colorblind\n");
  else OUT("extends = dark\n");
  for (int i = 0; i < t->items.count; i++) {
    const titem *it = &t->items.data[i];
    char v[400];
    int k = 0;
    switch (it->kind) {
    case TI_COLOR: fmt_color(v, sizeof v, it->v.c); break;
    case TI_CONST: snprintf(v, sizeof v, "%g", (double)it->v.k); break;
    case TI_FONT: snprintf(v, sizeof v, "%s", it->font_path ? it->font_path : "builtin:sans"); break;
    default: {
      const cw_ui_style *s = &it->v.s;
      if (!s->draw) {
        snprintf(v, sizeof v, "empty");
        break;
      }
      k += snprintf(v + k, sizeof v - (size_t)k, "style(bg=");
      k += fmt_color(v + k, (int)sizeof v - k, s->bg);
      float zero[4] = { 0 };
      if (memcmp(s->border, zero, sizeof zero)) {
        k += snprintf(v + k, sizeof v - (size_t)k, ", border=");
        k += fmt_nums(v + k, (int)sizeof v - k, s->border, 4);
        k += snprintf(v + k, sizeof v - (size_t)k, ", border_color=");
        k += fmt_color(v + k, (int)sizeof v - k, s->border_color);
      }
      if (memcmp(s->radius, zero, sizeof zero)) {
        k += snprintf(v + k, sizeof v - (size_t)k, ", radius=");
        k += fmt_nums(v + k, (int)sizeof v - k, s->radius, 4);
      }
      if (memcmp(s->margin, zero, sizeof zero)) {
        k += snprintf(v + k, sizeof v - (size_t)k, ", margin=");
        k += fmt_nums(v + k, (int)sizeof v - k, s->margin, 4);
      }
      if (s->shadow.a > 0) {
        k += snprintf(v + k, sizeof v - (size_t)k, ", shadow=");
        k += fmt_color(v + k, (int)sizeof v - k, s->shadow);
        k += snprintf(v + k, sizeof v - (size_t)k, ", shadow_size=%g, shadow_offset=%g %g", (double)s->shadow_size,
                      (double)s->shadow_offset.x, (double)s->shadow_offset.y);
      }
      if (!s->draw_center) k += snprintf(v + k, sizeof v - (size_t)k, ", draw_center=0");
      snprintf(v + k, sizeof v - (size_t)k, ")");
      break;
    }
    }
    if (n + 600 > cap) {
      cap *= 2;
      out = cw_realloc(out, cap, CW_MEM_UI);
    }
    if (*it->type) OUT("%s.%s = %s\n", it->type, it->name, v);
    else OUT("%s = %s\n", it->name, v);
  }
#undef OUT
  return out;
}

/* ── built-in themes ────────────────────────────────────────────────── */

/* The template is the theme text format with {tokens} filled from a
 * palette, so the built-ins are exactly what anyone could write. */
static const char *TEMPLATE =
    "default_font = builtin:sans\n"
    "default_font_size = 15\n"
    "font_color = {text}\n"
    "focus = style(bg=#00000000, border=2, border_color={accent}, radius=6, draw_center=0)\n"
    "Label.font_color = {text}\n"
    "Panel.panel = style(bg={bg0})\n"
    "PanelContainer.panel = style(bg={bg1}, border=1, border_color={border}, radius=6, margin=10)\n"
    "Button.normal = style(bg={btn}, border=1, border_color={border}, radius=5, margin=12 6)\n"
    "Button.hover = style(base=Button.normal, bg={btn_hover}, border_color={border_hover})\n"
    "Button.pressed = style(base=Button.normal, bg={btn_pressed}, border_color={accent})\n"
    "Button.disabled = style(base=Button.normal, bg={btn_disabled}, border_color={border_disabled})\n"
    "Button.focus = style(bg=#00000000, border=2, border_color={accent}, radius=6, draw_center=0)\n"
    "Button.font_color = {text}\n"
    "Button.font_hover_color = {text_strong}\n"
    "Button.font_pressed_color = {text_strong}\n"
    "Button.font_disabled_color = {text_disabled}\n"
    "Button.h_separation = 8\n"
    "CheckBox.normal = style(bg=#00000000, radius=4, margin=4 3)\n"
    "CheckBox.hover = style(bg={hover_tint}, radius=4, margin=4 3)\n"
    "CheckBox.pressed = style(bg={hover_tint}, radius=4, margin=4 3)\n"
    "CheckBox.disabled = style(bg=#00000000, radius=4, margin=4 3)\n"
    "CheckBox.focus = style(bg=#00000000, border=2, border_color={accent}, radius=5, draw_center=0)\n"
    "CheckBox.box = style(bg={field}, border=1, border_color={border_strong}, radius=4)\n"
    "CheckBox.box_checked = style(bg={accent}, border=1, border_color={accent}, radius=4)\n"
    "CheckBox.check_color = {on_accent}\n"
    "CheckBox.check_size = 16\n"
    "CheckButton.normal = style(bg=#00000000, radius=4, margin=4 3)\n"
    "CheckButton.hover = style(bg={hover_tint}, radius=4, margin=4 3)\n"
    "CheckButton.pressed = style(bg={hover_tint}, radius=4, margin=4 3)\n"
    "CheckButton.disabled = style(bg=#00000000, radius=4, margin=4 3)\n"
    "CheckButton.focus = style(bg=#00000000, border=2, border_color={accent}, radius=5, draw_center=0)\n"
    "CheckButton.track = style(bg={field}, border=1, border_color={border_strong}, radius=9)\n"
    "CheckButton.track_on = style(bg={accent}, border=1, border_color={accent}, radius=9)\n"
    "CheckButton.knob_color = {knob}\n"
    "CheckButton.switch_width = 34\n"
    "CheckButton.switch_height = 18\n"
    "OptionButton.arrow_color = {text_dim}\n"
    "LineEdit.normal = style(bg={field}, border=1, border_color={border}, radius=5, margin=8 5)\n"
    "LineEdit.focus = style(bg=#00000000, border=2, border_color={accent}, radius=5, draw_center=0)\n"
    "LineEdit.read_only = style(base=LineEdit.normal, bg={bg1})\n"
    "LineEdit.font_color = {text}\n"
    "LineEdit.font_placeholder_color = {text_dim}\n"
    "LineEdit.caret_color = {caret}\n"
    "LineEdit.selection_color = {selection}\n"
    "LineEdit.minimum_character_width = 4\n"
    "SpinBox.arrow_color = {text_dim}\n"
    "SpinBox.buttons_width = 18\n"
    "TextEdit.normal = style(bg={field_code}, border=1, border_color={border}, radius=5, margin=6 4)\n"
    "TextEdit.focus = style(bg=#00000000, border=2, border_color={accent}, radius=5, draw_center=0)\n"
    "TextEdit.font = builtin:mono\n"
    "TextEdit.font_size = 14\n"
    "TextEdit.font_color = {text}\n"
    "TextEdit.line_number_color = {text_dim}\n"
    "TextEdit.current_line_color = {current_line}\n"
    "TextEdit.caret_color = {caret}\n"
    "TextEdit.selection_color = {selection}\n"
    "TextEdit.keyword_color = {syn_keyword}\n"
    "TextEdit.type_color = {syn_type}\n"
    "TextEdit.string_color = {syn_string}\n"
    "TextEdit.number_color = {syn_number}\n"
    "TextEdit.comment_color = {syn_comment}\n"
    "TextEdit.preprocessor_color = {syn_pre}\n"
    "TextEdit.gutter_separation = 10\n"
    "ItemList.panel = style(bg={field}, border=1, border_color={border}, radius=5, margin=4)\n"
    "ItemList.item = style(bg=#00000000, margin=8 4)\n"
    "ItemList.selected = style(bg={selection_strong}, radius=4)\n"
    "ItemList.hovered = style(bg={hover_tint}, radius=4)\n"
    "ItemList.font_selected_color = {text_strong}\n"
    "ItemList.v_separation = 1\n"
    "ItemList.h_separation = 6\n"
    "Tree.panel = style(bg={field}, border=1, border_color={border}, radius=5, margin=4)\n"
    "Tree.item = style(bg=#00000000, margin=4 3)\n"
    "Tree.selected = style(bg={selection_strong}, radius=3)\n"
    "Tree.hovered = style(bg={hover_tint}, radius=3)\n"
    "Tree.title_button = style(bg={bg2}, border=0 0 0 1, border_color={border}, margin=8 4)\n"
    "Tree.title_font_color = {text_dim}\n"
    "Tree.font_selected_color = {text_strong}\n"
    "Tree.arrow_color = {text_dim}\n"
    "Tree.guide_color = {guide}\n"
    "Tree.item_margin = 16\n"
    "Tree.v_separation = 1\n"
    "Tree.column_separation = 1\n"
    "ScrollBar.scroll = style(bg=#00000000, radius=5, margin=2)\n"
    "ScrollBar.grabber = style(bg={grabber}, radius=3)\n"
    "ScrollBar.grabber_hover = style(bg={grabber_hover}, radius=3)\n"
    "ScrollBar.grabber_pressed = style(bg={accent}, radius=3)\n"
    "ScrollBar.thickness = 10\n"
    "ScrollBar.minimum_grabber = 20\n"
    "Slider.slider = style(bg={track}, radius=3, margin=0 3)\n"
    "Slider.grabber_area = style(bg={accent}, radius=3, margin=0 3)\n"
    "Slider.grabber_color = {knob}\n"
    "Slider.grabber_border_color = {accent}\n"
    "Slider.grabber_radius = 8\n"
    "Slider.tick_color = {border_strong}\n"
    "ProgressBar.background = style(bg={track}, radius=5, margin=0 3)\n"
    "ProgressBar.fill = style(bg={accent}, radius=5)\n"
    "ProgressBar.font_color = {text_strong}\n"
    "ProgressBar.font_size = 13\n"
    "TabContainer.panel = style(bg={bg1}, border=1, border_color={border}, radius=0 6 6 6, margin=10)\n"
    "TabContainer.tab_selected = style(bg={bg1}, border=1 2 1 0, border_color={accent_tab}, radius=6 6 0 0, margin=14 6)\n"
    "TabContainer.tab_unselected = style(bg={bg2}, radius=6 6 0 0, margin=14 6)\n"
    "TabContainer.tab_hovered = style(bg={btn_hover}, radius=6 6 0 0, margin=14 6)\n"
    "TabContainer.font_selected_color = {text_strong}\n"
    "TabContainer.font_unselected_color = {text_dim}\n"
    "TabContainer.font_hovered_color = {text}\n"
    "TabContainer.tab_separation = 2\n"
    "SplitContainer.separation = 8\n"
    "SplitContainer.grabber_color = {border_strong}\n"
    "SplitContainer.grabber_hover_color = {accent}\n"
    "BoxContainer.separation = 6\n"
    "GridContainer.h_separation = 6\n"
    "GridContainer.v_separation = 6\n"
    "FlowContainer.h_separation = 6\n"
    "FlowContainer.v_separation = 6\n"
    "PopupMenu.panel = style(bg={popup}, border=1, border_color={border_strong}, radius=6, margin=4, shadow=#00000066, shadow_size=10, shadow_offset=0 3)\n"
    "PopupMenu.item = style(bg=#00000000, margin=10 4)\n"
    "PopupMenu.hover = style(bg={accent}, radius=4)\n"
    "PopupMenu.font_color = {text}\n"
    "PopupMenu.font_hover_color = {on_accent}\n"
    "PopupMenu.font_disabled_color = {text_disabled}\n"
    "PopupMenu.separator_color = {border}\n"
    "PopupMenu.check_color = {text}\n"
    "PopupMenu.font_shortcut_color = {text_dim}\n"
    "PopupPanel.panel = style(bg={popup}, border=1, border_color={border_strong}, radius=6, margin=6, shadow=#00000066, shadow_size=10, shadow_offset=0 3)\n"
    "DragPreview.panel = style(bg={popup}, border=1, border_color={accent}, radius=4, margin=8 4, shadow=#00000050, shadow_size=6, shadow_offset=0 2)\n"
    "DragPreview.font_color = {text}\n"
    "DragPreview.font_size = 13\n"
    "DropTarget.highlight = style(bg={selection}, border=2, border_color={accent}, radius=4)\n"
    "PopupMenu.h_separation = 8\n"
    "MenuBar.normal = style(bg=#00000000, radius=4, margin=10 4)\n"
    "MenuBar.hover = style(bg={hover_tint}, radius=4, margin=10 4)\n"
    "MenuBar.pressed = style(bg={accent}, radius=4, margin=10 4)\n"
    "MenuBar.font_color = {text}\n"
    "MenuBar.font_pressed_color = {on_accent}\n"
    "AcceptDialog.panel = style(bg={bg1}, border=1, border_color={border_strong}, radius=8, margin=16 12, shadow=#00000088, shadow_size=18, shadow_offset=0 6)\n"
    "AcceptDialog.title_font_size = 17\n"
    "AcceptDialog.title_color = {text_strong}\n"
    "AcceptDialog.dim_color = {dim}\n"
    "AcceptDialog.separation = 12\n"
    "TooltipPanel.panel = style(bg={tooltip}, border=1, border_color={border_strong}, radius=4, margin=8 4, shadow=#00000050, shadow_size=6, shadow_offset=0 2)\n"
    "TooltipLabel.font_color = {text}\n"
    "TooltipLabel.font_size = 13\n"
    "ColorPicker.sv_size = 160\n"
    "ColorPicker.strip_width = 16\n"
    "ColorPicker.separation = 8\n"
    "ColorPicker.swatch = style(bg=#00000000, border=1, border_color={border_strong}, radius=4)\n"
    "ColorPicker.cursor_color = #ffffff\n"
    "RichTextLabel.font_color = {text}\n"
    "RichTextLabel.link_color = {link}\n";

typedef struct { const char *k, *v; } pal;

static const pal DARK[] = {
  { "text", "#e3e6ed" }, { "text_strong", "#ffffff" }, { "text_dim", "#949cab" }, { "text_disabled", "#6a717d" },
  { "bg0", "#1b1e24" }, { "bg1", "#23272f" }, { "bg2", "#2b3039" }, { "btn", "#323844" }, { "btn_hover", "#3b4350" },
  { "btn_pressed", "#272c35" }, { "btn_disabled", "#2a2f38" }, { "border", "#3a404c" }, { "border_hover", "#4b5361" },
  { "border_strong", "#535c6c" }, { "border_disabled", "#30353f" }, { "accent", "#4c8dff" }, { "accent_tab", "#4c8dff" },
  { "on_accent", "#ffffff" }, { "hover_tint", "#ffffff14" }, { "field", "#191c22" }, { "field_code", "#16191e" },
  { "caret", "#e3e6ed" }, { "selection", "#4c8dff66" }, { "selection_strong", "#4c8dff59" },
  { "current_line", "#ffffff0b" }, { "knob", "#f2f4f8" }, { "track", "#14171c" }, { "grabber", "#ffffff2e" },
  { "grabber_hover", "#ffffff50" }, { "guide", "#ffffff1c" }, { "popup", "#262a33" }, { "tooltip", "#2c313b" },
  { "dim", "#00000080" }, { "link", "#6fa6ff" }, { "syn_keyword", "#ff7a93" }, { "syn_type", "#5fd4e8" },
  { "syn_string", "#a6dd84" }, { "syn_number", "#f7b267" }, { "syn_comment", "#6f7888" }, { "syn_pre", "#c792ea" },
  { NULL, NULL } };

static const pal LIGHT[] = {
  { "text", "#1d2129" }, { "text_strong", "#0b0d11" }, { "text_dim", "#5d6573" }, { "text_disabled", "#a0a6b0" },
  { "bg0", "#eef0f4" }, { "bg1", "#fbfbfd" }, { "bg2", "#e4e7ed" }, { "btn", "#ffffff" }, { "btn_hover", "#f1f3f7" },
  { "btn_pressed", "#e2e6ee" }, { "btn_disabled", "#f3f4f6" }, { "border", "#cbd0d9" }, { "border_hover", "#aab2c0" },
  { "border_strong", "#9aa3b2" }, { "border_disabled", "#dde1e7" }, { "accent", "#2f6fec" }, { "accent_tab", "#2f6fec" },
  { "on_accent", "#ffffff" }, { "hover_tint", "#0000000f" }, { "field", "#ffffff" }, { "field_code", "#ffffff" },
  { "caret", "#1d2129" }, { "selection", "#2f6fec40" }, { "selection_strong", "#2f6fec33" },
  { "current_line", "#0000000a" }, { "knob", "#ffffff" }, { "track", "#d9dde5" }, { "grabber", "#00000030" },
  { "grabber_hover", "#00000050" }, { "guide", "#00000020" }, { "popup", "#ffffff" }, { "tooltip", "#ffffff" },
  { "dim", "#0000004d" }, { "link", "#1f5fd6" }, { "syn_keyword", "#c41a5b" }, { "syn_type", "#0b7a8f" },
  { "syn_string", "#3c7d19" }, { "syn_number", "#b55a00" }, { "syn_comment", "#7d8593" }, { "syn_pre", "#7a3fb5" },
  { NULL, NULL } };

/* Accessibility variants: the dark palette with overrides. HIGH_CONTRAST:
 * pure black and white, yellow focus and accents, white borders (text over
 * 15:1, disabled text 8:1). COLORBLIND: the Okabe-Ito colours, which stay
 * apart under deuteranopia, protanopia and tritanopia (sky blue accents,
 * orange selection, syntax colours from the same set); states also differ by
 * border, never by hue alone. */
static const pal HC_OVR[] = {
  { "text", "#ffffff" }, { "text_strong", "#ffffff" }, { "text_dim", "#e6e6e6" }, { "text_disabled", "#a6a6a6" },
  { "bg0", "#000000" }, { "bg1", "#000000" }, { "bg2", "#121212" }, { "btn", "#000000" }, { "btn_hover", "#1f1f1f" },
  { "btn_pressed", "#333333" }, { "btn_disabled", "#000000" }, { "border", "#ffffff" }, { "border_hover", "#ffff00" },
  { "border_strong", "#ffffff" }, { "border_disabled", "#8c8c8c" }, { "accent", "#ffff00" }, { "accent_tab", "#ffff00" },
  { "on_accent", "#000000" }, { "hover_tint", "#ffffff33" }, { "field", "#000000" }, { "field_code", "#000000" },
  { "caret", "#ffffff" }, { "selection", "#00ffff66" }, { "selection_strong", "#00ffff59" }, { "current_line", "#ffffff1f" },
  { "knob", "#ffffff" }, { "track", "#000000" }, { "grabber", "#ffffff99" }, { "grabber_hover", "#ffffffd9" },
  { "guide", "#ffffff4d" }, { "popup", "#000000" }, { "tooltip", "#000000" }, { "dim", "#000000cc" }, { "link", "#00ffff" },
  { "syn_keyword", "#ffff00" }, { "syn_type", "#00ffff" }, { "syn_string", "#80ff80" }, { "syn_number", "#ffb000" },
  { "syn_comment", "#c8c8c8" }, { "syn_pre", "#ff8cff" }, { NULL, NULL } };
static const pal CB_OVR[] = {
  { "accent", "#56b4e9" }, { "accent_tab", "#56b4e9" }, { "on_accent", "#000000" }, { "link", "#56b4e9" },
  { "selection", "#e69f0066" }, { "selection_strong", "#e69f0059" }, { "syn_keyword", "#e69f00" }, { "syn_type", "#56b4e9" },
  { "syn_string", "#009e73" }, { "syn_number", "#f0e442" }, { "syn_comment", "#9a9a9a" }, { "syn_pre", "#cc79a7" },
  { NULL, NULL } };

static cw_theme build(const pal *p);
static cw_theme build_variant(const pal *base, const pal *ovr) {
  pal merged[64];
  int n = 0;
  for (const pal *q = base; q->k && n < 63; q++) {
    merged[n] = *q;
    for (const pal *o = ovr; o->k; o++)
      if (!strcmp(o->k, q->k)) merged[n].v = o->v;
    n++;
  }
  merged[n] = (pal){ NULL, NULL };
  return build(merged);
}

static cw_theme build(const pal *p) {
  /* fill the template */
  size_t cap = strlen(TEMPLATE) * 2, n = 0;
  char *txt = cw_alloc(cap, CW_MEM_UI);
  for (const char *s = TEMPLATE; *s;) {
    if (*s == '{') {
      const char *e = strchr(s, '}');
      const char *v = NULL;
      for (const pal *q = p; q->k; q++)
        if ((size_t)(e - s - 1) == strlen(q->k) && !strncmp(s + 1, q->k, (size_t)(e - s - 1))) v = q->v;
      CW_ASSERT(v, "theme template token without a palette entry");
      size_t vl = strlen(v);
      memcpy(txt + n, v, vl);
      n += vl;
      s = e + 1;
    } else txt[n++] = *s++;
  }
  txt[n] = 0;
  theme_t tmp;
  (void)tmp;
  cw_theme t = cw_theme_new((cw_theme){ 0 });
  /* parse into t directly (cw_theme_parse would give it the dark base) */
  int ln = 0;
  for (char *line = txt; line && *line;) {
    char *e = strchr(line, '\n');
    if (e) *e = 0;
    ln++;
    if (*line && !set_line(t, line, "builtin")) CW_FATAL("built-in theme line %d does not parse: %s", ln, line);
    line = e ? e + 1 : NULL;
  }
  cw_free(txt);
  TH(t)->builtin = true;
  return t;
}

cw_theme cw_theme_default_dark(void) {
  ensure_pool();
  if (CW_HANDLE_IS_NULL(dark)) dark = build(DARK);
  return dark;
}
cw_theme cw_theme_default_light(void) {
  ensure_pool();
  if (CW_HANDLE_IS_NULL(light)) light = build(LIGHT);
  return light;
}
cw_theme cw_theme_high_contrast(void) {
  ensure_pool();
  if (CW_HANDLE_IS_NULL(high_contrast)) high_contrast = build_variant(DARK, HC_OVR);
  return high_contrast;
}
cw_theme cw_theme_colorblind(void) {
  ensure_pool();
  if (CW_HANDLE_IS_NULL(colorblind)) colorblind = build_variant(DARK, CB_OVR);
  return colorblind;
}

/* ── accessibility settings ─────────────────────────────────────────── */

static bool mark_all(cw_node n, void *u);
static void relayout_all(void) {
  UI.theme_epoch++;
  cw_tree *tr = cw_current_tree();
  if (!tr) return;
  ui_tree_state *s = ui_state(tr);
  ui_roots_refresh(s);
  for (int i = 0; i < s->roots.count; i++) {
    cw_node_walk(s->roots.data[i], mark_all, NULL);
    ui_mark_min_dirty(s->roots.data[i]);
  }
  for (int i = 0; i < s->popups.count; i++) {
    cw_node_walk(s->popups.data[i].node, mark_all, NULL);
    ui_mark_min_dirty(s->popups.data[i].node);
  }
}
void cw_ui_set_text_scale(float scale) {
  float s = scale > 0 ? CW_CLAMP(scale, 0.5f, 4.0f) : 1;
  if (s == UI.text_scale) return;
  UI.text_scale = s;
  relayout_all();
}
float cw_ui_text_scale(void) { return UI.text_scale > 0 ? UI.text_scale : 1; }
void cw_ui_set_reduced_motion(bool on) { UI.reduced_motion = on; }
bool cw_ui_reduced_motion(void) { return UI.reduced_motion; }

void cw_ui_set_theme(cw_theme t) {
  UI.project_theme = t;
  UI.theme_epoch++;
  /* every control may change size */
  cw_tree *tr = cw_current_tree();
  if (tr) {
    ui_tree_state *s = ui_state(tr);
    ui_roots_refresh(s);
    for (int i = 0; i < s->roots.count; i++) cw_control_set_theme(s->roots.data[i], (cw_theme){ CTL(s->roots.data[i])->_theme_id });
  }
}
cw_theme cw_ui_theme(void) { return UI.project_theme; }

static bool mark_fn(cw_node n, void *u) {
  (void)u;
  cw_control *c = CTL(n);
  if (c) c->_flags |= CF_MIN_DIRTY | CF_LAYOUT_DIRTY | CF_SUBTREE_DIRTY;
  return true;
}
static bool mark_all(cw_node n, void *u) { return mark_fn(n, u); }

void cw_control_set_theme(cw_node n, cw_theme t) {
  cw_control *c = CTL(n);
  if (!c) return;
  c->_theme_id = t.id;
  UI.theme_epoch++;
  cw_node_walk(n, mark_fn, NULL);
  ui_mark_min_dirty(n);
}

static cw_theme overrides_of(cw_node n) {
  cw_control *c = CTL(n);
  if (!c->_override_id || !TH((cw_theme){ c->_override_id })) c->_override_id = cw_theme_new((cw_theme){ 0 }).id;
  return (cw_theme){ c->_override_id };
}
void cw_control_override_color(cw_node n, const char *name, cw_color col) {
  if (!CTL(n)) return;
  cw_theme_set_color(overrides_of(n), "", name, col);
  ui_mark_min_dirty(n);
}
void cw_control_override_constant(cw_node n, const char *name, float v) {
  if (!CTL(n)) return;
  cw_theme_set_constant(overrides_of(n), "", name, v);
  ui_mark_min_dirty(n);
}
void cw_control_override_font(cw_node n, const char *name, cw_font f) {
  if (!CTL(n)) return;
  cw_theme_set_font(overrides_of(n), "", name, f);
  ui_mark_min_dirty(n);
}
void cw_control_override_style(cw_node n, const char *name, const cw_ui_style *s) {
  if (!CTL(n)) return;
  cw_theme_set_style(overrides_of(n), "", name, s);
  ui_mark_min_dirty(n);
}
void cw_control_clear_overrides(cw_node n) {
  cw_control *c = CTL(n);
  if (!c || !c->_override_id) return;
  cw_theme_destroy((cw_theme){ c->_override_id });
  c->_override_id = 0;
  ui_mark_min_dirty(n);
}

/* ── lookups for a control ──────────────────────────────────────────── */

void ui_theme_init(void) {
  cw_theme_default_dark();
  cw_theme_default_light();
  cw_theme_high_contrast();
  cw_theme_colorblind();
}

const char *ui_theme_type(cw_node n) {
  cw_control *c = CTL(n);
  return c && c->_vt->ntheme_types ? c->_vt->theme_types[0] : cw_intern("");
}

static cw_theme nearest_theme(cw_node n) {
  cw_control *c = CTL(n);
  if (c->_theme_epoch == UI.theme_epoch) return (cw_theme){ c->_eff_theme };
  uint64_t found = 0;
  cw_node q = n;
  for (int guard = 0; guard < 256 && cw_node_valid(q); guard++) {
    cw_control *qc = CTL(q);
    if (qc && qc->_theme_id) {
      found = qc->_theme_id;
      break;
    }
    cw_node p = cw_node_parent(q);
    if (CW_HANDLE_IS_NULL(p) && qc && (qc->_flags & CF_INTERNAL)) p = qc->_owner;
    q = p;
  }
  c->_eff_theme = found;
  c->_theme_epoch = UI.theme_epoch;
  return (cw_theme){ found };
}

static bool copy_out(const titem *i, int kind, void *out) {
  switch (kind) {
  case TI_COLOR: *(cw_color *)out = i->v.c; break;
  case TI_CONST: *(float *)out = i->v.k; break;
  case TI_FONT: *(cw_font *)out = i->v.f; break;
  default: *(cw_ui_style *)out = i->v.s; break;
  }
  return true;
}

bool ui_lookup(cw_node n, const char *type, const char *name, int kind, void *out) {
  cw_control *c = CTL(n);
  const char *iname = cw_intern(name);
  const char *empty = cw_intern("");
  const char *types[UI_MAX_THEME_TYPES + 1];
  int nt = 0;
  if (type) types[nt++] = cw_intern(type);
  else if (c)
    for (int i = 0; i < c->_vt->ntheme_types; i++) types[nt++] = c->_vt->theme_types[i];
  types[nt++] = empty;
  if (c && c->_override_id) {
    const titem *i = find((cw_theme){ c->_override_id }, empty, iname, kind);
    if (i) return copy_out(i, kind, out);
  }
  cw_theme srcs[3] = { c ? nearest_theme(n) : (cw_theme){ 0 }, UI.project_theme, cw_theme_default_dark() };
  for (int s = 0; s < 3; s++) {
    if (CW_HANDLE_IS_NULL(srcs[s])) continue;
    if (s > 0 && srcs[s].id == srcs[0].id) continue;
    /* the theme's own items (every type, then "") before its base's */
    for (cw_theme h = srcs[s]; !CW_HANDLE_IS_NULL(h);) {
      theme_t *th = TH(h);
      if (!th) break;
      for (int k = 0; k < nt; k++) {
        uint64_t v;
        if (cw_map_get(&th->idx, ikey(types[k], iname, kind), &v)) return copy_out(&th->items.data[v], kind, out);
      }
      h = th->base;
    }
  }
  return false;
}

cw_color cw_control_theme_color(cw_node n, const char *type, const char *name) {
  cw_color c = { 0, 0, 0, 0 };
  ui_lookup(n, type, name, TI_COLOR, &c);
  return c;
}
float cw_control_theme_constant(cw_node n, const char *type, const char *name) {
  float v = 0;
  ui_lookup(n, type, name, TI_CONST, &v);
  return v;
}
cw_font cw_control_theme_font(cw_node n, const char *type, const char *name) {
  cw_font f = { 0 };
  if (ui_lookup(n, type, name, TI_FONT, &f) && !CW_HANDLE_IS_NULL(f)) return f;
  if (ui_lookup(n, "", "default_font", TI_FONT, &f) && !CW_HANDLE_IS_NULL(f)) return f;
  return cw_ui_font_sans();
}
float cw_control_theme_font_size(cw_node n, const char *type, const char *name) {
  float v = 0, k = UI.text_scale > 0 ? UI.text_scale : 1; /* the accessibility text size */
  if (ui_lookup(n, type, name, TI_CONST, &v) && v > 0) return v * k;
  if (ui_lookup(n, "", "default_font_size", TI_CONST, &v) && v > 0) return v * k;
  return 15 * k;
}
cw_ui_style cw_control_theme_style(cw_node n, const char *type, const char *name) {
  cw_ui_style s = { 0 };
  ui_lookup(n, type, name, TI_STYLE, &s);
  return s;
}
